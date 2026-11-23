# Lesson 5: Inheritance, interfaces, dynamic dispatch

Edax keeps single inheritance with virtual methods, and adds interfaces, which are like C++20
concepts and abstract base classes in one construct. Each use of them makes its cost visible.

## Classes: `open`, `virtual`, `override`

```edax
open class actor : entity {           // final unless `open`; base class first, then interfaces
public:
    virtual f32 speed() abstract;     // a new virtual method with no body (pure virtual)
    std::string describe() override;  // override is required, and only allowed when something matches
}
class enemy : actor {
public:
    f32 speed() override { return -1.0; }
    std::string describe() override { return std::format(f"enemy {this.actor::describe()}"); }
}
```

- **A class is final unless it is declared `open`.** Inheritance is always public and single. The
  destructor of an `open` class is virtual.
- `virtual` introduces a method; `override` implements one. They never appear together, and the
  compiler checks both directions.
- `virtual` is allowed only in `open` classes. A non-`open` class has no vtable pointer at all.
- The base version is called as `this.actor::describe()`.
- A derived class is built with the base's public fields and its own in one literal:
  `enemy{ .name = ..., .x = 10.0 }`.
- **No slicing:** copying or moving an `enemy` into an `actor` value is an error. Polymorphism
  works only through references and pointers (`actor&`, `std::unique_ptr<actor>`).

## Interfaces

```edax
interface entity {
    void update(f32 dt) mut;
    std::string describe();
    bool alive() { return true; }     // a default body; it may only use the interface's methods
}
```

- No fields. They are implemented by classes **and structs**, with `override`.
- **An interface adds nothing to the object.** An `entity&` or `std::unique_ptr<entity>` is a fat
  pointer (the object plus a table), like a Rust trait object. Only `virtual` in a class puts a
  vtable pointer inside the object.
- `implement entity for some_type { ... }` adds an interface to a type you can't edit. It must be
  written in the module of the interface or of the type, never in a third one.
- `cast::dynamic<enemy&>(e)` returns `std::option<enemy&>`, as `dynamic_cast` does, but you can't
  forget the null check.

## Two kinds of dispatch

| | Static | Dynamic |
|---|---|---|
| Written as | `template<type T : entity> void f(mut T& e)` | `void f(mut entity& e)`, `std::unique_ptr<entity>` |
| Cost | none: one copy per type, calls can inline | an indirect call |

Even dynamic calls often aren't dynamic in the end. In optimized builds the compiler sees the
whole program, and turns a call with only one possible target into a direct call (lesson 12).

## `std::unique_ptr`

`std::unique_ptr<player>::make(player{...})` allocates. A `unique_ptr<player>` converts to
`unique_ptr<entity>` implicitly; that's an upcast. `p->m()` and `*p` work as in C++. A
`unique_ptr` is never null; "maybe no pointer" is `std::option<std::unique_ptr<T>>`.

## Your goal

`exercise.ed` has the interface `entity`, a generic `step_twice` and `main`. Write the types
`actor`, `player`, `enemy` and `coin` (the comment in the file describes each one) so that it
prints:

```
ada at 6.0
enemy slime at 9.0
enemy ghost at 3.0 (dead)
coin worth 5, spun 90 degrees
2 enemies
coin worth 1, spun 180 degrees
```

The program doesn't build until the four types exist. Build often, and read what each error asks
for.

## Try this afterwards

1. Remove `open` from `actor`.
2. Write `player p = player{ .name = std::string::from("p") };` and then `actor a = move p;`.
3. In `player`, write `f32 speed() { ... }` without `override`.
4. Make `coin` a class with a destructor that prints. How often does it print, and when?
5. Call `step_twice` with an `enemy`. What does `T` become, and which `update` runs?
