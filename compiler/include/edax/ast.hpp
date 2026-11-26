#pragma once
// Abstract syntax tree produced by the parser.
//
// The tree is purely syntactic: names are not resolved and nothing is type-checked. Every node
// owns its children through unique_ptr and carries the Span it was parsed from.
//
// Node families (each with a `kind` tag; use `as<T>()` to downcast):
//   TypeExpr  written types:  u32, std::darray<employee>, T&, R(Args)
//   Expr      expressions, including block-like ones (if, match, { }) and return/break/continue
//   Pattern   match patterns
//   Stmt      statements inside blocks
//   Decl      top-level and member declarations

#include "edax/source.hpp"
#include "edax/token.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace edax::sema {
struct Symbol;
}
namespace edax::types {
struct Type;
}
namespace edax::ct {
struct Value;
struct BuildGraph;
}

namespace edax::ast {

// Filled in by name resolution (M3); null until then or when unresolved.
using SymbolRef = sema::Symbol*;

struct TypeExpr;
struct Expr;
struct Pattern;
struct Stmt;
struct Decl;
struct BlockExpr;

using TypePtr    = std::unique_ptr<TypeExpr>;
using ExprPtr    = std::unique_ptr<Expr>;
using PatternPtr = std::unique_ptr<Pattern>;
using StmtPtr    = std::unique_ptr<Stmt>;
using DeclPtr    = std::unique_ptr<Decl>;
using BlockPtr   = std::unique_ptr<BlockExpr>;

// Base for every node family: a kind tag plus the source span.
template <typename Kind>
struct Node {
    Kind kind;
    Span span;

    explicit Node(Kind k, Span s) : kind(k), span(s) {}
    virtual ~Node() = default;
    Node(const Node&)            = delete;
    Node& operator=(const Node&) = delete;

