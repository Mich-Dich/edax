# Lesson 9: Containers and text

## The containers

| Edax | C++ | Notes |
|---|---|---|
| `std::array<T, N>` | `std::array` | inline; `.as_span()` |
| `std::darray<T>` | `std::vector` | elements are moved with `memcpy` (every move is a byte copy) |
| `std::span<T>` | `std::span` | a view |
| `std::bitset<N>` | `std::bitset` | |
| `std::hash_map<K, V>` | `std::unordered_map` | SwissTable; seeded per process, so iteration order differs between runs |
| `std::tree_map<K, V>` | `std::map` | B-tree; keys in order |

- `a[i]` **panics** when out of bounds in every mode. `a.get(i)` returns an `std::option<T&>`.
- There are no iterator pairs. A `for` loop goes over a *container* (`len()` plus `operator[]`)
  or a *stream* (`next()` returning an `std::option`). `map.entries()` is a stream of
  `std::map_entry<K, V>` views with `.key` and `.value`.
- Maps: `insert` (returns the replaced value), `get` / `get_mut` (options), `remove`, `m[k]`
  (panics when the key is missing), and `get_or_insert(k, v)`, which returns `mut V&`. A call
  that returns `mut V&` is a place, so `m.get_or_insert(k, 0) += 1;` counts.
- The algorithms are methods: `retain`, `sort` (introsort), `sort_by(less)`,
  `sort_by_key(order, key)` (stable merge sort, or radix sort for integer keys), `contains`,
  `swap_remove`. Their complexity is part of the spec.
- Every allocating type takes an allocator as its last template parameter:
  `std::darray<u32, my_arena>`. A build target can also replace the global allocator (see
  `docs/design/stdlib/memory/allocators.md`).

## Text

| Type | Owns | Use for |
|---|---|---|
| `std::string` | yes | building and storing text (24 bytes, up to 23 inline) |
| `std::str` | no (a view) | parameters and slices |
| `std::static_str` | static memory | literals; can be stored anywhere |

- `std::str` methods: `len` (bytes), `trim`, `split(c)` (a `std::darray<std::str>` that borrows
  the text), `starts_with`, `find`, `slice(from, to)`, `parse<T>()` (an `std::option`).
- `std::string`: `std::string::from(s)`, `push_str`, `push(char)`, `std::format(f"...")`.
- A string literal is one line, and `+` doesn't join strings. Data that is really text goes in a
  file: `std::embed_text("frames.log")` puts a file's contents into the program at compile time,
  as an `std::static_str`.
- Format specs: `{x:<8}`, `{x:>3}`, `{x:^5}`, `{x:.2}`, `{x:x}` (hex), `{x:+}`. User types become
  formattable with `@std::derive::formattable` or by implementing `std::formattable`.

## Views in containers

`std::darray<std::str>` is a view type, because it borrows the text. A struct holding a
`std::str` field has to be declared `view struct`. It then follows the reference rules from
lesson 2: it can be a local and be passed down, but not stored for later. That is exactly what you
want for parsed records that point into a buffer.

## Negative patterns

`is` is not an operator: `!(x is std::some(v))` doesn't parse. To bail out on the other case, use
a `match` with a `never` arm:

```edax
u32 n = match (word.parse<u32>()) {
    std::some(v) => v,
    std::none => { skipped += 1; continue; },
};
```

## Your goal

`exercise.ed` reads `frames.log` (embedded at compile time) and has to print this report:

```
levels: info 6, warn 3, error 1
audio     14 ms
physics   12 ms
render    20 ms
ui         7 ms
slowest: audio=12 physics=9 render=7
skipped 2 lines
```

Keep the parsed samples as views into the log: `sample` must not copy any text.

## Try this afterwards

1. Change `per_system` to a `hash_map` and print its entries. Run it twice.
2. Make `sample` an ordinary `struct` with an `std::string` field. What do you need to add,
   and what does it cost?
3. Read `per_level["fatal"]`.
4. Return the `std::darray<sample>` from a function that takes the log as a parameter.
   Then try it with the log as a `compile_time` constant inside that function.
