# Build Modes

A **build mode** is a named set of settings that applies to a whole build: optimization, debug info, how arithmetic and checks behave, and which mode-gated checks (see [Checks](../language/errors/checks.md)) are compiled in. A mode is a class; the standard library defines `debug` and `release` (and `release_with_debug_info`, derived from `release`). Projects derive their own in `build.ed` (see [Edax as a Build System](build-system.md)).

## Rules

### Built-in modes
| Mode (`std::build::mode::...`) | Base class | Optimization | Debug info | Overflow, shifts, `bit(n)` | Bounds | `run_time` checks |
|---|---|---|---|---|---|---|
| `debug` | - | none | full | panic | panic | ungated + `debug` |
| `development` | `debug` | basic | full | panic | panic | ungated + `debug` |
| `release` | - | speed | none | panic | panic | ungated + `release` |
| `release_with_debug_info` | `release` | speed | full | panic | panic | ungated + `release` |

```
edaxc build                                 # debug (the default)
edaxc build --mode=development              # debug with light optimization: playable, still debuggable
edaxc build --mode=release
edaxc build --mode=profile                  # a mode defined in build.ed
```

**All built-in modes are safe.** The language rules from 16 (overflow panics, bounds checks, etc.) hold in each of them.

### Modes are classes
A mode is a **class** in `std::build::mode` that implements the interface `std::build::mode::settings`. The settings are its methods, so a mode changes a setting the way any derived class changes behaviour: it **overrides** the method.
```edax
// in the standard library (std::build::mode)
interface settings {
    compile_time std::build::optimization    optimization();
    compile_time std::build::debug_info      debug_info();
    compile_time std::build::overflow        overflow();
    compile_time std::build::shifts          shifts();
    compile_time std::build::bounds          bounds();
    compile_time std::build::run_time_checks run_time_checks();
    compile_time std::build::panic_behavior  panic();
    compile_time bool                        link_time_optimization();
    compile_time bool                        allow_undefined_behavior();
    compile_time std::build::float_determinism float_determinism();
}

open class debug : settings { ... }            // every method returns the debug value
open class release : settings { ... }
open class release_with_debug_info : release {
public:
    compile_time std::build::debug_info debug_info() override { return std::build::debug_info::full; }
}
```
- The standard library defines the two modes `debug` and `release`. `release_with_debug_info` is the first derived mode, and the library provides it too.
- A user mode **inherits** from one of them (or from another user mode) and overrides what it changes. Everything else comes from its base. Because the modes are `open`, every built-in mode can be a base.
- A mode "is" its base: `release_with_debug_info` is a `release` mode. That matters for mode-gated checks (see [Checks](../language/errors/checks.md)) and `std::build::mode_is`.
- A mode is selected on the command line by its class name: `--mode=profile`.

| Setting (method) | Values | `debug` | `release` |
|---|---|---|---|
| `optimization()` | `none`, `basic`, `size`, `speed` | `none` | `speed` |
| `debug_info()` | `none`, `line_tables`, `full` | `full` | `none` |
| `overflow()` | `panic`, `wrap` | `panic` | `panic` |
| `shifts()` | `panic`, `mask` (count `& (width - 1)`) | `panic` | `panic` |
| `bounds()` | `panic`, **`unchecked`** | `panic` | `panic` |
| `run_time_checks()` | `all`, `gated_only`, **`none`** | `all` | `all` |
| `panic()` | `abort`, `trap` (CPU trap instruction, no message) | `abort` | `abort` |
| `link_time_optimization()` | `bool` | `false` | `false` |
| `allow_undefined_behavior()` | `bool` | `false` | `false` |

### Modes can change safety rules, explicitly
A user mode may relax the language's safety rules. There are two levels:

1. **Still defined behaviour:** `overflow()` returning `wrap`, `shifts()` returning `mask`, `panic()` returning `trap`. The program is still free of undefined behaviour; it just behaves differently from the built-in modes on overflow and on bad shifts.
2. **Undefined behaviour possible:** `bounds()` returning `unchecked` and `run_time_checks()` returning `none` (or `gated_only`) remove the guards that safe code relies on. These are only accepted when the mode also overrides **`allow_undefined_behavior()` to return `true`**. Without it, the build script is a compile error.

Every build in a mode that relaxes a rule prints one line at the start, which also appears in CI logs:
```
note: building in mode 'fast' (base: release): overflow wraps, bounds checks off - undefined behaviour is allowed
```

Code that must keep a guarantee in **every** mode uses the explicit operators, which never change with the mode:

| Always | Operation |
|---|---|
| Wraps | `+%`, `-%`, `*%` |
| Checks and returns `std::option` | `std::checked_add`, `a.get(i)`, `cast::checked` |
| Saturates | `std::saturating_add`, `cast::saturate` |

`+`, `-`, `*`, `a[i]`, `<<` and `bit(n)` follow the mode.

### How edaxc uses the settings
- `optimization()`: LLVM's standard pipeline at `-O2` (`speed`; `-O3` was measured slower on `bench/`), `-O1` (`basic`) or `-Os` (`size`); none for `none`. Files and functions can have their own level (`g.optimize_files`, `@std::optimize`, see [Performance](../language/performance/performance.md)): the build runs the pipeline of the highest level asked for, and functions at `none` are compiled as written.
- `float_determinism()`: `relaxed` (the default) or `strict`; `strict` makes every `@std::float_mode` an error, so float results are identical on every machine.
- `sanitize()`: `none` (the default), `address` or `thread`. Every Edax function is instrumented (AddressSanitizer: out-of-bounds and use-after-free in `unsafe` code; ThreadSanitizer: data races), the program is linked with the sanitizer runtime, and the C and C++ targets of the build are compiled with the same `-fsanitize=`. Safe Edax code can't have these bugs; the setting finds them in `unsafe` code and in C/C++ dependencies.
  ```edax
  class asan : std::build::mode::debug {
  public:
      compile_time std::build::sanitizer sanitize() override { return std::build::sanitizer::address; }
  }
  ```
