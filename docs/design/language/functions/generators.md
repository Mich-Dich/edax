# Generators and Resumable Functions

> **Planned:** nothing on this page is implemented yet. Generators, resumable functions and `std::raw::stack_switch` are lowered on the mid-level IR, which edaxc doesn't have yet.

A **generator** produces a sequence of values, one per `yield`, and keeps its state between them. A **resumable function** can stop at an `await` and continue later. Both are ordinary values with a size known at compile time: they never allocate unless the program says so.

## Rules

### 1. Generators
```edax
generator<u32> evens(u32 limit) {
    for (u32 i : 0..limit) {
        if (i % 2 == 0) {
            yield i;
        }
    }
}

for (u32 n : evens(10)) {
    std::println(f"{n}");
}
```
- `generator<T>` as the return type makes the function a generator. `yield value;` hands out one value; reaching the end (or `return;`) ends the sequence.
- Calling it runs nothing: it returns a **value of an unnamed type** that holds the function's state (its parameters and the locals alive across a `yield`) inline. Like a lambda, it is stored with `auto` or passed to a template.
- The value is a stream: `next() mut` returns `std::option<T>`, and `for` walks it.
- A generator with reference parameters or view captures is a **view** (see [Ownership](../memory/ownership-and-references.md)): it can't outlive what it borrows.
- `std::boxed_generator<T>` erases the type and puts the state on the heap (one visible allocation), for storing generators of different functions in one container.
- A generator that calls itself needs a box: its state would contain itself. The error says so.

### 2. Resumable functions
```edax
resumable<texture> load_texture(std::filesystem::path p, mut io::reader& r) {
    std::darray<u8> bytes = await r.read_all(p);       // stops here until the read is done
    return texture::decode(bytes);
}
```
- `resumable<T>` as the return type, and `await x` where `x` implements `std::awaitable<U>`:
  ```edax
  interface awaitable<U> {
      std::option<U> poll(mut std::waker& w) mut;     // none: not ready; call w.wake() when it is
  }
  ```
- Like a generator, calling it returns a value holding its state inline, and it does nothing until polled. A `resumable<T>` value is itself an `awaitable<T>`.
- **No built-in executor.** The game loop, a thread pool or a script scheduler polls resumables. Nothing runs that the program didn't call.
- `await` is allowed only in resumable functions. Locals that live across an `await` follow the same rules as captures of a stored lambda: a reference held across an `await` makes the resumable a view.

### 3. Fibers
Fibers (stackful tasks for job systems) are library code on one intrinsic:
- `std::raw::stack_switch(mut from, to)` (`unsafe`) saves the registers into `from` and continues on `to`'s stack. Stacks come from an allocator with a guard page.
- Functions that may switch fibers are marked `@std::may_switch_fiber`; the compiler doesn't keep a `thread_local` address in a register across a call to them. The library's job system marks its wait functions.

## Rationale
- **State inline, size known:** C++ coroutines allocate their frame on the heap unless the optimizer removes it. In Edax the frame is part of the value, like a lambda's captures, so the cost is visible and usually zero.
- **Borrow rules for frames:** a generator that borrows is a view, so a frame can't outlive what it points to. That is the classic dangling-coroutine bug of C++.
- **No executor in the language:** engines schedule work their own way (per frame, per job system, per script). A built-in runtime would be a hidden cost and a second way to do it.
- **Generators replace iterator classes:** most hand-written iterators are a loop with a cursor. A generator is that loop, written once.
