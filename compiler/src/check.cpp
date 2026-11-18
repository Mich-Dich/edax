#include "rift/check.hpp"

#include "rift/ast_clone.hpp"
#include "rift/ast_printer.hpp"
#include "rift/compile_time.hpp"
#include "rift/lexer.hpp"
#include "rift/parser.hpp"
#include "rift/resolve.hpp"
#include "rift/types.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <memory>
#include <set>

namespace rift {

using namespace ast;
using sema::Module;
using sema::Symbol;
using sema::SymbolKind;
using types::Kind;
using types::Type;

namespace {

std::string quoted(const Type* t) { return "'" + types::to_string(t) + "'"; }

/// cast<T> conversions that never lose information (spec 17).
bool lossless(const Type* from, const Type* to) {
    if (from == to) return true;
    if (from->is(Kind::bool_) && to->is_integer()) return true;
    if (from->is_integer() && to->is_integer()) {
        auto min_bits = [](const Type* t) { return t->is_size ? 32 : t->bits; };   // size types: 32..64
        auto max_bits = [](const Type* t) { return t->is_size ? 64 : t->bits; };
        if (from->is_signed == to->is_signed) return max_bits(from) <= min_bits(to);
        if (!from->is_signed && to->is_signed) return max_bits(from) < min_bits(to);
        return false;   // signed -> unsigned can lose the sign
    }
    if (from->is_integer() && to->is_float()) {
        int bits = from->is_size ? 64 : from->bits;
        return to->bits == 32 ? bits <= 16 : bits <= 32;
    }
    if (from->is_float() && to->is_float()) return from->bits <= to->bits;
    if (from->is_user(SymbolKind::enum_) && to->is_integer()) {
        // A simple enum converts to its tag type (checked by the caller against the tag).
        return true;
    }
    return false;
}

bool is_comparison(TokenKind op) {
    return op == TokenKind::less || op == TokenKind::less_eq || op == TokenKind::greater ||
           op == TokenKind::greater_eq;
}

bool is_arithmetic(TokenKind op) {
    return op == TokenKind::plus || op == TokenKind::minus || op == TokenKind::star ||
           op == TokenKind::slash || op == TokenKind::percent;
}

bool is_wrapping(TokenKind op) {
    return op == TokenKind::plus_percent || op == TokenKind::minus_percent || op == TokenKind::star_percent;
}

bool is_bitwise(TokenKind op) {
    return op == TokenKind::amp || op == TokenKind::pipe || op == TokenKind::caret;
}

bool is_shift(TokenKind op) { return op == TokenKind::shl || op == TokenKind::shr; }

/// The binary operator a compound assignment applies: `+=` -> `+`.
TokenKind compound_base(TokenKind op) {
    switch (op) {
    case TokenKind::plus_eq: return TokenKind::plus;
    case TokenKind::minus_eq: return TokenKind::minus;
    case TokenKind::star_eq: return TokenKind::star;
    case TokenKind::slash_eq: return TokenKind::slash;
    case TokenKind::percent_eq: return TokenKind::percent;
    case TokenKind::amp_eq: return TokenKind::amp;
    case TokenKind::pipe_eq: return TokenKind::pipe;
    case TokenKind::caret_eq: return TokenKind::caret;
    case TokenKind::shl_eq: return TokenKind::shl;
    case TokenKind::shr_eq: return TokenKind::shr;
    case TokenKind::plus_percent_eq: return TokenKind::plus_percent;
    case TokenKind::minus_percent_eq: return TokenKind::minus_percent;
    case TokenKind::star_percent_eq: return TokenKind::star_percent;
    default: return TokenKind::eq;
    }
}

/// Does evaluating this expression/statement always leave the function (return, panic, an
/// endless loop)? Used for "may end without returning" and for `never` block types.
bool diverges(const Expr& e);

bool diverges_block(const BlockExpr& b) {
    for (const StmtPtr& s : b.statements) {
        switch (s->kind) {
        case StmtKind::expression:
            if (diverges(*s->as<ExprStmt>().expr)) return true;
            break;
        case StmtKind::variable:
            if (s->as<VariableStmt>().init && diverges(*s->as<VariableStmt>().init)) return true;
            break;
        case StmtKind::discard:
            if (diverges(*s->as<DiscardStmt>().value)) return true;
            break;
        case StmtKind::assign:
        case StmtKind::check: break;
        }
    }
    return b.tail && diverges(*b.tail);
}

bool contains_break(const BlockExpr& b) {
    bool found = false;
    std::function<void(const Expr&)> walk = [&](const Expr& e) {
        switch (e.kind) {
        case ExprKind::break_: found = true; break;
        case ExprKind::block:
            for (const StmtPtr& s : e.as<BlockExpr>().statements) {
                if (s->kind == StmtKind::expression) walk(*s->as<ExprStmt>().expr);
            }
            if (e.as<BlockExpr>().tail) walk(*e.as<BlockExpr>().tail);
            break;
        case ExprKind::if_:
            walk(*e.as<IfExpr>().then_block);
            if (e.as<IfExpr>().else_branch) walk(*e.as<IfExpr>().else_branch);
            break;
        case ExprKind::match:
            for (const MatchArm& a : e.as<MatchExpr>().arms) walk(*a.body);
            break;
        default: break;   // nested loops own their breaks; lambdas cannot break out
        }
    };
    walk(b);
    return found;
}

bool diverges(const Expr& e) {
    switch (e.kind) {
    case ExprKind::return_:
    case ExprKind::break_:
    case ExprKind::continue_: return true;
    case ExprKind::block: return diverges_block(e.as<BlockExpr>());
    case ExprKind::if_: {
        const auto& i = e.as<IfExpr>();
        return i.else_branch && diverges_block(*i.then_block) && diverges(*i.else_branch);
    }
    case ExprKind::match: {
        const auto& m = e.as<MatchExpr>();
        if (diverges(*m.scrutinee)) return true;
        return !m.arms.empty() && std::all_of(m.arms.begin(), m.arms.end(), [](const MatchArm& a) { return diverges(*a.body); });
    }
    case ExprKind::while_: {
        const auto& w = e.as<WhileExpr>();
        bool always = w.condition->kind == ExprKind::bool_literal && w.condition->as<LiteralExpr>().bool_value;
        return always && !contains_break(*w.body);
    }
    case ExprKind::call:
        return e.type && e.type->is(Kind::never);
    default: return e.type && e.type->is(Kind::never);
    }
}

struct FunctionState {
    const Type*         result      = nullptr;   // null: unknown (lambda without '->')
    bool                in_function = false;
    Symbol*             self        = nullptr;
    bool                has_this    = false;
    bool                this_mut    = false;
    bool                is_compile_time = false;
    bool                generic = false;   // the generic check of a template (parameters unknown)
    bool                deducing = false;  // a lambda without '->', or a `deduce` function: result from `return`
    const Type*         deduced  = nullptr;
    std::string         name;
    const FunctionDecl* decl = nullptr;   // the function being checked (not for lambdas)
};

/// Where an expression's value lives, for assignment and `mut` checks.
struct Place {
    bool        is_place   = false;
    bool        is_mutable = false;
    bool        via_this   = false;
    bool        is_temporary = false;
    std::string name;
    const Symbol* local = nullptr;
};

class Instantiator;

class Checker {
public:
    Checker(sema::Program& program, DiagnosticEngine& diags, Instantiator* instantiator = nullptr)
        : program_(program), diags_(diags), tc_(*program.types), instantiator_(instantiator) {}

    void run() {
        setup_mode();
        for (auto& [name, m] : program_.modules) {
            module_ = m.get();
            if (m->header) check_file(m->header->ast);
            if (m->source) check_file(m->source->ast);
        }
        if (program_.entry) check_main(*program_.entry);
    }

    /// build.rifts (spec 25): runs `compile_time void build(mut std::build::graph& g)`, whose
    /// calls of the graph's methods fill in program_.build_graph.
    void run_build() {
        Module* m = program_.entry;
        if (!m) return;
        auto it = m->items.find("build");
        const Symbol* fn = it == m->items.end() ? nullptr : it->second.front();
        const FunctionDecl* f = fn ? fn->function() : nullptr;
        const Symbol* graph = nullptr;
        if (auto bm = program_.modules.find("std::build"); bm != program_.modules.end()) {
            if (auto g = bm->second->items.find("graph"); g != bm->second->items.end()) graph = g->second.front();
        }
        Span where = m->source ? Span{m->source->id, 0, 0} : Span{};
        if (!f || !f->is_compile_time || f->params.size() != 1 || !graph) {
            note(error(DiagCode::E4216, fn ? fn->span : where, "build.rifts defines no 'build' function", "no build function"),
                 "write 'compile_time void build(mut std::build::graph& g) { ... }' (spec 25)");
            return;
        }
        ct::Value g;
        g.kind = ct::Value::Kind::aggregate;
        g.type = tc_.user(graph);
        evaluator().call(fn, {g}, fn->span);
    }

    /// `if compile_time (c) { checks }` as a statement of a function's entry, or null.
    static IfExpr* entry_gate(Stmt& st) {
        if (st.kind != StmtKind::expression) return nullptr;
        Expr& e = *st.as<ExprStmt>().expr;
        if (e.kind != ExprKind::if_) return nullptr;
        auto& i = e.as<IfExpr>();
        if (!i.is_compile_time || i.else_branch || i.then_block->tail || i.then_block->statements.empty()) return nullptr;
        for (StmtPtr& inner : i.then_block->statements) {
            if (inner->kind != StmtKind::check) return nullptr;
        }
        return &i;
    }

    /// The class of the build mode and the values of its settings (spec 24): the interpreter
    /// runs the mode's methods once. A mode that removes guards safe code relies on must also
    /// return true from `allow_undefined_behavior()` (E4215).
    void setup_mode() {
        if (!program_.build_mode_class) {
            for (const char* module : {"std::build::mode", "build"}) {
                auto m = program_.modules.find(module);
                if (m == program_.modules.end()) continue;
                auto it = m->second->items.find(program_.build_mode);
                if (it != m->second->items.end() && it->second.front()->kind == SymbolKind::class_) {
                    program_.build_mode_class = it->second.front();
                    break;
                }
            }
        }
        if (!program_.build_mode_class) {   // an unknown mode (the driver says so): checked as debug
            if (auto m = program_.modules.find("std::build::mode"); m != program_.modules.end()) {
                if (auto it = m->second->items.find("debug"); it != m->second->items.end()) program_.build_mode_class = it->second.front();
            }
        }
        const Symbol* mode = program_.build_mode_class;
        if (!mode) return;
        for (const char* name : {"optimization", "debug_info", "overflow", "shifts", "bounds", "run_time_checks", "panic",
                                 "link_time_optimization", "allow_undefined_behavior"}) {
            std::shared_ptr<const ct::Value> v = evaluator().mode_setting(mode, name);
            if (!v) continue;
            program_.mode_settings[name] = v->kind == ct::Value::Kind::boolean ? (v->boolean ? "true" : "false")
                                         : v->symbol                          ? v->symbol->name
                                                                              : ct::to_string(*v);
        }
        auto& ms = program_.mode_settings;
        bool unsafe_mode = ms["bounds"] == "unchecked" || (ms["run_time_checks"] != "all" && !ms["run_time_checks"].empty());
        if (unsafe_mode && ms["allow_undefined_behavior"] != "true") {
            Diagnostic& d = error(DiagCode::E4215, mode->span,
                                  std::format("the mode '{}' turns off {} but does not allow undefined behaviour", mode->name,
                                              ms["bounds"] == "unchecked" ? "bounds checks" : "run_time checks"),
                                  "unsafe mode");
            note(d, "override 'bool allow_undefined_behavior() override { return true; }' to say so (spec 24)");
        }
    }

    /// A template value argument written in a type (`box<N + 1>`), with `subst` for the
    /// template parameters in scope there.
    const Type* value_arg_for(const Symbol* templ, std::size_t index, Expr& value,
                              const std::map<const Symbol*, const Type*>* subst) {
        if (subst) subst_ = *subst;
        for (const auto& [p, t] : subst_) {
            if (t->is_unknown()) return tc_.error();   // the generic check of a template
        }
        if (templ->module) module_ = templ->module;
        return evaluate_value_arg(templ, index, value);
    }

    /// The count of a `std::raw::array<T, N>`: a compile-time usize.
    const Type* count_for(Expr& value, const std::map<const Symbol*, const Type*>* subst) {
        if (subst) subst_ = *subst;
        for (const auto& [p, t] : subst_) {
            if (t->is_unknown()) return tc_.error();
        }
        if (program_.std_module) module_ = program_.std_module;
        const Type*  want   = tc_.builtin("usize");
        std::size_t  errors = diags_.error_count();
        infer(value);
        coerce(value, want);
        if (diags_.error_count() != errors) return tc_.error();
        std::shared_ptr<const ct::Value> v = evaluate(value, "the size of a std::raw::array must be known at compile time");
        if (!v) return tc_.error();
        return tc_.value_arg(v->integer, want);
    }

    /// Checks one function on demand (a compile_time function that compile-time code calls
    /// before the normal pass reaches it).
    void check_function_symbol(const Symbol* s) {
        const Decl* d = s->definition ? s->definition : s->decl;
        if (!d || d->kind != DeclKind::function) return;
        module_ = s->module;
        if (const auto* subst = sema::substitution_of(s)) subst_ = *subst;
        Symbol* self = s->parent && s->parent->is_type() ? s->parent : nullptr;
        check_function(const_cast<FunctionDecl&>(d->as<FunctionDecl>()), self);
    }

    /// Checks (and evaluates) a global compile_time constant on demand.
    void check_global_symbol(const Symbol* g) {
        if (!g->decl) return;
        module_ = g->module;
        check_decl(const_cast<Decl&>(*g->decl), nullptr);
    }

    /// Checks the members of a template instance (spec 14): `copy` is the instance's own copy of
    /// the template's declaration, checked with the parameters replaced by their arguments.
    void check_instance(Symbol* inst, Decl& copy) {
        module_ = inst->module;
        subst_  = inst->template_subst;
        if (copy.kind == DeclKind::enum_) {
            check_decl(copy, inst);
            return;
        }
        if (copy.kind == DeclKind::function) {
            check_decl(copy, inst->kind == SymbolKind::method ? inst->parent : nullptr);
            return;
        }
        check_promises(copy.as<TypeDecl>(), copy.kind);
        for (DeclPtr& m : copy.as<TypeDecl>().members) {   // `void` is a template argument for results only (spec 02, 27)
            if (m->kind != DeclKind::field || m->as<VariableDecl>().storage != Storage::local) continue;
            const Type* ft = lower(*m->as<VariableDecl>().type);
            if (ft->is(Kind::void_)) {
                note(error(DiagCode::E4180, instance_site_ ? *instance_site_ : m->name_span,
                           std::format("'{}' would have a field '{}' of type void", types::to_string(tc_.user(inst)), m->name),
                           "void field"),
                     "void can be a template argument only where it is a result or a payload (std::option<void>, std::result<void, E>)",
                     m->name_span);
            }
        }
        for (DeclPtr& m : copy.as<TypeDecl>().members) {
            bool lazy = m->kind == DeclKind::function && !m->as<FunctionDecl>().is_destructor &&
                        !m->as<FunctionDecl>().is_virtual && !m->as<FunctionDecl>().is_override;
            std::size_t mark = diags_.size();
            check_decl(*m, inst);
            if (!lazy || diags_.size() == mark) continue;
            std::vector<Diagnostic> found = diags_.take_since(mark);
            bool errors = std::any_of(found.begin(), found.end(), [](const Diagnostic& d) { return d.severity == Severity::error; });
            if (errors) {
                program_.deferred_errors[m.get()] = std::move(found);   // reported if the method is used
            } else {
                for (Diagnostic& d : found) diags_.add(std::move(d));
            }
        }
    }

    /// A method of a template instance is used (called, or an operator): its deferred errors
    /// are reported now, at the use.
    void use_method(const Symbol* s, Span at) {
        if (!s || !s->decl) return;
        auto it = program_.deferred_errors.find(s->decl);
        if (it == program_.deferred_errors.end()) return;
        std::vector<Diagnostic> found = std::move(it->second);
        program_.deferred_errors.erase(it);
        std::string owner = s->parent ? types::to_string(tc_.user(s->parent)) + "::" : "";
        Diagnostic& d = error(DiagCode::E4214, at, std::format("'{}{}' does not compile for this instance", owner, s->name),
                              "used here");
        for (const Diagnostic& e : found) {
            if (e.severity == Severity::error) d.notes.push_back(Note{e.message, e.span});
        }
    }

private:
    // --- diagnostics ----------------------------------------------------------------------

    Diagnostic& error(DiagCode code, Span span, std::string message, std::string label = {}) {
        return diags_.report(code, span, std::move(message), std::move(label));
    }
    static void note(Diagnostic& d, std::string message, std::optional<Span> span = std::nullopt) {
        d.notes.push_back(Note{std::move(message), span});
    }

    // --- lowering written types -----------------------------------------------------------

    /// A type written in the code being checked: template parameters are those of the instance
    /// being checked (`subst_`), if any.
    const Type* lower(const TypeExpr& t, const std::map<const Symbol*, const Type*>* subst = nullptr) {
        return tc_.lower(t, subst ? subst : &subst_);
    }

    /// The parameter substitution of the template instance that declares `s` (a member, payload
    /// or alternative), or null outside instances.
    static const std::map<const Symbol*, const Type*>* substitution_of(const Symbol* s) {
        for (; s; s = s->parent) {
            if (!s->template_subst.empty()) return &s->template_subst;
        }
        return nullptr;
    }

    /// The alternative that `alt` stands for in the enum type `enum_type`: an instance of a template
    /// enum has its own alternatives (spec 14). `alt` itself otherwise.
    static Symbol* instance_alternative(Symbol* alt, const Type* enum_type) {
        const Symbol* templ = alt->parent;
        if (!templ || !templ->decl || !templ->decl->is_template || templ->template_of) return alt;
        if (!enum_type || !enum_type->is(Kind::user) || enum_type->symbol->template_of != templ) return alt;
        auto it = enum_type->symbol->members.find(alt->name);
        return it == enum_type->symbol->members.end() ? alt : it->second.front();
    }

    /// The enum type that an alternative path names: `maybe<u32>::some` names `maybe<u32>`
    /// (spec 14). A plain enum's alternative names the enum.
    const Type* enum_type_of(const Symbol* alt, const Path& path) {
        const Symbol* templ = alt->parent;
        if (!templ || !templ->decl || !templ->decl->is_template || templ->template_of || path.segments.size() < 2) {
            return tc_.user(templ);
        }
        const PathSegment& seg = path.segments[path.segments.size() - 2];
        std::vector<const Type*> args;
        for (const TemplateArg& a : seg.template_args) args.push_back(a.type ? lower(*a.type) : tc_.error());
        if (args.size() != templ->decl->template_params.size()) {
            error(DiagCode::E4180, path.span,
                  std::format("'{}' takes {} template argument{}, but {} {} given", templ->name,
                              templ->decl->template_params.size(), templ->decl->template_params.size() == 1 ? "" : "s",
                              args.size(), args.size() == 1 ? "is" : "are"),
                  "wrong number of template arguments");
            return tc_.error();
        }
        return tc_.instance(templ, args);
    }

    /// A type written in the declaration of `owner` (a field, payload, method signature, ...),
    /// which may belong to a template instance other than the code being checked (spec 14).
    const Type* lower_owned(const Symbol* owner, const TypeExpr& t) { return tc_.lower(t, substitution_of(owner)); }

    void set_local(sema::Symbol* sym, const Type* t) {
        local_types_[sym] = t;
        sym->type         = t;
    }

    const Type* type_of_symbol(const Symbol* s) {
        if (!s) return tc_.error();
        switch (s->kind) {
        case SymbolKind::local: {
            auto it = local_types_.find(s);
            if (it != local_types_.end()) return it->second;
            return s->type ? s->type : tc_.error();   // a local of code checked by another checker
        }
        case SymbolKind::global: return lower(*s->decl->as<VariableDecl>().type);
        case SymbolKind::field: return lower_owned(s, *s->decl->as<VariableDecl>().type);
        case SymbolKind::alternative: return tc_.user(s->parent);
        default: return tc_.error();
        }
    }

    // --- assignability --------------------------------------------------------------------

    static bool is_ancestor(const Symbol* t, const Symbol* base) {
        for (const Symbol* b : t->bases) {
            if (b == base || is_ancestor(b, base)) return true;
        }
        return false;
    }

    /// 0 = exact, 1 = upcast, -1 = not assignable. `from` is the value's type, `to` the target.
    int assign_cost(const Type* from, const Type* to) {
        if (!from || !to) return 0;
        bool to_ref = to->is(Kind::reference) || to->is(Kind::pointer);
        const Type* f = types::strip_ref(from);
        const Type* t = types::strip_ref(to);
        if (f->is_unknown() || t->is_unknown()) return 0;
        if (f == t || f->is(Kind::never)) return 0;
        if (f->is(Kind::external) && t->is(Kind::external)) return 0;
        if (f->is(Kind::static_str) && t->is(Kind::external)) return 0;
        if (is_std_string(f) && t->is(Kind::external) && t->name == "std::str" && !to_ref) return 0;   // a borrow (spec 19)
        if (f->is(Kind::static_str) && is_std_string(t) && !to_ref) return 0;   // allocates, with a warning
        if (f->is(Kind::nullptr_) && t->is(Kind::pointer)) return 0;
        if (f->is(Kind::pointer) && t->is(Kind::pointer)) {
            if (!f->is_mut && t->is_mut) return -1;
            if (f->inner == t->inner || f->inner->is_unknown() || t->inner->is_unknown()) return 0;
            if (f->inner->is(Kind::user) && t->inner->is(Kind::user) && is_ancestor(f->inner->symbol, t->inner->symbol)) return 1;
            return -1;
        }
        if (f->is(Kind::user) && t->is(Kind::user) && to_ref && is_ancestor(f->symbol, t->symbol)) return 1;
        if (smart_upcast(f, t)) return 1;
        if (lambda_fits(f, t) && !to_ref) return 0;
        return -1;
    }

    /// Whether `t` is `std::string`: the library's class, or the name before std is imported.
    bool is_std_string(const Type* t) const {
        if (!t) return false;
        if (t->is(Kind::user)) return t->symbol == std_item("string");
        return t->is(Kind::external) && t->name == "std::string";
    }

    /// A lambda converts to `std::function<R(A...)>` with the same signature, and to
    /// `std::fn_ptr<R(A...)>` when it captures nothing (spec 18).
    bool lambda_fits(const Type* f, const Type* t) const {
        if (!f || !t || !f->is(Kind::closure) || !(t->is(Kind::function) || t->is(Kind::fn_ptr))) return false;
        if (f->args != t->args) return false;
        if (t->is(Kind::fn_ptr)) {
            const ast::LambdaExpr* l = tc_.lambda_of(f);
            if (!l || !l->captures.empty()) return false;
        }
        return true;
    }

    /// `std::unique_ptr<circle>` → `std::unique_ptr<shape>` (and shared_ptr, weak_ptr): an
    /// implicit upcast, like `circle&` → `shape&` (spec 07, 13).
    bool smart_upcast(const Type* f, const Type* t) const {
        if (!f || !t || !f->is(Kind::user) || !t->is(Kind::user) || f == t) return false;
        for (const char* name : {"unique_ptr", "shared_ptr", "weak_ptr"}) {
            const Symbol* templ = std_item(name);
            if (!templ || f->symbol->template_of != templ || t->symbol->template_of != templ) continue;
            if (f->symbol->template_args.size() != 1 || t->symbol->template_args.size() != 1) return false;
            const Type* fa = f->symbol->template_args[0];
            const Type* ta = t->symbol->template_args[0];
            return fa->is(Kind::user) && ta->is(Kind::user) && is_ancestor(fa->symbol, ta->symbol);
        }
        return false;
    }

    void mismatch(Span span, const Type* found, const Type* expected, std::string_view what = "") {
        const Type* f = types::strip_ref(found);
        const Type* t = types::strip_ref(expected);
        if (f && f->is(Kind::type_value) && t && !t->is_unknown()) {
            Diagnostic& d = error(DiagCode::E3022, span, std::format("expected {}{}, found a type", quoted(expected), what),
                                  "this is a type, not a value");
            if (t->is(Kind::user) && (t->symbol->kind == SymbolKind::struct_ || t->symbol->kind == SymbolKind::class_)) {
                note(d, std::format("construct a value with '{}{{ ... }}'", types::to_string(t)));
            }
            return;
        }
        std::string found_text = found == tc_.untyped_int()     ? "an integer literal"
                               : found == tc_.untyped_float()   ? "a float literal"
                               : found == tc_.untyped_default() ? "'default'"
                               : found->is(Kind::reference) && !expected->is(Kind::reference) ? quoted(f)   // a reference reads the value
                                                                : quoted(found);
        Diagnostic& e = error(DiagCode::E4010, span,
                              std::format("expected {}{}, found {}", quoted(expected), what, found_text),
                              std::format("this is {}", found_text));
        if (f->is_numeric() && t->is_numeric()) {
            if (lossless(f, t)) {
                note(e, std::format("there are no implicit conversions; write 'cast<{}>(...)'", types::to_string(t)));
            } else {
                note(e, std::format("this conversion can lose data: use 'cast::truncate<{0}>', 'cast::saturate<{0}>' "
                                    "or 'cast::checked<{0}>'",
                                    types::to_string(t)));
            }
        } else if (f->is_integer() && t->is(Kind::bool_)) {
            note(e, "integers convert to bool only in conditions; write 'x != 0'");
        } else if (f->is(Kind::user) && t->is(Kind::user) && is_ancestor(f->symbol, t->symbol)) {
            note(e, "copying a derived object into a base value would slice it; use a reference");
        }
    }

    // --- the standard library's core types (spec 08, 11) ------------------------------------

    /// An item of the built-in std module (`option`, `result`, `exit_code`), or null when no
    /// file imports std.
    const Symbol* std_item(std::string_view name) const {
        const Module* m = program_.std_module;
        if (!m) return nullptr;
        auto it = m->items.find(std::string(name));
        return it == m->items.end() ? nullptr : it->second.front();
    }

    /// `std::ordering`, the result of `<=>` (spec 16).
    const Type* ordering_type() {
        if (const Symbol* o = std_item("ordering")) return tc_.user(o);
        return tc_.external("std::ordering");
    }

    /// Whether `t` (references removed) is an instance of the std template `name`: `std::option<u32>`.
    static bool is_std_instance(const Type* t, std::string_view name) { return types::is_std_instance(t, name); }

    /// `r?` (spec 11): unwraps `ok`/`some`, or returns the error / `none` from the function.
    const Type* infer_try(TryExpr& t) {
        const Type* o = infer(*t.operand);
        const Type* s = types::strip_ref(o);
        if (s->is(Kind::pending)) {
            no_context(*t.operand, "before '?'");
            return set(t, tc_.error());
        }
        if (s->is_unknown() || s->is(Kind::external) || s->is(Kind::never)) return set(t, tc_.error());
        bool is_result = is_std_instance(s, "result");
        if (!is_result && !is_std_instance(s, "option")) {
            if (s->is_untyped()) settle(*t.operand, tc_.error());
            note(error(DiagCode::E4190, t.span, std::format("'?' needs a std::result or std::option, but this is {}", quoted(o)),
                       "not a result or option"),
                 "'?' unwraps 'std::ok(x)' / 'std::some(x)' and returns anything else from the function");
            return set(t, tc_.error());
        }
        const Type* r = fn_.result;
        if (fn_.in_function && r && !r->is_unknown()) {
            std::string_view kind = is_result ? "result" : "option";
            if (!is_std_instance(r, kind)) {
                Diagnostic& d = error(DiagCode::E4191, t.span,
                                      std::format("'?' would return from '{}', which returns {}, not a std::{}", fn_.name,
                                                  quoted(r), kind),
                                      std::format("returns the {} early", is_result ? "error" : "'none'"));
                note(d, std::format("'?' on a std::{0} works only in functions that return std::{0}; use 'match' or "
                                    "'.value_or(...)' here",
                                    kind));
            } else if (is_result && types::strip_ref(r)->symbol->template_args[1] != s->symbol->template_args[1]) {
                const Type* want = types::strip_ref(r)->symbol->template_args[1];
                const Type* have = s->symbol->template_args[1];
                Diagnostic& d = error(DiagCode::E4191, t.span,
                                      std::format("'?' would return the error {}, but '{}' returns errors of type {}",
                                                  quoted(have), fn_.name, quoted(want)),
                                      "different error type");
                note(d, std::format("there is no implicit error conversion; convert it with '.or_error(...)' or "
                                    "'.map_error(...)' before '?'"));
            }
        }
        return set(t, s->symbol->template_args[0]);
    }

