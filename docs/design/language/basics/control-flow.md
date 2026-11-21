# Control Flow

## Rules

### General
- Braces are mandatory everywhere (see [Lexical Structure](lexical.md)).
- **Assignment is a statement, not an expression**, so `if (x = 0)` can't be written.
- There is no `++` / `--`. Write `x += 1;`.
- Conditions are `bool` or contextually converted to it (integers, `std::raw_ptr`; see [Built-in Types and Conversions](types.md)).

### Blocks as values
`if`, `match` and plain `{ }` blocks can be used as expressions. Their value is the **last expression without a `;`**:
```rift
std::str label = if (x > 0) { "positive" } else { "non-positive" };
```
- When used as an expression, an `if` must have an `else`, and every branch must produce the same type.
- Branches of type `never` (`return`, `break`, `continue`, `std::panic(...)`) fit with any type.

### `if` / `else`
```rift
if (ptr) { ... }                       // OK: std::raw_ptr in a condition
if (count > 10) { ... } else if (count) { ... } else { ... }
```

### `else` after a call
A call to a function marked **`@std::check_result`** (see [Annotations](../compile-time/annotations.md)), written as a statement, may be followed by an `else` block. The block runs when the call returns false:
```rift
std::log::validate(texture.is_valid(), f"texture '{name}' is invalid") else {
    return false;
}
connect(address) else { continue; }          // connect is declared @std::check_result
```
- It means `if (!f(...)) { ... }`, with the call written first.
- Braces are required, as for `if`.
- Only a call to a `@std::check_result` function may be followed by `else`; such a function returns `bool`. For any other call, write `if (!f(...)) { ... }`.
- `@std::check_result` is for functions that report a failure themselves (log it, count it) and return whether the check passed, like `std::log::validate` and `std::log::assert`.

### `while`
```rift
while (file.has_more()) {
    ...
}
```
`break` and `continue` work in `while` and `for`.

### `for`: range-based only
```rift
for (employee& e : employees) { ... }         // immutable reference
for (mut employee& e : employees) { ... }     // mutable reference (employees must be mut)
for (u32 x : numbers) { ... }                 // by value: copy types only
for (usize i : 0..employees.len()) { ... }    // half-open range
for (u8 b : 0..=255) { ... }                  // inclusive range
```
- What a `for` loop can go over:
  - a range `a..b` / `a..=b`;
  - a **container**: a type with `usize len()` and `operator[](usize)`. The loop visits the indices in order. `mut T&` elements need the `mut operator[]` and a `mut` container;
  - a **stream**: a type with a `next()` method that returns a `std::option<T>`. The loop runs until `none`; each element is a value that the loop variable owns. A temporary stream (`file.lines()`) lives until the loop ends.
- `a..b` and `a..=b` are range expressions. They use the same syntax as range patterns in `match`. Literal bounds take their type from the loop variable.
- There is no C-style `for (;;)`.
- While a loop holds references into a container, the container can't be changed (see [Ownership, Copies, Moves and References](../memory/ownership-and-references.md)).

### `match`
- It is an **expression**, it must be **exhaustive**, and there is **no fallthrough**.
- **Every arm ends with `,`.**
- Patterns: literals, ranges (`0..=9`), enum alternatives (`token::number(x)`, `std::ok(x)`), struct destructuring (`employee{ .age = a, .. }`), `_`, and guards (`x if x > 100`).
- **Bindings:**
  - If the matched expression is a variable, bindings are immutable references.
  - `match (mut v)` makes them mutable references (`v` must be `mut`).
  - If it is a temporary or `move v`, bindings own the values.

```rift
std::str label = match (code) {
    200 => "ok",
    404 => "not_found",
    _   => "other",
};

match (parse_line(line)) {                     // temporary: e is owned
    std::ok(e)    => employees.push(move e),
    std::error(_) => std::eprintln(f"skipping bad line: {line}"),
}

std::darray<employee> list = match (load(path)) {
    std::ok(l)    => move l,
    std::error(e) => {
        std::eprintln(f"error: {e}");
        return std::exit_code::failure;        // type never: fits any arm
    },
};
```

## Rationale
- **Assignment as a statement:** together with contextual bool conversion, this rules out the `if (x = 0)` bug completely.
- **No `++`/`--`:** `+= 1` already exists. One way.
- **One range syntax:** loops and patterns use the same `a..b` / `a..=b`; there is no library range type.
- **`,` after every arm:** one rule instead of "comma unless the arm is a block".
- **`else` after a call, only where it's meant:** checks that log or report a failure (`std::log::validate`, see [Logging: `std::log`](../../stdlib/logging/logging.md)) read as one statement, and the recovery is ordinary code in a block instead of an argument to a macro. Limiting it to `@std::check_result` functions keeps `if (!f())` the one way for every other call.
