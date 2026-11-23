#include "edax/ownership.hpp"
#include "edax/compile_time.hpp"

#include "edax/ast_printer.hpp"
#include "edax/types.hpp"

#include <algorithm>
#include <utility>
#include <format>
#include <functional>
#include <set>

namespace edax {

using namespace ast;
using sema::LocalKind;
using sema::Module;
using sema::Symbol;
using sema::SymbolKind;
using types::Kind;
using types::Type;

namespace {

enum class Copy { yes, no, unknown };

std::string quoted(const Type* t) { return "'" + types::to_string(t) + "'"; }

// Moved locals at one program point. Unreachable states (after return/break) merge as identity.
struct State {
    bool                           reachable = true;
    std::map<const Symbol*, Span>  moved;   // local -> where it was moved

    static State unreachable() {
        State s;
        s.reachable = false;
        return s;
    }
    bool operator==(const State& o) const {
        if (reachable != o.reachable) return false;
        if (moved.size() != o.moved.size()) return false;
        for (const auto& [sym, span] : moved) {
            if (!o.moved.count(sym)) return false;
        }
        return true;
    }
};

State merge(const State& a, const State& b) {
    if (!a.reachable) return b;
    if (!b.reachable) return a;
    State out = a;
    for (const auto& [sym, span] : b.moved) out.moved.emplace(sym, span);
    return out;
}

struct LoopContext {
    State         breaks    = State::unreachable();
    State         continues = State::unreachable();
    const Symbol* borrowed  = nullptr;   // container the loop iterates by reference
    bool          borrow_mut = false;
    std::vector<std::string> path;       // the fields from `borrowed` to the container (`w.b`: b), spec 34
    Span          span;
    std::string   var;
};

// A local reference `T& r = x...;` keeps `x` borrowed until r's last use.
struct Borrow {
    const Symbol* ref;
    const Symbol* root;
    Span          decl;
    std::uint32_t live_end;
};

// A view held in a local (`holder`) that borrows `root` (spec 12, 27): a lambda with reference
// captures, a `std::str` of a string, a thread that borrows, a darray of such handles. While the
// holder is alive, `root` is frozen (shared) or not usable at all (exclusive).
struct ViewBorrow {
    const Symbol* holder;
    const Symbol* root;
    bool          exclusive;
    Span          at;         // where the borrow starts
    bool          to_scope_end;   // the holder has a destructor: it borrows until its scope ends
    int           depth;      // the block depth of the holder's scope
};

class Ownership {
public:
    Ownership(sema::Program& program, DiagnosticEngine& diags)
        : program_(program), diags_(diags), tc_(*program.types) {}

    void run() {
        for (auto& [name, m] : program_.modules) {
            module_ = m.get();
            check_types_and_signatures(*m);
            if (m->header) walk_file(m->header->ast);
            if (m->source) walk_file(m->source->ast);
        }
        for (std::size_t i = 0; i < program_.instances.size(); ++i) {
            walk_instance(*program_.instances[i], *program_.instance_copies[i]);
        }
    }

    // The members of a template instance (spec 14), walked with its parameters substituted.
    void walk_instance(Symbol& inst, ast::Decl& copy) {
        if (copy.kind == DeclKind::enum_) return;
        module_    = inst.module;
        cur_subst_ = &inst.template_subst;
        if (copy.kind == DeclKind::function) {
            walk_function(copy.as<FunctionDecl>());
        } else {
            for (DeclPtr& m : copy.as<TypeDecl>().members) {
                // A method nobody uses may not compile for these arguments (`clone()` of an
                // element type that can't be cloned): like the type checker, only used methods count.
                bool lazy = m->kind == DeclKind::function && !m->as<FunctionDecl>().is_destructor &&
                            !m->as<FunctionDecl>().is_virtual && !m->as<FunctionDecl>().is_override &&
                            !program_.used_instance_methods.count(m.get());
                if (lazy) ++silent_;
                walk_decl(*m);
                if (lazy) --silent_;
            }
        }
        cur_subst_ = nullptr;
    }

    // Lowers a type written in the code being checked (`owner` null) or in the declaration of
    // `owner`, which may belong to a template instance (spec 14).
    const Type* lower(const TypeExpr& t, const Symbol* owner = nullptr) {
        return tc_.lower(t, owner ? substitution_of(owner) : cur_subst_);
    }

private:
    // --- diagnostics ----------------------------------------------------------------------

    Diagnostic& error(DiagCode code, Span span, std::string message, std::string label = {}) {
        if (silent_ > 0) {
            scratch_ = Diagnostic{code, Severity::error, {}, span, {}, {}};
            return scratch_;
        }
        return diags_.report(code, span, std::move(message), std::move(label));
    }
    static void note(Diagnostic& d, std::string message, std::optional<Span> span = std::nullopt) {
        d.notes.push_back(Note{std::move(message), span});
    }

    // --- classification -------------------------------------------------------------------

    Copy copy_kind(const Type* t) {
        if (!t) return Copy::unknown;
        switch (t->kind) {
        case Kind::error:
        case Kind::type_param:
        case Kind::untyped_int:
        case Kind::untyped_float:
        case Kind::untyped_default: return Copy::unknown;
        case Kind::user: return user_copy(t->symbol);
        case Kind::external: return external_copy(t);
        case Kind::function: return Copy::no;   // owns its callable on the heap (spec 18)
        case Kind::array: return copy_kind(t->inner);
        case Kind::tuple: {   // a tuple is a copy type if all its elements are
            Copy c = Copy::yes;
            for (const Type* e : t->args) c = combine(c, copy_kind(e));
            return c;
        }
        case Kind::closure: {   // a lambda copies like its captured values (references are views)
            Copy c = Copy::yes;
            if (const LambdaExpr* l = tc_.lambda_of(t)) {
                for (const Capture& cap : l->captures) {
                    if (cap.kind == CaptureKind::ref || cap.kind == CaptureKind::mut_ref || !cap.symbol) continue;
                    c = combine(c, copy_kind(cap.symbol->type));
                }
            }
            return c;
        }
        default: return Copy::yes;   // built-ins, references, raw pointers, ranges, string literals
        }
    }

    Copy combine(Copy a, Copy b) {
        if (a == Copy::no || b == Copy::no) return Copy::no;
        if (a == Copy::unknown || b == Copy::unknown) return Copy::unknown;
        return Copy::yes;
    }

    Copy external_copy(const Type* t) {
        // std::format_args is move-only: formatting consumes it, so its holes run at most once (spec 31).
        static const std::set<std::string_view> copy = {"std::str",       "std::span",
                                                        "std::exit_code", "std::ordering", "std::sort_order",
                                                        "std::weak_ptr_never"};
        static const std::set<std::string_view> owning = {
            "std::string",   "std::darray", "std::unique_ptr", "std::shared_ptr", "std::weak_ptr",
            "std::file",     "std::hash_map", "std::tree_map", "std::function",  "std::mutex",
            "std::atomic",   "std::filesystem::path", "std::format_args"};
        std::string_view base = t->external_base();
        if (base.starts_with("std::exit_code") || base.starts_with("std::ordering")) return Copy::yes;
        if (copy.count(base)) return Copy::yes;
        if (owning.count(base)) return Copy::no;
        if (base == "std::result" || base == "std::option" || base == "std::array") {
            const std::vector<const Type*>* args = tc_.external_args(t);
            if (!args) return Copy::unknown;
            Copy c = Copy::yes;
            for (const Type* a : *args) c = combine(c, copy_kind(a));
            return c;
        }
        return Copy::unknown;
    }

