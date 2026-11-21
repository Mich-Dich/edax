#include "edax/resolve.hpp"
#include "edax/ast_walk.hpp"

#include "edax/ast_clone.hpp"
#include "edax/ast_printer.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <ostream>
#include <set>

namespace edax {

using namespace ast;
using sema::Module;
using sema::Program;
using sema::Symbol;
using sema::SymbolKind;
using sema::LocalKind;
using sema::AnnotationTarget;
using sema::AnnotationCheck;

namespace sema {
std::string_view to_string(SymbolKind kind) {
    switch (kind) {
    case SymbolKind::function: return "function";
    case SymbolKind::global: return "variable";
    case SymbolKind::struct_: return "struct";
    case SymbolKind::class_: return "class";
    case SymbolKind::interface: return "interface";
    case SymbolKind::enum_: return "enum";
    case SymbolKind::alternative: return "enum alternative";
    case SymbolKind::field: return "field";
    case SymbolKind::method: return "method";
    case SymbolKind::type_param: return "template type parameter";
    case SymbolKind::value_param: return "template value parameter";
    case SymbolKind::local: return "local variable";
    }
    return "?";
}
} // namespace sema

namespace {

// --- signatures ---------------------------------------------------------------------------

// Parameter types and trailing `mut`: what overloads and header/source pairs are matched on.
std::string params_sig(const FunctionDecl& f) {
    std::string s = "(";
    for (std::size_t i = 0; i < f.params.size(); ++i) {
        s += (i ? ", " : "") + std::string(f.params[i].is_implicit ? "implicit " : "") +
             (f.params[i].is_compile_time ? "compile_time " : "") + to_string(*f.params[i].type);
    }
    s += ")";
    return f.is_mut ? s + " mut" : f.is_move ? s + " move" : s;
}

std::string result_sig(const FunctionDecl& f) { return f.result ? to_string(*f.result) : ""; }

std::string template_sig(const Decl& d) {
    if (!d.is_template) {
        return "";
    }
    std::string s = "template<";
    for (std::size_t i = 0; i < d.template_params.size(); ++i) {
        const TemplateParam& p = d.template_params[i];
        s += i ? ", " : "";
        if (p.is_type) {
            s += "type " + p.name;
            for (std::size_t c = 0; c < p.constraints.size(); ++c) {
                s += (c ? " & " : " : ") + to_string(*p.constraints[c]);
            }
        } else {
            s += to_string(*p.value_type) + " " + p.name;
        }
    }
    return s + ">";
}

std::string bases_sig(const TypeDecl& t) {
    std::string s;
    for (std::size_t i = 0; i < t.bases.size(); ++i) {
        s += (i ? ", " : "") + to_string(*t.bases[i]);
    }
    return s;
}

std::string describe_fn(const Symbol& s) {
    const FunctionDecl* f = s.function();
    std::string         prefix = s.parent ? s.parent->name + "::" : "";
    return f ? prefix + s.name + params_sig(*f) : prefix + s.name;
}

// Edit distance where swapping two adjacent characters counts as one edit ("inptu" -> "input").
std::size_t edit_distance(std::string_view a, std::string_view b) {
    std::vector<std::vector<std::size_t>> d(a.size() + 1, std::vector<std::size_t>(b.size() + 1));
    for (std::size_t i = 0; i <= a.size(); ++i) d[i][0] = i;
    for (std::size_t j = 0; j <= b.size(); ++j) d[0][j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        for (std::size_t j = 1; j <= b.size(); ++j) {
            std::size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) {
                d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
            }
        }
    }
    return d[a.size()][b.size()];
}

std::optional<std::string> closest(std::string_view name, const std::vector<std::string>& candidates) {
    std::size_t best_dist = std::max<std::size_t>(1, name.size() / 3) + 1;
    std::optional<std::string> best;
    for (const std::string& c : candidates) {
        if (c == name) {
            continue;
        }
        std::size_t d = edit_distance(name, c);
        if (d < best_dist) {
            best_dist = d;
            best      = c;
        }
    }
    return best;
}

std::string join_ns(const std::string& ns, const std::string& name) { return ns.empty() ? name : ns + "::" + name; }

std::string item_key(const Decl& d) { return join_ns(d.ns, d.name); }

// "a::b" → {"a::b", "a", ""}: the namespaces searched for an unqualified name (spec 30).
std::vector<std::string> enclosing_levels(const std::string& ns) {
    std::vector<std::string> out;
    std::string              cur = ns;
    while (!cur.empty()) {
        out.push_back(cur);
        std::size_t p = cur.rfind("::");
        cur           = p == std::string::npos ? "" : cur.substr(0, p);
    }
    out.push_back("");
    return out;
}

// A key as written in messages: a global name gets its `::` ("::version") so it reads as one.
std::string global_display(const std::string& key) {
    return key.find("::") == std::string::npos ? "::" + key : key;
}

std::string ns_display(const std::string& ns) { return ns.empty() ? "the global namespace" : "namespace '" + ns + "'"; }

// `f32x8`, `u8x32`, ...: the SIMD short names (a number type, `x`, a power of two from 2 to 64).
bool is_vector_type_name(std::string_view name) {
    std::size_t x = name.rfind('x');
    if (x == std::string_view::npos || x == 0 || x + 1 >= name.size()) return false;
    std::optional<TokenKind> k = keyword_kind(name.substr(0, x));
    if (!k || !is_type_keyword(*k)) return false;
    std::string_view lane = name.substr(0, x);
    if (lane == "bool" || lane == "char" || lane == "void" || lane == "never" || lane.ends_with("size") || lane.ends_with("128")) return false;
    std::string_view digits = name.substr(x + 1);
    if (digits.find_first_not_of("0123456789") != std::string_view::npos || digits.size() > 2 || digits[0] == '0') return false;
    int n = std::stoi(std::string(digits));
    return n >= 2 && n <= 64 && (n & (n - 1)) == 0;
}

bool is_builtin_type_name(std::string_view name) {
    std::optional<TokenKind> k = keyword_kind(name);
    return (k && (is_type_keyword(*k) || *k == TokenKind::kw_type)) || is_vector_type_name(name);
}

// `type`: the type of type values (spec 26).
bool is_type_type(const TypeExpr& t) {
    if (t.kind != TypeKind::named) return false;
    const Path& p = t.as<NamedType>().path;
    return p.segments.size() == 1 && p.segments[0].name == "type" && p.segments[0].template_args.empty();
}

enum class Want { value, type };

struct ExprFlags {
    bool allow_mut_marker = false;
    bool is_callee        = false;
};

struct Scope {
    std::map<std::string, Symbol*> names;
    bool                           lambda_boundary = false;
};

// What we know while resolving inside one function body.
struct FunctionContext {
    Symbol* self_type   = nullptr;   // enclosing struct/class/interface
    bool    has_this    = false;
    bool    is_static   = false;
    int     loop_depth  = 0;
};

class Resolver {
public:
    Resolver(Program& program, DiagnosticEngine& diags) : program_(program), diags_(diags) {
        for (auto& [name, m] : program_.modules) {
            if (name == "std" || name.starts_with("std::")) std_modules_.push_back(m.get());
        }
    }

    // The namespace that the code being resolved is written in (instances: their template's).
    void set_namespace(std::string ns) { ns_ = std::move(ns); }

    void run() {
        for (auto& [name, m] : program_.modules) collect(*m);
        check_namespaces();
        for (auto& [name, m] : program_.modules) process_annotations(*m);
        for (auto& [name, m] : program_.modules) resolve_bases(*m);
        for (auto& [name, m] : program_.modules) check_hierarchy(*m);
        for (auto& [name, m] : program_.modules) resolve_bodies(*m);
    }

    // An instance of a template type (spec 14): a symbol for a copy `d` of the template's
    // declaration, with its members collected and resolved in the template's module. The copy
    // is not registered in the module's items. `params` receives its template parameters.
    Symbol* build_instance(Module& m, bool from_header, Decl& d, std::vector<Symbol*>& params) {
        FileScope fs(*this, &m, from_header);
        instance_ = true;
        SymbolKind kind = d.kind == DeclKind::struct_ ? SymbolKind::struct_
                        : d.kind == DeclKind::class_  ? SymbolKind::class_
                        : d.kind == DeclKind::enum_   ? SymbolKind::enum_
                                                      : SymbolKind::interface;
        Symbol* sym    = m.make(kind, d.name, d.name_span);
        sym->decl      = &d;
        sym->in_header = false;
        sym->ns        = ns_;
        d.ns           = ns_;
        symbol_of_[&d] = sym;
        TemplateScope ts(*this, d);
        params = template_params_.back();
        if (d.kind == DeclKind::enum_) {
            auto& e = d.as<EnumDecl>();
            for (EnumAlternative& alt : e.alternatives) {
                Symbol* a      = m.make(SymbolKind::alternative, alt.name, alt.span);
                a->parent      = sym;
                a->ns          = sym->ns;
                a->alternative = &alt;
                sym->members[alt.name].push_back(a);
                sym->member_order.push_back(a);
            }
            resolve_type(*e.tag_type);
            for (EnumAlternative& alt : e.alternatives) {
                if (alt.payload) resolve_type(*alt.payload);
                if (alt.value) resolve_expr(*alt.value);
            }
            annotate_type(m, d, sym, false);
            return sym;
        }
        for (const DeclPtr& mem : d.as<TypeDecl>().members) add_member(m, sym, *mem, false);
        annotate_type(m, d, sym, false);
        for (const TypePtr& base : d.as<TypeDecl>().bases) {   // generated interfaces of the instance
            if (!pre_resolved_.count(base.get())) continue;
            const Symbol* b = base->kind == TypeKind::named ? base->as<NamedType>().symbol : nullptr;
            if (b && b->is_type() && b->kind != SymbolKind::type_param) sym->bases.push_back(const_cast<Symbol*>(b));
            else sym->has_opaque_base = true;
        }
        for (ThreadPromise& p : d.as<TypeDecl>().promises) {
            if (p.condition) {
                scopes_.emplace_back();
                resolve_expr(*p.condition);
                scopes_.pop_back();
            }
        }
        for (const DeclPtr& mem : d.as<TypeDecl>().members) resolve_decl(*mem, sym);
        return sym;
    }

    // Makes the symbol of a function template instance: the copy's signature and body are resolved
    // in the template's module, with its parameters declared (see build_instance).
    Symbol* build_function_instance(Module& m, bool from_header, FunctionDecl& d, std::vector<Symbol*>& params) {
        FileScope fs(*this, &m, from_header);
        Symbol*   sym  = m.make(SymbolKind::function, d.name, d.name_span);
        sym->decl      = &d;
        sym->in_header = false;
        sym->ns        = ns_;
        TemplateScope ts(*this, d);
        params = template_params_.back();
        resolve_function(d, nullptr);
        if (!d.annotations.empty()) {   // the wraps of a function template apply to its instances
            instance_      = true;
            symbol_of_[&d] = sym;
            annotate_function(d, sym, nullptr);
            apply_wraps(d, nullptr);
        }
        return sym;
    }

private:
    // --- diagnostics ----------------------------------------------------------------------

    Diagnostic& error(DiagCode code, Span span, std::string message, std::string label = {}) {
        return diags_.report(code, span, std::move(message), std::move(label));
    }
    static void note(Diagnostic& d, std::string message, std::optional<Span> span = std::nullopt) {
        d.notes.push_back(Note{std::move(message), span});
    }
    static void suggest(Diagnostic& d, std::string_view name, const std::vector<std::string>& candidates) {
        if (std::optional<std::string> best = closest(name, candidates)) {
            note(d, std::format("did you mean '{}'?", *best));
        }
    }

    // --- phase A: collect declarations, merge header and source ---------------------------

    void collect(Module& m) {
        module_ = &m;
        for (const std::optional<sema::ModuleFile>* f : {&m.header, &m.source}) {
            if (!*f) continue;
            for (const ast::NamespaceBlock& b : (*f)->ast.namespaces) {
                for (const std::string& level : enclosing_levels(b.path)) {
                    if (!level.empty()) m.namespaces.insert(level);
                }
            }
        }
        if (m.header) {
            check_header(m);
            for (const DeclPtr& d : m.header->ast.decls) {
                add_item(m, *d, /*from_header=*/true);
            }
        }
        if (m.source) {
            for (const DeclPtr& d : m.source->ast.decls) {
                if (m.header) {
                    merge_source_item(m, *d);
                } else {
                    add_item(m, *d, /*from_header=*/false);
                }
            }
        }
        check_definitions(m);
        check_duplicates(m.items, nullptr);
        for (Symbol* s : m.item_order) {
            if (s->is_type()) {
                check_duplicates(s->members, s);
            }
        }
    }

    void check_header(Module& m) {
        auto no_check = [&](const Decl& d) {
            if (d.kind != DeclKind::check) return;
            note(error(DiagCode::E3019, d.span, "checks are written in the .ed, not in a header", "check in a header"),
                 "move it into the source file; importers still get it through the compiled module (spec 23)");
        };
        for (const DeclPtr& d : m.header->ast.decls) {
            no_check(*d);
            if (d->kind == DeclKind::struct_ || d->kind == DeclKind::class_) {
                for (const DeclPtr& mem : d->as<TypeDecl>().members) no_check(*mem);
            }
            if (d->is_private) {
                note(error(DiagCode::E3012, d->name_span, std::format("'{}' is marked 'private' in a header", d->name),
                           "private item"),
                     "private items live only in the .ed");
            }
            if (d->kind == DeclKind::function && d->as<FunctionDecl>().body) {
                note(error(DiagCode::E3011, d->as<FunctionDecl>().body->span,
                           std::format("function '{}' has a body in a header", d->name), "body"),
                     "declare it here as 'T f(...);' and move the body into the .ed");
            }
            if (d->kind == DeclKind::struct_ || d->kind == DeclKind::class_) {
                const auto& t = d->as<TypeDecl>();
                for (const DeclPtr& mem : t.members) {
                    if (mem->kind == DeclKind::function && mem->as<FunctionDecl>().body) {
                        note(error(DiagCode::E3011, mem->as<FunctionDecl>().body->span,
                                   std::format("method '{}::{}' has a body in a header", d->name, mem->name), "body"),
                             "method bodies are defined only inside the type's block in the .ed");
                    }
                    if (d->kind == DeclKind::class_ &&
                        (mem->access == Access::private_ || mem->access == Access::none)) {
                        Diagnostic& e = error(DiagCode::E3012, mem->name_span,
                                              std::format("'{}::{}' is private in a header", d->name, mem->name),
                                              "private member");
                        note(e, mem->access == Access::none
                                    ? "class members in a header go under 'public:' or 'protected:'"
                                    : "private members are declared only in the class block of the .ed");
                    }
                }
            }
        }
    }

    Symbol* add_item(Module& m, const Decl& d, bool from_header) {
        Symbol* sym = nullptr;
        switch (d.kind) {
        case DeclKind::import:
        case DeclKind::check: return nullptr;
        case DeclKind::function: {
            sym = m.make(SymbolKind::function, d.name, d.name_span);
            sym->is_static = d.as<FunctionDecl>().is_static;
            break;
        }
        case DeclKind::variable:
        case DeclKind::field: {
            sym = m.make(SymbolKind::global, d.name, d.name_span);
            sym->is_mut = d.as<VariableDecl>().is_mut;
            sym->is_type_value = d.as<VariableDecl>().storage == Storage::compile_time && d.as<VariableDecl>().type &&
                                 is_type_type(*d.as<VariableDecl>().type);
            break;
        }
        case DeclKind::struct_:
        case DeclKind::class_:
        case DeclKind::interface: {
            SymbolKind k = d.kind == DeclKind::struct_ ? SymbolKind::struct_
                         : d.kind == DeclKind::class_  ? SymbolKind::class_
                                                       : SymbolKind::interface;
            sym = m.make(k, d.name, d.name_span);
            sym->ns = d.ns;
            for (const DeclPtr& mem : d.as<TypeDecl>().members) {
                add_member(m, sym, *mem, from_header);
            }
            break;
        }
        case DeclKind::enum_: {
            sym = m.make(SymbolKind::enum_, d.name, d.name_span);
            for (const EnumAlternative& alt : d.as<EnumDecl>().alternatives) {
                Symbol* a      = m.make(SymbolKind::alternative, alt.name, alt.span);
                a->parent      = sym;
                a->ns          = d.ns;
                a->alternative = &alt;
                a->in_header   = from_header;
                sym->members[alt.name].push_back(a);
                sym->member_order.push_back(a);
            }
            break;
        }
        }
        sym->decl       = &d;
        sym->ns         = d.ns;
        sym->definition = from_header ? nullptr : &d;
        sym->in_header  = from_header;
        sym->is_private = d.is_private || (!from_header && m.header.has_value());
        m.items[item_key(d)].push_back(sym);
        m.item_order.push_back(sym);
        symbol_of_[&d] = sym;
        return sym;
    }

    static ast::Access member_access(const Symbol* type, const Decl& mem) {
        if (type->kind != SymbolKind::class_) {
            return Access::public_;
        }
        return mem.access == Access::none ? Access::private_ : mem.access;
    }

    Symbol* add_member(Module& m, Symbol* type, const Decl& mem, bool from_header) {
        if (mem.kind == DeclKind::check) return nullptr;   // type-body checks are not members (spec 23)
        bool    is_fn = mem.kind == DeclKind::function;
        Symbol* sym   = m.make(is_fn ? SymbolKind::method : SymbolKind::field, mem.name, mem.name_span);
        sym->parent     = type;
        sym->ns         = type->ns;
        sym->decl       = &mem;
        sym->definition = from_header ? nullptr : &mem;
        sym->in_header  = from_header;
        sym->access     = member_access(type, mem);
        sym->is_static  = is_fn ? mem.as<FunctionDecl>().is_static
                                : mem.as<VariableDecl>().storage != Storage::local;
        type->members[mem.name].push_back(sym);
        type->member_order.push_back(sym);
        symbol_of_[&mem] = sym;
        return sym;
    }

