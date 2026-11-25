# Lesson 6: Templates

Edax templates are C++ templates with concepts built in from the start, and without the parts that make C++ templates hard to read: there is no SFINAE, no specialization, no ADL, and no recursive variadic tricks. A constrained template is checked **where it is written**, so its errors point at the line that's wrong.

**You will learn:** type and value parameters, constraints (interfaces as concepts), deduction and why literals don't drive it, `if compile_time` instead of specialization, parameter packs, type aliases and distinct types.

**You will build:** a fixed-capacity ring buffer, a generic `largest`, and a variadic `join` (`exercise.ed`).

Examples are in [`examples/`](examples); `python3 course/check.py 6 --examples` checks them.

---

## 1. Declaring templates

```edax
template<type T : std::comparable>          // a type parameter with a constraint
T larger(T a, T b) { return if (a < b) { b } else { a }; }

template<type T, usize N>                   // a type and a compile-time value
struct fixed_stack {
    std::array<T, N> items = default;
    usize count = 0;
}

template<type T, type A = std::global_allocator>   // a default argument, left out from the end
class darray { ... }
```

- Parameters are always declared with `template<...>`. There are no `auto` parameters (`void f(auto x)`): that would be a template without the word "template".
- A **value parameter** can be any type that exists at compile time: integers, `bool`, enums, and structs of those.
- **Type arguments are written out** for types: `fixed_stack<u32, 4>`, never deduced from a literal (no CTAD).
- **Function** template arguments are deduced from the call: `larger(3u32, 9)` makes `T = u32`, and the `9` then takes `u32` from `T`.

### A literal never decides what `T` is

```
error[E4160]: cannot deduce template parameter 'T' of 'larger' from untyped literals
  = note: write 'larger<u32>(...)' or give a literal a suffix
```

`larger(3, 4)` would be `larger<int>` in C++. Edax has no `int` to fall back on, and choosing one silently is exactly what lesson 1 is against. So you say it: `larger(3u32, 4)` or `larger<u32>(3, 4)`.

## 2. Constraints are checked twice, at the right places

A constraint is an interface (lesson 5) that `T` must implement. The standard ones are *structural*: a type satisfies them by having the operation.

| Constraint | Satisfied by |
|---|---|
| `std::comparable` | numbers, `char`, strings, anything with `operator<=>` |
| `std::cloneable` | anything with `clone()` |
| `std::formattable` | anything that can go into an f-string hole |
| `std::addable<R>`, ... | types with that operator |
| your own interface | types that implement it |

**A constrained template is checked once, at its definition.** Its body may only use what the constraints provide. Calling it with a type that doesn't satisfy them is an error **at the call**, saying what's missing:

```
error[E4161]: 'point' has no 'operator<=>'
  --> err_constraint.ed:15:15
   |
15 |     point p = larger(point{}, point{ .x = 1.0 });
   |               ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ 'larger' needs 'std::comparable' for 'T'
   |
 9 | template<type T : std::comparable>
   |          ------------------------
   = note: the constraint is here
```

**An unconstrained template is checked per instance**, as in C++. The error then points into the template body, with a note for the call that asked for this instance:

```
error[E4013]: operator '<' is not defined for 'point'
  --> err_unconstrained.ed:11:16
   |
11 |     return if (a < b) { b } else { a };
   |                ^^^^^ no such operator
   |
15 |     point p = larger(point{}, point{ .x = 1.0 });
   |               ----------------------------------
   = note: in the instance 'larger<point>', asked for here
```

Both are readable, which is the improvement over C++. The constraint is still worth writing: it documents what the template needs, and it moves the error to the caller, who is the one who can fix it.

A method of a class template only has to compile if it is used. `std::darray<std::unique_ptr<x>>` is fine, until you call its `clone()`.

Examples: [`templates.ed`](examples/templates.ed), [`err_deduce.ed`](examples/err_deduce.ed), [`err_constraint.ed`](examples/err_constraint.ed), [`err_unconstrained.ed`](examples/err_unconstrained.ed).

---

## 3. No specialization: `if compile_time`

```edax
template<type T>
std::string kind(T value) {
    if compile_time (std::is_integer<T>()) {
        return std::format(f"integer {value}");
    } else if compile_time (std::is_float<T>()) {
        return std::format(f"float {value:.1}");
    } else {
        return std::format(f"other {value}");
    }
}
```

The branch that isn't taken is not compiled for that instance, like C++17's `if constexpr`. This one form replaces full and partial specialization, `enable_if` and tag dispatch: the variants of a template are all in one body, where you can read them together.

**Lookups happen where the template is written.** A name in a template body means what it means at the template's definition, never something found at the instantiation site. There is no ADL, so a type can't hijack a template's call to `swap` by declaring its own.

## 4. Packs

```edax
template<type... A>
void print_all(A&... values) {
    for compile_time (A& v : values) { std::println(kind(v)); }   // copied once per argument
    std::println(f"{values.len()} values");                      // a compile-time usize
}
```

- `for compile_time` walks a pack. The loop body is written once and copied once per argument, so each copy works with its own type.
- `values...` passes the whole pack on to another function.
- There are no fold expressions and no recursive "peel off the first argument" templates: the loop covers both.

Example: [`compile_time_if.ed`](examples/compile_time_if.ed).

