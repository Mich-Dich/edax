# Lesson 3: Structs, classes, modules

Edax keeps C++'s header/source split, but it is a real module system rather than textual
inclusion. Types come in two kinds: plain `struct`s and encapsulated `class`es.

## `struct` vs `class`

| | `struct` | `class` |
|---|---|---|
| Access | everything public, no access labels | `public:` / `protected:` / `private:` |
| Inheritance | none (may implement interfaces) | single inheritance, `virtual` (lesson 5) |
| Destructor | none | `~name()` |
| Copy type | if all fields are | if all fields are **and** it has no destructor |

- **Construction uses designated fields only:** `vec2{ .x = 1.0 }`, in any order. A field left out
  takes its declared default; leaving out a field with no default is an error. There are no
  constructors: a class offers `static` factory methods (`polygon::named("x")`).
- **Members are named bare, as in C++:** `points.push(p)` in a method is `this.points.push(p)`.
  Unlike C++, nothing can hide a member: a local or parameter may not take a member's name
  (`void set(u32 value) mut` in a class with a field `value` is an error), and a name that is
  both a member and a free function must be written out (`this.next()`). Inside a method, `this`
  is a reference: an immutable one, or a mutable one in a method marked `mut` (written after the
  parameters, like C++'s `const` but the other way round).
- Field names and method names share one scope: a field `owner` and a method `owner()` clash.
- `==` is opt-in: `bool operator==(vec2& other) = default;`. Operators are methods or free
  functions in the type's own namespace and module.
- `clone()` exists automatically when every field can be cloned. A class with a destructor gets
  it only by implementing `std::cloneable` itself.

## Modules: `.edh` + `.ed`

- `geometry.edh` plus `geometry.ed` is the module **`geometry`**; `shapes/circle.edh` is
  `shapes::circle`. You `import geometry;` by path, and a module's `.ed` sees its own `.edh`.
- **A module is not a namespace.** Names live in the `namespace` blocks they are written in, so
  `import geometry;` gives you `geo::polygon`. There is no `using`; a file can write
  `namespace g = geo;`.

| | `.edh` (what importers see) | `.ed` |
|---|---|---|
| function | declaration | body |
| struct | **all** fields and method declarations | `struct vec2 { ... }` block with method bodies only |
| class | `public:`/`protected:` members, destructor | `class polygon { ... }` block: bodies, then `private:` members |
| helpers | - | marked `private` (module-local) |

- Method bodies are written **only** inside the type's block in the `.ed`. There is no
  `void polygon::add(...)` out of line.
- Private fields live only in the `.ed`, yet importers still know the class's size: no PIMPL.
- A header never contains bodies or `check`s. In a `.ed` that has a header, every top-level item
  the header doesn't declare must be `private`.
- Imports form an acyclic graph, and declaration order inside a module doesn't matter.

## Your goal

`exercise/` holds the module `geometry` and a `main.ed` that uses it. `geometry.edh` and `main.ed`
are complete; write `geometry.ed` so that `python3 course/check.py 3` prints exactly:

```
square: 4 corners, area 4.00, perimeter 8.00
triangle: 3 corners, area 6.00, perimeter 12.00
a == b: true, a + b = (2.0, 4.0)
triangle is gone
square is gone
```

Hints:
- Give the private fields defaults where that makes `polygon{ .label = ... }` enough.
- A helper "next index around the polygon" is a module-local `private` function.
- Why does `triangle` go first in the last two lines?

## Try this afterwards

1. In `main.ed`, read `square.label`. Then call your private helper as `geo::next(0, 4)`.
2. Remove `= 0.0` from `vec2::y` and see which lines of `main.ed` break.
3. Write a `polygon` method `scaled(f32 k)` that returns a new polygon. Can it be a non-`mut`
   method? Does it need `clone()`?
4. Give `polygon` a method `void rename(std::str label) mut`. What does the compiler say, and
   why is that better than C++'s silent shadowing? Then add a free function `corners()` next to
   the method and call `corners()` inside a method.
5. Write a second module `shapes.edh` that also declares a `geo::polygon` (in its own
   `namespace geo` block) and import both. Where is the error reported, and why there?