    Copy user_copy(const Symbol* s) {
        if (auto it = copy_memo_.find(s); it != copy_memo_.end()) return it->second;
        copy_memo_[s] = Copy::unknown;   // recursion guard
        Copy c = Copy::yes;
        if (s->kind == SymbolKind::interface) {
            c = Copy::no;
        } else if (s->kind == SymbolKind::enum_) {
            for (const Symbol* a : s->member_order) {
                if (a->alternative && a->alternative->payload) c = combine(c, copy_kind(lower(*a->alternative->payload, a)));
            }
        } else {
            std::function<void(const Symbol*)> walk = [&](const Symbol* t) {
                for (const Symbol* b : t->bases) walk(b);
                if (t->has_opaque_base) c = combine(c, Copy::unknown);
                for (const Symbol* m : t->member_order) {
                    if (m->kind == SymbolKind::method && m->function() && m->function()->is_destructor) c = Copy::no;
                    if (m->kind == SymbolKind::field && !m->is_static) {
                        c = combine(c, copy_kind(lower(*m->decl->as<VariableDecl>().type, m)));
                    }
                }
            };
            walk(s);
        }
        copy_memo_[s] = c;
        return c;
    }

    bool is_view(const Type* t) {
        if (!t) return false;
        if (t->is_c_function()) return true;   // a view of its std::c_library
        switch (t->kind) {
        case Kind::reference: return true;
        case Kind::array: return is_view(t->inner);
        case Kind::tuple:   // a view if any element is
            return std::any_of(t->args.begin(), t->args.end(), [&](const Type* e) { return is_view(e); });
        case Kind::closure: {   // a lambda with reference captures is a view (spec 18)
            const LambdaExpr* l = tc_.lambda_of(t);
            if (!l) return false;
            for (const Capture& c : l->captures) {
                if (c.kind == CaptureKind::ref || c.kind == CaptureKind::mut_ref) return true;
                if (c.symbol && is_view(c.symbol->type)) return true;
            }
            return false;
        }
        case Kind::external: {
            std::string_view base = t->external_base();
            if (base == "std::str" || base == "std::span" || base == "std::format_args") return true;
            if (const std::vector<const Type*>* args = tc_.external_args(t)) {
                for (const Type* a : *args) {
                    if (is_view(a)) return true;
                }
            }
            return false;
        }
        case Kind::user: {
            const Symbol* s = t->symbol;
            if (s->decl && (s->kind == SymbolKind::struct_ || s->kind == SymbolKind::class_)) {
                if (s->decl->as<TypeDecl>().is_view) return true;
                // A container of views is a view: `std::darray<std::str>` (spec 12).
                return std::any_of(s->template_args.begin(), s->template_args.end(), [&](const Type* a) { return is_view(a); });
            }
            if (s->kind == SymbolKind::enum_) {
                if (view_memo_.count(s)) return view_memo_[s];
                view_memo_[s] = false;
                bool v = false;
                for (const Symbol* a : s->member_order) {
                    if (a->alternative && a->alternative->payload && is_view(lower(*a->alternative->payload, a))) v = true;
                }
                return view_memo_[s] = v;
            }
            return false;
        }
        default: return false;
        }
    }

    // Why a type cannot be cloned (empty if it can, or if unknown).
    std::string clone_blocker(const Type* t, int depth = 0) {
        if (!t || depth > 8 || copy_kind(t) == Copy::yes) return "";
        if (t->is(Kind::function)) return std::format("{} cannot be cloned (it owns a callable)", quoted(t));
        if (t->is(Kind::array)) return clone_blocker(t->inner, depth + 1);
        if (t->is(Kind::tuple)) {
            for (const Type* e : t->args) {
                if (std::string b = clone_blocker(e, depth + 1); !b.empty()) return b;
            }
            return "";
        }
        if (t->is(Kind::external)) {
            std::string_view base = t->external_base();
            if (base == "std::unique_ptr" || base == "std::file" || base == "std::mutex" || base == "std::atomic") {
                return std::format("{} cannot be cloned", quoted(t));
            }
            return "";
        }
        if (!t->is(Kind::user)) return "";
        const Symbol* s = t->symbol;
        if (s->kind == SymbolKind::class_) {
            if (!s->members.count("clone")) return std::format("class {} has no 'clone()' method", quoted(t));
            for (const Type* a : s->template_args) {   // `std::darray<T>` clones its elements
                std::string why = clone_blocker(a, depth + 1);
                if (!why.empty()) return why;
            }
            return "";
        }
        if (s->kind == SymbolKind::struct_) {
            for (const Symbol* m : s->member_order) {
                if (m->kind != SymbolKind::field || m->is_static) continue;
                const Type* ft = lower(*m->decl->as<VariableDecl>().type, m);
                std::string why = clone_blocker(ft, depth + 1);
                if (!why.empty()) return std::format("field '{}': {}", m->name, why);
            }
        }
        if (s->kind == SymbolKind::enum_) {
            for (const Symbol* a : s->member_order) {
                if (!a->alternative || !a->alternative->payload) continue;
                std::string why = clone_blocker(lower(*a->alternative->payload, a), depth + 1);
                if (!why.empty()) return std::format("alternative '{}': {}", a->name, why);
            }
        }
        return "";
    }

    // --- type and signature rules ---------------------------------------------------------

    void check_types_and_signatures(Module& m) {
        for (Symbol* s : m.item_order) {
            if (s->kind == SymbolKind::struct_ || s->kind == SymbolKind::class_) {
                bool view = s->decl->as<TypeDecl>().is_view;
                for (Symbol* f : s->member_order) {
                    if (f->kind == SymbolKind::method) {
                        check_signature(*f, /*is_method=*/!f->is_static);
                        continue;
                    }
                    if (f->kind != SymbolKind::field || view) continue;
                    const auto& v  = f->decl->as<VariableDecl>();
                    const Type* ft = lower(*v.type);
                    if (s->decl->as<TypeDecl>().is_union) {   // spec 34: nothing to destroy, since nobody knows the member
                        if (copy_kind(ft) == Copy::no || ft->is(Kind::reference)) {
                            note(error(DiagCode::E5001, v.type->span,
                                       std::format("member '{}::{}' has type {}, which is not a copy type", s->name, f->name, quoted(ft)),
                                       "not plain data"),
                                 "a union doesn't know which member it holds, so it can't destroy or copy one: store plain data, "
                                 "or use an enum with payloads");
                        }
                        if (v.init) {
                            error(DiagCode::E5001, v.init->span, std::format("member '{}::{}' of a union has a default", s->name, f->name),
                                  "a literal sets one member");
                        }
                        continue;
                    }
                    if (ft->is(Kind::reference)) {
                        Diagnostic& d = error(DiagCode::E5010, v.type->span,
                                              std::format("field '{}::{}' is a reference", s->name, f->name),
                                              "reference field");
                        note(d, "references only live in parameters, locals and loops; store a value, a smart "
                                "pointer, or declare the type 'view'");
                    } else if (is_view(ft)) {
                        Diagnostic& d = error(DiagCode::E5011, v.type->span,
                                              std::format("field '{}::{}' has view type {}, but '{}' is not a view type",
                                                          s->name, f->name, quoted(ft), s->name),
                                              "view field");
                        note(d, std::format("declare it 'view {} {}', or store an owning type such as std::string",
                                            sema::to_string(s->kind), s->name));
                    }
                }
            } else if (s->kind == SymbolKind::function) {
                check_signature(*s, false);
            }
        }
    }

    // A function returning a reference or view must have one place to borrow from (spec 12).
    void check_signature(const Symbol& s, bool is_method) {
        const FunctionDecl* f = s.function();
        if (!f || !f->result || f->is_destructor) return;
        const Type* r = lower(*f->result, &s);
        if (!is_view(r) || is_method) return;
        // Compile-time-only code returns values of the interpreter, which live for the whole
        // compilation (`std::build::current_mode()`, spec 24).
        if (f->is_compile_time && !f->is_run_time) return;
        std::vector<const Param*> sources;
        for (const Param& p : f->params) {
            // In a template, a parameter whose type depends on the template's parameters
            // (`std::span<T>`) is known only per instance: it may be the place to borrow from.
            const Type* t = lower(*p.type, &s);
            if (is_view(t) || (f->is_template && t && t->kind == Kind::error)) sources.push_back(&p);
        }
        if (sources.size() == 1) return;
        if (sources.empty()) {
            Diagnostic& d = error(DiagCode::E5012, f->result->span,
                                  std::format("'{}' returns {} but has no reference parameter to borrow from", f->name,
                                              quoted(r)),
                                  "nothing to borrow from");
            note(d, "return an owning value (e.g. std::string) instead");
        } else {
            Diagnostic& d = error(DiagCode::E5012, f->result->span,
                                  std::format("'{}' returns {}, which could borrow from '{}' or '{}'", f->name, quoted(r),
                                              sources[0]->name, sources[1]->name),
                                  "ambiguous source");
            note(d, "a returned reference must borrow from 'this' or from the only reference parameter");
        }
    }

