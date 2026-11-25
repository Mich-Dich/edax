# Lesson 7: Lambdas and callables

Lambdas look almost like C++'s, with two differences that matter: **every capture is named**, and a lambda that captures a reference follows the reference rules from lesson 2. That is all it takes to rule out C++'s most common lambda bug, a `[&]` callback that outlives the locals it captured.

**You will learn:** capture forms, lambdas with state, which callable type to use where (templates, `std::function`, `std::inline_function`, `std::fn_ptr`), and the algorithms that take lambdas.

**You will build:** an event bus and a leaderboard (`exercise.ed`).

Examples are in [`examples/`](examples); `python3 course/check.py 7 --examples` checks them.

---

## 1. Syntax and captures

```
[<captures>](<typed parameters>) [-> R] { body }
```

```edax
auto add   = [threshold](u32 x) { return x + threshold; };          // copy (copy types only)
auto label = [move name](u32 x) { return std::format(f"{name}: {x}"); };   // the lambda owns name
auto peek  = [&list]() { return list.len(); };                      // immutable reference
auto inc   = [mut &counter]() { counter += 1; };                    // mutable reference
auto init  = [base = 5u32](u32 x) -> u32 { return x + base; };     // a new variable
mut auto next = [mut n = 0u32]() { n += 1; return n; };            // state the lambda changes
```

| Capture | Means | C++ |
|---|---|---|
| `[x]` | copy `x` (it must be a copy type) | `[x]` |
| `[move x]` | move `x` into the lambda | `[x = std::move(x)]` |
| `[&x]` | immutable reference | `[&x]` (as `const`) |
| `[mut &x]` | mutable reference | `[&x]` |
| `[y = expr]` | a new variable initialized from `expr` | `[y = expr]` |
| `[mut n = 0u32]` | a variable the lambda may change between calls | `[n = 0u] () mutable` |

- **Every capture is named.** There is no `[=]`, `[&]` or `[this]`: `error[E2026]: '[=]' captures are not allowed; name every capture`. A capture list shows what the lambda depends on, which a reader (and the borrow checker) needs. To use an object's members, capture the object: `[&obj]`.
- The capture list is required, even when empty: `[]`.
- Parameter types are always written. There are no generic lambdas (`[](auto x)`): generic code is a template that takes the lambda as `F` (below).
- The return type is deduced from the body unless you write `-> R`.
- A lambda with a `mut` capture has a `mut` call operator. You call it through a `mut` variable (`mut auto next = ...`) or a `mut F` parameter.

Example: [`captures.ed`](examples/captures.ed).

## 2. A lambda with a reference capture is a view

A lambda that captures `&x` or `mut &x` holds a reference, so it **is** a view type (lesson 2). It can be called, and passed *down* to a function that calls it, but it can't be stored or returned:

```edax
mut u32 total = 0;
each(scores.as_span(), [mut &total](score& s) { total += s.points; });   // OK: called and gone
bus.on([mut &total](u32 v) { total += v; });                             // ERROR: stored
```

```
error[E5015]: a std::function cannot hold a lambda with reference captures
  --> err_view_lambda.ed:14:11
   |
14 |     b.on([mut &total](u32 v) { total += v; });
   |           ^^^^^^^^^^ a reference capture
   = note: a std::function is stored, and a reference could outlive what it refers to (spec 18); capture by value or 'move'
```

In C++ the second line compiles and becomes a use-after-return as soon as the function that owns `total` returns while the bus lives on. In Edax, a callback that's kept must own what it uses: a copy, a `move`, or a shared object (`std::shared_ptr`, a `std::mutex<T>` from lesson 10).

Example: [`err_view_lambda.ed`](examples/err_view_lambda.ed).

## 3. Which callable type?

| Type | Cost | Use for |
|---|---|---|
| `template<type F> void f(F g)` | nothing: one copy of `f` per lambda, calls inline | parameters you just call (any lambda, even a view) |
| `std::function<R(A...)>` | one heap allocation | callbacks you **store** |
| `std::inline_function<R(A...), N>` | none: captures up to `N` bytes stored inline | stored callbacks in hot paths |
| `std::fn_ptr<R(A...)>` | a plain function pointer | capture-less lambdas, C callbacks |

- `std::function` is move-only: it's never copied (copying would copy the captures, which might allocate).
- `std::function<void(), std::sendable>` adds a bound: it only accepts lambdas that may be sent to another thread (lesson 10).
- A field holding a callable is called like a method: `on_click(n)`, `handlers[i](v)`.

### A function's name is not a value

```edax
std::fn_ptr<void(u32)> f = shout;                      // ERROR E4073
std::fn_ptr<void(u32)> f = [](u32 v) { shout(v); };    // OK: a capture-less lambda converts
```

```
error[E4073]: 'shout' is a function; call it
  = note: write 'shout(...)'; a function is not a value: to pass it on, wrap it in a lambda, '[](...) { shout(...); }'
```

Overloaded functions have no single address, and a name that is sometimes a call and sometimes a pointer is a C-ism Edax drops. The wrapping lambda is free: it's inlined.

Examples: [`callables.ed`](examples/callables.ed), [`err_fn_name.ed`](examples/err_fn_name.ed), [`err_captures.ed`](examples/err_captures.ed).

---

## 4. The algorithms take lambdas

The container algorithms are methods:

