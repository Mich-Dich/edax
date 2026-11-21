# Rift Language Design

Rift is a C++-inspired systems language: explicit, simple to use, with full control over memory and cost. These pages describe the language and its standard library **as they are**. Each page explains one topic and can be read on its own, with **Rules**, **Examples** and **Rationale**.

- Source files are `.rifts`, header files `.rifth`.
- Undecided points are collected in [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md), not in the pages.
- A complete program: [`examples/employees.rifts`](../../examples/employees.rifts).

## Design principles

1. **Explicit beats implicit.** If a value's type, cost or behaviour can't be read from the source, the compiler refuses to guess. The *complete* list of implicit conversions fits in one table (see [Built-in types](language/basics/types.md)).
2. **One way to do each thing.** One way to initialize (`=`), one way to build a value (`T{...}`), one way to define a method body.
3. **Safe by default.** Safe code cannot trigger undefined behaviour. Overflow, out-of-bounds access and bad shifts panic. Unsafe operations live inside a visible `unsafe { }` block. All built-in build modes are safe; a user mode can relax this only by saying so in `build.rifts`.
4. **Immutable by default.** Variables, parameters and `this` are immutable unless marked `mut`.
5. **No uninitialized values.** Every variable has a value from its declaration on.
6. **Composition and inheritance, both explicit.** Inheritance requires `open` on the base class.
7. **Nothing costs more than it looks.** Allocation, copies of owning types, moves and dynamic dispatch are always visible in the code.
8. **Control when you ask for it.** Every safe default has a named escape hatch: `+%` wrapping arithmetic, `cast::truncate`, `std::raw_ptr`, `unsafe`.
9. **Fast, legible compilation.** Each error fits on one screen.

## Learning path

New to Rift? Read in this order; each step only uses what came before.

1. **Basics:** [Lexical structure](language/basics/lexical.md) → [Built-in types](language/basics/types.md) → [Tuples and list literals](language/basics/tuples-and-lists.md) → [Variables](language/basics/variables.md) → [Operators](language/basics/operators.md) → [Casts](language/basics/casts.md) → [Control flow](language/basics/control-flow.md)
2. **Functions:** [Functions](language/functions/functions.md) → [Lambdas](language/functions/lambdas.md)
3. **Your own types:** [Structs](language/user-types/structs.md) → [Classes](language/user-types/classes.md) → [Inheritance and interfaces](language/user-types/inheritance-and-interfaces.md) → [Enums](language/user-types/enums.md)
4. **Errors:** [Error handling](language/errors/error-handling.md) → [Checks](language/errors/checks.md)
5. **Memory:** [Ownership and references](language/memory/ownership-and-references.md) → [Raw pointers and `unsafe`](language/memory/raw-pointers-and-unsafe.md)
6. **Organizing code:** [Namespaces](language/organization/namespaces.md) → [Headers, sources and modules](language/organization/modules.md)
7. **Generic and compile-time code:** [Templates](language/compile-time/templates.md) → [Compile-time evaluation](language/compile-time/compile-time.md) → [Metaprogramming](language/compile-time/metaprogramming.md) → [Annotations](language/compile-time/annotations.md)
8. **Performance:** [SIMD](language/simd/simd.md)
9. **The standard library:** start at its [overview](stdlib/README.md).
10. **Building:** [Build system](build/build-system.md) → [Packages](build/packages.md) → [Build modes](build/build-modes.md)

## All pages

### The language: `language/`
| Folder | Pages |
|---|---|
| `basics/` | [Lexical structure](language/basics/lexical.md), [Built-in types](language/basics/types.md), [Tuples and list literals](language/basics/tuples-and-lists.md), [Variables](language/basics/variables.md), [Operators](language/basics/operators.md), [Casts](language/basics/casts.md), [Control flow](language/basics/control-flow.md) |
| `functions/` | [Functions](language/functions/functions.md) (parameters, `compile_time` and `implicit` parameters, `std::call_site`, `main`), [Lambdas](language/functions/lambdas.md) |
| `user-types/` | [Structs](language/user-types/structs.md), [Classes](language/user-types/classes.md), [Inheritance and interfaces](language/user-types/inheritance-and-interfaces.md), [Enums](language/user-types/enums.md) |
| `errors/` | [Error handling](language/errors/error-handling.md) (`std::result`, `?`, panics), [Checks](language/errors/checks.md) (`compile_time` / `run_time check`, custom compiler messages) |
| `memory/` | [Ownership and references](language/memory/ownership-and-references.md), [Raw pointers and `unsafe`](language/memory/raw-pointers-and-unsafe.md) |
| `organization/` | [Namespaces](language/organization/namespaces.md), [Headers, sources and modules](language/organization/modules.md) |
| `compile-time/` | [Templates](language/compile-time/templates.md), [Compile-time evaluation](language/compile-time/compile-time.md), [Metaprogramming](language/compile-time/metaprogramming.md), [Annotations](language/compile-time/annotations.md) |
| `simd/` | [SIMD](language/simd/simd.md): vector types, masks, blocks, multiversioning |
| `interop/` | [C and C++ interop](language/interop/c-interop.md) |

### The standard library: `stdlib/`
| Folder | Pages |
|---|---|
| (top) | [Overview and map of the library](stdlib/README.md) |
| `core/` | [Core utilities](stdlib/core/utilities.md): ownership helpers, checked arithmetic, bits, hashing, std interfaces |
| `text/` | [Strings](stdlib/text/strings.md), [Formatting and printing](stdlib/text/formatting.md) |
| `containers/` | [Sequences](stdlib/containers/sequences.md) (`darray`, `array`, `span`, `bitset`), [Maps](stdlib/containers/maps.md) |
| `memory/` | [Smart pointers](stdlib/memory/smart-pointers.md), [Allocators](stdlib/memory/allocators.md), [Raw memory](stdlib/memory/raw-memory.md) |
| `concurrency/` | [Threads](stdlib/concurrency/threads.md), [Synchronization](stdlib/concurrency/synchronization.md), [Channels](stdlib/concurrency/channels.md), [Thread pool](stdlib/concurrency/thread-pool.md) |
| `io/` | [Files, filesystem and environment](stdlib/io/io.md) |
| `time/` | [Time](stdlib/time/time.md) |
| `system/` | [Dynamic libraries and plugins](stdlib/system/dynamic-libraries.md) |
| `logging/` | [Logging](stdlib/logging/logging.md), [Logging output](stdlib/logging/output.md) |

### Building: `build/`
[Build system](build/build-system.md) (`build.rifts`, targets, C/C++), [Packages](build/packages.md) (outside code, CMake projects, the lock file), [Build modes](build/build-modes.md) (`debug`, `release`, user-defined modes)

### Not part of the design
- [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md): what is not designed yet.
- `drafts/`: proposals under review (events, windowing). They become pages once decided.