    Symbol* header_item(Module& m, const std::string& name, SymbolKind kind) {
        auto it = m.items.find(name);
        if (it == m.items.end()) return nullptr;
        for (Symbol* s : it->second) {
            if (s->in_header && s->kind == kind) return s;
        }
        return nullptr;
    }

    std::vector<Symbol*> header_items(Module& m, const std::string& name) {
        std::vector<Symbol*> out;
        if (auto it = m.items.find(name); it != m.items.end()) {
            for (Symbol* s : it->second) {
                if (s->in_header) out.push_back(s);
            }
        }
        return out;
    }

    void not_in_header(const Decl& d, std::string_view what) {
        Diagnostic& e = error(DiagCode::E3017, d.name_span,
                              std::format("{} '{}' is not declared in the header", what, d.name),
                              "not in the .edh");
        note(e, "mark it 'private' if it is local to this module, or declare it in the .edh");
    }

    // Header and source versions of one function must agree on everything but the body.
    void check_signature(const FunctionDecl& header, const FunctionDecl& source) {
        std::vector<std::string> diffs;
        if (result_sig(header) != result_sig(source)) {
            diffs.push_back(std::format("return type '{}' vs '{}'", result_sig(header), result_sig(source)));
        }
        if (header.is_static != source.is_static) diffs.push_back("'static'");
        if (header.is_virtual != source.is_virtual) diffs.push_back("'virtual'");
        if (header.is_override != source.is_override) diffs.push_back("'override'");
        if (header.is_compile_time != source.is_compile_time) diffs.push_back("'compile_time'");
        if (header.is_unsafe != source.is_unsafe) diffs.push_back("'unsafe'");
        if (template_sig(header) != template_sig(source)) diffs.push_back("template parameters");
        if (diffs.empty()) return;
        std::string list;
        for (std::size_t i = 0; i < diffs.size(); ++i) list += (i ? ", " : "") + diffs[i];
        Diagnostic& e = error(DiagCode::E3014, source.name_span,
                              std::format("'{}' differs from its header declaration: {}", source.name, list),
                              "differs from the header");
        note(e, "declared in the header here", header.name_span);
    }

    void merge_source_item(Module& m, const Decl& d) {
        switch (d.kind) {
        case DeclKind::import:
        case DeclKind::check: return;
        case DeclKind::function: {
            const auto&          fn    = d.as<FunctionDecl>();
            std::vector<Symbol*> cands = header_items(m, item_key(d));
            std::erase_if(cands, [](Symbol* s) { return s->kind != SymbolKind::function; });
            auto match = std::find_if(cands.begin(), cands.end(), [&](Symbol* s) {
                return params_sig(*s->function()) == params_sig(fn);
            });
            if (match != cands.end()) {
                Symbol* s = *match;
                if (s->definition) {
                    note(error(DiagCode::E3010, d.name_span, std::format("'{}' is defined twice", d.name), "second definition"),
                         "first definition here", s->definition->name_span);
                    return;
                }
                check_signature(*s->function(), fn);
                s->definition  = &d;
                symbol_of_[&d] = s;
                return;
            }
            if (d.is_private) {
                add_item(m, d, false);
            } else if (!cands.empty()) {
                Diagnostic& e = error(DiagCode::E3014, d.name_span,
                                      std::format("'{}{}' does not match its header declaration", d.name, params_sig(fn)),
                                      "different parameters");
                note(e, std::format("the header declares '{}'", describe_fn(*cands.front())), cands.front()->span);
                for (Symbol* c : cands) mismatched_.insert(c);   // no extra "never defined"
            } else {
                not_in_header(d, "function");
            }
            return;
        }
        case DeclKind::struct_:
        case DeclKind::class_: {
            SymbolKind k    = d.kind == DeclKind::struct_ ? SymbolKind::struct_ : SymbolKind::class_;
            Symbol*    same = header_item(m, item_key(d), k);
            if (same) {
                merge_type(m, same, d.as<TypeDecl>());
                return;
            }
            std::vector<Symbol*> other = header_items(m, item_key(d));
            if (!other.empty()) {
                note(error(DiagCode::E3014, d.name_span,
                           std::format("'{}' is a {} here but a {} in the header", d.name,
                                       d.kind == DeclKind::struct_ ? "struct" : "class",
                                       sema::to_string(other.front()->kind)),
                           "different kind"),
                     "declared in the header here", other.front()->span);
                return;
            }
            if (d.is_private) add_item(m, d, false);
            else not_in_header(d, d.kind == DeclKind::struct_ ? "struct" : "class");
            return;
        }
        case DeclKind::interface:
        case DeclKind::enum_: {
            if (d.kind == DeclKind::interface) {   // the bodies of default methods (spec 07)
                if (Symbol* same = header_item(m, item_key(d), SymbolKind::interface)) {
                    merge_type(m, same, d.as<TypeDecl>());
                    return;
                }
            }
            std::vector<Symbol*> other = header_items(m, item_key(d));
            if (!other.empty()) {
                note(error(DiagCode::E3018, d.name_span,
                           std::format("'{}' is already complete in the header", d.name), "defined again"),
                     "enums and interfaces are written only in the .edh", other.front()->span);
                return;
            }
            if (d.is_private) add_item(m, d, false);
            else not_in_header(d, d.kind == DeclKind::enum_ ? "enum" : "interface");
            return;
        }
        case DeclKind::variable:
        case DeclKind::field: {
            std::vector<Symbol*> other = header_items(m, item_key(d));
            if (!other.empty()) {
                note(error(DiagCode::E3010, d.name_span, std::format("'{}' is already defined in the header", d.name),
                           "defined again"),
                     "header definition here", other.front()->span);
                return;
            }
            if (d.is_private) add_item(m, d, false);
            else not_in_header(d, "variable");
            return;
        }
        }
    }

    void merge_type(Module& m, Symbol* type, const TypeDecl& src) {
        symbol_of_[&src]   = type;
        type->source_block = &src;
        const auto& header = type->decl->as<TypeDecl>();
        if (template_sig(header) != template_sig(src)) {
            note(error(DiagCode::E3014, src.name_span,
                       std::format("template parameters of '{}' differ from the header", src.name), "differs"),
                 "declared in the header here", header.name_span);
        }
        if (!src.bases.empty() && bases_sig(src) != bases_sig(header)) {
            note(error(DiagCode::E3014, src.name_span, std::format("bases of '{}' differ from the header", src.name),
                       "differs"),
                 "declared in the header here", header.name_span);
        }
        bool is_struct = type->kind == SymbolKind::struct_ || type->kind == SymbolKind::interface;
        for (const DeclPtr& mem : src.members) {
            if (mem->kind == DeclKind::check) continue;
            Access access = member_access(type, *mem);
            std::vector<Symbol*> cands;
            if (auto it = type->members.find(mem->name); it != type->members.end()) {
                for (Symbol* s : it->second) {
                    if (s->in_header) cands.push_back(s);
                }
            }
            if (mem->kind == DeclKind::function) {
                const auto& fn = mem->as<FunctionDecl>();
                std::erase_if(cands, [](Symbol* s) { return s->kind != SymbolKind::method; });
                auto match = std::find_if(cands.begin(), cands.end(), [&](Symbol* s) {
                    return params_sig(*s->function()) == params_sig(fn);
                });
                if (match != cands.end()) {
                    Symbol* s = *match;
                    if (s->definition) {
                        note(error(DiagCode::E3010, mem->name_span,
                                   std::format("'{}::{}' is defined twice", type->name, mem->name), "second definition"),
                             "first definition here", s->definition->name_span);
                        continue;
                    }
                    if (!is_struct && s->access != access) {
                        note(error(DiagCode::E3014, mem->name_span,
                                   std::format("'{}::{}' is in a different access section than in the header",
                                               type->name, mem->name),
                                   "different access"),
                             "declared in the header here", s->span);
                    }
                    check_signature(*s->function(), fn);
                    s->definition    = mem.get();
                    symbol_of_[mem.get()] = s;
                } else if (!is_struct && access == Access::private_) {
                    add_member(m, type, *mem, false);
                } else if (!cands.empty()) {
                    Diagnostic& e = error(DiagCode::E3014, mem->name_span,
                                          std::format("'{}::{}{}' does not match its header declaration",
                                                      type->name, mem->name, params_sig(fn)),
                                          "different parameters");
                    note(e, std::format("the header declares '{}'", describe_fn(*cands.front())), cands.front()->span);
                    for (Symbol* c : cands) mismatched_.insert(c);
                } else {
                    Diagnostic& e = error(DiagCode::E3015, mem->name_span,
                                          std::format("'{}::{}' is not declared in the header", type->name, mem->name),
                                          "not in the .edh");
                    note(e, is_struct ? "declare every method of a struct in its .edh block"
                                      : "declare it under 'public:' in the .edh, or move it under 'private:'");
                }
                continue;
            }
            // Fields.
            if (is_struct) {
                note(error(DiagCode::E3016, mem->name_span,
                           std::format("struct '{}' cannot add field '{}' in its .ed block", type->name, mem->name),
                           "new field"),
                     "all fields of a struct are declared in the header");
            } else if (access == Access::private_) {
                add_member(m, type, *mem, false);
            } else if (!cands.empty()) {
                note(error(DiagCode::E3010, mem->name_span,
                           std::format("field '{}::{}' is already declared in the header", type->name, mem->name),
                           "declared again"),
                     "header declaration here", cands.front()->span);
            } else {
                note(error(DiagCode::E3015, mem->name_span,
                           std::format("public field '{}::{}' is not declared in the header", type->name, mem->name),
                           "not in the .edh"),
                     "public and protected members are declared in the .edh; private ones go under 'private:'");
            }
        }
    }

    static bool needs_body(const FunctionDecl& f) {
        return !f.is_extern && !f.is_defaulted && !f.is_abstract;
    }

    void check_definitions(Module& m) {
        auto check = [&](Symbol* s, std::string_view display) {
            const FunctionDecl* f = s->function();
            if (!f || !needs_body(*f)) return;
            if (s->in_header && !s->definition) {
                if (f->body || mismatched_.count(s)) return;   // already reported (E3011 / E3014)
                Diagnostic& e = error(DiagCode::E3013, s->span,
                                      std::format("'{}' is declared in the header but never defined", display),
                                      "no definition");
                note(e, m.source ? "define it in the module's .ed"
                                 : "this module has no .ed to define it in");
            } else if (!s->in_header && s->definition && !s->definition->as<FunctionDecl>().body) {
                error(DiagCode::E3013, s->span, std::format("'{}' has no body", display), "add '{ ... }'");
            }
        };
        for (Symbol* s : m.item_order) {
            check(s, s->name);
            if (s->kind == SymbolKind::struct_ || s->kind == SymbolKind::class_) {
                for (Symbol* mem : s->member_order) {
                    check(mem, s->name + "::" + mem->name);
                }
            }
        }
    }

    void check_duplicates(const std::map<std::string, std::vector<Symbol*>>& table, const Symbol* owner) {
        for (const auto& [name, syms] : table) {
            for (std::size_t i = 1; i < syms.size(); ++i) {
                for (std::size_t j = 0; j < i; ++j) {
                    const Symbol* a = syms[j];
                    const Symbol* b = syms[i];
                    bool overloadable = (a->kind == SymbolKind::function || a->kind == SymbolKind::method) &&
                                        a->kind == b->kind;
                    if (overloadable && params_sig(*a->function()) != params_sig(*b->function())) {
                        continue;
                    }
                    std::string display = owner ? owner->name + "::" + name : name;
                    Diagnostic& e = error(DiagCode::E3010, b->span,
                                          overloadable ? std::format("'{}' is defined twice with the same parameters", display)
                                                       : std::format("'{}' is defined more than once", display),
                                          "duplicate");
                    note(e, "first definition here", a->span);
                    break;
                }
            }
        }
    }

    // --- file context and template parameters --------------------------------------------

    struct FileScope {
        Resolver& r;
        Module*     saved_module;
        bool        saved_header;
        std::string saved_ns;
        FileScope(Resolver& res, Module* m, bool header)
            : r(res), saved_module(res.module_), saved_header(res.in_header_), saved_ns(res.ns_) {
            r.module_    = m;
            r.in_header_ = header;
        }
        ~FileScope() {
            r.module_    = saved_module;
            r.in_header_ = saved_header;
            r.ns_        = saved_ns;
        }
        FileScope(const FileScope&)            = delete;
        FileScope& operator=(const FileScope&) = delete;
    };

    // Pushes a declaration's template parameters for the duration of a scope.
    struct TemplateScope {
        Resolver& r;
        bool      pushed = false;
        TemplateScope(Resolver& res, const Decl& d, bool resolve_constraints = true) : r(res) {
            if (!d.is_template) return;
            pushed = true;
            r.template_params_.emplace_back();
            d.template_param_symbols.clear();
            for (const TemplateParam& p : d.template_params) {
                Symbol* s = r.module_->make(p.is_type ? SymbolKind::type_param : SymbolKind::value_param, p.name, p.span);
                s->value_type = p.value_type.get();
                s->template_param = &p;
                r.template_params_.back().push_back(s);
                d.template_param_symbols.push_back(s);
            }
            // Constraints and value types may refer to the parameters themselves.
            if (!resolve_constraints) return;
            for (const TemplateParam& p : d.template_params) {
                for (const TypePtr& c : p.constraints) r.resolve_type(*c);
                if (p.value_type) r.resolve_type(*p.value_type);
                if (p.default_type) r.resolve_type(*p.default_type);
                if (p.default_value) r.resolve_expr(*p.default_value);
            }
        }
        ~TemplateScope() {
            if (pushed) r.template_params_.pop_back();
        }
        TemplateScope(const TemplateScope&)            = delete;
        TemplateScope& operator=(const TemplateScope&) = delete;
    };

    Symbol* find_template_param(const std::string& name) {
        for (auto it = template_params_.rbegin(); it != template_params_.rend(); ++it) {
            for (Symbol* s : *it) {
                if (s->name == name) return s;
            }
        }
        return nullptr;
    }

    // --- namespaces (spec 30) ------------------------------------------------------------

