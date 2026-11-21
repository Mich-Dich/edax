# SIMD

SIMD (single instruction, multiple data) runs one operation on several values at once. In Edax it is part of the language: **vector types** that behave like numbers, **masks** for per-lane conditions, and **blocks** that walk an array in vector-sized steps, including the leftover elements at the end. Nothing is hidden: the type says how many lanes of which number, the operators say what is computed, and `edaxc simd-report` says which instructions it became.

```edax
void add_arrays(std::span<f32> a, std::span<f32> b, std::span<mut f32> result) {
    run_time check (a.len() == result.len() && b.len() == result.len()) else "add_arrays: lengths differ";
    for (std::simd::block<8> at : std::simd::blocks<8>(result.len())) {
        result.store(at, a.load(at) + b.load(at));        // 8 floats per step; the last block may be partial
    }
}
```
On an AVX2 target this compiles to the same loop as the C++ version with `_mm256_loadu_ps` / `_mm256_add_ps` / `_mm256_storeu_ps`, plus one masked step for the remainder instead of a second scalar loop.

## Rules

### 1. Vector types
**`std::simd::vec<T, N>`** holds `N` values ("lanes") of the number type `T`:
- `T` is `i8` ... `i64`, `u8` ... `u64`, `f32` or `f64`.
- `N` is a power of two from 2 to 64.

The common ones have built-in short names, reserved like `i32`: `<type>x<lanes>`.

| Short name | Is | Hardware (x86-64 with AVX2 / ARM NEON) |
|---|---|---|
| `f32x4`, `f32x8`, `f32x16` | `vec<f32, 4>`, `vec<f32, 8>`, `vec<f32, 16>` | one SSE / one AVX / two AVX registers; one / two / four NEON registers |
| `f64x2`, `f64x4`, `f64x8` | `vec<f64, ...>` | likewise |
| `i8x16`, `u8x16`, `u8x32`, `i16x8`, `i16x16`, `i32x4`, `i32x8`, `u32x8`, `i64x4`, ... | the integer vectors | likewise |

- **A vector type always has exactly `N` lanes**, on every target. A width the hardware doesn't have is made from several registers (`f32x16` on AVX2 is two `ymm` registers); this is still SIMD and costs nothing over writing it by hand.
- On a target with **no** SIMD unit at all, vectors become one scalar operation per lane. That compiles everywhere, with a warning `std::simd::scalarized` (an id you can turn into an error, see [Checks](../errors/checks.md)).
- `std::simd::native_lanes<T>()` is a `compile_time usize`: how many lanes of `T` fill one register of the target (8 for `f32` on AVX2, 16 with AVX-512, 4 on NEON). `std::simd::vec<f32, std::simd::native_lanes<f32>()>` is "one register of floats", whatever the target.
- Vectors are **copy types**. They are passed in registers where the target has registers of that width.

### 2. Creating vectors
```edax
f32x8 zeros = default;
f32x8 twos  = f32x8::splat(2.0);                          // the same value in every lane
f32x8 ramp  = [0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0];   // a list literal (see Tuples and List Literals)
i32x8 idx   = i32x8::iota();                              // 0, 1, 2, ..., 7
```
A list literal must have exactly `N` elements.

### 3. Operators: like numbers, lane by lane
| Operators | On | Notes |
|---|---|---|
| `+ - * /` | all vectors | Lane by lane, with the same rules as for scalars (see [Operators](../basics/operators.md)): integer overflow and integer division by zero **panic** if any lane overflows |
| `+% -% *%` | integer vectors | Wrapping, never panics: the usual choice in hot integer loops |
| `& \| ^ ~` | integer vectors | Bitwise |
| `<< >>` | integer vectors | By a scalar (all lanes) or by a vector (per lane); a count ≥ the lane width panics, as for scalars |
| `== != < <= > >=` | all vectors | Give a **mask** (section 4), never a `bool` |

- **No implicit broadcast:** `v * 2.0` is an error; write `v * f32x8::splat(2.0)`. A scalar never turns into a vector silently.
- **No mixing:** `f32x8 + f64x8` and `f32x8 + f32x4` are errors; convert with a cast (section 7).
- Build modes apply as for scalars: a mode whose `overflow()` is `wrap` makes `+ - *` wrap on vectors too (see [Build Modes](../../build/build-modes.md)).

