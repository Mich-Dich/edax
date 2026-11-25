# The Standard Library

Everything in `std::` is ordinary Edax code, except for the few things the compiler must provide. This page explains the library's rules and lists every area with its page.

## Rules

### Naming
- Always `std::`-qualified. There is no `using`; a file can shorten a namespace with an alias (see [Namespaces](../language/organization/namespaces.md)).
- `snake_case`, with **full words over abbreviations**: `std::error`, not `std::err`. Established short forms are kept where they are clearer than the long name: `darray`, `str`, `ptr`, `io`.
- Names say what something *is*. There is no `std::list`: in C++ that name means a linked list.
- **The boundary test:** if a feature can be written as an ordinary Edax function or type, it belongs in `std::`, not in the language.

### Map of the library
| Area | Names | Page |
|---|---|---|
| Core | `std::take`, `replace`, `swap`; `std::checked_*`, `std::saturating_*`; bit functions; `std::hash`, `std::hashable`; `std::ordering`, `std::sort_order`; `std::comparable`, `std::cloneable`, `std::formattable`; `std::exit_code`, `std::panic`, `std::set_panic_hook`; `std::version` | [Core utilities](core/utilities.md) |
| Optimizer facts, layout | `std::assume`, `std::unreachable`, `std::bit_cast`, `std::padding<N>`, `std::nonzero<T>` | [Core utilities](core/utilities.md), [Memory layout](../language/user-types/layout.md) |
| Results | `std::result<T, E>`, `std::option<T>`, `std::ok`, `std::error`, `std::some`, `std::none` | [Error handling](../language/errors/error-handling.md) |
| Strings | `std::string`, `std::string_with<A>`, `std::str`, `std::static_str` | [Strings](text/strings.md) |
| Formatting | `std::format_args`, `std::format`, `std::print`, `std::println`, `std::eprint`, `std::eprintln` | [Formatting and printing](text/formatting.md) |
| Sequences | `std::darray<T>`, `std::array<T, N>`, `std::span<T>`, `std::bitset<N>` | [Sequences](containers/sequences.md) |
| Maps | `std::hash_map<K, V>`, `std::tree_map<K, V>`, `std::map_entry<K, V>` | [Maps](containers/maps.md) |
| Pointers | `std::unique_ptr<T>`, `std::shared_ptr<T>`, `std::weak_ptr<T>` (and the language's `std::raw_ptr<T>`) | [Smart pointers](memory/smart-pointers.md) |
| Allocators | `std::allocator`, `std::global_allocator`, `std::arena`, `std::arena_allocator`, `std::sealable_allocator`, `std::seal_heap`, `std::no_allocator` | [Allocators](memory/allocators.md) |
| Raw memory | `std::raw::allocate`, `read`, `write`, atomics, futex, `std::raw::array<T, N>` | [Raw memory](memory/raw-memory.md) |
| Callables | `std::function<R(A...), I...>`, `std::inline_function<R(A...), N>`, `std::fn_ptr<R(A...)>`; planned: `std::any_of<I...>` | [Lambdas](../language/functions/lambdas.md) |
| Threads | `std::thread::spawn`, `handle<R>`, `scoped_handle<R>`, `options`, `id`, `sleep`; `std::sendable`, `std::shareable`, `std::thread_bound`; `std::cache_aligned<T>` | [Threads](concurrency/threads.md) |
| Synchronization | `std::atomic<T>`, `std::memory_order`, `std::mutex<T>`, `std::rw_mutex<T>`, `std::condition` | [Synchronization](concurrency/synchronization.md) |
| Channels | `std::channel<T>`, `std::channel_sender<T>`, `std::channel_receiver<T>` | [Channels](concurrency/channels.md) |
| Thread pool | `std::thread_pool`, `task<R>`, `scoped_task<R>`; `std::parallel::sort`, `find`, `transform`, ... | [Thread pool](concurrency/thread-pool.md) |
| Time | `std::time::duration`, `instant`, `wall_time`, `civil_time`, `zone` | [Time](time/time.md) |
| Dynamic libraries | `std::plugin<I>`, `std::c_library` | [Dynamic libraries and plugins](system/dynamic-libraries.md) |
| Files and environment | `std::file`, `std::io_error`, `std::filesystem::path`, `std::env` | [Files, filesystem and environment](io/io.md) |
| Windows | `std::windowing::system`, `window`, `gl_window`, `vulkan_window`, events, `input_state`, `bindings<A>`, `opengl::share_group`, `opengl::frame`, `vulkan::loader` | [Windowing](windowing/windowing.md) |
| Graphics APIs | `std::graphics::api`, `api_name`, `function_source`; `std::graphics::opengl` and `std::graphics::vulkan` (made from the Khronos registries) | [Graphics APIs](graphics/graphics.md) |
| Math | `std::math::vec2`, `vec3`, `vec4`, `rect`, `color`, `oklch`, `lerp`, `smoothstep` | [Math](math/math.md) |
| Images | `std::image::rgba8`, `decode_png`, `decode_qoi`, `encode_png`, `encode_qoi`, `inflate_zlib` | [Images](image/image.md) |
| Fonts | `std::text::font`, `distance_field`, `line_metrics`, `outline` | [Fonts](text/fonts.md) |
| User interfaces | `std::ui::theme`, `palette`, `controls_style`, `skin`, `painter`, `paint::*`, `text_style`, `fonts`, `draw_list`, `texture`; `std::ui::opengl::renderer<A>` | [User interface core](ui/core.md) |
| UIs from types | `@ui::range`, `label`, `unit`, `group`, ... (annotations), `ui::form<T>`, `ui::edit`, `ui::rules`; `scope.inspect`, `field` | [UIs from types](ui/forms.md) |
| Immediate-mode UI | `std::ui::immediate::context`, `frame`, `scope` (windows, sections, cards, rows, buttons, checkboxes, toggles, sliders, progress, own widgets); `std::ui::input_of` | [Immediate-mode widgets](ui/immediate.md) |
| Docking | `std::ui::dock::layout` (splits, tab stacks, the central area, floating stacks, closed windows; text), `workspace` (named layouts, saving), `f.dockspace`, `space.window`/`document`, `w.split_x`/`split_y`, `window_menu` | [Docking](ui/docking.md) |
| Tools and editors | `ui::theme::studio`, `ui::skin::studio`, `ui::icon` and `draw_list::draw_icon`; `scope.icon_button`, `icon_toggle`, `badge`, `spacer`, `menu`, `menu_item`, `available_height`; `frame.panel(id, rect, order, inset)` | [Tools and editors](ui/studio.md) |
| Node editor | `std::ui::nodes::editor` (`w.node_editor`: nodes with pins and scope bodies, links, frames, selection, zoom, minimap, styles), `nodes::edits`, `nodes::topological_order`; `nodes::graph<K>` and `nodes::graph_editor` (a whole editor from an enum of kinds: `@nodes::kind`, `@nodes::in`, `@nodes::out`, `@nodes::converts`, add-node menu, copy and paste, undo, text files) | [Node editor](ui/nodes.md) |
| Plots and debug tools | `std::ui::plot::scope` (`w.plot`: lines, areas, scatter, bars, reflection, logarithmic axes), `history<T, N>`, `decimate`, `w.sparkline`, `w.stat`; `std::ui::debug::perf_overlay`, `log_sink`, `log_view` | [Plots and debug tools](ui/plots.md) |
| Logging | `std::log::info` ... `fatal`, `validate`, `assert`, `session`, sinks, patterns | [Logging](logging/logging.md), [Logging output](logging/output.md) |
| Call sites | `std::call_site`, `std::implicit<T>` | [Functions](../language/functions/functions.md) |
| SIMD | `std::simd::vec<T, N>` (`f32x8`, ...), `std::simd::mask<N>`, `std::simd::block<N>`, `std::simd::blocks`, `std::simd::native_lanes`, `std::simd::x86::*`, `std::simd::arm::*` | [SIMD](../language/simd/simd.md) |
| Compile time | `std::is_integer`, `std::is_float`, `std::is_same`, `std::implements`, `std::size_of`, `std::align_of`, `std::type_name` | [Compile-time evaluation](../language/compile-time/compile-time.md) |
| Reflection | `std::meta::*`, `std::meta::type_id`, `std::embed`, `std::embed_text`; planned: `std::meta::runtime_info` | [Metaprogramming](../language/compile-time/metaprogramming.md) |
| Annotations | `std::derive::*`, `std::deprecated`, `std::allow`, `std::must_use`, `std::inline_always`, `std::inline_never`, `std::cold`, `std::test`, `std::optimize`, `std::float_mode`, `std::no_checks_left`, `std::interior_mutable`, `std::layout`, `std::hot_reload_state`; planned: `std::reflect` | [Annotations](../language/compile-time/annotations.md) |
| Diagnostics | `std::diagnostic`, `std::compile_error`, `compile_warning`, `compile_note`, `std::location_of`, `std::is_compile_time`, `std::literal_text` | [Checks](../language/errors/checks.md) |
| Build | `std::build::graph`, `std::build::mode::*`, `std::build::current_mode`, `mode_is`, `std::build::setting<T>`, `target_has`, `capability`, `std::build::package` | [Build modes](../build/build-modes.md), [Build system](../build/build-system.md), [Packages](../build/packages.md) |

`import std;` makes all of it available; `import std::filesystem;` and the like load only part of it.

### What the compiler provides
The library is Edax code built on `std::raw` (see [Raw Memory: `std::raw`](memory/raw-memory.md)) and a few `extern(std::build::language::c)` functions. The compiler provides only what can't be written in Edax:
- the `std::raw` intrinsics,
- `std::option` / `std::result` methods and `?` (see [Error Handling](../language/errors/error-handling.md)),
- formatting (`f"..."`, `std::format`, printing),
- `std::checked_*` / `std::saturating_*` and the bit functions,
- the compile-time functions (`std::size_of`, `std::meta::...`, the diagnostic functions),
- `std::graphics::opengl` and `std::graphics::vulkan`, which edaxc writes from the Khronos registries it embeds (see [Graphics APIs](graphics/graphics.md)).

Binary files of the library (the fonts of `std::text::font`, the shaders of `std::ui::opengl`) are built into edaxc too: `std::embed` and `std::embed_text` in a std module read them from edaxc, not from the disk.

### Performance
The standard library is meant to be the fast option, not a baseline that needs a third-party replacement. Each container uses the design the fastest C++ libraries converged on, and its layout is part of its page. None of this weakens safety: every public operation stays bounds- and state-checked; the `unsafe` code is inside the library, behind the same API. Edax functions have no exceptions, so the optimizer can inline and specialize across the whole program.

## Rationale
- **Library over language:** the boundary test keeps the language small. A feature in `std::` can be read, debugged and replaced like any other code.
- **Full names:** `std::error` and `std::darray` are longer than `err` and `vec`, but every reader understands them without a glossary.
