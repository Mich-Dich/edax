# Build Modes

A **build mode** is a named set of settings that applies to a whole build: optimization, debug info, how arithmetic and checks behave, and which mode-gated checks (see [Checks](../language/errors/checks.md)) are compiled in. A mode is a class; the standard library defines `debug` and `release`, and `development` (derived from `debug`) and `release_with_debug_info` (derived from `release`). Projects derive their own in `build.ed` (see [Edax as a Build System](build-system.md)).

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

**All built-in modes are safe.** The language's safety rules (overflow panics, bounds checks, etc., see [Operators](../language/basics/operators.md)) hold in each of them.

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
    compile_time std::build::sanitizer       sanitize();
    compile_time std::build::optimization    std_optimization();
    compile_time std::option<std::static_str> profile_guided();
}

open class debug : settings { ... }            // every method returns the debug value
open class release : settings { ... }
open class development : debug {
public:
    compile_time std::build::optimization optimization() override { return std::build::optimization::basic; }
}
open class release_with_debug_info : release {
public:
    compile_time std::build::debug_info debug_info() override { return std::build::debug_info::full; }
}
```
- The standard library defines the two base modes `debug` and `release`, and two derived ones: `development` (a `debug` with light optimization) and `release_with_debug_info`.
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
| `float_determinism()` | `relaxed`, `strict` | `relaxed` | `relaxed` |
| `sanitize()` | `none`, `address`, `thread` | `none` | `none` |
| `std_optimization()` | as `optimization()` | `speed` | `speed` |
| `profile_guided()` | `std::option<std::static_str>` | `std::none` | `std::none` |

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
- `std_optimization()`: how the standard library's own code is optimized, whatever `optimization()` says for the program's code. `speed` in every built-in mode, `debug` included: the containers, sorts, hashing and formatting a game leans on run at full speed in a debug build, while the program's own code stays unoptimized and steppable. Calls from unoptimized code into the library are calls (they aren't inlined into it); the library inlines into itself. A mode returns `none` to step through the library in a debugger:
  ```edax
  class debug_std : std::build::mode::debug {
  public:
      compile_time std::build::optimization std_optimization() override { return std::build::optimization::none; }
  }
  ```
  Measured on `bench/` in a debug build: `sort` 0.129 s → 0.036 s; a loop of `vec3` math whose functions are the program's own, 0.29 s → 0.20 s (only its `darray` accesses got faster). Building costs about 0.1 s more for small programs.
- `sanitize()`: `none` (the default), `address` or `thread`. Every Edax function is instrumented (AddressSanitizer: out-of-bounds and use-after-free in `unsafe` code; ThreadSanitizer: data races), the program is linked with the sanitizer runtime, and the C and C++ targets of the build are compiled with the same `-fsanitize=`. Safe Edax code can't have these bugs; the setting finds them in `unsafe` code and in C/C++ dependencies.
  ```edax
  class asan : std::build::mode::debug {
  public:
      compile_time std::build::sanitizer sanitize() override { return std::build::sanitizer::address; }
  }
  ```
- `profile_guided()`: `std::none` (every built-in mode), or the path of a merged profile (relative to the project) for profile-guided optimization. The profile is an input of the build: a newer profile rebuilds the program. The workflow:
  ```
  edaxc build --mode=release --profile-generate app   # instrumented: writes build/release/app-<pid>.profraw when it runs
  ./build/release/app                                 # play a representative session
  llvm-profdata merge -o app.profdata build/release/app-*.profraw
  edaxc build --mode=pgo app                          # a mode whose profile_guided() returns std::some("app.profdata")
  ```
  `--profile-generate` needs LLVM's profile runtime (compiler-rt, e.g. `libclang-rt-18-dev`); edaxc says so when it is missing. A profile only helps optimized code, so programs built with one are compiled as one module (without the object cache).
- `debug_info()`: `line_tables` gives DWARF 5 line tables (statement by statement) for every Edax function, including the standard library's, enough for a debugger's breakpoints, stepping and backtraces. `full` also describes locals, parameters, `this`, loop variables and `match` bindings with their types, so that a debugger shows variables:
  - Built-in types are typedefs with their Edax names (`u32`, `f64`) of the matching C type; structs and classes are described from the backend's layout (base class first, then the vtable pointer or fields); simple enums are DWARF enumerations; an enum with payloads is `struct { tag; union { <alternative> ... } payload; }`.
  - Namespaces are DWARF namespaces (`std::darray<u32>`, `geometry::point`). A function is named by its full path, `namespace::type::name` (`std::darray<u32>::push`), and that is also its DWARF linkage name, so debuggers show it as written.
  - The standard library's files point at `library/std` of the tree edaxc was built from (when it still exists, otherwise at `/<std>/...`).
  - The prologue has no line, as in C/C++ at `-O0`: stepping into a function stops at its first statement with the parameters in place.
  - Building a value runs its type's field defaults in place (`default`, or fields left out of `T{...}`). Debug info describes that code as an inlined call of `<type>::default` located at the field declarations, the way C++ inlines a constructor, so Step Into walks `std::darray<T>`'s fields in `std.ed`.
- `overflow()`, `shifts()`, `panic()`: how `+ - *`, `<< >> bit(n)` and panics are compiled.
- `bounds()`: the standard library's index checks are written as `if compile_time (std::build::current_mode().bounds() == std::build::bounds::panic) { run_time check ... }`.
- `run_time_checks()`: applies to the program's own `run_time` checks; the library's index checks follow `bounds()`.
- `link_time_optimization()`: a program's Edax code is optimized as a whole (one LLVM module, or ThinLTO for `development`, see "The object cache"); with `true`, the C/C++ targets it links are compiled with `-flto` (fat objects) and the program is linked with `-flto`, so C code is optimized together at link time.
- A mode's methods are `compile_time` (the interface declares them so, see [Inheritance and Interfaces](../language/user-types/inheritance-and-interfaces.md)); edaxc evaluates them while compiling.

### The object cache
Builds whose program code is unoptimized (`debug` and the modes derived from it, as long as no file or function of the program asks for optimization) reuse object code between builds:
- edaxc compiles the program in **units**: one per module of the program, and a few per module of the standard library (a type's methods stay in one unit). Each unit's object code is stored under a hash of its LLVM IR and of everything else that decides the code: the edaxc build, LLVM, the target, the CPU level, the optimization level and the sanitizer. A unit whose IR is the same as in an earlier build is not compiled again; in practice the standard library's units almost always are.
- The key is the code itself, so a cached object can't be stale: anything that changes a unit's code (an edit, another mode setting, a new template instance) changes its key.
- Names in the generated code come from what they name (a type, a template's arguments, a string's contents), never from the order code was generated in, so an edit in one module leaves the other units' code unchanged.
- Small optimized functions a library unit calls from another unit are copied into it so they still inline there; the program's own unoptimized code calls the library, as it does without the cache.
- **`basic` optimization (the `development` mode) goes through the cache too, with LLVM's ThinLTO.** Each unit is stored as bitcode with a summary of what it defines and calls; ThinLTO then imports the functions worth inlining into each unit (up to 300 instructions), makes internal what no other unit uses, optimizes each unit and compiles it. Its objects are cached under the unit, everything it imported and the edaxc build, so an edit recompiles the edited unit and the units that imported from it. Windowing example, `development`: 4.4 s as one module; with the cache 7 s the first time, 2.1-2.4 s after an edit in `main.ed`. The price is run time: across units, a called function stays external, so `bench/` runs up to 1.5x slower than as one module (`text_parse` 1.5x, `vec_push_sum` 1.25x, `hash_int` 1.15x; the rest within 5%).
- **`speed` and `size` (`release`, `release_with_debug_info`) stay one module**, so everything can be inlined into everything: they are the builds that ship. `EDAX_THINLTO=on` puts `speed` through ThinLTO as well (an edit to the windowing example: 3.2 s → 1.5 s, at the run-time price above); `EDAX_THINLTO=off` keeps `basic` in one module.
- Builds with a profile (`profile_guided()`, `--profile-generate`), a sanitizer, plugins and shared libraries are compiled as one module.
- The cache is `$EDAX_CACHE_DIR`, else `$XDG_CACHE_HOME/edax/objects`, else `~/.cache/edax/objects`. `EDAX_CACHE_DIR=off` turns it off. It is shared by all projects, kept under 2 GB, and objects unused for 30 days are removed. `EDAX_CACHE_STATS=1` prints how many units were reused; `EDAX_CACHE_STATS=2` also names the units that were compiled, and `EDAX_CACHE_DUMP=<dir>` writes each unit's IR there.

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
- `std::build::current_mode()` returns a reference to the interpreter's value of the mode; compile-time-only functions may return references with no parameter to borrow from (see [Ownership](../language/memory/ownership-and-references.md)), since their results live for the whole compilation.
- Prefer testing a **setting** (`.overflow() == wrap`) over a mode name: the code then also does the right thing in modes it has never heard of.

### Scope of a mode
- A build runs in **one mode**. Every module, including dependencies, is compiled in that mode, so cached objects are kept per mode (the mode's settings are part of the object cache's key; see [Headers, sources and modules](../language/organization/modules.md)).
- `build.ed` may override the mode **per target** with `g.set_mode(t, mode)` (see [Edax as a Build System](build-system.md)), to build heavy dependencies optimized inside a `debug` build:
  - a C or C++ target is compiled with that mode's optimization;
  - an Edax program or plugin target is built entirely in that mode;
  - an Edax library target is compiled into each program that links it, as part of that program: its files get the mode's optimization (like `g.optimize_files`), while checks, overflow, shifts and debug info follow the program's mode. A program has one set of safety rules.
- **Checks follow the program.** A program is compiled in one mode, so its mode decides every check, the entry checks of functions in other modules included. A `run_time` entry check is compiled into the function and reports the caller's line, which each call passes along.

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
