# Rift

The Rift programming language - C++-inspired, but explicitly simpler,
safer, and more consistent. The language design lives in [`docs/design/`](docs/design/README.md).

**Status: v0.8.0** - the language and standard library of [`docs/design/`](docs/design/README.md),
with the gaps listed under "Known limits" below. `riftc build` turns a program into a native
executable through LLVM (no C code is generated; the system's `cc` only links). Supported: checked
arithmetic, structs and classes with destructors, operators and nested types, inheritance with
virtual calls, interfaces with default methods, enums with payloads and `match`, tuples and list
literals, templates with constraints and default arguments, lambdas, `std::function` and
`std::fn_ptr`, ownership and borrow checks, raw pointers and `extern(std::build::language::c)`,
compile-time code, `compile_time` and `implicit` parameters, checks, reflection and registries
(`std::meta::functions_with`), annotations (checks, `generate`, `wrap`), and SIMD vector types
(`f32x8`, masks, blocks). The standard library (`library/std`) is written in Rift on the `std::raw`
intrinsics: `option`/`result`, `darray`, `array`, `span`, `bitset`, `hash_map`, `tree_map`,
`string` and the `str` methods, smart pointers, threads, synchronization, channels, the thread pool
and `std::parallel`, `std::time`, `std::log`, `file`, `filesystem`, `env`, `std::windowing`
(several windows with OpenGL share groups and Vulkan surfaces, input layers and key bindings, on
X11; see `example/windowing/`), and `std::build` for build scripts.

**Projects** are built by a `build.rifts`: everything in it is declared in `namespace build` and
every target imports it. It describes targets (Rift programs, libraries and tests, C/C++ libraries,
system libraries), build modes as classes with `compile_time` settings, declared settings
(`compile_time std::build::setting<bool> validation = default;`, read in code as
`build::validation.value()`), options, custom steps, and path or git dependencies pinned in
`build.lock`:

```bash
riftc build                      # every program of ./build.rifts, in debug
riftc run app --mode=release     # build and run one target
riftc test                       # build and run the tests
riftc build --list               # targets, modes and options
```

Example programs for most features are in [`example/`](example/README.md):
`cmake --build build --target examples` builds them into `build/example/bin`.

The standard library is built for speed from the start: a SwissTable `hash_map`, a B-tree
`tree_map`, small-string-optimized `string`, introsort and a stable merge sort. `bench/`
compares it with libstdc++, Abseil and Boost (`python3 bench/run.py`, see
[`bench/README.md`](bench/README.md)); it is within ~1.3x of the fastest C++ container on every
benchmark and ahead on several.

Threads (spec 27, 28): `std::thread::spawn`, `std::atomic`, `std::mutex`, `std::condition`,
channels, `thread_local` and `std::thread_pool` (work stealing, parallel loops over chunks,
deterministic reductions). **Data races are compile errors:** the compiler derives
`std::sendable` / `std::shareable` from a type's fields, and a thread that borrows keeps what it
borrows borrowed until it is joined. See `example/concurrency.rifts`.

Also in 0.8.0: **packages** (`g.package` with git (commit or tag), archive and local sources,
`.path`, `.options`, `.uses`, `.patches`, locked in `build.lock`; packages with a `build.rifts`,
a `CMakeLists.txt` read by riftc's own CMake reader, or only headers; `riftc package
list|fetch|status`), **C and C++ interop** (`g.import_c_headers` turns C headers into a Rift module,
`g.wrap_cpp` wraps C++ classes through generated C wrappers with exceptions as
`std::cpp_exception` errors, structs passed by value to and from C as the C calling convention
does on x86-64 and AArch64), shared C libraries and Rift plugins (`g.add_shared_library` of a C
target, `g.add_plugin`, `std::plugin<I>`, `std::c_library`), allocators (`std::allocator`, arenas,
`g.set_global_allocator`, a sealable heap), target platforms and CPU levels (`--target`, `--cpu`,
target descriptions), step verification, parameter packs, file annotation checks, SIMD
multiversioning, `must_vectorize`, raw SIMD intrinsics and `riftc simd-report`, `std::rw_mutex`
and the timed waits, thread platform options, time zones, and wrapping annotations on templates.

Known limits of 0.8.0 (designed, not implemented yet):
- Shared **Rift** libraries (`g.add_shared_library(std::build::rift_target{...})`); a shared C or
  C++ library works.
- Package descriptions in the project (`packages/<name>.rifts`, `package_build::script`), the
  CMake converter (`riftc package convert`), `riftc package update`, and
  `package_build::cmake_external`. The CMake reader stops, with the command's file and line, at
  what it doesn't translate (`execute_process`, `ExternalProject_Add`, `FetchContent`, ...).
- An allocator parameter for `std::tree_map` and `std::string_with<A>`.
- `g.wrap_cpp` reads the headers with clang (`clang++`, or `$RIFT_CLANGXX`); it wraps numbers,
  `bool`, strings (`std::string`, `std::string_view`, `const char*`) and the wrapped classes
  themselves, and lists everything else in its report.
- Structs by value through `extern` functions are supported on x86-64 (System V) and AArch64; other
  targets report it.

## Building

Requires:

- CMake ≥ 3.25
- Ninja (recommended) or Make
- Clang 18+ or GCC 14+, with `-std=c++2c` support
- LLVM 18, 19, 20 or 21 development files (e.g. `llvm-20-dev`, plus `libzstd-dev` for its CMake exports); the backend emits LLVM IR. CMake takes the first of 18, 20, 21, 19 it finds; `-DRIFT_LLVM_VERSION=21` or `-DLLVM_DIR=...` picks one
- Python 3.10+ (test runner only)
- To link Rift programs: a system linker driver such as GCC or Clang as `cc` (or set `RIFT_CC` / `--cc=`)

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

The binary is `build/compiler/riftc`:

```bash
$ ./build/compiler/riftc --version
riftc 0.8.0
$ ./build/compiler/riftc run example/hello.rifts           # build in a temp dir and run
$ ./build/compiler/riftc build example/hello.rifts         # -> example/build/debug/hello
$ ./build/compiler/riftc build --mode=release example/hello.rifts   # -> example/build/release/hello
$ ./build/compiler/riftc emit-llvm example/hello.rifts      # the generated LLVM IR
$ ./build/compiler/riftc check example/zoo.rifts     # silent + exit 0 when clean
$ ./build/compiler/riftc lex example/hello.rifts   # token dump
$ ./build/compiler/riftc parse example/zoo.rifts     # syntax tree (S-expressions)
$ ./build/compiler/riftc symbols example/zoo.rifts   # merged module symbol table
$ ./build/compiler/riftc typed example/zoo.rifts     # syntax tree with every expression's type
$ ./build/compiler/riftc check --root=proj proj/main.rifts  # project root for imports
$ ./build/compiler/riftc explain E1003                      # what a diagnostic code means
```

## Testing

```bash
scripts/test.sh                 # build + run unit, feature and action tests
scripts/test.sh --suite feature # one suite
scripts/test.sh lexer           # filter by name/path
scripts/test.sh --update        # re-record golden files for action tests
```

| Suite | What | Where |
|---|---|---|
| unit | C++ tests of compiler internals | `tests/unit/test_*.cpp` |
| feature | good/bad `.rifts` files per spec rule, inline `//~ error[E1009]` annotations | `tests/feature/<chapter>/` |
| action | real `riftc` runs; stdout, stderr and exit code compared with golden files (`tests/action/run/` runs compiled programs) | `tests/action/**/*.rifts` + `.expected` |

Details, file formats and the `XFAIL` workflow for not-yet-implemented features:
[`docs/testing.md`](docs/testing.md). CTest runs the same suites:
`ctest --test-dir build --output-on-failure`.

## Layout

| Path | Purpose |
|---|---|
| `compiler/include/rift/` | Public headers of `librift` (source map, diagnostics, tokens, lexer, AST, parser, driver) |
| `compiler/include/rift/ast.hpp` | The syntax tree; `ast_printer.hpp` prints it for `riftc parse` |
| `compiler/include/rift/symbols.hpp` | Modules and symbols; `modules.hpp` loads imports, `resolve.hpp` resolves names |
| `compiler/include/rift/types.hpp` | Types; `check.hpp` is the type checker, `ownership.hpp` the ownership checker |
| `compiler/include/rift/compile_time.hpp` | The compile-time evaluator (constants, `if`/`for compile_time`, `std::meta`); `ast_clone.hpp` copies loop bodies |
| `compiler/include/rift/llvm_backend.hpp` | The LLVM backend: checked program to LLVM IR, object file, executable; the runtime is LLVM IR in `src/llvm_runtime.cpp` |
| `compiler/include/rift/diagnostic_codes.def` | Every diagnostic code, in one table |
| `compiler/include/rift/token_kinds.def` | Every token kind, keyword and operator |
| `compiler/src/` | `librift` implementation |
| `library/std/` | The standard library's Rift source, compiled into `riftc` and loaded by `import std;` |
| `compiler/driver/main.cpp` | The `riftc` executable (thin wrapper around `rift::run_driver`) |
| `tests/run_tests.py` | Test runner for all three suites |
| `tests/unit/`, `tests/feature/`, `tests/action/` | The test suites |
| `scripts/test.sh` | Build + test in one command |
| `example/` | Showcase programs (`--target examples`) |
| `bench/` | Standard library benchmarks against C++ STL / Abseil / Boost |
| `docs/design/` | Language and standard library design (`language/`, `stdlib/`, `build/`) |
