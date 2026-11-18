# 20 - Standard Library Overview

## Rules

### Naming
- Always `std::`-qualified. There is no `using`.
- `snake_case`, with **full words over abbreviations**: `std::error`, not `std::err`. Established short forms are kept where they are clearer than the long name: `darray`, `str`, `ptr`, `io`.
- Names say what something *is*. `std::list` was removed because in C++ it means a linked list.
- **The boundary test:** if a feature can be written as an ordinary Rift function or type, it belongs in `std::`, not in the language.

### Catalogue (v0.3)
| Area | Names |
|---|---|
| Containers | `std::array<T, N>` (fixed size, inline), `std::darray<T>` (growable, heap), `std::span<T>` / `std::span<mut T>` (view of contiguous elements), `std::hash_map<K, V>`, `std::tree_map<K, V>`, `std::bitset<N>` |
| Strings | `std::string`, `std::str`, `std::static_str`, `std::format`, `std::format_args` |
| Results | `std::result<T, E>`, `std::option<T>`, `std::ok`, `std::error`, `std::some`, `std::none` |
| Pointers | `std::unique_ptr<T>`, `std::shared_ptr<T>`, `std::weak_ptr<T>`, `std::raw_ptr<T>` |
| Callables | `std::function<R(Args...)>`, `std::fn_ptr<R(Args...)>` |
| Ownership | `std::take`, `std::replace`, `std::swap` |
| Arithmetic | `std::checked_add/sub/mul`, `std::saturating_add/sub/mul`, `std::ordering` |
| Concurrency | `std::atomic<T>`, `std::mutex<T>` (design pending, see 22) |
| I/O | `std::println`, `std::eprintln`, `std::file`, `std::io_error` |
| Filesystem | `std::filesystem::path`, `std::filesystem::executable_dir` |
| Program | `std::exit_code` (`success`, `failure`), `std::env::raw_args`, `std::panic` |
| Interfaces | `std::comparable`, `std::cloneable`, `std::formattable` |
| Compile time | `std::is_integer`, `std::is_float`, `std::is_same`, `std::implements`, `std::size_of`, `std::align_of`, `std::type_name` |
| Reflection (see 26) | `std::meta::fields`, `std::meta::alternatives`, `std::meta::field`, `std::meta::alternative`, `std::meta::is_struct/is_class/is_enum` |
| Build (see 24, 25) | `std::build::graph`, `std::build::mode::settings` (interface), the mode classes `std::build::mode::debug` / `::release` / `::release_with_debug_info`, `std::build::current_mode`, `std::build::mode_is`, `std::build::setting` |
| Diagnostics (see 23) | `std::diagnostic` (a declared warning id), `std::compile_error`, `std::compile_warning`, `std::compile_note`, `std::source_location`, `std::location_of`, `std::is_compile_time`. There is no `std::assert`: use `run_time check` |
| Sorting | `std::sort_order::ascending`, `std::sort_order::descending` |

### Written in Rift
The library is Rift code compiled into `riftc` (library/std): `std::darray`, `std::array`,
`std::bitset`, `std::hash_map`, `std::tree_map`, `std::string`, the
methods of `std::str`, `std::file` and `std::filesystem::path` are ordinary classes and
functions built on `std::raw` (see 13) and a few `extern(std::build::language::c)` functions. The compiler provides
only what cannot be written in Rift: the `std::raw` intrinsics, `std::option`/`std::result`
methods and `?` (see 11), printing and `std::format`, `std::checked_*`/`std::saturating_*`,
and the compile-time functions (`std::size_of`, `std::meta::...`).

- `text.split(',')` on a `std::str` calls the library's `str_split(text, ',')`: the methods of
  `std::str` (`trim`, `split`, `parse<T>`, `starts_with`, `ends_with`, `find`, `contains`,
  `slice`) are functions of the library with the text as their first parameter.
- `std::filesystem` and `std::env` are modules of their own (`import std;` includes them).
- `std::span<T>` is a view class: `list.as_span()` on a `std::darray`, `std::span<T>::from_raw(p, n)` (unsafe).
- `std::io_error` is a simple enum (`not_found`, `permission_denied`, `already_exists`,
  `is_directory`, `invalid_data`, `other`). Simple enums print as the name of their
  alternative: `f"{e}"` gives `not_found`.
- **Platform:** file I/O and `std::filesystem::executable_dir()` use the C library; the current
  library targets Linux (glibc).

### Container access
- `a[i]` panics when out of bounds. `a.get(i)` returns `std::option<T&>`.
- `std::darray<T>` methods include `push(T value) mut`, `pop() mut → std::option<T>`, `insert(i, value) mut`, `remove(i) mut → T`, `len() → usize`, `retain(pred) mut`, `sort_by_key(order, key) mut`.
- `std::array<T, N>` keeps its `N` elements inline (no heap); it is a copy type when `T` is. It
  has `len()`, `a[i]`, `get(i)` and `as_span()`, and `for` walks it. `default` gives every element
  its default.
- `std::bitset<N>` is `N` bits stored inline: `test(i)`, `set(i, bool) mut`, `count()`, `len()`.
- `std::option<T&>` (from `get`, or `std::some(place)` where a `std::option<T&>` is expected)
  refers to a place; it is a view (see 12). `value_or(place)` on it returns a reference too.

### Maps
- `std::hash_map<K, V>`: keys in no particular order, found by `std::hash(key)` (open addressing).
  Keys are integers, `char`, `bool`, `std::string`, `std::str`, or a type with a `u64 hash()`
  method; keys are compared with `==`.
- `std::tree_map<K, V>`: keys in ascending order, compared with `<` (kept sorted, binary search).
- Both have `insert(key, value) mut → std::option<V>` (the replaced value), `contains(key&)`,
  `get(key&) → std::option<V&>`, `remove(key&) mut → std::option<V>`, `m[key]` (panics when the key
  is missing), `len()`, `is_empty()`, `clear() mut`, and `entries()`: a
  `std::darray<std::map_entry<K, V>>` whose elements are views with `.key` and `.value`.
```rift
mut std::hash_map<std::string, u32> ages = default;
ages.insert(std::string::from("ada"), 36);
for (std::map_entry<std::string, u32> e : ages.entries()) { std::println(f"{e.key}: {e.value}"); }
```
- An empty container is created with `default` or `T{}`: `std::darray<employee> list = default;`.

### Removed from v0.2
| Removed | Replacement |
|---|---|
| `std::err` | `std::error` |
| `std::list`, `std::d_array` | `std::darray` |
| `std::variant` | Enums with payloads (see 08) |
| `std::box`, `std::dyn` | `std::unique_ptr`, interface pointers (see 07) |
| `std::range` | `a..b` / `a..=b` (see 10) |
| `std::convert` | `.or_error()` / `.map_error()` (see 11) |
| `std::filesystem::get_executable_path` | `std::filesystem::executable_dir` |
