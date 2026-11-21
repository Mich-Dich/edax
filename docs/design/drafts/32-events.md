# 32 - Events (`std::event`) — DRAFT

> **Status:** design draft for review. Nothing here is implemented.
> **Source:** the C++ event bus of the GLT engine (`event.h`, `event_bus.h/.inl/.cpp`, `input_event.h`), translated to Rift's rules and widened into a family of dispatchers.

An **event** is a value that says something happened. The code that sends it doesn't know who reacts. `std::event` is **one vocabulary** (events, `flow`, handlers, subscriptions, `@std::event::on`) and **several dispatchers**, each with a visible cost:

| You need | Use | Cost of one delivery | Allocations |
|---|---|---|---|
| Listeners known at compile time | `std::event::deliver(mut target, e)` + `@std::event::on` methods (section 3) | Direct calls, inlinable | None |
| One event type, owned by the object that sends it | `std::event::signal<E, C>` (section 4) | A loop over handlers, one indirect call each | One handler per subscription |
| Any event type, chosen at run time, one thread | `std::event::bus<C>` (section 5) | One hash lookup, then the same loop | Per subscription; per event type on first use |
| Other threads send into a bus | `bus.inbound()` → `std::event::sender` (section 6) | One channel push; delivered by the owner at `dispatch_inbound` | None for events up to 48 bytes |
| Every subscriber gets every event, on its own thread | `std::event::broadcast<E>` + `std::event::inbox<E>` (section 7) | One copy into each inbox | None: bounded rings |

There is **no global bus**. The bus a function uses is in its signature, like `std::thread_pool` (see 28).

## 0. The design space

The C++ bus answers six questions one way each. Rift's standard library has to answer them for everybody, so they are listed here with the answer `std::event` gives.

| Question | Choices | What the choice changes | `std::event` |
|---|---|---|---|
| Is the set of event types known at compile time? | closed / open | Type erasure, a hash lookup, whether "nobody handles this" can be a compile warning | Both: `deliver` (closed), `bus` (open) |
| Are the listeners known at compile time? | yes / subscribed at run time | Stored callables on the heap, registration order | Both: `@on` methods, `subscribe` |
| When do handlers run? | inside the send / later, at a chosen point | Re-entrancy, ordering | Both, on the **same** bus: `send` and `enqueue` |
| On which thread? | the sender's / the bus owner's / each subscriber's own | `std::sendable` bounds, synchronization | Sender's (`bus`, `signal`, `deliver`); owner's (`inbound`); subscriber's (`broadcast`) |
| Push or pull? | handlers are called / subscribers drain a queue | Whether the subscriber's state is reachable from the handler | Push for one thread; pull across threads |
| Ordering and propagation | priority + stop / every subscriber, unordered | | Priority + `flow::stop` for push; every inbox for pull |

**Why not one generic `bus<policies...>`.** A single type with policy parameters would change, depending on its parameters, whether it is `std::shareable`, whether handlers get `mut E&` or `E&`, whether events must be `std::sendable`, and whether handlers run at all (pull). Every one of those would be an `if compile_time` in the API, error messages would talk about policies instead of the code, and the cost would no longer be readable from the type name (principle 7). Rift already splits things this way: `std::thread::spawn` and `pool.spawn` (see 28), `std::channel` and `std::mutex` (see 27). So the split follows **thread and cost**, and everything else is shared:

- the same `std::event::flow`, `subscribe_options`, `subscription` and `@std::event::on` everywhere,
- immediate vs. queued delivery is a **method** on the same bus (`send` / `enqueue`), because it changes neither safety nor constraints,
- moving code from one dispatcher to another is mechanical.

## 1. What changes from the C++ bus

