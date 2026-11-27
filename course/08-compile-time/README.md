# Lesson 8: Compile-time code and checks

C++ grew `constexpr`, `consteval`, `static_assert`, `assert`, `if constexpr` and template metaprogramming one at a time, each with its own rules. Edax has **two keywords**, `compile_time` and `run_time`, and **one statement**, `check`. Compile-time code is ordinary code: the same loops, the same types, the same standard library.

**You will learn:** `compile_time` and `compile_time run_time` functions, compile-time constants and tables, `compile_time` parameters, `if compile_time`, types as compile-time values, and the `check` statement in its three positions.

**You will build:** a CRC-8 table computed by the compiler, a checked protocol header, and APIs whose preconditions are checked at the call (`exercise.ed`).

Examples are in [`examples/`](examples); `python3 course/check.py 8 --examples` checks them.

---

## 1. Compile-time functions and values

| Declaration | Runs | C++ |
|---|---|---|
| `compile_time T f(...)` | only in the compiler, only with compile-time arguments | `consteval` |
| `compile_time run_time T f(...)` | in the compiler or at run time, same body rules | `constexpr` |
| `T f(...)` | at run time | a normal function |

```edax
compile_time u64 factorial(u64 n) {
    mut u64 r = 1;
    for (u64 i : 2..=n) { r *= i; }
    return r;
}
compile_time u64 f10 = factorial(10);                    // computed while compiling
compile_time std::array<u32, 5> table = squares();       // stored read-only: no startup code
```

**What is a compile-time value?** A literal, a `compile_time` constant, a template value parameter, or a result of `compile_time` functions on those. That's all. In particular, an immutable local is **not** one:

```edax
u64 n = 5;                       // a run-time value, even though it never changes
u64 f = factorial(n);            // error[E4200]: 'n' is not a compile-time value
```

The rule is simple on purpose. In C++, whether `const int n = 5;` is usable in a constant expression depends on its type and initializer; in Edax you say `compile_time u64 n = 5;` and it is.

What compile-time code can do:
- Loops, `if`, `match`, structs, enums, `std::array`, and containers and strings built while compiling (a `compile_time` table can be an `std::darray` or a struct of them).
- Call `compile_time` functions. Ordinary methods (like most container methods) are run-time functions; inside compile-time code, prefer template values such as `N` over `array.length()`.
- No I/O, no `static mut`. It runs on a step budget, so a mistake can't hang the build.

A function `compile_time run_time` is checked under both sets of rules, so the same `checksum` can be called while compiling (`compile_time u8 known = checksum([1, 2, 3]);`) and at run time.

### Types are values too

At compile time, a type is a value of type `type`. A `compile_time` function may return one, and a named constant then works as a type (lesson 6 showed aliases):

```edax
compile_time type counter_for(u64 max) {
    return if (max <= 255) { u8 } else { u32 };
}
compile_time type small = counter_for(100);
small c = 200;                   // a u8
```

### `compile_time` parameters

```edax
u32 mask(compile_time u32 bits) { ... }
```

A `compile_time` parameter makes one instance of the function per value, like a template value argument, but called like a normal function: `mask(4)`. Inside, `bits` is a constant, so it can be checked and branched on at compile time.

Examples: [`compile_time.ed`](examples/compile_time.ed), [`err_compile_time.ed`](examples/err_compile_time.ed).

---

## 2. `if compile_time`

```edax
std::static_str mode = if compile_time (std::build::mode_is(std::build::mode::debug)) { "debug" }
                       else { "optimized" };
```

