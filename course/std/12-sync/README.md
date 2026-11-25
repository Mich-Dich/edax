# Std lesson 12: Synchronization in depth

Language lesson 10 showed why safe Edax has no data races: the borrow rules apply across threads, and the only things that change through a shared reference are **synchronized types**. This lesson is about those types: atomics with explicit memory orders, `std::mutex<T>` and `std::rw_mutex<T>` that own their data, `std::condition` for waiting until something is true, and the thread utilities around them.

**You will learn:** the atomic operations and memory orders, compare-exchange loops, mutex guards and their lifetime, `try_lock`, readers/writer locks, conditions with mandatory predicates, the debug check for self-deadlock, thread names and options, `available_parallelism`, and `std::cache_aligned`.

**You will build:** a job board that worker threads take jobs from (`exercise.ed`).

Examples are in [`examples/`](examples); `python3 course/check.py s12 --examples` checks them.

---

## 1. Atomics

`std::atomic<T>` for integers, `bool`, `char` and raw pointers. Created with `default` (zero) or `std::atomic<T>::make(v)`; a `static` one needs no `mut`, because it changes through immutable references.

| Method | Does |
|---|---|
| `load(order)`, `store(v, order)` | read, write |
| `exchange(v, order)` | write, return the old value |
| `compare_exchange(expected, desired, success, failure)` | `std::ok(old)` if it was `expected` and now is `desired`, else `std::error(actual)` |
| `compare_exchange_weak(...)` | may fail spuriously: cheaper inside a retry loop |
| `fetch_wrapping_add(v, order)`, `fetch_wrapping_sub` | add/subtract, **wrapping**, return the old value |
| `fetch_and`, `fetch_or`, `fetch_xor` | bit operations, return the old value |

