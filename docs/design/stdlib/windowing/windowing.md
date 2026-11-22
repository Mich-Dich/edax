# Windowing: `std::windowing`

`std::windowing` makes windows, reads their input and gives them to a graphics API: an OpenGL context or a Vulkan surface. It is a GLFW-style library written in Edax on the system's own libraries, which it loads when they are first needed, so a program that uses it builds and links like any other. The platform today is X11 (also under XWayland).

## Rules

### The system and the windows
```edax
namespace wnd = std::windowing;

compile_time std::graphics::api gl_33 = std::graphics::api{
    .name = std::graphics::api_name::opengl_core, .version = std::version{ .major = 3, .minor = 3 } };

mut wnd::system sys = wnd::system::start()?;
mut wnd::gl_window win = sys.create_gl_window(wnd::window::config{ .title = "demo" },
                                              wnd::opengl::config{ .api = gl_33 }, wnd::opengl::sharing::automatic)?;
mut std::darray<wnd::event> events = default;
while (!win.should_close()) {
    sys.poll();                                   // every window's events, sorted by window
    win.drain_events(mut events);                 // this window's
    for (wnd::event e : events) { ... }
    mut wnd::opengl::frame frame = win.begin_frame();
    ...                                           // draw (std::graphics::opengl::functions<gl_33>)
    frame.present();
}
```
- `wnd::system::start()` connects to the window system: `error::no_display` without one, `error::already_started` while another system or its windows live. It lists `monitors()` and makes the windows.
- `sys.poll()` reads the system's events once and sorts them into the windows' queues; `sys.wait()` and `sys.wait_for(duration)` sleep until an event comes (an idle program costs nothing).
- **A window's kind is its type:**

| Made by | Type | Graphics |
|---|---|---|
| `sys.create_window(config)` | `wnd::window` | None |
| `sys.create_gl_window(config, gl, sharing)` | `wnd::gl_window` | An OpenGL context |
| `sys.create_vulkan_window(config, api)` | `wnd::vulkan_window` | A Vulkan surface |

  `gl_window` and `vulkan_window` derive from the `open class window`, so code that handles input or titles takes a `window&`. `begin_frame()` exists only on a `gl_window`.
- `window::config` says how a window starts: `title`, `size`, `position` (none: the window manager decides), `resizable`, `decorated`, `visible`, `kind` (`normal`, `tool(owner)`: stays above its owner with no taskbar entry, `popup(owner)`: borderless) and `redraw` (`continuous`, or `on_demand`: drawn only after `request_redraw()`, an expose or a resize).
- `window.graphics()` is the `std::graphics::api` a window was made for, or none.
- Each window has its own event queue, input state, cursor, swap interval and redraw policy. Destroying a window closes it.