    // The modules whose declarations this file can name: itself, its imports, and the standard
    // library's modules after `import std;`. A module is not a namespace: their names are found
    // under the namespaces they declare.
    std::vector<Module*> visible_modules() const {
        std::vector<Module*> out{module_};
        auto add = [&](Module* m) {
            if (std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
        };
        for (Module* m : module_->header_imports) add(m);
        if (!in_header_) {
            for (Module* m : module_->source_imports) add(m);
        }
        if (std_visible()) {
            for (Module* m : std_modules_) add(m);
        }
        // build.ed is imported by every target of the project (its names are in namespace build).
        if (auto b = program_.modules.find("build"); b != program_.modules.end() && !module_->name.starts_with("std")) {
            add(b->second.get());
        }
        return out;
    }

    struct Found {
        std::vector<Symbol*> symbols;          // visible declarations called `key`
        bool                 hidden = false;   // a private one of another module exists
    };

    Found find_items(const std::string& key) const {
        Found f;
        for (Module* m : visible_modules()) {
            auto it = m->items.find(key);
            if (it == m->items.end()) continue;
            for (Symbol* s : it->second) {
                if (m != module_ && s->is_private) f.hidden = true;
                else f.symbols.push_back(s);
            }
        }
        return f;
    }

    // A loaded module that declares `key` but is not imported by this file (for E3023), or null.
    const Module* unimported_owner(const std::string& key) const {
        std::vector<Module*> visible = visible_modules();
        for (auto& [name, m] : program_.modules) {
            if (std::find(visible.begin(), visible.end(), m.get()) != visible.end()) continue;
            if (auto it = m->items.find(key); it != m->items.end() && !it->second.front()->is_private) return m.get();
        }
        return nullptr;
    }

    bool report_unimported(const std::string& key, Span span) {
        const Module* owner = unimported_owner(key);
        if (!owner) return false;
        note(error(DiagCode::E3023, span, std::format("'{}' is declared in module '{}', which this file does not import", key, owner->name),
                   "not imported"),
             std::format("add 'import {};'", owner->name));
        return true;
    }

    bool namespace_visible(const std::string& ns) const {
        if (ns.empty()) return true;
        for (Module* m : visible_modules()) {
            if (m->namespaces.count(ns)) return true;
        }
        return false;
    }

    // The names declared directly in `ns` that this file can see.
    std::vector<std::string> names_in(const std::string& ns) const {
        std::vector<std::string> out;
        std::string prefix = ns.empty() ? "" : ns + "::";
        for (Module* m : visible_modules()) {
            for (const auto& [key, syms] : m->items) {
                if (!key.starts_with(prefix) || key.find("::", prefix.size()) != std::string::npos) continue;
                if (m != module_ && syms.front()->is_private) continue;
                out.push_back(key.substr(prefix.size()));
            }
            for (const std::string& n : m->namespaces) {
                if (n.starts_with(prefix) && n.size() > prefix.size() && n.find("::", prefix.size()) == std::string::npos) {
                    out.push_back(n.substr(prefix.size()));
                }
            }
        }
        return out;
    }

    // `namespace vk = gfx::vulkan;` of the current file, visible in the block it is written in.
    const ast::NamespaceAlias* find_alias(const std::string& name) const {
        const auto& f = in_header_ ? module_->header : module_->source;
        if (!f) return nullptr;
        for (const ast::NamespaceAlias& a : f->ast.aliases) {
            if (a.name == name && (a.ns.empty() || ns_ == a.ns || ns_.starts_with(a.ns + "::"))) return &a;
        }
        return nullptr;
    }

    // Reports one name declared in one namespace by two different modules (once per name).
    void check_one_module(const std::vector<Symbol*>& syms, const std::string& key, Span use) {
        bool all_functions = std::all_of(syms.begin(), syms.end(), [](const Symbol* s) { return s->kind == SymbolKind::function; });
        if (all_functions) return;   // overloads may come from several modules of the namespace
        for (Symbol* s : syms) {
            if (s->module == syms.front()->module) continue;
            if (!clash_reported_.insert(key).second) return;
            Diagnostic& e = error(DiagCode::E3065, use,
                                  std::format("'{}' is declared by two modules, '{}' and '{}'", key,
                                              syms.front()->module->name, s->module->name),
                                  "declared twice");
            note(e, "one declaration", syms.front()->span);
            note(e, "the other declaration", s->span);
            note(e, "a name in a namespace comes from one module; rename one of them or move it into another namespace");
            return;
        }
    }

    struct Unqualified {
        Symbol* symbol       = nullptr;
        bool    reported     = false;   // an ambiguity was reported
        bool    is_namespace = false;   // the name is a namespace (or an alias of one)
        bool    hidden       = false;
    };

    // An unqualified name: the enclosing namespaces from the innermost out to the global one.
    // A name found at more than one level is ambiguous (spec 30, rule 3).
    Unqualified lookup_unqualified(const std::string& name, Span span, bool global, bool report) {
        Unqualified out;
        std::vector<std::string> levels = global ? std::vector<std::string>{""} : enclosing_levels(ns_);
        std::vector<std::pair<std::string, std::vector<Symbol*>>> hits;
        for (const std::string& level : levels) {
            std::string key = join_ns(level, name);
            Found       f   = find_items(key);
            out.hidden      = out.hidden || f.hidden;
            if (!f.symbols.empty()) {
                if (report) check_one_module(f.symbols, key, span);
                hits.emplace_back(key, std::move(f.symbols));
            }
            if (namespace_visible(key)) out.is_namespace = true;
        }
        if (!global && find_alias(name)) out.is_namespace = true;
        if (hits.size() > 1) {
            if (report) {
                Diagnostic& e = error(DiagCode::E3060, span,
                                      std::format("'{}' is both {}; write the one you mean", name,
                                                  [&] {
                                                      std::string list;
                                                      for (std::size_t i = 0; i < hits.size(); ++i) {
                                                          list += (i == 0 ? "" : i + 1 == hits.size() ? " and " : ", ") +
                                                                  ("'" + global_display(hits[i].first) + "'");
                                                      }
                                                      return list;
                                                  }()),
                                      "ambiguous");
                for (const auto& [key, syms] : hits) note(e, std::format("'{}' is declared here", global_display(key)), syms.front()->span);
                note(e, "an unqualified name is searched in every enclosing namespace; one found at two levels is an error, never silently the inner one");
            }
            out.reported = true;
            return out;
        }
        if (!hits.empty()) out.symbol = hits.front().second.front();
        return out;
    }

    // Checks the file's namespace aliases: they name a visible namespace and shadow nothing.
    void check_aliases() {
        const auto& f = in_header_ ? module_->header : module_->source;
        if (!f) return;
        std::set<std::string> seen;
        for (const ast::NamespaceAlias& a : f->ast.aliases) {
            std::string saved = ns_;
            ns_               = a.ns;
            std::string target;
            for (const PathSegment& seg : a.target.segments) target = join_ns(target, seg.name);
            if (!seen.insert(a.name).second) {
                error(DiagCode::E3064, a.span, std::format("alias '{}' is declared twice in this file", a.name), "declared again");
            } else if (!namespace_visible(target)) {
                Found       it = find_items(target);
                Diagnostic& e  = error(DiagCode::E3064, a.target.span,
                                       it.symbols.empty() ? std::format("'{}' is not a namespace this file can see", target)
                                                          : std::format("'{}' is a {}, not a namespace", target,
                                                                        sema::to_string(it.symbols.front()->kind)),
                                       "not a namespace");
                note(e, it.symbols.empty() ? "an alias names a namespace declared by this file or a module it imports"
                                           : "aliases shorten namespaces; a type alias is 'compile_time type name = T;' (spec 26)");
            } else {
                for (const std::string& level : enclosing_levels(a.ns)) {
                    std::string key = join_ns(level, a.name);
                    Found       it  = find_items(key);
                    if (!it.symbols.empty() || namespace_visible(key)) {
                        Diagnostic& e = error(DiagCode::E3064, a.span,
                                              std::format("alias '{}' has the same name as {} '{}'", a.name,
                                                          it.symbols.empty() ? "the namespace" : sema::to_string(it.symbols.front()->kind),
                                                          key),
                                              "shadows a visible name");
                        if (!it.symbols.empty()) note(e, "declared here", it.symbols.front()->span);
                        note(e, "aliases never shadow: choose another name");
                        break;
                    }
                }
            }
            ns_ = saved;
        }
    }

    // Program-wide namespace rules (spec 30, rules 1 and 6).
    void check_namespaces() {
        std::map<std::string, const Module*> declared;   // every namespace, by one module declaring it
        for (auto& [name, m] : program_.modules) {
            for (const std::string& n : m->namespaces) declared.emplace(n, m.get());
        }
        // Each top-level namespace belongs to one project (spec 30, rule 6).
        std::map<std::string, std::pair<const Module*, Span>> owner;
        for (auto& [name, m] : program_.modules) {
            if (m->project == "std") continue;
            for (const std::optional<sema::ModuleFile>* f : {&m->header, &m->source}) {
                if (!*f) continue;
                for (const ast::NamespaceBlock& b : (*f)->ast.namespaces) {
                    std::string top = b.path.substr(0, b.path.find("::"));
                    if (top == "std") continue;
                    auto [it, fresh] = owner.emplace(top, std::make_pair(m.get(), b.span));
                    if (fresh || it->second.first->project == m->project) continue;
                    auto project_name = [](const Module* x) { return x->project.empty() ? std::string("this program") : "'" + x->project + "'"; };
                    Diagnostic& e = error(DiagCode::E3061, b.span,
                                          std::format("namespace '{}' is declared by two projects: {} and {}", top,
                                                      project_name(it->second.first), project_name(m.get())),
                                          "another project's namespace");
                    note(e, "declared here first", it->second.second);
                    note(e, "a top-level namespace belongs to one project; rename one of them");
                }
            }
        }
        for (auto& [name, m] : program_.modules) {
            bool is_std = name == "std" || name.starts_with("std::");
            if (!is_std && !m->project.empty()) {   // a library: everything in a namespace (spec 30)
                for (Symbol* s : m->item_order) {
                    if (!s->ns.empty() || s->is_private) continue;
                    note(error(DiagCode::E3063, s->span,
                               std::format("library module '{}' declares '{}' in the global namespace", m->name, s->name),
                               "not in a namespace"),
                         "a library puts its names in a namespace of its own: 'namespace name { ... }' (spec 30)");
                }
            }
            for (const std::optional<sema::ModuleFile>* f : {&m->header, &m->source}) {
                if (!*f) continue;
                for (const ast::NamespaceBlock& b : (*f)->ast.namespaces) {
                    if (!is_std && (b.path == "std" || b.path.starts_with("std::"))) {
                        note(error(DiagCode::E3061, b.span,
                                   "namespace 'std' belongs to the standard library; declarations can't be added to it",
                                   "not this project's namespace"),
                             "put your declarations in a namespace of your own");
                    }
                }
            }
            for (Symbol* s : m->item_order) {
                if (is_std && s->ns.empty()) {
                    error(DiagCode::E3063, s->span,
                          std::format("the standard library declares '{}' in the global namespace", s->name),
                          "not in a namespace");
                }
                if (auto it = declared.find(s->key()); it != declared.end()) {
                    Diagnostic& e = error(DiagCode::E3062, s->span,
                                          std::format("'{}' is both a namespace and a {}", s->key(), sema::to_string(s->kind)),
                                          "also a namespace");
                    note(e, std::format("module '{}' declares the namespace '{}'", it->second->name, s->key()));
                    note(e, "a name is either a namespace or a declaration in its parent, never both");
                }
            }
        }
    }

    bool std_visible() const {
        return module_->header_imports_std || (!in_header_ && module_->source_imports_std);
    }

    // --- annotations (spec 29) ---------------------------------------------------------------

    struct AnnotationContext {
        Symbol* value  = nullptr;   // `annotation`: the constant holding this use's value
        Symbol* type   = nullptr;   // `annotated` when it is a type
        Symbol* handle = nullptr;   // `annotated` when it is a field, function, parameter or alternative
    };

    struct GeneratedOrigin {
        Module*     module = nullptr;   // where the annotation is declared
        bool        header = false;
        std::string ns;
        Symbol*     value  = nullptr;
    };

    static std::string_view target_word(AnnotationTarget t) {
        switch (t) {
        case AnnotationTarget::type: return "type";
        case AnnotationTarget::field: return "field";
        case AnnotationTarget::function: return "function";
        case AnnotationTarget::parameter: return "parameter";
        case AnnotationTarget::alternative: return "alternative";
        case AnnotationTarget::other: return "other";
        case AnnotationTarget::file: return "file";
        }
        return "";
    }

    static std::string_view target_display(AnnotationTarget t) {
        switch (t) {
        case AnnotationTarget::type: return "a type";
        case AnnotationTarget::field: return "a field";
        case AnnotationTarget::function: return "a function";
        case AnnotationTarget::parameter: return "a parameter";
        case AnnotationTarget::alternative: return "an enum alternative";
        case AnnotationTarget::other: return "a variable";
        case AnnotationTarget::file: return "a file";
        }
        return "";
    }

    void process_annotations(Module& m) {
        for (bool header : {true, false}) {
            auto& f = header ? m.header : m.source;
            if (!f) continue;
            FileScope fs(*this, &m, header);
            if (!f->ast.annotations.empty()) {   // `@a{ ... };` (spec 31): only in a .ed
                ns_ = "";
                if (header) {
                    note(error(DiagCode::E3073, f->ast.annotations.front().span,
                               "a file annotation describes the code of a .ed file, not a header", "in a header"),
                         "write it at the top of the .ed file");
                } else {
                    for (AnnotationUse& u : f->ast.annotations) {
                        if (Symbol* a = use_annotation(u, AnnotationTarget::file)) add_checks(u, a, AnnotationTarget::file, nullptr, nullptr, 0);
                    }
                }
            }
            for (DeclPtr& d : f->ast.decls) {
                ns_        = d->ns;
                auto    it = symbol_of_.find(d.get());
                Symbol* s  = it == symbol_of_.end() ? nullptr : it->second;
                // The .ed part of something declared in the header: annotations belong to the
                // declaration (spec 29).
                bool part = !header && m.header && s && s->decl != d.get();
                if (part) {
                    reject_annotations_in_source(*d);
                    continue;
                }
                switch (d->kind) {
                case DeclKind::function: annotate_function(*d, s, nullptr); break;
                case DeclKind::struct_:
                case DeclKind::class_:
                case DeclKind::interface:
                case DeclKind::enum_:
                    if (s) annotate_type(m, *d, s, header);
                    break;
                case DeclKind::variable:
                    for (AnnotationUse& u : d->annotations) use_annotation(u, AnnotationTarget::other);
                    break;
                default: break;
                }
            }
        }
    }

    void reject_annotations_in_source(Decl& d) {
        auto reject = [&](std::vector<AnnotationUse>& uses) {
            if (uses.empty()) return;
            note(error(DiagCode::E3073, uses.front().span, "annotations are written on the declaration in the header",
                       "not in the .ed"),
                 "the .ed doesn't repeat them (spec 29)");
        };
        reject(d.annotations);
        if (d.kind == DeclKind::function) {
            for (Param& p : d.as<FunctionDecl>().params) reject(p.annotations);
        }
        if (d.kind == DeclKind::struct_ || d.kind == DeclKind::class_) {
            for (DeclPtr& mem : d.as<TypeDecl>().members) {
                auto it = symbol_of_.find(mem.get());
                // private members added in the .ed are declared there
                if (it != symbol_of_.end() && it->second->decl == mem.get()) {
                    annotate_member(*mem, it->second);
                } else {
                    reject(mem->annotations);
                    if (mem->kind == DeclKind::function) {
                        for (Param& p : mem->as<FunctionDecl>().params) reject(p.annotations);
                    }
                }
            }
        }
    }

    // A type's annotations, its generated members, and the annotations of its members.
    void annotate_type(Module& m, Decl& d, Symbol* type, bool header) {
        std::set<const Symbol*> seen;
        for (AnnotationUse& u : d.annotations) {
            Symbol* a = use_annotation(u, AnnotationTarget::type);
            if (!a) continue;
            if (!seen.insert(a).second) {
                duplicate(u);
                continue;
            }
            add_checks(u, a, AnnotationTarget::type, type, type, 0);
            if (a->decl->as<TypeDecl>().generate) generate_members(m, d, type, u, a, header);
        }
        if (d.kind == DeclKind::enum_) {
            for (EnumAlternative& alt : d.as<EnumDecl>().alternatives) {
                std::set<const Symbol*> alt_seen;
                Symbol* as = nullptr;
                if (auto it = type->members.find(alt.name); it != type->members.end()) as = it->second.front();
                for (AnnotationUse& u : alt.annotations) {
                    Symbol* a = use_annotation(u, AnnotationTarget::alternative);
                    if (!a) continue;
                    if (!alt_seen.insert(a).second) {
                        duplicate(u);
                        continue;
                    }
                    if (as) add_checks(u, a, AnnotationTarget::alternative, type, as, 0);
                }
            }
            return;
        }
        for (DeclPtr& mem : d.as<TypeDecl>().members) {
            if (mem->generated_by) continue;
            auto it = symbol_of_.find(mem.get());
            annotate_member(*mem, it == symbol_of_.end() ? nullptr : it->second);
        }
    }

    void annotate_member(Decl& mem, Symbol* s) {
        if (mem.kind == DeclKind::function) {
            annotate_function(mem, s, s ? s->parent : nullptr);
            return;
        }
        if (mem.kind != DeclKind::field) return;
        std::set<const Symbol*> seen;
        for (AnnotationUse& u : mem.annotations) {
            Symbol* a = use_annotation(u, AnnotationTarget::field);
            if (!a) continue;
            if (!seen.insert(a).second) {
                duplicate(u);
                continue;
            }
            if (s) add_checks(u, a, AnnotationTarget::field, s->parent, s, 0);
        }
    }

    void annotate_function(Decl& d, Symbol* s, const Symbol* owner) {
        std::set<const Symbol*> seen;
        for (AnnotationUse& u : d.annotations) {
            Symbol* a = use_annotation(u, AnnotationTarget::function);
            if (!a) continue;
            if (!seen.insert(a).second) {
                duplicate(u);
                continue;
            }
            if (s) add_checks(u, a, AnnotationTarget::function, owner ? owner : s, s, 0);
        }
        auto& params = d.as<FunctionDecl>().params;
        for (std::size_t i = 0; i < params.size(); ++i) {
            std::set<const Symbol*> pseen;
            for (AnnotationUse& u : params[i].annotations) {
                Symbol* a = use_annotation(u, AnnotationTarget::parameter);
                if (!a) continue;
                if (!pseen.insert(a).second) {
                    duplicate(u);
                    continue;
                }
                if (s) add_checks(u, a, AnnotationTarget::parameter, owner ? owner : s, s, i);
            }
        }
    }

    void duplicate(const AnnotationUse& u) {
        note(error(DiagCode::E3072, u.span, std::format("'@{}' is written twice here", ast::to_string(u.path)), "again"),
             "an annotation stands at most once on a declaration");
    }

    // Resolves the annotation a use names, checks where it is written, and makes the constant
    // that holds its value (resolved here, where the values are written).
    Symbol* use_annotation(AnnotationUse& u, AnnotationTarget target) {
        if (u.symbol) return u.symbol;   // done already (a type and its header/source)
        Symbol* a = lookup_path(u.path, Want::type, u.path.span);
        if (!a) return nullptr;
        if (a->kind != SymbolKind::struct_ || !a->decl->as<TypeDecl>().is_annotation) {
            note(error(DiagCode::E3070, u.path.span, std::format("'{}' is not an annotation", ast::to_string(u.path)),
                       "not an annotation"),
                 "an annotation is declared as 'annotation(std::meta::target::...) struct name { ... }' (spec 29)");
            return nullptr;
        }
        std::vector<std::string> allowed;
        bool                     ok = false;
        for (const Path& p : a->decl->as<TypeDecl>().annotation_targets) {
            std::string word = p.segments.empty() ? "" : p.segments.back().name;
            allowed.push_back(word);
            ok = ok || word == target_word(target);
        }
        if (!ok) {
            std::string list;
            for (std::size_t i = 0; i < allowed.size(); ++i) list += (i ? ", " : "") + allowed[i];
            note(error(DiagCode::E3071, u.path.span,
                       std::format("'@{}' can't be written on {}", ast::to_string(u.path), target_display(target)),
                       "wrong target"),
                 std::format("its targets are: {}", list));
            return nullptr;
        }
        u.symbol = a;
        // `compile_time A annotation = A{ ... };`
        auto vd       = std::make_unique<VariableDecl>(DeclKind::variable, u.span);
        vd->name      = "annotation";
        vd->name_span = u.span;
        vd->ns        = ns_;
        vd->storage   = Storage::compile_time;
        Path tp;
        tp.span = u.path.span;
        for (const PathSegment& seg : u.path.segments) {
            PathSegment c;
            c.name = seg.name;
            c.span = seg.span;
            tp.segments.push_back(std::move(c));
        }
        tp.global      = u.path.global;
        auto type      = std::make_unique<NamedType>(u.path.span, std::move(tp));
        type->symbol   = a;
        vd->type       = std::move(type);
        vd->init       = std::move(u.value);
        resolve_expr(*vd->init);
        Symbol* v      = module_->make(SymbolKind::global, "annotation", u.span);
        v->decl        = vd.get();
        v->definition  = vd.get();
        v->ns          = ns_;
        v->is_private  = true;
        u.value_symbol = v;
        program_.cloned_decls.push_back(std::move(vd));
        return a;
    }

    // The file an annotation is declared in: its code (checks, generated members) is resolved there.
    static bool declared_in_header(const Symbol* a) { return a->in_header; }

    // Copies the checks of annotation `a` for one use, resolved with `annotation` and `annotated`.
    void add_checks(AnnotationUse& u, Symbol* a, AnnotationTarget target, const Symbol* owner, const Symbol* member,
                    std::size_t param_index) {
        const auto& ad = a->decl->as<TypeDecl>();
        // Checks of an annotated template run per instance; the generic template has none.
        for (const Symbol* o = owner; o; o = o->parent) {
            if (o->decl && o->decl->is_template && !o->template_of) return;
        }
        std::vector<const Decl*> checks;   // in the declaration, and in the .ed block of a header's annotation
        for (const DeclPtr& mem : ad.members) checks.push_back(mem.get());
        if (a->source_block) {
            for (const DeclPtr& mem : a->source_block->as<TypeDecl>().members) checks.push_back(mem.get());
        }
        for (const Decl* mem : checks) {
            if (mem->kind != DeclKind::check) continue;
            const auto& original = mem->as<CheckDecl>();
            auto        copy     = std::make_unique<CheckDecl>(original.span);
            copy->name           = original.name;
            copy->name_span      = original.name_span;
            ast::Cloner cl([&](const Symbol& o) { return a->module->make(o.kind, o.name, o.span); });
            const Check& k        = original.check;
            Check&       c        = copy->check;
            c.is_compile_time     = k.is_compile_time;
            c.is_run_time         = k.is_run_time;
            c.compile_modes       = k.compile_modes;
            c.run_modes           = k.run_modes;
            c.compile_modes_span  = k.compile_modes_span;
            c.run_modes_span      = k.run_modes_span;
            if (k.condition) c.condition = cl.expr(*k.condition);
            if (k.message) c.message = cl.expr(*k.message);
            if (k.block) c.block = cl.block(*k.block);
            AnnotationCheck ac;
            ac.target      = target;
            ac.check       = copy.get();
            ac.use         = &u;
            ac.origin      = a->module;
            ac.owner       = owner;
            ac.member      = member;
            ac.param_index = param_index;
            AnnotationContext ctx{u.value_symbol, nullptr, nullptr};
            if (target == AnnotationTarget::type) {
                ctx.type = const_cast<Symbol*>(owner);
            } else {
                static const char* spelling[] = {"", "field", "function", "parameter", "alternative", "", "file"};
                auto hd       = std::make_unique<VariableDecl>(DeclKind::variable, u.span);
                hd->name      = "annotated";
                hd->name_span = u.span;
                hd->storage   = Storage::compile_time;
                Path hp;
                hp.span = u.span;
                for (std::string_view part : {std::string_view("std"), std::string_view("meta"),
                                              std::string_view(spelling[static_cast<int>(target)])}) {
                    PathSegment ps;
                    ps.name = std::string(part);
                    ps.span = u.span;
                    hp.segments.push_back(std::move(ps));
                }
                hd->type     = std::make_unique<NamedType>(u.span, std::move(hp));
                Symbol* h    = a->module->make(SymbolKind::global, "annotated", u.span);
                h->decl      = hd.get();
                h->is_private = true;
                ac.handle    = h;
                ctx.handle   = h;
                program_.cloned_decls.push_back(std::move(hd));
            }
            {
                FileScope fs(*this, a->module, declared_in_header(a) && a->module->header.has_value());
                ns_                                    = a->ns;
                std::optional<AnnotationContext> saved = actx_;
                actx_                                  = ctx;
                FunctionContext saved_fn               = fn_;
                fn_                                    = FunctionContext{};
                declared_later_.clear();
                if (c.block) collect_declarations(*c.block);
                scopes_.emplace_back();
                resolve_check(c);
                scopes_.pop_back();
                fn_   = saved_fn;
                actx_ = saved;
            }
            program_.annotation_checks.push_back(ac);
            program_.cloned_decls.push_back(std::move(copy));
        }
    }

    // Adds the members of `a`'s generate block to the annotated type, as if its author had
    // written them (spec 29). Interfaces go into the base list.
    void generate_members(Module& m, Decl& d, Symbol* type, AnnotationUse& u, Symbol* a, bool header) {
        if (d.kind == DeclKind::enum_ || d.kind == DeclKind::interface) {
            if (d.kind == DeclKind::interface) {
                note(error(DiagCode::E3074, u.span, std::format("'@{}' generates members, but an interface has no method bodies",
                                                                ast::to_string(u.path)),
                           "on an interface"),
                     "write it on a struct, class or enum");
                return;
            }
        }
        const GenerateBlock& g      = *a->decl->as<TypeDecl>().generate;
        bool                 origin_header = declared_in_header(a) && a->module->header.has_value();
        ast::Cloner          cl([&](const Symbol& o) { return a->module->make(o.kind, o.name, o.span); });
        if (d.kind == DeclKind::enum_) {
            if (a->key() == "std::derive::formattable") return;   // the compiler prints such enums (spec 29)
            note(error(DiagCode::E3074, u.span, std::format("'@{}' generates methods, but enums have no methods",
                                                            ast::to_string(u.path)),
                       "on an enum"),
                 "write it on a struct or class");
            return;
        }
        auto& td = d.as<TypeDecl>();
        for (const TypePtr& base : g.bases) {
            TypePtr copy = cl.type(*base);
            {
                FileScope fs(*this, a->module, origin_header);
                ns_ = a->ns;
                resolve_type(*copy);
            }
            pre_resolved_.insert(copy.get());
            td.bases.push_back(std::move(copy));
        }
        for (const DeclPtr& mem : g.members) {
            std::unique_ptr<FunctionDecl> fn = cl.function(mem->as<FunctionDecl>());
            fn->access       = d.kind == DeclKind::struct_ ? Access::none : (mem->access == Access::none ? Access::public_ : mem->access);
            fn->generated_by = &u;
            fn->ns           = d.ns;
            // Same signature as a written member, or as another generated one: an error naming both.
            if (auto it = type->members.find(fn->name); it != type->members.end()) {
                for (Symbol* other : it->second) {
                    const FunctionDecl* of = other->function();
                    if (!of || params_sig(*of) != params_sig(*fn)) continue;
                    Diagnostic& e = error(DiagCode::E3075, u.span,
                                          std::format("'@{}' generates '{}', which '{}' already has", ast::to_string(u.path),
                                                      describe_generated(*fn), type->name),
                                          "conflicting member");
                    note(e, other->decl->generated_by ? "also generated here" : "written here",
                         other->decl->generated_by ? other->decl->generated_by->span : other->span);
                    fn.reset();
                    break;
                }
                if (!fn) continue;
            }
            Symbol* sym     = add_member(m, type, *fn, header);
            sym->definition = fn.get();
            generated_[fn.get()] = GeneratedOrigin{a->module, origin_header, a->ns, u.value_symbol};
            td.members.push_back(std::move(fn));
        }
    }

    static std::string describe_generated(const FunctionDecl& f) { return f.name + params_sig(f); }

    // --- phase B and C: bases and class hierarchies --------------------------------------

    void resolve_bases(Module& m) {
        for (Symbol* t : m.item_order) {
            if (t->kind != SymbolKind::struct_ && t->kind != SymbolKind::class_ && t->kind != SymbolKind::interface) {
                continue;
            }
            FileScope     fs(*this, &m, t->in_header);
            ns_ = t->ns;
            TemplateScope ts(*this, *t->decl, /*resolve_constraints=*/false);   // phase D reports those
            for (const TypePtr& base : t->decl->as<TypeDecl>().bases) {
                if (!pre_resolved_.count(base.get())) resolve_type(*base);   // generated bases are resolved where written
                const Symbol* b = base->kind == TypeKind::named ? base->as<NamedType>().symbol : nullptr;
                if (b && b->is_type() && b->kind != SymbolKind::type_param) {
                    t->bases.push_back(const_cast<Symbol*>(b));
                } else if (!b || b->kind == SymbolKind::type_param) {
                    t->has_opaque_base = true;   // std:: types, template parameters, or already reported
                }
            }
        }
    }

    struct Inherited {
        std::vector<Symbol*> methods;   // every method of every ancestor
        bool                 opaque = false;
    };

    static void collect_inherited(const Symbol* t, Inherited& out, std::set<const Symbol*>& seen) {
        out.opaque = out.opaque || t->has_opaque_base;
        for (Symbol* b : t->bases) {
            if (!seen.insert(b).second) continue;
            for (Symbol* mem : b->member_order) {
                if (mem->kind == SymbolKind::method) out.methods.push_back(mem);
            }
            collect_inherited(b, out, seen);
        }
    }

    static bool is_dynamic(const Symbol* method) {
        const FunctionDecl* f = method->function();
        return method->parent->kind == SymbolKind::interface || f->is_virtual || f->is_abstract || f->is_override;
    }

    // Whether two written types are the same type: by the symbols they resolved to, so that
    // `wnd::event` (through a namespace alias) and `event` (inside the namespace) match.
    static bool same_type_expr(const TypeExpr& a, const TypeExpr& b) {
        if (a.kind == TypeKind::reference && b.kind == TypeKind::reference) {
            const auto& ra = a.as<ReferenceType>();
            const auto& rb = b.as<ReferenceType>();
            return ra.is_mut == rb.is_mut && ra.inner && rb.inner && same_type_expr(*ra.inner, *rb.inner);
        }
        if (a.kind == TypeKind::named && b.kind == TypeKind::named) {
            const auto& na = a.as<NamedType>();
            const auto& nb = b.as<NamedType>();
            if (na.symbol && nb.symbol && !na.path.segments.empty() && !nb.path.segments.empty()) {
                const PathSegment& la = na.path.segments.back();
                const PathSegment& lb = nb.path.segments.back();
                if (na.symbol != nb.symbol || la.template_args.size() != lb.template_args.size()) return false;
                for (std::size_t i = 0; i < la.template_args.size(); ++i) {
                    const TemplateArg& x = la.template_args[i];
                    const TemplateArg& y = lb.template_args[i];
                    if (x.is_mut != y.is_mut) return false;
                    if (x.type && y.type) {
                        if (!same_type_expr(*x.type, *y.type)) return false;
                    } else {
                        return false;      // value arguments: only an identical spelling matched (above)
                    }
                }
                return true;
            }
        }
        return to_string(a) == to_string(b);
    }

    // Resolves the parameter types of `method` where it is declared, ahead of the bodies phase,
    // so that signatures spelled differently (an alias, a qualified name) can be compared.
    // Errors are left to the bodies phase, which resolves them again and reports them once.
    void resolve_signature_early(const Symbol* method) {
        auto* f = const_cast<FunctionDecl*>(method->function());
        if (!f || !method->module) return;
        FileScope   fs(*this, method->module, method->in_header);
        ns_ = method->ns.empty() && method->parent ? method->parent->ns : method->ns;
        std::size_t mark = diags_.size();
        for (Param& p : f->params) {
            const TypeExpr* inner = p.type.get();
            while (inner && inner->kind == TypeKind::reference) inner = inner->as<ReferenceType>().inner.get();
            if (inner && inner->kind == TypeKind::named && !inner->as<NamedType>().symbol) resolve_type(*p.type);
        }
        (void)diags_.take_since(mark);
    }

    bool same_signature(const Symbol* a, const Symbol* b) {
        if (a->name != b->name) return false;
        if (params_sig(*a->function()) == params_sig(*b->function())) return true;
        if (a->function()->params.size() != b->function()->params.size()) return false;
        resolve_signature_early(a);
        resolve_signature_early(b);
        const FunctionDecl& fa = *a->function();
        const FunctionDecl& fb = *b->function();
        if (fa.params.size() != fb.params.size() || fa.is_mut != fb.is_mut || fa.is_move != fb.is_move) return false;
        for (std::size_t i = 0; i < fa.params.size(); ++i) {
            const Param& pa = fa.params[i];
            const Param& pb = fb.params[i];
            if (pa.is_implicit != pb.is_implicit || pa.is_compile_time != pb.is_compile_time) return false;
            if (!pa.type || !pb.type || !same_type_expr(*pa.type, *pb.type)) return false;
        }
        return true;
    }

    static bool is_ancestor(const Symbol* t, const Symbol* maybe_base) {
        for (const Symbol* b : t->bases) {
            if (b == maybe_base || is_ancestor(b, maybe_base)) return true;
        }
        return false;
    }

    void check_hierarchy(Module& m) {
        for (Symbol* t : m.item_order) {
            if (t->kind != SymbolKind::struct_ && t->kind != SymbolKind::class_ && t->kind != SymbolKind::interface) {
                continue;
            }
            check_bases(t);
            add_default_methods(m, t);
            check_methods(t);
        }
    }

    void check_bases(Symbol* t) {
        const auto& decl        = t->decl->as<TypeDecl>();
        int         class_bases = 0;
        for (std::size_t i = 0; i < decl.bases.size(); ++i) {
            const TypeExpr& base_expr = *decl.bases[i];
            const Symbol*   b = base_expr.kind == TypeKind::named ? base_expr.as<NamedType>().symbol : nullptr;
            if (!b || b->kind == SymbolKind::type_param) continue;
            if (t->kind == SymbolKind::struct_ || t->kind == SymbolKind::interface) {
                if (b->kind != SymbolKind::interface) {
                    note(error(DiagCode::E3046, base_expr.span,
                               std::format("{} '{}' can only extend interfaces; '{}' is a {}", sema::to_string(t->kind),
                                           t->name, b->name, sema::to_string(b->kind)),
                               "not an interface"),
                         t->kind == SymbolKind::struct_ ? "structs cannot inherit; use a class" : "interfaces extend interfaces");
                }
                continue;
            }
            if (b->kind == SymbolKind::class_) {
                ++class_bases;
                if (class_bases > 1) {
                    error(DiagCode::E3041, base_expr.span,
                          std::format("class '{}' has more than one base class", t->name), "second base class");
                } else if (i != 0) {
                    error(DiagCode::E3041, base_expr.span,
                          std::format("base class '{}' must be listed first", b->name), "move this first");
                }
                if (!b->is_open()) {
                    Diagnostic& e = error(DiagCode::E3040, base_expr.span,
                                          std::format("class '{}' is not open and cannot be inherited from", b->name),
                                          "not open");
                    note(e, std::format("declare it as 'open class {}'", b->name), b->span);
                }
            } else if (b->kind != SymbolKind::interface) {
                error(DiagCode::E3046, base_expr.span,
                      std::format("cannot inherit from {} '{}'", sema::to_string(b->kind), b->name),
                      "not a class or interface");
            }
        }
    }

    // The body of an interface method with a default (spec 07), or null.
    static const FunctionDecl* default_body(const Symbol* req) {
        if (req->parent->kind != SymbolKind::interface) return nullptr;
        if (req->definition && req->definition->as<FunctionDecl>().body) return &req->definition->as<FunctionDecl>();
        if (req->function() && req->function()->body) return req->function();
        return nullptr;
    }

    // Interface default methods (spec 07): a struct or class that doesn't implement such a
    // method gets a copy of the default as its own `override`, resolved where the interface is.
    void add_default_methods(Module& m, Symbol* t) {
        if (t->kind == SymbolKind::interface) return;
        Inherited               inh;
        std::set<const Symbol*> seen;
        collect_inherited(t, inh, seen);
        std::vector<Symbol*> chain{t};
        for (Symbol* b : inh.methods) {
            if (b->parent->kind == SymbolKind::class_ && std::find(chain.begin(), chain.end(), b->parent) == chain.end()) {
                chain.push_back(b->parent);
            }
        }
        std::map<std::string, Symbol*> chosen;   // signature → the interface method whose default is used
        for (Symbol* req : inh.methods) {
            const FunctionDecl* def = default_body(req);
            if (!def) continue;
            bool implemented = false;
            for (Symbol* c : chain) {
                for (Symbol* mem : c->member_order) {
                    if (mem->kind == SymbolKind::method && same_signature(mem, req) && !mem->function()->is_abstract) implemented = true;
                }
            }
            if (implemented) continue;
            std::string key = req->name + params_sig(*req->function());
            if (auto it = chosen.find(key); it != chosen.end()) {
                if (it->second->parent == req->parent) continue;
                Diagnostic& e = error(DiagCode::E3045, t->span,
                                      std::format("'{}' gets two defaults for '{}{}', from '{}' and '{}'", t->name,
                                                  req->name, params_sig(*req->function()), it->second->parent->name, req->parent->name),
                                      "override it");
                note(e, "one default here", it->second->span);
                note(e, "the other here", req->span);
                continue;
            }
            chosen[key] = req;
        }
        for (auto& [key, req] : chosen) {
            const FunctionDecl* def    = default_body(req);
            Module*             origin = req->parent->module;
            ast::Cloner         cl([&](const Symbol& o) { return origin->make(o.kind, o.name, o.span); });
            std::unique_ptr<FunctionDecl> fn = cl.function(*def);
            fn->access      = t->kind == SymbolKind::struct_ ? Access::none : Access::public_;
            fn->is_override = true;
            fn->is_virtual  = false;
            fn->is_abstract = false;
            fn->ns          = t->ns;
            Symbol* sym     = add_member(m, t, *fn, t->in_header);
            sym->definition = fn.get();
            bool header     = def == req->function() ? req->in_header : false;
            generated_[fn.get()] = GeneratedOrigin{origin, header, req->parent->ns, nullptr};
            Decl* owner_decl = const_cast<Decl*>(t->source_block ? t->source_block : static_cast<const Decl*>(t->decl));
            auto* owner      = &owner_decl->as<TypeDecl>();
            owner->members.push_back(std::move(fn));
        }
    }

    void check_methods(Symbol* t) {
        Inherited               inh;
        std::set<const Symbol*> seen;
        collect_inherited(t, inh, seen);
        bool open_class = t->is_open();

        for (Symbol* mem : t->member_order) {
            if (mem->kind != SymbolKind::method) continue;
            const FunctionDecl& f = *mem->function();
            if (f.is_destructor && f.name != "~" + t->name) {
                error(DiagCode::E3049, mem->span, std::format("destructor '{}' in class '{}'", f.name, t->name),
                      std::format("write '~{}'", t->name));
            }
            if ((f.is_virtual || f.is_abstract) && !open_class && t->kind != SymbolKind::interface) {
                Diagnostic& e = error(DiagCode::E3042, mem->span,
                                      std::format("'{}' is {} but '{}' is not an open class", mem->name,
                                                  f.is_abstract ? "abstract" : "virtual", t->name),
                                      "needs an open class");
                note(e, std::format("declare 'open class {}', or remove the keyword", t->name));
            }
            if (f.is_abstract) {
                const Decl* def = mem->definition;
                if (!f.is_virtual) {
                    error(DiagCode::E3048, mem->span, std::format("abstract method '{}' must be declared 'virtual'", mem->name),
                          "write 'virtual ... abstract'");
                }
                if (def && def->as<FunctionDecl>().body) {
                    error(DiagCode::E3048, mem->span, std::format("abstract method '{}' has a body", mem->name),
                          "abstract methods have no implementation");
                }
            }
            if (f.is_destructor) continue;

            std::vector<Symbol*> same_name;
            for (Symbol* b : inh.methods) {
                if (b->name == mem->name) same_name.push_back(b);
            }
            if (f.is_override) {
                bool ok = std::any_of(same_name.begin(), same_name.end(),
                                      [&](Symbol* b) { return is_dynamic(b) && same_signature(b, mem); });
                for (Symbol* b : same_name) {
                    if (!is_dynamic(b) || !same_signature(b, mem)) continue;
                    if (b->function()->is_compile_time != f.is_compile_time) {
                        Diagnostic& e = error(DiagCode::E3051, mem->span,
                                              std::format("'{}' must {}be 'compile_time', like the method it implements",
                                                          describe_fn(*mem), b->function()->is_compile_time ? "" : "not "),
                                              b->function()->is_compile_time ? "add 'compile_time'" : "remove 'compile_time'");
                        note(e, "implemented method here", b->span);
                    }
                    break;
                }
                if (!ok && !inh.opaque) {
                    Diagnostic& e = error(DiagCode::E3043, mem->span,
                                          std::format("'{}' is marked 'override' but overrides nothing", describe_fn(*mem)),
                                          "no matching base method");
                    for (Symbol* b : same_name) {
                        note(e, is_dynamic(b) ? std::format("'{}' has different parameters", describe_fn(*b))
                                              : std::format("'{}' is not virtual", describe_fn(*b)),
                             b->span);
                    }
                }
            } else if (!same_name.empty() && t->kind != SymbolKind::interface) {
                Diagnostic& e = error(DiagCode::E3044, mem->span,
                                      std::format("'{}' has the same name as '{}'", mem->name, describe_fn(*same_name.front())),
                                      "add 'override' or rename it");
                note(e, "inherited method declared here", same_name.front()->span);
            }
        }

        // Every interface method and every abstract base method needs an implementation in
        // classes that cannot be inherited from further.
        if (open_class || t->kind == SymbolKind::interface || inh.opaque) return;
        std::vector<Symbol*> chain{t};   // t and its base classes
        for (Symbol* b : inh.methods) {
            if (b->parent->kind == SymbolKind::class_ &&
                std::find(chain.begin(), chain.end(), b->parent) == chain.end()) {
                chain.push_back(b->parent);
            }
        }
        std::set<std::string> reported;
        for (Symbol* req : inh.methods) {
            bool required = req->parent->kind == SymbolKind::interface || req->function()->is_abstract;
            if (!required) continue;
            bool implemented = false;
            for (Symbol* c : chain) {
                for (Symbol* mem : c->member_order) {
                    if (mem->kind == SymbolKind::method && same_signature(mem, req) && !mem->function()->is_abstract) {
                        implemented = true;
                    }
                }
            }
            std::string key = describe_fn(*req);
            if (!implemented && reported.insert(req->name + params_sig(*req->function())).second) {
                Diagnostic& e = error(DiagCode::E3045, t->span,
                                      std::format("'{}' does not implement '{}'", t->name, key),
                                      "missing implementation");
                note(e, "declared here", req->span);
            }
        }
    }

    // --- phase D: bodies ------------------------------------------------------------------

    void resolve_bodies(Module& m) {
        if (m.header) {
            FileScope fs(*this, &m, true);
            check_aliases();
            for (const DeclPtr& d : m.header->ast.decls) {
                ns_ = d->ns;
                resolve_decl(*d, nullptr);
            }
        }
        if (m.source) {
            FileScope fs(*this, &m, false);
            check_aliases();
            for (const DeclPtr& d : m.source->ast.decls) {
                ns_ = d->ns;
                resolve_decl(*d, nullptr);
            }
        }
    }

    void resolve_decl(Decl& d, Symbol* self) {
        if (auto g = generated_.find(&d); g != generated_.end()) {
            // A member an annotation generated: its code is the annotation's, so names are found
            // where the annotation is written; `this` is the annotated type (spec 29).
            FileScope fs(*this, g->second.module, g->second.header);
            ns_                                  = g->second.ns;
            std::optional<AnnotationContext> saved = actx_;
            actx_                                = AnnotationContext{g->second.value, self, nullptr};
            resolve_function(d.as<FunctionDecl>(), self);
            actx_ = saved;
            return;
        }
        if (d.kind == DeclKind::check && self && self->decl && self->decl->kind == DeclKind::struct_ &&
            self->decl->as<TypeDecl>().is_annotation) {
            return;   // an annotation's checks run once per use, with `annotation` and `annotated` (spec 29)
        }
        TemplateScope ts(*this, d);
        switch (d.kind) {
        case DeclKind::import: return;
        case DeclKind::check: {   // file scope or a type body: compile-time code of its own
            Check& c = d.as<CheckDecl>().check;
            declared_later_.clear();
            if (c.block) collect_declarations(*c.block);
            scopes_.emplace_back();
            resolve_check(c);
            scopes_.pop_back();
            return;
        }
        case DeclKind::function:
            resolve_function(d.as<FunctionDecl>(), self);
            if (!self && d.as<FunctionDecl>().is_operator) check_operator_home(d.as<FunctionDecl>());
            apply_wraps(d.as<FunctionDecl>(), self);
            return;
        case DeclKind::variable:
        case DeclKind::field: {
            auto& v = d.as<VariableDecl>();
            resolve_type(*v.type);
            if (v.init) resolve_expr(*v.init);
            return;
        }
        case DeclKind::struct_:
        case DeclKind::class_:
        case DeclKind::interface: {
            auto    it = symbol_of_.find(&d);
            Symbol* t  = it == symbol_of_.end() ? nullptr : it->second;
            for (ThreadPromise& p : d.as<TypeDecl>().promises) {   // `unsafe std::sendable(cond)` (spec 27)
                if (p.condition) {
                    scopes_.emplace_back();
                    resolve_expr(*p.condition);
                    scopes_.pop_back();
                }
            }
            // Inside a type's body its nested types are found by their short names.
            std::string saved_ns = ns_;
            ns_                  = join_ns(d.ns, d.name);
            for (const DeclPtr& mem : d.as<TypeDecl>().members) resolve_decl(*mem, t);
            ns_ = saved_ns;
            return;
        }
        case DeclKind::enum_: {
            auto& e = d.as<EnumDecl>();
            resolve_type(*e.tag_type);
            for (EnumAlternative& alt : e.alternatives) {
                if (alt.payload) resolve_type(*alt.payload);
                if (alt.value) resolve_expr(*alt.value);
            }
            return;
        }
        }
    }

    // --- wrapping a function body (annotations, section 8) ------------------------------------

    static bool is_wrapped_body_call(const Expr& e) {
        if (e.kind != ExprKind::call) return false;
        const auto& c = e.as<CallExpr>();
        return c.args.empty() && c.callee->kind == ExprKind::path && to_string(c.callee->as<PathExpr>().path) == "std::meta::wrapped_body";
    }

    // How often `std::meta::wrapped_body()` runs on the paths through `e`: (fewest, most).
    // A call inside a loop counts as "many".
    static std::pair<int, int> wrapped_calls(Expr& e) {
        static constexpr int many = 1000;
        auto add = [](std::pair<int, int> a, std::pair<int, int> b) {
            return std::pair<int, int>{std::min(a.first + b.first, many), std::min(a.second + b.second, many)};
        };
        if (is_wrapped_body_call(e)) return {1, 1};
        switch (e.kind) {
        case ExprKind::block: {
            std::pair<int, int> sum{0, 0};
            for (StmtPtr& st : e.as<BlockExpr>().statements) {
                switch (st->kind) {
                case StmtKind::variable:
                    if (st->as<VariableStmt>().init) sum = add(sum, wrapped_calls(*st->as<VariableStmt>().init));
                    break;
                case StmtKind::assign: sum = add(sum, wrapped_calls(*st->as<AssignStmt>().value)); break;
                case StmtKind::discard: sum = add(sum, wrapped_calls(*st->as<DiscardStmt>().value)); break;
                case StmtKind::expression: sum = add(sum, wrapped_calls(*st->as<ExprStmt>().expr)); break;
                case StmtKind::destructure: sum = add(sum, wrapped_calls(*st->as<DestructureStmt>().init)); break;
                case StmtKind::check: break;
                }
            }
            if (e.as<BlockExpr>().tail) sum = add(sum, wrapped_calls(*e.as<BlockExpr>().tail));
            return sum;
        }
        case ExprKind::if_: {
            auto&               i = e.as<IfExpr>();
            std::pair<int, int> t = wrapped_calls(*i.then_block);
            std::pair<int, int> f = i.else_branch ? wrapped_calls(*i.else_branch) : std::pair<int, int>{0, 0};
            return add(wrapped_calls(*i.condition), {std::min(t.first, f.first), std::max(t.second, f.second)});
        }
        case ExprKind::match: {
            auto&               m = e.as<MatchExpr>();
            std::pair<int, int> arms{many, 0};
            for (MatchArm& a : m.arms) {
                std::pair<int, int> c = wrapped_calls(*a.body);
                arms                  = {std::min(arms.first, c.first), std::max(arms.second, c.second)};
            }
            if (m.arms.empty()) arms = {0, 0};
            return add(wrapped_calls(*m.scrutinee), arms);
        }
        case ExprKind::while_:
        case ExprKind::for_: {
            BlockExpr& body = e.kind == ExprKind::while_ ? *e.as<WhileExpr>().body : *e.as<ForExpr>().body;
            return wrapped_calls(body).second > 0 ? std::pair<int, int>{0, many} : std::pair<int, int>{0, 0};
        }
        case ExprKind::lambda: return {0, 0};
        default: {
            std::pair<int, int> sum{0, 0};
            ast::walk_exprs(e, [&](Expr& x) {
                if (&x == &e) return true;
                sum = add(sum, wrapped_calls(x));
                return false;
            });
            return sum;
        }
        }
    }

    // Gives each `std::meta::wrapped_body()` call in `e` its own copy of `body`.
    void attach_wrapped(Expr& e, const BlockExpr& body, Module& origin, bool& first, BlockPtr& original) {
        ast::walk_exprs(e, [&](Expr& x) {
            if (!is_wrapped_body_call(x)) return true;
            auto& c = x.as<CallExpr>();
            if (first) {
                c.wrapped = std::move(original);
                first     = false;
            } else {
                ast::Cloner cl([&](const Symbol& o) { return origin.make(o.kind, o.name, o.span); });
                c.wrapped = cl.block(body);
            }
            return false;
        });
    }

    std::set<const Symbol*> wrap_checked_;
    bool                    instance_ = false;   // resolving the copy of a template for an instance

    // The `wrap` blocks of the annotations on `fn` replace its body; the first annotation is the
    // outermost. Each block is resolved where its annotation is declared, without the parameters.
    void apply_wraps(FunctionDecl& fn, Symbol* self) {
        if (!fn.body) return;
        auto it = symbol_of_.find(&fn);
        if (it == symbol_of_.end() || !it->second->decl) return;
        Symbol* fs = it->second;
        std::vector<AnnotationUse*> uses;
        for (AnnotationUse& u : const_cast<Decl*>(static_cast<const Decl*>(fs->decl))->annotations) {
            if (u.symbol && u.symbol->decl && u.symbol->decl->kind == DeclKind::struct_ && wrap_block_of(u.symbol)) uses.push_back(&u);
        }
        if (uses.empty()) return;
        // A template's wraps are applied to each instance (its own copy of the body).
        if (!instance_ && (fn.is_template || (self && self->decl && self->decl->is_template))) return;
        for (auto u = uses.rbegin(); u != uses.rend(); ++u) {
            Symbol*          a    = (*u)->symbol;
            const BlockExpr& wrap = *wrap_block_of(a);
            if (wrap_checked_.insert(a).second) {
                BlockExpr& w = const_cast<BlockExpr&>(wrap);
                std::pair<int, int> n = wrapped_calls(w);
                if (n.first != 1 || n.second != 1) {
                    note(error(DiagCode::E3076, w.span,
                               "the wrap block must call 'std::meta::wrapped_body()' exactly once on every path", "wrap block"),
                         n.second == 0 ? "it never calls it" : n.first == 0 ? "some paths don't call it" : "some paths call it more than once");
                }
            }
            ast::Cloner cl([&](const Symbol& o) { return a->module->make(o.kind, o.name, o.span); });
            BlockPtr    copy = cl.block(wrap);
            // `annotated`: the function, as a std::meta::function (the checker gives it its value).
            auto hd       = std::make_unique<VariableDecl>(DeclKind::variable, (*u)->span);
            hd->name      = "annotated";
            hd->name_span = (*u)->span;
            hd->storage   = Storage::compile_time;
            Path hp;
            hp.span = (*u)->span;
            for (std::string_view part : {std::string_view("std"), std::string_view("meta"), std::string_view("function")}) {
                PathSegment ps;
                ps.name = std::string(part);
                ps.span = (*u)->span;
                hp.segments.push_back(std::move(ps));
            }
            hd->type      = std::make_unique<NamedType>((*u)->span, std::move(hp));
            Symbol* h     = a->module->make(SymbolKind::global, "annotated", (*u)->span);
            h->decl       = hd.get();
            h->is_private = true;
            program_.cloned_decls.push_back(std::move(hd));
            program_.wrap_handles.emplace_back(h, fs);
            {
                FileScope fs_scope(*this, a->module, declared_in_header(a) && a->module->header.has_value());
                std::string saved_ns = ns_;
                ns_                  = a->ns;
                std::optional<AnnotationContext> saved = actx_;
                actx_                                  = AnnotationContext{(*u)->value_symbol, nullptr, h};
                FunctionContext saved_fn               = fn_;
                fn_                                    = FunctionContext{};
                declared_later_.clear();
                collect_declarations(*copy);
                scopes_.emplace_back();
                resolve_block(*copy);
                scopes_.pop_back();
                fn_    = saved_fn;
                actx_  = saved;
                ns_    = saved_ns;
            }
            BlockPtr original = std::move(fn.body);
            bool     first    = true;
            const BlockExpr& body = *original;
            attach_wrapped(*copy, body, *fs->module, first, original);
            fn.body = std::move(copy);
        }
    }

    // The `wrap` block of annotation `a`: in its declaration or its .ed block.
    static const BlockExpr* wrap_block_of(const Symbol* a) {
        if (a->decl && a->decl->as<TypeDecl>().wrap) return a->decl->as<TypeDecl>().wrap.get();
        if (a->source_block && a->source_block->as<TypeDecl>().wrap) return a->source_block->as<TypeDecl>().wrap.get();
        return nullptr;
    }

    void resolve_function(FunctionDecl& fn, Symbol* self) {
        FunctionContext saved = fn_;
        fn_                   = FunctionContext{};
        fn_.self_type         = self;
        fn_.is_static         = fn.is_static;
        fn_.has_this          = self && !fn.is_static;
        if (fn.result) resolve_type(*fn.result);
        for (Param& p : fn.params) resolve_type(*p.type);
        if (fn.body) {
            declared_later_.clear();
            collect_declarations(*fn.body);
            scopes_.emplace_back();
            for (Param& p : fn.params) {
                bool ref = p.type->kind == TypeKind::reference;
                p.symbol = declare_local(p.name, p.span, ref ? LocalKind::ref_param : LocalKind::value_param, p.is_mut, ref);
                p.symbol->is_type_value = fn.is_compile_time && is_type_type(*p.type);
            }
            resolve_block(*fn.body);
            scopes_.pop_back();
        }
        fn_ = saved;
    }

    // A free `operator` function lives with one of its operand types: same namespace, same
    // module (spec 16, 30). So only the module that declares a type defines its operators.
    void check_operator_home(const FunctionDecl& fn) {
        std::vector<const Symbol*> types;
        for (const Param& p : fn.params) {
            const TypeExpr* t = p.type.get();
            if (t->kind == TypeKind::reference) t = t->as<ReferenceType>().inner.get();
            if (t->kind != TypeKind::named) continue;
            const Symbol* s = t->as<NamedType>().symbol;
            if (!s || !s->is_type()) return;   // unresolved (already reported) or a template parameter: decided per instance
            if (s->kind == SymbolKind::type_param) return;
            if (s->module == module_ && s->ns == ns_) return;
            types.push_back(s);
        }
        if (types.empty()) return;   // only built-in operand types: the checker reports that
        std::sort(types.begin(), types.end());
        types.erase(std::unique(types.begin(), types.end()), types.end());
        Diagnostic& e = error(DiagCode::E3066, fn.name_span,
                              std::format("'operator{}' is declared away from its operand types", fn.name),
                              "not with its operand types");
        for (const Symbol* t : types) {
            note(e, std::format("'{}' is declared in {} of module '{}'", t->key(), ns_display(t->ns), t->module->name), t->span);
        }
        note(e, "a free operator is declared in the same namespace and module as one of its operand types; or make it a method");
    }

    // --- locals ---------------------------------------------------------------------------

    Symbol* declare_local(const std::string& name, Span span, LocalKind kind, bool is_mut, bool is_reference,
                          bool check_shadowing = true) {
        if (check_shadowing) {
            for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
                if (auto found = it->names.find(name); found != it->names.end()) {
                    Diagnostic& e = error(DiagCode::E3028, span,
                                          std::format("'{}' is already declared in this function", name),
                                          "declared again");
                    note(e, "earlier declaration here", found->second->span);
                    note(e, "Edax has no shadowing; choose a different name");
                    break;
                }
            }
            if (Symbol* tp = find_template_param(name)) {
                note(error(DiagCode::E3028, span, std::format("'{}' is already a template parameter", name),
                           "declared again"),
                     "template parameter here", tp->span);
            }
        }
        Symbol* s       = module_->make(SymbolKind::local, name, span);
        s->local_kind   = kind;
        s->is_mut       = is_mut;
        s->is_reference = is_reference;
        scopes_.back().names[name] = s;
        return s;
    }

