# Performance: What the Compiler Knows and Uses

Edax is meant to be **faster than C++ by default**, not only when written carefully. The language rules give the compiler facts that a C++ compiler never has: references don't alias, nothing can point into itself, nothing throws. This page lists those guarantees, what the compiler does with them, and the controls a program has over optimization.

## Rules

### 1. Aliasing facts go to the optimizer
The aliasing rule (see [Ownership](../memory/ownership-and-references.md)) is a promise the compiler checks, so the optimizer is allowed to rely on it:

| Parameter | What the optimizer is told |
|---|---|
| `mut T&`, and `this` of a `mut` method | Nothing else reaches the object during the call (`noalias`) |
| `T&` and `this` of a non-`mut` method, when `T` is **frozen** | Nothing else reaches it, and it does not change during the call (`noalias readonly`): loads may be kept in registers across calls |
| `T&` of a type that isn't frozen | Nothing |

- **What a reference owns is covered too.** Two `mut std::darray<f32>&` parameters are different objects with different heap buffers, so `a[i]` and `b[j]` never overlap. Loads and stores whose address comes from a parameter (directly, through a local reference, or as the result of a method of it that returns a reference into `this`) are put in that parameter's alias scope, marked as not overlapping the other parameters' scopes. The scopes survive inlining, where they matter most: there the buffer pointers are loaded from the objects, and a C++ compiler has to assume that `px[i]` and `vx[i]` can be the same float.
- A **frozen** type is one whose bytes can't change through an immutable reference: numbers, `bool`, `char`, raw and function pointers, SIMD values, and structs, classes, payload enums, tuples and arrays made only of these. A type marked `@std::interior_mutable` is not frozen, and neither is anything containing one: `std::atomic`, `std::mutex`, `std::rw_mutex` and `std::condition` change through `T&` (atomics and locks on their own fields). A `std::darray<T>&` is frozen: its elements live behind a pointer, and the object itself doesn't change.
- A type whose methods change its own fields through `T&` in `unsafe` code must be marked `@std::interior_mutable`; without the mark that is undefined behaviour.
- `unsafe` code that changes a frozen value through a `T&`, or reaches a `mut T&`'s object another way, has undefined behaviour (see [Raw Pointers and `unsafe`](../memory/raw-pointers-and-unsafe.md)).

### 2. Parameter passing is the compiler's choice
- An immutable `T&` of a **copy type of at most 16 bytes** may be passed in registers instead of by address. A `T&` guarantees that the object doesn't change during the call, so the two are indistinguishable.
- Implemented by LLVM's argument promotion on `noalias readonly` references (up to four scalars, a `vec4`); every Edax function is internal, so all callers change with it.
- So the guideline for inputs is always `T&`, never "by value for small types". `extern` functions keep the C ABI.

### 2b. Branches that only choose a value are removed
- In a loop, `if (c) { a[i] = x; b[i] = y; }` after `a[i]` and `b[i]` were already written in the same iteration becomes `a[i] = c ? x : a[i]; b[i] = c ? y : b[i];`. The earlier stores show the addresses are valid and written by this thread, so the stores can't fault or race. The loop then has no branch and vectorizes on every CPU level, without masked stores.

### 3. Every value can be moved by copying its bytes
- References can't be stored and views can't outlive their source, so **no Edax value points into itself**. Moving a value is a copy of its bytes; the moved-from value is not destroyed.
- Growing a `std::darray`, rehashing a map, `std::swap` and sorting are `memcpy`/`memmove` for every element type. There is no "is it nothrow-movable" question as in C++.
- C++ objects that must not move stay behind a pointer in their wrappers (see [C and C++ Interop](../interop/c-interop.md)).

### 4. In-place construction
- A `T{...}` or a call returning `T` that initializes a variable, a field, a container slot or a return value is **built in place**: no temporary is made and moved.
- Library functions that build into a slot (`darray::push_with(make)`) get the same guarantee for `make()`'s result.

### 5. Checks that can't fail are removed
Bounds, overflow and shift checks stay in every safe mode. The compiler removes the ones it can prove never fire:
- the index of `for (usize i : 0..xs.len())` into `xs`, when `xs` isn't resized in the loop (the borrow rules prove that it isn't);
- a run of indexes `xs[i]`, `xs[i + 1]`, `xs[i + 2]` becomes one check;
- overflow of `i + 1` when `i < len` and `len <= isize::max`.

A program can add facts:

| Function | Safe? | Meaning |
|---|---|---|
| `std::assume(condition)` | Yes | The optimizer may rely on `condition`. Checked (a panic at the call) in every mode that doesn't allow undefined behaviour. Write one fact per call: `std::assume(a.len() == n); std::assume(b.len() == n);` gives two facts, `std::assume(x && y)` only their combination |
| `std::unreachable()` | Yes | Returns `never`. Panics when reached in a safe mode; removed as a path in a mode that allows undefined behaviour |
| `std::raw::assume(c)`, `std::raw::unreachable()` | `unsafe` | The unchecked intrinsics both are written with |

