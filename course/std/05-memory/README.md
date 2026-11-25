# Std lesson 5: Smart pointers and allocators

Most Edax code never touches the heap directly: containers and strings do it for you. When you do need an object on the heap (for polymorphism, for sharing, for a recursive structure), there are three smart pointers, and none of them can be null. When the *global* heap is the wrong place, every allocating type takes an allocator as a template parameter, and the standard library brings an arena.

**You will learn:** `std::unique_ptr`, `std::shared_ptr` and `std::weak_ptr`, why a shared pointer is read-only and what to do instead, `std::option` as the nullable pointer, `std::arena` with `std::arena_allocator`, `std::string_with<A>`, and replacing the global allocator.

**You will build:** a texture cache with weak pointers and a per-frame arena (`exercise.ed`).

Examples are in [`examples/`](examples); `python3 course/check.py s5 --examples` checks them.

---

## 1. The three smart pointers

| Type | Owns | Copy? | Access | C++ |
|---|---|---|---|---|
| `std::unique_ptr<T>` | alone | no, moves | read, and write through a `mut` variable | `std::unique_ptr<T>` |
| `std::shared_ptr<T>` | shared, counted (atomically) | `.clone()` adds an owner | **read-only** | `std::shared_ptr<const T>` |
| `std::weak_ptr<T>` | nothing: observes a `shared_ptr` | `.clone()` | `.lock()` gives `std::option<std::shared_ptr<T>>` | `std::weak_ptr<T>` |

- **Never null.** `make` is the only way to create one, and there's no `reset()` to null. "Maybe a pointer" is `std::option<std::unique_ptr<T>>`, so the type tells you whether to check.
- Creation says what it costs: `std::unique_ptr<circle>::make(circle{ ... })` allocates; `shared.clone()` only increments a count.
- `*p` and `p->member` work as in C++. A `unique_ptr<circle>` converts to `unique_ptr<shape>` (an upcast), and destroying it runs `circle`'s destructor (destructors of `open` classes are virtual).
- `s.owners()` is the number of `shared_ptr`s to the value; `s.weak()` makes a `weak_ptr`.

```edax
std::shared_ptr<config> a = std::shared_ptr<config>::make(config{ ... });
std::shared_ptr<config> b = a.clone();                 // 2 owners, one config
std::weak_ptr<config> watcher = a.weak();
if (watcher.lock() is std::some(p)) { ... }            // none once the last owner is gone
```

A recursive structure is just an option of a pointer:

```edax
class node {
public:
    u32 value = 0;
    std::option<std::unique_ptr<node>> next = std::none;
}
```

Examples: [`unique.ed`](examples/unique.ed), [`shared.ed`](examples/shared.ed).

## 2. Shared means read-only

```edax
std::shared_ptr<counter> c = std::shared_ptr<counter>::make(counter{});
c->n += 1;      // error[E4020]: cannot assign to 'n': it is not mutable
```

Shared *and* mutable is where data races and "who changed this?" bugs come from, so a `shared_ptr` only lends `T&`. When several owners really need to change one value, put a synchronized type behind the pointer, the same way you would between threads (std lesson 12):

```edax
std::shared_ptr<std::mutex<counter>> c = std::shared_ptr<std::mutex<counter>>::make(std::mutex<counter>::make(counter{}));
{
    mut std::mutex_guard<counter> g = c->lock();
    g->n += 1;
}
```

There is only one `shared_ptr`, and its count is always atomic: the increment is cheap next to the allocation that made the pointer, and a second, non-atomic type would be a second way to do the same thing.

Examples: [`err_shared.ed`](examples/err_shared.ed), [`shared_mutable.ed`](examples/shared_mutable.ed).

## 3. Weak pointers: caches and observers

A `weak_ptr` remembers a value without keeping it alive. The classic use is a cache: it hands out `shared_ptr`s, keeps only `weak_ptr`s itself, and the value is freed as soon as its last user lets go. The next request finds the weak pointer expired (`lock()` gives `none`) and loads it again. That's exactly the exercise.

## 4. Allocators and arenas

Every allocating type has a last template parameter `A = std::global_allocator`:

```edax
std::darray<u32>                              // the global allocator
std::darray<u32, std::arena_allocator>        // memory from an arena
std::string_with<std::arena_allocator>        // text in an arena
std::unique_ptr<mesh, std::arena_allocator>
```

An **arena** is one block of memory handed out front to back. Freeing a single allocation does nothing; `arena.reset()` makes the whole block available again. That's ideal for memory that lives exactly one frame, one request or one parse: no per-object `free`, no fragmentation.

```edax
mut std::arena frame = std::arena::make(64 * 1024);
{
    mut std::darray<vertex, std::arena_allocator> verts =
        std::darray<vertex, std::arena_allocator>::with_allocator(frame.allocator());
    ...
}
frame.reset();
```

**`frame.allocator()` borrows the arena.** The allocator handle is a view, so every container using it is a view too (language lesson 2): it can't outlive the arena, and the arena can't be reset while one exists:

