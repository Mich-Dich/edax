# Lambdas

## Rules

```
[<captures>](<parameters>) [-> <return_type>] { <body> }
```
- The capture list is **mandatory**, even when empty: `[]`. A `[...]` followed by `(` is always a capture list; anything else in brackets is a list literal (see [Tuples and List Literals](../basics/tuples-and-lists.md)).
- **Every capture is named.** There is no `[=]` or `[&]`.
- Parameter types are always written. Parameters follow the rules for functions (see [Functions](functions.md)), except that a lambda has no `implicit` and no `compile_time` parameters: a lambda is usually called through a template parameter or a `std::function`, where the call site is not the place the lambda's author can see.
- The return type is deduced from the `return` statements, or written explicitly with `-> T`.

| Capture | Meaning |
|---|---|
| `[x]` | Copy (copy types only) |
| `[move x]` | Move `x` into the lambda |
| `[&x]` | Immutable reference |
| `[mut &x]` | Mutable reference (`x` must be `mut`) |
| `[name = expr]` | Initialize a new captured value |

```edax
mut u32 counter = 0;
u32 threshold = 10;

auto inc  = [mut &counter]() { counter += 1; };
auto add  = [threshold](u32 x) { return x + threshold; };
auto init = [value = 10u32](u32 x) -> u32 { return x + value; };
auto own  = [move name](u32 x) { return std::format(f"{name}: {x}"); };
auto noop = []() { };

employees.retain([](employee& e) { return e.age >= 30; });
```

### Types and storage
- Every lambda has its own unnamed type, so lambdas are stored with `auto` or passed to template parameters.
- A lambda with **reference captures is a view type** (see [Ownership, Copies, Moves and References](../memory/ownership-and-references.md)). It can be passed down and called, but not stored in a field or returned.
- A lambda is `std::sendable` / `std::shareable` when its captures are (see [Threads](../../stdlib/concurrency/threads.md)). `std::thread::spawn` takes any sendable lambda; one with reference captures becomes a borrowing (scoped) thread.
- To store a callable, use `std::function<R(Args...)>`. It **always** puts the callable on the heap, and the type makes that visible.
- Lambdas with no captures convert to `std::fn_ptr<R(Args...)>`, a plain function pointer for callbacks and FFI.
- A parameter of a callable type may be a mutable reference: `std::function<void(mut ui::painter&, ui::button_part&)>`. The call marks the argument as any call does: `skin.button(mut p, part)`. A stored callback can then change what it is given (a UI skin's painters draw into a list).
- A `std::function` owns its callable: it is moved, never copied or cloned. A field of a callable type is called like a method: `on_click(n)` (or `this.on_click(n)`).

### Changing captured values
- `[mut count = 0u32]` (or `[mut move state]`, `[mut x]`) declares a captured value that the lambda may change. Such a lambda's call is `mut`: it is called through a `mut` variable or a `mut` parameter (`template<type F> void each(mut F f)`), and moving it keeps its state.
- A lambda with `mut` captures doesn't convert to `std::function` (whose call isn't `mut`).
- Library functions that call a callable on one thread, in order, take it as `mut F` (`retain`, `std::thread::spawn`). Comparators (`sort_by`) and parallel callbacks (`for_index_chunks`) stay `F`: a callable that changes itself can't be called from several threads at once.
```edax
mut auto counter = [mut count = 0u32]() { count += 1; return count; };
counter();                                   // 1
auto frozen = [mut n = 0u32]() { n += 1; };
frozen();                                    // ERROR: calling 'frozen' changes its 'mut' captures
```

### Callables with bounds
- `std::function<R(A...), I...>` stores a callable that implements the interfaces `I...` and implements them itself: `std::function<void(), std::sendable>` can be moved to another thread. Bounds other than `std::sendable` and `std::shareable` are **planned**, and so is `std::any_of<I...>`, a value of any type implementing `I...`.
- `std::function` takes an allocator like the containers (**planned**; today it uses the global allocator).
- `std::inline_function<R(A...), N>` stores captures of up to `N` bytes inline and never allocates. A lambda with bigger captures is a compile error that names its size. `N` is rounded up to whole 8-byte words; the value takes `N + 16` bytes (the code and the drop function). Bounds come after the size: `std::inline_function<void(), 32, std::sendable>`.
- The bounds are `std::sendable` and `std::shareable`. A lambda converts to a bounded function only when its captures meet the bound; the function then meets it too (it can go to another thread).

## Rationale
- **`[mut &x]`:** it uses the same `mut` marker as references and call sites, so a lambda that changes a variable says so in its capture list.
- **Reference-capturing lambdas are views:** this closes the classic C++ dangling-capture bug (returning `[&]` lambdas) without any lifetime syntax.
- **`mut` captures:** a handler with its own counter or cache is common; marking the call `mut` keeps "nothing changes behind an immutable reference" true.
- **Bounds on `std::function`:** an event bus or job queue that moves callables between threads needs them to be sendable, and the type must say so.
- **`inline_function`:** per-frame callbacks shouldn't allocate. The size limit is in the type, so the cost is visible.
