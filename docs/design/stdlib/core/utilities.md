# Core Utilities

Small building blocks used everywhere: ownership helpers, arithmetic with explicit overflow behaviour, bit operations, hashing, ordering and the program's entry and exit.

## Rules

### Ownership helpers
| Function | Does |
|---|---|
| `std::take(mut place)` | Returns the value and leaves the type's `default` behind (moving out of a field, see [Ownership, Copies, Moves and References](../../language/memory/ownership-and-references.md)) |
| `std::replace(mut place, value)` | Returns the old value and stores `value` |
| `std::swap(mut a, mut b)` | Exchanges two values |

### Arithmetic
| Function | Result |
|---|---|
| `std::checked_add/sub/mul(a, b)` | `std::option<T>`: `none` on overflow |
| `std::saturating_add/sub/mul(a, b)` | Clamped to `T::min` / `T::max` |

Wrapping arithmetic is the operators `+% -% *%` (see [Operators](../../language/basics/operators.md)).

### 8-bit floats
`std::f8e4m3` and `std::f8e5m2` are the two OCP FP8 formats: one byte each, for storage (network weights, compressed vertex attributes, HDR data in memory). They are structs holding the bits (`.bits`), so arrays of them are plain bytes.

| | `std::f8e4m3` | `std::f8e5m2` |
|---|---|---|
| Layout | 1 sign, 4 exponent (bias 7), 3 mantissa bits | 1 sign, 5 exponent (bias 15), 2 mantissa bits: the top byte of an `f16` |
| Largest finite | 448 | 57344 |
| Smallest positive | 2^-9 | 2^-16 |
| `epsilon()` | 0.125 | 0.25 |
| Infinities | none | `infinity()` |
| NaN | `0x7F`, `0xFF` | exponent all ones, mantissa not 0 |
| `from_f32` beyond the range | NaN | ±inf |

- **Conversions:** `from_f32(v)` rounds to nearest, ties to even. `saturate(v)` clamps values beyond the range (and ±inf) to ±max; NaN stays NaN. `from_bits(b)` takes the raw byte. `to_f32()` and `to_f16()` are exact.
- **Arithmetic:** `+ - * /` convert both sides to `f32`, compute, and round back once. This is the correctly rounded result, because `f32` has more than twice their precision. Unary `-` flips the sign bit.
- **Comparison:** `==` is IEEE (NaN is not equal to itself, `0` equals `-0`). There is no `<=>`; `total_cmp` orders by value with NaN last, for sorting. For IEEE `<`, compare `a.to_f32() < b.to_f32()`.
- **Limits** are functions: `max()`, `min()`, `epsilon()`, `min_positive()`, `nan()`, and `infinity()` for E5M2. `is_nan()`, and `is_infinite()` for E5M2.
- Everything except formatting runs at compile time: `compile_time std::f8e4m3 k = std::f8e4m3::from_f32(0.3);`.
- Formatting prints the exact value through `f32` (`0.1015625` for `from_f32(0.1)` in E4M3).

```edax
std::f8e4m3 w = std::f8e4m3::from_f32(0.1);    // 0.1015625
std::f8e5m2 g = std::f8e5m2::saturate(1.0e9);  // 57344
f32 sum = w.to_f32() + g.to_f32();             // accumulate in f32
```

### Facts for the optimizer
| Function | Meaning |
|---|---|
| `std::assume(condition)` | The optimizer may rely on `condition`. A false one panics at the call in every mode that doesn't allow undefined behaviour |
| `std::unreachable()` | `never`: panics when reached in a safe mode; the path is removed where undefined behaviour is allowed |
| `std::bit_cast<T>(x)` | The bits of `x` as a `T` of the same size; both plain bits (see [Memory Layout](../../language/user-types/layout.md)) |
| `std::padding<N>` | `N` bytes of explicit padding in a struct: zero, never read |
| `std::nonzero<T>` | An integer that is never 0, so `std::option<std::nonzero<T>>` is the size of `T`. `nonzero<T>::make(v)` checks `v`, `nonzero<T>::of(v)` gives `none` for 0, `.get()` reads it |
| `@std::never_zero` | On an integer or pointer field: a promise that it is never 0 in a live value, so options of the type need no tag (see [Memory Layout](../../language/user-types/layout.md)) |