**Methods** (lane by lane unless they say otherwise):

| Method | Result |
|---|---|
| `v.min(w)`, `v.max(w)`, `v.clamp(lo, hi)`, `v.abs()` | |
| `v.mul_add(b, c)` | `v * b + c` with one rounding (fused multiply-add) |
| `v.sqrt()`, `v.floor()`, `v.ceil()`, `v.round()`, `v.trunc()` | Floats only |
| `v.saturating_add(w)`, `v.saturating_sub(w)` | Integers only; clamp instead of overflowing |
| `v.reduce_add()`, `v.reduce_mul()`, `v.reduce_min()`, `v.reduce_max()` | One scalar from all lanes. Floats are added as a fixed pairwise tree, so the result is the same on every target for the same `N` |
| `v.reduce_and()`, `v.reduce_or()`, `v.reduce_xor()` | Integers only |
| `v[i]` | Lane `i` (a scalar); `i` out of range panics, a constant `i` out of range is a compile error |
| `v.with(i, x)` | A copy of `v` with lane `i` set to `x` |

### 4. Masks
A comparison gives a **`std::simd::mask<N>`**: one yes/no per lane.
```edax
std::simd::mask<8> hot = temperatures > f32x8::splat(80.0);
if (hot.any()) {
    std::log::warn(f"{hot.count()} sensors over 80 °C");
}
f32x8 limited = hot.select(f32x8::splat(80.0), temperatures);    // 80 where hot, the reading elsewhere
```
| On a mask | Result |
|---|---|
| `m.any()`, `m.all()`, `m.none()` | `bool` |
| `m.count()` | `usize`: lanes that are set |
| `m.first()` | `std::option<usize>`: the first lane that is set |
| `m.select(a, b)` | Lane by lane: `a` where set, `b` where not. The **only** per-lane condition |
| `m & n`, `m \| n`, `m ^ n`, `!m` | Combining masks |

- A mask **never converts to `bool`**, not even in a condition: `if (a > b)` on vectors is an error that suggests `.any()` or `.all()`.
- A mask has only a lane count, not an element type: a mask from comparing `f32x8`s can select between `f64x8`s. The compiler picks the hardware form (full-width lanes on AVX2, a mask register with AVX-512); converting between forms is one instruction.
- There is no branching per lane. Code that would branch per lane computes both sides and selects.

### 5. Memory: loads, stores and blocks
**Single loads and stores** on spans (see [Sequences](../../stdlib/containers/sequences.md)):

| Call | Does |
|---|---|
| `f32x8::load(s, i)` | Lanes `s[i] ... s[i + 7]`; panics if they aren't all inside the span |
| `v.store(mut s, i)` | Writes the lanes to `s[i] ... s[i + 7]`, same check |
| `f32x8::gather(s, indices)` | Lane `k` from `s[indices[k]]` (`indices` is a `u32x8` or `u64x8`); each index is checked |
| `v.scatter(mut s, indices)` | The reverse; two lanes with the same index is a panic |
| `load_unchecked`, `store_unchecked`, `gather_unchecked`, `scatter_unchecked` | The same without the checks; `unsafe` |

All loads and stores accept any alignment. `std::darray` and `std::array` of number types align their elements to the cache line, so vector loads from them never straddle one.

**Blocks** walk a range in steps of `N` and handle the end, so no remainder loop is ever written:
```edax
for (std::simd::block<8> at : std::simd::blocks<8>(n)) { ... }
```
- `std::simd::blocks<N>(n)` is a stream (see [Control Flow](../basics/control-flow.md)) of `std::simd::block<N>` values covering `0..n`. Every block is full (`N` elements) except possibly the last one.
- A block has `.start`, `.len()` (`N`, or fewer in the last block), `.active` (a `std::simd::mask<N>` of the lanes inside `0..n`) and `.is_full()`.
- `s.load(at)` on a `std::span<T>` gives a `vec<T, N>`; lanes past the end are `0` (they are never read from memory). `s.store(at, v)` on a `std::span<mut T>` writes only the active lanes. Both check the block against the span's length once per block.
- **Inactive lanes can't panic:** integer overflow and division checks only look at active lanes, so the zeros in a partial block are harmless.
- **What it compiles to:** a loop over the full blocks with plain vector loads and stores, followed by **one** step with masked loads and stores (native on AVX-512 and SVE; a short sequence elsewhere). There is no per-block branch in the full-block loop.

