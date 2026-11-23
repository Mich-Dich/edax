# Sequences: `std::darray`, `std::array`, `std::span`, `std::bitset`

## Rules

### Access
- `a[i]` panics when out of bounds. `a.get(i)` returns `std::option<T&>`.
- `std::darray<T>` methods include `push(T value) mut`, `pop() mut → std::option<T>`, `insert(i, value) mut`, `remove(i) mut → T`, `len() → usize`, `retain(pred) mut`, `with_capacity(n)` (static), `reserve(n) mut`, `capacity()`, `truncate(n) mut`, `shrink_to_fit() mut`, `resize(n, value) mut`, `swap_remove(i) mut → T` (O(1), the order changes), `contains(value&)`, `as_mut_span() mut`, and the sorts:
  - `sort() mut`: ascending with `<`; introsort (median-of-3 quicksort, insertion sort below 24
    elements, heapsort past a depth budget), so never worse than O(n log n). Not stable.
  - `sort_by(less) mut`: the same with a `bool(T&, T&)` callable.
  - `sort_by_key(order, key) mut`: **stable** merge sort (runs of 16, one buffer of `n` elements).
  - Integers (`sort()`) and integer keys (`sort_by_key`) of at least 256 elements use an LSD radix
    sort instead: one pass per byte that differs, stable, O(n) per byte, one buffer of `n` elements.
- `std::array<T, N>` keeps its `N` elements inline (no heap); it is a copy type when `T` is. It
  has `len()`, `a[i]`, `get(i)` and `as_span()`, and `for` walks it. `default` gives every element
  its default.
- `std::bitset<N>` is `N` bits stored inline: `test(i)`, `set(i, bool) mut`, `count()`, `len()`.
- `std::option<T&>` (from `get`, or `std::some(place)` where a `std::option<T&>` is expected)
  refers to a place; it is a view (see [Ownership, Copies, Moves and References](../../language/memory/ownership-and-references.md)). `value_or(place)` on it returns a reference too.
- `s.load(block)` and `s.store(block, v)` read and write SIMD vectors (see [SIMD](../../language/simd/simd.md)).
- `std::span<T>` is a view of contiguous elements: `list.as_span()`, `std::span<T>::from_raw(p, n)` (unsafe). `std::span<mut T>` (`list.as_mut_span()`) gives mutable access to the elements, also through an immutable variable. `len()`, `s[i]`, `get(i)`, `slice(from, to)`, `chunks(n)` (a stream of non-overlapping parts, e.g. to hand parts to threads, see [Threads](../concurrency/threads.md)), `sort()` and the other sorts on `std::span<mut T>` (no `mut` needed: the span gives mutable access to its elements).
- **Splitting:** `s.split_at(i)` consumes a span and returns both halves as a tuple, `(std::span<T>, std::span<T>)` (also for `std::span<mut T>`); the two halves never overlap, so both can be changed at once (see [Tuples and List Literals](../../language/basics/tuples-and-lists.md)):
  ```edax
  (std::span<mut i64> left, std::span<mut i64> right) = items.split_at(middle);
  ```
- **From a list:** `std::darray<T>::from([a, b, c])` copies a list literal into a new darray (it allocates); `std::array<T, N> a = [a, b, c];` needs no allocation.
- **Allocators:** `std::darray<T, A>` takes its memory from the allocator `A`, by default the global one (see [Allocators](../memory/allocators.md)).
- An empty container is created with `default` or `T{}`: `std::darray<employee> list = default;`.
- A method declared `mut T& f() mut` returns a mutable reference, and its call is a place: `list.at(i) = x`.

### Layout
| Type | Layout |
|---|---|
| `std::darray<T>` | Pointer, length, capacity; grows by doubling. For number types the elements are aligned to the cache line, so SIMD loads never straddle one |
| `std::array<T, N>` | `N` values inline |
| `std::span<T>` | Pointer and length |
| `std::bitset<N>` | `(N + 63) / 64` words inline |

## Rationale
- **`a[i]` panics, `a.get(i)` asks:** the short form is for indices the code knows are valid; the `std::option` form is for indices that come from outside.
- **Sorts with their algorithm in the spec:** `sort()` is never worse than O(n log n) and `sort_by_key` is stable, so callers can rely on both.
