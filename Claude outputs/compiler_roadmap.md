# Edax: language and compiler extensions (design proposal)

> **Status:** proposal, not decided. Scope: the language and `edaxc` only. The standard library comes later, but where a library type needs something from the compiler, that need is listed here.
> **Goal:** a language for engine programmers that is **faster than C++**, **safe by default**, and **simple to use**, with room for Boost-sized support libraries on top.
> **Based on:** the combined design (`edax_design_current.md`), the compiler status notes, the events draft (32) and a look at `compiler/src` (AST → `check`/`ownership` → `llvm_backend`, tree-walking compile-time `Evaluator`).

Every proposal is checked against the nine principles. Each one says what it costs in simplicity, and what it would break if done differently.

---

## 0. Where the compiler is today, and what that means

| Observation (from `compiler/src`) | Consequence |
|---|---|
| The compiler lowers the checked AST straight to LLVM IR. There is no mid-level IR. | Every analysis that needs control flow (borrows, moves, bounds-check removal, coroutine lowering, devirtualization) has to be written against the AST, which gets harder with each feature. |
| Reference parameters get no `noalias`/`readonly`. Only `sret` is marked `noalias`. | The aliasing guarantees the language already enforces (§12 aliasing rule) are **not passed to the optimizer**. This is the biggest free speedup available. |
| The pipeline is LLVM `O2` / `Os`. | `O3`, PGO and target-specific tuning aren't used yet. |
| Overflow checks use `*_with_overflow` intrinsics per operation. | That part is right. What's missing is range analysis to remove checks that provably can't fire. |
| The compile-time `Evaluator` walks the AST with `shared_ptr<const Value>` values. | It's fine for build scripts. It's too slow for heavy metaprogramming (SoA, reflection tables, registries) and for the generated graphics modules, which cost 1.2–2.7 s per build. |
| The ownership check works on the AST (`ownership.cpp`). | Moves are flow-sensitive. Borrows are per-function, by variable. Field-level borrows aren't modelled. |

**Proposal 0 (prerequisite): add a mid-level IR, "EIR".** It's a typed control-flow graph with explicit moves, drops, borrows and checks, sitting between `check` and `llvm_backend`.
- Ownership, borrow and definite-initialization checks move onto EIR. They get simpler and more precise (field paths, loops).
- Edax-specific optimizations run on EIR before LLVM, where the language's guarantees are still visible: check removal, drop elision, devirtualization of `final` classes, relocation as `memcpy`.
- Coroutines (§4.1) are lowered on EIR.
- The compile-time evaluator runs EIR as bytecode (§6.1). That gives one semantics for compile time and run time, instead of two implementations.

Cost to simplicity: none for users. It's internal. For the compiler it's the largest single piece of work, and most of the later items depend on it.

---

## 1. Performance: turning the guarantees into speed

The rules Edax already enforces give it an advantage C++ can't have. C++ must assume aliasing, may throw anywhere, and can't move objects with `memcpy` in general. Most of this section is about telling the optimizer what the language already knows.

### 1.1 Aliasing information for LLVM *(compiler only, no syntax)*
- `mut T&` parameters → `noalias`. The aliasing rule guarantees the reference is the only path to that object while it is alive.
- `T&` parameters → `noalias readonly`. The object can't change while the reference lives, so the optimizer may cache loads across calls.
- `std::span<mut T>` / `std::span<T>` parameters → scoped alias metadata on their data pointer. This needs a small annotation on the view type so the backend knows which field is the data pointer: `@std::compiler::unique_data`.
- `unsafe` code that breaks this through a `std::raw_ptr` is already undefined behaviour under the current rules. Document it in *Raw Pointers and `unsafe`*.

### 1.2 Parameter passing chosen by the compiler *(rule change, simpler for users)*
- Rule: **an immutable `T&` parameter of a copy type up to 16 bytes is passed by value.** This is safe because a `T&` guarantees the object doesn't change during the call.
- Users stop wondering whether to write `vec3` or `vec3&`. The guideline becomes "always `&` for inputs".
- This doesn't apply to `extern` functions, which follow the C ABI.