| C++ | Rift | Why |
|---|---|---|
| Base class `event` with a `handled` field and a virtual `to_string()` | **Any non-view type is an event.** No base class. `@std::derive::formattable` for logs and tools | Events are plain data (05). A vtable per event, and inheritance only to carry one flag, cost without use |
| A handler sets `handled`; the bus checks it after each call | A handler **returns** `std::event::flow::stop` or `pass` | The decision is the handler's result, visible in its signature. Event types don't need to know about buses |
| `std::type_index(typeid(T))` as the table key | `std::meta::type_id<T>()` (new, section 9) | Rift has no RTTI. The id is a compile-time constant |
| `std::function<void(T&)>` that captures `this` | Handlers own their captures. The state they change is the bus's **context** `C`, passed to `send` | A stored lambda can't hold references (18). A handler capturing `mut &this` is exactly the dangling pointer C++ allows |
| `post` from inside a handler runs **depth-first**; a snapshot pool per nesting depth keeps it safe | `d.post(e)` from a handler is a **follow-up**: delivered after the current event is finished, before `send` returns (breadth-first) | `send` is `mut` on the bus, so a handler can't reach the bus (12). No nesting means no snapshots and no half-delivered events |
| Unsubscribing during a dispatch: `active = false`, the snapshot keeps the entry alive | Same flag, but **shared by the subscription and the bus**; entries are removed after the `send` | The comment in `event_bus.inl` (release the callback when the dispatch ends) still holds |
| `subscription_guard` keeps a raw `bus*` and "must not outlive the bus" | `std::event::subscription` shares only the flag. Either side may be destroyed first | No raw pointer, no lifetime rule to remember |
| A raw `handle` **and** an RAII guard | One `subscription`, which unsubscribes on destruction; `.keep() move` makes it permanent | One way, like `detach()` on thread handles (27) |
| Process-wide `global()` and forwarding free functions | No global bus | See 28: hidden globals make costs and ownership invisible |
| `std::atomic<handle>` inside a class documented as not thread-safe | A plain counter. Thread safety is a different type | Nothing costs more than it looks |
| `mouse_event` with a tag and fields that are only valid for some tags | A payload enum | One way to write a sum type (08) |

### Notes on the C++ code
Found while translating; worth fixing in GLT regardless of Rift:

- `mouse_event::m_state` is never set by either constructor, so `get_state()` always returns the default.
- `event_function<T>` (`bool(*)(T&)`) isn't accepted by the bus, which takes `std::function<void(T&)>`.
- `const action_type m_action_type` makes `mouse_event` non-assignable, so it can't live in a reused buffer (a queued bus would need that).
- Depth-first nested `post`: if a priority-100 handler of event A posts B, B's handlers run before the priority-0 handlers of A, which then see a world B already changed. This is the main argument for follow-ups (section 5).

## 2. The vocabulary

### 2.1 Events
- **Any type that isn't a view** (12) can be an event: a struct, class, enum or number. An event may be stored (queues, follow-ups), so a view is an error at the `subscribe`/`send`: "an event may be queued, so it can't borrow; 'std::str' is a view".
- **Exact type only.** Subscribers of `damage` don't see `fire_damage`. A family of events is a payload enum (08): subscribe to `input_event` and `match` on it.
- Recommended: `@std::derive::formattable` (29), so tracing and the inspector can print events (section 8). The bus checks for it with `std::implements<E, std::formattable>()` and falls back to the type name.

### 2.2 `flow`
```rift
namespace std::event {
    enum flow : u8 { pass, stop }
}
```
- Every handler returns one. `stop` ends the delivery of **this event**: handlers with lower priority don't see it. Follow-ups already posted are still delivered.
- `send`, `deliver` and `signal::send` return `stop` if a handler stopped the event, `pass` otherwise. Scripts use it to ask "did anybody consume this?".

### 2.3 Subscribing
```rift
namespace std::event {
    struct subscribe_options {
        i32  priority = 0;       // higher runs first; equal priorities run in subscription order
        bool once     = false;   // removed after its first call, whatever it returns
    }
}
```
- Every `subscribe` has two overloads: `subscribe(handler)` and `subscribe(options, handler)`. Options come first, like `std::thread::spawn` (27). There are no default arguments (04).
- Every `subscribe` takes a last `implicit compile_time std::call_site where` (31): tools can show **where** each subscriber was registered (section 8). It costs static data only.
- Priorities are plain `i32`. Name them: `compile_time i32 armor_priority = 100;`.