```
error[E5022]: cannot call mut method 'reset' on 'frame' while 'xs' borrows it
  = note: the borrow lasts until 'xs' is destroyed or moved: put it in a block of its own
```

In C++, resetting an arena under a live container is a use-after-free that shows up much later. Here the same borrow rules that protect references protect the arena, with no run-time cost.

- Containers with another allocator are another type: `std::darray<u32>` and `std::darray<u32, std::arena_allocator>` don't convert.
- They are created with `T::with_allocator(alloc)` (or `with_capacity_in(n, alloc)`); `default` only works when the allocator has a default.
- `build.ed` can replace the **global** allocator of a target: `g.set_global_allocator(app, std::sealable_allocator)`. After `std::seal_heap()`, every allocation panics with its location, which is how real-time code checks "no allocation after start-up" on every run. `std::no_allocator` forbids the heap entirely.

Examples: [`arena.ed`](examples/arena.ed), [`err_arena.ed`](examples/err_arena.ed).

---

## C++ → Edax at a glance

| C++ | Edax |
|---|---|
| `std::make_unique<T>(...)` | `std::unique_ptr<T>::make(T{ ... })` |
| `std::unique_ptr<T> p = nullptr;` | `std::option<std::unique_ptr<T>> p = std::none;` |
| `std::make_shared<T>(...)` | `std::shared_ptr<T>::make(T{ ... })` |
| `std::shared_ptr<T> b = a;` | `std::shared_ptr<T> b = a.clone();` |
| `a.use_count()` | `a.owners()` |
| `std::weak_ptr<T> w = a; w.lock()` | `a.weak()`, `w.lock()` (an option) |
| shared mutable state | `std::shared_ptr<std::mutex<T>>` |
| `std::pmr::monotonic_buffer_resource` | `std::arena` + `std::arena_allocator` |
| `std::pmr::vector<T>` | `std::darray<T, std::arena_allocator>` |

---

## Your goal

`exercise.ed` has a texture cache that hands out `shared_ptr`s and remembers `weak_ptr`s, sprites that share textures, and a function that uses an arena for scratch memory. Fill in `cache::get` and `unique_bytes` so that it prints:

```
grass has 2 owners
frame 1: 9216 bytes
freed rock
frame 2: 9216 bytes
freed hero
freed grass
loads 3, hits 1
loads 4, hits 1
freed rock
```

Read the output as a story: the second `grass` is a cache hit; removing the only sprite with `rock` frees it at once; at the end of the block every sprite is gone, so every texture is freed; asking for `rock` again finds an expired weak pointer and loads it a fourth time.

<details>
<summary>Hint 1: <code>cache::get</code></summary>

Walk `names` by index. On a match: `if (entries[i].lock() is std::some(alive)) { hits += 1; return alive; }`; otherwise `load` it, store `fresh.weak()` in `entries[i]` and return it. No match: `load`, then push the name (`std::string::from(name)`) and `fresh.weak()`. Compare a `std::string` with a `std::str` through `names[i].as_str() == name`.
</details>

<details>
<summary>Hint 2: <code>unique_bytes</code></summary>

`mut std::darray<std::str, std::arena_allocator> seen = std::darray<std::str, std::arena_allocator>::with_allocator(frame.allocator());`, then for each sprite: `if (!seen.contains(name)) { seen.push(name); total += s.image->size; }`.
</details>

---

## Try this afterwards

1. Make `entries` a `std::darray<std::shared_ptr<texture>>` (store `fresh.clone()`). When are textures freed now?
2. In `main`, call `frame.reset()` inside `unique_bytes`, after the loop.
3. Return `seen` from `unique_bytes` (change its return type).
4. Write `std::shared_ptr<texture> t = textures.get("x"); t->size = 1;`.

<details>
<summary>What happens</summary>

1. Only at the very end: the cache owns every texture, so "freed rock" no longer appears after the sprite is removed, and the last `get("rock")` is a hit. A cache of `shared_ptr`s is a memory leak with extra steps.
2. `error[E5022]`: `seen` borrows `frame` until the end of the function. Resetting must wait until the scratch data is gone, which is what `main` does.
3. `error[E5012]: 'unique_bytes' returns 'std::darray<std::str, std::arena_allocator>', which could borrow from 'sprites' or 'frame'`. The darray is a view of the arena *and* of the sprites' names, so there's no single source to tie it to. Scratch memory stays scratch.
4. `error[E4020]`: the shared texture is read-only.
</details>

## Recap

- `unique_ptr` owns alone, `shared_ptr` shares read-only, `weak_ptr` observes; none is ever null, `std::option` says "maybe".
- Shared and mutable: `shared_ptr<mutex<T>>`.
- Allocators are template parameters; an arena hands out a block and frees it at once, and the borrow rules keep containers from outliving or surviving a reset of it.

Next: [std lesson 6](../06-math/README.md), vectors, rectangles and colours.
