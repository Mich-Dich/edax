# Lesson 4: Enums, `match` and errors without exceptions

One `enum` construct covers both C++'s `enum class` and `std::variant`. Errors are values. There is
no `throw`, and a panic ends the process.

## Enums

```edax
enum op : u8 { add, sub, mul, div }          // the tag type is mandatory; always scoped: op::add

enum token : u8 {                            // alternatives may carry ONE value each
    number(i64),
    apply(op),
}
token t = token::number(42);                 // built with (), unlike types, which use {}
```

- Several values go in a struct payload: `circle(circle_data)`.
- Converting needs a function: `cast<u8>(op::mul)` gives the tag, and `op::from_value(2)` gives an
  `std::option<op>`.
- An enum is a copy type when its payloads are.

## `match` and `is`

```edax
std::static_str name = match (o) {
    op::add => "plus",
    op::sub => "minus",
    _ => "other",                     // every arm ends with ','
};
```

- `match` is an expression, it must be **exhaustive**, and there is no fallthrough.
- Patterns: literals, ranges `0..=9`, alternatives `token::number(n)`, struct fields
  `item{ .weight = w, .. }`, `_`, and guards `n if n > 100`. A guarded arm doesn't count toward
  exhaustiveness.
- Bindings **borrow** when you match a variable, become mutable with `match (mut v)`, and **own**
  when you match a temporary or `move v`.
- `if (x is std::some(v)) { ... }` and `while (q.pop() is std::some(job)) { ... }` are one-arm
  matches written as a condition.
- `match` works on strings as well: `match (word) { "+" => ..., _ => ... }`.

## `std::option` and `std::result`

- `std::option<T>` is `std::some(x)` / `std::none`; `std::result<T, E>` is `std::ok(x)` /
  `std::error(e)`. The bare names `std::ok`, `std::error`, `std::some` and `std::none` get their
  template arguments from context.
- **A result can't be ignored:** calling `f();` when `f` returns a result is a compile error.
  Write `_ = f();` to drop it on purpose.
- **`r?`** unwraps an `ok` or returns the error from the current function. The error types must
  be **identical**; nothing converts them implicitly. Change them with `.or_error(e)` (this also
  turns an option into a result) or `.map_error(fn)`.
- Other helpers: `.value_or(x)`, `.expect(f"why")` (panics), and `.is_ok()` / `.is_some()` to look
  without taking the value.

## Panics are the end

Overflow, a bad index, `std::panic(...)`, `.expect` on an error, and a failed `run_time check` all
**abort the process**:
- there is no unwinding;
- destructors don't run;
- other threads don't survive.

A panic hook (`std::set_panic_hook`) may flush logs, but it can't stop the abort. Anything the
program should recover from is a `result`.

## Your goal

`exercise.ed` is a reverse-Polish calculator (`"3 4 + 2 *"` is `(3 + 4) * 2`). The enums and
`main` are written. Implement `read_token`, `apply`, `evaluate` and `describe` so that the program
prints:

```
3 4 + 2 * = 14
10 2 8 * + 3 - = 23
1 +: an operator is missing an operand
4 0 /: division by zero
2 x *: word 1 is not a number or an operator
1 2 3: 3 values left over
```

Aim for these:
- No `if` chains where a `match` says it. `apply` is a single `match` expression.
- Every failure travels up with `?`: `evaluate` has exactly one explicit `return std::error(...)`.
- `describe` uses a guard for the "nothing to compute" case.

## Try this afterwards

1. Delete the `op::div` arm in `apply`. Read the error.
2. Call `read_token("7", 0);` as a statement.
3. Make `apply` return `std::result<i64, std::str>` and keep `stack.push(apply(o, a, b)?);` in
   `evaluate`. Fix it with `.map_error` and a lambda (lesson 7 has the details).
4. Evaluate `"9223372036854775807 1 +"`. What happens, and what would turn it into a
   `calc_error` instead (`std::checked_add`)?
