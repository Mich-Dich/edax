#include "edax/simd_intrinsics.hpp"
#include "edax/llvm_backend.hpp"
#include "edax/check.hpp"
#include "edax/ast_walk.hpp"

#include "edax/ast_clone.hpp"
#include "edax/ast_printer.hpp"
#include "edax/compile_time.hpp"
#include "edax/lexer.hpp"
#include "edax/parser.hpp"
#include "edax/resolve.hpp"
#include "edax/types.hpp"

#include <algorithm>
#include <utility>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <set>

namespace edax {

using namespace ast;
using sema::Module;
using sema::Symbol;
using sema::SymbolKind;
using types::Kind;
using types::Type;

namespace {

std::string quoted(const Type* t) { return "'" + types::to_string(t) + "'"; }

// cast<T> conversions that never lose information (spec 17).
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
        return to->bits == 16 ? bits <= 8 : to->bits == 32 ? bits <= 16 : bits <= 32;
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

// The binary operator a compound assignment applies: `+=` -> `+`.
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

// Does evaluating this expression/statement always leave the function (return, panic, an
// endless loop)? Used for "may end without returning" and for `never` block types.
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
        case StmtKind::destructure:
            if (diverges(*s->as<DestructureStmt>().init)) return true;
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
        if (i.is_compile_time && i.taken >= 0) {   // only the taken branch exists (spec 15)
            if (i.taken == 0) return diverges_block(*i.then_block);
            return i.else_branch && diverges(*i.else_branch);
        }
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
    bool                entry_failed = false;   // one of its own compile_time entry checks fails without arguments
    bool                entry_known  = false;   // entry_failed has been worked out
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

// Where an expression's value lives, for assignment and `mut` checks.
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

    // `annotated` in wrap blocks (annotations, section 8): the wrapped function. Also for the
    // wraps of template instances, which are made while checking.
    void bind_wrap_handles() {
        for (auto& [handle, fn] : program_.wrap_handles) {
            if (handle->ct_value) continue;
            ct::Value h;
            h.kind       = ct::Value::Kind::function;
            h.type       = tc_.meta_function();
            h.symbol     = fn;
            h.type_value = fn->parent && fn->parent->is_type() ? tc_.user(fn->parent) : nullptr;
            handle->ct_value = std::make_shared<const ct::Value>(std::move(h));
        }
    }

    void run() {
        setup_mode();
        bind_wrap_handles();
        for (auto& [name, m] : program_.modules) {
            module_ = m.get();
            if (m->header) check_file(m->header->ast);
            if (m->source) check_file(m->source->ast);
        }
        if (program_.entry) check_main(*program_.entry);
        make_versions();
        read_codegen_annotations();
        if (program_.is_plugin) find_plugin_entry();
    }

    // `@std::layout` (spec 34): the alignment is a power of two up to 64, written as a number;
    // with `.no_implicit_padding` every gap between fields is an error.
    void check_layout(Symbol* t) {
        if (!t->decl || (t->decl->is_template && !t->template_of)) return;
        for (const Symbol* f : t->member_order) {   // @std::never_zero (spec 34): integers and pointers
            if (f->kind != SymbolKind::field || !f->decl) continue;
            const AnnotationUse* nz = sema::find_annotation(f->decl->annotations, "std::never_zero");
            if (!nz) continue;
            const Type* ft = tc_.lower(*f->decl->as<VariableDecl>().type, sema::substitution_of(f));
            if (!ft->is_integer() && !ft->is(types::Kind::pointer)) {
                error(DiagCode::E4210, nz->span, std::format("@std::never_zero is for integer and pointer fields, not {}", quoted(ft)),
                      "not an integer or pointer");
            }
        }
        const AnnotationUse* u = sema::find_annotation(t->decl->annotations, "std::layout");
        if (!u) return;
        if (t->kind != SymbolKind::struct_ && t->kind != SymbolKind::class_) {
            error(DiagCode::E4210, u->span, "@std::layout is for structs and classes", "not a struct or class");
            return;
        }
        std::shared_ptr<const ct::Value> v = u->value_symbol ? ensure_constant(u->value_symbol) : nullptr;
        if (!v || v->elements.size() < 2) return;
        std::uint64_t align = static_cast<std::uint64_t>(v->elements[0].integer);
        if (align != 0) {
            if (types::requested_alignment(t) != align) {
                error(DiagCode::E4210, u->span, "write the alignment of @std::layout as a number: '.align = 16'", "not a number");
            } else if ((align & (align - 1)) != 0 || align > 64) {
                error(DiagCode::E4210, u->span, std::format("the alignment {} is not a power of two up to 64", align), "bad alignment");
            }
        }
        if (!v->elements[1].boolean) return;
        const Type*   self   = tc_.user(t);
        std::uint64_t offset = 0;
        const Symbol* prev   = nullptr;
        for (const Symbol* f : t->member_order) {
            if (f->kind != SymbolKind::field || f->is_static) continue;
            types::Layout l = tc_.layout(tc_.lower(*f->decl->as<VariableDecl>().type, sema::substitution_of(f)));
            std::uint64_t at = (offset + l.align - 1) / l.align * l.align;
            if (at != offset) {
                note(error(DiagCode::E4210, f->span,
                           std::format("{} bytes of padding before '{}' ({} has no implicit padding)", at - offset, f->name, quoted(self)),
                           "implicit padding"),
                     std::format("write the gap out: 'std::padding<{}> _pad_{};' {}", at - offset, f->name,
                                 prev ? std::format("after '{}'", prev->name) : std::string("first")));
            }
            offset = at + l.size;
            prev   = f;
        }
        types::Layout whole = tc_.layout(self);
        if (whole.size > offset && prev) {
            note(error(DiagCode::E4210, u->span, std::format("{} bytes of padding at the end of {}", whole.size - offset, quoted(self)),
                       "implicit padding"),
                 std::format("write it out: 'std::padding<{}> _pad_end;' after '{}'", whole.size - offset, prev->name));
        }
    }

    // `@std::optimize` and `@std::float_mode` (spec 34): what the backend needs from them, stored
    // on the function (for a template, on the template: every instance follows it).
    void read_codegen_annotations() {
        for (auto& [name, m] : program_.modules) {
            for (Symbol* item : m->item_order) {
                if (item->is_type()) check_layout(item);
            }
            std::vector<Symbol*> functions;
            for (Symbol* item : m->item_order) {
                if (item->kind == SymbolKind::function) functions.push_back(item);
                if (item->is_type()) {
                    for (Symbol* member : item->member_order) {
                        if (member->kind == SymbolKind::method) functions.push_back(member);
                    }
                }
            }
            for (Symbol* f : functions) {
                if (!f->decl) continue;
                const auto& uses = f->decl->annotations;
                if (const AnnotationUse* u = sema::find_annotation(uses, "std::optimize")) {
                    std::shared_ptr<const ct::Value> v = u->value_symbol ? ensure_constant(u->value_symbol) : nullptr;
                    if (v && !v->elements.empty() && v->elements[0].symbol) f->optimize_level = v->elements[0].symbol->name;
                }
                if (const AnnotationUse* u = sema::find_annotation(uses, "std::float_mode")) {
                    std::shared_ptr<const ct::Value> v = u->value_symbol ? ensure_constant(u->value_symbol) : nullptr;
                    static constexpr unsigned bits[] = {sema::float_mode_contract, sema::float_mode_reassociate, sema::float_mode_finite_only,
                                                        sema::float_mode_no_signed_zeros, sema::float_mode_reciprocal};
                    for (std::size_t i = 0; v && i < v->elements.size() && i < std::size(bits); ++i) {
                        if (v->elements[i].kind == ct::Value::Kind::boolean && v->elements[i].boolean) f->float_mode |= bits[i];
                    }
                    if (f->float_mode != 0 && program_.mode_settings["float_determinism"] == "strict") {
                        note(error(DiagCode::E4210, u->span, "@std::float_mode is not allowed in this build: the mode's float_determinism() is strict",
                                   "relaxes floating point"),
                             "a strict mode keeps float results identical on every machine (lockstep games, replays)");
                    }
                }
            }
        }
    }

    // A plugin (`g.add_plugin`): its one `@std::plugin_entry` class T, which implements an
    // interface I and has `static T make()`; its `create` is std::plugin_create<T, I>.
    void find_plugin_entry() {
        Module* m = program_.entry;
        if (!m) return;
        std::vector<Symbol*> entries;
        for (Symbol* item : m->item_order) {
            if (item->is_type() && item->decl && sema::find_annotation(item->decl->annotations, "std::plugin_entry")) entries.push_back(item);
        }
        Span where = m->source ? Span{m->source->id, 0, 0} : Span{};
        if (entries.size() != 1) {
            Diagnostic& d = error(DiagCode::E4210, entries.size() > 1 ? entries[1]->span : where,
                                  entries.empty() ? "a plugin needs a class marked '@std::plugin_entry'" : "a plugin has exactly one '@std::plugin_entry' class",
                                  entries.empty() ? "no entry class" : "a second entry class");
            note(d, "the loader makes the plugin's object with its 'static T make()'");
            return;
        }
        Symbol*       t     = entries[0];
        const Symbol* iface = nullptr;
        for (const Symbol* b : t->bases) {
            if (b->kind == SymbolKind::interface) iface = b;
        }
        if (!iface) {
            error(DiagCode::E4210, t->span, std::format("the plugin's entry class '{}' implements no interface", t->name),
                  "the program uses the plugin through an interface");
            return;
        }
        const Symbol* make = nullptr;
        if (auto it = t->members.find("make"); it != t->members.end()) {
            for (const Symbol* f : it->second) {
                const FunctionDecl* fd = f->function();
                if (fd && fd->is_static && fd->params.empty()) make = f;
            }
        }
        if (!make) {
            error(DiagCode::E4210, t->span, std::format("the plugin's entry class '{}' needs 'static {} make()'", t->name, t->name),
                  "no make()");
            return;
        }
        program_.plugin_class          = t;
        program_.plugin_interface      = iface;
        program_.plugin_make           = make;
        program_.plugin_interface_hash = ct::interface_hash(iface);
    }

    // `@std::simd::multiversion{ .levels = [...] }`: a copy of the function per level, checked
    // with that level's vector widths and CPU features, and one with the build's own level for
    // CPUs below all of them. The backend calls the best one for the running CPU.
    void make_versions() {
        std::vector<Symbol*> functions;
        for (auto& [name, m] : program_.modules) {
            for (Symbol* item : m->item_order) {
                if (item->kind == SymbolKind::function) functions.push_back(item);
                if (item->is_type()) {
                    for (Symbol* member : item->member_order) {
                        if (member->kind == SymbolKind::method) functions.push_back(member);
                    }
                }
            }
        }
        for (Symbol* f : functions) {
            const Decl* d = f->definition ? f->definition : f->decl;
            if (!d || d->kind != DeclKind::function || !f->decl) continue;
            const AnnotationUse* u = sema::find_annotation(f->decl->annotations, "std::simd::multiversion");
            if (!u) continue;
            const auto& fd = d->as<FunctionDecl>();
            if (fd.is_template || !fd.body || (f->parent && f->parent->decl && f->parent->decl->is_template)) {
                error(DiagCode::E4210, u->span, "@std::simd::multiversion works on functions that are not templates", "on a template");
                continue;
            }
            std::shared_ptr<const ct::Value> v = u->value_symbol ? ensure_constant(u->value_symbol) : nullptr;
            if (!v || v->elements.empty()) continue;
            std::vector<std::string> levels;
            for (const ct::Value& level : v->elements[0].elements) {
                if (level.symbol) levels.push_back(level.symbol->name);
            }
            std::vector<std::string> known = cpu_levels();
            for (const std::string& level : levels) {
                if (std::find(known.begin(), known.end(), level) == known.end()) {
                    std::string list;
                    for (const std::string& k : known) list += (list.empty() ? "" : ", ") + k;
                    error(DiagCode::E4210, u->span, std::format("'{}' is not a CPU level of the target (its levels: {})", level, list),
                          "not for this target");
                    levels.clear();
                    break;
                }
            }
            if (levels.empty()) continue;
            levels.push_back("");   // the build's own level, for CPUs below every listed one
            for (const std::string& level : levels) {
                Symbol* copy = make_version(f, fd, level);
                if (copy) f->versions.push_back(copy);
            }
        }
    }

    // A copy of `f` (declared by `fd`) checked for the CPU level `level`.
    Symbol* make_version(Symbol* f, const FunctionDecl& fd, const std::string& level) {
        Module* home = f->module;
        ast::Cloner cloner([home](const Symbol& original) {
            Symbol* copy  = home->make(original.kind, original.name, original.span);
            Module* owner = copy->module;
            *copy         = original;
            copy->module  = owner;
            copy->type    = nullptr;
            copy->ct_value.reset();
            return copy;
        });
        std::unique_ptr<FunctionDecl> copy = cloner.function(fd);
        Symbol* version     = home->make(f->kind, f->name, f->span);
        version->decl       = copy.get();
        version->parent     = f->parent;
        version->access     = f->access;
        version->ns         = f->ns;
        version->is_static  = f->is_static;
        version->cpu_level  = level;
        version->version_of = f;
        FunctionDecl& raw   = *copy;
        program_.cloned_decls.push_back(std::move(copy));
        std::string before = set_cpu_level_override(level);
        Module*     saved  = module_;
        module_            = home;
        check_function(raw, f->parent && f->parent->is_type() ? f->parent : nullptr);
        module_ = saved;
        set_cpu_level_override(before);
        return version;
    }

    // The checks of one annotation use (spec 29): run once for every place the annotation is
    // written, with `annotated` set to what it's written on. A failure is reported at the
    // annotation, with a note pointing into the check.
    void run_annotation_check(sema::AnnotationCheck& ac) {
        if (ac.done) return;
        ac.done = true;
        module_ = ac.origin;
        const Type* owner = ac.owner && ac.owner->is_type() ? tc_.user(ac.owner) : nullptr;
        if (ac.handle) {
            ct::Value h;
            h.type_value = owner;
            h.symbol     = ac.member;
            switch (ac.target) {
            case sema::AnnotationTarget::field: h.kind = ct::Value::Kind::field, h.type = tc_.meta_field(); break;
            case sema::AnnotationTarget::alternative: h.kind = ct::Value::Kind::alternative, h.type = tc_.meta_alternative(); break;
            case sema::AnnotationTarget::function: h.kind = ct::Value::Kind::function, h.type = tc_.meta_function(); break;
            case sema::AnnotationTarget::parameter:
                h.kind  = ct::Value::Kind::parameter;
                h.type  = tc_.meta_parameter();
                h.index = ac.param_index;
                break;
            case sema::AnnotationTarget::file: {   // a std::meta::file: the file's name and its module
                h.kind   = ct::Value::Kind::file;
                h.type   = tc_.meta_file();
                h.string = source_path(ac.use->span);
                for (const auto& [name, m] : program_.modules) {
                    if (m->source && m->source->id == ac.use->span.file) {
                        ct::Value mod;
                        mod.kind   = ct::Value::Kind::string;
                        mod.type   = tc_.static_str();
                        mod.string = name;
                        h.elements.push_back(std::move(mod));
                    }
                }
                break;
            }
            default: return;
            }
            ac.handle->ct_value = std::make_shared<const ct::Value>(std::move(h));
        }
        Check&      c      = ac.check->check;
        std::size_t errors = diags_.error_count();
        check_check(c, Where::type_body, ac.check->span);
        if (diags_.error_count() != errors || !c.is_compile_time || !c.compile_active) return;
        std::string            what = "@" + ast::to_string(ac.use->path);
        ct::Evaluator::Context ctx;
        ctx.site = ac.use->span;
        if (c.block) {
            evaluator().evaluate_in(*c.block, "", ctx);
            return;
        }
        std::shared_ptr<const ct::Value> v = evaluator().evaluate_in(*c.condition, "", ctx);
        if (!v || v->kind != ct::Value::Kind::boolean || v->boolean) return;
        ctx.quiet                          = true;
        std::shared_ptr<const ct::Value> m = evaluator().evaluate_in(*c.message, "", ctx);
        Diagnostic& d = error(DiagCode::E4210, ac.use->span, m && m->kind == ct::Value::Kind::string ? m->string : "check failed",
                              std::format("fails the check of '{}'", what));
        if (ac.origin->name != "std" && !ac.origin->name.starts_with("std::")) note(d, "the check is here", c.condition->span);
    }

    // build.ed (spec 25): runs `compile_time void build(mut std::build::graph& g)`, whose
    // calls of the graph's methods fill in program_.build_graph.
    void run_build() {
        Module* m = program_.entry;
        if (!m) return;
        // Everything in build.ed is declared in namespace build (build system).
        for (const Symbol* item : m->item_order) {
            if (item->ns == "build" || item->ns.starts_with("build::")) continue;
            note(error(DiagCode::E4216, item->span, std::format("'{}' is declared outside namespace build", item->name),
                       "global name in build.ed"),
                 "build.ed declares everything in 'namespace build { ... }'; every target imports it");
        }
        const Symbol* fn = m->item("build::build");
        const FunctionDecl* f = fn ? fn->function() : nullptr;
        const Symbol* graph = nullptr;
        if (auto bm = program_.modules.find("std::build"); bm != program_.modules.end()) {
            graph = bm->second->item("std::build::graph");
        }
        Span where = m->source ? Span{m->source->id, 0, 0} : Span{};
        if (!f || !f->is_compile_time || f->params.size() != 1 || !graph) {
            note(error(DiagCode::E4216, fn ? fn->span : where, "build.ed defines no 'build' function", "no build function"),
                 "write 'namespace build { compile_time void build(mut std::build::graph& g) { ... } }'");
            return;
        }
        ct::Value g;
        g.kind = ct::Value::Kind::aggregate;
        g.type = tc_.user(graph);
        evaluator().call(fn, {g}, fn->span);
    }

    // `if compile_time (c) { checks }` as a statement of a function's entry, or null.
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

    // The class of the build mode and the values of its settings (spec 24): the interpreter
    // runs the mode's methods once. A mode that removes guards safe code relies on must also
    // return true from `allow_undefined_behavior()` (E4215).
    void setup_mode() {
        if (!program_.build_mode_class) {
            for (const char* module : {"std::build::mode", "build"}) {
                auto m = program_.modules.find(module);
                if (m == program_.modules.end()) continue;
                std::string   key = std::string(module) == "build"
                                        ? (program_.build_mode.starts_with("build::") ? program_.build_mode : "build::" + program_.build_mode)
                                        : "std::build::mode::" + program_.build_mode;
                const Symbol* cls = m->second->item(key);
                if (cls && cls->kind == SymbolKind::class_) {
                    program_.build_mode_class = cls;
                    break;
                }
            }
        }
        if (!program_.build_mode_class) {   // an unknown mode (the driver says so): checked as debug
            if (auto m = program_.modules.find("std::build::mode"); m != program_.modules.end()) {
                program_.build_mode_class = m->second->item("std::build::mode::debug");
            }
        }
        const Symbol* mode = program_.build_mode_class;
        if (!mode) return;
        for (const char* name : {"optimization", "debug_info", "overflow", "shifts", "bounds", "run_time_checks", "panic",
                                 "link_time_optimization", "allow_undefined_behavior", "float_determinism", "sanitize", "std_optimization"}) {
            std::shared_ptr<const ct::Value> v = evaluator().mode_setting(mode, name);
            if (!v) continue;
            program_.mode_settings[name] = v->kind == ct::Value::Kind::boolean ? (v->boolean ? "true" : "false")
                                         : v->symbol                          ? v->symbol->name
                                                                              : ct::to_string(*v);
        }
        // profile_guided(): `std::some(path)` or `std::none` (spec 24); the path, or "".
        if (std::shared_ptr<const ct::Value> v = evaluator().mode_setting(mode, "profile_guided")) {
            const ct::Value* payload = v->symbol && v->symbol->name == "some" && !v->elements.empty() ? &v->elements[0] : nullptr;
            program_.mode_settings["profile_guided"] = payload && payload->kind == ct::Value::Kind::string ? payload->string : "";
        }
        auto& ms = program_.mode_settings;
        bool unsafe_mode = ms["bounds"] == "unchecked" || (ms["run_time_checks"] != "all" && !ms["run_time_checks"].empty());
        if (unsafe_mode && ms["allow_undefined_behavior"] != "true") {
            Diagnostic& d = error(DiagCode::E4215, mode->span,
                                  std::format("the mode '{}' turns off {} but does not allow undefined behaviour", mode->name,
                                              ms["bounds"] == "unchecked" ? "bounds checks" : "run_time checks"),
                                  "unsafe mode");
            note(d, "override 'compile_time bool allow_undefined_behavior() override { return true; }' to say so");
        }
    }

    // A template value argument written in a type (`box<N + 1>`), with `subst` for the
    // template parameters in scope there.
    const Type* value_arg_for(const Symbol* templ, std::size_t index, Expr& value,
                              const std::map<const Symbol*, const Type*>* subst) {
        if (subst) subst_ = *subst;
        for (const auto& [p, t] : subst_) {
            if (t->is_unknown()) return tc_.error();   // the generic check of a template
        }
        if (templ->module) module_ = templ->module;
        return evaluate_value_arg(templ, index, value);
    }

    // The count of a `std::raw::array<T, N>`: a compile-time usize.
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

    // Checks one function on demand (a compile_time function that compile-time code calls
    // before the normal pass reaches it).
    void check_function_symbol(const Symbol* s) {
        const Decl* d = s->definition ? s->definition : s->decl;
        if (!d || d->kind != DeclKind::function) return;
        module_ = s->module;
        if (const auto* subst = sema::substitution_of(s)) subst_ = *subst;
        Symbol* self = s->parent && s->parent->is_type() ? s->parent : nullptr;
        check_function(const_cast<FunctionDecl&>(d->as<FunctionDecl>()), self);
    }

    // Checks (and evaluates) a global compile_time constant on demand.
    void check_global_symbol(const Symbol* g) {
        if (!g->decl) return;
        module_        = g->module;
        global_symbol_ = g;   // e.g. the constant holding an annotation's value (spec 29): not a module item
        check_decl(const_cast<Decl&>(*g->decl), nullptr);
    }

    // An associated type `type element = u32;` (spec 34), checked when it is first used as a type.
    void check_member_type(const Symbol* m) {
        module_        = m->parent->module;
        subst_         = m->parent->template_subst;
        global_symbol_ = m;
        check_decl(const_cast<Decl&>(*m->decl), const_cast<Symbol*>(m->parent));
    }