    // --- function walk --------------------------------------------------------------------

    void walk_file(ast::File& file) {
        for (DeclPtr& d : file.decls) walk_decl(*d);
    }

    void walk_decl(Decl& d) {
        switch (d.kind) {
        case DeclKind::function:
            if (program_.deferred_errors.count(&d)) break;   // a broken instance method nobody uses
            walk_function(d.as<FunctionDecl>());
            break;
        case DeclKind::struct_:
        case DeclKind::class_:
        case DeclKind::interface:
            for (DeclPtr& m : d.as<TypeDecl>().members) walk_decl(*m);
            break;
        default: break;
        }
    }

    std::map<std::uint32_t, std::string> consumed_by_;   // the span of a consuming call: its method's name

    struct FunctionFrame {
        State                          state;
        std::vector<LoopContext>       loops;
        std::vector<Borrow>            borrows;
        std::vector<ViewBorrow>        views;
        int                            depth = 0;
        std::map<const Symbol*, std::uint32_t> live_end;
        bool                           returns_view = false;
        std::string                    name;
    };

    void walk_function(FunctionDecl& fn) {
        if (!fn.body) return;
        FunctionFrame saved = std::move(frame_);
        frame_              = FunctionFrame{};
        frame_.name         = fn.name;
        const Type* result  = fn.result && (fn.result->kind == ast::TypeKind::deduce || fn.result->kind == ast::TypeKind::auto_)
                                  ? fn.deduced_result
                                  : fn.result ? lower(*fn.result) : nullptr;
        frame_.returns_view = result && is_view(result);
        compute_live_ranges(*fn.body);
        walk_block(*fn.body);
        frame_ = std::move(saved);
    }

    // For every local: the position up to which it is used. A use inside a loop that the
    // local was declared outside of keeps it alive until the end of that loop.
    void compute_live_ranges(BlockExpr& body) {
        std::vector<Span> loop_spans;
        std::function<void(Expr&)> visit = [&](Expr& e) {
            if (e.kind == ExprKind::path) {
                const Symbol* s = e.as<PathExpr>().symbol;
                if (s && s->kind == SymbolKind::local) {
                    std::uint32_t end = e.span.end;
                    for (const Span& l : loop_spans) {
                        if (s->span.begin < l.begin && e.span.begin >= l.begin) end = std::max(end, l.end);
                    }
                    frame_.live_end[s] = std::max(frame_.live_end[s], end);
                }
            }
            bool is_loop = e.kind == ExprKind::while_ || e.kind == ExprKind::for_;
            if (is_loop) loop_spans.push_back(e.span);
            for_each_child(e, visit);
            if (is_loop) loop_spans.pop_back();
        };
        visit(body);
    }

    // Calls `f` on every direct sub-expression (statements included).
    static void for_each_child(Expr& e, const std::function<void(Expr&)>& f) {
        auto stmt = [&](Stmt& s) {
            switch (s.kind) {
            case StmtKind::variable:
                if (s.as<VariableStmt>().init) f(*s.as<VariableStmt>().init);
                break;
            case StmtKind::assign:
                f(*s.as<AssignStmt>().target);
                f(*s.as<AssignStmt>().value);
                break;
            case StmtKind::discard: f(*s.as<DiscardStmt>().value); break;
            case StmtKind::destructure: f(*s.as<DestructureStmt>().init); break;
            case StmtKind::expression: f(*s.as<ExprStmt>().expr); break;
            case StmtKind::check: {   // only the run-time part runs in the program (spec 23)
                Check& c = s.as<CheckStmt>().check;
                if (c.run_active && c.condition) {
                    f(*c.condition);
                    f(*c.message);
                }
                break;
            }
            }
        };
        switch (e.kind) {
        case ExprKind::unary: f(*e.as<UnaryExpr>().operand); break;
        case ExprKind::binary:
            f(*e.as<BinaryExpr>().lhs);
            f(*e.as<BinaryExpr>().rhs);
            break;
        case ExprKind::range:
            f(*e.as<RangeExpr>().lo);
            f(*e.as<RangeExpr>().hi);
            break;
        case ExprKind::call:
            f(*e.as<CallExpr>().callee);
            for (ExprPtr& a : e.as<CallExpr>().args) f(*a);
            if (e.as<CallExpr>().wrapped) f(*e.as<CallExpr>().wrapped);
            break;
        case ExprKind::index:
            f(*e.as<IndexExpr>().object);
            f(*e.as<IndexExpr>().index);
            break;
        case ExprKind::member: f(*e.as<MemberExpr>().object); break;
        case ExprKind::string_literal:
            for (FormatPart& part : e.as<LiteralExpr>().format_parts) {
                if (part.expr) f(*part.expr);
            }
            break;
        case ExprKind::try_: f(*e.as<TryExpr>().operand); break;
        case ExprKind::cast: f(*e.as<CastExpr>().operand); break;
        case ExprKind::bit: f(*e.as<BitExpr>().operand); break;
        case ExprKind::struct_literal:
            for (FieldInit& fi : e.as<StructLiteralExpr>().fields) f(*fi.value);
            break;
        case ExprKind::tuple:
            for (ExprPtr& x : e.as<TupleExpr>().elements) f(*x);
            break;
        case ExprKind::list:
            for (ExprPtr& x : e.as<ListExpr>().elements) f(*x);
            break;
        case ExprKind::lambda:
            for (Capture& c : e.as<LambdaExpr>().captures) {
                if (c.init) f(*c.init);
            }
            f(*e.as<LambdaExpr>().body);
            break;
        case ExprKind::if_: {
            auto& i = e.as<IfExpr>();
            if (i.is_compile_time && i.taken >= 0) {   // only the taken branch exists (spec 15)
                if (i.taken == 0) f(*i.then_block);
                if (i.taken == 1) f(*i.else_branch);
                break;
            }
            f(*i.condition);
            f(*i.then_block);
            if (i.else_branch) f(*i.else_branch);
            break;
        }
        case ExprKind::match:
            f(*e.as<MatchExpr>().scrutinee);
            for (MatchArm& a : e.as<MatchExpr>().arms) {
                if (a.guard) f(*a.guard);
                f(*a.body);
            }
            break;
        case ExprKind::block:
            for (StmtPtr& s : e.as<BlockExpr>().statements) stmt(*s);
            if (e.as<BlockExpr>().tail) f(*e.as<BlockExpr>().tail);
            break;
        case ExprKind::while_:
            f(*e.as<WhileExpr>().condition);
            f(*e.as<WhileExpr>().body);
            break;
        case ExprKind::for_:
            if (e.as<ForExpr>().unrolled) {   // `for compile_time`: the checked copies (spec 26)
                for (BlockPtr& copy : e.as<ForExpr>().copies) f(*copy);
                break;
            }
            f(*e.as<ForExpr>().iterable);
            f(*e.as<ForExpr>().body);
            break;
        case ExprKind::return_:
            if (e.as<ReturnExpr>().value) f(*e.as<ReturnExpr>().value);
            break;
        default: break;
        }
    }

    // --- uses, moves, mutations -----------------------------------------------------------

    static const Symbol* local_of(const Expr& e) {
        if (e.kind != ExprKind::path) return nullptr;
        const Symbol* s = e.as<PathExpr>().symbol;
        return s && s->kind == SymbolKind::local ? s : nullptr;
    }

    // The local an expression's place is rooted in: `a.b[i].c` -> a. `this` has its own root.
    const Symbol* root_of(const Expr& e) {
        switch (e.kind) {
        case ExprKind::path: return local_of(e);
        case ExprKind::this_: return &this_root_;
        case ExprKind::member: return e.as<MemberExpr>().arrow ? nullptr : root_of(*e.as<MemberExpr>().object);
        case ExprKind::index: return root_of(*e.as<IndexExpr>().object);
        case ExprKind::unary: {
            UnaryOp op = e.as<UnaryExpr>().op;
            return op == UnaryOp::move || op == UnaryOp::mut_marker ? root_of(*e.as<UnaryExpr>().operand) : nullptr;
        }
        default: return nullptr;
        }
    }

