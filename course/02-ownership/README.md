# Lesson 2: Ownership: copy, move, borrow

C++ copies by default and leaves dangling references to you. Edax does neither. A value is either a
**copy type** or it has exactly one owner, and references are checked at compile time, without
lifetime annotations.

## Copy types and owning types

- **Copy types:** the built-in numbers, `bool`, `char`, raw pointers, enums whose payloads are copy
  types, and structs/classes whose fields are all copy types and that have no destructor. They are
  copied the way an `int` is.
- **Everything else** (`std::string`, `std::darray`, `std::unique_ptr`, your class with a
  destructor) is **never copied implicitly**:
  ```edax
  std::string a = std::string::from("hi");
  std::string b = a;            // ERROR: not a copy type
  std::string c = a.clone();    // a visible copy (usually allocates)
  std::string d = move a;       // ownership moves; `a` can't be used any more (checked)
  ```
- Moves happen implicitly only for **temporaries** (`push(item{...})`) and for `return local;`.
- A move is always a plain byte copy, and the source is just forgotten: there are no move
  constructors and no "valid but unspecified" moved-from objects. (No value can point into
  itself, so copying the bytes is always correct.)
- You can't `move` out of a field or through a reference. `std::take(mut obj.field)` takes the
  value and leaves the field's default behind; `std::replace(mut obj.field, v)` puts `v` there
  instead.
- RAII as in C++: locals are destroyed at the end of their scope in reverse order, and a
  moved-from variable is not destroyed.

## References are second-class

`T&` and `mut T&` never own anything, and they can only live where the compiler can see their whole
life:

| Allowed | Not allowed |
|---|---|
| parameters, locals, `for` variables, `match` bindings, lambda captures | struct/class fields (except in `view` types), container elements |
| returning a reference into `this`, or into the **only** reference/view parameter | returning a reference that could come from two parameters |

`std::str` (a string view), `std::span<T>` and `std::option<T&>` are **view types**: they follow the
same rules as references. A function that returns `std::str` must borrow from exactly one
parameter. If the result could come from either of two parameters, return an owning
`std::string`.

## One writer or many readers

While a reference is alive:
- a `mut T&` is the only way to reach the object;
- a `T&` means the object doesn't change.

```edax
for (item& it : items) {
    items.push(it.clone());          // ERROR: items is borrowed by the loop
}
swap(mut a, mut a);                  // ERROR: two mutable references to one place
integrate(mut w.positions, w.velocities);   // OK: two different fields of w
```

The rule is about **places**. `w.positions` and `w.velocities` are separate places, so they can be
borrowed independently. `xs[i]` and `xs[j]` count as the same place, because `i` may equal `j`.

Mutability is checked at every use:
- A `mut` method needs a `mut` variable.
- A `for (mut T& x : c)` loop needs a `mut` container.
- `match (mut v)` gives mutable bindings.

## `std::option` as a nullable reference

`std::option<item&>` is how a function returns "maybe a reference". Test it with `is`:
```edax
if (bag.heaviest() is std::some(h)) { std::println(h.name); }
```

## Your goal

`exercise.ed` is a small inventory program written the way you would write it in C++. It doesn't
compile. Fix it so that it prints exactly:

```
heaviest: lamp (5)
ada carries 10
bob carries 23, ada carries 0
ada
bag of bob destroyed with 3 items
bag of ada destroyed with 0 items
```

Rules of the game:
- Keep what each line does. Decide for every error whether the line should **copy** (`clone`),
  **move**, or **borrow**.
- Clone only where the original is still needed afterwards.
- The compiler reports in stages. The first errors are about mutability. Once those are fixed, the
  ownership checker reports the rest: 6 more errors.
- Look at the last two output lines: who is destroyed first, and why?

## Try this afterwards

- Make `longer` return `std::str` again, but take only **one** view parameter (a
  `std::span<std::str>` of names). Why does that compile?
- Move `std::string name` out of an `item&` with `move it.name`. Then make it work with
  `std::take`. What does the item hold afterwards?
- Add a `std::str`-typed field to `item`. The compiler tells you what the struct would have to
  become.
