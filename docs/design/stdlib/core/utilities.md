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

### The program
- `std::exit_code` is the simple enum `success`, `failure` that `main` may return (see [Functions](../../language/functions/functions.md)).
- `std::panic(message)` ends the program (see [Error Handling](../../language/errors/error-handling.md)); `std::set_panic_hook` installs a hook that runs first.

## Rationale
- **Overflow behaviour in the name:** `checked_`, `saturating_` and the `%` operators say what happens on overflow, so a reader never has to guess.
- **Structural std interfaces:** printing, comparing, cloning and hashing work for every type that has the needed operation, without a declaration that would have to live next to the type.