    void use(const Symbol* s, Span span) {
        if (!frame_.state.reachable) return;
        if (auto it = frame_.state.moved.find(s); it != frame_.state.moved.end()) {
            if (silent_ == 0 && reported_.insert({s, span.begin}).second) {
                bool earlier_iteration = it->second.begin > span.begin;
                Diagnostic& d = error(DiagCode::E5002, span, std::format("'{}' is used after it was moved", s->name),
                                      "used after move");
                std::string where = earlier_iteration ? "moved here, in an earlier iteration of the loop" : "moved here";
                if (auto c = consumed_by_.find(it->second.begin); c != consumed_by_.end()) {
                    where += std::format(" by the consuming method '{}'", c->second);
                }
                note(d, where, it->second);
                note(d, std::format("assign a new value to '{}' first, or move a '.clone()' instead", s->name));
            }
        }
        for (const ViewBorrow& v : frame_.views) {
            if (v.root == s && v.exclusive && view_active(v, span)) view_conflict(v, span, "use");
        }
        for (const LoopContext& l : frame_.loops) {
            if (l.borrowed == s && l.borrow_mut && reaches_loop(l, place_)) {
                Diagnostic& d = error(DiagCode::E5021, span,
                                      std::format("'{}' is used while the loop borrows it mutably through '{}'", s->name, l.var),
                                      "conflicting use");
                note(d, "a 'mut T&' is the only way to reach the object while it is alive", l.span);
            }
        }
    }

    void mutation(const Symbol* root, Span span, std::string_view action, const Expr* place = nullptr) {
        if (!root) return;
        for (const ViewBorrow& v : frame_.views) {
            if (v.root == root && !v.exclusive && view_active(v, span)) view_conflict(v, span, action);
        }
        for (const LoopContext& l : frame_.loops) {
            if (l.borrowed == root && reaches_loop(l, place)) {
                Diagnostic& d = error(DiagCode::E5021, span,
                                      std::format("cannot {} '{}' while the loop borrows it", action, root->name),
                                      "the loop holds a reference");
                note(d, std::format("'{}' refers into '{}' for the whole loop", l.var, root->name), l.span);
            }
        }
        for (const Borrow& b : frame_.borrows) {
            if (b.root == root && span.begin > b.decl.end && span.begin < b.live_end) {
                Diagnostic& d = error(DiagCode::E5022, span,
                                      std::format("cannot {} '{}' while '{}' refers to it", action, root->name, b.ref->name),
                                      "still borrowed");
                note(d, std::format("'{}' borrows from '{}' here and is used later", b.ref->name, root->name), b.decl);
            }
        }
    }

    // --- consumption: copies and implicit moves -------------------------------------------

    // `value` is stored into a place of type `target` (or returned / passed by value).
    void consume(Expr& value, const Type* target, bool is_return = false) {
        // binds a reference or view (std::string -> std::str): no copy. A std::format_args is a view
        // that moves: formatting it consumes it (spec 31).
        const Type* tt = target ? types::strip_ref(target) : nullptr;
        bool format_args = tt && tt->is(Kind::external) && tt->name == "std::format_args" && !target->is(Kind::reference);
        if (target && is_view(target) && !format_args) return;
        check_copy(value, is_return);
    }

    void check_copy(Expr& e, bool is_return) {
        switch (e.kind) {
        case ExprKind::unary: {
            UnaryOp op = e.as<UnaryExpr>().op;
            if (op == UnaryOp::move || op == UnaryOp::mut_marker) return;
            if (op != UnaryOp::deref) return;
            break;   // *p copies the pointee
        }
        case ExprKind::block:
            if (e.as<BlockExpr>().tail) check_copy(*e.as<BlockExpr>().tail, is_return);
            return;
        case ExprKind::if_: {
            auto& i = e.as<IfExpr>();
            check_copy(*i.then_block, is_return);
            if (i.else_branch) check_copy(*i.else_branch, is_return);
            return;
        }
        case ExprKind::match:
            for (MatchArm& a : e.as<MatchExpr>().arms) check_copy(*a.body, is_return);
            return;
        case ExprKind::path: {
            const Symbol* sym = e.as<PathExpr>().symbol;
            if (sym && sym->kind == SymbolKind::alternative) return;   // `std::none`, `token::end`: a new value
            break;
        }
        case ExprKind::member:
        case ExprKind::index: break;
        default: return;   // temporaries are moved
        }
        const Type* t = types::strip_ref(e.type);
        if (copy_kind(t) != Copy::no) return;
        const Symbol* local = local_of(e);
        if (local && frame_.state.moved.count(local)) return;   // already reported as E5002
        if (is_return && local && !local->is_reference &&
            (local->local_kind == LocalKind::variable || local->local_kind == LocalKind::value_param ||
             local->local_kind == LocalKind::binding || local->local_kind == LocalKind::capture)) {
            return;   // `return x;` moves a local implicitly (spec 12)
        }
        std::string what = e.kind == ExprKind::path ? ast::to_string(e.as<PathExpr>().path) : "this value";
        if (e.kind == ExprKind::member) what = "this field";
        Diagnostic& d = error(DiagCode::E5001, e.span,
                              std::format("{} has type {}, which is not a copy type",
                                          e.kind == ExprKind::path ? "'" + what + "'" : what, quoted(t)),
                              "would be copied");
        if (local && !local->is_reference) {
            note(d, std::format("write 'move {0}' to transfer ownership, or '{0}.clone()' for a visible copy", what));
        } else if (e.kind == ExprKind::member) {
            note(d, "a field cannot be moved out: use '.clone()', 'std::take(mut obj.field)' or 'std::replace(...)'");
        } else {
            note(d, "use '.clone()' to make a visible copy");
        }
    }

    // --- statements and expressions -------------------------------------------------------

    void walk_block(BlockExpr& b) {
        std::size_t borrows = frame_.borrows.size();
        ++frame_.depth;
        for (StmtPtr& s : b.statements) walk_stmt(*s);
        if (b.tail) walk(*b.tail);
        frame_.borrows.resize(borrows);   // references declared in the block are gone
        int d = frame_.depth--;
        std::erase_if(frame_.views, [&](const ViewBorrow& v) { return v.depth >= d; });   // so are its views
    }

    // --- views that borrow (spec 12, 27) ----------------------------------------------------

    std::map<const Symbol*, int> holder_depth_;

    // Whether destroying a value of `t` runs code (a destructor somewhere inside).
    bool drops(const Type* t, int depth = 0) {
        if (!t || depth > 20) return false;
        switch (t->kind) {
        case Kind::array: return drops(t->inner, depth + 1);
        case Kind::tuple:
            return std::any_of(t->args.begin(), t->args.end(), [&](const Type* e) { return drops(e, depth + 1); });
        case Kind::function: return true;
        case Kind::closure: {
            const LambdaExpr* l = tc_.lambda_of(t);
            if (!l) return false;
            for (const Capture& c : l->captures) {
                if ((c.kind == CaptureKind::move || c.kind == CaptureKind::init) && c.symbol && drops(c.symbol->type, depth + 1)) return true;
            }
            return false;
        }
        case Kind::user: {
            const Symbol* s = t->symbol;
            for (const Symbol* m : s->member_order) {
                if (m->kind == SymbolKind::method && m->function() && m->function()->is_destructor) return true;
                if (m->kind == SymbolKind::field && !m->is_static && drops(lower(*m->decl->as<VariableDecl>().type, m), depth + 1)) return true;
                if (m->kind == SymbolKind::alternative && m->alternative->payload && drops(lower(*m->alternative->payload, m), depth + 1)) return true;
            }
            if (const Symbol* b = s->base_class()) return drops(tc_.user(b), depth + 1);
            return false;
        }
        default: return false;
        }
    }

