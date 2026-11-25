# Lesson 11: `build.ed`: the build is Edax code

There is no CMake, no Makefile and no preprocessor. A project's `build.ed` is ordinary Edax code that the compiler runs **at compile time** (lesson 8). It doesn't run commands: it **describes** a graph of targets, and `edaxc` decides the order, the parallelism, and what is up to date. The configuration that C++ passes through `#define` becomes typed **settings** that code reads at compile time, with misspellings caught on both sides.

**You will learn:** targets and how they link, C libraries, settings and command-line options, build modes as classes, and the commands that build, run and test.

**You will build:** you will complete a project's `build.ed` (`exercise/build.ed`).

---

## 1. A minimal project

```edax
// build.ed
import std;

namespace build {
    compile_time void build(mut std::build::graph& g) {
        std::build::target app = g.add_executable(std::build::edax_target{ .name = "app", .root = "app/main.ed" });
    }
}
```

That's the whole build of a program. The function is called `build::build`, it is `compile_time`, and it receives the graph to fill in. The root file is the program's `main` file; the modules it imports are found by their paths.

```
edaxc build                 # every program of ./build.ed, in debug
edaxc run app               # build one target and run it
edaxc build --mode=release  # or development, release_with_debug_info, or your own mode
edaxc build --list          # the targets, modes and options
edaxc test                  # build and run the test targets
--root=<dir>                # use <dir>/build.ed instead of ./build.ed
--out-dir=<dir>             # where build/<mode>/ goes
```

`edaxc build file.ed` and `edaxc run file.ed` (as in the earlier lessons) work without any `build.ed`. Note that `edaxc build file.ed` writes `build/<mode>/` **next to the file**, while `edaxc run file.ed` builds in a temporary directory.

## 2. Targets

| Call | Builds |
|---|---|
| `g.add_executable(edax_target{ .name, .root })` | a program |
| `g.add_library(edax_target{...})` | an Edax library, compiled into the programs that link it |
| `g.add_shared_library(edax_target{...})` | `lib<name>.so`, loaded at startup |
| `g.add_plugin(edax_target{...})` | a plugin, loaded at run time (lesson 12) |
| `g.add_c_library(c_target{ .name })` | C/C++ code, with `g.add_sources(t, "...")`, `g.add_include_dir`, `g.define(t, "NAME", "value")` |
| `g.add_test(edax_target{...})` | run by `edaxc test`: its `main`, or each of its `@std::test` functions |
| `g.package(...)` | outside code: git, archive or local; built by its own build.ed, its CMakeLists.txt (read by edaxc), or a description of yours |

Each call returns a `std::build::target`. **`g.link(user, used)`** connects two of them:
- linking an Edax library lets the program `import` the library's modules (`import stats::stats;` for `stats/stats.edh`);
- linking a C library lets the program call its functions, declared with `extern(std::build::language::c) f64 scale(f64 x);` (calls into C are `unsafe`), or through modules generated from its headers with `g.import_c_headers`.

## 3. Settings instead of `#define`

A setting is a typed, named value that `build.ed` gives to a target, and that the target's code reads **at compile time**:

```edax
// build.ed
namespace build {
    // Print more.                                       <- this comment is the setting's documentation
    compile_time std::build::setting<bool> verbose = default;

    compile_time void build(mut std::build::graph& g) {
        bool chatty = g.option(verbose, std::build::mode_is(std::build::mode::debug), "Print more");
        ...
        g.set(app, verbose, chatty);
    }
}
```
```edax
// app/main.ed
compile_time bool verbose = build::verbose.value();
...
if compile_time (verbose) { std::println("verbose: on"); }
```

- `g.option(setting, default, help)` makes it a **command-line option**: `-Dverbose=false`. Its default here depends on the mode: on in debug builds, off otherwise. `edaxc build --list` shows the options with their help text and current value.
- `g.set(target, setting, value)` gives a target its value. Different targets can get different values.
- **A misspelled setting is an error on both sides**, because it's a name, not a string: `error[E3020]: cannot find 'verbos' in this scope` + `note: did you mean 'verbose'?`, in `build.ed` or in the code that reads it.
- **`value()` is a compile error when the build didn't set it for this target:** `error[E4201]: build.ed does not set 'build::greeting' for this target`. There are no silently missing `#define`s, and no `#ifdef` typos that quietly compile the other branch.

## 4. Build modes are classes

```edax
class fast : std::build::mode::release {
public:
    compile_time std::build::overflow overflow() override { return std::build::overflow::wrap; }
}
...
g.add_mode(fast);
```

- A mode is a class of `compile_time` settings: `optimization()`, `debug_info()`, `overflow()`, `bounds()`, `sanitize()`, `link_time_optimization()`, `std_optimization()`, `profile_guided()` and more. Derive from the closest built-in mode and override what differs. That's lesson 5's inheritance, evaluated at compile time.
- Code asks `std::build::mode_is(build::fast)`: true for `fast` **and every mode derived from it**. So `mode_is(std::build::mode::release)` is true in `fast` too.
- Modes that weaken safety must say so. A mode that turns off bounds checks is refused unless it also overrides `allow_undefined_behavior()` to return `true`: `error[E4215]: the mode 'fast' turns off bounds checks but does not allow undefined behaviour`.
- Checks can be tied to modes: `run_time(std::build::mode::debug) check ...` (lesson 8).
- Per target: `g.set_mode(t, std::build::mode::release)` keeps a heavy dependency optimized inside a debug build, and `g.optimize_files(t, "engine/math/**", speed)` does the same for some files.
- Unoptimized and `development` builds are cached per unit, so an edit recompiles little.

