# Std lesson 4: Text and formatting

Text in Edax is always UTF-8, and its three types differ only in who owns the bytes (language lesson 9). This lesson is about *working* with text: searching and cutting it without copying, parsing numbers out of it, building new text with as few allocations as possible, and printing values (including your own types) with precise layout.

**You will learn:** the `std::str` methods, `parse<T>`, building with `std::string` (`with_capacity`, `push`, `push_str`), every format spec, `std::formatter` for your own types, what a `std::format_args` is and why log calls are cheap, and `stderr`.

**You will build:** an invoice printed from a text order (`exercise.ed`).

Examples are in [`examples/`](examples); `python3 course/check.py s4 --examples` checks them.

---

## 1. Reading text without copying

Every method of `std::str` works on `std::string` and literals too (they convert for free), and none of them allocates except `split`, which returns a darray of views.

| Method | Gives |
|---|---|
| `len()`, `is_empty()` | length in **bytes** |
| `trim()` | a `std::str` without the surrounding whitespace |
| `starts_with(s)`, `ends_with(s)`, `contains(s)` | `bool` |
| `find(s)` | `std::option<usize>`: the byte offset of the first match |
| `slice(from, to)` | the bytes `from..to` as a `std::str`; panics out of range or inside a UTF-8 character |
| `split(c)` | `std::darray<std::str>`: views between the separators |
| `parse<T>()` | `std::option<T>` for numbers and `bool` |
| `byte(i)` | the `u8` at byte `i` |
| `==`, `<=>` | byte-wise |

