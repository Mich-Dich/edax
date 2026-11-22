#pragma once
// Types (M4). Every Type is interned in a TypeContext, so two types are equal exactly when
// their pointers are equal.
//
// Besides the spec's types there are a few checker-internal kinds:
//   error         an expression whose type could not be determined; it never causes further
//                 errors (keeps one mistake from producing a cascade)
//   untyped_int   an unsuffixed integer literal (or arithmetic on only such literals) that has
//   untyped_float not received its type from context yet (spec 02)
//   external      a standard-library type that library/std does not declare yet: `std::string`.
//                 Compared by spelling; conversions between external types are not checked.
//                 (`std::option`, `std::result` and `std::exit_code` are real enums of module std.)
//   type_param    `T` inside a template; checked only at instantiation, so treated like error
//   type_value    `type`: the type of types used as compile-time values (spec 26)
//   meta          `std::meta::field` / `alternative` / `function` / `parameter`: compile-time-only reflection data
//   pending       `std::none`, `std::ok(x)`: an alternative of a template enum whose template
//                 arguments are not all known yet. Like a literal it takes them from context
//                 (spec 08); `args` holds the known ones and null for the others.

#include "edax/symbols.hpp"
#include "edax/token.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace edax::types {

enum class Kind {
    error, untyped_int, untyped_float, untyped_default,   // untyped_default: `default` without context
    untyped_list,   // a list literal `[a, b]`, or a tuple `(1, 2)` with untyped elements: typed by context
    int_, float_, bool_, char_, void_, never, nullptr_, static_str,
    user,        // struct / class / enum / interface (non-template)
    reference,   // T& / mut T&
    pointer,     // std::raw_ptr<T> / std::raw_ptr<mut T>
    range,       // a..b
    external,    // std:: types
    type_param,
    pending,     // template enum with arguments still to come from context (symbol = template)
    type_value,  // `type` (spec 26)
    value_arg,   // a template value argument: `4` in `box<4>` (inner = its type; the value: value_bits)
    closure,     // the unnamed type of one lambda (spec 18); args = parameter types, then the result
    function,    // std::function<R(A...)>: a callable on the heap; args = parameters, then the result
    fn_ptr,      // std::fn_ptr<R(A...)>: a plain function pointer; args as for function
    array,       // std::raw::array<T, N>: N values of T inline (inner = T, args[0] = the value_arg N)
    meta,        // std::meta::field, std::meta::alternative (name says which)
    tuple,       // (A, B, ...): args = the element types, two or more
    vector,      // std::simd::vec<T, N> (f32x8, ...): inner = the lane type, bits = the lane count
    mask,        // std::simd::mask<N>: bits = the lane count
};

struct Type {
    Kind                kind      = Kind::error;
    int                 bits      = 0;       // int_ / float_
    bool                is_signed = false;   // int_
    bool                is_size   = false;   // isize / usize
    const sema::Symbol* symbol    = nullptr; // user / type_param
    const Type*         inner     = nullptr; // reference / pointer / range
    bool                is_mut    = false;   // reference / pointer
    std::string         name;                // spelling (int names, external spelling)
    std::vector<const Type*> args;           // pending: known template arguments, null = unknown

    [[nodiscard]] bool is(Kind k) const { return kind == k; }
    [[nodiscard]] bool is_integer() const { return kind == Kind::int_; }
    [[nodiscard]] bool is_float() const { return kind == Kind::float_; }
    [[nodiscard]] bool is_numeric() const { return kind == Kind::int_ || kind == Kind::float_; }
    [[nodiscard]] bool is_untyped() const {
        return kind == Kind::untyped_int || kind == Kind::untyped_float || kind == Kind::untyped_default ||
               kind == Kind::pending || kind == Kind::untyped_list;
    }
    // Types whose values exist only at compile time (spec 26).
    [[nodiscard]] bool is_compile_time_only() const { return kind == Kind::type_value || kind == Kind::meta; }
    // A C function of a loaded library (`std::c_library::function<R(A...)>`): a function pointer
    // that is a view of its library and is called in `unsafe`.
    [[nodiscard]] bool is_c_function() const { return kind == Kind::fn_ptr && is_size; }
    // Types the checker cannot reason about: anything goes, nothing is reported.
    [[nodiscard]] bool is_unknown() const { return kind == Kind::error || kind == Kind::type_param; }
    [[nodiscard]] bool is_user(sema::SymbolKind k) const;
    // `std::result<...>` and friends: the external spelling up to the first '<'.
    [[nodiscard]] std::string_view external_base() const;
};

