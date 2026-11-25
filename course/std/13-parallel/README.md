# Std lesson 13: Channels, thread pools and parallel algorithms

Threads and locks (std lesson 12) are the building blocks. Most parallel programs are better written one level up: **channels** connect stages that run on their own threads, a **thread pool** runs many short tasks and data-parallel loops on a fixed set of threads, and **`std::parallel`** has the common algorithms ready-made. The borrow rules still apply everywhere, so what a task borrows stays borrowed until it's finished, and a loop body that could race doesn't compile.

**You will learn:** the full channel API (`try_send`, `try_receive`, `receive_for`, closing), pipelines of threads, `std::thread_pool` with `spawn`, `for_chunks`, `for_index_chunks` and `reduce_chunks`, why the chunk size is always written, and `std::parallel::sort / find / any / all / transform`.

**You will build:** a measurement pipeline: a reader thread, two parser threads, then statistics on a pool (`exercise.ed`).

Examples are in [`examples/`](examples); `python3 course/check.py s13 --examples` checks them.

---

## 1. Channels in depth

| Method | Result |
|---|---|
| `sender.send(v)` | `std::result<void, T>`: on a full bounded channel it waits; `error(v)` when closed: **the value comes back** |
| `sender.try_send(v)` | never waits: `error(std::send_error<T>::full(v))` or `closed(v)` |
| `receiver.receive()` | `std::option<T>`: waits; `none` once closed **and** empty |
| `receiver.try_receive()` | never waits: `error(std::receive_error::empty)` or `closed` |
| `receiver.receive_for(d)` | waits at most `d`: `error(std::receive_error::timed_out)` |
| `for (T v : receiver)` | until closed and drained |

- A value is never lost: a failed send hands it back so you can retry, log it or drop it on purpose.
- `channel.close()` (or on any sender) closes it. Destroying the `std::channel` object **and** every sender closes it too, because nobody could send any more.
- Each value is received exactly once. Several receivers make a work queue, not a broadcast.
- `bounded(n)` allocates once and applies backpressure; `unbounded()` grows instead. The name says which.
- A `std::result<void, E>`'s `ok` alternative is matched as `std::ok(_)`.

**Pipelines.** Stage threads that move their channel ends in (`[move input, move output]`) and loop `for (x : input)` shut down in order: close the first channel, join the stage, close the next, and so on. Forgetting a `close` is a hang, not a compile error.

Example: [`channels.ed`](examples/channels.ed).

## 2. Thread pools

```edax
std::thread_pool pool = std::thread_pool::make(std::thread_pool::options{ .threads = 4 });
```

Starting an OS thread costs tens of microseconds; handing a task to a pool, well under one. There is **no global pool**: a function that runs in parallel takes a `std::thread_pool&`, so its signature tells you.

| Call | Does |
|---|---|
| `pool.spawn(f)` | a single task: a `task<R>` if `f` owns its captures, a `scoped_task<R>` (a view) if it borrows. `join()` returns the result |
| `pool.for_chunks(span, chunk, f)` | calls `f` on consecutive, non-overlapping chunks, in parallel; returns when all are done |
| `pool.for_index_chunks(count, chunk, f)` | the same with index ranges `f(from, to)` |
| `pool.reduce_chunks(span, chunk, identity, map, combine)` | `map` per chunk, `combine` the results in **chunk order** |

- **Loops borrow safely.** `for_chunks` returns only when every chunk is done, so its body may borrow anything. With a `std::span<mut T>`, each chunk is changed by exactly one thread.
- **The body must be shareable**: it runs on several threads at once, so it may capture copies and `&` references, never `mut &`:
  ```
  error[E4231]: 'thread_pool::for_chunks' runs the lambda on several threads at once, but it can't be shared between them
  ```
  Mutable results come out through the chunks, or through `reduce_chunks`.
- **The chunk size is always written.** It's the most important performance setting of a parallel loop: too small and scheduling dominates, too large and cores run out of work. A few thousand simple elements per chunk is a good start.
- **`reduce_chunks` is deterministic.** The chunks depend only on `chunk`, not on the number of threads, and results are combined in a fixed order. A floating-point sum gives the same bits on 2 cores and on 64, which unsynchronized parallel sums and atomics don't.
- A thread that joins a task runs other tasks while it waits, so nested parallelism can't deadlock the pool. Long *blocking* work (I/O, waiting on a channel) belongs on its own `std::thread`.

Examples: [`pool.ed`](examples/pool.ed), [`err_pool.ed`](examples/err_pool.ed).

## 3. `std::parallel`

