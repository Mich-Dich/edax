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
