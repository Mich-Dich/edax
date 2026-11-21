# Formatting and Printing

## Rules

### Format strings: `f"..."`
```edax
std::println(f"{e.name:<20} {e.age:>3} {e.salary:>10.2}");
std::string s = std::format(f"{count} items");      // allocates: visible through the function
```
- `{expr}` inserts any expression whose type implements `std::formattable`.
- A hole may contain string and char literals: `f"[{"left":<8}] {join(parts, ", ")}"`.
- `{expr:spec}` adds a format spec: alignment `<` `>` `^`, width, `.precision`, `x`/`b`/`o` for hex/binary/octal, `+` for the sign.
- `{{` and `}}` produce a literal `{` and `}`.
- The **spec and the types are checked at compile time**.
- An `f"..."` produces a `std::format_args`, a view type that **doesn't allocate**. `std::println`, `std::eprintln`, `std::format` and `std::panic` take it. Only `std::format` allocates, and it returns `std::string`.

### Lazy holes
An `f"..."` passed to a function's `std::format_args` parameter is **evaluated when the function formats it**, not at the call: at most once, on the calling thread, before the function returns. If the function never formats it, its holes are never evaluated.
```edax
std::log::debug(f"tree: {tree.dump()}");   // dump() runs only if the record is formatted
```
- `std::format_args` is a view (it can't outlive the call, so everything its holes read is alive) and **move-only**: `std::println`, `std::format` and `std::panic` consume it, and passing it on is `move message`.
- In an f-string passed to a function's `std::format_args` parameter, a hole may not contain `move`, a `mut` argument, or a call of a `mut` or consuming method (an error): whether a hole ran never changes visible state. `std::println`, `std::format` and `std::panic` format at once, so their f-strings have no such restriction.
- A plain string literal passed as a `std::format_args` is an f-string without holes.

### Making a type printable
A type is `std::formattable` (see [Core Utilities](../core/utilities.md)) when it has a method
```edax
void format(mut std::formatter& out);
```
- `out.write(text) mut` appends text; `out.spec()` returns the hole's `std::format_spec` (alignment, width, precision, radix, sign) for types that use them.
- Alignment and width are applied by the formatter around everything `format` writes, so most types ignore the spec.
- `@std::derive::formattable` (see [Annotations](../../language/compile-time/annotations.md)) generates `format` from the fields.

### Printing
| Function | Writes |
|---|---|
| `std::print(message)`, `std::println(message)` | To stdout; `println` adds a line break |
| `std::eprint(message)`, `std::eprintln(message)` | To stderr |
| `std::format(message) → std::string` | Into a new string (allocates) |
| `std::panic(message) → never` | Panics with the message (see [Error Handling](../../language/errors/error-handling.md)) |

`message` is a `std::format_args`: an `f"..."` or a plain string literal. These four format at once, so their holes have no restrictions. For logging see [Logging: `std::log`](../logging/logging.md).

## Rationale
- **`f` prefix:** an explicit prefix makes interpolation visible, and plain strings stay plain: `"{x}"` is never a hole by accident.
- **No allocation unless asked:** an f-string is a view of its holes; only `std::format` builds a string, and its name says so.
- **Lazy holes:** a message that isn't printed (a filtered log call) costs nothing, and because holes can't change anything, whether they ran is invisible.
