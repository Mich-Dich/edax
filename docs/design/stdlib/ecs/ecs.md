# Entity Component System: `std::ecs`

`std::ecs` keeps a game's objects as **entities** (handles) with **components** (plain structs), and runs code over every entity that has a given set of components. The API follows EnTT's (`create`, `emplace` → `add`, `get`, `view<A, B>().each(...)` → `each(...)`), simplified where Edax can say more: **a query is the parameter list of its lambda**.

```edax
import std;
import std::ecs;
namespace ecs = std::ecs;

struct position { f32 x = 0.0; f32 y = 0.0; }
struct velocity { f32 x = 0.0; f32 y = 0.0; }

@ecs::resource
struct frame_time { f32 dt = 0.0; }

mut ecs::world w = ecs::world::make();
w.set_resource(frame_time{ .dt = 1.0 / 60.0 });
ecs::entity e = w.spawn(position{}, velocity{ .x = 1.0 });

w.each([](mut position& p, velocity& v, frame_time& t) {   // reads velocity, changes position
    p.x += v.x * t.dt;
    p.y += v.y * t.dt;
});
```

## Rules

### 1. Entities and components
| Call | Does |
|---|---|
| `ecs::world::make()` | An empty world |
| `w.create()` | A new entity without components |
| `w.spawn(a, b, ...)` | A new entity with these components, moved straight into their table |
| `w.add(e, c)` | Adds a component, or replaces the entity's one (EnTT's `emplace_or_replace`) |
| `w.remove<C>(e)` | Removes it; nothing when the entity doesn't have it |
| `w.has<C>(e)`, `w.get<C>(e)`, `w.get_mut<C>(e)` | Asks, reads, changes; `get` panics when the component is missing |
| `w.find<C>(e)`, `w.find_mut<C>(e)` | `std::option<C&>` / `std::option<mut C&>` |
| `w.destroy(e)`, `w.alive(e)`, `w.count()`, `w.count_with<C>()` | |

- An `ecs::entity` is an index and a generation. A destroyed entity's slot is reused with the next generation, so an old handle is never mistaken for the new entity: `w.alive(old)` is false, `w.get` on it panics.
- **Components are plain values** of any type. They are moved in and out of the world as bytes (Edax moves are bitwise), so they may own memory (`std::string`, `std::darray`).
- References from `get`, `find` and `each` borrow the world: the borrow rules stop a reference from outliving a change to the world.

### 2. Queries
```edax
w.each([](ecs::entity e, mut position& p, velocity& v) { ... });
w.each(ecs::filter::make().with<enemy>().without<dead>(), [](mut position& p) { ... });
```
- The lambda's parameters are the query (`std::meta::parameters_of`, see [Metaprogramming](../../language/compile-time/metaprogramming.md)):

| Parameter | Is |
|---|---|
| `T&` | a component the entity must have, read |
| `mut T&` | a component the entity must have, changed |
| `ecs::entity e` | the entity (a copy) |
| `R&` / `mut R&` of a `@ecs::resource` type | the world's resource (below) |

