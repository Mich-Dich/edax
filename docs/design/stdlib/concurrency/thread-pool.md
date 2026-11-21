# Thread Pool

`std::thread_pool` runs **many short tasks on a fixed set of threads**. Starting an OS thread costs tens of microseconds; handing a task to a pool costs well under one. Parallel loops (`for_chunks`), parallel reductions (`reduce_chunks`) and single tasks (`spawn`) all go through it.

The pool follows the thread rules of 27: what a task borrows stays borrowed until the task is finished, and data races are compile errors.

## Rules

### 1. Creating a pool
```rift
std::thread_pool pool = std::thread_pool::make(std::thread_pool::options{
    .threads = std::thread::available_parallelism(),
});
```

| Option | Meaning | Default |
|---|---|---|
| `.threads` | Number of worker threads | **required** |
| `.name` | Prefix for the workers' names (`"pool-0"`, `"pool-1"`, ...) | `"pool"` |
| `.stack_size` | Stack size of each worker in bytes | the platform's |
| `.idle_spin` | How long an idle worker spins before it sleeps in the OS (`std::time::duration`) | `std::time::duration::microseconds(50)` |

- **There is no global pool.** A function that wants to run in parallel takes a `std::thread_pool&` parameter, so you can see from the signature which code uses which threads and how many.
- A pool is `std::shareable`: tasks can capture `[&pool]` and start more tasks in it.
- **Destroying the pool** stops accepting tasks, runs every task already submitted (including detached ones) to the end, and joins the workers.

### 2. Single tasks: `pool.spawn`
Works like `std::thread::spawn` (see [Threads](threads.md)): the same lambda rules and the same choice of handle, just on the pool's threads.

| The lambda... | `pool.spawn(f)` returns |
|---|---|
| owns all its captures | `std::thread_pool::task<R>`: can be stored, returned, `detach()`ed |
| borrows | `std::thread_pool::scoped_task<R>`, a **view**: what it borrows stays borrowed until it is joined or destroyed |

- `join() move` returns the result `R`. Destroying a task handle joins it.
- **A thread that joins a task never sits idle:** while it waits, it runs other tasks from the pool. Nested parallelism (a task that spawns and joins tasks) therefore can't deadlock the pool or leave cores unused.
- **Tasks must not block for long:** a task waiting on I/O, a lock held elsewhere or a channel keeps a worker away from every other task. Blocking work belongs on its own `std::thread`.

### 3. Parallel loops: `for_chunks` and `for_index_chunks`
```rift
pool.for_chunks(data, chunk_size, f);           // data: std::span<T> or std::span<mut T>
pool.for_index_chunks(count, chunk_size, f);    // f(usize from, usize to) over 0..count
```
- The data is split into consecutive, **non-overlapping** chunks of `chunk_size` elements (the last one may be shorter). `f` is called once per chunk, with a `std::span<T>` / `std::span<mut T>` of that chunk, or with its index range `from..to`.
- **The call returns when every chunk is done.** The loop may therefore borrow anything: `data`, and whatever `f` captures by reference.
- `f` runs on several threads at once, so it must be **`std::shareable`**. It may capture copies and `&` references to shareable data, but **not** `mut &` references; mutable data comes in through the chunk it's given. A capture that breaks this is a compile error naming it.
- **The calling thread works too:** it takes chunks like a worker instead of just waiting.
- **The chunk size is always written.** It's the most important performance setting of a parallel loop: too small and the scheduling overhead dominates, too large and some cores run out of work. A rule of thumb is a few thousand simple elements, or `count / (pool.thread_count() * 8)` for uneven work.
- `f` gets whole chunks, not single elements, so its inner loop is ordinary sequential code that the compiler can vectorize.

### 4. Parallel reduction: `reduce_chunks`
```rift
R total = pool.reduce_chunks(data, chunk_size, identity, map, combine);
```
- `map(std::span<T> chunk) → R` computes one result per chunk; `combine(R a, R b) → R` joins two results; `identity` is the result for empty data.
- **The result is deterministic:** chunk boundaries depend only on `chunk_size` (not on the number of threads), and results are combined in chunk order as a fixed binary tree. A floating-point sum gives the same bits on 2 threads and on 64. Unsynchronized parallel sums (or atomics) don't.
- `map` and `combine` must be `std::shareable`, like `f` above.

### 5. How it works (and what it costs)
These are guarantees of the standard library, not just implementation details:

- **Work stealing.** Each worker has its own task queue (a Chase–Lev deque). It pushes and pops its own tasks at one end without contention. An idle worker steals from the other end of someone else's queue. Tasks spawned from outside the pool go into a shared lock-free queue.
- **Loops split recursively.** `for_chunks` cuts the range in half until pieces are one chunk, so idle workers steal *large* pieces first and balance the load with few steals.
- **No allocation in steady state.** `for_chunks` and `reduce_chunks` allocate nothing: their split tasks live on the stacks of the threads running them. `spawn` takes task memory from per-worker free lists; the global allocator is only used while those lists grow.
- **Idle workers** spin for `.idle_spin` (with `std::thread::spin_hint()`), then sleep in the OS until new work arrives. Raise `.idle_spin` for lower latency, lower it to save power.
- The per-worker queue positions are kept on separate cache lines (`std::cache_aligned`, see [Threads](threads.md)) to avoid false sharing.

