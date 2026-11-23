# Lesson 11: `build.ed`: the build is Edax code

There is no CMake. A project's `build.ed` is Edax code that the compiler runs at compile time. It
**describes** a graph of targets, and `edaxc` then decides the order, the parallelism and what is
up to date.

```edax
// build.ed
import std;

namespace build {
    compile_time void build(mut std::build::graph& g) {
        std::build::target app = g.add_executable(std::build::edax_target{ .name = "app", .root = "app/main.ed" });
    }
}
```

```
edaxc build                 # every program of ./build.ed, in debug
edaxc run app               # build and run one target
edaxc build --mode=release  # or development, release_with_debug_info, or your own mode
edaxc build --list          # the targets, modes and options
edaxc test                  # the test targets
```

## Targets

| Call | Builds |
|---|---|
| `g.add_executable(edax_target{ .name, .root })` | a program |
| `g.add_library(edax_target{...})` | an Edax library, compiled into the programs that link it |
| `g.add_shared_library(edax_target{...})` | `lib<name>.so`, loaded at startup |
| `g.add_plugin(edax_target{...})` | a plugin, loaded at run time (lesson 12) |
| `g.add_c_library(c_target{ .name, .language })` | C/C++ code: `g.add_sources`, `g.add_include_dir`, `g.define` |
| `g.add_test(edax_target{...})` | run by `edaxc test` |
| `g.package(...)` | outside code: git, archive or local; its own build.ed, its CMakeLists.txt (read by edaxc), or a description of yours |

`g.link(user, used)` connects them. A program imports a linked library's modules, and calls into a
C library through `extern(std::build::language::c)` declarations, or through modules generated
from its headers with `g.import_c_headers`.

## Settings instead of `#define`

```edax
// Print more.                                          <- the comment documents the setting
compile_time std::build::setting<bool> verbose = default;
...
bool chatty = g.option(verbose, std::build::mode_is(std::build::mode::debug), "Print more");   // -Dverbose=...
g.set(app, verbose, chatty);
```

The code reads it at compile time: `compile_time bool verbose = build::verbose.value();`.
- A misspelled setting is an unknown name on **both** sides.
- `value()` is a compile error when the build didn't `set` it for this target, so there are no
  silently missing `#define`s.

## Build modes are classes

```edax
class fast : std::build::mode::release {
public:
    compile_time std::build::overflow overflow() override { return std::build::overflow::wrap; }
}
```

- A mode is a class of `compile_time` settings: `optimization()`, `debug_info()`, `overflow()`,
  `bounds()`, `sanitize()`, `link_time_optimization()`, `std_optimization()`,
  `profile_guided()`, and more. Derive one and override what differs; register it with
  `g.add_mode(fast)`.
- Code asks `std::build::mode_is(build::fast)`. That is true for `fast` and for every mode derived
  from it.
- Checks can be gated by mode: `run_time(std::build::mode::debug) check ...` (lesson 8).
- Per target: `g.set_mode(t, std::build::mode::release)` keeps a heavy dependency optimized
  inside a debug build. `g.optimize_files(t, "engine/math/**", speed)` does the same for some
  files.
- Unoptimized and `development` builds are cached per unit, so an edit recompiles little.

## Steps and packages

- `g.add_step(...)` runs a tool with **declared** inputs and outputs (shaders, code generators).
  The build checks afterwards that the step wrote nothing else.
- Packages are pinned in `build.lock`. See `docs/design/build/packages.md` and the
  `edaxc package` commands.

## Your goal

`exercise/` is a project with a program (`app/`), a Edax library (`stats/`) and a C file
(`vendor/scale.c`). Its `build.ed` only declares the program, so nothing builds yet. Complete
`build.ed` so that the program prints:

```
hello from build.ed
mean 2.5, scaled 7.5
verbose: on
fast mode: false
```

Then run it yourself with `--mode=fast` and with `-Dverbose=false`:

```
edaxc run --root=course/11-build/exercise --out-dir=/tmp/b11 --mode=fast app
```

## Try this afterwards

1. Misspell `verbose` in `g.set`. Then misspell it in `app/main.ed`.
2. Remove the `g.set(app, greeting, ...)` line.
3. Add `std::build::bounds bounds() override { return std::build::bounds::unchecked; }` to
   `fast`. The error explains what else such a mode must say.
4. Add a test target `tests/stats_test.ed` with a `@std::test` function, and run `edaxc test`.
