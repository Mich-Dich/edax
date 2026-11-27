# Lesson 4: Enums, `match` and errors without exceptions

C++ has three ways to say "one of several things" (`enum class`, `std::variant`, a class hierarchy) and three ways to report failure (exceptions, error codes, `std::expected`). Edax has one of each: an `enum` whose alternatives may carry a value, and errors as ordinary values of type `std::result`. There is no `throw`. What can't be handled, a **panic**, ends the process.

**You will learn:** payload enums, exhaustive `match` and its patterns, `is` in conditions, `std::option` / `std::result`, the `?` operator, and what a panic is (and isn't).

**You will build:** a reverse-Polish calculator (`exercise.ed`).

Examples are in [`examples/`](examples); `python3 course/check.py 4 --examples` checks them.

---

## 1. Enums: `enum class` and `std::variant` in one

```edax
enum op : u8 { add, sub, mul, div }      // the tag type is required; names are always scoped: op::add

enum shape : u8 {                        // alternatives may carry ONE value each
    point,
    circle(circle_data),                 // several values: put them in a struct
    square(f32),
}

shape s = shape::circle(circle_data{ .radius = 2.0 });   // built with (), like a call
op o = op::mul;                                          // no payload: just the name
```

- An alternative with a payload is built with `()`: it's a *value constructor*, not a type. (Types are built with `{}`, lesson 3.)
- The tag type (`: u8`) is mandatory and decides the tag's size. `cast<u8>(op::mul)` gives `2`.
- An enum is a copy type when all its payloads are.
- Think of `shape` as `std::variant<std::monostate, circle_data, float>` where each alternative has a name, and the compiler checks that you handle all of them.

## 2. `match`

```edax
f32 area(shape s) {
    return match (s) {
        shape::point => 0.0,
        shape::circle(c) => 3.0 * c.radius * c.radius,
        shape::square(side) => side * side,
    };
}
```

- `match` is an **expression**: it has a value, so it can be returned or assigned. Every arm ends with `,`, and an arm whose body is a block (`=> { ... }`) gives the block's value.
- It must be **exhaustive**. Forget an alternative and the build stops, which is the point: adding an alternative to an enum later shows you every `match` that needs updating.

  ```
  error[E4060]: match on 'op' does not cover op::div
  ```

  `_ => ...` covers "everything else", but then the compiler can't remind you any more. Prefer listing the alternatives of your own enums.
- There is no fallthrough and no `break`.

### Patterns

| Pattern | Matches |
|---|---|
| `0`, `'a'`, `"+"` | a literal (strings work too) |
| `1..=9` | a range |
| `shape::circle(c)` | an alternative, binding its payload to `c` |
| `item{ .weight = w, .. }` | a struct by fields; `..` ignores the rest |
| `_` | anything, binding nothing |
| `x if x > 100` | a guard: the arm only matches when the condition holds |

A guarded arm doesn't count toward exhaustiveness, because the compiler can't know when the guard is false. A `match` with guards still needs a plain arm for what's left.

### What the bindings are: borrowed or owned

- `match (v)` on a **variable**: the bindings **borrow** from `v` (lesson 2), so nothing is copied or moved.
- `match (mut v)`: the bindings are mutable references, and you can change the payload in place.
- `match (make_value())` on a **temporary**, or `match (move v)`: the bindings **own** the payload.

### `is`: a one-arm match in a condition

```edax
if (bag.heaviest() is std::some(h)) { std::println(h.name); }
while (queue.pop() is std::some(job)) { run(job); }
```

`is` is not an operator that gives a `bool`. It can only be the condition of an `if` or a `while` (possibly followed by `&& more conditions`), and its bindings exist only inside that branch or loop body. So `!(x is ...)` doesn't parse. For "bail out unless it matches", use a `match` whose other arm leaves:

```edax
u32 n = match (word.parse<u32>()) {
    std::some(v) => v,
    std::none => { skipped += 1; continue; },   // continue, break and return have type `never`
};
```

Examples: [`enums.ed`](examples/enums.ed), [`patterns.ed`](examples/patterns.ed), [`err_exhaustive.ed`](examples/err_exhaustive.ed).

---

## 3. Errors are values: `std::option` and `std::result`

| Type | Alternatives | Use for |
|---|---|---|
| `std::option<T>` | `std::some(x)`, `std::none` | "there may be no value" (a lookup, a parse) |
| `std::result<T, E>` | `std::ok(x)`, `std::error(e)` | "this can fail, and here is why" |

They are ordinary enums from the standard library. Their alternatives are written without template arguments (`std::ok(5)`, not `std::result<u32, my_error>::ok(5)`): the arguments come from context, the same way literals take their type.

The error type is usually your own enum, so a caller can `match` on what went wrong:

```edax
enum parse_error : u8 {
    empty,
    not_a_number(usize),     // which word
}

std::result<u32, parse_error> parse_word(std::str word, usize at) {
    if (word.is_empty()) { return std::error(parse_error::empty); }
    u32 n = word.parse<u32>().or_error(parse_error::not_a_number(at))?;
    return std::ok(n);
}
```

### `?`: pass the error up

`expr?` unwraps an `ok` (or a `some`) and gives its value. If it's an `error`, the current function **returns that error** right there. It's the early-return you would write by hand after every call, in one character:

```edax
std::result<u32, parse_error> sum(std::str text) {
    mut u32 total = 0;
    for (std::str word : text.split(' ')) {
        total += parse_word(word, at)?;     // on an error, sum returns it
        ...
    }
    return std::ok(total);
}
```

The error types must be **identical**: nothing converts one error type into another behind your back.

```
error[E4191]: '?' would return the error 'small_error', but 'run' returns errors of type 'big_error'
```

Change it on the way with:
- `.or_error(e)`: turns an `option` into a `result` with the error `e` (as in `parse_word` above);
- `.map_error(f)`: turns one error into another with a function or lambda (lesson 7).

### A result can't be ignored

Calling a function that returns a `result` and dropping it is a compile error, because ignoring an error should be a decision:

```
error[E4150]: the 'std::result<u32, small_error>' returned here is ignored
```

Write `_ = f();` to drop it on purpose.

### Other helpers

| Method | Gives |
|---|---|
| `.value_or(x)` | the value, or `x` when there's none / an error |
| `.expect(f"why")` | the value, or a **panic** with your message |
| `.is_some()`, `.is_ok()` | a `bool`, without taking the value |
| `.or_error(e)` | option → result |
| `.map_error(f)` | result → result with another error type |

Examples: [`results.ed`](examples/results.ed), [`err_results.ed`](examples/err_results.ed).

---

## 4. Panics are the end

Some failures are bugs, not errors: an overflow, an index past the end, `.expect` on an error, a failed `run_time check` (lesson 8), `std::panic("...")`. These **panic**, and a panic aborts the process:

- there is **no unwinding**: destructors don't run, and nothing can catch it;
- other threads don't survive either;
- a panic hook (`std::set_panic_hook`) may flush a log, but it can't stop the abort.

```
panic.ed:14:21: panic in main: index 7 out of bounds (length 3)
```

So the rule is simple: anything the program should **recover from** is a `result`; a panic is for "this can't happen unless the program is wrong". When you're not sure an index is valid, ask first: `xs.get(i)` returns an `std::option<T&>` instead of panicking.

**Why no exceptions?** An exception is an invisible extra return path from every call. With results, every way out of a function is written in it (a `return` or a `?`), the compiler checks that callers handle errors, and the generated code has no unwinding tables.

Example: [`panic.ed`](examples/panic.ed).

---

## C++ → Edax at a glance

| C++ | Edax |
|---|---|
| `enum class op : uint8_t { add, sub };` | `enum op : u8 { add, sub }` |
| `std::variant<A, B>` | `enum v : u8 { a(A), b(B) }` |
| `switch` + `break` | `match`, exhaustive, no fallthrough |
| `std::visit` | `match` |
| `if (auto* p = std::get_if<B>(&v))` | `if (v is v::b(p))` |
| `std::optional<T>` | `std::option<T>` |
| `std::expected<T, E>` | `std::result<T, E>` |
| `throw` / `try` / `catch` | return `std::error(e)` / `?` / `match` on the result |
| `[[nodiscard]]` | always on for results |
| `assert` / `std::terminate` | `run_time check` / `std::panic` |

---

## Your goal

`exercise.ed` is a reverse-Polish calculator: `"3 4 + 2 *"` means `(3 + 4) * 2`. Numbers are pushed on a stack; an operator pops two values and pushes the result. The enums and `main` are written. Implement `read_token`, `apply`, `evaluate` and `describe` so that the program prints:

```
3 4 + 2 * = 14
10 2 8 * + 3 - = 23
1 +: an operator is missing an operand
4 0 /: division by zero
2 x *: word 1 is not a number or an operator
1 2 3: 3 values left over
```

Aim for these (they're what the lesson is about):
- No `if` chains where a `match` says it. `apply` is a single `match` expression.
- Every failure travels up with `?`: `evaluate` has exactly one explicit `return std::error(...)`.
- `describe` uses a guard for the "nothing to compute" case.

<details>
<summary>Hint 1: <code>read_token</code></summary>

Match on the word for the four operators, giving an `std::option<op>`; if it's `some`, return `std::ok(token::apply(o))`. Otherwise `word.parse<i64>()` gives an `std::option<i64>`: `.or_error(calc_error::bad_token(position))?` turns "not a number" into your error and returns it.
</details>

<details>
<summary>Hint 2: <code>apply</code></summary>

One `match (o)` with four arms, each giving a result. The `div` arm's value is an `if` expression: `if (b == 0) { std::error(...) } else { std::ok(a / b) }`.
</details>

<details>
<summary>Hint 3: <code>evaluate</code></summary>

`text.split(' ')` gives the words. Keep `mut std::darray<i64> stack`. `stack.pop()` is an `std::option<i64>`: `stack.pop().or_error(calc_error::missing_operand)?`. Pop `b` first, then `a` (the right operand is on top). Count the word position yourself. At the end, one value must be left: that's the one explicit `return std::error(calc_error::leftover(stack.length()))`.
</details>

---

## Try this afterwards

1. Delete the `op::div` arm in `apply`. Read the error.
2. Call `read_token("7", 0);` as a statement.
3. Make `apply` return `std::result<i64, std::static_str>` and keep `stack.push(apply(o, a, b)?);` in `evaluate`. Then fix the call with `.map_error` and a lambda (`[](std::static_str s) { return calc_error::division_by_zero; }`, lesson 7 has the details).
4. Evaluate `"9223372036854775807 1 +"`. What happens? What would make it a `calc_error` instead (`std::checked_add`)?

<details>
<summary>What happens</summary>

1. `error[E4060]: match on 'op' does not cover op::div`.
2. `error[E4150]: the 'std::result<token, calc_error>' returned here is ignored`. `_ = read_token("7", 0);` compiles.
3. `error[E4191]`: `?` would return a `std::static_str` from a function whose errors are `calc_error`. `apply(o, a, b).map_error(...)?` converts it first.
4. The program panics with an integer overflow: `i64::max + 1` doesn't fit. With `std::checked_add(a, b).or_error(calc_error::...)` the overflow becomes an error value (you would add an alternative such as `overflow` to `calc_error`).
</details>

## Recap

- One `enum` for enumerations and tagged unions; payloads are built with `()`.
- `match` is an exhaustive expression; `is` is a one-arm match in an `if` or `while`.
- Errors are `std::result` values; `?` returns them; identical error types only; results can't be ignored.
- A panic is for bugs, and it ends the process without unwinding.

Next: [lesson 5](../05-interfaces/README.md), inheritance, interfaces and dynamic dispatch.
