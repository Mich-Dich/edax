# Threads

Threads are part of the standard library (`std::thread`), and **data races are compile errors**. The rules come from what the language already has: references are second-class, views can't outlive what they borrow, `T&` means "not changed", and a `mut T&` is the only way to reach an object while it exists (see [Ownership, Copies, Moves and References](../../language/memory/ownership-and-references.md)). Threads only add two compiler-derived properties of types and a handful of synchronized library types.

Edax prevents **data races**: two threads touching the same memory without synchronization, with at least one writing. It does not prevent deadlocks or logic races (two correct steps in an unlucky order). Those are bugs, not undefined behaviour.

## Rules

### Which values may go to another thread
Two **structural interfaces** say what may cross threads. Like copy-ness (see [Ownership, Copies, Moves and References](../../language/memory/ownership-and-references.md)), the compiler derives them from a type's fields. Nobody writes `: std::sendable` on an ordinary type.

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
  ```edax
  template<type T>
  class darray : unsafe std::sendable(std::implements<T, std::sendable>()),
                 unsafe std::shareable(std::implements<T, std::shareable>()) { ... }

  class device_handle : unsafe std::sendable { ... }      // unconditional promise
  ```
  `unsafe` marks a promise the compiler can't check, as everywhere else (see [Raw Pointers and `unsafe`](../../language/memory/raw-pointers-and-unsafe.md)). The standard library's containers, strings and pointers make this promise; user code needs it only for its own `std::raw_ptr` wrappers.
- **Opting out** uses composition: a field of type `std::thread_bound` (a zero-size struct that is neither sendable nor shareable) keeps a type on its thread even without raw pointers. Example: a wrapper around an OpenGL texture id, which is only an integer.
- Use them as constraints like any interface: `template<type T : std::sendable>`.

### Changing shared data: only through synchronized types
`T&` means the object isn't changed (see [Ownership, Copies, Moves and References](../../language/memory/ownership-and-references.md)). The **only** exceptions are types whose job is synchronization. They change their state through an immutable reference, safely:

| Type | Purpose |
|---|---|
| `std::atomic<T>` | One integer, `bool`, `char` or `std::raw_ptr` changed atomically |
| `std::mutex<T>` | Data `T` that one thread at a time can lock and change |
| `std::condition` | Waiting until data behind a mutex reaches a state |
| `std::channel_sender<T>` / `std::channel_receiver<T>` | Passing values between threads |

These are written in Edax on top of `std::raw` and a few compiler intrinsics (see [Raw Pointers and `unsafe`](../../language/memory/raw-pointers-and-unsafe.md)). Every other type changes only through `mut`, which a second thread can never have at the same time.

### Starting a thread: `std::thread::spawn`
There is **one** function that starts a thread. What it returns depends on what the function **captures**:
```edax
std::thread::handle<u64> worker = std::thread::spawn([move queue]() { return drain(queue); });   // owns everything
std::thread::scoped_handle<f64> half = std::thread::spawn([left]() { return sum(left); });     // borrows (left is a view)
```

| The lambda... | `spawn` returns | The thread may... |
|---|---|---|
| owns all its captures | `std::thread::handle<R>` | outlive the current function: the handle can be stored, returned and detached |
| borrows (reference captures, or captured views) | `std::thread::scoped_handle<R>`, a **view** | only run while what it borrows exists: it is joined at the latest when the handle is destroyed |

- The lambda must be `std::sendable`, and so must its result `R`. Otherwise you get a compile error naming the capture or field that isn't (example 5).
- **Borrowing works through the existing view rules** (see [Ownership, Copies, Moves and References](../../language/memory/ownership-and-references.md)):
  - The `scoped_handle` is returned from a call whose one view argument is the lambda, so it borrows what the lambda borrows.
  - While the handle exists, those places are borrowed: read-only for `[&x]`, exclusively for `[mut &x]`.
  - A second thread capturing `mut &x`, or the current thread touching `x`, is a compile error until the handle is joined or destroyed (example 6).
