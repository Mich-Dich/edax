# Built-in Types and Conversions

## Rules

### Built-in types
| Type | Meaning |
|---|---|
| `i8` `i16` `i32` `i64` `i128` | Signed integers, two's complement |
| `u8` `u16` `u32` `u64` `u128` | Unsigned integers |
| `isize` `usize` | Pointer-sized integers. Used for sizes and indices (`len()` returns `usize`) |
| `f16` `f32` `f64` `f128` | IEEE-754 floats. `f16` (2 bytes) is for GPU and asset data: its arithmetic is rounded to `f16` after each operation (computed in `f32` where the CPU has no half-precision instructions). `cast<f32>(h)` is lossless; `u8`/`i8` convert losslessly to `f16`; anything wider needs `cast::round<f16>`. `f128` (binary128, 16 bytes, 16-byte aligned, 113-bit significand) is for precise reference computations, not hot code: it runs in software (libgcc/compiler-rt arithmetic, glibc's `sqrtf128`, `sinf128`, ...). Every integer up to 64 bits converts to it losslessly |
| `bool` | `true` / `false` |
| `char` | One Unicode scalar value (4 bytes) |
| `void` | "No value": a return type, or a template argument used only as a result or payload (`std::thread::handle<void>`, `std::result<void, E>`, see [Threads](../../stdlib/concurrency/threads.md)) |
| `never` | The type of expressions that never finish: `return`, `break`, `continue`, `std::panic(...)` |

There is no `int`. Fixed-size arrays are `std::array<T, N>` and growable arrays are `std::darray<T>` (see [The Standard Library](../../stdlib/README.md)). There are no C-style `T[N]` arrays.

### Limits of number types
Every number type has `compile_time` constants reached through its name:

| Constant | Integers | Floats |
|---|---|---|
| `T::min` | Smallest value (`i8::min` is `-128`, `u8::min` is `0`) | Most negative finite value |
| `T::max` | Largest value | Largest finite value |
| `T::bits` | Width in bits, as a `u32` | Width in bits, as a `u32` |
| `f32::infinity`, `f32::nan`, `f32::epsilon` (and `f16::...`, `f64::...`, `f128::...`) | - | IEEE special values and the machine epsilon |

The 8-bit floats are library types, not built-in ones: `std::f8e4m3` and `std::f8e5m2` (see [Utilities](../../stdlib/core/utilities.md#8-bit-floats)).

### Methods of numbers
Numbers have methods; the arguments take the receiver's type:

| Types | Methods |
|---|---|
| floats | `sqrt`, `abs`, `floor`, `ceil`, `round`, `trunc`, `min(x)`, `max(x)`, `clamp(lo, hi)`, `mul_add(y, z)`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2(x)`, `exp`, `exp2`, `log`, `log2`, `log10`, `pow(x)`, `copysign(x)`, `is_nan`, `is_infinite`, `is_finite` |
| integers | `abs` (panics on the minimum of a signed type), `min(x)`, `max(x)`, `clamp(lo, hi)` |

```edax
f32 length = (x * x + y * y).sqrt();
f32 t      = progress.clamp(0.0, 1.0);
f64 fused  = a.mul_add(b, c);          // a * b + c with one rounding, always
```
- `min` and `max` of floats ignore a NaN operand (IEEE `minNum`/`maxNum`). `clamp` with `lo > hi` panics.
- `mul_add` is always one fused operation, whatever `@std::float_mode` says (see [Performance](../performance/performance.md)).
- They are `compile_time run_time`: constants can use them. The bit functions (`std::count_ones`, ...) stay free functions (see [Core utilities](../../stdlib/core/utilities.md)).

### SIMD vectors
`f32x8`, `i32x4` and the other vector types hold several numbers that are processed together (see [SIMD](../simd/simd.md)).

### Tuples and lists
Tuples `(A, B)` group values of different types, and list literals `[a, b, c]` write several values of one type (see [Tuples and List Literals](tuples-and-lists.md)).

### Function types
`R(A, B)` is the type of a function taking `A` and `B` and returning `R`. It is used only as a template argument of the callable types `std::function<R(A, B)>` and `std::fn_ptr<R(A, B)>` (see [Lambdas](../functions/lambdas.md)).

### Literal typing
An unsuffixed literal **takes its type from context**. If there is no context, that is a compile error.

Context comes from:

1. the declared type of the variable being initialized or assigned,
2. the parameter type at a call,
3. the function's return type in `return`,
4. the other operand of a binary operator, if that operand's type is known,
5. the element or field type in `T{ ... }` and in container literals.

```edax
u32 a = 0;                 // OK: u32 from the declaration
u64 b = a + 1;             // ERROR: a is u32 and the result is u64 (no implicit widening)
u32 c = a + 1;             // OK: 1 becomes u32 from the other operand
u8  d = 300;               // ERROR: 300 doesn't fit in u8
f64 e = 0;                 // ERROR: an integer literal never becomes a float; write 0.0
auto f = 5;                // ERROR: no context; write 5u32 or `u32 f = 5;`
auto g = 5u32;             // OK: the suffix gives the type
if (3 < 4) { }             // ERROR: no context for either literal
```

### No implicit numeric conversion
Mixing integer widths, signedness or int/float in one operation is a compile error. Convert with `cast` (see [Casts](casts.md)).

### Contextual conversion to `bool`
In a **condition context**, integers and `std::raw_ptr` convert to `bool` implicitly:

| Source type | Becomes `true` when |
|---|---|
| Any integer type | value `!= 0` |
| `std::raw_ptr<T>` | pointer `!= nullptr` |

**Condition contexts:** the condition of `if` and `while`, operands of `!`, `&&` and `||`, and `match` guards.

Not converted: floats (NaN makes "is zero" ambiguous), `char`, enums, `std::option`, smart pointers (never null, see [Raw Pointers and `unsafe`](../memory/raw-pointers-and-unsafe.md)) and user-defined types.

```edax
if (count) { ... }                 // OK
if (flags & bit(4)) { ... }        // OK: u32 result, condition context
while (ptr) { ... }                // OK (std::raw_ptr)
bool b = count;                    // ERROR: not a condition context; write count != 0
take_flag(count);                  // ERROR: passing u32 to a bool parameter
if (ratio) { ... }                 // ERROR: f64 doesn't convert
```

### The complete list of implicit conversions
This is **every** implicit conversion in the language. Anything else needs a `cast` or a function call.

| From | To | Where | Note |
|---|---|---|---|
| Integer, `std::raw_ptr` | `bool` | Condition contexts | See above |
| `std::string` | `std::str` | Anywhere | Borrow, no cost |
| `std::static_str` (string literal) | `std::str` | Anywhere | No cost |
| `std::static_str` | `std::string` | Anywhere | **Compiler warning:** allocates. Use `std::string::from(...)` (see [Strings](../../stdlib/text/strings.md)) |
| `derived&` / pointer to derived | `base&` / pointer to base or interface | Anywhere | Upcast is safe and lossless (see [Inheritance and Interfaces](../user-types/inheritance-and-interfaces.md)) |
| Unsuffixed literal | Context type | Where context exists | See literal typing |
| List literal `[a, b]` | `std::array<T, N>` or `std::span<T>` | Where context exists | See [Tuples and List Literals](tuples-and-lists.md) |

## Rationale
- **Context-typed literals:** `u32 x = 0;` is clearly a `u32`, so requiring `0u32` would only be noise. When there's no context, guessing `i32` like C would hide a decision, so it's an error instead.
- **No int → float literals:** `f64 half = 1 / 2;` would otherwise be `0.5` here but `0` in C. Writing `1.0` removes the doubt.
- **Bool conversion only in conditions:** you get the convenience of `if (ptr)` and `if (flags & mask)` without the C++ pitfall where `5` silently binds to a `bool` parameter.
- **`usize`:** sizes depend on the target (32-bit embedded vs 64-bit desktop). Hard-coding `u64` would be wrong on small targets.
- **Methods on numbers:** `x.sqrt()` reads left to right and needs no `std::math::` prefix or overload set per type. Integers get no `sqrt`: the error says to convert first, so the precision is chosen in the code.
- **`f16` for data:** GPU buffers and compressed assets use half floats. Arithmetic on `f16` is rounded to `f16` after each operation, like every other type's; code that wants more precision converts with `cast<f32>(h)`, which is lossless.
- **`f128` built in, f8 in the library:** `f128` needs the compiler (an LLVM type, its own ABI, literals with 34 digits, exact compile-time evaluation). The two 8-bit formats are only storage with conversions, so a struct with a `u8` does the job and the language stays smaller. Two named types instead of one `f8`, because the formats differ in range and precision and choosing one is the point (E4M3 for precision, E5M2 for range).
- **Exact `f128` at compile time:** compile-time arithmetic, limits, casts and `bit_cast` on `f128` work on the binary128 value itself, not on a double, so a `compile_time` constant has the same bits as the run-time computation. Transcendental methods (`sqrt`, `sin`, ...) don't run at compile time for `f128`.
- **Formatting:** every float prints the shortest text that reads back as the same value of its own type: `0.1f16` prints `0.1` (not the f32 digits of the f16 value), `1.0f128 / 3.0f128` prints 34 digits.