    /// `value_or`, `expect`, `or_error`, `map_error` on std::option / std::result (spec 11). They
    /// consume the receiver.
    const Type* infer_std_method(CallExpr& c, const Type* t, const std::string& name, std::vector<Expr*>& args) {
        c.callee->type     = tc_.error();
        const Type* value  = t->symbol->template_args[0];
        if (types::is_std_query_method(name)) {   // `o.is_some()`: a bool, the value stays
            if (!args.empty()) {
                error(DiagCode::E4030, c.span, std::format("'{}' takes no arguments", name), "wrong number of arguments");
            }
            return set(c, tc_.bool_());
        }
        if (args.size() != 1) {
            error(DiagCode::E4030, c.span, std::format("'{}' takes 1 argument, but {} {} given", name, args.size(),
                                                       args.size() == 1 ? "is" : "are"),
                  "wrong number of arguments");
            for (Expr* a : args) {
                if (a->type && a->type->is_untyped()) settle(*a, tc_.error());
            }
            return set(c, name == "or_error" || name == "map_error" ? tc_.error() : value);
        }
        Expr& a = *args[0];
        if (name == "value_or") {
            coerce(a, value);
            return set(c, value);
        }
        if (name == "expect") {
            const Type* m = types::strip_ref(a.type);
            if (m->is_untyped()) {
                mismatch(a.span, a.type, tc_.static_str(), " (the message)");
                settle(a, tc_.error());
            } else if (!m->is_unknown() && !m->is(Kind::static_str) &&
                       !(m->is(Kind::external) && (m->name == "std::str" || m->name == "std::format_args"))) {
                mismatch(a.span, a.type, tc_.static_str(), " (the message)");
            }
            return set(c, value);
        }
        if (name == "or_error") {
            const Type* e = types::strip_ref(a.type);
            if (e->is_untyped()) {
                no_context(a, "as the new error ('or_error' takes the error type from its argument)");
                return set(c, tc_.error());
            }
            if (e->is_unknown() || e->is(Kind::never) || e->is(Kind::external)) return set(c, tc_.error());
            const Symbol* result = std_item("result");
            return set(c, result ? tc_.instance(result, {value, e}) : tc_.error());
        }
        // map_error(f): a lambda from the error type to the new error type.
        const Type* f = types::strip_ref(a.type);
        if (f->is(Kind::closure)) {
            const Type* old_error = t->symbol->template_args[1];
            if (f->args.size() != 2 || (types::strip_ref(f->args[0]) != old_error && !f->args[0]->is_unknown())) {
                error(DiagCode::E4010, a.span, std::format("'map_error' needs a lambda that takes the error {}", quoted(old_error)),
                      "wrong lambda");
                return set(c, tc_.error());
            }
            const Symbol* result = std_item("result");
            const Type*   e2     = types::strip_ref(f->args.back());
            if (!result || e2->is_unknown() || e2->is(Kind::void_)) return set(c, tc_.error());
            return set(c, tc_.instance(result, {value, e2}));
        }
        if (!f->is_unknown()) {
            error(DiagCode::E4010, a.span, std::format("'map_error' takes a lambda, not {}", quoted(a.type)), "not a lambda");
        }
        return set(c, tc_.error());
    }

    /// `std::some`, `std::none`, `std::ok`, `std::error` written without template arguments: the
    /// alternatives std exports directly, whose arguments come from context (spec 08).
    static bool is_std_export(const PathExpr& pe) {
        const Symbol* s = pe.symbol;
        return s && s->kind == SymbolKind::alternative && pe.path.segments.size() == 2 &&
               pe.path.segments[0].name == "std" && pe.path.segments[1].template_args.empty() && s->parent &&
               s->parent->decl && s->parent->decl->is_template && !s->parent->template_of;
    }

    /// Index of the template parameter that the payload of `alt` is (`some(T)` -> T), or -1.
    static int payload_param(const Symbol* alt) {
        const TypeExpr* p = alt->alternative->payload.get();
        if (!p || p->kind != TypeKind::named) return -1;
        const auto& nt = p->as<NamedType>();
        if (nt.path.segments.size() != 1) return -1;
        const auto& params = alt->parent->decl->template_params;
        for (std::size_t i = 0; i < params.size(); ++i) {
            if (params[i].name == nt.path.segments[0].name) return static_cast<int>(i);
        }
        return -1;
    }

    /// The type of `std::ok(x)` / `std::none`: the instance when the payload fixes every
    /// template argument, otherwise a pending type that context completes later.
    const Type* std_alternative_type(PathExpr& pe, Expr* payload) {
        const Symbol*            alt   = pe.symbol;
        const Symbol*            templ = alt->parent;
        std::vector<const Type*> args(templ->decl->template_params.size(), nullptr);
        if (payload) {
            const Type* at = types::strip_ref(payload->type);
            if (at->is_unknown()) return tc_.error();
            int i = payload_param(alt);
            if (i >= 0 && !at->is_untyped() && !at->is(Kind::never)) args[static_cast<std::size_t>(i)] = at;
        }
        for (const Type* a : args) {
            if (!a) return tc_.pending(templ, args);
        }
        const Type* et = tc_.instance(templ, args);
        pe.symbol      = instance_alternative(pe.symbol, et);
        return et;
    }

    /// A pending `std::ok(x)` / `std::none` meets the type `t` it is used as.
    void settle_pending(Expr& e, const Type* t) {
        const Type* p       = e.type;
        auto&       pe      = e.kind == ExprKind::call ? e.as<CallExpr>().callee->as<PathExpr>() : e.as<PathExpr>();
        Expr*       payload = e.kind == ExprKind::call && e.as<CallExpr>().args.size() == 1
                                  ? e.as<CallExpr>().args[0].get() : nullptr;
        if (t->is_unknown() || t->is(Kind::external)) {
            e.type = pe.type = t;
            if (payload && payload->type && payload->type->is_untyped()) settle(*payload, tc_.error());
            return;
        }
        bool fits = t->is(Kind::user) && t->symbol->template_of == p->symbol &&
                    t->symbol->template_args.size() == p->args.size();
        for (std::size_t i = 0; fits && i < p->args.size(); ++i) {
            fits = !p->args[i] || p->args[i] == t->symbol->template_args[i];
        }
        if (!fits) {
            mismatch(e.span, p, t);
            e.type = pe.type = tc_.error();
            if (payload && payload->type && payload->type->is_untyped()) settle(*payload, tc_.error());
            return;
        }
        pe.symbol = instance_alternative(pe.symbol, t);
        e.type = pe.type = t;
        if (payload && pe.symbol->alternative->payload) {
            coerce(*payload, lower_owned(pe.symbol, *pe.symbol->alternative->payload));
        }
    }

    void no_context_pending(Expr& e, std::string_view where) {
        const Type*  p     = e.type;
        const auto&  tps   = p->symbol->decl->template_params;
        std::string  unknown;
        int          count = 0;
        for (std::size_t i = 0; i < tps.size(); ++i) {
            if (p->args[i]) continue;
            unknown += (unknown.empty() ? "'" : ", '") + tps[i].name + "'";
            ++count;
        }
        const Expr& callee = e.kind == ExprKind::call ? *e.as<CallExpr>().callee : e;
        std::string what   = ast::to_string(callee.as<PathExpr>().path) + (e.kind == ExprKind::call ? "(...)" : "");
        Diagnostic& d = error(DiagCode::E4001, e.span, std::format("'{}' has no type {}", what, where), "type unknown");
        note(d, std::format("the template argument{} {} of 'std::{}' must come from context; write the type, as in "
                            "'std::{}<{}> x = {};'",
                            count == 1 ? "" : "s", unknown, p->symbol->name, p->symbol->name,
                            p->symbol->name == "result" ? "T, E" : "T", what));
        e.type = tc_.error();
    }

    // --- untyped values -------------------------------------------------------------------

    /// Gives an untyped value (literal, `bit(n)`, `default`, or arithmetic on those) the type
    /// `target`, checking that it fits.
    void settle(Expr& e, const Type* target, bool negative = false) {
        const Type* t = types::strip_ref(target);
        if (t->is_untyped()) return;   // still no context: leave it untyped
        if (e.type && e.type->is(Kind::pending) && (e.kind == ExprKind::call || e.kind == ExprKind::path)) {
            settle_pending(e, t);
            return;
        }
        if (t->is_unknown() || t->is(Kind::external)) {
            e.type = t;
            return;
        }
        switch (e.kind) {
        case ExprKind::int_literal: {
            const auto& lit = e.as<LiteralExpr>();
            e.type          = t;
            if (t->is_float()) {
                note(error(DiagCode::E4003, e.span, std::format("integer literal used as {}", quoted(t)),
                           "integer literal"),
                     std::format("write '{}.0'", lit.text));
            } else if (!t->is_integer()) {
                mismatch(e.span, tc_.untyped_int(), t);
            } else if (!types::literal_fits(lit.int_value, negative, t)) {
                note(error(DiagCode::E4002, e.span,
                           std::format("{}{} does not fit in {}", negative ? "-" : "", lit.text, quoted(t)),
                           "out of range"),
                     std::format("{} holds {}", quoted(t), types::integer_range(t)));
            }
            return;
        }
        case ExprKind::float_literal:
            e.type = t;
            if (!t->is_float()) mismatch(e.span, tc_.untyped_float(), t);
            return;
        case ExprKind::unary: {
            auto& u = e.as<UnaryExpr>();
            if (u.op == UnaryOp::negate) {
                if (t->is_integer() && !t->is_signed && u.operand->kind != ExprKind::int_literal) {
                    error(DiagCode::E4015, e.span, std::format("cannot negate a value of unsigned type {}", quoted(t)),
                          "unsigned");
                }
                settle(*u.operand, target, !negative);
            } else {
                settle(*u.operand, target, negative);
            }
            e.type = t;
            return;
        }
        case ExprKind::binary: {
            auto& b = e.as<BinaryExpr>();
            if (b.lhs->type && b.lhs->type->is_untyped()) settle(*b.lhs, target);
            if (b.rhs->type && b.rhs->type->is_untyped()) settle(*b.rhs, is_shift(b.op) ? target : target);
            check_operator_type(b.op, t, e.span);
            e.type = t;
            return;
        }
        case ExprKind::bit: {
            auto& b = e.as<BitExpr>();
            e.type  = t;
            if (!t->is_integer()) {
                error(DiagCode::E4013, e.span, std::format("bit(n) produces an integer, but {} is expected", quoted(t)),
                      "not an integer type");
                return;
            }
            check_bit_index(*b.operand, t);
            return;
        }
        case ExprKind::default_:
            e.type = t;
            check_has_default(t, e.span);
            return;
        case ExprKind::block: {
            auto& b = e.as<BlockExpr>();
            if (b.tail) settle(*b.tail, target, negative);
            e.type = t;
            return;
        }
        case ExprKind::if_: {
            auto& i = e.as<IfExpr>();
            coerce(*i.then_block, target);
            if (i.else_branch) coerce(*i.else_branch, target);
            e.type = t;
            return;
        }
        case ExprKind::match: {
            for (MatchArm& arm : e.as<MatchExpr>().arms) coerce(*arm.body, target);
            e.type = t;
            return;
        }
        default: e.type = t; return;
        }
    }

    /// The expression's value is used where `target` is expected: settle untyped values,
    /// otherwise check assignability.
    void coerce(Expr& e, const Type* target) {
        if (!target || !e.type) return;
        if (e.type->is_untyped()) {
            settle(e, target);
            return;
        }
        // Branches are checked one by one so the error points at the wrong branch.
        if (e.type->is(Kind::error)) {
            if (e.kind == ExprKind::if_ || e.kind == ExprKind::match || e.kind == ExprKind::block) {
                settle(e, target);
            }
            return;
        }
        if (e.kind == ExprKind::block) {
            auto& b = e.as<BlockExpr>();
            if (b.tail) {
                coerce(*b.tail, target);
                return;
            }
        }
        const Type* t = types::strip_ref(target);
        if (some_reference(e, t)) return;
        if (e.type->is(Kind::void_) && !t->is(Kind::void_) && !t->is_unknown()) {
            Span end = Span{e.span.file, e.span.end > 0 ? e.span.end - 1 : 0, e.span.end};
            error(DiagCode::E4010, e.kind == ExprKind::block ? end : e.span,
                  std::format("expected {}, found no value", quoted(target)), "this block has no value")
                ;
            return;
        }
        if (assign_cost(e.type, target) < 0) {
            mismatch(e.span, e.type, target);
            return;
        }
        if (e.type->is(Kind::static_str) && is_std_string(t) && !target->is(Kind::reference)) {
            note(error(DiagCode::W4002, e.span, "implicit allocation of a std::string from a string literal",
                       "allocates"),
                 "write 'std::string::from(\"...\")' to make the allocation visible");
            if (t->is(Kind::user)) e.conversion = Expr::Conversion::literal_to_string;
        }
        if (lambda_fits(types::strip_ref(e.type), t) && !target->is(Kind::reference)) {
            e.conversion   = Expr::Conversion::to_callable;
            e.converted_to = t;
            const ast::LambdaExpr* l = tc_.lambda_of(types::strip_ref(e.type));
            for (std::size_t ci = 0; l && ci < l->captures.size(); ++ci) {
                const ast::Capture& c = l->captures[ci];
                if (t->is(Kind::function) && (c.kind == ast::CaptureKind::ref || c.kind == ast::CaptureKind::mut_ref)) {
                    note(error(DiagCode::E5015, c.span, "a std::function cannot hold a lambda with reference captures",
                               "a reference capture"),
                         "a std::function is stored, and a reference could outlive what it refers to (spec 18); capture by value or 'move'");
                    break;
                }
            }
        }
        if (smart_upcast(types::strip_ref(e.type), t) && !target->is(Kind::reference)) {
            e.conversion   = Expr::Conversion::pointer_upcast;
            e.converted_to = t;
        }
        if (is_std_string(types::strip_ref(e.type)) && t->is(Kind::external) && t->name == "std::str" &&
            !target->is(Kind::reference)) {
            e.conversion = Expr::Conversion::string_to_str;
        }
    }

    /// `std::some(x)` where a `std::option<T&>` is expected: the option refers to the place x
    /// (spec 11, 20: `a.get(i)`). True when `e` was such a call and is now typed `t`.
    bool some_reference(Expr& e, const Type* t) {
        if (e.kind != ExprKind::call || !is_std_instance(t, "option") || t->symbol->template_args.size() != 1) return false;
        const Type* want = t->symbol->template_args[0];
        if (!want->is(Kind::reference)) return false;
        auto& c = e.as<CallExpr>();
        if (c.callee->kind != ExprKind::path || c.args.size() != 1) return false;
        auto& pe = c.callee->as<PathExpr>();
        if (!pe.symbol || pe.symbol->kind != SymbolKind::alternative || pe.symbol->name != "some") return false;
        const Type* have = types::strip_ref(e.type);
        if (!is_std_instance(have, "option") || have->symbol->template_args.size() != 1 ||
            have->symbol->template_args[0] != types::strip_ref(want)) {
            return false;
        }
        Expr& arg = *c.args[0];
        Place p   = place_of(arg);
        if (!p.is_place || p.is_temporary) {
            error(DiagCode::E4010, arg.span, std::format("{} needs a place to refer to", quoted(t)), "a temporary");
        } else if (want->is_mut) {
            require_mutable(p, arg, "refer mutably to");
        }
        auto it = t->symbol->members.find("some");
        if (it != t->symbol->members.end()) pe.symbol = it->second.front();
        e.type = t;
        return true;
    }

    /// An untyped value in a place without any context (`auto x = 5;`, `if (3 < 4)`).
    void no_context(Expr& e, std::string_view where) {
        if (e.type && e.type->is(Kind::pending)) {
            no_context_pending(e, where);
            return;
        }
        std::string what = e.type == tc_.untyped_default() ? "'default'"
                         : e.kind == ExprKind::bit          ? "'bit(n)'"
                         : e.type == tc_.untyped_float()    ? "this float literal"
                                                            : "this integer literal";
        Diagnostic& d = error(DiagCode::E4001, e.span, std::format("{} has no type {}", what, where), "type unknown");
        if (e.kind == ExprKind::int_literal) {
            note(d, std::format("give it a suffix like '{}u32', or a typed declaration like 'u32 x = {}'",
                                e.as<LiteralExpr>().text, e.as<LiteralExpr>().text));
        } else if (e.kind == ExprKind::bit) {
            note(d, "write e.g. 'u32 x = bit(4);' or 'bit<u32>(4)'");
        } else if (e.kind == ExprKind::float_literal) {
            note(d, std::format("give it a suffix like '{}f64'", e.as<LiteralExpr>().text));
        }
        e.type = tc_.error();
    }

    /// `{expr:spec}` in an f-string: the value needs a type, and the spec must fit it (spec 19).
    void check_format_hole(FormatPart& part) {
        Expr&       e = *part.expr;
        const Type* t = types::strip_ref(infer(e));
        if (t->is_untyped()) {
            no_context(e, "in a format string");
            return;
        }
        const FormatSpec& s = part.spec;
        if (!s.present || t->is_unknown() || t->is(Kind::external) || t->is(Kind::user)) return;
        auto bad = [&](std::string_view what, std::string_view fix) {
            note(error(DiagCode::E4170, s.span, std::format("{} cannot be used with {}", what, quoted(t)),
                       "does not fit the value"),
                 std::string(fix));
        };
        if (s.radix && !t->is_integer() && !t->is(Kind::char_)) {
            bad(std::format("'{}' (hex/binary/octal)", s.radix), "only integers can be written in another base");
        } else if (s.precision >= 0 && !t->is_float()) {
            bad("a precision", "precision applies to floats only");
        } else if (s.plus && !t->is_numeric()) {
            bad("'+'", "'+' applies to numbers only");
        }
    }

    // --- declarations ---------------------------------------------------------------------

    void check_file(ast::File& file) {
        for (DeclPtr& d : file.decls) check_decl(*d, nullptr);
    }

    void check_decl(Decl& d, Symbol* self) {
        switch (d.kind) {
        case DeclKind::import: return;
        case DeclKind::check: {
            Check& c = d.as<CheckDecl>().check;
            check_check(c, self ? Where::type_body : Where::file_scope, d.span);
            if (c.is_compile_time && c.compile_active && !c.is_run_time) {
                run_check_now(c, d.span, self);
            }
            return;
        }
        case DeclKind::function: check_function(d.as<FunctionDecl>(), self); return;
        case DeclKind::variable:
        case DeclKind::field: {
            auto& v = d.as<VariableDecl>();
            if (d.kind == DeclKind::variable && v.storage == Storage::static_ && !v.is_mut && !fn_.generic) {
                // Every thread can see a static, so its type must be shareable (spec 27).
                const Type* t = lower(*v.type);
                std::string why;
                std::optional<Span> where;
                if (!t->is_unknown() && !thread_safe(t, true, &why, &where)) {
                    Diagnostic& e = error(DiagCode::E4231, v.name_span,
                                          std::format("'static {}' is visible to every thread, but {} can't be shared between threads",
                                                      v.name, quoted(t)),
                                          "not std::shareable");
                    note(e, why, where);
                    note(e, "use 'thread_local' for one copy per thread, a synchronized type (std::atomic<T>, std::mutex<T>), or "
                            "'static mut' with 'unsafe' at every use");
                }
            }
            if (!v.init) {
                if (d.kind == DeclKind::field && compile_time_only(lower(*v.type))) {
                    run_time_use(v.type->span, lower(*v.type), std::format("'{}' is a field", v.name),
                                 "values of a struct or class exist at run time");
                }
                return;
            }
            if (!begin_check(&d)) return;
            std::size_t   errors = diags_.error_count();
            FunctionState saved = fn_;
            fn_                 = FunctionState{};
            fn_.is_compile_time = v.storage != Storage::local;   // static/compile_time initializers run at compile time
            infer(*v.init);
            const Type* declared = lower(*v.type);
            coerce(*v.init, declared);
            fn_ = saved;
            if (d.kind == DeclKind::variable && v.storage == Storage::compile_time) {
                if (Symbol* g = symbol_for(d); g && diags_.error_count() == errors) {
                    g->ct_value = evaluate(*v.init, std::format("the value of compile_time constant '{}' is computed at compile time", v.name));
                }
            } else if (d.kind == DeclKind::variable && v.storage == Storage::thread_local_ && diags_.error_count() == errors) {
                // Each thread's copy starts with this value: it is computed at compile time, so a
                // thread_local needs no "initialized yet?" check (spec 27).
                if (auto c = evaluate(*v.init, std::format("the initial value of thread_local '{}' is computed at compile time", v.name))) {
                    v.init->constant = c;
                }
            } else if (compile_time_only(declared)) {
                run_time_use(v.type->span, declared,
                             std::format("'{}' is {}", v.name, d.kind == DeclKind::field ? "a field" : "a static variable"),
                             "declare it 'compile_time'");
            }
            end_check(&d);
            return;
        }
        case DeclKind::struct_:
        case DeclKind::class_:
        case DeclKind::interface: {
            Symbol* t = symbol_for(d);
            check_promises(d.as<TypeDecl>(), d.kind);
            for (DeclPtr& m : d.as<TypeDecl>().members) check_decl(*m, t);
            return;
        }
        case DeclKind::enum_: {
            auto&       e   = d.as<EnumDecl>();
            const Type* tag = lower(*e.tag_type);
            if (!tag->is_integer() && !tag->is_unknown()) {
                error(DiagCode::E4130, e.tag_type->span,
                      std::format("the tag type of enum '{}' must be an integer type, not {}", e.name, quoted(tag)),
                      "not an integer");
                return;
            }
            for (EnumAlternative& alt : e.alternatives) {
                if (alt.value) {
                    infer(*alt.value);
                    coerce(*alt.value, tag);
                }
            }
            return;
        }
        }
    }

    Symbol* symbol_for(const Decl& d) {
        auto it = module_->items.find(d.name);
        if (it == module_->items.end()) return nullptr;
        for (Symbol* s : it->second) {
            if (s->decl == &d || s->definition == &d) return s;
        }
        return it->second.front()->is_type() ? it->second.front() : nullptr;
    }

    void check_function(FunctionDecl& fn, Symbol* self) {
        if (!fn.body) return;
        if (!begin_check(&fn)) return;   // checked already (on demand, for compile-time code)
        FunctionState saved   = fn_;
        int           unsafe0 = unsafe_depth_;
        fn_                   = FunctionState{};
        fn_.in_function       = true;
        fn_.result            = fn.result ? lower(*fn.result) : tc_.void_();
        if (fn.result && fn.result->kind == TypeKind::deduce) {
            fn_.result   = nullptr;
            fn_.deducing = true;
        }
        fn_.self            = self;
        fn_.has_this        = self && !fn.is_static;
        fn_.this_mut        = fn.is_mut || fn.is_destructor || fn.is_move;
        fn_.is_compile_time = fn.is_compile_time;
        fn_.decl            = &fn;
        fn_.name            = fn.name;
        fn_.generic         = (fn.is_template && subst_.empty()) ||
                      (self && self->decl && self->decl->is_template && !self->template_of);
        if (fn.is_unsafe) ++unsafe_depth_;
        for (Param& p : fn.params) {
            if (p.symbol) set_local(p.symbol, lower(*p.type));
        }
        // The entry: the checks at the very start of the body (spec 23), also those inside an
        // `if compile_time` that holds only checks (the library's index checks follow the mode).
        for (StmtPtr& st : fn.body->statements) {
            if (st->kind == StmtKind::check) {
                st->as<CheckStmt>().check.is_entry = true;
                continue;
            }
            IfExpr* gate = entry_gate(*st);
            if (!gate) break;
            for (StmtPtr& inner : gate->then_block->statements) inner->as<CheckStmt>().check.is_entry = true;
        }
        infer_block(*fn.body);

        const Type* result = fn_.result;
        if (result && !result->is(Kind::void_) && !result->is_unknown()) {
            if (fn.body->tail && !(fn.body->tail->type && fn.body->tail->type->is(Kind::void_)) &&
                !diverges(*fn.body->tail)) {
                Diagnostic& e = error(DiagCode::E4043, fn.body->tail->span,
                                      std::format("'{}' returns its value without 'return'", fn.name), "missing 'return'");
                note(e, "write 'return ...;'");
            } else if (!diverges_block(*fn.body)) {
                Span end = Span{fn.body->span.file, fn.body->span.end - 1, fn.body->span.end};
                Diagnostic& e = error(DiagCode::E4042, end,
                                      std::format("'{}' may reach its end without returning a {}", fn.name, quoted(result)),
                                      "can end here without 'return'");
                note(e, "declared here", fn.name_span);
            }
        }
        if (fn_.deducing) fn.deduced_result = fn_.deduced ? fn_.deduced : tc_.void_();
        unsafe_depth_ = unsafe0;
        fn_           = saved;
        end_check(&fn);
        if (!fn.is_compile_time) {
            for (Param& p : fn.params) {
                const Type* pt = p.symbol ? p.symbol->type : nullptr;
                if (pt && compile_time_only(pt)) {
                    run_time_use(p.type->span, pt, std::format("'{}' is a run-time function", fn.name),
                                 "make the function 'compile_time', or pass the type as a template parameter");
                }
            }
            if (fn.result && compile_time_only(lower(*fn.result))) {
                run_time_use(fn.result->span, lower(*fn.result), std::format("'{}' is a run-time function", fn.name),
                             "make the function 'compile_time'");
            }
        }
    }

    // --- compile time (spec 15, 26) -------------------------------------------------------

    /// Types whose values exist only at compile time: `type`, std::meta data, lists of them.
    bool compile_time_only(const Type* t) const {
        t = types::strip_ref(t);
        if (!t) return false;
        if (t->is_compile_time_only()) return true;
        if (t->is(Kind::external)) {
            if (const auto* args = tc_.external_args(t)) {
                for (const Type* a : *args) {
                    if (a && compile_time_only(a)) return true;
                }
            }
        }
        return false;
    }

    void run_time_use(Span span, const Type* t, std::string where, std::string fix) {
        Diagnostic& d = error(DiagCode::E4202, span, std::format("{} exists only at compile time, but {}", quoted(t), where),
                              "compile-time only");
        note(d, std::move(fix));
    }

    ct::Evaluator& evaluator() {
        if (!evaluator_) {
            ct::Hooks h;
            h.check_function = [this](const Symbol* f) { return ensure_function(f); };
            h.constant_value = [this](const Symbol* c) -> std::shared_ptr<const ct::Value> {
                if (c->ct_value) return c->ct_value;
                return c->kind == SymbolKind::global ? ensure_constant(c) : nullptr;
            };
            h.lower      = [this](const TypeExpr& t) { return lower(t); };
            h.depends_on_template = [this](const Symbol* s) { return unknown_values_.count(s) > 0; };
            h.substitute = [this](const Symbol* param) -> const Type* {
                auto it = subst_.find(param);
                return it == subst_.end() ? nullptr : it->second;
            };
            h.visible_fields = [this](const Type* t) {
                std::vector<const Symbol*> out;
                for (const Symbol* f : t->symbol->member_order) {
                    if (f->kind == SymbolKind::field && !f->is_static && member_visible(f)) out.push_back(f);
                }
                return out;
            };
            h.is_view = [this](const Type* t) { return view_type(t); };
            h.implements = [this](const Type* t, const Type* i) {
                std::string why;
                return satisfies(t, i, why);
            };
            evaluator_ = std::make_unique<ct::Evaluator>(program_, diags_, std::move(h));
        }
        return *evaluator_;
    }

    /// Evaluates a checked expression at compile time; null when it cannot be (reported, or it
    /// depends on a template parameter, see `evaluator().unknown()`).
    /// A call of a compile_time-only function in run-time code runs at compile time: its
    /// result is a constant (spec 15). So does a settings method called on the build mode
    /// (`std::build::current_mode().optimization()`, spec 24).
    void fold_compile_time_call(CallExpr& c) {
        if (c.constant || fn_.is_compile_time || fn_.generic || !c.type || c.type->is_unknown()) return;
        const Symbol* s = nullptr;
        bool on_constant = false;
        if (c.callee->kind == ExprKind::path) s = c.callee->as<PathExpr>().symbol;
        if (c.callee->kind == ExprKind::member) {
            s           = c.callee->as<MemberExpr>().symbol;
            on_constant = c.callee->as<MemberExpr>().object->constant != nullptr;
        }
        const FunctionDecl* f = s ? s->function() : nullptr;
        if (!f) return;
        bool mode_method = on_constant && s->parent && s->parent->module &&
                           (s->parent->module->name == "std::build::mode");
        if (!mode_method) {
            for (const Symbol* k = s->parent; k && k->kind == SymbolKind::class_ && !mode_method; k = k->base_class()) {
                mode_method = on_constant && k->module && k->module->name == "std::build::mode";
            }
        }
        if (!(f->is_compile_time && !f->is_run_time) && !mode_method) return;
        for (const ExprPtr& a : c.args) {
            if (!a->type || a->type->is_unknown()) return;
        }
        c.constant = evaluate(c, "a compile_time function called from run-time code runs at compile time");
    }

    std::shared_ptr<const ct::Value> evaluate(Expr& e, std::string_view why) {
        if (!e.type || e.type->is_unknown()) return nullptr;
        return evaluator().evaluate(e, why);
    }

