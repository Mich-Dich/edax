# Raw Memory: `std::raw`

The standard library's containers are written in Edax. Their only access to memory is a small
set of compiler intrinsics in `std::raw`; all of them require `unsafe`:

| Intrinsic | Does |
|---|---|
| `std::raw::allocate<T>(n)` | Heap memory for `n` values of `T` (`std::raw_ptr<mut T>`); panics when out of memory |
| `std::raw::reallocate(p, n)` | Grows or shrinks the block to `n` values (it may move) |
| `std::raw::free(p)` | Releases the block. The values in it are **not** destroyed |
| `std::raw::read(p)` | Moves the value out of `*p` (the memory is left as if moved from) |
| `std::raw::write(p, v)` | Moves `v` into `*p` **without** destroying what was there |
| `std::raw::drop(p)` | Destroys the value at `*p` in place |
| `std::raw::copy(to, from, n)` | Copies `n` values bit by bit; the ranges may overlap |
| `std::raw::str_from(p, n)` | The `n` bytes at `p` as a `std::str` (the caller vouches that they are UTF-8) |
| `std::raw::atomic_load(p, order)`, `atomic_store(p, v, order)`, `atomic_exchange(p, v, order)` | Atomic access to an integer, `bool`, `char` or raw pointer at `p` (see [Threads](../concurrency/threads.md)); `p` may be a `std::raw_ptr<T>`: synchronized types change through immutable references |
| `std::raw::atomic_compare_exchange(p, expected, desired, success, failure, weak)` | If `*p == *expected`, stores `desired` and returns `true`; otherwise writes the value found to `*expected` and returns `false` |
| `std::raw::atomic_fetch_add/sub/and/or/xor(p, v, order)` | Read-modify-write (wrapping); returns the old value |
| `std::raw::atomic_fence(order)` | A fence |
| `std::raw::futex_wait(p, expected)`, `futex_wake(p, count)` | Sleep while the `u32` at `p` is `expected`; wake up to `count` sleepers (spurious wake-ups possible). What the library's mutex, condition, channels and thread pool sleep on |
| `std::raw::spin_hint()` | `pause` (x86) / `yield` (ARM) in a busy-wait loop |

A memory order that is not a compile-time value makes the operation sequentially consistent (always correct); the library's checks make every order a constant.

`std::raw::array<T, N>` is `N` values of `T` stored inline, the storage of `std::array` and
`std::bitset` (`N` is a compile-time `usize`, e.g. `(N + 63) / 64`). It has no methods; the library
reaches its elements through `cast::reinterpret<std::raw_ptr<T>>(&items)`.

`*p = v` assigns: it destroys the old value first, which is wrong for uninitialized memory, so
containers use `write`.

## Rationale
- **A small, fixed set of intrinsics:** containers, strings, smart pointers and synchronization are ordinary Edax code. Only what can't be expressed in Edax (allocation, moving bits, atomic instructions, sleeping on an address) is built into the compiler, and every use is inside `unsafe`.