- `profile_guided()`: `std::none`, or the path of a profile recorded with `edaxc build --profile-generate`; the profile is a recorded input of the build.
- `debug_info()`: `line_tables` gives DWARF 5 line tables (statement by statement) for every Edax function, including the standard library's, enough for a debugger's breakpoints, stepping and backtraces. `full` also describes locals, parameters, `this`, loop variables and `match` bindings with their types, so that a debugger shows variables:
  - Built-in types are typedefs with their Edax names (`u32`, `f64`) of the matching C type; structs and classes are described from the backend's layout (base class first, then the vtable pointer or fields); simple enums are DWARF enumerations; an enum with payloads is `struct { tag; union { <alternative> ... } payload; }`.
  - Namespaces are DWARF namespaces (`std::darray<u32>`, `geometry::point`). A function is named by its full path, `namespace::type::name` (`std::darray<u32>::push`), and that is also its DWARF linkage name, so debuggers show it as written.
  - The standard library's files point at `library/std` of the tree edaxc was built from (when it still exists, otherwise at `/<std>/...`).
  - The prologue has no line, as in C/C++ at `-O0`: stepping into a function stops at its first statement with the parameters in place.
  - Building a value runs its type's field defaults in place (`default`, or fields left out of `T{...}`). Debug info describes that code as an inlined call of `<type>::default` located at the field declarations, the way C++ inlines a constructor, so Step Into walks `std::darray<T>`'s fields in `std.ed`.
- `overflow()`, `shifts()`, `panic()`: how `+ - *`, `<< >> bit(n)` and panics are compiled.
- `bounds()`: the standard library's index checks are written as `if compile_time (std::build::current_mode().bounds() == std::build::bounds::panic) { run_time check ... }`.
- `run_time_checks()`: applies to the program's own `run_time` checks; the library's index checks follow `bounds()`.
- `link_time_optimization()`: a program's Edax code is always one LLVM module; with `true`, the C/C++ targets it links are compiled with `-flto` (fat objects) and the program is linked with `-flto`, so C code is optimized together at link time.
- A mode's methods are `compile_time` (the interface declares them so, see [Inheritance and Interfaces](../language/user-types/inheritance-and-interfaces.md)); edaxc evaluates them while compiling.

### Reading the mode in code
All of these are `compile_time` functions:
```edax
std::build::mode::settings& m = std::build::current_mode();  // the settings of the mode being built
bool is_dbg = std::build::mode_is(std::build::mode::debug);  // true for debug and every mode derived from it

if compile_time (std::build::current_mode().debug_info() != std::build::debug_info::none) {
    ...
}
```
- `mode_is` with a user mode's name works only if the build defines that mode. Library code should use the built-in modes.
- Mode lists name a mode of build.ed by its full name, `build::profile`: build.ed declares everything in `namespace build` and every target imports it (see [Edax as a Build System](build-system.md#names-from-builded)).
- `std::build::current_mode()` returns a reference to the interpreter's value of the mode; compile-time-only functions may return references with no parameter to borrow from (12), since their results live for the whole compilation.
- Prefer testing a **setting** (`.overflow() == wrap`) over a mode name: the code then also does the right thing in modes it has never heard of.

### Scope of a mode
- A build runs in **one mode**. Every module, including dependencies, is compiled in that mode, so compiled interfaces are kept per mode.
- `build.ed` may override the mode **per target**, for example to build heavy dependencies with `release` inside a `debug` build (see [Edax as a Build System](build-system.md)). A target then uses that mode for all its own modules.
- **Entry checks follow the caller.** A `run_time` entry check (see [Checks](../language/errors/checks.md)) is compiled into each call, so the calling module's mode decides whether it's there. All other checks follow the mode of the module they're written in.

## Example
```edax
// build.ed (excerpt, see Edax as a Build System)
namespace build {
    // release with debug info and link-time optimization, for profiling.
    open class profile : std::build::mode::release_with_debug_info {
    public:
        compile_time bool link_time_optimization() override { return true; }
    }

    // Fast and unchecked: for benchmarks only.
    class fast : std::build::mode::release {
    public:
        compile_time std::build::overflow overflow() override { return std::build::overflow::wrap; }
        compile_time std::build::bounds bounds() override { return std::build::bounds::unchecked; }
        compile_time bool allow_undefined_behavior() override { return true; }   // required because of bounds()
    }

    compile_time void build(mut std::build::graph& g) {
        g.add_mode(profile);   // a type as a value (see Metaprogramming)
        g.add_mode(fast);
        ...
    }
}
```

## Rationale
- **Built-in modes, all safe:** anyone building a project knows what `debug` and `release` mean, and a library can rely on them.
- **Modes are classes:** a variation of a mode is exactly what inheritance expresses, so no second mechanism ("base" fields, copied settings) is needed. `profile` is a `release_with_debug_info`, so checks and code written for `release` apply to it automatically, and the compiler checks the overrides like any other.
- **Relaxing safety is possible, but never quiet:** the mode has to say what it relaxes; undefined behaviour additionally needs `allow_undefined_behavior`, and every build says so. That keeps principle 8 (control when you ask for it) without breaking principle 3 (safe by default).
- **Explicit operators don't follow the mode:** code that depends on wrapping or checking (hashes, checksums, parsers of untrusted input) says so with `+%` or `checked_*` and stays correct in every mode.