- **`join() move`** waits for the thread and returns its result `R` (`void` for none). It consumes the handle; see "consuming methods" below.
- **Destroying a handle joins the thread.** A `std::thread::handle` that should keep running is given up explicitly with `detach() move`. A `scoped_handle` has no `detach`, because a detached borrowing thread could outlive what it borrows.
- **A panic in any thread aborts the whole process** (see [Error Handling](../../language/errors/error-handling.md)), so `join()` never fails and returns `R` directly.
- Options are a second overload:
  ```edax
  std::thread::spawn(std::thread::options{ .name = "decoder", .stack_size = 256 * 1024 }, [move input]() { ... });
  ```
  `.name` also names the thread for logs and debuggers. The default stack size is the platform's.
- **Scheduling, priority and CPU affinity** are platform-specific and are set with the platform's own option struct in `.platform`:
  ```edax
  std::thread::spawn(std::thread::options{
      .name     = "control",
      .platform = std::thread::platform_options::linux(std::thread::linux_options{
          .policy   = std::thread::linux_policy::fifo,
          .priority = 80,
          .cpus     = [2, 3],
      }),
  }, [move loop]() { ... });
  ```
  `std::thread::platform_options` has one alternative per supported OS (`linux`, `windows`, `macos`) plus `none` (the default). Using an alternative for another platform than the target's is a compile error. Each options struct names exactly what that OS offers, with no portable approximation.
- **Targets without threads:** on a target without the `threads` capability (see [Edax as a Build System](../../build/build-system.md)), `std::thread::spawn` is a compile error at the call. `std::atomic` still works.

### The current thread
| Call | Result |
|---|---|
| `std::thread::current_id()` | `std::thread::id`: a copy type, comparable, formattable |
| `std::thread::set_current_name(name)`, `std::thread::current_name()` | Sets / returns (`std::string`) the thread's name |
| `std::thread::available_parallelism()` | `usize`: how many threads can run at once |
| `std::thread::sleep(d)` | Pauses for at least `d` (a `std::time::duration`) |
| `std::thread::yield_now()`, `std::thread::spin_hint()` | See performance tools |

### Globals and thread-local storage
| Declaration | Rule |
|---|---|
| `static T x = v;` | Visible to every thread, so `T` must be `std::shareable`. Shared mutable globals are `static` synchronized types: `static std::atomic<u64> count = default;` |
| `static mut T x = v;` | Every read and write needs `unsafe`, for every type (see [Variables](../../language/basics/variables.md), [Raw Pointers and `unsafe`](../../language/memory/raw-pointers-and-unsafe.md)) |
| `thread_local T x = v;` / `thread_local mut T x = v;` | One copy per thread. The initializer is compile-time evaluable (like `static`); `mut` needs no `unsafe` because no other thread can see it. Values are destroyed when their thread ends, in reverse declaration order |

- `thread_local` is a new keyword, a storage modifier like `static`.
- A reference to a `thread_local` can't be captured by `std::thread::spawn` (it would reach another thread's copy).
- **Cost:** because the initializer is evaluated at compile time, accessing a `thread_local` is a load at a fixed offset from the thread pointer. C++ needs a hidden "already initialized?" check for some thread-locals; Edax never does.

### Performance tools
| Tool | Purpose |
|---|---|
| `std::cache_line_size` | A `compile_time usize` for the target (64 on x86-64, 128 on Apple ARM; a target description may say otherwise with `.cache_line`). `@std::layout{ .align = std::cache_line_size }` aligns a type to it |
| `std::cache_aligned<T>` | `T` aligned and padded to a cache line, so values written by different threads never share one ("false sharing"). Access with `*x` / `x->` |
| `std::thread::spin_hint()` | Tells the CPU the thread is busy-waiting (`pause` on x86, `yield` on ARM). Use it inside spin loops |
| `std::thread::yield_now()` | Gives the rest of the time slice to another thread |
| `std::thread_pool` | Runs many short tasks on a fixed set of threads (see [Thread Pool](thread-pool.md)) |

**Starting a thread is expensive** (an OS call and a new stack, typically tens of microseconds). `std::thread::spawn` is for long-running or blocking work: an I/O loop, a device driver, a thread with its own priority. Short computations go to a `std::thread_pool` (see [Thread Pool](thread-pool.md)).

## Examples