**Seeing what is left:** `@std::no_checks_left` on a hot function makes every check the optimizer couldn't remove a compile error that names the check's place, when the program is built (like `@std::simd::must_vectorize`, see [SIMD](../simd/simd.md)). The checks of `std::assume` and `std::unreachable` don't count: the program asked for them. `edaxc explain-checks <file>` lists every check of the file's functions (bounds, overflow, the entry checks of what they call) with `kept` or `removed`, as a release build optimizes them. Each function is looked at on its own, for any arguments: a check that only goes away because one caller passes a constant is `kept`.
```
$ edaxc explain-checks physics/integrate.ed
physics/integrate.ed: 3 run-time checks, 2 removed by the optimizer, 1 kept
  removed  physics/integrate.ed:9:19  the checks of std::darray<f32>::[]
  kept     physics/integrate.ed:15:12  index out of bounds
  removed  physics/integrate.ed:19:12  integer overflow in '*' (u32)
```
```
error[E6004]: 1 run-time check is left in a '@std::no_checks_left' function
  --> physics/integrate.ed:16:6
   = note: a check at physics/integrate.ed:18:17
```

### 6. Optimization control
| Control | Scope | Effect |
|---|---|---|
| The mode's `optimization()` (see [Build Modes](../../build/build-modes.md)) | The build | `none`, `basic` (the `development` mode), `size`, `speed` |
| `g.optimize_files(t, "math/**/*.ed", level)` (see [Build System](../../build/build-system.md)) | Files of a target | The math and the hot code at `speed` in a debug build, or one file at `none` to debug it in a release build |
| `@std::optimize{ .level = ... }` | One function | The same for a single function |

- Functions at `none` are compiled as written (no inlining into or out of them), whatever the rest of the build does.
- Cross-module inlining needs nothing: a program is compiled as one unit, so a small function of another module inlines like a local one.

### 7. Floating point
- The default is **strict IEEE**: no fused multiply-add unless written, no reordering. The same program gives the same results on every machine.
- `x.mul_add(y, z)` is always one fused operation (see [Built-in Types](../basics/types.md)).
- `@std::float_mode{ ... }` relaxes the float operations written in one function:

| Field | Allows |
|---|---|
| `.contract` | `a * b + c` becomes one fused multiply-add |
| `.reassociate` | Sums are reordered: vectorized reductions |
| `.finite_only` | The optimizer assumes no NaN or infinity |
| `.no_signed_zeros` | `-0.0` and `0.0` are treated alike |
| `.reciprocal` | `a / b` may become `a * (1 / b)`; approximate math functions |

- A mode whose `float_determinism()` returns `strict` (lockstep multiplayer, replays) makes every `@std::float_mode` a compile error, including in packages.

### 8. Dynamic dispatch
- A call through a reference whose exact class is known (a final class, a value just created) is a direct call.
- **Whole-program devirtualization:** the program is one module, so the compiler sees every class and every type implementing an interface. A virtual call that no derived class overrides, and an interface call with one implementing type in the program, are direct calls (and can be inlined); `edaxc insights` shows them as `devirtualized`. Not in plugins, nor in a program that loads plugins (`std::plugin<I>`): there the types aren't all known.
- `@std::dispatch_hint<circle>` on a call through an interface makes a guarded direct call for the expected class and the dynamic call for the others: speculative devirtualization, written where it's wanted.

### 9. Measured, not claimed
`bench/` runs each workload in Edax and in C++ (`clang`/`gcc -O2`, libstdc++, Abseil, Boost) and fails when the checksums differ. Every optimization of this page lands with a benchmark that shows it.

## Examples

### A kernel that a C++ compiler can't optimize as well
```edax
void integrate(mut std::span<f32> positions, std::span<f32> velocities, f32 dt) {
    for (usize i : 0..positions.len()) {
        positions[i] += velocities[i] * dt;   // positions and velocities don't overlap: vectorized without a runtime overlap check
    }
}
```

### Debugging one function in a release build
```edax
@std::optimize{ .level = std::build::optimization::none }
void resolve_contacts(mut physics::world& w) { ... }   // every local visible in the debugger
```

### A playable debug build
```edax
compile_time void build(mut std::build::graph& g) {
    std::build::target game = g.add_executable(std::build::edax_target{ .name = "game", .root = "game/main.ed" });
    g.optimize_files(game, "engine/math/**/*.ed", std::build::optimization::speed);
}
```

## Rationale
- **Guarantees instead of hints:** C++ has `__restrict` and `[[assume]]`, but they're unchecked promises that most code never makes. Edax checks the aliasing rule, so every function gets the benefit without writing anything.
- **Frozen types only for `readonly`:** synchronized types legitimately change through `T&`. Marking them read-only would let the optimizer cache a value another thread is changing.
- **Byte-copy moves:** they follow from second-class references. C++ needs move constructors, `noexcept` and "trivially relocatable" proposals for the same result.
- **Strict floats by default:** results that differ between debug and release, or between two players' machines, are bugs that are very hard to find. Speed is one annotation away, and it's visible where it applies.
- **Per-function and per-file optimization:** an engine's debug build is only useful if it runs at a playable frame rate. Optimizing the hot code while keeping the rest debuggable is the common answer, and it belongs in the build description, not in compiler flags.
