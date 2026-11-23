# Maps: `std::hash_map`, `std::tree_map`

## Rules

- `std::hash_map<K, V>`: keys in no particular order, found by `std::hash(key)`.
  Keys implement `std::hashable` (integers, `char`, `bool`, `std::string`, `std::str`, and any type with a `u64 hash()` method, see [Core Utilities](../core/utilities.md)) and are compared with `==`.
- `std::tree_map<K, V>`: keys in ascending order, compared with `<`. Both maps take an allocator as their last parameter (`std::hash_map<K, V, A>`, `std::tree_map<K, V, A>`, see [Allocators](../memory/allocators.md)).
- Both have `insert(key, value) mut → std::option<V>` (the replaced value), `contains(key&)`,
  `get(key&) → std::option<V&>`, `get_mut(key&) mut → std::option<mut V&>`,
  `get_or_insert(key, value) mut → mut V&` (`m.get_or_insert(k, 0) += 1` counts),
  `remove(key&) mut → std::option<V>`, `m[key]` (panics when the key is missing), `len()`,
  `is_empty()`, `clear() mut`, and `entries()`: a stream (see [Control Flow](../../language/basics/control-flow.md), `for`) of
  `std::map_entry<K, V>` views with `.key` and `.value`. It allocates nothing and borrows the map.
- `hash_map` also has `with_capacity(n)` (static) and `reserve(n) mut`; `std::string` has `with_capacity(n)`, `reserve(n)` and `capacity()`; `tree_map` has `first()` / `last() → std::option<std::map_entry<K, V>>`.
```edax
mut std::hash_map<std::string, u32> ages = default;
ages.insert(std::string::from("ada"), 36);
for (std::map_entry<std::string, u32> e : ages.entries()) { std::println(f"{e.key}: {e.value}"); }
```
- `m.get_or_insert(k, 0) += 1` works because a method returning `mut V&` is a place.

### Layout
| Type | Layout |
|---|---|
| `std::hash_map<K, V>` | SwissTable: one control byte per slot (empty, deleted, or 7 bits of the hash), probed 8 at a time with word-wide (SWAR) compares; slots hold key and value together; maximum load 7/8. Removal leaves a tombstone only when a group is full |
| `std::tree_map<K, V>` | B-tree of minimum degree 8 (up to 15 keys per node, leaves without child pointers). `entries()` walks it with a fixed stack, no allocation |

## Rationale
- **Two maps, two promises:** `hash_map` is the fast one with no order; `tree_map` keeps keys sorted. The name says which promise you get.
- **Hash-flooding resistance:** `std::hash` is seeded per process, so inputs that collide can't be prepared in advance. Iteration order of a `hash_map` therefore differs between runs.
