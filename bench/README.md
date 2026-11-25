# Standard library benchmarks

Edax's standard library is meant to be the fast option out of the box: no "use Boost/Abseil
when it matters". These benchmarks keep it honest by running the same workload in Edax and in
C++ (libstdc++, and Abseil / Boost where they have a competing container).

```
python3 bench/run.py [--edaxc build/compiler/edaxc] [--runs 3] [filter...]
```

- `edax/<name>.ed` is built with `edaxc build --mode=release`.
- `cpp/<name>.cpp` or `cpp/<name>.<impl>.cpp` (`impl` = `std`, `absl`, `boost`) is built with
  `c++ -O2 -std=c++20 -DNDEBUG` (Abseil via `pkg-config`). Missing libraries skip that row.
- Every program prints a checksum; the runner fails if Edax and C++ disagree.
- The table shows the best wall-clock time over `--runs` and Edax relative to the fastest C++.

Requirements: a C++20 compiler; optionally `libabsl-dev` and `libboost-dev`.

| Benchmark | Workload |
|---|---|
| `hash_int` | `hash_map<u64, u64>`: 2M inserts, 4M lookups (half hits), 1M removes |
| `hash_words` | `hash_map<string, u32>`: count 2M words drawn from 50k distinct ones |
| `sort` | sort 2M pseudo-random `u32` |
| `sort_by_key` | stable sort of 500k records by a key |
| `string_build` | 5M appends, plus many short strings |
| `text_parse` | split a 1M-line CSV text and parse numbers |
| `tree_map` | `tree_map<u32, u32>`: 1M random inserts, 2M lookups, ordered walk |
| `vec_push_sum` | `darray`: push 20M integers, sum by index and by `for` |
| `particles` | 200k particles in SoA columns, 1000 steps of gravity, drag and a bounce (spec 34) |
| `plot_decimate` | `std::ui::plot::decimate_indexed`: 1M `f32` reduced to 1000 pixel columns, 200 times (C++: the same algorithm, a plain loop) |

## Results (spec 34, x86-64 baseline, GCC 13 `-O2` for C++)

Measured in a shared container (best of 5; the times vary by about 10%). Abseil and Boost were not
installed there, so the comparison is against libstdc++ only; the earlier table has them.

| Benchmark | Edax (s) | C++ std (s) | Edax / C++ | What changed |
|---|---|---|---|---|
| hash_int | 0.260 | 0.795 | 0.33x | |
| hash_words | 0.077 | 0.154 | 0.50x | |
| particles | 0.135 | 0.247 | 0.55x | alias scopes for owned memory, conditional stores as selects: vectorized (was 1.37x) |
| plot_decimate | 0.278 | 0.879 | 0.32x | a column of an index-x series is a slice (1.4 ms per million points) |
| sort | 0.024 | 0.125 | 0.19x | radix sort for integers (was 1.12x) |
| sort_by_key | 0.006 | 0.038 | 0.16x | radix sort for integer keys (was 1.20x) |
| string_build | 0.014 | 0.017 | 0.82x | |
| text_parse | 0.116 | 0.163 | 0.72x | |
| tree_map | 0.594 | 1.647 | 0.36x | |
| vec_push_sum | 0.123 | 0.226 | 0.55x | |

With `--cpu=x86_64_v3` (AVX2), `particles` takes 0.11 s; g++ `-march=x86-64-v3` takes 0.32 s (it
doesn't vectorize the loop).

## Results (v0.1 + std optimization, x86-64, GCC 14 for C++)

| Benchmark | Edax (s) | C++ (s) | Edax / fastest |
|---|---|---|---|
| hash_int | 0.235 | absl 0.226, boost 0.187, std 0.766 | 1.26x |
| hash_words | 0.058 | absl 0.056, boost 0.068, std 0.122 | 1.03x |
| sort | 0.183 | std 0.162 | 1.13x |
| sort_by_key | 0.035 | std 0.036 | 0.95x |
| string_build | 0.016 | std 0.020 | 0.83x |
| text_parse | 0.143 | std 0.195 | 0.73x |
| tree_map | 0.521 | absl 0.440, std 1.534 | 1.19x |
| vec_push_sum | 0.124 | std 0.268 | 0.46x |

Before the optimization pass: `hash_int` 1.90x, `string_build` 2.22x, `tree_map` 141x (a sorted
array), and `sort` was an O(n²) insertion sort.

Edax's maps are always seeded per process (hash-flooding resistance) and every access is
checked; the C++ versions do neither, so a small gap remains on `hash_int` and `tree_map`.
