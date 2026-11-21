# Packages

A **package** is outside code your project uses: a Rift library, a C or C++ library, or a header-only one. You say where it comes from, where it goes in your tree, and which of your targets link it. `riftc` fetches it, works out how to build it, locks the exact version, and builds it with the same build system as your own code, including packages that only have a `CMakeLists.txt`.

```rift
// build.rifts
namespace build {
    compile_time void build(mut std::build::graph& g) {
        std::build::package glfw = g.package(std::build::package_spec{
            .source = std::build::git{ .url = "https://github.com/glfw/glfw", .tag = "3.4" },
            .path   = "vendor/glfw",
        });

        std::build::target app = g.add_executable(std::build::rift_target{ .name = "game", .root = "app/main.rifts" });
        g.link(app, glfw.target("glfw"));
    }
}
```
That is all a CMake-based C library needs: `riftc build` clones GLFW into `vendor/glfw`, reads its `CMakeLists.txt`, and builds the `glfw` target with the project's compiler, mode and platform.

## Rules

### 1. Declaring a package
`g.package(spec)` returns a `std::build::package`. The spec:

| Field | Meaning | Default |
|---|---|---|
| `.source` | Where it comes from: `std::build::git{ .url, .commit }` or `{ .url, .tag }`, `std::build::archive{ .url, .sha256 }`, or `std::build::local{ .path }` | required |
| `.path` | Where it lives in the project, relative to the root (`"vendor/glfw"`) | `"build/packages/<name>"` |
| `.name` | The package's name in messages, the lock file and commands | the last part of the URL or path |
| `.build` | How to build it (section 3) | `automatic` |
| `.options` | Settings for the package's build, as `(name, value)` tuples: `[("GLFW_BUILD_EXAMPLES", "OFF")]` | none |
| `.uses` | Packages and system libraries that stand in for the package's own dependencies (section 5) | none |
| `.patches` | Patch files from your project, applied after fetching (section 7) | none |

- **Everything is pinned.** A `.commit` is used as is. A `.tag` is resolved to its commit once and recorded in `build.lock` (section 6); a branch name isn't accepted, because it would make builds differ from day to day.
- **`.path` is yours to choose.** `vendor/...` puts the code in the project tree, where you can read it, step into it in a debugger, and commit it if you like. The default `build/packages/...` keeps it out of version control.
- `pkg.target("name")` is one of the package's targets, used with `g.link`, `g.set_mode` and the like. An unknown name is an error that lists the package's targets.
- `pkg.glob(pattern)` lists files inside the package, and `pkg.path` is its directory (for describing a package by hand, section 3). A description makes a target visible as a package target with `pkg.export_target(t)`.

### 2. Fetching
- `riftc build` fetches every package that is missing or at another version than `build.lock` says, then builds.
- **Local changes are never overwritten.** If files under a package's `.path` differ from the locked version, the build stops and lists them. You commit them as a patch (section 7), revert them, or switch that package to `std::build::local` while you work on it.
- Fetching happens before `build.rifts` is evaluated, so evaluation itself stays free of network access (see [Rift as a Build System](build-system.md)). The fetched files are recorded inputs like any other.
- A Git source is cloned shallowly at the locked commit. An archive is checked against its `.sha256` before it is unpacked.

### 3. How a package is built
`.build` says how `riftc` learns the package's targets. With `automatic` (the default), the first match wins:

| The package has... | `riftc` uses | `.build` to force it |
|---|---|---|
| a `build.rifts` | it, like a project of its own, in the same mode | `std::build::package_build::rift` |
| a description in your project (`packages/<name>.rifts`, below) | that description | `std::build::package_build::script("packages/glfw.rifts")` |
| a `CMakeLists.txt` | the **CMake reader** (section 4) | `std::build::package_build::cmake` |
| only headers | one header-only target named after the package; its include directory is `include/` if it exists, else the root | `std::build::package_build::headers_only("include")` |

If nothing matches, or the CMake reader meets something it can't translate, the error says which file and line, and how to continue: write a description, or run the converter (section 4).

**A package description in your project** is a Rift file that describes a package which has no `build.rifts` of its own:
```rift
// packages/stb.rifts
namespace build::packages::stb {
    compile_time void describe(mut std::build::graph& g, std::build::package& p) {
        std::build::target t = g.add_c_library(std::build::c_target{ .name = "stb_image", .language = std::build::language::c });
        g.add_sources(t, p.glob("stb_image.c"));
        g.add_include_dirs(t, [p.path], std::build::visibility::shared);
        p.export_target(t);
    }
}
```
Descriptions live in your repository, next to `build.rifts`, so they're versioned with your code and never touch the vendored files.