- Anything else is a compile error at the call: a component taken by value, `ecs::commands` outside a system, a lambda that changes its captures in a parallel query (see [checks](#checks)).
- A **filter** adds conditions that hand nothing out: `with<C>()`, `without<C>()`, and for tracked components `added<C>()` and `changed<C>()` (section 4).
- Inside a query the world's structure must not change: entities aren't created, destroyed or given other components. Record those changes as **commands** (section 5). The borrow rules enforce it: the lambda can't capture the world mutably while `each` runs.

**Whole columns.** `each_table` calls the lambda once per table with spans of its columns, for hand-written SIMD loops:
```edax
w.each_table([](std::span<mut position> ps, std::span<velocity> vs) {
    for (usize i : 0..ps.length()) { ps[i].x += vs[i].x; }
});
```
`std::span<T>`, `std::span<mut T>`, `std::span<ecs::entity>` and resources may be parameters.

**In parallel.** `w.par_each(pool, fn)` splits the matching rows into chunks of 4096 and runs them on a `std::thread_pool`. `fn` must be `std::shareable` and must not change its own captures; mutable data comes in through its `mut T&` parameters, which are different rows on every thread.

### 3. Resources and events
- A **resource** is one value per world (the frame time, input, a random generator). Its type is marked `@ecs::resource`; `w.set_resource(r)`, `w.resource<R>()`, `w.resource_mut<R>()`, `w.has_resource<R>()`, `w.take_resource<R>()`. Queries and systems get it as a parameter.
- **Events:** `w.send(e)` and `w.events<E>()` (a span, oldest first). An event stays readable until the second `w.update()` after it was sent, so a reader that looks once per frame sees every event, whenever in the frame it was sent.

### 4. Change detection
- A component type marked `@ecs::tracked` gets two ticks per entity: when it was added and when it was last changed. Untracked components cost nothing for this.
- A tracked component is **changed** when a query hands it out as `mut T&`, or by `add` and `get_mut`/`find_mut`. The write marks it; whether the value really differs isn't compared.
- `filter.added<C>()` / `filter.changed<C>()` match entities whose tick is later than `filter.since(tick)`, by default the last `w.update()`. A system compares with its own previous run, so it sees each change once.
- `w.removed<C>()` lists the entities that lost a tracked `C` (or were destroyed with it) since the last `update()`.
- `w.update()` ends a frame: a new tick, events older than a frame dropped, `removed` lists emptied.

### 5. Commands
```edax
mut ecs::commands cmd = ecs::commands::make();
w.each([mut &cmd](ecs::entity e, health& h) {
    if (h.value <= 0) {
        ecs::entity corpse = cmd.spawn(position{}, decay{ .seconds = 5.0 });
        cmd.destroy(e);
    }
});
w.apply(mut cmd);
```
- `create`, `spawn`, `destroy`, `destroy_recursive`, `add`, `remove`, `set_parent` and `send` are recorded and carried out by `w.apply(mut cmd)`, in order.
- Entities a command buffer creates are **provisional**: their handles can be used by the following commands (`cmd.add(corpse, ...)`) and become real entities when applied.
- Component values are moved into the buffer's own memory, not boxed one by one; dropping a buffer without applying it drops them.

### 6. Hierarchy
- `w.set_parent(child, parent)` gives the child an `ecs::parent` component and appends it to the parent's `ecs::children` list (ordinary components, so queries can ask for them). Making an entity its own ancestor panics.
- `w.remove_parent(child)`, `w.parent_of(e)`, `w.children_of(e)` (a span), `w.each_descendant(root, fn)` (parents before their children: transform propagation).
- `w.destroy(e)` leaves its children without a parent; `w.destroy_recursive(e)` destroys the subtree.

### 7. Systems
```edax
w.add_system("update", "movement", [](mut position& p, velocity& v, frame_time& t) { ... });
w.add_system("update", "reap", [](ecs::entity e, health& h, mut ecs::commands& cmd) { ... });
w.add_system("update", "ai", ecs::filter::make().with<enemy>(), [](mut brain& b, position& p) { ... });

w.run("update", pool);   // or w.run("update") on this thread
```
- A system is a query's lambda kept by the world under a schedule's name. It may also take `mut ecs::commands&`: its own buffer, applied after its stage.
- **What a system reads and writes is its signature.** `run` makes **stages** of the schedule's systems in the order they were added: a system joins the current stage unless it writes something another system there reads or writes, or reads what another writes. The systems of a stage run at the same time, their rows split across the pool; then the stage's commands are applied in the order of the systems.
- A system that changes a resource or records commands runs on one thread (its rows aren't split), but still at the same time as the other systems of its stage.
- `w.stages("update")` shows the stages, for tools and for checking that two systems really run together.
- Systems must be `std::shareable` and must not change their captures. The order of systems is the order they were added; there are no explicit "before/after" constraints yet.

### 8. How it is stored
- **Archetype tables.** Every set of component types has a table; each component is one column, aligned to the cache line (`std::cache_line_size`), plus one column of entity handles. An entity is a row.
- A query walks only the tables that have its components (matched once per new table, then cached), and calls the lambda row by row with references into the columns. After inlining, the loop is the plain loop over arrays one would write by hand: the movement query below runs as fast as `for (i) p[i].x += v[i].x * dt` over two arrays compiled by Clang (1.9 ms per frame for 1M entities in both).
- Adding or removing a component moves the entity's row to another table (the target is cached per table and component). That makes structural changes slower than in a sparse-set ECS, and queries over several components much faster: there are no lookups per entity.
- `get<C>(e)` is a lookup of the entity's table and row and of the column (a direct-mapped cache of the component's index, then an array).

| 1M entities (`bench/edax/ecs_world.ed` vs EnTT 3.13 views, GCC 13 -O2, best of 5) | std::ecs (ms) | EnTT (ms) | |
|---|---|---|---|
| spawn with 2 components | 50 | 50 | 1.00x |
| add a component to 833k | 47 | 15 | 3.09x |
| 100 x `pos += vel * dt` over 1M | 192 | 447 | 0.43x |
| 100 x a 2-component query over 500k (of 1M) | 33 | 208 | 0.16x |
| 1M `get<position>` in shuffled order | 40 | 34 | 1.19x |
| add + remove a component on 100k | 34 | 10 | 3.59x |
| destroy 1M | 151 | 337 | 0.45x |
| **whole program** | **533** | **1182** | **0.45x** |

EnTT's owning groups make its iteration faster than its views shown here, at the cost of declaring them; std::ecs needs nothing declared.

### Checks
Calls with parameters a query can't hand out are compile errors at the call, with the instance chain: `w.each([](position p) {})` (by value), `ecs::commands` in `each`, `set_resource` of a type without `@ecs::resource`, `changed<T>()` of an untracked `T`, `par_each`/`add_system` with a lambda that changes its captures.

## Rationale
- **The parameter list is the query.** EnTT writes `registry.view<position, const velocity>().each([](position&, const velocity&) {})`: the components twice, and `const` must match. Here the lambda alone says which components and which are written, with the `mut` that already marks every mutable reference in Edax. Systems get their reads and writes (for scheduling) from the same place, so the schedule can't disagree with the code.
- **Archetypes over sparse sets.** Games iterate far more often than they add components. Archetype tables make every query a walk over contiguous arrays, which is what the optimizer vectorizes and what caches like. Structural changes cost a row move; record them in commands and apply them in bulk.
- **Nothing costs more than it looks.** Change ticks only exist for `@ecs::tracked` components; resources and events are opt-in types; parallelism needs a pool argument. The query loop is the hand-written loop.
- **Safe by default.** Handles carry generations; references borrow the world; structural changes inside queries go through commands; systems that would race are put in different stages, and lambdas that would race are compile errors.
- **One way.** Systems are the same lambdas as `each`; filters are values; commands are the only deferred mutation.

## Not yet
- System ordering constraints (`before`/`after`), run conditions and fixed-timestep schedules.
- Events and the world itself as system parameters (systems read events through resources for now).
- Relationships beyond parent/child, prefabs, serialization of a world (reflection makes it a library function).
- Sparse-set storage for components that are added and removed every frame.
