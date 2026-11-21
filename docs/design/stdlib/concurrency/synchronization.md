# Synchronization: Atomics, Mutexes, Conditions

The synchronized types are the only types that change through an immutable reference (see [Threads](threads.md)). They are written in Edax on `std::raw`'s atomic and futex intrinsics (see [Raw Memory: `std::raw`](../memory/raw-memory.md)).

## Rules

### `std::atomic<T>`
- `T` is an integer type, `bool`, `char` or `std::raw_ptr<U>`. It is created with `default` (zero) or `std::atomic<T>::make(value)`.
- **Atomics are always lock-free.** A `std::atomic<T>` the target can't do with hardware instructions (e.g. `std::atomic<u128>` without a 16-byte compare-exchange) is a compile error, never a hidden lock as in C++. `std::atomic<T>` has the size and alignment of `T`.
- **Every operation names its memory order.** There is no hidden default. The order must be a compile-time value; an order that is invalid for the operation is a compile error from the library's entry check (see [Checks](../../language/errors/checks.md)).

| Method (all on immutable `this`) | Does |
|---|---|
| `load(order) → T` | Read |
| `store(value, order)` | Write |
| `exchange(value, order) → T` | Write, return the old value |
| `compare_exchange(expected, desired, success_order, failure_order) → std::result<T, T>` | `ok(old)` if it was `expected` and is now `desired`, otherwise `error(actual)` |
| `compare_exchange_weak(...)` (same parameters) | Like `compare_exchange`, but **may fail even when the value matched**. Cheaper inside a retry loop on ARM and other LL/SC machines |
| `fetch_wrapping_add(v, order)`, `fetch_wrapping_sub(v, order) → T` | Add/subtract, **wrapping** on overflow, return the old value |
| `fetch_and`, `fetch_or`, `fetch_xor` `(v, order) → T` | Bit operations, return the old value |

- `std::memory_order` names: `relaxed`, `acquire`, `release`, `acquire_release`, `sequentially_consistent`. Full words, like the rest of `std` (see [The Standard Library](../README.md)).
- Atomic arithmetic **wraps**, and the method name says so. A checked atomic add would need a retry loop, which costs more than it looks (principle 7). Build it from `compare_exchange` when needed.
- `std::atomic_fence(order)` is a standalone fence.
- **The memory model is C++'s** (as implemented by LLVM). A program without data races, which every safe Edax program is, behaves as if its threads were interleaved.

### `std::mutex<T>`
A mutex **owns** the data it protects. There is no way to reach the data without locking.
```edax
std::mutex<stats> shared = std::mutex<stats>::make(stats{});
```
- `lock() → std::mutex_guard<T>` blocks until the mutex is free. `try_lock() → std::option<std::mutex_guard<T>>` doesn't wait; `try_lock_for(d) → std::option<std::mutex_guard<T>>` waits at most the `std::time::duration` `d`.
- The **guard** is a view of the mutex (it borrows it, see [Ownership, Copies, Moves and References](../../language/memory/ownership-and-references.md)), so it can't be stored in a field or outlive the mutex.
  - Through a `mut` guard variable, `*g` / `g->` give mutable access; through an immutable one, read access.
  - The mutex is unlocked when the guard is destroyed, or earlier with `unlock() move`.
- **Debug help:** in modes based on `std::build::mode::debug`, locking a mutex the same thread already holds panics with both locations instead of hanging. It is a `run_time(std::build::mode::debug) check` in the library.
- A mutex is `std::shareable` when `T` is `std::sendable`. One thread at a time gets a `mut T&`, which is exactly what sending means.
- **Cost:** a `std::mutex<T>` is `T` plus one 32-bit word, stored inline (no heap). Locking an uncontended mutex is one atomic compare-exchange. A contended lock spins briefly, then sleeps in the OS (futex on Linux) until it is woken. The guard is a pointer and costs nothing. The debug-only owner check adds a thread id field in modes based on `std::build::mode::debug` only.

### `std::rw_mutex<T>`
A lock for data that is read often and written rarely: any number of readers, or one writer.
- `read() → std::read_guard<T>` gives immutable access and can be held by several threads at once. `write() → std::write_guard<T>` gives mutable access to one thread, with no readers.
- `try_read()`, `try_write()` return `std::option`s and don't wait.
- Writers are not starved: once a writer waits, new readers wait behind it.
- It is `std::shareable` when `T` is `std::sendable` and `std::shareable` (readers on several threads share a `T&`).
- **Cost:** one 32-bit word next to `T`, like `std::mutex`. Use it only where reads really dominate; an uncontended `std::mutex` is cheaper.

### `std::condition`
Waits until the data behind a mutex reaches a state:
```edax
std::condition ready = default;

// waiting thread
mut std::mutex_guard<queue> q = jobs.lock();
ready.wait(mut q, [](queue& value) { return !value.is_empty(); });   // unlocks while waiting
job next = q->pop();

// other thread
jobs.lock()->push(move j);
ready.notify_one();                                                  // or notify_all()
```
- **The predicate is mandatory**, so spurious wake-ups and missed notifications can't cause bugs. `wait` returns only when the predicate is true, with the guard locked.
- `ready.wait_for(mut q, predicate, d) → bool` waits at most `d`; it returns whether the predicate is true, with the guard locked either way.

## Examples

### Shared state: mutex and atomic
```edax
struct stats {
    u32 files   = 0;
    u64 largest = 0;
}

private static std::atomic<u64> bytes_seen = default;

void record(std::mutex<stats>& shared, u64 size) {
    mut std::mutex_guard<stats> s = shared.lock();
    s->files += 1;
    if (size > s->largest) {
        s->largest = size;
    }
    _ = bytes_seen.fetch_wrapping_add(size, std::memory_order::relaxed);
}   // the guard is destroyed: unlocked
```

### Memory orders checked at compile time (in the library)
```edax
// std (atomic.ed)
T load(std::memory_order order) {
    compile_time check (order != std::memory_order::release && order != std::memory_order::acquire_release)
        else f"an atomic load can't use '{order}' ordering (use acquire, relaxed or sequentially_consistent)";
    ...
}
```

## Rationale
- **The mutex owns its data:** you can't forget to lock, because without locking there is no way to reach the data.
- **Memory orders always written out:** a hidden `sequentially_consistent` costs more than it looks on ARM, and a hidden `relaxed` would be a bug. The library rejects invalid orders at compile time with a `check` (see [Checks](../../language/errors/checks.md)).
- **Performance is part of the contract:** lock-free atomics or a compile error, an inline one-word mutex, no allocation per message on bounded channels, no initialization check on `thread_local`. These are promises the standard library has to keep on every target, so you can plan with them.