All the results that are text are **views into the original**, so a parser can return pieces of its input without copying (lesson 2's rule: a function with one view parameter may return a view of it).

`find` and `parse` return options, which combine well with `?`: in a function that returns an `std::option`, `?` on a `none` returns `none` right away.

```edax
std::option<line> parse_line(std::str text) {
    usize x = text.find(" x ")?;                         // no " x ": none
    u32 quantity = text.slice(0, x).trim().parse<u32>()?;   // not a number: none
    ...
}
```

Example: [`str_methods.ed`](examples/str_methods.ed).

## 2. Building text

| Call | Note |
|---|---|
| `std::string::from(s)` | a new string from any text (allocates, unless it fits in 23 bytes) |
| `std::string::with_capacity(n)`, `reserve(n) mut`, `capacity()` | room before you write |
| `push_str(s) mut`, `push(c) mut` | append text, append one `char` |
| `clear() mut`, `clone()` | |
| `std::format(f"...")` | a new `std::string` from an f-string |

- There's no `+` on strings: `a + "b"` is `error[E4011]: operands have different types`. Use `std::format(f"{a}b")` or `push_str`.
- `std::string s = "literal";` works but warns (`W4002`): it allocates where nobody can see it. Write `std::string::from("literal")`.
- Up to 23 bytes are stored inside the string itself, with no heap allocation at all.

Example: [`building.ed`](examples/building.ed), [`err_text.ed`](examples/err_text.ed).

## 3. Format specs

A hole is `{expression}` or `{expression:spec}`. The spec is `[<>^][+][width][.precision][x|X|b|o]`, checked at compile time against the value's type:

| Spec | Means | `255` / `3.14159` |
|---|---|---|
| `<6`, `>6`, `^7` | left, right, centre in that many columns | `[255   ]`, `[   255]`, `[  255  ]` |
| `.3` | decimals (floats) | `3.142` |
| `>8.2` | combined | `[    3.14]` |
| `x`, `X`, `b`, `o` | hex, upper hex, binary, octal (integers) | `ff`, `FF`, `11111111`, `377` |
| `+` | always a sign | `+255` |
| `{{`, `}}` | literal braces | |

Not there, coming from C++/Python: a **fill character** (`{:-<31}` is `error[E2030]: invalid format spec`) and **zero padding** (`{x:06}` is read as width 6, padded with spaces). Pad by hand where you need it.

A hole may hold string and char literals (`{"total":>8}`), which is how a table header lines up with its rows.

Example: [`format_specs.ed`](examples/format_specs.ed).

## 4. Printing your own types

A type is `std::formattable` when it has

```edax
void format(mut std::formatter& out);
```

`out.write(text)` appends text. Alignment and width are applied by the formatter **around** what `format` writes, so `{m:>14}` works for your type with no extra code. (`out.spec()` gives the hole's spec, for the rare type that wants to read precision or radix itself.)

```edax
struct money {
    i64 cents = 0;
    void format(mut std::formatter& out) {
        std::static_str pad = if (cents % 100 < 10) { "0" } else { "" };
        out.write(std::format(f"{cents / 100}.{pad}{cents % 100}"));
    }
}
```

For a debug-style `point { x: 1.5, y: -2.0 }`, put `@std::derive::formattable` on the type instead. Simple enums print the name of their alternative without anything.

Example: [`formattable.ed`](examples/formattable.ed).

## 5. What an f-string is

`f"..."` doesn't build a string. It makes a **`std::format_args`**: a small view of the holes, formatted by whoever receives it. Consequences:

- `std::println(f"...")` formats straight to the output: no temporary string.
- Only `std::format` allocates, and its name says so.
- A function can take a `std::format_args` parameter and decide whether to format it. If it doesn't, **the holes never run**. That's why a filtered log call (std lesson 9) costs nearly nothing, even with an expensive expression in it.
- Because a skipped hole must not change anything, holes in an f-string passed to such a function can't contain `move`, `mut` arguments or `mut` method calls (a compile error). `std::println`, `std::format` and `std::panic` format at once, so they have no such restriction.
- A `std::format_args` is move-only: passing it on is `move message`.

| Function | Writes to |
|---|---|
| `std::print`, `std::println` | stdout |
| `std::eprint`, `std::eprintln` | stderr |
| `std::format` | a new `std::string` |
| `std::panic` | the panic message (and the end of the program) |

Example: [`lazy.ed`](examples/lazy.ed).

---

## C++ → Edax at a glance

| C++ | Edax |
|---|---|
| `std::string_view::substr(a, n)` | `s.slice(a, a + n)` |
| `s.find(x) != npos` | `s.find(x) is std::some(at)` |
| `std::from_chars` / `std::stoi` | `s.parse<i32>()` |
| `a + b` on strings | `std::format(f"{a}{b}")` or `push_str` |
| `std::format("{:>8.2f}", x)` | `f"{x:>8.2}"` |
| `std::format("{:06}", x)` | pad by hand |
| `std::formatter<T>` specialization | a `void format(mut std::formatter& out)` method |
| `std::cerr <<` | `std::eprintln(f"...")` |

---

## Your goal

`exercise.ed` reads an order written as text, one line per item (`2 x coffee @ 3.50`), and prints an invoice. A line that doesn't parse is reported on stderr and left out. Fill in the TODOs so that stdout is:

```
item       qty   price    total
coffee       2    3.50     7.00
croissant    1    2.05     2.05
water        4    1.00     4.00
-------------------------------
sum                       13.05
3 lines, 1 rejected.
```

<details>
<summary>Hint 1: <code>money::format</code></summary>

`cents / 100` is the whole part and `cents % 100` the rest; write a `"0"` before a rest below 10. One `out.write(std::format(f"..."))` does it.
</details>

<details>
<summary>Hint 2: <code>parse_money</code> and <code>parse_line</code></summary>

`text.split('.')` must give two parts, the second of length 2; then `parts[0].parse<i64>()?`. In `parse_line`, `t.find(" x ")?` and `t.find(" @ ")?` give the two offsets; the name is `t.slice(x + 3, at).trim()` (`" x "` is 3 bytes).
</details>

<details>
<summary>Hint 3: the table</summary>

Header: `f"{"item":<10} {"qty":>3} {"price":>7} {"total":>8}"`. Rows: the same specs on `l.name`, `l.quantity`, `l.price` and `total`; your `money` gets the alignment for free. The sum line is `{"sum":<22} {sum:>8}`.
</details>

---

## Try this afterwards

1. Make `line` an ordinary `struct` (remove `view`).
2. Use `{l.price:>7.2}` on the `money` value.
3. In `parse_line`, return `std::some(line{ .name = std::string::from(name).as_str(), ... })`.
4. Write `std::println(f"{lines}")`.

<details>
<summary>What happens</summary>

1. `error[E5011]`: the field `name` is a `std::str`, so the struct must be a `view struct`.
2. It compiles and prints the same: alignment and width are applied around your `format`; the precision is yours to read with `out.spec()`, and `money` ignores it.
3. A borrow error: the view would point into a `std::string` that is destroyed when the function returns. Exactly the dangling `string_view` C++ lets through.
4. An error: a `std::darray<line>` isn't `std::formattable`. Print the elements yourself.
</details>

## Recap

- `std::str` methods cut and search without copying; results are views of the input; `find` and `parse` give options.
- Build with `std::string` (`with_capacity`, `push_str`) or `std::format`; there's no `+`.
- Specs: align, width, precision, radix, sign; no fill character, no zero padding.
- `void format(mut std::formatter& out)` makes a type printable; the formatter aligns it.
- An f-string is a lazy, non-allocating view; unformatted holes never run.

Next: [std lesson 5](../05-memory/README.md), smart pointers and allocators.
