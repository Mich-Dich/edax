# Lesson 8: Compile-time code and checks

`constexpr`, `consteval`, `static_assert` and `assert` become one system in Edax, with two
keywords and one `check` statement.

## Functions

| Declaration | C++ | Called |
|---|---|---|
| `compile_time T f(...)` | `consteval` | only by the compiler, only with compile-time arguments |
| `compile_time run_time T f(...)` | `constexpr` | either way, with the same body rules |
| `T f(...)` | a normal function | at run time |

- Compile-time code may only call `compile_time` functions. Ordinary methods such as
  `array.len()` are run-time functions, so inside compile-time code use the template value `N`.
- It may not do I/O or touch `static mut`, and it runs on a step budget, so builds stay fast.
- A **compile-time value** is a literal, a `compile_time` constant, a template value, or a result
  of `compile_time` functions on those. An immutable local such as `u32 x = 5;` is a **run-time**
  value.
- `compile_time std::array<u8, 256> crc8 = crc8_table();` is computed while compiling and stored
  read-only. Containers, strings and structs of them can be built this way too.
- `compile_time u32 bits` as a **parameter** instantiates the function once per value, like a
  template value argument. Inside the body the parameter is a constant.

## `if compile_time`

```edax
if compile_time (std::build::mode_is(std::build::mode::debug)) { ... } else { ... }
```

The branch that isn't taken is not type-checked for this instance. It is used for build modes
(lesson 11), for type traits in templates (`std::is_integer<T>()`), and for anything else decided
at compile time.

## `check`: `static_assert` and `assert` in one statement

| Statement | When | Fails with |
|---|---|---|
| `compile_time check (c) else msg;` | while compiling | a compile error: nothing is built |
| `run_time check (c) else msg;` | when the program runs, in every build mode | a panic |
| `compile_time run_time check (c) else msg;` | at compile time when the values are known, else at run time | either one |
| `run_time(std::build::mode::debug) check ...` | only in builds of those modes | |

**Where a check is written decides when it runs:**
- **File scope** or a **type body**: once, or once per template instance. This is how you check
  layout: `compile_time check (std::size_of<header>() == 8) else ...;`.
- **The entry of a function** (the checks before any other statement): its preconditions,
  evaluated **at every call with that call's arguments**. A failed `compile_time` entry check is
  reported at the caller's line, with your message, and the compiler's own errors for that call
  are suppressed.
- **Anywhere else** in a body: `run_time` only (an assert).

The message is an f-string formatted by the compiler or at the panic. There is also a block form,
`compile_time check { ... std::compile_error(f"...") ... }`, for logic and for warnings with an id
(`std::compile_warning`).

## Annotations, briefly

`@std::derive::formattable` on a type generates its `format` method, so it can go into an
f-string. Annotations are ordinary structs the compiler evaluates. Library authors write their own
with checks and generated members; see `docs/design/language/compile-time/annotations.md`.

## Your goal

`exercise.ed` computes a CRC-8 lookup table at compile time, defines a protocol header,
`percent_to_byte` with a precondition, and `mask(bits)`. Fill in the TODOs so that the program
prints the lines below, and then panics on its last call:

```
crc8 of 123456789: 244
crc8 of [1, 2, 3]: 72
header { id: 7, kind: 2, flags: 0, length: 512 }
127 255
masks 15 4095 4294967295
built as debug
```

The panic message should read `180% is more than 100%`. The checker compares only stdout.

## Try this afterwards

1. Call `percent_to_byte(150)`, then `mask(0)`, then `mask(33)`. All three should fail to build,
   each with your message, at the call.
2. Add a `u8` field to `header`.
3. Build with `--mode=release` (`edaxc run --mode=release course/08-compile-time/exercise.ed`).
   Which line changes?
4. Make `checksum` an ordinary run-time function. Which line stops compiling, and why?
5. Turn the run-time check into `run_time(std::build::mode::debug) check`, and build with
   `--mode=release`.