    Symbol* lookup_local(const std::string& name, Span use) {
        bool crossed_lambda = false;
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (auto found = it->names.find(name); found != it->names.end()) {
                if (crossed_lambda) {
                    Diagnostic& e = error(DiagCode::E3050, use,
                                          std::format("the lambda uses '{}' without capturing it", name),
                                          "not captured");
                    note(e, std::format("add it to the capture list: [{0}], [&{0}] or [move {0}]", name));
                }
                return found->second;
            }
            if (it->lambda_boundary) crossed_lambda = true;
        }
        return nullptr;
    }

    std::vector<std::string> value_candidates() const {
        std::vector<std::string> out;
        for (const Scope& sc : scopes_) {
            for (const auto& [n, sym] : sc.names) out.push_back(n);
        }
        for (const std::string& level : enclosing_levels(ns_)) {
            for (std::string& n : names_in(level)) out.push_back(std::move(n));
        }
        return out;
    }

    std::vector<std::string> type_candidates() const {
        std::vector<std::string> out = {"bool", "char", "void", "never", "i8", "i16", "i32", "i64", "i128", "isize",
                                        "u8", "u16", "u32", "u64", "u128", "usize", "f32", "f64"};
        for (const std::string& level : enclosing_levels(ns_)) {
            for (std::string& n : names_in(level)) {
                Found f = find_items(join_ns(level, n));
                if (!f.symbols.empty() && f.symbols.front()->is_type()) out.push_back(std::move(n));
            }
        }
        for (const auto& lv : template_params_) {
            for (Symbol* sym : lv) out.push_back(sym->name);
        }
        return out;
    }