The branch that isn't taken isn't type-checked for this build or instance (C++'s `if constexpr`). It's how code depends on build modes (lesson 11), on type traits in templates (`std::is_integer<T>()`, lesson 6), and on `compile_time` parameters (`if compile_time (bits == 32)`, where `bit<u32>(32)` wouldn't exist).

---

## 3. `check`: `static_assert` and `assert` in one statement

```edax
compile_time check (condition) else "message";
```

| Statement | Runs | Fails with |
|---|---|---|
| `compile_time check (c) else msg;` | while compiling | a compile error: nothing is built |
| `run_time check (c) else msg;` | when the program runs, **in every build mode** | a panic (lesson 4) |
| `compile_time run_time check (c) else msg;` | while compiling when the values are known, else at run time | either |
| `run_time(std::build::mode::debug) check ...` | at run time, only in builds of those modes | a panic |

The message is an f-string, formatted by the compiler (for a compile error) or at the panic.

### Where a check is written decides when it runs

| Position | Runs | Typical use |
|---|---|---|
| **file scope**, or **a type's body** | once (once per template instance) | layout, sizes, configuration: `static_assert` |
| **the entry of a function** (checks before any other statement) | **at every call, with that call's arguments** | preconditions |
| **anywhere else** in a body | `run_time` only | `assert` |

A check in a type body:

```edax
struct header {
    u16 id = 0;
    u8  kind = 0;
    u32 length = 0;
    u8  flags = 0;                // a new field: the header grows
    compile_time check (std::size_of<header>() == 8) else f"header is {std::size_of<header>()} bytes";
}
```
```
error[E4210]: header is 12 bytes
```

**Entry checks are the interesting kind.** They're a function's preconditions, and they are checked *per call*: when the arguments are known while compiling, by the compiler; otherwise, when the program runs.

```edax
u8 percent_to_byte(u32 percent) {
    compile_time run_time check (percent <= 100) else f"{percent}% is more than 100%";
    return cast::truncate<u8>(percent * 255 / 100);
}

percent_to_byte(50);          // fine
percent_to_byte(150);         // a compile error, at THIS line
percent_to_byte(user_value);  // checked when it runs: a panic if user_value > 100
```

The error is reported **at the caller's line**, with your message, and notes where the check is:

```
error[E4210]: 150% is more than 100%
  --> err_checks.ed:15:12
   |
15 |     u8 b = percent_to_byte(150);
   |            ^^^^^^^^^^^^^^^^^^^^ this call fails the check
   |
 5 |     compile_time run_time check (percent <= 100) else f"{percent}% is more than 100%";
   |                                  --------------
   = note: the check of 'percent_to_byte' is here
```

That's what makes library errors readable: `sort_by` (lesson 7) uses an entry check to say "less must return bool" in one line at your call, instead of a page of errors from inside the library. When an entry check fails, the compiler's own errors for that call are suppressed.

There is also a block form for more logic, and for warnings with an id: `compile_time check { ... std::compile_error(f"..."); ... }` and `std::compile_warning`.

Examples: [`checks.ed`](examples/checks.ed), [`err_checks.ed`](examples/err_checks.ed), [`err_layout.ed`](examples/err_layout.ed).

---

## 4. Annotations, briefly

`@std::derive::formattable` before a type generates its `format` method, so the type can go into an f-string hole. Annotations are ordinary structs that the compiler evaluates; they can check the type they're attached to and generate members. Library authors write their own, see `docs/design/language/compile-time/annotations.md`. (`edaxc expand file.ed type` shows what an annotation generates.)

---

## C++ → Edax at a glance

| C++ | Edax |
|---|---|
| `consteval int f()` | `compile_time i32 f()` |
| `constexpr int f()` | `compile_time run_time i32 f()` |
| `constexpr int n = 8;` | `compile_time i32 n = 8;` |
| `static constexpr std::array t = make();` | `compile_time std::array<...> t = make();` |
| `template<int Bits> uint32_t mask()` + `mask<4>()` | `u32 mask(compile_time u32 bits)` + `mask(4)` |
| `if constexpr` | `if compile_time` |
| `static_assert(c, "msg")` | `compile_time check (c) else "msg";` |
| `assert(c)` (debug only) | `run_time(std::build::mode::debug) check (c) else "msg";` |
| `assert` that stays in release | `run_time check (c) else "msg";` |
| precondition (`[[pre: ...]]`, C++26) | an entry check |
| `std::conditional_t<...>` | a `compile_time` function returning a `type` |

---

## Your goal

`exercise.ed` computes a CRC-8 lookup table at compile time, defines a protocol header, `percent_to_byte` with a precondition, and `mask(bits)`. Fill in the TODOs so that the program prints the lines below, and **then panics on its last call** (that's intended):

```
crc8 of 123456789: 244
crc8 of [1, 2, 3]: 72
header { id: 7, kind: 2, flags: 0, length: 512 }
127 255
masks 15 4095 4294967295
built as debug
```

The panic message should read `180% is more than 100%`. The checker compares only stdout.

<details>
<summary>Hint 1: the CRC table</summary>

For each `i` in `0..256`: start with `mut u8 c = cast::truncate<u8>(i);`, then 8 times: `if (c & 0x80) { c = (c << 1) ^ 0x07; } else { c = c << 1; }`. The shift drops the top bit, as a CRC wants (`<<` doesn't panic on bits shifted out).
</details>

<details>
<summary>Hint 2: <code>checksum</code></summary>

`crc = crc8[cast<usize>(crc ^ b)];` for every byte. `u8` to `usize` is lossless, so plain `cast`.
</details>

<details>
<summary>Hint 3: <code>mask</code></summary>

Make the parameter `compile_time u32 bits`. Its entry check refuses 0 and anything above 32. Then `if compile_time (bits == 32) { return u32::max; } else { return bit<u32>(bits) - 1; }`: `bit<u32>(32)` doesn't exist, and the branch that isn't taken isn't compiled.
</details>

<details>
<summary>Hint 4: the precondition</summary>

`compile_time run_time check (percent <= 100) else f"{percent}% is more than 100%";` as the first statement of `percent_to_byte`.
</details>

---

## Try this afterwards

1. Call `percent_to_byte(150)`, then `mask(0)`, then `mask(33)`. Each should fail to build with your message, at the call.
2. Add a `u8` field to `header`.
3. Build with `--mode=release` (`build/compiler/edaxc run --mode=release course/08-compile-time/exercise.ed`). Which line changes?
4. Make `checksum` an ordinary run-time function (remove `compile_time run_time`). Which line stops compiling, and why?
5. Turn the run-time check into `run_time(std::build::mode::debug) check`, and build with `--mode=release`.

<details>
<summary>What happens</summary>

1. Three `error[E4210]`s, each at its call, each with your message.
2. Your layout check fails: `error[E4210]` with the size you print in the message.
3. `built as optimized`: `mode_is(debug)` is true for `debug` and modes derived from it.
4. `compile_time u8 known = checksum([1, 2, 3]);`: a `compile_time` constant can only call `compile_time` functions.
5. In release the check is gone: there's no panic, and the last line prints the truncated byte (`180 * 255 / 100 = 459`, truncated to a `u8`: `203`).
</details>

## Recap

- `compile_time` = consteval, `compile_time run_time` = constexpr, and only literals, constants and template/compile-time parameters are compile-time values.
- Tables and types can be computed by ordinary compile-time code.
- One `check` statement; its position decides when it runs. Entry checks are preconditions, checked at each call, at compile time when possible.

Next: [lesson 9](../09-containers-and-text/README.md), containers and text.