### 6. Parallel algorithms: `std::parallel`
Ready-made algorithms built on a pool. Each takes the pool as its first argument and returns when it's done:

| Function | Does |
|---|---|
| `std::parallel::sort(pool, items)` | Sorts a `std::span<mut T>` with `<`; not stable |
| `std::parallel::sort_by(pool, items, less)` | The same with a comparison |
| `std::parallel::sort_by_key(pool, items, order, key)` | Stable sort by a key |
| `std::parallel::find(pool, items, predicate) → std::option<usize>` | The **first** index whose element matches (the same result as a sequential search) |
| `std::parallel::any(pool, items, predicate)`, `all(...)` | `bool` |
| `std::parallel::transform(pool, input, output, f)` | `output[i] = f(input[i])` for spans of equal length |

- `pool` is a `std::thread_pool&`. Predicates, keys and `f` must be `std::shareable`, as for `for_chunks`.
- Small inputs (below a few thousand elements) run on the calling thread alone.
- They're free functions in `std::parallel`, not methods of the pool, so the pool's own API stays small: tasks, loops, reductions.

## Examples

### 1. Scaling a buffer in place
```rift
void scale_all(std::thread_pool& pool, mut std::darray<f32>& samples, f32 factor) {
    pool.for_chunks(samples.as_mut_span(), 16 * 1024, [factor](std::span<mut f32> part) {
        for (mut f32& x : part) {
            x *= factor;
        }
    });
}
```
Compare example 2 of 27: no handles, no darray, no OS threads started, and the inner loop vectorizes.

### 2. A deterministic parallel sum
```rift
f64 total(std::thread_pool& pool, std::span<f64> values) {
    return pool.reduce_chunks(values, 8 * 1024, 0.0,
        [](std::span<f64> part) {
            mut f64 sum = 0.0;
            for (f64 v : part) {
                sum += v;
            }
            return sum;
        },
        [](f64 a, f64 b) { return a + b; });
}
```

### 3. Nested tasks: summing a tree
```rift
struct node {
    u64                                value = 0;
    std::option<std::unique_ptr<node>> left  = std::none;
    std::option<std::unique_ptr<node>> right = std::none;
}

u64 tree_sum(std::thread_pool& pool, node& n) {
    std::thread_pool::scoped_task<u64> left = pool.spawn([&pool, &n]() { return child_sum(pool, n.left); });
    u64 right = child_sum(pool, n.right);
    return n.value + left.join() + right;                  // join() runs other tasks while it waits
}

u64 child_sum(std::thread_pool& pool, std::option<std::unique_ptr<node>>& child) {
    return match (child) {
        std::some(c) => tree_sum(pool, *c),
        std::none    => 0,
    };
}
```
- The tasks only *read* the tree, and immutable borrows may overlap, so every level can run in parallel.
- Each task spawns more tasks and joins them. Because a joining thread keeps running other tasks, the recursion never blocks a worker.
- In real code, stop spawning below a certain depth: a task per node is too fine-grained, the same way a chunk size of 1 would be.

### 4. A task that outlives the function
```rift
std::thread_pool::task<std::darray<u8>> start_compress(std::thread_pool& pool, std::darray<u8> input) {
    return pool.spawn([move input]() { return compress(input); });   // owns its data: a normal, returnable task
}
```

### 5. A capture that would race
```rift
mut u64 count = 0;
pool.for_index_chunks(n, 1024, [mut &count](usize from, usize to) { count += to - from; });   // ERROR
```
```
error: the loop body runs on several threads at once, so it can't capture 'count' as 'mut &'
note: count with a std::atomic<u64>, or compute per-chunk results with reduce_chunks
```

## Rationale
- **`std::thread::spawn` and `pool.spawn` both exist** because they have different costs, like a heap and a stack. One starts an OS thread for long or blocking work; the other queues a short task on threads that already exist. Both follow the same rules and return the same kinds of handles, so there's one way to *write* concurrent code and two visible costs to choose from.
- **No global pool:** a hidden global would decide thread counts and stack sizes for every library at once, and make costs invisible (principle 7). Passing `std::thread_pool&` is explicit and lets a program run, say, a real-time pool and a background pool side by side.
- **Chunks instead of single elements:** per-element callbacks prevent vectorization and add a call per element. A chunk keeps the hot loop sequential, which is what the hardware is fastest at.
- **Explicit chunk size:** automatic chunking guesses, and the guess is wrong for uneven work. Writing it keeps the most important tuning knob visible (principle 8).
- **Deterministic reduction:** the same inputs give the same result, whatever the core count. That matters for testing, for reproducing bugs, and for numerical code.
- **Work stealing with helping joins:** the design behind the fastest task schedulers (Cilk, TBB, Rayon). Nested parallelism composes instead of deadlocking.