- **Every operation names its order**: `relaxed`, `acquire`, `release`, `acquire_release`, `sequentially_consistent` (full words: `acq_rel` doesn't exist). There's no hidden default, because a hidden `seq_cst` is slow on ARM and a hidden `relaxed` would be a bug.
- An order that's invalid for the operation is a **compile error** from the library's own entry check:
  ```
  error[E4210]: an atomic load can't use 'release' ordering (use acquire, relaxed or sequentially_consistent)
  ```
- **Always lock-free.** An atomic the target can't do in hardware is a compile error, never a hidden lock.
- Atomic arithmetic **wraps**, and the name says so. A checked or "max" update is a compare-exchange loop:

```edax
mut u32 current = best.load(std::memory_order::relaxed);
while (v > current) {
    match (best.compare_exchange_weak(current, v, std::memory_order::relaxed, std::memory_order::relaxed)) {
        std::ok(_) => { return; },
        std::error(actual) => { current = actual; },     // someone else won: retry with their value
    }
}
```

The memory model is C++'s, as implemented by LLVM.

Examples: [`atomics.ed`](examples/atomics.ed), [`err_orders.ed`](examples/err_orders.ed).

## 2. Mutexes own their data

```edax
std::mutex<stats> shared = std::mutex<stats>::make(stats{});

void record(std::mutex<stats>& shared, u64 size) {
    mut std::mutex_guard<stats> s = shared.lock();
    s->files += 1;
}   // the guard is destroyed: unlocked
```

- `lock()` returns a **guard**: through a `mut` guard variable `g->` / `*g` give mutable access, through an immutable one read access. Unlocking is destroying the guard (or `g.unlock()`, which consumes it).
- `try_lock()` doesn't wait: an `std::option<std::mutex_guard<T>>`. `try_lock_for(d)` waits at most a duration.
- The guard is a view of the mutex: it can't be stored in a field or outlive it.
- **Keep guards short.** A guard lives until the end of its scope, and a temporary guard until the end of its statement. A block expression is the neat way to take what you need and unlock before the slow part:

```edax
u32 units = {
    mut std::mutex_guard<board> b = s.jobs.lock();
    ...
    b->waiting.pop().value_or(0)
};          // unlocked here
do_slow_work(units);
```

- **Self-deadlock is caught in debug builds.** Locking a mutex the same thread already holds would wait forever; in debug-based modes it panics with both places instead:

```edax
std::println(f"{*m.lock()} {*m.lock()}");   // two temporary guards alive in one statement
// panic: this thread already holds this mutex: locking it again would wait forever
```

- **Cost:** `T` plus one 32-bit word, inline. An uncontended lock is one compare-exchange; a contended one spins briefly, then sleeps in the OS (a futex).

### `std::rw_mutex<T>`

Many readers or one writer: `read()` gives a `std::read_guard<T>` (several may exist at once), `write()` a `std::write_guard<T>`. Writers aren't starved. Use it only where reads really dominate; an uncontended `std::mutex` is cheaper.

Examples: [`locks.ed`](examples/locks.ed), [`err_relock.ed`](examples/err_relock.ed).

## 3. Conditions

`std::condition` lets a thread wait until the data behind a mutex reaches a state:

```edax
mut std::mutex_guard<board> b = jobs.lock();
changed.wait(mut b, [](board& v) { return !v.waiting.is_empty() || v.closed; });   // unlocks while waiting
// here the predicate is true and b is locked again
```

and the other side changes the data, then notifies:

```edax
{ mut std::mutex_guard<board> b = jobs.lock(); b->waiting.push(units); }
changed.notify_one();        // or notify_all()
```

**The predicate is mandatory.** In C++, `cv.wait(lock)` without a predicate is a bug waiting for a spurious wake-up or a missed notification. Here `wait` only returns when the predicate is true, so neither can happen. `wait_for(mut g, predicate, d)` waits at most `d` and returns whether the predicate held.

Use `notify_all` when the change matters to every waiter (closing the board: every worker must see it and leave); `notify_one` when one waiter can act on it (one new job).

Example: [`condition.ed`](examples/condition.ed).

## 4. Threads, briefly

| Call | Does |
|---|---|
| `std::thread::spawn(std::thread::options{ .name = "io", .stack_size = ... }, f)` | a thread with options |
| `std::thread::set_current_name(name)`, `current_name()` | names show in logs, panics and debuggers |
| `std::thread::available_parallelism()` | how many threads can run at once |
| `std::thread::sleep(d)`, `spin_hint()` | |
| `std::cache_aligned<T>` | `T` aligned and padded to a cache line (64 bytes): values written by different threads never share one ("false sharing"); access with `*x` / `x->` |
| `std::thread_bound` (a zero-size field) | keeps a type on its thread: neither sendable nor shareable |

Example: [`threads_misc.ed`](examples/threads_misc.ed).

---

## C++ → Edax at a glance

| C++ | Edax |
|---|---|
| `std::atomic<int> x; x++` | `std::atomic<i32> x = default; x.fetch_wrapping_add(1, order)` |
| `std::memory_order_acq_rel` | `std::memory_order::acquire_release` |
| `std::mutex m; T data;` + `std::lock_guard` | `std::mutex<T>` + `lock()` |
| `std::shared_mutex` + `std::shared_lock` | `std::rw_mutex<T>` + `read()` |
| `cv.wait(lock, pred)` | `cond.wait(mut guard, pred)` (the predicate is required) |
| a recursive lock hang | a debug-mode panic naming both locks |
| `alignas(64)` + padding | `std::cache_aligned<T>` |
| `std::thread::hardware_concurrency()` | `std::thread::available_parallelism()` |

---

## Your goal

`exercise.ed` runs two named worker threads that take jobs from a shared board. A job is a number of units; its receipt is units × the price per unit, which is read from an `rw_mutex`. Fill in `work`, `post`, `close` and the price, so that the output is the same on every run:

```
6 jobs done, 6 receipts
smallest 5, largest 45, total 130
```

<details>
<summary>Hint 1: taking a job</summary>

In `work`, `while (true)` with a block expression: lock the board, `s.changed.wait(mut b, [](board& v) { return !v.waiting.is_empty() || v.closed; });`, then `match (b->waiting.pop())`: `std::some(u) => u`, `std::none => { return; }`. The block ends, the board unlocks, and the work happens outside the lock.
</details>

<details>
<summary>Hint 2: doing the job</summary>

`u32 price = *s.price_per_unit.read();`, `s.receipts.lock()->push(units * price);`, `_ = s.done.fetch_wrapping_add(1, std::memory_order::relaxed);`.
</details>

<details>
<summary>Hint 3: <code>post</code>, <code>close</code>, the price</summary>

Change the board through a `mut std::mutex_guard<board>` in a block of its own, then notify after the block (`notify_one` in `post`, `notify_all` in `close`). The price: `*s.price_per_unit.write() = 5;`.
</details>

---

## Try this afterwards

1. In `close`, use `notify_one` instead of `notify_all`.
2. Do the work (`receipts` and `done`) while still holding the board's guard. Does the output change? What does change?
3. Count the jobs with `mut u32 done` captured as `[mut &done]` by both workers.
4. Print `std::size_of<std::cache_aligned<u8>>()`.

<details>
<summary>What happens</summary>

1. It usually still finishes, which is what makes this bug dangerous: the workers are rarely *both* waiting at the moment the board closes. When they are, only one wakes up and leaves, and the other waits forever. Closing concerns every waiter.
2. The output is the same, but the workers no longer work in parallel: each job is done while the other worker is locked out. Lock for the shortest time that keeps the data consistent.
3. `error[E5023]`: two threads can't both borrow `done` mutably. That's what the atomic is for.
4. `64`: one byte of data, padded to a cache line.
</details>

## Recap

- Atomics are lock-free, wrap, and name their memory order; invalid orders don't compile.
- A mutex owns its data; the guard is the only way in; keep guards short (block expressions); self-deadlock panics in debug.
- `rw_mutex` for read-mostly data; `condition` waits on a mandatory predicate; `notify_all` when everyone must know.
- Name threads; pad hot counters with `std::cache_aligned`.

Next: [std lesson 13](../13-parallel/README.md), channels, pools and parallel algorithms.
