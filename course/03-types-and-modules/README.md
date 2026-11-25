# Lesson 3: Structs, classes, modules

Edax keeps C++'s split between a header that others read and a source file with the bodies, but it is a real module system, not text pasted in by `#include`. Types come in two kinds: plain `struct`s, and encapsulated `class`es.

**You will learn:** `struct` vs `class`, designated construction and factory methods instead of constructors, bare member names and why nothing can hide them, operators, and how a module's `.edh` and `.ed` divide the work.

**You will build:** the `.ed` half of a geometry module (`exercise/geometry.ed`).

Examples are in [`examples/`](examples); `python3 course/check.py 3 --examples` checks them.

---

## 1. `struct` and `class`

| | `struct` | `class` |
|---|---|---|
| Meant for | plain data: everything is visible | an object that guards its own state |
| Access | everything public, no labels | `public:` / `protected:` / `private:` |
| Inheritance | none (it may implement interfaces, lesson 5) | single inheritance, `virtual` (lesson 5) |
| Destructor | none | `~name()` |
| Copy type (lesson 2) | if all its fields are | if all its fields are **and** it has no destructor |

```edax
struct vec2 {
    f32 x = 0.0;
    f32 y = 0.0;

    vec2 operator+(vec2& other) { return vec2{ .x = x + other.x, .y = y + other.y }; }
    bool operator==(vec2& other) = default;    // == is opt-in
    f32 length() { return (x * x + y * y).sqrt(); }
}

class counter {
public:
    static counter named(std::str name) { return counter{ .label = std::string::from(name) }; }
    void hit() mut { hits += 1; }
    u32 count() { return hits; }
    ~counter() { std::println(f"{label} counted {hits}"); }
private:
    std::string label;
    u32 hits = 0;
}
```

No `;` after the closing `}` of a type: Edax has no `struct x { } instance;` form, so the `;` would only be something to forget.

### Building values: designated fields, no constructors

```edax
vec2 a = vec2{ .x = 3.0, .y = 4.0 };   // fields by name, in any order
vec2 b = vec2{ .y = 1.0 };             // x takes its declared default
vec2 c = vec2{};                       // every field takes its default
```

- A field left out takes its declared default. A field **without** a default must be given: `error[E3035]: missing field 'from' in 'span2{...}'`.
- There is no positional form (`span2{ 1, 4 }` is `error[E2017]`), so adding or reordering fields never silently changes what a literal means.
- **There are no constructors.** `span2(1, 4)` is a call, and a type isn't a function: `error[E3022]: 'span2' is a struct, not a function`. A class that needs set-up offers `static` factory methods, which have names (`polygon::named("x")`, `file::open(path)`) and can return an `std::result` when they can fail (lesson 4). Inside the class, the factory builds the value with a literal, including the private fields.

**Why no constructors?** A C++ constructor can't fail except by throwing, can't have a name, and can leave half-built objects when a member's constructor throws. A factory function is ordinary code: it builds every field, then returns the finished value.

### Members are named bare, and nothing can hide them

Inside a method, `hits` means `this.hits`, as in C++. `this` is a reference to the object: immutable, or mutable in a method marked `mut` after its parameters. That's C++'s `const` method turned around: methods are read-only unless they say otherwise.

Unlike C++, **no local or parameter may take a member's name**:

```
error[E3028]: 'balance' is the name of a field of 'account'
  --> err_members.ed:6:18
   |
 6 |     void deposit(u32 balance) mut {
   |                  ^^^^^^^^^^^ a member's name
   |
10 |     u32 balance = 0;
   |         -------
   = note: the member
   = note: in a method 'balance' means the member; choose another name for this one
```

In C++ the parameter would quietly hide the field, and `balance += x` would change the copy. In Edax a bare name in a method always means the same thing. The same goes for a method and a free function with the same name: inside a method, write `this.next()` or qualify the free one.

`this.x` is still allowed, and you need it for a base class's method: `this.actor::describe()`.

Fields and methods share one scope, so a field `owner` and a method `owner()` clash (`error[E3010]: 'item::owner' is defined more than once`). Name the field `owner_name`, or the method `owner_of()`.

### Operators and `clone`

- Operators are methods (`vec2 operator+(vec2& other)`) or free functions in the type's own namespace and module.
- `==` and `!=` exist only when you ask: `bool operator==(vec2& other) = default;` compares field by field.
- `clone()` exists automatically for a struct when every field can be cloned. A class with a destructor gets it only by implementing `std::cloneable` itself, because a destructor usually means "this object holds something unique".

