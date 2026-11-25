# Lesson 12: Performance, data-oriented code, plugins

Edax's safety is checked: every `a[i]` checks its index and every `+` checks for overflow (lessons 1 and 9). That sounds expensive, and it would be if the checks stayed. In practice the optimizer removes most of them, and the language gives you the tools to **see** which ones remain and to **prove** a hot loop free of them. The rest of this lesson covers the engine-level tools: SIMD, dispatch hints, plugins and hot reload.

**You will learn:** why the defaults are already fast, `std::assume`, `@std::no_checks_left`, the `explain-checks` / `insights` / `simd-report` tools, SIMD vector types and `std::simd::blocks`, and the plugin API.

**You will build:** a check-free particle step and a SIMD energy sum (`exercise.ed`).

Examples are in [`examples/`](examples); `python3 course/check.py 12 --examples` checks them.

---

## 1. Why the defaults are already fast

- **No aliasing between parameters.** A `mut T&` is the only way to its object (lesson 2), so the compiler marks every mutable reference parameter `noalias` for LLVM, like C's `restrict` on every pointer. Separate fields are separate places, so `integrate(mut p.x, mut p.y, ...)` hands out four columns of one struct at once, each known not to overlap the others.
- **Moves are `memcpy`.** A `darray` grows by copying bytes, with no per-element move constructor calls.
- **Calling conventions are chosen for you.** Small `T&` parameters go in registers, values of 128 bytes or more through memory.
- **Whole-program devirtualization.** A virtual or interface call with only one possible target becomes a direct call. `@std::dispatch_hint<circle>` before a call adds a guarded direct call for the case you know is common.

## 2. Making checks disappear

The optimizer removes a check when it can prove it can't fail. In a loop that's most of them:

```edax
for (f32 v : xs) { ... }                         // no index at all: nothing to check
for (usize i : 0..xs.len()) { xs[i] += 1.0; }    // i < xs.len() is known: the check goes
```

It can't remove a check it can't prove. Here nothing says `a` and `b` are as long as `out`:

```edax
for (usize i : 0..out.len()) { out[i] = a[i] + b[i]; }   // a[i] and b[i] keep their checks
```

So you tell it, once:

```edax
@std::no_checks_left                  // a build error if any check survives, naming each one
void add_into(mut std::darray<f32>& out, std::darray<f32>& a, std::darray<f32>& b) {
    usize n = out.len();
    std::assume(a.len() == n);        // a fact: checked once per call, then known to the optimizer
    std::assume(b.len() == n);
    for (usize i : 0..n) { out[i] = a[i] + b[i]; }
}
```

- `std::assume(c)` is **not** C++'s `[[assume]]`: it is checked (once, at that point), so a wrong assumption panics instead of causing undefined behaviour. One check per call replaces one per element.
- `@std::no_checks_left` turns "I hope the optimizer removed the checks" into a build error when it didn't, listing each survivor:

  ```
  error[E6004]: 2 run-time checks are left in a '@std::no_checks_left' function
   --> err_no_checks.ed:5:6
    |
  5 | void add_into(mut std::darray<f32>& out, std::darray<f32>& a, std::darray<f32>& b) {
    |      ^^^^^^^^ checks left
    = note: a check at err_no_checks.ed:6:45
    = note: a check at err_no_checks.ed:6:52
    = note: state what the optimizer can't see (std::assume), iterate instead of indexing, or use the unchecked forms in unsafe code
  ```

  This error comes from code generation, so `edaxc check` doesn't report it: build or run.

### Seeing what the compiler does

| Command | Shows |
|---|---|
| `edaxc explain-checks file.ed` | every run-time check in the file and whether the optimizer removes it (as in a release build) |
| `edaxc insights file.ed` | what happens behind your back: heap allocations, drops, moves, copies, devirtualized calls (JSON, for editors) |
| `edaxc simd-report file.ed [function]` | what each vector operation becomes on the target CPU |
| `edaxc emit-llvm file.ed` | the LLVM IR |

```
exercise.ed: 12 run-time checks, 7 removed by the optimizer, 5 kept
  kept     exercise.ed:19:9  index {i} out of bounds (length {this.count})
  removed  exercise.ed:20:9  the checks of std::darray<f32>::[]
  ...
```

Examples: [`no_checks.ed`](examples/no_checks.ed), [`err_no_checks.ed`](examples/err_no_checks.ed).

---

## 3. SIMD

```edax
f32 dot(std::span<f32> a, std::span<f32> b) {
    run_time check (a.len() == b.len()) else "dot: lengths differ";
    mut f32x8 sum = default;
    for (std::simd::block<8> at : std::simd::blocks<8>(a.len())) {   // full blocks, then one masked tail
        sum = a.load(at).mul_add(b.load(at), sum);
    }
    return sum.reduce_add();
}
```