    // What the value of `e` borrows: (root, exclusive) pairs.
    std::vector<std::pair<const Symbol*, bool>> borrows_of(const Expr& e) {
        std::vector<std::pair<const Symbol*, bool>> out;
        auto add_holder = [&](const Symbol* holder) {
            for (const ViewBorrow& v : frame_.views) {
                if (v.holder == holder) out.push_back({v.root, v.exclusive});
            }
        };
        switch (e.kind) {
        case ExprKind::lambda:
            for (const Capture& c : e.as<LambdaExpr>().captures) {
                const Symbol* outer = c.captured;
                if (!outer) continue;
                if (c.kind == CaptureKind::ref || c.kind == CaptureKind::mut_ref) {
                    if (outer->kind == SymbolKind::local) out.push_back({outer, c.kind == CaptureKind::mut_ref});
                    add_holder(outer);
                } else {
                    add_holder(outer);   // a view captured by copy or move keeps its borrows
                }
            }
            return out;
        case ExprKind::path:
            if (const Symbol* l = local_of(e)) add_holder(l);
            return out;
        case ExprKind::unary: {
            const auto& u = e.as<UnaryExpr>();
            if (u.op == UnaryOp::move || u.op == UnaryOp::mut_marker) return borrows_of(*u.operand);
            return out;
        }
        case ExprKind::call: {
            const auto& c = e.as<CallExpr>();
            if (!c.type || !is_view(c.type) || c.type->is(Kind::reference)) return out;
            if (c.callee->kind == ExprKind::member) {   // `s.as_str()`, `list.as_mut_span()`: borrows the receiver
                const auto& me = c.callee->as<MemberExpr>();
                const FunctionDecl* fn = me.symbol ? me.symbol->function() : nullptr;
                if (fn && !fn->is_static) {
                    const Symbol* root = root_of(*me.object);
                    if (root && root != &this_root_) out.push_back({root, fn->is_mut});
                    if (root) add_holder(root);
                    if (!root) {   // a method of a temporary view: `list.as_mut_span().chunks(n)`
                        for (auto b : borrows_of(*me.object)) out.push_back(b);
                    }
                }
            }
            // A function with exactly one view argument returns something that borrows what that
            // argument borrows (spec 12): `std::thread::spawn([&x]() { ... })`.
            const Expr* only = nullptr;
            int         views = 0;
            for (const ExprPtr& a : c.args) {
                if (a->type && is_view(a->type)) {
                    ++views;
                    only = a.get();
                }
            }
            if (views == 1) {
                for (auto b : borrows_of(*only)) out.push_back(b);
            }
            return out;
        }
        default: return out;
        }
    }

    // `holder` (declared at `at`) now borrows what `value` borrows.
    void hold(const Symbol* holder, const Expr& value, Span at) {
        if (!holder) return;
        int depth = holder_depth_.contains(holder) ? holder_depth_[holder] : frame_.depth;
        bool to_end = drops(types::strip_ref(holder->type));
        for (auto [root, exclusive] : borrows_of(value)) {
            if (root == holder) continue;
            bool known = std::any_of(frame_.views.begin(), frame_.views.end(), [&](const ViewBorrow& v) {
                return v.holder == holder && v.root == root && v.at.begin == at.begin;
            });
            if (!known) frame_.views.push_back(ViewBorrow{holder, root, exclusive, at, to_end, depth});
        }
    }

    bool view_active(const ViewBorrow& v, Span span) {
        if (span.begin <= v.at.end) return false;
        if (frame_.state.moved.count(v.holder)) return false;   // joined, consumed or moved away
        if (v.to_scope_end) return true;
        auto it = frame_.live_end.find(v.holder);
        return it != frame_.live_end.end() && span.begin < it->second;
    }

    void view_conflict(const ViewBorrow& v, Span span, std::string_view action) {
        if (silent_ != 0 || !reported_.insert({v.root, span.begin}).second) return;
        Diagnostic& d = v.exclusive
            ? error(DiagCode::E5023, span,
                    std::format("'{}' is already borrowed mutably by '{}'", v.root->name, v.holder->name), "borrowed mutably")
            : error(DiagCode::E5022, span,
                    std::format("cannot {} '{}' while '{}' borrows it", action, v.root->name, v.holder->name), "still borrowed");
        note(d, v.to_scope_end ? std::format("the borrow lasts until '{}' is joined, consumed or destroyed", v.holder->name)
                               : std::format("the borrow lasts until the last use of '{}'", v.holder->name),
             v.at);
        if (v.exclusive) {
            note(d, "a 'mut &' borrow is the only way to reach the value while it lasts; to share a counter between threads "
                    "use a std::atomic or a std::mutex (spec 27)");
        }
    }

    void walk_stmt(Stmt& s) {
        switch (s.kind) {
        case StmtKind::check: {
            Check& c = s.as<CheckStmt>().check;
            if (c.run_active && c.condition) {
                walk(*c.condition);
                walk(*c.message);
            }
            return;
        }
        case StmtKind::variable: {
            auto& v = s.as<VariableStmt>();
            if (v.init) {
                walk(*v.init);
                consume(*v.init, v.symbol ? v.symbol->type : nullptr);
            }
            if (v.symbol) {
                frame_.state.moved.erase(v.symbol);
                const Type* t = v.symbol->type;
                holder_depth_[v.symbol] = frame_.depth;
                if (t && !t->is(Kind::reference) && is_view(t) && v.init) hold(v.symbol, *v.init, v.span);
                if (t && t->is(Kind::reference) && v.init) {
                    if (const Symbol* root = root_of(*v.init); root && root != &this_root_) {
                        frame_.borrows.push_back(Borrow{v.symbol, root, v.span, frame_.live_end[v.symbol]});
                    }
                }
            }
            return;
        }
        case StmtKind::assign: {
            auto& a = s.as<AssignStmt>();
            walk(*a.value);
            const Symbol* target_local = local_of(*a.target);
            if (a.op == TokenKind::eq && target_local) {
                mutation(target_local, a.target->span, "assign to");
                frame_.state.moved.erase(target_local);   // a moved-from variable is alive again
            } else {
                walk(*a.target);
                mutation(root_of(*a.target), a.target->span, "change", a.target.get());
            }
            if (a.op == TokenKind::eq) consume(*a.value, a.target->type);
            return;
        }
        case StmtKind::discard: walk(*s.as<DiscardStmt>().value); return;
        case StmtKind::expression: walk(*s.as<ExprStmt>().expr); return;
        case StmtKind::destructure: {   // consumes the tuple: its elements move into the new variables
            auto& d = s.as<DestructureStmt>();
            walk(*d.init);
            consume(*d.init, d.init->type);
            for (DestructureBinding& b : d.bindings) {
                if (!b.symbol) continue;
                frame_.state.moved.erase(b.symbol);
                holder_depth_[b.symbol] = frame_.depth;
                if (b.symbol->type && is_view(b.symbol->type)) hold(b.symbol, *d.init, d.span);
            }
            return;
        }
        }
    }

