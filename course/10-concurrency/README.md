# Lesson 10: Threads without data races

The borrow rules from lesson 2 extend across threads. A program that compiles has no data races.
Deadlocks and logic races are still possible.

## Two kinds of thread handle

```edax
std::thread::handle<u64> w = std::thread::spawn([]() { return sum_to(1000); });   // owns its captures
u64 r = w.join();

std::thread::scoped_handle<void> s = std::thread::spawn([&counter]() { ... });     // borrows
s.join();
```

- `spawn` takes a lambda. If the lambda captures **references**, you get a `scoped_handle`: the
  borrow lasts until the handle is joined, consumed or destroyed (and destroying it joins). It is
  `std::thread` and `std::jthread` with the lifetime checked for you.
- Options go first: `std::thread::spawn(std::thread::options{ .name = "io" }, f)`.
- A **panic in any thread aborts the process**, so `join()` returns the value directly; there is
  no "the thread failed" case.

## `std::sendable` and `std::shareable`

They are like Rust's `Send` and `Sync`, and the compiler derives them:
- A lambda can go to another thread only if its captures are sendable.
- A `T&` capture requires `T` to be shareable.
- A `mut &x` capture is still the only way to reach `x`. A second thread taking `x` is the usual
  "already borrowed mutably" error, and its note suggests an atomic or a mutex.

## Sharing state

| Type | |
|---|---|
| `std::atomic<T>` | `load`, `store`, `fetch_wrapping_add`, ... with explicit `std::memory_order` |
| `std::mutex<T>` | **owns** the value; `lock()` returns a guard, and `guard->push(x)` uses it; the guard unlocks when destroyed |
| `std::rw_mutex<T>`, `std::condition` | as you would expect |
| `thread_local` globals | per thread; `mut` needs no `unsafe` |

You can't reach the data of a `std::mutex<T>` without holding the lock, so there is nothing to
forget. Synchronized types change through **immutable** references, the way `std::atomic` does in
C++. That is why `[&log]` (not `mut &`) is enough to push into a mutex from two threads.

## Channels, pools, parallel algorithms

- `std::channel<T>::bounded(n)` / `::unbounded()`. You get `sender()` / `receiver()` ends, and
  `for (job j : receiver)` runs until the channel is closed and empty.
- `std::thread_pool::make(options)` has `for_chunks`, `reduce_chunks` (deterministic: the chunks
  are combined in order), and `spawn`, which gives a scoped task.
- `std::parallel::sort / sort_by / find / any / all / transform (pool, span, ...)`.

## Your goal

`exercise.ed` counts the primes below 10000 with three workers and two channels, collects a log
from two threads through a mutex, and sums 100000 samples on a thread pool. The output must be
identical on every run:

```
primes below 10000: 1229, first block 168, last block 112
jobs done: 10
logged 10 values, sum 45
pool sum 2475000.0
```

Things to get right:
- Results arrive in any order: sort them by id before printing.
- The worker lambdas *move* their channel ends in, and *borrow* the atomic. Which kind of handle
  does that make?
- Close the job channel, or the workers never finish.

## Try this afterwards

1. Replace the `std::atomic<u32>` with a plain `mut u32` captured as `[mut &jobs_done]`.
2. In part 2, make both threads capture a plain `std::darray<u32>` as `mut &`.
3. Forget `jobs.close()`. What happens? Is it a compile error, a panic, or a hang?
4. Use `std::parallel::sort` on `samples` and time it against `samples.sort()` with
   `std::time` (see `docs/design/stdlib/time/time.md`).
