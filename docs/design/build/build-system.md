# Edax as a Build System

A project is built by **`build.ed`**, a Edax file at the project root. It describes targets (programs, libraries, plugins, tests), build modes, settings, dependencies and custom steps. It builds **Edax and C/C++** code, so a mixed code base needs no CMake.

## Rules

### How `build.ed` runs
- `edaxc` evaluates `build.ed` with its **compile-time interpreter** (see [Compile-Time Evaluation](../language/compile-time/compile-time.md)). It isn't compiled into a program and never runs as one.
- Everything in `build.ed` is declared in **`namespace build`**, and it defines exactly one function there:
  ```edax
  namespace build {
      compile_time void build(mut std::build::graph& g) { ... }
  }
  ```
  The function adds things to `g`. The result is a **build graph**, which `edaxc` then executes, in parallel and incrementally.
- The full language is available: variables, loops, `if`, `match`, helper functions, structs, list literals, `import`s of other build modules (see "Reuse" below).
- **The build script is deterministic.** Like all compile-time code it has no I/O of its own. Its only view of the outside world is through `g`, and **every outside value it reads is recorded as an input**:

| Read through `g` | Recorded as input |
|---|---|
| `g.glob("src/**/*.ed")` | The matching file list |
| `g.option(setting, default, help)` | The option value from the command line |
| `g.env("VULKAN_SDK")` | The variable's value |
| `g.find_program("slangc")` | The program's path and hash |
| `g.host()`, `g.target_platform()` | Host and target platform (below) |

  If none of the recorded inputs changed, `edaxc` reuses the previous graph without evaluating `build.ed` again.

### Command line
```
edaxc build [--mode=<mode>] [--target=<platform>] [-D<option>=<value>] [<target>...]
edaxc run   [--mode=<mode>] <target> [-- <program args>]
edaxc test  [--mode=<mode>] [<test>...]
edaxc build --list            # targets, modes and options with their help text
edaxc build --compile-commands  # writes compile_commands.json for IDEs (clangd for the C/C++ parts)
edaxc expand <file> <name>    # prints a declaration with everything annotations generate (see Annotations)
edaxc simd-report <file> [<function>]   # how vector types and operations map to the target's instructions (see SIMD)
```
- `--root=DIR` names the project root (default: the current directory) and `--out-dir=DIR` the build directory (default: `<root>/build`).
- A mode that relaxes a rule prints its note first (see [Build Modes](build-modes.md)). `edaxc test` prints `PASS`/`FAIL` per test and fails when one fails.
- Without `build.ed`, `edaxc build main.ed` builds that one file as a program in `debug` mode.
- Build output goes to `build/<mode>/` (`build/<mode>/<platform>/` when cross-compiling).
- What a build prints (the checklist of targets, details, `build.log`, `--progress`, `--details`, `--stop-on-error`) is described in [Build output and diagnostics](build-output.md).

### Targets
| Method | Builds |
|---|---|
| `g.add_executable(std::build::edax_target{...})` | A Edax program: `.root` is its main `.ed` file, relative to the project root |
| `g.add_library(std::build::edax_target{...})` | A static Edax library (one or more modules) |
| `g.add_shared_library(std::build::edax_target{...})` | A shared Edax library, linked when the program starts |
| `g.add_plugin(std::build::edax_target{...})` | A shared library loaded by the program at run time (see [Dynamic Libraries and Plugins](../stdlib/system/dynamic-libraries.md)) |
| `g.add_c_library(std::build::c_target{...})` | A C or C++ library; `.linkage = std::build::linkage::shared` makes it a shared one |
| `g.add_test(std::build::edax_target{...})` | A test program, run by `edaxc test`: its `main`, or each of its `@std::test` functions |

Each method returns a **target handle** (`std::build::target`), a small copy type used to connect targets:
```edax
std::build::target core = g.add_library(std::build::edax_target{ .name = "core", .root = "core/core.ed" });
std::build::target app  = g.add_executable(std::build::edax_target{ .name = "app", .root = "app/main.ed" });
g.link(app, core);              // app imports core's modules and links it
```