### 2.4 `subscription`
```rift
namespace std::event {
    class subscription {                        // move-only: not a copy type, not cloneable
    public:
        bool            is_active();
        subscription_id id();
        void            cancel() move;          // unsubscribe now
        subscription_id keep() move;            // never unsubscribe on destruction
        ~subscription();                        // unsubscribes
    }
}
```
- **Destroying a subscription unsubscribes.** That is the C++ guard, and it is the only form. A subscription that should live as long as the bus is given up explicitly with `keep()`, the way a thread is `detach()`ed (27). A kept subscription can still be removed with `bus.unsubscribe(id)`.
- `subscribe` is `@std::must_use` (29): a subscription dropped at the end of the statement would unsubscribe immediately, which is never what the code meant (example 6).
- **How it works:** the subscription and the bus's entry share one flag (`std::shared_ptr<std::atomic<bool>>`). `cancel` clears it. The bus skips entries whose flag is clear and removes them after the current `send`. If the bus is destroyed first, cancelling only clears a flag nobody reads. A subscription is a move-only, `std::sendable` value: it can be stored in a field, moved into a struct, or destroyed on another thread.

## 3. Static delivery: `@std::event::on` and `deliver`

When the listeners are known when the program is compiled (engine systems, a fixed set of components), no table is needed at all.

```rift
namespace std::event {
    annotation(std::meta::target::function) struct on {
        i32 priority = 0;
        // checks (23, 29): exactly one parameter, `E&` or `mut E&` of a non-view type; returns std::event::flow
    }

    annotation(std::meta::target::field) struct route { }    // deliver also looks inside this field

    template<type T, type E>
    std::event::flow deliver(mut T& target, E event);
}
```
- `deliver` collects, **at compile time**, every method of `T` annotated `@std::event::on` whose parameter is `E&` or `mut E&`, and the same for every field of `T` annotated `@std::event::route` (recursively). It sorts them by priority, then by declaration order (depth-first through the fields), and calls them in that order until one returns `stop`.
- The generated code is a sequence of direct calls. It inlines, allocates nothing and looks nothing up. It is ordinary library code: `for compile_time` over `std::meta::methods<T>()` and `std::meta::fields<T>()` (26), with a compile-time sort.
- **Checks** (23):

  | Check | Kind |
  |---|---|
  | `deliver` of an event that no handler of `T` takes | warning `std::event::unhandled`, at the call |
  | `@on` on a method that doesn't have the handler shape | error at the `@`, naming what's wrong |
  | `@route` on a field whose type has no `@on` methods and no `@route` fields | warning `std::event::empty_route` |

- `std::event::handlers_of<T, E>()` is the `compile_time` list `deliver` uses (name, owning field path, priority). An inspector can print it; `riftc expand` shows the generated `deliver`.
- The target **is** the context: handlers are methods, so they reach their own state through `this`. A `mut` handler needs nothing special, because `deliver` takes `mut T&`.

## 4. `std::event::signal<E, C>`

One event type, stored in the object that sends it (`button.clicked`, `inventory.changed`). No type ids, no hash map: it's what the bus is built from.

```rift
namespace std::event {
    template<type E, type C>
    class signal {
    public:
        @std::must_use subscription subscribe(std::function<std::event::flow(mut E& event, mut C& context)> handler,
                                              implicit compile_time std::call_site where);
        @std::must_use subscription subscribe(subscribe_options options,
                                              std::function<std::event::flow(mut E& event, mut C& context)> handler,
                                              implicit compile_time std::call_site where);

        std::event::flow send(mut C& context, E event) mut;
        usize            count();
    }
}
```
- With `C = void` (27 §10) the handler is `flow(mut E& event)` and the call is `send(event)`.
- Handlers are kept in a `std::darray` sorted by priority. `send` moves the event into a local, calls each active handler with it until one returns `stop`, then removes cancelled and `once` entries.
- A signal has no follow-ups (a signal knows only `E`). Code that needs them uses a bus.
- `signal` is a value with no hidden sharing: it is moved and destroyed with the object that owns it. Destroying it releases every handler; subscriptions to it become inactive.