### 6. Choosing widths and CPU levels
- **The build decides the instruction set.** The target platform (see [Edax as a Build System](../../build/build-system.md)) has a CPU level, such as `x86_64_v2` (SSE4.2), `x86_64_v3` (AVX2, FMA) or `x86_64_v4` (AVX-512), or `armv8` / `armv9` with SVE. `std::simd::native_lanes<T>()` follows it.
- **One binary for several CPU levels:** `@std::simd::multiversion` compiles a function once per listed level and picks the best one for the running CPU at the first call:
  ```edax
  @std::simd::multiversion{ .levels = [std::build::cpu::x86_64_v2, std::build::cpu::x86_64_v3, std::build::cpu::x86_64_v4] }
  void blur_row(std::span<f32> input, std::span<mut f32> output) {
      compile_time usize w = std::simd::native_lanes<f32>();      // 4, 8 or 16, per version
      for (std::simd::block<w> at : std::simd::blocks<w>(output.len())) { ... }
  }
  ```
  The choice is made once per process (one indirect call per call afterwards). Functions it calls are compiled for the same level.
- **Requiring vectorized code:** `@std::simd::must_vectorize` on a function makes every plain `for` loop in it a compile error unless the compiler turns it into vector code, with the compiler's reason (a dependency between iterations, a call it can't inline, ...). Scalar loops can be kept scalar-looking and still be guaranteed fast.

### 7. Conversions
- `cast<f64x8>(i32x8)` converts lane by lane, with the scalar rules: lossless only (see [Casts](../basics/casts.md)). `cast::truncate`, `cast::saturate` and `cast::round` work the same way on vectors with the same `N`.
- `cast::checked<T>(v)` gives `std::option<T>`: `none` if any lane doesn't fit.
- `v.to_bits()` reinterprets the lanes as unsigned integers of the same width (`f32x8` → `u32x8`); `f32x8::from_bits(u)` the reverse. Both are safe.
- **Rearranging lanes:** `v.shuffle([3, 2, 1, 0, 7, 6, 5, 4])` takes lane `order[k]` of `v` for lane `k`; `a.shuffle_with(b, order)` picks from both (indices `0 .. 2N`). The order is a `compile_time` parameter (see [Functions](../functions/functions.md)), so it becomes a single shuffle instruction; an index out of range is a compile error. `v.reverse()`, `v.rotate_lanes(k)`, `a.interleave_low(b)` / `interleave_high(b)` are common shuffles with names.
- `v.split()` gives the two halves as a tuple (`f32x16` → `(f32x8, f32x8)`), `std::simd::join(lo, hi)` the reverse.

### 8. Raw intrinsics
Every instruction of the target is also available under its vendor's name, for the cases the portable operations don't cover:
```edax
unsafe {
    u8x32 shuffled = std::simd::x86::mm256_shuffle_epi8(bytes, table);     // _mm256_shuffle_epi8
    u8x16 looked_up = std::simd::arm::vqtbl1q_u8(table, idx);               // NEON vqtbl1q_u8
}
```
- The names are the vendor's names without leading underscores, so their manuals apply directly. They take and return Edax vector types (`__m256` is `f32x8`, `__m256i` an integer vector of 256 bits).
- They're `unsafe`, and using one the build's CPU level doesn't include is a compile error at the call (`std::build::target_has_cpu_feature(...)` lets code test for it in `if compile_time`).

### 9. Seeing what happens
`edaxc simd-report <file> [<function>]` lists, per function, every vector type with its hardware form (`f32x16: 2 × ymm`), every operation that was split or emulated, the masked tail steps, the multiversion copies, and the loops `@std::simd::must_vectorize` checked.

## Examples

### 1. The C++ example, side by side
```edax
void add_arrays(std::span<f32> a, std::span<f32> b, std::span<mut f32> result) {
    run_time check (a.len() == result.len() && b.len() == result.len()) else "add_arrays: lengths differ";
    for (std::simd::block<8> at : std::simd::blocks<8>(result.len())) {
        result.store(at, a.load(at) + b.load(at));
    }
}
```
| C++ | Edax |
|---|---|
| `__m256 va = _mm256_loadu_ps(&a[i]);` | `a.load(at)`: the type `f32x8` says 8 floats, the span checks the bounds |
| `_mm256_add_ps(va, vb)` | `+` |
| `_mm256_storeu_ps(&result[i], vr);` | `result.store(at, ...)` |
| `for (; i + 7 < size; i += 8)` and a second remainder loop | `std::simd::blocks<8>(n)`: the remainder is the last block |
| Unchecked pointers, sizes passed separately | Spans; one length check per block, none in the remainder loop |

