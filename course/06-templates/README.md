# Lesson 6: Templates

Edax templates are C++ templates with concepts built in. There is no SFINAE and no
specialization, and a constrained template is checked when it is written, not when it is used.

## Declaring

```edax
template<type T, usize N>             // type parameters and compile-time values
class ring { ... }

template<type T : std::comparable>    // a constraint: an interface T must implement
std::option<T&> largest(std::span<T> values) { ... }

template<type T, type A = std::global_allocator>   // a default, left out from the end
class darray { ... }
```

- Template parameters are always declared; there is no `auto` parameter. A value parameter can
  be any compile-time-capable type: integers, `bool`, enums, and structs of those.
- Type arguments are always written out (`ring<std::string, 3>`). Function arguments are deduced
  from the call, but an **untyped literal never fixes `T`**: `larger(3, 4)` is an error, while
  `larger(3u32, 4)` and `larger<u32>(3, 4)` work.
- **Constrained** templates are checked at their definition: the body may only use what the
  constraints provide. A wrong type is an error **at the call**, which names the missing
  operation.
- **Unconstrained** templates are checked per instance. The error then points into the template
  and adds a note: "in the instance `sum<p>`, asked for here".
- The std constraints are structural: `std::comparable` (numbers, `char`, strings, anything with
  `<=>`), `std::cloneable`, `std::formattable`, and the operator interfaces `std::addable<R>` and
  friends. Your own interfaces work the same way (lesson 5).
- A method of a class template only has to compile if it is used.
  `std::darray<std::unique_ptr<x>>` is fine until you call its `clone()`.

## No specialization: `if compile_time`

```edax
template<type T>
std::string kind(T value) {
    if compile_time (std::is_integer<T>()) {
        return std::format(f"integer {value}");
    } else {
        return std::format(f"something else: {value}");
    }
}
```

The branch that isn't taken is not compiled for that instance, like `if constexpr`. Lookups in a
template body happen **where the template is written**, never at the instantiation site, so
there is no ADL hijacking.

## Packs

```edax
template<type... A>
void print_all(A&... values) {
    for compile_time (A& v : values) { std::print(f"{v} "); }   // one copy of the body per argument
}
```

A pack can be walked with `for compile_time`, or passed on whole as `values...`. Its length
`values.len()` is a `compile_time usize`. There are no fold expressions and no recursive
peeling.

## Checks inside templates

`compile_time check (N > 0) else "a ring needs room for at least one value";` in a template body
rejects an instance with your own message (lesson 8 covers checks).

## Your goal

`exercise.ed` needs three generic pieces:
- **`ring<T, N>`**: a fixed-capacity queue whose `push` drops (and returns) the oldest value when it
  is full.
- **`largest`**: a reference to the largest element of a span. It works for numbers and strings,
  and is refused for types without an order.
- **`join(...)`**: any number of values of any types, each described by `kind`.

The output must be:

```
dropped one
dropped two
kept three
kept four
kept five
best 9.0
last zoe
integer 7, float 2.2, something else: x, integer -3
```

Store the ring's slots as `std::array<std::option<T>, N>` (which is `default`-constructible), and
move values in and out with `std::replace` and `std::take`.

## Try this afterwards

1. `mut ring<std::string, 0> r = default; _ = r.push(std::string::from("x"));`
2. Call `largest` on a span of a struct of yours. Then add `std::ordering operator<=>(...)` to
   the struct.
3. Remove `: std::comparable` from `largest`, and call it with the struct again. Compare the
   two error messages.
4. `ring<std::unique_ptr<u32>, 2>`: does it work? (The elements are never cloned.)