## 5. `std::event::bus<C>`

The translation of the C++ bus: any event type, subscribed at run time, delivered on the calling thread.

```rift
namespace std::event {
    struct bus_options {
        usize max_cascade = 10_000;    // follow-ups per send before it panics (an event loop)
        bool  trace       = false;     // log every send at `trace` (section 8)
    }

    template<type C>
    view struct dispatch {             // passed to every bus handler, valid during the call
        mut C& context() mut;
        template<type T> void post(T event) mut;
        void unsubscribe_self() mut;
        template<type T> @std::must_use subscription subscribe(handler<T, C> h, implicit compile_time std::call_site where) mut;
        template<type T> @std::must_use subscription subscribe(subscribe_options o, handler<T, C> h, implicit compile_time std::call_site where) mut;
        std::type_id   event_type();
        std::call_site subscribed_at();
    }

    template<type E, type C>
    compile_time type handler = std::function<std::event::flow(mut E& event, mut std::event::dispatch<C>& d)>;

    template<type C>
    class bus {
    public:
        static bus<C> make(bus_options options);         // `default` uses bus_options{}

        template<type E> @std::must_use subscription subscribe(handler<E, C> h, implicit compile_time std::call_site where) mut;
        template<type E> @std::must_use subscription subscribe(subscribe_options o, handler<E, C> h, implicit compile_time std::call_site where) mut;
        void unsubscribe(subscription_id id) mut;

        template<type E> std::event::flow send(mut C& context, E event) mut;   // now
        template<type E> void enqueue(E event) mut;                           // later
        usize dispatch_queued(mut C& context) mut;                            // everything enqueued, in order

        std::event::sender inbound();                                         // section 6
        usize dispatch_inbound(mut C& context) mut;

        template<type E> usize subscriber_count();
        std::darray<std::event::subscriber_info> describe();                  // section 8
    }
}
```

### 5.1 The context
A handler is **stored**, so its lambda can't capture references (18): no `[mut &player]`, no `this`. The state handlers change is passed in by the sender: `bus.send(mut world, e)`, and every handler reaches it with `d.context()`. All handlers of one bus share the context type `C`; a game passes its `world`, an editor its `document`.

- With `C = void` there is no context: `send(e)`, and `d.context()` is a compile error.
- Handlers may still **own** data: `[threshold = 10u32]`, `[move table]`.
- **The bus is not part of its context.** `state.events.send(mut state, e)` with the bus inside `state` is two mutable borrows of `state` (12). Keep the bus beside the context (example 6).

### 5.2 Sending now: `send`
1. Find the table of `E` (one hash lookup on a precomputed `std::type_id`). No table: return `pass`.
2. Call every **active** handler in priority order with the event, until one returns `stop`.
3. Remove cancelled and `once` entries, and add subscriptions made through `d.subscribe`. They take effect from the next event.
4. Deliver the **follow-ups** that handlers posted with `d.post(...)`, first in first out, each one completely (steps 1–4), until none are left.
5. Return the flow of the original event.

- More than `max_cascade` follow-ups in one `send` panic with the types of the last ones: A posts B posts A is a bug, not a slow program.
- Follow-ups are **breadth-first**: every handler of an event sees the world after the event, never in the middle of another one. That is the one behaviour change from the C++ bus, and the reason is in section 1.
- `d.unsubscribe_self()` removes the running handler after it returns; it isn't called again, not even for follow-ups of the same `send`. `once` is the same thing decided at subscription time.

### 5.3 Sending later: `enqueue` and `dispatch_queued`
- `enqueue(e)` stores the event in the bus and runs **no** handler. It's for code that produces events while the world is busy (physics substeps, a parser) and wants them handled at a point it chooses.
- `dispatch_queued(mut ctx)` sends every queued event in enqueue order, each like `send` (with its follow-ups), and returns how many it sent.
- The queue is type-erased: events up to 48 bytes sit inline in reused blocks; bigger ones allocate once. Once the blocks have grown, enqueueing allocates nothing.