    static std::vector<std::string> member_names(const Symbol* t) {
        std::vector<std::string> out;
        for (const auto& [n, syms] : t->members) out.push_back(n);
        return out;
    }

    // --- paths ----------------------------------------------------------------------------

    // A template type needs exactly its parameters as arguments (spec 14).
    void check_template_arity(const Path& path, const Symbol* s, Span span) {
        if (!s || !s->is_type() || !s->decl || !s->decl->is_template) return;
        std::size_t given = path.segments.empty() ? 0 : path.segments.back().template_args.size();
        std::size_t wanted = s->decl->template_params.size();
        std::size_t required = 0;
        for (const TemplateParam& p : s->decl->template_params) {
            if (!p.default_type && !p.default_value) ++required;
        }
        if (given >= required && given <= wanted) {
            // Types for `type` parameters, values for value parameters (`template<usize N>`).
            const auto& args = path.segments.back().template_args;
            for (std::size_t i = 0; i < given; ++i) {
                bool wants_type = s->decl->template_params[i].is_type;
                if (wants_type == (args[i].type != nullptr)) continue;
                error(DiagCode::E4180, args[i].type ? args[i].type->span : args[i].value->span,
                      std::format("'{}' takes a {} here, not a {}", s->name, wants_type ? "type" : "value",
                                  wants_type ? "value" : "type"),
                      wants_type ? "a value" : "a type");
            }
            return;
        }
        error(DiagCode::E4180, span,
              std::format("'{}' takes {}{} template argument{}, but {} {} given", s->name,
                          required == wanted ? "" : std::format("{} to ", required), wanted, wanted == 1 ? "" : "s",
                          given, given == 1 ? "is" : "are"),
              "wrong number of template arguments");
    }

