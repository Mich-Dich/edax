# Lesson 10: Threads without data races

A data race is two threads touching the same memory at the same time, at least one of them writing, with no synchronization. In C++ it's undefined behaviour that the compiler can't see. In Edax it's a **compile error**, and you already know the rule that catches it: lesson 2's "one writer or many readers", applied across threads. A program that compiles has no data races. (Deadlocks and logic races, such as two correct but badly ordered steps, are still possible.)

**You will learn:** owned and scoped threads, how captures decide which one you get, `sendable` and `shareable`, atomics, `std::mutex<T>` that owns its data, channels, the thread pool, and the parallel algorithms.

**You will build:** a prime counter with workers and channels, a log filled from two threads, and a sum on a thread pool (`exercise.ed`).

Examples are in [`examples/`](examples); `python3 course/check.py 10 --examples` checks them.

---

## 1. Two kinds of thread handle

`std::thread::spawn` takes a lambda (lesson 7). **The lambda's captures decide what kind of thread you get:**

```edax
// No reference captures: the thread owns all it uses. Like std::thread, but it returns a value.
std::thread::handle<u64> w = std::thread::spawn([]() { return sum_to(1000); });
u64 r = w.join();

// A reference capture: a scoped thread. `totals` stays borrowed until the handle is joined.
std::thread::scoped_handle<void> s = std::thread::spawn([mut &totals]() { totals[0] = 55; });
s.join();
```

| | `handle<R>` | `scoped_handle<R>` |
|---|---|---|
| You get it when | the lambda has no reference captures | the lambda captures `&x` or `mut &x` |
| It may | outlive the function that started it | not outlive what it borrows |
| Destroying it | joins (call `detach()` to let it run on, visibly) | joins; there is no `detach` |
| C++ | `std::jthread` that can be returned | `std::jthread` with lifetimes checked |

A scoped handle **is** a borrow, the same as a reference: while it exists, `totals` is borrowed (mutably here) and the function can't touch it. Joining (or destroying) the handle ends the borrow. That's how a worker thread can safely use your locals.

- `join()` returns the lambda's result directly. A **panic in any thread aborts the whole process** (lesson 4), so there's no "the thread failed" case to handle.
- Options come first: `std::thread::spawn(std::thread::options{ .name = "io" }, f)`.

Example: [`threads.ed`](examples/threads.ed).

## 2. Why races don't compile

```edax
mut u32 hits = 0;
std::thread::scoped_handle<void> a = std::thread::spawn([mut &hits]() { hits += 1; });
std::thread::scoped_handle<void> b = std::thread::spawn([mut &hits]() { hits += 1; });   // ERROR
```

```
error[E5023]: 'hits' is already borrowed mutably by 'a'
 --> err_race.ed:7:62
  |
7 |     std::thread::scoped_handle<void> b = std::thread::spawn([mut &hits]() { hits += 1; });
  |                                                              ^^^^^^^^^ borrowed mutably
  |
6 |     std::thread::scoped_handle<void> a = std::thread::spawn([mut &hits]() { hits += 1; });
  |     --------------------------------------------------------------------------------------
  = note: the borrow lasts until 'a' is joined, consumed or destroyed
  = note: a 'mut &' borrow is the only way to reach the value while it lasts; to share a counter between threads use a std::atomic or a std::mutex (spec 27)
```

Nothing thread-specific is going on: it's the same error as two `mut` arguments for one variable (lesson 2), because a scoped handle is a borrow that lasts until it's joined.

### `std::sendable` and `std::shareable`