// Bytes of a type that never hold one value in a live value (spec 34, "Niches"): an `option` of
// the type stores `none` as that value there and needs no tag. Zero for references and
// `@std::never_zero` fields, 2 for `bool`, 0x110000 for `char`, an unused tag for enums.
struct Niche {
    std::uint64_t offset = 0;
    std::uint64_t size   = 0;   // 1, 2, 4 or 8
    std::uint64_t value  = 0;   // the bits that mean "the other alternative"
};

// A two-alternative enum (`std::option<T>`, ...) stored without a tag: `with` carries a payload
// that has a niche, `without` carries nothing and is stored as the niche's value.
struct NicheEnum {
    const sema::Symbol* with    = nullptr;
    const sema::Symbol* without = nullptr;
    Niche               niche;
};

// Size and alignment in bytes, following the backend's layout (the interop rules of 13).
struct Layout {
    std::uint64_t size  = 0;
    std::uint64_t align = 1;
    bool          empty = false;   // a struct without data: size 0, but known (std::global_allocator)
};

// Evaluates a template value argument (`N + 1`) for the template parameter `param` of `templ`,
// with `subst` for the parameters in scope. Set by the checker; null when it cannot be done.
using ValueArgResolver = std::function<const Type*(const sema::Symbol* templ, std::size_t index, const ast::Expr& value,
                                                   const std::map<const sema::Symbol*, const Type*>* subst)>;

// The type a compile_time `type` variable or parameter holds (spec 26). Set by the checker.
using TypeValueResolver = std::function<const Type*(const sema::Symbol* symbol)>;

// Makes the instance of a template type for concrete arguments (spec 14). Set by the checker.
using TypeInstantiator = std::function<const Type*(const sema::Symbol* templ, const std::vector<const Type*>& args)>;

class TypeContext {
public:
    TypeContext();

    // Without an instantiator, template types lower to `error` (nothing is instantiated).
    void set_instantiator(TypeInstantiator instantiator) { instantiator_ = std::move(instantiator); }
    void set_type_value_resolver(TypeValueResolver resolver) { type_values_ = std::move(resolver); }
    void set_value_arg_resolver(ValueArgResolver resolver) { value_args_ = std::move(resolver); }

    // The interned template value argument `bits` of type `of` (an integer type or bool).
    const Type* value_arg(u128 bits, const Type* of);
    // The type of the lambda `l` (one per lambda). Its signature is set once the lambda is checked.
    const Type* closure(const ast::LambdaExpr* l);
    void        set_closure_signature(const Type* closure, std::vector<const Type*> params, const Type* result);
    const ast::LambdaExpr* lambda_of(const Type* closure) const;
    // `std::function<R(A...)>` (kind function) or `std::fn_ptr<R(A...)>` (kind fn_ptr), interned.
    // `std::function<R(A...), I...>` / `std::inline_function<R(A...), N, I...>` / `std::fn_ptr`.
    // A function type keeps `inline_bytes` in `bits` (0: the callable is on the heap), the bound
    // `std::sendable` in `is_signed` and `std::shareable` in `is_mut` (spec 34).
    const Type* callable(Kind kind, std::vector<const Type*> params, const Type* result, unsigned inline_bytes = 0,
                         bool sendable = false, bool shareable = false);
    // The size and alignment of a lambda's captures, as the backend lays them out.
    Layout closure_layout(const Type* closure);
    // `std::c_library::function<R(A...)>`, also a bare `R(A...)` written as a type.
    const Type* c_function(std::vector<const Type*> params, const Type* result);
    // `std::raw::array<T, N>` (the storage of std::array, spec 13 and 20), interned.
    const Type* array(const Type* element, const Type* count);
    // The tuple type `(A, B, ...)`, interned.
    const Type* tuple(std::vector<const Type*> elements);
    // `std::simd::vec<T, N>` and `std::simd::mask<N>` (SIMD), interned.
    const Type* vector(const Type* lane, int lanes);
    const Type* mask(int lanes);
    // A built-in type name, including the SIMD short names `f32x8`, `u8x32`, ...
    const Type* builtin_or_vector(std::string_view name);
    // The number of elements of an array type.
    std::uint64_t array_count(const Type* array) const;
    // Where the type being instantiated right now is written (for the type-body checks of 23).
    [[nodiscard]] std::optional<Span> instantiation_site() const { return site_; }
    u128        value_bits(const Type* value_arg) const;
    // A template value argument of any compile-time-capable type (a `compile_time` parameter's
    // argument, spec 31: enums, `std::static_str`, structs), interned by its source-like text.
    // `payload` is the checker's value (a ct::Value), kept opaque here.
    const Type* value_arg_of(const std::string& text, const Type* of, std::shared_ptr<const void> payload);
    // The payload of a value argument made by value_arg_of; null for integer and bool arguments.
    [[nodiscard]] std::shared_ptr<const void> value_payload(const Type* value_arg) const;