    template <typename T> T&       as() { return static_cast<T&>(*this); }
    template <typename T> const T& as() const { return static_cast<const T&>(*this); }
};

// --- node family bases (defined first so unique_ptr members can be destroyed) ------

enum class TypeKind { named, reference, function, auto_, deduce, tuple };

struct TypeExpr : Node<TypeKind> {
    using Node::Node;
};

enum class ExprKind {
    int_literal, float_literal, char_literal, string_literal, bool_literal, nullptr_literal,
    this_, default_, path, unary, binary, range, call, index, member, try_, cast, bit,
    struct_literal, lambda, if_, match, block, while_, for_, return_, break_, continue_, error,
    tuple, list,
};

struct Expr : Node<ExprKind> {
    using Node::Node;
    const types::Type* type = nullptr;   // filled in by the type checker (M4)
    // The value, when the checker computed it at compile time and code generation should use it
    // as a constant (compile_time constants, `f.name` in a `for compile_time` loop, ...).
    std::shared_ptr<const ct::Value> constant;
    // An implicit conversion the checker found where the value is used (spec 19): a
    // `std::string` read as a `std::str`, or a literal that becomes a `std::string`.
    enum class Conversion : unsigned char { none, string_to_str, literal_to_string, pointer_upcast, to_callable };
    Conversion conversion = Conversion::none;
    const types::Type* converted_to = nullptr;   // pointer_upcast: `std::unique_ptr<shape>` from `<circle>`
};

enum class PatternKind { wildcard, literal, range, binding, enum_alternative, struct_, error, tuple };

struct Pattern : Node<PatternKind> {
    using Node::Node;
};

enum class StmtKind { variable, assign, discard, expression, check, destructure };

struct Stmt : Node<StmtKind> {
    using Node::Node;
    // `@std::allow{ .id = gfx::deprecated } stmt;`: the warning ids silenced for this statement
    // alone (the `.id` values, as written).
    std::vector<ExprPtr> allowed;
};

// --- paths and template arguments ---------------------------------------------------------

// One argument in `<...>`: a type (`employee`, `mut T`) or a value (`16`, `N + 1`).
// A bare name like `buffer_size` is parsed as a type; name resolution decides later.
struct TemplateArg {
    TypePtr type;
    ExprPtr value;
    bool    is_mut = false;   // `std::raw_ptr<mut T>`
    Span    span;
    // A value argument, once the checker has evaluated it (spec 14, 15): an interned value type.
    mutable const types::Type* evaluated = nullptr;
};

struct PathSegment {
    std::string              name;
    Span                     span;
    bool                     has_template_args = false;
    std::vector<TemplateArg> template_args;
};

// `std::darray<employee>::make`, `u32`, `parse_error::bad_line`.
struct Path {
    std::vector<PathSegment> segments;
    Span                     span;
    bool                     global = false;   // `::name`: starts at the global namespace (spec 30)
};

// --- types --------------------------------------------------------------------------------

struct NamedType : TypeExpr {
    Path path;
    SymbolRef symbol = nullptr;   // the type (struct/class/enum/interface/template parameter)
    NamedType(Span s, Path p) : TypeExpr(TypeKind::named, s), path(std::move(p)) {}
};

// `T&` or `mut T&`.
struct ReferenceType : TypeExpr {
    TypePtr inner;
    bool    is_mut = false;
    ReferenceType(Span s, TypePtr i) : TypeExpr(TypeKind::reference, s), inner(std::move(i)) {}
};

// `R(A, B)` inside template arguments: `std::function<u32(u32)>`.
struct FunctionType : TypeExpr {
    TypePtr              result;
    std::vector<TypePtr> params;
    FunctionType(Span s, TypePtr r) : TypeExpr(TypeKind::function, s), result(std::move(r)) {}
};

// `(A, B, ...)`: a tuple type (two or more elements).
struct TupleType : TypeExpr {
    std::vector<TypePtr> elements;
    TupleType(Span s) : TypeExpr(TypeKind::tuple, s) {}
};

// `auto` (variables) and `deduce` (template return types) carry no data.
struct PlaceholderType : TypeExpr {
    using TypeExpr::TypeExpr;
};

// --- expressions --------------------------------------------------------------------------

// `{expr:spec}` in an f-string (spec 19). Everything is optional except the expression.
struct FormatSpec {
    char align     = 0;    // 0, '<', '>' or '^'
    bool plus      = false;
    int  width     = -1;
    int  precision = -1;
    char radix     = 0;    // 0, 'x', 'X', 'b' or 'o'
    Span span;             // the spec text after ':' (empty span when there is none)
    bool present   = false;
};

// One piece of an f-string: literal text, or a hole with an expression.
struct FormatPart {
    std::string text;   // literal text (escapes decoded); empty for holes
    ExprPtr     expr;   // the hole's expression; null for literal text
    FormatSpec  spec;
    Span        span;
};

struct LiteralExpr : Expr {
    TokenKind   token_kind;   // which literal token produced it
    std::string text;         // source text
    std::string value;        // decoded string/char contents
    std::vector<FormatPart> format_parts;   // f-strings only: text and holes in order
    u128        int_value = 0;
    std::string suffix;
    bool        bool_value = false;
    LiteralExpr(ExprKind k, Span s, TokenKind tk) : Expr(k, s), token_kind(tk) {}
};

// `{ stmts; tail }`. The tail is the last expression without `;` (the block's value).
struct BlockExpr : Expr {
    bool                 is_unsafe = false;
    std::vector<StmtPtr> statements;
    ExprPtr              tail;
    BlockExpr(Span s) : Expr(ExprKind::block, s) {}
};

// `this`, `default`, `break`, `continue`, error placeholders.
struct SimpleExpr : Expr {
    using Expr::Expr;
};

struct PathExpr : Expr {
    Path path;
    SymbolRef symbol = nullptr;   // what the path names (local, function, global, alternative, ...)
    PathExpr(Span s, Path p) : Expr(ExprKind::path, s), path(std::move(p)) {}
};

enum class UnaryOp {
    negate,        // -x
    logical_not,   // !x
    bit_not,       // ~x
    deref,         // *p
    address_of,    // &x
    address_of_mut,// mut &x
    move,          // move x
    mut_marker,    // mut x   (call-site / match marker: f(mut v), match (mut v))
    spread,        // values...  (a parameter pack passed on whole, templates "Parameter packs")
};

struct UnaryExpr : Expr {
    UnaryOp   op;
    ExprPtr   operand;
    SymbolRef op_symbol = nullptr;   // a user-defined `-` or `~` (spec 16), set by the checker
    // Arithmetic of a distinct type (spec 34): the operand is cast to the underlying type, and
    // the result is wrapped back into this type.
    const types::Type* distinct_result = nullptr;
    UnaryExpr(Span s, UnaryOp o, ExprPtr e) : Expr(ExprKind::unary, s), op(o), operand(std::move(e)) {}
};

struct BinaryExpr : Expr {
    TokenKind op;   // the operator token: plus, amp_amp, eq_eq, ...
    ExprPtr   lhs;
    ExprPtr   rhs;
    SymbolRef op_symbol = nullptr;   // a user-defined operator (spec 16), set by the checker
    // Arithmetic of a distinct type (spec 34): the operands are casts to the underlying type,
    // the operation is the underlying type's, and the result is wrapped back into this type.
    const types::Type* distinct_result = nullptr;
    BinaryExpr(Span s, TokenKind o, ExprPtr l, ExprPtr r)
        : Expr(ExprKind::binary, s), op(o), lhs(std::move(l)), rhs(std::move(r)) {}
};

// `a..b` and `a..=b`.
struct RangeExpr : Expr {
    ExprPtr lo;
    ExprPtr hi;
    bool    inclusive = false;
    RangeExpr(Span s, ExprPtr l, ExprPtr h, bool inc)
        : Expr(ExprKind::range, s), lo(std::move(l)), hi(std::move(h)), inclusive(inc) {}
};

struct CallExpr : Expr {
    ExprPtr              callee;
    std::vector<ExprPtr> args;
    // `std::meta::wrapped_body()` in a function an annotation wraps (annotations, section 8): the
    // function's own body. It runs like a lambda's body; its `return` gives the call its value.
    std::unique_ptr<BlockExpr> wrapped;
    // `@std::dispatch_hint<T> call` (performance §8): the class the call is expected to reach.
    TypePtr   dispatch_hint;
    SymbolRef hint_symbol = nullptr;   // that class (the checker)
    CallExpr(Span s, ExprPtr c) : Expr(ExprKind::call, s), callee(std::move(c)) {}
};

struct IndexExpr : Expr {
    ExprPtr object;
    ExprPtr index;
    SymbolRef op_symbol = nullptr;   // `operator[]` of a user type (spec 16), set by the checker
    IndexExpr(Span s, ExprPtr o, ExprPtr i) : Expr(ExprKind::index, s), object(std::move(o)), index(std::move(i)) {}
};

// `obj.name`, `obj.name<T>`, `this.shape::describe`, `ptr->field`.
struct MemberExpr : Expr {
    ExprPtr object;
    Path    name;
    bool    arrow = false;
    SymbolRef symbol = nullptr;   // only resolved for `this.member`
    SymbolRef deref_symbol = nullptr;   // `p->x` on a type with `operator*` (a smart pointer, spec 13, 16)
    MemberExpr(Span s, ExprPtr o, Path n, bool a)
        : Expr(ExprKind::member, s), object(std::move(o)), name(std::move(n)), arrow(a) {}
};

struct TryExpr : Expr {
    ExprPtr operand;
    TryExpr(Span s, ExprPtr e) : Expr(ExprKind::try_, s), operand(std::move(e)) {}
};

// `cast<T>(x)`, `cast::truncate<T>(x)`, ...; `variant` is empty for the plain lossless cast.
struct CastExpr : Expr {
    std::string variant;
    TypePtr     target;
    ExprPtr     operand;
    CastExpr(Span s) : Expr(ExprKind::cast, s) {}
};

// `bit(n)` or `bit<T>(n)`.
struct BitExpr : Expr {
    TypePtr type;   // may be null
    ExprPtr operand;
    BitExpr(Span s) : Expr(ExprKind::bit, s) {}
};

struct FieldInit {
    std::string name;
    Span        span;
    ExprPtr     value;
};

// `(a, b, ...)`: a tuple value.
struct TupleExpr : Expr {
    std::vector<ExprPtr> elements;
    TupleExpr(Span s) : Expr(ExprKind::tuple, s) {}
};

// `[a, b, c]`: a list literal; its type (std::array<T, N> or std::span<T>) comes from context.
struct ListExpr : Expr {
    std::vector<ExprPtr> elements;
    ListExpr(Span s) : Expr(ExprKind::list, s) {}
};

// `T{ .field = value, ... }`.
struct StructLiteralExpr : Expr {
    TypePtr                type;
    std::vector<FieldInit> fields;
    StructLiteralExpr(Span s, TypePtr t) : Expr(ExprKind::struct_literal, s), type(std::move(t)) {}
};

// `@range{ .min = 0, .max = 100 }` or `@json::skip` before a declaration, field, parameter or
// enum alternative (spec 29).
struct AnnotationUse {
    Path      path;                     // the annotation, as written
    Span      span;                     // from '@' to the end
    ExprPtr   value;                    // `A{ ... }` (without braces: `A{}`, every field takes its default)
    SymbolRef symbol       = nullptr;   // the annotation struct (name resolution)
    SymbolRef value_symbol = nullptr;   // a compile_time constant holding the value (name resolution)
};

enum class CaptureKind { copy, move, ref, mut_ref, init };

struct Capture {
    CaptureKind kind;
    std::string name;
    Span        span;
    ExprPtr     init;   // CaptureKind::init only
    bool        is_mut = false;     // `[mut x]`, `[mut move x]`, `[mut x = e]`: the lambda may change its own copy (spec 34)
    SymbolRef   symbol = nullptr;   // the local the capture introduces inside the lambda
    SymbolRef   captured = nullptr; // the enclosing function's local it captures (not for init-captures)
};

struct Param {
    bool        is_mut = false;   // `mut T x` (by-value mutable); `mut T&` sets ReferenceType::is_mut
    // `implicit T x` (spec 31): a call may leave the argument out; the compiler supplies
    // `T::from_call(<call site>)`.
    bool        is_implicit = false;
    // `compile_time T x` (spec 31): the argument is a compile-time value and a constant in the
    // body. The parser adds a hidden template value parameter `$x` for it, so the function is
    // instantiated per distinct value.
    bool        is_compile_time = false;
    // `A&... values`: takes the arguments matching the template's pack `A`.
    bool        is_pack = false;
    TypePtr     type;
    std::string name;
    Span        span;
    SymbolRef   symbol = nullptr;
    std::vector<AnnotationUse> annotations;   // `@name{...}` before the type (spec 29)
};

struct LambdaExpr : Expr {
    std::vector<Capture> captures;
    std::vector<Param>   params;
    TypePtr              result;   // may be null (deduced)
    BlockPtr             body;
    LambdaExpr(Span s) : Expr(ExprKind::lambda, s) {}
};

struct IfExpr : Expr {
    bool     is_compile_time = false;
    bool     from_call_else  = false;  // `f(...) else { ... }`: condition is `!f(...)` (spec 10)
    int      taken = -1;              // `if compile_time`: 0 = then, 1 = else, 2 = neither (set by the checker)
    ExprPtr  condition;
    BlockPtr then_block;
    ExprPtr  else_branch;   // BlockExpr or IfExpr, may be null
    IfExpr(Span s) : Expr(ExprKind::if_, s) {}
};

struct MatchArm {
    PatternPtr pattern;
    ExprPtr    guard;   // may be null
    ExprPtr    body;
    Span       span;
};

struct MatchExpr : Expr {
    ExprPtr               scrutinee;
    std::vector<MatchArm> arms;
    MatchExpr(Span s) : Expr(ExprKind::match, s) {}
};

struct ReturnExpr : Expr {
    ExprPtr value;   // may be null
    ReturnExpr(Span s, ExprPtr v) : Expr(ExprKind::return_, s), value(std::move(v)) {}
};

// --- patterns -----------------------------------------------------------------------------

struct LiteralPattern : Pattern {
    ExprPtr literal;   // LiteralExpr, or unary negate of one
    LiteralPattern(Span s, ExprPtr l) : Pattern(PatternKind::literal, s), literal(std::move(l)) {}
};

struct RangePattern : Pattern {
    ExprPtr lo;
    ExprPtr hi;
    bool    inclusive = false;
    RangePattern(Span s) : Pattern(PatternKind::range, s) {}
};

struct BindingPattern : Pattern {
    std::string name;
    SymbolRef   symbol = nullptr;
    BindingPattern(Span s, std::string n) : Pattern(PatternKind::binding, s), name(std::move(n)) {}
};

// `token::plus`, `token::number(n)`, `std::ok(e)`.
struct EnumPattern : Pattern {
    Path       path;
    PatternPtr payload;   // may be null
    SymbolRef  symbol = nullptr;   // the alternative
    EnumPattern(Span s, Path p) : Pattern(PatternKind::enum_alternative, s), path(std::move(p)) {}
};

struct FieldPattern {
    std::string name;
    Span        span;
    PatternPtr  pattern;
};

// `employee{ .age = a, .. }`.
struct StructPattern : Pattern {
    Path                      path;
    std::vector<FieldPattern> fields;
    bool                      has_rest = false;
    StructPattern(Span s, Path p) : Pattern(PatternKind::struct_, s), path(std::move(p)) {}
};

// `(p1, p2, ...)`: matches a tuple element by element.
struct TuplePattern : Pattern {
    std::vector<PatternPtr> elements;
    TuplePattern(Span s) : Pattern(PatternKind::tuple, s) {}
};

struct SimplePattern : Pattern {   // wildcard, error
    using Pattern::Pattern;
};

// --- statements ---------------------------------------------------------------------------

enum class Storage { local, compile_time, static_, thread_local_ };

// `[mut] T name = init;`, `compile_time T name = init;`
struct VariableStmt : Stmt {
    Storage     storage = Storage::local;
    bool        is_mut  = false;
    TypePtr     type;
    std::string name;
    Span        name_span;
    ExprPtr     init;
    SymbolRef   symbol = nullptr;
    VariableStmt(Span s) : Stmt(StmtKind::variable, s) {}
};

// One name of a destructuring declaration: `mut u32 lo`, `auto` names, or `_`.
struct DestructureBinding {
    bool        is_mut   = false;
    bool        wildcard = false;   // `_`
    TypePtr     type;               // null with `auto (a, b)` and for `_`
    std::string name;
    Span        span;
    SymbolRef   symbol = nullptr;
};

// `(T1 a, T2 b) = expr;` or `auto (a, b) = expr;`: consumes a tuple into new variables.
struct DestructureStmt : Stmt {
    bool                            is_auto = false;
    std::vector<DestructureBinding> bindings;
    ExprPtr                         init;
    DestructureStmt(Span s) : Stmt(StmtKind::destructure, s) {}
};

// `target op value;` where op is `=`, `+=`, `<<=`, `+%=`, ...
struct AssignStmt : Stmt {
    TokenKind op;
    ExprPtr   target;
    ExprPtr   value;
    SymbolRef op_symbol = nullptr;   // `a += b` with a user-defined `operator+=` or `operator+`
    // `d += x` on a distinct type (spec 34): the target is changed as its underlying type.
    const types::Type* distinct_underlying = nullptr;
    AssignStmt(Span s, TokenKind o, ExprPtr t, ExprPtr v)
        : Stmt(StmtKind::assign, s), op(o), target(std::move(t)), value(std::move(v)) {}
};

// `_ = expr;`
struct DiscardStmt : Stmt {
    ExprPtr value;
    DiscardStmt(Span s, ExprPtr v) : Stmt(StmtKind::discard, s), value(std::move(v)) {}
};

struct ExprStmt : Stmt {
    ExprPtr expr;
    ExprStmt(Span s, ExprPtr e) : Stmt(StmtKind::expression, s), expr(std::move(e)) {}
};

// A check (spec 23): `compile_time check (c) else msg;`, `run_time(debug) check (c) else msg;`,
// `compile_time run_time check (...)`, or the block form `compile_time check { ... }`.
struct Check {
    bool                     is_compile_time = false;
    bool                     is_run_time     = false;
    std::vector<std::string> compile_modes;   // `compile_time(release)`; empty = every build mode
    std::vector<std::string> run_modes;       // `run_time(debug)`
    Span                     compile_modes_span;
    Span                     run_modes_span;
    ExprPtr                  condition;       // condition form
    ExprPtr                  message;
    BlockPtr                 block;           // block form
    // Set by the checker:
    bool                     is_entry = false;   // at the entry of a function body
    bool                     compile_active = true;   // the compile_time part is in this build mode
    bool                     run_active     = true;   // the run_time part is in this build mode
};

struct CheckStmt : Stmt {
    Check check;
    CheckStmt(Span s) : Stmt(StmtKind::check, s) {}
};

// --- loops (expressions of type void, always used as statements) -------------------------

struct WhileExpr : Expr {
    ExprPtr  condition;
    BlockPtr body;
    WhileExpr(Span s) : Expr(ExprKind::while_, s) {}
};

// `for ([mut] T name : iterable) { }`
struct ForExpr : Expr {
    bool        is_compile_time = false;   // `for compile_time`: the body is copied per element (spec 26)
    std::vector<BlockPtr>  copies;          // the checked copies of the body, in order (set by the checker)
    std::vector<SymbolRef> copy_symbols;    // each copy's loop variable
    bool        unrolled = false;           // the checker made `copies` (else the body was checked as is)
    bool        is_mut = false;
    TypePtr     type;
    std::string name;
    Span        name_span;
    ExprPtr     iterable;
    BlockPtr    body;
    SymbolRef   symbol = nullptr;   // the loop variable
    // Set by the checker for loops over a user type (spec 10): a container (`len()` and
    // `operator[]`) or a stream (`next()`, which returns a `std::option`).
    SymbolRef   len_symbol   = nullptr;
    SymbolRef   index_symbol = nullptr;
    SymbolRef   next_symbol  = nullptr;
    ForExpr(Span s) : Expr(ExprKind::for_, s) {}
};

// --- declarations -------------------------------------------------------------------------

enum class DeclKind { import, function, variable, field, struct_, class_, interface, enum_, check };

enum class Access { none, public_, protected_, private_ };

struct TemplateParam {
    bool                 is_type = true;   // `type T` vs `usize N`
    bool                 is_pack = false;  // `type... A`: any number of types
    std::string          name;
    Span                 span;
    std::vector<TypePtr> constraints;      // `type T : a & b`
    TypePtr              value_type;       // `usize N`
    TypePtr              default_type;     // `type A = std::global_allocator`
    ExprPtr              default_value;    // `usize N = 16`
};

struct Decl : Node<DeclKind> {
    std::string                name;
    Span                       name_span;
    std::string                doc;          // from `//` comments
    std::string                ns;           // the enclosing `namespace a::b { }` of a file-scope declaration ("" = global, spec 30)
    bool                       is_private = false;   // file-scope `private`
    // A type declared inside a struct or class (`thread_pool::options`): the outer type's name.
    // The parser moves it to file scope with `ns` = the outer type's namespace + its name.
    std::string                nested_in;
    Access                     access     = Access::none;   // class member section
    bool                       is_template = false;
    std::vector<TemplateParam> template_params;
    std::vector<AnnotationUse> annotations;   // `@name{...}` written before it (spec 29)
    // Set on members an annotation's `generate` block added (spec 29): the annotation use.
    const AnnotationUse*       generated_by = nullptr;
    // The parameters' symbols from the latest resolution of this declaration (name resolution).
    mutable std::vector<SymbolRef> template_param_symbols;
    using Node::Node;
};

struct ImportDecl : Decl {
    Path path;
    ImportDecl(Span s) : Decl(DeclKind::import, s) {}
};

struct FunctionDecl : Decl {
    TypePtr            result;          // null for destructors
    std::vector<Param> params;
    bool               is_static       = false;
    bool               is_virtual      = false;
    bool               is_override     = false;
    bool               is_abstract     = false;
    bool               is_mut          = false;   // trailing `mut`
    bool               is_move         = false;   // trailing `move`: consumes `this` (spec 06, 27)
    bool               is_compile_time = false;
    bool               is_run_time     = false;
    bool               is_unsafe       = false;
    bool               is_extern       = false;
    std::string        abi;                       // `extern(c)`: "c" or "cpp" (spec 13)
    bool               is_destructor   = false;
    bool               is_operator     = false;   // name is the operator spelling, e.g. "=="
    bool               is_defaulted    = false;   // `= default;`
    BlockPtr           body;                      // null for declarations
    // `deduce` (spec 15): the return type found in the body, once checked.
    mutable const types::Type* deduced_result = nullptr;
    FunctionDecl(Span s) : Decl(DeclKind::function, s) {}
};

// File-scope variable (`static` / `compile_time`) or a struct/class field.
struct VariableDecl : Decl {
    Storage storage = Storage::local;
    bool    is_mut  = false;
    TypePtr type;
    ExprPtr init;   // fields: default value, may be null
    VariableDecl(DeclKind k, Span s) : Decl(k, s) {}
};

// `unsafe std::sendable(condition)` in a base list (spec 27): a thread-safety promise the
// compiler can't check.
struct ThreadPromise {
    bool                shareable = false;   // std::shareable (else std::sendable)
    Path                path;                // as written
    ExprPtr             condition;           // may be null: unconditional
    Span                span;
    std::optional<bool> value;               // the condition's value, once evaluated (per instance)
};

// struct, class and interface share one node.
// `generate : interfaces { members }` in an annotation (spec 29).
struct GenerateBlock {
    Span                 span;
    std::vector<TypePtr> bases;
    std::vector<DeclPtr> members;
};

// `for compile_time (std::meta::field f : std::meta::fields<T>()) { std::darray<f.type> f.name = default; }`
// in a struct or class template (spec 26): fields made from the fields of a template argument when
// the template is instantiated, one per field of `T` for each field written in the block. In
// `fields`, the type `f.type` is the NamedType `$field.type`, and the name `f.name` is "".
struct FieldGenerator {
    Span                 span;
    std::string          var;            // `f`
    TypePtr              source;         // `T` of `std::meta::fields<T>()`
    std::size_t          position = 0;   // the number of members written before it
    std::vector<DeclPtr> fields;         // VariableDecl (DeclKind::field)
};

struct TypeDecl : Decl {
    bool                 is_open = false;
    bool                 is_view = false;
    // `distinct type meters = f32;` (spec 34): a struct with one hidden field `$value` of the
    // underlying type; only `cast` converts between the two.
    bool                 is_distinct = false;
    bool                 is_union    = false;   // `union u { u32 a; f32 b; }` (spec 34): fields share memory
    // `implement I for T { ... }` (spec 34): the type T; `bases` holds I. Not a type of its own:
    // name resolution adds I and the methods to T.
    TypePtr              implement_target;
    // `annotation(std::meta::target::field, ...) struct` (spec 29): the targets as written.
    bool                 is_annotation = false;
    std::vector<Path>    annotation_targets;
    std::unique_ptr<GenerateBlock> generate;
    std::unique_ptr<BlockExpr>     wrap;        // `wrap { ... }` in a function annotation (section 8)
    std::vector<TypePtr> bases;
    std::vector<ThreadPromise> promises;     // `unsafe std::sendable(...)` bases
    std::vector<DeclPtr> members;
    std::vector<FieldGenerator> field_generators;   // spec 26; expanded in each instance
    TypeDecl(DeclKind k, Span s) : Decl(k, s) {}
};

struct EnumAlternative {
    std::string name;
    Span        span;
    TypePtr     payload;   // may be null
    ExprPtr     value;     // explicit tag, may be null
    std::vector<AnnotationUse> annotations;   // spec 29
};

struct EnumDecl : Decl {
    TypePtr                      tag_type;
    std::vector<EnumAlternative> alternatives;
    EnumDecl(Span s) : Decl(DeclKind::enum_, s) {}
};

// A check at file scope or in a type body (spec 23).
struct CheckDecl : Decl {
    Check check;
    CheckDecl(Span s) : Decl(DeclKind::check, s) {}
};

// One parsed source file.
// `namespace vk = gfx::vulkan;` (spec 30): a file-local short name for a namespace.
struct NamespaceAlias {
    std::string name;
    Span        span;     // the alias name
    Path        target;
    std::string ns;       // the namespace block it is written in
};

// One `namespace a::b {` opening (spec 30), for ownership and name-conflict checks.
struct NamespaceBlock {
    std::string path;     // "a::b"
    Span        span;     // the written path
};

struct File {
    FileId                      file = 0;
    std::vector<DeclPtr>        decls;
    std::vector<NamespaceAlias> aliases;
    std::vector<NamespaceBlock> namespaces;
    // `@a{ ... };` after the imports: annotations of the file itself (spec 31).
    std::vector<AnnotationUse>  annotations;
};

} // namespace edax::ast
