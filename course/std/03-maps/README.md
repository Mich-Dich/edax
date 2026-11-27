# Std lesson 3: Maps and your own keys

Two maps, two promises: `std::hash_map` is the fast one with no order, `std::tree_map` keeps its keys sorted. Both have the same API, and both hand values back instead of silently overwriting or inserting. This lesson covers the whole API, how to make your own types into keys, and the borrowing you meet when you walk a map.

**You will learn:** `insert` / `remove` / `get` / `get_mut` / `get_or_insert` / `[]`, `entries()`, `first()` / `last()`, keys of your own types with `@std::derive::hashable` and `@std::derive::ordered`, maps of containers, and how to get a sorted view of a `hash_map`.

**You will build:** a warehouse index (`exercise.ed`).

Examples are in [`examples/`](examples); `python3 course/check.py s3 --examples` checks them.

---

## 1. One API for both maps

| Call | Gives | Note |
|---|---|---|
| `m.insert(k, v) mut` | `std::option<V>`: the value it replaced | so you can tell "new" from "overwritten" |
| `m.remove(k) mut` | `std::option<V>`: the value it removed | |
| `m.get(k)` | `std::option<V&>` | asks |
| `m.get_mut(k) mut` | `std::option<mut V&>` | change a value in place |
| `m[k]` | `V&` | **panics** when the key is missing; never inserts |
| `m.get_or_insert(k, v) mut` | `mut V&`: inserts `v` first if `k` is missing | a place: `+=`, `.push(...)` |
| `m.contains(k)`, `length()`, `is_empty()`, `clear() mut` | | |
| `m.entries()` | a stream of `std::map_entry<K, V>` views (`.key`, `.value`) | borrows the map, allocates nothing |
| `tree_map`: `first()`, `last()` | `std::option<std::map_entry<K, V>>` | the smallest and largest key |
| `hash_map`: `with_capacity(n)`, `reserve(n) mut` | | |

```edax
mut std::tree_map<std::str, u32> stock = default;
stock.insert("tea", 3);                       // none
u32 old = stock.insert("tea", 5).value_or(0); // 3: the replaced value
stock.get_or_insert("pot", 0) += 2;           // insert 0, then add 2
if (stock.get_mut("tea") is std::some(n)) { n += 10; }
```

**`[]` never inserts.** In C++, `counts["b"]` on a missing key quietly creates it, so a typo in a read creates an entry. In Edax `m[k]` is a lookup that panics when the key is missing (`panic in ...: key not found`). Creating an entry is always `insert` or `get_or_insert`, which you can see.

Examples: [`map_basics.ed`](examples/map_basics.ed), [`err_keys.ed`](examples/err_keys.ed).

## 2. Which map?

| | `std::hash_map<K, V>` | `std::tree_map<K, V>` |
|---|---|---|
| Order of `entries()` | none, and **different on every run** (the hash is seeded per process) | ascending keys |
| Lookup | O(1): one group of control bytes, usually one slot (SwissTable) | O(log n): a B-tree with up to 15 keys per node |
| Key needs | `u64 hash()` and `==` | `<=>` |
| Extra | `with_capacity`, `reserve` | `first()`, `last()` |

The changing order is on purpose: a program can't come to depend on an order nobody promised, and inputs that make every key collide can't be prepared in advance. When you need a `hash_map` in some order, copy what you need and sort it (section 4).

## 3. Your own types as keys

Built-in numbers, `char`, `bool` and the string types are keys already. For a struct, generate what the map needs from its fields:

```edax
@std::derive::hashable                     // u64 hash() from all fields
struct cell {
    i32 x = 0;
    i32 y = 0;
    bool operator==(cell& other) = default;
}

@std::derive::ordered                      // <=> from the fields, the first that differs decides
struct date {
    u16 year = 0;
    u8 month = 0;
    u8 day = 0;
    bool operator==(date& other) = default;
}
```

The maps check their key type when you name them, so a missing piece is one message at your declaration:

```
error[E4210]: 'location' can't be a hash_map key: give it a 'u64 hash()' method (or @std::derive::hashable)
  --> exercise.ed:34:9
   |
34 |     mut std::hash_map<location, std::str> shelves = default;
   |         ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ check failed
```

and likewise "give it an '=='" for a hash key without `==`, or "give it an 'operator<=>'" for a `tree_map` key. You can write `hash()` and `<=>` by hand too (std lesson 1); the annotations are ordinary library code, which std lesson 11 takes apart.

Example: [`own_keys.ed`](examples/own_keys.ed).

