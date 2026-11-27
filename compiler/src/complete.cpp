// `edaxc complete <file> <line>:<column>`: the names that can be written at a place, for an
// editor's suggestions. The text before the place says what is asked for: a path
// (`std::log::`), a member (`log.`) or a plain name. The program is loaded with that expression
// blanked out, so the file parses; namespaces and types come from the resolved program, the
// types of locals from the checker (or from their declared type when the program doesn't check).

#include "edax/complete.hpp"

#include "edax/ast.hpp"
#include "edax/resolve.hpp"
#include "edax/token.hpp"
#include "edax/types.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>

namespace edax {
namespace {

using namespace ast;
using sema::Symbol;
using sema::SymbolKind;

bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || (c & 0x80); }

// Whether `offset` is inside a `//` comment or a string literal on its line.
bool in_comment_or_string(std::string_view text, std::uint32_t offset) {
    std::size_t line = text.rfind('\n', offset == 0 ? 0 : offset - 1);
    line             = line == std::string_view::npos ? 0 : line + 1;
    bool in_string = false, in_char = false;
    for (std::size_t i = line; i < offset; ++i) {
        char c = text[i];
        if ((in_string || in_char) && c == '\\') {
            ++i;
            continue;
        }
        if (in_string) {
            in_string = c != '"';
        } else if (in_char) {
            in_char = c != '\'';
        } else if (c == '"') {
            in_string = true;
        } else if (c == '\'') {
            in_char = true;
        } else if (c == '/' && i + 1 < offset && text[i + 1] == '/') {
            return true;
        }
    }
    return in_string || in_char;
}

// Skips back over a balanced `(...)`, `[...]` or `<...>` that ends just before `p`; returns
// where it begins, or npos.
std::size_t skip_group_back(std::string_view text, std::size_t p, char open, char close) {
    int depth = 0;
    while (p > 0) {
        char c = text[--p];
        if (c == close) ++depth;
        else if (c == open && --depth == 0) return p;
        else if (c == '\n' && open == '<') return std::string_view::npos;
    }
    return std::string_view::npos;
}

std::uint32_t name_start(std::string_view text, std::uint32_t p) {
    while (p > 0 && is_ident_char(text[p - 1])) --p;
    return p;
}

std::string collapse(std::string_view s, std::size_t limit) {
    std::string out;
    bool        space = false;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    if (out.size() > limit) out = out.substr(0, limit) + "...";
    return out;
}

// The declaration as written: its text up to the body or the `;`.
std::string declaration_text(const Symbol* s, const SourceManager& sources) {
    const Decl* d = s->decl;
    if (!d) return {};
    if (d->span.file >= sources.count()) return {};
    std::string_view text = sources.file(d->span.file).text();
    if (d->span.end > text.size() || d->span.begin >= d->span.end) return {};
    std::string_view whole = text.substr(d->span.begin, d->span.end - d->span.begin);
    int              depth = 0;
    std::size_t      cut   = whole.size();
    for (std::size_t i = 0; i < whole.size(); ++i) {
        char c = whole[i];
        if (c == '(' || c == '[') ++depth;
        else if (c == ')' || c == ']') --depth;
        else if (depth == 0 && (c == '{' || c == ';')) {
            cut = i;
            break;
        } else if (depth == 0 && c == '=' && (d->kind == DeclKind::field || d->kind == DeclKind::variable) && i + 1 < whole.size() && whole[i + 1] != '=') {
            cut = i;   // a field's or global's initializer
            break;
        }
    }
    whole = whole.substr(0, cut);
    // Without the annotations in front (`@std::inline_always`): the declaration is what's asked for.
    while (true) {
        std::size_t at = whole.find_first_not_of(" \t\r\n");
        if (at == std::string_view::npos || whole[at] != '@') break;
        std::size_t i = at + 1;
        while (i < whole.size() && (is_ident_char(whole[i]) || whole[i] == ':')) ++i;
        if (i < whole.size() && whole[i] == '{') {
            int braces = 0;
            for (; i < whole.size(); ++i) {
                if (whole[i] == '{') ++braces;
                else if (whole[i] == '}' && --braces == 0) {
                    ++i;
                    break;
                }
            }
        }
        whole = whole.substr(i);
    }
    return collapse(whole, 200);
}

std::string_view kind_name(const Symbol* s) {
    switch (s->kind) {
    case SymbolKind::function: return "function";
    case SymbolKind::global: return "global";
    case SymbolKind::struct_: return "struct";
    case SymbolKind::class_: return "class";
    case SymbolKind::interface: return "interface";
    case SymbolKind::enum_: return "enum";
    case SymbolKind::alternative: return "alternative";
    case SymbolKind::field: return "field";
    case SymbolKind::method: return "method";
    case SymbolKind::type_param:
    case SymbolKind::value_param: return "type";
    case SymbolKind::local: return "local";
    }
    return "function";
}

// A local in scope at the place: its symbol (typed by the checker) and its declared type.
struct Local {
    std::string     name;
    const Symbol*   symbol = nullptr;
    const TypeExpr* type   = nullptr;
};

// The locals in scope at `offset` and the type declaration around it.
class Scope {
public:
    explicit Scope(std::uint32_t offset) : offset_(offset) {}