Examples: [`struct_class.ed`](examples/struct_class.ed), [`err_construct.ed`](examples/err_construct.ed), [`err_positional.ed`](examples/err_positional.ed), [`err_construct2.ed`](examples/err_construct2.ed), [`err_members.ed`](examples/err_members.ed), [`err_same_name.ed`](examples/err_same_name.ed).

---

## 2. Modules: `.edh` and `.ed`

A **module** is a pair of files with the same name: `inventory.edh` (the interface) and `inventory.ed` (the implementation). `import inventory;` reads the `.edh`. A file in a subdirectory, `shapes/circle.edh`, is the module `shapes::circle`.

The rule that decides what goes where is short: **the `.edh` declares, the `.ed` defines, and nothing is written twice.**

Here is one complete module, [`examples/module/`](examples/module):

<table>
<tr><th><code>inventory.edh</code>: what importers see</th><th><code>inventory.ed</code>: the bodies</th></tr>
<tr><td>

```edax
import std;

namespace shop {
    struct price {
        u32 cents = 0;

        std::string text();
    }

    class stock {
    public:
        static stock make();
        void add(std::str name, price p) mut;
        usize len();
        price total();
        ~stock();
    }
}
```

</td><td>

```edax
import std;

namespace shop {
    struct price {
        std::string text() { return std::format(...); }
    }

    class stock {
    public:
        static stock make() { return stock{}; }
        void add(std::str name, price p) mut { ... }
        usize len() { return names.len(); }
        price total() { ... }
        ~stock() { ... }
    private:
        std::darray<std::string> names = default;
        std::darray<price>       prices = default;
    }

    private u32 sum_of(std::span<price> ps) { ... }
}
```

</td></tr>
</table>

Read it piece by piece:

| What | In the `.edh` | In the `.ed` |
|---|---|---|
| a free function | its declaration, ending in `;` | its body, once |
| a `struct` | **all** fields (with defaults) and the method declarations | a `struct price { ... }` block with **only the method bodies**: no fields |
| a `class` | the `public:` and `protected:` members and the destructor, as declarations | a `class stock { ... }` block: the bodies of those methods, then `private:` with the private fields and methods |
| a helper nobody else may use | nothing | the item, marked `private` |

So the `.ed`'s blocks reopen the types the header declared, and fill in what's missing. They don't repeat anything. These are the mistakes C++ habits lead to, and what the compiler says:

| Mistake | Error |
|---|---|
| repeating a struct field in the `.ed` | `error[E3016]: struct 'price' cannot add field 'cents' in its .ed block` |
| repeating a method declaration (no body) in the `.ed` | `error[E3013]: 'stock::len' is declared again in the .ed, without a body` |
| leaving a header method without a body | `error[E3013]: 'vec2::cross' is declared in the header but never defined` |
| a new public method only in the `.ed` | `error[E3015]: 'stock::extra' is not declared in the header` |
| a helper function that isn't in the header and isn't `private` | `error[E3017]: function 'sum_of' is not declared in the header` + "mark it 'private'" |

More rules worth knowing:
- Method bodies are written **only** inside the type's block. There is no out-of-line `void stock::add(...) { }`.
- **Private fields live only in the `.ed`**, yet importers still know the class's size, because the compiler reads both files. You get encapsulation without the PIMPL idiom, and changing a private field doesn't require changing the header.
- A header never contains bodies (except `= default` operators) and never contains `check`s.
- Imports form an acyclic graph. Inside a module, declaration order doesn't matter: `total()` can call `sum_of`, which is written below it.

### A module is not a namespace

`import inventory;` makes the module's declarations visible; their names come from the `namespace` blocks they're written in. So after `import inventory;` you write `shop::stock`, not `inventory::stock`. There is no `using namespace`; a file can shorten a namespace with a file-local alias, `namespace s = shop;`.

### Privacy

From outside the module:

```
error[E4071]: 'stock::names' is private to module 'inventory'
error[E3024]: 'shop::sum_of' is private to its module
```

`private` on a class member means private to the **module**, not to the class: every function in `inventory.ed` may use `stock`'s private fields (C++ would need `friend`).

Examples: [`module/`](examples/module), [`err_module/`](examples/err_module), [`err_private_helper/`](examples/err_private_helper).

---

## C++ → Edax at a glance

