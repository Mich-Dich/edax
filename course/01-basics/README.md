# Lesson 1: The C++ habits that don't compile

Edax looks like C++ on purpose, so your hands already know most of it. The surprises are the places where C++ quietly does something you didn't ask for: an uninitialized variable, an `int` that becomes a `bool`, a `uint8_t` that wraps around, a shadowed name. Edax turns each of those into a compile error or a run-time check. This lesson goes through them in the order you will meet them.

**You will learn:** `mut` and initialization, the number types, casts that say what they may lose, checked arithmetic, function signatures and `mut` at the call, text return types, and expression-style control flow.

**You will build:** a temperature report (`exercise.ed`).

Every example in this lesson is a complete program in [`examples/`](examples). Run one with `build/compiler/edaxc run course/01-basics/examples/casts.ed`, change it, and see what happens. `python3 course/check.py 1 --examples` checks that they all still do what this page says.

---

## 1. Variables: `=` only, immutable by default

```edax
u32 a = 5;                         // immutable
mut u32 b = 5;                     // mutable
b += 1;                            // there is no ++ or --
std::darray<u32> list = default;   // the type's default value, instead of `std::vector<u32> list;`
auto n = 7u32;                     // auto needs a type of its own: here the suffix gives it
```

There is exactly one way to declare a variable: `[mut] type name = value;`. Every C++ variant that isn't that is an error, and each error tells you what to write instead:

| You write (C++ habit) | Edax says |
|---|---|
| `u32 c;` | `error[E2010]: variable 'c' has no initial value` - use `= default` for "the type's default" |
| `u32 d{5};` | `error[E2011]: initialize 'd' with '='` |
| `a = 6;` with `a` not `mut` | `error[E4020]: cannot assign to 'a': it is not mutable` + `note: declare it 'mut u32 a'` |
| `auto n = 5;` | `error[E4001]: this integer literal has no type` - write `5u32` or `u32 n = 5;` |
| a second `count` in an inner block | `error[E3028]: 'count' is already declared in this function` |

**Why immutable by default?** Most variables are written once. If `mut` is rare, it means something when you see it: a reader knows exactly which values can change. It also feeds the ownership checker in lesson 2.

**Why no shadowing?** In C++, an inner `count` that hides the outer one is a classic source of "I updated the wrong variable". In Edax a name means one variable for the whole function. Lesson 3 extends this to class members: a parameter can't be named like a field either.

Good to know:
- `mut` belongs to the *variable*. A `mut` struct variable has every field mutable; fields have no `mut` of their own, and there is nothing like C++'s `mutable`.
- `default` gives `0`, `0.0`, `false`, an empty container, `std::none`, or a struct whose fields all take their declared defaults. Types without a sensible default (a file, a `unique_ptr`) refuse it.
- Globals say what kind of storage they are: `compile_time u32 limit = 100;` is a constant (C++ `constexpr`), `static` is program-lifetime storage, and `static mut` needs `unsafe` at every access. A plain `u32 x = 5;` at file scope is an error.
- **Tuples** can be unpacked into new variables, `(i16 lo, i16 hi) = min_max(...);`, or assigned to existing ones: `(lo, hi) = (hi, lo);` is a swap.
- `view` and `check` are keywords only in front of `struct`/`class` and after `compile_time`/`run_time`, so a variable may be called `view` or `check`.

Example: [`variables.ed`](examples/variables.ed). The errors: [`err_immutable.ed`](examples/err_immutable.ed), [`err_uninitialized.ed`](examples/err_uninitialized.ed), [`err_brace_init.ed`](examples/err_brace_init.ed), [`err_shadowing.ed`](examples/err_shadowing.ed).

---

## 2. Numbers: no `int`, no silent conversions

| Kind | Types |
|---|---|
| signed integers | `i8` `i16` `i32` `i64` `i128` |
| unsigned integers | `u8` `u16` `u32` `u64` `u128` |
| sizes and indices | `usize` (what `length()` returns), `isize` |
| floats | `f16` (storage, GPU data), `f32`, `f64`, `f128` (exact reference math, slow) |
| 8-bit floats | library types `std::f8e4m3`, `std::f8e5m2` (machine-learning storage) |