    std::map<std::string, Local> locals;
    const TypeDecl*              type = nullptr;   // the innermost struct/class the place is in
    std::string                  ns;               // the namespace of the declaration the place is in

    void file(const File& f) {
        for (const DeclPtr& d : f.decls) decl(*d);
    }

private:
    std::uint32_t offset_;

    bool covers(Span s) const { return s.begin <= offset_ && offset_ <= s.end; }

    void add(const std::string& name, const Symbol* s, const TypeExpr* t) {
        if (!name.empty() && name != "_" && !name.starts_with("$")) locals[name] = Local{name, s, t};
    }

    void params(const std::vector<Param>& ps) {
        for (const Param& p : ps) add(p.name, p.symbol, p.type.get());
    }

    void decl(const Decl& d) {
        if (!covers(d.span)) return;
        ns = d.ns;
        switch (d.kind) {
        case DeclKind::function: {
            const auto& f = d.as<FunctionDecl>();
            params(f.params);
            if (f.body) block(*f.body);
            break;
        }
        case DeclKind::struct_:
        case DeclKind::class_:
        case DeclKind::interface: {
            type = &d.as<TypeDecl>();
            for (const DeclPtr& m : d.as<TypeDecl>().members) decl(*m);
            break;
        }
        default: break;
        }
    }

    void block(const BlockExpr& b) {
        if (!covers(b.span)) return;
        for (const StmtPtr& s : b.statements) {
            if (s->span.begin > offset_) break;
            switch (s->kind) {
            case StmtKind::variable: {
                const auto& v = s->as<VariableStmt>();
                if (v.init) expr(*v.init);
                if (v.span.end <= offset_) add(v.name, v.symbol, v.type.get());
                break;
            }
            case StmtKind::destructure: {
                const auto& ds = s->as<DestructureStmt>();
                expr(*ds.init);
                if (ds.span.end <= offset_) {
                    for (const DestructureBinding& b2 : ds.bindings) add(b2.name, b2.symbol, b2.type.get());
                }
                break;
            }
            case StmtKind::assign:
                expr(*s->as<AssignStmt>().target);
                expr(*s->as<AssignStmt>().value);
                break;
            case StmtKind::discard: expr(*s->as<DiscardStmt>().value); break;
            case StmtKind::expression: expr(*s->as<ExprStmt>().expr); break;
            case StmtKind::check: {
                const Check& c = s->as<CheckStmt>().check;
                if (c.block) expr(*c.block);
                break;
            }
            }
        }
        if (b.tail) expr(*b.tail);
    }

    void pattern(const Pattern& p) {
        switch (p.kind) {
        case PatternKind::binding: add(p.as<BindingPattern>().name, p.as<BindingPattern>().symbol, nullptr); break;
        case PatternKind::enum_alternative:
            if (p.as<EnumPattern>().payload) pattern(*p.as<EnumPattern>().payload);
            break;
        case PatternKind::struct_:
            for (const FieldPattern& f : p.as<StructPattern>().fields) {
                if (f.pattern) pattern(*f.pattern);
            }
            break;
        case PatternKind::tuple:
            for (const PatternPtr& e : p.as<TuplePattern>().elements) pattern(*e);
            break;
        default: break;
        }
    }

