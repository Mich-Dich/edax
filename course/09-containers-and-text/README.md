# Lesson 9: Containers and text

The standard containers will feel familiar: `std::darray` is your `std::vector`, the maps are your maps. Three things are different: there are **no iterators** (a loop goes over a container or a stream), the algorithms are **methods**, and text comes in **three types** whose difference is ownership (lesson 2). Containers of views put the borrowing rules to work: parsed records can point into the text they came from, with no copies and no dangling.

**You will learn:** the containers and how they differ from the STL, map lookups and the `get_or_insert` place, the three text types, splitting and parsing, `std::embed_text`, format specs, `view struct`, and the "negative pattern" idiom.

**You will build:** a report over a log file (`exercise.ed`).

Examples are in [`examples/`](examples); `python3 course/check.py 9 --examples` checks them.

---

## 1. The containers

| Edax | C++ | Notes |
|---|---|---|
| `std::array<T, N>` | `std::array` | inline, fixed size; `.as_span()` |
| `std::darray<T>` | `std::vector` | growable; elements move with `memcpy` (every move is a byte copy) |
| `std::span<T>`, `std::span<mut T>` | `std::span` | a view (lesson 2) |
| `std::bitset<N>` | `std::bitset` | |
| `std::hash_map<K, V>` | `std::unordered_map` | SwissTable; seeded per process, so **iteration order differs between runs** |
| `std::tree_map<K, V>` | `std::map` | B-tree; iteration in key order |

- `xs[i]` **panics** when out of bounds, in every build mode. `xs.get(i)` returns an `std::option<T&>` when you don't know.
- **No iterators, no `begin()`/`end()`.** A `for` loop goes over:
  - a *container*: anything with `length()` and `operator[]`;
  - a *stream*: anything with a `next()` returning an `std::option`. `map.entries()` is a stream of `std::map_entry<K, V>` views with `.key` and `.value`.

  With no iterator pairs, there's nothing to invalidate (lesson 2 already rejects changing a container while a loop borrows it), and no mismatched-`end()` bugs.
- C++ names don't exist: `length()` not `size()`, `push` not `push_back`, `is_empty()` not `empty()`. The compiler's note says so when you slip.
- The algorithms are methods: `sort` (introsort), `sort_by(less)`, `sort_by_key(order, key)` (stable: merge sort, or radix sort for integer keys), `retain`, `contains`, `swap_remove`. Their complexity is part of the specification.
- Every allocating type takes an allocator as its last template parameter (`std::darray<u32, my_arena>`), and a build target can replace the global allocator (see `docs/design/stdlib/memory/allocators.md`).

### Maps

| Call | Gives |
|---|---|
| `m.insert(k, v)` | the replaced value, an `std::option<V>` |
| `m.get(k)` / `m.get_mut(k)` | `std::option<V&>` / `std::option<mut V&>` |
| `m[k]` | `V&`; **panics** if the key is missing (no silent insert as in C++) |
| `m.get_or_insert(k, v)` | `mut V&`, inserting `v` first if the key is missing |
| `m.remove(k)` | the removed value, an `std::option<V>` |

A call that returns `mut V&` is a **place**, so you can assign through it directly. Counting is one line:

```edax
counts.get_or_insert(word, 0) += 1;
```

Example: [`containers.ed`](examples/containers.ed).

---

## 2. Text

| Type | Owns | Use for | C++ |
|---|---|---|---|
| `std::string` | yes | building and storing text (24 bytes, up to 23 inline) | `std::string` |
| `std::str` | no, a view | parameters and slices | `std::string_view` |
| `std::static_str` | static memory | literals; can be stored anywhere | `const char*` to a literal |

`std::string` and `std::static_str` convert to `std::str` for free, so a function that only reads text takes `std::str`. A literal *to* `std::string` allocates, so it isn't silent: write `std::string::from("...")`.

- `std::str` methods: `length()` (bytes), `trim()`, `split(c)` (an `std::darray<std::str>` that borrows the text), `starts_with`, `find` (an `std::option<usize>`), `slice(from, to)`, `parse<T>()` (an `std::option<T>`), `is_empty()`.
- `std::string`: `std::string::from(s)`, `push_str(s)`, `push(c)`, and `std::format(f"...")` to build one from an f-string.
- A string literal is one line, and `+` doesn't join strings: use `std::format` or `push_str`. Raw strings `r"C:\path"` skip escape processing.
- **Text that is really data goes in a file:** `std::embed_text("frames.log")` reads a file while compiling and gives an `std::static_str`, like C23's `#embed`.

### Format specs

| Spec | Gives | Example with `n = 5` |
|---|---|---|
| `{x:<6}` | left-aligned in 6 columns | `5     ` |
| `{x:>3}` | right-aligned in 3 | `  5` |
| `{x:^5}` | centred in 5 | `  5  ` |
| `{x:.2}` | 2 decimals | `5.00` (floats) |
| `{x:x}` | hexadecimal | `5` |
| `{x:+}` | always a sign | `+5` |

Your own types become formattable with `@std::derive::formattable` (lesson 8) or by implementing `std::formattable`.

Example: [`text.ed`](examples/text.ed).

---

## 3. Views in containers