## 4. Values that are containers, and walking a map

A map owns its values, so a value can be a container that you fill through `get_or_insert`:

```edax
mut std::tree_map<std::str, std::darray<std::static_str>> by_letter = default;
by_letter.get_or_insert(w.slice(0, 1), default).push(w);
```

`entries()` borrows the map, and each `std::map_entry` is a view into it. If you collect entries into a darray (to sort a `hash_map` by value, for instance), that darray **borrows the map** for as long as it lives, and the map can't change meanwhile:

```
error[E5022]: cannot call mut method 'remove' on 'totals' while 'ranking' borrows it
```

Give the collection a block of its own, so the borrow ends where you're done with it:

```edax
{
    mut std::darray<std::map_entry<std::str, u32>> ranking = default;
    for (std::map_entry<std::str, u32> e : totals.entries()) { ranking.push(e); }
    ranking.sort_by(...);
    ...
}
totals.remove("glue");      // fine: ranking is gone
```

Example: [`values_owning.ed`](examples/values_owning.ed).

---

## C++ → Edax at a glance

| C++ | Edax |
|---|---|
| `std::unordered_map` / `std::map` | `std::hash_map` / `std::tree_map` |
| `auto [it, inserted] = m.insert_or_assign(k, v)` | `std::option<V> old = m.insert(k, v);` |
| `m[k] += 1` (inserts silently) | `m.get_or_insert(k, 0) += 1;` |
| `m.at(k)` (throws) | `m[k]` (panics) or `m.get(k)` (asks) |
| `if (auto it = m.find(k); it != m.end())` | `if (m.get(k) is std::some(v))` |
| `m.erase(k)` | `m.remove(k)` (gives the value back) |
| `m.begin()->first` | `m.first()` (tree_map) |
| `std::hash<T>` specialization + `operator==` | `@std::derive::hashable` + `operator== = default` |
| `operator<` for a `std::map` key | `@std::derive::ordered` or `operator<=>` |

---

## Your goal

`exercise.ed` indexes seven deliveries to a warehouse by item, by category and by shelf. It doesn't build until `location` can be a `hash_map` key. Then fill in the TODOs so that it prints:

```
shelf 1/1 already held bolt
bolt    250
nut     175
tape     40
paint    30
glue     12
chemicals: 2 items
hardware: 2 items
office: 1 items
at 1/2: nut
at 9/9: nothing
sold all 12 glue, 4 items left
```

<details>
<summary>Hint 1: the loop</summary>

Check `totals.contains(d.item)` **before** adding to the total: if it isn't there yet, `by_category.get_or_insert(d.category, default).push(d.item);`. Then `totals.get_or_insert(d.item, 0) += d.count;`. For the shelf: `if (shelves.insert(d.at, d.item) is std::some(before)) { ... }`.
</details>

<details>
<summary>Hint 2: the ranking</summary>

In a block: a `mut std::darray<std::map_entry<std::str, u32>>`, filled from `totals.entries()`, sorted with `sort_by` (`a.value > b.value`, and `a.key < b.key` for equal values), then printed with `{e.key:<6} {e.value:>4}`.
</details>

<details>
<summary>Hint 3: the categories</summary>

`by_category` is a `tree_map`, so `entries()` is already in name order; `e.value.length()` is the number of items.
</details>

---

## Try this afterwards

1. Change `totals` into a `tree_map`, drop the sort, and print its entries. What order do you get?
2. Read `totals["screw"]`.
3. Move the ranking out of its block, after the categories, and keep the `remove` at the end.
4. Remove `bool operator==` from `location`.

<details>
<summary>What happens</summary>

1. Alphabetical: `bolt glue nut paint tape`. A `tree_map` orders by key, not by value; sorting by count still needs the copy.
2. A panic, `key not found`: `[]` doesn't insert.
3. `error[E5022]: cannot call mut method 'remove' on 'totals' while 'ranking' borrows it`.
4. `error[E4210]: 'location' can't be a hash_map key: give it an '=='`, at the declaration of `shelves`.
</details>

## Recap

- `insert` and `remove` give values back; `get` asks; `[]` insists and never inserts; `get_or_insert` is a place.
- `hash_map` is fast and unordered (differently each run); `tree_map` is sorted.
- Keys: `hash()` + `==` for hash maps, `<=>` for tree maps; the `std::derive` annotations write them.
- Entries borrow the map: collect them in a block of their own.

Next: [std lesson 4](../04-text/README.md), text and formatting.