| Function | Does |
|---|---|
| `std::parallel::sort(pool, span)`, `sort_by`, `sort_by_key` | like the sequential sorts (`sort_by_key` stable) |
| `std::parallel::find(pool, span, pred)` | the **first** matching index, the same as a sequential search |
| `std::parallel::any`, `all` | `bool` |
| `std::parallel::transform(pool, input, output, f)` | `output[i] = f(input[i])` |

Small inputs (below a few thousand elements) run on the calling thread alone, so calling them on small data costs nothing extra. Predicates and keys must be shareable, as for `for_chunks`.

Example: [`algorithms.ed`](examples/algorithms.ed).

---

## C++ → Edax at a glance

| C++ | Edax |
|---|---|
| a queue + mutex + condition variable | `std::channel<T>` |
| `std::async` | `pool.spawn(f)` |
| `std::for_each(std::execution::par, ...)` | `pool.for_chunks(span, chunk, f)` |
| `std::reduce(std::execution::par, ...)` (order unspecified) | `pool.reduce_chunks(...)` (deterministic) |
| `std::sort(std::execution::par, ...)` | `std::parallel::sort(pool, span)` |
| `std::find_if(std::execution::par, ...)` | `std::parallel::find(pool, span, pred)` |
| TBB / a global pool | an explicit `std::thread_pool&` |

---

## Your goal

`exercise.ed` reads measurements from text. A reader thread sends lines through a bounded channel to two parser threads, which send the numbers on through a second channel and count the lines that aren't numbers. Then a pool computes the range, the mean, the deviations in place, and finds the first value above the mean. Fill in the TODOs so that it prints:

```
7 values, 2 rejected, smallest 11.5, largest 15.5
range 11.5 to 15.5
mean 13.14, deviations from -1.64 to 2.36
first value above the mean is number 5
```

<details>
<summary>Hint 1: the parsers</summary>

For each of two parsers: take `lines.receiver()` and `values.sender()` into locals and spawn `[move input, move output, &rejected]() { for (std::str line : input) { match (line.trim().parse<f32>()) { ... } } }`. The `&rejected` capture makes it a `scoped_handle<void>`.
</details>

<details>
<summary>Hint 2: the reader, and shutting down</summary>

`[move reader, text]() { for (std::str line : text.split('\n')) { if (!line.trim().is_empty()) { _ = reader.send(line); } } }` (a channel of `std::str` views makes it a scoped thread). Then: join the reader, `lines.close()`, join the parsers, `values.close()`, and `for (f32 v : values.receiver())` into `out`.
</details>

<details>
<summary>Hint 3: the range</summary>

`pool.reduce_chunks(values.as_span(), 2, (f32::max, f32::min), map, combine)` where `map` returns `(lo, hi)` of a chunk and `combine` is `[]((f32, f32) a, (f32, f32) b) { return (a.0.min(b.0), a.1.max(b.1)); }`. The identity `(f32::max, f32::min)` changes nothing when combined.
</details>

<details>
<summary>Hint 4: mean, deviations, first above</summary>

A `reduce_chunks` sum, divided by the count; `pool.for_chunks(values.as_mut_span(), 2, [mean](std::span<mut f32> part) { for (mut f32& v : part) { v -= mean; } });`; then `std::parallel::find(pool, values.as_span(), [](f32& v) { return v > 0.0; })`.
</details>

---

## Try this afterwards

1. Remove `lines.close()`.
2. Print the values before `std::parallel::sort`, and run the program a few times.
3. Make the `for_chunks` body count the values it changed with `[mean, mut &count]`.
4. Replace `reduce_chunks` for the sum with a `for_chunks` that adds into a `std::atomic<u32>` of the values times 100. What do you lose?

<details>
<summary>What happens</summary>

1. A hang: the parsers wait for more lines forever, and the joins never return.
2. The order differs between runs: two parsers race to send. Everything after the sort is deterministic again.
3. `error[E4231]`: the body runs on several threads at once and can't hold a `mut &`.
4. It works, but you had to turn floats into integers to add them atomically, and the code no longer says "a sum in a fixed order". `reduce_chunks` is the tool for results.
</details>

## Recap

- Channels hand values back on failure, never lose them, and close explicitly; pipelines shut down stage by stage.
- A pool runs tasks, chunked loops and deterministic reductions; loop bodies must be shareable; the chunk size is always yours.
- `std::parallel` has sort, find, any, all and transform, with sequential results.

That's the standard-library track. The reference for everything here is [`docs/design/stdlib/`](../../../docs/design/stdlib/README.md).
