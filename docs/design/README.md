# Edax Language Design

Edax (Latin for "devouring") is a C++-inspired systems language: explicit, simple to use, with full control over memory and cost. These pages describe the language and its standard library **as they are**. Each page explains one topic and can be read on its own, with **Rules**, **Examples** and **Rationale**.

- Source files are `.ed`, header files `.edh`.
- Undecided points are collected in [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md), not in the pages.
- Decided but not implemented yet: such parts carry a **Planned** note (`> **Planned:** ...`) where they are described, so that every page without one describes what edaxc does today. [`../compiler/spec-34-status.md`](../compiler/spec-34-status.md) lists them all.
- Example programs: [`example/`](../../example/README.md).

## Design principles

1. **Explicit beats implicit.** If a value's type, cost or behaviour can't be read from the source, the compiler refuses to guess. The *complete* list of implicit conversions fits in one table (see [Built-in types](language/basics/types.md)).
2. **One way to do each thing.** One way to initialize (`=`), one way to build a value (`T{...}`), one way to define a method body.
3. **Safe by default.** Safe code cannot trigger undefined behaviour. Overflow, out-of-bounds access and bad shifts panic. Unsafe operations live inside a visible `unsafe { }` block. All built-in build modes are safe; a user mode can relax this only by saying so in `build.ed`.
4. **Immutable by default.** Variables, parameters and `this` are immutable unless marked `mut`.
5. **No uninitialized values.** Every variable has a value from its declaration on.
6. **Composition and inheritance, both explicit.** Inheritance requires `open` on the base class.
7. **Nothing costs more than it looks.** Allocation, copies of owning types, moves and dynamic dispatch are always visible in the code.
8. **Control when you ask for it.** Every safe default has a named escape hatch: `+%` wrapping arithmetic, `cast::truncate`, `std::raw_ptr`, `unsafe`.
9. **Fast, legible compilation.** Each error fits on one screen.
10. **Faster than C++ by default.** The rules that make code safe (no aliasing `mut` references, no self-pointing values, no exceptions) are also facts the optimizer uses. Speed doesn't need unchecked promises (see [Performance](language/performance/performance.md)).

### What Edax leaves out
| Left out | Instead |
|---|---|
| Macros and textual code generation | [Checks](language/errors/checks.md) (custom compile errors and warnings), [annotations](language/compile-time/annotations.md) (generated members, wrapped bodies) and [compile-time code](language/compile-time/metaprogramming.md): typed, checked, and with normal error messages |
| Exceptions | `std::result`, `?` and panics ([Error handling](language/errors/error-handling.md)) |
| Implicit conversions beyond one table | `cast` spellings that say what is lost |
| Template specialization, SFINAE, ADL | `if compile_time` and interface constraints |
| Global constructors | Compile-time initializers and registries (`std::meta::functions_with`, `types_with`) |
| Lifetime annotations | Second-class references, field-precise borrows, views |
| A garbage collector | Ownership and RAII. An opt-in collector is a possible far-future feature ([OPEN_QUESTIONS](OPEN_QUESTIONS.md)) |

## Learning path

New to Edax? Read in this order; each step only uses what came before.

1. **Basics:** [Lexical structure](language/basics/lexical.md) → [Built-in types](language/basics/types.md) → [Tuples and list literals](language/basics/tuples-and-lists.md) → [Variables](language/basics/variables.md) → [Operators](language/basics/operators.md) → [Casts](language/basics/casts.md) → [Control flow](language/basics/control-flow.md)
2. **Functions:** [Functions](language/functions/functions.md) → [Lambdas](language/functions/lambdas.md) → [Generators and resumable functions](language/functions/generators.md)
3. **Your own types:** [Structs](language/user-types/structs.md) → [Classes](language/user-types/classes.md) → [Inheritance and interfaces](language/user-types/inheritance-and-interfaces.md) → [Enums](language/user-types/enums.md) → [Memory layout](language/user-types/layout.md)
4. **Errors:** [Error handling](language/errors/error-handling.md) → [Checks](language/errors/checks.md)
5. **Memory:** [Ownership and references](language/memory/ownership-and-references.md) → [Raw pointers and `unsafe`](language/memory/raw-pointers-and-unsafe.md)
6. **Organizing code:** [Namespaces](language/organization/namespaces.md) → [Headers, sources and modules](language/organization/modules.md)
7. **Generic and compile-time code:** [Templates](language/compile-time/templates.md) → [Compile-time evaluation](language/compile-time/compile-time.md) → [Metaprogramming](language/compile-time/metaprogramming.md) → [Annotations](language/compile-time/annotations.md)
8. **Performance:** [Performance](language/performance/performance.md) → [SIMD](language/simd/simd.md)
9. **The standard library:** start at its [overview](stdlib/README.md).
10. **Building:** [Build system](build/build-system.md) → [Packages](build/packages.md) → [Build modes](build/build-modes.md) → [Build output and diagnostics](build/build-output.md)