Two compiler-derived properties (Rust's `Send` and `Sync`) cover the rest:
- A lambda can go to another thread only if all its captures are **sendable** (most types are; raw pointers and some handles aren't).
- A `&x` capture (shared between threads) needs `x`'s type to be **shareable**: reading it from two threads at once must be safe.

You rarely write them, but you'll see them in error messages and in bounds such as `std::function<void(), std::sendable>`.

Example: [`err_race.ed`](examples/err_race.ed).

---

## 3. Sharing state

When several threads really must change one thing, use a type that synchronizes:

| Type | |
|---|---|
| `std::atomic<T>` | `load`, `store`, `fetch_wrapping_add`, ... with an explicit `std::memory_order` |
| `std::mutex<T>` | **owns** the value; `lock()` returns a guard, `guard->push(x)` uses it; unlocks when the guard is destroyed |
| `std::rw_mutex<T>`, `std::condition` | as you would expect |
| `thread_local` globals | one per thread; `mut` needs no `unsafe` |

**A mutex owns what it protects.** In C++ a `std::mutex` sits next to the data, and nothing stops code from reading the data without locking. In Edax the data is *inside* the mutex, and the only way to it is through the guard that `lock()` returns. There's nothing to forget.

**Synchronized types change through immutable references**, as `std::atomic` does in C++. So threads capture them with `[&log]`, not `[mut &log]`: many readers are fine, and the type does the synchronizing.

```edax
std::mutex<std::darray<u32>> log = std::mutex<std::darray<u32>>::make(default);
std::thread::scoped_handle<void> a = std::thread::spawn([&hits, &log]() {
    _ = hits.fetch_wrapping_add(1, std::memory_order::relaxed);
    log.lock()->push(1);                     // the guard unlocks at the end of the statement
});
```

Example: [`sharing.ed`](examples/sharing.ed).

---

## 4. Channels, pools, parallel algorithms

**Channels** send values from one thread to another:

- `std::channel<T>::bounded(n)` (a full channel makes `send` wait) or `::unbounded()`.
- `channel.sender()` / `channel.receiver()` give the ends. Workers usually **move** their ends into the lambda: `[move input, move output]`. With no reference captures, that's an owned thread.
- `for (job j : receiver)` runs until the channel is **closed and empty**. Forget `channel.close()` and the loop waits forever: that's a hang, not a compile error.
- `receiver.receive()` gives one value as an `std::option` (`none` once closed and empty).

**The thread pool** is for data-parallel work:

- `std::thread_pool::make(std::thread_pool::options{ .threads = 4 })`.
- `pool.for_chunks(span, chunk, f)` runs `f` on pieces of a span.
- `pool.reduce_chunks(span, chunk, init, map, combine)` maps each chunk, then combines the results **in order**, so a floating-point sum gives the same result on every run.
- `pool.spawn(f)` gives a scoped task.

**Parallel algorithms:** `std::parallel::sort / sort_by / find / any / all / transform(pool, span, ...)`.

Example: [`channels.ed`](examples/channels.ed).

---

## C++ → Edax at a glance

| C++ | Edax |
|---|---|
| `std::thread t(f); t.join();` | `std::thread::handle<void> t = std::thread::spawn(f); t.join();` |
| `std::async` + `future.get()` | `std::thread::spawn(f).join()` returns the value |
| `std::jthread` with `[&]` captures | `scoped_handle`, borrow checked |
| `std::mutex m; T data;` | `std::mutex<T> data;` |
| `std::lock_guard g(m); data.push(x);` | `data.lock()->push(x);` |
| `std::atomic<int>` | `std::atomic<i32>` with explicit memory orders |
| a queue + mutex + condition variable | `std::channel<T>` |
| a data race | a compile error |

---

## Your goal

`exercise.ed` counts the primes below 10000 with three workers and two channels, collects a log from two threads through a mutex, and sums 100000 samples on a thread pool. The output must be identical on every run:

```
primes below 10000: 1229, first block 168, last block 112
jobs done: 10
logged 10 values, sum 45
pool sum 2475000.0
```

Things to get right:
- Results arrive in any order: sort them by id before printing.
- The worker lambdas *move* their channel ends in, and *borrow* the atomic. Which kind of handle does that make?
- Close the job channel, or the workers never finish.

<details>
<summary>Hint 1: the workers</summary>

Per worker: take `jobs.receiver()` and `results.sender()` into locals, then spawn `[move input, move output, &jobs_done]() { for (range_job j : input) { ... } }`. The `&jobs_done` capture makes it a `std::thread::scoped_handle<void>`; keep the three in a `mut std::darray<std::thread::scoped_handle<void>>`.
</details>

<details>
<summary>Hint 2: submitting and collecting</summary>

Send the ten jobs through `jobs.sender()`, then `jobs.close()`. Receive exactly ten results with `results.receiver().receive().expect(...)`, then join the workers (`while (workers.pop() is std::some(w)) { w.join(); }`), then `sort_by_key(std::sort_order::ascending, [](range_result& r) { return r.id; })`.
</details>

<details>
<summary>Hint 3: the mutex part</summary>

`std::mutex<std::darray<u32>>::make(default)`, two scoped threads capturing `[&log]` inside a `{ }` block (so they're joined before you lock it yourself), each doing `log.lock()->push(...)`. Afterwards `mut std::mutex_guard<std::darray<u32>> entries = log.lock(); entries->sort();`.
</details>

<details>
<summary>Hint 4: the pool</summary>

`pool.reduce_chunks(samples.as_span(), 8192, 0.0, map, combine)` where `map` sums one `std::span<f64>` chunk and `combine` is `[](f64 a, f64 b) { return a + b; }`.
</details>

---

## Try this afterwards

1. Replace the `std::atomic<u32>` with a plain `mut u32` captured as `[mut &jobs_done]`.
2. In part 2, make both threads capture a plain `std::darray<u32>` as `mut &`.
3. Forget `jobs.close()`. Is it a compile error, a panic, or a hang?
4. Use `std::parallel::sort` on `samples` and time it against `samples.sort()` with `std::time` (see `docs/design/stdlib/time/time.md`).

<details>
<summary>What happens</summary>

1. `error[E5023]`: the second worker's `mut &jobs_done` overlaps the first one's, and the note suggests an atomic or a mutex.
2. The same error: two scoped threads may not both borrow the darray mutably.
3. A hang: the workers' loops wait for more jobs, and `main` waits for results that never come. The compiler prevents races, not deadlocks.
4. On a large enough array the parallel sort wins; on small ones the overhead of the pool does.
</details>

## Recap

- No reference captures: an owned `handle`. Reference captures: a `scoped_handle`, which is a borrow until joined.
- The lesson 2 borrow rules make data races compile errors.
- Synchronized types (`std::atomic`, `std::mutex<T>`) change through `&`; a mutex owns its data.
- Channels for pipelines, the pool and `std::parallel` for data-parallel work.

Next: [lesson 11](../11-build/README.md), `build.ed`.