### 1.3 Every type is trivially relocatable *(guarantee + optimization)*
- References can't be stored, and views can't outlive their source, so **no Edax object can point into itself.** Moving any value is therefore a bit copy, followed by forgetting the source (moved-from values aren't destroyed).
- Write this down as a language guarantee. `darray` growth, `hash_map` rehash, `swap` and sort then become `memcpy`/`memmove` for every type, with no "is it nothrow-movable" question as in C++.
- Compiler: moves of large values become `memcpy`, and EIR removes the drop flags of moved-from locals.

### 1.4 Guaranteed in-place construction
- Rule: a `T{...}`, or a call returning `T`, that initializes a variable, field, container slot or return value is **built in place**. Today that already happens for `sret`. Make it guaranteed, including for `push(T{...})`.
- Library hook: `darray::push_with(F make)` constructs directly in the slot. The compiler guarantees that `make()`'s result is built there (no temporary).

### 1.5 Removing checks without removing safety
Bounds and overflow checks stay in every safe mode. The compiler removes the ones it can prove never fire:
- **Range analysis on EIR:** the loop variable of `for (usize i : 0..xs.len())` indexes `xs` without a check, if `xs` isn't resized in the loop (the borrow rules already prove this).
- **Check hoisting:** a run of checks `xs[i]`, `xs[i+1]`, `xs[i+2]` becomes one check of `i + 2 < len`.
- **Overflow:** for `i + 1`, where `i < len` and `len ≤ isize::max`, no check is needed.
- **`std::assume(cond)`** (`unsafe`): passes a fact to the optimizer. In safe modes it's checked, like a `run_time check`, unless `allow_undefined_behavior()`.
- **`unreachable`** becomes `std::unreachable()` (`unsafe`) and a `never` result.
- **Reporting:** `edaxc explain-checks file.ed:42` lists the checks left in a function and why each one stayed. `@std::no_checks_left` is an annotation that makes a hot function a compile error if any check remains. This is the same idea as `@std::simd::must_vectorize`, which already exists.

### 1.6 Optimization control
| Addition | Form |
|---|---|
| A built-in **`development` mode**: `speed`-lite (O1 + inlining), `line_tables`, all checks | `std::build::mode::development`, a third built-in mode. Debug-build speed is the main complaint of game programmers about C++ and Rust |
| Per-file / per-package optimization | `g.set_for_files(t, "math/*.ed", std::build::optimization_setting, speed)` and `.optimization` on a package. The per-file setting mechanism already exists |
| Per-function | `@std::optimize{ .level = speed }`, `@std::optimize{ .level = none }` (to debug one function in a release build) |
| `O3` for `speed` | The pipeline currently uses `O2` |
| **PGO** | A mode setting `profile_guided() → std::option<path>`. The profile file is a recorded input of the build, so builds stay reproducible. `edaxc build --mode=release --profile-generate` makes an instrumented build |
| Target CPU | `g.target_cpu(t, "x86-64-v3")`. The SIMD multiversioning keeps working for code that wants several levels |
| Cross-module inlining without LTO | The compiled interface carries the EIR of **small, non-generic functions** (≤ N instructions, or `@std::inline_always`). Importers can inline them. Changing such a body recompiles importers, and the docs say so |

### 1.7 Floating point
- Default: strict IEEE, **no FMA contraction**, the same results on every target. Document this.
- `@std::float_mode{ .contract = true, .reassociate = true, .finite_only = true, .no_signed_zeros = true }` on a function or file (file annotations already exist). It maps to LLVM fast-math flags on the instructions of that body only.
- `std::fma(a, b, c)` is always fused, so code doesn't depend on a flag.
- A mode setting `float_determinism() → strict | relaxed` lets a lockstep game refuse `@std::float_mode` in its whole build.
- New type **`f16`** (storage + conversions; arithmetic promotes to `f32` unless the target has native support). It's needed for GPU data.

### 1.8 Dynamic dispatch
- **Devirtualization** on EIR: a call through a `final` class, or through a reference whose exact type is known, becomes direct.
- `std::unique_ptr<I>` to an interface is a fat pointer. Its dispatch table can be **const-folded** when the object was just created.
- `@std::dispatch_hint<circle>` on a call site (optional): a guarded direct call for the hot case. This is speculative devirtualization made explicit.

### 1.9 A benchmark suite as part of the compiler
"Faster than C++" needs a proof that runs on every commit:
- `bench/` gets pairs `name.ed` / `name.cpp` (clang -O3, same algorithm), with a CI job that fails when Edax is more than X % slower.
- Starter set: nbody, matrix multiply, particle update (AoS and SoA), hash-map insert/lookup, sort, JSON parse, ECS iteration, string building.

---

## 2. Data layout

Engines live and die by layout. Today a struct has C layout, there's `std::cache_aligned<T>`, and layout checks work through `compile_time check`.

### 2.1 Layout annotations
```edax
@std::layout{ .align = 16 }
struct vec4 { f32 x; f32 y; f32 z; f32 w; }

@std::layout{ .packed = true }               // fields at byte offsets, no padding; unaligned fields can only be copied, not referenced
struct file_header { u32 magic; u16 version; u64 size; }

struct gpu_light {
    vec3              position;
    std::padding<4>   _;                     // explicit padding: a field that can't be read and is zero when built
    vec3              color;
    f32               intensity;
}
```
- `std::padding<N>` keeps "no hidden state" true. The padding is written in the source.
- A warning, `std::layout::implicit_padding` (off by default, on for `@std::layout{ .no_implicit_padding = true }`), names every gap the compiler inserted. This is what GPU and file structs want.
- A field of a packed struct can't be borrowed (`&h.size` is an error), only read or assigned. That closes the classic misaligned-pointer UB.

### 2.2 `std::bit_cast<T>(x)` *(safe)*
- Allowed in safe code when **both types are plain copy types of the same size**, have no padding, and every bit pattern is valid for `T` (numbers, SIMD vectors, arrays and structs of those). Otherwise it's a compile error that names the field.
- This replaces most uses of `cast::reinterpret`, which stays as the `unsafe` escape hatch.

### 2.3 Unions
- `union` as a type kind: `unsafe` to read, safe to write. It's for interop and hand-written variant storage. Payload enums stay the normal sum type.

### 2.4 Bit fields
- `u32 kind : 4;` inside `@std::layout{ .bits = true }` structs. Reads and writes are generated shift/mask code. A bit field can't be borrowed.
- Alternative, if this feels too much like C: a library `std::bit_struct` built with the type construction of §2.5. **Recommendation:** library, once §2.5 exists.

### 2.5 Type construction from written declarations (the SoA problem)
Today annotations can generate methods but not fields, and metaprogramming can't make types. Engines need SoA containers, component storage, GPU mirrors of CPU structs, and network-replication structs.

Proposal: **field-generating templates.** A struct template may declare fields with `for compile_time` over a **compile-time list of fields of another type**. Every generated field takes its **name from a written declaration** (the source type's field), so the rule "every name has a written declaration" still holds.
```edax
template<type T, type A = std::global_allocator>
struct soa_columns {
    for compile_time (std::meta::field f : std::meta::fields<T>()) {
        std::darray<f.type, A> f.name;            // one column per field of T, with T's field name
    }
}

soa_columns<particle> ps = default;
ps.position.push(p0);                             // `position` comes from particle's declaration
```
- Allowed inside: field declarations whose type and name come from the loop value (`f.name`, or `f.name` with a written prefix/suffix such as `f.name ~ "_dirty"`, never from computed strings). Annotations from the source field may be copied: `@f.annotations`.
- `go to definition` on `ps.position` goes to `particle::position`. Tools can always show where a name came from.
- The same mechanism gives `gpu_mirror<T>` (padding inserted for std140), `replicated<T>`, `delta<T>`.
- Cost to simplicity: one new form of `for compile_time`. It's the only way a type's shape can depend on another type, and it keeps that visible in the template.

### 2.6 Layout of enums: niches
- `std::option<T>` takes no extra space when `T` has an invalid bit pattern: `unique_ptr`, `shared_ptr`, references, `char`, `bool`, enums with fewer than 256 alternatives, and **`std::nonzero<u32>` / `u64`** (new). This is what makes `option<handle>` 8 bytes.
- Payload enums use leftover padding of their payloads for the tag where possible.
- `@std::layout{ .tag = u8 }` on an enum fixes the tag type, for file formats.

---

## 3. Safety and borrowing for engine code

### 3.1 Disjoint borrows of fields *(rule change)*
Today it isn't specified whether two fields of one object can be borrowed mutably at once. Engines need it for every system that reads one component array and writes another.
- Rule: within a function, borrows are tracked by **path**: `w.positions` and `w.velocities` are different places. `mut w.positions` and `mut w.velocities` may be alive together. `mut w` and `w.positions` may not.
- Across a call, a `mut` method still borrows all of `this`. To hand out several disjoint parts, see §3.2.
- Element paths of containers aren't tracked (`xs[i]` and `xs[j]` stay one place). Libraries offer `split_at`, `get_many_mut`, `chunks`.

### 3.2 Returning several references
The current rule allows a function to return a reference into `this` or into the only view parameter. Extend it:
- A method may return a **tuple of references into distinct fields of `this`**: `(mut transform_store&, physics_store&) split() mut`. The compiler checks distinctness in the body, like the existing rule.
- `get_many_mut` in the library is then an ordinary method returning `std::option<(mut T&, mut T&)>`, with a run-time check that the handles differ.

### 3.3 Lambdas with changing state *(open question 50)*
- `[mut count = 0u32]` declares a captured value the lambda may change. The lambda's call is then `mut`, so it can only be called through a `mut` variable or a `std::function` it's stored in. This reuses the `mut` marker, with no new keyword.

### 3.4 `std::function` with bounds *(open question 47)*
- `std::function<R(A...), std::sendable>`: the second argument is a list of interfaces that the erased callable must implement. The type implements them too.
- Compiler need: interface constraints that the erased object carries. Type erasure with bounds is a generic feature, so `std::any_of<I...>` (a value of any type implementing interfaces) comes with it.
- `std::function` gets an allocator parameter like the containers (`std::function<R(A...), std::sendable, A>`).
- Optional small-buffer variant: **`std::inline_function<R(A...), N>`** stores captures up to `N` bytes inline and never allocates. A capture that's too big is a compile error. The cost is visible in the type.

### 3.5 Self-referential and pinned values
Not needed in safe code: the relocation guarantee (§1.3) depends on its absence. The C++ interop wrappers (§7) handle C++ objects that must not move by keeping them behind a `std::unique_ptr`.

### 3.6 Run-time–checked borrows for long-lived data
For editor and gameplay code that needs "a reference that lives longer than a call":
- `std::cell<T>` (single thread, run-time-checked shared/mutable borrow, panics on conflict), the library equivalent of `RefCell`.
- Compiler need: a borrow guard type that is a view of the cell. This already exists for `mutex` guards.

### 3.7 Sanitizer modes
Safe Edax has no UB, but engines link a lot of C/C++ and use `unsafe`.
- Mode settings `sanitize() → none | address | thread | undefined | memory`, valid only in modes that inherit from `debug` or `development`.
- C/C++ targets of the same build get the same flags, so the whole program is instrumented consistently.

---

## 4. Control flow and concurrency in the language

### 4.1 Generators and resumable functions
They're needed for easy iterators (simplicity), gameplay scripts that wait (`sleep_until_event`, open question 53), asset streaming state machines, and tools.
```edax
generator<u32> evens(u32 limit) {
    for (u32 i : 0..limit) {
        if (i % 2 == 0) { yield i; }
    }
}

for (u32 n : evens(10)) { ... }
```
- **No hidden allocation:** a generator call returns a value of an unnamed type that contains its frame inline. The size is known at compile time, as with lambdas. Stored as `auto`, passed to templates, or erased into `std::boxed_generator<T>`, which allocates visibly.
- A generator that borrows (reference parameters, view captures) is a view type. The usual rules prevent dangling frames, which C++ coroutines don't.
- **Resumable functions:** `resumable<T> load_level(...)` with `await x`, where `x` implements `std::awaitable<T>` (`poll(mut waker&) → std::option<T>`). Same frame rules. There's no built-in executor: the game loop, a thread pool or a script scheduler polls. This keeps "nothing runs that you didn't call".
- Compiler work: EIR state-machine lowering. Recursion needs an explicit box (the frame size must be finite), and the error says so.
- Cost to simplicity: two keywords (`yield`, `await`). Generators replace most hand-written iterator classes, so users write less code overall.

### 4.2 Fibers: one intrinsic, the rest in the library
- `std::raw::stack_switch(from, to)` (`unsafe`), plus a stack allocator with a guard page. A fiber job system à la Naughty Dog is then library code.
- The compiler must know about it: `thread_local` access isn't cached across a call that may switch fibers. `@std::may_switch_fiber` on such functions disables that caching. The library marks its wait functions.

### 4.3 `defer` *(small, optional)*
- `defer { ... }` runs at scope exit. It's mainly for C interop code without RAII wrappers. Recommendation: add it only if C-interop examples show the need. RAII is the one way for Edax types.

### 4.4 Pattern-matching sugar
- `if (opt is some(v)) { ... }` and `while (it.next() is some(x))`: a `match` with one arm, binding into the branch. It removes most `match` boilerplate around `option`/`result` while staying a single concept (patterns).

---

## 5. Types and generics for Boost-sized libraries

For libraries the size of Boost (graph, geometry, spirit-like parsers, units, containers), the template system needs a few more tools. It must still avoid what makes C++ templates painful: SFINAE, specialization, error novels.

| Addition | Why | Shape |
|---|---|---|
| **Associated types in interfaces** | Iterators, containers and graphs need "the element type of `C`" | `interface iterable { type element; std::option<element> next() mut; }`. Constraint: `template<type C : iterable>` and then `C::element` |
| **Interface implementation for existing types** (retroactive) | A geometry library must make `my_engine::vec3` a `geometry::point` without editing it | `implement geometry::point for math::vec3 { ... }` in the module of the interface **or** of the type (the orphan rule), so there are never two competing implementations |
| **Operators via interfaces** | Generic math over user types | `std::addable<T>`, `std::multipliable<T, R>`, ... are implemented by writing the operator. Constraints can then name them |
| **Variadic value packs** | Shapes and strides: `tensor<f32, 4, 4>` | `template<usize... N>` with `for compile_time` |
| **Strong typedefs** | Units (meters vs. seconds), ids, handles | `distinct type meters = f32;` is a new type with the same layout. Arithmetic is opt-in through `@std::derive::arithmetic`. No implicit conversion, like everything else |
| **Compile-time strings as template arguments** | Units, hashed names, shader slots, `std::name<"player">` | `template<std::static_str S>`. Value template parameters of struct type already exist, so this extends the same mechanism |
| **Error-message control** | Library authors replace generic errors | Already there with checks and `std::diagnostic`. Add `@std::diagnostic_frame{ .hide = true }` so library-internal frames are dropped from instantiation chains |

What stays out: specialization, partial ordering of overloads by "more specialized", ADL, implicit conversions in generic code. `if compile_time` plus constraints cover these needs.

---

## 6. Compile time, reflection, metaprogramming

### 6.1 A fast compile-time evaluator
- Run compile-time code as **EIR bytecode** (§0) with flat values (no `shared_ptr` per value), memoize pure calls, and cache results in the compiled interface.
- Targets: generated graphics modules from about 2.7 s to under 0.3 s, and reflection over 1000 types in under 1 s.
- Budget: `--time-report` shows compile-time evaluation per function. Endless loops stop at a step limit with a message, not a hang.

### 6.2 `std::meta::type_id<T>()` *(from the events draft)*
- A 64-bit hash of the fully qualified type, `compile_time run_time`. A collision is a link error naming both types.
- For plugins: ids are computed from the compiled interface, so the same type has the same id in the host and in every plugin built from the same interface.

### 6.3 Runtime type information on request
Editors, serializers, undo systems and network replication need type information at run time.
- `@std::reflect` on a type (or `std::meta::runtime_info<T>()` called anywhere) makes the compiler emit a **static, read-only table**: name, size, alignment, fields (name, offset, type id, annotations that are themselves `@std::reflect`), methods marked `@std::reflect`, enum alternatives.
- Compiler need: **materializing compile-time values into static data**, including `std::static_str`, `std::fn_ptr` thunks for `m.call(...)` and nested arrays. This is useful far beyond RTTI: lookup tables, CRC tables, baked curves.
- Nothing is emitted for types that don't ask. Principle 7 holds: the cost is in the code that asks.

### 6.4 Embedding files
- `std::embed("shaders/blit.spv")` → `std::span<u8>` in static memory, or `std::embed_text(...)` → `std::static_str`.
- The path is a recorded input of the build (like everything the build script reads), so incremental builds stay correct. Paths are relative to the file, with the same warning as `std::filesystem`.

### 6.5 Registries across the program
- `functions_with<annotation>` already exists. Add `types_with<annotation>()`: the compile-time list of every type in the target that carries an annotation. Component registration, serializer lists and editor panels then need no hand-written lists and no static constructors.

### 6.6 `edaxc expand`
- It prints a file with every annotation-generated member, every `for compile_time` unrolled and every `deliver` expanded. Proposed in the events draft; it becomes essential once §2.5 exists.

---

## 7. C and C++ interop

Engines depend on C++ (Jolt, PhysX, ImGui, FMOD, Wwise, Assimp, platform SDKs). `cpp_wrap.cpp` already generates C wrappers for C++ classes. What's missing for real engines:

| Need | Proposal |
|---|---|
| Edax implementing a C++ virtual interface (Jolt contact listeners, ImGui backends, allocator hooks) | `extern(cpp) class my_listener : JPH::ContactListener { ... override ... }`. The wrapper generator emits a C++ subclass that forwards each virtual to an Edax function pointer |
| Templates on the C++ side (`std::vector<T>`, `JPH::Ref<T>`) | The build script lists the instantiations it needs: `g.cpp_instantiate("JPH::Ref<JPH::Shape>")`. They're wrapped like classes. No automatic C++ template instantiation in edaxc |
| C++ value types by value | `@cpp::value` on a wrapped class with a trivially relocatable C++ type: stored inline in Edax, not behind a pointer. The generator checks `std::is_trivially_copyable` with a `static_assert` in the generated C++ |
| Strings and spans across the boundary | Automatic `std::str` ↔ `std::string_view` and `std::span` conversions in wrappers |
| Exceptions | A wrapped call catches everything at the boundary and returns `std::result<T, cpp::exception>` (or panics, per wrapper option). An exception never crosses into Edax frames |
| Windows | The MSVC C++ ABI for wrappers, `__vectorcall`/`__stdcall` in `extern(c)` declarations |

---

## 8. Platforms, debugging, tooling

### 8.1 Targets
- **Windows x64**: COFF output, MSVC-compatible C ABI, **PDB/CodeView debug info** (LLVM supports it), linking with `lld-link`, the Win32 runtime pieces (threads, futex → `WaitOnAddress`, TLS).
- **macOS arm64**: Mach-O, `objc` message sends for the windowing layer (`extern(objc)` or generated C shims).
- **Custom targets for consoles and embedded**: `std::build::platform` as a class (like modes) describing the triple, capabilities, ABI notes, linker and system libraries. NDA platforms can then live in a private package.
- Cross compilation as the normal case: `edaxc build --target=windows-x64` from Linux with `lld`.

### 8.2 Debugging
- **Pretty-printers generated by the compiler** from layout information, for every type including payload enums, `darray`, `hash_map`, `option`: gdb/lldb Python and `.natvis`, emitted next to the binary.
- Stepping through a `?` or a check doesn't stop in compiler-generated code (`DW_AT_artificial`).
- Panic messages carry the `call_site` chain of `implicit` call sites, not only the innermost frame.

### 8.3 Iteration speed
| Item | Target |
|---|---|
| Cached std and generated modules (compiled interfaces on disk, keyed by compiler hash) | `std` + graphics checked once per compiler version, not per build |
| Parallel checking of independent modules | The import graph is acyclic, so this is a plain topological schedule |
| Function-level incremental codegen (EIR hashes per function, cached object code) | Changing one function body rebuilds only that function's object code |
| Fast debug codegen | Optional: a non-LLVM backend for `debug` (Cranelift-like, or a direct x86-64 emitter from EIR). Large effort, deferred, but EIR makes it possible |
| Edit-and-continue for plugins | §8.4 |

### 8.4 Hot reload
- `g.add_plugin(..., .hot_reload = true)`: the build keeps the compiled interface's layout table in the plugin.
- `std::plugin<I>::reload(path)`: loads the new library and checks the interface hash. If **state layouts** changed, it migrates them field by field using the layout tables (fields matched by name; new fields get their default; removed fields are dropped; type changes are an error value). Only types marked `@std::hot_reload_state` are migrated. Everything else must be rebuilt by the plugin.
- Compiler need: the layout tables (from §6.3) and a guarantee that `std::type_id` is stable across rebuilds when the type didn't change.

### 8.5 Language server
- `edaxc lsp`, built from the compiler's own front end (not a separate parser): diagnostics, go-to-definition (including names from §2.5), hover with layout (size, alignment, offsets, padding), inlay hints for moves and remaining checks (§1.5).

---

## 9. What stays out

To keep Edax simple:
- **No macros, no textual code generation.** Annotations, `for compile_time` and field-generating templates cover the uses.
- **No exceptions.** `result` and panics.
- **No implicit conversions** beyond the existing table, also not for user types.
- **No template specialization, no SFINAE, no ADL.**
- **No garbage collector, no hidden reference counting.**
- **No global constructors.** `static` initializers are compile-time, and registries come from §6.5.
- **No lifetime annotations.** §3.1–3.2 widen what the local check accepts instead.

---

## 10. Order of work

| Phase | Items | Why first |
|---|---|---|
| **1. Free speed and playable debug builds** | §1.1 aliasing attributes, §1.2 by-value small refs, §1.6 `development` mode + per-file optimization + O3, §1.9 benchmark suite | Small compiler changes with a large effect. The benchmark suite measures everything after it |
| **2. EIR** | §0, moving ownership/borrow checks onto it, §1.3 relocation, §1.4 in-place construction | The base for phases 3–5 |
| **3. Engine data** | §2.1–2.4 layout, §2.6 niches + `nonzero`, §3.1–3.2 disjoint borrows, §1.7 float modes + `f16` | What every engine subsystem touches |
| **4. Compile time** | §6.1 fast evaluator, §6.2 `type_id`, §6.3 static data + RTTI, §6.4 `embed`, §6.5 `types_with`, §2.5 field-generating templates | Unlocks the std work: ECS storage, serialization, editor, events |
| **5. Control flow** | §4.1 generators/resumables, §4.2 stack switch, §1.5 check removal, §1.8 devirtualization, §4.4 `is` patterns | Needs EIR; makes the library both faster and nicer |
| **6. Generic libraries** | §5 associated types, retroactive implementations, operator interfaces, strong typedefs, string template arguments, §3.3–3.4 lambdas/functions | The base for Boost-sized libraries |
| **7. Reach** | §8.1 Windows + PDB, §7 C++ interop extensions, §8.2 pretty-printers, §8.3 caching/incremental, §8.4 hot reload, §8.5 LSP, §3.7 sanitizers | Needed before other people use it for real engines |

**Rule for every phase:** each item lands with a design page (Rules / Examples / Rationale), feature tests, and, for performance items, a benchmark in `bench/` that shows the gain.
