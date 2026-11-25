# Std lesson 1: Core utilities

The smallest pieces of `std` are the ones you use everywhere: moving values out of places, arithmetic that says what happens on overflow, bit tricks, ordering and hashing, and a few small types that make a program's intent visible in its types. C++ spreads these over `<utility>`, `<numeric>`, `<bit>`, `<compare>` and `<functional>`; in Edax they are all in `std::` after `import std;`.

**You will learn:** `std::take` / `replace` / `swap`, `checked_*` and `saturating_*` arithmetic, the bit functions, `std::ordering` and `<=>`, the structural interfaces `std::comparable` and `std::hashable`, `std::version`, `std::nonzero`, the 8-bit floats and `std::exit_code`.

**You will build:** a small resource manager (`exercise.ed`).

Examples are in [`examples/`](examples); `python3 course/check.py s1 --examples` checks them.

---

## 1. Ownership helpers

| Function | Does | C++ |
|---|---|---|
| `std::take(mut place)` | returns the value, leaves the type's `default` behind | `std::exchange(x, {})` |
| `std::replace(mut place, value)` | returns the old value, stores `value` | `std::exchange(x, value)` |
| `std::swap(mut a, mut b)` | exchanges two values | `std::swap(a, b)` |

Language lesson 2 explained why you need them: `move` only works on locals and by-value parameters, because moving a field out would leave a hole in its object. These three always leave the place holding a complete value, so the object's destructor stays correct.

```edax
mut slot s = slot{ .label = std::string::from("first") };
std::string old = std::take(mut s.label);                                    // "first"; s.label is ""
std::string before = std::replace(mut s.label, std::string::from("second"));   // ""; s.label is "second"
```

`std::take` needs a type with a `default`. For one without (a `std::file`, a `unique_ptr`), use `std::replace` with the value that should stay, or wrap the field in an `std::option` and take that.

Example: [`ownership_helpers.ed`](examples/ownership_helpers.ed).

## 2. Arithmetic that says what happens on overflow

Plain `+ - *` panic on overflow in every build mode (language lesson 1). When overflow is expected, the name of the operation says what happens:

| Write | On overflow | Use for |
|---|---|---|
| `a + b` | panic | normal arithmetic: overflow is a bug |
| `std::checked_add(a, b)` (`_sub`, `_mul`) | `std::none` | budgets, sizes from outside: overflow is a case to handle |
| `std::saturating_add(a, b)` (`_sub`, `_mul`) | clamps to `T::min` / `T::max` | levels, audio samples, colours |
| `a +% b`, `-%`, `*%` | wraps around | hashes, checksums, ring counters |

```edax
std::checked_add(250u8, 10u8)       // none
std::saturating_sub(5u8, 10u8)      // 0
std::saturating_mul(-100i8, 2i8)    // -128
5u8 -% 10                           // 251
```

The checked form turns a panic into a value you can `match` on, or pass up with `.or_error(e)?` (language lesson 4).

Example: [`arithmetic.ed`](examples/arithmetic.ed).

## 3. Bits

