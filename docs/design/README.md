# Rift - Language Design (v0.12)

> **Status:** Draft v0.12. This folder replaces `docs/design_doc.md` (v0.2).
> **Lineage:** C++-inspired. Explicit, simple to use, high control.
> **Files:** `.rifts` = source, `.rifth` = header.

Each file below describes **one concept** and can be read on its own. Every file has the same layout: **Rules**, **Examples**, **Rationale**. Cross-references use the file number, e.g. *(see 12)*.

## Files

| # | File | Concept |
|---|---|---|
| 01 | [01-lexical.md](01-lexical.md) | Files, comments, literals, keywords, `;` and braces |
| 02 | [02-types.md](02-types.md) | Built-in types, literal typing, bool conversion, every implicit conversion |
| 03 | [03-variables.md](03-variables.md) | Declarations, `=`, `mut`, `auto`, `default`, file scope |
| 04 | [04-functions.md](04-functions.md) | Functions, parameter passing, call-site `mut`, `main` |
| 05 | [05-structs.md](05-structs.md) | `struct`: plain data with methods |
| 06 | [06-classes.md](06-classes.md) | `class`: encapsulation, `mut` methods, destructors |
| 07 | [07-inheritance-interfaces.md](07-inheritance-interfaces.md) | `open`, `virtual`, `override`, `abstract`, `interface` |
| 08 | [08-enums.md](08-enums.md) | Simple enums and enums with payloads |
| 09 | [09-headers-modules.md](09-headers-modules.md) | `.rifth` / `.rifts` merge, `import`, `private` |
| 10 | [10-control-flow.md](10-control-flow.md) | `if`, `while`, `for`, `match`, block values, `never` |
| 11 | [11-error-handling.md](11-error-handling.md) | `std::result`, `std::option`, `?`, panics |
| 12 | [12-ownership-references.md](12-ownership-references.md) | Copy, `move`, `clone`, references, view types, destruction |
| 13 | [13-pointers-unsafe.md](13-pointers-unsafe.md) | `std::raw_ptr`, `std::unique_ptr`, `std::shared_ptr`, `nullptr`, `unsafe`, FFI |
| 14 | [14-templates.md](14-templates.md) | `template<...>`, constraints, compilation model |
| 15 | [15-compile-time.md](15-compile-time.md) | `compile_time`, `run_time`, `deduce`, `if compile_time` |
| 16 | [16-operators.md](16-operators.md) | Precedence, overflow, bit operations, `bit(n)`, overloading |
| 17 | [17-casts.md](17-casts.md) | `cast<T>` and its explicit variants |
| 18 | [18-lambdas.md](18-lambdas.md) | Lambdas and captures |
| 19 | [19-strings-formatting.md](19-strings-formatting.md) | `std::str`, `std::string`, literals, `f"..."` |
| 20 | [20-standard-library.md](20-standard-library.md) | Naming rules and type catalogue |
| 21 | [21-simd.md](21-simd.md) | SIMD sketch (deferred) |
| 22 | [22-open-questions.md](22-open-questions.md) | Everything not decided yet |
| 23 | [23-compile-checks.md](23-compile-checks.md) | Checks: `compile_time check`, `run_time check`, mode-gated checks, custom compiler messages |
| 24 | [24-build-modes.md](24-build-modes.md) | `debug`, `release`, `release_with_debug_info` and user-defined modes |
| 25 | [25-build-system.md](25-build-system.md) | `build.rifts`: Rift as the build system, including C/C++ |
| 26 | [26-metaprogramming.md](26-metaprogramming.md) | Types as values, reflection with `std::meta`, `for compile_time` |
| 27 | [27-threads.md](27-threads.md) | Threads, `std::sendable`/`std::shareable`, atomics, mutex, condition, channels |
| 28 | [28-thread-pool.md](28-thread-pool.md) | `std::thread_pool`: tasks, parallel loops, deterministic reductions |
| 29 | [29-annotations.md](29-annotations.md) | `@annotations`: data, checks and generated members on declarations |
| 30 | [30-namespaces.md](30-namespaces.md) | `namespace`, name lookup without hijacking, aliases |
| 31 | [31-logging.md](31-logging.md) | `std::log`, `implicit` and `compile_time` parameters, `std::call_site`, lazy f-strings, file annotations |

A complete program written to these rules is in [`examples/employees.rifts`](../../examples/employees.rifts).

## Design principles