| C++ | Edax |
|---|---|
| `struct S { int x; };` | `struct s { i32 x = 0; }` (no `;` after `}`) |
| `S{1, 2}` | `s{ .x = 1, .y = 2 }` |
| constructor `S(int x)` | `static s make(i32 x) { return s{ .x = x }; }` |
| `int get() const;` | `i32 get();` |
| `void set(int v);` (non-const) | `void set(i32 v) mut;` |
| `bool operator==(const S&) const = default;` | `bool operator==(s& other) = default;` |
| `void S::f() { }` in the .cpp | the body inside `struct s { ... }` in the .ed |
| `#include "geometry.h"` | `import geometry;` |
| `using namespace geo;` | `namespace g = geo;` (file-local alias) |
| anonymous namespace / `static` function | `private` item in the `.ed` |
| PIMPL for private members | private fields in the `.ed` |

---

## Your goal

`exercise/` holds the module `geometry` and a `main.ed` that uses it. `geometry.edh` and `main.ed` are complete. Write `geometry.ed` so that `python3 course/check.py 3` prints exactly:

```
square: 4 corners, area 4.00, perimeter 8.00
triangle: 3 corners, area 6.00, perimeter 12.00
a == b: true, a + b = (2.0, 4.0)
triangle is gone
square is gone
```

Read `geometry.edh` first: it is your to-do list. Every method declared there needs a body in `geometry.ed`, and nothing in it is written again.

<details>
<summary>Hint 1: the shape of <code>geometry.ed</code></summary>

```edax
import std;

namespace geo {
    struct vec2 {
        // bodies of operator+, operator-, cross, length. No fields, no `operator==`
        // (`= default` in the header is already its definition).
    }

    class polygon {
    public:
        // bodies of named, add, corners, area, perimeter, name, ~polygon
    private:
        // the fields: a label and the points
    }

    // a private helper, and the body of describe
}
```
</details>

<details>
<summary>Hint 2: the fields of <code>polygon</code></summary>

A `std::string label;` and a `std::darray<vec2> points = default;`. With a default on `points`, `named` only has to write `polygon{ .label = std::string::from(name) }`.
</details>

<details>
<summary>Hint 3: area and perimeter go "around" the polygon</summary>

Both walk every corner `i` together with the next one, and the next one after the last corner is corner 0. A small `private usize next(usize i, usize count)` function says that once. The area is half the absolute value of the sum of `points[i].cross(points[next(i, n)])` (the shoelace formula); the perimeter is the sum of `(points[next(i, n)] - points[i]).length()`.
</details>

<details>
<summary>Hint 4: <code>describe</code></summary>

`std::format(f"...")` makes an `std::string`. `{p.area():.2}` prints two decimals.
</details>

---

## Try this afterwards

1. In `main.ed`, read `square.label`. Then call your helper as `geo::next(0, 4)`.
2. Remove `= 0.0` from `vec2::y` in the header. Which lines of `main.ed` break, and why those?
3. Write a `polygon` method `scaled(f32 k)` that returns a new polygon. Can it be a non-`mut` method? Does it need `clone()`?
4. Give `polygon` a method `void rename(std::str label) mut`. Then add a free function `corners()` next to the method and call `corners()` inside a method.
5. Write a second module `shapes.edh` that also declares a `geo::polygon` (in its own `namespace geo` block), and import both into `main.ed`. Where is the error reported?

<details>
<summary>What happens</summary>

1. `error[E4071]: 'polygon::label' is private to module 'geometry'`, and `error[E3024]: 'geo::next' is private to its module`.
2. Every `geo::vec2{ ... }` literal that leaves out `.y` now fails with `error[E3035]: missing field 'y' in 'geo::vec2{...}'`: four of them, two on line 7 and the first two `triangle.add` lines. The literals that give `.y` still compile.
3. Yes, it only reads `this`. It builds a new polygon and pushes the scaled points (copies: `vec2` is a copy type), and copies the label with `label.clone()`, because the new polygon needs its own string.
4. `error[E3028]: 'label' is the name of a field of 'polygon'`. Call the parameter `new_label`. With a free `corners()` next to the method `corners()`, a bare `corners()` in a method is `error[E3026]: 'corners' is both a member of 'polygon' and 'geo::corners'; write the one you mean`: `this.corners()` or `geo::corners()`.
5. `error[E3065]: 'geo::polygon' is declared by two modules, 'geometry' and 'shapes'`, reported where `main.ed` first **uses** the name, with notes pointing at both declarations. Importing both is fine until the name is used, because only then is it ambiguous.
</details>

## Recap

- `struct` is plain public data, `class` encapsulates. Both are built with `T{ .field = ... }`; classes offer named `static` factories instead of constructors.
- Members are named bare, and no local or parameter can hide one.
- The `.edh` declares, the `.ed` defines, nothing is written twice. Private fields and helpers live only in the `.ed`.
- Modules give visibility, namespaces give names.

Next: [lesson 4](../04-enums-and-errors/README.md), enums, `match`, and errors as values.
