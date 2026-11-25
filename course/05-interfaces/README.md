# Lesson 5: Inheritance, interfaces, dynamic dispatch

Edax keeps what C++ programmers use inheritance for (a base class with shared state and `virtual` methods) and removes the traps: a class is final unless it says otherwise, `override` is required and checked, and objects can't be sliced. Next to classes it adds **interfaces**: one construct that works like a C++20 concept in templates and like an abstract base class at run time. Every use makes its cost visible.

**You will learn:** `open`, `virtual`, `override`, `abstract`, calling the base version, interfaces with default methods, `implement ... for`, `cast::dynamic`, `std::unique_ptr` and the difference between static and dynamic dispatch.

**You will build:** a tiny game world (`exercise.ed`).

Examples are in [`examples/`](examples); `python3 course/check.py 5 --examples` checks them.

---

## 1. Classes: `open`, `virtual`, `override`

```edax
open class animal {                               // others may derive from it
public:
    std::string name;

    virtual std::static_str sound() abstract;     // a new virtual method without a body
    virtual std::string greet() { return std::format(f"{name} says {sound()}"); }
}

class dog : animal {                              // final: nothing derives from dog
public:
    std::static_str sound() override { return "woof"; }
}

class puppy : animal {
public:
    u32 age = 0;
    std::static_str sound() override { return "yip"; }
    std::string greet() override { return std::format(f"{this.animal::greet()} (age {age})"); }
}
```

| Keyword | Means | C++ |
|---|---|---|
| `open class` | may be derived from | every class (Edax classes are `final` by default) |
| `virtual` | **introduces** a method that derived classes may replace | `virtual` |
| `abstract` (instead of a body) | introduces it without a body | `= 0` |
| `override` | **replaces** an inherited method; required | `override` (optional in C++) |

- `virtual` and `override` never appear on the same method: one introduces, the other replaces. The compiler checks both directions, so these C++ bugs become errors:

  | Mistake | Error |
  |---|---|
  | deriving from a class that isn't `open` | `error[E3040]: class 'closed' is not open and cannot be inherited from` |
  | a method with the name of a virtual one, without `override` (in C++, a silent new method) | `error[E3044]: 'value' has the same name as 'base::value()'` + "add 'override' or rename it" |
  | `override` that matches nothing (a typo in the name or a parameter) | `error[E3043]: 'child::other()' is marked 'override' but overrides nothing` |

- `virtual` is only allowed in an `open` class. A class that isn't `open` has no vtable pointer at all, so it costs nothing. The destructor of an `open` class is automatically virtual.
- Inheritance is single and always public. The base class comes first after `:`, then any interfaces: `open class actor : entity { ... }` (here `entity` is an interface).
- The base version is called as `this.animal::describe()`.
- A derived object is built in **one** literal with the base's public fields and its own: `puppy{ .name = std::string::from("pip"), .age = 1 }`. There are no constructors to chain.

### No slicing