1. **Explicit beats implicit.** If a value's type, cost or behaviour can't be read from the source, the compiler refuses to guess. The *complete* list of implicit conversions fits in one table (see 02).
2. **One way to do each thing.** One way to initialize (`=`), one way to build a value (`T{...}`), one way to define a method body.
3. **Safe by default.** Safe code cannot trigger undefined behaviour. Overflow, out-of-bounds access and bad shifts panic. Unsafe operations live inside a visible `unsafe { }` block. All built-in build modes are safe; a user mode can relax this only by saying so in `build.rifts` (see 24).
4. **Immutable by default.** Variables, parameters and `this` are immutable unless marked `mut`.
5. **No uninitialized values.** Every variable has a value from its declaration on.
6. **Composition and inheritance, both explicit.** Inheritance requires `open` on the base class.
7. **Nothing costs more than it looks.** Allocation, copies of owning types, moves and dynamic dispatch are always visible in the code.
8. **Control when you ask for it.** Every safe default has a named escape hatch: `+%` wrapping arithmetic, `cast::truncate`, `std::raw_ptr`, `unsafe`.
9. **Fast, legible compilation.** Each error fits on one screen.

## Changes from v0.11

| Change | Reason |
|---|---|
| New: `f(...) else { ... }` after a call that returns `bool` (see 10) | Checks that report a failure read as one statement; the recovery is a block, not a macro argument |
| New: `std::log::validate` and `std::log::assert` (see 31) | Logged checks the program survives, distinct from the language's `run_time check` |
| `std::log::fatal` returns instead of panicking (see 31) | A program may reset or retry after a fatal error |
| New: per-level colors (`std::log::palette`, per sink) and ready-made patterns `std::log::patterns::*` (see 31) | Configurable output without writing patterns by hand |

## Changes from v0.10

| Change | Reason |
|---|---|
| New: `std::log` (see 31): compile-time levels per function, type, file and target, sinks owned by one log thread, settings in message order, a panic hook | A logger without macros, in the standard library |
| New: `implicit` parameters and `std::implicit<T>`; `std::call_site` (see 31, 04) | A call can carry where it is written, through any type, without default arguments |
| New: `compile_time` parameters (see 31, 15) | A body can decide at compile time from an argument; closes open question 18 |
| f-strings passed to a function are evaluated only if formatted; `std::format_args` is move-only (see 19) | Removed or filtered log calls cost nothing |
| File annotations `@a{ ... };` and target `std::meta::target::file` (see 29) | A level (or any setting) stated at the top of a file |
| Per-file build settings: `g.set_for_files`, `std::build::setting_at` (see 25) | Configure groups of files from build.rifts |
| `std::meta::function_annotation` / `type_annotation` / `file_annotation`, `std::literal_text`, compile-time `slice`/`find` (see 26, 23, 15) | General tools `std::log` is built from |

## Changes from v0.9

| Change | Reason |
|---|---|
| New: annotations `@name{ ... }`, declared as `annotation(targets) struct` (see 29) | Attach data to declarations, check them, generate methods |
| An annotation's `generate` block adds methods and interfaces to the annotated type | Derive-style code (`@std::derive::formattable`, `hashable`, `ordered`) without macros; answers open questions 13, 27 (partly), 28 |
| Standard annotations: `@std::deprecated`, `@std::allow`, `@std::must_use`, `@std::inline_always/never`, `@std::cold`, `@std::test` | One syntax for what would otherwise need several special forms |
| Reflection of methods (`std::meta::methods`, `m.call(...)`), interfaces, template arguments, annotations; `std::meta::name_of` / `from_name` (see 26) | Metaprogramming that covers command tables, serializers and validators |
| New: C++-style namespaces `namespace a::b { ... }`, separate from modules (see 30, 09) | Names are organized by namespace; files can move without renaming |
| No argument-dependent lookup, no `using`; an unqualified name found at two levels is an ambiguity error | Adding a declaration can cause an error, never silently change what code calls |
| Operators only with their types; interfaces only in the type's declaration; template bodies resolve names where they're written | Behaviour belongs to the type; no injection from other modules |
| Top-level namespaces belong to one project; nothing can be added to `std` | No library can change another library's behaviour |
| File-local namespace aliases `namespace vk = gfx::vulkan;` | Short names without `using`; closes open questions 12 and 30 |

## Changes from v0.8