## All pages

### The language: `language/`
| Folder | Pages |
|---|---|
| `basics/` | [Lexical structure](language/basics/lexical.md), [Built-in types](language/basics/types.md), [Tuples and list literals](language/basics/tuples-and-lists.md), [Variables](language/basics/variables.md), [Operators](language/basics/operators.md), [Casts](language/basics/casts.md), [Control flow](language/basics/control-flow.md) |
| `functions/` | [Functions](language/functions/functions.md) (parameters, `compile_time` and `implicit` parameters, `std::call_site`, `main`), [Lambdas](language/functions/lambdas.md), [Generators and resumable functions](language/functions/generators.md) |
| `user-types/` | [Structs](language/user-types/structs.md), [Classes](language/user-types/classes.md), [Inheritance and interfaces](language/user-types/inheritance-and-interfaces.md), [Enums](language/user-types/enums.md), [Memory layout](language/user-types/layout.md) |
| `errors/` | [Error handling](language/errors/error-handling.md) (`std::result`, `?`, panics), [Checks](language/errors/checks.md) (`compile_time` / `run_time check`, custom compiler messages) |
| `memory/` | [Ownership and references](language/memory/ownership-and-references.md), [Raw pointers and `unsafe`](language/memory/raw-pointers-and-unsafe.md) |
| `organization/` | [Namespaces](language/organization/namespaces.md), [Headers, sources and modules](language/organization/modules.md) |
| `compile-time/` | [Templates](language/compile-time/templates.md), [Compile-time evaluation](language/compile-time/compile-time.md), [Metaprogramming](language/compile-time/metaprogramming.md), [Annotations](language/compile-time/annotations.md) |
| `performance/` | [Performance](language/performance/performance.md): what the optimizer is told, check removal, optimization and float control |
| `simd/` | [SIMD](language/simd/simd.md): vector types, masks, blocks, multiversioning |
| `interop/` | [C and C++ interop](language/interop/c-interop.md) |

### The standard library: `stdlib/`
| Folder | Pages |
|---|---|
| (top) | [Overview and map of the library](stdlib/README.md) |
| `core/` | [Core utilities](stdlib/core/utilities.md): ownership helpers, checked arithmetic, bits, hashing, std interfaces, versions |
| `text/` | [Strings](stdlib/text/strings.md), [Formatting and printing](stdlib/text/formatting.md) |
| `containers/` | [Sequences](stdlib/containers/sequences.md) (`darray`, `array`, `span`, `bitset`), [Maps](stdlib/containers/maps.md) |
| `memory/` | [Smart pointers](stdlib/memory/smart-pointers.md), [Allocators](stdlib/memory/allocators.md), [Raw memory](stdlib/memory/raw-memory.md) |
| `concurrency/` | [Threads](stdlib/concurrency/threads.md), [Synchronization](stdlib/concurrency/synchronization.md), [Channels](stdlib/concurrency/channels.md), [Thread pool](stdlib/concurrency/thread-pool.md) |
| `io/` | [Files, filesystem and environment](stdlib/io/io.md) |
| `time/` | [Time](stdlib/time/time.md) |
| `system/` | [Dynamic libraries and plugins](stdlib/system/dynamic-libraries.md) |
| `logging/` | [Logging](stdlib/logging/logging.md), [Logging output](stdlib/logging/output.md) |
| `windowing/` | [Windowing](stdlib/windowing/windowing.md): windows, input, OpenGL contexts and Vulkan surfaces |
| `graphics/` | [Graphics APIs](stdlib/graphics/graphics.md): an API is a name and a version; OpenGL and Vulkan from the Khronos registries |

### Building: `build/`
[Build system](build/build-system.md) (`build.ed`, targets, C/C++), [Packages](build/packages.md) (outside code, CMake projects, the lock file), [Build modes](build/build-modes.md) (`debug`, `release`, user-defined modes), [Build output and diagnostics](build/build-output.md) (the checklist, `build.log`, the error limit)

### Not part of the design
- [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md): what is not designed yet.
- [`../compiler/spec-34-status.md`](../compiler/spec-34-status.md): which parts of the performance and engine extensions (spec 34) edaxc implements so far.
- `drafts/`: proposals under review (events, windowing). They become pages once decided.