    // Only the expressions the place is in open scopes.
    void expr(const Expr& e) {
        if (!covers(e.span)) return;
        switch (e.kind) {
        case ExprKind::block: block(e.as<BlockExpr>()); return;
        case ExprKind::lambda: {
            const auto& l = e.as<LambdaExpr>();
            for (const Capture& c : l.captures) add(c.name, c.symbol, nullptr);
            params(l.params);
            if (l.body) block(*l.body);
            return;
        }
        case ExprKind::if_: {
            const auto& i = e.as<IfExpr>();
            expr(*i.condition);
            if (i.then_block) block(*i.then_block);
            if (i.else_branch) expr(*i.else_branch);
            return;
        }
        case ExprKind::while_:
            expr(*e.as<WhileExpr>().condition);
            if (e.as<WhileExpr>().body) block(*e.as<WhileExpr>().body);
            return;
        case ExprKind::for_: {
            const auto& f = e.as<ForExpr>();
            expr(*f.iterable);
            if (f.body && covers(f.body->span)) {
                add(f.name, f.symbol, f.type.get());
                block(*f.body);
            }
            return;
        }
        case ExprKind::match: {
            const auto& m = e.as<MatchExpr>();
            expr(*m.scrutinee);
            for (const MatchArm& a : m.arms) {
                if (!covers(a.span)) continue;
                if (a.pattern) pattern(*a.pattern);
                if (a.body) expr(*a.body);
            }
            return;
        }
        case ExprKind::call: {
            const auto& c = e.as<CallExpr>();
            expr(*c.callee);
            for (const ExprPtr& a : c.args) expr(*a);
            return;
        }
        case ExprKind::binary:
            expr(*e.as<BinaryExpr>().lhs);
            expr(*e.as<BinaryExpr>().rhs);
            return;
        case ExprKind::unary: expr(*e.as<UnaryExpr>().operand); return;
        case ExprKind::member: expr(*e.as<MemberExpr>().object); return;
        case ExprKind::struct_literal:
            for (const FieldInit& f : e.as<StructLiteralExpr>().fields) {
                if (f.value) expr(*f.value);
            }
            return;
        case ExprKind::return_:
            if (e.as<ReturnExpr>().value) expr(*e.as<ReturnExpr>().value);
            return;
        default: return;
        }
    }
};

class Completer {
public:
    Completer(const sema::Program* program, const SourceManager& sources, const sema::Program& library,
              const SourceManager& library_sources, FileId file, std::uint32_t offset)
        : program_(program), library_(library), sources_(sources), library_sources_(library_sources), scope_(offset) {
        for (const auto& [name, m] : library_.modules) library_modules_.insert(m.get());
        if (program_) {
            program_->refresh_index();
            for (const auto& [name, m] : program_->modules) {
                if (m->header && m->header->id == file) ast_ = &m->header->ast, module_ = m.get();
                if (m->source && m->source->id == file) ast_ = &m->source->ast, module_ = m.get();
            }
            if (ast_) scope_.file(*ast_);
        }
        library_.refresh_index();
    }

    std::vector<Completion> run(const CompletionQuery& q) {
        switch (q.kind) {
        case CompletionQuery::Kind::path: path(q.qualifier); break;
        case CompletionQuery::Kind::member: member(q.receiver); break;
        case CompletionQuery::Kind::name: names(); break;
        case CompletionQuery::Kind::none: break;
        }
        // All of them: the editor filters by what is written as the name grows.
        std::vector<Completion> out;
        for (auto& [name, c] : found_) out.push_back(std::move(c));
        return out;
    }

private:
    const sema::Program*             program_;
    const sema::Program&             library_;
    const SourceManager&             sources_;
    const SourceManager&             library_sources_;
    std::set<const sema::Module*>    library_modules_;
    const File*                      ast_    = nullptr;
    const sema::Module*              module_ = nullptr;
    Scope                            scope_;
    std::map<std::string, Completion> found_;