    /// The compile-time value a template value argument stands for.
    ct::Value value_of_arg(const Type* arg) const {
        const Type* of = arg->inner;
        if (of->is(Kind::bool_)) return ct::Value::of_bool(tc_.value_bits(arg) != 0, of);
        return ct::Value::of_integer(tc_.value_bits(arg), of);
    }

    /// Evaluates a template value argument for the parameter `index` of `templ` (spec 14).
    const Type* evaluate_value_arg(const Symbol* templ, std::size_t index, Expr& value) {
        if (!templ->decl || index >= templ->decl->template_params.size() || templ->decl->template_params[index].is_type ||
            !templ->decl->template_params[index].value_type) {
            error(DiagCode::E4180, value.span, std::format("'{}' takes a type here, not a value", templ->name), "a value");
            return tc_.error();
        }
        const Type* want = lower(*templ->decl->template_params[index].value_type);
        std::size_t errors = diags_.error_count();
        infer(value);
        coerce(value, want);
        if (diags_.error_count() != errors) return tc_.error();
        if (!want->is_integer() && !want->is(Kind::bool_)) {
            error(DiagCode::E4180, value.span, std::format("template value parameters of type {} are not supported yet", quoted(want)),
                  "use an integer or bool");
            return tc_.error();
        }
        std::shared_ptr<const ct::Value> v =
            evaluate(value, std::format("a template argument of '{}' must be known at compile time", templ->name));
        if (!v) return tc_.error();
        return tc_.value_arg(v->kind == ct::Value::Kind::boolean ? u128{v->boolean} : v->integer, want);
    }

    /// The type a type value denotes, when the expression is one (`u32`, `point`, `T`).
    const Type* denoted_type(const Expr& e) const {
        if (!e.constant || e.constant->kind != ct::Value::Kind::type) return nullptr;
        return e.constant->type_value;
    }

    // --- checks (spec 23) -----------------------------------------------------------------

    enum class Where { file_scope, type_body, entry, body };

    /// Whether a check's mode list includes the mode being built (spec 23, 24). Built-in modes
    /// only, until build.rifts can define more.
    bool mode_active(const std::vector<std::string>& modes, Span span) {
        if (modes.empty()) return true;
        const std::string& m      = program_.build_mode;
        bool               active = false;
        constexpr std::string_view prefix = "std::build::mode::";
        for (const std::string& path : modes) {
            if (path.starts_with("build::")) {   // a mode class of build.rifts (spec 24, open question 31)
                const Symbol* cls = nullptr;
                if (auto bm = program_.modules.find("build"); bm != program_.modules.end()) {
                    if (auto it = bm->second->items.find(path.substr(7)); it != bm->second->items.end()) cls = it->second.front();
                }
                if (!cls || cls->kind != SymbolKind::class_) {
                    note(error(DiagCode::E4212, span, std::format("unknown build mode '{}'", path), "unknown mode"),
                         "build.rifts does not define this mode class");
                    continue;
                }
                for (const Symbol* c = program_.build_mode_class; c; c = c->base_class()) active = active || c == cls;
                continue;
            }
            std::string name = path.starts_with(prefix) ? path.substr(prefix.size()) : std::string();
            if (name != "debug" && name != "release" && name != "release_with_debug_info") {
                Diagnostic& d = error(DiagCode::E4212, span, std::format("unknown build mode '{}'", path), "unknown mode");
                if (path == "debug" || path == "release" || path == "release_with_debug_info") {
                    note(d, std::format("write 'std::build::mode::{}': std names are written in full", path));
                } else {
                    note(d, "the built-in modes are std::build::mode::debug, ::release and ::release_with_debug_info "
                            "(user modes need build.rifts)");
                }
                continue;
            }
            if (program_.build_mode_class) {   // the mode is (or derives from) that class
                for (const Symbol* c = program_.build_mode_class; c; c = c->base_class()) active = active || c->name == name;
            } else if (name == m || (name == "release" && m == "release_with_debug_info")) {
                active = true;
            }
        }
        return active;
    }

    void check_check(Check& c, Where where, Span span) {
        if ((where == Where::file_scope || where == Where::type_body) && c.is_run_time) {
            note(error(DiagCode::E4211, span,
                       std::format("a run_time check needs a function body; {} only has compile_time checks",
                                   where == Where::file_scope ? "file scope" : "a type body"),
                       "not allowed here"),
                 "write 'compile_time check', or move the check into a function");
        }
        if (where == Where::body && c.is_compile_time) {
            note(error(DiagCode::E4211, span, "a compile_time check must be at the entry of a function, at file scope or in a type body",
                       "not at the entry"),
                 "the entry is the run of checks at the very start of the body; write 'run_time check' for an assert here");
        }
        if (where == Where::entry && c.is_compile_time && fn_.decl &&
            (fn_.decl->is_virtual || fn_.decl->is_override || fn_.decl->is_abstract)) {
            note(error(DiagCode::E4211, span, "a virtual, override or abstract method may only have run_time entry checks",
                       "compile_time check"),
                 "the method a dynamic call reaches is not known at compile time (spec 23); write 'run_time check'");
        }
        c.compile_active = c.is_compile_time && mode_active(c.compile_modes, c.compile_modes_span);
        c.run_active     = c.is_run_time && mode_active(c.run_modes, c.run_modes_span);
        // The mode's run_time_checks() (spec 24): `none` drops the program's run_time checks,
        // `gated_only` keeps those with a mode list. The library's own checks follow bounds().
        const std::string& rtc = program_.mode_settings["run_time_checks"];
        bool in_std = module_ && (module_->name == "std" || module_->name.starts_with("std::"));
        if (!in_std && (rtc == "none" || (rtc == "gated_only" && c.run_modes.empty()))) c.run_active = false;
        bool saved       = fn_.is_compile_time;
        // Compile-time-only code may only call compile_time functions (spec 15).
        if (c.is_compile_time && !c.is_run_time) fn_.is_compile_time = true;
        if (c.block) {
            infer_block(*c.block);
        } else {
            const Type* t = types::strip_ref(infer(*c.condition));
            if (t->is_untyped()) settle(*c.condition, tc_.bool_());
            else if (!t->is(Kind::bool_) && !t->is_unknown()) mismatch(c.condition->span, c.condition->type, tc_.bool_(), " (the condition)");
            const Type* m = types::strip_ref(infer(*c.message));
            if (m->is_untyped()) {
                mismatch(c.message->span, c.message->type, tc_.static_str(), " (the message)");
                settle(*c.message, tc_.error());
            } else if (!m->is_unknown() && !m->is(Kind::static_str) &&
                       !(m->is(Kind::external) && (m->name == "std::str" || m->name == "std::format_args"))) {
                mismatch(c.message->span, c.message->type, tc_.static_str(), " (the message)");
            }
        }
        fn_.is_compile_time = saved;
    }

    /// A file-scope or type-body compile_time check: evaluated once (per instance for templates).
    void run_check_now(Check& c, Span span, Symbol* self) {
        if (self && self->decl && self->decl->is_template && !self->template_of) return;   // the generic template
        if (c.block) {
            evaluator().evaluate(*c.block, "");
            return;
        }
        std::shared_ptr<const ct::Value> v = evaluate(*c.condition, "the condition of a compile_time check must be known at compile time");
        if (!v || v->kind != ct::Value::Kind::boolean || v->boolean) return;
        std::shared_ptr<const ct::Value> m = evaluate(*c.message, "");
        bool        at_use = self && self->template_of && instance_site_;   // reported where the instance is written (spec 23)
        Diagnostic& d = error(DiagCode::E4210, at_use ? *instance_site_ : c.condition->span,
                              m && m->kind == ct::Value::Kind::string ? m->string : "check failed", "check failed");
        if (self && self->template_of) note(d, std::format("in the instance {}", quoted(tc_.user(self))));
        if (at_use) note(d, "the check is here", c.condition->span);
        (void)span;
    }

    /// The entry checks of `callee` for one call (spec 23): compile_time checks are evaluated with
    /// the call's arguments, which must then be compile-time values; `compile_time run_time`
    /// checks are evaluated when the arguments happen to be known.
    void run_entry_checks(const Symbol* callee, CallExpr& c, const std::vector<Expr*>& args) {
        if (fn_.is_compile_time) return;   // compile-time code: the evaluator runs them when it calls
        const Decl* d = callee->definition ? callee->definition : callee->decl;
        if (!d || d->kind != DeclKind::function) return;
        const auto& f = d->as<FunctionDecl>();
        if (!f.body || f.body->statements.empty() || f.body->statements.front()->kind != StmtKind::check) return;
        if (!ensure_function(callee)) return;
        for (const StmtPtr& st : f.body->statements) {
            if (st->kind != StmtKind::check) break;
            const Check& k = st->as<CheckStmt>().check;
            if (!k.is_compile_time || !k.compile_active) continue;
            bool quiet = k.is_run_time;
            LineCol at = sources_line(st->span);
            ct::Evaluator::Context ctx;
            ctx.function  = callee;
            ctx.quiet     = quiet;
            ctx.site      = c.span;
            ctx.parameter = [&](const Symbol* p) -> std::shared_ptr<const ct::Value> {
                for (std::size_t i = 0; i < f.params.size() && i < args.size(); ++i) {
                    if (f.params[i].symbol != p) continue;
                    Expr& arg = *args[i];
                    if (quiet) {
                        ct::Evaluator::Context q;
                        q.quiet = true;
                        return evaluator().evaluate_in(arg, "", q);
                    }
                    return evaluator().evaluate(arg, std::format("'{}' must be known at compile time: the compile_time check "
                                                                 "at line {} of '{}' reads it",
                                                                 p->name, at.line, callee->name));
                }
                return nullptr;
            };
            ctx.parameter_known = [&](const Symbol* p) {
                for (std::size_t i = 0; i < f.params.size() && i < args.size(); ++i) {
                    if (f.params[i].symbol != p) continue;
                    ct::Evaluator::Context q;
                    q.quiet = true;
                    return evaluator().evaluate_in(*args[i], "", q) != nullptr;
                }
                return false;
            };
            ctx.parameter_site = [&](const Symbol* p) -> std::optional<Span> {
                for (std::size_t i = 0; i < f.params.size() && i < args.size(); ++i) {
                    if (f.params[i].symbol == p) return args[i]->span;
                }
                return std::nullopt;
            };
            if (k.block) {
                evaluator().evaluate_in(*k.block, "", ctx);
                continue;
            }
            std::shared_ptr<const ct::Value> v = evaluator().evaluate_in(*k.condition, "", ctx);
            if (!v || v->kind != ct::Value::Kind::boolean || v->boolean) continue;
            ctx.quiet = true;   // the arguments are known now
            std::shared_ptr<const ct::Value> m = evaluator().evaluate_in(*k.message, "", ctx);
            Diagnostic& e = error(DiagCode::E4210, c.span, m && m->kind == ct::Value::Kind::string ? m->string : "check failed",
                                  "this call fails the check");
            note(e, std::format("the check of '{}' is here", callee->name), k.condition->span);
        }
    }

    LineCol sources_line(Span span) const;

    void check_main(Module& m) {
        if (!m.source) return;
        for (DeclPtr& d : m.source->ast.decls) {
            if (d->kind != DeclKind::function || d->name != "main") continue;
            auto&       fn = d->as<FunctionDecl>();
            const Type* r  = fn.result ? lower(*fn.result) : tc_.void_();
            bool ok_result = r->is(Kind::void_) || r->is_integer() || r->is_unknown() ||
                             (r->is(Kind::external) && r->name == "std::exit_code") ||
                             (r->is(Kind::user) && r->symbol == std_item("exit_code"));
            if (!ok_result) {
                note(error(DiagCode::E4110, fn.result->span, std::format("main cannot return {}", quoted(r)),
                           "invalid return type"),
                     "main returns 'void', 'std::exit_code' or an integer type");
            }
            bool ok_params = fn.params.empty();
            if (fn.params.size() == 1) {
                const Type* p = lower(*fn.params[0].type);
                ok_params     = p->is_unknown() || (p->is(Kind::external) && p->name == "std::span<std::str>") ||
                            (p->is(Kind::user) && p->symbol->template_of == std_item("span") && p->symbol->template_args.size() == 1 &&
                             p->symbol->template_args[0]->is(Kind::external) && p->symbol->template_args[0]->name == "std::str");
            }
            if (!ok_params) {
                note(error(DiagCode::E4111, fn.name_span, "invalid parameters for main", "parameters"),
                     "main takes no parameters or 'std::span<std::str> args'");
            }
        }
    }

    // --- statements -----------------------------------------------------------------------

    const Type* infer_block(BlockExpr& b) {
        if (b.is_unsafe) ++unsafe_depth_;
        for (StmtPtr& s : b.statements) check_stmt(*s);
        const Type* t;
        if (b.tail) {
            t = infer(*b.tail);
        } else {
            t = diverges_block(b) ? tc_.never() : tc_.void_();
        }
        if (b.is_unsafe) --unsafe_depth_;
        b.type = t;
        return t;
    }

    void check_stmt(Stmt& s) {
        switch (s.kind) {
        case StmtKind::variable: {
            auto& v = s.as<VariableStmt>();
            if (v.symbol && v.init && v.type->kind == TypeKind::reference && reaches_thread_local(*v.init)) {
                thread_local_refs_.insert(v.symbol);   // can't go to another thread (spec 27)
            }
            bool  is_auto = v.type->kind == TypeKind::auto_;
            const Type* declared = is_auto ? nullptr : lower(*v.type);
            std::size_t errors   = diags_.error_count();
            if (v.init) {
                bool saved_ct = fn_.is_compile_time;
                if (v.storage == Storage::compile_time) fn_.is_compile_time = true;
                const Type* t = infer(*v.init);
                fn_.is_compile_time = saved_ct;
                if (is_auto) {
                    if (branch_conflicts_.count(v.init.get())) {
                        Diagnostic& d = error(DiagCode::E4050, v.init->span,
                                              "the branches of this value have different types", "different types");
                        note(d, "'auto' takes the type of the value; write the type to say which one is meant");
                    }
                    if (t->is_untyped()) {
                        no_context(*v.init, "('auto' needs a value whose type is known on its own)");
                        t = tc_.error();
                    }
                    declared = t->is(Kind::never) ? tc_.error() : t;
                } else {
                    coerce(*v.init, declared);
                }
            }
            if (v.symbol) set_local(v.symbol, declared ? declared : tc_.error());
            if (v.storage == Storage::compile_time && v.init && v.symbol && diags_.error_count() == errors) {
                v.symbol->ct_value =
                    evaluate(*v.init, std::format("the value of compile_time constant '{}' is computed at compile time", v.name));
                if (!v.symbol->ct_value && evaluator().unknown()) unknown_values_.insert(v.symbol);
            } else if (declared && compile_time_only(declared) && !fn_.is_compile_time) {
                run_time_use(v.type->span, declared, std::format("'{}' is a run-time variable", v.name),
                             std::format("declare it 'compile_time {} {} = ...;'", types::to_string(declared), v.name));
            }
            return;
        }
        case StmtKind::assign: check_assign(s.as<AssignStmt>()); return;
        case StmtKind::check: {
            Check& c = s.as<CheckStmt>().check;
            check_check(c, c.is_entry ? Where::entry : Where::body, s.span);
            return;
        }
        case StmtKind::discard: {
            Expr& v = *s.as<DiscardStmt>().value;
            if (infer(v)->is(Kind::pending)) no_context(v, "here ('_ =' needs a value whose type is known)");
            return;
        }
        case StmtKind::expression: {
            Expr&       e = *s.as<ExprStmt>().expr;
            const Type* t = infer(e);
            if (t->is(Kind::pending)) {
                no_context(e, "here");
                return;
            }
            if ((e.kind == ExprKind::call) && (is_std_instance(t, "result") ||
                                                (t->is(Kind::external) && t->external_base() == "std::result"))) {
                Diagnostic& d = error(DiagCode::E4150, e.span, std::format("the {} returned here is ignored", quoted(t)),
                                      "result not used");
                note(d, "handle it with 'match' or '?', or discard it explicitly with '_ = ...;'");
            }
            return;
        }
        }
    }

    void check_assign(AssignStmt& a) {
        const Type* target = infer(*a.target);
        if (a.target->kind == ExprKind::index && a.target->as<IndexExpr>().op_symbol) {
            // Assigning through `v[i]` uses `mut T& operator[](usize) mut` (spec 16).
            auto&       ix  = a.target->as<IndexExpr>();
            const Type* obj = types::strip_ref(ix.object->type);
            if (Symbol* m = index_operator(obj, true); m && m->function()->is_mut) {
                ix.op_symbol    = m;
                use_method(m, ix.span);
                target          = m->function()->result ? lower_owned(m, *m->function()->result) : target;
                a.target->type  = target;
                require_mutable(place_of(*ix.object), *ix.object, "change an element of");
            }
        }
        const Type* value  = infer(*a.value);
        Place       p      = place_of(*a.target);
        require_mutable(p, *a.target, "assign to");
        if (a.op == TokenKind::eq) {
            coerce(*a.value, target);
            return;
        }
        TokenKind   op = compound_base(a.op);
        const Type* t  = types::strip_ref(target);
        if (t->is_unknown() || t->is(Kind::external)) {
            if (value->is_untyped()) settle(*a.value, t);
            return;
        }
        if (is_shift(op)) {
            check_operator_type(op, t, a.span);
            if (value->is_untyped()) settle(*a.value, t);
            check_shift_count(*a.value, t);
            return;
        }
        if (t->is(Kind::user)) {   // `operator+=`, else `a = a + b` with `operator+` (spec 16)
            std::string compound(token_kind_spelling(a.op));
            if (find_operator(compound, t, types::strip_ref(value), &a.op_symbol) ||
                find_operator(token_kind_spelling(op), t, types::strip_ref(value), &a.op_symbol)) {
                settle_operands(a.op_symbol, nullptr, *a.value);
                return;
            }
        }
        check_operator_type(op, t, a.span);
        coerce(*a.value, t);
    }

    // --- places and mutability ------------------------------------------------------------

    Place place_of(const Expr& e) {
        Place p;
        switch (e.kind) {
        case ExprKind::call: {   // `f.get(mut value)`: a reference to a field (spec 26)
            const Type* t = e.type;
            if (!t || t->is_unknown()) {
                p.is_place = p.is_mutable = true;   // unknown (the generic check of a template): don't complain
            } else if (t->is(Kind::reference)) {
                p.is_place   = true;
                p.is_mutable = t->is_mut;
                p.name       = "the field";
            }
            return p;
        }
        case ExprKind::path: {
            const Symbol* s = e.as<PathExpr>().symbol;
            if (!s) {
                p.is_place = p.is_mutable = true;   // unknown: don't complain
                return p;
            }
            p.name = s->name;
            if (s->kind == SymbolKind::local) {
                const Type* t = type_of_symbol(s);
                p.is_place    = true;
                p.local       = s;
                p.is_mutable  = s->is_mut || (t->is(Kind::reference) && t->is_mut) || t->is_unknown();
                if (t->is(Kind::reference) && !t->is_mut) p.is_mutable = false;
                return p;
            }
            if (s->kind == SymbolKind::global) {
                p.is_place   = true;
                p.is_mutable = s->is_mut && (s->decl->as<VariableDecl>().storage == Storage::static_ ||
                                             s->decl->as<VariableDecl>().storage == Storage::thread_local_);
                return p;
            }
            return p;
        }
        case ExprKind::this_:
            p.is_place   = true;
            p.is_mutable = fn_.this_mut;
            p.via_this   = true;
            p.name       = "this";
            return p;
        case ExprKind::member: {
            const auto& m    = e.as<MemberExpr>();
            const Type* objt = m.object->type ? m.object->type : tc_.error();
            if (m.arrow) {
                const Type* pt = types::strip_ref(objt);
                p.is_place     = true;
                p.is_mutable   = m.deref_symbol ? m.deref_symbol->function()->is_mut : !pt->is(Kind::pointer) || pt->is_mut;
                p.name         = ast::to_string(m.name);
                return p;
            }
            Place base = place_of(*m.object);
            p          = base;
            p.name     = (base.name.empty() ? "" : base.name + ".") + ast::to_string(m.name);
            if (objt->is(Kind::reference) && base.local == nullptr) p.is_mutable = objt->is_mut;
            if (!base.is_place) {
                p.is_place     = true;   // fields of temporaries: writable but pointless; not our concern here
                p.is_mutable   = true;
                p.is_temporary = true;
            }
            return p;
        }
        case ExprKind::index: {
            Place base = place_of(*e.as<IndexExpr>().object);
            base.name += "[...]";
            if (!base.is_place) {
                base.is_place = base.is_mutable = true;
            }
            return base;
        }
        case ExprKind::unary: {
            const auto& u = e.as<UnaryExpr>();
            if (u.op == UnaryOp::deref) {
                const Type* pt = types::strip_ref(u.operand->type ? u.operand->type : tc_.error());
                p.is_place     = true;
                p.is_mutable   = !pt->is(Kind::pointer) || pt->is_mut;
                p.name         = "*" + place_of(*u.operand).name;
                return p;
            }
            if (u.op == UnaryOp::move || u.op == UnaryOp::mut_marker) return place_of(*u.operand);
            return p;
        }
        default: return p;   // temporaries
        }
    }

    void require_mutable(const Place& p, const Expr& e, std::string_view action) {
        if (!p.is_place) {
            error(DiagCode::E4025, e.span, std::format("cannot {} this expression", action), "not a variable or field");
            return;
        }
        if (p.is_mutable) return;
        if (p.via_this) {
            Diagnostic& d = error(DiagCode::E4021, e.span,
                                  std::format("cannot {} '{}' in a non-mut method", action, p.name), "'this' is immutable here");
            note(d, std::format("add 'mut' after the parameter list: '{}(...) mut'", fn_.name));
            return;
        }
        Diagnostic& d = error(DiagCode::E4020, e.span, std::format("cannot {} '{}': it is not mutable", action, p.name),
                              "immutable");
        if (p.local) {
            const Type* t = type_of_symbol(p.local);
            if (t->is(Kind::reference)) {
                note(d, std::format("'{}' is an immutable reference; declare it 'mut {}'", p.local->name,
                                    types::to_string(t)),
                     p.local->span);
            } else {
                note(d, std::format("declare it 'mut {} {}'", types::to_string(t), p.local->name), p.local->span);
            }
        }
    }

    // --- expressions ----------------------------------------------------------------------

    const Type* set(Expr& e, const Type* t) {
        e.type = t;
        return t;
    }

    const Type* infer(Expr& e) {
        switch (e.kind) {
        case ExprKind::int_literal: {
            const auto& lit = e.as<LiteralExpr>();
            if (!lit.suffix.empty()) {
                const Type* t = tc_.builtin(lit.suffix);
                bool negative = negated_literal_;
                negated_literal_ = false;
                if (t && t->is_integer() && !types::literal_fits(lit.int_value, negative, t)) {
                    note(error(DiagCode::E4002, e.span,
                               std::format("{}{} does not fit in {}", negative ? "-" : "", lit.text, quoted(t)),
                               "out of range"),
                         std::format("{} holds {}", quoted(t), types::integer_range(t)));
                }
                return set(e, t ? t : tc_.error());
            }
            return set(e, tc_.untyped_int());
        }
        case ExprKind::float_literal: {
            const auto& lit = e.as<LiteralExpr>();
            if (!lit.suffix.empty()) return set(e, tc_.builtin(lit.suffix));
            return set(e, tc_.untyped_float());
        }
        case ExprKind::char_literal: return set(e, tc_.char_());
        case ExprKind::bool_literal: return set(e, tc_.bool_());
        case ExprKind::nullptr_literal: return set(e, tc_.nullptr_());
        case ExprKind::string_literal: {
            auto& lit = e.as<LiteralExpr>();
            for (FormatPart& part : lit.format_parts) {
                if (part.expr) check_format_hole(part);
            }
            if (lit.token_kind == TokenKind::c_string_literal) return set(e, tc_.pointer(tc_.builtin("u8"), false));
            return set(e, lit.token_kind == TokenKind::format_string_literal ? tc_.external("std::format_args")
                                                                              : tc_.static_str());
        }
        case ExprKind::this_:
            if (!fn_.self || !fn_.has_this) return set(e, tc_.error());
            return set(e, tc_.reference(tc_.user(fn_.self), fn_.this_mut));
        case ExprKind::default_: return set(e, tc_.untyped_default());
        case ExprKind::break_:
        case ExprKind::continue_: return set(e, tc_.never());
        case ExprKind::error: return set(e, tc_.error());
        case ExprKind::path: return infer_path(e.as<PathExpr>());
        case ExprKind::unary: return infer_unary(e.as<UnaryExpr>());
        case ExprKind::binary: return infer_binary(e.as<BinaryExpr>());
        case ExprKind::range: {
            auto&       r  = e.as<RangeExpr>();
            const Type* lo = types::strip_ref(infer(*r.lo));
            const Type* hi = types::strip_ref(infer(*r.hi));
            const Type* el = unify_operands(*r.lo, *r.hi, lo, hi, e.span, "range bounds");
            if (!el->is_integer() && !el->is_untyped() && !el->is_unknown() && !el->is(Kind::char_)) {
                error(DiagCode::E4013, e.span, std::format("ranges need integer bounds, found {}", quoted(el)),
                      "not an integer");
                el = tc_.error();
            }
            return set(e, tc_.range(el));
        }
        case ExprKind::call: {
            const Type* t = infer_call(e.as<CallExpr>());
            fold_compile_time_call(e.as<CallExpr>());
            return t;
        }
        case ExprKind::index: {
            auto& i = e.as<IndexExpr>();
            return set(e, infer_index(i));
        }
        case ExprKind::member: return infer_member(e.as<MemberExpr>());
        case ExprKind::try_: return infer_try(e.as<TryExpr>());
        case ExprKind::cast: return infer_cast(e.as<CastExpr>());
        case ExprKind::bit: return infer_bit(e.as<BitExpr>());
        case ExprKind::struct_literal: return infer_struct_literal(e.as<StructLiteralExpr>());
        case ExprKind::lambda: return infer_lambda(e.as<LambdaExpr>());
        case ExprKind::if_: return infer_if(e.as<IfExpr>());
        case ExprKind::match: return infer_match(e.as<MatchExpr>());
        case ExprKind::block: return infer_block(e.as<BlockExpr>());
        case ExprKind::while_: {
            auto& w = e.as<WhileExpr>();
            check_condition(*w.condition, "while");
            infer_block(*w.body);
            return set(e, diverges(e) ? tc_.never() : tc_.void_());
        }
        case ExprKind::for_: return infer_for(e.as<ForExpr>());
        case ExprKind::return_: {
            auto& r = e.as<ReturnExpr>();
            check_return(r);
            return set(e, tc_.never());
        }
        }
        return set(e, tc_.error());
    }

    void check_return(ReturnExpr& r) {
        if (r.value) infer(*r.value);
        if (!fn_.result && fn_.deducing) {   // the first `return` decides the result type (spec 15, 18)
            if (fn_.generic) return;   // decided per instance
            if (!r.value) {
                if (!fn_.deduced) fn_.deduced = tc_.void_();
                return;
            }
            if (!fn_.deduced) {
                const Type* t = types::strip_ref(r.value->type);
                if (t->is_untyped()) {
                    no_context(*r.value, "(the return type is taken from the first 'return'; give the value a type)");
                    t = tc_.error();
                }
                fn_.deduced = t->is(Kind::never) ? nullptr : t;
                return;
            }
            coerce(*r.value, fn_.deduced);
            return;
        }
        const Type* result = fn_.result;
        if (!result) return;   // lambda without '->', or deduce
        if (result->is(Kind::void_)) {
            if (r.value && !(r.value->type && r.value->type->is(Kind::void_))) {
                error(DiagCode::E4041, r.value->span, std::format("'{}' returns void but a value is returned here", fn_.name),
                      "remove this value");
            }
            return;
        }
        if (!r.value) {
            if (!result->is_unknown() && !result->is(Kind::never)) {
                error(DiagCode::E4040, r.span, std::format("'{}' must return a {}", fn_.name, quoted(result)),
                      "return without a value");
            }
            return;
        }
        coerce(*r.value, result);
    }

    void check_condition(Expr& e, std::string_view construct) {
        const Type* t = types::strip_ref(infer(e));
        if (t->is_untyped()) {
            no_context(e, std::format("in this {} condition", construct));
            return;
        }
        if (t->is(Kind::bool_) || t->is_integer() || t->is(Kind::pointer) || t->is_unknown() || t->is(Kind::external) ||
            t->is(Kind::never)) {
            return;
        }
        Diagnostic& d = error(DiagCode::E4012, e.span, std::format("{} does not convert to bool", quoted(t)),
                              "not a condition");
        if (t->is_float()) note(d, "floats are not conditions (NaN makes \"is zero\" ambiguous); write 'x != 0.0'");
        else note(d, "only bool, integers and std::raw_ptr can be conditions");
    }

