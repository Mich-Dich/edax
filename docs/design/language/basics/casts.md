# Casts

## Rules

There are no C-style casts (`(T)x`) and no constructor-style casts (`T(x)`). Every conversion is named after **what it may lose**:

| Spelling | Allowed conversions | On loss | Safe? |
|---|---|---|---|
| `cast<T>(x)` | **Lossless only**: integer widening, `u8`/`i8` → `f16`, `u8`/`u16`/`i8`/`i16` → `f32`, any ≤ 32-bit integer → `f64`, any ≤ 64-bit integer → `f128`, `f16` → `f32`/`f64`/`f128`, `f32` → `f64`/`f128`, `f64` → `f128`, simple enum → its tag type, `bool` → integer, a distinct type ↔ its underlying type | Compile error if any value could be lost | Yes |
| `cast::truncate<T>(x)` | Integer → narrower integer, signed ↔ unsigned; float → integer | Integers keep the low bits. A float loses its fraction (toward zero); a value outside the integer's range, or NaN, gives an unspecified integer | Yes |
| `cast::saturate<T>(x)` | Integer → integer, float → integer | Clamps to the range; NaN → 0 | Yes |
| `cast::round<T>(x)` | Float → narrower float, wide integer → float | Rounds to nearest | Yes |
| `cast::checked<T>(x)` | Any numeric → numeric | Returns `std::option<T>`: `none` if the value doesn't fit exactly | Yes |
| `cast::dynamic<T>(x)` | Base/interface reference or pointer → derived | Returns `std::option<T>` | Yes |
| `std::bit_cast<T>(x)` | The same bits as another type of the same size; both must be *plain bits* (numbers, SIMD vectors, arrays and structs of them without padding, see [Memory Layout](../user-types/layout.md)) | - | Yes |
| `cast::reinterpret<T>(x)` | Bit-level reinterpretation, integer ↔ `std::raw_ptr` | - | **`unsafe` only** |

A `char` is a Unicode scalar value (at most `0x10FFFF`): `cast<u32>(c)` (or any integer of at least 32 bits) is lossless. The other direction can fail, so it is `cast::checked<char>(n)`, which is `none` for surrogates and values above `0x10FFFF`.

```edax
u32 small = 300;
u64 wide  = cast<u64>(small);                     // OK: lossless
u8  a     = cast<u8>(small);                      // ERROR: may lose data
u8  b     = cast::truncate<u8>(small);            // 44
u8  c     = cast::saturate<u8>(small);            // 255
std::option<u8> d = cast::checked<u8>(small);     // std::none
i32 e     = cast::saturate<i32>(3.9);             // 3 (float → int: toward zero)
i32 g     = cast::truncate<i32>(3.9);             // 3 as well, with the CPU's one conversion instruction
std::option<circle&> f = cast::dynamic<circle&>(some_shape);
```

Upcasts (derived → base) are implicit (see [Built-in Types and Conversions](types.md)). Integer → `bool` is contextual only (see [Built-in Types and Conversions](types.md)); outside conditions, write `x != 0`.

## Rationale
- **Each lossy conversion has its own name:** in C++ `static_cast<u8>(x)` hides *how* data is lost. Here a reviewer can grep for `cast::truncate` and see exactly where data is dropped, and what the behaviour is.
- **Two float → integer casts:** `cast::saturate` is defined for every input and costs a few instructions more for it; `cast::truncate` is the single conversion instruction, for hot loops whose values are known to fit (the cell of a position in a grid or a noise lattice). Out of range it still gives *some* integer, never undefined behaviour, so it needs no `unsafe`.
- **`cast<T>` lossless only:** the short, common spelling can never lose data, so the safe choice is also the easiest one to type.
