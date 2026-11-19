# 27 - Threads and Synchronization

Threads are part of the standard library (`std::thread`), and **data races are compile errors**. The rules come from what the language already has: references are second-class, views can't outlive what they borrow, `T&` means "not changed", and a `mut T&` is the only way to reach an object while it exists (see 12). Threads only add two compiler-derived properties of types and a handful of synchronized library types.

Rift prevents **data races**: two threads touching the same memory without synchronization, with at least one writing. It does not prevent deadlocks or logic races (two correct steps in an unlucky order). Those are bugs, not undefined behaviour.

## Rules

### 1. Which values may go to another thread
Two **structural interfaces** say what may cross threads. Like copy-ness (see 12), the compiler derives them from a type's fields. Nobody writes `: std::sendable` on an ordinary type.

| Interface | Meaning |
|---|---|
| `std::sendable` | A value of this type may be **moved** to another thread |
| `std::shareable` | A `T&` to this type may be **used from several threads at the same time** |

How they are derived:

| Type | `std::sendable` if | `std::shareable` if |
|---|---|---|
| Built-in numbers, `bool`, `char`, `std::static_str` | always | always |
| `std::raw_ptr<T>` | **never** | **never** |
| `T&` | `T` is shareable | `T` is shareable |
| `mut T&` | `T` is sendable | `T` is shareable |
| struct, class, enum, lambda | every field / payload / capture is | every field / payload / capture is |

- **`std::raw_ptr` is neither**, because the compiler can't know whether the memory behind it is owned, shared or tied to a thread. Every type that holds one (a container, a C handle) is therefore neither by default.
- **A type that is safe anyway says so with `unsafe`** in its base list, optionally with a compile-time condition in parentheses, in the same `keyword(parameter)` form as `run_time(std::build::mode::debug)`:
  ```rift
  template<type T>
  class darray : unsafe std::sendable(std::implements<T, std::sendable>()),
                 unsafe std::shareable(std::implements<T, std::shareable>()) { ... }

  class device_handle : unsafe std::sendable { ... }      // unconditional promise
  ```
  `unsafe` marks a promise the compiler can't check, as everywhere else (see 13). The standard library's containers, strings and pointers make this promise; user code needs it only for its own `std::raw_ptr` wrappers.
- **Opting out** uses composition: a field of type `std::thread_bound` (a zero-size struct that is neither sendable nor shareable) keeps a type on its thread even without raw pointers. Example: a wrapper around an OpenGL texture id, which is only an integer.
- Use them as constraints like any interface: `template<type T : std::sendable>`.

### 2. Changing shared data: only through synchronized types
`T&` means the object isn't changed (see 12). The **only** exceptions are types whose job is synchronization. They change their state through an immutable reference, safely:

| Type | Purpose |
|---|---|
| `std::atomic<T>` | One integer, `bool`, `char` or `std::raw_ptr` changed atomically |
| `std::mutex<T>` | Data `T` that one thread at a time can lock and change |
| `std::condition` | Waiting until data behind a mutex reaches a state |
| `std::channel_sender<T>` / `std::channel_receiver<T>` | Passing values between threads |

These are written in Rift on top of `std::raw` and a few compiler intrinsics (see 13). Every other type changes only through `mut`, which a second thread can never have at the same time.

### 3. Starting a thread: `std::thread::spawn`
There is **one** function that starts a thread. What it returns depends on what the function **captures**:
```rift
std::thread::handle<u64> worker = std::thread::spawn([move queue]() { return drain(queue); });   // owns everything
std::thread::scoped_handle<f64> half = std::thread::spawn([left]() { return sum(left); });     // borrows (left is a view)
```

| The lambda... | `spawn` returns | The thread may... |
|---|---|---|
| owns all its captures | `std::thread::handle<R>` | outlive the current function: the handle can be stored, returned and detached |
| borrows (reference captures, or captured views) | `std::thread::scoped_handle<R>`, a **view** | only run while what it borrows exists: it is joined at the latest when the handle is destroyed |

- The lambda must be `std::sendable`, and so must its result `R`. Otherwise you get a compile error naming the capture or field that isn't (example 5).
- **Borrowing works through the existing view rules** (see 12):
  - The `scoped_handle` is returned from a call whose one view argument is the lambda, so it borrows what the lambda borrows.
  - While the handle exists, those places are borrowed: read-only for `[&x]`, exclusively for `[mut &x]`.
  - A second thread capturing `mut &x`, or the current thread touching `x`, is a compile error until the handle is joined or destroyed (example 6).
