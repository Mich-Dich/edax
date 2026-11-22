# Edax examples

Programs that show what Edax 0.8.0 can do. Each one is a single `.ed` file (plus a module or
two); `build.ed` builds them all.

```bash
cmake --build build --target examples          # all programs -> build/example/bin
build/example/bin/zoo                          # run one

# or with edaxc directly, from the repository root:
edaxc build --root=example --out-dir=build/example --bin-dir=build/example/bin
edaxc run --root=example calculator -- "2 * (3 + 4)"
edaxc run --root=example --mode=fast build_info   # a build mode defined in build.ed
edaxc test --root=example                         # the example's test program
edaxc build --root=example --list                 # targets, modes and options
```

| Program | Shows | Specs |
|---|---|---|
| `hello` | variables, functions, `while`/`for`/`match`, block values, f-strings with format specs | 03, 04, 10, 19 |
| `numbers` | integer widths up to 128 bits, checked/wrapping/saturating arithmetic, named casts, bits, floats, chars | 02, 16, 17 |
| `geometry` | a module with a header (`geometry/vec`, declaring namespace `geometry`), operators, enums with payloads, struct patterns, guards, sorting with a key | 05, 08, 09, 10, 16 |
| `zoo` | classes, `open`/`abstract`/`virtual`/`override`, interfaces, `cast::dynamic`, `unique_ptr`/`shared_ptr`/`weak_ptr`, destructors | 06, 07, 12, 13 |
| `generics` | templates with deduction, constraints, value parameters, method templates, generic enums, `if compile_time` | 14, 15 |
| `lambdas` | capture kinds, `std::function`, `std::fn_ptr`, an event bus of callbacks, `retain`/`sort_by_key` | 18 |
| `calculator` | a tokenizer and parser with errors as values (`std::result`, `?`), a `view` struct, `main(args)` | 04, 08, 11, 12 |
| `containers` | `darray`, `array`, `bitset`, `span`, `hash_map`, `tree_map`, text functions | 19, 20 |
| `ownership` | copies and moves, borrowing, `std::take`/`replace`/`swap`, destructor order, returned views | 12 |
| `gradebook` | files next to the executable, parsing a CSV, error enums, `map_error`, a report | 11, 19, 20 |
| `compile_time_demo` | compile-time functions and tables, types as values, checks, a declared warning, reflection | 15, 23, 26 |
| `ffi` | raw pointers, `unsafe`, `std::raw` memory, a C library built by `build.ed`, a Edax function called from C, libm | 13, 25 |
| `concurrency` | threads that borrow or own, channels, a mutex and an atomic, the thread pool's parallel loop and deterministic reduction, a pool task | 27, 28 |
| `annotations` | namespaces and an alias, `@std::derive::formattable/hashable/ordered`, annotations with checks, a table printer driven by `@column`, a command table from annotated methods (`std::meta::methods`, `m.call`), `std::meta::name_of`/`from_name` | 26, 29, 30 |
| `build_info` | reading the build mode and settings from `build.ed`, mode-gated checks, a user mode | 24, 25 |
| `windowing` | several windows with `std::windowing` (its own folder: `main.ed` the entry point, `application` the program): OpenGL Core 3.3 windows in one share group, an independent OpenGL 2.1 window, a Vulkan 1.3 window, tool windows drawn on demand, key bindings, mouse capture; the APIs are `std::graphics::api` values and their functions come from `std::graphics` (needs an X server) | windowing, graphics, 14, 26, 29 |

`compile_time_demo` prints a compiler warning on purpose: it shows a declared warning id
(`edaxc explain legacy`, run in this directory). Several files contain lines that are commented out
because they don't compile; their comments name the error you get.
