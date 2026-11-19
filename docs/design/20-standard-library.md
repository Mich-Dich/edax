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
| Threads (see 27) | `std::thread::spawn`, `std::thread::handle<R>`, `std::thread::scoped_handle<R>`, `std::thread::options`, `std::thread::available_parallelism`, `std::thread::current_id`, `std::thread::sleep`, `std::thread::yield_now`, `std::thread::spin_hint` |
| Synchronization (see 27) | `std::sendable`, `std::shareable` (derived interfaces), `std::thread_bound`, `std::atomic<T>`, `std::memory_order`, `std::atomic_fence`, `std::mutex<T>`, `std::mutex_guard<T>`, `std::condition`, `std::channel<T>`, `std::channel_sender<T>`, `std::channel_receiver<T>`, `std::send_error<T>`, `std::receive_error`, `std::cache_line_size`, `std::cache_aligned<T>` |
| Thread pool (see 28) | `std::thread_pool`, `std::thread_pool::options`, `std::thread_pool::task<R>`, `std::thread_pool::scoped_task<R>`; methods `spawn`, `for_chunks`, `for_index_chunks`, `reduce_chunks`, `thread_count` |
| Time | `std::time::duration` (`milliseconds(n)`, `seconds(n)`, ...), `std::time::instant` (`now()`, `elapsed()`) |
| I/O | `std::println`, `std::eprintln`, `std::file` (`open`, `create`, `append`, `write`, `flush`), `std::io_error` |
| Filesystem | `std::filesystem::path`, `std::filesystem::executable_dir` |
| Program | `std::exit_code` (`success`, `failure`), `std::env::raw_args`, `std::panic` |
| Interfaces | `std::comparable`, `std::cloneable`, `std::formattable`, `std::hashable` (`u64 hash()`; `std::hash_seed()`, `std::hash_combine(a, b)` help implement it) |
| Compile time | `std::is_integer`, `std::is_float`, `std::is_same`, `std::implements<T, I>` (also for `std::sendable` / `std::shareable` / `std::comparable`), `std::size_of`, `std::align_of`, `std::type_name` |
| Reflection (see 26) | `std::meta::fields`, `std::meta::alternatives`, `std::meta::field`, `std::meta::alternative`, `std::meta::is_struct/is_class/is_enum`, `std::meta::function`, `std::meta::parameter`, `std::meta::methods`, `std::meta::interfaces`, `std::meta::template_args`, `std::meta::annotation`, `std::meta::has_annotation`, `std::meta::target`, `std::meta::name_of`, `std::meta::from_name` |
| Annotations (see 29) | `std::derive::formattable`, `std::derive::hashable`, `std::derive::ordered`, `std::deprecated`, `std::allow`, `std::must_use`, `std::inline_always`, `std::inline_never`, `std::cold`, `std::test` |
| Build (see 24, 25) | `std::build::graph`, `std::build::mode::settings` (interface), the mode classes `std::build::mode::debug` / `::release` / `::release_with_debug_info`, `std::build::current_mode`, `std::build::mode_is`, `std::build::setting` |
| Diagnostics (see 23) | `std::diagnostic` (a declared warning id), `std::compile_error`, `std::compile_warning`, `std::compile_note`, `std::source_location`, `std::location_of`, `std::is_compile_time`, `std::literal_text`. There is no `std::assert`: use `run_time check` |
| Call sites (see 31) | `std::call_site`, `std::implicit<T>` |
| Logging (see 31) | `std::log::trace/debug/info/warn/error/fatal`, `std::log::validate`, `std::log::assert`, `std::log::level`, `@std::log::level_override`, `std::log::start`, `std::log::session`, `std::log::sink`, `console`, `file`, `memory`, `history`, `pattern`, `patterns::*`, `palette` / `style` / `color`, `filter`, `std::log::set_level` / `add_options` for build.rifts |
| Sorting | `std::sort_order::ascending`, `std::sort_order::descending` |
| Bits | `std::trailing_zeros`, `std::leading_zeros`, `std::count_ones` (→ `u32`), `std::rotate_left(x, n)`, `std::rotate_right(x, n)`: on any unsigned or signed integer, one machine instruction each |
| Hashing | `std::hash(key&) → u64`, `std::hash_bytes(raw_ptr<u8>, n)` (unsafe) |

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
- `std::span<T>` is a view class: `list.as_span()` on a `std::darray`, `std::span<T>::from_raw(p, n)` (unsafe). `std::span<mut T>` (`list.as_mut_span()`) gives mutable access to the elements; `slice(from, to)` and `chunks(n)` (a stream of non-overlapping parts) split spans, e.g. to hand parts to threads (see 27).
- `std::io_error` is a simple enum (`not_found`, `permission_denied`, `already_exists`,
  `is_directory`, `invalid_data`, `other`). Simple enums print as the name of their
  alternative: `f"{e}"` gives `not_found`.