    const Type* infer_path(PathExpr& pe) {
        const Symbol* s = pe.symbol;
        if (!s) {
            // `u32` used as a type value (spec 26).
            if (pe.path.segments.size() == 1) {
                if (const Type* b = tc_.builtin(pe.path.segments[0].name)) {
                    pe.constant = std::make_shared<const ct::Value>(ct::Value::of_type(b, tc_.type_value()));
                    return set(pe, tc_.type_value());
                }
            }
            return set(pe, tc_.error());
        }
        if (s->kind == SymbolKind::global) check_static_mut_access(*s, pe.span);
        if (s->kind == SymbolKind::value_param) {   // `N` of `template<usize N>` (spec 14)
            const Type* t = s->value_type ? lower(*s->value_type) : tc_.error();
            if (auto it = subst_.find(s); it != subst_.end() && it->second->is(Kind::value_arg)) {
                pe.constant = std::make_shared<const ct::Value>(value_of_arg(it->second));
            }
            return set(pe, t);
        }
        if (s->is_type()) {   // a type used as a value: `point`, `T`, `box<u32>` (spec 26)
            pe.constant = evaluate(pe, "");
            return set(pe, tc_.type_value());
        }
        switch (s->kind) {
        case SymbolKind::local:
        case SymbolKind::global: {
            const Type* t = type_of_symbol(s);
            if (s->kind == SymbolKind::global && s->decl && s->decl->kind == DeclKind::variable &&
                s->decl->as<VariableDecl>().storage == Storage::compile_time) {
                pe.constant = s->ct_value ? s->ct_value : ensure_constant(s);
            } else if (s->ct_value) {
                pe.constant = s->ct_value;   // a compile_time local or a `for compile_time` loop variable
            }
            return set(pe, t);
        }
        case SymbolKind::alternative: {
            if (is_std_export(pe)) return set(pe, std_alternative_type(pe, nullptr));
            const Type* et = enum_type_of(s, pe.path);
            pe.symbol      = instance_alternative(pe.symbol, et);
            return set(pe, et);
        }
        case SymbolKind::function:
        case SymbolKind::method:
            note(error(DiagCode::E4073, pe.span, std::format("'{}' is a function; call it", s->name), "not called"),
                 std::format("write '{}(...)'", ast::to_string(pe.path)));
            return set(pe, tc_.error());
        default: return set(pe, tc_.error());
        }
    }

    /// `x.join()` where `join` ends with `move` (spec 06, 27): `x` must be a value the caller owns,
    /// a local variable, a by-value parameter or a temporary, never something reached through a
    /// reference, a field or `this` of a method that doesn't consume it.
    void check_consumed_receiver(const Expr& object, const std::string& name, Span span) {
        const Expr* o = &object;
        if (o->kind == ExprKind::unary && o->as<UnaryExpr>().op == UnaryOp::move) o = o->as<UnaryExpr>().operand.get();
        const Type* t = o->type;
        if (!t || t->is_unknown()) return;
        std::string why;
        if (o->kind == ExprKind::path) {
            const Symbol* l = o->as<PathExpr>().symbol;
            if (!l || l->kind != SymbolKind::local) why = "it is not a local variable";
            else if (t->is(Kind::reference) || l->is_reference) why = std::format("'{}' is a reference", l->name);
            else if (l->local_kind == sema::LocalKind::capture) why = std::format("'{}' is a capture of the lambda", l->name);
            else return;
        } else if (o->kind == ExprKind::this_) {
            if (fn_.decl && fn_.decl->is_move) return;
            why = "'this' is only borrowed by this method (add 'move' after its parameter list to consume it)";
        } else if (o->kind == ExprKind::member || o->kind == ExprKind::index || t->is(Kind::reference) ||
                   (o->kind == ExprKind::unary && o->as<UnaryExpr>().op == UnaryOp::deref)) {
            why = "it is part of another value or reached through a reference";
        } else {
            return;   // a temporary
        }
        note(error(DiagCode::E4220, span, std::format("'{}' consumes the object it is called on, but {}", name, why),
                   "needs an owned value"),
             "a method ending in 'move' takes ownership of 'this'; call it on a local variable, a by-value parameter or a "
             "temporary (spec 06)");
    }

    void check_static_mut_access(const Symbol& s, Span span) {
        const auto& v = s.decl->as<VariableDecl>();
        if (!v.is_mut || v.storage != Storage::static_ || unsafe_depth_ > 0) return;
        const Type* t = lower(*v.type);
        note(error(DiagCode::E4102, span, std::format("'{}' is a 'static mut' of type {}", s.name, quoted(t)),
                   "needs 'unsafe'"),
             "every thread can see a 'static mut', so every access is unsafe; for shared mutable state use a 'static' "
             "synchronized type such as std::atomic<T> or std::mutex<T> (spec 27)");
    }

    const Type* unify_operands(Expr& lhs, Expr& rhs, const Type* l, const Type* r, Span span, std::string_view what) {
        if (l->is_unknown() || r->is_unknown()) {
            if (l->is_untyped()) settle(lhs, r);
            if (r->is_untyped()) settle(rhs, l);
            return tc_.error();
        }
        if (l->is_untyped() && r->is_untyped()) {
            if (l != r) {
                error(DiagCode::E4011, span, "integer and float literals cannot be mixed", "write e.g. '1.0' instead of '1'");
                return tc_.error();
            }
            return l;
        }
        if (l->is_untyped()) {
            settle(lhs, r);
            return r;
        }
        if (r->is_untyped()) {
            settle(rhs, l);
            return l;
        }
        if (l != r) {
            Diagnostic& d = error(DiagCode::E4011, span,
                                  std::format("{} have different types: {} and {}", what, quoted(l), quoted(r)),
                                  "mismatched types");
            if (l->is_numeric() && r->is_numeric()) {
                const Type* wide = lossless(l, r) ? r : lossless(r, l) ? l : nullptr;
                if (wide) note(d, std::format("there are no implicit conversions; convert one side with 'cast<{}>(...)'",
                                              types::to_string(wide)));
                else note(d, "there are no implicit conversions; convert one side with a cast");
            }
            return tc_.error();
        }
        return l;
    }

    /// Checks that `op` applies to values of type `t` (already unified).
    bool check_operator_type(TokenKind op, const Type* t, Span span) {
        if (t->is_unknown() || t->is_untyped() || t->is(Kind::external)) return true;
        std::string_view o = token_kind_spelling(op);
        if (is_arithmetic(op)) {
            if (t->is_numeric()) return true;
        } else if (is_wrapping(op) || is_bitwise(op) || is_shift(op)) {
            if (t->is_integer()) return true;
            if (is_wrapping(op) && t->is_float()) {
                error(DiagCode::E4013, span, std::format("'{}' is wrapping integer arithmetic; floats never wrap", o),
                      std::format("use '{}'", o.substr(0, 1)));
                return false;
            }
        } else {
            return true;
        }
        Diagnostic& d = error(DiagCode::E4013, span,
                              std::format("operator '{}' does not apply to {}", o, quoted(t)), "invalid operand type");
        if (is_bitwise(op) && t->is(Kind::bool_)) note(d, "for booleans use '&&' and '||'");
        return false;
    }

    /// A user-defined operator for `l op r`: a method `operator op` on `l` or a free function.
    /// `found` (if given) receives the operator's symbol.
    const Type* find_operator(std::string_view op, const Type* l, const Type* r, Symbol** found = nullptr) {
        auto try_fn = [&](Symbol* s, bool is_method) -> const Type* {
            const Type* t = match_operator(s, is_method, op, l, r);
            if (t) use_method(s, s->span);
            if (t && found) *found = s;
            return t;
        };
        return search_operator(op, l, try_fn);
    }

    const Type* match_operator(const Symbol* s, bool is_method, std::string_view op, const Type* l, const Type* r) {
        {
            const FunctionDecl* f = s->function();
            if (!f || !f->is_operator || f->name != op) return nullptr;
            std::size_t want = (r ? 2u : 1u) - (is_method ? 1u : 0u);   // r is null for unary operators
            if (f->params.size() != want) return nullptr;
            const Type* first = is_method ? l : lower_owned(s, *f->params[0].type);
            const Type* second = r ? lower_owned(s, *f->params[want - 1].type) : nullptr;
            if (!is_method && !operand_fits(l, first)) return nullptr;
            if (r && !operand_fits(r, second)) return nullptr;
            if (f->is_defaulted && op == "==") return tc_.bool_();
            return f->result ? lower_owned(s, *f->result) : tc_.void_();
        }
    }

    /// Whether an operand of type `from` can be passed for an operator parameter of type `to`;
    /// an untyped literal takes the parameter's type (spec 02).
    bool operand_fits(const Type* from, const Type* to) {
        const Type* t = types::strip_ref(to);
        switch (from->kind) {
        case Kind::untyped_int: return t->is_unknown() || t->is_numeric();
        case Kind::untyped_float: return t->is_unknown() || t->is_float();
        case Kind::untyped_default: return true;
        default: return assign_cost(from, to) >= 0;
        }
    }

    /// Gives the untyped literal operands of the operator call the types of its parameters.
    void settle_operands(const Symbol* s, Expr* lhs, Expr& rhs) {
        const FunctionDecl* f = s->function();
        if (!f) return;
        bool method = f->params.size() == 1;
        if (lhs && !method && lhs->type && lhs->type->is_untyped()) settle(*lhs, types::strip_ref(lower_owned(s, *f->params[0].type)));
        if (rhs.type && rhs.type->is_untyped() && !f->params.empty()) {
            settle(rhs, types::strip_ref(lower_owned(s, *f->params.back().type)));
        }
    }

    template <typename F>
    const Type* search_operator(std::string_view op, const Type* l, F&& try_fn) {
        if (l->is(Kind::user)) {
            if (auto it = l->symbol->members.find(std::string(op)); it != l->symbol->members.end()) {
                for (Symbol* m : it->second) {
                    if (const Type* t = try_fn(m, true)) return t;
                }
            }
        }
        for (const auto* items : {&module_->items}) {
            if (auto it = items->find(std::string(op)); it != items->end()) {
                for (Symbol* s : it->second) {
                    if (const Type* t = try_fn(s, false)) return t;
                }
            }
        }
        // Operators declared in the module that owns the type.
        if (l->is(Kind::user) && l->symbol->module != module_) {
            auto& items = l->symbol->module->items;
            if (auto it = items.find(std::string(op)); it != items.end()) {
                for (Symbol* s : it->second) {
                    if (const Type* t = try_fn(s, false)) return t;
                }
            }
        }
        return nullptr;
    }

    const Type* infer_binary(BinaryExpr& b) {
        TokenKind op = b.op;
        if (op == TokenKind::amp_amp || op == TokenKind::pipe_pipe) {
            check_condition(*b.lhs, std::string(token_kind_spelling(op)));
            check_condition(*b.rhs, std::string(token_kind_spelling(op)));
            return set(b, tc_.bool_());
        }
        const Type* l = types::strip_ref(infer(*b.lhs));
        const Type* r = types::strip_ref(infer(*b.rhs));

        if (op == TokenKind::spaceship) {
            if (l->is(Kind::user) || r->is(Kind::user)) {
                if (const Type* res = find_operator("<=>", l, r, &b.op_symbol)) {
                    settle_operands(b.op_symbol, b.lhs.get(), *b.rhs);
                    return set(b, res);
                }
                error(DiagCode::E4013, b.span, std::format("operator '<=>' is not defined for {}", quoted(l->is(Kind::user) ? l : r)),
                      "no such operator");
                return set(b, ordering_type());
            }
            if (l->is_untyped() && r->is_untyped()) {
                no_context(*b.lhs, "(both sides of '<=>' are untyped literals)");
                return set(b, ordering_type());
            }
            const Type* t = unify_operands(*b.lhs, *b.rhs, l, r, b.span, "operands of '<=>'");
            if (!t->is_numeric() && !t->is(Kind::char_) && !t->is_unknown() && !t->is_untyped()) {
                error(DiagCode::E4013, b.span, std::format("{} values cannot be ordered", quoted(t)), "not ordered");
            }
            return set(b, ordering_type());
        }
        // User-defined operators.
        if (l->is(Kind::user) || r->is(Kind::user)) {
            std::string_view spelling = token_kind_spelling(op);
            bool is_eq = op == TokenKind::eq_eq || op == TokenKind::bang_eq;
            if (is_eq) spelling = "==";
            if (is_comparison(op)) spelling = "<=>";
            if (const Type* res = find_operator(spelling, l, r, &b.op_symbol)) {
                settle_operands(b.op_symbol, b.lhs.get(), *b.rhs);
                return set(b, is_eq || is_comparison(op) ? tc_.bool_() : res);
            }
            if (is_eq && l == r && l->is_user(SymbolKind::enum_) && !enum_has_payload(l->symbol)) {
                return set(b, tc_.bool_());
            }
            if (is_eq && l == r) {
                Diagnostic& d = error(DiagCode::E4014, b.span, std::format("{} has no 'operator=='", quoted(l)),
                                      "cannot compare");
                note(d, std::format("'==' is opt-in: add 'bool operator==({}& other) = default;'", l->symbol->name));
                return set(b, tc_.bool_());
            }
            if (!l->is_unknown() && !r->is_unknown() && !l->is_untyped() && !r->is_untyped() && l != r) {
                unify_operands(*b.lhs, *b.rhs, l, r, b.span, "operands");
                return set(b, tc_.error());
            }
            error(DiagCode::E4013, b.span,
                  std::format("operator '{}' is not defined for {}", token_kind_spelling(op), quoted(l->is(Kind::user) ? l : r)),
                  "no such operator");
            return set(b, is_eq || is_comparison(op) ? tc_.bool_() : tc_.error());
        }
        if (l->is(Kind::external) || r->is(Kind::external)) {
            if (l->is_untyped()) settle(*b.lhs, r);
            if (r->is_untyped()) settle(*b.rhs, l);
            bool boolean = is_comparison(op) || op == TokenKind::eq_eq || op == TokenKind::bang_eq;
            return set(b, boolean ? tc_.bool_() : tc_.error());
        }
        if (is_shift(op)) {
            check_operator_type(op, l, b.span);
            if (r->is_untyped()) settle(*b.rhs, l->is_untyped() ? tc_.builtin("u32") : l);
            else if (!r->is_integer() && !r->is_unknown()) {
                error(DiagCode::E4013, b.rhs->span, std::format("shift count must be an integer, found {}", quoted(r)),
                      "not an integer");
            }
            if (!l->is_untyped()) check_shift_count(*b.rhs, l);
            return set(b, l);
        }
        if (op == TokenKind::eq_eq || op == TokenKind::bang_eq || is_comparison(op)) {
            if (l->is_untyped() && r->is_untyped()) {
                no_context(*b.lhs, "(both sides of the comparison are untyped literals)");
                return set(b, tc_.bool_());
            }
            if ((l->is(Kind::pointer) && r->is(Kind::nullptr_)) || (l->is(Kind::nullptr_) && r->is(Kind::pointer))) {
                return set(b, tc_.bool_());
            }
            if (l->is(Kind::pointer) && r->is(Kind::pointer) && l->inner == r->inner) {   // `mut` does not matter
                return set(b, tc_.bool_());
            }
            const Type* t = unify_operands(*b.lhs, *b.rhs, l, r, b.span, "the compared values");
            if (is_comparison(op) && !t->is_numeric() && !t->is(Kind::char_) && !t->is_unknown() && !t->is_untyped()) {
                error(DiagCode::E4013, b.span, std::format("{} values cannot be ordered", quoted(t)), "not ordered");
            }
            return set(b, tc_.bool_());
        }
        const Type* t = unify_operands(*b.lhs, *b.rhs, l, r, b.span, "operands");
        check_operator_type(op, t, b.span);
        return set(b, t);
    }

    static bool enum_has_payload(const Symbol* e) {
        return std::any_of(e->member_order.begin(), e->member_order.end(),
                           [](const Symbol* a) { return a->alternative && a->alternative->payload; });
    }

    /// Constant shift counts must be smaller than the width (spec 16).
    void check_shift_count(const Expr& count, const Type* t) {
        if (!t->is_integer()) return;
        const Expr* c = &count;
        if (c->kind != ExprKind::int_literal) return;
        u128 n = c->as<LiteralExpr>().int_value;
        if (n >= static_cast<u128>(t->bits)) {
            note(error(DiagCode::E4080, count.span,
                       std::format("shift by {} is not smaller than the width of {} ({} bits)", rift::to_string(n),
                                   quoted(t), t->bits),
                       "out of range"),
                 "a shift by the full width or more would panic at run time");
        }
    }

    void check_bit_index(Expr& operand, const Type* t) {
        const Type* n = operand.type ? operand.type : infer(operand);
        if (n->is_untyped()) settle(operand, tc_.builtin("u32"));
        if (operand.kind == ExprKind::int_literal && t->is_integer()) {
            u128 v = operand.as<LiteralExpr>().int_value;
            if (v >= static_cast<u128>(t->bits)) {
                note(error(DiagCode::E4080, operand.span,
                           std::format("bit({}) does not exist in {} ({} bits)", rift::to_string(v), quoted(t), t->bits),
                           "out of range"),
                     std::format("bit indices of {} are 0 to {}", quoted(t), t->bits - 1));
            }
        }
    }

    const Type* infer_unary(UnaryExpr& u) {
        // `-128i8`: a suffixed literal under '-' is range-checked with its sign.
        bool negated_literal = u.op == UnaryOp::negate && u.operand->kind == ExprKind::int_literal &&
                               !u.operand->as<LiteralExpr>().suffix.empty();
        negated_literal_      = negated_literal;
        const Type* t         = infer(*u.operand);
        negated_literal_      = false;
        const Type* s = types::strip_ref(t);
        if (s->is(Kind::user) && u.op == UnaryOp::deref) {   // `*p` on a smart pointer: its `operator*`
            if (const Type* target = smart_target(*u.operand, s, u.op_symbol)) {
                return set(u, tc_.reference(target, u.op_symbol->function()->is_mut));
            }
        }
        if (s->is(Kind::user) && (u.op == UnaryOp::negate || u.op == UnaryOp::bit_not)) {   // spec 16
            std::string_view spelling = u.op == UnaryOp::negate ? "-" : "~";
            if (const Type* res = find_operator(spelling, s, nullptr, &u.op_symbol)) return set(u, res);
            error(DiagCode::E4013, u.span, std::format("operator '{}' is not defined for {}", spelling, quoted(s)),
                  "no such operator");
            return set(u, tc_.error());
        }
        switch (u.op) {
        case UnaryOp::negate:
            if (s->is_untyped()) return set(u, s);
            if (s->is_integer() && !s->is_signed) {
                error(DiagCode::E4015, u.span, std::format("cannot negate a value of unsigned type {}", quoted(s)),
                      "unsigned");
            } else if (!s->is_numeric() && !s->is_unknown() && !s->is(Kind::external) && !s->is(Kind::user)) {
                error(DiagCode::E4013, u.span, std::format("operator '-' does not apply to {}", quoted(s)), "not a number");
            }
            return set(u, s);
        case UnaryOp::logical_not:
            check_condition_type(*u.operand, s);
            return set(u, tc_.bool_());
        case UnaryOp::bit_not:
            if (s->is_untyped()) return set(u, s);
            check_operator_type(TokenKind::amp, s, u.span);
            return set(u, s);
        case UnaryOp::deref:
            if (s->is(Kind::pointer)) {
                require_unsafe(u.span, "dereferencing a raw pointer");
                return set(u, s->inner);
            }
            if (s->is_unknown() || s->is(Kind::external)) return set(u, tc_.error());
            error(DiagCode::E4013, u.span, std::format("cannot dereference a value of type {}", quoted(s)), "not a pointer");
            return set(u, tc_.error());
        case UnaryOp::address_of: return set(u, tc_.pointer(s, false));
        case UnaryOp::address_of_mut:
            require_mutable(place_of(*u.operand), *u.operand, "take a mutable pointer to");
            return set(u, tc_.pointer(s, true));
        case UnaryOp::move:
        case UnaryOp::mut_marker: return set(u, t);
        }
        return set(u, tc_.error());
    }

    void check_condition_type(Expr& e, const Type* t) {
        if (t->is_untyped()) {
            no_context(e, "in this condition");
            return;
        }
        if (t->is(Kind::bool_) || t->is_integer() || t->is(Kind::pointer) || t->is_unknown() || t->is(Kind::external)) return;
        Diagnostic& d = error(DiagCode::E4012, e.span, std::format("{} does not convert to bool", quoted(t)), "not a condition");
        if (t->is_float()) note(d, "write 'x != 0.0'");
    }

    void require_unsafe(Span span, std::string_view what) {
        if (unsafe_depth_ > 0) return;
        note(error(DiagCode::E4100, span, std::format("{} requires 'unsafe'", what), "outside 'unsafe'"),
             "wrap it in 'unsafe { ... }'");
    }

    // --- members --------------------------------------------------------------------------

    Symbol* find_member(const Symbol* t, const std::string& name, bool& opaque) {
        opaque = opaque || t->has_opaque_base;
        if (auto it = t->members.find(name); it != t->members.end() && !it->second.empty()) return it->second.front();
        for (const Symbol* b : t->bases) {
            if (Symbol* s = find_member(b, name, opaque)) return s;
        }
        return nullptr;
    }

    std::vector<Symbol*> find_methods(const Symbol* t, const std::string& name) {
        std::vector<Symbol*> out;
        std::set<const Symbol*> seen;
        std::function<void(const Symbol*)> walk = [&](const Symbol* s) {
            if (!seen.insert(s).second) return;
            if (auto it = s->members.find(name); it != s->members.end()) {
                for (Symbol* m : it->second) {
                    if (m->kind == SymbolKind::method) out.push_back(m);
                }
            }
            if (out.empty()) {
                for (const Symbol* b : s->bases) walk(b);
            }
        };
        walk(t);
        return out;
    }

    bool member_visible(const Symbol* m) const {
        return m->access != Access::private_ || !m->parent || m->parent->module == module_;
    }

    const Type* infer_member(MemberExpr& m) {
        const Type* objt = infer(*m.object);
        const Type* t    = types::strip_ref(objt);
        std::string name = m.name.segments.back().name;
        if (m.arrow) {
            if (t->is(Kind::pointer)) {
                require_unsafe(m.span, "dereferencing a raw pointer with '->'");
                t = t->inner;
            } else if (const Type* target = smart_target(*m.object, t, m.deref_symbol)) {
                t = target;   // `p->x` is `(*p).x` (spec 16)
            } else {
                if (!t->is_unknown() && !t->is(Kind::external)) {
                    error(DiagCode::E4074, m.span, std::format("'->' needs a pointer, but this is {}", quoted(t)), "not a pointer");
                }
                return set(m, tc_.error());
            }
        } else if (t->is(Kind::meta)) {
            return infer_meta_member(m, t, name);
        } else if (t->is(Kind::pointer)) {
            if (name != "offset") {
                note(error(DiagCode::E4074, m.span, "use '->' to access members through a raw pointer", "use '->'"),
                     "or dereference it first: '(*p).member', inside 'unsafe'");
            }
            return set(m, tc_.error());
        }
        if (!t->is(Kind::user)) return set(m, tc_.error());
        bool    opaque = false;
        Symbol* mem    = m.symbol ? m.symbol : find_member(t->symbol, name, opaque);
        if (m.name.segments.size() > 1) {
            mem = m.symbol;   // this.base::method, resolved in M3
        }
        if (!mem) {
            if (!opaque) {
                error(DiagCode::E4070, m.name.span, std::format("{} has no field or method '{}'", quoted(t), name), "unknown member");
            }
            return set(m, tc_.error());
        }
        m.symbol = mem;
        if (!member_visible(mem)) {
            note(error(DiagCode::E4071, m.name.span,
                       std::format("'{}::{}' is private to module '{}'", t->symbol->name, name, mem->parent->module->name),
                       "private"),
                 "declared here", mem->span);
        }
        if (mem->kind == SymbolKind::method) {
            note(error(DiagCode::E4073, m.name.span, std::format("'{}' is a method; call it", name), "not called"),
                 std::format("write '.{}(...)'", name));
            return set(m, tc_.error());
        }
        if (mem->kind != SymbolKind::field) return set(m, tc_.error());
        return set(m, lower_owned(mem, *mem->decl->as<VariableDecl>().type));
    }

    // --- reflection (spec 26) -------------------------------------------------------------

    /// `std::is_integer<T>()`, `std::size_of<T>()`, `std::meta::fields<T>()`, ...: compile-time
    /// functions of the compiler. Their value is computed here, so run-time code gets a constant.
    const Type* infer_builtin(CallExpr& c, const std::string& name) {
        if (name.starts_with("compile_")) return infer_diagnostic(c, name);
        if (name == "is_compile_time" || name == "location_of") return infer_parameter_query(c, name);
        auto&       seg   = c.callee->as<PathExpr>().path.segments.back();
        std::size_t wants = name == "is_same" || name == "implements" ? 2 : 1;
        if (seg.template_args.size() != wants || !c.args.empty()) {
            error(DiagCode::E4030, c.span,
                  std::format("'{}' takes {} template argument{} and no arguments: '{}<{}>()'", ast::to_string(c.callee->as<PathExpr>().path),
                              wants, wants == 1 ? "" : "s", ast::to_string(c.callee->as<PathExpr>().path), wants == 1 ? "T" : "A, B"),
                  "wrong arguments");
            return set(c, tc_.error());
        }
        for (const TemplateArg& a : seg.template_args) {
            if (a.type) lower(*a.type);
        }
        const Type* t = name == "size_of" || name == "align_of"   ? tc_.builtin("usize")
                      : name == "type_name"                       ? tc_.static_str()
                      : name == "meta::result_of"                 ? tc_.type_value()
                      : name == "meta::fields"                    ? list_of(tc_.meta_field())
                      : name == "meta::alternatives"              ? list_of(tc_.meta_alternative())
                                                                  : tc_.bool_();
        set(c, t);
        c.constant = evaluate(c, "");
        return t;
    }

    /// `std::raw::allocate<T>(n)`, `reallocate(p, n)`, `free(p)`, `read(p)`, `write(p, v)`,
    /// `drop(p)` and `copy(to, from, n)`: the memory operations the library is written with
    /// (spec 13). All of them require `unsafe`.
    /// The thread intrinsics of std::raw (spec 27): atomic operations on memory, the futex and
    /// the spin-loop hint. The library's synchronized types are written with them.
    const Type* infer_thread_raw(CallExpr& c, const std::string& name, const std::vector<Expr*>& args) {
        static const std::map<std::string, std::size_t> arity{
            {"atomic_load", 2}, {"atomic_store", 3}, {"atomic_exchange", 3}, {"atomic_compare_exchange", 6},
            {"atomic_fetch_add", 3}, {"atomic_fetch_sub", 3}, {"atomic_fetch_and", 3}, {"atomic_fetch_or", 3},
            {"atomic_fetch_xor", 3}, {"atomic_fence", 1}, {"futex_wait", 2}, {"futex_wake", 2}, {"spin_hint", 0}};
        auto it = arity.find(name);
        if (it == arity.end() || args.size() != it->second) {
            error(DiagCode::E4030, c.span, std::format("'std::raw::{}' takes {} arguments", name, it == arity.end() ? 0 : it->second),
                  "wrong arguments");
            for (Expr* a : args) {
                if (a->type && a->type->is_untyped()) settle(*a, tc_.error());
            }
            return set(c, tc_.error());
        }
        const Symbol* order_sym = std_item("memory_order");
        const Type*   order     = order_sym ? tc_.user(order_sym) : tc_.error();
        auto ordering = [&](Expr& e) { coerce(e, order); };
        auto atomic_pointer = [&](Expr& e) -> const Type* {
            const Type* t = types::strip_ref(e.type);
            if (t->is_unknown()) return nullptr;
            const Type* v = t->is(Kind::pointer) ? t->inner : nullptr;
            if (!v || !(v->is_integer() || v->is(Kind::bool_) || v->is(Kind::char_) || v->is(Kind::pointer) || v->is_unknown())) {
                error(DiagCode::E4010, e.span,
                      std::format("expected a 'std::raw_ptr' to an integer, bool, char or raw pointer, found {}", quoted(t)),
                      "not atomic");
                return nullptr;
            }
            return v;
        };
        if (name == "spin_hint") return set(c, tc_.void_());
        if (name == "atomic_fence") {
            ordering(*args[0]);
            return set(c, tc_.void_());
        }
        if (name == "futex_wait" || name == "futex_wake") {
            coerce(*args[0], tc_.pointer(tc_.builtin("u32"), false));
            coerce(*args[1], tc_.builtin("u32"));
            return set(c, tc_.void_());
        }
        const Type* v = atomic_pointer(*args[0]);
        if (name == "atomic_load") {
            ordering(*args[1]);
            return set(c, v ? v : tc_.error());
        }
        if (name == "atomic_compare_exchange") {
            if (v) {
                coerce(*args[1], tc_.pointer(v, true));
                coerce(*args[2], v);
            }
            ordering(*args[3]);
            ordering(*args[4]);
            coerce(*args[5], tc_.bool_());
            return set(c, tc_.bool_());
        }
        if (v) coerce(*args[1], v);
        ordering(*args[2]);
        if (name.starts_with("atomic_fetch_") && v && !v->is_integer() && !v->is_unknown()) {
            error(DiagCode::E4010, args[0]->span, std::format("'std::raw::{}' works on integers, not {}", name, quoted(v)), "not an integer");
        }
        return set(c, name == "atomic_store" ? tc_.void_() : v ? v : tc_.error());
    }

