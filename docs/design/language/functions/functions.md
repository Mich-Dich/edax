# Functions

## Rules

### Declaration
```
[compile_time [run_time]] <return_type> <name>(<parameters>) { <body> }
```
```edax
std::result<employee, parse_error> parse_line(std::str line) { ... }
void log(std::str message) { std::println(message); }
```
- The return type is always written. `deduce` is only allowed in templates (see [Compile-Time Evaluation](../compile-time/compile-time.md)).
- No default arguments and no named arguments. Write an overload or pass a struct instead.
- A parameter may be marked **`compile_time`** (its argument is a compile-time value, below) or **`implicit`** (the compiler supplies it from the call's location, below).
- **Overloading** by parameter types is allowed. A call that matches more than one overload is an error (for example `f(5)` with both `f(u32)` and `f(u64)`; write `f(5u32)`). Overloading on return type alone is not allowed. Among templates the most specific overload wins (see [Templates](../compile-time/templates.md)).
- There are no nested functions; use lambdas (see [Lambdas](lambdas.md)).

### Parameter passing
| Parameter | Meaning | Call site |
|---|---|---|
| `T x` | By value. Copy types are copied; other types must be moved or cloned in | `f(v)`, `f(move s)`, `f(s.clone())` |
| `mut T x` | By value, mutable inside the function | Same as above |
| `T& x` | Immutable reference | `f(v)` |
| `mut T& x` | Mutable reference | **`f(mut v)`**, where `v` must be a `mut` variable |

```edax
void bump(mut u32& x) { x += 1; }

mut u32 counter = 0;
bump(mut counter);        // OK: you can see that counter may change
bump(counter);            // ERROR: mutable reference needs 'mut' at the call site
```
- Only the **receiver** of a method call (`obj.method()`) has no call-site marker. Whether the method is `mut` is checked against the variable (see [Classes](../user-types/classes.md)).
- **One mutable reference at a time:** `f(mut a, a)` and `f(mut a, mut a)` are errors (see [Ownership, Copies, Moves and References](../memory/ownership-and-references.md)).
- References may be returned only under the rules in 12.

### `compile_time` parameters
A parameter marked `compile_time` takes a **compile-time value** and is a compile-time constant in the body:
```edax
void write(compile_time std::log::level lvl, std::format_args message, implicit compile_time std::call_site where) {
    if compile_time (cast<u8>(lvl) >= cast<u8>(std::log::level_at(where))) {     // decided per call site
        ...
    }
}
```
- The argument must be a compile-time value; anything else is an error at the argument.
- In the body it can be used wherever a compile-time value can: `if compile_time`, template arguments, `compile_time` constants, calls of `compile_time` functions.
- The function is **instantiated once per distinct set of compile-time arguments**, like a template with value parameters (see [Templates](../compile-time/templates.md)). The parameter may have any compile-time-capable type: integers, `bool`, enums, `std::static_str`, structs of those such as `std::call_site`.
- The argument is still passed at run time; inside an instance every use of the parameter is the constant.
- `compile_time` is part of the signature and is written in the header. Lambdas have no `compile_time` parameters.

- **`compile_time` parameter or `compile_time check`?** An entry check (see [Checks](../errors/checks.md)) can also demand a compile-time argument, but it only *checks* the value and doesn't instantiate the function per value. Use a `compile_time` parameter when the body needs the value at compile time (to choose code or a type), and an entry check when only the check does.

### `implicit` parameters
The contextual keyword **`implicit`** marks a parameter whose argument the **compiler supplies at each call** when the caller leaves it out.
```edax
void info(std::format_args message, implicit std::call_site where);

std::log::info(f"ready");                 // where: supplied for this call
std::log::info(f"ready", where);          // where: written (forwarding)
```
- **Implicit parameters come last**, after every ordinary parameter. A call may leave out any number of them from the end. Writing one passes it like any argument.
- **The type decides the value.** The parameter's type must implement `std::implicit<T>`:
  ```edax
  namespace std {
      template<type T>
      interface implicit {
          compile_time static T from_call(std::call_site where);
      }
  }
  ```
  For each call that leaves the argument out, the compiler evaluates `T::from_call(<this call's site>)` at compile time and passes the result. A type without `compile_time static T from_call(std::call_site)` can't be left out (an error at the call).
- A supplied value depends **only on where the call is**. It is a compile-time value, so it works with `compile_time` parameters (above).
- `implicit` and `compile_time` can be combined, in either order: `implicit compile_time std::call_site where`.
- `implicit` is part of the signature and is written in the header. Lambdas have no implicit parameters (see [Lambdas](lambdas.md)).
- `implicit` is a keyword only before a parameter's type: `void f(implicit x)` is a parameter `x` of a type named `implicit`.

**Forwarding** keeps the original caller's location:
```edax
void warn_slow(std::time::duration d, implicit compile_time std::call_site where) {
    std::log::warn(f"took {d.as_milliseconds()} ms", where);     // reported at warn_slow's caller
}
```

**Another type:**
```edax
namespace profiler {
    struct zone : std::implicit<profiler::zone> {
        std::static_str name = "";
        u32             line = 0;

        compile_time static profiler::zone from_call(std::call_site where) override {
            return profiler::zone{ .name = where.function_name, .line = where.line };
        }
    }

    void begin(implicit profiler::zone z);
}

profiler::begin();          // a zone named after the calling function
```

Interfaces may declare **`compile_time static` methods** for this (see [Inheritance and Interfaces](../user-types/inheritance-and-interfaces.md)). They are implemented with `override` like any interface method, and such an interface is used as a base or a template constraint.

### `std::call_site`
`std::call_site` is **the place a call is written**. The compiler builds one for every call that leaves out an implicit argument; every field is static data it computed:
```edax
namespace std {
    struct call_site : std::implicit<std::call_site> {
        std::static_str file;             // as edaxc was given it: `renderer/vulkan/pipeline.ed`
        std::static_str file_name;        // `pipeline.ed`
        std::static_str function;         // `gfx::vulkan::create_pipeline`
        std::static_str function_name;    // `create_pipeline`
        std::static_str namespace_name;   // `gfx::vulkan`
        std::static_str module;           // `renderer::vulkan::pipeline`
        std::static_str target;           // the build target, or the main file's name without `.ed`
        u32             line;
        u32             column;
    }
}
```
- A copy type of static data: `std::sendable`, `std::shareable`, storable anywhere.
- Every compiler-made `std::call_site` is a compile-time value. Besides its fields it knows the function and file it describes, which compile-time code reads with (see [Metaprogramming](../compile-time/metaprogramming.md)):

| Call (`compile_time` only) | Result |
|---|---|
| `std::meta::function_annotation<A>(where)` | `std::option<A>`: annotation `A` on the function the call is in |
| `std::meta::type_annotation<A>(where)` | `std::option<A>`: on that function's type |
| `std::meta::file_annotation<A>(where)` | `std::option<A>`: on that file (see [Annotations](../compile-time/annotations.md)) |
| `setting.value_at(where)` | `std::option<T>`: what build.ed passed to that file, else to the target, for a `std::build::setting<T>` (see [Edax as a Build System](../../build/build-system.md)) |

### `main`
```edax
std::exit_code main() { ... }
std::exit_code main(std::span<std::str> args) { ... }
```
- Return type: `void`, `std::exit_code`, or any integer type. If an integer return value is outside the platform's exit-status range, the program panics instead of silently truncating it.
- `args[0]` is the executable path; `args[1..]` are the user's arguments. If an argument isn't valid UTF-8, the program stops at startup with an error message. Raw bytes are available through `std::env::raw_args()`, which returns copies (`std::darray<std::darray<u8>>`).

```edax
// examples/compile_check.ed - valid:
u32 main() {
    return 0;            // 0 becomes u32 from the return type; 0u32 also works
}
```

## Rationale
- **`implicit` belongs to the parameter, the value to the type:** the keyword says "the caller may leave this out", visible in the signature and the header. The type says what is supplied, so the mechanism isn't tied to one struct. Values can only depend on where the call is, so `implicit` can't hide run-time values the way default arguments would.
- **Implicit parameters last:** a call reads like the call without them, and forwarding is just writing the argument.
- **`compile_time` parameters:** a function can make compile-time decisions from an argument without becoming a template the caller has to spell out.
- **`mut` at the call site:** this is the most useful single marker for readers. `sort(mut list)` tells you at the call that `list` changes, without looking up the signature. Immutable references stay unmarked because they can't cause surprises.
- **No receiver marker:** writing `mut employees.push(x)` on every method call would be noise. The variable itself is already declared `mut`, which is the visible marker.
- **No default arguments:** they hide values at the call site, and they interact badly with overloading.
- **`std::span<std::str>` for `main`:** the runtime already holds the arguments in memory, so viewing them costs nothing. `std::darray<std::string>` would allocate before `main` even runs.
- **No parameter grouping:** C-style `T& {a, b}` shorthand would be a second way to write the same parameter list.