### 5.4 Threads
`std::function` isn't `std::sendable` (open question 47), so a bus is neither sendable nor shareable: it lives on the thread that made it, usually the main thread. Other threads send into it through `inbound()` (section 6).

### 5.5 Cost
| Operation | Cost |
|---|---|
| `subscribe` | The `std::function` (one allocation, 18), the shared flag (one allocation), one entry in the type's `darray`. The first subscription of a type also allocates its table |
| `send`, no subscribers | One hash lookup on a 64-bit id |
| `send`, N handlers | One hash lookup; per handler one relaxed atomic load and one indirect call |
| Follow-ups, `enqueue` | Inline in reused buffers; no allocation once grown, for events up to 48 bytes |
| Unsubscribe | Clearing a flag; the entry is removed after the current or next `send` of its type |

## 6. Sending from other threads: `inbound()`

The common case in an engine: a worker finishes something and the main thread should react, in its own time.

```rift
namespace std::event {
    class sender : std::cloneable {               // std::sendable, std::shareable
    public:
        template<type E : std::sendable>
        std::result<void, E> post(E event);       // never waits; error(event) once the bus is gone
    }
}
```
- `bus.inbound()` returns a sender for a multi-producer queue owned by the bus: an unbounded channel (27 §7) of type-erased `std::sendable` events. `.clone()` makes another sender.
- `post` works from any thread and never waits. When the bus has been destroyed, it returns `error(event)`, handing the event back like `channel_sender::send`.
- `bus.dispatch_inbound(mut ctx)` sends everything that arrived, like `send`, on the owner's thread. Call it where it fits: once per frame, before the simulation step.
- **Order:** events posted by one thread arrive in that thread's order. There is no order between threads.
- Events up to 48 bytes are stored inline in the channel's blocks; bigger ones allocate once per event.

## 7. `std::event::broadcast<E>`: every subscriber, on its own thread

For data that several threads each want a copy of (frame statistics to a HUD and a telemetry thread, game state to an audio thread). Nothing runs on the publisher's thread: each subscriber **pulls** from its own inbox, when it chooses. This is the same model as `win.drain_events(...)` in the windowing draft.

```rift
namespace std::event {
    enum when_full : u8 { wait, drop_oldest, drop_newest }

    struct inbox_options {
        usize                 capacity;       // required
        std::event::when_full when_full;      // required
    }

    template<type E : std::sendable>
    class broadcast {                         // a synchronized type (27 §2): std::shareable
    public:
        static broadcast<E> make();
        usize publish(E& event);              // returns how many inboxes received it
        std::event::inbox<E> subscribe(inbox_options options);
        usize subscriber_count();
    }

    template<type E>
    class inbox {                             // std::sendable; works on immutable `this` like a channel receiver
    public:
        std::option<E>                       receive();      // waits; none once the broadcast is gone and the inbox empty
        std::result<E, std::receive_error>   try_receive();
        usize                                drain(mut std::darray<E>& out);
        u64                                  dropped();      // events lost to drop_oldest / drop_newest
        std::option<E>                       next();         // a stream: for (E e : inbox) { ... }
    }
}
```
- `publish` copies (copy types) or clones (`std::cloneable`) the event **once per inbox**; that is its cost, written here and in the reference. For large events publish a `std::shared_ptr<E>`: one allocation, and every inbox shares a read-only pointer (13).
- An inbox is a bounded ring (the channel machinery of 27 §7). **`capacity` and `when_full` are always written**: a slow subscriber either slows every publisher (`wait`) or loses events, and only the program knows which one is acceptable. Like the chunk size in 28, the important setting isn't guessed.
- Destroying an inbox unsubscribes it. Destroying the broadcast closes every inbox: they still return what is queued, then `none`.
- Each inbox receives events in publish order per publishing thread.