    void walk(Expr& e) {
        switch (e.kind) {
        case ExprKind::path:
            if (const Symbol* s = local_of(e)) use(s, e.span);
            return;
        case ExprKind::unary: {
            auto& u = e.as<UnaryExpr>();
            walk(*u.operand);
            if (u.op == UnaryOp::move) {
                if (const Symbol* s = local_of(*u.operand)) {
                    mutation(s, e.span, "move");
                    if (frame_.state.reachable) frame_.state.moved[s] = e.span;
                }
            } else if (u.op == UnaryOp::address_of_mut) {
                mutation(root_of(*u.operand), e.span, "take a mutable pointer to");
            }
            return;
        }
        case ExprKind::call:
            if (e.as<CallExpr>().wrapped) {   // the wrapped function body: its returns end only the body
                State saved = frame_.state;
                walk_block(*e.as<CallExpr>().wrapped);
                frame_.state = saved;
                return;
            }
            walk_call(e.as<CallExpr>());
            return;
        case ExprKind::try_: {
            Expr& operand = *e.as<TryExpr>().operand;
            walk(operand);
            consume(operand, nullptr);   // `r?` takes the value out of `r` (spec 11)
            return;
        }
        case ExprKind::struct_literal: {
            auto&       sl = e.as<StructLiteralExpr>();
            const Type* t  = types::strip_ref(e.type);
            for (FieldInit& f : sl.fields) {
                walk(*f.value);
                const Type* ft = nullptr;
                if (t && t->is(Kind::user)) {
                    if (auto it = t->symbol->members.find(f.name); it != t->symbol->members.end()) {
                        ft = lower(*it->second.front()->decl->as<VariableDecl>().type, it->second.front());
                    }
                }
                consume(*f.value, ft);
            }
            return;
        }
        case ExprKind::tuple: {
            auto&       te = e.as<TupleExpr>();
            const Type* t  = types::strip_ref(e.type);
            for (std::size_t i = 0; i < te.elements.size(); ++i) {
                walk(*te.elements[i]);
                consume(*te.elements[i], t && t->is(Kind::tuple) && i < t->args.size() ? t->args[i] : nullptr);
            }
            return;
        }
        case ExprKind::list: {
            auto&       le = e.as<ListExpr>();
            const Type* lt = types::strip_ref(e.type);
            const Type* el = nullptr;   // the element type: std::span<T>, std::array<T, N>, std::raw::array<T, N>
            if (lt && lt->is(Kind::array)) el = lt->inner;
            else if (lt && lt->is(Kind::user) && !lt->symbol->template_args.empty()) el = lt->symbol->template_args[0];
            for (ExprPtr& x : le.elements) {
                walk(*x);
                consume(*x, el ? el : x->type);
            }
            return;
        }
        case ExprKind::lambda: walk_lambda(e.as<LambdaExpr>()); return;
        case ExprKind::if_: {
            auto& i = e.as<IfExpr>();
            if (i.is_compile_time && i.taken >= 0) {
                if (i.taken == 0) walk_block(*i.then_block);
                if (i.taken == 1) walk(*i.else_branch);
                return;
            }
            walk(*i.condition);
            State before = frame_.state;
            walk_block(*i.then_block);
            State then_state = frame_.state;
            frame_.state     = before;
            if (i.else_branch) walk(*i.else_branch);
            frame_.state = merge(then_state, frame_.state);
            return;
        }
        case ExprKind::match: {
            auto& m = e.as<MatchExpr>();
            walk(*m.scrutinee);
            State before = frame_.state;
            State after  = State::unreachable();
            for (MatchArm& arm : m.arms) {
                frame_.state = before;
                forget_bindings(*arm.pattern);   // fresh bindings in every arm (and iteration)
                if (arm.guard) walk(*arm.guard);
                walk(*arm.body);
                after = merge(after, frame_.state);
            }
            if (!m.arms.empty()) frame_.state = after;
            return;
        }
        case ExprKind::block: walk_block(e.as<BlockExpr>()); return;
        case ExprKind::while_: {
            auto& w = e.as<WhileExpr>();
            bool  forever = w.condition->kind == ExprKind::bool_literal && w.condition->as<LiteralExpr>().bool_value;
            walk_loop(w.condition.get(), *w.body, nullptr, nullptr, false, e.span, "", forever);
            return;
        }
        case ExprKind::for_: {
            auto& f = e.as<ForExpr>();
            if (f.unrolled) {   // the copies run one after the other; break/continue leave a copy
                frame_.loops.push_back(LoopContext{});
                for (BlockPtr& copy : f.copies) {
                    walk_block(*copy);
                    frame_.state                    = merge(frame_.state, frame_.loops.back().continues);
                    frame_.loops.back().continues   = State::unreachable();
                }
                State breaks = frame_.loops.back().breaks;
                frame_.loops.pop_back();
                frame_.state = merge(frame_.state, breaks);
                return;
            }
            walk(*f.iterable);
            if (f.symbol && f.symbol->type && is_view(f.symbol->type)) {   // `for (std::span<mut T> part : list.as_mut_span().chunks(n))`
                holder_depth_[f.symbol] = frame_.depth;
                hold(f.symbol, *f.iterable, f.iterable->span);
            }
            const Symbol* borrowed = nullptr;
            bool          by_ref   = f.type->kind == TypeKind::reference;
            if (by_ref) borrowed = root_of(*f.iterable);
            if (borrowed == &this_root_) borrowed = nullptr;
            bool is_mut = by_ref && f.type->as<ReferenceType>().is_mut;
            loop_path_.clear();
            if (borrowed) {   // `for (f32& v : w.b)` borrows w.b, not w.a (spec 34)
                bool ended = false;
                field_path(*f.iterable, loop_path_, ended);
            }
            walk_loop(nullptr, *f.body, f.symbol, borrowed, is_mut, e.span, f.name, false);
            return;
        }
        case ExprKind::return_: {
            auto& r = e.as<ReturnExpr>();
            if (r.value) {
                walk(*r.value);
                if (!frame_.returns_view) consume(*r.value, nullptr, /*is_return=*/true);   // a reference is not a copy
                if (frame_.returns_view) check_returned_reference(*r.value);
            }
            frame_.state = State::unreachable();
            return;
        }
        case ExprKind::break_:
            if (!frame_.loops.empty()) frame_.loops.back().breaks = merge(frame_.loops.back().breaks, frame_.state);
            frame_.state = State::unreachable();
            return;
        case ExprKind::continue_:
            if (!frame_.loops.empty()) frame_.loops.back().continues = merge(frame_.loops.back().continues, frame_.state);
            frame_.state = State::unreachable();
            return;
        case ExprKind::member:
        case ExprKind::index: {   // `w.a`, `w.a[i]`: the place used, for loops that borrow another field (spec 34)
            const Expr* saved = place_;
            if (!place_) place_ = &e;
            for_each_child(e, [&](Expr& c) { walk(c); });
            place_ = saved;
            return;
        }
        default: {
            const Expr* saved = std::exchange(place_, nullptr);   // arguments, operands: places of their own
            for_each_child(e, [&](Expr& c) { walk(c); });
            place_ = saved;
            return;
        }
        }
    }

    const Expr* place_ = nullptr;   // the member or index expression whose root is being walked
    std::vector<std::string> loop_path_;   // the place the next loop borrows

    // Whether `place` (null: the whole variable) can reach what the loop `l` borrows.
    bool reaches_loop(const LoopContext& l, const Expr* place) {
        if (!place || l.path.empty()) return true;
        std::vector<std::string> path;
        bool                     ended = false;
        field_path(*place, path, ended);
        return overlaps(path, l.path);
    }

    void forget_bindings(const Pattern& p) {
        switch (p.kind) {
        case PatternKind::binding:
            if (p.as<BindingPattern>().symbol) frame_.state.moved.erase(p.as<BindingPattern>().symbol);
            return;
        case PatternKind::enum_alternative:
            if (p.as<EnumPattern>().payload) forget_bindings(*p.as<EnumPattern>().payload);
            return;
        case PatternKind::struct_:
            for (const FieldPattern& f : p.as<StructPattern>().fields) forget_bindings(*f.pattern);
            return;
        default: return;
        }
    }

    void walk_loop(Expr* cond, BlockExpr& body, const Symbol* loop_var, const Symbol* borrowed, bool borrow_mut,
                   Span span, std::string var, bool forever) {
        State pre   = frame_.state;
        State entry = pre;
        // Find the state at the loop head by iterating to a fixpoint without reporting,
        // then walk once more reporting errors.
        for (int pass = 0; pass < 4; ++pass) {
            ++silent_;
            LoopContext ctx;
            frame_.loops.push_back(ctx);
            frame_.state = entry;
            if (loop_var) frame_.state.moved.erase(loop_var);
            if (cond) walk(*cond);
            walk_block(body);
            State end = merge(frame_.state, frame_.loops.back().continues);
            frame_.loops.pop_back();
            --silent_;
            State next = merge(pre, end);
            if (next == entry) break;
            entry = next;
        }
        LoopContext ctx;
        ctx.borrowed   = borrowed;
        ctx.borrow_mut = borrow_mut;
        ctx.path       = std::move(loop_path_);
        loop_path_.clear();
        ctx.span       = span;
        ctx.var        = std::move(var);
        frame_.loops.push_back(ctx);
        frame_.state = entry;
        if (loop_var) frame_.state.moved.erase(loop_var);
        if (cond) walk(*cond);
        State on_exit = forever ? State::unreachable() : frame_.state;
        walk_block(body);
        State end    = merge(frame_.state, frame_.loops.back().continues);
        State breaks = frame_.loops.back().breaks;
        frame_.loops.pop_back();
        if (!cond && !forever) on_exit = merge(entry, end);   // for: leaves when the range is done
        frame_.state = merge(on_exit, breaks);
    }