- The vector types are built in: `<number type>x<lanes>`, such as `f32x8`, `i32x4`, `u8x32`. Their operators work lane by lane with the usual rules (overflow is checked per lane), and comparisons give masks.
- They work on **every** CPU level: a vector wider than the hardware is split, a narrower one is native. You write `f32x8` once.
- `std::simd::blocks<8>(n)` walks `0..n` in blocks of 8. The last block is **masked** when `n` isn't a multiple of 8, and `span.load(block)` reads only the valid lanes (zeros elsewhere). There is no scalar remainder loop to write.
- `g.target_cpu(t, std::build::cpu::x86_64_v3)` in `build.ed` compiles a target for a CPU level; `@std::simd::multiversion` makes several copies of a function and picks one at the first call.
- `std::simd::x86::...` and `std::simd::arm::...` are the raw intrinsics, when you need them.

Example: [`simd.ed`](examples/simd.ed).

---

## 4. Plugins and hot reload

```edax
// in the plugin (g.add_plugin in build.ed)
@std::plugin_entry
@std::hot_reload_state                  // its fields survive a reload, matched by name
class game : api::game { ... static game make() { ... } }

// in the program
mut std::plugin<api::game> g = std::plugin<api::game>::load(dir / "game.so")?;
g->update(dt);
g.reload(dir / "game.so")?;             // the new build of the same plugin, state carried over
```

- `load` checks the interface (a hash of it) and the compiler version: a mismatch is an error value, never a crash at the first call.
- On `reload`, a field whose type changed refuses the reload (`state_changed`), and the old build stays loaded.
- `std::filesystem::watch(dir)` tells you when to rebuild. The project in `tests/action/build/reload_project` is a complete example.

---

## C++ → Edax at a glance

| C++ | Edax |
|---|---|
| `__restrict` | every `mut T&` parameter, automatically |
| `[[assume(c)]]` (UB if false) | `std::assume(c)` (checked once) |
| reading the assembly to see if a check is gone | `@std::no_checks_left`, `edaxc explain-checks` |
| `__m256` + intrinsics | `f32x8` and its operators |
| a scalar remainder loop | the masked last block of `std::simd::blocks<N>` |
| `__attribute__((target_clones))` | `@std::simd::multiversion` |
| `dlopen` + `dlsym` | `std::plugin<I>::load` |

---

## Your goal

`exercise.ed` simulates 1003 particles stored as columns (structure of arrays). It doesn't build: `@std::no_checks_left` rejects `integrate` and lists the checks left in it. Make the loop check-free **without removing the annotation**, then write `energy` with `f32x8`, so that the program prints:

```
energy before: 827.35
energy after:  1958.76
first particle at (2.000, 0.002)
```

Then run it with `--mode=release` as well. The output must be the same.

<details>
<summary>Hint 1: which checks survive, and why</summary>

Run `build/compiler/edaxc explain-checks course/12-performance/exercise.ed`. The loop runs `i` over `0..x.len()`, so `x[i]` is proven. `y`, `vx` and `vy` could be shorter than `x` as far as the optimizer knows.
</details>

<details>
<summary>Hint 2: the fix</summary>

`usize n = x.len();`, then `std::assume(y.len() == n);` and the same for `vx` and `vy`, and loop over `0..n`.
</details>

<details>
<summary>Hint 3: <code>energy</code></summary>

Section 3's `dot`, twice in one loop: `sum = a.mul_add(a, b.mul_add(b, sum));` with `f32x8 a = vx.load(at);` and `f32x8 b = vy.load(at);`. Check the lengths first with a `run_time check`, and don't forget the `* 0.5` at the end.
</details>

---

## Try this afterwards

1. Run `edaxc explain-checks course/12-performance/exercise.ed` before and after your fix.
2. Pass the same column twice: `integrate(mut p.x, mut p.x, ...)`.
3. Rewrite the particles as `std::darray<particle>` (an array of structs) and compare the checks left and the generated code (`edaxc emit-llvm`).
4. Write `energy` with a plain loop and compare `edaxc simd-report` for both versions.

<details>
<summary>What happens</summary>

1. Before: 5 checks kept, 3 of them in `integrate` (the indexes of `y`, `vx`, `vy`). After: the indexes in `integrate` are all removed, and the only checks kept there are your three `std::assume`s, which run once per call.
2. `error[E5020]: 'p.x' is passed as a mutable reference and also as another argument`. That rule is what lets the compiler mark the four columns `noalias`.
3. With one darray there's only one length, so no assumptions are needed, but each particle's fields are interleaved in memory, which makes vectorizing the loop harder.
4. The plain loop may or may not be vectorized by LLVM; the `f32x8` version always is, and the report shows what each operation became.
</details>

## Recap

- `mut T&` means `noalias`; moves are `memcpy`; one-target virtual calls become direct.
- Checks the optimizer can't remove: state the facts with `std::assume`, and let `@std::no_checks_left` hold you to it.
- `f32x8` and friends with `std::simd::blocks<N>` for SIMD on any CPU level.
- Plugins load with a checked interface, and reload with their state.

That's the language course. Next comes the [standard-library track](../std/README.md). The full reference is [`docs/design/`](../../docs/design/README.md), and [`ERRORS.md`](../ERRORS.md) collects the errors you met along the way.