## 8. Tooling
- **Who listens:** `bus.describe()` returns, per event type, every subscriber with its priority, `once`, and the `std::call_site` of its `subscribe` (section 2.3). An in-game inspector can list "`game::damage`: 3 subscribers — rules.rifts:21 (200), rules.rifts:30 (100), rules.rifts:39 (0)".
- **What happened:** with `bus_options{ .trace = true }`, every `send` is logged at `trace` (31): the event (`{event}` if it is formattable, its type name otherwise), the number of handlers called, and whether one stopped it. It is an ordinary `std::log::trace` call, so it is removed at compile time where `trace` is off.
- **Static:** `std::event::handlers_of<T, E>()` and `riftc expand` (section 3).

## 9. Language and library additions

| Addition | Used for |
|---|---|
| `std::meta::type_id<T>() → std::type_id` (`compile_time run_time`). `std::type_id` is a copy type: `==`, `std::hashable`, `std::formattable` (prints the type name), sendable and shareable. Its value is a 64-bit hash of the fully qualified type, including template arguments, so it is the same in every module. Two types with the same hash are a **link error** naming both (all ids are known at link time, like template instances in 14) | The bus's table, `inbound` type erasure |
| `@std::must_use{ .message = "..." }`: an optional message for the error | Explaining why a subscription must be kept (example 6) |
| `cast::dynamic` from an interface pointer to a class (17, existing) | The bus finds the typed table behind an erased one without `unsafe` |

Everything else is library code on top of 26 (reflection), 27 (channels, atomics), 29 (annotations) and 31 (`implicit` call sites, logging).

## Examples

### 1. The GLT input events in Rift
```rift
// engine/input/events.rifth
import std;
import platform::window;
import math::vector;

namespace glt::input {
    @std::derive::formattable
    enum mouse_action : u8 {
        moved(math::vec2),             // delta in pixels
        scrolled(math::vec2),          // wheel delta
        cursor_entered(bool),          // true: entered the window, false: left it
    }

    @std::derive::formattable
    struct mouse_event {
        platform::window_handle window;
        mouse_action            action;
    }

    @std::derive::formattable
    struct key_event {
        platform::window_handle window;
        key_code                key;
        key_state               state;
        modifiers               modifiers = default;

        bool is(key_code k, key_state s);
    }

    @std::derive::formattable
    struct char_event {                  // text input, not keys
        platform::window_handle window;
        char                    text;    // a Unicode scalar value (02); the C++ version used u32
        modifiers               modifiers = default;
    }
}
```
- No base class and no `to_string()`: `@std::derive::formattable` prints `mouse_event { window: ..., action: moved(...) }`.
- `entered` can't be read on a `moved` event any more: `match` (10) only gives you the payload that exists.
- If `std::windowing` lands as drafted, these are thin wrappers or simply `wnd::key_event` itself: an event can be any type.

### 2. Game rules on a bus
```rift
// game/rules.rifts
import std;
import game::world;

namespace game {
    compile_time i32 god_mode_priority = 200;
    compile_time i32 armor_priority    = 100;

    std::darray<std::event::subscription> install_rules(mut std::event::bus<world>& events) {
        mut std::darray<std::event::subscription> rules = default;

        // God mode swallows damage before anyone else sees it.
        rules.push(events.subscribe<damage>(std::event::subscribe_options{ .priority = god_mode_priority },
            [](mut damage& e, mut std::event::dispatch<world>& d) {
                if (d.context().is_invulnerable(e.target)) {
                    return std::event::flow::stop;
                }
                return std::event::flow::pass;
            }));

        // Armor reduces the amount for every later handler.
        rules.push(events.subscribe<damage>(std::event::subscribe_options{ .priority = armor_priority },
            [](mut damage& e, mut std::event::dispatch<world>& d) {
                e.amount = std::saturating_sub(e.amount, d.context().armor_of(e.target));
                return std::event::flow::pass;
            }));

        // Health applies it. A death is a follow-up: delivered after every damage handler ran.
        rules.push(events.subscribe<damage>([](mut damage& e, mut std::event::dispatch<world>& d) {
            bool died = d.context().apply_damage(e.target, e.amount);
            if (died) {
                d.post(entity_died{ .who = e.target });
            }
            return std::event::flow::pass;
        }));

        return rules;
    }
}
```
```rift
// game/main.rifts
mut game::world                    world  = game::world::load(level_path)?;
mut std::event::bus<game::world>   events = default;
std::darray<std::event::subscription> rules = game::install_rules(mut events);   // live as long as `rules`

events.send(mut world, game::damage{ .target = player, .amount = 30, .kind = game::damage_kind::fire });
```
`d.context().apply_damage(...)` is finished before `d.post(...)` starts. Holding `mut world& w = d.context();` across the `post` would be two mutable borrows of `d` (12).

