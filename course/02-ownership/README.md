# Lesson 2: Ownership: copy, move, borrow

In C++ you decide, for every value, whether a line copies it, moves it or points at it, and the language trusts you: a copy may allocate behind your back, a moved-from object stays around in a "valid but unspecified" state, and a reference may outlive what it points to. Edax makes the same three decisions **visible in the code and checked by the compiler**. There are no lifetime annotations: a few simple rules about where references may live do the work.

**You will learn:** copy types vs owning types, `move`, `clone`, `std::take`, destruction order, references as "second-class" values, view types (`std::str`, `std::span`), and the "one writer or many readers" rule.

**You will build:** you will fix a C++-style inventory program that doesn't compile (`exercise.ed`).

Examples are in [`examples/`](examples); `python3 course/check.py 2 --examples` checks them.

---

## 1. Two kinds of type

Every type is one of two kinds, and the compiler works out which:

| Copy types | Owning types (everything else) |
|---|---|
| numbers, `bool`, `char`, raw pointers | `std::string`, `std::darray`, `std::unique_ptr`, `std::file` |
| enums whose payloads are copy types | enums with an owning payload |
| structs and classes whose fields are all copy types **and** that have no destructor | anything with a destructor, or with an owning field |
| copied by `=` and by value passing, like an `int` | **never copied implicitly** |

A `point { f32 x; f32 y; }` is a copy type: `point b = a;` copies it, as in C++. A struct with a `std::string` field is an owning type, and `=` on it doesn't compile. That rule is the whole point: in C++, `item b = a;` might allocate a new string, and nothing at the call site tells you. In Edax every allocation is something you can see.

## 2. Owning types: move, clone, or take

With an owning type, you say what you mean:

```edax
std::string a = std::string::from("hi");
std::string b = a;            // ERROR E5001: not a copy type
std::string c = a.clone();    // a copy you can see (it allocates)
std::string d = move a;       // ownership moves to d; a can't be used any more
std::println(a);              // ERROR E5002: 'a' is used after it was moved
```

The compiler tells you both options:

```
error[E5001]: 'a' has type 'std::string', which is not a copy type
 --> err_copy.ed:6:21
  |
6 |     std::string b = a;
  |                     ^ would be copied
  = note: write 'move a' to transfer ownership, or 'a.clone()' for a visible copy
```

**What a move is.** A move copies the bytes and then forgets the source: no move constructor runs, and the source isn't left in some special state. It simply can't be used again (checked at compile time), and its destructor doesn't run. This only works because no Edax value can point into itself (references are never stored, see below), so copying the bytes is always correct.

**Implicit moves** happen only where nobody could use the value afterwards:
- a **temporary**: `bag.add(item{ ... })`, `std::ok(parse(x))`;
- `return local;` of a local variable or by-value parameter.

Everything else needs `move`.

### Fields: `std::take` and `std::replace`

`move` works only on local variables and by-value parameters. You can't `move obj.field`, because `obj` would be left with a hole in it, and its destructor would later destroy a field that's gone:

```
error[E3032]: only local variables and by-value parameters can be moved
  = note: to take a field's value use 'std::take(mut obj.field)' or 'std::replace(mut obj.field, value)'
```

Instead:
- `std::take(mut obj.field)` returns the value and leaves the field's **default** behind (an empty string, an empty darray, `std::none`).
- `std::replace(mut obj.field, new_value)` returns the value and puts `new_value` in its place.

Both leave the object complete, so its destructor stays correct.

### Choosing: a small decision table

When the compiler says "not a copy type", ask what the line is *for*:

| The line should... | Write | Afterwards |
|---|---|---|
| give the value away, and the variable isn't needed again | `move x` | `x` is gone |
| keep the original **and** have a second, independent one | `x.clone()` | two values, one allocation more |
| take what a field holds and leave the object empty | `std::take(mut obj.f)` | `obj.f` is the default |
| look at it, without owning it | a reference: `T&` parameter, `for (T& x : ...)` | nothing changes hands |

The tempting mistake is to use `clone()` to silence every error. It compiles, but it changes what the program does: a method meant to *hand over* a bag's items that returns `items.clone()` leaves the bag full. Clone only when you really want two copies.

Example: [`copy_move_clone.ed`](examples/copy_move_clone.ed).

## 3. Destruction: RAII as in C++

- Locals are destroyed at the end of their scope, in **reverse order of declaration**.
- A moved-from variable is **not** destroyed: its value lives on wherever it went.
- Temporaries are destroyed at the end of the statement.
- An object's destructor body runs first, then its fields are destroyed in reverse order.

