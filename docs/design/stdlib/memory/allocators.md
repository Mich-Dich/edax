# Allocators

Every type that allocates takes its memory from an **allocator**. The default is the global allocator; a container can be given another one as a template argument, and a build target can replace the global allocator.

## Rules

### The allocator interface
```rift
namespace std {
    interface allocator {
        std::raw_ptr<mut u8> allocate(usize size, usize alignment) mut;      // nullptr when out of memory
        unsafe void free(std::raw_ptr<mut u8> block, usize size, usize alignment) mut;
        unsafe std::raw_ptr<mut u8> reallocate(std::raw_ptr<mut u8> block, usize old_size, usize new_size, usize alignment) mut;
    }
}
```
- The methods are `unsafe`: the caller vouches for the sizes and alignments. Only container code calls them.
- `reallocate` has a default body (allocate, copy, free; see [Inheritance and Interfaces](../../language/user-types/inheritance-and-interfaces.md)); allocators that can grow in place override it.
- A container that gets `nullptr` panics with "out of memory".

### Containers take an allocator parameter
Every allocating type of the standard library has a last template parameter **`A = std::global_allocator`**:
```rift
std::darray<u32>                          // the global allocator
std::darray<u32, std::arena_allocator>    // memory from an arena
std::hash_map<std::str, u32, std::arena_allocator>
std::unique_ptr<mesh, std::arena_allocator>
```
- The container stores its allocator value. `std::global_allocator` is an empty struct, so the default costs no space.
- A container with another allocator is another type: `std::darray<u32>` and `std::darray<u32, std::arena_allocator>` don't convert into each other.
- Containers with a non-default allocator are created with `T::with_allocator(alloc)` (and `with_capacity_in(n, alloc)`); `default` only works when `A` has a default.
- `std::string` is `std::string_with<std::global_allocator>`; text in another allocator is a `std::string_with<A>`.

### Allocators in the standard library
| Allocator | Memory from | Freed |
|---|---|---|
| `std::global_allocator` | The program's global allocator (below) | Per block |
| `std::arena_allocator` | An `std::arena`: one large block, handed out front to back | All at once, when the arena is destroyed or `reset()` |

```rift
std::arena frame_memory = std::arena::make(1024 * 1024);                   // one heap block of 1 MiB
mut std::darray<vertex, std::arena_allocator> verts =
    std::darray<vertex, std::arena_allocator>::with_allocator(frame_memory.allocator());
```
- `arena.allocator()` returns a handle that borrows the arena: it is a **view** (see [Ownership, Copies, Moves and References](../../language/memory/ownership-and-references.md)), so every container using it is a view too and can't outlive the arena.
- `free` on an arena does nothing; `arena.reset() mut` makes all of its memory available again (and is an error while containers still borrow it).

### Replacing the global allocator
`build.rifts` can give a target another global allocator:
```rift
g.set_global_allocator(app, std::sealable_allocator);
```
- The type implements `std::allocator`, is `std::shareable` (every thread uses it) and has a `default`.
- `std::sealable_allocator` is the global allocator plus a switch: after `std::seal_heap()`, every allocation panics with its location. Embedded and real-time programs call `std::seal_heap()` at the end of start-up, so "no allocation after start-up" is checked on every run.
- `std::no_allocator` panics on the first allocation, for programs that must never use the heap.
- On targets without the `heap` capability (see [Rift as a Build System](../../build/build-system.md)), only the types with a non-default allocator can allocate.

## Rationale
- **A template parameter, not a stored pointer:** the default costs nothing and calls are direct. Using another allocator is visible in the type, like every other cost.
- **Arenas as views:** the borrow rules that already protect references make sure no container outlives its arena, with no extra checks.
- **A replaceable global allocator with a seal:** real-time code needs "no allocation after start-up" for the whole program, including libraries it doesn't control. Checking it in the allocator catches every allocation, wherever it comes from.