    void walk_call(CallExpr& c) {
        Expr&         callee = *c.callee;
        const FunctionDecl* fn = nullptr;
        const Symbol*       owner_sym = nullptr;
        const Symbol* receiver = nullptr;
        bool          mut_receiver = false;
        const EnumAlternative* alt = nullptr;
        if (callee.kind == ExprKind::member) {
            auto& me = callee.as<MemberExpr>();
            walk(*me.object);
            if (me.symbol) fn = me.symbol->function();
            if (me.symbol && me.symbol->kind == SymbolKind::function) {   // `text.split(c)`: str_split(text, c)
                for (ExprPtr& a : c.args) walk(*a);
                return;
            }
            if (!me.symbol && types::is_mutating_library_method(types::strip_ref(me.object->type), me.name.segments.back().name)) {
                receiver     = root_of(*me.object);
                mut_receiver = true;
                mutation(receiver, me.object->span, std::format("call '{}' on", me.name.segments.back().name), me.object.get());
            }
            if (fn && fn->is_mut) {
                receiver     = root_of(*me.object);
                mut_receiver = true;
                mutation(receiver, me.object->span, std::format("call mut method '{}' on", fn->name), me.object.get());
                // A view passed to a mut method of a view (`running.push(handle)`) borrows as long
                // as that object is alive (spec 12).
                if (const Symbol* holder = local_of(*me.object); holder && is_view(types::strip_ref(holder->type))) {
                    for (ExprPtr& a : c.args) {
                        if (a->type && is_view(a->type)) hold(holder, *a, c.span);
                    }
                }
            }
            if (fn && fn->is_move) {   // a consuming method (spec 06, 27): the receiver is moved
                const Expr* o = me.object.get();
                if (o->kind == ExprKind::unary && o->as<UnaryExpr>().op == UnaryOp::move) o = o->as<UnaryExpr>().operand.get();
                if (const Symbol* l = local_of(*o)) {
                    mutation(l, me.object->span, std::format("call the consuming method '{}' on", fn->name));
                    for (ExprPtr& a : c.args) {
                        walk(*a);
                        consume(*a, nullptr);
                    }
                    if (frame_.state.reachable) {
                        frame_.state.moved[l] = me.name.span;
                        consumed_by_[me.name.span.begin] = fn->name;
                    }
                    return;
                }
            }
            check_clone(c, me);
            if (!me.symbol && types::is_std_query_method(me.name.segments.back().name) &&
                types::is_std_builtin_method(me.object->type, me.name.segments.back().name)) {
                return;   // is_some and friends only look
            }
            if (!me.symbol && types::is_std_builtin_method(me.object->type, me.name.segments.back().name)) {
                consume(*me.object, nullptr);   // value_or, expect, or_error, map_error take the value (spec 11)
                for (ExprPtr& a : c.args) {
                    walk(*a);
                    consume(*a, nullptr);
                }
                return;
            }
        } else if (callee.kind == ExprKind::path) {
            if (ct::raw_intrinsic(c) == "write" && c.args.size() == 2) {   // the value moves into memory (spec 13)
                walk(*c.args[0]);
                walk(*c.args[1]);
                consume(*c.args[1], nullptr);
                return;
            }
            const Symbol* s = callee.as<PathExpr>().symbol;
            if (s && s->kind == SymbolKind::local) use(s, callee.span);
            if (s && (s->kind == SymbolKind::function || s->kind == SymbolKind::method)) fn = s->function(), owner_sym = s;
            if (s && s->kind == SymbolKind::alternative) alt = s->alternative, owner_sym = s;
        } else {
            walk(callee);
        }
        for (std::size_t i = 0; i < c.args.size(); ++i) {
            Expr& arg = *c.args[i];
            walk(arg);
            bool marked = arg.kind == ExprKind::unary && arg.as<UnaryExpr>().op == UnaryOp::mut_marker;
            if (marked) mutation(root_of(arg), arg.span, "pass as a mutable reference", &arg);
            const Type* param = nullptr;
            if (fn && i < fn->params.size() && (!fn->is_template || (owner_sym && owner_sym->template_of))) {
                param = lower(*fn->params[i].type, owner_sym);
            }
            if (alt && alt->payload) param = lower(*alt->payload, owner_sym);
            // std::println, std::format, std::panic (compiler built-ins) consume a std::format_args (spec 31).
            const Type* at = types::strip_ref(arg.type);
            if (!fn && !alt && at && at->is(Kind::external) && at->name == "std::format_args" && arg.kind == ExprKind::path) {
                param = at;
            }
            if (param && !marked) consume(arg, param);
        }
        check_aliasing(c, receiver, mut_receiver);
    }

    // The fields from the root to a place (spec 34, disjoint borrows): `w.a.b` -> a, b. An index
    // ends the path with "[]": the elements of a container are one place (`xs[i]` and `xs[j]` may
    // be the same element), and so is everything inside them.
    void field_path(const Expr& e, std::vector<std::string>& out, bool& ended) {
        switch (e.kind) {
        case ExprKind::member: {
            const auto& me = e.as<MemberExpr>();
            field_path(*me.object, out, ended);
            if (!ended) out.push_back(me.name.segments.back().name);
            return;
        }
        case ExprKind::index:
            field_path(*e.as<IndexExpr>().object, out, ended);
            if (!ended) out.push_back("[]");
            ended = true;
            return;
        case ExprKind::unary: {
            UnaryOp op = e.as<UnaryExpr>().op;
            if (op == UnaryOp::move || op == UnaryOp::mut_marker) field_path(*e.as<UnaryExpr>().operand, out, ended);
            return;
        }
        default: return;
        }
    }

    // Two places of one root overlap unless their paths part ways at some field: `w.a` and
    // `w.b` are disjoint, `w` and `w.a` are not.
    static bool overlaps(const std::vector<std::string>& a, const std::vector<std::string>& b) {
        for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
            if (a[i] != b[i]) return false;
        }
        return true;
    }

    // One mutable reference at a time: `f(mut a, a)`, `f(mut a, mut a)`, `a.push(mut a)`. Different
    // fields of one value are different places: `f(mut w.positions, w.velocities)` (spec 34).
    void check_aliasing(CallExpr& c, const Symbol* receiver, bool mut_receiver) {
        struct Arg {
            const Symbol*            root;
            bool                     is_mut;
            Span                     span;
            std::vector<std::string> path;
        };
        std::vector<Arg> args;
        if (receiver) {
            Arg r{receiver, mut_receiver, c.callee->span, {}};
            if (c.callee->kind == ExprKind::member) {
                bool ended = false;
                field_path(*c.callee->as<MemberExpr>().object, r.path, ended);
            }
            args.push_back(std::move(r));
        }
        for (ExprPtr& a : c.args) {
            bool marked = a->kind == ExprKind::unary && a->as<UnaryExpr>().op == UnaryOp::mut_marker;
            const Symbol* root = root_of(*a);
            if (a->kind == ExprKind::unary && a->as<UnaryExpr>().op == UnaryOp::move) root = nullptr;
            if (!root) continue;
            Arg  arg{root, marked, a->span, {}};
            bool ended = false;
            field_path(*a, arg.path, ended);
            args.push_back(std::move(arg));
        }
        for (std::size_t i = 0; i < args.size(); ++i) {
            for (std::size_t j = i + 1; j < args.size(); ++j) {
                if (args[i].root != args[j].root || !(args[i].is_mut || args[j].is_mut)) continue;
                if (!overlaps(args[i].path, args[j].path)) continue;
                const Arg& m = args[i].is_mut ? args[i] : args[j];
                const Arg& o = args[i].is_mut ? args[j] : args[i];
                std::string name = args[i].root == &this_root_ ? "this" : args[i].root->name;
                const std::vector<std::string>& shorter = args[i].path.size() <= args[j].path.size() ? args[i].path : args[j].path;
                for (const std::string& part : shorter) {
                    if (part == "[]") break;
                    name += "." + part;
                }
                Diagnostic& d = error(DiagCode::E5020, o.span,
                                      std::format("'{}' is passed as a mutable reference and also as another argument", name),
                                      "second reference");
                note(d, "the mutable reference is here; while it exists it must be the only way to reach the value", m.span);
                return;
            }
        }
    }

