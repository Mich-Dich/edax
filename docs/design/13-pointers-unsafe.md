# 13 - Pointers and `unsafe`

## Rules

### Pointer types (all in `std::`)
| Type | Ownership | Can be null? | Copy? | Deref in safe code? |
|---|---|---|---|---|
| `std::unique_ptr<T>` | Sole owner (heap) | **No** | No (move only) | Yes |
| `std::shared_ptr<T>` | Shared owner, reference counted | **No** | No (`.clone()` increments the count) | Yes (read-only, see below) |
| `std::weak_ptr<T>` | Non-owning observer of a `shared_ptr` | - | No (`.clone()`) | Via `.lock()` → `std::option<std::shared_ptr<T>>` |
| `std::raw_ptr<T>` / `std::raw_ptr<mut T>` | None | **Yes** (`nullptr`) | Yes | **No**, only in `unsafe` |

- **`nullptr` exists only for `std::raw_ptr`.** Smart pointers are never null; a "maybe-pointer" is written `std::option<std::unique_ptr<T>>`.
- Creation always says what it costs:
  ```rift
  std::unique_ptr<circle> c = std::unique_ptr<circle>::make(circle{ .radius = 1.0 });  // heap allocation
  std::shared_ptr<config> s = std::shared_ptr<config>::make(load_config());
  std::shared_ptr<config> s2 = s.clone();                                               // count + 1
  ```
- Access: `*p` and `p->member` (the smart pointers define `operator*`, see 16). A `mut std::unique_ptr<T>` allows changes to the pointee. A `std::shared_ptr<T>` gives **read-only** access to its pointee; shared mutation needs a thread-safe wrapper such as `std::shared_ptr<std::mutex<T>>` (see 22).
- Smart pointers to interfaces (`std::unique_ptr<printable>`) are fat pointers (see 07).
- A `std::unique_ptr<circle>` converts implicitly to a `std::unique_ptr<shape>` when `circle` derives from `shape` (likewise `shared_ptr` and `weak_ptr`), like `circle&` → `shape&`. Destroying it runs `circle`'s destructor: the destructor of an `open` class is virtual (see 07).
- `std::shared_ptr<T>::weak()` makes a `std::weak_ptr<T>`; `shared.owners()` is the number of owners.
- The smart pointers are written in Rift in the standard library, on `std::raw` (below).

### Raw pointers
- **Creating, copying and comparing raw pointers is safe.** **Dereferencing them and doing pointer arithmetic is not.**
- `&x` gives `std::raw_ptr<T>`. `mut &x` gives `std::raw_ptr<mut T>` and requires `x` to be `mut`.
- In a condition context a raw pointer converts to `bool` (see 02):
```rift
mut u32 value = 5;
std::raw_ptr<mut u32> p = mut &value;

if (p) {                         // OK: contextual bool
    unsafe {
        *p = 6;                  // deref only inside unsafe
        std::raw_ptr<mut u32> q = p.offset(1);   // pointer arithmetic: unsafe
    }
}
```

### `unsafe`
`unsafe { ... }` is a **per-block** marker. It is never a file or module switch. Only these operations require it:

1. Dereferencing a `std::raw_ptr`, and pointer arithmetic (`.offset(n)`)
2. `cast::reinterpret<T>(x)` (see 17)
3. Calling an `unsafe` function, including all `extern` functions
4. Accessing a `static mut` that isn't a thread-safe type (see 03)

Functions whose callers must follow a contract are declared `unsafe`:
```rift
unsafe u32 read_register(usize address) { ... }
```

### Memory: `std::raw`
The standard library's containers are written in Rift. Their only access to memory is a small
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

`std::raw::array<T, N>` is `N` values of `T` stored inline, the storage of `std::array` and
`std::bitset` (`N` is a compile-time `usize`, e.g. `(N + 63) / 64`). It has no methods; the library
reaches its elements through `cast::reinterpret<std::raw_ptr<T>>(&items)`.

`*p = v` assigns: it destroys the old value first, which is wrong for uninitialized memory, so
containers use `write`.

### FFI: `extern(language)`
`extern` takes the language as a parameter, the same way `run_time(std::build::mode::debug)`
takes a build mode (see 23). The language is an alternative of `std::build::language` (see 25),
written out in full like every std name: `extern(std::build::language::c)`,
`extern(std::build::language::cpp)`. There is no implicit namespace; a shorter name is the
user's choice (an alias, once the language has them, see 22).
```rift
extern(std::build::language::c) i32 puts(std::raw_ptr<u8> text);          // one function

extern(std::build::language::c) {                                         // several at once
    std::raw_ptr<mut u8> malloc(usize size);
    void free(std::raw_ptr<mut u8> p);
}

unsafe {
    i32 written = puts(message.as_ptr());
}
```
- **Without a body**, an `extern(std::build::language::c)` function is **imported**: it is defined in C and is always `unsafe` to call.
- **With a body**, it is **exported**: it is Rift code that C can call under its plain name (`int is_even(int)` for `extern(std::build::language::c) bool is_even(i32 x) { ... }`). Rift code calls it like any function, without `unsafe`. A header can declare it and the source define it, as for any function (see 09).
- An `extern` block applies its modifiers (`private`, `unsafe`) to every declaration inside; it holds only function declarations, and they can have doc comments.
- Their parameters and results are numbers, `bool`, `char`, simple enums and `std::raw_ptr`. Structs are passed through a `std::raw_ptr` (the layout is C's, see 05); passing them by value is not supported yet.
- `text.as_ptr()` gives the bytes of a `std::str` as a `std::raw_ptr<u8>`. A `std::str` is not NUL-terminated; a literal for C is written `c"..."` (see 01), which is.
- `extern(std::build::language::cpp)` (C++ names and calling convention) is reserved: it parses, but the compiler cannot call C++ yet.
- If a foreign function can throw a C++ exception, it must be wrapped so the exception can't cross into Rift. Letting one through aborts the program.

## Rationale
- **Non-null smart pointers:** a null check is either forgotten or redundant. `std::option` makes "maybe absent" part of the type and forces handling. `nullptr` remains only for raw pointers, where C APIs need it.
- **Pointers in `std::`:** they can be written in Rift (with `unsafe` inside), so they're library types, not keywords.
- **`mut &x`:** this is the same marker as call-site `mut` (see 04). Producing a mutable pointer looks like producing a mutable reference.
- **`extern(std::build::language::c)`, not `extern "C"`:** the language is a name the language already has (`std::build::language::c`), not a string the compiler has to recognize. It reads like the other modifiers with a parameter (`run_time(std::build::mode::debug)`), a typo is a compile error instead of an unknown string, and the block form removes the repetition of long C headers.
- **Full names inside the parentheses:** a bare `extern(c)` or `run_time(debug)` would make the compiler look the name up in a namespace the code never mentions. Writing the full name keeps principle 1 (explicit) and the rule that std names are always `std::`-qualified (see 20).
- **Read-only `shared_ptr`:** shared and mutable at once is where data races come from. Requiring a `mutex` (or similar) wrapper makes that cost visible.