- **`join() move`** waits for the thread and returns its result `R` (`void` for none). It consumes the handle; see "consuming methods" below.
- **Destroying a handle joins the thread.** A `std::thread::handle` that should keep running is given up explicitly with `detach() move`. A `scoped_handle` has no `detach`, because a detached borrowing thread could outlive what it borrows.
- **A panic in any thread aborts the whole process** (see 11), so `join()` never fails and returns `R` directly.
- Options are a second overload:
  ```rift
  std::thread::spawn(std::thread::options{ .name = "decoder", .stack_size = 256 * 1024 }, [move input]() { ... });
  ```
  The default stack size is the platform's. Priority and CPU affinity are open (see 22).

### 4. `std::atomic<T>`
- `T` is an integer type, `bool`, `char` or `std::raw_ptr<U>`. It is created with `default` (zero) or `std::atomic<T>::make(value)`.
- **Atomics are always lock-free.** A `std::atomic<T>` the target can't do with hardware instructions (e.g. `std::atomic<u128>` without a 16-byte compare-exchange) is a compile error, never a hidden lock as in C++. `std::atomic<T>` has the size and alignment of `T`.
- **Every operation names its memory order.** There is no hidden default. The order must be a compile-time value; an order that is invalid for the operation is a compile error from the library's entry check (see 23).

| Method (all on immutable `this`) | Does |
|---|---|
| `load(order) → T` | Read |
| `store(value, order)` | Write |
| `exchange(value, order) → T` | Write, return the old value |
| `compare_exchange(expected, desired, success_order, failure_order) → std::result<T, T>` | `ok(old)` if it was `expected` and is now `desired`, otherwise `error(actual)` |
| `compare_exchange_weak(...)` (same parameters) | Like `compare_exchange`, but **may fail even when the value matched**. Cheaper inside a retry loop on ARM and other LL/SC machines |
| `fetch_wrapping_add(v, order)`, `fetch_wrapping_sub(v, order) → T` | Add/subtract, **wrapping** on overflow, return the old value |
| `fetch_and`, `fetch_or`, `fetch_xor` `(v, order) → T` | Bit operations, return the old value |

- `std::memory_order` names: `relaxed`, `acquire`, `release`, `acquire_release`, `sequentially_consistent`. Full words, like the rest of `std` (see 20).
- Atomic arithmetic **wraps**, and the method name says so. A checked atomic add would need a retry loop, which costs more than it looks (principle 7). Build it from `compare_exchange` when needed.
- `std::atomic_fence(order)` is a standalone fence.
- **The memory model is C++'s** (as implemented by LLVM). A program without data races, which every safe Rift program is, behaves as if its threads were interleaved.

### 5. `std::mutex<T>`
A mutex **owns** the data it protects. There is no way to reach the data without locking.
```rift
std::mutex<stats> shared = std::mutex<stats>::make(stats{});
```
- `lock() → std::mutex_guard<T>` blocks until the mutex is free. `try_lock() → std::option<std::mutex_guard<T>>` doesn't wait.
- The **guard** is a view of the mutex (it borrows it, see 12), so it can't be stored in a field or outlive the mutex.
  - Through a `mut` guard variable, `*g` / `g->` give mutable access; through an immutable one, read access.
  - The mutex is unlocked when the guard is destroyed, or earlier with `unlock() move`.
- **Debug help:** in modes based on `std::build::mode::debug`, locking a mutex the same thread already holds panics with both locations instead of hanging. It is a `run_time(std::build::mode::debug) check` in the library.
- A mutex is `std::shareable` when `T` is `std::sendable`. One thread at a time gets a `mut T&`, which is exactly what sending means.
- **Cost:** a `std::mutex<T>` is `T` plus one 32-bit word, stored inline (no heap). Locking an uncontended mutex is one atomic compare-exchange. A contended lock spins briefly, then sleeps in the OS (futex on Linux) until it is woken. The guard is a pointer and costs nothing. The debug-only owner check adds a thread id field in modes based on `std::build::mode::debug` only.

