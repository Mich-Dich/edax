# Std lesson 2: Sequences in depth

Language lesson 9 introduced `std::darray` and friends. This lesson goes through what you reach for once the basics are in place: controlling allocations, removing elements cheaply, working on parts of a sequence through spans (including two mutable parts at once), choosing a sort by the promise it makes, and fixed-size storage with `std::array` and `std::bitset`.

**You will learn:** `with_capacity` / `reserve` / `capacity`, `insert` / `remove` / `swap_remove` / `truncate` / `resize`, `retain`, spans with `slice`, `chunks` and `split_at`, `std::span<mut T>`, the three sorts, `std::array` and `std::bitset`.

**You will build:** a small pipeline over sensor readings (`exercise.ed`).

Examples are in [`examples/`](examples); `python3 course/check.py s2 --examples` checks them.

---

## 1. `std::darray`: growing and shrinking

| Call | Cost | Note |
|---|---|---|
| `std::darray<T>::with_capacity(n)` | one allocation | room for `n` elements, length 0 |
| `xs.reserve(n) mut`, `xs.capacity()` | at most one allocation | room for `n` more |
| `xs.push(v) mut` | amortized O(1) | grows by doubling |
| `xs.pop() mut` | O(1) | an `std::option<T>` |
| `xs.insert(i, v) mut` | O(n) | shifts the tail |
| `xs.remove(i) mut` | O(n) | keeps the order; returns the element |
| `xs.swap_remove(i) mut` | **O(1)** | the last element takes its place; returns the element |
| `xs.retain(keep) mut` | O(n) | removes every element `keep` refuses, in one pass |
| `xs.truncate(n) mut`, `xs.resize(n, v) mut` | O(removed) / O(added) | |
| `xs.contains(v)`, `xs.get(i)`, `xs[i]` | | `[i]` panics, `get` asks |
| `std::darray<T>::from([a, b, c])` | one allocation | a darray from a list literal |

Two habits from C++ carry over, and two don't:
- Reserve when you know the size: `with_capacity(n)` saves the regrowth copies.
- Order doesn't matter? `swap_remove` instead of `remove`.
- There's no erase-remove idiom: `retain` is the whole thing.
- Moving elements is always a byte copy (no move constructors), so growth is a `memcpy` whatever `T` is.

Example: [`darray.ed`](examples/darray.ed).

## 2. Spans: working on parts

A `std::span<T>` is a pointer and a length: a view (language lesson 2) of contiguous elements, from a darray, an array, or another span.

| Call | Gives |
|---|---|
| `xs.as_span()` | `std::span<T>`: read-only elements |
| `xs.as_mut_span() mut` | `std::span<mut T>`: the elements can change, even through an immutable span variable |
| `s.slice(from, to)` | the elements `from..to`. On a `std::span<mut T>` it consumes the span: a mutable span is never copied, because two copies would be two writers |
| `s.chunks(n)` | a stream of consecutive, non-overlapping parts of `n` (the last may be shorter) |
| `s.split_at(i)` | **both** halves as a tuple: `(std::span<T>, std::span<T>)` |
| `s.sort()`, `sort_by`, `sort_by_key` | on `std::span<mut T>`: sorts that part in place |

`std::span<mut T>` is the type for "a function that changes elements but not the length". It needs no `mut` on the parameter: the mutability is in the element type.

```edax
void double_all(std::span<mut u32> xs) {
    for (mut u32& x : xs) { x *= 2; }
}
```

### Two mutable parts at once

The borrow rules say one place, one writer. Two `as_mut_span()` of one darray in one call are refused, and so is a mutable and a shared view of it:

```
error[E5020]: 'xs' is passed as a mutable reference and also as another argument
```

`split_at` is the way out: it consumes the span and hands back two halves that are guaranteed not to overlap, so both can change at once:

```edax
(std::span<mut u32> left, std::span<mut u32> right) = xs.as_mut_span().split_at(3);
double_all(left);
right[0] = 100;
```

`chunks` does the same for many parts, which is how work is split across threads (std lesson 13).

Examples: [`spans.ed`](examples/spans.ed), [`err_spans.ed`](examples/err_spans.ed).

## 3. Three sorts, three promises

| Call | Order | Stable? | Algorithm |
|---|---|---|---|
| `sort()` | `<` | no | introsort: never worse than O(n log n); radix sort for 256+ integers |
| `sort_by(less)` | `less(a, b)` returns a `bool` | no | the same |
| `sort_by_key(order, key)` | by `key(x)`, `std::sort_order::ascending` or `descending` | **yes** | merge sort; radix sort for 256+ integer keys |

**Stable** means elements with equal keys keep their relative order. That's what makes "sort by priority, and within a priority keep the arrival order" a single call. In C++ you'd have to remember `std::stable_sort`; here the function that takes a key is the stable one.

