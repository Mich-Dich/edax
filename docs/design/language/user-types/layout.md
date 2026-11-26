# Memory Layout

How structs, classes and enums are laid out, and the tools that control it: alignment, explicit padding, packed structs, bit casts, unions and enum tags. Engine code shares memory with GPUs, files and the network, so the layout of a type is part of its contract.

## Rules

### 1. The default layout
- A struct or class keeps its fields **in declaration order**, each at the next offset aligned for its type (C's layout, see [C and C++ Interop](../interop/c-interop.md)). A class with a base has the base object first; a class with `virtual` methods has the vtable pointer first.
- The size is rounded up to the alignment. An empty struct takes no space.
- The compiler never reorders fields. Padding that the order creates is visible with `std::meta::fields<T>()` offsets and, on request, as errors (below).

### 2. `@std::layout`
```edax
@std::layout{ .align = 16 }
struct vec4 { f32 x; f32 y; f32 z; f32 w; }

@std::layout{ .no_implicit_padding = true }
struct gpu_light {
    vec3            position;
    std::padding<4> _pad0;
    vec3            color;
    f32             intensity;
}
```
| Field | Meaning |
|---|---|
| `.align = N` | The type's alignment is at least `N`, a power of two up to 128, written as a number or as `std::cache_line_size` (the target's cache line). The size is rounded up to it. Stack values, container elements and heap blocks all honour it |
| `.no_implicit_padding = true` | Every gap the compiler would insert between fields or at the end is an error that names the gap and suggests the `std::padding<N>` to write |
| `.packed = true` | No padding at all: every field at the next byte |

- `@std::layout` is written on structs and classes, not on enums.
- `std::padding<N>` is `N` bytes that are zero when the value is built and are never read. A padding field is never written in a struct literal.

### 3. Packed structs
- A field of a packed struct may be misaligned, so it **can't be borrowed**: `&h.size`, `mut h.size` as an argument and calling a method on it are errors. It can be read (a copy) and assigned.
- Packed structs are for file and network formats. Data that code works on stays aligned.

### 4. `std::bit_cast<T>(x)`
The bits of `x` as a `T` of the same size. It is **safe** when both types are *plain bits*:

| Plain bits | Not plain bits |
|---|---|
| Integers, floats, SIMD number vectors | `bool`, `char` (they have invalid bit patterns) |
| Arrays, tuples and structs of plain bits **without padding** | Pointers, references, classes, enums |

```edax
u32  bits  = std::bit_cast<u32>(1.0f32);            // 0x3f800000
rgba color = std::bit_cast<rgba>(0x04030201u32);    // struct rgba { u8 r; u8 g; u8 b; u8 a; }
u64  wide  = std::bit_cast<u64>(bits);              // ERROR: different sizes
```
- A type with padding is not plain bits: its padding bytes have no value. `std::padding<N>` fields count as plain bits.
- Anything else is `cast::reinterpret` in `unsafe` code (see [Casts](../basics/casts.md)).

### 5. Unions
- `union` is a type whose members share memory, with the size and alignment of the largest. Writing a member is safe; reading one is `unsafe` (the compiler can't know which member was written last).
- `union bits { u32 word; f32 number; }`. Members are plain data (copy types, no destructor) without defaults: a union doesn't know which member it holds, so it can't destroy one.
- A literal sets exactly one member: `bits{ .number = 1.0 }`; the bytes past it are zero. `u.word = 5;` is safe (it needs a `mut` union); any other use of a member, including `u.word += 1`, reads it and needs `unsafe`.
- `bool operator==(bits& other) = default;` compares the bytes. Unions are run-time values only (their bytes depend on the target's layout).
- Unions are for C interop and hand-written storage. A value that is one of several things is a payload enum (see [Enums](enums.md)).

### 6. Enum layout and niches
- A simple enum is its tag. An enum with payloads is a tag plus storage for the largest payload.
- `enum kind : u8 { ... }` fixes the tag type, which file formats need.
- **Niches:** `std::option<T>` takes no extra space when `T` has bit patterns that are never valid: smart pointers and references (never null), `bool`, `char`, enums with unused tag values, and `std::nonzero<u32>` / `std::nonzero<u64>` (an integer that is never 0). `std::option<std::unique_ptr<T>>` is one pointer; `std::option<entity_handle>` with a `nonzero` generation is 8 bytes.
- The rule: an enum with two alternatives, one carrying a payload with a **niche** and one carrying nothing, has no tag. The empty alternative is stored as the niche's value. Niches:
  - references (`T&`, the object of an interface reference) and fields marked `@std::never_zero` (integers and pointers; `std::unique_ptr`, `std::shared_ptr` and `std::nonzero` use it): 0;
  - `bool`: 2; `char`: `0x110000`;
  - an enum with a tag: a tag value no alternative uses (so `std::option<std::option<u32>>` is 8 bytes, the inner option's tag says which);
  - a struct or class: the first niche of its fields.
- An enum stored without a tag has no niche left: an option of it gets a tag.
- `@std::never_zero` is a promise the compiler doesn't check: a value with 0 there reads as the empty alternative. Code that can't keep it uses `std::nonzero<T>`, whose `make` checks.

### 7. Layout checks in code
Everything about a layout is a compile-time value, so checks can state it (see [Checks](../errors/checks.md)):
```edax
compile_time check (std::size_of<gpu_light>() == 32) else "gpu_light must match the shader's struct";
compile_time check (std::meta::fields<gpu_light>()[2].offset == 16) else "color at 16 (std140)";
```
A library annotation such as `@gpu::std140` is ordinary Edax: it walks `std::meta::fields<T>()` and reports every offset that doesn't follow the GPU rules.

## Rationale
- **No field reordering:** reordering saves bytes but breaks every format the type is shared with, silently. `.no_implicit_padding` shows the cost instead, and the programmer decides.
- **Explicit padding as a type:** "no hidden state" stays true for GPU and file structs; the padding is in the source, zeroed and never read.
- **Safe `bit_cast`:** most reinterpretations in engine code are between numbers and plain structs. Checking "plain bits" at compile time makes them safe; what remains is visibly `unsafe`.
- **Niches:** `option` of a handle or a pointer is everywhere in engines. Without niches every one would cost a tag and padding.