    static bool hidden_name(std::string_view name) {
        return name.empty() || !(std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_' || (name.front() & 0x80)) ||
               name.starts_with("__") || name.starts_with("operator");
    }

    void add(const std::string& name, std::string_view kind, std::string detail, std::string doc) {
        if (hidden_name(name) || found_.contains(name)) return;
        found_[name] = Completion{name, std::string(kind), std::move(detail), std::move(doc)};
    }

    void add_symbol(const std::string& name, const Symbol* s, std::size_t overloads = 1) {
        if (hidden_name(name) || found_.contains(name)) return;
        std::string detail = declaration_text(s, s->module && library_modules_.contains(s->module) ? library_sources_ : sources_);
        if (detail.empty() && s->parent) detail = s->parent->key() + "::" + name;
        if (overloads > 1) detail += std::format("  (+{} overloads)", overloads - 1);
        std::string doc = s->decl ? s->decl->doc : std::string();
        // `// text` comments keep the space after the slashes.
        std::string trimmed;
        for (std::size_t at = 0; at <= doc.size();) {
            std::size_t next = doc.find('\n', at);
            if (next == std::string::npos) next = doc.size();
            std::string_view l = std::string_view(doc).substr(at, next - at);
            if (l.starts_with(" ")) l.remove_prefix(1);
            trimmed += std::string(l) + (next < doc.size() ? "\n" : "");
            at = next + 1;
        }
        add(name, kind_name(s), std::move(detail), std::move(trimmed));
    }

    // Whether this file may name `s` (private items of other modules are not offered).
    bool visible(const Symbol* s) const { return !s->is_private || (module_ && s->module == module_); }

    static bool namespace_exists(const sema::Program& p, const std::string& ns) {
        return ns.empty() || p.namespace_index.contains(ns);
    }

    static const Symbol* item(const sema::Program& p, const std::string& key) {
        auto it = p.item_index.find(key);
        if (it == p.item_index.end()) return nullptr;
        for (const auto& entry : it->second) {
            if (!entry.symbols->empty()) return entry.symbols->front();
        }
        return nullptr;
    }

    // The items and namespaces directly in `ns`.
    void list_namespace(const sema::Program& p, const std::string& ns) {
        std::string prefix = ns.empty() ? "" : ns + "::";
        for (const auto& [key, entries] : p.item_index) {
            if (!key.starts_with(prefix) || key.size() == prefix.size()) continue;
            std::string_view rest = key.substr(prefix.size());
            if (rest.find("::") != std::string_view::npos) continue;
            const Symbol* first = nullptr;
            std::size_t   count = 0;
            for (const auto& entry : entries) {
                for (const Symbol* s : *entry.symbols) {
                    if (!visible(s) && &p == program_) continue;
                    if (&p != program_ && s->is_private) continue;
                    if (!first) first = s;
                    ++count;
                }
            }
            if (first) add_symbol(std::string(rest), first, count);
        }
        for (const auto& [name, modules] : p.namespace_index) {
            if (!name.starts_with(prefix) || name.size() == prefix.size()) continue;
            std::string_view rest = name.substr(prefix.size());
            if (rest.find("::") != std::string_view::npos) continue;
            add(std::string(rest), "namespace", "namespace " + std::string(name), {});
        }
    }

    // A type's static members and alternatives (`std::log::console::make`, `shape::circle`).
    void list_statics(const Symbol* type) {
        for (const auto& [name, syms] : type->members) {
            if (syms.empty()) continue;
            const Symbol* s = syms.front();
            bool is_static  = s->kind == SymbolKind::alternative || s->is_static ||
                             (s->kind == SymbolKind::method && s->function() && s->function()->is_static);
            if (is_static) add_symbol(name, s, syms.size());
        }
    }

    // `a::b` written in the file: what it names, trying the enclosing namespaces and the file's
    // namespace aliases.
    std::vector<std::string> candidates(const std::vector<std::string>& written) const {
        std::string joined;
        for (const std::string& s : written) joined += (joined.empty() ? "" : "::") + s;
        std::vector<std::string> out;
        if (ast_ && !written.empty()) {
            for (const NamespaceAlias& a : ast_->aliases) {
                if (a.name != written.front()) continue;
                std::string target;
                for (const PathSegment& seg : a.target.segments) target += (target.empty() ? "" : "::") + seg.name;
                for (std::size_t i = 1; i < written.size(); ++i) target += "::" + written[i];
                out.push_back(target);
            }
        }
        for (std::string ns = scope_.ns;;) {
            out.push_back(ns.empty() ? joined : ns + "::" + joined);
            if (ns.empty()) break;
            std::size_t at = ns.rfind("::");
            ns             = at == std::string::npos ? "" : ns.substr(0, at);
        }
        return out;
    }

    void path(const std::vector<std::string>& written) {
        for (const std::string& full : candidates(written)) {
            bool any = false;
            for (const sema::Program* p : {program_, &library_}) {
                if (!p) continue;
                if (namespace_exists(*p, full) && !full.empty()) {
                    list_namespace(*p, full);
                    any = true;
                }
                if (const Symbol* t = item(*p, full); t && t->is_type()) {
                    list_statics(t);
                    list_namespace(*p, full);   // types declared inside it
                    any = true;
                }
            }
            if (full == "std") {
                for (const std::string& name : compiler_std_names()) add(name, "builtin", "std::" + name + " (provided by the compiler)", {});
            }
            if (any) return;
        }
    }

    // The type a declared type names, if it is a struct, class, interface or enum.
    static const Symbol* named(const TypeExpr* t) {
        while (t && t->kind == TypeKind::reference) t = t->as<ReferenceType>().inner.get();
        if (!t || t->kind != TypeKind::named) return nullptr;
        const Symbol* s = t->as<NamedType>().symbol;
        return s && s->is_type() && s->kind != SymbolKind::type_param ? s : nullptr;
    }

    const Symbol* of_type(const types::Type* t) const {
        while (t && (t->kind == types::Kind::reference || t->kind == types::Kind::pointer)) t = t->inner;
        if (!t) return nullptr;
        if (t->kind == types::Kind::user) return t->symbol;
        if (t->kind == types::Kind::external) {
            std::string base(t->external_base());
            if (program_) {
                if (const Symbol* s = item(*program_, base)) return s;
            }
            return item(library_, base);
        }
        return nullptr;
    }

    // The member `name` of `type` or one of its bases.
    static const std::vector<Symbol*>* find_member(const Symbol* type, const std::string& name) {
        for (int guard = 0; type && guard < 32; ++guard) {
            if (auto it = type->members.find(name); it != type->members.end() && !it->second.empty()) return &it->second;
            const Symbol* next = nullptr;
            for (const Symbol* b : type->bases) {
                if (auto it = b->members.find(name); it != b->members.end() && !it->second.empty()) return &it->second;
                if (!next) next = b;
            }
            type = next;
        }
        return nullptr;
    }

    // The type of one step after the receiver: a field's, or a method's result.
    const Symbol* step(const Symbol* type, std::string name) {
        bool call = name.ends_with(")");
        bool index = name.ends_with("]");
        if (call || index) name = name.substr(0, name.find(call ? '(' : '['));
        const std::vector<Symbol*>* m = find_member(type, name);
        if (!m) return nullptr;
        const Symbol* s = m->front();
        if (s->type) return of_type(s->type);
        if (s->kind == SymbolKind::field && s->decl) {
            const Symbol* t = named(s->decl->as<VariableDecl>().type.get());
            return index ? nullptr : t;
        }
        if (s->kind == SymbolKind::method && call && s->function()) return named(s->function()->result.get());
        return nullptr;
    }

    const Symbol* receiver_type(const std::vector<std::string>& steps) {
        if (steps.empty()) return nullptr;
        const Symbol* t     = nullptr;
        std::string   first = steps.front();
        if (first == "this") {
            if (scope_.type && program_) {
                const TypeDecl& d = *scope_.type;
                t = item(*program_, d.ns.empty() ? d.name : d.ns + "::" + d.name);
            }
        } else if (auto it = scope_.locals.find(first); it != scope_.locals.end()) {
            const Local& l = it->second;
            if (l.symbol && l.symbol->type) t = of_type(l.symbol->type);
            if (!t) t = named(l.type);
        } else if (scope_.type && program_) {
            // An implicit member (`size` in a method means `this.size`).
            const TypeDecl& d    = *scope_.type;
            const Symbol*   self = item(*program_, d.ns.empty() ? d.name : d.ns + "::" + d.name);
            if (self) t = step(self, first);
        }
        for (std::size_t i = 1; t && i < steps.size(); ++i) t = step(t, steps[i]);
        return t;
    }

    void list_members(const Symbol* type, bool inside) {
        for (int guard = 0; type && guard < 32; ++guard) {
            for (const auto& [name, syms] : type->members) {
                if (syms.empty()) continue;
                const Symbol* s = syms.front();
                if (s->kind != SymbolKind::field && s->kind != SymbolKind::method) continue;
                if (s->is_static || (s->function() && (s->function()->is_static || s->function()->is_destructor || s->function()->is_operator))) {
                    continue;
                }
                if (!inside && (s->access == Access::private_ || s->access == Access::protected_)) continue;
                add_symbol(name, s, syms.size());
            }
            const Symbol* next = nullptr;
            for (const Symbol* b : type->bases) {
                if (b->kind == SymbolKind::class_) next = b;
                else list_members(b, inside);
            }
            type = next;
        }
    }

    // The type a distinct type with a check lends its read-only methods from, or null.
    static const Symbol* lends_from(const Symbol* t) {
        if (!t || !t->decl || t->kind != SymbolKind::struct_ || !t->decl->as<TypeDecl>().is_distinct || !t->members.contains("$check")) {
            return nullptr;
        }
        for (const Symbol* m : t->member_order) {
            if (m->kind == SymbolKind::field && m->decl) return named(m->decl->as<VariableDecl>().type.get());
        }
        return nullptr;
    }

    void member(const std::vector<std::string>& steps) {
        const Symbol* t = receiver_type(steps);
        if (!t) return;
        bool inside = steps.front() == "this" || (scope_.type && t->decl == scope_.type);
        list_members(t, inside);
        if (const Symbol* u = lends_from(t)) {   // `p.` on an absolute_path: path's methods that only read
            add("clone", "method", t->name + " clone()", "A copy (allocates when the value does).");
            for (const auto& [name, syms] : u->members) {
                if (syms.empty() || syms.front()->kind != SymbolKind::method) continue;
                const FunctionDecl* f = syms.front()->function();
                if (!f || f->is_static || f->is_mut || f->is_move || f->is_destructor || f->is_operator || name == "clone") continue;
                if (syms.front()->access == Access::private_ || syms.front()->access == Access::protected_) continue;
                add_symbol(name, syms.front(), syms.size());
            }
        }
    }

    void names() {
        for (const auto& [name, l] : scope_.locals) {
            std::string detail;
            if (l.symbol && l.symbol->type) {
                detail = types::to_string(l.symbol->type) + " " + name;
            } else if (l.type && l.type->span.file < sources_.count() && l.type->span.end <= sources_.file(l.type->span.file).text().size()) {
                detail = collapse(sources_.file(l.type->span.file).text().substr(l.type->span.begin, l.type->span.size()), 120) + " " + name;
            }
            add(name, "local", std::move(detail), {});
        }
        if (scope_.type && program_) {
            const TypeDecl& d    = *scope_.type;
            const Symbol*   self = item(*program_, d.ns.empty() ? d.name : d.ns + "::" + d.name);
            if (self) list_members(self, true);
        }
        if (ast_) {
            for (const NamespaceAlias& a : ast_->aliases) {
                std::string target;
                for (const PathSegment& seg : a.target.segments) target += (target.empty() ? "" : "::") + seg.name;
                add(a.name, "alias", "namespace " + a.name + " = " + target, {});
            }
        }
        for (std::string ns = scope_.ns;;) {
            if (program_) list_namespace(*program_, ns);
            if (ns.empty()) break;
            std::size_t at = ns.rfind("::");
            ns             = at == std::string::npos ? "" : ns.substr(0, at);
        }
        if (!program_) add("std", "namespace", "namespace std", {});
        static const std::vector<std::string_view> keywords = {
#define EDAX_TOKEN(name, spelling)
#define EDAX_KEYWORD(word) #word,
#define EDAX_TYPE_KEYWORD(word) #word,
#include "edax/token_kinds.def"
#undef EDAX_TOKEN
#undef EDAX_KEYWORD
#undef EDAX_TYPE_KEYWORD
        };
        for (std::string_view k : keywords) add(std::string(k), "keyword", {}, {});
    }
};

} // namespace

CompletionQuery completion_query(std::string_view text, std::uint32_t offset) {
    CompletionQuery q;
    offset = std::min<std::uint32_t>(offset, static_cast<std::uint32_t>(text.size()));
    if (in_comment_or_string(text, offset)) return q;
    std::uint32_t begin = name_start(text, offset);
    std::uint32_t end   = offset;
    while (end < text.size() && is_ident_char(text[end])) ++end;
    q.prefix = std::string(text.substr(begin, offset - begin));
    q.end    = end;
    q.start  = begin;
    if (!q.prefix.empty() && std::isdigit(static_cast<unsigned char>(q.prefix.front()))) return q;
    if (begin >= 2 && text.substr(begin - 2, 2) == "::") {
        // A path: `name(<args>)?::` going back.
        std::size_t p = begin - 2;
        std::vector<std::string> segs;
        while (true) {
            if (p > 0 && text[p - 1] == '>') {
                p = skip_group_back(text, p, '<', '>');
                if (p == std::string_view::npos) return q;
            }
            std::uint32_t s = name_start(text, static_cast<std::uint32_t>(p));
            if (s == p) break;
            segs.insert(segs.begin(), std::string(text.substr(s, p - s)));
            p = s;
            if (p >= 2 && text.substr(p - 2, 2) == "::") p -= 2;
            else break;
        }
        if (segs.empty()) return q;
        q.kind      = CompletionQuery::Kind::path;
        q.qualifier = std::move(segs);
        q.start     = static_cast<std::uint32_t>(p);
        return q;
    }
    if (begin >= 1 && text[begin - 1] == '.' && !(begin >= 2 && text[begin - 2] == '.')) {
        // A member: `name`, `name(...)`, `name[...]` steps separated by dots going back.
        std::size_t p = begin - 1;
        std::vector<std::string> steps;
        while (true) {
            std::size_t stop = p;
            if (p > 0 && (text[p - 1] == ')' || text[p - 1] == ']')) {
                p = skip_group_back(text, p, text[p - 1] == ')' ? '(' : '[', text[p - 1]);
                if (p == std::string_view::npos) return q;
            }
            std::uint32_t s = name_start(text, static_cast<std::uint32_t>(p));
            if (s == p) return q;   // `(a + b).`, a literal: not handled
            std::string name(text.substr(s, p - s));
            std::string rest(text.substr(p, stop - p));
            steps.insert(steps.begin(), name + (rest.empty() ? "" : rest.substr(0, 1) + rest.substr(rest.size() - 1)));
            p = s;
            if (p >= 1 && text[p - 1] == '.') --p;
            else break;
        }
        if (!steps.empty() && std::isdigit(static_cast<unsigned char>(steps.front().front()))) return q;
        q.kind     = CompletionQuery::Kind::member;
        q.receiver = std::move(steps);
        q.start    = static_cast<std::uint32_t>(p);
        return q;
    }
    q.kind = CompletionQuery::Kind::name;
    return q;
}

std::string without_query(std::string_view text, const CompletionQuery& query, std::uint32_t offset, bool whole_line) {
    std::string out(text);
    std::size_t from = std::min<std::size_t>(query.start, out.size());
    std::size_t to   = std::min<std::size_t>(std::max(query.end, offset), out.size());
    if (whole_line) {
        std::size_t line = from == 0 ? std::string::npos : out.rfind('\n', from - 1);
        from             = line == std::string::npos ? 0 : line + 1;
        std::size_t next = out.find('\n', to);
        to               = next == std::string::npos ? out.size() : next;
    }
    for (std::size_t i = from; i < to; ++i) {
        if (out[i] != '\n' && out[i] != '\r') out[i] = ' ';
    }
    return out;
}

std::vector<Completion> find_completions(const sema::Program* program, const SourceManager& sources, const sema::Program& library,
                                         const SourceManager& library_sources, FileId file, std::uint32_t offset,
                                         const CompletionQuery& query) {
    Completer c(program, sources, library, library_sources, file, offset);
    return c.run(query);
}

} // namespace edax
