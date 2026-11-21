# windowing: several windows, OpenGL and Vulkan, organized like an engine

A program on `std::windowing` (see [the library page](../../docs/design/stdlib/windowing/windowing.md)),
built the way a game engine is: an application that owns the window system, the renderer and the
main window, a stack of layers it drives, and settings kept in the project's `config/` directory.

```bash
riftc run --root=example windowing                  # from the repository root
riftc run --root=example windowing -- path/to/project   # with a project directory
riftc build --root=example windowing                # -> example/build/debug/windowing
```

The project directory (the first argument, else the executable's directory) holds
`config/app_settings.cfg` (the main window's title, size, position, vsync and fullscreen, saved
when the program ends and restored at start-up) and `config/bindings.txt` (Ctrl+S).

It needs an X server (X11, or XWayland under Wayland) and at run time `libX11.so.6`, `libGL.so.1`
for the OpenGL windows and `libvulkan.so.1` for the Vulkan one (`libXrandr.so.2` for the list of
monitors). Nothing is linked: std::windowing loads them when they're first needed, so a missing
library is an error value and a log line, not a crash or a link error.

## How it is organized

```
main.rifts                 entry point: logging, the project directory, the application, its layers
engine/application         the application: window system, renderer, main window, layers, the loop
engine/layer               the layer interface, the layer stack, what layers see (context, commands)
engine/renderer            the OpenGL share group, the GL functions, the asset loader thread
engine/settings            window_attributes and serialize_window_attributes (config/app_settings.cfg)
engine/timing              frame stats (delta, total time, fps) and the frame-rate cap
engine/gl, engine/vk       OpenGL functions loaded by reflection; a Vulkan window with its instance
game/game_layer            the world, movement, the player's key bindings, drawing the game
game/tools_layer           the editor's tool windows: inspectors, the sandbox, the Vulkan window
game/debug_overlay         an overlay layer: sees events first, draws last (where an ImGui would go)
game/actions               the action enums with their default keys, rebinding, saving bindings
```

**The loop** (`application::run`): poll the window system (or wait, when a layer said the frame
is idle: the game while paused) → the main window's events through the layers, top to bottom, until
one handles it → every layer's update, bottom to top → what layers asked for through `commands`
(quit, fullscreen) → assets the loader thread finished, uploaded into the share group → one frame
of the main window, every layer rendering bottom to top → the frame clock (fps in the title, the cap).

**Layers** implement `engine::layer`: `on_attach`, `on_detach`, `on_event`, `on_update` and
`on_render` all have defaults, so a layer writes only what it needs. During events and updates a
layer gets `engine::context`: the window system (the tools layer opens windows with it), the main
window, the renderer, the frame stats and `commands`. Rendering gets the frame instead: the frame
borrows the main window while it is drawn, so no layer can resize or close it mid-frame.

**Each layer owns its input**: the game layer reads `game_action`, the tools layer `tool_action`,
each inspector `inspector_action`, all with their own `wnd::bindings`. The debug overlay sits above
them: open (F1), it takes the keyboard and mouse.

**Resources**: the renderer's share group has its own context, so the shared texture is uploaded
with no window involved and outlives every window. Every window asking for the renderer's
OpenGL config joins the group. Vulkan has no share groups: its resources belong to a `VkDevice`,
which presents to any number of window surfaces, so the Vulkan window just has its surface.

## What it shows

| Window | How it's made | What to look at |
|---|---|---|
| main | `sys.create_gl_window(..., sharing::join(renderer.share_group()))`, redrawn every frame | a box you move; the debug overlay (F1) draws into its frame |
| inspectors (Ctrl+N) | tool windows of the main window, `redraw::on_demand`, `sharing::automatic` | the frame stats as bars; they join the renderer's share group by themselves |
| sandbox (Ctrl+Shift+N) | OpenGL 2.1 compatibility, `sharing::independent` | a group of its own |
| Vulkan (Ctrl+K) | `sys.create_vulkan_window(...)`, the program's own VkInstance | title shows the surface size from `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` |

Every OpenGL window has a small marker in its top-right corner: **green** when the texture the
loader thread made exists in that window's context (`glIsTexture`), **red** when it doesn't. The
main window and the inspectors are green, the sandbox is red: that's sharing, visible.

| Key | Action |
|---|---|
| W A S D (arrows after Tab) | move |
| right mouse button (held) | mouse look: the cursor is captured |
| P | pause (the loop then sleeps in `sys.wait_for`) |
| F2 | rebind "pause": press the new key next |
| Tab | swap the whole binding profile |
| Ctrl+S | save the bindings to `config/bindings.txt` (loaded at start-up) |
| F1 | the debug-overlay stand-in: takes input before the game |
| F11 | fullscreen (needs a window manager) |
| Esc | quit (in an inspector: close it; R redraws it) |

## Language features on the way

- **Interfaces with default methods**: `engine::layer`; the layer stack holds
  `std::darray<std::unique_ptr<engine::layer>>` and calls them dynamically.
- **View structs of references**: `engine::context` lends the layers the application's parts for
  one frame; the borrow checker makes sure nothing keeps them.
- **A window's kind is its type**: `gl_window` and `vulkan_window` derive from the `open class
  window`; `begin_frame()` exists only on a `gl_window`.
- **A current context is a value**: `wnd::opengl::frame` is a view that borrows the window; GL code
  takes the frame (or `wnd::opengl::current`) as proof that a context is current. `present()` is a
  consuming method (`move`). Reading `i.refreshes` *before* `begin_frame()` in `tools_layer.rifts` is the
  borrow checker at work: the frame borrows `i` mutably.
- **Bindings are data, actions are types**: `@wnd::default_key` annotations on enum alternatives,
  read by reflection (`std::meta::alternatives`) in `bindings<A>::defaults()`; two actions on the
  same default key is a compile error (try the commented line in `game/actions.rifth`).
- **Reflection-built function tables**: `gl::functions` and `vk::instance_functions` are structs
  whose field names are the C functions; `for compile_time` over `std::meta::fields` loads them.
- **Threads**: the checkerboard is made on a `std::thread` and sent through a `std::channel`;
  `wnd::image` is plain data (sendable), the windows are `std::thread_bound` (moving one into a
  thread doesn't compile).
- **RAII**: closing a window is destroying it (`inspectors.retain(...)`, `sandbox = std::none`);
  the application's fields are declared in the order things are made, so they are destroyed the
  other way round (layers and their windows, main window, renderer, window system);
  `vk::surface_view` declares its fields loader, instance, window, so they're destroyed window
  first, instance second, loader last: the order Vulkan needs, with no destructor code.