C and C++ targets:
```edax
std::build::target vma = g.add_c_library(std::build::c_target{
    .name     = "vma",
    .language = std::build::language::cpp,
    .standard = std::build::cpp_standard::cpp20,
});
g.add_sources(vma, g.glob("vendor/VulkanMemoryAllocator/src/*.cpp"));
g.add_include_dirs(vma, ["vendor/VulkanMemoryAllocator/include"], std::build::visibility::shared);
g.define(vma, "VMA_STATIC_VULKAN_FUNCTIONS", "0");
```
- `visibility::shared` means "applies to the target and to everything that links it", like CMake's `PUBLIC`; `visibility::own` keeps it to the target (`PRIVATE`); `visibility::users` applies only to what links it (`INTERFACE`). (`public`/`private` are keywords, so the alternatives have other names.)
- Methods that take several values take a list: a list literal (see [Built-in Types and Conversions](../language/basics/types.md)) or what `g.glob` returns.
- **Edax calling C and C++:** `g.import_c_headers` turns C headers into a Edax header, and `g.wrap_cpp` makes C++ classes usable from Edax through generated C wrappers (see [C and C++ Interop](../language/interop/c-interop.md)).

### Target platforms
`g.target_platform()` returns a `std::build::platform`: the triple (`x86_64-linux-gnu`, `thumbv7em-none-eabihf`) and its **capabilities**:

| Capability | Meaning |
|---|---|
| `std::build::capability::os` | An operating system is present |
| `threads` | `std::thread`, the thread pool and blocking synchronization work |
| `heap` | The global allocator exists |
| `files` | `std::file` and `std::filesystem` work |
| `dynamic_libraries` | Shared libraries and plugins can be loaded |
| `simd` | The CPU has vector registers (otherwise vector types are computed lane by lane, see [SIMD](../language/simd/simd.md)) |

- The platform also has a **CPU level**: `std::build::cpu::x86_64_v2`, `x86_64_v3`, `x86_64_v4`, `armv8`, `armv9`, ..., set with `edaxc build --cpu=<level>` or in the target description. It decides which instructions vector code may use; `std::build::target_has_cpu_feature(std::build::cpu_feature::avx2)` tests a single feature at compile time.
- Code asks with the `compile_time` function `std::build::target_has(std::build::capability::threads)`.
- Standard-library functions that need a capability say so with an entry check, so using `std::thread::spawn` on a target without threads is a compile error at the call, with the library's message (see [Checks](../language/errors/checks.md)).
- A user target description (a struct passed to `edaxc --target=<file>`) can declare a custom bare-metal platform and its capabilities.