### Splitting work over borrowed data
```edax
f64 parallel_sum(std::span<f64> values) {
    usize half = values.length() / 2;
    std::span<f64> left  = values.slice(0, half);
    std::span<f64> right = values.slice(half, values.length());

    std::thread::scoped_handle<f64> other = std::thread::spawn([left]() { return sum(left); });
    f64 mine = sum(right);                        // this thread works too
    return other.join() + mine;
}
```
`left` is a view, so the lambda borrows and `spawn` returns a `scoped_handle`. The function can't return before the thread is joined: `other` is a local, and destroying it joins.

### Changing disjoint parts in parallel
```edax
void scale_all(mut std::darray<f32>& samples, f32 factor) {
    if (samples.is_empty()) {
        return;
    }
    usize part_size = samples.length() / std::thread::available_parallelism() + 1;
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
- `running` is a darray of views, so it is a view itself (see [Ownership, Copies, Moves and References](../../language/memory/ownership-and-references.md)) and can't escape the function.
- This shows the borrowing rules, but it starts one OS thread per part. In real code the same loop is one call: `pool.for_chunks(samples.as_mut_span(), 4096, [factor](std::span<mut f32> part) { ... })` (see [Thread Pool](thread-pool.md)).

### A type that must stay on its thread
```edax
class window {                       // wraps a GLFW window: must stay on the main thread
public:
    void draw() mut;
private:
    std::raw_ptr<mut glfw_window> handle;
}
```
```edax
std::thread::handle<void> t = std::thread::spawn([move win]() { win.draw(); });
```
```
error: the thread's lambda can't be moved to another thread
 --> app.ed:12:56
   |
12 |     std::thread::handle<void> t = std::thread::spawn([move win]() { win.draw(); });
   |                                                        ^^^^^^^^ 'win' has type 'gfx::window'
note: 'gfx::window' isn't std::sendable because its field 'handle' is a 'std::raw_ptr<mut glfw_window>'
 --> gfx/window.ed:7:5
```

### A data race that doesn't compile
```edax
mut u32 counter = 0;
std::thread::scoped_handle<void> a = std::thread::spawn([mut &counter]() { counter += 1; });
std::thread::scoped_handle<void> b = std::thread::spawn([mut &counter]() { counter += 1; });   // ERROR
```
```
error: 'counter' is already borrowed mutably by the thread 'a'
 --> main.ed:3:61
note: the borrow lasts until 'a' is joined or destroyed (main.ed:2:38)
```
To count from several threads, use a `std::atomic<u32>` (captured by `&`) or a `std::mutex<u32>`.

## Rationale
- **Data races as compile errors, from rules that already exist:** a borrowing thread is a view (see [Ownership, Copies, Moves and References](../../language/memory/ownership-and-references.md)), so the borrow checker's ordinary "frozen while borrowed" rule is what prevents races. Threads add no lifetime syntax, the same way the rest of the language has none.
- **One `spawn`, two handle types:** the user doesn't choose between "scoped" and "owned" APIs. What the lambda captures decides, and the handle's type makes the decision visible (principle 1).
- **Derived `sendable` / `shareable`:** like copy types, the answer follows from the fields. Explicit `unsafe` is needed only where the compiler can't see (raw pointers), and that's exactly where a human has to think.
- **Synchronized types are the one exception to "`T&` doesn't change":** that rule is what makes sharing safe, so its only exceptions are the types whose job is synchronization. That keeps the list short and greppable.
- **Destroying a handle joins:** this is RAII, where destruction already releases files and memory. A thread that should run on is `detach()`ed, visibly. Destruction that silently kills or abandons a thread (C++'s `std::thread` terminates the program) isn't an option for a safe language.
- **Threads, not `async`:** threads, the thread pool and channels cover the programs Edax is for (engines, tools, embedded control loops) without function colouring or an executor. A program with ten thousand waiting connections needs another design.
- **Platform options, not portable priorities:** "high priority" means different things on every OS; real-time code needs the exact policy and number, so it writes them.
- **No unwinding, so join can't fail:** a panic in any thread aborts the process (see [Error Handling](../../language/errors/error-handling.md)). Every thread's result type is just `R`.
