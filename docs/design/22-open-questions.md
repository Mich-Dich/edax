# 22 - Open Questions

Ordered roughly by how much they affect the rest of the design.

| # | Topic | Current state | Notes |
|---|---|---|---|
| 1 | **Lifetimes beyond second-class references** | Reference elision rule only (see 12) | Is "borrow from `this` or the only parameter" enough? If not: explicit lifetime parameters, or more `std::shared_ptr` use |
| 2 | **Concurrency** | **Designed** (see 27): threads, `std::sendable`/`std::shareable`, atomics, mutex, condition, channels | |
| 3 | **`shared_ptr` reference count** | **Decided:** always atomic (see 13, 27) | |
| 4 | **Consuming methods** | **Decided:** trailing `move` (see 06, 27) | The option/result built-ins can become ordinary `move` methods |
| 5 | **Panic strategy** | Abort, or trap via build mode (see 24) | A user-defined panic handler for embedded and safety-critical targets |
| 6 | **Interface default methods** | Not allowed | Useful, but adds rules about which implementation wins |
| 7 | **Variadic templates** | Not available | Needed for `std::function<R(Args...)>`, which currently relies on compiler magic |
| 8 | **Tuples** | Removed | Structs are the one way. Revisit if multi-value returns get too verbose |
| 9 | **Self-referential types** | Impossible (bitwise moves) | A "pinned" concept, if ever needed |
| 10 | **Allocators** | Global allocator only | Per-container allocators (`std::darray<T, A>`) matter for embedded use |
| 11 | **Compile-time heap** | Allowed, stored in static memory (see 15) | Exact rules for which types support this |
| 12 | **Module aliases** | Not available | `import std::filesystem as fs;` would be short *and* explicit. Does it break "always fully qualified"? |
| 13 | **Attributes** | Not designed | v0.2 *Future thoughts* used `[required]`, which clashes with lambda syntax `[...]`. Proposal: `@required` |
| 14 | **Types as values / reflection** | **Designed** (see 26): `type` values, `std::meta`, `for compile_time` | `.type = std::filesystem::path` from *Future thoughts* is now a `type` value |
| 27 | **Generating declarations** | Not available (see 26) | Compile-time code that creates types, fields or functions (`std::meta::struct_builder`, like Zig). Needs rules for names, headers and error locations |
| 28 | **Derive** | Not available | `@derive(std::formattable)` implementing an interface from the fields (needs attributes, #13). With 26 the same code can already be written as a generic function |
| 30 | **Aliases** | Not available: every name is written in full (`std::build::mode::debug`) | A user-chosen short name for a namespace or type (never an implicit one). Syntax and scope (module-local only?) open |
| 31 | **Names from `build.rifts` in code** | A user mode class is named `build::profile` in mode lists (see 23, 24) | How code reaches declarations of `build.rifts` in general (settings, mode classes) |
| 29 | **Reflecting methods and template arguments** | Not available | `std::meta::methods<T>()`, `std::meta::template_args<T>()`, calling a method through reflection |
| 15 | **Language as build system** | **Designed** (see 25) | |
| 16 | **SIMD** | Sketch (see 21) | |
| 18 | **`compile_time` parameters** | Covered by `compile_time check` entry checks (see 23) | A parameter modifier would only be needed if the *body* must use the value at compile time (e.g. to pick a type) |
| 19 | **In-source warning control** | Command line only (`--warn=id=off`) | Needs attributes (#13), e.g. `@allow(gfx::deprecated)` on a statement or function |
| 20 | **Fix-it suggestions** | Not available | `std::compile_fixit(location, replacement)` so tools can apply the author's suggested fix |
| 21 | **Postconditions** | Entry checks are preconditions (see 23); debug-only checks are `run_time(std::build::mode::debug) check` | Checks on return values, e.g. a `check` that sees the function's result? |
| 22 | **Implicit `std::static_str` → `std::filesystem::path`** | Avoided: `std::file::open`/`create` have a `std::str` overload (19); build targets name files as `std::static_str` (25) | Whether paths should get a literal form or an implicit conversion after all |
| 23 | **List literals** | Not available | `[a, b, c]` would shorten build scripts and container setup. Clashes with lambda captures `[x](...)` unless resolved by the token after `]` |
| 24 | **C/C++ headers → Rift headers** | Hand-written `extern(std::build::language::c)` `.rifth` | A build step that generates `.rifth` from C headers (like `bindgen`). C++ classes are much harder than C |
| 25 | **Build sandboxing and remote cache** | Steps declare inputs/outputs (see 25) | Enforce declarations with a sandbox; share build outputs between machines and CI |
| 26 | **Package registry** | Git and path dependencies only | A central registry with versions instead of commits? |
| 32 | **Async / tasks** | Not available | Lightweight tasks or `async` functions on top of threads; needs an executor design |
| 33 | **Thread pools and parallel algorithms** | **Designed** (see 28): `std::thread_pool`, `for_chunks`, `reduce_chunks` | Parallel `sort` and other algorithms on top of the pool |
| 34 | **Real-time threads** | Not available | Priority, CPU affinity and scheduling policy in `std::thread::options` (platform-specific) |
| 35 | **Targets without threads** | Not designed | Bare-metal targets: `std::thread::spawn` as a compile error there (a `compile_time check` on the target platform), atomics still available |
| 36 | **Timeouts** | Not available | `try_lock_for`, `wait_for`, `receive_for` with a `std::time::duration` |
| 37 | **Read-write lock** | Not available | `std::rw_mutex<T>` with `read()` / `write()` guards |
| 39 | **Nested type declarations** | Not available | The library names three types as members of `std::thread_pool` (`options`, `task<R>`, `scoped_task<R>`); they are declared as `thread_pool_options` etc. and the compiler maps the member names. User code can't nest types yet |
| 40 | **Deducing from a lambda's result** | Only literals | `reduce_chunks(data, n, 0.0, map, combine)`: a template parameter given only by an untyped literal takes the result type of a lambda argument that fits the literal. A general rule (deducing through `std::meta::result_of`) is open |
| 38 | **Splitting a mutable span in two** | Only `chunks(n)` (equal parts) | `split_at(i)` must return two `std::span<mut T>` at once. Needs tuples or destructuring (q8), or a consuming method returning a pair struct whose fields can be moved out |
| 17 | **Name** | `Rift` (working name) | |