There is no `int`, `long` or `double`: every type says its size.

### A literal takes its type from where it is used

`5` has no type of its own. It becomes a `u32` in `u32 x = 5;`, a `u8` when passed to a `u8` parameter, and an `i64` in `sum + 5` when `sum` is an `i64`. When nothing says which type, it's an error rather than a guess:

```edax
u32 a = 200;
u64 wide = a + 1;     // error[E4010]: expected 'u64', found 'u32'
f64 half = 0;         // error[E4003]: integer literal used as 'f64'   (write 0.0)
u8 small = 300;       // error[E4002]: 300 does not fit in 'u8'
i32 mixed = a;        // error[E4010]: expected 'i32', found 'u32'
bool any = a;         // error[E4010]: integers convert to bool only in conditions
```

Read the first line carefully, because it is the one that surprises C++ programmers most: `a + 1` is computed as a `u32` (the `1` takes `a`'s type), and a `u32` result does not widen into a `u64` by itself. Widen first: `u64 wide = cast<u64>(a) + 1;`.

**Mixing types in one operation is always an error**: different widths, signed with unsigned, integer with float. That removes the whole family of C++ bugs where `-1 < 0u` is false.

### Casts say what they may lose

| C++ | Edax | Meaning |
|---|---|---|
| `(int64_t)x`, widening | `cast<i64>(x)` | **lossless only**: a cast that could lose anything is a compile error |
| `(uint8_t)x` | `cast::truncate<u8>(x)` | keeps the low bits (`1000` becomes `232`) |
| `std::clamp` + cast | `cast::saturate<u8>(x)` | clamps into range (`-5` becomes `0`); float to int rounds toward zero |
| `(double)big_int` | `cast::round<f64>(x)` | to the nearest float: `i64`/`u64`/`usize` to `f64` can round |
| - | `cast::checked<u8>(x)` | an `std::option<u8>`: `none` when it doesn't fit |

So when the compiler refuses `cast<f64>(sum)` for an `i64 sum`, it's not being difficult: an `f64` has 53 bits of precision, so a large `i64` really can change. Writing `cast::round<f64>` says "I know, and rounding is fine here". `u16` or `u32` to `f64` is lossless, so plain `cast` works for them.

Integer division is integer division, as in C++: `7 / 2` is `3`. If you want `3.5`, convert both sides to a float *before* dividing. Converting the result afterwards only converts the `3`.

Example: [`casts.ed`](examples/casts.ed).

### Arithmetic is checked, in every build mode

An integer overflow doesn't wrap silently and it isn't undefined behaviour: it stops the program, also in release builds:

```
panic in main: integer overflow in 'level + 100' (u8): 200 + 100
```

When you *want* something else, say so:

| You want | Write |
|---|---|
| wrap around (hashes, counters, checksums) | `a +% b`, `a -% b`, `a *% b` |
| know whether it fit | `std::checked_add(a, b)`, an `std::option` |
| stop at the limit | `std::saturating_add(a, b)` |

The check costs very little: the optimizer removes the ones it can prove can't fail (lesson 12 shows how to see which remain). Example: [`overflow.ed`](examples/overflow.ed).

### Numbers have methods

`x.min(y)`, `x.max(y)`, `x.clamp(lo, hi)`, `x.abs()`; for floats also `sqrt`, `floor`, `round`, `pow`, `sin`, `mul_add` and more. Limits are constants on the type: `i16::max`, `u8::bits`, `f32::epsilon`. Bit operators bind tighter than comparisons, so `flags & mask == 0` means `(flags & mask) == 0`, and `bit(4)` replaces `1 << 4` (its type comes from context).

---

## 3. Functions

```edax
void bump(mut u32& counter) { counter += 1; }     // a mutable reference parameter

(u32, u32) min_max(std::span<u32> values) { ... }  // several results: a tuple

i32 main() {
    mut u32 clicks = 0;
    bump(mut clicks);                               // the caller writes `mut` too
    (u32 low, u32 high) = min_max(values.as_span());
}
```

- The return type is always written. There is no `auto f()`.
- `T&` is an immutable reference, like `const T&`; `mut T&` is a mutable one. **The caller writes `mut` as well**, so `bump(mut clicks)` tells the reader that `clicks` changes. Without it: `error[E4023]: a mutable reference argument is marked with 'mut'`. The only exception is the object a method is called on: `list.push(x)` has no marker.
- There are no default arguments and no named arguments. Write an overload, or take a struct of options (lesson 3 shows designated construction, which reads like named arguments).
- Overloading exists, but a call that fits two overloads is an error instead of a ranking puzzle: `twice(5)` with `twice(u32)` and `twice(u64)` is ambiguous. Write `twice(5u32)`.
- **`std::array` is not a `std::span`.** A function that takes a `std::span<u32>` gets `values.as_span()`. Only a list literal like `[1, 2, 3]` converts by itself.
- Lengths come from `.length()` (there is no `.size()`), and they are `usize`.

### Returning text

Edax has three text types, and the return type of a function decides which one you need:

| Type | Is | Return it when |
|---|---|---|
| `std::static_str` | a string literal, in the program's read-only memory | the result is always one of a few literals |
| `std::str` | a borrowed view, like `std::string_view` | the result is a piece of the function's **one** reference or view parameter |
| `std::string` | owned, growable text | the function makes new text |

The middle row is the one to understand. A `std::str` points into someone else's memory, so the compiler checks where that memory comes from. A function with no reference parameter has nothing to borrow from:

```
error[E5012]: 'describe' returns 'std::str' but has no reference parameter to borrow from
 --> err_return_text.ed:4:1
  |
4 | std::str describe(f64 mean) {
  | ^^^^^^^^ nothing to borrow from
  = note: return an owning value (e.g. std::string) instead
```

`std::string` would work, but it allocates for no reason. A function that returns one of a few literals returns `std::static_str`. Lesson 2 explains the borrowing rules behind this error.

Examples: [`functions.ed`](examples/functions.ed), [`err_functions.ed`](examples/err_functions.ed), [`err_return_text.ed`](examples/err_return_text.ed).

---

## 4. Control flow

- Braces are mandatory, even for one-line bodies. Assignment is a statement, so `if (x = 0)` can't be written by accident.
- Integers and raw pointers become `bool` **only in conditions**: `if (count)` and `if (flags & bit(2))` work, `bool b = count;` does not.
- `for` is range-based only:

  | Loop | Goes over |
  |---|---|
  | `for (usize i : 0..n)` | `0, 1, ..., n - 1` |
  | `for (u32 i : 1..=3)` | `1, 2, 3` |
  | `for (T& x : container)` | each element, by reference |
  | `for (mut T& x : container)` | each element, mutably (the container must be `mut`) |
  | `for (u32 x : container)` | each element, copied (copy types only) |

- **`if`, `match` and `{ }` blocks are expressions.** The last expression without `;` is the value, which replaces C++'s `?:` and the "declare, then assign in branches" pattern:

  ```edax
  std::static_str feel = if (t < 0) { "freezing" } else if (t < 15) { "cool" } else { "warm" };
  u32 squares = {
      mut u32 s = 0;
      for (u32 i : 1..=3) { s += i * i; }
      s                       // no `;`: this is the block's value
  };
  ```

- `std::println(f"...")` formats with f-strings. The holes are checked at compile time against the values' types: `{x}`, `{x:.2}` (2 decimals), `{x:+}` (always a sign), `{x:>5}` (right-aligned in 5 columns), `{x:x}` (hex). A literal inside a hole has no context, so it needs a suffix: `{3.14159f64:.2}`.

Example: [`control_flow.ed`](examples/control_flow.ed).

---

## C++ → Edax at a glance

| C++ | Edax |
|---|---|
| `int x;` | `i32 x = 0;` or `i32 x = default;` |
| `const int x = 5;` | `i32 x = 5;` |
| `int x = 5; x++;` | `mut i32 x = 5; x += 1;` |
| `constexpr int n = 8;` (global) | `compile_time i32 n = 8;` |
| `(uint8_t)x` | `cast::truncate<u8>(x)` (or `saturate`, `checked`) |
| `v.size()` | `v.length()` |
| `void f(int& x); f(x);` | `void f(mut i32& x); f(mut x);` |
| `void f(const T& x);` | `void f(T& x);` |
| `std::string_view` | `std::str` |
| `"literal"` as `const char*` | `std::static_str` |
| `cond ? a : b` | `if (cond) { a } else { b }` |
| `for (int i = 0; i < n; ++i)` | `for (usize i : 0..n)` |
| `printf("%.2f", x)` | `std::println(f"{x:.2}")` |

---

## Your goal

`exercise.ed` reports a day of temperature readings, stored as tenths of a degree in `i16` (`-35` means -3.5 degrees). Fill in the TODOs so that the program prints exactly:

```
low -35, high 122, span 157 tenths
mean 3.89 degrees: cold
3 readings at 5.0 or above, 2 below zero
saturated 122, wrapped 66
```

Check it from the repository root (with edaxc built in `build/`):

```
python3 course/check.py 1          # done, or a diff of what is still wrong
python3 course/check.py 1 --show   # also print everything your program prints
```

The exercise starts with **one compile error on purpose** (`describe`). Fix that first; after that it builds and prints wrong numbers until you fill in the TODOs.

<details>
<summary>Hint 1: <code>min_max</code> starts from which values?</summary>

To find the smallest value, start from the *largest* possible one, so that any reading is smaller: `mut i16 low = i16::max;`, and the other way round for `high`. Then `low = low.min(v);` for every value.
</details>

<details>
<summary>Hint 2: the mean comes out as 3 or doesn't compile</summary>

Add the readings up in an `i64` (`sum += cast<i64>(v);`, lossless). Then divide **as floats**: both `sum` (an `i64`) and `values.length()` (a `usize`) need `cast::round<f64>`, because both are 64 bits wide. Dividing first and converting afterwards gives an integer result. Finally divide by `10.0` to go from tenths to degrees.
</details>

<details>
<summary>Hint 3: <code>describe</code></summary>

All three results are literals, so the return type is `std::static_str`, and the body is one `if` expression: `return if (mean < 0.0) { "freezing" } else if ... ;`
</details>

<details>
<summary>Hint 4: the last line</summary>

`cast::saturate<u8>(high)` clamps `122` into a `u8` (it fits, so it stays 122). Then `level +% 200` wraps: 322 - 256 = 66.
</details>

---

## Try this afterwards

Add each line to `main` of your finished exercise, one at a time, and read what happens. Then open the answer.

1. `u8 x = 300;`
2. `u64 w = warm + 1u32;`
3. `if (warm > 0) { usize warm = 1; }`
4. `i16 boom = high * 300;`
5. `bool any = warm;`
6. `u32 n = readings.size();`

<details>
<summary>What happens</summary>

1. `error[E4002]: 300 does not fit in 'u8'`, with a note that `u8` holds 0 to 255.
2. `error[E4011]: operands have different types: 'usize' and 'u32'`. Two different types in one `+` is always an error, whatever the result type.
3. `error[E3028]: 'warm' is already declared in this function`. No shadowing.
4. It builds, then stops: `panic in main: integer overflow in 'high * 300' (i16): 122 * 300`. The compiler can't know the value of `high`, so the check runs when the program does.
5. `error[E4010]: expected 'bool', found 'usize'`, with the note `write 'x != 0'`.
6. `error[E4070]: 'std::array<i16, 8>' has no method 'size'`, with the note `'size' is the C++ name; in Edax it is 'length'`. And `length()` returns a `usize`, so `u32 n` would need a cast too.
</details>

## Recap

- One declaration form: `[mut] type name = value;`. Immutable by default, no uninitialized variables, no shadowing.
- Literals take their type from context. Types never mix, and casts name what they may lose.
- Overflow panics in every build. `+%` wraps, `std::checked_add` asks, `std::saturating_add` clamps.
- `mut` is written at the call for mutable references. `std::array` → `std::span` is `.as_span()`.
- Return `std::static_str` for literals, `std::str` for a piece of your one parameter, `std::string` for new text.
- `if`, `match` and blocks are expressions.

Next: [lesson 2](../02-ownership/README.md), where the compiler starts tracking who owns what.