### 3. A script that waits for a door
The C++ comment about `sleep_until_event` describes this case: a handler that removes itself.
```rift
// wake on any door, once
script.wake = events.subscribe<door_opened>(std::event::subscribe_options{ .once = true },
    [id = script.id()](mut door_opened& e, mut std::event::dispatch<game::world>& d) {
        d.context().scripts.wake(id);
        return std::event::flow::pass;
    });

// wake on one particular door: unsubscribe when it's the right one
script.wake = events.subscribe<door_opened>(
    [id = script.id(), door = wanted_door](mut door_opened& e, mut std::event::dispatch<game::world>& d) {
        if (e.door == door) {
            d.context().scripts.wake(id);
            d.unsubscribe_self();
        }
        return std::event::flow::pass;
    });
```
Assigning to `script.wake` destroys the previous subscription, which unsubscribes it.

### 4. Static delivery to engine systems
```rift
namespace game {
    class audio_system {
    public:
        @std::event::on
        std::event::flow on_damage(damage& e) mut;          // plays a hit sound
    }

    class achievement_system {
    public:
        @std::event::on{ .priority = -10 }
        std::event::flow on_death(entity_died& e) mut;
    }

    struct systems {
        @std::event::route audio_system       audio;
        @std::event::route achievement_system achievements;
        physics_system                        physics;      // not routed: deliver never looks inside
    }
}

_ = std::event::deliver(mut sys, game::entity_died{ .who = boss });   // calls sys.achievements.on_death directly
std::event::deliver(mut sys, game::door_opened{ .door = d });         // nobody takes it:
```
```
warning[W4300]: no handler of 'game::systems' takes 'game::door_opened'
 --> game/loop.rifts:41:5
  = note: handlers are methods with @std::event::on in 'game::systems' and its @std::event::route fields
  = note: warning id: std::event::unhandled
```

### 5. A loader thread and a broadcast
```rift
// The loader posts into the main thread's bus; the main thread handles it once per frame.
std::thread::handle<void> loader = std::thread::spawn([to_main = events.inbound(), move paths]() {
    for (std::filesystem::path& p : paths) {
        _ = to_main.post(asset_loaded{ .path = p.clone(), .bytes = std::filesystem::read_all(p) });
    }
});

// Frame statistics go to every thread that asks, each at its own pace.
std::event::broadcast<frame_stats> stats = std::event::broadcast<frame_stats>::make();
std::thread::handle<void> telemetry = std::thread::spawn([inbox = stats.subscribe(std::event::inbox_options{
        .capacity  = 256,
        .when_full = std::event::when_full::drop_oldest,
    })]() {
    for (frame_stats s : inbox) {                 // until `stats` is destroyed
        telemetry::upload(s);
    }
});

while (running) {
    _ = events.dispatch_inbound(mut world);       // asset_loaded handlers run here, on the main thread
    _ = events.dispatch_queued(mut world);
    simulate(mut world, mut events);
    _ = stats.publish(world.frame_stats());       // one copy into each inbox
}
```