## 5. Steps and packages

- `g.add_step(...)` runs a tool (a shader compiler, a code generator) with **declared** inputs and outputs. The build checks afterwards that the step wrote nothing else, so incremental builds stay correct.
- Packages are pinned in `build.lock`. See `docs/design/build/packages.md` and the `edaxc package` commands.

---

## C++/CMake → Edax at a glance

| CMake / C++ | Edax |
|---|---|
| `CMakeLists.txt` | `build.ed`, Edax code run at compile time |
| `add_executable(app main.cpp)` | `g.add_executable(edax_target{ .name = "app", .root = "app/main.ed" })` |
| `add_library(stats ...)` | `g.add_library(edax_target{...})` |
| `target_link_libraries(app stats)` | `g.link(app, stats)` |
| `target_compile_definitions(scale PRIVATE FACTOR=3)` | `g.define(scale, "FACTOR", "3")` (C code) |
| `option(VERBOSE ...)` + `#ifdef VERBOSE` | `g.option(verbose, ...)` + `build::verbose.value()` |
| `CMAKE_BUILD_TYPE=Release` | `--mode=release` |
| a custom build type | a mode class derived from a built-in one |
| `#ifdef NDEBUG` | `if compile_time (std::build::mode_is(std::build::mode::debug))` |
| `add_test` + CTest | `g.add_test` + `edaxc test` |
| `FetchContent` | `g.package(...)` + `build.lock` |

---

## Your goal

`exercise/` is a project with a program (`app/`), an Edax library (`stats/`) and a C file (`vendor/scale.c`). Its `build.ed` only declares the program, so nothing builds yet. Complete `build.ed` so that the program prints:

```
hello from build.ed
mean 2.5, scaled 7.5
verbose: on
fast mode: false
```

`app/main.ed` is written: read it first, it tells you every name `build.ed` has to provide (`build::greeting`, `build::verbose`, `build::fast`, the library, the C function).

Then run it yourself with `--mode=fast` and with `-Dverbose=false`, and explain each difference:

```
build/compiler/edaxc run --root=course/11-build/exercise --out-dir=/tmp/b11 --mode=fast app
build/compiler/edaxc run --root=course/11-build/exercise --out-dir=/tmp/b11 -Dverbose=false app
```

<details>
<summary>Hint 1: the mode and the settings</summary>

Inside `namespace build`: the `fast` class from section 4, then two settings, `compile_time std::build::setting<std::static_str> greeting = default;` and the `bool` one, each with a `//` comment above it.
</details>

<details>
<summary>Hint 2: the C library</summary>

```edax
std::build::target scale = g.add_c_library(std::build::c_target{ .name = "scale" });
g.add_sources(scale, "vendor/scale.c");
g.define(scale, "FACTOR", "3");
```
</details>

<details>
<summary>Hint 3: putting it together</summary>

`g.add_mode(fast);` and the `g.option(...)` for `verbose` come first. After the targets: `g.link(app, lib); g.link(app, scale); g.set(app, greeting, "hello from build.ed"); g.set(app, verbose, chatty);`
</details>

<details>
<summary>Why <code>--mode=fast</code> prints no "verbose: on"</summary>

The option's default is `mode_is(debug)`, and `fast` derives from `release`. Without `-Dverbose=true`, `verbose` is false in `fast`, so the `if compile_time` branch isn't even compiled.
</details>

---

## Try this afterwards

1. Misspell `verbose` in `g.set`. Then misspell it in `app/main.ed`.
2. Remove the `g.set(app, greeting, ...)` line.
3. Add `compile_time std::build::bounds bounds() override { return std::build::bounds::unchecked; }` to `fast`, and build with `--mode=fast`.
4. Add a test target `tests/stats_test.ed` with a `@std::test` function, link `stats` to it, and run `edaxc test`.

<details>
<summary>What happens</summary>

1. `error[E3020]: cannot find 'verbos' in this scope` with `note: did you mean 'verbose'?` in `build.ed`; `cannot find 'verbos' in namespace 'build'` in `main.ed`.
2. `error[E4201]: build.ed does not set 'build::greeting' for this target`, at the `value()` in `main.ed`.
3. `error[E4215]: the mode 'fast' turns off bounds checks but does not allow undefined behaviour`, with a note on the override that says so. In debug builds nothing changes: the mode isn't used.
4. A test file is a program without `main`:
   ```edax
   import std;
   import stats::stats;

   @std::test
   void mean_of_two() {
       std::array<f64, 2> v = [1.0, 3.0];
       run_time check (stats::mean(v.as_span()) == 2.0) else "mean of 1 and 3";
   }
   ```
   with `std::build::target t = g.add_test(std::build::edax_target{ .name = "stats_test", .root = "tests/stats_test.ed" }); g.link(t, lib);`. `edaxc test` prints `PASS stats_test::mean_of_two` and `1 of 1 tests passed`.
</details>

## Recap

- `build.ed` is compile-time Edax code that describes targets; `edaxc` schedules them.
- `g.link` connects programs, Edax libraries and C libraries.
- Settings are typed and checked on both sides; `g.option` makes them `-D` options.
- Modes are classes; `mode_is` follows inheritance; unsafe modes must say so.

Next: [lesson 12](../12-performance/README.md), performance and plugins.