| Function | Gives |
|---|---|
| `std::count_ones(x)` | how many bits are set (`popcount`) |
| `std::trailing_zeros(x)` | the index of the lowest set bit (the type's width for 0: 32 for a `u32`) |
| `std::leading_zeros(x)` | zeros above the highest set bit |
| `std::rotate_left(x, n)`, `std::rotate_right(x, n)` | rotation |
| `bit(n)` (language) | `1 << n` in the type the context asks for |

They work on any integer type and compile to one instruction each where the CPU has it. All three counting functions return `u32`. The format spec `{x:b}` prints binary, which helps when you play with them.

A set of flags is just an unsigned integer with named bits:

```edax
compile_time u32 read = bit(0);
compile_time u32 write = bit(1);
u32 user = read | write;
bool can_write = user & write != 0;       // & binds tighter than != in Edax
u32 without_read = user & ~read;
```

Example: [`bits.ed`](examples/bits.ed).

## 4. Ordering and hashing

**`<=>` returns a `std::ordering`**: `less`, `equal` or `greater`. Numbers, `char` and strings have it built in. A type of yours gets `<`, `<=`, `>`, `>=` by writing `operator<=>`, and `==` by asking for it:

```edax
struct version_tag {
    u32 major = 0;
    u32 minor = 0;

    std::ordering operator<=>(version_tag& other) {
        if (major != other.major) { return major <=> other.major; }
        return minor <=> other.minor;
    }
    bool operator==(version_tag& other) = default;
}
```

Swapping the operands, `other.priority <=> priority`, reverses the order: that's how you sort "highest first" without a separate comparator.

**Hashing.** A type is `std::hashable` when it has a `u64 hash()` method. Integers, `char`, `bool` and the string types already are. Build yours from its fields:

```edax
u64 hash() { return std::hash_combine(std::hash(major), std::hash(minor)); }
```

Keys that are `==` must have the same hash. The hash is seeded per process, so hash values (and `hash_map` iteration order, std lesson 3) differ between runs. Never store them.

**Structural interfaces.** `std::comparable`, `std::hashable`, `std::cloneable` and `std::formattable` are satisfied by *having* the operation; you never write them in a base list. As soon as `version_tag` has `<=>`, a `template<type T : std::comparable>` accepts it, and `darray<version_tag>::sort()` works.

**`std::version`** is three `u32`s, `major`, `minor`, `patch`, with comparison and formatting (`1.4.320`). `v.at_least(other)` is `compile_time run_time`, so a `compile_time check` can require a version.

Example: [`ordering.ed`](examples/ordering.ed).

## 5. Small types with a purpose

| Type | What for |
|---|---|
| `std::nonzero<T>` | an integer that is never 0. `std::option<std::nonzero<u32>>` is 4 bytes: 0 means `none`. Ideal for ids. `make(v)` checks, `of(v)` gives an option, `get()` reads |
| `std::f8e4m3`, `std::f8e5m2` | 8-bit floats for storage (weights, compressed attributes). `from_f32` rounds, `saturate` clamps, `to_f32` is exact. Compute in `f32` |
| `std::exit_code` | `success` / `failure`, as the return type of `main` instead of a magic `i32` |
| `std::assume(c)`, `std::unreachable()` | facts for the optimizer, checked in safe modes (language lesson 12) |

Example: [`small_types.ed`](examples/small_types.ed).

---

## C++ → Edax at a glance

| C++ | Edax |
|---|---|
| `std::exchange(x, {})` | `std::take(mut x)` |
| `std::exchange(x, v)` | `std::replace(mut x, v)` |
| `__builtin_add_overflow` | `std::checked_add(a, b)` |
| hand-written clamp after widening | `std::saturating_add(a, b)` |
| unsigned wrap-around | `a +% b` |
| `std::popcount`, `std::countr_zero`, `std::countl_zero` | `std::count_ones`, `std::trailing_zeros`, `std::leading_zeros` |
| `std::rotl` | `std::rotate_left` |
| `std::strong_ordering` | `std::ordering` |
| `std::hash<T>` specialization | a `u64 hash()` method |
| `boost::hash_combine` | `std::hash_combine` |
| `EXIT_SUCCESS` | `std::exit_code::success` |

---

## Your goal

`exercise.ed` manages a budget, permission flags and a list of resources. Fill in the TODOs so that it prints:

```
spend 40: true, left 60
spend 50: true, left 10
spend 30: false, left 10
spend 10: true, left 0
user has 2 permissions, root has 4
lowest of root without read: write, of nothing: none
order: cpu:3, gpu:3, disk:1
same hash for equal keys: true
draft -> final, left ''
level +10: saturated 255, wrapped 4
```

<details>
<summary>Hint 1: <code>spend</code></summary>

`if (std::checked_sub(budget, cost) is std::some(left)) { budget = left; return true; }`, else `false`. The budget stays unchanged when the cost is too high.
</details>

<details>
<summary>Hint 2: <code>lowest</code></summary>

0 has no lowest bit, so handle it first. Otherwise `match (std::trailing_zeros(flags))` on the bit indexes 0, 1, 2 and 7, with `_ => "unknown"`.
</details>

<details>
<summary>Hint 3: the order</summary>

`if (priority != other.priority) { return other.priority <=> priority; }` puts higher priorities first; then `return name <=> other.name;`.
</details>

---

## Try this afterwards

1. Call `std::take(mut current.priority)`. What does `priority` hold afterwards?
2. Remove `operator<=>` from `resource`. Which line stops compiling?
3. Remove the `hash()` method and put resources in a `std::hash_map<resource, u32>`.
4. Print `std::size_of<std::option<u32>>()` next to `std::size_of<std::option<std::nonzero<u32>>>()`.

<details>
<summary>What happens</summary>

1. `0`: the default of `u8`.
2. `all.sort()`: `error[E4214]: 'std::darray<resource>::sort' does not compile for this instance`, with the note `operator '<' is not defined for 'resource'`. `<` comes from `<=>`.
3. The first thing that needs a hash fails: `error[E4070]: 'resource' has no method 'hash'`, reported inside the library's `std::hash` with a note pointing at your call. A `hash_map` key must be `std::hashable`.
4. `8` and `4`: the plain option needs a tag next to the `u32`; the `nonzero` one uses 0 as `none`.
</details>

## Recap

- `take`, `replace` and `swap` move values through places that must stay whole.
- The arithmetic function's name says what overflow does: panic, `checked_`, `saturating_`, or `%` to wrap.
- `<=>` gives ordering, `hash()` gives hashing, and the std interfaces are satisfied structurally.
- `nonzero`, the 8-bit floats, `version` and `exit_code` put intent into types.

Next: [std lesson 2](../02-sequences/README.md), sequences in depth.