### 6. Compile errors
```rift
mut player p = player::spawn();
events.subscribe<damage>([mut &p](mut damage& e, mut std::event::dispatch<world>& d) { ... });
```
```
error[E4310]: a bus handler is stored by the bus, so it can't capture references: 'mut &p'
 --> game/rules.rifts:12:30
  = note: handlers reach the state they change through the bus's context ('game::world'): d.context()
error[E4111]: the result of 'std::event::bus<game::world>::subscribe' must be used
 --> game/rules.rifts:12:5
  = note: destroying a std::event::subscription unsubscribes: store it, or call .keep() to subscribe for the bus's lifetime
```
```rift
struct game_state {
    game::world                  world;
    std::event::bus<game_state>  events;
}
state.events.send(mut state, e);
```
```
error: 'state' is borrowed mutably twice: by 'state.events' (the receiver) and by 'mut state'
 --> game/main.rifts:30:23
  = note: keep the bus beside its context: std::event::bus<game::world> and game::world as separate values
```

## Rationale
- **One vocabulary, several dispatchers:** the costs that differ (compile-time calls, a heap callable per subscriber, a hash lookup, a channel, a copy per inbox) are what decides which one fits. Splitting along them keeps every cost readable from the type name; sharing `flow`, options, subscriptions and `@on` keeps switching cheap.
- **No base class for events:** in C++ the base class exists for the `handled` flag and `to_string()`. A returned `flow` and `@std::derive::formattable` do both without a vtable, and any existing type (including another module's) can be an event.
- **The context parameter:** it's what Rift's rules force (stored lambdas own their captures), and it's also the better design: what a handler can change is written in the bus's type, instead of hidden in whatever each lambda captured.
- **Follow-ups instead of nested sends:** the borrow rules forbid re-entering a bus, and the result is simpler than the C++ snapshot pool: handlers never see a half-delivered event, and a loop is a clear panic instead of a stack overflow.
- **The subscription owns a flag, not the bus:** the C++ guard's documented rule ("must not outlive the bus") becomes something the type guarantees. Both orders of destruction are safe.
- **`deliver` with annotations:** for the fixed parts of an engine there's nothing to register and nothing to forget. An unhandled event is a compile-time warning, which no run-time bus can give.
- **Pull across threads:** a handler running on the publisher's thread would need its subscriber's state to be shareable, so it would take a lock. An inbox moves the data instead, and the subscriber handles it with ordinary `mut` code (27).
- **`capacity` and `when_full` required:** dropping events and blocking publishers are both correct somewhere and wrong elsewhere. A hidden default would be a decision made for the program.
- **No global bus:** the same reasoning as 28. A program that wants one makes one and passes it where it's needed.

## Open questions
| # | Topic | Notes |
|---|---|---|
| 47 | **Sendable `std::function`** | `std::function` erases its captures, so it can't be `std::sendable`. A `std::function<R(Args...), std::sendable>` (or a separate type) would make `bus` sendable, so a whole bus could move to a worker thread |
| 48 | **Subscribing to a family** | Exact types only. Payload enums cover families today; subscribing to "every `glt::input` event" by namespace or annotation would need a second lookup per send |
| 49 | **Follow-ups from static handlers** | `@on` methods can't post. A second parameter `mut std::event::followups&` and a loop in `deliver` would allow it |
| 50 | **Changing owned captures** | 18 doesn't say whether a lambda may change a value it captured (`[count = 0u32]` then `count += 1`). Handlers with their own counters need it |
| 51 | **Bounded `inbound`** | `inbound` is unbounded, so a flooding worker grows memory. `bus_options.inbound = std::event::inbound_options{ .capacity, .when_full }` like inboxes |
| 52 | **`type_id` and dynamic libraries** | Ids are unique per program. With plugins (42), an unloaded module's handlers must be cancelled first, and ids must match across libraries |
| 53 | **Waiting for an event in a script** | `sleep_until_event` wants something like `co_await bus.next<door_opened>()`. Rift has no coroutines yet |
| 54 | **Producers that don't care where events go** | An `interface std::event::target<E> { void post(E event) mut; }` that a bus, a signal and an inbox all implement, so `std::windowing` could forward input into any of them |
| 55 | **Registering an object's `@on` methods on a bus** | `bus.subscribe_all<T>(...)` would reach `T` through the context by reflection (26), so one set of annotated methods serves both `deliver` and a bus |