```edax
noisy a = noisy::make("a");
noisy b = noisy::make("b");
noisy c = noisy::make("c");
keep(move b);            // b's value is destroyed at the end of keep()
// end of main: destroy c, then destroy a (b was moved away)
```

Example: [`destruction.ed`](examples/destruction.ed).

## 4. References are second-class

`T&` and `mut T&` never own anything, and the compiler only lets them live where it can see their whole life:

| References may be | References may **not** be |
|---|---|
| parameters | struct/class fields (except in `view` types) |
| local variables, `for` variables, `match` bindings | elements of containers (except containers that are themselves views) |
| lambda captures (lesson 7) | returned, except by the rule below |

**Returning a reference** is allowed when it points into `this` (in a method) or into the function's **only** reference or view parameter. Then the compiler knows what the result borrows from, and checks the caller keeps that alive:

```edax
std::option<item&> heaviest() { ... }        // OK: borrows from this
std::str first_word(std::str s) { ... }      // OK: borrows from s, the only view parameter
std::str longer(std::str a, std::str b) { }  // ERROR E5012: could borrow from a or from b
```

```
error[E5012]: 'longer' returns 'std::str', which could borrow from 'a' or 'b'
  = note: a returned reference must borrow from 'this' or from the only reference parameter
```

That's the trade Edax makes instead of lifetime annotations: a function whose result could come from either of two inputs returns an owned value (`std::string`), or is split into two functions.

**Why this is enough.** Almost every real use of a reference is "borrow this for the duration of a call or a loop". Those are exactly the places where references are allowed. The rare cases that need more (storing a borrowed pointer for later) are where C++ programs dangle.

### View types

A **view type** holds references inside, so it follows the same rules as a reference:
- `std::str` (like `std::string_view`) and `std::span<T>`;
- `std::option<T&>` ("maybe a reference", the safe nullable pointer);
- a lambda that captures by reference (lesson 7);
- containers of views, such as `std::darray<std::str>`;
- your own `view struct` / `view class` (lesson 9).

`std::static_str` (a string literal) is *not* a view: it points to the program's static memory, which lives forever, so it can be stored anywhere.

Examples: [`references.ed`](examples/references.ed), [`err_references.ed`](examples/err_references.ed).

## 5. One writer or many readers

While a reference is alive:
- a `mut T&` is **the only way** to reach the object, and
- a `T&` means the object **doesn't change**.

```edax
for (u32& x : xs) {
    xs.push(x);                 // ERROR E5021: the loop borrows xs
}
swap_values(mut n, mut n);      // ERROR E5020: two mutable references to n
grow(mut xs, xs);               // ERROR E5020: xs is borrowed mutably and shared
```

The first one is C++'s iterator invalidation: `push` may reallocate the buffer the loop is reading. Edax rejects it at compile time:

```
error[E5021]: cannot call mut method 'push' on 'xs' while the loop borrows it
  --> err_aliasing.ed:18:9
   |
18 |         xs.push(x);
   |         ^^ the loop holds a reference
   |
17 |     for (u32& x : xs) {
   |     -------------------
   = note: 'x' refers into 'xs' for the whole loop
```

### Places, not variables

The rule is about **places**: a variable followed by fields, like `w`, `w.positions`, `w.camera.position`. Two places overlap when one is a prefix of the other. So:

```edax
integrate(mut w.positions, w.velocities);   // OK: two different fields
w.positions.push(w.positions[0]);           // OK: the value is read before push starts
integrate(mut w.positions, w.positions);    // ERROR: the same place, mutably and shared
swap(mut xs[i], mut xs[j]);                 // ERROR: i might equal j; use xs.swap(i, j)
```

An index ends a place: the compiler can't know whether `i == j`, so `xs[i]` and `xs[j]` count as one place. Containers offer run-time-checked ways to hand out several elements (`split_at`, `get_many_mut`).

The second line works because of a **two-phase rule**: an argument passed *by value* (a copy type here) is read before the call starts, so it may read the very thing the call is about to change. `v.push(v[0])` is fine; passing `v[0]` *by reference* to a function that also gets `mut v` is not.

### Mutability is checked at every use

- Calling a `mut` method (one that changes `this`) needs a `mut` variable.
- `for (mut T& x : c)` needs a `mut` container.
- `match (mut v)` gives mutable bindings (lesson 4).

Examples: [`err_aliasing.ed`](examples/err_aliasing.ed), [`places.ed`](examples/places.ed).