Copying a `puppy` into an `animal` *value* would cut off the `puppy` part (C++'s slicing). In Edax it is an error:

```
error[E4010]: expected 'base', found 'child'
  = note: copying a derived object into a base value would slice it; use a reference
```

Polymorphism works through references and pointers only: `animal&`, `mut animal&`, `std::unique_ptr<animal>`. A `puppy&` converts to an `animal&` implicitly (an upcast is always safe).

Examples: [`classes.ed`](examples/classes.ed), [`err_classes.ed`](examples/err_classes.ed), [`err_slicing.ed`](examples/err_slicing.ed).

---

## 2. Interfaces

```edax
interface shape {
    f32 area();
    std::string describe() { return std::format(f"a shape of area {area():.1}"); }   // default body
}

struct square : shape {
    f32 side = 0.0;
    f32 area() override { return side * side; }      // keeps the default describe()
}
```

- An interface has **no fields**: only methods, some with default bodies. A default body may only use the interface's own methods.
- Classes **and structs** implement interfaces, with `override`. A type may implement many.
- **An interface adds nothing to the object.** A `square` is still 4 bytes. A reference to an interface, `shape&` or `std::unique_ptr<shape>`, is a *fat pointer*: the object's address plus a pointer to the table of `square`'s methods. (Like a Rust trait object, or a C++ `std::function` that stores the type's function table next to the object.) Only `virtual` in a class puts a vtable pointer *inside* the object.
- To add an interface to a type you can't edit, write `implement shape for circle { ... }`. It must be written in the module of the interface or of the type, never in a third one, so two libraries can't both implement the same interface for the same type.

### Asking for the concrete type: `cast::dynamic`

```edax
if (cast::dynamic<circle&>(*s) is std::some(c)) { std::println(c.radius); }
```

It returns an `std::option<circle&>`: like C++'s `dynamic_cast` to a pointer, except that you can't forget the null check, because the reference is only reachable through `is`.

### Two kinds of dispatch

The same interface works both ways, and the call site decides:

| | Static | Dynamic |
|---|---|---|
| Written as | `template<type T : shape> f32 twice_area(T& s)` | `f32 total(std::span<std::unique_ptr<shape>> shapes)` |
| Works like | a C++20 concept | a C++ abstract base class |
| Code | one copy per type | one copy |
| Call | direct, can be inlined | indirect, through the table |
| Use when | the type is known where you call | different types live in one container |

Dynamic calls are often cheaper than they look: optimized builds see the whole program, and turn an interface call with only one possible target into a direct call (lesson 12).

Example: [`interfaces.ed`](examples/interfaces.ed).

---

## 3. `std::unique_ptr`

- `std::unique_ptr<square>::make(square{ ... })` allocates and returns the pointer.
- A `unique_ptr<square>` converts to `unique_ptr<shape>` implicitly (an upcast).
- `p->area()` and `*p` work as in C++.
- **A `unique_ptr` is never null.** "Maybe no object" is `std::option<std::unique_ptr<T>>`, so the type tells you whether you have to check.
- It's an owning type (lesson 2): it moves, and it isn't copied.

---

## C++ → Edax at a glance

| C++ | Edax |
|---|---|
| `class base { virtual ~base(); };` | `open class base { ... }` (the destructor is virtual automatically) |
| `class d final : public base` | `class d : base` |
| `virtual void f() = 0;` | `virtual void f() abstract;` |
| `void f() override;` | `void f() override;` (required) |
| `base::f()` | `this.base::f()` |
| abstract base class / concept | `interface` |
| `dynamic_cast<d*>(p)` | `cast::dynamic<d&>(*p)`, an `std::option<d&>` |
| `std::make_unique<d>(...)` | `std::unique_ptr<d>::make(d{ ... })` |
| `std::unique_ptr<T>` that may be null | `std::option<std::unique_ptr<T>>` |

---

## Your goal

`exercise.ed` has the interface `entity`, a generic `step_twice` and `main`. Write the types `actor`, `player`, `enemy` and `coin` (the comment in the file describes each one) so that it prints:

```
ada at 6.0
enemy slime at 9.0
enemy ghost at 3.0 (dead)
coin worth 5, spun 90 degrees
2 enemies
coin worth 1, spun 180 degrees
```

The program doesn't build until the four types exist. Build often, one type at a time, and read what each error asks for.

<details>
<summary>Hint 1: the shape of <code>actor</code></summary>

```edax
open class actor : entity {
public:
    std::string name;
    f32 x = 0.0;
    i32 health = 10;

    virtual f32 speed() abstract;
    void update(f32 dt) mut override { ... }
    std::string describe() override { ... }
    bool alive() override { ... }
}
```
It implements the interface's methods with `override`, and introduces `speed` with `virtual`.
</details>

<details>
<summary>Hint 2: <code>player</code> and <code>enemy</code></summary>

They derive from `actor` (`class player : actor`) and only override what differs. `enemy`'s `describe` calls the base: `std::format(f"enemy {this.actor::describe()}")`.
</details>

<details>
<summary>Hint 3: <code>coin</code></summary>

A `struct coin : entity` with its two fields, `update` and `describe`. It doesn't write `alive`, so it keeps the interface's default.
</details>

---

## Try this afterwards

1. Remove `open` from `actor`.
2. Write `player p = player{ .name = std::string::from("p") };` and then `actor a = move p;`.
3. In `player`, write `f32 speed() { ... }` without `override`.
4. Make `coin` a class with a destructor that prints. How often does it print, and when?
5. Call `step_twice` with an `enemy`. What does `T` become, and which `update` runs?

<details>
<summary>What happens</summary>

1. `error[E3040]: class 'actor' is not open and cannot be inherited from`, once for `player` and once for `enemy`, and before them `error[E3042]: 'speed' is abstract but 'actor' is not an open class`.
2. `error[E4010]: expected 'actor', found 'player'` with the note about slicing. Use `actor& a = p;` instead.
3. `error[E3044]: 'speed' has the same name as 'actor::speed()'`, with "add 'override' or rename it".
4. Twice, after the last line of output: first `coin 1` (`c`, declared last), then `coin 5`, the one inside `world`, when the darray is destroyed. A class with a destructor isn't a copy type, so the `coin{ ... }` literals are moved into place, never copied: there is never a third coin to destroy.
5. `T` is `enemy`, so the compiler makes a copy of `step_twice` for `enemy`, and `e.update(dt)` is a direct call to `actor::update` (which `enemy` inherits). Inside it, `speed()` is still a virtual call: `actor::update` is compiled once for every actor.
</details>

## Recap

- Classes are final unless `open`; `virtual` introduces, `override` replaces, both are checked.
- No slicing: polymorphism through references and `std::unique_ptr` only.
- Interfaces have no fields, add nothing to the object, and serve both as template constraints and as fat-pointer types.
- `cast::dynamic` returns an `std::option`; a `unique_ptr` is never null.

Next: [lesson 6](../06-templates/README.md), templates without SFINAE.
