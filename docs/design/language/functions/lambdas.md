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

```rift
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
- A `std::function` owns its callable: it is moved, never copied or cloned. A field of a callable type is called like a method: `this.on_click(n)`.

## Rationale
- **`[mut &x]`:** it uses the same `mut` marker as references and call sites, so a lambda that changes a variable says so in its capture list.
- **Reference-capturing lambdas are views:** this closes the classic C++ dangling-capture bug (returning `[&]` lambdas) without any lifetime syntax.