**Why it's worth it:** these rules are also what makes Edax fast. Because a `mut T&` is known to be the only way to its object, the compiler tells LLVM that it doesn't alias anything, like `restrict` on every parameter (lesson 12). And in lesson 10, the same rules prevent data races between threads.

---

## C++ → Edax at a glance

| C++ | Edax |
|---|---|
| `T b = a;` (copy of an owning type) | `T b = a.clone();` |
| `T b = std::move(a);` | `T b = move a;` |
| `T v = std::exchange(obj.f, {});` | `T v = std::take(mut obj.f);` |
| `T v = std::exchange(obj.f, x);` | `T v = std::replace(mut obj.f, x);` |
| `const T&` parameter | `T&` parameter |
| `T&` parameter | `mut T&` parameter, `f(mut x)` at the call |
| `T*` that may be null | `std::option<T&>` |
| `std::string_view` | `std::str` |
| a `T&` member | not allowed: store a value, a smart pointer, or make the type a `view` |

---

## Your goal

`exercise.ed` is a small inventory program written the way you would write it in C++. It doesn't compile. Fix it so that it prints exactly:

```
heaviest: lamp (5)
ada carries 10
bob carries 23, ada carries 0
ada
bag of bob destroyed with 3 items
bag of ada destroyed with 0 items
```

Rules of the game:
- Keep what each line does. For every error decide whether the line should **copy** (`clone`), **move** (`move`, `std::take`) or **borrow** (a reference).
- Clone only where the original is still needed afterwards.
- The compiler reports in **stages**. First 2 errors about mutability. Once those are fixed, the ownership checker reports 6 more. Don't be discouraged when the count goes up: it's the next stage, not new mistakes.
- Look at the last two output lines: who is destroyed first, and why?

<details>
<summary>Hint 1: the two mutability errors</summary>

Both notes say exactly what to write. A loop that changes the elements is `for (mut item& it : ...)`, and a variable you call a `mut` method on (`add`) is declared `mut`.
</details>

<details>
<summary>Hint 2: <code>empty_out</code></summary>

The comment says the bag is empty afterwards. `items.clone()` compiles, but then the bag keeps its items and the third output line says `ada carries 10`. You want to take the field's value and leave an empty darray behind: that's `std::take`.
</details>

<details>
<summary>Hint 3: <code>rope</code> is used twice</summary>

The first use needs a copy, because `rope` is used again two lines later. The second use is the last one, so it can give `rope` away.
</details>

<details>
<summary>Hint 4: <code>longer</code></summary>

The result could be either parameter, so it can't be a view. Return a new `std::string` (`std::string::from(a)`).
</details>

<details>
<summary>Hint 5: the destruction order</summary>

Locals are destroyed in reverse order of declaration: `yours` was declared after `mine`.
</details>

---

## Try this afterwards

1. Make `longer` return `std::str` again, but take only **one** view parameter: a `std::span<std::str>` of names. Why does that compile?
2. In `main`, write `std::string n = move taken[0].name;`. Then make it work with `std::take`. What does the item hold afterwards?
3. Add a field `std::str label = "";` to `item`. The compiler tells you what the struct would have to become. Why could `bag` then no longer hold items?
4. Declare `taken` as `mut` and write `for (item& it : taken) { taken.push(it.clone()); }`. Which C++ bug is this?

<details>
<summary>What happens</summary>

1. With one view parameter, the compiler knows the result borrows from it (from the span's strings), so returning a `std::str` is fine.
2. `error[E3032]: only local variables and by-value parameters can be moved`. With `std::take(mut taken[0].name)` (and `taken` declared `mut`) it works, and the item's name is empty afterwards.
3. `error[E5011]: field 'item::label' has view type 'std::str', but 'item' is not a view type`. A `view struct` follows the reference rules, so a `std::darray<item>` would be a view too, and a view can't be a field of `bag`.
4. Iterator invalidation: `push` can reallocate the buffer the loop is reading. Here it's `error[E5021]` at compile time.
</details>

## Recap

- Copy types copy; owning types are never copied implicitly. `move`, `clone()`, `std::take`, `std::replace` say what happens.
- A move copies bytes and forgets the source. Only temporaries and `return local;` move implicitly.
- References and views live in parameters, locals, loops and captures. A returned one borrows from `this` or the one reference parameter.
- While a `mut` reference lives, it is the only way to its place. Different fields are different places.

Next: [lesson 3](../03-types-and-modules/README.md), structs, classes and modules.