    const Type* infer_raw(CallExpr& c, const std::string& name, const std::vector<Expr*>& args) {
        require_unsafe(c.span, std::format("'std::raw::{}'", name));
        if (name.starts_with("atomic_") || name.starts_with("futex_") || name == "spin_hint") return infer_thread_raw(c, name, args);
        auto& seg = c.callee->as<PathExpr>().path.segments.back();
        std::size_t want = name == "allocate" || name == "free" || name == "read" || name == "drop" ? 1
                         : name == "copy"                                                         ? 3
                                                                                                  : 2;   // reallocate, write, str_from
        bool templ = name == "allocate";
        if (args.size() != want || seg.template_args.size() != (templ ? 1u : 0u)) {
            error(DiagCode::E4030, c.span,
                  std::format("'std::raw::{}' takes {}", name,
                              name == "allocate"     ? "a type and a count: 'std::raw::allocate<T>(n)'"
                              : name == "reallocate" ? "a pointer and a count"
                              : name == "write"      ? "a pointer and a value"
                              : name == "copy"       ? "a destination, a source and a count"
                              : name == "str_from"   ? "a 'std::raw_ptr<u8>' and a length"
                                                     : "a pointer"),
                  "wrong arguments");
            for (Expr* a : args) {
                if (a->type && a->type->is_untyped()) settle(*a, tc_.error());
            }
            return set(c, tc_.error());
        }
        auto count = [&](Expr& e) { coerce(e, tc_.builtin("usize")); };
        auto pointer = [&](Expr& e, bool want_mut) -> const Type* {
            const Type* t = types::strip_ref(e.type);
            if (t->is_unknown()) return nullptr;
            if (!t->is(Kind::pointer) || (want_mut && !t->is_mut)) {
                error(DiagCode::E4010, e.span,
                      std::format("expected a {}raw pointer, found {}", want_mut ? "mutable " : "", quoted(t)),
                      want_mut ? "not a 'std::raw_ptr<mut T>'" : "not a 'std::raw_ptr'");
                return nullptr;
            }
            return t;
        };
        if (name == "allocate") {
            const Type* t = seg.template_args[0].type ? lower(*seg.template_args[0].type) : tc_.error();
            count(*args[0]);
            return set(c, tc_.pointer(t, true));
        }
        if (name == "reallocate") {
            const Type* p = pointer(*args[0], true);
            count(*args[1]);
            return set(c, p ? p : tc_.error());
        }
        if (name == "free" || name == "drop") {
            pointer(*args[0], true);
            return set(c, tc_.void_());
        }
        if (name == "read") {
            const Type* p = pointer(*args[0], false);
            return set(c, p ? p->inner : tc_.error());
        }
        if (name == "write") {
            if (const Type* p = pointer(*args[0], true)) coerce(*args[1], p->inner);
            return set(c, tc_.void_());
        }
        if (name == "str_from") {   // UTF-8 bytes as a std::str (the caller vouches for them)
            const Type* p = pointer(*args[0], false);
            if (p && p->inner != tc_.builtin("u8")) mismatch(args[0]->span, p, tc_.pointer(tc_.builtin("u8"), false));
            count(*args[1]);
            return set(c, tc_.external("std::str"));
        }
        const Type* to   = pointer(*args[0], true);   // copy
        const Type* from = pointer(*args[1], false);
        if (to && from && to->inner != from->inner && !deep_unknown(to->inner) && !deep_unknown(from->inner)) {
            mismatch(args[1]->span, from, tc_.pointer(to->inner, false));
        }
        count(*args[2]);
        return set(c, tc_.void_());
    }

    /// `std::is_compile_time(param)` and `std::location_of(param)` (spec 23): about the argument
    /// a call gives a parameter, so only in a function's compile-time code.
    const Type* infer_parameter_query(CallExpr& c, const std::string& name) {
        const Symbol* loc_sym = std_item("source_location");
        const Type*   result  = name == "is_compile_time" ? tc_.bool_() : loc_sym ? tc_.user(loc_sym) : tc_.error();
        const Symbol* p = c.args.size() == 1 && c.args[0]->kind == ExprKind::path ? c.args[0]->as<PathExpr>().symbol : nullptr;
        bool is_param = p && p->kind == SymbolKind::local &&
                        (p->local_kind == sema::LocalKind::value_param || p->local_kind == sema::LocalKind::ref_param);
        if (!is_param) {
            error(DiagCode::E4030, c.args.empty() ? c.span : c.args[0]->span,
                  std::format("'std::{}' takes a parameter of the function: 'std::{}(path)'", name, name), "not a parameter");
            for (ExprPtr& a : c.args) {
                if (a->type && a->type->is_untyped()) settle(*a, tc_.error());
            }
        } else if (!fn_.is_compile_time) {
            error(DiagCode::E4120, c.span, std::format("'std::{}' is only meaningful in a compile_time check", name),
                  "run-time code");
        }
        return set(c, result);
    }

    /// std::compile_error([id,] msg), std::compile_warning(id, msg), std::compile_note(msg) (spec 23).
    /// In compile-time code they run when that code runs; elsewhere (a taken `if compile_time`
    /// branch) when the code is checked.
    const Type* infer_diagnostic(CallExpr& c, const std::string& name) {
        std::size_t first = 0;   // an optional std::source_location: where to report (spec 23)
        if (!c.args.empty()) {
            const Type* t0 = types::strip_ref(c.args[0]->type);
            if (t0 && t0->is(Kind::user) && t0->symbol == std_item("source_location")) first = 1;
        }
        std::size_t lo = (name == "compile_warning" ? 2 : 1) + first;
        std::size_t hi = (name == "compile_note" ? 1 : 2) + first;
        const Type* result = name == "compile_error" ? tc_.never() : tc_.void_();
        if (c.args.size() < lo || c.args.size() > hi) {
            error(DiagCode::E4030, c.span,
                  name == "compile_warning" ? std::string("'std::compile_warning' takes an id and a message")
                  : name == "compile_note"  ? std::string("'std::compile_note' takes a message")
                                            : std::string("'std::compile_error' takes a message, or an id and a message"),
                  "wrong arguments");
            return set(c, result);
        }
        for (std::size_t i = first; i < c.args.size(); ++i) {
            Expr&       a  = *c.args[i];
            const Type* t  = types::strip_ref(a.type);
            bool is_id     = c.args.size() - first == 2 && i == first;
            if (is_id) {
                check_diagnostic_id(a);
                continue;
            }
            bool ok        = t->is_unknown() || t->is(Kind::static_str) ||
                      (!is_id && t->is(Kind::external) && (t->name == "std::str" || t->name == "std::format_args"));
            if (!ok) {
                mismatch(a.span, a.type, tc_.static_str(), is_id ? " (the id)" : " (the message)");
                if (t->is_untyped()) settle(a, tc_.error());
            }
        }
        set(c, result);
        if (!fn_.is_compile_time && !fn_.generic) evaluate(c, "");
        return result;
    }

    /// The id of a warning or error is the name of a declared `compile_time std::diagnostic`
    /// (spec 23): `std::compile_warning(gfx::deprecated, ...)`, never a string.
    void check_diagnostic_id(Expr& a) {
        const Symbol* diag = std_item("diagnostic");
        const Type*   t    = types::strip_ref(a.type);
        if (t->is_unknown()) return;
        const Symbol* s = a.kind == ExprKind::path ? a.as<PathExpr>().symbol : nullptr;
        bool          named = s && s->kind == SymbolKind::global && s->decl &&
                     s->decl->as<VariableDecl>().storage == ast::Storage::compile_time;
        if (named && diag && t->is(Kind::user) && t->symbol == diag) return;
        Diagnostic& d = error(DiagCode::E4213, a.span, "the id of a diagnostic is a declared 'std::diagnostic'",
                              t->is(Kind::static_str) ? "a string, not a name" : "not a diagnostic id");
        note(d, "declare it once, with a doc comment that explains it: 'compile_time std::diagnostic deprecated = default;'");
        if (a.type && a.type->is_untyped()) settle(a, tc_.error());
    }

    /// `std::span<T>` of compile-time data: the result of `std::meta::fields<T>()`.
    const Type* list_of(const Type* element) {
        std::string spelling = "std::span<" + types::to_string(element) + ">";
        const Type* t        = tc_.external(spelling);
        if (!tc_.external_args(t)) tc_.set_external_args(t, {element});
        return t;
    }

    /// `f.name`, `a.payload`, ...: the parts of std::meta::field / std::meta::alternative.
    const Type* infer_meta_member(MemberExpr& m, const Type* t, const std::string& name) {
        bool        field = t == tc_.meta_field();
        const Type* r     = name == "name"                                  ? tc_.static_str()
                          : name == "index" || (field && name == "offset") ? tc_.builtin("usize")
                          : field && name == "type"                         ? tc_.type_value()
                          : !field && name == "payload"                     ? tc_.type_value()
                          : !field && name == "has_payload"                 ? tc_.bool_()
                                                                            : nullptr;
        if (!r) {
            error(DiagCode::E4070, m.name.span, std::format("{} has no field '{}'", quoted(t), name), "unknown member");
            return set(m, tc_.error());
        }
        set(m, r);
        if (m.object->constant) m.constant = evaluate(m, "");
        return r;
    }

    /// `f.get(value)`, `a.is(value)`, `a.get(value)`, `a.make(...)` (spec 26). The handle must be
    /// a compile-time value: what the call does depends on which field or alternative it is.
    const Type* infer_meta_method(CallExpr& c, MemberExpr& me, const Type* t, const std::string& name, std::vector<Expr*>& args) {
        c.callee->type = tc_.error();
        bool field     = t == tc_.meta_field();
        bool known     = field ? name == "get" : name == "is" || name == "get" || name == "make";
        if (!known) {
            error(DiagCode::E4070, me.name.span, std::format("{} has no method '{}'", quoted(t), name), "unknown method");
            return set(c, tc_.error());
        }
        const ct::Value* handle = me.object->constant.get();
        if (!handle) {
            if (evaluator().unknown() || types::strip_ref(me.object->type)->is_unknown()) return set(c, tc_.error());
            Diagnostic& d = error(DiagCode::E4200, me.object->span,
                                  std::format("'.{}' needs a {} that is known at compile time", name, quoted(t)),
                                  "not known at compile time");
            note(d, "use the loop variable of a 'for compile_time' loop, or a 'compile_time' constant");
            return set(c, tc_.error());
        }
        const Type* owner = handle->type_value;
        if (name == "make") {
            const Symbol* alt = handle->symbol;
            if (alt->alternative->payload) {
                if (args.size() != 1) {
                    error(DiagCode::E4030, c.span, std::format("'{}' carries one value: 'a.make(value)'", alt->name), "wrong number of values");
                    return set(c, owner);
                }
                coerce(*args[0], lower_owned(alt, *alt->alternative->payload));
            } else if (!args.empty()) {
                error(DiagCode::E4030, c.span, std::format("'{}' carries no value: 'a.make()'", alt->name), "wrong number of values");
            }
            return set(c, owner);
        }
        if (args.size() != 1) {
            error(DiagCode::E4030, c.span, std::format("'{}' takes the value as its one argument", name), "wrong number of arguments");
            return set(c, tc_.error());
        }
        Expr& arg    = *args[0];
        bool  marked = arg.kind == ExprKind::unary && arg.as<UnaryExpr>().op == UnaryOp::mut_marker;
        const Type* at = types::strip_ref(arg.type);
        if (at->is_untyped()) settle(arg, owner);
        else if (!at->is_unknown() && at != owner) mismatch(arg.span, arg.type, owner, " (the reflected value)");
        if (field) {
            if (marked) require_mutable(place_of(*arg.as<UnaryExpr>().operand), *arg.as<UnaryExpr>().operand, "change a field of");
            const Symbol* f = handle->symbol;
            return set(c, tc_.reference(lower_owned(f, *f->decl->as<VariableDecl>().type), marked));
        }
        if (name == "is") return set(c, tc_.bool_());
        const Symbol* alt = handle->symbol;   // a.get(value): std::option<P&>
        if (!alt->alternative->payload) {
            error(DiagCode::E4070, me.name.span, std::format("'{}' has no payload; use 'a.is(value)'", alt->name), "no payload");
            return set(c, tc_.error());
        }
        const Symbol* option = std_item("option");
        if (!option) return set(c, tc_.error());
        return set(c, tc_.instance(option, {tc_.reference(lower_owned(alt, *alt->alternative->payload), false)}));
    }

    // --- calls ----------------------------------------------------------------------------

    struct Candidate {
        Symbol*                                 symbol;
        const FunctionDecl*                     decl;
        std::map<const Symbol*, const Type*>    subst;
        std::vector<const Type*>                params;
        const Type*                             result = nullptr;
        int                                     cost   = 0;
        bool                                    undeducible = false;
        std::string                             undeduced_name;
        bool                                    mismatch = false;   // an argument is an instance of another template
    };

    /// Whether `at` can be an instance of what the parameter pattern names: `std::span<T>` never
    /// accepts a `std::span<mut f32>`, so such a template overload is not viable.
    static bool shape_fits(const TypeExpr& pattern, const Type* at) {
        if (!at || at->is_unknown() || at->is_untyped()) return true;
        if (pattern.kind == TypeKind::reference) return shape_fits(*pattern.as<ReferenceType>().inner, types::strip_ref(at));
        if (pattern.kind != TypeKind::named) return true;
        const auto&   nt  = pattern.as<NamedType>();
        const Symbol* sym = nt.symbol;
        if (!sym || !sym->decl || !sym->decl->is_template || nt.path.segments.empty()) return true;
        const auto& ta = nt.path.segments.back().template_args;
        if (sym->name == "span" && sym->module && sym->module->name == "std" && ta.size() == 1 && ta[0].is_mut && ta[0].type &&
            ta[0].type->kind != TypeKind::reference) {
            auto it = sym->module->items.find("span_mut");
            if (it != sym->module->items.end()) sym = it->second.front();
        }
        at = types::strip_ref(at);
        return at->is(Kind::user) && at->symbol->template_of == sym;
    }

    /// The template parameter symbol a parameter type is spelled as (`T` or `T&`), if any.
    static const Symbol* param_template_symbol(const TypeExpr& t) {
        const TypeExpr* inner = t.kind == TypeKind::reference ? t.as<ReferenceType>().inner.get() : &t;
        if (inner->kind != TypeKind::named) return nullptr;
        const Symbol* s = inner->as<NamedType>().symbol;
        return s && s->kind == SymbolKind::type_param ? s : nullptr;
    }

    /// Template parameter symbols of a function, by matching the names used in its signature.
    std::vector<const Symbol*> template_symbols(const FunctionDecl& f) {
        std::vector<const Symbol*> out(f.template_params.size(), nullptr);
        std::function<void(const TypeExpr&)> scan = [&](const TypeExpr& t) {
            if (t.kind == TypeKind::reference) {
                scan(*t.as<ReferenceType>().inner);
                return;
            }
            if (t.kind != TypeKind::named) return;
            const auto& nt = t.as<NamedType>();
            for (const PathSegment& seg : nt.path.segments) {   // `std::darray<T>&`: T inside the arguments
                for (const TemplateArg& a : seg.template_args) {
                    if (a.type) scan(*a.type);
                }
            }
            if (nt.symbol && nt.symbol->kind == SymbolKind::type_param) {
                for (std::size_t i = 0; i < f.template_params.size(); ++i) {
                    if (f.template_params[i].name == nt.symbol->name && !out[i]) out[i] = nt.symbol;
                }
            }
            for (const PathSegment& seg : nt.path.segments) {
                for (const TemplateArg& a : seg.template_args) {
                    if (a.type) scan(*a.type);
                }
            }
        };
        for (const Param& p : f.params) scan(*p.type);
        if (f.result) scan(*f.result);
        // A parameter that only the body uses (`std::is_integer<T>()`): its own symbol.
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (!out[i] && i < f.template_param_symbols.size()) out[i] = f.template_param_symbols[i];
        }
        return out;
    }

    /// Whether `t` is unknown or built from an unknown type (`std::raw_ptr<node<T>>` in a generic check).
    static bool deep_unknown(const Type* t) {
        if (!t) return false;
        if (t->is_unknown()) return true;
        if (t->is(Kind::pointer) || t->is(Kind::reference)) return deep_unknown(t->inner);
        if (t->is(Kind::user)) {
            for (const Type* a : t->symbol->template_args) {
                if (deep_unknown(a)) return true;
            }
        }
        return false;
    }

    /// The template parameters a parameter type mentions stay unknown (an argument whose type is
    /// not known yet, inside a template's generic check): no "cannot deduce" error.
    static void unknown_params(const TypeExpr& pattern, const Type* unknown, std::map<const Symbol*, const Type*>& subst) {
        if (pattern.kind == TypeKind::reference) {
            unknown_params(*pattern.as<ReferenceType>().inner, unknown, subst);
            return;
        }
        if (pattern.kind != TypeKind::named) return;
        const auto& nt = pattern.as<NamedType>();
        if (nt.symbol && nt.symbol->kind == SymbolKind::type_param && !subst.count(nt.symbol)) subst[nt.symbol] = unknown;
        for (const PathSegment& seg : nt.path.segments) {
            for (const TemplateArg& a : seg.template_args) {
                if (a.type) unknown_params(*a.type, unknown, subst);
            }
        }
    }

    /// Deduces template parameters by matching a parameter's type against an argument's type:
    /// `std::darray<T>&` against `std::darray<u32>` gives T = u32 (spec 14).
    static void unify(const TypeExpr& pattern, const Type* at, std::map<const Symbol*, const Type*>& subst) {
        if (!at) return;
        if (pattern.kind == TypeKind::reference) {
            unify(*pattern.as<ReferenceType>().inner, types::strip_ref(at), subst);
            return;
        }
        if (pattern.kind != TypeKind::named) return;
        const auto&   nt  = pattern.as<NamedType>();
        const Symbol* sym = nt.symbol;
        if (at->is(Kind::pointer) && !nt.path.segments.empty() && nt.path.segments.back().name == "raw_ptr") {   // std::raw_ptr<mut T>
            const auto& targs = nt.path.segments.back().template_args;
            if (!targs.empty() && targs[0].type) unify(*targs[0].type, at->inner, subst);
            return;
        }
        if (!sym) return;
        if (sym->kind == SymbolKind::type_param) {
            if (!subst.count(sym)) subst[sym] = at;
            return;
        }
        if (!nt.path.segments.empty()) {   // `std::span<mut T>` is the library's span_mut<T> (spec 20)
            const auto& ta = nt.path.segments.back().template_args;
            if (sym->name == "span" && sym->module && sym->module->name == "std" && ta.size() == 1 && ta[0].is_mut && ta[0].type &&
                ta[0].type->kind != TypeKind::reference) {
                auto it = sym->module->items.find("span_mut");
                if (it != sym->module->items.end()) sym = it->second.front();
            }
        }
        if (!at->is(Kind::user) || at->symbol->template_of != sym || nt.path.segments.empty()) return;
        const auto& targs = nt.path.segments.back().template_args;
        const auto& given = at->symbol->template_args;
        for (std::size_t i = 0; i < targs.size() && i < given.size(); ++i) {
            if (targs[i].type) {
                unify(*targs[i].type, given[i], subst);
            } else if (targs[i].value && targs[i].value->kind == ExprKind::path) {   // a value parameter: `N`
                const Symbol* v = targs[i].value->as<PathExpr>().symbol;
                if (v && v->kind == SymbolKind::type_param && !subst.count(v)) subst[v] = given[i];
            }
        }
    }

    Candidate make_candidate(Symbol* s, const std::vector<Expr*>& args, const PathSegment* explicit_args) {
        Candidate c{s, s->function(), {}, {}, nullptr, 0, false, {}};
        const FunctionDecl& f = *c.decl;
        if (f.is_template) {
            // A method template of a class template instance also uses the class's arguments.
            if (const auto* outer = s->parent ? substitution_of(s->parent) : nullptr) c.subst = *outer;
            std::vector<const Symbol*> syms = template_symbols(f);
            if (explicit_args) {
                for (std::size_t i = 0; i < explicit_args->template_args.size() && i < syms.size(); ++i) {
                    const TemplateArg& a = explicit_args->template_args[i];
                    if (syms[i] && a.type) c.subst[syms[i]] = lower(*a.type);
                    if (syms[i] && a.value) {
                        if (!a.evaluated) {
                            const Type* v = evaluate_value_arg(s, i, *a.value);
                            if (!v->is_unknown()) a.evaluated = v;
                        }
                        c.subst[syms[i]] = a.evaluated ? a.evaluated : tc_.error();
                    }
                }
            }
            for (std::size_t i = 0; i < f.params.size() && i < args.size(); ++i) {
                const Symbol* tp = param_template_symbol(*f.params[i].type);
                if (!tp) {   // `std::darray<T>&`, `either<L, R>`: deduced from the argument's instance
                    const Type* at = types::strip_ref(args[i]->type);
                    if (!shape_fits(*f.params[i].type, at)) c.mismatch = true;
                    if (at && !at->is_untyped() && !at->is_unknown()) unify(*f.params[i].type, at, c.subst);
                    if (at && deep_unknown(at)) unknown_params(*f.params[i].type, tc_.error(), c.subst);   // the generic check
                    continue;
                }
                if (c.subst.count(tp)) continue;
                const Type* at = types::strip_ref(args[i]->type);
                if (at->is_untyped()) {
                    // A literal for a parameter that a lambda argument returns too takes the lambda's
                    // result type: `reduce_chunks(data, n, 0.0, map, combine)` with `map` returning f64.
                    const Type* from_lambda = nullptr;
                    for (const Expr* other : args) {
                        const Type* ot = other->type ? types::strip_ref(other->type) : nullptr;
                        if (!ot || !ot->is(Kind::closure) || ot->args.empty()) continue;
                        const Type* r = ot->args.back();
                        bool fits = at == tc_.untyped_float() ? r->is_float() : r->is_integer() || r->is_float();
                        if (fits && !from_lambda) from_lambda = r;
                    }
                    if (from_lambda) {
                        c.subst[tp] = from_lambda;
                    } else {
                        c.undeducible    = true;
                        c.undeduced_name = tp->name;
                    }
                } else if (!at->is(Kind::never)) {
                    c.subst[tp] = at;
                }
            }
            if (c.undeducible && c.subst.count(syms.empty() ? nullptr : syms[0])) c.undeducible = false;
        }
        // A member of a template instance uses its parameters (spec 14); a function template's are deduced.
        const std::map<const Symbol*, const Type*>* owner = f.is_template ? nullptr : substitution_of(s);
        for (const Param& p : f.params) c.params.push_back(owner ? tc_.lower(*p.type, owner) : lower(*p.type, &c.subst));
        c.result = f.result ? (f.result->kind == TypeKind::deduce ? tc_.error()
                               : owner ? tc_.lower(*f.result, owner) : lower(*f.result, &c.subst))
                            : tc_.void_();
        return c;
    }

    /// Cost of passing `arg` to a parameter of type `param`, or -1.
    int arg_cost(const Expr& arg, const Type* param) {
        const Type* at = arg.type;
        const Type* p  = types::strip_ref(param);
        if (at->is_untyped()) {
            if (p->is_unknown() || p->is(Kind::external)) return 0;
            if (at == tc_.untyped_default()) return 0;
            if (at == tc_.untyped_int() && p->is_integer()) {
                const Expr* lit = &arg;
                bool        neg = false;
                while (lit->kind == ExprKind::unary && lit->as<UnaryExpr>().op == UnaryOp::negate) {
                    neg = !neg;
                    lit = lit->as<UnaryExpr>().operand.get();
                }
                if (lit->kind == ExprKind::int_literal && !types::literal_fits(lit->as<LiteralExpr>().int_value, neg, p)) {
                    return -1;
                }
                return 0;
            }
            if (at == tc_.untyped_float() && p->is_float()) return 0;
            return -1;
        }
        return assign_cost(at, param);
    }

    static const Expr& strip_marker(const Expr& e) {
        if (e.kind == ExprKind::unary && e.as<UnaryExpr>().op == UnaryOp::mut_marker) return *e.as<UnaryExpr>().operand;
        return e;
    }

    /// The function a call to `chosen` runs (spec 14): for a function template, its instance for the
    /// deduced arguments. Arguments that are still unknown (inside a template that is not instantiated)
    /// keep the template itself. Null, after reporting, when a parameter could not be deduced.
    Symbol* call_target(const Candidate& chosen, Span span, std::string_view display) {
        if (!chosen.decl->is_template) return chosen.symbol;
        std::vector<const Symbol*> syms = template_symbols(*chosen.decl);
        std::vector<const Type*>   args;
        for (const Symbol* p : syms) {
            auto it = p ? chosen.subst.find(p) : chosen.subst.end();
            if (it == chosen.subst.end()) {
                std::string name = p ? p->name : std::string("?");
                error(DiagCode::E4160, span, std::format("cannot deduce template parameter '{}' of '{}'", name, display),
                      "cannot deduce");
                return nullptr;
            }
            args.push_back(it->second);
        }
        for (const Type* a : args) {
            if (a->is_unknown()) return chosen.symbol;
        }
        if (!check_constraints(syms, args, span, display)) return nullptr;
        if (chosen.symbol->kind == SymbolKind::method) return instantiate_method(chosen.symbol, args);
        return instantiate_function(chosen.symbol, args);
    }

    /// Picks an overload, reports problems, coerces the arguments. Returns the chosen symbol.
    const Candidate* resolve_overload(std::vector<Candidate>& cands, std::vector<Expr*>& args, Span span,
                                      std::string_view display) {
        std::vector<Candidate*> arity;
        for (Candidate& c : cands) {
            if (c.params.size() == args.size()) arity.push_back(&c);
        }
        if (arity.empty()) {
            if (cands.size() == 1) {
                Diagnostic& d = error(DiagCode::E4030, span,
                                      std::format("'{}' takes {} argument{}, but {} {} given", display,
                                                  cands[0].params.size(), cands[0].params.size() == 1 ? "" : "s",
                                                  args.size(), args.size() == 1 ? "is" : "are"),
                                      "wrong number of arguments");
                note(d, "declared here", cands[0].symbol->span);
            } else {
                error(DiagCode::E4032, span, std::format("no overload of '{}' takes {} arguments", display, args.size()),
                      "no matching overload");
            }
            return nullptr;
        }
        std::vector<Candidate*> viable;
        for (Candidate* c : arity) {
            c->cost = 0;
            bool ok = !c->mismatch;
            for (std::size_t i = 0; i < args.size() && ok; ++i) {
                int cost = arg_cost(strip_marker(*args[i]), c->params[i]);
                if (cost < 0) ok = false;
                c->cost += std::max(cost, 0);
            }
            if (ok) viable.push_back(c);
        }
        Candidate* chosen = nullptr;
        if (viable.empty()) {
            if (arity.size() > 1) {
                std::string list;
                for (std::size_t i = 0; i < args.size(); ++i) {
                    list += (i ? ", " : "") + types::to_string(strip_marker(*args[i]).type);
                }
                Diagnostic& d = error(DiagCode::E4032, span, std::format("no overload of '{}' accepts ({})", display, list),
                                      "no matching overload");
                for (Candidate* c : arity) note(d, "candidate", c->symbol->span);
                return nullptr;
            }
            chosen = arity.front();   // report argument by argument below
        } else {
            int best = std::min_element(viable.begin(), viable.end(), [](Candidate* a, Candidate* b) {
                           return a->cost < b->cost;
                       })[0]->cost;
            std::vector<Candidate*> top;
            for (Candidate* c : viable) {
                if (c->cost == best) top.push_back(c);
            }
            if (top.size() > 1) {
                Diagnostic& d = error(DiagCode::E4031, span, std::format("call to '{}' is ambiguous", display),
                                      "matches several overloads");
                for (Candidate* c : top) note(d, "candidate", c->symbol->span);
                note(d, "give literals a suffix, e.g. '5u32', to pick one");
                return nullptr;
            }
            chosen = top.front();
        }
        if (chosen->undeducible) {
            Diagnostic& d = error(DiagCode::E4160, span,
                                  std::format("cannot deduce template parameter '{}' of '{}' from untyped literals",
                                              chosen->undeduced_name, display),
                                  "type unknown");
            note(d, std::format("write '{}<u32>(...)' or give a literal a suffix", display));
            return nullptr;
        }
        for (std::size_t i = 0; i < args.size(); ++i) check_argument(*args[i], chosen->params[i]);
        return chosen;
    }

