# Raw Pointers and `unsafe`

`std::raw_ptr<T>` is the language's only nullable, unchecked pointer. Everything that can't be checked by the compiler happens inside a visible `unsafe { }` block. The owning smart pointers (`std::unique_ptr`, `std::shared_ptr`, `std::weak_ptr`) are library types written on top of these (see [Smart Pointers](../../stdlib/memory/smart-pointers.md)).

## Rules

### `std::raw_ptr<T>`
| Type | Ownership | Can be null? | Copy? | Deref in safe code? |
|---|---|---|---|---|
| `std::raw_ptr<T>` / `std::raw_ptr<mut T>` | None | **Yes** (`nullptr`) | Yes | **No**, only in `unsafe` |

- **`nullptr` exists only for `std::raw_ptr`.** Every other "maybe absent" value is a `std::option`.
- **Creating, copying and comparing raw pointers is safe.** **Dereferencing them and doing pointer arithmetic is not.**
- `&x` gives `std::raw_ptr<T>`. `mut &x` gives `std::raw_ptr<mut T>` and requires `x` to be `mut`.
- In a condition context a raw pointer converts to `bool` (see [Built-in Types and Conversions](../basics/types.md)):
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
2. `cast::reinterpret<T>(x)` (see [Casts](../basics/casts.md))
3. Calling an `unsafe` function, including all `extern` functions
4. Accessing a `static mut` (see [Variables](../basics/variables.md))
5. Promising thread safety the compiler can't check: `class x : unsafe std::sendable` (see [Threads](../../stdlib/concurrency/threads.md))
6. Calling the memory intrinsics of `std::raw` (see [Raw Memory: `std::raw`](../../stdlib/memory/raw-memory.md))

Functions whose callers must follow a contract are declared `unsafe`:
```rift
unsafe u32 read_register(usize address) { ... }
```

## Rationale
- **`nullptr` only for raw pointers:** C APIs need a null pointer; everything else uses `std::option`, which forces the absent case to be handled.
- **`mut &x`:** this is the same marker as call-site `mut` (see [Functions](../functions/functions.md)). Producing a mutable pointer looks like producing a mutable reference.