### 4. CMake packages
`riftc` reads a package's `CMakeLists.txt` **itself**. It doesn't run CMake: it evaluates the script to find the targets, then builds those targets with Rift's build system. The result behaves like any other target: it follows the build mode, cross-compiles with the project, appears in `compile_commands.json` and is cached like your own code.

**What the CMake reader understands:**

| CMake | Becomes |
|---|---|
| `project`, `cmake_minimum_required`, variables (`set`, `unset`, `list`, `string`, `math`), `if` / `elseif` / `else`, `foreach`, `while`, `function`, `macro`, `include`, `add_subdirectory` | Evaluated as CMake would |
| `option(NAME ...)`, cache variables | Defaults, overridden by the package's `.options` |
| `add_library` (`STATIC`, `SHARED`, `INTERFACE`, `OBJECT`), `add_executable`, `add_library(... ALIAS ...)` | Targets of the package, under their CMake names (`glfw`, also `glm::glm` for an alias) |
| `target_sources`, `target_include_directories`, `target_compile_definitions`, `target_compile_options`, `target_compile_features`, `target_link_libraries` | Sources, include directories, defines, options, language standard and links of the target |
| `PUBLIC` / `PRIVATE` / `INTERFACE` | `visibility::shared` / `own` / `users` (see [Rift as a Build System](build-system.md)) |
| `file(GLOB ...)`, `file(GLOB_RECURSE ...)` | `p.glob` |
| `configure_file`, `add_custom_command` with declared outputs | Build steps with inputs and outputs |
| `find_package(X)`, `find_library`, `pkg_check_modules` | A package or system library from `.uses` (section 5), else a system library looked up on the build machine |
| `check_include_file`, `check_symbol_exists`, `check_c_source_compiles` and similar checks | Answered from the target platform: compiled once per platform and cached |
| `CMAKE_BUILD_TYPE`, `CMAKE_SYSTEM_NAME`, `CMAKE_C_COMPILER_ID`, `WIN32` / `UNIX` / `APPLE`, ... | Set from the build mode and target platform (`debug` → `Debug`, `release` → `Release`, `release_with_debug_info` → `RelWithDebInfo`; a user mode uses its base's name) |
| Generator expressions: `$<BUILD_INTERFACE:...>`, `$<INSTALL_INTERFACE:...>`, `$<CONFIG:...>`, `$<TARGET_FILE:...>`, `$<COMPILE_LANGUAGE:...>`, `$<BOOL:...>`, `$<AND/OR/NOT:...>` | Evaluated |
| `install`, `export`, `enable_testing`, `add_test`, CPack, `message` | Ignored (`message(FATAL_ERROR ...)` stops with that message) |

Anything else (`execute_process`, `ExternalProject_Add`, `FetchContent`, `try_run`, custom properties the build depends on) stops the reader with the command, its file and line, and the two ways forward below. The reader never guesses.

**The way forward when the reader can't continue: convert the package.**
```
riftc package convert glfw
```
The converter runs the same reader, but instead of building, it **writes a package description**, `packages/glfw.rifts`, plus a report, `packages/glfw.convert.md`:
- every target with its sources, include directories, defines, options and links, written as ordinary build-API calls;
- platform-dependent parts (`if (WIN32)`, feature checks) as `if` statements on `g.target_platform()`, so one description serves every platform;
- every command that couldn't be translated as a `// TODO(convert):` comment at the place it belongs, with the original CMake lines quoted;
- the report lists each CMake file read, each command and what it became, each `TODO` with an explanation, and the options used.

You finish the `TODO`s, and from then on the package is built from `packages/glfw.rifts` (it matches row 2 of section 3). When the package is updated, `riftc package convert glfw --compare` converts the new version and shows the difference against your description, so you see exactly what upstream changed.

**Running real CMake as a last resort:** `.build = std::build::package_build::cmake_external{ .outputs = ["lib/libfoo.a"], .include_dirs = ["include"] }` runs CMake as a build step and uses the files it declares. The step is verified like every step (see [Rift as a Build System](build-system.md)), but `riftc` can't see inside it: no per-file rebuilds, no `compile_commands.json` entries, and the package's own compiler settings.

### 5. A package's own dependencies
Many packages look for others (`find_package(Freetype)`, `add_subdirectory(deps/zlib)`). `.uses` connects those to your packages, so every library exists once in the program:
```rift
std::build::package zlib = g.package(std::build::package_spec{
    .source = std::build::git{ .url = "https://github.com/madler/zlib", .tag = "v1.3.1" },
    .path   = "vendor/zlib",
});
std::build::package freetype = g.package(std::build::package_spec{
    .source = std::build::git{ .url = "https://gitlab.freedesktop.org/freetype/freetype", .tag = "VER-2-13-3" },
    .path   = "vendor/freetype",
    .uses   = [("ZLIB", zlib.target("zlibstatic"))],    // find_package(ZLIB) inside freetype → our zlib
});
```
- A `find_package` that `.uses` doesn't answer becomes a system library looked up on the build machine, recorded as an input. With `--hermetic`, that is an error instead, so a build depends only on what the project declares.
- A package's own `build.rifts` that declares packages gets them as usual; two packages that pin **different** versions of the same source are an error that names both, until one of them is given the other through `.uses`.

### 6. The lock file
`build.lock` records, per package: its name, source, resolved commit (or archive hash), the content hash of the fetched files, and how it was built (`rift`, `script`, `cmake` with the reader's version, `headers_only`, `cmake_external`). It belongs in version control.
- A content hash that doesn't match on another machine is an error: the source changed under the same commit or tag.
- Changing `.source` in `build.rifts` updates the lock at the next build and says so.

### 7. Patches
Small changes to a package's own files are kept as patch files in your project:
```rift
.patches = ["packages/glfw-wayland-fix.patch"],
```
They're applied after fetching, in order, and their hashes are part of the lock. A patch that no longer applies after an update is an error naming the file.

### 8. Commands
```
riftc package list                    # every package: version, path, how it's built, local changes
riftc package fetch                   # fetch without building
riftc package update glfw --tag 3.5   # change the version: edits build.rifts and build.lock
riftc package convert glfw [--compare]
riftc package status                  # packages whose files differ from the lock
```

## Example: an engine with a Rift, a CMake and a header-only package
```rift
// build.rifts
import std::build;

namespace build {
    compile_time void build(mut std::build::graph& g) {
        std::build::package glfw = g.package(std::build::package_spec{
            .source  = std::build::git{ .url = "https://github.com/glfw/glfw", .tag = "3.4" },
            .path    = "vendor/glfw",
            .options = [("GLFW_BUILD_EXAMPLES", "OFF"), ("GLFW_BUILD_TESTS", "OFF"), ("GLFW_BUILD_DOCS", "OFF")],
        });
        std::build::package glm = g.package(std::build::package_spec{
            .source = std::build::git{ .url = "https://github.com/g-truc/glm", .tag = "1.0.1" },
            .path   = "vendor/glm",
        });
        std::build::package mathlib = g.package(std::build::package_spec{
            .source = std::build::git{ .url = "https://github.com/example/rift-mathlib", .commit = "4e1f9c2a0b7d3e8f5a6c1b2d9e0f7a3c4b5d6e8f" },
            .path   = "vendor/mathlib",                         // has its own build.rifts
        });

        std::build::target app = g.add_executable(std::build::rift_target{ .name = "engine", .root = "app/main.rifts" });
        g.link(app, glfw.target("glfw"));                       // from CMakeLists.txt
        g.link(app, glm.target("glm::glm"));                    // a CMake alias
        g.link(app, mathlib.target("mathlib"));                 // a Rift library
        g.set_mode(glfw.target("glfw"), std::build::mode::release);
    }
}
```

## Rationale
- **Location, version, link: nothing else needed.** The common case is one `g.package` call and one `g.link`. Everything else (options, patches, `.uses`) is there for the packages that need it.
- **Read CMake, don't run it:** running CMake would put a second build system, with its own compiler choices and caches, inside every build. Reading the script gives Rift the targets, so packages follow the project's mode and platform and are rebuilt incrementally like your own code.
- **Never guess:** CMake is a full programming language, and a reader that silently skipped a command would build something subtly different. Stopping with the exact command and a way forward keeps the result trustworthy.
- **Conversion produces code you own:** the generated description is ordinary Rift, versioned with your project, readable and editable. `--compare` makes updates a reviewable diff instead of a surprise.
- **One copy of every library:** `.uses` and the version-conflict error keep two packages from each bringing their own zlib.
- **Pinned, locked and hashed:** the same commit of your project always builds the same code, on every machine.
