# Lesson 12: Performance, data-oriented code, plugins

Safety in Edax is checked, and the checks cost something unless the optimizer can prove them
away. This lesson covers the tools for writing hot loops that end up with no checks, and the
engine-level features: SIMD, dispatch hints and hot reload.

## Why the defaults are already fast

- **No aliasing between parameters:** a `mut T&` is the only reference to its object (lesson 2),
  so every mutable reference parameter is `noalias` to LLVM, like `restrict` everywhere.
  Separate fields are separate places, so `integrate(mut p.x, mut p.y, ...)` hands out four
  columns of one struct at once.
- **Moves are `memcpy`**, and containers grow without per-element move constructors.
- Small `T&` parameters are passed in registers; values of 128 bytes or more go through memory.
- **Whole-program devirtualization:** a virtual or interface call with one possible target
  becomes a direct call. `@std::dispatch_hint<circle>` before a call adds a guarded direct call
  for the common case.

## Making checks disappear

Every `a[i]` and every `+` is checked (lessons 1 and 9). In a loop the optimizer removes most of
those checks: `for (x : xs)` and `for (i : 0..xs.len())` with `xs[i]` are both free. It can't
remove a check it can't prove. In `y[i]` inside `for (i : 0..x.len())`, nothing tells it that `y`
is as long as `x`.

```edax
@std::no_checks_left                  // a build error if any check survives, naming each one
void integrate(mut std::darray<f32>& x, mut std::darray<f32>& y, ...) {
    usize n = x.len();
    std::assume(y.len() == n);        // a fact: checked once at run time, then known to the optimizer
    for (usize i : 0..n) { ... }
}
```

- `std::assume(c)` costs one check per call, instead of one per element.
- `edaxc explain-checks file.ed` lists every check in a file and whether the optimizer removes it.
- `edaxc insights file.ed` lists what the compiler does behind your back: heap allocations,
  drops, moves, copies, devirtualized calls. The output is JSON, for editors.

## SIMD

```edax
mut f32x8 sum = default;
for (std::simd::block<8> at : std::simd::blocks<8>(a.len())) {   // full blocks, then one masked tail
    sum = a.load(at).mul_add(b.load(at), sum);
}
f32 total = sum.reduce_add();
```

- The vector types (`f32x8`, `i32x4`, `u8x32`, ...) have operators that work lane by lane, with
  the usual overflow rules; comparisons give masks.
- They run on every CPU level: wider than the hardware is split, narrower is native.
- `g.target_cpu(t, std::build::cpu::x86_64_v3)` compiles a target for a CPU level.
  `@std::simd::multiversion` makes several copies of a function and picks one at the first call.
- `edaxc simd-report file.ed` shows what each vector operation becomes on the CPU level.
- `std::simd::x86::...` / `std::simd::arm::...` are raw intrinsics, for when you need them.

## Plugins and hot reload

```edax
// in the plugin (g.add_plugin in build.ed)
@std::plugin_entry
@std::hot_reload_state                  // its fields survive a reload, matched by name
class game : api::game { ... static game make() { ... } }

// in the program
mut std::plugin<api::game> g = std::plugin<api::game>::load(dir / "game.so")?;
g->update(dt);
g.reload(dir / "game.so")?;             // the new build of the same plugin, with the state carried over
```

- `load` checks the interface (a hash of it) and the compiler version: a mismatch is an error
  value, never a crash at the first call.
- On `reload`, a field whose type changed refuses the reload (`state_changed`), and the old build
  stays loaded.
- `std::filesystem::watch(dir)` tells you when to rebuild. The project in
  `tests/action/build/reload_project` is a complete example.

## Your goal

`exercise.ed` simulates 1003 particles stored as columns. It doesn't build:
`@std::no_checks_left` rejects `integrate` and lists the checks left in it. Make the loop
check-free without removing the annotation, then write `energy` with `f32x8`, so that the program
prints:

```
energy before: 827.35
energy after:  1958.76
first particle at (2.000, 0.002)
```

Then run it with `--mode=release` as well. The output must be the same.

## Try this afterwards

1. Run `edaxc explain-checks course/12-performance/exercise.ed` before and after your fix.
2. Pass the same column twice: `integrate(mut p.x, mut p.x, ...)`.
3. Rewrite the particles as `std::darray<particle>` (array of structs) and compare the build
   errors and the generated code (`edaxc emit-llvm`).
4. Write `energy` with a plain loop and compare `edaxc simd-report` for both versions.