## 5. Checks inside templates

```edax
compile_time check (N > 0) else "a ring needs room for at least one value";
```

in a template body rejects an instance with your own message, at the line that asked for it. Lesson 8 covers checks in detail.

---

## 6. Type aliases and distinct types

C++'s `using id = std::uint16_t;` is a compile-time constant whose value is a type:

```edax
compile_time type vec2i = vec2<i32>;     // an alias: the same type, another name
compile_time type id = u16;

vec2i p = vec2i{ .x = 3, .y = -1 };      // builds values like the type itself
id biggest = id::max;                    // and reaches its static members and limits
```

Types are values at compile time, so an alias can also be *computed* by a `compile_time` function (lesson 8). Write aliases at file or namespace scope.

When you want the opposite, a **new** type with the same layout that does *not* mix with the original, use `distinct type`:

```edax
distinct type meters = f32;
distinct type seconds = f32;

meters walk(meters speed_per_s, seconds t) { ... }
walk(t, t);    // error[E4010]: expected 'meters', found 'seconds'
```

`cast<meters>(x)` and `cast<f32>(m)` are the only conversions. It's C++'s "strong typedef" idiom without writing a wrapper class.

Examples: [`aliases.ed`](examples/aliases.ed), [`err_distinct.ed`](examples/err_distinct.ed).

---

## C++ → Edax at a glance

| C++ | Edax |
|---|---|
| `template<typename T> requires std::totally_ordered<T>` | `template<type T : std::comparable>` |
| `template<typename T, size_t N>` | `template<type T, usize N>` |
| `void f(auto x)` | `template<type T> void f(T x)` |
| `std::pair p{1, 2};` (CTAD) | type arguments always written |
| specialization, `enable_if` | `if compile_time (...)` |
| `if constexpr` | `if compile_time` |
| `template<typename... A>` + fold | `template<type... A>` + `for compile_time` |
| `sizeof...(A)` | `values.len()` |
| `static_assert` in a template | `compile_time check (...) else "...";` |
| `using id = uint16_t;` | `compile_time type id = u16;` |
| strong typedef wrapper | `distinct type meters = f32;` |

---

## Your goal

`exercise.ed` needs three generic pieces:
- **`ring<T, N>`**: a fixed-capacity queue whose `push` drops (and returns) the oldest value when it's full. `ring<T, 0>` must be a compile error with your own message.
- **`largest`**: a reference to the largest element of a span (the first one if several are equal). It works for numbers and strings, and is refused for types without an order.
- **`join(...)`**: any number of values of any types, each described by `kind`, separated by `", "`.

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

<details>
<summary>Hint 1: the ring's state</summary>

Store the slots as `std::array<std::option<T>, N> slots = default;` (an array of options can be `default`ed even when `T` can't), plus `usize start = 0;` (the oldest) and `usize count = 0;`. The next free slot is `(start + count) % N`.
</details>

<details>
<summary>Hint 2: moving values in and out of a slot</summary>

A slot is a place inside the ring, so `move` can't take from it (lesson 2). Use `std::replace(mut slots[i], std::some(move value))`, which also gives you the old content (the value to drop and return when the ring is full), and `std::take(mut slots[start])` in `pop`.
</details>

<details>
<summary>Hint 3: <code>largest</code></summary>

Keep `mut std::option<T&> best = std::none;` and loop with `for (T& v : values)`. Replace `best` only when `v` is greater (`!(v > b)` skips equal ones, so the first stays). Returning a reference into `values` is fine: it's the only view parameter.
</details>

<details>
<summary>Hint 4: <code>join</code></summary>

`mut std::string out = default;` then `for compile_time (A& v : values) { ... }`, adding `", "` before every piece except the first (`out.is_empty()`), and `out.push_str(kind(v))`.
</details>

---

## Try this afterwards

1. `mut ring<std::string, 0> r = default; _ = r.push(std::string::from("x"));`
2. Call `largest` on a span of a struct of yours. Then add `std::ordering operator<=>(...)` to the struct.
3. Remove `: std::comparable` from `largest`, and call it with the struct again. Compare the two error messages.
4. `ring<std::unique_ptr<u32>, 2>`: does it work? (The elements are never cloned.)
5. Add `compile_time type names = ring<std::string, 3>;` and declare `recent` with it.

<details>
<summary>What happens</summary>

1. Your message, as a compile error at the `push` call: `a ring needs room for at least one value`.
2. `error[E4161]: '<your struct>' has no 'operator<=>'` at the call. With an `operator<=>` the struct is `std::comparable` and the call compiles.
3. Now the error points into `largest`'s body (`operator '>' is not defined`), with a note for the instance. The constrained version told the caller; this one tells the template's author.
4. Yes: the ring only moves values in and out, and a `unique_ptr` moves fine.
5. It works exactly like `ring<std::string, 3>`, including `names{}` and `= default`.
</details>

## Recap

- Templates declare their parameters; literals never decide `T`.
- Constrained templates are checked at the definition and refuse types at the call; unconstrained ones are checked per instance.
- `if compile_time` replaces specialization; `for compile_time` walks packs.
- `compile_time type` is an alias, `distinct type` a new type.

Next: [lesson 7](../07-lambdas/README.md), lambdas and callables.