    const Type* error() const { return error_; }
    const Type* untyped_int() const { return untyped_int_; }
    const Type* untyped_float() const { return untyped_float_; }
    const Type* untyped_default() const { return untyped_default_; }
    const Type* untyped_list() const { return untyped_list_; }
    const Type* bool_() const { return bool_t_; }
    const Type* char_() const { return char_t_; }
    const Type* void_() const { return void_t_; }
    const Type* never() const { return never_; }
    const Type* nullptr_() const { return nullptr_t_; }
    const Type* static_str() const { return static_str_; }
    const Type* type_value() const { return type_value_; }
    const Type* meta_field() const { return meta_field_; }
    const Type* meta_alternative() const { return meta_alternative_; }
    const Type* meta_function() const { return meta_function_; }
    const Type* meta_file() const { return meta_file_; }
    const Type* meta_parameter() const { return meta_parameter_; }

    // Size and alignment of a sized type (64-bit host). Null-safe; unknown types give {0, 1}.
    Layout layout(const Type* t);
    // Byte offset of each non-static field of a struct/class type, in declaration order.
    std::vector<std::uint64_t> field_offsets(const Type* t);
    // A part of `t` with a value it never holds, if any (references, `@std::never_zero` fields,
    // `std::nonzero<T>`, `bool`, `char`, enum tags, and structs holding one).
    std::optional<Niche> niche(const Type* t);
    // Whether the enum `e` is stored without a tag (see NicheEnum).
    std::optional<NicheEnum> niche_enum(const sema::Symbol* e);

    // Built-in type by keyword spelling ("u32", "f64", "bool", ...), or null.
    const Type* builtin(std::string_view name) const;
    const Type* reference(const Type* inner, bool is_mut);
    const Type* pointer(const Type* inner, bool is_mut);
    const Type* range(const Type* element);
    const Type* user(const sema::Symbol* symbol);
    const Type* type_param(const sema::Symbol* symbol);
    const Type* external(const std::string& spelling);
    // The instance of the template type `templ` for `args` (spec 14). Error when the arguments
    // do not fit, are unknown, or no instantiator is set.
    const Type* instance(const sema::Symbol* templ, const std::vector<const Type*>& args);
    // A value of the template enum `templ` whose null arguments are not known yet.
    const Type* pending(const sema::Symbol* templ, const std::vector<const Type*>& args);

    // The type a written TypeExpr denotes (after name resolution). Template parameters found in
    // `subst` are replaced; template user types are not instantiated yet (error).
    const Type* lower(const ast::TypeExpr& t, const std::map<const sema::Symbol*, const Type*>* subst = nullptr);
    // Template arguments of an external type as written, e.g. `std::result<u32, e>` -> {u32, e}.
    const std::vector<const Type*>* external_args(const Type* t) const;
    void set_external_args(const Type* t, std::vector<const Type*> args) { external_args_[t] = std::move(args); }

private:
    std::map<const sema::Symbol*, std::optional<NicheEnum>> niche_cache_;   // niche_enum
    const Type* make(Type t);