    void check_clone(CallExpr& c, MemberExpr& me) {
        if (me.symbol || !c.args.empty() || me.name.segments.back().name != "clone") return;
        const Type* t = types::strip_ref(me.object->type);
        if (!t || !t->is(Kind::user)) return;
        std::string why = clone_blocker(t);
        if (why.empty()) return;
        Diagnostic& d = error(DiagCode::E5030, c.span, std::format("{} cannot be cloned", quoted(t)), "not cloneable");
        note(d, why);
    }

    void walk_lambda(LambdaExpr& l) {
        for (Capture& cap : l.captures) {
            if (cap.init) {
                walk(*cap.init);
                consume(*cap.init, nullptr);
                continue;
            }
            const Symbol* outer = cap.captured;
            if (!outer) continue;
            use(outer, cap.span);
            if (cap.kind == CaptureKind::move) {
                mutation(outer, cap.span, "move");
                if (frame_.state.reachable) frame_.state.moved[outer] = cap.span;
            } else if (cap.kind == CaptureKind::copy && copy_kind(types::strip_ref(outer->type)) == Copy::no) {
                Diagnostic& d = error(DiagCode::E5001, cap.span,
                                      std::format("capturing '{}' by copy would copy a {}", outer->name,
                                                  quoted(types::strip_ref(outer->type))),
                                      "copy capture");
                note(d, std::format("capture it as '[move {0}]', '[&{0}]', or '[{0} = {0}.clone()]'", outer->name));
            } else if (cap.kind == CaptureKind::mut_ref) {
                mutation(outer, cap.span, "capture a mutable reference to");
            }
        }
        // The body is its own function: fresh moves, no enclosing loops.
        FunctionFrame saved  = std::move(frame_);
        frame_               = FunctionFrame{};
        frame_.name          = "lambda";
        frame_.live_end      = saved.live_end;
        walk_block(*l.body);
        frame_ = std::move(saved);
    }

    // A returned reference must point into `this` or into a reference parameter.
    void check_returned_reference(Expr& value) {
        if (value.kind == ExprKind::tuple) {   // `(mut this.a, this.b)` (spec 34): each element, and distinct places
            const Type* t = types::strip_ref(value.type);
            auto&       elements = value.as<TupleExpr>().elements;
            struct Ref {
                const Symbol*            root;
                bool                     is_mut;
                Span                     span;
                std::vector<std::string> path;
            };
            std::vector<Ref> refs;
            for (std::size_t i = 0; i < elements.size(); ++i) {
                Expr& el = *elements[i];
                if (!t || !t->is(Kind::tuple) || i >= t->args.size() || !(t->args[i]->is(Kind::reference) || is_view(t->args[i]))) continue;
                check_returned_reference(el);
                Ref r{root_of(el), t->args[i]->is(Kind::reference) && t->args[i]->is_mut, el.span, {}};
                bool ended = false;
                field_path(el, r.path, ended);
                if (r.root) refs.push_back(std::move(r));
            }
            for (std::size_t i = 0; i < refs.size(); ++i) {
                for (std::size_t j = i + 1; j < refs.size(); ++j) {
                    if (refs[i].root != refs[j].root || !(refs[i].is_mut || refs[j].is_mut) || !overlaps(refs[i].path, refs[j].path)) continue;
                    const Ref& m = refs[i].is_mut ? refs[i] : refs[j];
                    const Ref& o = refs[i].is_mut ? refs[j] : refs[i];
                    note(error(DiagCode::E5020, o.span, "the references of a returned tuple must be distinct fields",
                               "overlaps a mutable reference"),
                         "the mutable reference is here; while it exists it must be the only way to reach the value", m.span);
                    return;
                }
            }
            return;
        }
        if (value.kind == ExprKind::lambda) {   // what a returned lambda refers to must outlive the call
            for (Capture& c : value.as<LambdaExpr>().captures) {
                if ((c.kind != CaptureKind::ref && c.kind != CaptureKind::mut_ref) || !c.captured) continue;
                const Symbol* s = c.captured;
                if (s->is_reference || s->local_kind == LocalKind::ref_param) continue;
                note(error(DiagCode::E5013, c.span,
                           std::format("returns a lambda that refers to '{}', which is destroyed when '{}' returns", s->name, frame_.name),
                           "does not outlive the function"),
                     std::format("capture it by value ('[{0}]' or '[move {0}]') instead", s->name));
            }
            return;
        }
        if (value.kind == ExprKind::call && types::is_std_instance(types::strip_ref(value.type), "option")) {
            // `std::some(place)` as a `std::option<T&>` borrows the place (spec 20)
            auto&       c    = value.as<CallExpr>();
            const auto& args = types::strip_ref(value.type)->symbol->template_args;
            bool payload_ref = !args.empty() && args[0] && args[0]->is(Kind::reference);
            if (payload_ref && c.args.size() == 1 && c.callee->kind == ExprKind::path) {
                check_returned_reference(*c.args[0]);
                return;
            }
        }
        const Symbol* root = root_of(value);
        if (root == &this_root_) return;
        if (!root) {
            if (value.kind == ExprKind::struct_literal) {
                // A new view value borrows what its fields borrow (spec 12); a reference to the
                // literal itself would dangle.
                if (value.type && is_view(value.type) && !value.type->is(Kind::reference)) {
                    const Type* t = types::strip_ref(value.type);
                    for (FieldInit& f : value.as<StructLiteralExpr>().fields) {
                        const Type* ft = nullptr;   // a reference field borrows its value's place
                        if (t && t->is(Kind::user)) {
                            if (auto it = t->symbol->members.find(f.name); it != t->symbol->members.end()) {
                                ft = lower(*it->second.front()->decl->as<VariableDecl>().type, it->second.front());
                            }
                        }
                        if (f.value && (is_view(f.value->type) || (ft && ft->is(Kind::reference)))) check_returned_reference(*f.value);
                    }
                    return;
                }
                error(DiagCode::E5013, value.span, "returns a reference to a temporary", "destroyed at the end of the statement");
            }
            return;
        }
        if (root->is_reference || is_view(root->type) || root->local_kind == LocalKind::ref_param) return;
        Diagnostic& d = error(DiagCode::E5013, value.span,
                              std::format("returns a reference into {} '{}', which is destroyed when '{}' returns",
                                          root->local_kind == LocalKind::value_param ? "by-value parameter" : "local",
                                          root->name, frame_.name),
                              "does not outlive the function");
        note(d, "return an owning value instead, or borrow from a reference parameter");
    }

    sema::Program&                         program_;
    DiagnosticEngine&                      diags_;
    types::TypeContext&                    tc_;
    Module*                                module_ = nullptr;
    const std::map<const Symbol*, const types::Type*>* cur_subst_ = nullptr;   // of the instance being walked
    FunctionFrame                          frame_;
    int                                    silent_ = 0;
    Diagnostic                             scratch_{DiagCode::E5001, Severity::error, {}, {}, {}, {}};
    std::set<std::pair<const Symbol*, std::uint32_t>> reported_;
    std::map<const Symbol*, Copy>          copy_memo_;
    std::map<const Symbol*, bool>          view_memo_;
    Symbol                                 this_root_;   // stands for `this` in root_of()
};

} // namespace

void check_ownership(sema::Program& program, DiagnosticEngine& diags) {
    if (!program.types) return;
    Ownership(program, diags).run();
}

} // namespace edax
