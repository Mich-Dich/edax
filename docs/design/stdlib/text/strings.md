# Strings

Rift has one owning string type and two non-owning ones. All three hold UTF-8.

## Rules

### String types
| Type | Owns? | Kind | Use |
|---|---|---|---|
| `std::string` | Yes (heap) | Owning | Building and storing text |
| `std::str` | No | `view` (see [Ownership, Copies, Moves and References](../../language/memory/ownership-and-references.md)) | Parameters and borrowed text |
| `std::static_str` | No (static memory) | Normal value | Type of string literals; can be stored anywhere |

All three hold UTF-8.

### Conversions
| From → To | How |
|---|---|
| `std::string` → `std::str` | Implicit (borrow) |
| `std::static_str` → `std::str` | Implicit |
| `std::str` → `std::string` | `std::string::from(s)` (allocates) |
| `std::static_str` → `std::string` | Implicit, **with a compiler warning** |

```rift
std::str      a = "hello";                         // OK
std::string   b = std::string::from("hello");      // OK: allocation is visible
std::string   c = "hello";                         // WARNING: implicit allocation of std::string from a literal
std::static_str d = "config.toml";                 // OK: can be stored in a struct field
```

### String literals
- `"..."`: escapes `\n \t \r \\ \" \0 \u{XXXX}`. **Never interpolated**, so `"{x}"` is just the characters `{x}`.
- `r"..."`: raw, with no escape processing.
- `c"..."`: a C string, NUL-terminated, of type `std::raw_ptr<u8>` (static memory). Only for calling C (see [Raw Pointers and `unsafe`](../../language/memory/raw-pointers-and-unsafe.md)); it cannot contain `\0`.
- `f"..."`: format string (see [Formatting and Printing](formatting.md)).

### Methods
| On `std::str` (and through it on `std::string` and literals) | Result |
|---|---|
| `len()`, `is_empty()` | Length in bytes (`usize`), `bool` |
| `starts_with(s)`, `ends_with(s)`, `contains(s)` | `bool` |
| `find(s)` | `std::option<usize>`: byte offset of the first match |
| `slice(from, to)` | `std::str` of the bytes `from..to`; panics if out of range or not on a character boundary |
| `trim()` | `std::str` without leading and trailing whitespace |
| `split(c)` | `std::darray<std::str>` of the parts between `c` (a view: it borrows the text) |
| `parse<T>()` | `std::option<T>` for numbers and `bool` |
| `as_ptr()` | `std::raw_ptr<u8>` to the first byte (for C, see [Raw Pointers and `unsafe`](../../language/memory/raw-pointers-and-unsafe.md)) |
| `==`, `<=>` | Byte-wise comparison |

`len`, `is_empty`, `starts_with`, `ends_with`, `contains`, `slice` and `find` also work at compile time (see [Compile-Time Evaluation](../../language/compile-time/compile-time.md)).

| On `std::string` | Result |
|---|---|
| `std::string::from(s)`, `std::string::with_capacity(n)` | A new string (allocates) |
| `push_str(s) mut`, `push(char) mut`, `clear() mut` | Changes the string |
| `reserve(n) mut`, `capacity()` | Room for growth |
| `clone()` | A copy (allocates) |

**Layout:** `std::string` is 24 bytes. Up to 23 bytes of text are stored inline (no heap); longer text is on the heap. Comparison uses `memcmp`.

## Rationale
- **`std::static_str`:** literals live forever, so they don't need view restrictions. That lets you write `struct option_spec { std::static_str name; }` for tables of options.
- **Warning on `std::string s = "..."`:** it's legal and convenient, but it hides an allocation. The warning keeps principle 7 without making every string declaration noisy.