The algorithms are part of the specification, so you can rely on their complexity.

Example: [`sorting.ed`](examples/sorting.ed).

## 4. Fixed-size storage

- **`std::array<T, N>`** keeps its `N` elements inline, no heap. It is a copy type when `T` is. `default` gives every element its default. Pass it on as `a.as_span()`.
- **`std::bitset<N>`** is `N` bits inline (`(N + 63) / 64` words): `set(i, bool) mut`, `test(i)`, `count()`, `len()`. Use it for "which of these ids have I seen" when the ids are small numbers.

Example: [`bitset_array.ed`](examples/bitset_array.ed).

---

## C++ → Edax at a glance

| C++ | Edax |
|---|---|
| `std::vector<T> v; v.reserve(n);` | `std::darray<T>::with_capacity(n)` |
| `v.erase(v.begin() + i)` | `v.remove(i)` |
| `std::swap(v[i], v.back()); v.pop_back();` | `v.swap_remove(i)` |
| `v.erase(std::remove_if(...), v.end())` | `v.retain(keep)` |
| `v.resize(n, x)` | `v.resize(n, x)` |
| `std::span<const T>` / `std::span<T>` | `std::span<T>` / `std::span<mut T>` |
| `s.subspan(a, n)` | `s.slice(a, a + n)` |
| two subspans, trusting they don't overlap | `s.split_at(i)` |
| `std::sort` / `std::stable_sort` | `sort()` / `sort_by_key(...)` |
| `std::bitset<N>` | `std::bitset<N>` |

---

## Your goal

`exercise.ed` reads ten sensor readings, drops the outliers, and analyses the rest. Fill in the TODOs so that it prints:

```
8 readings kept
sensors seen: 3
values: 12 15 14 11 13 16 18 17
means of 3: 13 13 17
adjusted: 1 4 3 0 113 116 118 117
by sensor: 0:12 0:14 0:13 0:17 1:15 1:11 1:16 2:18
swap-removed 0:12, first is now 2:18
```

<details>
<summary>Hint 1: the outliers and the sensors</summary>

`log.retain([](reading& r) { return r.value >= -100 && r.value <= 100; });`. For the sensors: `mut std::bitset<8> sensors = default;` and `sensors.set(cast<usize>(r.sensor), true);` in the loop, then `sensors.count()`.
</details>

<details>
<summary>Hint 2: <code>chunk_means</code></summary>

`for (std::span<i32> part : values.chunks(size))`, sum the part, and divide by `cast::truncate<i32>(part.len())`. The capacity is `(values.len() + size - 1) / size`.
</details>

<details>
<summary>Hint 3: <code>adjust_halves</code></summary>

`(std::span<mut i32> first, std::span<mut i32> second) = values.split_at(values.len() / 2);`, then find the minimum of `first`, subtract it in a `for (mut i32& v : first)` loop, and add 100 in the other half.
</details>

<details>
<summary>Hint 4: by sensor, stably</summary>

`log.sort_by_key(std::sort_order::ascending, [](reading& r) { return r.sensor; });`. `sort_by` would put the sensor's readings in any order.
</details>

---

## Try this afterwards

1. In `adjust_halves`, replace `split_at` by `values.slice(0, n / 2)` and `values.slice(n / 2, n)` passed to one function that changes both.
2. Replace `sort_by_key` by `sort_by([](reading& a, reading& b) { return a.sensor < b.sensor; })`. Is the output still the same?
3. Call `log.remove(log.len())`.
4. Print `std::darray<u32>::with_capacity(5).capacity()`, and the capacity of a `default` darray after five `push`es.

<details>
<summary>What happens</summary>

1. `error[E5002]: 'values' is used after it was moved`. A `std::span<mut T>` isn't a copy type (two copies would be two writers), so `slice` **consumes** it, and the second `slice` has nothing left to cut. `split_at` consumes it once and gives both halves.
2. Maybe, maybe not: `sort_by` is not stable, so the readings of one sensor may come out in another order. On small inputs it often happens to match, which is why relying on it is a bug.
3. A panic: `remove position 8 out of bounds (length 8)`.
4. `5` and `8`: `with_capacity` allocates exactly what you ask for; growing by `push` doubles.
</details>

## Recap

- Reserve when you know the size; `swap_remove` when order doesn't matter; `retain` to filter in place.
- `std::span<mut T>` changes elements, not the length. `slice`, `chunks` and `split_at` make parts; only `split_at` and `chunks` give several mutable parts at once.
- `sort` is fast, `sort_by` takes a `bool` comparison, `sort_by_key` is stable.
- `std::array` and `std::bitset` are inline.

Next: [std lesson 3](../03-maps/README.md), maps and your own keys.