    /// Call-site `mut` (spec 04) and the argument's type.
    void check_argument(Expr& arg, const Type* param) {
        bool marked  = arg.kind == ExprKind::unary && arg.as<UnaryExpr>().op == UnaryOp::mut_marker;
        bool mut_ref = param->is(Kind::reference) && param->is_mut;
        Expr& value  = marked ? *arg.as<UnaryExpr>().operand : arg;
        if (mut_ref && !marked) {
            Place p = place_of(value);
            Diagnostic& d = error(DiagCode::E4023, arg.span, "a mutable reference argument is marked with 'mut'",
                                  std::format("write 'mut {}'", p.name.empty() ? "..." : p.name));
            note(d, "the parameter is 'mut T&': the call can change the argument, and the call site says so");
        } else if (marked && !mut_ref && !param->is_unknown()) {
            error(DiagCode::E4024, arg.span,
                  std::format("'mut' marks a mutable reference argument, but the parameter is {}", quoted(param)),
                  "remove 'mut'");
        }
        if (marked || mut_ref) require_mutable(place_of(value), value, "pass as a mutable reference");
        coerce(value, param);
        if (marked) arg.type = value.type;
    }

    void check_compile_time_call(const Symbol* callee, Span span) {
        if (!fn_.is_compile_time || !callee) return;
        const FunctionDecl* f = callee->function();
        if (!f || f->is_compile_time) return;
        // The settings of a build mode are read at compile time (spec 24).
        if (callee->parent && callee->parent->module && callee->parent->module->name == "std::build::mode") return;
        for (const Symbol* c = callee->parent; c && c->kind == SymbolKind::class_; c = c->base_class()) {
            if (c->module && c->module->name == "std::build::mode") return;
        }
        Diagnostic& d = error(DiagCode::E4120, span,
                              std::format("compile-time code calls '{}', which is not a compile_time function", callee->name),
                              "run-time function");
        note(d, "declared here", callee->span);
    }

    void check_unsafe_call(const Symbol* callee, Span span) {
        if (!callee || unsafe_depth_ > 0) return;
        const FunctionDecl* f = callee->function();
        if (!f) return;
        // An extern(c) function written in Rift (one with a body) is safe to call from Rift.
        bool foreign = f->is_extern && !f->body && !(callee->definition && callee->definition->as<FunctionDecl>().body);
        if (!f->is_unsafe && !foreign) return;
        note(error(DiagCode::E4101, span,
                   std::format("'{}' is {} and can only be called inside 'unsafe'", callee->name,
                               foreign ? "an extern function" : "an unsafe function"),
                   "needs 'unsafe'"),
             "wrap the call in 'unsafe { ... }'");
    }

    const Type* infer_call(CallExpr& c) {
        std::vector<Expr*> args;
        for (ExprPtr& a : c.args) {
            infer(*a);
            args.push_back(a.get());
        }
        Expr& callee = *c.callee;

        // Free functions, static methods, enum alternatives.
        if (callee.kind == ExprKind::path) {
            auto&   pe = callee.as<PathExpr>();
            Symbol* s  = pe.symbol;
            if (Symbol* member = instance_member(pe, s)) s = pe.symbol = member;   // `box<u32>::make(...)`
            if (std::string b = ct::builtin_name(c); !b.empty()) return infer_builtin(c, b);
            if (std::string r = ct::raw_intrinsic(c); !r.empty()) return infer_raw(c, r, args);
            if (s && s->is_type()) {
                Diagnostic& d = error(DiagCode::E3022, callee.span,
                                      std::format("'{}' is a {}, not a function", ast::to_string(pe.path), sema::to_string(s->kind)),
                                      "not a function");
                if (s->kind == SymbolKind::struct_ || s->kind == SymbolKind::class_) {
                    note(d, std::format("construct a value with '{}{{ ... }}'", ast::to_string(pe.path)));
                }
                return set(c, tc_.error());
            }
            if (!s) {
                if (!pe.path.segments.empty() && pe.path.segments[0].name == "std" &&
                    pe.path.segments.back().name == "panic") {
                    return set(c, tc_.never());
                }
                if (pe.path.segments.size() == 2 && pe.path.segments[0].name == "std" && is_arith_builtin(pe.path.segments[1].name)) {
                    return infer_arith(c, pe.path.segments[1].name, args);
                }
                if (pe.path.segments.size() == 2 && pe.path.segments[0].name == "std" && is_bit_builtin(pe.path.segments[1].name)) {
                    return infer_bits(c, pe.path.segments[1].name, args);
                }
                if (pe.path.segments.size() == 2 && pe.path.segments[0].name == "std" && pe.path.segments[1].name == "format") {
                    if (const Symbol* str = std_item("string")) return set(c, tc_.user(str));   // allocates (spec 19)
                }
                for (Expr* a : args) {
                    if (a->type && a->type->is_untyped() && a->type != tc_.untyped_default()) settle(*a, tc_.error());
                }
                return set(c, tc_.error());   // std::... and unresolved names
            }
            if (s->kind == SymbolKind::alternative && is_std_export(pe)) {
                if (s->alternative->payload && args.empty()) {   // `std::ok()`: a `void` payload (spec 08, 27)
                    const Symbol*            templ = s->parent;
                    std::vector<const Type*> targs(templ->decl->template_params.size(), nullptr);
                    if (int i = payload_param(s); i >= 0) {
                        targs[static_cast<std::size_t>(i)] = tc_.void_();
                        bool done = std::all_of(targs.begin(), targs.end(), [](const Type* x) { return x != nullptr; });
                        const Type* t = done ? tc_.instance(templ, targs) : tc_.pending(templ, targs);
                        if (done) pe.symbol = instance_alternative(pe.symbol, t);
                        callee.type = t;
                        return set(c, t);
                    }
                }
                if (!s->alternative->payload || args.size() != 1) {
                    error(DiagCode::E4030, c.span,
                          s->alternative->payload
                              ? std::format("'{}' carries exactly one value, but {} are given", ast::to_string(pe.path), args.size())
                              : std::format("'{}' carries no value", ast::to_string(pe.path)),
                          "wrong number of values");
                    for (Expr* a : args) {
                        if (a->type && a->type->is_untyped()) settle(*a, tc_.error());
                    }
                    return set(c, tc_.error());
                }
                const Type* t = std_alternative_type(pe, args[0]);
                callee.type   = t;
                // `std::some(mut place)`: a `std::option<mut T&>` (some_reference checks the place).
                bool mut_place = args[0]->kind == ExprKind::unary && args[0]->as<UnaryExpr>().op == UnaryOp::mut_marker;
                if (t->is(Kind::user) && !mut_place) check_argument(*args[0], lower_owned(pe.symbol, *pe.symbol->alternative->payload));
                return set(c, t);
            }
            if (s->kind == SymbolKind::alternative) {
                const Type* et      = enum_type_of(s, pe.path);
                Symbol*     alt     = instance_alternative(pe.symbol, et);
                pe.symbol           = alt;
                const Type* payload = alt->alternative->payload ? lower_owned(alt, *alt->alternative->payload) : nullptr;
                if (payload && payload->is(Kind::void_) && args.empty()) payload = nullptr;   // `maybe<void>::some()`
                if (payload) {
                    if (args.size() != 1) {
                        error(DiagCode::E4030, c.span,
                              std::format("'{}' carries exactly one value, but {} are given", ast::to_string(pe.path), args.size()),
                              "wrong number of values");
                    } else {
                        check_argument(*args[0], payload);
                    }
                }
                callee.type = et;
                return set(c, et);
            }
            if (s->kind == SymbolKind::function || s->kind == SymbolKind::method) {
                std::vector<Symbol*> overloads;
                auto&                table = s->kind == SymbolKind::function ? s->module->items : s->parent->members;
                for (Symbol* o : table[s->name]) {
                    if (o->kind == s->kind) overloads.push_back(o);
                }
                std::vector<Candidate> cands;
                for (Symbol* o : overloads) cands.push_back(make_candidate(o, args, &pe.path.segments.back()));
                const Candidate* chosen = resolve_overload(cands, args, c.span, ast::to_string(pe.path));
                if (!chosen) return set(c, tc_.error());
                Symbol* target = call_target(*chosen, c.span, ast::to_string(pe.path));
                if (!target) return set(c, tc_.error());
                pe.symbol   = target;
                use_method(target, callee.span);
                callee.type = nullptr;   // functions are not values
                check_compile_time_call(target, c.span);
                check_unsafe_call(target, c.span);
                const Type* result = chosen->result;
                if (const FunctionDecl* tf = target->function(); tf && tf->result && tf->result->kind == TypeKind::deduce) {
                    if (!tf->deduced_result) ensure_function(target);
                    result = tf->deduced_result ? tf->deduced_result : tc_.error();   // set when its body was checked
                }
                set(c, result);
                run_entry_checks(target, c, args);
                return result;
            }
            if (s->kind == SymbolKind::local || s->kind == SymbolKind::global) {
                const Type* ct = types::strip_ref(infer(callee));
                if (ct->is(Kind::closure) || ct->is(Kind::function) || ct->is(Kind::fn_ptr)) return infer_closure_call(c, ct, args);
                for (Expr* a : args) {
                    if (a->type && a->type->is_untyped()) settle(*a, tc_.error());
                }
                return set(c, tc_.error());   // lambdas and function pointers
            }
            return set(c, tc_.error());
        }

        // Method calls: obj.method(args).
        if (callee.kind == ExprKind::member) {
            auto&       me   = callee.as<MemberExpr>();
            const Type* objt = infer(*me.object);
            const Type* t    = types::strip_ref(objt);
            std::string name = me.name.segments.back().name;
            if (t->is(Kind::pending)) {
                no_context(*me.object, std::format("before '.{}'", name));
                for (Expr* a : args) {
                    if (a->type && a->type->is_untyped()) settle(*a, tc_.error());
                }
                return set(c, tc_.error());
            }
            if (types::is_std_builtin_method(t, name)) return infer_std_method(c, t, name, args);
            if (t->is(Kind::meta)) return infer_meta_method(c, me, t, name, args);
            if (t->is(Kind::pointer) && !me.arrow) {   // spec 13: `p.offset(n)` is the only method
                callee.type = tc_.error();
                if (name != "offset") {
                    error(DiagCode::E4070, me.name.span, std::format("{} has no method '{}'", quoted(t), name), "unknown method");
                    return set(c, tc_.error());
                }
                require_unsafe(c.span, "pointer arithmetic");
                if (args.size() != 1) {
                    error(DiagCode::E4030, c.span, std::format("'offset' takes 1 argument, found {}", args.size()), "wrong number of arguments");
                    return set(c, t);
                }
                const Type* n = types::strip_ref(args[0]->type);
                if (n->is_untyped()) settle(*args[0], tc_.builtin("isize"));
                else if (!n->is_integer() && !n->is_unknown()) {
                    error(DiagCode::E4010, args[0]->span, std::format("expected an integer offset, found {}", quoted(n)), "not an integer");
                }
                return set(c, t);
            }
            if (me.arrow && t->is(Kind::pointer)) {
                require_unsafe(c.span, "calling a method through a raw pointer");
                t = t->inner;
            } else if (me.arrow && t->is(Kind::user)) {
                const Type* target = smart_target(*me.object, t, me.deref_symbol);
                if (!target) {
                    error(DiagCode::E4074, me.span, std::format("'->' needs a pointer, but this is {}", quoted(t)), "not a pointer");
                    return set(c, tc_.error());
                }
                t = target;
            }
            if (t->is(Kind::type_param)) {
                if (const Type* r = infer_constrained_call(c, me, t, name, args)) return r;
            }
            if (!t->is(Kind::user)) {
                callee.type = tc_.error();
                if (types::is_mutating_library_method(t, name)) {
                    Place p = place_of(*me.object);
                    if (p.is_place && !p.is_mutable && !p.is_temporary) {
                        if (p.via_this) {
                            note(error(DiagCode::E4021, c.span,
                                       std::format("'{}' changes the container; 'this' is immutable in a non-mut method", name),
                                       "'this' is immutable here"),
                                 std::format("add 'mut' after the parameter list: '{}(...) mut'", fn_.name));
                        } else {
                            Diagnostic& d = error(DiagCode::E4022, c.span,
                                                  std::format("'{}' changes {}, but '{}' is not mutable", name, quoted(t), p.name),
                                                  "immutable receiver");
                            if (p.local) note(d, std::format("declare it 'mut {} {}'", types::to_string(type_of_symbol(p.local)),
                                                             p.local->name),
                                              p.local->span);
                        }
                    }
                }
                if (t->is(Kind::static_str) || (t->is(Kind::external) && t->name == "std::str")) {
                    if (const Type* r = infer_str_method(c, me, name, args)) return r;
                }
                for (Expr* a : args) {
                    if (a->type && a->type->is_untyped() && a->type != tc_.untyped_default()) settle(*a, tc_.error());
                }
                // A few string methods are known before the library exists (the backend implements them).
                // Built-in copy types are cloneable too (std::cloneable, spec 14): the copy.
                if (args.empty() && name == "clone" &&
                    (t->is_integer() || t->is_float() || t->is(Kind::bool_) || t->is(Kind::char_) || t->is(Kind::pointer) ||
                     t->is(Kind::static_str) || (t->is(Kind::external) && t->name == "std::str"))) {
                    return set(c, t);
                }
                if (args.empty() && (t->is(Kind::static_str) || (t->is(Kind::external) && t->name == "std::str"))) {
                    if (name == "len") return set(c, tc_.builtin("usize"));
                    if (name == "is_empty") return set(c, tc_.bool_());
                    if (name == "as_ptr") return set(c, tc_.pointer(tc_.builtin("u8"), false));   // spec 13: for C
                }
                // Lists of compile-time data (`std::meta::fields<T>().len()`).
                if (args.empty() && t->is(Kind::external) && t->external_base() == "std::span" && compile_time_only(t)) {
                    const Type* r = name == "len" ? tc_.builtin("usize") : name == "is_empty" ? tc_.bool_() : nullptr;
                    if (r) {
                        set(c, r);
                        if (me.object->constant) c.constant = evaluate(c, "");
                        return r;
                    }
                }
                return set(c, tc_.error());
            }
            std::vector<Symbol*> methods;
            if (me.name.segments.size() > 1 && me.symbol) {
                methods.push_back(me.symbol);   // this.base::method()
            } else {
                methods = find_methods(t->symbol, name);
            }
            callee.type = nullptr;
            if (methods.empty() && name == "clone" && args.empty() &&
                (t->is_user(SymbolKind::struct_) || t->is_user(SymbolKind::enum_))) {
                return set(c, t);   // provided automatically (spec 05); cloneability is checked in M5
            }
            if (methods.empty()) {
                bool    opaque = false;
                Symbol* other  = find_member(t->symbol, name, opaque);
                if (other && other->kind == SymbolKind::field && !other->is_static) {   // a stored callable: `this.on_done(x)`
                    const Type* ft = lower_owned(other, *other->decl->as<VariableDecl>().type);
                    if (ft->is(Kind::function) || ft->is(Kind::fn_ptr)) {
                        me.symbol      = other;
                        callee.type    = ft;
                        return infer_closure_call(c, ft, args);
                    }
                }
                if (other) {
                    error(DiagCode::E4033, me.name.span, std::format("'{}' is a field, not a method", name), "not callable");
                } else if (!opaque) {
                    Diagnostic& d = error(DiagCode::E4070, me.name.span, std::format("{} has no method '{}'", quoted(t), name),
                                          "unknown method");
                    if (name == "clone" && t->is_user(SymbolKind::class_)) {
                        note(d, "classes implement 'clone()' themselves (interface std::cloneable); structs get it automatically");
                    }
                }
                return set(c, tc_.error());
            }
            std::vector<Candidate> cands;
            for (Symbol* m : methods) cands.push_back(make_candidate(m, args, &me.name.segments.back()));
            const Candidate* chosen = resolve_overload(cands, args, c.span, t->symbol->name + "::" + name);
            if (!chosen) return set(c, tc_.error());
            me.symbol = chosen->symbol;
            use_method(chosen->symbol, me.name.span);
            if (chosen->decl->is_template) {   // a method template (spec 14): its instance
                Symbol* target = call_target(*chosen, c.span, t->symbol->name + "::" + name);
                if (!target) return set(c, tc_.error());
                me.symbol = target;
            }
            if (!member_visible(chosen->symbol)) {
                note(error(DiagCode::E4071, me.name.span,
                           std::format("'{}::{}' is private to module '{}'", t->symbol->name, name,
                                       chosen->symbol->parent->module->name),
                           "private"),
                     "declared here", chosen->symbol->span);
            }
            if (chosen->decl->is_move) check_consumed_receiver(*me.object, name, me.name.span);
            if (chosen->decl->is_mut) {
                Place p = place_of(*me.object);
                if (p.is_place && !p.is_mutable && !p.is_temporary) {
                    if (p.via_this) {
                        Diagnostic& d = error(DiagCode::E4021, c.span,
                                              std::format("'{}' is a mut method; it cannot be called on 'this' in a non-mut method", name),
                                              "'this' is immutable here");
                        note(d, std::format("add 'mut' after the parameter list: '{}(...) mut'", fn_.name));
                    } else {
                        Diagnostic& d = error(DiagCode::E4022, c.span,
                                              std::format("'{}' is a mut method, but '{}' is not mutable", name, p.name),
                                              "immutable receiver");
                        if (p.local) note(d, std::format("declare it 'mut {} {}'", types::to_string(type_of_symbol(p.local)),
                                                         p.local->name),
                                          p.local->span);
                    }
                }
            }
            check_compile_time_call(chosen->symbol, c.span);
            check_unsafe_call(chosen->symbol, c.span);
            set(c, chosen->result);
            run_entry_checks(chosen->symbol, c, args);
            return chosen->result;
        }

        infer(callee);
        for (Expr* a : args) {
            if (a->type && a->type->is_untyped()) settle(*a, tc_.error());
        }
        return set(c, tc_.error());
    }

    // --- other expressions ----------------------------------------------------------------

    const Type* infer_cast(CastExpr& c) {
        const Type* to   = lower(*c.target);
        const Type* from = types::strip_ref(infer(*c.operand));
        set(c, to);
        const std::string& v = c.variant;
        if (v == "reinterpret") {
            if (unsafe_depth_ == 0) {
                note(error(DiagCode::E4103, c.span, "cast::reinterpret requires 'unsafe'", "outside 'unsafe'"),
                     "wrap it in 'unsafe { ... }'");
            }
            if (from->is_untyped()) settle(*c.operand, to);
            return to;
        }
        if (v == "checked") {
            const Symbol* option = std_item("option");
            set(c, option ? tc_.instance(option, {to}) : tc_.external("std::option<" + types::to_string(to) + ">"));
        } else if (v == "dynamic") {   // base reference -> derived reference, checked at run time (spec 07, 17)
            const Symbol* option = std_item("option");
            set(c, option ? tc_.instance(option, {to}) : tc_.external("std::option<" + types::to_string(to) + ">"));
            const Type* target = types::strip_ref(to);
            if (from->is_unknown() || target->is_unknown()) return c.type;
            if (from->is(Kind::user) && from->symbol->kind == SymbolKind::interface) {   // interface → its implementer
                if (!to->is(Kind::reference) || !target->is(Kind::user) ||
                    (target->symbol->kind != SymbolKind::class_ && target->symbol->kind != SymbolKind::struct_) ||
                    !is_ancestor(target->symbol, from->symbol)) {
                    error(DiagCode::E4091, c.span, std::format("cast::dynamic cannot convert {} to {}", quoted(from), quoted(to)),
                          "the target does not implement it");
                }
                if (to->is_mut) require_mutable(place_of(*c.operand), *c.operand, "cast to a mutable reference");
                return c.type;
            }
            if (!to->is(Kind::reference) || !target->is(Kind::user) || !from->is(Kind::user) ||
                target->symbol->kind != SymbolKind::class_ || !is_ancestor(target->symbol, from->symbol)) {
                error(DiagCode::E4091, c.span,
                      std::format("cast::dynamic cannot convert {} to {}", quoted(from), quoted(to)), "not a downcast");
                return c.type;
            }
            const Symbol* root = from->symbol;
            while (root->base_class()) root = root->base_class();
            if (!root->is_open()) {
                error(DiagCode::E4091, c.span, std::format("cast::dynamic needs an open class hierarchy; {} is not one", quoted(from)),
                      "not polymorphic");
            }
            if (to->is_mut) require_mutable(place_of(*c.operand), *c.operand, "cast to a mutable reference");
            return c.type;
        } else if (!v.empty() && v != "truncate" && v != "saturate" && v != "round") {
            error(DiagCode::E4092, c.span, std::format("unknown cast 'cast::{}'", v), "unknown cast")
                ;
            return c.type;
        }
        if (from->is_untyped()) {
            if (to->is_numeric() && (from == tc_.untyped_float() ? true : to->is_integer())) {
                settle(*c.operand, from == tc_.untyped_float() ? tc_.builtin("f64") : to);
            } else {
                settle(*c.operand, from == tc_.untyped_float() ? tc_.builtin("f64") : tc_.builtin("i64"));
            }
            from = types::strip_ref(c.operand->type);
        }
        if (from->is_unknown() || to->is_unknown() || from->is(Kind::external) || to->is(Kind::external)) return c.type;
        auto invalid = [&](std::string_view why) {
            error(DiagCode::E4091, c.span,
                  std::format("{} cannot convert {} to {}", v.empty() ? "cast" : "cast::" + v, quoted(from), quoted(to)),
                  std::string(why));
        };
        if (v.empty()) {
            if (from->is_user(SymbolKind::enum_)) {
                const Type* tag = lower_owned(from->symbol, *from->symbol->decl->as<EnumDecl>().tag_type);
                if (enum_has_payload(from->symbol)) invalid("enums with payloads have no tag value");
                else if (tag != to && !lossless(tag, to)) invalid(std::format("the tag type is {}", quoted(tag)));
                return c.type;
            }
            if (from->is_numeric() || from->is(Kind::bool_)) {
                if (!to->is_numeric()) {
                    invalid("not a number type");
                } else if (!lossless(from, to)) {
                    Diagnostic& d = error(DiagCode::E4090, c.span,
                                          std::format("cast<{}> from {} may lose data", types::to_string(to), quoted(from)),
                                          "lossy conversion");
                    note(d, std::format("say how: 'cast::truncate<{0}>', 'cast::saturate<{0}>', 'cast::round<{0}>' or "
                                        "'cast::checked<{0}>'",
                                        types::to_string(to)));
                }
                return c.type;
            }
            if (from->is(Kind::char_)) {   // a scalar value is at most 0x10FFFF: 21 bits
                if (!to->is_integer() || (!to->is_size && to->bits < 32)) invalid("a char needs a 32-bit integer or wider");
                return c.type;
            }
            if ((from->is(Kind::reference) || from->is(Kind::user)) && assign_cost(from, to) >= 0) return c.type;
            invalid("cast<T> converts numbers, bools, chars and simple enums");
            return c.type;
        }
        if (v == "truncate" && !(from->is_integer() && to->is_integer())) invalid("truncate converts integers to integers");
        if (v == "saturate" && !(from->is_numeric() && to->is_integer())) invalid("saturate converts numbers to integers");
        if (v == "round" && !(from->is_numeric() && to->is_float())) invalid("round converts numbers to floats");
        if (v == "checked" && !(from->is_numeric() && to->is_numeric()) && !(from->is_integer() && to->is(Kind::char_))) {
            invalid("checked converts numbers to numbers, and integers to chars");
        }
        return c.type;
    }

    const Type* infer_bit(BitExpr& b) {
        if (b.type) {
            const Type* t = lower(*b.type);
            infer(*b.operand);
            if (!t->is_integer() && !t->is_unknown()) {
                error(DiagCode::E4013, b.type->span, std::format("bit<T> needs an integer type, not {}", quoted(t)),
                      "not an integer type");
                return set(b, tc_.error());
            }
            check_bit_index(*b.operand, t);
            return set(b, t);
        }
        infer(*b.operand);
        return set(b, tc_.untyped_int());
    }

    void check_has_default(const Type* t, Span span) {
        const Type* s = t;
        if (s->is(Kind::reference)) {
            error(DiagCode::E4140, span, "references have no default value", "no default");
            return;
        }
        if (!s->is(Kind::user)) return;
        const Symbol* sym = s->symbol;
        if (is_std_instance(s, "option")) return;   // std::none (spec 03)
        if (sym->kind == SymbolKind::enum_ || sym->kind == SymbolKind::interface) {
            error(DiagCode::E4140, span, std::format("{} '{}' has no default value", sema::to_string(sym->kind), sym->name),
                  "no default");
            return;
        }
        std::vector<std::string> missing;
        std::function<void(const Symbol*)> walk = [&](const Symbol* ts) {
            for (const Symbol* b : ts->bases) {
                if (b->kind == SymbolKind::class_) walk(b);
            }
            for (const Symbol* m : ts->member_order) {
                if (m->kind == SymbolKind::field && !m->is_static && !m->decl->as<VariableDecl>().init) {
                    missing.push_back("'" + m->name + "'");
                }
            }
        };
        walk(sym);
        if (missing.empty() || sym->has_opaque_base) return;
        std::string list;
        for (std::size_t i = 0; i < missing.size(); ++i) list += (i ? ", " : "") + missing[i];
        Diagnostic& d = error(DiagCode::E4140, span, std::format("'{}' has no default value", sym->name), "no default");
        note(d, std::format("{} {} no default", missing.size() == 1 ? "field" : "fields", list + (missing.size() == 1 ? " has" : " have")));
    }

    const Type* infer_struct_literal(StructLiteralExpr& sl) {
        const Type* t = lower(*sl.type);
        for (FieldInit& f : sl.fields) {
            infer(*f.value);
            if (!t->is(Kind::user)) {
                if (f.value->type && f.value->type->is_untyped() && f.value->type != tc_.untyped_default()) {
                    settle(*f.value, tc_.error());
                }
                continue;
            }
            bool    opaque = false;
            Symbol* field  = find_member(t->symbol, f.name, opaque);
            if (field && field->kind == SymbolKind::field) coerce(*f.value, lower_owned(field, *field->decl->as<VariableDecl>().type));
        }
        return set(sl, t);
    }

    const Type* infer_lambda(LambdaExpr& l) {
        FunctionState saved = fn_;
        fn_.result          = l.result ? lower(*l.result) : nullptr;
        fn_.deducing        = !l.result;
        fn_.deduced         = nullptr;
        fn_.in_function     = true;
        fn_.name            = "lambda";
        for (Capture& cap : l.captures) {
            if (cap.init) {
                const Type* t = infer(*cap.init);
                if (t->is_untyped()) {
                    no_context(*cap.init, "(an init-capture needs a typed value)");
                    t = tc_.error();
                }
                if (cap.symbol) set_local(cap.symbol, t);
            } else if (cap.symbol) {
                // Same type as the captured local; reference captures are references.
                const Type* outer = tc_.error();
                if (auto it = cap.captured ? local_types_.find(cap.captured) : local_types_.end(); it != local_types_.end()) {
                    outer = it->second;   // the local the resolver found
                } else {
                    for (const auto& [sym, ty] : local_types_) {
                        if (sym->name == cap.name && sym != cap.symbol && sym->span.begin < cap.span.begin) outer = ty;
                    }
                }
                bool ref = cap.kind == CaptureKind::ref || cap.kind == CaptureKind::mut_ref;
                set_local(cap.symbol, ref ? tc_.reference(types::strip_ref(outer), cap.kind == CaptureKind::mut_ref)
                                               : types::strip_ref(outer));
            }
        }
        std::vector<const Type*> params;
        for (Param& p : l.params) {
            params.push_back(lower(*p.type));
            if (p.symbol) set_local(p.symbol, params.back());
        }
        infer_block(*l.body);
        const Type* result = l.result ? fn_.result : fn_.deduced ? fn_.deduced : tc_.void_();
        fn_ = saved;
        const Type* t = tc_.closure(&l);
        tc_.set_closure_signature(t, std::move(params), result);
        return set(l, t);   // a type of its own (spec 18)
    }

    /// `f(args)` where `f` holds a lambda.
    const Type* infer_closure_call(CallExpr& c, const Type* closure, std::vector<Expr*>& args) {
        std::size_t n = closure->args.size() - 1;
        if (args.size() != n) {
            error(DiagCode::E4030, c.span, std::format("this lambda takes {} argument{}, but {} {} given", n, n == 1 ? "" : "s",
                                                       args.size(), args.size() == 1 ? "is" : "are"),
                  "wrong number of arguments");
        }
        for (std::size_t i = 0; i < args.size() && i < n; ++i) check_argument(*args[i], closure->args[i]);
        return set(c, closure->args.back());
    }

    const Type* unify_branches(std::vector<Expr*> branches) {
        const Type* result = nullptr;
        for (Expr* b : branches) {
            const Type* t = b->type;
            if (!t || t->is(Kind::never)) continue;
            if (!result) {
                result = t;
                continue;
            }
            const Type* rs = types::strip_ref(result);
            const Type* ts = types::strip_ref(t);
            if (rs == ts) continue;
            if (rs->is_unknown() || ts->is_unknown()) return tc_.error();
            if (rs->is(Kind::pending) && ts->is(Kind::pending) && rs->symbol == ts->symbol) {
                result = merge_pending(rs, ts);   // `if (c) { std::ok(1u32) } else { std::error(e) }`
                if (result->is(Kind::error)) return result;
                continue;
            }
            if (rs->is_untyped() && ts->is_untyped()) {
                if (rs != ts) return tc_.error();
                continue;
            }
            if (rs->is_untyped()) {
                result = ts;
                continue;
            }
            if (ts->is_untyped()) continue;
            return tc_.error();   // different types: reported where a value is needed
        }
        return result ? result : tc_.never();
    }

