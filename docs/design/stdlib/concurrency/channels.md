# Channels

## Rules

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
| `receiver.receive_for(d) → std::result<T, std::receive_error>` | Waits at most the `std::time::duration` `d`: `error(std::receive_error::timed_out)` after that |
| `receiver.next()` | Same as `receive()`: a receiver is a **stream**, so `for (job j : input) { ... }` runs until the channel is closed and drained (see [Control Flow](../../language/basics/control-flow.md)) |

All of them work on immutable ends, because channels are synchronized types (see [Threads](threads.md)).

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

## Examples

### Producer and consumers with a channel
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

## Rationale
- **`std::channel<T>` as a separate object:** creating a channel gives the queue a name. Its ends are made from it, so there is no need to return two values at once, and closing is something you *do* (`close()`), not something that happens when the last handle disappears in some other thread.
- **Bounded by default in examples, unbounded by name:** a channel that can grow without limit says so in its constructor, so unbounded memory use is never an accident.
