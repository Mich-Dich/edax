# Lesson 7: Lambdas and callables

## Syntax

```edax
[<captures>](<typed parameters>) [-> R] { body }

auto add  = [threshold](u32 x) { return x + threshold; };   // copy (copy types only)
auto own  = [move name](u32 x) { return std::format(f"{name}: {x}"); };
auto inc  = [mut &counter]() { counter += 1; };              // mutable reference (counter is mut)
auto peek = [&list]() { return list.len(); };                // immutable reference
auto init = [value = 10u32](u32 x) -> u32 { return x + value; };
mut auto next = [mut n = 0u32]() { n += 1; return n; };     // state the lambda changes
```

- **Every capture is named:** there is no `[=]`, `[&]` or `[this]` (write `[&obj]`). The capture
  list is mandatory even when empty.
- Parameter types are always written; there are no generic lambdas. Generic code is a template
  that takes the lambda as `F`.
- A lambda with `mut` captures has a `mut` call operator: it is called through a `mut` variable or
  a `mut F` parameter, and it doesn't convert to `std::function`.
- **A lambda with reference captures is a view** (lesson 2). It can be passed down and called,
  but not stored or returned. This is how Edax prevents dangling `[&]` captures.

## Storing and passing callables

| Type | Costs | Use for |
|---|---|---|
| `template<type F> void f(F g)` | nothing (inlined) | parameters you just call |
| `std::function<R(A...)>` | always one heap allocation | stored callbacks |
| `std::inline_function<R(A...), N>` | none; captures up to N bytes inline | stored callbacks in hot paths |
| `std::fn_ptr<R(A...)>` | a plain function pointer | capture-less lambdas, C callbacks |

- A **function's name is not a value**: `plain = shout;` is an error. Wrap it in a capture-less
  lambda, `[](u32 v) { shout(v); }`, which converts to an `std::fn_ptr`.
- `std::function` is move-only; it is never copied.
- `std::function<void(), std::sendable>` adds a bound: it can only hold lambdas that may be sent
  to another thread (lesson 10).
- A field holding a callable is called like a method: `on_click(n)`.

## The std algorithms take lambdas

- `darray::retain(keep)`: removes in place.
- `sort_by(less)`: `less(a, b)` returns `bool`, not an `std::ordering`.
- `sort_by_key(std::sort_order::ascending, key)`: stable.
- `std::parallel`, the thread pool, `std::thread::spawn` (lesson 10).

## Your goal

`exercise.ed` keeps a list of scores and an event bus that stores named handlers. Implement the
TODOs so that the program prints:

```
1. ann 75
2. zoe 75
3. lee 55
4. kim 40
total 245
[log] value 245
[bonus] with bonus 250
[tagged] final: 245
[plain] plain function got 245
```

Requirements:
- `each` already takes `mut F f`. Pass it one lambda that both adds to `total` (by mutable
  reference) and calls your `rank` counter (which has its own `mut` state).
- `tagged` takes ownership of `tag`, with no clone.
- `plain` goes through an `std::fn_ptr<void(u32)>`.

## Try this afterwards

1. Return a `std::ordering` from the `sort_by` lambda (`a.points <=> b.points`). There is one
   message, at your call.
2. Store `[&total](u32 v) { ... }` in the bus. Why does it fail, while passing the same lambda to
   `each` works?
3. Give the bus a handler with a `mut` capture (`[mut count = 0u32]`).
4. Make `handlers` a `std::darray<std::inline_function<void(u32), 8>>` and capture a 16-byte
   value.