### 6. `std::condition`
Waits until the data behind a mutex reaches a state:
```rift
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
- A timed wait is open (see 22).

### 7. Channels
A **channel** is a thread-safe queue: values go in at one end and come out at the other, in order (first in, first out). There are three types:

| Type | What it is |
|---|---|
| `std::channel<T>` | The channel itself: a handle to the queue. It **creates the ends** and **closes** the channel. It is not an end itself |
| `std::channel_sender<T>` | An end that puts values in |
| `std::channel_receiver<T>` | An end that takes values out |

```rift
std::channel<job> jobs = std::channel<job>::bounded(64);   // a queue with room for 64 values
std::channel_sender<job>   submit = jobs.sender();          // an end to put values in
std::channel_receiver<job> input  = jobs.receiver();        // an end to take values out
```

**The queue**
- All three are handles to one shared queue on the heap. The queue is freed when the last handle of any kind is destroyed.
- `jobs.sender()` / `jobs.receiver()` can be called as often as needed, and `.clone()` on an end makes another end of the same kind. Any number of threads may send and receive.
- **Each value is received exactly once.** With several receivers, whichever is free first takes the next value. It's a work queue, not a broadcast.
- Values are **moved** through the channel, so `T` must be `std::sendable`. The receiving thread owns what it receives.

**Sending and receiving**

| Method | Result |
|---|---|
| `sender.send(T value) → std::result<void, T>` | `ok()`: the value is in the queue. On a full bounded channel `send` waits for room. `error(value)`: the channel is closed, and the value is handed back so it isn't lost |
| `sender.try_send(T value) → std::result<void, std::send_error<T>>` | Never waits: `error(std::send_error<T>::full(value))` or `error(std::send_error<T>::closed(value))`, so the value always comes back |
| `receiver.receive() → std::option<T>` | `some(value)`. On an empty channel `receive` waits. `none`: the channel is closed **and** empty, so nothing will ever arrive |
| `receiver.try_receive() → std::result<T, std::receive_error>` | Never waits: `error(std::receive_error::empty)` or `error(std::receive_error::closed)` |
| `receiver.next()` | Same as `receive()`: a receiver is a **stream**, so `for (job j : input) { ... }` runs until the channel is closed and drained (see 10) |

All of them work on immutable ends, because channels are synchronized types (section 2).

**Closing**
- `jobs.close()` closes the channel, on the `std::channel` object or on any sender. After that, `send` returns `error(value)`, and receivers still get every value already in the queue, then `none`.
- If the `std::channel` object **and** every sender have been destroyed, the channel is closed too: nobody could send anything any more, and a receiver must not wait forever.
- Closing doesn't depend on timing. A receiver started before the first sender exists still waits, because the `std::channel` object can still make senders.

**Bounded and unbounded**

| Created with | Memory | `send` when the consumer is slow |
|---|---|---|
| `std::channel<T>::bounded(n)` | Allocates room for `n` values **once**, when created. No allocation per message | Waits until there is room (backpressure) |
| `std::channel<T>::unbounded()` | Grows in blocks of values on the heap, and reuses freed blocks | Never waits, so memory grows instead |

**Cost:** a bounded channel is a lock-free ring buffer. Each slot holds a value and a sequence number; senders and receivers claim slots with one atomic operation, and the send and receive positions sit on separate cache lines. A thread only sleeps in the OS when the queue is full (sender) or empty (receiver).

### 8. Globals and thread-local storage
| Declaration | Rule |
|---|---|
| `static T x = v;` | Visible to every thread, so `T` must be `std::shareable`. Shared mutable globals are `static` synchronized types: `static std::atomic<u64> count = default;` |
| `static mut T x = v;` | Every read and write needs `unsafe`, for every type (see 03, 13) |
| `thread_local T x = v;` / `thread_local mut T x = v;` | One copy per thread. The initializer is compile-time evaluable (like `static`); `mut` needs no `unsafe` because no other thread can see it. Values are destroyed when their thread ends, in reverse declaration order |

- `thread_local` is a new keyword, a storage modifier like `static`.
- A reference to a `thread_local` can't be captured by `std::thread::spawn` (it would reach another thread's copy).
- **Cost:** because the initializer is evaluated at compile time, accessing a `thread_local` is a load at a fixed offset from the thread pointer. C++ needs a hidden "already initialized?" check for some thread-locals; Rift never does.

### 9. Consuming methods: trailing `move`
`join()`, `detach()` and `unlock()` end the life of their object. That needs methods that take ownership of `this`, so this file also settles open question 4:
```rift
R join() move;
```
- A method ending in **`move`** consumes `this`. It can be called on an owned value (a local, a by-value parameter or a temporary), never through `T&` or `mut T&`.
- After `worker.join()`, `worker` counts as moved: using it again is the normal use-after-move error, with a note naming the consuming method.
- It stands where `mut` stands in a mutating method's declaration (see 06), and it means "takes `this`" the way `move x` means "takes `x`" (see 12).

### 10. `void` as a template argument
`std::thread::handle<void>` and `std::result<void, T>` need it, so:

- `void` may be a template argument. Instantiating a template that uses it as a field or parameter type is an error at instantiation.
- An enum alternative whose payload type is `void` has no payload: `std::ok()`.

### 11. Performance tools
| Tool | Purpose |
|---|---|
| `std::cache_line_size` | A `compile_time usize` for the target (64 on x86-64, 128 on Apple ARM) |
| `std::cache_aligned<T>` | `T` aligned and padded to a cache line, so values written by different threads never share one ("false sharing"). Access with `*x` / `x->` |
| `std::thread::spin_hint()` | Tells the CPU the thread is busy-waiting (`pause` on x86, `yield` on ARM). Use it inside spin loops |
| `std::thread::yield_now()` | Gives the rest of the time slice to another thread |
| `std::thread_pool` | Runs many short tasks on a fixed set of threads (see 28) |

**Starting a thread is expensive** (an OS call and a new stack, typically tens of microseconds). `std::thread::spawn` is for long-running or blocking work: an I/O loop, a device driver, a thread with its own priority. Short computations go to a `std::thread_pool` (see 28).

## Examples

### 1. Splitting work over borrowed data
```rift
f64 parallel_sum(std::span<f64> values) {
    usize half = values.len() / 2;
    std::span<f64> left  = values.slice(0, half);
    std::span<f64> right = values.slice(half, values.len());

    std::thread::scoped_handle<f64> other = std::thread::spawn([left]() { return sum(left); });
    f64 mine = sum(right);                        // this thread works too
    return other.join() + mine;
}
```
`left` is a view, so the lambda borrows and `spawn` returns a `scoped_handle`. The function can't return before the thread is joined: `other` is a local, and destroying it joins.

### 2. Changing disjoint parts in parallel
```rift
void scale_all(mut std::darray<f32>& samples, f32 factor) {
    if (samples.is_empty()) {
        return;
    }
    usize part_size = samples.len() / std::thread::available_parallelism() + 1;
    mut std::darray<std::thread::scoped_handle<void>> running = default;

    for (std::span<mut f32> part : samples.as_mut_span().chunks(part_size)) {
        running.push(std::thread::spawn([move part, factor]() {
            for (mut f32& x : part) {
                x *= factor;
            }
        }));
    }
}   // 'running' is destroyed here: every thread is joined before the caller sees 'samples' again
```
- `chunks(n)` yields **non-overlapping** `std::span<mut f32>` views, so each thread gets exclusive access to its part. Two threads can't capture the same part, because a `std::span<mut T>` isn't a copy type and moving it twice is an error.
- A `std::span<mut T>` gives mutable access to its elements even through an immutable variable or capture, like a `mut T&` parameter does.
- `running` is a darray of views, so it is a view itself (see 12) and can't escape the function.
- This shows the borrowing rules, but it starts one OS thread per part. In real code the same loop is one call: `pool.for_chunks(samples.as_mut_span(), 4096, [factor](std::span<mut f32> part) { ... })` (see 28).

### 3. Producer and consumers with a channel
```rift
struct job {
    u32         id;
    std::string path;
}