    /// `if compile_time (c)` (spec 15): the branch that is not taken is not checked. Nullopt when
    /// the condition depends on a template parameter (the generic check): then both are checked.
    std::optional<const Type*> infer_if_compile_time(IfExpr& i) {
        const Type* ct = types::strip_ref(infer(*i.condition));
        if (ct->is_untyped()) settle(*i.condition, tc_.bool_());
        else if (!ct->is(Kind::bool_) && !ct->is_unknown()) mismatch(i.condition->span, i.condition->type, tc_.bool_(), " (the condition)");
        std::shared_ptr<const ct::Value> v = evaluate(*i.condition, "the condition of 'if compile_time' must be known at compile time");
        if (!v || v->kind != ct::Value::Kind::boolean) {
            if (!v && !evaluator().unknown() && !ct->is_unknown()) {
                set(i, tc_.error());
                return tc_.error();   // reported
            }
            return std::nullopt;
        }
        if (v->boolean) {
            i.taken = 0;
            return set(i, i.else_branch ? infer_block(*i.then_block) : (infer_block(*i.then_block), tc_.void_()));
        }
        if (!i.else_branch) {
            i.taken = 2;
            return set(i, tc_.void_());
        }
        i.taken = 1;
        return set(i, infer(*i.else_branch));
    }

    /// Two pending values of the same template: the arguments either knows. Error on a conflict.
    const Type* merge_pending(const Type* a, const Type* b) {
        std::vector<const Type*> args = a->args;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (!args[i]) args[i] = b->args[i];
            else if (b->args[i] && b->args[i] != args[i]) return tc_.error();
        }
        for (const Type* x : args) {
            if (!x) return tc_.pending(a->symbol, args);
        }
        return tc_.instance(a->symbol, args);
    }

    const Type* infer_if(IfExpr& i) {
        if (i.is_compile_time) {
            if (std::optional<const Type*> t = infer_if_compile_time(i)) return *t;
        }
        check_condition(*i.condition, "if");
        infer_block(*i.then_block);
        if (!i.else_branch) return set(i, tc_.void_());
        infer(*i.else_branch);
        const Type* t = unify_branches({i.then_block.get(), i.else_branch.get()});
        // A value whose branches settle on one concrete type: settle untyped branches to it.
        if (!t->is_untyped() && !t->is_unknown() && !t->is(Kind::never)) {
            for (Expr* b : {static_cast<Expr*>(i.then_block.get()), i.else_branch.get()}) {
                if (b->type && b->type->is_untyped()) settle(*b, t);
            }
        }
        if (t->is(Kind::error) && branch_types_conflict({i.then_block.get(), i.else_branch.get()})) {
            branch_conflicts_.insert(&i);
        }
        return set(i, t);
    }

    bool branch_types_conflict(std::vector<Expr*> branches) {
        std::set<const Type*> seen;
        for (Expr* b : branches) {
            const Type* t = b->type ? types::strip_ref(b->type) : nullptr;
            if (!t || t->is(Kind::never) || t->is_unknown()) continue;
            seen.insert(t->is_untyped() ? t : t);
        }
        return seen.size() > 1;
    }

    /// `c[i]` on the std containers: sequences take a 'usize' index, maps their key type.
    /// Everything else is a library type the checker does not know (unknown, never an error).
    /// `operator[]` of a user type: the `mut` overload when `want_mut`, if there is one (spec 16).
    Symbol* index_operator(const Type* obj, bool want_mut) {
        auto it = obj->symbol->members.find("[]");
        if (it == obj->symbol->members.end()) return nullptr;
        Symbol* first = nullptr;
        for (Symbol* m : it->second) {
            const FunctionDecl* f = m->function();
            if (!f || f->params.size() != 1) continue;
            if (!first) first = m;
            if (f->is_mut == want_mut) return m;
        }
        return first;
    }

    const Type* infer_index(IndexExpr& i) {
        const Type* obj = types::strip_ref(infer(*i.object));
        const Type* idx = infer(*i.index);
        if (obj->is(Kind::user)) {
            Symbol* op = index_operator(obj, false);
            if (!op) {
                error(DiagCode::E4013, i.span, std::format("{} has no 'operator[]'", quoted(obj)), "cannot index");
                if (idx->is_untyped()) settle(*i.index, tc_.error());
                return tc_.error();
            }
            i.op_symbol = op;
            use_method(op, i.span);
            const FunctionDecl* f  = op->function();
            const Type*         pt = lower_owned(op, *f->params[0].type);
            if (idx->is_untyped() || operand_fits(idx, pt)) coerce(*i.index, pt);
            else if (!idx->is_unknown()) mismatch(i.index->span, idx, pt, " (the index)");
            return f->result ? lower_owned(op, *f->result) : tc_.void_();
        }
        const Type* key = nullptr;
        const Type* el  = tc_.error();
        if (obj->is(Kind::external)) {
            std::string_view                 base = obj->external_base();
            const std::vector<const Type*>* args = tc_.external_args(obj);
            if (args && !args->empty() && (base == "std::darray" || base == "std::array" || base == "std::span")) {
                key = tc_.builtin("usize");
                el  = (*args)[0];
            } else if (args && args->size() >= 2 && (base == "std::hash_map" || base == "std::tree_map")) {
                key = (*args)[0];
                el  = (*args)[1];
            }
        }
        if (!key) {
            if (idx->is_untyped()) settle(*i.index, tc_.error());
            return tc_.error();
        }
        if (idx->is_untyped()) {
            settle(*i.index, key);
        } else if (!idx->is_unknown() && !key->is_unknown() && types::strip_ref(idx) != key) {
            mismatch(i.index->span, idx, key, " (the index)");
        }
        return el;
    }

    const Type* infer_for(ForExpr& f) {
        if (f.is_compile_time) {
            if (infer_for_compile_time(f)) return set(f, tc_.void_());
        }
        const Type* it   = infer(*f.iterable);
        const Type* var  = lower(*f.type);
        const Type* sit  = types::strip_ref(it);
        if (f.symbol) set_local(f.symbol, var);
        if (sit->is(Kind::range)) {
            const Type* el = sit->inner;
            if (el->is_untyped()) {
                auto& r = f.iterable->as<RangeExpr>();
                settle(*r.lo, var);
                settle(*r.hi, var);
                f.iterable->type = tc_.range(types::strip_ref(var));
            } else if (!el->is_unknown() && el != types::strip_ref(var) && !var->is_unknown()) {
                mismatch(f.type->span, var, el, " (the range's element type)");
            }
            if (var->is(Kind::reference)) {
                error(DiagCode::E4010, f.type->span, "range elements are values, not references",
                      std::format("write '{}'", types::to_string(types::strip_ref(var))));
            }
        } else if (sit->is(Kind::user)) {
            infer_for_over(f, sit, var);
        } else if (!sit->is_unknown() && !sit->is(Kind::external)) {
            error(DiagCode::E4013, f.iterable->span, std::format("cannot iterate over {}", quoted(sit)), "not iterable");
        }
        infer_block(*f.body);
        return set(f, tc_.void_());
    }

    static bool is_bit_builtin(const std::string& n) {
        return n == "trailing_zeros" || n == "leading_zeros" || n == "count_ones" || n == "rotate_left" || n == "rotate_right";
    }

    /// `std::trailing_zeros(x)`, `leading_zeros`, `count_ones` (a u32: the width for 0) and
    /// `std::rotate_left(x, n)` / `rotate_right` (spec 16): single instructions on most CPUs.
    const Type* infer_bits(CallExpr& c, const std::string& name, const std::vector<Expr*>& args) {
        bool rotate = name.starts_with("rotate");
        if (args.size() != (rotate ? 2u : 1u)) {
            error(DiagCode::E4030, c.span, std::format("'std::{}' takes {}", name, rotate ? "an integer and a count" : "one integer"),
                  "wrong number of arguments");
            for (Expr* a : args) {
                if (a->type && a->type->is_untyped()) settle(*a, tc_.error());
            }
            return set(c, tc_.error());
        }
        const Type* t = types::strip_ref(args[0]->type);
        if (t->is_untyped()) {
            no_context(*args[0], std::format("(the argument of 'std::{}' is an untyped literal)", name));
            return set(c, tc_.error());
        }
        if (!t->is_integer() && !t->is_unknown()) {
            error(DiagCode::E4013, c.span, std::format("'std::{}' needs an integer, not {}", name, quoted(t)), "not an integer");
            return set(c, tc_.error());
        }
        if (rotate) {
            coerce(*args[1], tc_.builtin("u32"));
            return set(c, t);
        }
        return set(c, tc_.builtin("u32"));
    }

    static bool is_arith_builtin(const std::string& n) {
        return n == "checked_add" || n == "checked_sub" || n == "checked_mul" || n == "saturating_add" ||
               n == "saturating_sub" || n == "saturating_mul";
    }

    /// `std::checked_add(a, b)` (a `std::option`: `none` on overflow) and `std::saturating_add(a, b)`
    /// (clamped), and the same for sub and mul (spec 16). Both operands have one integer type.
    const Type* infer_arith(CallExpr& c, const std::string& name, const std::vector<Expr*>& args) {
        bool checked = name.starts_with("checked");
        if (args.size() != 2) {
            error(DiagCode::E4030, c.span, std::format("'std::{}' takes two integers", name), "wrong number of arguments");
            for (Expr* a : args) {
                if (a->type && a->type->is_untyped()) settle(*a, tc_.error());
            }
            return set(c, tc_.error());
        }
        const Type* l = types::strip_ref(args[0]->type);
        const Type* r = types::strip_ref(args[1]->type);
        if (l->is_untyped() && r->is_untyped()) {
            no_context(*args[0], std::format("(both arguments of 'std::{}' are untyped literals)", name));
            settle(*args[1], tc_.error());
            return set(c, tc_.error());
        }
        const Type* t = unify_operands(*args[0], *args[1], l, r, c.span, "the arguments");
        if (!t->is_integer() && !t->is_unknown()) {
            error(DiagCode::E4013, c.span, std::format("'std::{}' needs integers, not {}", name, quoted(t)), "not an integer");
            return set(c, tc_.error());
        }
        if (!checked || t->is_unknown()) return set(c, t);
        const Symbol* option = std_item("option");
        return set(c, option ? tc_.instance(option, {t}) : tc_.external("std::option<" + types::to_string(t) + ">"));
    }

    /// `text.split(',')`, `text.parse<u32>()`, ...: the methods of `std::str` are functions of
    /// the library, `str_split(std::str text, char separator)`, called with the text first.
    /// Null when the library has no such function.
    const Type* infer_str_method(CallExpr& c, MemberExpr& me, const std::string& name, const std::vector<Expr*>& args) {
        const Module* m = program_.std_module;
        if (!m) return nullptr;
        auto it = m->items.find("str_" + name);
        if (it == m->items.end()) return nullptr;
        std::vector<Expr*> all{me.object.get()};
        all.insert(all.end(), args.begin(), args.end());
        std::vector<Candidate> cands;
        for (Symbol* f : it->second) {
            if (f->kind == SymbolKind::function) cands.push_back(make_candidate(f, all, &me.name.segments.back()));
        }
        if (cands.empty()) return nullptr;
        std::string display = "std::str::" + name;
        const Candidate* chosen = resolve_overload(cands, all, c.span, display);
        if (!chosen) return set(c, tc_.error());
        Symbol* target = call_target(*chosen, c.span, display);
        if (!target) return set(c, tc_.error());
        me.symbol   = target;
        c.callee->type = nullptr;
        return set(c, chosen->result);
    }

    /// `operator*` of a user type (a smart pointer, spec 13 and 16): the `mut` one when `object` is a
    /// mutable place. Sets `op` and returns the target type, or null when there is none.
    const Type* smart_target(Expr& object, const Type* t, SymbolRef& op) {
        if (!t->is(Kind::user)) return nullptr;
        auto it = t->symbol->members.find("*");
        if (it == t->symbol->members.end()) return nullptr;
        Place p          = place_of(object);
        bool  want_mut   = p.is_place && (p.is_mutable || p.is_temporary);
        const Symbol* chosen = nullptr;
        for (const Symbol* m : it->second) {
            const FunctionDecl* f = m->function();
            if (!f || !f->params.empty() || !f->is_operator) continue;
            if (!chosen || f->is_mut == want_mut) chosen = m;
        }
        if (!chosen || !chosen->function()->result) return nullptr;
        op = const_cast<Symbol*>(chosen);
        use_method(chosen, object.span);
        return types::strip_ref(lower_owned(chosen, *chosen->function()->result));
    }

    /// `std::unique_ptr<circle>::make(...)`: the resolver names the template's member; the call
    /// uses the member of the instance the path spells. Null when the path names no instance.
    Symbol* instance_member(PathExpr& pe, Symbol* s) {
        if (!s || !s->parent || !s->parent->decl || !s->parent->decl->is_template || s->parent->template_of) return nullptr;
        auto& segs = pe.path.segments;
        if (segs.size() < 2 || !segs[segs.size() - 2].has_template_args) return nullptr;
        std::vector<const Type*> args;
        for (const TemplateArg& a : segs[segs.size() - 2].template_args) {
            if (!a.type) return nullptr;
            const Type* t = lower(*a.type);
            if (t->is_unknown() || t->is(Kind::type_param)) return nullptr;
            args.push_back(t);
        }
        const Type* inst = tc_.instance(s->parent, args);
        if (!inst->is(Kind::user)) return nullptr;
        auto it = inst->symbol->members.find(s->name);
        if (it == inst->symbol->members.end()) return nullptr;
        const FunctionDecl* f = s->function();
        auto sig = [](const FunctionDecl* fd) {
            std::string out;
            for (const Param& p : fd->params) out += ast::to_string(*p.type) + ",";
            return out + (fd->is_mut ? "mut" : "");
        };
        for (Symbol* m : it->second) {
            const FunctionDecl* mf = m->function();
            if (f && mf && sig(f) == sig(mf) && mf->is_static == f->is_static) return m;
        }
        return nullptr;
    }

    // --- template constraints (spec 14) -----------------------------------------------------

    /// The constraints of the template parameter `p`, lowered: interfaces, or std's
    /// `comparable`, `cloneable`, `formattable` (externals).
    std::vector<const Type*> constraints_of(const Symbol* p) {
        std::vector<const Type*> out;
        if (!p || !p->template_param) return out;
        for (const TypePtr& c : p->template_param->constraints) out.push_back(tc_.lower(*c, nullptr));
        return out;
    }

    // --- threads (spec 27): std::sendable and std::shareable ---------------------------------

    /// `unsafe std::sendable(cond)` in a base list: the condition is evaluated per instance.
    void check_promises(TypeDecl& d, DeclKind kind) {
        for (ThreadPromise& p : d.promises) {
            std::string name = ast::to_string(p.path);
            if (name != "std::sendable" && name != "std::shareable") {
                note(error(DiagCode::E4232, p.span, std::format("'unsafe' can only promise std::sendable or std::shareable, not '{}'", name),
                           "not a thread-safety interface"),
                     "write 'unsafe std::sendable' or 'unsafe std::shareable' (spec 27)");
                continue;
            }
            if (kind == DeclKind::interface) {
                error(DiagCode::E4232, p.span, "an interface can't promise thread safety; the types implementing it do", "in an interface");
                continue;
            }
            if (!p.condition) {
                p.value = true;
                continue;
            }
            infer(*p.condition);
            coerce(*p.condition, tc_.bool_());
            if (auto v = evaluate(*p.condition, "the condition of a thread-safety promise is evaluated at compile time");
                v && v->kind == ct::Value::Kind::boolean) {
                p.value = v->boolean;
            }
        }
    }

    /// Whether a value of type `t` may be moved to another thread (`shareable` false) or used
    /// through `T&` from several threads at once (`shareable` true), derived from its parts like
    /// copy-ness (spec 27). `why` says which part isn't, `where` points at it.
    bool thread_safe(const Type* t, bool shareable, std::string* why = nullptr, std::optional<Span>* where = nullptr, int depth = 0) {
        if (!t || depth > 40) return true;
        auto fail = [&](std::string w, std::optional<Span> at = std::nullopt) {
            if (why && why->empty()) *why = std::move(w);
            if (where && !*where && at) *where = at;
            return false;
        };
        switch (t->kind) {
        case Kind::int_: case Kind::float_: case Kind::bool_: case Kind::char_: case Kind::void_: case Kind::never:
        case Kind::nullptr_: case Kind::static_str: case Kind::fn_ptr: case Kind::range: case Kind::type_value:
        case Kind::value_arg: case Kind::meta:
            return true;
        case Kind::pointer:
            return fail(std::format("{} is a raw pointer: the compiler can't know who owns the memory or which thread may use it", quoted(t)));
        case Kind::reference: {
            bool ok = (t->is_mut && !shareable) ? thread_safe(t->inner, false, why, where, depth + 1)
                                                : thread_safe(t->inner, true, why, where, depth + 1);
            return ok;
        }
        case Kind::array: return thread_safe(t->inner, shareable, why, where, depth + 1);
        case Kind::function:
            return fail(std::format("{} can hold any lambda, so what it captures is unknown; pass the lambda itself (a template "
                                    "parameter) instead", quoted(t)));
        case Kind::closure: {
            const LambdaExpr* l = tc_.lambda_of(t);
            if (!l) return true;
            for (const Capture& c : l->captures) {
                const Type* ct = c.symbol ? type_of_symbol(c.symbol) : nullptr;
                if (!ct) continue;
                if ((c.kind == CaptureKind::ref || c.kind == CaptureKind::mut_ref) && c.captured && thread_local_refs_.contains(c.captured)) {
                    return fail(std::format("the lambda captures '{}', a reference to a thread_local: another thread would reach this "
                                            "thread's copy", c.name), c.span);
                }
                if (shareable && c.kind == CaptureKind::mut_ref) {
                    return fail(std::format("the lambda captures '{}' as 'mut &': calls from several threads at once would change "
                                            "it at the same time", c.name), c.span);
                }
                std::string inner;
                const Type* base = types::strip_ref(ct);
                bool        ok   = c.kind == CaptureKind::ref     ? thread_safe(base, true, &inner, nullptr, depth + 1)
                                 : c.kind == CaptureKind::mut_ref ? thread_safe(base, false, &inner, nullptr, depth + 1)
                                                                  : thread_safe(ct, shareable, &inner, nullptr, depth + 1);
                if (!ok) {
                    return fail(std::format("the lambda's capture '{}' has type {}{}", c.name, quoted(base), inner.empty() ? "" : ": " + inner), c.span);
                }
            }
            return true;
        }
        case Kind::external: {
            std::string_view base = t->external_base();
            if (base == "std::function") return fail(std::format("{} can hold any lambda", quoted(t)));
            return true;   // std::str, std::format_args: views of immutable text
        }
        case Kind::user: break;
        default: return true;   // unknown, type parameters: checked per instance
        }
        const Symbol* s = t->symbol;
        if (!s || !s->decl) return true;
        if (s->name == "thread_bound" && s->module && s->module->name == "std") {
            return fail("'std::thread_bound' keeps a type on the thread that made it");
        }
        auto key = std::make_pair(t, shareable);
        if (auto m = thread_memo_.find(key); m != thread_memo_.end() && m->second) return true;   // or in progress (a cycle)
        if (s->decl->kind != DeclKind::enum_) {
            for (const ThreadPromise& p : s->decl->as<TypeDecl>().promises) {
                if (p.shareable != shareable) continue;
                if (!p.value) return true;   // not known yet (a generic check)
                if (*p.value) return true;
                return fail(std::format("{} promises {} only under a condition that is false here", quoted(t),
                                        shareable ? "std::shareable" : "std::sendable"), p.span);
            }
        }
        thread_memo_[key] = true;
        bool ok = true;
        if (s->decl->kind == DeclKind::enum_) {
            for (const Symbol* a : s->member_order) {
                if (!a->alternative || !a->alternative->payload) continue;
                std::string inner;
                const Type* pt = lower_owned(a, *a->alternative->payload);
                if (!thread_safe(pt, shareable, &inner, nullptr, depth + 1)) {
                    ok = fail(std::format("{}'s alternative '{}' carries {}{}", quoted(t), a->name, quoted(pt), inner.empty() ? "" : ": " + inner),
                              a->span);
                    break;
                }
            }
        } else {
            for (const Symbol* f : s->member_order) {
                if (f->kind != SymbolKind::field || f->is_static) continue;
                std::string inner;
                const Type* ft = lower_owned(f, *f->decl->as<VariableDecl>().type);
                if (!thread_safe(ft, shareable, &inner, nullptr, depth + 1)) {
                    ok = fail(std::format("{} isn't {} because its field '{}' is a {}{}", quoted(t),
                                          shareable ? "std::shareable" : "std::sendable", f->name, quoted(ft),
                                          inner.empty() || ft->is(Kind::pointer) ? "" : " (" + inner + ")"),
                              f->span);
                    break;
                }
            }
            if (ok) {
                if (const Symbol* b = s->base_class()) ok = thread_safe(tc_.user(b), shareable, why, where, depth + 1);
            }
        }
        thread_memo_[key] = ok;
        return ok;
    }

    std::map<std::pair<const Type*, bool>, bool> thread_memo_;
    std::set<const Symbol*>                      thread_local_refs_;   // local references into a thread_local

    /// Whether `e` names a thread_local (or a part of one, or a reference to one).
    bool reaches_thread_local(const Expr& e) const {
        switch (e.kind) {
        case ExprKind::path: {
            const Symbol* s = e.as<PathExpr>().symbol;
            if (!s) return false;
            if (thread_local_refs_.contains(s)) return true;
            return s->kind == SymbolKind::global && s->decl && s->decl->kind == DeclKind::variable &&
                   s->decl->as<VariableDecl>().storage == Storage::thread_local_;
        }
        case ExprKind::member: return !e.as<MemberExpr>().arrow && reaches_thread_local(*e.as<MemberExpr>().object);
        case ExprKind::index: return reaches_thread_local(*e.as<IndexExpr>().object);
        case ExprKind::unary: {
            UnaryOp op = e.as<UnaryExpr>().op;
            return (op == UnaryOp::mut_marker || op == UnaryOp::move) && reaches_thread_local(*e.as<UnaryExpr>().operand);
        }
        default: return false;
        }
    }

    /// Whether values of `t` borrow (spec 12): references, views, lambdas with reference
    /// captures, containers of views. The same rule as the ownership checker's.
    bool view_type(const Type* t, int depth = 0) {
        if (!t || depth > 40) return false;
        switch (t->kind) {
        case Kind::reference: return true;
        case Kind::array: return view_type(t->inner, depth + 1);
        case Kind::closure: {
            const LambdaExpr* l = tc_.lambda_of(t);
            if (!l) return false;
            for (const Capture& c : l->captures) {
                if (c.kind == CaptureKind::ref || c.kind == CaptureKind::mut_ref) return true;
                if (c.symbol && view_type(type_of_symbol(c.symbol), depth + 1)) return true;
            }
            return false;
        }
        case Kind::external: {
            std::string_view base = t->external_base();
            return base == "std::str" || base == "std::span" || base == "std::format_args";
        }
        case Kind::user: {
            const Symbol* s = t->symbol;
            if (s->decl && (s->kind == SymbolKind::struct_ || s->kind == SymbolKind::class_)) {
                if (s->decl->as<TypeDecl>().is_view) return true;
                return std::any_of(s->template_args.begin(), s->template_args.end(), [&](const Type* a) { return view_type(a, depth + 1); });
            }
            if (s->kind == SymbolKind::enum_) {
                for (const Symbol* a : s->member_order) {
                    if (a->alternative && a->alternative->payload && view_type(lower_owned(a, *a->alternative->payload), depth + 1)) return true;
                }
            }
            return false;
        }
        default: return false;
        }
    }

    /// Whether `t` satisfies the constraint `c`; `why` says what is missing.
    bool satisfies(const Type* t, const Type* c, std::string& why) {
        if (c->is(Kind::external) && (c->name == "std::sendable" || c->name == "std::shareable")) {
            if (t->is_unknown()) return true;
            return thread_safe(t, c->name == "std::shareable", &why);
        }
        t = types::strip_ref(t);
        if (t->is_unknown() || c->is_unknown()) return true;
        if (c->is(Kind::user) && c->symbol->kind == SymbolKind::interface) {
            if (t->is(Kind::user) && (t->symbol == c->symbol || is_ancestor(t->symbol, c->symbol))) return true;
            why = std::format("{} does not implement {}", quoted(t), quoted(c));
            return false;
        }
        if (!c->is(Kind::external)) return true;
        std::string_view name = c->name;
        if (name == "std::comparable") {
            if (t->is_numeric() || t->is(Kind::char_) || t->is(Kind::static_str) || (t->is(Kind::external) && t->name == "std::str")) return true;
            if (t->is(Kind::user) && find_operator("<=>", t, t)) return true;
            why = std::format("{} has no 'operator<=>'", quoted(t));
            return false;
        }
        if (name == "std::cloneable") {
            if (!t->is(Kind::user) || t->symbol->kind != SymbolKind::class_ || t->symbol->members.count("clone")) return true;
            why = std::format("class {} has no 'clone()'", quoted(t));
            return false;
        }
        if (name == "std::formattable") {
            if (t->is_numeric() || t->is(Kind::bool_) || t->is(Kind::char_) || t->is(Kind::static_str) || is_std_string(t) ||
                (t->is(Kind::external) && t->name == "std::str") || (t->is_user(SymbolKind::enum_) && !enum_has_payload(t->symbol))) {
                return true;
            }
            why = std::format("{} cannot be formatted", quoted(t));
            return false;
        }
        return true;
    }

    /// Checks the arguments of a template against the constraints of its parameters (E4161).
    bool check_constraints(const std::vector<const Symbol*>& params, const std::vector<const Type*>& args, Span span,
                           std::string_view display) {
        bool ok = true;
        for (std::size_t i = 0; i < params.size() && i < args.size(); ++i) {
            for (const Type* c : constraints_of(params[i])) {
                if (c->is(Kind::external) && (c->name == "std::sendable" || c->name == "std::shareable")) {
                    bool                shareable = c->name == "std::shareable";
                    std::string         why;
                    std::optional<Span> where;
                    if (args[i]->is_unknown() || thread_safe(args[i], shareable, &why, &where)) continue;
                    bool lambda = args[i]->is(Kind::closure);
                    std::string what = lambda ? (shareable ? std::format("'{}' runs the lambda on several threads at once, but it can't be shared between them", display)
                                                           : std::format("'{}' moves the lambda to another thread, but it can't go there", display))
                                              : std::format("'{}' needs {} to be {}, but {} isn't", display, params[i]->name, quoted(c), quoted(args[i]));
                    Diagnostic& d = error(shareable ? DiagCode::E4231 : DiagCode::E4230, span, what,
                                          shareable ? "not std::shareable" : "not std::sendable");
                    note(d, why, where);
                    if (lambda && shareable) {
                        note(d, "count with a std::atomic, compute per-chunk results with reduce_chunks, or pass mutable data in "
                                "through the chunk (spec 28)");
                    }
                    ok = false;
                    continue;
                }
                std::string why;
                if (satisfies(args[i], c, why)) continue;
                Diagnostic& d = error(DiagCode::E4161, span, why, std::format("'{}' needs {} for '{}'", display, quoted(c), params[i]->name));
                note(d, "the constraint is here", params[i]->template_param->span);
                ok = false;
            }
        }
        return ok;
    }

    /// A method call on a value of a constrained template parameter: only the methods of the
    /// constraining interfaces exist (spec 14). Null when `t` is not constrained by an interface.
    const Type* infer_constrained_call(CallExpr& c, MemberExpr& me, const Type* t, const std::string& name,
                                       std::vector<Expr*>& args) {
        std::vector<const Type*> cs = constraints_of(t->symbol);
        std::vector<Symbol*> methods;
        bool any_interface = false;
        for (const Type* k : cs) {
            if (!k->is(Kind::user) || k->symbol->kind != SymbolKind::interface) continue;
            any_interface = true;
            for (Symbol* m : find_methods(k->symbol, name)) methods.push_back(m);
        }
        if (!any_interface) return nullptr;
        if (methods.empty()) {
            note(error(DiagCode::E4162, me.name.span,
                       std::format("'{}' is constrained to {}, which has no method '{}'", t->symbol->name, quoted(cs.front()), name),
                       "not in the constraints"),
                 "a constrained template may only use what its constraints provide");
            for (Expr* a : args) {
                if (a->type && a->type->is_untyped()) settle(*a, tc_.error());
            }
            return set(c, tc_.error());
        }
        std::vector<Candidate> cands;
        for (Symbol* m : methods) cands.push_back(make_candidate(m, args, nullptr));
        const Candidate* chosen = resolve_overload(cands, args, c.span, t->symbol->name + "::" + name);
        c.callee->type = nullptr;
        return set(c, chosen ? chosen->result : tc_.error());
    }

