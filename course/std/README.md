# The standard library, for C++ developers

The second track of the course. The [language course](../README.md) teaches Edax itself; this one teaches the standard library you build programs with: containers and text in depth, memory, math, time, files, logging, crash handling, reflection, and concurrency beyond the basics. It assumes the language course's lessons 1–10 (lessons 11 and 12 help for a few details).

Each lesson works like the language lessons: a `README.md` that explains by contrast with the C++ standard library, `examples/` with complete programs, an exercise with an exact expected output, hints that open one at a time, and "Try this afterwards" experiments with folded answers.

```
python3 course/check.py s1              # check your std lesson 1
python3 course/check.py s1 --show       # also print what your program prints
python3 course/check.py s1 --examples   # the lesson's examples
python3 course/check.py --status --track std
```

## Lessons

| # | Lesson | You build |
|---|---|---|
| s1 | [Core utilities](01-core/README.md): `take`/`replace`/`swap`, checked and saturating arithmetic, bits, `<=>`, hashing, `nonzero`, `version`, 8-bit floats | a small resource manager |
| s2 | [Sequences in depth](02-sequences/README.md): capacity, `swap_remove`, `retain`, spans, `chunks`, `split_at`, the three sorts, `array`, `bitset` | a sensor-data pipeline |
| s3 | [Maps and your own keys](03-maps/README.md): the map API, `hash_map` vs `tree_map`, `@std::derive::hashable` / `ordered`, entries and borrowing | a warehouse index |
| s4 | [Text and formatting](04-text/README.md): `str` methods, parsing, building strings, format specs, `std::formatter`, lazy f-strings | an invoice from a text order |
| s5 | [Smart pointers and allocators](05-memory/README.md): `unique_ptr`, `shared_ptr`, `weak_ptr`, arenas, `string_with<A>` | a texture cache and a frame arena |
| s6 | [Vectors, rectangles and colours](06-math/README.md): `std::math`, half-open rects, OKLCH, contrast, easing | a toolbar layout |
| s7 | [Time](07-time/README.md): durations, `instant` vs `wall_time`, civil time, zones, testable time | a rate limiter and time stamps |
| s8 | [Files and the environment](08-files/README.md): `std::file`, `io_error`, paths, the relative-path warning, `std::env` | a settings file |
| s9 | [Logging](09-logging/README.md): sessions, sinks, patterns, compile-time levels, `validate`/`assert`, status lines | the logging of an asset importer |
| s10 | [Panics and crash handling](10-crashes/README.md): panic messages, the panic hook, chaining with `std::log`, debuggers, thread names | a crash reporter |
| s11 | [Reflection and annotations](11-meta/README.md): `std::meta`, `for compile_time`, enum names, writing annotations | a generic settings serializer |
| s12 | [Synchronization in depth](12-sync/README.md): atomics and orders, mutexes, `rw_mutex`, conditions, `cache_aligned` | a job board for worker threads |
| s13 | [Channels, pools, parallel algorithms](13-parallel/README.md): the channel API, pipelines, `thread_pool`, `std::parallel` | a measurement pipeline |

Lessons s1–s4 build on each other; after them, take the rest in any order (s12 before s13). The reference is [`docs/design/stdlib/`](../../docs/design/stdlib/README.md).
