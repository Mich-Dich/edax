# Variables

## Rules

### Declaration
```
[mut] <type> <name> = <expression>;
```
- **`=` is the only way to initialize.** `T x{...};`, `T x(...);` and `T x;` are compile errors.
- **Immutable by default.** You can only assign to the variable, call `mut` methods on it or pass it as `mut` if it is declared `mut`.
- **Mutability belongs to the variable, not to fields.** If the variable is `mut`, all its fields can be changed; if not, none can. Fields have no `mut` marker.
- **No shadowing.** You can't declare a name that is already visible in an enclosing scope of the same function. In a method the members are visible too: a local or parameter can't take a member's name (see [Classes](../user-types/classes.md)).

```edax
u32 x = 5;                          // immutable
mut u32 y = 5;                      // mutable
y = 6;                              // OK
x = 6;                              // ERROR: x is immutable

test t = test{ .x = 5 };            // OK
test t2{ .x = 5 };                  // ERROR: initialize with '='
u32 z;                              // ERROR: no uninitialized values
```

### Building values: `{}` for types, `()` for functions
- A type is instantiated with `{}`: `test{}`, `test{ .x = 5 }`.
- A function is called with `()`: `my_func(42)`, `n.abs()`, `std::string::from("x")`.
- `test()` and `my_func{42}` are compile errors. This removes C++'s "most vexing parse" by construction.
- Enum alternatives with a payload, such as `std::ok(x)`, are value constructors and use `()` (see [Enums](../user-types/enums.md)).

### `auto`
`auto` is allowed only when the initializer has a **fully known type on its own**:

```edax
auto a = 5u32;                       // OK
auto b = parse_line(line);           // OK: the function's return type
auto c = [](u32 x) { return x; };    // OK: lambda types can't be named
auto d = 5;                          // ERROR: literal without context
auto e = bit(4);                     // ERROR: bit(n) without context
auto f = std::none;                  // ERROR: option of what?
```
`auto` is never allowed in function signatures. Use templates (see [Templates](../compile-time/templates.md)) instead.

### `default`
`default` produces a type's default value:

- Integers → `0`, floats → `0.0`, `bool` → `false`, `char` → `'\0'`, `std::raw_ptr` → `nullptr`.
- Structs and classes → each field's declared default. This is allowed only if **every** field has a default, either written in the type or coming from the field's own type.
- Library types define their own default: an empty `std::string`, an empty `std::darray`, `std::none` for options.
- Types with no sensible default (`std::file`, references, `std::unique_ptr`) can't be `default`ed.

```edax
u32 n = default;                     // 0
std::darray<u32> v = default;        // empty
std::file f = default;               // ERROR: std::file has no default
```

### File-scope variables
| Declaration | Meaning |
|---|---|
| `compile_time T x = v;` | Constant with no storage. May be declared in a `.edh` |
| `static T x = v;` | Storage for the program's lifetime. `v` must be compile-time evaluable |
| `static mut T x = v;` | Mutable global: every access needs `unsafe` |
| `thread_local [mut] T x = v;` | One copy per thread; `mut` needs no `unsafe` (see [Threads](../../stdlib/concurrency/threads.md)) |
| `private` + any of the above | Visible only in this module (see [Headers, Sources and Modules](../organization/modules.md)) |
| `T x = v;` | **ERROR:** say `static` or `compile_time` |

- Static initializers are evaluated at compile time, so there is no initialization-order problem at startup.
- **`static` is visible to every thread**, so its type must be `std::shareable` (see [Threads](../../stdlib/concurrency/threads.md)). Shared mutable globals are `static` synchronized types (`std::atomic<T>`, `std::mutex<T>`), which change through immutable references.
- **`static mut` is always unsafe:** every read and write must be inside `unsafe { }`, whatever the type.
- `private` and `static` are orthogonal: `private static u32 x = 0;` is module-local *and* has program lifetime.

```edax
compile_time usize buffer_size = 4096;
private static std::atomic<u32> request_count = default;       // no mut needed: atomics synchronize
static mut u32 raw_counter = 0;          // allowed, but every access needs unsafe
```

## Rationale
- **Only `=`:** C++ has `T x = v`, `T x(v)`, `T x{v}` and `T x = {v}`, with different rules for each. One form is enough.
- **Mutability on the variable:** a field-level `mut` would need extra rules (can a `mut` method change a non-`mut` field? can you reassign the whole struct?). One rule covers every case: if the variable is `mut`, everything inside it can be changed.
- **No shadowing:** in a language without a lifetime checker, two `x` in one function are a common source of misreading.
- **`auto` only with a known type:** this follows directly from principle 1 and is the same rule that bans `auto x = bit(4)`.
- **Compile-time static initializers:** C++'s "static initialization order fiasco" can't happen.
