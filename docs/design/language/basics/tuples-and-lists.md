# Tuples and List Literals

Two small built-in forms for grouping values: **tuples** group a fixed number of values of different types, **list literals** write several values of one type.

## Rules

### Tuples
```rift
(u32, u32) min_max(std::span<u32> values) { ... return (lo, hi); }

(u32 lo, u32 hi) = min_max(samples.as_span());      // destructuring declaration
auto (first, rest) = items.split_at(1);             // the same with auto
(u32, u32) both = min_max(samples.as_span());
u32 low = both.0;                                   // elements by position
```
- **Type:** `(A, B, ...)` with **two or more** element types. `(x)` is just `x` in parentheses; there are no one-element or empty tuples (`void` means "nothing").
- **Value:** `(a, b, ...)`. Unsuffixed literals take their types from context, like everywhere (see [Built-in Types and Conversions](types.md)).
- **Elements:** `t.0`, `t.1`, ... (a compile-time index).
- **Destructuring declaration:** `(T1 a, T2 b) = expr;` or `auto (a, b) = expr;`. Each name may be `mut`: `(mut u32 lo, u32 hi) = ...`. Destructuring **consumes** the tuple, so its elements are moved into the new variables; this is how a function hands back two owned or two mutable things at once.
- `_` skips an element: `(u32 lo, _) = min_max(values);`.
- A tuple is a **copy type** if all its elements are, and a **view** if any element is (see [Ownership, Copies, Moves and References](../memory/ownership-and-references.md)).
- `==` and `<=>` compare element by element, when every element supports them.
- **Patterns:** `match (pair) { (0, _) => ..., (x, y) if x == y => ..., _ => ..., }`.
- Tuples have no names and no methods. When the parts have meanings worth naming, use a struct.

### List literals
```rift
std::array<u32, 3> sizes = [16, 32, 64];
std::span<std::static_str> names = ["low", "medium", "high"];       // static data
g.add_arguments(step, [shader.text, "-o", spirv.text]);             // a std::span<std::str> parameter
```
- `[a, b, c]` takes its type from context, like an unsuffixed literal:

  | Context | Result |
  |---|---|
  | `std::array<T, N>` | The `N` elements inline; the count must match |
  | `std::span<T>` (a variable or parameter) | A view of the elements: in static memory if they're all compile-time values, otherwise in a temporary array that lives until the end of the full statement |

  Without a context it's an error (`auto x = [1, 2];`).
- All elements must have the element type `T` after literal typing.
- **No hidden allocation:** a list literal never becomes a `std::darray`. Write `std::darray<u32>::from([1, 2, 3])`, which says that it allocates, just as `std::string::from("...")` does for text (see [Strings](../../stdlib/text/strings.md)).
- `[]` is the empty list; it needs a context too.
- **Lambdas:** a `[...]` directly followed by `(` is a lambda's capture list (see [Lambdas](../functions/lambdas.md)); anything else is a list literal.

## Rationale
- **Tuples for returning several values:** a function that splits a mutable span must return two `std::span<mut T>` at once, and only a destructuring declaration can move both out. A struct can't, because fields can't be moved out of it.
- **No tuple names, no methods:** tuples are for passing values along, not for modelling data. Anything that deserves names is a struct, so there is still one way to define a data type.
- **List literals typed by context, never allocating:** the same rule as number literals, and the same rule as string literals: allocation is always written.