| Change | Reason |
|---|---|
| Channels: `std::channel<T>` is the channel; it makes the ends (`sender()`, `receiver()`) and closes the channel (`close()`) (see 27) | Creating "a receiver" to get a channel read wrong; closing is now something you do, and doesn't race with thread start-up |
| `try_send` / `try_receive` that never wait | Polling loops and real-time code must not block |
| New: `std::thread_pool` with `spawn`, `for_chunks`, `for_index_chunks`, `reduce_chunks` (see 28) | Short tasks shouldn't pay for an OS thread each |
| Performance guarantees in the spec: lock-free atomics or a compile error, inline one-word mutex, no allocation per message on bounded channels, no allocation in parallel loops, no init check on `thread_local` (see 27, 28) | The design aims for the best possible performance; these are promises, not hopes |
| `compare_exchange_weak`, `std::cache_aligned<T>`, `std::cache_line_size`, `std::thread::spin_hint()` | Control over the costs that matter in lock-free and parallel code |
| `reduce_chunks` is deterministic: same result for any number of threads | Reproducible numerical results |

## Changes from v0.7

| Change | Reason |
|---|---|
| New: threads in the standard library, `std::thread::spawn` (see 27) | Concurrency was open question 2 |
| Data races are compile errors: derived interfaces `std::sendable` / `std::shareable`; a borrowing thread is a view and keeps its captures borrowed until it's joined | Built from the existing reference and view rules, with no lifetime syntax |
| One `spawn`; it returns `std::thread::handle<R>` (owns its captures) or `std::thread::scoped_handle<R>` (borrows) | The captures decide, and the handle's type shows it |
| Synchronized types (`std::atomic`, `std::mutex`, `std::condition`, channels) are the only types that change through `T&` | Keeps "`T&` means not changed" true everywhere else |
| Atomics name their memory order on every call; invalid orders are compile errors | No hidden `sequentially_consistent` |
| `unsafe std::sendable(cond)` in a base list promises thread safety for `std::raw_ptr` wrappers; a `std::thread_bound` field opts out | The compiler can't see through raw pointers |
| New keyword `thread_local`; `static` requires a `std::shareable` type; `static mut` is always unsafe (see 03) | Globals follow the same thread rules as everything else |
| Consuming methods: trailing `move` (see 06) | `join()`, `detach()`, `unlock()` need them; closes open question 4 |
| `void` as a template argument; a `void` payload is no payload (`std::ok()`) (see 02, 08) | `std::thread::handle<void>`, `std::result<void, T>` |
| `std::shared_ptr` always has an atomic count (see 13) | Closes open question 3; one pointer type instead of two |

## Changes from v0.6

| Change | Reason |
|---|---|
| `extern(std::build::language::c)` replaces `extern "C"`; `extern(...) { ... }` declares several functions; with a body it exports a Rift function to C (see 13) | The language is a real name (`std::build::language::c`), written in full like the mode in `run_time(std::build::mode::debug)`, not a string the compiler has to recognize |
| Mode lists use full names: `run_time(std::build::mode::debug)` (see 23) | No implicit namespaces: a bare `debug` was looked up somewhere the code never named |
| Build modes are classes: `std::build::mode::debug` / `release` are `open` classes implementing `std::build::mode::settings`; a user mode derives from one and overrides the settings it changes (see 24) | A variation of a mode is what inheritance already expresses; no `.base` field and no copied settings |
| `c"..."` C string literals (see 01, 19) | Calling C no longer needs hand-written `\0`s |
| Diagnostic ids are declared names: `compile_time std::diagnostic deprecated = default;`, used as `std::compile_warning(gfx::deprecated, ...)` (see 23) | A string id looked like a path but a typo went unnoticed |
| `open` is a keyword only before a type declaration (see 01) | `std::file::open` is a method name |
| `cast<u32>(c)` for chars, `cast::checked<char>(n)` (see 17) | Chars and integers had no conversion at all |
| `for` over containers (`len()` + `operator[]`) and streams (`next()`) (see 10); method templates (see 14); `std::raw` memory intrinsics (see 13) | What the standard library needs to be written in Rift |
| Build API details settled while implementing 24/25: `.root` names a target's main file, `std::build::visibility::own/shared` (`public`/`private` are keywords), `g.target_of(project, name)`, `g.glob_in(project, pattern)`, `std::build::local{...}` path dependencies, `g.target_platform()`; `std::file::open(std::str)` overload instead of an implicit path conversion (22, q22) | What the first implementation needed; each is the smallest change to the sketch |
| A mode's settings drive the compiler (optimization pipeline, DWARF line tables, wrap/mask/trap, run_time checks); the library's index checks follow `bounds()` (see 24) | The settings were names; now each has an effect |
| Run-time entry checks report the caller's line, then the check (see 23); interface methods implemented in an open class hierarchy are dispatched dynamically (see 07) | As the spec said; the first implementation deviated |
| `std::array`, `std::bitset`, `std::hash_map`, `std::tree_map` written in Rift; `std::raw::array<T, N>` inline storage (see 13, 20); `get(i)` returns `std::option<T&>` | The catalogue's containers exist now, with no compiler magic beyond inline storage |

