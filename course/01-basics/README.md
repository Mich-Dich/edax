# Lesson 1: The C++ habits that don't compile

Edax reads like C++, but many everyday C++ habits are compile errors in Edax. This lesson covers variables, numbers, functions and control flow, which is where you hit them first.

## Variables: `=` only, immutable by default

```edax
u32 a = 5;               // immutable
mut u32 b = 5;           // mutable
b += 1;                  // OK; there is no ++ or --
u32 c;                   // ERROR: no uninitialized variables
u32 d{5};                // ERROR: initialize with '='
std::darray<u32> v = default;   // the type's default value (empty), instead of `T v;`
```

- `mut` belongs to the variable. A `mut` struct variable has every field mutable; fields have no `mut` (and nothing like `mutable` either).
- **No shadowing:** an `x` inside an `if` block is an error when the function already has an `x`.
- `auto` only works when the initializer already has a type on its own: `auto n = 5u32;` is fine, `auto n = 5;` is not.
- Globals need a storage keyword. `compile_time` is a constant (like `constexpr`), `static` is storage, and a `static mut` needs `unsafe` at every access.

## Numbers: no `int`, no silent conversions

- Integer types are `i8`…`i128`, `u8`…`u128`, `isize`, `usize`; floats are `f16`, `f32`, `f64`. Sizes and indices are `usize`.
- **A literal takes its type from context:** the declaration, the parameter, the other operand. `u64 x = a + 1;` with `a` a `u32` is an error, because there is no implicit widening. `f64 y = 0;` is an error too: write `0.0`.
- **Mixing types is always an error**, whether widths, signedness, or int with float. A conversion names what it may lose:

| C++ | Edax | |
|---|---|---|
| `(int64_t)x` (widening) | `cast<i64>(x)` | Lossless only; anything that can lose data is a compile error |
| `(uint8_t)x` | `cast::truncate<u8>(x)` | Keeps the low bits |
| `std::clamp` + cast | `cast::saturate<u8>(x)` | Clamps; float to int rounds toward zero |
| `(double)big_int` | `cast::round<f64>(x)` | `i64`/`usize` to `f64` can round |
| | `cast::checked<u8>(x)` | `std::option<u8>` |

- **Arithmetic is checked in every build mode**, release included. `u8 b = a + 100;` with `a == 200` stops the program with `panic: integer overflow in '+' (u8)`. If you want wrapping, say so with `+% -% *%`. There are also `std::checked_add` (an `std::option`) and `std::saturating_add`.
- Bit operators bind tighter than comparisons, so `flags & mask == 0` means what it looks like. `bit(4)` replaces `1 << 4`, and its type comes from context.
- Numbers have methods: `x.min(y)`, `x.clamp(lo, hi)`, `f.sqrt()`. Limits are `i16::max`, `u8::bits`, and so on.

## Functions

```edax
f64 mean(std::span<f32> values) { ... }          // the return type is always written
void bump(mut u32& x) { x += 1; }
bump(mut counter);                                // a mutable reference is visible at the call
```

- There are no default arguments and no named arguments: write an overload or pass a struct. Overloading is allowed, but a call that fits two overloads is an error: `f(5)` with `f(u32)` and `f(u64)` has to be written `f(5u32)`.
- `T&` is an immutable reference (like `const T&`); `mut T&` is a mutable one. **The caller writes `mut`.** Only a method's receiver (`obj.method()`) has no marker.
- **Tuples** group values: `(i16, i16) f()`, and `(i16 lo, i16 hi) = f();` unpacks them.
- **Returning text.** `std::str` is a borrowed view, like `std::string_view`. A function may return one only if it borrows from the function's single reference or view parameter. Text with no source to borrow from is a string literal, `std::static_str`. A function that makes new text returns `std::string`.

## Control flow

- Braces are mandatory. Assignment is a statement, so `if (x = 0)` can't be written.
- Integers and raw pointers convert to `bool` **only in conditions**: `if (count)` works, but `bool b = count;` does not.
- `if`, `match` and `{ }` blocks are expressions. The last expression without `;` is the value:
  ```edax
  std::static_str label = if (x > 0) { "positive" } else { "not positive" };
  ```
- `for` is range-based only: `for (usize i : 0..n)`, `0..=255`, `for (T& x : container)`, `for (mut T& x : container)`.
- `std::println(f"{x} and {y:.2}")` formats with f-strings: the holes are checked at compile time against the values' types.

## Your goal

`exercise.ed` reports a day of temperature readings (tenths of a degree, as `i16`). Fill in the TODOs so that the program prints exactly:

```
low -35, high 122, span 157 tenths
mean 3.89 degrees: cold
3 readings at 5.0 or above, 2 below zero
saturated 122, wrapped 66
```

Check it with `python3 course/check.py 1` from the repository root (with edaxc built in `build/`).

Things to notice along the way:
- `std::array<i16, 8>` doesn't turn into a `std::span<i16>` by itself: call `.as_span()`. Only a list literal `[a, b]` converts.
- The sum of `i16`s needs a wider type. Which of your casts can be plain `cast<...>` and which have to say how they may lose data?
- `describe` doesn't compile as written. The error message tells you why.

## Try this afterwards

Each of these is one line to add to `main`. Read the error, or the panic, each one gives:
1. `u8 x = 300;`
2. `u64 w = warm + 1u32;`
3. `if (warm > 0) { usize warm = 1; }`
4. `i16 boom = high * 300;` (it builds, then panics at run time)
5. `bool any = warm;`