- **Platform:** file I/O and `std::filesystem::executable_dir()` use the C library; the current
  library targets Linux (glibc).

### Container access
- `a[i]` panics when out of bounds. `a.get(i)` returns `std::option<T&>`.
- `std::darray<T>` methods include `push(T value) mut`, `pop() mut → std::option<T>`, `insert(i, value) mut`, `remove(i) mut → T`, `len() → usize`, `retain(pred) mut`, `with_capacity(n)` (static), `reserve(n) mut`, `capacity()`, `truncate(n) mut`, `shrink_to_fit() mut`, `resize(n, value) mut`, `swap_remove(i) mut → T` (O(1), the order changes), `contains(value&)`, `as_mut_span() mut`, and the sorts:
  - `sort() mut`: ascending with `<`; introsort (median-of-3 quicksort, insertion sort below 24
    elements, heapsort past a depth budget), so never worse than O(n log n). Not stable.
  - `sort_by(less) mut`: the same with a `bool(T&, T&)` callable.
  - `sort_by_key(order, key) mut`: **stable** merge sort (runs of 16, one buffer of `n` elements).
- `std::array<T, N>` keeps its `N` elements inline (no heap); it is a copy type when `T` is. It
  has `len()`, `a[i]`, `get(i)` and `as_span()`, and `for` walks it. `default` gives every element
  its default.
- `std::bitset<N>` is `N` bits stored inline: `test(i)`, `set(i, bool) mut`, `count()`, `len()`.
- `std::option<T&>` (from `get`, or `std::some(place)` where a `std::option<T&>` is expected)
  refers to a place; it is a view (see 12). `value_or(place)` on it returns a reference too.

### Maps
- `std::hash_map<K, V>`: keys in no particular order, found by `std::hash(key)`.
  Keys are integers, `char`, `bool`, `std::string`, `std::str`, or a type with a `u64 hash()`
  method; keys are compared with `==`.
- `std::tree_map<K, V>`: keys in ascending order, compared with `<`.
- Both have `insert(key, value) mut → std::option<V>` (the replaced value), `contains(key&)`,
  `get(key&) → std::option<V&>`, `get_mut(key&) mut → std::option<mut V&>`,
  `get_or_insert(key, value) mut → mut V&` (`m.get_or_insert(k, 0) += 1` counts),
  `remove(key&) mut → std::option<V>`, `m[key]` (panics when the key is missing), `len()`,
  `is_empty()`, `clear() mut`, and `entries()`: a stream (see 10, `for`) of
  `std::map_entry<K, V>` views with `.key` and `.value`. It allocates nothing and borrows the map.
- `hash_map` also has `with_capacity(n)` (static) and `reserve(n) mut`; `std::string` has `with_capacity(n)`, `reserve(n)` and `capacity()`; `tree_map` has `first()` / `last() → std::option<std::map_entry<K, V>>`.
```rift
mut std::hash_map<std::string, u32> ages = default;
ages.insert(std::string::from("ada"), 36);
for (std::map_entry<std::string, u32> e : ages.entries()) { std::println(f"{e.key}: {e.value}"); }
```
- An empty container is created with `default` or `T{}`: `std::darray<employee> list = default;`.
- A method declared `mut T& f() mut` returns a mutable reference, and its call is a place:
  `m.get_or_insert(k, 0) += 1`, `list.at(i) = x`.

### Performance
The standard library is meant to be the fast option, not a baseline that needs a third-party
replacement. Each container uses the design the fastest C++ libraries converged on, and
`bench/` compares release builds against libstdc++, Abseil and Boost (see bench/README.md).
None of this weakens safety: every public operation stays bounds- and state-checked; the
`unsafe` code is inside the library, behind the same API.

| Type | Layout |
|---|---|
| `std::string` | small-string optimization: 24 bytes; up to 23 bytes inline, longer text on the heap. Comparison uses `memcmp`. |
| `std::darray<T>` | pointer, length, capacity; grows by doubling. |
| `std::hash_map<K, V>` | SwissTable layout: one control byte per slot (empty, deleted, or 7 bits of the hash), probed 8 at a time with word-wide (SWAR) compares; slots hold key and value together; maximum load 7/8. Removal leaves a tombstone only when a group is full. |
| `std::tree_map<K, V>` | B-tree of minimum degree 8 (up to 15 keys per node, leaves without child pointers). `entries()` walks it with a fixed stack, no allocation. |
| `std::hash` | wyhash-style 64×64→128 multiply-fold, reading 8 bytes at a time; seeded per process from an ASLR address, so hash-flooding inputs can't be precomputed and iteration order is not stable across runs. |

Compiler side: Rift functions get internal linkage and `nounwind` (Rift has no exceptions), so
LLVM inlines and specializes across the whole program.

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