## Changes from v0.4

| Change | Reason |
|---|---|
| New: build modes `debug`, `release`, `release_with_debug_info` (see 24) | The language knows build modes out of the box |
| User-defined modes with a base mode; they may change safety rules, and undefined behaviour needs `.allow_undefined_behavior = true` | Full control, but never silent |
| Mode-gated checks: `run_time(std::build::mode::debug) check`, `compile_time(std::build::mode::release) check` (see 23) | Expensive checks can be left out of a build completely |
| New: `build.rifts`, evaluated at compile time, builds Rift and C/C++ (see 25) | Rift is its own build system; no CMake needed |
| Build settings reach code as `std::build::setting<T>(...)` | Typed replacement for `-D` defines |

## Changes from v0.3

| Change | Reason |
|---|---|
| New: `compile_time check`, `run_time check`, `compile_time run_time check` (see 23) | One statement for build requirements and run-time asserts; the modifier says when it is evaluated |
| Checks at the start of a function body are **entry checks**, evaluated at every call | Preconditions that report errors at the caller's line |
| New: `std::compile_error` / `compile_warning` / `compile_note` for block-form checks | Library and template authors can report mistakes in the user's code, in their own words |
| No `std::assert` | `run_time check` replaces it |
| The relative-path warning (19) is now a library `check` on `std::file::open` | It passes the boundary test: a library can implement it |
| Checks are written only in the `.rifts`; the compiled interface carries them to importers (09) | The source is the single source of truth |

## Changes from v0.2

| Change | Reason |
|---|---|
| Spec split into one file per concept | Easier to review and extend each concept on its own |
| Extensions `.rifts` / `.rifth` | Replaces `.slate`, `.rfts` and `.h.slate` |
| `const` removed everywhere | Everything is immutable by default |
| `run_time` (not `runtime`) | Matches the spelling of `compile_time` |
| Trailing `;` after `}` of types removed | It was only there because of C's `struct x {...} var;` |
| Initialization only with `=`; `T x{...}` is banned | One way to initialize |
| Explicit stdlib names: `std::error`, `std::darray`, `std::array` | `std::err` and `std::list` were abbreviations or misleading |
| Pointers in `std::`: `raw_ptr`, `unique_ptr`, `shared_ptr`, `weak_ptr` | Library types, not keywords |
| `nullptr` replaces `null`, only for `std::raw_ptr` | Absence of a value is `std::option`; one way each |
| Struct methods are normal methods | v0.2 made them `compile_time`, so they couldn't be called on runtime data |
| Mutating methods end with `mut` | Answers the open question of how a method declares that it changes `this` |
| Template parameters are declared: `template<type T>` | v0.2 used `T` without declaring it |
| Templates are type-checked at compile time; only codegen is deduplicated at link time | `sizeof`, `compile_time` and `deduce` need instantiation at compile time |
| Contextual bool conversion for integers and `std::raw_ptr` | `if (ptr)` and `if (flags & bit(4))` are allowed |
| `bit(n)` requires a known type; `auto x = bit(4)` is an error | The type must be visible somewhere |
| `std::string s = "literal";` is a warning | It allocates silently |
| Unsuffixed literals take their type from context | `u32 x = 0;` works; `0u32` is only needed without context |
| Defined: overflow, out-of-bounds, shifts, division by zero → panic | v0.2 promised no undefined behaviour but left these open |
| `move`, `.clone()`, call-site `mut` | Copies and mutation are visible at the call site |
| References are parameter-style (second-class) | Safety without lifetime annotations |
| Lossy casts have explicit names | `cast<T>` is lossless only |
| `f"..."` marks string interpolation | Plain strings are never interpolated |
| Mandatory braces, no `++`/`--`, assignment is a statement | One way, and `if (x = 0)` can't happen |
| Parameter grouping `T& {a, b}` removed | A second way to write a parameter list |
| `std::variant` removed; enums can carry payloads | One way to write a sum type |
| `?` requires matching error types; `.or_error()` / `.map_error()` convert | Error conversion is visible |
| Method bodies are defined only inside the `class`/`struct` block in `.rifts` | One way; out-of-line `test::f()` definitions removed |