    TypeInstantiator                                            instantiator_;
    TypeValueResolver                                           type_values_;
    ValueArgResolver                                            value_args_;
    std::map<std::pair<u128, const Type*>, const Type*>         value_args_made_;
    std::map<const Type*, u128>                                 value_of_;
    std::map<std::pair<std::string, const Type*>, const Type*>  rich_values_made_;
    std::map<const Type*, std::shared_ptr<const void>>          rich_value_of_;
    std::optional<Span>                                         site_;
    std::map<const ast::LambdaExpr*, const Type*>               closures_;
    std::map<std::tuple<Kind, unsigned, bool, bool, std::vector<const Type*>>, const Type*> callables_;
    std::map<std::pair<const Type*, const Type*>, const Type*>       arrays_;
    std::map<std::vector<const Type*>, const Type*>                 tuples_;
    std::map<std::pair<const Type*, int>, const Type*>               vectors_;
    std::map<int, const Type*>                                       masks_;
    std::map<const Type*, const ast::LambdaExpr*>               lambdas_;
    std::map<std::pair<const sema::Symbol*, std::vector<const Type*>>, const Type*> instances_;
    std::deque<Type>                                            storage_;
    std::map<std::string, const Type*>                          builtins_;
    std::map<std::tuple<int, const Type*, bool>, const Type*>   composites_;   // (kind, inner, mut)
    std::map<const sema::Symbol*, const Type*>                  users_;
    std::map<const sema::Symbol*, const Type*>                  params_;
    std::map<std::string, const Type*>                          externals_;
    std::map<const Type*, std::vector<const Type*>>             external_args_;
    std::map<std::pair<const sema::Symbol*, std::vector<const Type*>>, const Type*> pendings_;
    const Type* error_;
    const Type* untyped_int_;
    const Type* untyped_float_;
    const Type* untyped_default_;
    const Type* untyped_list_;
    const Type* bool_t_;
    const Type* char_t_;
    const Type* void_t_;
    const Type* never_;
    const Type* nullptr_t_;
    const Type* static_str_;
    const Type* type_value_;
    const Type* meta_field_;
    const Type* meta_alternative_;
    const Type* meta_function_;
    const Type* meta_file_;
    const Type* meta_parameter_;
};

// Source-like spelling: `u32`, `mut employee&`, `std::raw_ptr<u8>`, `{integer}`.
std::string to_string(const Type* type);
// Like to_string, with every user type's module: `geometry::point`, `std::darray<shapes::circle>`.
std::string qualified_name(const Type* type);
std::string type_name(const Type* type, bool qualified);

// Whether `t` (references removed) is an instance of the std template `name` ("option",
// "result"): `std::option<u32>`.
bool is_std_instance(const Type* t, std::string_view name);

// The built-in methods of std::option and std::result (spec 11), provided by the compiler.
bool is_std_builtin_method(const Type* receiver, std::string_view method);
// `is_some`/`is_none`/`is_ok`/`is_error`: they look at the value without taking it.
// `@std::layout{ .align = N }` on a struct or class (spec 34): N, or 0 without one.
std::uint64_t requested_alignment(const sema::Symbol* s);
// `@std::layout{ .packed = true }` on a struct (spec 34).
bool is_packed(const sema::Symbol* s);

// Whether `s` is a `union` (spec 34): its fields share memory at offset 0.
bool is_union(const sema::Symbol* s);

// `x.sqrt()`, `a.min(b)`, ...: a method of a number type (spec 34), and how many arguments it takes.
bool        is_number_method(const Type* receiver, std::string_view method);
std::size_t number_method_arity(std::string_view method);
bool is_std_query_method(std::string_view method);

// Removes any reference: `T&` -> `T`.
const Type* strip_ref(const Type* type);

// Whether an integer literal with this magnitude (negated if `negative`) fits in `type`.
bool literal_fits(u128 magnitude, bool negative, const Type* type);

// Methods of standard-library containers known to change their receiver (`push`, `clear`, ...).
// Until the library is written in Edax, this table stands in for their trailing `mut`.
bool is_mutating_library_method(const Type* receiver, std::string_view method);

// Smallest and largest value of an integer type, as text (for messages).
std::string integer_range(const Type* type);

} // namespace edax::types
