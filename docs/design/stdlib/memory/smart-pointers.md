# Smart Pointers

`std::unique_ptr`, `std::shared_ptr` and `std::weak_ptr` own heap objects. They are never null and are written in Rift on top of `std::raw_ptr` and the `std::raw` intrinsics.

## Rules

### The pointer types
| Type | Ownership | Can be null? | Copy? | Deref in safe code? |
|---|---|---|---|---|
| `std::unique_ptr<T>` | Sole owner (heap) | **No** | No (move only) | Yes |
| `std::shared_ptr<T>` | Shared owner, reference counted | **No** | No (`.clone()` increments the count) | Yes (read-only, see below) |
| `std::weak_ptr<T>` | Non-owning observer of a `shared_ptr` | - | No (`.clone()`) | Via `.lock()` → `std::option<std::shared_ptr<T>>` |
| `std::raw_ptr<T>` (language, see [Raw Pointers and `unsafe`](../../language/memory/raw-pointers-and-unsafe.md)) | None | **Yes** (`nullptr`) | Yes | **No**, only in `unsafe` |

- **`nullptr` exists only for `std::raw_ptr`.** Smart pointers are never null; a "maybe-pointer" is written `std::option<std::unique_ptr<T>>`.
- Creation always says what it costs:
  ```rift
  std::unique_ptr<circle> c = std::unique_ptr<circle>::make(circle{ .radius = 1.0 });  // heap allocation
  std::shared_ptr<config> s = std::shared_ptr<config>::make(load_config());
  std::shared_ptr<config> s2 = s.clone();                                               // count + 1
  ```
- Access: `*p` and `p->member` (the smart pointers define `operator*`, see [Operators](../../language/basics/operators.md)). A `mut std::unique_ptr<T>` allows changes to the pointee. A `std::shared_ptr<T>` gives **read-only** access to its pointee; shared mutation needs a synchronized type such as `std::shared_ptr<std::mutex<T>>` (see [Threads](../concurrency/threads.md)). Its reference count is always atomic, so a `shared_ptr` can be cloned from any thread; there is no second, single-threaded variant.
- Smart pointers to interfaces (`std::unique_ptr<printable>`) are fat pointers (see [Inheritance and Interfaces](../../language/user-types/inheritance-and-interfaces.md)).
- A `std::unique_ptr<circle>` converts implicitly to a `std::unique_ptr<shape>` when `circle` derives from `shape` (likewise `shared_ptr` and `weak_ptr`), like `circle&` → `shape&`. Destroying it runs `circle`'s destructor: the destructor of an `open` class is virtual (see [Inheritance and Interfaces](../../language/user-types/inheritance-and-interfaces.md)).
- `std::shared_ptr<T>::weak()` makes a `std::weak_ptr<T>`; `shared.owners()` is the number of owners.
- The smart pointers are written in Rift, on `std::raw` (see [Raw Memory: `std::raw`](raw-memory.md)). `std::unique_ptr<T, A>` and `std::shared_ptr<T, A>` take an allocator like the containers (see [Allocators](allocators.md)).

## Rationale
- **Non-null smart pointers:** a null check is either forgotten or redundant. `std::option` makes "maybe absent" part of the type and forces handling. `nullptr` remains only for raw pointers, where C APIs need it.
- **Pointers in `std::`:** they can be written in Rift (with `unsafe` inside), so they're library types, not keywords.
- **Read-only `shared_ptr`:** shared and mutable at once is where data races come from. Requiring a `mutex` (or similar) wrapper makes that cost visible.
- **One `std::shared_ptr`, always with an atomic count:** two pointer types (single-threaded and atomic) would be a second way to do the same thing. The atomic increment is cheap next to the allocation that created the pointer.