### Graphics APIs
The API is a [`std::graphics::api`](../graphics/graphics.md): a name and a version, the same value for both kinds of window.
- **OpenGL:** `wnd::opengl::config{ .api = gl_33 }` with `debug`, `depth_bits`, `stencil_bits`, `samples` and `srgb`. `opengl_core`, `opengl_compatibility` and `opengl_es` are allowed; `error::version_unavailable` when the driver can't make that version, `error::wrong_api` for a Vulkan api.
- **Vulkan:** `sys.create_vulkan_window(config, vk_13)`. `error::version_unavailable` when the loader's newest version is older than the api's, `error::wrong_api` for an OpenGL api. The program makes the instance and the device; the window makes and owns the surface (`create_surface(instance)`, `unsafe` because the instance must outlive the window) and answers `presentation_support(instance, physical_device, family)`.
- **Where the functions come from:** a `frame` (or a share group's `current`) and the `vulkan::loader` (`sys.vulkan()`) are `std::graphics::function_source`s, what `std::graphics::opengl::functions<A>::load` and `std::graphics::vulkan::functions<A>::load` take. The loader also tells its newest Vulkan `version()` and the instance extensions a surface needs (`required_extensions()`).

### A current OpenGL context is a value
- `gl_window.begin_frame()` makes the window's context current and returns a `frame`: a `view` that borrows the window mutably, so it can't be closed, resized or moved while it is drawn. `frame.present()` swaps the buffers and consumes the frame. GL code takes the frame as the proof that a context is current.
- One frame (or `current`) per thread at a time, checked in debug builds.
- `set_swap_interval(n)`: how many refreshes a `present()` waits for (1 is vsync), per window.

### Share groups
OpenGL objects (textures, buffers, programs) live in a context; contexts in the same **share group** see the same objects.
- `wnd::opengl::sharing` says which group a new window joins: `automatic` (a group with an equal config, else a new one), `join(group)` (that group; `error::share_incompatible` if its config differs), `independent` (a new group of its own).
- `sys.create_share_group(gl)` makes a group with a hidden context of its own: `group.make_current()` returns an `opengl::current`, so objects can be made before any window exists and outlive every window. A `share_group` is a counted handle (`clone()`); the group ends with its last owner.
- Vulkan has no share groups: its resources belong to a `VkDevice`, which presents to any number of surfaces.

### Input
- **Events** are plain data (`wnd::event`): `close_requested`, `key`, `text`, `mouse_button`, `cursor_moved`, `cursor_entered`, `scrolled`, `resized`, `framebuffer_resized`, `moved`, `focus_changed`, `minimized`, `redraw_requested`. `window.drain_events(out)` moves a window's events into a darray the program owns and starts a new input frame.
- **Input state** (`window.input()`) is the keyboard and mouse as they are now: `key_down`, `key_pressed`, `key_released`, `mouse_down`, ..., `cursor`, `cursor_delta`, `scroll`, `text`, `modifiers`. "This frame" is between two `drain_events`.
- Keys are named by their place on a US layout (`wnd::key::w` is left of `e` on any layout); the character typed comes as `event::text`.
- `set_cursor_mode(captured)` hides the cursor and holds it in the window while `cursor_delta()` keeps counting: mouse look.
- **Bindings:** `wnd::bindings<A>` binds the program's actions (an enum) to keys and buttons. The defaults are written on the enum with `@wnd::default_key{ ... }` and `@wnd::default_button{ ... }`; two actions on the same default chord are a compile error. Bindings change at run time (`bind`, `unbind`, `reset`), are looked up per event (`action_for(e)`) or per state (`is_down`), and save as text (`to_text`, `from_text`).
- **Input layers:** `wnd::route(events, layer, input)` offers each event to a `wnd::input_layer` (a debug UI, a console) and removes what it consumes.

### Windows and the screen
`set_title`, `set_icon(image)`, `set_cursor(shape)`, `enter_fullscreen(monitor)` / `leave_fullscreen()`, `show`, `hide`, `minimize`, `focus`, `set_size`, `set_position`; `size()` is in screen units and `framebuffer_size()` in pixels (they differ on HiDPI screens), `content_scale()` is the ratio.

### Threads and libraries
- The system, the windows and the share groups are `std::thread_bound`: they stay on the thread that made them. Events, keys and images are plain data and go anywhere.
- Libraries are loaded when first needed: `libX11.so.6` when the system starts, `libXrandr.so.2` for the monitors (one monitor without it), `libGL.so.1` for the first OpenGL window or share group, `libvulkan.so.1` for Vulkan. A missing library is `error::library_missing` (or `opengl_unavailable`, `vulkan_unavailable`), never a crash or a link error.

## Rationale
- **The kind in the type:** an OpenGL call on a Vulkan window, or presenting a window with no context, doesn't compile. Code that only needs the common part takes a `window&`.
- **The frame as a value:** a current context is global state in C. As a value that borrows the window, the borrow checker enforces what GLFW only documents: nothing closes or resizes a window while it is drawn, and GL code can ask for proof that a context is current.
- **The API as one value:** the window's version and the functions' version come from the same `std::graphics::api`, so they can't disagree.
- **Loading libraries on first use:** a program runs on machines without OpenGL or Vulkan as long as it doesn't ask for them, and gets an error value when it does.