    // A bare name as a template argument is parsed as a type; it is a value argument when it
    // names a value: a template value parameter or a compile_time constant (spec 14, 15).
    bool names_value(const TypeExpr& t) {
        if (t.kind != TypeKind::named) return false;
        const Path& p = t.as<NamedType>().path;
        if (p.segments.size() != 1 || !p.segments[0].template_args.empty()) return false;
        const std::string& name = p.segments[0].name;
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (auto found = it->names.find(name); found != it->names.end()) return !found->second->is_type_value;
        }
        if (Symbol* tp = find_template_param(name)) return tp->kind == SymbolKind::value_param;
        if (Symbol* item = lookup_unqualified(name, p.span, p.global, false).symbol) {
            return item->kind == SymbolKind::global && !item->is_type_value;
        }
        return false;
    }

    void resolve_template_args(Path& path) {
        for (PathSegment& seg : path.segments) {
            for (TemplateArg& arg : seg.template_args) {
                if (arg.type && names_value(*arg.type)) {
                    Path p     = std::move(arg.type->as<NamedType>().path);
                    Span span  = arg.type->span;
                    arg.value  = std::make_unique<PathExpr>(span, std::move(p));
                    arg.type.reset();
                }
                if (arg.type) resolve_type(*arg.type);
                if (arg.value) resolve_expr(*arg.value);
            }
        }
    }

    // Finds a member (field, method, alternative) in a type or its ancestors.
    static Symbol* find_member(const Symbol* t, const std::string& name, bool& opaque) {
        opaque = opaque || t->has_opaque_base;
        if (auto it = t->members.find(name); it != t->members.end() && !it->second.empty()) {
            return it->second.front();
        }
        for (const Symbol* b : t->bases) {
            if (Symbol* s = find_member(b, name, opaque)) return s;
        }
        return nullptr;
    }

    bool member_visible(const Symbol* mem) const {
        // Generated members see the private parts of the type they were written for (spec 29).
        if (actx_ && actx_->type && (mem->parent == actx_->type || is_ancestor(actx_->type, mem->parent))) return true;
        if (mem->access == Access::private_ && mem->parent->module != module_) return false;
        if (mem->access == Access::protected_ && mem->parent->module != module_) {
            return fn_.self_type && (fn_.self_type == mem->parent || is_ancestor(fn_.self_type, mem->parent));
        }
        return true;
    }

    Symbol* check_want(Symbol* sym, Want want, Span span, std::string_view display) {
        if (!sym) return nullptr;
        if (want == Want::type && sym->is_type_value) return sym;   // a compile_time type value used as a type (spec 26)
        if (want == Want::value && sym->is_type()) return sym;      // a type used as a value (spec 26)
        if (want == Want::type && !sym->is_type()) {
            error(DiagCode::E3022, span, std::format("'{}' is a {}, not a type", display, sema::to_string(sym->kind)),
                  "not a type");
            return nullptr;
        }
        return sym;
    }

    Symbol* lookup_single(PathSegment& seg, Want want, Span span, bool global = false) {
        const std::string& name = seg.name;
        if (!global && actx_ && (name == "annotation" || name == "annotated")) {   // inside an annotation (spec 29)
            Symbol* s = name == "annotation" ? actx_->value : actx_->type ? actx_->type : actx_->handle;
            if (s) return check_want(s, want, span, name);
        }
        if (!global) {
            if (want == Want::type) {
                // A local compile_time `type` constant or a `type` parameter used as a type (spec 26).
                for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
                    if (auto found = it->names.find(name); found != it->names.end()) {
                        if (found->second->is_type_value) return found->second;
                        break;
                    }
                }
            } else if (Symbol* local = lookup_local(name, span)) {
                return local;
            }
            if (Symbol* tp = find_template_param(name)) {
                return check_want(tp, want, span, name);
            }
        }
        Unqualified u = lookup_unqualified(name, span, global, true);
        if (u.reported) return nullptr;
        if (u.symbol) return check_want(u.symbol, want, span, name);
        std::string_view what = want == Want::type ? "type" : "value";
        if (u.is_namespace || name == "std") {
            note(error(DiagCode::E3022, span, std::format("'{}' is a namespace, not a {}", name, what), "namespace"),
                 std::format("name something in it: '{}::...'", name));
            return nullptr;
        }
        if (program_.modules.count(name) && name != module_->name) {
            note(error(DiagCode::E3022, span, std::format("'{}' is a module, not a {}", name, what), "module"),
                 "a module is a file; its names are reached through the namespaces it declares (spec 30)");
            return nullptr;
        }
        if (!u.hidden) {
            for (const std::string& level : global ? std::vector<std::string>{""} : enclosing_levels(ns_)) {
                if (report_unimported(join_ns(level, name), span)) return nullptr;
            }
        }
        if (u.hidden) {
            note(error(DiagCode::E3024, span, std::format("'{}' is private to its module", name), "private"),
                 "only items declared in a module's .edh can be used from other modules");
            return nullptr;
        }
        if (want == Want::type) {
            suggest(error(DiagCode::E3021, span, std::format("cannot find type '{}'", name), "unknown type"), name,
                    type_candidates());
            return nullptr;
        }
        if (fn_.self_type) {
            bool opaque = false;
            if (Symbol* mem = find_member(fn_.self_type, name, opaque);
                mem && (mem->kind == SymbolKind::field || mem->kind == SymbolKind::method)) {
                Diagnostic& e = error(DiagCode::E3026, span,
                                      std::format("'{}' is a member of '{}'; write 'this.{}'", name,
                                                  fn_.self_type->name, name),
                                      std::format("use 'this.{}'", name));
                note(e, "members are always accessed through 'this.'");
                return nullptr;
            }
        }
        Diagnostic& e = error(DiagCode::E3020, span, std::format("cannot find '{}' in this scope", name), "not found");
        if (auto later = declared_later_.find(name); later != declared_later_.end() && later->second.begin > span.begin) {
            note(e, std::format("'{}' is declared later in this function; move the declaration up", name), later->second);
        } else {
            suggest(e, name, value_candidates());
            if (!ns_.empty() || global) {
                note(e, std::format("searched {}", [&] {
                         std::string list;
                         std::vector<std::string> levels = global ? std::vector<std::string>{""} : enclosing_levels(ns_);
                         for (std::size_t i = 0; i < levels.size(); ++i) {
                             list += (i ? ", " : "") + (levels[i].empty() ? std::string("the global namespace") : "'" + levels[i] + "'");
                         }
                         return list;
                     }()));
            }
        }
        return nullptr;
    }

    // Records every local variable declared in a function body, for "declared later" notes.
    void collect_declarations(const BlockExpr& block) {
        std::function<void(const Expr&)> walk_expr;
        std::function<void(const BlockExpr&)> walk_block = [&](const BlockExpr& b) {
            for (const StmtPtr& st : b.statements) {
                if (st->kind == StmtKind::variable) {
                    const auto& v = st->as<VariableStmt>();
                    declared_later_.emplace(v.name, v.name_span);
                } else if (st->kind == StmtKind::destructure) {
                    for (const DestructureBinding& binding : st->as<DestructureStmt>().bindings) {
                        if (!binding.wildcard) declared_later_.emplace(binding.name, binding.span);
                    }
                } else if (st->kind == StmtKind::expression) {
                    walk_expr(*st->as<ExprStmt>().expr);
                }
            }
            if (b.tail) walk_expr(*b.tail);
        };
        walk_expr = [&](const Expr& e) {
            switch (e.kind) {
            case ExprKind::block: walk_block(e.as<BlockExpr>()); break;
            case ExprKind::if_: {
                const auto& i = e.as<IfExpr>();
                walk_block(*i.then_block);
                if (i.else_branch) walk_expr(*i.else_branch);
                break;
            }
            case ExprKind::while_: walk_block(*e.as<WhileExpr>().body); break;
            case ExprKind::for_: walk_block(*e.as<ForExpr>().body); break;
            default: break;
            }
        };
        walk_block(block);
    }

    // The four alternatives exported directly in `std::` (spec 08), or null.
    Symbol* std_export(const std::string& name) const {
        Module* stdm = program_.std_module;
        std::string_view owner = name == "some" || name == "none" ? "option" : name == "ok" || name == "error" ? "result" : "";
        if (!stdm || owner.empty()) return nullptr;
        Symbol* type = stdm->item("std::" + std::string(owner));
        if (!type) return nullptr;
        auto alt = type->members.find(name);
        return alt == type->members.end() ? nullptr : alt->second.front();
    }

    // Walks `T::member::...` from a type symbol.
    Symbol* lookup_members(Symbol* base, Path& path, std::size_t i, Want want, Span span) {
        std::string display;
        for (std::size_t k = 0; k < i; ++k) display += (k ? "::" : "") + path.segments[k].name;
        while (i < path.segments.size()) {
            const std::string& name = path.segments[i].name;
            Span               seg  = path.segments[i].span;
            if (base->kind == SymbolKind::type_param) {
                return nullptr;   // depends on the instantiation
            }
            if (!base->is_type() || base->kind == SymbolKind::interface) {
                error(DiagCode::E3025, seg,
                      std::format("'{}' is a {} and has no members that can be named with '::'", display,
                                  sema::to_string(base->kind)),
                      "no such member");
                return nullptr;
            }
            bool    opaque = false;
            Symbol* mem    = find_member(base, name, opaque);
            if (!mem) {
                if (opaque) return nullptr;
                Diagnostic& e = error(DiagCode::E3025, seg,
                                      base->kind == SymbolKind::enum_
                                          ? std::format("enum '{}' has no alternative '{}'", display, name)
                                          : std::format("'{}' has no member '{}'", display, name),
                                      "not found");
                suggest(e, name, member_names(base));
                return nullptr;
            }
            if (mem->kind != SymbolKind::alternative && !member_visible(mem)) {
                std::string why = mem->access == Access::protected_
                                      ? std::format("'{}::{}' is protected", display, name)
                                      : std::format("'{}::{}' is private to module '{}'", display, name,
                                                    mem->parent->module->name);
                Diagnostic& e = error(DiagCode::E3024, seg, why, "not visible here");
                if (mem->access == Access::protected_) note(e, "protected members are visible to derived classes only");
                note(e, "declared here", mem->span);
                return nullptr;
            }
            if (mem->kind == SymbolKind::field || (mem->kind == SymbolKind::method && !mem->is_static)) {
                Diagnostic& e = error(DiagCode::E3038, seg,
                                      std::format("'{}::{}' is a {}{}", display, name,
                                                  mem->kind == SymbolKind::field ? "field" : "method",
                                                  mem->kind == SymbolKind::field ? "" : " but not static"),
                                      "not static");
                note(e, std::format("access it on a value: 'value.{}'", name));
                return nullptr;
            }
            display += "::" + name;
            base = mem;
            ++i;
        }
        return check_want(base, want, span, display);
    }

    Symbol* lookup_path(Path& path, Want want, Span span) {
        resolve_template_args(path);
        auto& segs = path.segments;
        if (segs.size() == 1 && !path.global) {
            if (is_builtin_type_name(segs[0].name)) return nullptr;   // `u32`, also as a type value
            return lookup_single(segs[0], want, span);
        }
        if (segs.size() == 1) return lookup_single(segs[0], want, span, /*global=*/true);
        if (!path.global && is_builtin_type_name(segs[0].name)) {
            return nullptr;   // `u32::max`: built-in members come with the type checker
        }
        // `vk::device` with `namespace vk = gfx::vulkan;`: the path is rewritten to its full form,
        // so later phases see `gfx::vulkan::device` (spec 30).
        bool shadowed = false;   // an alias that collides with a real name (reported) is not used
        for (const std::string& level : enclosing_levels(ns_)) {
            shadowed = shadowed || namespace_visible(join_ns(level, segs[0].name)) || !find_items(join_ns(level, segs[0].name)).symbols.empty();
        }
        if (!path.global && !shadowed) {
            if (const ast::NamespaceAlias* a = find_alias(segs[0].name)) {
                std::vector<PathSegment> full;
                for (const PathSegment& t : a->target.segments) {
                    PathSegment c;
                    c.name = t.name;
                    c.span = segs[0].span;
                    full.push_back(std::move(c));
                }
                for (std::size_t i = 1; i < segs.size(); ++i) full.push_back(std::move(segs[i]));
                segs        = std::move(full);
                path.global = true;
            }
        }
        const std::string& first = segs[0].name;
        if (!path.global && find_template_param(first)) return nullptr;   // `T::make`: depends on the instance
        if (first == "std" && !std_visible()) {
            if (std_reported_.insert(segs[0].span.file).second) {
                note(error(DiagCode::E3023, segs[0].span, "the standard library is not imported", "'std' not imported"),
                     "add 'import std;' at the top of the file (reported once per file)");
            }
            return nullptr;
        }

        // Every enclosing namespace where the path starts (qualified names are looked up where
        // they say; the first name is found like an unqualified one, spec 30).
        struct Match {
            std::string level;
            std::size_t k   = 0;         // segs[0..k] name the declaration
            Symbol*     sym = nullptr;   // null: only the namespace segs[0] exists here
        };
        std::vector<Match>       matches;
        std::vector<std::string> levels = path.global ? std::vector<std::string>{""} : enclosing_levels(ns_);
        for (const std::string& level : levels) {
            std::vector<std::string> prefixes;
            std::string              q = level;
            for (const PathSegment& seg : segs) prefixes.push_back(q = join_ns(q, seg.name));
            Match m{level};
            bool  any = false;
            for (std::size_t k = segs.size(); k-- > 0;) {
                Found f = find_items(prefixes[k]);
                if (!f.symbols.empty()) {
                    check_one_module(f.symbols, prefixes[k], segs[0].span);
                    m.k   = k;
                    m.sym = f.symbols.front();
                    any   = true;
                    break;
                }
            }
            if (any || namespace_visible(prefixes[0])) matches.push_back(m);
        }
        if (matches.size() > 1) {
            Diagnostic& e = error(DiagCode::E3060, segs[0].span,
                                  std::format("'{}' is both '{}' and '{}'; write the full name", first,
                                              global_display(join_ns(matches[0].level, first)),
                                              global_display(join_ns(matches[1].level, first))),
                                  "ambiguous");
            note(e, "a leading '::' starts at the global namespace: '::" + ast::to_string(path) + "'");
            return nullptr;
        }
        if (matches.empty()) {
            for (const std::string& level : levels) {
                std::string q = level;
                for (const PathSegment& seg : segs) {
                    q = join_ns(q, seg.name);
                    if (report_unimported(q, segs[0].span)) return nullptr;
                }
            }
            // `geometry::vec::vec2`: a module path used like a namespace (before spec 30).
            for (std::size_t k = segs.size() - 1; k >= 1; --k) {
                std::string name;
                for (std::size_t j = 0; j < k; ++j) name = join_ns(name, segs[j].name);
                auto mod = program_.modules.find(name);
                if (mod == program_.modules.end()) continue;
                Diagnostic& e = error(DiagCode::E3020, segs[0].span,
                                      std::format("'{}' is a module, not a namespace", name), "module path");
                note(e, "a module is a file; 'import' loads it, and its names are reached through the namespaces it declares (spec 30)");
                for (const auto& [key, syms] : mod->second->items) {
                    if (key == segs[k].name || key.ends_with("::" + segs[k].name)) {
                        note(e, std::format("that module declares it {}: write '{}'",
                                            syms.front()->ns.empty() ? "in the global namespace" : "in namespace '" + syms.front()->ns + "'",
                                            key));
                        break;
                    }
                }
                return nullptr;
            }
            suggest(error(DiagCode::E3020, segs[0].span, std::format("cannot find '{}' in this scope", first),
                          "not a namespace, type or enum"),
                    first, value_candidates());
            return nullptr;
        }
        const Match& m = matches.front();
        if (m.sym) {
            if (m.k + 1 == segs.size()) return check_want(m.sym, want, span, join_ns(m.level, ast::to_string(path)));
            return lookup_members(m.sym, path, m.k + 1, want, span);
        }
        // Only namespaces: find how far they go, then report the missing name.
        std::string ns = join_ns(m.level, first);
        std::size_t k  = 1;
        while (k + 1 < segs.size() && namespace_visible(join_ns(ns, segs[k].name))) ns = join_ns(ns, segs[k++].name);
        if (ns == "std" || ns.starts_with("std::")) {
            if (segs.size() == 2) {
                if (Symbol* alt = std_export(segs[1].name)) return check_want(alt, want, span, "std::" + segs[1].name);
            }
            return nullptr;   // names the compiler provides itself (`std::println`, `std::meta::fields`)
        }
        const std::string& missing = segs[k].name;
        Found f = find_items(join_ns(ns, missing));
        if (!f.hidden && report_unimported(join_ns(ns, missing), segs[k].span)) return nullptr;
        if (f.hidden) {
            note(error(DiagCode::E3024, segs[k].span, std::format("'{}::{}' is private to its module", ns, missing), "private"),
                 "only items declared in a module's .edh can be used from other modules");
        } else {
            Diagnostic& e = error(DiagCode::E3020, segs[k].span, std::format("cannot find '{}' in namespace '{}'", missing, ns),
                                  "not found");
            suggest(e, missing, names_in(ns));
        }
        return nullptr;
    }

    // --- types ----------------------------------------------------------------------------

    void resolve_type(TypeExpr& t) {
        switch (t.kind) {
        case TypeKind::named: {
            auto& nt  = t.as<NamedType>();
            {   // `std::c_library::function<R(A...)>`: a C function type (dynamic libraries)
                auto& segs = nt.path.segments;
                if (segs.size() == 3 && segs[0].name == "std" && segs[1].name == "c_library" && segs[2].name == "function" &&
                    segs[2].template_args.size() == 1 && segs[2].template_args[0].type &&
                    segs[2].template_args[0].type->kind == TypeKind::function) {
                    resolve_type(*segs[2].template_args[0].type);
                    nt.symbol = nullptr;
                    return;
                }
            }
            nt.symbol = lookup_path(nt.path, Want::type, nt.span);
            // `annotated` in a generated member of a template type is the instance: the template
            // with its own parameters as arguments (spec 29).
            if (actx_ && actx_->type && nt.symbol == actx_->type && nt.path.segments.size() == 1 &&
                nt.path.segments[0].name == "annotated" && nt.symbol->decl && nt.symbol->decl->is_template &&
                !template_params_.empty() && nt.path.segments[0].template_args.empty()) {
                for (Symbol* p : template_params_.back()) {
                    TemplateArg arg;
                    arg.span = nt.span;
                    Path pp;
                    pp.span = nt.span;
                    PathSegment ps;
                    ps.name = p->name;
                    ps.span = nt.span;
                    pp.segments.push_back(std::move(ps));
                    if (p->kind == SymbolKind::type_param) {
                        auto at   = std::make_unique<NamedType>(nt.span, std::move(pp));
                        at->symbol = p;
                        arg.type  = std::move(at);
                    } else {
                        auto pe   = std::make_unique<PathExpr>(nt.span, std::move(pp));
                        pe->symbol = p;
                        arg.value = std::move(pe);
                    }
                    nt.path.segments[0].template_args.push_back(std::move(arg));
                }
                nt.path.segments[0].has_template_args = true;
                // In an instance, the copy is not the template: name the template itself.
                if (Symbol* templ = nt.symbol->module ? nt.symbol->module->item(nt.symbol->key()) : nullptr; templ && templ->is_type()) {
                    nt.symbol = templ;
                }
            }
            check_template_arity(nt.path, nt.symbol, nt.span);
            return;
        }
        case TypeKind::reference: resolve_type(*t.as<ReferenceType>().inner); return;
        case TypeKind::function: {
            auto& f = t.as<FunctionType>();
            resolve_type(*f.result);
            for (TypePtr& p : f.params) resolve_type(*p);
            return;
        }
        case TypeKind::auto_:
        case TypeKind::deduce: return;
        case TypeKind::tuple:
            for (TypePtr& e : t.as<TupleType>().elements) resolve_type(*e);
            return;
        }
    }

    // --- statements and expressions -------------------------------------------------------

    void resolve_block(BlockExpr& b) {
        scopes_.emplace_back();
        for (StmtPtr& s : b.statements) resolve_stmt(*s);
        if (b.tail) resolve_expr(*b.tail);
        scopes_.pop_back();
    }

    void resolve_check(Check& c) {
        if (c.block) {
            resolve_block(*c.block);
            return;
        }
        resolve_expr(*c.condition);
        resolve_expr(*c.message);
    }

    void resolve_stmt(Stmt& s) {
        for (ExprPtr& id : s.allowed) resolve_expr(*id);   // `@std::allow{ .id = ... }` on the statement
        switch (s.kind) {
        case StmtKind::check: resolve_check(s.as<CheckStmt>().check); return;
        case StmtKind::variable: {
            auto& v = s.as<VariableStmt>();
            resolve_type(*v.type);
            if (v.init) resolve_expr(*v.init);
            v.symbol = declare_local(v.name, v.name_span, LocalKind::variable, v.is_mut,
                                     v.type->kind == TypeKind::reference);
            v.symbol->is_type_value = v.storage == Storage::compile_time && is_type_type(*v.type);
            return;
        }
        case StmtKind::assign: {
            auto& a = s.as<AssignStmt>();
            resolve_expr(*a.target);
            resolve_expr(*a.value);
            return;
        }
        case StmtKind::discard: resolve_expr(*s.as<DiscardStmt>().value); return;
        case StmtKind::expression: resolve_expr(*s.as<ExprStmt>().expr); return;
        case StmtKind::destructure: {
            auto& d = s.as<DestructureStmt>();
            resolve_expr(*d.init);
            for (DestructureBinding& b : d.bindings) {
                if (b.wildcard) continue;
                if (b.type) resolve_type(*b.type);
                b.symbol = declare_local(b.name, b.span, LocalKind::variable, b.is_mut,
                                         b.type && b.type->kind == TypeKind::reference);
            }
            return;
        }
        }
    }

    void resolve_expr(Expr& e, ExprFlags flags = {}) {
        switch (e.kind) {
        case ExprKind::int_literal:
        case ExprKind::float_literal:
        case ExprKind::char_literal:
        case ExprKind::bool_literal:
        case ExprKind::nullptr_literal:
        case ExprKind::default_:
        case ExprKind::error: return;
        case ExprKind::string_literal:
            for (FormatPart& part : e.as<LiteralExpr>().format_parts) {
                if (part.expr) resolve_expr(*part.expr);
            }
            return;
        case ExprKind::this_:
            if (!fn_.has_this) {
                error(DiagCode::E3027, e.span,
                      fn_.self_type && fn_.is_static ? "static methods have no 'this'"
                                                     : "'this' is only available inside methods",
                      "no 'this' here");
            }
            return;
        case ExprKind::break_:
        case ExprKind::continue_:
            if (fn_.loop_depth == 0) {
                error(DiagCode::E3030, e.span,
                      std::format("'{}' outside of a loop", e.kind == ExprKind::break_ ? "break" : "continue"),
                      "not inside 'while' or 'for'");
            }
            return;
        case ExprKind::path: resolve_path_expr(e.as<PathExpr>(), flags.is_callee); return;
        case ExprKind::unary: {
            auto& u = e.as<UnaryExpr>();
            if (u.op == UnaryOp::mut_marker && !flags.allow_mut_marker) {
                note(error(DiagCode::E3031, e.span, "'mut' is not allowed here", "misplaced 'mut'"),
                     "'mut x' marks a call argument passed as a mutable reference, or a match subject; "
                     "'mut &x' takes a mutable pointer");
            }
            resolve_expr(*u.operand);
            if (u.op == UnaryOp::move) check_move(*u.operand);
            return;
        }
        case ExprKind::binary: {
            auto& b = e.as<BinaryExpr>();
            resolve_expr(*b.lhs);
            resolve_expr(*b.rhs);
            return;
        }
        case ExprKind::range: {
            auto& r = e.as<RangeExpr>();
            resolve_expr(*r.lo);
            resolve_expr(*r.hi);
            return;
        }
        case ExprKind::call: {
            auto& c = e.as<CallExpr>();
            resolve_expr(*c.callee, ExprFlags{false, true});
            for (ExprPtr& arg : c.args) resolve_expr(*arg, ExprFlags{true, false});
            return;
        }
        case ExprKind::index: {
            auto& i = e.as<IndexExpr>();
            resolve_expr(*i.object);
            resolve_expr(*i.index);
            return;
        }
        case ExprKind::member: resolve_member(e.as<MemberExpr>()); return;
        case ExprKind::try_: resolve_expr(*e.as<TryExpr>().operand); return;
        case ExprKind::cast: {
            auto& c = e.as<CastExpr>();
            resolve_type(*c.target);
            resolve_expr(*c.operand);
            return;
        }
        case ExprKind::bit: {
            auto& b = e.as<BitExpr>();
            if (b.type) resolve_type(*b.type);
            resolve_expr(*b.operand);
            return;
        }
        case ExprKind::struct_literal: resolve_struct_literal(e.as<StructLiteralExpr>()); return;
        case ExprKind::tuple:
            for (ExprPtr& x : e.as<TupleExpr>().elements) resolve_expr(*x);
            return;
        case ExprKind::list:
            for (ExprPtr& x : e.as<ListExpr>().elements) resolve_expr(*x);
            return;
        case ExprKind::lambda: resolve_lambda(e.as<LambdaExpr>()); return;
        case ExprKind::if_: {
            auto& i = e.as<IfExpr>();
            resolve_expr(*i.condition);
            resolve_block(*i.then_block);
            if (i.else_branch) resolve_expr(*i.else_branch);
            return;
        }
        case ExprKind::match: {
            auto& m = e.as<MatchExpr>();
            resolve_expr(*m.scrutinee, ExprFlags{true, false});
            for (MatchArm& arm : m.arms) {
                scopes_.emplace_back();
                resolve_pattern(*arm.pattern);
                if (arm.guard) resolve_expr(*arm.guard);
                resolve_expr(*arm.body);
                scopes_.pop_back();
            }
            return;
        }
        case ExprKind::block: resolve_block(e.as<BlockExpr>()); return;
        case ExprKind::while_: {
            auto& w = e.as<WhileExpr>();
            resolve_expr(*w.condition);
            ++fn_.loop_depth;
            resolve_block(*w.body);
            --fn_.loop_depth;
            return;
        }
        case ExprKind::for_: {
            auto& f = e.as<ForExpr>();
            resolve_expr(*f.iterable);
            scopes_.emplace_back();
            resolve_type(*f.type);
            f.symbol = declare_local(f.name, f.name_span, LocalKind::loop_variable, f.is_mut,
                                     f.type->kind == TypeKind::reference);
            f.symbol->is_type_value = f.is_compile_time && is_type_type(*f.type);
            ++fn_.loop_depth;
            resolve_block(*f.body);
            --fn_.loop_depth;
            scopes_.pop_back();
            return;
        }
        case ExprKind::return_: {
            auto& r = e.as<ReturnExpr>();
            if (r.value) resolve_expr(*r.value);
            return;
        }
        }
    }

    void resolve_path_expr(PathExpr& pe, bool is_callee) {
        Symbol* sym = lookup_path(pe.path, Want::value, pe.span);
        pe.symbol   = sym;
        if (sym && sym->kind == SymbolKind::alternative) {
            bool        payload = sym->alternative->payload != nullptr;
            std::string display = to_string(pe.path);
            if (payload && !is_callee) {
                note(error(DiagCode::E3037, pe.span, std::format("'{}' carries a value", display), "missing payload"),
                     std::format("construct it with '{}(...)'", display));
            } else if (!payload && is_callee) {
                note(error(DiagCode::E3037, pe.span, std::format("'{}' has no payload", display), "not callable"),
                     std::format("write '{}' without '(...)'", display));
            }
        }
    }

    void check_move(const Expr& operand) {
        if (operand.kind == ExprKind::path) {
            const auto& pe = operand.as<PathExpr>();
            if (!pe.symbol) return;   // already reported, or unchecked (std::)
            if (pe.symbol->kind == SymbolKind::local) {
                if (pe.symbol->is_reference) {
                    note(error(DiagCode::E3032, operand.span,
                               std::format("'{}' is a reference and cannot be moved", pe.symbol->name), "reference"),
                         "references never own their value; pass the owner by value to move it");
                }
                return;
            }
            error(DiagCode::E3032, operand.span,
                  std::format("cannot move {} '{}'", sema::to_string(pe.symbol->kind), pe.symbol->name),
                  "not a local variable");
            return;
        }
        Diagnostic& e = error(DiagCode::E3032, operand.span, "only local variables and by-value parameters can be moved",
                              "not a local variable");
        if (operand.kind == ExprKind::member || operand.kind == ExprKind::index) {
            note(e, "to take a field's value use 'std::take(mut obj.field)' or 'std::replace(mut obj.field, value)'");
        }
    }

    void resolve_member(MemberExpr& me) {
        resolve_expr(*me.object);
        resolve_template_args(me.name);
        if (me.object->kind != ExprKind::this_ || !fn_.self_type || !fn_.has_this || me.arrow) {
            return;   // members of other values are resolved by the type checker
        }
        Symbol* self = fn_.self_type;
        auto&   segs = me.name.segments;
        bool    opaque = false;
        if (segs.size() == 1) {
            Symbol* mem = find_member(self, segs[0].name, opaque);
            if (!mem && !opaque) {
                Diagnostic& e = error(DiagCode::E3025, segs[0].span,
                                      std::format("'{}' has no member '{}'", self->name, segs[0].name), "not found");
                suggest(e, segs[0].name, member_names(self));
            }
            me.symbol = mem;
            return;
        }
        // this.base::method(): `base` must be an ancestor class.
        const std::string& base_name = segs[0].name;
        Symbol*            base      = nullptr;
        std::function<void(Symbol*)> search = [&](Symbol* t) {
            for (Symbol* b : t->bases) {
                if (!base && b->name == base_name) base = b;
                search(b);
            }
        };
        search(self);
        if (!base) {
            if (!self->has_opaque_base) {
                error(DiagCode::E3047, segs[0].span,
                      std::format("'{}' is not a base class of '{}'", base_name, self->name), "not a base");
            }
            return;
        }
        Symbol* mem = find_member(base, segs[1].name, opaque);
        if (!mem && !opaque) {
            error(DiagCode::E3025, segs[1].span, std::format("'{}' has no member '{}'", base->name, segs[1].name),
                  "not found");
        }
        me.symbol = mem;
    }

    // Fields of a type and its base classes, base fields first.
    static void collect_fields(const Symbol* t, std::vector<Symbol*>& out, bool& opaque) {
        opaque = opaque || t->has_opaque_base;
        for (const Symbol* b : t->bases) {
            if (b->kind == SymbolKind::class_) collect_fields(b, out, opaque);
        }
        for (Symbol* m : t->member_order) {
            if (m->kind == SymbolKind::field && !m->is_static) out.push_back(m);
        }
    }

    void resolve_struct_literal(StructLiteralExpr& sl) {
        resolve_type(*sl.type);
        for (FieldInit& f : sl.fields) resolve_expr(*f.value);
        const Symbol* t = sl.type->kind == TypeKind::named ? sl.type->as<NamedType>().symbol : nullptr;
        if (!t || t->kind == SymbolKind::type_param) return;
        std::string display = to_string(*sl.type);
        if (t->kind == SymbolKind::enum_ || t->kind == SymbolKind::interface) {
            Diagnostic& e = error(DiagCode::E3022, sl.type->span,
                                  std::format("{} '{}' cannot be constructed with '{{...}}'", sema::to_string(t->kind), display),
                                  "not a struct or class");
            if (t->kind == SymbolKind::enum_) note(e, std::format("write '{}::alternative'", display));
            return;
        }
        std::vector<Symbol*> fields;
        bool                 opaque = false;
        collect_fields(t, fields, opaque);
        std::map<std::string, const FieldInit*> given;
        for (const FieldInit& f : sl.fields) {
            auto field = std::find_if(fields.begin(), fields.end(), [&](Symbol* s) { return s->name == f.name; });
            if (field == fields.end()) {
                if (opaque) continue;
                std::vector<std::string> names;
                for (Symbol* s : fields) names.push_back(s->name);
                suggest(error(DiagCode::E3033, f.span, std::format("'{}' has no field '{}'", display, f.name), "unknown field"),
                        f.name, names);
                continue;
            }
            if (auto prev = given.find(f.name); prev != given.end()) {
                note(error(DiagCode::E3034, f.span, std::format("field '{}' is given twice", f.name), "given again"),
                     "first given here", prev->second->span);
                continue;
            }
            given[f.name] = &f;
            if ((*field)->access == Access::private_ && (*field)->parent->module != module_) {
                note(error(DiagCode::E3036, f.span,
                           std::format("field '{}' is private to module '{}'", f.name, (*field)->parent->module->name),
                           "cannot be set here"),
                     "outside its module a class's private fields always take their defaults");
            }
        }
        if (opaque) return;
        std::vector<std::string> missing;
        bool                     missing_private = false;
        for (Symbol* f : fields) {
            if (given.count(f->name) || f->decl->as<VariableDecl>().init) continue;
            missing.push_back("'" + f->name + "'");
            missing_private = missing_private || (f->access == Access::private_ && f->parent->module != module_);
        }
        if (!missing.empty()) {
            std::string list;
            for (std::size_t i = 0; i < missing.size(); ++i) list += (i ? ", " : "") + missing[i];
            Diagnostic& e = error(DiagCode::E3035, sl.type->span,
                                  std::format("missing {} {} in '{}{{...}}'", missing.size() == 1 ? "field" : "fields",
                                              list, display),
                                  "incomplete");
            note(e, "fields without a default value must be given");
            if (missing_private) {
                note(e, "private fields cannot be set outside their module; give them a default or provide a "
                        "static factory method");
            }
        }
    }

    void resolve_lambda(LambdaExpr& l) {
        for (Capture& c : l.captures) {
            if (c.init) resolve_expr(*c.init);
        }
        std::vector<Symbol*> outer(l.captures.size(), nullptr);
        for (std::size_t i = 0; i < l.captures.size(); ++i) {
            Capture& c = l.captures[i];
            if (c.kind == CaptureKind::init) continue;
            outer[i]   = lookup_local(c.name, c.span);
            c.captured = outer[i];
            if (!outer[i]) {
                Diagnostic& e = error(DiagCode::E3020, c.span, std::format("cannot capture '{}': no such local", c.name),
                                      "not a local variable");
                if (module_->items.count(c.name)) note(e, "module-level items can be used without capturing them");
                else suggest(e, c.name, value_candidates());
            } else if (c.kind == CaptureKind::move && outer[i]->is_reference) {
                error(DiagCode::E3032, c.span, std::format("'{}' is a reference and cannot be moved", c.name),
                      "reference");
            }
        }
        Scope boundary;
        boundary.lambda_boundary = true;
        scopes_.push_back(boundary);
        for (std::size_t i = 0; i < l.captures.size(); ++i) {
            Capture& c   = l.captures[i];
            bool     ref = c.kind == CaptureKind::ref || c.kind == CaptureKind::mut_ref;
            c.symbol     = declare_local(c.name, c.span, LocalKind::capture, c.kind == CaptureKind::mut_ref, ref,
                                         /*check_shadowing=*/c.kind == CaptureKind::init);
        }
        for (Param& p : l.params) {
            resolve_type(*p.type);
            bool ref = p.type->kind == TypeKind::reference;
            p.symbol = declare_local(p.name, p.span, ref ? LocalKind::ref_param : LocalKind::value_param, p.is_mut, ref);
        }
        if (l.result) resolve_type(*l.result);
        int saved_loops  = fn_.loop_depth;
        fn_.loop_depth   = 0;
        resolve_block(*l.body);
        fn_.loop_depth = saved_loops;
        scopes_.pop_back();
    }

    void resolve_pattern(Pattern& p) {
        switch (p.kind) {
        case PatternKind::wildcard:
        case PatternKind::literal:
        case PatternKind::range:
        case PatternKind::error: return;
        case PatternKind::binding: {
            auto& b  = p.as<BindingPattern>();
            b.symbol = declare_local(b.name, b.span, LocalKind::binding, false, false);
            return;
        }
        case PatternKind::tuple:
            for (PatternPtr& el : p.as<TuplePattern>().elements) resolve_pattern(*el);
            return;
        case PatternKind::enum_alternative: {
            auto&   ep  = p.as<EnumPattern>();
            Symbol* sym = lookup_path(ep.path, Want::value, ep.path.span);
            ep.symbol   = sym;
            std::string display = to_string(ep.path);
            if (sym && sym->kind != SymbolKind::alternative) {
                error(DiagCode::E3022, ep.path.span,
                      std::format("'{}' is a {}, not an enum alternative", display, sema::to_string(sym->kind)),
                      "not an alternative");
            } else if (sym) {
                bool payload = sym->alternative->payload != nullptr;
                if (payload && !ep.payload) {
                    note(error(DiagCode::E3037, ep.path.span, std::format("'{}' carries a value", display), "missing payload"),
                         std::format("match it with '{}(x)' or '{}(_)'", display, display));
                } else if (!payload && ep.payload) {
                    note(error(DiagCode::E3037, ep.payload->span, std::format("'{}' has no payload", display),
                               "nothing to match"),
                         std::format("write '{}' without '(...)'", display));
                }
            }
            if (ep.payload) resolve_pattern(*ep.payload);
            return;
        }
        case PatternKind::struct_: {
            auto&   sp  = p.as<StructPattern>();
            Symbol* sym = lookup_path(sp.path, Want::type, sp.path.span);
            for (FieldPattern& f : sp.fields) {
                if (sym && (sym->kind == SymbolKind::struct_ || sym->kind == SymbolKind::class_)) {
                    std::vector<Symbol*> fields;
                    bool                 opaque = false;
                    collect_fields(sym, fields, opaque);
                    bool found = std::any_of(fields.begin(), fields.end(), [&](Symbol* s) { return s->name == f.name; });
                    if (!found && !opaque) {
                        error(DiagCode::E3033, f.span, std::format("'{}' has no field '{}'", sym->name, f.name),
                              "unknown field");
                    }
                }
                resolve_pattern(*f.pattern);
            }
            return;
        }
        }
    }

    Program&                              program_;
    DiagnosticEngine&                     diags_;
    Module*                               module_    = nullptr;
    bool                                  in_header_ = false;
    std::map<const Decl*, Symbol*>        symbol_of_;
    std::vector<std::vector<Symbol*>>     template_params_;
    std::vector<Scope>                    scopes_;
    FunctionContext                       fn_;
    std::set<FileId>                      std_reported_;
    std::vector<Module*>                  std_modules_;     // the standard library's modules
    std::optional<AnnotationContext>      actx_;            // inside an annotation's checks or generated members
    std::map<const Decl*, GeneratedOrigin> generated_;      // generated members → where their code is written
    std::set<const TypeExpr*>             pre_resolved_;    // generated interfaces, resolved where written
    std::string                           ns_;              // namespace of the code being resolved
    std::set<std::string>                 clash_reported_;  // E3065 once per name
    std::set<const Symbol*>               mismatched_;     // header functions with a mismatching definition
    std::multimap<std::string, Span>      declared_later_;   // locals of the current function   // "std not imported" once per file
};

