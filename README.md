# Rift

The Rift programming language - C++-inspired, but explicitly simpler,
safer, and more consistent. The language design lives in [`docs/design/`](docs/design/README.md).

**Status: v0.1.0** - everything in [`docs/design/`](docs/design/README.md) except the SIMD sketch
(spec 21) is implemented. `riftc build` turns a program into a native executable through LLVM (no C
code is generated; the system's `cc` only links). Supported: checked arithmetic, structs and
classes with destructors and operators, inheritance with virtual calls, interfaces (references,
smart pointers to them, `cast::dynamic`), enums with payloads and `match`, templates with
constraints, lambdas and `std::function`, ownership and borrow checks, raw pointers and
`extern(std::build::language::c)`, compile-time code, checks (spec 23) and reflection (spec 26).
The standard library (`library/std`) is written in Rift on the `std::raw` intrinsics:
`option`/`result`, `darray`, `array`, `span`, `bitset`, `hash_map`, `tree_map`, `string` and the
`str` methods, smart pointers, `file`, `filesystem`, `env`, and `std::build` for build scripts.

**Projects** are built by a `build.rifts` (spec 25): targets (Rift programs, libraries and tests,
C/C++ libraries, system libraries), build modes as classes (spec 24), options, settings, custom
steps, and path or git dependencies pinned in `build.lock`:

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

Known limits of 0.1.0: no cross compiling (`--target=`), steps run one after the other,
`build.rifts` is evaluated on every run, debug info is line tables only, and
`link_time_optimization()` has no effect yet.

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
riftc 0.1.0
$ ./build/compiler/riftc run examples/hello.rifts           # build in a temp dir and run
$ ./build/compiler/riftc build examples/hello.rifts         # -> examples/build/debug/hello
$ ./build/compiler/riftc build --mode=release examples/hello.rifts   # -> examples/build/release/hello
$ ./build/compiler/riftc emit-llvm examples/hello.rifts      # the generated LLVM IR
$ ./build/compiler/riftc check examples/employees.rifts     # silent + exit 0 when clean
$ ./build/compiler/riftc lex examples/compile_check.rifts   # token dump
$ ./build/compiler/riftc parse examples/employees.rifts     # syntax tree (S-expressions)
$ ./build/compiler/riftc symbols examples/employees.rifts   # merged module symbol table
$ ./build/compiler/riftc typed examples/employees.rifts     # syntax tree with every expression's type
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
| `examples/` | Example `.rifts` programs used in the docs and tests |
| `example/` | Showcase programs (`--target examples`) |
| `bench/` | Standard library benchmarks against C++ STL / Abseil / Boost |
| `docs/design/` | Language specification (v0.7) |