    /// A method of `t` named `name` that takes no arguments, or null.
    Symbol* nullary_method(const Type* t, const std::string& name) {
        for (Symbol* m : find_methods(t->symbol, name)) {
            if (m->function() && m->function()->params.empty() && !m->function()->is_template) return m;
        }
        return nullptr;
    }

    /// `for (T& x : c)` over a user type (spec 10): a container has `len()` and `operator[]`
    /// (the loop visits every index); a stream has `next() mut` returning a `std::option` (the
    /// loop runs until `none`).
    void infer_for_over(ForExpr& f, const Type* t, const Type* var) {
        bool want_mut = var->is(Kind::reference) && var->is_mut;
        Symbol* len   = nullary_method(t, "len");
        Symbol* at    = index_operator(t, want_mut);
        if (len && at) {
            f.len_symbol   = len;
            f.index_symbol = at;
            use_method(len, f.iterable->span);
            use_method(at, f.iterable->span);
            const FunctionDecl* af = at->function();
            const Type* el = af->result ? types::strip_ref(lower_owned(at, *af->result)) : tc_.error();
            if (!var->is_unknown() && !el->is_unknown() && types::strip_ref(var) != el) {
                mismatch(f.type->span, var, el, " (the container's elements)");
            }
            if (want_mut) {
                // `std::span<mut T>`: its `operator[]` gives `mut T&` without being `mut` itself (spec 20).
                bool gives_mut = af->result && af->result->kind == TypeKind::reference && af->result->as<ReferenceType>().is_mut;
                if (!af->is_mut && !gives_mut) {
                    error(DiagCode::E4022, f.type->span, std::format("{} has no 'mut operator[]' to change its elements", quoted(t)),
                          "cannot change elements");
                } else if (af->is_mut) {
                    require_mutable(place_of(*f.iterable), *f.iterable, "iterate mutably over");
                }
            }
            return;
        }
        Symbol* next = nullary_method(t, "next");
        const FunctionDecl* nf = next ? next->function() : nullptr;
        const Type* produced = nf && nf->result ? types::strip_ref(lower_owned(next, *nf->result)) : nullptr;
        if (produced && is_std_instance(produced, "option") && !produced->symbol->template_args.empty()) {
            f.next_symbol  = next;
            use_method(next, f.iterable->span);
            const Type* el = produced->symbol->template_args[0];
            if (var->is(Kind::reference)) {
                error(DiagCode::E4010, f.type->span, "a stream's elements are values, not references",
                      std::format("write '{}'", types::to_string(el)));
            } else if (!var->is_unknown() && !el->is_unknown() && var != el) {
                mismatch(f.type->span, var, el, " (the stream's elements)");
            }
            if (nf->is_mut) {
                Place p = place_of(*f.iterable);
                if (p.is_place && !p.is_temporary) require_mutable(p, *f.iterable, "read from");
            }
            return;
        }
        Diagnostic& d = error(DiagCode::E4013, f.iterable->span, std::format("cannot iterate over {}", quoted(t)), "not iterable");
        note(d, "a 'for' loop needs 'len()' and 'operator[]' (a container), or 'next()' returning a std::option (a stream)");
    }

    /// `for compile_time` (spec 26): the body is copied once per element and each copy is
    /// checked with the loop variable as a compile-time constant. False when the elements depend
    /// on a template parameter (the generic check): then the body is checked once as usual.
    bool infer_for_compile_time(ForExpr& f) {
        std::size_t errors = diags_.error_count();
        const Type* it     = infer(*f.iterable);
        if (diags_.error_count() != errors) return true;   // reported (e.g. std::meta::fields of a non-struct)
        const Type* var = lower(*f.type);
        if (f.iterable->kind == ExprKind::range) {
            auto& r = f.iterable->as<RangeExpr>();
            if (r.lo->type && r.lo->type->is_untyped()) settle(*r.lo, var);
            if (r.hi->type && r.hi->type->is_untyped()) settle(*r.hi, var);
            f.iterable->type = tc_.range(types::strip_ref(var));
        } else if (types::strip_ref(it)->is_unknown()) {
            if (f.symbol) unknown_values_.insert(f.symbol);
            return false;
        }
        std::optional<std::vector<ct::Value>> items = evaluator().elements(*f.iterable);
        if (!items) {
            if (evaluator().unknown()) {
                if (f.symbol) unknown_values_.insert(f.symbol);   // the generic check of a template
                return false;
            }
            return true;   // reported
        }
        for (const ct::Value& item : *items) {
            if (item.type && !var->is_unknown() && types::strip_ref(item.type) != types::strip_ref(var)) {
                mismatch(f.type->span, var, item.type, " (the loop's elements)");
                return true;
            }
        }
        Module* home = module_;
        f.unrolled   = true;
        for (const ct::Value& item : *items) {
            Cloner cloner([home](const Symbol& original) {
                Symbol* copy = home->make(original.kind, original.name, original.span);
                Module* owner = copy->module;
                *copy         = original;
                copy->module  = owner;
                copy->ct_value.reset();
                copy->type = nullptr;
                return copy;
            });
            Symbol* loop_var = home->make(SymbolKind::local, f.name, f.name_span);
            if (f.symbol) {
                Module* owner = loop_var->module;
                *loop_var     = *f.symbol;
                loop_var->module = owner;
                cloner.map(f.symbol, loop_var);
            }
            loop_var->ct_value = std::make_shared<const ct::Value>(item);
            set_local(loop_var, var);
            f.copies.push_back(cloner.block(*f.body));
            f.copy_symbols.push_back(loop_var);
            infer_block(*f.copies.back());
        }
        return true;
    }

    // --- match ----------------------------------------------------------------------------

    const Type* infer_match(MatchExpr& m) {
        const Type* scrut   = infer(*m.scrutinee);
        const Type* s       = types::strip_ref(scrut);
        bool        mut_ref = m.scrutinee->kind == ExprKind::unary && m.scrutinee->as<UnaryExpr>().op == UnaryOp::mut_marker;
        if (s->is_untyped()) {
            no_context(*m.scrutinee, "in this match subject");
            s = tc_.error();
        }
        std::vector<Expr*> bodies;
        for (MatchArm& arm : m.arms) {
            check_pattern(*arm.pattern, s, mut_ref);
            if (arm.guard) check_condition(*arm.guard, "match guard");
            infer(*arm.body);
            bodies.push_back(arm.body.get());
        }
        check_exhaustive(m, s);
        const Type* t = unify_branches(bodies);
        if (!t->is_untyped() && !t->is_unknown() && !t->is(Kind::never)) {
            for (Expr* b : bodies) {
                if (b->type && b->type->is_untyped()) settle(*b, t);
            }
        }
        if (t->is(Kind::error) && branch_types_conflict(bodies)) branch_conflicts_.insert(&m);
        return set(m, t);
    }

    /// The payload type of std::ok/error/some patterns, from the subject's template arguments.
    const Type* std_payload(const Type* s, const std::string& alt) {
        const std::vector<const Type*>* found = tc_.external_args(s);
        if (!found) return tc_.error();
        const auto& args = *found;
        std::string_view base = s->external_base();
        if (base == "std::result" && args.size() == 2) return alt == "ok" ? args[0] : alt == "error" ? args[1] : tc_.error();
        if (base == "std::option" && args.size() == 1 && alt == "some") return args[0];
        return tc_.error();
    }

    void check_pattern(Pattern& p, const Type* s, bool mut_ref) {
        switch (p.kind) {
        case PatternKind::wildcard:
        case PatternKind::error: return;
        case PatternKind::binding: {
            auto& b = p.as<BindingPattern>();
            if (b.symbol) {
                b.symbol->is_mut = mut_ref;
                set_local(b.symbol, s);
            }
            return;
        }
        case PatternKind::literal: {
            Expr& lit = *p.as<LiteralPattern>().literal;
            const Type* t = infer(lit);
            if (s->is_unknown() || s->is(Kind::external)) {
                if (t->is_untyped()) settle(lit, tc_.error());
                return;
            }
            if (t->is_untyped()) {
                settle(lit, s);
                return;
            }
            const Type* ts = types::strip_ref(t);
            if (ts != s && !(ts->is(Kind::static_str) && s->is(Kind::external))) {
                error(DiagCode::E4061, lit.span,
                      std::format("this pattern is {}, but the match subject is {}", quoted(ts), quoted(s)),
                      "wrong type");
            }
            return;
        }
        case PatternKind::range: {
            auto& r = p.as<RangePattern>();
            for (Expr* e : {r.lo.get(), r.hi.get()}) {
                const Type* t = infer(*e);
                if (s->is_unknown() || s->is(Kind::external)) continue;
                if (!s->is_integer() && !s->is(Kind::char_)) {
                    error(DiagCode::E4061, p.span, std::format("range patterns need an integer or char subject, not {}", quoted(s)),
                          "wrong type");
                    return;
                }
                if (t->is_untyped()) settle(*e, s);
                else if (types::strip_ref(t) != s) {
                    error(DiagCode::E4061, e->span, std::format("this bound is {}, but the subject is {}", quoted(t), quoted(s)),
                          "wrong type");
                }
            }
            return;
        }
        case PatternKind::enum_alternative: {
            auto& ep  = p.as<EnumPattern>();
            const Symbol* alt = ep.symbol;
            const Type*   payload = tc_.error();
            if (alt && alt->kind == SymbolKind::alternative) {
                ep.symbol      = instance_alternative(ep.symbol, s);
                alt            = ep.symbol;
                const Type* et = tc_.user(alt->parent);
                if (!s->is_unknown() && s != et) {
                    error(DiagCode::E4061, ep.path.span,
                          std::format("'{}' is an alternative of {}, but the match subject is {}", ast::to_string(ep.path),
                                      quoted(et), quoted(s)),
                          "wrong enum");
                }
                if (alt->alternative->payload) payload = lower_owned(alt, *alt->alternative->payload);
            } else if (!alt && s->is(Kind::external)) {
                payload = std_payload(s, ep.path.segments.back().name);
            }
            if (ep.payload) check_pattern(*ep.payload, payload, mut_ref);
            return;
        }
        case PatternKind::struct_: {
            auto& sp = p.as<StructPattern>();
            const Symbol* type = nullptr;
            auto it = module_->items.find(sp.path.segments.back().name);
            if (s->is(Kind::user)) type = s->symbol;
            for (FieldPattern& f : sp.fields) {
                const Type* ft = tc_.error();
                if (type) {
                    bool    opaque = false;
                    Symbol* field  = find_member(type, f.name, opaque);
                    if (field && field->kind == SymbolKind::field) ft = lower_owned(field, *field->decl->as<VariableDecl>().type);
                }
                check_pattern(*f.pattern, ft, mut_ref);
            }
            (void)it;
            return;
        }
        }
    }

    /// Matches every value: `_`, a binding, or a struct pattern made only of such patterns.
    static bool irrefutable(const Pattern& p) {
        if (p.kind == PatternKind::wildcard || p.kind == PatternKind::binding) return true;
        if (p.kind != PatternKind::struct_) return false;
        const auto& sp = p.as<StructPattern>();
        return std::all_of(sp.fields.begin(), sp.fields.end(),
                           [](const FieldPattern& f) { return !f.pattern || irrefutable(*f.pattern); });
    }

    /// Does this arm match every value on its own (no guard, `_` or a binding)?
    static bool is_catch_all(const MatchArm& a) {
        return !a.guard && (a.pattern->kind == PatternKind::wildcard || a.pattern->kind == PatternKind::binding);
    }

    /// Whether the patterns together match every value of `t` (spec 10): nested enum patterns
    /// count, so `std::error(io_error::full)` and `std::error(io_error::closed)` cover `std::error`.
    bool covers(const std::vector<const Pattern*>& pats, const Type* t) {
        for (const Pattern* p : pats) {
            if (irrefutable(*p)) return true;
        }
        t = types::strip_ref(t);
        if (t->is_user(SymbolKind::enum_)) {
            for (const Symbol* a : t->symbol->member_order) {
                if (a->alternative && !alternative_covered(pats, t->symbol, a->name)) return false;
            }
            return true;
        }
        if (t->is(Kind::bool_)) {
            bool yes = false, no = false;
            for (const Pattern* p : pats) {
                if (p->kind != PatternKind::literal) continue;
                const Expr& lit = *p->as<LiteralPattern>().literal;
                if (lit.kind == ExprKind::bool_literal) (lit.as<LiteralExpr>().bool_value ? yes : no) = true;
            }
            return yes && no;
        }
        return false;
    }

    /// Whether the patterns cover the alternative `name` of the enum `e`, payload included.
    bool alternative_covered(const std::vector<const Pattern*>& pats, const Symbol* e, const std::string& name) {
        std::vector<const Pattern*> payloads;
        for (const Pattern* p : pats) {
            if (irrefutable(*p)) return true;
            if (p->kind != PatternKind::enum_alternative) continue;
            const auto& ep = p->as<EnumPattern>();
            if (ep.path.segments.back().name != name) continue;
            if (!ep.payload || irrefutable(*ep.payload)) return true;
            payloads.push_back(ep.payload.get());
        }
        if (payloads.empty()) return false;
        for (const Symbol* a : e->member_order) {
            if (a->name == name && a->alternative && a->alternative->payload) {
                return covers(payloads, lower_owned(a, *a->alternative->payload));
            }
        }
        return false;
    }

    void check_exhaustive(MatchExpr& m, const Type* s) {
        // Unreachable arms after a catch-all.
        bool covered = false;
        for (MatchArm& arm : m.arms) {
            if (covered) {
                note(error(DiagCode::W4001, arm.pattern->span, "this arm can never match", "unreachable"),
                     "an earlier arm already matches every value");
                break;
            }
            covered = is_catch_all(arm);
        }
        if (covered || s->is_unknown()) return;

        std::vector<std::string> all;   // alternative names that must be covered
        std::string              prefix;
        if (s->is_user(SymbolKind::enum_)) {
            for (const Symbol* a : s->symbol->member_order) all.push_back(a->name);
            // option and result: their alternatives are written `std::ok` (spec 08).
            prefix = is_std_instance(s, "option") || is_std_instance(s, "result") ? "std::" : s->symbol->name + "::";
        } else if (s->is(Kind::bool_)) {
            all = {"true", "false"};
        } else if (s->is(Kind::external) && s->external_base() == "std::result") {
            all    = {"ok", "error"};
            prefix = "std::";
        } else if (s->is(Kind::external) && s->external_base() == "std::option") {
            all    = {"some", "none"};
            prefix = "std::";
        } else if (s->is(Kind::external)) {
            return;   // library types: unknown shape
        }

        std::set<std::string> seen;
        std::vector<const Pattern*> unguarded;
        for (const MatchArm& arm : m.arms) {
            if (!arm.guard) unguarded.push_back(arm.pattern.get());
        }
        for (const MatchArm& arm : m.arms) {
            if (arm.guard) continue;
            const Pattern& p = *arm.pattern;
            if (p.kind == PatternKind::enum_alternative) {
                const auto& ep = p.as<EnumPattern>();
                std::string name = ep.path.segments.back().name;
                if (s->is_user(SymbolKind::enum_) ? alternative_covered(unguarded, s->symbol, name)
                                                  : (!ep.payload || irrefutable(*ep.payload))) {
                    seen.insert(name);
                }
            } else if (p.kind == PatternKind::literal && s->is(Kind::bool_)) {
                const Expr& lit = *p.as<LiteralPattern>().literal;
                if (lit.kind == ExprKind::bool_literal) seen.insert(lit.as<LiteralExpr>().bool_value ? "true" : "false");
            } else if (p.kind == PatternKind::struct_) {
                if (irrefutable(p)) return;   // a struct pattern with only bindings matches everything
            }
        }
        if (all.empty()) {
            Diagnostic& d = error(DiagCode::E4060, m.span,
                                  std::format("match on {} does not cover every value", quoted(s)), "not exhaustive");
            note(d, "add a final '_ => ...,' arm");
            return;
        }
        std::vector<std::string> missing;
        for (const std::string& a : all) {
            if (!seen.count(a)) missing.push_back(prefix + a);
        }
        if (missing.empty()) return;
        std::string list;
        for (std::size_t i = 0; i < missing.size(); ++i) list += (i ? ", " : "") + missing[i];
        Diagnostic& d = error(DiagCode::E4060, m.span, std::format("match on {} does not cover {}", quoted(s), list),
                              "not exhaustive");
        note(d, "add the missing arms, or a final '_ => ...,' arm");
    }

    // --- state ----------------------------------------------------------------------------

    sema::Program&                                      program_;
    DiagnosticEngine&                                   diags_;
    types::TypeContext&                                 tc_;
    Module*                                             module_ = nullptr;
    FunctionState                                       fn_;
    int                                                 unsafe_depth_ = 0;
    bool                                                negated_literal_ = false;   // the next int literal sits under a unary minus
    std::map<const Symbol*, const Type*>                local_types_;
    std::set<const Expr*>                               branch_conflicts_;
    std::map<const Symbol*, const Type*>                subst_;   // template parameters of the instance being checked
    Instantiator*                                       instantiator_ = nullptr;   // makes function instances
    std::unique_ptr<ct::Evaluator>                      evaluator_;   // compile-time evaluation (spec 15, 26)
    std::set<const Symbol*>                             unknown_values_;
public:
    std::optional<Span>                                 instance_site_;   // where the instance being checked was written
    bool constraints_hold(const std::vector<const Symbol*>& params, const std::vector<const Type*>& args, Span span,
                          std::string_view display) {
        return check_constraints(params, args, span, display);
    }
private:   // compile-time values that depend on a template parameter

    // On-demand checking for compile-time code; defined after Instantiator, which keeps the state.
    bool begin_check(const Decl* d);
    void end_check(const Decl* d);
    bool ensure_function(const Symbol* s);
    std::shared_ptr<const ct::Value> ensure_constant(const Symbol* g);

    /// The instance of the function template `templ` for `args` (spec 14), defined after Instantiator.
    Symbol* instantiate_function(const Symbol* templ, const std::vector<const Type*>& args);
    Symbol* instantiate_method(const Symbol* templ, const std::vector<const Type*>& args);
};

/// Makes the instances of template types (spec 14). An instance is a copy of the template's
/// declaration, re-parsed from its source text so that it has an AST of its own; it is resolved
/// in the template's module and checked with the parameters replaced by the arguments. Instances
/// are created on demand, wherever a type `box<u32>` is lowered.
class Instantiator {
public:
    Instantiator(sema::Program& program, const SourceManager& sources, DiagnosticEngine& diags)
        : program_(program), sources_(sources), diags_(diags) {}

    const Type* instantiate(const Symbol* templ, const std::vector<const Type*>& args) {
        auto key = std::make_pair(templ, args);
        if (auto it = memo_.find(key); it != memo_.end()) return it->second;
        Decl* copy = copy_of(*templ);
        if (!copy) return program_.types->error();
        std::vector<Symbol*> params;
        Symbol*              inst = resolve_instance(program_, diags_, templ, *copy, params);
        if (params.size() != args.size()) return program_.types->error();
        inst->template_of    = templ;
        inst->instance_index = ++count_[templ];
        for (std::size_t i = 0; i < params.size(); ++i) inst->template_subst[params[i]] = args[i];
        inst->template_args = args;
        const Type* t = program_.types->user(inst);
        memo_[key]    = t;
        program_.instances.push_back(inst);
        program_.instance_copies.push_back(copy);
        Checker checker(program_, diags_, this);
        checker.instance_site_ = program_.types->instantiation_site();
        if (checker.instance_site_ && templ->decl) {   // constraints of a type template (spec 14), where it is written
            std::vector<const Symbol*> declared(templ->decl->template_param_symbols.begin(), templ->decl->template_param_symbols.end());
            checker.constraints_hold(declared, args, *checker.instance_site_, templ->name);
        }
        checker.check_instance(inst, *copy);
        return t;
    }

    /// The instance of the function template `templ` for `args`, made once per argument list.
    Symbol* instantiate_function(const Symbol* templ, const std::vector<const Type*>& args) {
        auto key = std::make_pair(templ, args);
        if (auto it = fn_memo_.find(key); it != fn_memo_.end()) return it->second;
        Decl* copy = copy_of(*templ);
        if (!copy || copy->kind != DeclKind::function) return nullptr;
        std::vector<Symbol*> params;
        Symbol*              inst = resolve_function_instance(program_, diags_, templ, copy->as<FunctionDecl>(), params);
        if (params.size() != args.size()) return nullptr;
        inst->template_of    = templ;
        inst->instance_index = ++count_[templ];
        for (std::size_t i = 0; i < params.size(); ++i) inst->template_subst[params[i]] = args[i];
        inst->template_args = args;
        fn_memo_[key] = inst;   // before checking the body, so that recursive calls find it
        program_.instances.push_back(inst);
        program_.instance_copies.push_back(copy);
        Checker(program_, diags_, this).check_instance(inst, *copy);
        return inst;
    }

    /// The instance of the method template `templ` for `args` (spec 14). The resolved method is
    /// copied (its body refers to the class's parameters, which the class instance substitutes);
    /// the copy's own parameters are replaced by `args`.
    Symbol* instantiate_method(const Symbol* templ, const std::vector<const Type*>& args) {
        auto key = std::make_pair(templ, args);
        if (auto it = fn_memo_.find(key); it != fn_memo_.end()) return it->second;
        const Decl* src = templ->definition ? templ->definition : templ->decl;
        if (!src || src->kind != DeclKind::function) return nullptr;
        const auto& f = src->as<FunctionDecl>();
        if (f.template_param_symbols.size() != args.size() || !f.body) return nullptr;
        Module* home = templ->module;
        ast::Cloner cloner([home](const Symbol& original) {
            Symbol* copy  = home->make(original.kind, original.name, original.span);
            Module* owner = copy->module;
            *copy         = original;
            copy->module  = owner;
            copy->type    = nullptr;
            copy->ct_value.reset();
            return copy;
        });
        std::unique_ptr<FunctionDecl> copy = cloner.function(f);
        Symbol* inst         = home->make(SymbolKind::method, f.name, f.name_span);
        inst->decl           = copy.get();
        inst->parent         = templ->parent;
        inst->access         = templ->access;
        inst->template_of    = templ;
        inst->instance_index = ++count_[templ];
        if (const auto* outer = sema::substitution_of(templ)) inst->template_subst = *outer;
        for (std::size_t i = 0; i < args.size(); ++i) inst->template_subst[f.template_param_symbols[i]] = args[i];
        inst->template_args = args;
        Decl* raw = copy.get();
        program_.cloned_decls.push_back(std::move(copy));
        fn_memo_[key] = inst;
        program_.instances.push_back(inst);
        program_.instance_copies.push_back(raw);
        Checker(program_, diags_, this).check_instance(inst, *raw);
        return inst;
    }

private:
    /// A fresh copy of the template's declaration, re-parsed from its source text, so that the
    /// instance has an AST of its own. It lives as long as the program: symbols refer to it.
    /// Null when the source does not hold exactly one declaration.
    Decl* copy_of(const Symbol& templ) {
        Decl* copy = reparse(templ.definition ? *templ.definition : *templ.decl);
        if (copy && templ.source_block) merge_source_block(*copy, reparse(*templ.source_block));
        return copy;
    }

    Decl* reparse(const Decl& src) {
        const SourceFile& file    = sources_.file(src.span.file);
        std::vector<Token> tokens = tokenize_range(file, src.span.file, diags_, src.span.begin, src.span.end);
        program_.instance_files.push_back(parse(file, src.span.file, tokens, diags_));
        ast::File& parsed = program_.instance_files.back();
        if (parsed.decls.size() != 1) return nullptr;
        return parsed.decls.front().get();
    }

    /// A class template split into a header and a source block (spec 09) becomes one declaration
    /// for the instance: the method bodies of the source go into the header's declarations, and
    /// the private members are added. The resolver checked already that the two match.
    static void merge_source_block(Decl& header, Decl* source) {
        if (!source || (header.kind != DeclKind::class_ && header.kind != DeclKind::struct_) || source->kind != header.kind) return;
        auto& into = header.as<TypeDecl>().members;
        auto  sig  = [](const FunctionDecl& f) {
            std::string s = f.name + "(";
            for (const Param& p : f.params) s += ast::to_string(*p.type) + ",";
            return s + (f.is_mut ? ") mut" : f.is_move ? ") move" : ")");
        };
        for (DeclPtr& mem : source->as<TypeDecl>().members) {
            if (mem->kind == DeclKind::function) {
                auto& fn    = mem->as<FunctionDecl>();
                auto  match = std::find_if(into.begin(), into.end(), [&](const DeclPtr& d) {
                    return d->kind == DeclKind::function && !d->as<FunctionDecl>().body && sig(d->as<FunctionDecl>()) == sig(fn);
                });
                if (match != into.end()) {
                    (*match)->as<FunctionDecl>().body = std::move(fn.body);
                    continue;
                }
            }
            into.push_back(std::move(mem));
        }
    }

public:
    LineCol line_col(Span span) const { return sources_.file(span.file).line_col(span.begin); }

    /// Which declarations are being checked or are done (functions and compile_time constants
    /// can be checked early, on demand, when compile-time code needs them).
    enum class State { checking, done };
    std::map<const Decl*, State> states;

private:
    sema::Program&                                                        program_;
    const SourceManager&                                                  sources_;
    DiagnosticEngine&                                                     diags_;
    std::map<std::pair<const Symbol*, std::vector<const Type*>>, const Type*> memo_;
    std::map<std::pair<const Symbol*, std::vector<const Type*>>, Symbol*>     fn_memo_;
    std::map<const Symbol*, unsigned>                                     count_;
};

LineCol Checker::sources_line(Span span) const { return instantiator_ ? instantiator_->line_col(span) : LineCol{}; }

bool Checker::begin_check(const Decl* d) {
    if (!instantiator_) return true;
    if (instantiator_->states.count(d)) return false;
    instantiator_->states[d] = Instantiator::State::checking;
    return true;
}

void Checker::end_check(const Decl* d) {
    if (instantiator_) instantiator_->states[d] = Instantiator::State::done;
}

bool Checker::ensure_function(const Symbol* s) {
    const Decl* d = s->definition ? s->definition : s->decl;
    if (!instantiator_ || !d) return false;
    auto it = instantiator_->states.find(d);
    if (it != instantiator_->states.end()) return it->second == Instantiator::State::done;
    Checker(program_, diags_, instantiator_).check_function_symbol(s);
    it = instantiator_->states.find(d);
    return it != instantiator_->states.end() && it->second == Instantiator::State::done;
}

std::shared_ptr<const ct::Value> Checker::ensure_constant(const Symbol* g) {
    if (g->ct_value || !instantiator_ || !g->decl) return g->ct_value;
    if (instantiator_->states.count(g->decl)) return g->ct_value;   // in progress (a cycle) or done without a value
    Checker(program_, diags_, instantiator_).check_global_symbol(g);
    return g->ct_value;
}

// Null only when the template's source did not parse (already reported) or no instantiator is set.
Symbol* Checker::instantiate_method(const Symbol* templ, const std::vector<const Type*>& args) {
    return instantiator_ ? instantiator_->instantiate_method(templ, args) : nullptr;
}

Symbol* Checker::instantiate_function(const Symbol* templ, const std::vector<const Type*>& args) {
    return instantiator_ ? instantiator_->instantiate_function(templ, args) : nullptr;
}

} // namespace

void check_types(sema::Program& program, const SourceManager& sources, DiagnosticEngine& diags) {
    if (!program.types) program.types = std::make_shared<types::TypeContext>();
    Instantiator instantiator(program, sources, diags);
    program.types->set_instantiator(
        [&](const Symbol* templ, const std::vector<const Type*>& args) { return instantiator.instantiate(templ, args); });
    // Template value arguments (`box<4>`, spec 14): checked and evaluated by a checker for the
    // parameters in scope where the type is written.
    program.types->set_value_arg_resolver([&](const Symbol* templ, std::size_t index, const Expr& value,
                                              const std::map<const Symbol*, const Type*>* subst) -> const Type* {
        Checker c(program, diags, &instantiator);
        if (!templ) return c.count_for(const_cast<Expr&>(value), subst);   // `std::raw::array<T, N>`
        return c.value_arg_for(templ, index, const_cast<Expr&>(value), subst);
    });
    // A compile_time `type` constant used as a type (spec 26): evaluated on demand while checking.
    program.types->set_type_value_resolver([&](const Symbol* s) -> const Type* {
        if (!s->ct_value && s->kind == SymbolKind::global && s->decl && !instantiator.states.count(s->decl)) {
            Checker(program, diags, &instantiator).check_global_symbol(s);
        }
        if (!s->ct_value || s->ct_value->kind != ct::Value::Kind::type) return nullptr;
        return s->ct_value->type_value;
    });
    Checker(program, diags, &instantiator).run();
    if (program.build_graph && !diags.has_errors()) Checker(program, diags, &instantiator).run_build();
    // Instances are only made while checking. Later phases see the ones that exist.
    program.types->set_instantiator(nullptr);
    program.types->set_type_value_resolver([](const Symbol* s) -> const Type* {
        if (!s->ct_value || s->ct_value->kind != ct::Value::Kind::type) return nullptr;
        return s->ct_value->type_value;
    });
}

} // namespace rift