// --- edaxc symbols ------------------------------------------------------------------------

std::string fn_line(const Symbol& s) {
    const FunctionDecl& f = *s.function();
    std::string out;
    out += template_sig(f).empty() ? "" : template_sig(f) + " ";
    out += f.is_static && s.parent ? "static " : "";
    out += f.is_virtual ? "virtual " : "";
    out += f.is_destructor ? "destructor " : s.parent ? "method " : "fn ";
    out += (s.parent ? s.name : s.key()) + params_sig(f);
    if (f.result) out += " -> " + to_string(*f.result);
    out += f.is_override ? " override" : "";
    out += f.is_abstract ? " abstract" : "";
    return out;
}

std::string origin(const Symbol& s) {
    if (s.in_header) return s.definition ? "  [header, defined in source]" : "  [header]";
    return "";
}

std::string access_prefix(const Symbol& s) {
    if (!s.parent || s.parent->kind != SymbolKind::class_) return "";
    switch (s.access) {
    case Access::public_: return "public ";
    case Access::protected_: return "protected ";
    case Access::private_: return "private ";
    case Access::none: return "";
    }
    return "";
}

} // namespace

void resolve_program(Program& program, DiagnosticEngine& diags) { Resolver(program, diags).run(); }

Symbol* resolve_instance(Program& program, DiagnosticEngine& diags, const Symbol* templ, Decl& decl,
                         std::vector<Symbol*>& params) {
    bool     from_header = !templ->definition && !templ->source_block && templ->in_header;
    Resolver r(program, diags);
    r.set_namespace(templ->ns);   // names in a template resolve where it is written (spec 14, 30)
    return r.build_instance(*templ->module, from_header, decl, params);
}