| Method | Does | Note |
|---|---|---|
| `xs.retain(keep)` | removes in place the elements for which `keep(x)` is false | C++'s erase-remove idiom in one call |
| `xs.sort_by(less)` | sorts with `less(a, b)`, a **`bool`** | not an `std::ordering`; not stable |
| `xs.sort_by_key(std::sort_order::ascending, key)` | sorts by `key(x)` | stable |
| `xs.sort()` | sorts comparable elements | |

```edax
xs.retain([](u32& v) { return v % 3 == 0; });
scores.sort_by([](score& a, score& b) {
    if (a.points != b.points) { return a.points > b.points; }   // highest first
    return a.player < b.player;                                   // then by name
});
```

The lambdas take their elements by reference (`u32&`, `score&`): the algorithm lends you each element, it doesn't copy it.

`std::parallel`, the thread pool and `std::thread::spawn` take lambdas too (lesson 10).

Example: [`algorithms.ed`](examples/algorithms.ed).

---

## C++ → Edax at a glance

| C++ | Edax |
|---|---|
| `[=]`, `[&]`, `[this]` | name every capture: `[x]`, `[&x]`, `[&obj]` |
| `[x = std::move(x)]` | `[move x]` |
| `[n = 0]() mutable { ++n; }` | `mut auto f = [mut n = 0u32]() { n += 1; };` |
| `[](auto x)` | a template that takes `F` |
| `std::function<void(int)>` | `std::function<void(i32)>` (move-only) |
| `void(*)(int)` | `std::fn_ptr<void(i32)>` |
| `&shout` | `[](u32 v) { shout(v); }` |
| `v.erase(std::remove_if(...), v.end())` | `v.retain(keep)` |
| `std::sort(v.begin(), v.end(), less)` | `v.sort_by(less)` |
| `std::stable_sort` by a key | `v.sort_by_key(std::sort_order::ascending, key)` |

---

## Your goal

`exercise.ed` keeps a list of scores and an event bus that stores named handlers. Implement the TODOs so that the program prints:

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
- `each` already takes `mut F f`. Pass it **one** lambda that both adds to `total` (by mutable reference) and calls your `rank` counter (a lambda with its own `mut` state).
- `tagged` takes ownership of `tag`, with no clone.
- `plain` goes through an `std::fn_ptr<void(u32)>`.

<details>
<summary>Hint 1: the event bus</summary>

`on` stores the name (`std::string::from(name)`) and moves the handler in (`handlers.push(move handler)`). `emit` walks both darrays by index: `std::print(f"[{names[i]}] "); handlers[i](value);`.
</details>

<details>
<summary>Hint 2: <code>rank</code> and the lambda passed to <code>each</code></summary>

`mut auto rank = [mut next = 1u32]() { u32 r = next; next += 1; return r; };`. The lambda for `each` captures both by mutable reference, `[mut &total, mut &rank](score& s) { ... }`, because it changes `total` and calls `rank`, whose call changes its state.
</details>

<details>
<summary>Hint 3: the four handlers</summary>

`[](u32 v) { ... }` for `log`, `[bonus](u32 v)` for `bonus`, `[move tag](u32 v)` for `tagged`. For `plain`, first `std::fn_ptr<void(u32)> plain = [](u32 v) { shout(v); };`, then a handler that captures `plain` by copy (a function pointer is a copy type) and calls it.
</details>

---

## Try this afterwards

1. Return a `std::ordering` from the `sort_by` lambda (`a.points <=> b.points`).
2. Store `[mut &total](u32 v) { total += v; }` in the bus. Why does it fail, while passing the same kind of lambda to `each` works?
3. Give the bus a handler with a `mut` capture (`[mut count = 0u32](u32 v) { count += 1; }`).
4. Make `handlers` a `std::darray<std::inline_function<void(u32), 8>>` and capture a 16-byte value.

<details>
<summary>What happens</summary>

1. One error, at your call: `error[E4210]: sort_by takes less(a, b) returning bool; a three-way comparison is (a <=> b) == std::ordering::less`. The message is `sort_by`'s own entry check (lesson 8 shows how to write those).
2. `error[E5015]: a std::function cannot hold a lambda with reference captures`. `each` takes the lambda as a template parameter and calls it before returning, so the reference can't outlive `total`. The bus *stores* it.
3. `error[E4010]: expected 'std::function<void(u32)>', found 'lambda(u32)'`. A lambda with a `mut` capture has a `mut` call operator, and `std::function` calls through an immutable one (`emit` isn't `mut` either), so the lambda doesn't convert. Keep such state outside the handler, for example as a field of the bus that `emit` updates.
4. `error[E4010]: this lambda's captures take 16 bytes, but 'std::inline_function<void(u32), 8>' holds 8`, with a note on how big to make it. The `tagged` handler fails the same way (an `std::string` takes 24 bytes). That's the point of the type: no hidden heap allocation, ever.
</details>

## Recap

- Every capture is named: copy, `move`, `&`, `mut &`, or a new variable.
- A lambda with reference captures is a view: call it, pass it down, never store it.
- Templates for callables you call, `std::function` for callables you store, `std::fn_ptr` for plain functions, `std::inline_function` when allocation matters.
- A function's name isn't a value: wrap it in a lambda.

Next: [lesson 8](../08-compile-time/README.md), compile-time code and checks.