    // Checks the members of a template instance (spec 14): `copy` is the instance's own copy of
    // the template's declaration, checked with the parameters replaced by their arguments.
    void check_instance(Symbol* inst, Decl& copy) {
        bind_wrap_handles();
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

    // A method of a template instance is used (called, or an operator): its deferred errors
    // are reported now, at the use.
    void use_method(const Symbol* s, Span at) {
        if (!s || !s->decl) return;
        program_.used_instance_methods.insert(s->decl);
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

    // A type written in the code being checked: template parameters are those of the instance
    // being checked (`subst_`), if any.
    const Type* lower(const TypeExpr& t, const std::map<const Symbol*, const Type*>* subst = nullptr) {
        const TypeExpr* named = t.kind == TypeKind::reference ? t.as<ReferenceType>().inner.get() : &t;
        if (named->kind == TypeKind::named && named->as<NamedType>().symbol) deprecated_use(named->as<NamedType>().symbol, named->span);
        return tc_.lower(t, subst ? subst : &subst_);
    }

    // --- the standard library's annotations (spec 29) --------------------------------------

    // The types an annotation's fields may have (spec 29, rule 1).
    bool annotation_value_type(const Type* t, std::set<const Symbol*>& seen) {
        if (t->is_numeric() || t->is(Kind::bool_) || t->is(Kind::char_) || t->is(Kind::static_str) || t->is(Kind::type_value)) return true;
        if (t->is(Kind::external) && t->external_base() == "std::span") {
            const auto* args = tc_.external_args(t);
            return args && args->size() == 1 && annotation_value_type((*args)[0], seen);
        }
        if (!t->is(Kind::user)) return false;
        if (is_std_instance(t, "span") && t->symbol->template_args.size() == 1) return annotation_value_type(t->symbol->template_args[0], seen);
        if (t->symbol->kind == SymbolKind::enum_) return !enum_has_payload(t->symbol);
        if (t->symbol->kind != SymbolKind::struct_) return false;
        if (!seen.insert(t->symbol).second) return true;
        for (const Symbol* f : t->symbol->member_order) {
            if (f->kind == SymbolKind::field && !f->is_static && !annotation_value_type(lower_owned(f, *f->decl->as<VariableDecl>().type), seen)) {
                return false;
            }
        }
        return true;
    }

    // The annotations written on what `s` declares (an instance: on its template).
    static const std::vector<AnnotationUse>* annotations_of(const Symbol* s) {
        if (!s) return nullptr;
        if (s->kind == SymbolKind::alternative) return s->alternative ? &s->alternative->annotations : nullptr;
        if (s->template_of && s->decl && s->decl->annotations.empty()) s = s->template_of;
        return s->decl ? &s->decl->annotations : nullptr;
    }

    // A string field of an annotation's value (`message` of std::deprecated), or "".
    std::string annotation_text(const AnnotationUse& u, std::string_view field) {
        std::shared_ptr<const ct::Value> v = u.value_symbol ? ensure_constant(u.value_symbol) : nullptr;
        if (!v || v->kind != ct::Value::Kind::aggregate || !u.symbol) return {};
        std::size_t i = 0;
        for (const Symbol* f : u.symbol->member_order) {
            if (f->kind != SymbolKind::field || f->is_static) continue;
            if (f->name == field) return i < v->elements.size() ? v->elements[i].string : std::string();
            ++i;
        }
        return {};
    }

    // `@std::deprecated`: a warning at every use of what it's written on (id std::deprecated),
    // except inside its own declaration.
    void deprecated_use(const Symbol* s, Span at) {
        const std::vector<AnnotationUse>* uses = annotations_of(s);
        const AnnotationUse*              u    = uses ? sema::find_annotation(*uses, "std::deprecated") : nullptr;
        if (!u || fn_.generic) return;
        const Symbol* own = s->kind == SymbolKind::method || s->kind == SymbolKind::field || s->kind == SymbolKind::alternative ? s->parent : s;
        if (fn_.self && (fn_.self == own || fn_.self->template_of == own)) return;
        if (fn_.decl && fn_.decl == s->decl) return;
        if (!program_.reported_uses.insert({at.file, at.begin, s}).second) return;
        const std::string id = "std::deprecated";
        if (std::find(program_.allowed_warnings.begin(), program_.allowed_warnings.end(), id) != program_.allowed_warnings.end()) return;
        auto level = program_.warn_levels.find(id);
        if (level != program_.warn_levels.end() && level->second == "off") return;
        bool        as_error = level != program_.warn_levels.end() && level->second == "error";
        std::string message  = annotation_text(*u, "message");
        Diagnostic& d = diags_.report(as_error ? DiagCode::E4210 : DiagCode::W4210, at,
                                      std::format("'{}' is deprecated{}{}", s->name, message.empty() ? "" : ": ", message),
                                      as_error ? "warning made an error" : "deprecated");
        note(d, "warning id: std::deprecated");
        note(d, "marked here", u->span);
    }

    // The function or method a checked call calls, or null.
    static const Symbol* called_symbol(const CallExpr& c) {
        if (c.callee->kind == ExprKind::path) return c.callee->as<PathExpr>().symbol;
        if (c.callee->kind == ExprKind::member) return c.callee->as<MemberExpr>().symbol;
        return nullptr;
    }

    // The warning ids `@std::allow{ .id = ... }` names on a function (spec 29).
    static std::vector<std::string> allowed_ids(const FunctionDecl& fn) {
        std::vector<std::string> out;
        for (const AnnotationUse& u : fn.annotations) {
            if (!u.symbol || u.symbol->key() != "std::allow" || !u.value_symbol || !u.value_symbol->decl) continue;
            const Expr* init = u.value_symbol->decl->as<VariableDecl>().init.get();
            if (!init || init->kind != ExprKind::struct_literal) continue;
            for (const FieldInit& f : init->as<StructLiteralExpr>().fields) {
                if (f.name != "id" || f.value->kind != ExprKind::path) continue;
                const Symbol* id = f.value->as<PathExpr>().symbol;
                out.push_back(id ? id->key() : ast::to_string(f.value->as<PathExpr>().path));
            }
        }
        return out;
    }

    // The parameter substitution of the template instance that declares `s` (a member, payload
    // or alternative), or null outside instances.
    static const std::map<const Symbol*, const Type*>* substitution_of(const Symbol* s) {
        for (; s; s = s->parent) {
            if (!s->template_subst.empty()) return &s->template_subst;
        }
        return nullptr;
    }

    // The alternative that `alt` stands for in the enum type `enum_type`: an instance of a template
    // enum has its own alternatives (spec 14). `alt` itself otherwise.
    static Symbol* instance_alternative(Symbol* alt, const Type* enum_type) {
        const Symbol* templ = alt->parent;
        if (!templ || !templ->decl || !templ->decl->is_template || templ->template_of) return alt;
        if (!enum_type || !enum_type->is(Kind::user) || enum_type->symbol->template_of != templ) return alt;
        auto it = enum_type->symbol->members.find(alt->name);
        return it == enum_type->symbol->members.end() ? alt : it->second.front();
    }

    // The enum type that an alternative path names: `maybe<u32>::some` names `maybe<u32>`
    // (spec 14). A plain enum's alternative names the enum.
    const Type* enum_type_of(const Symbol* alt, const Path& path) {
        const Symbol* templ = alt->parent;
        if (!templ || !templ->decl || !templ->decl->is_template || templ->template_of || path.segments.size() < 2) {
            return tc_.user(templ);
        }
        const PathSegment& seg = path.segments[path.segments.size() - 2];
        std::vector<const Type*> args;
        for (const TemplateArg& a : seg.template_args) args.push_back(a.type ? lower(*a.type) : tc_.error());
        for (std::size_t i = args.size(); i < templ->decl->template_params.size(); ++i) {
            const TemplateParam& p = templ->decl->template_params[i];
            if (!p.default_type) break;
            args.push_back(lower(*p.default_type));
        }
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

    // A type written in the declaration of `owner` (a field, payload, method signature, ...),
    // which may belong to a template instance other than the code being checked (spec 14).
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

    // 0 = exact, 1 = upcast, -1 = not assignable. `from` is the value's type, `to` the target.
    int assign_cost(const Type* from, const Type* to) {
        if (!from || !to) return 0;
        bool to_ref = to->is(Kind::reference) || to->is(Kind::pointer);
        const Type* f = types::strip_ref(from);
        const Type* t = types::strip_ref(to);
        if (f->is_unknown() || t->is_unknown()) return 0;
        if (f == t || f->is(Kind::never)) return 0;
        if (f->is(Kind::external) && t->is(Kind::external)) {
            // An f-string is a std::format_args: it doesn't become text without std::format (spec 19).
            if (f->name == "std::format_args" && t->name != "std::format_args") return -1;
            return 0;
        }
        if (f->is(Kind::static_str) && t->is(Kind::external)) return 0;
        if (is_std_text(f) && t->is(Kind::external) && t->name == "std::str" && !to_ref) return 0;   // a borrow (spec 19)
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

    // Whether `t` is text that converts to `std::str` and formats like it: `std::string` or a
    // `std::string_with<A>` (allocators).
    bool is_std_text(const Type* t) const { return is_std_string(t) || types::is_std_instance(t, "string_with"); }

    // Whether `t` is `std::string`: the library's class, or the name before std is imported.
    bool is_std_string(const Type* t) const {
        if (!t) return false;
        if (t->is(Kind::user)) return t->symbol == std_item("string");
        return t->is(Kind::external) && t->name == "std::string";
    }

    // Spec 34: an inline function holds captures of at most its size; a bound needs the lambda to
    // meet it (`std::function<void(), std::sendable>` takes only lambdas whose captures are sendable).
    void check_callable_fits(const Expr& e, const Type* closure, const Type* t) {
        if (t->bits > 0) {
            types::Layout l = tc_.closure_layout(closure);
            if (l.size > static_cast<std::uint64_t>(t->bits)) {
                note(error(DiagCode::E4010, e.span,
                           std::format("this lambda's captures take {} bytes, but {} holds {}", l.size, quoted(t), t->bits),
                           "too big to store inline"),
                     std::format("make the inline size at least {} (a multiple of 8), capture less, or use std::function (on the heap)",
                                 (l.size + 7) / 8 * 8));
            } else if (l.align > 8) {
                error(DiagCode::E4010, e.span, std::format("this lambda's captures need {}-byte alignment; an inline function gives 8", l.align),
                      "over-aligned captures");
            }
        }
        for (bool shareable : {false, true}) {
            if (shareable ? !t->is_mut : !t->is_signed) continue;
            std::string why;
            if (!thread_safe(closure, shareable, &why, nullptr, 0)) {
                note(error(DiagCode::E4010, e.span,
                           std::format("this lambda is not {}, but {} needs one that is", shareable ? "std::shareable" : "std::sendable", quoted(t)),
                           "not thread-safe"),
                     why);
            }
        }
    }

    // A lambda converts to `std::function<R(A...)>` with the same signature, and to
    // `std::fn_ptr<R(A...)>` when it captures nothing (spec 18).
    bool lambda_fits(const Type* f, const Type* t) const {
        if (!f || !t || !f->is(Kind::closure) || !(t->is(Kind::function) || t->is(Kind::fn_ptr))) return false;
        if (f->args != t->args) return false;
        if (t->is(Kind::fn_ptr)) {
            const ast::LambdaExpr* l = tc_.lambda_of(f);
            if (!l || !l->captures.empty()) return false;
        }
        if (has_mut_captures(f)) return false;   // its call changes it: std::function calls aren't `mut`
        return true;
    }

    // A lambda with `[mut ...]` captures: calling it changes it (spec 34).
    bool has_mut_captures(const Type* closure) const {
        const ast::LambdaExpr* l = closure && closure->is(Kind::closure) ? tc_.lambda_of(closure) : nullptr;
        if (!l) return false;
        for (const ast::Capture& c : l->captures) {
            if (c.is_mut) return true;
        }
        return false;
    }

    // `std::unique_ptr<circle>` → `std::unique_ptr<shape>` (and shared_ptr, weak_ptr): an
    // implicit upcast, like `circle&` → `shape&` (spec 07, 13).
    bool smart_upcast(const Type* f, const Type* t) const {
        if (!f || !t || !f->is(Kind::user) || !t->is(Kind::user) || f == t) return false;
        for (const char* name : {"unique_ptr", "shared_ptr", "weak_ptr"}) {
            const Symbol* templ = std_item(name);
            if (!templ || f->symbol->template_of != templ || t->symbol->template_of != templ) continue;
            const auto& fa_all = f->symbol->template_args;
            const auto& ta_all = t->symbol->template_args;
            if (fa_all.empty() || fa_all.size() != ta_all.size()) return false;
            if (!std::equal(fa_all.begin() + 1, fa_all.end(), ta_all.begin() + 1)) return false;   // the same allocator
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

    // The modules whose items code in this module can name (spec 30): itself, its imports and,
    // after `import std;`, the standard library's modules.
    std::vector<const Module*> visible_modules() const {
        std::vector<const Module*> out;
        if (!module_) return out;
        out.push_back(module_);
        auto add = [&](const Module* m) {
            if (std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
        };
        for (const Module* m : module_->header_imports) add(m);
        for (const Module* m : module_->source_imports) add(m);
        if (module_->header_imports_std || module_->source_imports_std) {
            for (const auto& [name, m] : program_.modules) {
                if (name == "std" || name.starts_with("std::")) add(m.get());
            }
        }
        return out;
    }

    // An item of the built-in std module (`option`, `result`, `exit_code`), or null when no
    // file imports std.
    const Symbol* std_item(std::string_view name) const {
        const Module* m = program_.std_module;
        if (!m) return nullptr;
        return m->item("std::" + std::string(name));
    }

    // `std::ordering`, the result of `<=>` (spec 16).
    const Type* ordering_type() {
        if (const Symbol* o = std_item("ordering")) return tc_.user(o);
        return tc_.external("std::ordering");
    }

    // Whether `t` (references removed) is an instance of the std template `name`: `std::option<u32>`.
    static bool is_std_instance(const Type* t, std::string_view name) { return types::is_std_instance(t, name); }

    // `r?` (spec 11): unwraps `ok`/`some`, or returns the error / `none` from the function.
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

    // `value_or`, `expect`, `or_error`, `map_error` on std::option / std::result (spec 11). They
    // consume the receiver.
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

    // `std::some`, `std::none`, `std::ok`, `std::error` written without template arguments: the
    // alternatives std exports directly, whose arguments come from context (spec 08).
    static bool is_std_export(const PathExpr& pe) {
        const Symbol* s = pe.symbol;
        return s && s->kind == SymbolKind::alternative && pe.path.segments.size() == 2 &&
               pe.path.segments[0].name == "std" && pe.path.segments[1].template_args.empty() && s->parent &&
               s->parent->decl && s->parent->decl->is_template && !s->parent->template_of;
    }

    // Index of the template parameter that the payload of `alt` is (`some(T)` -> T), or -1.
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

    // The type of `std::ok(x)` / `std::none`: the instance when the payload fixes every
    // template argument, otherwise a pending type that context completes later.
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

    // A pending `std::ok(x)` / `std::none` meets the type `t` it is used as.
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

    // Gives an untyped value (literal, `bit(n)`, `default`, or arithmetic on those) the type
    // `target`, checking that it fits.
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
        case ExprKind::tuple: settle_tuple(e.as<TupleExpr>(), t); return;
        case ExprKind::list: settle_list(e.as<ListExpr>(), t); return;
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

    // The expression's value is used where `target` is expected: settle untyped values,
    // otherwise check assignability.
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
        if (e.kind == ExprKind::tuple && t->is(Kind::tuple)) {
            settle_tuple(e.as<TupleExpr>(), t);
            return;
        }
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
            if (t->is(Kind::function)) check_callable_fits(e, types::strip_ref(e.type), t);
        }
        if (smart_upcast(types::strip_ref(e.type), t) && !target->is(Kind::reference)) {
            e.conversion   = Expr::Conversion::pointer_upcast;
            e.converted_to = t;
        }
        if (is_std_text(types::strip_ref(e.type)) && t->is(Kind::external) && t->name == "std::str" &&
            !target->is(Kind::reference)) {
            e.conversion = Expr::Conversion::string_to_str;
        }
    }

    // `std::some(x)` where a `std::option<T&>` is expected: the option refers to the place x
    // (spec 11, 20: `a.get(i)`). True when `e` was such a call and is now typed `t`.
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

    // An untyped value in a place without any context (`auto x = 5;`, `if (3 < 4)`).
    void no_context(Expr& e, std::string_view where) {
        if (e.type && e.type->is(Kind::pending)) {
            no_context_pending(e, where);
            return;
        }
        if (e.kind == ExprKind::list) {
            Diagnostic& d = error(DiagCode::E4001, e.span, std::format("this list literal has no type {}", where), "type unknown");
            note(d, "a list literal becomes a std::array or a std::span: write e.g. 'std::array<u32, 3> x = [1, 2, 3];'");
            e.type = tc_.error();
            return;
        }
        if (e.kind == ExprKind::tuple) {
            for (ExprPtr& x : e.as<TupleExpr>().elements) {
                if (x->type && x->type->is_untyped()) no_context(*x, where);
            }
            e.type = tc_.error();
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

    // `{expr:spec}` in an f-string: the value needs a type, and the spec must fit it (spec 19).
    // An f-string passed to a function's `std::format_args` parameter is evaluated only if the
    // function formats it (spec 31), so its holes can't change anything. (std::println,
    // std::format and std::panic always format it, once.)
    void check_lazy_holes(Expr& arg) {
        if (arg.kind != ExprKind::string_literal || arg.as<LiteralExpr>().token_kind != TokenKind::format_string_literal) return;
        for (FormatPart& part : arg.as<LiteralExpr>().format_parts) {
            if (!part.expr) continue;
            Expr& e = *part.expr;
            ast::walk_exprs(e, [&](Expr& x) {
                const char* what = nullptr;
                if (x.kind == ExprKind::unary && x.as<UnaryExpr>().op == UnaryOp::move) what = "move a value";
                if (x.kind == ExprKind::unary && x.as<UnaryExpr>().op == UnaryOp::mut_marker) what = "pass a mutable reference";
                if (x.kind == ExprKind::call && x.as<CallExpr>().callee->kind == ExprKind::member) {
                    const Symbol*       m  = x.as<CallExpr>().callee->as<MemberExpr>().symbol;
                    const FunctionDecl* fd = m ? m->function() : nullptr;
                    if (fd && (fd->is_mut || fd->is_move)) what = fd->is_mut ? "call a mut method" : "call a consuming method";
                }
                if (!what) return true;
                note(error(DiagCode::E4251, x.span, std::format("an f-string hole can't {}", what), "changes something"),
                     "a hole is only evaluated if the message is formatted; compute the value before the f-string");
                return false;
            });
        }
    }

    void check_format_hole(FormatPart& part) {
        Expr&       e = *part.expr;
        const Type* t = types::strip_ref(infer(e));
        if (t->is_untyped()) {
            no_context(e, "in a format string");
            return;
        }
        const FormatSpec& s = part.spec;
        if (t->is(Kind::user) && !formattable(t)) {
            Diagnostic& d = error(DiagCode::E4171, e.span, std::format("{} can't be printed in an f-string", quoted(t)),
                                  "not std::formattable");
            if (t->symbol->kind == SymbolKind::enum_) {
                note(d, "write '@std::derive::formattable' on the enum to print its alternative and payload (spec 29)");
            } else {
                note(d, "give it a 'void format(mut std::string& out)' method (std::formattable), or write '@std::derive::formattable' on it");
            }
            return;
        }
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
        case DeclKind::function: {
            if (!d.generated_by) {
                check_function(d.as<FunctionDecl>(), self);
                return;
            }
            // Errors in generated code are reported at the `@` that caused it, with a note into
            // the generate block (spec 29).
            std::size_t mark = diags_.size();
            check_function(d.as<FunctionDecl>(), self);
            for (Diagnostic& g : diags_.take_since(mark)) {
                Diagnostic moved = g;
                moved.span       = d.generated_by->span;
                moved.label      = std::format("in the code '@{}' generates for '{}'", ast::to_string(d.generated_by->path),
                                               self ? self->name : "?");
                moved.notes.insert(moved.notes.begin(), Note{"the generated code is here", g.span});
                diags_.add(std::move(moved));
            }
            return;
        }
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
                if (d.kind == DeclKind::field && compile_time_only(lower(*v.type)) && v.storage != Storage::compile_time) {
                    run_time_use(v.type->span, lower(*v.type), std::format("'{}' is a field", v.name),
                                 "values of a struct or class exist at run time");
                }
                return;
            }
            if (!begin_check(&d)) return;
            if (d.kind == DeclKind::field && v.init->kind == ExprKind::default_ && !subst_.empty()) {
                // `A alloc = default;` in an instance: without a default for this argument the
                // instance just has no default either (allocators: `std::arena_allocator`).
                const Type* declared = lower(*v.type);
                std::size_t mark     = diags_.size();
                check_has_default(declared, v.init->span);
                if (diags_.size() != mark) {
                    (void)diags_.take_since(mark);
                    if (Symbol* f = symbol_for(d)) f->no_default = true;
                    v.init->type = declared;
                    end_check(&d);
                    return;
                }
            }
            std::size_t   errors = diags_.error_count();
            FunctionState saved = fn_;
            fn_                 = FunctionState{};
            fn_.is_compile_time = v.storage != Storage::local;   // static/compile_time initializers run at compile time
            infer(*v.init);
            const Type* declared = lower(*v.type);
            coerce(*v.init, declared);
            fn_ = saved;
            if ((d.kind == DeclKind::variable || d.kind == DeclKind::field) && v.storage == Storage::compile_time) {   // a field: `type element = u32;`
                Symbol* g = symbol_for(d);
                if (!g && d.kind == DeclKind::field && self) {
                    if (auto it = self->members.find(v.name); it != self->members.end() && !it->second.empty()) g = it->second.front();
                }
                if (g && diags_.error_count() == errors) {
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
            bool annotation = t && t->decl && t->decl->kind == DeclKind::struct_ &&
                              t->decl->as<TypeDecl>().is_annotation;   // its checks run per use (spec 29)
            if (annotation && t->decl == &d) {   // fields hold compile-time values (spec 29, rule 1)
                for (const DeclPtr& m : d.as<TypeDecl>().members) {
                    if (m->kind != DeclKind::field) continue;
                    const Type* ft = lower(*m->as<VariableDecl>().type);
                    std::set<const Symbol*> seen;
                    if (!ft->is_unknown() && !annotation_value_type(ft, seen)) {
                        note(error(DiagCode::E4240, m->as<VariableDecl>().type->span,
                                   std::format("an annotation's field can't be {}", quoted(ft)), "not a compile-time value"),
                             "use numbers, bool, char, std::static_str, type, simple enums, structs of these, or std::span of these");
                    }
                }
            }
            for (DeclPtr& m : d.as<TypeDecl>().members) {
                if (annotation && m->kind == DeclKind::check) continue;
                check_decl(*m, t);
            }
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
        if (global_symbol_ && global_symbol_->decl == &d) return const_cast<Symbol*>(global_symbol_);
        auto it = module_->items.find(d.ns.empty() ? d.name : d.ns + "::" + d.name);
        if (it == module_->items.end()) return nullptr;
        for (Symbol* s : it->second) {
            if (s->decl == &d || s->definition == &d) return s;
        }
        return it->second.front()->is_type() ? it->second.front() : nullptr;
    }

    // Whether `fn` is a template with a parameter pack (templates, "Parameter packs").
    static bool has_pack(const FunctionDecl& fn) {
        return fn.is_template && std::any_of(fn.template_params.begin(), fn.template_params.end(), [](const TemplateParam& p) { return p.is_pack; });
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
        bool in_pack_type = self && self->decl && self->decl->is_template && !self->template_of &&
                            !self->decl->template_params.empty() && self->decl->template_params.back().is_pack;
        if (has_pack(fn) || in_pack_type) {   // a pack template's body is checked per argument count, in its instances
            fn_ = saved;
            end_check(&fn);
            return;
        }
        if (fn.is_unsafe) ++unsafe_depth_;
        std::vector<std::string> allowed = allowed_ids(fn);   // `@std::allow{ .id = ... }` (spec 29)
        program_.allowed_warnings.insert(program_.allowed_warnings.end(), allowed.begin(), allowed.end());
        for (Param& p : fn.params) {
            if (p.symbol) set_local(p.symbol, lower(*p.type));
            if (!p.is_compile_time || !p.symbol) continue;   // a constant in an instance (spec 31)
            for (const auto& [sym, t] : subst_) {
                if (sym && sym->name == "$" + p.name && t && t->is(Kind::value_arg)) {
                    p.symbol->ct_value = std::make_shared<const ct::Value>(value_of_arg(t));
                }
            }
            if (!p.symbol->ct_value) unknown_values_.insert(p.symbol);   // the generic check: not known yet
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
        program_.allowed_warnings.resize(program_.allowed_warnings.size() - allowed.size());
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

    // Types whose values exist only at compile time: `type`, std::meta data, lists of them.
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
            h.check_field    = [this](const Symbol* f) { ensure_field(f); };
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
            h.visible_methods = [this](const Type* t) {
                std::vector<const Symbol*> out;
                for (const Symbol* m : t->symbol->member_order) {
                    if (m->kind != SymbolKind::method || !member_visible(m)) continue;
                    const FunctionDecl* f = m->function();
                    if (!f || f->is_destructor || f->is_operator || f->is_template) continue;
                    out.push_back(m);
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

    // Evaluates a checked expression at compile time; null when it cannot be (reported, or it
    // depends on a template parameter, see `evaluator().unknown()`).
    // A call of a compile_time-only function in run-time code runs at compile time: its
    // result is a constant (spec 15). So does a settings method called on the build mode
    // (`std::build::current_mode().optimization()`, spec 24).
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

    // The compile-time value a template value argument stands for.
    ct::Value value_of_arg(const Type* arg) const {
        if (std::shared_ptr<const void> rich = tc_.value_payload(arg)) return *std::static_pointer_cast<const ct::Value>(rich);
        const Type* of = arg->inner;
        if (of->is(Kind::bool_)) return ct::Value::of_bool(tc_.value_bits(arg) != 0, of);
        return ct::Value::of_integer(tc_.value_bits(arg), of);
    }

    // Evaluates a template value argument for the parameter `index` of `templ` (spec 14).
    const Type* evaluate_value_arg(const Symbol* templ, std::size_t index, Expr& value) {
        if (templ->decl && !templ->decl->template_params.empty() && templ->decl->template_params.back().is_pack &&
            index >= templ->decl->template_params.size()) {
            index = templ->decl->template_params.size() - 1;   // the rest of the arguments go to the pack (spec 34)
        }
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
        std::shared_ptr<const ct::Value> v =
            evaluate(value, std::format("a template argument of '{}' must be known at compile time", templ->name));
        if (!v) return tc_.error();
        if (want->is_integer() || want->is(Kind::bool_)) {
            return tc_.value_arg(v->kind == ct::Value::Kind::boolean ? u128{v->boolean} : v->integer, want);
        }
        // Any other compile-time value (a struct, an enum): interned by its text, like the
        // argument of a `compile_time` parameter (spec 31).
        return tc_.value_arg_of(ct::to_string(*v), want, v);
    }

    // The type a type value denotes, when the expression is one (`u32`, `point`, `T`).
    const Type* denoted_type(const Expr& e) const {
        if (!e.constant || e.constant->kind != ct::Value::Kind::type) return nullptr;
        return e.constant->type_value;
    }

    // --- checks (spec 23) -----------------------------------------------------------------

    enum class Where { file_scope, type_body, entry, body };

    // Whether a check's mode list includes the mode being built (spec 23, 24). Built-in modes
    // only, until build.ed can define more.
    bool mode_active(const std::vector<std::string>& modes, Span span) {
        if (modes.empty()) return true;
        const std::string& m      = program_.build_mode;
        bool               active = false;
        constexpr std::string_view prefix = "std::build::mode::";
        for (const std::string& path : modes) {
            if (path.starts_with("build::")) {   // a mode class of build.ed (spec 24, open question 31)
                const Symbol* cls = nullptr;
                if (auto bm = program_.modules.find("build"); bm != program_.modules.end()) {
                    cls = bm->second->item(path);
                }
                if (!cls || cls->kind != SymbolKind::class_) {
                    note(error(DiagCode::E4212, span, std::format("unknown build mode '{}'", path), "unknown mode"),
                         "build.ed does not define this mode class");
                    continue;
                }
                for (const Symbol* c = program_.build_mode_class; c; c = c->base_class()) active = active || c == cls;
                continue;
            }
            std::string name = path.starts_with(prefix) ? path.substr(prefix.size()) : std::string();
            auto builtin = [](const std::string& x) {
                return x == "debug" || x == "development" || x == "release" || x == "release_with_debug_info";
            };
            if (!builtin(name)) {
                Diagnostic& d = error(DiagCode::E4212, span, std::format("unknown build mode '{}'", path), "unknown mode");
                if (builtin(path)) {
                    note(d, std::format("write 'std::build::mode::{}': std names are written in full", path));
                } else {
                    note(d, "the built-in modes are std::build::mode::debug, ::development, ::release and "
                            "::release_with_debug_info (user modes need build.ed)");
                }
                continue;
            }
            if (program_.build_mode_class) {   // the mode is (or derives from) that class
                for (const Symbol* c = program_.build_mode_class; c; c = c->base_class()) active = active || c->name == name;
            } else if (name == m || (name == "release" && m == "release_with_debug_info") || (name == "debug" && m == "development")) {
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

    // A file-scope or type-body compile_time check: evaluated once (per instance for templates).
    void run_check_now(Check& c, Span span, Symbol* self) {
        if (self && self->decl && self->decl->is_template && !self->template_of) return;   // the generic template
        if (c.block) {
            evaluator().evaluate(*c.block, "");
            return;
        }
        std::size_t errors = diags_.error_count();
        std::shared_ptr<const ct::Value> v = evaluate(*c.condition, "the condition of a compile_time check must be known at compile time");
        bool broken = false;   // an operand already reported (or still waiting for a template parameter)
        ast::walk_exprs(*c.condition, [&](Expr& x) {
            if (!x.type || x.type->is_unknown() || x.type->is_untyped()) broken = true;
            return !broken;
        });
        if (!v && diags_.error_count() == errors && !fn_.generic && !broken) {
            // Not reported and not waiting for a template parameter: the evaluator can't run this
            // condition. A check must never pass without being evaluated (spec 23).
            note(error(DiagCode::E4200, c.condition->span, "the compiler can't evaluate this condition at compile time",
                       "not evaluated"),
                 "a compile_time check that isn't evaluated would pass unseen, so it is an error");
            return;
        }
        if (!v || v->kind != ct::Value::Kind::boolean || v->boolean) return;
        std::shared_ptr<const ct::Value> m = evaluate(*c.message, "");
        bool        at_use = self && self->template_of && instance_site_;   // reported where the instance is written (spec 23)
        Diagnostic& d = error(DiagCode::E4210, at_use ? *instance_site_ : c.condition->span,
                              m && m->kind == ct::Value::Kind::string ? m->string : "check failed", "check failed");
        if (self && self->template_of) note(d, std::format("in the instance {}", quoted(tc_.user(self))));
        if (at_use) note(d, "the check is here", c.condition->span);
        (void)span;
    }

    // The entry checks of `callee` for one call (spec 23): compile_time checks are evaluated with
    // the call's arguments, which must then be compile-time values; `compile_time run_time`
    // checks are evaluated when the arguments happen to be known.
    void run_entry_checks(const Symbol* callee, CallExpr& c, const std::vector<Expr*>& args) {
        if (fn_.is_compile_time) return;   // compile-time code: the evaluator runs them when it calls
        if (own_entry_fails()) return;     // a body that is never reached
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
            ctx.parameter_text = [&](const Symbol* p) -> std::optional<std::string> {
                for (std::size_t i = 0; i < f.params.size() && i < args.size(); ++i) {
                    if (f.params[i].symbol != p) continue;
                    const Expr& a = *args[i];
                    if (a.kind != ExprKind::string_literal) return std::nullopt;
                    const auto& lit = a.as<LiteralExpr>();
                    if (lit.token_kind != TokenKind::format_string_literal) return lit.value;
                    std::string text;
                    for (const FormatPart& part : lit.format_parts) text += part.expr ? std::string("{}") : part.text;
                    return text;
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
            // The author's message is the only one for this call: errors from instantiating the
            // callee's body (outside the call's own text) are dropped (spec 23, "Order").
            if (auto mark = call_marks_.find(&c); mark != call_marks_.end() && mark->second <= diags_.size()) {
                for (Diagnostic& found : diags_.take_since(mark->second)) {
                    bool inside = found.span.file == c.span.file && found.span.begin >= c.span.begin && found.span.end <= c.span.end;
                    if (inside) diags_.add(std::move(found));
                }
            }
            Diagnostic& e = error(DiagCode::E4210, c.span, m && m->kind == ct::Value::Kind::string ? m->string : "check failed",
                                  "this call fails the check");
            note(e, std::format("the check of '{}' is here", callee->name), k.condition->span);
        }
    }

    // Whether one of the current function's own compile_time entry checks fails whatever the
    // arguments (a capability the target lacks). Such a body is never reached, so the checks of
    // the calls in it stay quiet and the caller gets the author's message once (spec 23,
    // "Order"). Evaluated at the first call in the body, when the entry has been checked.
    bool own_entry_fails() {
        if (fn_.entry_known || !fn_.decl || !fn_.decl->body) return fn_.entry_failed;
        fn_.entry_known = true;
        for (StmtPtr& st : fn_.decl->body->statements) {
            if (st->kind != StmtKind::check) break;
            Check& k = st->as<CheckStmt>().check;
            if (!k.is_compile_time || k.is_run_time || !k.condition || !k.condition->type) continue;
            ct::Evaluator::Context q;
            q.quiet = true;
            std::shared_ptr<const ct::Value> v = evaluator().evaluate_in(*k.condition, "", q);
            if (v && v->kind == ct::Value::Kind::boolean && !v->boolean) fn_.entry_failed = true;
        }
        return fn_.entry_failed;
    }

    // The number of diagnostics when the inference of each call began.
    std::map<const CallExpr*, std::size_t> call_marks_;

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
        if (s.allowed.empty()) return check_stmt_inner(s);
        std::size_t n = 0;   // `@std::allow{ .id = ... } stmt;`: silenced for this statement alone
        for (ExprPtr& id : s.allowed) {
            const Symbol* sym = id->kind == ExprKind::path ? id->as<PathExpr>().symbol : nullptr;
            if (!sym) continue;   // unknown: reported by name resolution
            program_.allowed_warnings.push_back(sym->key());
            ++n;
        }
        check_stmt_inner(s);
        program_.allowed_warnings.resize(program_.allowed_warnings.size() - n);
    }

    void check_stmt_inner(Stmt& s) {
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
                    if (declared && declared->is(Kind::reference)) reject_packed_reference(*v.init, "borrowed");
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
        case StmtKind::destructure: check_destructure(s.as<DestructureStmt>()); return;
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
            if (e.kind == ExprKind::call && !t->is(Kind::void_) && !t->is(Kind::never)) {   // @std::must_use (spec 29)
                const Symbol*                     f    = called_symbol(e.as<CallExpr>());
                const std::vector<AnnotationUse>* on_f = annotations_of(f && f->template_of && f->template_of->kind == f->kind ? f->template_of : f);
                const Type*                       rt   = types::strip_ref(t);
                const std::vector<AnnotationUse>* on_t = rt->is(Kind::user) ? annotations_of(rt->symbol) : nullptr;
                bool by_f = on_f && sema::find_annotation(*on_f, "std::must_use");
                if (by_f || (on_t && sema::find_annotation(*on_t, "std::must_use"))) {
                    Diagnostic& d = error(DiagCode::E4150, e.span,
                                          by_f ? std::format("the result of '{}' must be used", f->name)
                                               : std::format("the {} returned here must be used", quoted(t)),
                                          "result not used");
                    note(d, "it is marked '@std::must_use'; discard it explicitly with '_ = ...;'");
                    return;
                }
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
        union_store_       = a.op == TokenKind::eq && a.target->kind == ExprKind::member;   // writing a union member is safe
        const Type* target = infer(*a.target);
        union_store_       = false;
        if (a.target->kind == ExprKind::index && a.target->as<IndexExpr>().op_symbol) {
            // Assigning through `v[i]` uses `mut T& operator[](usize) mut` (spec 16).
            auto&       ix  = a.target->as<IndexExpr>();
            const Type* obj = types::strip_ref(ix.object->type);
            if (Symbol* m = index_operator(obj, true); m && m->function_decl().is_mut) {
                ix.op_symbol    = m;
                use_method(m, ix.span);
                target          = m->function_decl().result ? lower_owned(m, *m->function_decl().result) : target;
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
        if (const Type* u = distinct_arithmetic(t)) {   // `d += e`, `d *= 2.0` on a distinct type (spec 34)
            const Type* vt = types::strip_ref(value);
            if (op == TokenKind::plus || op == TokenKind::minus || op == TokenKind::percent) {
                if (vt != t && !vt->is_unknown()) {
                    error(DiagCode::E4013, a.span, std::format("'{}' on {} takes a {}", token_kind_spelling(a.op), quoted(t), quoted(t)), "wrong type");
                    return;
                }
                to_underlying(a.value, u);
            } else if (op == TokenKind::star || op == TokenKind::slash) {
                coerce(*a.value, u);
            } else {
                error(DiagCode::E4013, a.span, std::format("'{}' is not defined for {}", token_kind_spelling(a.op), quoted(t)), "no such operator");
                return;
            }
            a.distinct_underlying = u;
            return;
        }
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
                p.is_mutable   = m.deref_symbol ? m.deref_symbol->function_decl().is_mut : !pt->is(Kind::pointer) || pt->is_mut;
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
            const auto& ix   = e.as<IndexExpr>();
            Place       base = place_of(*ix.object);
            base.name += "[...]";
            // `std::span<mut T>`: a non-mut `operator[]` that gives `mut T&` (spec 20).
            const Type* ot = ix.object->type ? types::strip_ref(ix.object->type) : nullptr;
            bool span_mut  = (ot && ot->is_unknown()) || (ot && ot->is(Kind::user) && (ot->symbol->name == "span_mut" ||
                                                          (ot->symbol->template_of && ot->symbol->template_of->name == "span_mut")));
            if (const FunctionDecl* f = ix.op_symbol ? ix.op_symbol->function() : nullptr;
                span_mut || (f && !f->is_mut && f->result && f->result->kind == TypeKind::reference && f->result->as<ReferenceType>().is_mut)) {
                base.is_place   = true;
                base.is_mutable = true;
                return base;
            }
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
            if (p.local->local_kind == sema::LocalKind::capture && !t->is(Kind::reference)) {
                note(d, std::format("a lambda changes its own copy only when the capture says so: '[mut {} ...]'", p.local->name),
                     p.local->span);
            } else if (t->is(Kind::reference)) {
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
        case ExprKind::default_:
            if (e.constant && e.type) return e.type;   // an argument the compiler supplied (spec 31)
            return set(e, tc_.untyped_default());
        case ExprKind::break_:
        case ExprKind::continue_: return set(e, tc_.never());
        case ExprKind::error: return set(e, tc_.error());
        case ExprKind::path: {
            const Type* t = infer_path(e.as<PathExpr>());
            if (const Symbol* s = e.as<PathExpr>().symbol; s && (s->kind == SymbolKind::alternative || s->kind == SymbolKind::global)) {
                deprecated_use(s->kind == SymbolKind::alternative && s->parent && s->parent->template_of
                                   ? nullptr : s, e.span);
            }
            return t;
        }
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
            if (const Symbol* f = called_symbol(e.as<CallExpr>())) {
                deprecated_use(f->template_of && f->template_of->kind == f->kind ? f->template_of : f,
                               e.as<CallExpr>().callee->kind == ExprKind::member ? e.as<CallExpr>().callee->as<MemberExpr>().name.span
                                                                                 : e.as<CallExpr>().callee->span);
            }
            return t;
        }
        case ExprKind::index: {
            auto& i = e.as<IndexExpr>();
            return set(e, infer_index(i));
        }
        case ExprKind::member: {
            const Type* t = infer_member(e.as<MemberExpr>());
            // A field of a compile-time value (`r.min` of a `compile_time range r`): a constant too.
            if (!e.constant && e.as<MemberExpr>().object->constant && !t->is_unknown() && !compile_time_only(t) &&
                e.as<MemberExpr>().object->constant->kind == ct::Value::Kind::aggregate) {
                ct::Evaluator::Context quiet;
                quiet.quiet = true;
                e.constant  = evaluator().evaluate_in(e, "", quiet);
            }
            if (const Symbol* f = e.as<MemberExpr>().symbol; f && f->kind == SymbolKind::field) deprecated_use(f, e.as<MemberExpr>().name.span);
            return t;
        }
        case ExprKind::try_: return infer_try(e.as<TryExpr>());
        case ExprKind::cast: return infer_cast(e.as<CastExpr>());
        case ExprKind::bit: return infer_bit(e.as<BitExpr>());
        case ExprKind::struct_literal: return infer_struct_literal(e.as<StructLiteralExpr>());
        case ExprKind::tuple: return infer_tuple(e.as<TupleExpr>());
        case ExprKind::list:
            for (ExprPtr& x : e.as<ListExpr>().elements) infer(*x);
            return set(e, tc_.untyped_list());
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
        else if (t->is(Kind::mask)) note(d, "a mask has one answer per lane: write '.any()' or '.all()'");
        else note(d, "only bool, integers and std::raw_ptr can be conditions");
    }

    // The limit constants of a number type: `T::min`, `T::max`, `T::bits`; floats also
    // `infinity`, `nan`, `epsilon`.
    std::optional<ct::Value> number_limit(const Type* t, std::string_view name) {
        if (name == "bits") return ct::Value::of_integer(static_cast<u128>(t->bits), tc_.builtin("u32"));
        if (t->is_integer()) {
            u128 one = 1;
            u128 umax = t->bits >= 128 ? ~u128{0} : (one << t->bits) - 1;
            if (name == "max") return ct::Value::of_integer(t->is_signed ? umax >> 1 : umax, t);
            if (name == "min") return ct::Value::of_integer(t->is_signed ? (umax >> 1) + 1 : 0, t);
            return std::nullopt;
        }
        ct::Value v;
        v.kind = ct::Value::Kind::floating;
        v.type = t;
        bool f32 = t->bits == 32;
        if (t->bits == 16) {   // f16 (spec 34): IEEE binary16
            if (name == "max") v.floating = 65504.0;
            else if (name == "min") v.floating = -65504.0;
            else if (name == "infinity") v.floating = std::numeric_limits<double>::infinity();
            else if (name == "nan") v.floating = std::numeric_limits<double>::quiet_NaN();
            else if (name == "epsilon") v.floating = 0.0009765625;
            else return std::nullopt;
            return v;
        }
        if (name == "max") v.floating = f32 ? static_cast<double>(std::numeric_limits<float>::max()) : std::numeric_limits<double>::max();
        else if (name == "min") v.floating = f32 ? -static_cast<double>(std::numeric_limits<float>::max()) : -std::numeric_limits<double>::max();
        else if (name == "infinity") v.floating = std::numeric_limits<double>::infinity();
        else if (name == "nan") v.floating = std::numeric_limits<double>::quiet_NaN();
        else if (name == "epsilon") v.floating = f32 ? static_cast<double>(std::numeric_limits<float>::epsilon()) : std::numeric_limits<double>::epsilon();
        else return std::nullopt;
        return v;
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
            // `std::fn_ptr<R(A...)>`, `std::function<R(A...)>`: built-in callable types as values.
            if (const auto& segs = pe.path.segments; segs.size() >= 2 && segs[0].name == "std" && segs.back().template_args.size() == 1 &&
                segs.back().template_args[0].type && segs.back().template_args[0].type->kind == TypeKind::function) {
                set(pe, tc_.type_value());
                pe.constant = evaluate(pe, "");
                return set(pe, pe.constant ? tc_.type_value() : tc_.error());
            }
            // `u32::max`, `i8::min`, `u64::bits`, `f32::infinity` (limits of number types).
            if (pe.path.segments.size() == 2 && !pe.path.segments[0].has_template_args) {
                if (const Type* b = tc_.builtin(pe.path.segments[0].name); b && b->is_numeric()) {
                    if (auto v = number_limit(b, pe.path.segments[1].name)) {
                        pe.constant = std::make_shared<const ct::Value>(*v);
                        return set(pe, v->type);
                    }
                    error(DiagCode::E3025, pe.path.segments[1].span,
                          std::format("'{}' has no constant '{}'", b->name, pe.path.segments[1].name),
                          b->is_integer() ? "integers have min, max and bits" : "floats have min, max, bits, infinity, nan and epsilon");
                    return set(pe, tc_.error());
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

    // `x.join()` where `join` ends with `move` (spec 06, 27): `x` must be a value the caller owns,
    // a local variable, a by-value parameter or a temporary, never something reached through a
    // reference, a field or `this` of a method that doesn't consume it.
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

    // Checks that `op` applies to values of type `t` (already unified).
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

    // A user-defined operator for `l op r`: a method `operator op` on `l` or a free function.
    // `found` (if given) receives the operator's symbol.
    const Type* find_operator(std::string_view op, const Type* l, const Type* r, Symbol** found = nullptr) {
        auto try_fn = [&](Symbol* s, bool is_method) -> const Type* {
            const Type* t = match_operator(s, is_method, op, l, r);
            if (t) use_method(s, s->span);
            if (t && found) *found = s;
            return t;
        };
        return search_operator(op, l, r, try_fn);
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

    // Whether an operand of type `from` can be passed for an operator parameter of type `to`;
    // an untyped literal takes the parameter's type (spec 02).
    bool operand_fits(const Type* from, const Type* to) {
        const Type* t = types::strip_ref(to);
        switch (from->kind) {
        case Kind::untyped_int: return t->is_unknown() || t->is_numeric();
        case Kind::untyped_float: return t->is_unknown() || t->is_float();
        case Kind::untyped_default: return true;
        default: return assign_cost(from, to) >= 0;
        }
    }

    // Gives the untyped literal operands of the operator call the types of its parameters.
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
    const Type* search_operator(std::string_view op, const Type* l, const Type* r, F&& try_fn) {
        if (l->is(Kind::user)) {
            if (auto it = l->symbol->members.find(std::string(op)); it != l->symbol->members.end()) {
                for (Symbol* m : it->second) {
                    if (const Type* t = try_fn(m, true)) return t;
                }
            }
        }
        // Free operators live with one of their operand types: in that type's namespace and
        // module (spec 16, 30). Nothing else is searched, so no other module can add one.
        const Symbol* homes[2] = {nullptr, nullptr};
        if (const Type* lt = types::strip_ref(l); lt && lt->is(Kind::user)) homes[0] = lt->symbol;
        if (const Type* rt = r ? types::strip_ref(r) : nullptr; rt && rt->is(Kind::user)) homes[1] = rt->symbol;
        for (int i = 0; i < 2; ++i) {
            const Symbol* home = homes[i];
            if (!home || (i == 1 && homes[0] && homes[0]->module == home->module && homes[0]->ns == home->ns)) continue;
            const Symbol* type = home->template_of ? home->template_of : home;
            std::string   key  = type->ns.empty() ? std::string(op) : type->ns + "::" + std::string(op);
            if (auto it = type->module->items.find(key); it != type->module->items.end()) {
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
        if (distinct_arithmetic(l) || distinct_arithmetic(r)) return infer_distinct_binary(b, l, r);

        if (l->is(Kind::vector) || r->is(Kind::vector) || l->is(Kind::mask) || r->is(Kind::mask)) return infer_vector_binary(b, l, r);
        if (l->is(Kind::tuple) || r->is(Kind::tuple)) {   // element by element
            const Type* t = l->is(Kind::tuple) ? l : r;
            coerce(*b.lhs, t);
            coerce(*b.rhs, t);
            bool is_eq   = op == TokenKind::eq_eq || op == TokenKind::bang_eq;
            bool ordered = op == TokenKind::spaceship || is_comparison(op);
            if (!is_eq && !ordered) {
                error(DiagCode::E4013, b.span, std::format("operator '{}' is not defined for tuples", token_kind_spelling(op)),
                      "no such operator");
                return set(b, tc_.error());
            }
            if (!tuple_comparable(t, ordered)) {
                note(error(DiagCode::E4013, b.span, std::format("{} values cannot be {}", quoted(t), ordered ? "ordered" : "compared"),
                           "not comparable"),
                     "tuples compare element by element; every element must support the operator");
            }
            return set(b, op == TokenKind::spaceship ? ordering_type() : tc_.bool_());
        }

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
            bool text = t->is(Kind::static_str) || (t->is(Kind::external) && t->name == "std::str");   // byte order (spec 19)
            if (!t->is_numeric() && !t->is(Kind::char_) && !text && !t->is_unknown() && !t->is_untyped()) {
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
            // The generic check of a template: one side depends on a template parameter, so its
            // operators are only known per instantiation, which is checked again.
            if (l->is_unknown() || r->is_unknown()) return set(b, is_eq || is_comparison(op) ? tc_.bool_() : tc_.error());
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
            bool text = t->is(Kind::static_str) || (t->is(Kind::external) && t->name == "std::str");   // byte order (spec 19)
            if (is_comparison(op) && !t->is_numeric() && !t->is(Kind::char_) && !text && !t->is_unknown() && !t->is_untyped()) {
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

    // Constant shift counts must be smaller than the width (spec 16).
    void check_shift_count(const Expr& count, const Type* t) {
        if (!t->is_integer()) return;
        const Expr* c = &count;
        if (c->kind != ExprKind::int_literal) return;
        u128 n = c->as<LiteralExpr>().int_value;
        if (n >= static_cast<u128>(t->bits)) {
            note(error(DiagCode::E4080, count.span,
                       std::format("shift by {} is not smaller than the width of {} ({} bits)", edax::to_string(n),
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
                           std::format("bit({}) does not exist in {} ({} bits)", edax::to_string(v), quoted(t), t->bits),
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
                return set(u, tc_.reference(target, u.op_symbol->function_decl().is_mut));
            }
        }
        if (const Type* under = u.op == UnaryOp::negate ? distinct_arithmetic(s) : nullptr) {   // `-d` (spec 34)
            to_underlying(u.operand, under);
            u.distinct_result = s;
            set(u, s);
            if (u.operand->constant) u.constant = evaluate(u, "");
            return s;
        }
        if (s->is(Kind::user) && (u.op == UnaryOp::negate || u.op == UnaryOp::bit_not)) {   // spec 16
            std::string_view spelling = u.op == UnaryOp::negate ? "-" : "~";
            if (const Type* res = find_operator(spelling, s, nullptr, &u.op_symbol)) return set(u, res);
            error(DiagCode::E4013, u.span, std::format("operator '{}' is not defined for {}", spelling, quoted(s)),
                  "no such operator");
            return set(u, tc_.error());
        }
        if ((s->is(Kind::vector) || s->is(Kind::mask)) && (u.op == UnaryOp::negate || u.op == UnaryOp::bit_not || u.op == UnaryOp::logical_not)) {
            bool ok = s->is(Kind::mask) ? u.op == UnaryOp::logical_not
                    : u.op == UnaryOp::negate ? (s->inner->is_float() || s->inner->is_signed) : u.op == UnaryOp::bit_not && s->inner->is_integer();
            if (!ok) {
                error(DiagCode::E4013, u.span, std::format("this operator does not apply to {}", quoted(s)), "not defined");
            }
            return set(u, s);
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
        case UnaryOp::spread:
            error(DiagCode::E4030, u.span, "only a parameter pack is passed on with '...', as an argument for another pack",
                  "not a pack");
            return set(u, tc_.error());
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
        bool        union_store = std::exchange(union_store_, false);   // `u.member = value;`
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
        if (t->is(Kind::tuple)) {   // `t.0`: an element by position
            std::size_t i = 0;
            bool        ok = !name.empty() && name.find_first_not_of("0123456789") == std::string::npos;
            if (ok) i = static_cast<std::size_t>(std::stoull(name));
            if (!ok || i >= t->args.size()) {
                error(DiagCode::E4070, m.name.span,
                      std::format("{} has elements .0 to .{}", quoted(t), t->args.size() - 1), "no such element");
                return set(m, tc_.error());
            }
            return set(m, t->args[i]);
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
        if (!union_store && types::is_union(t->symbol)) {   // spec 34: which member holds a value is not known
            require_unsafe(m.span, std::format("reading the member '{}' of the union {}", name, quoted(t)));
        }
        return set(m, lower_owned(mem, *mem->decl->as<VariableDecl>().type));
    }

    bool union_store_ = false;   // the target of `x.member = value;` is being checked

    // --- reflection (spec 26) -------------------------------------------------------------

    // `std::is_integer<T>()`, `std::size_of<T>()`, `std::meta::fields<T>()`, ...: compile-time
    // functions of the compiler. Their value is computed here, so run-time code gets a constant.
    // `std::embed("file")` → the file's bytes as a std::span<u8> in static memory;
    // `std::embed_text("file")` → its text as a std::static_str (spec 34). The path is relative to
    // the source file, and the file is an input of the build.
    const Type* infer_embed(CallExpr& c, const std::string& name) {
        namespace fs = std::filesystem;
        bool        text = name == "embed_text";
        const Type* u8   = tc_.builtin("u8");
        const Symbol* span = std_item("span");
        const Type* result = text ? tc_.static_str() : span ? tc_.instance(span, {u8}) : tc_.error();
        set(c, result);
        if (c.args.size() != 1) {
            error(DiagCode::E4030, c.span, std::format("'std::{}' takes one path: 'std::{}(\"shaders/blit.spv\")'", name, name), "wrong arguments");
            return result;
        }
        coerce(*c.args[0], tc_.static_str());
        std::string rel;
        if (c.args[0]->kind == ExprKind::string_literal) {
            rel = c.args[0]->as<LiteralExpr>().value;
        } else if (c.args[0]->constant && c.args[0]->constant->kind == ct::Value::Kind::string) {
            rel = c.args[0]->constant->string;
        } else {
            error(DiagCode::E4200, c.args[0]->span, std::format("the path of 'std::{}' must be known at compile time", name), "not a constant");
            return result;
        }
        std::error_code ec;
        fs::path        path = fs::path(rel).is_absolute() ? fs::path(rel) : fs::path(source_path(c.span)).parent_path() / rel;
        std::ifstream   in(path, std::ios::binary);
        if (!in) {
            note(error(DiagCode::E4201, c.args[0]->span, std::format("cannot read '{}'", path.lexically_normal().string()), "no such file"),
                 "the path is relative to the directory of the source file");
            return result;
        }
        std::string bytes = read_rest(in);
        if (text && !valid_utf8(bytes)) {
            error(DiagCode::E4201, c.args[0]->span, std::format("'{}' is not UTF-8 text; use std::embed for bytes", rel), "not text");
            return result;
        }
        program_.embedded_files.push_back(fs::absolute(path, ec).lexically_normal().string());
        auto v    = std::make_shared<ct::Value>();
        v->kind   = ct::Value::Kind::string;
        v->type   = result;
        v->string = std::move(bytes);
        c.constant = v;
        return result;
    }

    static bool valid_utf8(const std::string& s) {
        std::size_t i = 0;
        while (i < s.size()) {
            auto c = static_cast<unsigned char>(s[i]);
            std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
            if (n == 0 || i + n > s.size()) return false;
            for (std::size_t k = 1; k < n; ++k) {
                if ((static_cast<unsigned char>(s[i + k]) >> 6) != 2) return false;
            }
            i += n;
        }
        return true;
    }

    const Type* infer_builtin(CallExpr& c, const std::string& name) {
        if (name == "embed" || name == "embed_text") return infer_embed(c, name);
        if (name.starts_with("compile_")) return infer_diagnostic(c, name);
        if (name == "is_compile_time" || name == "location_of" || name == "literal_text") return infer_parameter_query(c, name);
        if (name == "meta::function_annotation" || name == "meta::type_annotation" || name == "meta::file_annotation") {
            auto&         s  = c.callee->as<PathExpr>().path.segments.back();
            const Symbol* cs = std_item("call_site");
            if (s.template_args.size() != 1 || !s.template_args[0].type || c.args.size() != 1 || !cs) {
                error(DiagCode::E4030, c.span,
                      std::format("'{}' takes one annotation and a call site: '{}<A>(where)'", ast::to_string(c.callee->as<PathExpr>().path),
                                  ast::to_string(c.callee->as<PathExpr>().path)),
                      "wrong arguments");
                return set(c, tc_.error());
            }
            coerce(*c.args[0], tc_.user(cs));
            const Type* t = option_of_annotation(s.template_args[0], c.span);
            set(c, t);
            if (!fn_.is_compile_time) {
                error(DiagCode::E4200, c.span, std::format("'{}' runs at compile time only", ast::to_string(c.callee->as<PathExpr>().path)),
                      "not in a compile_time function");
                return t;
            }
            if (c.args[0]->constant) c.constant = evaluate(c, "");
            return t;
        }
        if (name == "meta::compiler_version") {   // the edaxc that compiles the program (plugins)
            set(c, tc_.static_str());
            c.constant = evaluate(c, "");
            return tc_.static_str();
        }
        auto&       seg   = c.callee->as<PathExpr>().path.segments.back();
        std::size_t wants = name == "is_same" || name == "implements" || name == "meta::annotation" || name == "meta::has_annotation" ? 2 : 1;
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
                      : name == "meta::methods" || name == "meta::functions_with" ? list_of(tc_.meta_function())
                      : name == "meta::types_with"                ? list_of(tc_.type_value())
                      : name == "meta::interfaces" || name == "meta::template_args" ? list_of(tc_.type_value())
                      : name == "meta::annotation"                ? option_of_annotation(seg.template_args[0], c.span)
                      : name == "meta::interface_hash" || name == "meta::type_id" ? tc_.builtin("u64")
                                                                  : tc_.bool_();
        set(c, t);
        c.constant = evaluate(c, "");
        return t;
    }

    // `std::raw::allocate<T>(n)`, `reallocate(p, n)`, `free(p)`, `read(p)`, `write(p, v)`,
    // `drop(p)` and `copy(to, from, n)`: the memory operations the library is written with
    // (spec 13). All of them require `unsafe`.
    // The thread intrinsics of std::raw (spec 27): atomic operations on memory, the futex and
    // the spin-loop hint. The library's synchronized types are written with them.
    const Type* infer_thread_raw(CallExpr& c, const std::string& name, const std::vector<Expr*>& args) {
        static const std::map<std::string, std::size_t> arity{
            {"atomic_load", 2}, {"atomic_store", 3}, {"atomic_exchange", 3}, {"atomic_compare_exchange", 6},
            {"atomic_fetch_add", 3}, {"atomic_fetch_sub", 3}, {"atomic_fetch_and", 3}, {"atomic_fetch_or", 3},
            {"atomic_fetch_xor", 3}, {"atomic_fence", 1}, {"futex_wait", 2}, {"futex_wait_for", 3}, {"futex_wake", 2}, {"spin_hint", 0}};
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
        if (name == "futex_wait" || name == "futex_wake" || name == "futex_wait_for") {
            coerce(*args[0], tc_.pointer(tc_.builtin("u32"), false));
            coerce(*args[1], tc_.builtin("u32"));
            if (name == "futex_wait_for") coerce(*args[2], tc_.builtin("u64"));   // the longest wait in nanoseconds
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

    // Why `t` can't be reinterpreted as plain bits (spec 34: std::bit_cast), or "" when it can:
    // numbers and SIMD number vectors, and arrays, tuples and structs of them without padding.
    // Every bit pattern of such a type is a valid value, and every bit of it is part of a value.
    std::string not_plain_bits(const Type* t) {
        if (!t) return "unknown";
        switch (t->kind) {
            case Kind::int_: case Kind::float_: return "";
            case Kind::vector: return not_plain_bits(t->inner);
            case Kind::array: return not_plain_bits(t->inner);
            case Kind::bool_: return "a bool has invalid bit patterns";
            case Kind::char_: return "a char has invalid bit patterns (surrogates, values above 0x10FFFF)";
            case Kind::pointer: case Kind::fn_ptr: return "a pointer is not plain bits";
            case Kind::reference: return "a reference is not plain bits";
            case Kind::tuple: case Kind::user: {
                std::vector<const Type*> parts;
                if (t->is(Kind::tuple)) {
                    parts = t->args;
                } else {
                    if (t->symbol->kind != SymbolKind::struct_) return std::format("{} is not a struct", quoted(t));
                    for (const Symbol* m : t->symbol->member_order) {
                        if (m->kind == SymbolKind::field && !m->is_static && m->decl) {
                            parts.push_back(tc_.lower(*m->decl->as<VariableDecl>().type, sema::substitution_of(m)));
                        }
                    }
                }
                std::uint64_t sum = 0;
                for (const Type* p : parts) {
                    if (std::string why = not_plain_bits(p); !why.empty()) return why;
                    sum += tc_.layout(p).size;
                }
                if (sum != tc_.layout(t).size) return std::format("{} has padding between or after its fields", quoted(t));
                return "";
            }
            default: return std::format("{} is not plain bits", quoted(t));
        }
    }

    // `std::bit_cast<T>(x)` (spec 34): the bits of `x` as a `T` of the same size. Safe: both
    // types must be plain bits (no padding, every bit pattern valid).
    const Type* infer_bit_cast(CallExpr& c, const std::vector<Expr*>& args) {
        auto& seg = c.callee->as<PathExpr>().path.segments.back();
        if (seg.template_args.size() != 1 || !seg.template_args[0].type || args.size() != 1) {
            error(DiagCode::E4030, c.span, "'std::bit_cast' takes a type and a value: 'std::bit_cast<T>(x)'", "wrong arguments");
            return set(c, tc_.error());
        }
        const Type* to   = lower(*seg.template_args[0].type);
        const Type* from = types::strip_ref(args[0]->type);
        if (from && from->is_untyped()) {
            no_context(*args[0], "(write the type of the value)");
            return set(c, tc_.error());
        }
        if (!from || from->is_unknown() || !to || to->is_unknown()) return set(c, to ? to : tc_.error());
        for (const auto& [t, which] : {std::pair{from, "value"}, std::pair{to, "result"}}) {
            if (std::string why = not_plain_bits(t); !why.empty()) {
                note(error(DiagCode::E4010, which == std::string("value") ? args[0]->span : c.span,
                           std::format("std::bit_cast needs plain bits, but the {} type {} is not: {}", which, quoted(t), why), "not plain bits"),
                     "plain bits are numbers, SIMD vectors, and arrays, tuples and structs of them without padding; "
                     "anything else is cast::reinterpret in unsafe code");
                return set(c, to);
            }
        }
        std::uint64_t a = tc_.layout(from).size, b = tc_.layout(to).size;
        if (a != b) {
            error(DiagCode::E4010, c.span, std::format("std::bit_cast between types of different sizes: {} is {} bytes, {} is {}", quoted(from), a,
                                                       quoted(to), b),
                  "different sizes");
        }
        set(c, to);
        if (args[0]->constant) c.constant = evaluate(c, "");
        return to;
    }

    const Type* infer_raw(CallExpr& c, const std::string& name, const std::vector<Expr*>& args) {
        require_unsafe(c.span, std::format("'std::raw::{}'", name));
        if (name == "assume" || name == "unreachable") {   // spec 34: facts for the optimizer
            std::size_t want = name == "assume" ? 1 : 0;
            if (args.size() != want) {
                error(DiagCode::E4030, c.span, std::format("'std::raw::{}' takes {} argument{}", name, want, want == 1 ? "" : "s"), "wrong arguments");
                return set(c, tc_.error());
            }
            if (want) coerce(*args[0], tc_.bool_());
            return set(c, name == "assume" ? tc_.void_() : tc_.never());
        }
        if (name.starts_with("atomic_") || name.starts_with("futex_") || name == "spin_hint") return infer_thread_raw(c, name, args);
        auto& seg = c.callee->as<PathExpr>().path.segments.back();
        std::size_t want = name == "allocate" || name == "free" || name == "read" || name == "drop" ? 1
                         : name == "copy"                                                         ? 3
                                                                                                  : 2;   // reallocate, write, str_from, static_str_from
        bool templ = name == "allocate";
        if (args.size() != want || seg.template_args.size() != (templ ? 1u : 0u)) {
            error(DiagCode::E4030, c.span,
                  std::format("'std::raw::{}' takes {}", name,
                              name == "allocate"     ? "a type and a count: 'std::raw::allocate<T>(n)'"
                              : name == "reallocate" ? "a pointer and a count"
                              : name == "write"      ? "a pointer and a value"
                              : name == "copy"       ? "a destination, a source and a count"
                              : name == "str_from" || name == "static_str_from" ? "a 'std::raw_ptr<u8>' and a length"
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
        if (name == "str_from" || name == "static_str_from") {   // UTF-8 bytes as text (the caller vouches for them;
            const Type* p = pointer(*args[0], false);                // static_str_from: and that they live forever)
            if (p && p->inner != tc_.builtin("u8")) mismatch(args[0]->span, p, tc_.pointer(tc_.builtin("u8"), false));
            count(*args[1]);
            return set(c, name == "str_from" ? tc_.external("std::str") : tc_.static_str());
        }
        const Type* to   = pointer(*args[0], true);   // copy
        const Type* from = pointer(*args[1], false);
        if (to && from && to->inner != from->inner && !deep_unknown(to->inner) && !deep_unknown(from->inner)) {
            mismatch(args[1]->span, from, tc_.pointer(to->inner, false));
        }
        count(*args[2]);
        return set(c, tc_.void_());
    }

    // `std::is_compile_time(param)` and `std::location_of(param)` (spec 23): about the argument
    // a call gives a parameter, so only in a function's compile-time code.
    const Type* infer_parameter_query(CallExpr& c, const std::string& name) {
        const Symbol* loc_sym = std_item("source_location");
        const Type*   result  = name == "is_compile_time" ? tc_.bool_()
                              : name == "literal_text"    ? tc_.static_str()
                              : loc_sym                   ? tc_.user(loc_sym)
                                                          : tc_.error();
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

    // std::compile_error([id,] msg), std::compile_warning(id, msg), std::compile_note(msg) (spec 23).
    // In compile-time code they run when that code runs; elsewhere (a taken `if compile_time`
    // branch) when the code is checked.
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

    // The id of a warning or error is the name of a declared `compile_time std::diagnostic`
    // (spec 23): `std::compile_warning(gfx::deprecated, ...)`, never a string.
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

    // `std::span<T>` of compile-time data: the result of `std::meta::fields<T>()`.
    const Type* list_of(const Type* element) {
        std::string spelling = "std::span<" + types::to_string(element) + ">";
        const Type* t        = tc_.external(spelling);
        if (!tc_.external_args(t)) tc_.set_external_args(t, {element});
        return t;
    }

    // `std::option<A>` for `annotation<A>()`: A must be an annotation (spec 29).
    const Type* option_of_annotation(const TemplateArg& arg, Span span) {
        const Symbol* option = std_item("option");
        if (!arg.type || !option) return tc_.error();
        const Type* a = lower(*arg.type);
        if (a->is_unknown()) return tc_.error();
        if (!a->is_user(SymbolKind::struct_) || !a->symbol->decl->as<TypeDecl>().is_annotation) {
            note(error(DiagCode::E3070, arg.type->span, std::format("{} is not an annotation", quoted(a)), "not an annotation"),
                 "an annotation is declared as 'annotation(std::meta::target::...) struct name { ... }' (spec 29)");
            (void)span;
            return tc_.error();
        }
        return tc_.instance(option, {a});
    }

    // The `std::meta::passing` enum of the library, or an error type without it.
    const Type* passing_type() {
        auto m = program_.modules.find("std::meta");
        const Symbol* p = m == program_.modules.end() ? nullptr : m->second->item("std::meta::passing");
        return p ? tc_.user(p) : tc_.error();
    }

    // `f.name`, `a.payload`, `m.parameters`, ...: the parts of the std::meta handles.
    const Type* infer_meta_member(MemberExpr& m, const Type* t, const std::string& name) {
        bool        field = t == tc_.meta_field();
        bool        alt   = t == tc_.meta_alternative();
        bool        fn    = t == tc_.meta_function();
        bool        param = t == tc_.meta_parameter();
        bool        file  = t == tc_.meta_file();
        const Type* r     = name == "name" || (fn && name == "namespace_name") || (file && name == "module") ? tc_.static_str()
                          : (field || alt) && (name == "index" || (field && name == "offset")) ? tc_.builtin("usize")
                          : (field || param) && name == "type"                               ? tc_.type_value()
                          : alt && name == "payload"                                         ? tc_.type_value()
                          : alt && name == "has_payload"                                     ? tc_.bool_()
                          : fn && name == "result"                                           ? tc_.type_value()
                          : fn && name == "parameters"                                       ? list_of(tc_.meta_parameter())
                          : fn && (name == "is_mut" || name == "is_static")                  ? tc_.bool_()
                          : param && name == "passing"                                       ? passing_type()
                          : param && name == "index"                                         ? tc_.builtin("usize")
                                                                                             : nullptr;
        if (!r) {
            error(DiagCode::E4070, m.name.span, std::format("{} has no field '{}'", quoted(t), name), "unknown member");
            return set(m, tc_.error());
        }
        set(m, r);
        if (m.object->constant) m.constant = evaluate(m, "");
        return r;
    }

    // `f.get(value)`, `a.is(value)`, `a.get(value)`, `a.make(...)` (spec 26). The handle must be
    // a compile-time value: what the call does depends on which field or alternative it is.
    const Type* infer_meta_method(CallExpr& c, MemberExpr& me, const Type* t, const std::string& name, std::vector<Expr*>& args) {
        c.callee->type = tc_.error();
        bool field     = t == tc_.meta_field();
        bool fn        = t == tc_.meta_function();
        bool param     = t == tc_.meta_parameter();
        if (name == "has" || name == "annotation") {   // `f.has<A>()`, `f.annotation<A>()` (spec 29)
            const auto& seg = me.name.segments.back();
            if (seg.template_args.size() != 1 || !seg.template_args[0].type || !args.empty()) {
                error(DiagCode::E4030, c.span, std::format("'{}' takes the annotation as its template argument: '.{}<A>()'", name, name),
                      "wrong arguments");
                return set(c, tc_.error());
            }
            const Type* r = name == "has" ? tc_.bool_() : option_of_annotation(seg.template_args[0], c.span);
            if (name == "has") {
                const Type* a = lower(*seg.template_args[0].type);
                if (!a->is_unknown() && (!a->is_user(SymbolKind::struct_) || !a->symbol->decl->as<TypeDecl>().is_annotation)) {
                    error(DiagCode::E3070, seg.template_args[0].type->span, std::format("{} is not an annotation", quoted(a)),
                          "not an annotation");
                    return set(c, tc_.error());
                }
            }
            set(c, r);
            if (me.object->constant && !r->is_unknown()) c.constant = evaluate(c, "");
            return r;
        }
        if (fn && name == "call") return infer_meta_call(c, me, args);
        bool known     = field ? name == "get" : !fn && !param && (name == "is" || name == "get" || name == "make");
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

    // `m.call(value, args...)` (spec 26): checked and compiled like `value.name(args...)`, so it
    // is rewritten into that call. `m` must be known at compile time.
    const Type* infer_meta_call(CallExpr& c, MemberExpr& me, std::vector<Expr*>& args) {
        const ct::Value* handle = me.object->constant.get();
        if (!handle || handle->kind != ct::Value::Kind::function) {
            if (evaluator().unknown() || types::strip_ref(me.object->type)->is_unknown()) return set(c, tc_.error());
            note(error(DiagCode::E4200, me.object->span, "'.call' needs a std::meta::function that is known at compile time",
                       "not known at compile time"),
                 "use the loop variable of a 'for compile_time' loop over std::meta::methods<T>()");
            return set(c, tc_.error());
        }
        Symbol* method = const_cast<Symbol*>(handle->symbol);
        Span    at     = me.name.span;
        if (method->is_static || method->kind == SymbolKind::function) {   // a static method or a free function
            Path p;
            p.span = at;
            PathSegment seg;
            seg.name = method->name;
            seg.span = at;
            p.segments.push_back(std::move(seg));
            auto callee   = std::make_unique<PathExpr>(at, std::move(p));
            callee->symbol = method;
            c.callee      = std::move(callee);
        } else {
            if (args.empty()) {
                error(DiagCode::E4030, c.span, std::format("'{}' is a method: '.call(value, ...)' needs the value first", method->name),
                      "no value");
                return set(c, tc_.error());
            }
            ExprPtr receiver = std::move(c.args.front());
            c.args.erase(c.args.begin());
            if (receiver->kind == ExprKind::unary && receiver->as<UnaryExpr>().op == UnaryOp::mut_marker) {
                receiver = std::move(receiver->as<UnaryExpr>().operand);
            }
            const Type* rt = types::strip_ref(receiver->type);
            if (!rt->is_unknown() && handle->type_value && rt != handle->type_value) {
                mismatch(receiver->span, receiver->type, handle->type_value, " (the value whose method is called)");
                return set(c, tc_.error());
            }
            Path p;
            p.span = at;
            PathSegment seg;
            seg.name = method->name;
            seg.span = at;
            p.segments.push_back(std::move(seg));
            auto callee    = std::make_unique<MemberExpr>(Span{receiver->span.file, receiver->span.begin, at.end}, std::move(receiver), std::move(p), false);
            c.callee       = std::move(callee);
        }
        return infer_call(c);
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
        // Arguments the compiler supplies for `implicit` parameters the call leaves out (spec 31).
        std::vector<ExprPtr>                    synth;
        // The hidden template parameter of each `compile_time` parameter and its argument.
        std::vector<std::pair<const Symbol*, Expr*>> ct_args;
    };

    // Whether `at` can be an instance of what the parameter pattern names: `std::span<T>` never
    // accepts a `std::span<mut f32>`, so such a template overload is not viable.
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
            if (Symbol* sm = sym->module->item("std::span_mut")) sym = sm;
        }
        at = types::strip_ref(at);
        return at->is(Kind::user) && at->symbol->template_of == sym;
    }

    // The template parameter symbol a parameter type is spelled as (`T` or `T&`), if any.
    static const Symbol* param_template_symbol(const TypeExpr& t) {
        const TypeExpr* inner = t.kind == TypeKind::reference ? t.as<ReferenceType>().inner.get() : &t;
        if (inner->kind != TypeKind::named) return nullptr;
        const Symbol* s = inner->as<NamedType>().symbol;
        return s && s->kind == SymbolKind::type_param ? s : nullptr;
    }

    // Template parameter symbols of a function, by matching the names used in its signature.
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

    // Whether `t` is unknown or built from an unknown type (`std::raw_ptr<node<T>>` in a generic check).
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

    // The template parameters a parameter type mentions stay unknown (an argument whose type is
    // not known yet, inside a template's generic check): no "cannot deduce" error.
    static void unknown_params(const TypeExpr& pattern, const Type* unknown, std::map<const Symbol*, const Type*>& subst) {
        if (pattern.kind == TypeKind::reference) {
            unknown_params(*pattern.as<ReferenceType>().inner, unknown, subst);
            return;
        }
        if (pattern.kind != TypeKind::named) return;
        const auto& nt = pattern.as<NamedType>();
        if (nt.symbol && (nt.symbol->kind == SymbolKind::type_param || nt.symbol->kind == SymbolKind::value_param) &&
            !subst.count(nt.symbol)) {
            subst[nt.symbol] = unknown;
        }
        for (const PathSegment& seg : nt.path.segments) {
            for (const TemplateArg& a : seg.template_args) {
                if (a.type) unknown_params(*a.type, unknown, subst);
                if (a.value && a.value->kind == ExprKind::path) {   // a value parameter: `N` of `box<N>&`
                    const Symbol* v = a.value->as<PathExpr>().symbol;
                    if (v && v->kind == SymbolKind::value_param && !subst.count(v)) subst[v] = unknown;
                }
            }
        }
    }

    // Deduces template parameters by matching a parameter's type against an argument's type:
    // `std::darray<T>&` against `std::darray<u32>` gives T = u32 (spec 14).
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
        if (sym->kind == SymbolKind::value_param) {   // `A` of `functions<A>&`: the argument's value
            if (!subst.count(sym)) subst[sym] = at;
            return;
        }
        if (!nt.path.segments.empty()) {   // `std::span<mut T>` is the library's span_mut<T> (spec 20)
            const auto& ta = nt.path.segments.back().template_args;
            if (sym->name == "span" && sym->module && sym->module->name == "std" && ta.size() == 1 && ta[0].is_mut && ta[0].type &&
                ta[0].type->kind != TypeKind::reference) {
                if (Symbol* sm = sym->module->item("std::span_mut")) sym = sm;
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
                if (v && (v->kind == SymbolKind::type_param || v->kind == SymbolKind::value_param) && !subst.count(v)) subst[v] = given[i];
            }
        }
    }

    // --- implicit and compile_time parameters (spec 31) ----------------------------------------

    // A candidate for a call: missing trailing `implicit` arguments are supplied, and the
    // arguments of `compile_time` parameters are noted for call_target.
    Candidate make_call_candidate(Symbol* s, const std::vector<Expr*>& given, const PathSegment* explicit_args, Span at) {
        const FunctionDecl*  f    = s->function();
        std::vector<Expr*>   args = given;
        std::vector<ExprPtr> synth;
        if (f && given.size() < f->params.size()) {
            bool all_implicit = true;
            for (std::size_t i = given.size(); i < f->params.size(); ++i) all_implicit = all_implicit && f->params[i].is_implicit;
            if (all_implicit) {
                const auto* owner = f->is_template ? nullptr : sema::substitution_of(s);
                for (std::size_t i = given.size(); i < f->params.size(); ++i) {
                    const Param& p  = f->params[i];
                    const Type*  pt = owner ? tc_.lower(*p.type, owner) : lower(*p.type);
                    synth.push_back(implicit_argument(pt, at));
                    args.push_back(synth.back().get());
                }
            }
        }
        Candidate c = make_candidate(s, args, explicit_args);
        c.synth     = std::move(synth);
        if (f && f->is_template) {
            std::vector<const Symbol*> syms = template_symbols(*f);
            for (std::size_t i = 0; i < f->params.size() && i < args.size(); ++i) {
                if (!f->params[i].is_compile_time) continue;
                const Symbol* hidden = nullptr;
                for (std::size_t k = 0; k < f->template_params.size() && k < syms.size(); ++k) {
                    if (f->template_params[k].name == "$" + f->params[i].name) hidden = syms[k];
                }
                c.ct_args.emplace_back(hidden, args[i]);
            }
        }
        return c;
    }

    // The call takes over the arguments supplied for its `implicit` parameters (spec 31).
    static void adopt_implicit(CallExpr& c, const Candidate& chosen) {
        for (ExprPtr& s : const_cast<Candidate&>(chosen).synth) c.args.push_back(std::move(s));
    }

    // The path of the file `span` is in, as the compiler was given it.
    std::string source_path(Span span) const;

    // The function being checked, with its namespace and type: `gfx::vulkan::create_pipeline`.
    std::string current_function_name() const {
        std::string name = fn_.decl ? fn_.decl->name : fn_.name;
        std::string prefix;
        if (fn_.self) {
            prefix = fn_.self->key();
        } else if (fn_.decl) {
            prefix = fn_.decl->ns;
        }
        if (name.empty()) return prefix;
        return prefix.empty() ? name : prefix + "::" + name;
    }

    // The symbol of the function being checked (null at file scope and in lambdas).
    const Symbol* current_function_symbol() const {
        if (!fn_.decl) return nullptr;
        if (fn_.self) {
            auto it = fn_.self->members.find(fn_.decl->name);
            if (it != fn_.self->members.end()) {
                for (const Symbol* m : it->second) {
                    if (m->decl == fn_.decl || m->definition == fn_.decl) return m;
                }
            }
        } else if (module_) {
            auto it = module_->items.find(fn_.decl->ns.empty() ? fn_.decl->name : fn_.decl->ns + "::" + fn_.decl->name);
            if (it != module_->items.end()) {
                for (const Symbol* s : it->second) {
                    if (s->decl == fn_.decl || s->definition == fn_.decl) return s;
                }
            }
        }
        for (const Symbol* s : program_.instances) {   // an instance of a function template
            if (s->decl == fn_.decl) return s;
        }
        return nullptr;
    }

    // The `std::call_site` value of code at `span` (spec 31): static data computed here.
    std::shared_ptr<const ct::Value> call_site_value(Span span) {
        const Symbol* cs = std_item("call_site");
        if (!cs) return nullptr;
        LineCol lc = sources_line(span);
        auto text = [&](std::string s) {
            ct::Value v;
            v.kind   = ct::Value::Kind::string;
            v.type   = tc_.static_str();
            v.string = std::move(s);
            return v;
        };
        std::string file = source_path(span);
        if (file.starts_with("./")) file = file.substr(2);
        std::map<std::string, ct::Value> by_name;
        by_name.emplace("file", text(file));
        by_name.emplace("file_name", text(file.substr(file.find_last_of('/') == std::string::npos ? 0 : file.find_last_of('/') + 1)));
        by_name.emplace("function", text(current_function_name()));
        by_name.emplace("function_name", text(fn_.decl ? fn_.decl->name : fn_.name));
        by_name.emplace("namespace_name", text(fn_.self ? fn_.self->ns : fn_.decl ? fn_.decl->ns : std::string()));
        by_name.emplace("module", text(module_ ? module_->name : std::string()));
        by_name.emplace("target", text(program_.build_target));
        by_name.emplace("line", ct::Value::of_integer(lc.line, tc_.builtin("u32")));
        by_name.emplace("column", ct::Value::of_integer(lc.column, tc_.builtin("u32")));
        auto v      = std::make_shared<ct::Value>();
        v->kind     = ct::Value::Kind::aggregate;
        v->type     = tc_.user(cs);
        v->location = span;                        // for std::meta::file_annotation
        v->symbol   = current_function_symbol();   // for std::meta::function_annotation / type_annotation
        for (const Symbol* m : cs->member_order) {
            if (m->kind != SymbolKind::field || m->is_static) continue;
            auto it = by_name.find(m->name);
            v->elements.push_back(it != by_name.end() ? it->second : ct::Value{});
        }
        return v;
    }

    // The argument the compiler supplies for an `implicit` parameter of type `pt` at a call at
    // `at` (spec 31): the call site itself, or `T::from_call(<call site>)` evaluated now.
    ExprPtr implicit_argument(const Type* pt, Span at) {
        auto e  = std::make_unique<SimpleExpr>(ExprKind::default_, at);
        e->type = pt;
        const Type*   t  = types::strip_ref(pt);
        const Symbol* cs = std_item("call_site");
        std::shared_ptr<const ct::Value> site = call_site_value(at);
        if (t->is_unknown()) {
            e->type = tc_.error();
            return e;
        }
        if (cs && t->is(Kind::user) && t->symbol == cs) {
            e->constant = site;
            return e;
        }
        Symbol* from = nullptr;
        if (t->is(Kind::user)) {
            for (Symbol* m : find_methods(t->symbol, "from_call")) {
                const FunctionDecl* fd = m->function();
                if (fd && fd->is_static && fd->params.size() == 1) from = m;
            }
        }
        if (!from || !site || !cs) {
            Diagnostic& d = error(DiagCode::E4250, at,
                                  std::format("{} can't be supplied for an implicit parameter: it has no "
                                              "'compile_time static {} from_call(std::call_site where)'",
                                              quoted(t), types::to_string(t)),
                                  "argument left out");
            note(d, "implement std::implicit<T> for the type, or write the argument");
            e->type = tc_.error();
            return e;
        }
        auto site_expr      = std::make_unique<SimpleExpr>(ExprKind::default_, at);
        site_expr->type     = tc_.user(cs);
        site_expr->constant = site;
        Path path;
        path.span = at;
        path.segments.push_back(PathSegment{t->symbol->name, at, false, {}});
        path.segments.push_back(PathSegment{"from_call", at, false, {}});
        auto callee    = std::make_unique<PathExpr>(at, std::move(path));
        callee->symbol = from;
        auto call      = std::make_unique<CallExpr>(at, std::move(callee));
        call->args.push_back(std::move(site_expr));
        infer(*call);
        e->constant = evaluate(*call, std::format("the implicit argument {} is made at compile time", quoted(t)));
        if (!e->constant) e->type = tc_.error();
        implicit_calls_.push_back(std::move(call));
        return e;
    }
    std::vector<ExprPtr> implicit_calls_;   // the `T::from_call(...)` calls evaluated for implicit arguments

    // The template value argument for a `compile_time` parameter (spec 31): any compile-time value.
    const Type* compile_time_argument(Expr& arg, std::string_view display) {
        const Type* t = types::strip_ref(arg.type);
        if (!t || t->is_unknown() || t->is_untyped()) return tc_.error();
        std::shared_ptr<const ct::Value> v = arg.constant;
        if (!v) {
            if (fn_.generic) return tc_.error();
            v = evaluate(arg, std::format("the argument of a compile_time parameter of '{}' must be known at compile time", display));
        }
        if (!v) return tc_.error();
        if (v->kind == ct::Value::Kind::boolean) return tc_.value_arg(u128{v->boolean}, t);
        if (v->kind == ct::Value::Kind::integer && t->is_integer()) return tc_.value_arg(v->integer, t);
        return tc_.value_arg_of(ct::to_string(*v), t, v);
    }

    Candidate make_candidate(Symbol* s, const std::vector<Expr*>& args, const PathSegment* explicit_args) {
        if (const FunctionDecl* pf = s->function(); pf && has_pack(*pf) && instantiator_) {
            // A pack template becomes an ordinary template for this many arguments.
            std::size_t fixed = 0;
            for (const Param& p : pf->params) {
                if (!p.is_pack && !p.is_implicit) ++fixed;
            }
            std::size_t n = args.size() > fixed ? args.size() - fixed : 0;
            if (!pf->template_params.empty() && !pf->template_params.back().is_type && pf->template_params.back().is_pack) {
                // a value pack: as many as the explicit arguments after the fixed parameters
                std::size_t given = explicit_args ? explicit_args->template_args.size() : 0;
                n = given + 1 > pf->template_params.size() ? given + 1 - pf->template_params.size() : 0;
            }
            if (Symbol* expanded = instantiator_pack_variant(s, n)) s = expanded;
        }
        const FunctionDecl& f = s->function_decl();
        Candidate c{s, &f, {}, {}, nullptr, 0, false, {}, false, {}, {}};
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
                    } else if (s->pack_origin && at == tc_.untyped_float() && tp->name.find('$') != std::string::npos) {
                        c.subst[tp] = tc_.builtin("f64");   // an element of a pack: a float literal is an f64
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

    // Cost of passing `arg` to a parameter of type `param`, or -1.
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

    // The function a call to `chosen` runs (spec 14): for a function template, its instance for the
    // deduced arguments. Arguments that are still unknown (inside a template that is not instantiated)
    // keep the template itself. Null, after reporting, when a parameter could not be deduced.
    Symbol* call_target(const Candidate& chosen, Span span, std::string_view display) {
        if (!chosen.decl->is_template) return chosen.symbol;
        std::vector<const Symbol*> syms = template_symbols(*chosen.decl);
        std::vector<const Type*>   args;
        std::map<const Symbol*, const Type*> subst = chosen.subst;
        for (const auto& [hidden, arg] : chosen.ct_args) {   // `compile_time` parameters (spec 31)
            if (hidden) subst[hidden] = compile_time_argument(*arg, display);
        }
        for (const Symbol* p : syms) {
            auto it = p ? subst.find(p) : subst.end();
            if (it == subst.end()) {
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
        if (chosen.symbol->kind == SymbolKind::method) return instantiate_method(chosen.symbol, args, span);
        return instantiate_function(chosen.symbol, args, span);
    }

    // Picks an overload, reports problems, coerces the arguments. Returns the chosen symbol.
    const Candidate* resolve_overload(std::vector<Candidate>& cands, std::vector<Expr*>& args, Span span,
                                      std::string_view display) {
        std::vector<Candidate*> arity;
        for (Candidate& c : cands) {
            if (c.params.size() == args.size() + c.synth.size()) arity.push_back(&c);
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
            for (std::size_t i = 0; i < c->synth.size() && ok; ++i) {   // supplied `implicit` arguments (spec 31)
                if (c->synth[i]->type->is_unknown()) continue;
                if (arg_cost(*c->synth[i], c->params[args.size() + i]) < 0) ok = false;
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
        for (const ExprPtr& s : chosen->synth) args.push_back(s.get());   // the caller takes them over (adopt_implicit)
        for (std::size_t i = 0; i < args.size(); ++i) check_argument(*args[i], chosen->params[i]);
        return chosen;
    }

    // Call-site `mut` (spec 04) and the argument's type.
    // A field of a packed struct (spec 34), reached through member accesses: `h.size`, `w.header.size`.
    bool in_packed_struct(const Expr& e) {
        if (e.kind != ExprKind::member) return false;
        const auto& me = e.as<MemberExpr>();
        const Type* ot = types::strip_ref(me.object->type);
        if (ot && ot->is(Kind::user) && ot->symbol && ot->symbol->kind == SymbolKind::struct_ && types::is_packed(ot->symbol)) return true;
        return in_packed_struct(*me.object);
    }

    void reject_packed_reference(const Expr& e, std::string_view what) {
        if (!in_packed_struct(e)) return;
        note(error(DiagCode::E4210, e.span, std::format("a field of a packed struct can't be {}: it may be misaligned", what), "packed field"),
             "copy it into a local first, and assign it back if it changes");
    }

    void check_argument(Expr& arg, const Type* param) {
        bool marked  = arg.kind == ExprKind::unary && arg.as<UnaryExpr>().op == UnaryOp::mut_marker;
        bool mut_ref = param->is(Kind::reference) && param->is_mut;
        Expr& value  = marked ? *arg.as<UnaryExpr>().operand : arg;
        if (param->is(Kind::reference)) reject_packed_reference(value, "passed by reference");
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
        if (param->is(Kind::external) && param->name == "std::format_args") {
            // A plain literal passed as a std::format_args is an f-string without holes (spec 31).
            if (value.kind == ExprKind::string_literal && value.as<LiteralExpr>().token_kind == TokenKind::string_literal) {
                auto&      lit = value.as<LiteralExpr>();
                FormatPart part;
                part.text      = lit.value;
                part.span      = lit.span;
                lit.token_kind = TokenKind::format_string_literal;
                lit.format_parts.clear();
                lit.format_parts.push_back(std::move(part));
                set(value, tc_.external("std::format_args"));
            }
            check_lazy_holes(value);
        }
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
        // An extern(c) function written in Edax (one with a body) is safe to call from Edax.
        bool foreign = f->is_extern && !f->body && !(callee->definition && callee->definition->as<FunctionDecl>().body);
        if (!f->is_unsafe && !foreign) return;
        note(error(DiagCode::E4101, span,
                   std::format("'{}' is {} and can only be called inside 'unsafe'", callee->name,
                               foreign ? "an extern function" : "an unsafe function"),
                   "needs 'unsafe'"),
             "wrap the call in 'unsafe { ... }'");
    }

    const Type* infer_call(CallExpr& c) {
        call_marks_[&c] = diags_.size();   // errors from here on that a failing entry check suppresses
        if (c.dispatch_hint) {   // @std::dispatch_hint<T> (performance §8): a class or struct with methods
            const Type* h = lower(*c.dispatch_hint);
            if (h->is(Kind::user) && (h->symbol->kind == SymbolKind::class_ || h->symbol->kind == SymbolKind::struct_)) {
                c.hint_symbol = const_cast<Symbol*>(h->symbol);
            } else if (!h->is_unknown()) {
                error(DiagCode::E4210, c.dispatch_hint->span, std::format("@std::dispatch_hint names a class or struct, not {}", quoted(h)),
                      "not a class");
            }
        }
        if (c.wrapped) {   // `std::meta::wrapped_body()`: the function's own body; its returns give the value
            bool saved_unsafe = unsafe_depth_ > 0;
            (void)saved_unsafe;
            infer_block(*c.wrapped);
            return set(c, fn_.result ? fn_.result : tc_.void_());
        }
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
            if (std::string r = ct::raw_intrinsic(c); r == "bit_cast") return infer_bit_cast(c, args);
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
                if (const Type* vt = vector_of_static_path(pe.path)) return infer_vector_static(c, vt, pe.path.segments.back().name, args);
                if (pe.path.segments.size() == 3 && pe.path.segments[0].name == "std" && pe.path.segments[1].name == "simd") {
                    return infer_simd_free(c, pe.path.segments[2], args);
                }
                if (pe.path.segments.size() == 4 && pe.path.segments[0].name == "std" && pe.path.segments[1].name == "simd" &&
                    (pe.path.segments[2].name == "x86" || pe.path.segments[2].name == "arm")) {
                    return infer_simd_intrinsic(c, pe.path, args);
                }
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
                const auto& segs = pe.path.segments;
                static const std::set<std::string> printing{"print", "println", "eprint", "eprintln"};
                bool plain = segs.size() == 2 || (segs.size() == 3 && segs[1].name == "meta" && !segs[1].has_template_args);
                if (plain && segs[0].name == "std" && !(segs.size() == 2 && printing.contains(segs[1].name)) && !fn_.generic) {
                    // The resolver leaves std paths it can't find to the compiler's own names.
                    std::string ns = segs[0].name;
                    for (std::size_t i = 1; i + 1 < segs.size(); ++i) ns += "::" + segs[i].name;
                    error(DiagCode::E3020, segs.back().span, std::format("cannot find '{}' in namespace '{}'", segs.back().name, ns),
                          "not found");
                } else if (segs.size() == 3 && segs[0].name == "std" && !segs[1].has_template_args && !fn_.generic) {
                    // `std::str::nope(...)`: a static function the std type doesn't have.
                    const Symbol* owner = std_item(segs[1].name);
                    if ((owner && owner->is_type() && owner->decl && !owner->decl->is_template) || segs[1].name == "str" || segs[1].name == "static_str") {
                        error(DiagCode::E3025, segs.back().span, std::format("'std::{}' has no member '{}'", segs[1].name, segs.back().name),
                              "not found");
                    }
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
                if (s->kind == SymbolKind::function) {
                    // One namespace's overloads, from every module this one can see (a namespace
                    // may span several modules, spec 30).
                    for (const Module* m : visible_modules()) {
                        auto it = m->items.find(s->key());
                        if (it == m->items.end()) continue;
                        for (Symbol* o : it->second) {
                            if (o->kind == s->kind && (m == module_ || !o->is_private)) overloads.push_back(o);
                        }
                    }
                } else {
                    for (Symbol* o : s->parent->members[s->name]) {
                        if (o->kind == s->kind) overloads.push_back(o);
                    }
                }
                std::vector<Candidate> cands;
                for (Symbol* o : overloads) cands.push_back(make_call_candidate(o, args, &pe.path.segments.back(), c.span));
                const Candidate* chosen = resolve_overload(cands, args, c.span, ast::to_string(pe.path));
                if (!chosen) return set(c, tc_.error());
                adopt_implicit(c, *chosen);
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
            if (t->is(Kind::vector) || t->is(Kind::mask)) return infer_vector_method(c, me, t, name, args);
            if (types::is_number_method(t, name)) return infer_number_method(c, t, name, args);
            if ((is_std_instance(t, "span") || is_std_instance(t, "span_mut")) && (name == "load" || name == "store") &&
                !args.empty() && is_simd_block(types::strip_ref(args[0]->type))) {
                return infer_span_block(c, me, t, name, args);
            }
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
                if (t->is_integer() || t->is_float() || t->is(Kind::bool_) || t->is(Kind::char_)) {
                    Diagnostic& d = error(DiagCode::E4070, me.name.span, std::format("{} has no method '{}'", quoted(t), name), "unknown method");
                    if (t->is_integer() && types::is_number_method(tc_.builtin("f64"), name)) {
                        note(d, std::format("'{}' is a method of f32 and f64: convert first, e.g. 'cast<f64>(x).{}()'", name, name));
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
            if (!methods.empty() && methods[0]->function() && !methods[0]->function_decl().is_static) {
                reject_packed_reference(*me.object, "the receiver of a method");
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
            for (Symbol* m : methods) cands.push_back(make_call_candidate(m, args, &me.name.segments.back(), c.span));
            const Candidate* chosen = resolve_overload(cands, args, c.span, t->symbol->name + "::" + name);
            if (!chosen) return set(c, tc_.error());
            adopt_implicit(c, *chosen);
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
            // A method template's checks are those of its instance (its parameters' types known).
            run_entry_checks(me.symbol ? me.symbol : chosen->symbol, c, args);
            return chosen->result;
        }

        // Calling a value: `make()(x)`, `handlers[i](x)` on a lambda or std::function (spec 18).
        const Type* ct = types::strip_ref(infer(callee));
        if (ct->is(Kind::closure) || ct->is(Kind::function) || ct->is(Kind::fn_ptr)) return infer_closure_call(c, ct, args);
        for (Expr* a : args) {
            if (a->type && a->type->is_untyped()) settle(*a, tc_.error());
        }
        return set(c, tc_.error());
    }

    // --- other expressions ----------------------------------------------------------------

    // The underlying type of a `distinct type` (spec 34), or null.
    const Type* distinct_underlying(const Type* t) {
        if (!t || !t->is(Kind::user) || !t->symbol || !t->symbol->decl || t->symbol->kind != SymbolKind::struct_) return nullptr;
        const auto& td = t->symbol->decl->as<TypeDecl>();
        if (!td.is_distinct) return nullptr;
        for (const Symbol* m : t->symbol->member_order) {
            if (m->kind == SymbolKind::field && !m->is_static) return tc_.lower(*m->decl->as<VariableDecl>().type, sema::substitution_of(m));
        }
        return nullptr;
    }

    // The underlying type of a distinct type with @std::derive::arithmetic (spec 34), or null.
    const Type* distinct_arithmetic(const Type* t) {
        const Type* u = distinct_underlying(t);
        if (!u || !sema::find_annotation(t->symbol->decl->annotations, "std::derive::arithmetic")) return nullptr;
        return u;
    }

    // Replaces `e` (a value of a distinct type) with `cast<underlying>(e)`, already checked.
    void to_underlying(ExprPtr& e, const Type* u) {
        auto c     = std::make_unique<CastExpr>(e->span);
        c->type    = u;
        if (e->constant && e->constant->kind == ct::Value::Kind::aggregate && !e->constant->elements.empty()) {
            c->constant = std::make_shared<const ct::Value>(e->constant->elements[0]);
        }
        c->operand = std::move(e);
        e          = std::move(c);
    }

    // `a + b`, `d * 2.0`, `a / b`, `a < b` on distinct types with @std::derive::arithmetic: the
    // underlying type's operation, rewritten on casts (spec 34).
    const Type* infer_distinct_binary(BinaryExpr& b, const Type* l, const Type* r) {
        TokenKind   op = b.op;
        const Type* ul = distinct_arithmetic(l);
        const Type* ur = distinct_arithmetic(r);
        auto fail = [&](std::string why) {
            note(error(DiagCode::E4013, b.span, std::format("operator '{}' is not defined for {} and {}", token_kind_spelling(op), quoted(l), quoted(r)),
                       "no such operator"),
                 why);
            return set(b, tc_.error());
        };
        auto finish = [&](const Type* result, const Type* wrap) {
            b.distinct_result = wrap;
            set(b, result);
            if (b.lhs->constant && b.rhs->constant) b.constant = evaluate(b, "");
            return result;
        };
        bool same = l == r;
        bool eq = op == TokenKind::eq_eq || op == TokenKind::bang_eq;
        if (eq || op == TokenKind::spaceship || is_comparison(op)) {
            if (!same) return fail("values of a distinct type compare only with values of the same type");
            to_underlying(b.lhs, ul);
            to_underlying(b.rhs, ul);
            return finish(op == TokenKind::spaceship ? ordering_type() : tc_.bool_(), nullptr);
        }
        if (op == TokenKind::plus || op == TokenKind::minus || op == TokenKind::percent) {
            if (!same) {
                return fail(std::format("'{}' takes two values of the same distinct type; scale with '*' and '/' by {}",
                                        token_kind_spelling(op), quoted(ul ? ul : ur)));
            }
            to_underlying(b.lhs, ul);
            to_underlying(b.rhs, ul);
            return finish(l, l);
        }
        if (op == TokenKind::star) {
            if (ul && (r == ul || r->is_untyped())) {   // meters * f32
                coerce(*b.rhs, ul);
                to_underlying(b.lhs, ul);
                return finish(l, l);
            }
            if (ur && (l == ur || l->is_untyped())) {   // f32 * meters
                coerce(*b.lhs, ur);
                to_underlying(b.rhs, ur);
                return finish(r, r);
            }
            return fail(std::format("a distinct type is scaled by its underlying type: {} * {}", quoted(ul ? l : r), quoted(ul ? ul : ur)));
        }
        if (op == TokenKind::slash) {
            if (ul && (r == ul || r->is_untyped())) {   // meters / f32
                coerce(*b.rhs, ul);
                to_underlying(b.lhs, ul);
                return finish(l, l);
            }
            if (same) {   // meters / meters: a plain ratio
                to_underlying(b.lhs, ul);
                to_underlying(b.rhs, ul);
                return finish(ul, nullptr);
            }
            return fail(std::format("a distinct type is divided by its underlying type or by itself"));
        }
        return fail(std::format("@std::derive::arithmetic gives + - * / % and comparisons"));
    }

    const Type* infer_cast(CastExpr& c) {
        const Type* to   = lower(*c.target);
        const Type* from = types::strip_ref(infer(*c.operand));
        set(c, to);
        const std::string& v = c.variant;
        // `cast<meters>(x)` and `cast<f32>(m)`: a distinct type and its underlying type (spec 34).
        if (v.empty() && (distinct_underlying(to) || distinct_underlying(from))) {
            const Type* under_to   = distinct_underlying(to);
            const Type* under_from = distinct_underlying(from);
            if (under_to && (from->is_untyped() || from == under_to)) {
                coerce(*c.operand, under_to);
                if (c.operand->constant) c.constant = evaluate(c, "");
                return to;
            }
            if (under_from && to == under_from) {
                if (c.operand->constant) c.constant = evaluate(c, "");
                return to;
            }
            if (from->is_unknown() || to->is_unknown()) return to;
            const Type* expected = under_to ? under_to : under_from;
            note(error(DiagCode::E4091, c.span, std::format("cast cannot convert {} to {}", quoted(from), quoted(to)), "not its underlying type"),
                 std::format("a distinct type converts only to and from its underlying type, {}; convert in two steps", quoted(expected)));
            return to;
        }
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
            for (const Symbol* b = root->base_class(); b; b = b->base_class()) root = b;
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
        if (from->is(Kind::vector) || to->is(Kind::vector)) {   // lane by lane, the scalar rules (SIMD, section 7)
            if (!from->is(Kind::vector) || !to->is(Kind::vector) || from->bits != to->bits) {
                error(DiagCode::E4091, c.span, std::format("casts convert vectors with the same number of lanes, not {} to {}", quoted(from), quoted(to)),
                      "different lanes");
                return c.type;
            }
            if (v == "checked") {
                error(DiagCode::E4092, c.span, "cast::checked on vectors is not supported yet", "not supported");
                return c.type;
            }
            if (v.empty() && !lossless(from->inner, to->inner) && from->inner != to->inner) {
                note(error(DiagCode::E4090, c.span, std::format("cast<{}> from {} may lose data", types::to_string(to), quoted(from)), "lossy conversion"),
                     std::format("say how: 'cast::truncate<{0}>', 'cast::saturate<{0}>' or 'cast::round<{0}>'", types::to_string(to)));
            }
            return c.type;
        }
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
                if (m->kind == SymbolKind::field && !m->is_static && (!m->decl->as<VariableDecl>().init || m->no_default)) {
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

    // --- tuples and list literals ------------------------------------------------------------

    // `(a, b)`: a tuple of the elements' types, or untyped while an element is an untyped literal.
    const Type* infer_tuple(TupleExpr& te) {
        std::vector<const Type*> elements;
        bool                     untyped = false;
        for (ExprPtr& x : te.elements) {
            const Type* t = types::strip_ref(infer(*x));
            if (t->is_untyped()) untyped = true;
            if (t->is(Kind::void_) || t->is(Kind::never)) {
                error(DiagCode::E4010, x->span, "a tuple element needs a value", "no value");
                t = tc_.error();
            }
            elements.push_back(t);
        }
        if (untyped) return set(te, tc_.untyped_list());
        for (const Type* t : elements) {
            if (t->is(Kind::error)) return set(te, tc_.error());
        }
        return set(te, tc_.tuple(std::move(elements)));
    }

    // A tuple value where the tuple type `t` is expected: each element gets its element type.
    void settle_tuple(TupleExpr& te, const Type* t) {
        if (t->is(Kind::tuple) && t->args.size() == te.elements.size()) {
            for (std::size_t i = 0; i < te.elements.size(); ++i) coerce(*te.elements[i], t->args[i]);
            te.type = t;
            return;
        }
        for (ExprPtr& x : te.elements) {
            if (x->type && x->type->is_untyped()) settle(*x, tc_.error());
        }
        if (!t->is_unknown()) {
            error(DiagCode::E4010, te.span,
                  t->is(Kind::tuple) ? std::format("expected {}, found a tuple of {} elements", quoted(t), te.elements.size())
                                     : std::format("expected {}, found a tuple", quoted(t)),
                  "wrong type");
        }
        te.type = tc_.error();
    }

    // The element type and count a list literal takes in `t`: `std::array<T, N>` (count N) or
    // `std::span<T>` / `std::span<mut T>` (any count). Null when `t` takes no list literal.
    const Type* list_element(const Type* t, std::optional<std::uint64_t>& count) {
        count.reset();
        if (is_std_instance(t, "array") && t->symbol->template_args.size() == 2) {
            count = static_cast<std::uint64_t>(tc_.value_bits(t->symbol->template_args[1]));
            return t->symbol->template_args[0];
        }
        if ((is_std_instance(t, "span") || is_std_instance(t, "span_mut")) && t->symbol->template_args.size() == 1) {
            return t->symbol->template_args[0];
        }
        if (t->is(Kind::array)) {
            count = tc_.array_count(t);
            return t->inner;
        }
        if (t->is(Kind::vector)) {   // `f32x8 ramp = [0.0, 1.0, ...]`: exactly N lanes
            count = static_cast<std::uint64_t>(t->bits);
            return t->inner;
        }
        return nullptr;
    }

    // `[a, b, c]` where `t` is expected (spec: tuples and list literals).
    void settle_list(ListExpr& l, const Type* t) {
        std::optional<std::uint64_t> count;
        const Type*                  element = t->is_unknown() ? nullptr : list_element(t, count);
        if (!element) {
            for (ExprPtr& x : l.elements) {
                if (x->type && x->type->is_untyped()) settle(*x, tc_.error());
            }
            if (!t->is_unknown()) {
                note(error(DiagCode::E4253, l.span, std::format("a list literal can't be {}", quoted(t)), "list literal"),
                     "a list literal becomes a std::array<T, N> or a std::span<T>; a std::darray is made with std::darray<T>::from([...])");
            }
            l.type = tc_.error();
            return;
        }
        if (count && *count != l.elements.size()) {
            error(DiagCode::E4253, l.span,
                  std::format("{} has {} element{}, but the list has {}", quoted(t), *count, *count == 1 ? "" : "s",
                              l.elements.size()),
                  "wrong count");
        }
        for (ExprPtr& x : l.elements) coerce(*x, element);
        l.type = t;
    }

    // Whether values of `t` can be compared with `==` (and ordered, when `ordered`) element by
    // element inside a tuple.
    bool tuple_comparable(const Type* t, bool ordered) {
        t = types::strip_ref(t);
        if (t->is_unknown() || t->is_numeric() || t->is(Kind::char_) || t->is(Kind::static_str)) return true;
        if (t->is(Kind::external) && t->name == "std::str") return true;
        if (t->is(Kind::bool_)) return !ordered;
        if (is_std_string(t)) return !ordered;
        if (t->is_user(SymbolKind::enum_) && !enum_has_payload(t->symbol)) return !ordered;
        if (t->is(Kind::tuple)) {
            return std::all_of(t->args.begin(), t->args.end(), [&](const Type* e) { return tuple_comparable(e, ordered); });
        }
        return false;
    }

    // `(T1 a, T2 b) = expr;` / `auto (a, b) = expr;`: the tuple is consumed into new variables.
    void check_destructure(DestructureStmt& d) {
        const Type* t = infer(*d.init);
        if (t->is_untyped() && d.init->kind == ExprKind::tuple) {
            auto& te = d.init->as<TupleExpr>();
            if (te.elements.size() == d.bindings.size()) {
                std::vector<const Type*> elements;
                for (std::size_t i = 0; i < te.elements.size(); ++i) {
                    const DestructureBinding& b = d.bindings[i];
                    if (b.type) {
                        coerce(*te.elements[i], lower(*b.type));
                    } else if (te.elements[i]->type->is_untyped()) {
                        no_context(*te.elements[i], d.is_auto ? "('auto' needs values whose types are known on their own)"
                                                              : "('_' gives it no type)");
                    }
                    elements.push_back(types::strip_ref(te.elements[i]->type));
                }
                t       = tc_.tuple(std::move(elements));
                te.type = t;
            }
        } else if (t->is_untyped()) {
            no_context(*d.init, "(a destructuring declaration needs a tuple)");
            t = tc_.error();
        }
        t = types::strip_ref(t);
        if (!t->is(Kind::tuple) && !t->is_unknown()) {
            error(DiagCode::E4010, d.init->span, std::format("only a tuple can be destructured, but this is {}", quoted(t)),
                  "not a tuple");
            t = tc_.error();
        } else if (t->is(Kind::tuple) && t->args.size() != d.bindings.size()) {
            error(DiagCode::E4010, d.init->span,
                  std::format("this tuple has {} elements, but {} names are given", t->args.size(), d.bindings.size()),
                  "wrong number of names");
            t = tc_.error();
        }
        for (std::size_t i = 0; i < d.bindings.size(); ++i) {
            DestructureBinding& b  = d.bindings[i];
            const Type*         el = t->is(Kind::tuple) ? t->args[i] : tc_.error();
            const Type*         bt = b.type ? lower(*b.type) : el;
            if (b.type && !el->is_unknown() && !bt->is_unknown() && assign_cost(el, bt) != 0) {
                error(DiagCode::E4010, b.span, std::format("element {} is {}, not {}", i, quoted(el), quoted(bt)), "wrong type");
            }
            if (b.symbol) set_local(b.symbol, bt);
        }
    }

    // --- SIMD ------------------------------------------------------------------------------------

    // `f32x8::splat` / `std::simd::vec<f32, 8>::splat`: the vector type a static path names.
    const Type* vector_of_static_path(const Path& p) {
        const auto& segs = p.segments;
        if (segs.size() == 2 && !segs[0].has_template_args) {
            const Type* t = tc_.builtin_or_vector(segs[0].name);
            return t && t->is(Kind::vector) ? t : nullptr;
        }
        if (segs.size() == 4 && segs[0].name == "std" && segs[1].name == "simd" && (segs[2].name == "vec" || segs[2].name == "mask")) {
            Path         head;
            for (std::size_t i = 0; i < 3; ++i) {
                PathSegment c;
                c.name              = segs[i].name;
                c.span              = segs[i].span;
                c.has_template_args = segs[i].has_template_args;
                for (const TemplateArg& a : segs[i].template_args) {
                    TemplateArg ca;
                    if (a.type) ca.type = ast::Cloner([](const Symbol&) { return nullptr; }).type(*a.type);
                    if (a.value) ca.value = ast::Cloner([](const Symbol&) { return nullptr; }).expr(*a.value);
                    ca.span = a.span;
                    c.template_args.push_back(std::move(ca));
                }
                head.segments.push_back(std::move(c));
            }
            NamedType   nt(p.span, std::move(head));
            const Type* t = lower(nt);
            return t->is(Kind::vector) || t->is(Kind::mask) ? t : nullptr;
        }
        return nullptr;
    }

    bool is_simd_block(const Type* t) const {
        return t && t->is(Kind::user) && t->symbol->template_of && t->symbol->template_of->name == "block" && t->symbol->ns == "std::simd";
    }

    const Type* lane_count_type() { return tc_.builtin("usize"); }

    // The unsigned vector with lanes as wide as `t`'s (`f32x8` → `u32x8`).
    const Type* bits_vector(const Type* t) {
        return tc_.vector(tc_.builtin("u" + std::to_string(t->inner->bits)), t->bits);
    }

    void wrong_args(CallExpr& c, std::vector<Expr*>& args, const std::string& how) {
        error(DiagCode::E4030, c.span, how, "wrong arguments");
        for (Expr* a : args) {
            if (a->type && a->type->is_untyped()) settle(*a, tc_.error());
        }
    }

    // `f32x8::splat(x)`, `iota()`, `load(s, i)`, `from_bits(u)`, `gather(s, idx)`; on masks `prefix(k)`.
    const Type* infer_vector_static(CallExpr& c, const Type* vt, const std::string& name, std::vector<Expr*>& args) {
        c.callee->type = vt;
        std::string what = types::to_string(vt);
        if (vt->is(Kind::mask)) {
            if (name == "prefix" && args.size() == 1) {   // lanes below k are set
                coerce(*args[0], tc_.builtin("usize"));
                return set(c, vt);
            }
            wrong_args(c, args, std::format("'{}::{}' is not a function of masks (they come from comparisons)", what, name));
            return set(c, tc_.error());
        }
        const Type* lane = vt->inner;
        if (name == "splat" && args.size() == 1) {
            coerce(*args[0], lane);
            return set(c, vt);
        }
        if (name == "iota" && args.empty()) return set(c, vt);
        if ((name == "load" || name == "load_unchecked") && args.size() == 2) {
            if (name == "load_unchecked") require_unsafe(c.span, "an unchecked vector load");
            const Type* st = types::strip_ref(args[0]->type);
            if (!(is_std_instance(st, "span") || is_std_instance(st, "span_mut")) || st->symbol->template_args[0] != lane) {
                if (!st->is_unknown()) mismatch(args[0]->span, st, tc_.external(std::format("std::span<{}>", types::to_string(lane))));
            }
            coerce(*args[1], tc_.builtin("usize"));
            return set(c, vt);
        }
        if ((name == "gather" || name == "gather_unchecked") && args.size() == 2) {
            if (name == "gather_unchecked") require_unsafe(c.span, "an unchecked gather");
            const Type* idx = types::strip_ref(args[1]->type);
            if (!idx->is(Kind::vector) || idx->bits != vt->bits || !idx->inner->is_integer() || idx->inner->is_signed) {
                if (!idx->is_unknown()) error(DiagCode::E4010, args[1]->span, std::format("the indices are a u32x{0} or u64x{0}", vt->bits), "not indices");
            }
            return set(c, vt);
        }
        if (name == "from_bits" && args.size() == 1) {
            coerce(*args[0], bits_vector(vt));
            return set(c, vt);
        }
        wrong_args(c, args, std::format("'{}' has no function '{}' with these arguments", what, name));
        return set(c, tc_.error());
    }

    // The CPU features code is checked for: the build's CPU level, or a multiversion copy's.
    bool has_cpu_feature(const std::string& feature) const { return target_platform().has_feature(feature); }

    // `std::simd::x86::mm256_shuffle_epi8(a, b)`: a raw intrinsic under its vendor's name (SIMD,
    // "Raw intrinsics"). `unsafe`, and an error when the CPU level lacks its feature.
    const Type* infer_simd_intrinsic(CallExpr& c, const Path& path, std::vector<Expr*>& args) {
        const std::string& arch = path.segments[2].name;
        const std::string& name = path.segments[3].name;
        const simd::Intrinsic* in = simd::find_intrinsic(arch, name);
        auto give_up = [&] {
            for (Expr* a : args) {
                if (a->type && a->type->is_untyped()) settle(*a, tc_.error());
            }
            return set(c, tc_.error());
        };
        if (!in) {
            error(DiagCode::E3020, path.segments[3].span, std::format("cannot find '{}' in namespace 'std::simd::{}'", name, arch),
                  "not an intrinsic edaxc knows");
            return give_up();
        }
        require_unsafe(c.span, "calling a raw SIMD intrinsic");
        if (!has_cpu_feature(std::string(in->feature))) {
            std::string level = target_platform().cpu;
            Diagnostic& d = error(DiagCode::E4255, c.span,
                                  std::format("'std::simd::{}::{}' needs {}, which the CPU level '{}' doesn't have", arch, name, in->feature, level),
                                  "not on this CPU level");
            note(d, std::format("build for a level with it (--cpu=...), or test 'std::build::target_has_cpu_feature(std::build::cpu_feature::{})' "
                                "in 'if compile_time'", in->feature));
            return give_up();
        }
        if (args.size() != in->params.size()) {
            wrong_args(c, args, std::format("'std::simd::{}::{}' takes {} argument{}", arch, name, in->params.size(), in->params.size() == 1 ? "" : "s"));
            return give_up();
        }
        auto int_type = [&](int bits, bool is_signed) { return tc_.builtin(std::format("{}{}", is_signed ? "i" : "u", bits)); };
        auto float_type = [&](int bits) { return tc_.builtin(std::format("f{}", bits)); };
        for (std::size_t i = 0; i < args.size(); ++i) {
            const simd::Shape& p = in->params[i];
            const Type*        t = types::strip_ref(args[i]->type);
            bool ok = true;
            std::string want;
            switch (p.kind) {
            case simd::Shape::Kind::int_vector:
                want = std::format("an integer vector of {} bits", p.bits);
                ok   = t->is(Kind::vector) && t->inner->is_integer() && t->inner->bits * t->bits == p.bits;
                break;
            case simd::Shape::Kind::float_vector:
                want = types::to_string(tc_.vector(float_type(p.lane_bits), p.lanes));
                coerce(*args[i], tc_.vector(float_type(p.lane_bits), p.lanes));
                continue;
            case simd::Shape::Kind::scalar_int: coerce(*args[i], int_type(p.lane_bits, p.is_signed)); continue;
            case simd::Shape::Kind::scalar_float: coerce(*args[i], float_type(p.lane_bits)); continue;
            case simd::Shape::Kind::same_as_first: continue;
            }
            if (!ok && !t->is_unknown()) {
                error(DiagCode::E4010, args[i]->span, std::format("'std::simd::{}::{}' takes {} here, not {}", arch, name, want, quoted(t)),
                      "wrong type");
                return give_up();
            }
        }
        const simd::Shape& r = in->result;
        switch (r.kind) {
        case simd::Shape::Kind::same_as_first: return set(c, types::strip_ref(args[0]->type));
        case simd::Shape::Kind::int_vector: return set(c, tc_.vector(int_type(r.lane_bits, r.is_signed), r.bits / r.lane_bits));
        case simd::Shape::Kind::float_vector: return set(c, tc_.vector(float_type(r.lane_bits), r.lanes));
        case simd::Shape::Kind::scalar_int: return set(c, int_type(r.lane_bits, r.is_signed));
        case simd::Shape::Kind::scalar_float: return set(c, float_type(r.lane_bits));
        }
        return give_up();
    }

    // `std::simd::native_lanes<T>()`, `std::simd::join(lo, hi)`.
    const Type* infer_simd_free(CallExpr& c, const PathSegment& seg, std::vector<Expr*>& args) {
        if (seg.name == "native_lanes" && seg.template_args.size() == 1 && seg.template_args[0].type && args.empty()) {
            const Type* lane = lower(*seg.template_args[0].type);
            u128        reg  = target_platform().vector_bytes;   // the widest register of the CPU level
            u128        n    = lane->is_numeric() ? std::max<u128>(1, reg / std::max<u128>(1, static_cast<u128>(lane->bits) / 8)) : 4;
            c.constant = std::make_shared<const ct::Value>(ct::Value::of_integer(n, tc_.builtin("usize")));
            return set(c, tc_.builtin("usize"));
        }
        if (seg.name == "join" && args.size() == 2) {
            const Type* lo = types::strip_ref(args[0]->type);
            const Type* hi = types::strip_ref(args[1]->type);
            if (!lo->is(Kind::vector) || lo != hi || lo->bits >= 64) {
                wrong_args(c, args, "'std::simd::join' takes two vectors of the same type");
                return set(c, tc_.error());
            }
            return set(c, tc_.vector(lo->inner, lo->bits * 2));
        }
        for (Expr* a : args) {
            if (a->type && a->type->is_untyped()) settle(*a, tc_.error());
        }
        return set(c, tc_.error());   // the library's std::simd functions resolve normally
    }

    // Methods of vectors and masks (SIMD, sections 3, 4, 5, 7).
    // The methods of numbers (spec 34): `x.sqrt()`, `a.min(b)`, `x.mul_add(y, z)`, `angle.sin()`.
    // Arguments take the receiver's type; there is no implicit conversion.
    const Type* infer_number_method(CallExpr& c, const Type* t, const std::string& name, std::vector<Expr*>& args) {
        c.callee->type = tc_.error();
        std::size_t want = types::number_method_arity(name);
        if (args.size() != want) {
            error(DiagCode::E4030, c.span, std::format("'{}' on {} takes {} argument{}, but {} {} given", name, quoted(t), want,
                                                       want == 1 ? "" : "s", args.size(), args.size() == 1 ? "is" : "are"),
                  "wrong number of arguments");
            for (Expr* a : args) {
                if (a->type && a->type->is_untyped()) settle(*a, tc_.error());
            }
            return set(c, tc_.error());
        }
        for (Expr* a : args) coerce(*a, t);
        if (name == "is_nan" || name == "is_infinite" || name == "is_finite") return set(c, tc_.bool_());
        return set(c, t);
    }

    const Type* infer_vector_method(CallExpr& c, MemberExpr& me, const Type* t, const std::string& name, std::vector<Expr*>& args) {
        me.type = tc_.error();
        auto same = [&](std::size_t n) {
            if (args.size() != n) return false;
            for (Expr* a : args) coerce(*a, t);
            return true;
        };
        if (t->is(Kind::mask)) {
            if ((name == "any" || name == "all" || name == "none") && args.empty()) return set(c, tc_.bool_());
            if (name == "count" && args.empty()) return set(c, tc_.builtin("usize"));
            if (name == "first" && args.empty()) {
                if (const Symbol* opt = std_item("option")) return set(c, tc_.instance(opt, {tc_.builtin("usize")}));
                return set(c, tc_.error());
            }
            if (name == "select" && args.size() == 2) {
                const Type* a = types::strip_ref(args[0]->type);
                if (a->is_untyped()) {
                    no_context(*args[0], "(write the vector type)");
                    return set(c, tc_.error());
                }
                if (!a->is(Kind::vector) || a->bits != t->bits) {
                    if (!a->is_unknown()) error(DiagCode::E4010, args[0]->span, std::format("select takes vectors of {} lanes", t->bits), "not a vector");
                    return set(c, tc_.error());
                }
                coerce(*args[1], a);
                return set(c, a);
            }
            wrong_args(c, args, std::format("{} has no method '{}' with these arguments", quoted(t), name));
            return set(c, tc_.error());
        }
        const Type* lane    = t->inner;
        bool        is_int  = lane->is_integer();
        bool        is_flt  = lane->is_float();
        if ((name == "min" || name == "max" || name == "saturating_add" || name == "saturating_sub") && same(1)) {
            if (!is_int && name.starts_with("saturating")) error(DiagCode::E4013, c.span, "saturating arithmetic is for integer vectors", "floats");
            return set(c, t);
        }
        if (name == "clamp" && same(2)) return set(c, t);
        if (name == "mul_add" && same(2)) return set(c, t);
        if ((name == "abs") && args.empty()) return set(c, t);
        if ((name == "sqrt" || name == "floor" || name == "ceil" || name == "round" || name == "trunc") && args.empty()) {
            if (!is_flt) error(DiagCode::E4013, c.span, std::format("'{}' is for float vectors", name), "integers");
            return set(c, t);
        }
        if ((name == "reduce_add" || name == "reduce_mul" || name == "reduce_min" || name == "reduce_max") && args.empty()) return set(c, lane);
        if ((name == "reduce_and" || name == "reduce_or" || name == "reduce_xor") && args.empty()) {
            if (!is_int) error(DiagCode::E4013, c.span, std::format("'{}' is for integer vectors", name), "floats");
            return set(c, lane);
        }
        if (name == "with" && args.size() == 2) {
            coerce(*args[0], tc_.builtin("usize"));
            coerce(*args[1], lane);
            return set(c, t);
        }
        if ((name == "store" || name == "store_unchecked" || name == "scatter" || name == "scatter_unchecked") && args.size() == 2) {
            if (name.ends_with("unchecked")) require_unsafe(c.span, "an unchecked vector store");
            Expr*       target = args[0];
            if (target->kind == ExprKind::unary && target->as<UnaryExpr>().op == UnaryOp::mut_marker) target = target->as<UnaryExpr>().operand.get();
            const Type* st = types::strip_ref(target->type);
            if (!is_std_instance(st, "span_mut") || st->symbol->template_args[0] != lane) {
                if (!st->is_unknown()) mismatch(args[0]->span, st, tc_.external(std::format("std::span<mut {}>", types::to_string(lane))));
            }
            if (name.starts_with("store")) coerce(*args[1], tc_.builtin("usize"));
            return set(c, tc_.void_());
        }
        if (name == "to_bits" && args.empty()) {
            if (!is_flt) return set(c, t);
            return set(c, bits_vector(t));
        }
        if ((name == "reverse") && args.empty()) return set(c, t);
        if (name == "rotate_lanes" && args.size() == 1) {
            coerce(*args[0], tc_.builtin("usize"));
            return set(c, t);
        }
        if ((name == "interleave_low" || name == "interleave_high") && same(1)) return set(c, t);
        if ((name == "shuffle" && args.size() == 1) || (name == "shuffle_with" && args.size() == 2)) {
            if (name == "shuffle_with") coerce(*args[0], t);
            Expr& order = *args.back();
            std::size_t limit = static_cast<std::size_t>(t->bits) * (name == "shuffle" ? 1 : 2);
            if (order.kind != ExprKind::list || order.as<ListExpr>().elements.size() != static_cast<std::size_t>(t->bits)) {
                error(DiagCode::E4200, order.span, std::format("the order is a list of {} constant lane indices", t->bits), "not a lane order");
            } else {
                for (ExprPtr& x : order.as<ListExpr>().elements) {
                    coerce(*x, tc_.builtin("u32"));
                    if (x->kind != ExprKind::int_literal || x->as<LiteralExpr>().int_value >= limit) {
                        error(DiagCode::E4200, x->span, std::format("a lane index from 0 to {}", limit - 1), "not a constant lane index");
                    }
                }
                order.type = tc_.array(tc_.builtin("u32"), tc_.value_arg(static_cast<u128>(t->bits), tc_.builtin("usize")));
            }
            return set(c, t);
        }
        if (name == "split" && args.empty()) {
            const Type* half = tc_.vector(lane, t->bits / 2);
            return set(c, tc_.tuple({half, half}));
        }
        wrong_args(c, args, std::format("{} has no method '{}' with these arguments", quoted(t), name));
        return set(c, tc_.error());
    }

    // `s.load(at)` / `s.store(at, v)` on a span with a `std::simd::block<N>` (SIMD, section 5).
    const Type* infer_span_block(CallExpr& c, MemberExpr& me, const Type* t, const std::string& name, std::vector<Expr*>& args) {
        me.type = tc_.error();
        const Type* lane  = t->symbol->template_args[0];
        const Type* block = types::strip_ref(args[0]->type);
        u128        n     = block->symbol->template_args.empty() ? 0 : tc_.value_bits(block->symbol->template_args[0]);
        if (!lane->is_numeric() || n < 2 || n > 64 || (n & (n - 1)) != 0) {
            error(DiagCode::E4010, c.span, "vector loads and stores need a span of numbers and a block of 2 to 64 lanes", "not a vector");
            return set(c, tc_.error());
        }
        const Type* vt = tc_.vector(lane, static_cast<int>(n));
        if (name == "load" && args.size() == 1) return set(c, vt);
        if (name == "store" && args.size() == 2) {
            if (!is_std_instance(t, "span_mut")) {
                error(DiagCode::E4022, c.span, "storing needs a std::span<mut T>", "not mutable");
            }
            coerce(*args[1], vt);
            return set(c, tc_.void_());
        }
        wrong_args(c, args, "write 's.load(at)' or 's.store(at, v)'");
        return set(c, tc_.error());
    }

    // Operators on vectors and masks: lane by lane, no broadcast, no mixing (SIMD, section 3).
    const Type* infer_vector_binary(BinaryExpr& b, const Type* l, const Type* r) {
        const Type* t = l->is(Kind::vector) || l->is(Kind::mask) ? l : r;
        TokenKind   op = b.op;
        if (l->is_untyped() || r->is_untyped()) {
            Expr& lit = l->is_untyped() ? *b.lhs : *b.rhs;
            note(error(DiagCode::E4010, lit.span, std::format("a scalar never turns into {} by itself", quoted(t)), "no broadcast"),
                 t->is(Kind::vector) ? std::format("write '{}::splat(...)'", types::to_string(t)) : "combine masks with masks");
            settle(lit, tc_.error());
            return set(b, tc_.error());
        }
        if (l != r) {
            if (!l->is_unknown() && !r->is_unknown()) {
                note(error(DiagCode::E4011, b.span, std::format("{} and {} can't be mixed", quoted(l), quoted(r)), "different vector types"),
                     "convert one with a cast; scalars become vectors with ::splat");
            }
            return set(b, tc_.error());
        }
        bool cmp = op == TokenKind::eq_eq || op == TokenKind::bang_eq || is_comparison(op);
        if (t->is(Kind::mask)) {
            if (op == TokenKind::amp || op == TokenKind::pipe || op == TokenKind::caret) return set(b, t);
            error(DiagCode::E4013, b.span, std::format("operator '{}' is not defined for masks", token_kind_spelling(op)), "masks combine with & | ^ !");
            return set(b, tc_.error());
        }
        if (cmp) return set(b, tc_.mask(t->bits));
        if (op == TokenKind::spaceship) {
            error(DiagCode::E4013, b.span, "vectors have no '<=>'; compare lane by lane with < and ==", "no ordering");
            return set(b, tc_.error());
        }
        bool is_int = t->inner->is_integer();
        bool ok = op == TokenKind::plus || op == TokenKind::minus || op == TokenKind::star || op == TokenKind::slash ||
                  (is_int && (op == TokenKind::percent || op == TokenKind::plus_percent || op == TokenKind::minus_percent ||
                              op == TokenKind::star_percent || op == TokenKind::amp || op == TokenKind::pipe || op == TokenKind::caret ||
                              op == TokenKind::shl || op == TokenKind::shr));
        if (!ok) {
            error(DiagCode::E4013, b.span, std::format("operator '{}' is not defined for {}", token_kind_spelling(op), quoted(t)), "no such operator");
            return set(b, tc_.error());
        }
        return set(b, t);
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

    // `f(args)` where `f` holds a lambda.
    const Type* infer_closure_call(CallExpr& c, const Type* closure, std::vector<Expr*>& args) {
        if (closure->is_c_function()) require_unsafe(c.span, "calling a C function of a library (std::c_library)");
        if (has_mut_captures(closure)) {   // the call changes the lambda's own values (spec 34)
            Place p = place_of(*c.callee);
            if (p.is_place && !p.is_mutable && !p.is_temporary) {
                Diagnostic& d = error(DiagCode::E4022, c.callee->span,
                                      std::format("calling '{}' changes its 'mut' captures, but '{}' is not mutable", p.name, p.name),
                                      "immutable lambda");
                if (p.local) note(d, std::format("declare it 'mut': 'mut auto {} = ...' (a parameter: 'mut F {}')", p.local->name, p.local->name),
                                  p.local->span);
            }
        }
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

    // `if compile_time (c)` (spec 15): the branch that is not taken is not checked. Nullopt when
    // the condition depends on a template parameter (the generic check): then both are checked.
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

    // Two pending values of the same template: the arguments either knows. Error on a conflict.
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
        if (i.from_call_else) {
            // `f(...) else { ... }`: the call must return bool (the condition is `!f(...)`).
            Expr& call = *i.condition->as<UnaryExpr>().operand;
            const Type* t = infer(call);
            const Type* plain = t ? types::strip_ref(t) : nullptr;
            if (plain && !plain->is_unknown() && !plain->is(Kind::error) && !plain->is(Kind::bool_)) {
                Diagnostic& d = error(DiagCode::E4252, call.span,
                                      std::format("'else' needs a call that returns 'bool', but this one returns {}",
                                                  quoted(t)));
                note(d, "'f(...) else { ... }' runs the block when the call returns false");
                infer_block(*i.then_block);
                return set(i, tc_.void_());
            }
            if (call.kind == ExprKind::call) {   // only a `@std::check_result` function (spec 10)
                const Symbol* f = called_symbol(call.as<CallExpr>());
                const Symbol* g = f && f->template_of && f->template_of->kind == f->kind ? f->template_of : f;
                const std::vector<AnnotationUse>* on_f = annotations_of(g);
                if (g && !(on_f && sema::find_annotation(*on_f, "std::check_result"))) {
                    Diagnostic& d = error(DiagCode::E4254, call.span,
                                          std::format("'else' after a call to '{}', which is not '@std::check_result'", g->name),
                                          "not a checking function");
                    note(d, "write 'if (!f(...)) { ... }'; 'f(...) else { ... }' is for functions that report their own failure");
                }
            }
            set(*i.condition, tc_.bool_());
        } else {
            check_condition(*i.condition, "if");
        }
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

    // `c[i]` on the std containers: sequences take a 'usize' index, maps their key type.
    // Everything else is a library type the checker does not know (unknown, never an error).
    // `operator[]` of a user type: the `mut` overload when `want_mut`, if there is one (spec 16).
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
        if (obj->is(Kind::vector)) {   // `v[i]`: lane i (SIMD)
            coerce(*i.index, tc_.builtin("usize"));
            if (i.index->kind == ExprKind::int_literal && i.index->as<LiteralExpr>().int_value >= static_cast<u128>(obj->bits)) {
                error(DiagCode::E4013, i.index->span, std::format("{} has lanes 0 to {}", quoted(obj), obj->bits - 1), "no such lane");
            }
            return obj->inner;
        }
        if (obj->is(Kind::user)) {
            Symbol* op = index_operator(obj, false);
            if (!op) {
                error(DiagCode::E4013, i.span, std::format("{} has no 'operator[]'", quoted(obj)), "cannot index");
                if (idx->is_untyped()) settle(*i.index, tc_.error());
                return tc_.error();
            }
            i.op_symbol = op;
            use_method(op, i.span);
            const FunctionDecl* f  = &op->function_decl();
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

    // `std::trailing_zeros(x)`, `leading_zeros`, `count_ones` (a u32: the width for 0) and
    // `std::rotate_left(x, n)` / `rotate_right` (spec 16): single instructions on most CPUs.
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

    // `std::checked_add(a, b)` (a `std::option`: `none` on overflow) and `std::saturating_add(a, b)`
    // (clamped), and the same for sub and mul (spec 16). Both operands have one integer type.
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

    // `text.split(',')`, `text.parse<u32>()`, ...: the methods of `std::str` are functions of
    // the library, `str_split(std::str text, char separator)`, called with the text first.
    // Null when the library has no such function.
    const Type* infer_str_method(CallExpr& c, MemberExpr& me, const std::string& name, const std::vector<Expr*>& args) {
        const Module* m = program_.std_module;
        if (!m) return nullptr;
        auto it = m->items.find("std::str_" + name);
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

    // `operator*` of a user type (a smart pointer, spec 13 and 16): the `mut` one when `object` is a
    // mutable place. Sets `op` and returns the target type, or null when there is none.
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
        if (!chosen || !chosen->function_decl().result) return nullptr;
        op = const_cast<Symbol*>(chosen);
        use_method(chosen, object.span);
        return types::strip_ref(lower_owned(chosen, *chosen->function_decl().result));
    }

    // `std::unique_ptr<circle>::make(...)`: the resolver names the template's member; the call
    // uses the member of the instance the path spells. Null when the path names no instance.
    Symbol* instance_member(PathExpr& pe, Symbol* s) {
        if (!s || !s->parent || !s->parent->decl || !s->parent->decl->is_template || s->parent->template_of) return nullptr;
        auto& segs = pe.path.segments;
        if (segs.size() < 2 || !segs[segs.size() - 2].has_template_args) return nullptr;
        std::vector<const Type*> args;
        for (std::size_t i = 0; i < segs[segs.size() - 2].template_args.size(); ++i) {
            const TemplateArg& a = segs[segs.size() - 2].template_args[i];
            if (!a.type) {   // a value argument: `box<3>::make()`, `functions<gl_33>::load(...)`
                if (!a.value) return nullptr;
                if (!a.evaluated) {
                    const Type* v = evaluate_value_arg(s->parent, i, *a.value);
                    if (!v->is_unknown()) a.evaluated = v;
                }
                if (!a.evaluated) return nullptr;
                args.push_back(a.evaluated);
                continue;
            }
            const Type* t = lower(*a.type);
            if (t->is_unknown() || t->is(Kind::type_param)) return nullptr;
            args.push_back(t);
        }
        {   // default template arguments: `std::darray<u32>::from` is `std::darray<u32, std::global_allocator>::from`
            const auto& params = s->parent->decl->template_params;
            const auto& syms   = s->parent->decl->template_param_symbols;
            for (std::size_t i = args.size(); i < params.size(); ++i) {
                if (!params[i].default_type) break;
                std::map<const Symbol*, const Type*> inner;
                for (std::size_t j = 0; j < i && j < syms.size(); ++j) inner[syms[j]] = args[j];
                args.push_back(tc_.lower(*params[i].default_type, &inner));
            }
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

    // The constraints of the template parameter `p`, lowered: interfaces, or std's
    // `comparable`, `cloneable`, `formattable` (externals).
    std::vector<const Type*> constraints_of(const Symbol* p) {
        std::vector<const Type*> out;
        if (!p || !p->template_param) return out;
        for (const TypePtr& c : p->template_param->constraints) out.push_back(tc_.lower(*c, nullptr));
        return out;
    }

    // --- threads (spec 27): std::sendable and std::shareable ---------------------------------

    // `unsafe std::sendable(cond)` in a base list: the condition is evaluated per instance.
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

    // Whether a value of type `t` may be moved to another thread (`shareable` false) or used
    // through `T&` from several threads at once (`shareable` true), derived from its parts like
    // copy-ness (spec 27). `why` says which part isn't, `where` points at it.
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
        case Kind::function:   // its bounds say it (spec 34): `std::function<void(), std::sendable>`
            if (shareable ? t->is_mut : t->is_signed) return true;
            return fail(std::format("{} can hold any lambda, so what it captures is unknown; add the bound to its type "
                                    "(`std::function<..., {}>`) or pass the lambda itself (a template parameter)",
                                    quoted(t), shareable ? "std::shareable" : "std::sendable"));
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

    // Whether `e` names a thread_local (or a part of one, or a reference to one).
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

    // Whether values of `t` borrow (spec 12): references, views, lambdas with reference
    // captures, containers of views. The same rule as the ownership checker's.
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

    const std::map<const Symbol*, const Type*>* constraint_args_ = nullptr;   // template parameter → argument, while checking

    // Whether `t` satisfies the constraint `c`; `why` says what is missing.
    bool satisfies(const Type* t, const Type* c, std::string& why) {
        if (c->is(Kind::external) && (c->name == "std::sendable" || c->name == "std::shareable")) {
            if (t->is_unknown()) return true;
            return thread_safe(t, c->name == "std::shareable", &why);
        }
        t = types::strip_ref(t);
        if (t->is_unknown() || c->is_unknown()) return true;
        if (c->is(Kind::user) && c->symbol->kind == SymbolKind::interface && c->symbol->ns == "std" &&
            (c->symbol->name == "formattable" || c->symbol->name == "hashable")) {
            // std's formattable and hashable are structural for built-in types (spec 14, 19, 20)
            bool ok = c->symbol->name == "formattable" ? formattable(t) : hashable(t);
            if (!ok) why = std::format("{} does not implement {}", quoted(t), quoted(c));
            return ok;
        }
        if (c->is(Kind::user) && c->symbol->kind == SymbolKind::interface) {
            if (t->is(Kind::user) && (t->symbol == c->symbol || is_ancestor(t->symbol, c->symbol))) return true;
            why = std::format("{} does not implement {}", quoted(t), quoted(c));
            return false;
        }
        if (!c->is(Kind::external)) return true;
        std::string_view name = c->name;
        {   // operators as interfaces (spec 34): implemented by writing the operator
            std::string_view base = c->external_base();
            static const std::map<std::string_view, std::string_view> ops = {
                {"std::addable", "+"}, {"std::subtractable", "-"}, {"std::multipliable", "*"}, {"std::dividable", "/"}, {"std::negatable", "-"}};
            if (auto it = ops.find(base); it != ops.end()) {
                bool        unary = base == "std::negatable";
                const auto* args  = tc_.external_args(c);
                const Type* r     = unary ? nullptr : args && !args->empty() && (*args)[0] ? types::strip_ref((*args)[0]) : t;
                if (r && r->is(Kind::type_param) && constraint_args_) {   // `V : std::addable<V>`: the instance's argument
                    if (auto at = constraint_args_->find(r->symbol); at != constraint_args_->end() && at->second) r = types::strip_ref(at->second);
                }
                if (r && r->is_unknown()) return true;
                bool ok = false;
                if (t->is_numeric() || t->is(Kind::vector)) {   // built-in arithmetic: the same type on both sides
                    ok = unary ? !(t->is_integer() && !t->is_signed) : r == t || (t->is(Kind::vector) && r == t->inner);
                } else if (const Type* u = distinct_arithmetic(t)) {   // a distinct type with @std::derive::arithmetic
                    std::string_view op = it->second;   // d + d, d - d, d * k, d / k, d / d, -d
                    ok = unary || ((op == "+" || op == "-") && r == t) || (op == "*" && r == u) || (op == "/" && (r == u || r == t));
                } else if (t->is(Kind::user)) {
                    ok = find_operator(it->second, t, r) != nullptr;
                }
                if (!ok) {
                    why = unary ? std::format("{} has no unary 'operator-'", quoted(t))
                                : std::format("{} has no 'operator{}' taking {}", quoted(t), it->second, quoted(r));
                }
                return ok;
            }
        }
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
            if (formattable(t)) return true;
            why = std::format("{} cannot be formatted", quoted(t));
            return false;
        }
        return true;
    }

    // What an f-string can print (spec 19): numbers, bools, chars, strings, simple enums, types
    // with a `format(mut std::string&)` method (std::formattable), and enums with
    // `@std::derive::formattable` whose payloads are formattable.
    bool formattable(const Type* t) {
        t = types::strip_ref(t);
        if (t->is_unknown() || t->is(Kind::type_param)) return true;
        if (t->is_numeric() || t->is(Kind::bool_) || t->is(Kind::char_) || t->is(Kind::static_str) || is_std_text(t) ||
            (t->is(Kind::external) && t->name == "std::str")) {
            return true;
        }
        if (!t->is(Kind::user)) return false;
        if (t->symbol->kind == SymbolKind::enum_) {
            if (!enum_has_payload(t->symbol)) return true;
            if (!t->symbol->decl || !sema::find_annotation(t->symbol->decl->annotations, "std::derive::formattable")) return false;
            for (const Symbol* a : t->symbol->member_order) {
                if (a->alternative && a->alternative->payload && !formattable(lower_owned(a, *a->alternative->payload))) return false;
            }
            return true;
        }
        return format_method(t->symbol) != nullptr;
    }

    // The `void format(mut std::formatter& out)` method of a type (or a base class), or null.
    static const Symbol* format_method(const Symbol* type) {
        for (const Symbol* t = type; t; t = t->base_class()) {
            auto it = t->members.find("format");
            if (it == t->members.end()) continue;
            for (const Symbol* m : it->second) {
                const FunctionDecl* f = m->function();
                if (m->kind == SymbolKind::method && f && !m->is_static && f->params.size() == 1) return m;
            }
        }
        return nullptr;
    }

    // A std::hash_map key (spec 20): integers, chars, bools, strings, or a type with `u64 hash()`.
    bool hashable(const Type* t) {
        t = types::strip_ref(t);
        if (t->is_unknown() || t->is(Kind::type_param)) return true;
        if (t->is_integer() || t->is(Kind::char_) || t->is(Kind::bool_) || is_std_string(t) || t->is(Kind::static_str) ||
            (t->is(Kind::external) && t->name == "std::str")) {
            return true;
        }
        if (!t->is(Kind::user)) return false;
        for (const Symbol* s = t->symbol; s; s = s->base_class()) {
            if (auto it = s->members.find("hash"); it != s->members.end()) {
                for (const Symbol* m : it->second) {
                    if (m->function() && m->function_decl().params.empty() && !m->is_static) return true;
                }
            }
        }
        return false;
    }

    // Checks the arguments of a template against the constraints of its parameters (E4161).
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
                std::string                              why;
                std::map<const Symbol*, const Type*>     by_param;   // the arguments, for `std::addable<V>`
                for (std::size_t j = 0; j < params.size() && j < args.size(); ++j) by_param[params[j]] = args[j];
                constraint_args_ = &by_param;
                bool met         = satisfies(args[i], c, why);
                constraint_args_ = nullptr;
                if (met) continue;
                Diagnostic& d = error(DiagCode::E4161, span, why, std::format("'{}' needs {} for '{}'", display, quoted(c), params[i]->name));
                note(d, "the constraint is here", params[i]->template_param->span);
                ok = false;
            }
        }
        return ok;
    }

    // A method call on a value of a constrained template parameter: only the methods of the
    // constraining interfaces exist (spec 14). Null when `t` is not constrained by an interface.
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

    // A method of `t` named `name` that takes no arguments, or null.
    Symbol* nullary_method(const Type* t, const std::string& name) {
        for (Symbol* m : find_methods(t->symbol, name)) {
            if (m->function() && m->function_decl().params.empty() && !m->function_decl().is_template) return m;
        }
        return nullptr;
    }

    // `for (T& x : c)` over a user type (spec 10): a container has `len()` and `operator[]`
    // (the loop visits every index); a stream has `next() mut` returning a `std::option` (the
    // loop runs until `none`).
    void infer_for_over(ForExpr& f, const Type* t, const Type* var) {
        bool want_mut = var->is(Kind::reference) && var->is_mut;
        Symbol* len   = nullary_method(t, "len");
        Symbol* at    = index_operator(t, want_mut);
        if (len && at) {
            f.len_symbol   = len;
            f.index_symbol = at;
            use_method(len, f.iterable->span);
            use_method(at, f.iterable->span);
            const FunctionDecl* af = &at->function_decl();
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

    // `for compile_time` (spec 26): the body is copied once per element and each copy is
    // checked with the loop variable as a compile-time constant. False when the elements depend
    // on a template parameter (the generic check): then the body is checked once as usual.
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

    // The payload type of std::ok/error/some patterns, from the subject's template arguments.
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
        case PatternKind::tuple: {
            auto& tp = p.as<TuplePattern>();
            bool  fits = s->is(Kind::tuple) && s->args.size() == tp.elements.size();
            if (!fits && !s->is_unknown()) {
                error(DiagCode::E4061, p.span,
                      std::format("this pattern is a tuple of {} elements, but the match subject is {}", tp.elements.size(), quoted(s)),
                      "wrong type");
            }
            for (std::size_t i = 0; i < tp.elements.size(); ++i) check_pattern(*tp.elements[i], fits ? s->args[i] : tc_.error(), mut_ref);
            return;
        }
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
            return;
        }
        }
    }

    // Matches every value: `_`, a binding, or a struct pattern made only of such patterns.
    static bool irrefutable(const Pattern& p) {
        if (p.kind == PatternKind::wildcard || p.kind == PatternKind::binding) return true;
        if (p.kind == PatternKind::tuple) {
            const auto& tp = p.as<TuplePattern>();
            return std::all_of(tp.elements.begin(), tp.elements.end(), [](const PatternPtr& e) { return irrefutable(*e); });
        }
        if (p.kind != PatternKind::struct_) return false;
        const auto& sp = p.as<StructPattern>();
        return std::all_of(sp.fields.begin(), sp.fields.end(),
                           [](const FieldPattern& f) { return !f.pattern || irrefutable(*f.pattern); });
    }

    // Does this arm match every value on its own (no guard, `_` or a binding)?
    static bool is_catch_all(const MatchArm& a) {
        return !a.guard && (a.pattern->kind == PatternKind::wildcard || a.pattern->kind == PatternKind::binding);
    }

    // Whether the patterns together match every value of `t` (spec 10): nested enum patterns
    // count, so `std::error(io_error::full)` and `std::error(io_error::closed)` cover `std::error`.
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

    // Whether the patterns cover the alternative `name` of the enum `e`, payload included.
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
            } else if (p.kind == PatternKind::struct_ || p.kind == PatternKind::tuple) {
                if (irrefutable(p)) return;   // a struct or tuple pattern with only bindings matches everything
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
    const Symbol*                                       global_symbol_ = nullptr;   // the global check_global_symbol checks
    bool constraints_hold(const std::vector<const Symbol*>& params, const std::vector<const Type*>& args, Span span,
                          std::string_view display) {
        return check_constraints(params, args, span, display);
    }
private:   // compile-time values that depend on a template parameter

    friend class Instantiator;   // reads annotation values (@std::diagnostic_frame) through ensure_constant

    // On-demand checking for compile-time code; defined after Instantiator, which keeps the state.
    bool begin_check(const Decl* d);
    void end_check(const Decl* d);
    bool ensure_function(const Symbol* s);
    std::shared_ptr<const ct::Value> ensure_constant(const Symbol* g);
    void ensure_field(const Symbol* f);

    // The instance of the function template `templ` for `args` (spec 14), defined after Instantiator.
    Symbol* instantiate_function(const Symbol* templ, const std::vector<const Type*>& args, std::optional<Span> site = std::nullopt);
    // The pack template `templ` as an ordinary template for `n` pack arguments.
    Symbol* instantiator_pack_variant(const Symbol* templ, std::size_t n);
    Symbol* instantiate_method(const Symbol* templ, const std::vector<const Type*>& args, std::optional<Span> site = std::nullopt);
};

// Makes the instances of template types (spec 14). An instance is a copy of the template's
// declaration, re-parsed from its source text so that it has an AST of its own; it is resolved
// in the template's module and checked with the parameters replaced by the arguments. Instances
// are created on demand, wherever a type `box<u32>` is lowered.
class Instantiator {
public:
    Instantiator(sema::Program& program, const SourceManager& sources, DiagnosticEngine& diags)
        : program_(program), sources_(sources), diags_(diags) {}

    // Where the next function instance is asked for (the call), set by the checker.
    std::optional<Span> next_site;

    // The instantiation chain of errors (templates): every error reported while an instance is
    // checked gets a note naming the instance and where it was asked for, innermost first. A
    // template marked `@std::diagnostic_frame{ .hide = true }` leaves no note, and an error in
    // its own code moves to the call that reached it, with a note where the check is.
    bool hidden_frame(const Symbol* templ) {
        const Decl* d = templ ? templ->decl : nullptr;
        const AnnotationUse* u = d ? sema::find_annotation(d->annotations, "std::diagnostic_frame") : nullptr;
        if (!u || !u->value_symbol) return false;
        std::shared_ptr<const ct::Value> v = u->value_symbol->ct_value;
        if (!v) v = Checker(program_, diags_, this).ensure_constant(u->value_symbol);
        return v && !v->elements.empty() && v->elements[0].kind == ct::Value::Kind::boolean && v->elements[0].boolean;   // .hide
    }

    static bool inside(Span inner, Span outer) { return inner.file == outer.file && inner.begin >= outer.begin && inner.end <= outer.end; }

    void add_frame(std::size_t mark, const Symbol* templ, const Symbol* inst, std::optional<Span> site) {
        if (diags_.size() == mark) return;
        std::vector<Diagnostic> made   = diags_.take_since(mark);
        bool                    hidden = hidden_frame(templ);
        for (Diagnostic& d : made) {
            if (d.severity != Severity::error && d.severity != Severity::warning) {
                diags_.add(std::move(d));
                continue;
            }
            const Decl* src = templ->definition ? templ->definition : templ->decl;
            if (hidden) {
                if (site && src && inside(d.span, src->span)) {   // in the library's code: at the call that reached it
                    bool noted = false;
                    for (const Note& n : d.notes) noted = noted || n.message.starts_with("inside the library");
                    if (!noted) d.notes.push_back(Note{std::format("inside the library, in '{}'", templ->name), d.span});
                    d.span = *site;
                }
            } else if (site) {
                std::size_t frames = 0;
                for (const Note& n : d.notes) if (n.message.starts_with("in the instance ")) ++frames;
                if (frames < 8) {
                    d.notes.push_back(Note{std::format("in the instance '{}', asked for here", instance_name(inst)), site});
                } else if (frames == 8) {
                    d.notes.push_back(Note{"in the instance ... (more instances)", std::nullopt});
                }
            }
            diags_.add(std::move(d));
        }
    }

    std::string instance_name(const Symbol* inst) {
        std::string out = inst->parent && inst->parent->template_of ? types::to_string(program_.types->user(inst->parent)) + "::" : "";
        if (inst->kind == SymbolKind::function || inst->kind == SymbolKind::method) {
            out += inst->name + "<";
            for (std::size_t i = 0; i < inst->template_args.size(); ++i) out += (i ? ", " : "") + types::to_string(inst->template_args[i]);
            return out + ">";
        }
        return types::to_string(program_.types->user(inst));
    }

    const Type* instantiate(const Symbol* templ, const std::vector<const Type*>& args) {
        auto key = std::make_pair(templ, args);
        if (auto it = memo_.find(key); it != memo_.end()) return it->second;
        Decl* copy = copy_of(*templ);
        if (!copy) return program_.types->error();
        if (copy->kind != DeclKind::function && !copy->template_params.empty() && copy->template_params.back().is_pack &&
            !copy->template_params.back().is_type) {   // `tensor<f32, 4, 4>` (spec 34): the value pack for these arguments
            std::size_t fixed = copy->template_params.size() - 1;
            expand_type_pack(copy->as<TypeDecl>(), args.size() > fixed ? args.size() - fixed : 0);
        }
        std::vector<Symbol*> params;
        Symbol*              inst = resolve_instance(program_, diags_, templ, *copy, params);
        if (params.size() != args.size()) return program_.types->error();
        inst->template_of    = templ;
        inst->instance_index = ++count_[templ];
        for (std::size_t i = 0; i < params.size(); ++i) inst->template_subst[params[i]] = args[i];
        inst->template_args = args;
        // The interfaces the template implements that don't depend on its parameters: `box<T> : named`.
        for (Symbol* b : templ->bases) {
            if (b && b->kind == SymbolKind::interface && !(b->decl && b->decl->is_template) &&
                std::find(inst->bases.begin(), inst->bases.end(), b) == inst->bases.end()) {
                inst->bases.push_back(b);
            }
        }
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
        std::size_t mark = diags_.size();
        checker.check_instance(inst, *copy);
        add_frame(mark, templ, inst, checker.instance_site_);
        return t;
    }

    // The instance of the function template `templ` for `args`, made once per argument list.
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
        std::optional<Span> site = std::exchange(next_site, std::nullopt);
        std::size_t         mark = diags_.size();
        Checker(program_, diags_, this).check_instance(inst, *copy);
        add_frame(mark, templ, inst, site);
        return inst;
    }

    // The instance of the method template `templ` for `args` (spec 14). The resolved method is
    // copied (its body refers to the class's parameters, which the class instance substitutes);
    // the copy's own parameters are replaced by `args`.
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
        std::optional<Span> site = std::exchange(next_site, std::nullopt);
        std::size_t         mark = diags_.size();
        Checker(program_, diags_, this).check_instance(inst, *raw);
        add_frame(mark, templ, inst, site);
        return inst;
    }

    // The template `templ` with its parameter pack expanded for `n` arguments: an ordinary
    // function template (templates, "Parameter packs"). Made once per count.
    Symbol* pack_variant(const Symbol* templ, std::size_t n) {
        auto key = std::make_pair(templ, n);
        if (auto it = pack_memo_.find(key); it != pack_memo_.end()) return it->second;
        Decl* copy = copy_of(*templ);
        if (!copy || copy->kind != DeclKind::function) return nullptr;
        expand_pack(copy->as<FunctionDecl>(), n);
        std::vector<Symbol*> params;
        Symbol* variant      = resolve_function_instance(program_, diags_, templ, copy->as<FunctionDecl>(), params);
        variant->pack_origin = templ;
        variant->pack_count  = n;
        variant->parent      = templ->parent;
        variant->access      = templ->access;
        pack_memo_[key]      = variant;
        return variant;
    }

    // Replaces the pack `type... A` by `A$0, ..., A$n-1`, the pack parameter `A&... values` by
    // `A$0& values$0, ...`, and expands the uses of the pack in the body.
    static void expand_pack(FunctionDecl& f, std::size_t n) {
        auto none = [](const sema::Symbol&) { return static_cast<sema::Symbol*>(nullptr); };
        std::string type_name;
        std::string value_pack;
        std::vector<TemplateParam> tparams;
        for (TemplateParam& p : f.template_params) {
            if (!p.is_pack) {
                tparams.push_back(std::move(p));
                continue;
            }
            if (!p.is_type) {   // `usize... N`: the values N$0, ..., N$n-1, walked as the pack `N` (spec 34)
                value_pack = p.name;
                for (std::size_t i = 0; i < n; ++i) {
                    ast::Cloner   c(none);
                    TemplateParam e;
                    e.is_type    = false;
                    e.value_type = c.type(*p.value_type);
                    e.name       = p.name + "$" + std::to_string(i);
                    e.span       = p.span;
                    tparams.push_back(std::move(e));
                }
                continue;
            }
            type_name = p.name;
            for (std::size_t i = 0; i < n; ++i) {
                TemplateParam e;
                e.is_type = true;
                e.name    = p.name + "$" + std::to_string(i);
                e.span    = p.span;
                tparams.push_back(std::move(e));
            }
        }
        f.template_params = std::move(tparams);
        if (!value_pack.empty()) {
            if (f.body) {
                ast::Cloner c(none);
                c.expand_pack("", value_pack, n);
                f.body = c.block(*f.body);
            }
            return;
        }
        std::string value_name;
        std::vector<Param> params;
        for (Param& p : f.params) {
            if (!p.is_pack) {
                params.push_back(std::move(p));
                continue;
            }
            value_name = p.name;
            for (std::size_t i = 0; i < n; ++i) {
                ast::Cloner c(none);
                c.rename(type_name, type_name + "$" + std::to_string(i));
                Param e;
                e.is_mut = p.is_mut;
                e.type   = c.type(*p.type);
                e.name   = p.name + "$" + std::to_string(i);
                e.span   = p.span;
                params.push_back(std::move(e));
            }
        }
        f.params = std::move(params);
        if (f.body && !value_name.empty()) {
            ast::Cloner c(none);
            c.expand_pack(type_name, value_name, n);
            f.body = c.block(*f.body);
        }
    }

    // A type template's value pack `usize... N` as `N$0, ..., N$n-1` (spec 34); its members walk
    // `N` with `for compile_time`, pass it on as `f<N...>` and count it with `N.len()`.
    static void expand_type_pack(TypeDecl& t, std::size_t n) {
        auto          none = [](const sema::Symbol&) { return static_cast<sema::Symbol*>(nullptr); };
        TemplateParam pack = std::move(t.template_params.back());
        t.template_params.pop_back();
        for (std::size_t i = 0; i < n; ++i) {
            ast::Cloner   c(none);
            TemplateParam e;
            e.is_type    = false;
            e.value_type = c.type(*pack.value_type);
            e.name       = pack.name + "$" + std::to_string(i);
            e.span       = pack.span;
            t.template_params.push_back(std::move(e));
        }
        auto cloner = [&] {
            ast::Cloner c(none);
            c.expand_pack("", pack.name, n);
            return c;
        };
        for (DeclPtr& m : t.members) {
            if (m->kind == DeclKind::function) {
                auto& f = m->as<FunctionDecl>();
                for (Param& p : f.params) p.type = cloner().type(*p.type);
                if (f.result) f.result = cloner().type(*f.result);
                if (f.body) f.body = cloner().block(*f.body);
            } else if (m->kind == DeclKind::field || m->kind == DeclKind::variable) {
                auto& v = m->as<VariableDecl>();
                if (v.type) v.type = cloner().type(*v.type);
                if (v.init) v.init = cloner().expr(*v.init);
            }
        }
    }

    std::map<std::pair<const Symbol*, std::size_t>, Symbol*> pack_memo_;

private:
    // A fresh copy of the template's declaration, re-parsed from its source text, so that the
    // instance has an AST of its own. It lives as long as the program: symbols refer to it.
    // Null when the source does not hold exactly one declaration.
    Decl* copy_of(const Symbol& templ) {
        if (templ.pack_origin) {   // a pack template made ordinary: the original, expanded again
            Decl* copy = copy_of(*templ.pack_origin);
            if (copy && copy->kind == DeclKind::function) expand_pack(copy->as<FunctionDecl>(), templ.pack_count);
            return copy;
        }
        Decl* copy = reparse(templ.definition ? *templ.definition : *templ.decl);
        if (copy && templ.source_block) merge_source_block(*copy, reparse(*templ.source_block));
        return copy;
    }

    Decl* reparse(const Decl& src) {
        const SourceFile& file    = sources_.file(src.span.file);
        std::vector<Token> tokens = tokens_of(file, src.span.file, src.span.begin, src.span.end);
        program_.instance_files.push_back(parse(file, src.span.file, tokens, diags_));
        ast::File& parsed = program_.instance_files.back();
        if (parsed.decls.size() != 1) return nullptr;
        return parsed.decls.front().get();
    }

    // The tokens of `file` between the byte offsets `begin` and `end`, and an end-of-file token:
    // the file is lexed once and its tokens are reused for every instance of its templates.
    std::vector<Token> tokens_of(const SourceFile& file, FileId id, std::size_t begin, std::size_t end) {
        auto it = program_.file_tokens.find(id);
        if (it == program_.file_tokens.end()) {
            DiagnosticEngine quiet;   // reported when the file was loaded
            it = program_.file_tokens.emplace(id, tokenize(file, id, quiet)).first;
        }
        const std::vector<Token>& all = it->second;
        auto first = std::lower_bound(all.begin(), all.end(), begin, [](const Token& t, std::size_t at) { return t.span.begin < at; });
        std::vector<Token> out;
        for (auto t = first; t != all.end() && t->kind != TokenKind::eof && t->span.end <= end; ++t) out.push_back(*t);
        Token eof;
        eof.kind = TokenKind::eof;
        eof.span = Span{id, static_cast<std::uint32_t>(end), static_cast<std::uint32_t>(end)};
        out.push_back(std::move(eof));
        return out;
    }

    // A class template split into a header and a source block (spec 09) becomes one declaration
    // for the instance: the method bodies of the source go into the header's declarations, and
    // the private members are added. The resolver checked already that the two match.
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
    std::string source_path(Span span) const { return span.file < sources_.count() ? sources_.file(span.file).path() : std::string(); }

    // Which declarations are being checked or are done (functions and compile_time constants
    // can be checked early, on demand, when compile-time code needs them).
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

std::string Checker::source_path(Span span) const { return instantiator_ ? instantiator_->source_path(span) : std::string(); }

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

void Checker::ensure_field(const Symbol* f) {
    if (!instantiator_ || !f->decl || instantiator_->states.count(f->decl)) return;
    Checker c(program_, diags_, instantiator_);
    c.module_ = f->module;
    if (const auto* sub = substitution_of(f)) c.subst_ = *sub;
    c.check_decl(const_cast<Decl&>(*f->decl), f->parent);
}

// Null only when the template's source did not parse (already reported) or no instantiator is set.
Symbol* Checker::instantiate_method(const Symbol* templ, const std::vector<const Type*>& args, std::optional<Span> site) {
    if (!instantiator_) return nullptr;
    instantiator_->next_site = site;
    return instantiator_->instantiate_method(templ, args);
}

Symbol* Checker::instantiate_function(const Symbol* templ, const std::vector<const Type*>& args, std::optional<Span> site) {
    if (!instantiator_) return nullptr;
    instantiator_->next_site = site;
    return instantiator_->instantiate_function(templ, args);
}

Symbol* Checker::instantiator_pack_variant(const Symbol* templ, std::size_t n) {
    return instantiator_ ? instantiator_->pack_variant(templ, n) : nullptr;
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
        if (!s->ct_value && s->kind == SymbolKind::field && s->is_type_value && s->decl && s->parent) {   // `type element = u32;`
            Checker(program, diags, &instantiator).check_member_type(s);
        }
        if (!s->ct_value || s->ct_value->kind != ct::Value::Kind::type) return nullptr;
        return s->ct_value->type_value;
    });
    Checker(program, diags, &instantiator).run();
    // Annotation checks, once per use (spec 29); instances made meanwhile add their own.
    for (std::size_t i = 0; i < program.annotation_checks.size(); ++i) {
        Checker(program, diags, &instantiator).run_annotation_check(program.annotation_checks[i]);
    }
    if (program.build_graph && !diags.has_errors()) Checker(program, diags, &instantiator).run_build();
    // Instances are only made while checking. Later phases see the ones that exist.
    program.types->set_instantiator(nullptr);
    program.types->set_type_value_resolver([](const Symbol* s) -> const Type* {
        if (!s->ct_value || s->ct_value->kind != ct::Value::Kind::type) return nullptr;
        return s->ct_value->type_value;
    });
}

} // namespace edax