Symbol* resolve_function_instance(Program& program, DiagnosticEngine& diags, const Symbol* templ,
                                  FunctionDecl& decl, std::vector<Symbol*>& params) {
    bool     from_header = !templ->definition && templ->in_header;
    Resolver r(program, diags);
    r.set_namespace(templ->ns);
    return r.build_function_instance(*templ->module, from_header, decl, params);
}

namespace {

std::string source_text(const SourceManager& sources, Span span) {
    std::string_view text = sources.file(span.file).text();
    if (span.end > text.size() || span.begin > span.end) return {};
    // From the start of the line, so the first line keeps its indentation.
    std::size_t begin = span.begin;
    while (begin > 0 && (text[begin - 1] == ' ' || text[begin - 1] == '\t')) --begin;
    return std::string(text.substr(begin, span.end - begin));
}

// Removes the common indentation of the lines of `text` and indents them by `indent`.
std::string reindent(const std::string& text, std::string_view indent) {
    std::vector<std::string> lines;
    std::size_t              at = 0;
    while (at <= text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        lines.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    std::size_t common = std::string::npos;
    for (const std::string& l : lines) {
        std::size_t n = l.find_first_not_of(" \t");
        if (n != std::string::npos) common = std::min(common, n);
    }
    if (common == std::string::npos) common = 0;
    std::string out;
    for (const std::string& l : lines) {
        if (l.find_first_not_of(" \t") == std::string::npos) {
            out += "\n";
            continue;
        }
        out += std::string(indent) + l.substr(common) + "\n";
    }
    return out;
}

// `text` with the word `annotated` replaced by `name` (the generated code names its type).
std::string replace_annotated(std::string text, const std::string& name) {
    const std::string word = "annotated";
    for (std::size_t at = text.find(word); at != std::string::npos; at = text.find(word, at)) {
        bool left  = at == 0 || !(std::isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_');
        bool right = at + word.size() >= text.size() ||
                     !(std::isalnum(static_cast<unsigned char>(text[at + word.size()])) || text[at + word.size()] == '_');
        if (left && right) {
            text.replace(at, word.size(), name);
            at += name.size();
        } else {
            at += word.size();
        }
    }
    return text;
}

} // namespace

bool print_expansion(std::ostream& out, const Program& program, const SourceManager& sources, std::string_view type) {
    const Symbol* found = nullptr;
    for (const auto& [name, m] : program.modules) {
        for (const Symbol* s : m->item_order) {
            if (!s->is_type()) continue;
            if (s->key() == type || (s->name == type && m.get() == program.entry)) found = found ? found : s;
        }
    }
    if (!found) return false;
    out << "// " << found->key() << " as written in " << sources.file(found->decl->span.file).path()
        << ", with the members its annotations generate (spec 29)\n";
    std::string written = source_text(sources, found->decl->span);
    out << reindent(written, "");
    if (found->source_block) out << "\n" << reindent(source_text(sources, found->source_block->span), "");
    bool any = false;
    for (const Symbol* m : found->member_order) {
        const ast::AnnotationUse* by = m->decl ? m->decl->generated_by : nullptr;
        if (!by) continue;
        if (!any) out << "\n// generated:\n";
        any = true;
        std::string text = replace_annotated(source_text(sources, m->decl->span), found->name);
        out << "\n    // by @" << ast::to_string(by->path) << "\n" << reindent(text, "    ");
    }
    // Interfaces the annotations add.
    const auto& td = found->decl->kind == DeclKind::struct_ || found->decl->kind == DeclKind::class_ ? &found->decl->as<TypeDecl>() : nullptr;
    if (td) {
        std::string bases;
        for (const Symbol* b : found->bases) bases += (bases.empty() ? "" : ", ") + b->key();
        if (!bases.empty()) out << "\n// implements: " << bases << "\n";
    }
    if (!any) out << "\n// (no generated members)\n";
    return true;
}

void print_symbols(std::ostream& out, const Module& m, const SourceManager& sources) {
    out << "module " << m.name << '\n';
    if (m.header) out << "  header: " << sources.file(m.header->id).path() << '\n';
    if (m.source) out << "  source: " << sources.file(m.source->id).path() << '\n';
    std::vector<std::string> imports;
    if (m.header_imports_std || m.source_imports_std) imports.push_back("std");
    for (const Module* i : m.header_imports) imports.push_back(i->name);
    for (const Module* i : m.source_imports) imports.push_back(i->name + " (source)");
    if (!imports.empty()) {
        out << "  imports:";
        for (const std::string& i : imports) out << ' ' << i;
        out << '\n';
    }
    for (const Symbol* s : m.item_order) {
        std::string priv = s->is_private ? "private " : "";
        switch (s->kind) {
        case SymbolKind::function: out << "  " << priv << fn_line(*s) << origin(*s) << '\n'; break;
        case SymbolKind::global: {
            const auto& v = s->decl->as<VariableDecl>();
            out << "  " << priv << (v.storage == Storage::compile_time ? "compile_time " : "static ")
                << (v.is_mut ? "mut " : "") << s->key() << " : " << to_string(*v.type) << '\n';
            break;
        }
        case SymbolKind::enum_: {
            out << "  " << priv << "enum " << s->key() << " : " << to_string(*s->decl->as<EnumDecl>().tag_type) << '\n';
            for (const Symbol* a : s->member_order) {
                out << "    " << a->name;
                if (a->alternative->payload) out << '(' << to_string(*a->alternative->payload) << ')';
                out << '\n';
            }
            break;
        }
        case SymbolKind::struct_:
        case SymbolKind::class_:
        case SymbolKind::interface: {
            const auto& t = s->decl->as<TypeDecl>();
            out << "  " << priv << (t.is_open ? "open " : "") << sema::to_string(s->kind) << ' ' << s->key();
            if (!t.bases.empty()) out << " : " << bases_sig(t);
            out << '\n';
            std::vector<std::string> layout;
            for (const Symbol* mem : s->member_order) {
                if (mem->kind == SymbolKind::method) {
                    out << "    " << access_prefix(*mem) << fn_line(*mem) << origin(*mem) << '\n';
                } else {
                    const auto& v = mem->decl->as<VariableDecl>();
                    out << "    " << access_prefix(*mem) << (mem->is_static ? "static " : "") << "field " << mem->name
                        << " : " << to_string(*v.type) << (v.init ? " = ..." : "") << '\n';
                    if (!mem->is_static) layout.push_back(mem->name);
                }
            }
            if (s->kind != SymbolKind::interface) {
                out << "    layout:";
                for (const std::string& l : layout) out << ' ' << l;
                out << '\n';
            }
            break;
        }
        default: break;
        }
    }
}

} // namespace edax