std::exit_code main() {
    std::channel<job> jobs    = std::channel<job>::bounded(64);
    std::channel<u64> results = std::channel<u64>::unbounded();

    mut std::darray<std::thread::handle<void>> workers = default;
    for (usize i : 0..4) {
        std::channel_receiver<job> input  = jobs.receiver();
        std::channel_sender<u64>   output = results.sender();
        workers.push(std::thread::spawn([move input, move output]() {
            for (job j : input) {                     // until 'jobs' is closed and empty
                _ = output.send(file_size(j.path));
            }
        }));
    }

    std::channel_sender<job> submit = jobs.sender();
    for (u32 n : 0..100) {
        _ = submit.send(job{ .id = n, .path = std::format(f"data/{n}.bin") });   // waits while 64 are queued
    }
    jobs.close();                                     // no more jobs: the workers' loops end once the queue is empty

    std::channel_receiver<u64> sizes = results.receiver();
    mut u64 total = 0;
    for (u32 n : 0..100) {                            // exactly one result per job
        total += sizes.receive().expect("a worker stopped early");
    }
    std::println(f"{total} bytes");
    return std::exit_code::success;
}   // 'workers' destroyed: every worker is joined
```

### 4. Shared state: mutex and atomic
```rift
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

### 5. A type that must stay on its thread
```rift
class window {                       // wraps a GLFW window: must stay on the main thread
public:
    void draw() mut;
private:
    std::raw_ptr<mut glfw_window> handle;
}
```
```rift
std::thread::handle<void> t = std::thread::spawn([move win]() { win.draw(); });
```
```
error: the thread's lambda can't be moved to another thread
 --> app.rifts:12:56
   |
12 |     std::thread::handle<void> t = std::thread::spawn([move win]() { win.draw(); });
   |                                                        ^^^^^^^^ 'win' has type 'gfx::window'
note: 'gfx::window' isn't std::sendable because its field 'handle' is a 'std::raw_ptr<mut glfw_window>'
 --> gfx/window.rifts:7:5
```