### Modes, settings and options
```edax
namespace build {
    open class profile : std::build::mode::release_with_debug_info { ... }    // see Build Modes

    // Enable the Vulkan validation layers.
    compile_time std::build::setting<bool> vulkan_validation = default;

    compile_time void build(mut std::build::graph& g) {
        g.add_mode(profile);
        bool validation = g.option(vulkan_validation, std::build::mode_is(std::build::mode::debug),
                                   "Enable the Vulkan validation layers");
        g.set(app, vulkan_validation, validation);            // visible to app's code
        g.set_mode(vma, std::build::mode::release);           // always optimize this dependency
    }
}
```
- Build modes are described in [Build Modes](build-modes.md).
- **A setting is a declared name**, a `compile_time std::build::setting<T>` constant. `T` is `bool`, an integer type, `std::static_str` or a simple enum. A misspelled setting is an unknown name on both sides, the build script and the code.
- **Options** make a setting configurable on the command line: `g.option(vulkan_validation, default, help)` reads `-Dvulkan_validation=true` (the setting's name, without its namespace) and returns the value.
- **Code reads a setting** with its `compile_time` methods:
  ```edax
  // in app's code
  compile_time bool validation = build::vulkan_validation.value();
  ```
  `value()` is a compile error when the build didn't `set` the setting for this target, so there are no silently missing `#define`s.
- **Per-file optimization:** `g.optimize_files(t, "engine/math/**/*.ed", std::build::optimization::speed)` optimizes the files of `t` that match (relative to the project root, or a package's root, so imported modules match too) at that level, whatever the mode. A debug build stays playable; one file can be kept at `none` in a release build (see [Performance](../language/performance/performance.md)). A pattern that starts with `<std>/` names the standard library's files instead (`g.optimize_files(t, "<std>/graphics/**", std::build::optimization::size)`), overriding the mode's `std_optimization()` for them.
- **Target CPU:** `g.target_cpu(t, std::build::cpu::x86_64_v3)` checks and compiles the Edax code of `t` for a CPU level instead of the platform's baseline: `std::build::target_has_cpu_feature` answers for that level, and the program needs such a CPU to run. A level of another architecture is an error when `t` is built.
- **Per-file values:** `g.set_for_files(t, "net/*.ed", setting, value)` passes a value to the files of `t` matching a glob (relative to the directory of the target's root file; a glob that matches nothing is an error). `setting.value_at(where)`, where `where` is a `std::call_site`, gives the file's value, else the target's, else `std::none`.
- Settings may be declared in any module both sides import; the standard library declares its own (e.g. the logger's levels, see [Logging](../stdlib/logging/logging.md)).

### Names from `build.ed`
- `build.ed` declares everything in `namespace build`, and **every target of the project imports it automatically**. Code names its declarations like any other: `build::profile` in a mode list (`run_time(build::profile) check`), `build::vulkan_validation.value()` for a setting, and types the build defines for shared use.
- `build.ed` itself can't declare global names; `namespace build` is reserved for it.

### Custom steps: tools and generated files
A step runs a program with **declared** inputs and outputs. The program is either a target of the same build (a Edax tool) or an external program:
```edax
std::build::program slangc = g.find_program("slangc");
for (std::build::path shader : g.glob("shaders/*.slang")) {
    std::build::path spirv = g.output_path(shader.with_extension("spv"));
    std::build::step s = g.add_step(std::build::step_spec{ .name = "compile shader", .program = slangc });
    g.add_arguments(s, [shader.text, "-o", spirv.text]);
    g.add_inputs(s, [shader]);
    g.add_outputs(s, [spirv]);
    g.depends_on(app, s);
}
```
- A step only runs when an input changed (or its outputs are missing).
- **Steps are verified after they run:** every declared output must exist, and a step that created or changed any other file in the project or the build directory fails the build, with the list of undeclared files. This keeps incremental builds correct without an OS sandbox.
- Steps run in parallel whenever the graph allows it.

### Packages
Outside code (Edax, C and C++ libraries, including CMake projects) comes in as **packages**: where it comes from, where it goes in the tree, and which targets link it:
```edax
std::build::package glfw = g.package(std::build::package_spec{
    .source = std::build::git{ .url = "https://github.com/glfw/glfw", .tag = "3.4" },
    .path   = "vendor/glfw",
});
g.link(app, glfw.target("glfw"));
```
Fetching, version locking (`build.lock`), CMake projects and the converter are described in [Packages](packages.md).

### Reuse
Build logic can live in ordinary modules and be imported:
```edax
// build.ed
import build_support::vulkan;          // build_support/vulkan.ed

namespace build {
    compile_time void build(mut std::build::graph& g) {
        std::build::target vulkan = build_support::vulkan::add(mut g);
        ...
    }
}
```
Imported modules follow the same rules: `compile_time` only, with the outside world reachable only through `g`.

## Example: an engine with C++ dependencies and shaders
```edax
// build.ed
import std::build;

namespace build {
    open class profile : std::build::mode::release_with_debug_info {
    public:
        compile_time bool link_time_optimization() override { return true; }
    }

    // Enable the Vulkan validation layers.
    compile_time std::build::setting<bool> vulkan_validation = default;

    compile_time void build(mut std::build::graph& g) {
        g.add_mode(profile);

        bool validation = g.option(vulkan_validation,
            std::build::mode_is(std::build::mode::debug),
            "Enable the Vulkan validation layers");

        // --- C/C++ dependencies -----------------------------------------
        std::build::target imgui = g.add_c_library(std::build::c_target{
            .name = "imgui",
            .language = std::build::language::cpp,
            .standard = std::build::cpp_standard::cpp20,
        });
        g.add_sources(imgui, g.glob("vendor/imgui/*.cpp"));
        g.add_include_dirs(imgui, ["vendor/imgui"], std::build::visibility::shared);
        g.set_mode(imgui, std::build::mode::release);

        std::build::target vulkan = g.add_system_library(std::build::system_library{
            .name = "vulkan",
            .root = g.env("VULKAN_SDK"),
        });

        // --- Edax code --------------------------------------------------
        std::build::target core = g.add_library(std::build::edax_target{ .name = "core", .root = "core/core.ed" });

        std::build::target renderer = g.add_library(std::build::edax_target{ .name = "renderer", .root = "renderer/renderer.ed" });
        g.link(renderer, core);
        g.link(renderer, imgui);
        g.link(renderer, vulkan);
        g.set(renderer, vulkan_validation, validation);

        std::build::target app = g.add_executable(std::build::edax_target{ .name = "engine", .root = "app/main.ed" });
        g.link(app, renderer);

        // --- Generated files --------------------------------------------
        std::build::program slangc = g.find_program("slangc");
        for (std::build::path shader : g.glob("shaders/*.slang")) {
            std::build::path spirv = g.output_path(shader.with_extension("spv"));
            std::build::step s = g.add_step(std::build::step_spec{ .name = "slang", .program = slangc });
            g.add_arguments(s, [shader.text, "-o", spirv.text]);
            g.add_inputs(s, [shader]);
            g.add_outputs(s, [spirv]);
            g.depends_on(app, s);
        }

        // --- Tests ------------------------------------------------------
        for (std::build::path test : g.glob("tests/*.ed")) {
            std::build::target t = g.add_test(std::build::edax_target{ .name = test.stem(), .root = test.text });
            g.link(t, core);
        }
    }
}
```
```
edaxc build                         # debug, validation on
edaxc build --mode=profile          # profile, validation off
edaxc run engine -Dvulkan_validation=true --mode=release
edaxc test
```

## Rationale
- **One language:** build logic uses the same syntax, types and error messages as the code it builds. There is no second language like CMake's to learn.
- **Evaluated at compile time:** the build script can't do I/O except through recorded inputs, so builds are reproducible and the graph can be cached reliably. Anything that needs real I/O (code generators, shader compilers) is a step with declared inputs and outputs, which keeps incremental builds correct.
- **A graph, not a script that does the building:** evaluating `build.ed` only *describes* the work. `edaxc` decides the order, the parallelism and what is up to date.
- **`namespace build`, imported everywhere:** build declarations follow the normal namespace rules, so code reaches them by an ordinary name and nothing is implicit about where they live.
- **Settings are declared names:** like diagnostic ids, a typo is an unknown name on both sides instead of a string that never matches.
- **Verify steps instead of sandboxing them:** a check after each step is portable and catches every undeclared write; an OS sandbox would be platform-specific.
- **Capabilities instead of separate libraries:** one standard library for every target; what a target can't do is a compile error at the call, in the library's words.
- **Pinned packages and `build.lock`:** a build from the same commit always uses the same package sources (see [Packages](packages.md)).
- **Settings instead of `#define`s:** values from the build reach the code as typed `compile_time` constants, and a missing one is an error instead of an empty macro.