`text.split(' ')` gives a `std::darray<std::str>`: a darray of views into `text`. That darray is itself a view type (it borrows), so the lesson 2 rules apply: it can be a local, it can be returned from a function whose one view parameter it borrows from, but it can't be stored in a field.

Your own struct that holds a `std::str` must be declared **`view struct`**:

```edax
view struct field {
    std::str name;              // points into the text it was parsed from
    std::str value;
}

std::darray<field> parse(std::str text) { ... }   // OK: borrows from `text`, the one view parameter
```

That's exactly what you want for parsed records: no allocation per field, and the compiler guarantees the records don't outlive the buffer. Trying to keep them longer is an error:

```
error[E5011]: field 'registry::fields' has view type 'std::darray<field>', but 'registry' is not a view type
```

When records must outlive the text, use `std::string` fields (and pay for the copies, visibly).

Examples: [`views.ed`](examples/views.ed), [`err_views.ed`](examples/err_views.ed).

---

## 4. Negative patterns

`is` isn't an operator, so `!(x is std::some(v))` doesn't parse. To bail out on the other case, use a `match` whose other arm leaves the loop or function. `continue`, `break` and `return` have the type `never`, which fits wherever a value is expected:

```edax
u32 n = match (word.parse<u32>()) {
    std::some(v) => v,
    std::none => {
        skipped += 1;
        continue;
    },
};
```

It reads as "take the number, or skip this line", and `n` is a plain `u32` afterwards.

Example: [`err_negative_is.ed`](examples/err_negative_is.ed).

---

## C++ → Edax at a glance

| C++ | Edax |
|---|---|
| `std::vector<T>` | `std::darray<T>` |
| `v.size()`, `v.push_back(x)`, `v.empty()` | `v.length()`, `v.push(x)`, `v.is_empty()` |
| `v.at(i)` (throws) | `v.get(i)` (an `std::option<T&>`) |
| `v[i]` (unchecked) | `v[i]` (checked, panics) |
| `std::map` / `std::unordered_map` | `std::tree_map` / `std::hash_map` |
| `m[k]++` (inserts silently) | `m.get_or_insert(k, 0) += 1;` |
| `for (auto& [k, v] : m)` | `for (std::map_entry<K, V> e : m.entries())` |
| `std::string_view` | `std::str` |
| `"a" + std::string(b)` | `std::format(f"a{b}")` |
| `std::stoi(s)` (throws) | `s.parse<i32>()` (an `std::option`) |
| `#embed` / a generated header | `std::embed_text("file")` |
| `std::format("{:>3}", x)` | `f"{x:>3}"` |

---

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

<details>
<summary>Hint 1: reading the lines</summary>

`for (std::str line : log.split('\n'))`, then `std::str trimmed = line.trim();` and `continue` on an empty one. `trimmed.split(' ')` gives the words; a line with `length() != 3` is skipped (count it).
</details>

<details>
<summary>Hint 2: a third word that isn't a number</summary>

The negative-pattern `match` from section 4: `u32 n = match (parts[2].parse<u32>()) { ... };` with a `std::none` arm that counts and `continue`s.
</details>

<details>
<summary>Hint 3: the levels line</summary>

A `hash_map` has no order, so walk a fixed `std::array<std::static_str, 3>` of `"info", "warn", "error"`, look each up with `per_level.get(level).value_or(0)`, and build the line in a `mut std::string` with `push_str`.
</details>

<details>
<summary>Hint 4: the slowest samples</summary>

`samples.sort_by_key(std::sort_order::descending, [](sample& s) { return s.ms; });`, then print the first three with `std::print` (no newline) and finish the line with `std::println("")`.
</details>

---

## Try this afterwards

1. Change `per_system` to a `hash_map` and print its entries. Run it twice.
2. Make `sample` an ordinary `struct` with an `std::string` field. What do you need to change, and what does it cost?
3. Read `per_level["fatal"]`.
4. Move the parsing of the samples into `std::darray<sample> parse(std::str text)`, called with `log`. Then give it a second parameter, `mut u32& skipped`, to count the bad lines. What breaks, and how can the function hand back both results?

<details>
<summary>What happens</summary>

1. The systems come out in some order, and a different one on the next run: the hash is seeded per process, so code can't come to depend on the order.
2. Remove `view`, and build the field with `std::string::from(parts[1])`: one allocation per sample (or none, for names up to 23 bytes, which fit inline).
3. A panic: the key is missing, and `[]` doesn't insert.
4. The first version compiles: the result borrows from `text`, its only reference parameter. With `mut u32& skipped` there are two, and the compiler can't tell which one the samples borrow from: `error[E5012]`, "a returned reference must borrow from 'this' or from the only reference parameter". Return both instead: `(std::darray<sample>, u32) parse(std::str text)`, unpacked with `(std::darray<sample> samples, u32 skipped) = parse(log);`.
</details>

## Recap

- `darray`, `array`, `span`, `hash_map`, `tree_map`; no iterators; algorithms are methods.
- `[]` panics, `get` asks; `get_or_insert` returns a place you can `+=`.
- `std::string` owns, `std::str` borrows, `std::static_str` is static. `embed_text` brings files in.
- A struct holding views is a `view struct`, and the borrow rules keep it honest.

Next: [lesson 10](../10-concurrency/README.md), threads without data races.