### 6. A data race that doesn't compile
```rift
mut u32 counter = 0;
std::thread::scoped_handle<void> a = std::thread::spawn([mut &counter]() { counter += 1; });
std::thread::scoped_handle<void> b = std::thread::spawn([mut &counter]() { counter += 1; });   // ERROR
```
```
error: 'counter' is already borrowed mutably by the thread 'a'
 --> main.rifts:3:61
note: the borrow lasts until 'a' is joined or destroyed (main.rifts:2:38)
```
To count from several threads, use a `std::atomic<u32>` (captured by `&`) or a `std::mutex<u32>`.

### 7. Memory orders checked at compile time (in the library)
```rift
// std (atomic.rifts)
T load(std::memory_order order) {
    compile_time check (order != std::memory_order::release && order != std::memory_order::acquire_release)
        else f"an atomic load can't use '{order}' ordering (use acquire, relaxed or sequentially_consistent)";
    ...
}
```

## Rationale
- **Data races as compile errors, from rules that already exist:** a borrowing thread is a view (see 12), so the borrow checker's ordinary "frozen while borrowed" rule is what prevents races. Threads add no lifetime syntax, the same way the rest of the language has none.
- **One `spawn`, two handle types:** the user doesn't choose between "scoped" and "owned" APIs. What the lambda captures decides, and the handle's type makes the decision visible (principle 1).
- **Derived `sendable` / `shareable`:** like copy types, the answer follows from the fields. Explicit `unsafe` is needed only where the compiler can't see (raw pointers), and that's exactly where a human has to think.
- **Synchronized types are the one exception to "`T&` doesn't change":** that rule is what makes sharing safe, so its only exceptions are the types whose job is synchronization. That keeps the list short and greppable.
- **The mutex owns its data:** you can't forget to lock, because without locking there is no way to reach the data.
- **Memory orders always written out:** a hidden `sequentially_consistent` costs more than it looks on ARM, and a hidden `relaxed` would be a bug. The library rejects invalid orders at compile time with a `check` (see 23).
- **Destroying a handle joins:** this is RAII, where destruction already releases files and memory. A thread that should run on is `detach()`ed, visibly. Destruction that silently kills or abandons a thread (C++'s `std::thread` terminates the program) isn't an option for a safe language.
- **No unwinding, so join can't fail:** a panic in any thread aborts the process (see 11). Every thread's result type is just `R`.
- **`std::channel<T>` as a separate object:** creating a channel gives the queue a name. Its ends are made from it, so there is no need to return two values at once, and closing is something you *do* (`close()`), not something that happens when the last handle disappears in some other thread.
- **Performance is part of the contract:** lock-free atomics or a compile error, an inline one-word mutex, no allocation per message on bounded channels, no initialization check on `thread_local`. These are promises the standard library has to keep on every target, so you can plan with them.
- **One `std::shared_ptr`, always with an atomic count:** this closes open question 3. Two pointer types (Rust's `Rc` and `Arc`) would be a second way to do the same thing. The atomic increment is cheap next to the allocation that created the pointer.