The same loop for whatever the target's widest register is:
```edax
compile_time usize w = std::simd::native_lanes<f32>();
for (std::simd::block<w> at : std::simd::blocks<w>(result.len())) {
    result.store(at, a.load(at) + b.load(at));
}
```

### 2. Written out by hand
Blocks are a convenience, not magic. The same loop with single loads and stores:
```edax
mut usize i = 0;
while (i + 8 <= result.len()) {
    (f32x8::load(a, i) + f32x8::load(b, i)).store(mut result, i);
    i += 8;
}
for (usize k : i..result.len()) {
    result[k] = a[k] + b[k];
}
```

### 3. Clamping with a mask
```edax
void limit(std::span<mut f32> samples, f32 ceiling) {
    f32x8 top = f32x8::splat(ceiling);
    for (std::simd::block<8> at : std::simd::blocks<8>(samples.len())) {
        f32x8 v = samples.load(at);
        samples.store(at, (v > top).select(top, v));       // or simply v.min(top)
    }
}
```

### 4. A dot product
```edax
f32 dot(std::span<f32> a, std::span<f32> b) {
    run_time check (a.len() == b.len()) else "dot: lengths differ";
    mut f32x8 sum = default;
    for (std::simd::block<8> at : std::simd::blocks<8>(a.len())) {
        sum = a.load(at).mul_add(b.load(at), sum);         // inactive lanes load as 0, so they add nothing
    }
    return sum.reduce_add();
}
```

### 5. Parallel and vectorized
```edax
void scale_all(std::thread_pool& pool, mut std::darray<f32>& samples, f32 factor) {
    pool.for_chunks(samples.as_mut_span(), 16 * 1024, [factor](std::span<mut f32> part) {
        f32x8 f = f32x8::splat(factor);
        for (std::simd::block<8> at : std::simd::blocks<8>(part.len())) {
            part.store(at, part.load(at) * f);
        }
    });
}
```
Threads split the data into chunks (see [Thread Pool](../../stdlib/concurrency/thread-pool.md)); each chunk runs as vector code.

### 6. Finding the first match
```edax
std::option<usize> find_byte(std::span<u8> text, u8 wanted) {
    u8x32 w = u8x32::splat(wanted);
    for (std::simd::block<32> at : std::simd::blocks<32>(text.len())) {
        std::simd::mask<32> hits = (text.load(at) == w) & at.active;
        match (hits.first()) {
            std::some(k) => { return std::some(at.start + k); },
            std::none    => { },
        }
    }
    return std::none;
}
```
`& at.active` keeps the zeros of a partial last block from matching a wanted `0`.

## Rationale
- **Vectors are numbers with more lanes:** the operators, overflow rules, casts and build modes are the scalar ones, so there is nothing new to learn about *what* is computed, only that it happens `N` times.
- **The width is in the type, the lane count is exact:** `f32x8` is eight floats everywhere. Code never silently changes meaning on another CPU; `native_lanes` is the explicit way to ask for "as wide as this machine".
- **Masks are not booleans:** "are these vectors equal?" has no single answer. Making the question explicit (`any`, `all`) removes a whole class of bugs, and `select` is how SIMD hardware actually works.
- **Blocks instead of remainder loops:** the remainder loop is where hand-written SIMD most often goes wrong (off-by-one, forgetting it, a second code path that drifts). A masked last step keeps one code path, with no cost to the full blocks.
- **Checked by default, `unsafe` when asked:** a load checks its block once, which is negligible next to the work done on eight lanes; unchecked loads exist for the rare loop where it matters.
- **Multiversioning and `must_vectorize` are annotations:** both are visible on the function they affect, and both turn "hope the compiler does it" into something the build guarantees or reports.
- **Vendor names for raw intrinsics:** the manuals, tutorials and search results all use them. Renaming them would make every one of those useless.