### Bits
`std::trailing_zeros(x)`, `std::leading_zeros(x)`, `std::count_ones(x)` (all `u32`), `std::rotate_left(x, n)`, `std::rotate_right(x, n)`: on any integer type, one machine instruction each. `bit(n)` is a language expression (see [Operators](../../language/basics/operators.md)).

### Hashing
- `std::hashable` is a structural interface: a type implements it by having a `u64 hash()` method (written by hand, or generated with `@std::derive::hashable`, see [Annotations](../../language/compile-time/annotations.md)). Integers, `char`, `bool` and the string types implement it.
- `std::hash(value&) → u64` hashes any `std::hashable`. `std::hash_seed()` and `std::hash_combine(a, b)` help write `hash()`; `std::hash_bytes(p, n)` (unsafe) hashes raw bytes.
- The hash is wyhash-style (64×64→128 multiply-fold, 8 bytes at a time), **seeded per process** from an address randomized by the OS.

### Ordering and sorting
- `std::ordering` is the enum `less`, `equal`, `greater` returned by `operator<=>` (see [Operators](../../language/basics/operators.md)).
- `std::sort_order::ascending` / `descending` choose the direction of `sort_by_key` (see [Sequences: `std::darray`, `std::array`, `std::span`, `std::bitset`](../containers/sequences.md)).

### Structural interfaces
| Interface | Implemented by |
|---|---|
| `std::comparable` | Numbers, `char`, strings, and types with `operator<=>` |
| `std::cloneable` | Everything except classes without `clone()` |
| `std::formattable` | What an f-string can print: numbers, `bool`, `char`, strings, simple enums, and types with a `format` method (or `@std::derive::formattable`) |
| `std::hashable` | See above |
| `std::sendable`, `std::shareable` | Derived from a type's fields (see [Threads](../concurrency/threads.md)) |

A structural interface is implemented by having what it requires; nothing is written in the base list.

### Versions
`std::version` is a version number: three `u32` fields, `major`, `minor` and `patch`.
```edax
std::version vulkan = std::version{ .major = 1, .minor = 4, .patch = 320 };
std::version gl     = std::version{ .major = 3, .minor = 3 };            // patch 0
std::println(f"{vulkan}");                                               // 1.4.320
bool newer = vulkan > std::version{ .major = 1, .minor = 3 };            // true: field by field, major first
bool enough = vulkan.at_least(std::version{ .major = 1, .minor = 3 });   // true; usable in compile-time code
```
- `==`, `<=>` compare field by field, major first; the version prints as `major.minor.patch`.
- `at_least(other)` is `compile_time run_time`: a `compile_time check` can ask whether a version is new enough. It is what [`std::graphics`](../graphics/graphics.md) uses to check API versions.
- Graphics APIs (`std::graphics::api`), libraries and file formats use it; a version is never three loose integers.

### The program
- `std::exit_code` is the simple enum `success`, `failure` that `main` may return (see [Functions](../../language/functions/functions.md)).
- `std::panic(message)` ends the program (see [Error Handling](../../language/errors/error-handling.md)); `std::set_panic_hook` installs a hook that runs first.

## Rationale
- **Overflow behaviour in the name:** `checked_`, `saturating_` and the `%` operators say what happens on overflow, so a reader never has to guess.
- **One version type:** an API's version, a library's and a file format's are the same three numbers with the same comparison. Writing them as one type keeps "is 1.4.320 at least 1.3?" one correct function, not a comparison every program writes again.
- **Structural std interfaces:** printing, comparing, cloning and hashing work for every type that has the needed operation, without a declaration that would have to live next to the type.
