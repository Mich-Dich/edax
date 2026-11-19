# 24 - Build Modes

A **build mode** is a named set of settings that applies to a whole build: optimization, debug info, how arithmetic and checks behave, and which mode-gated checks (see 23) are compiled in. A mode is a class; the standard library defines `debug` and `release` (and `release_with_debug_info`, derived from `release`). Projects derive their own in `build.rifts` (see 25).

## Rules

### Built-in modes
| Mode (`std::build::mode::...`) | Base class | Optimization | Debug info | Overflow, shifts, `bit(n)` | Bounds | `run_time` checks |
|---|---|---|---|---|---|---|
| `debug` | - | none | full | panic | panic | ungated + `debug` |
| `release` | - | speed | none | panic | panic | ungated + `release` |
| `release_with_debug_info` | `release` | speed | full | panic | panic | ungated + `release` |

```
riftc build                                 # debug (the default)
riftc build --mode=release
riftc build --mode=profile                  # a mode defined in build.rifts
```

**All built-in modes are safe.** The language rules from 16 (overflow panics, bounds checks, etc.) hold in each of them.

### Modes are classes
A mode is a **class** in `std::build::mode` that implements the interface `std::build::mode::settings`. The settings are its methods, so a mode changes a setting the way any derived class changes behaviour: it **overrides** the method.
```rift
// in the standard library (std::build::mode)
interface settings {
    std::build::optimization    optimization();
    std::build::debug_info      debug_info();
    std::build::overflow        overflow();
    std::build::shifts          shifts();
    std::build::bounds          bounds();
    std::build::run_time_checks run_time_checks();
    std::build::panic_behavior  panic();
    bool                        link_time_optimization();
    bool                        allow_undefined_behavior();
}

open class debug : settings { ... }            // every method returns the debug value
open class release : settings { ... }
open class release_with_debug_info : release {
public:
    std::build::debug_info debug_info() override { return std::build::debug_info::full; }
}
```
- The standard library defines the two modes `debug` and `release`. `release_with_debug_info` is the first derived mode, and the library provides it too.
- A user mode **inherits** from one of them (or from another user mode) and overrides what it changes. Everything else comes from its base. Because the modes are `open`, every built-in mode can be a base.
- A mode "is" its base: `release_with_debug_info` is a `release` mode. That matters for mode-gated checks (see 23) and `std::build::mode_is`.
- A mode is selected on the command line by its class name: `--mode=profile`.

| Setting (method) | Values | `debug` | `release` |
|---|---|---|---|
| `optimization()` | `none`, `size`, `speed` | `none` | `speed` |
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

### How riftc uses the settings
- `optimization()`: LLVM's standard pipeline at `-O2` (`speed`) or `-Os` (`size`); none for `none`.
- `debug_info()`: `line_tables` gives DWARF 5 line tables (statement by statement) for every Rift function, including the standard library's, enough for a debugger's breakpoints, stepping and backtraces. `full` also describes locals, parameters, `this`, loop variables and `match` bindings with their types, so that a debugger shows variables:
  - Built-in types are typedefs with their Rift names (`u32`, `f64`) of the matching C type; structs and classes are described from the backend's layout (base class first, then the vtable pointer or fields); simple enums are DWARF enumerations; an enum with payloads is `struct { tag; union { <alternative> ... } payload; }`.
  - Modules are namespaces (`std::darray<u32>`, `geometry::point`). A function is named `module::type::name` (`std::darray<u32>::push`), and that is also its DWARF linkage name, so debuggers show it as written.
  - The standard library's files point at `library/std` of the tree riftc was built from (when it still exists, otherwise at `/<std>/...`).
  - The prologue has no line, as in C/C++ at `-O0`: stepping into a function stops at its first statement with the parameters in place.
  - Building a value runs its type's field defaults in place (`default`, or fields left out of `T{...}`). Debug info describes that code as an inlined call of `<type>::default` located at the field declarations, the way C++ inlines a constructor, so Step Into walks `std::darray<T>`'s fields in `std.rifts`.
- `overflow()`, `shifts()`, `panic()`: how `+ - *`, `<< >> bit(n)` and panics are compiled.
- `bounds()`: the standard library's index checks are written as `if compile_time (std::build::current_mode().bounds() == std::build::bounds::panic) { run_time check ... }`.
- `run_time_checks()`: applies to the program's own `run_time` checks; the library's index checks follow `bounds()`.
- `link_time_optimization()`: a program's Rift code is always one LLVM module; with `true`, the C/C++ targets it links are compiled with `-flto` (fat objects) and the program is linked with `-flto`, so C code is optimized together at link time.
- A mode's methods run at compile time without being marked `compile_time`: riftc reads them with its interpreter.

### Reading the mode in code
All of these are `compile_time` functions:
```rift
std::build::mode::settings& m = std::build::current_mode();  // the settings of the mode being built
bool is_dbg = std::build::mode_is(std::build::mode::debug);  // true for debug and every mode derived from it

if compile_time (std::build::current_mode().debug_info() != std::build::debug_info::none) {
    ...
}
```
- `mode_is` with a user mode's name works only if the build defines that mode. Library code should use the built-in modes.
- Mode lists name a mode of build.rifts as `build::profile` (build.rifts is the module `build` of every target; see 22, question 31).
- `std::build::current_mode()` returns a reference to the interpreter's value of the mode; compile-time-only functions may return references with no parameter to borrow from (12), since their results live for the whole compilation.
- Prefer testing a **setting** (`.overflow() == wrap`) over a mode name: the code then also does the right thing in modes it has never heard of.

### Scope of a mode
- A build runs in **one mode**. Every module, including dependencies, is compiled in that mode, so compiled interfaces are kept per mode.
- `build.rifts` may override the mode **per target**, for example to build heavy dependencies with `release` inside a `debug` build (see 25). A target then uses that mode for all its own modules.
- **Entry checks follow the caller.** A `run_time` entry check (see 23) is compiled into each call, so the calling module's mode decides whether it's there. All other checks follow the mode of the module they're written in.

## Example
```rift
// build.rifts (excerpt, see 25)

/// release with debug info and link-time optimization, for profiling.
open class profile : std::build::mode::release_with_debug_info {
public:
    bool link_time_optimization() override { return true; }
}

/// Fast and unchecked: for benchmarks only.
class fast : std::build::mode::release {
public:
    std::build::overflow overflow() override { return std::build::overflow::wrap; }
    std::build::bounds bounds() override { return std::build::bounds::unchecked; }
    bool allow_undefined_behavior() override { return true; }   // required because of bounds()
}

g.add_mode(profile);   // a type as a value (see 26)
g.add_mode(fast);
```

## Rationale
- **Built-in modes, all safe:** anyone building a project knows what `debug` and `release` mean, and a library can rely on them.
- **Modes are classes:** a variation of a mode is exactly what inheritance expresses, so no second mechanism ("base" fields, copied settings) is needed. `profile` is a `release_with_debug_info`, so checks and code written for `release` apply to it automatically, and the compiler checks the overrides like any other.
- **Relaxing safety is possible, but never quiet:** the mode has to say what it relaxes; undefined behaviour additionally needs `allow_undefined_behavior`, and every build says so. That keeps principle 8 (control when you ask for it) without breaking principle 3 (safe by default).
- **Explicit operators don't follow the mode:** code that depends on wrapping or checking (hashes, checksums, parsers of untrusted input) says so with `+%` or `checked_*` and stays correct in every mode.
