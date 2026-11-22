# windowing: several windows, OpenGL and Vulkan

A program on [`std::windowing`](../../docs/design/stdlib/windowing/windowing.md) that shows what the
window system can do, with OpenGL and Vulkan from [`std::graphics`](../../docs/design/stdlib/graphics/graphics.md).

```bash
edaxc run --root=example windowing          # from the repository root
edaxc build --root=example windowing        # -> example/build/debug/windowing
```

It needs an X server (X11, or XWayland under Wayland) and at run time `libX11.so.6`, `libGL.so.1`
for the OpenGL windows and `libvulkan.so.1` for the Vulkan one (`libXrandr.so.2` for the list of
monitors). Nothing is linked: std::windowing loads them when they're first needed, so a missing
library is an error value and a log line, not a crash or a link error.

## How it is organized

```
main.ed              the entry point: starts the log, makes the application, runs it
application.edh/.ed  the application: the windows, their input, what they draw
std::windowing       the windows themselves (the standard library)
std::graphics        OpenGL's and Vulkan's functions, made from the Khronos registries
```

The graphics APIs are three constants in `application.edh`, each a name and a version:

```edax
compile_time std::graphics::api opengl = std::graphics::api{
    .name = std::graphics::api_name::opengl_core, .version = std::version{ .major = 3, .minor = 3 } };
compile_time std::graphics::api opengl_legacy = std::graphics::api{
    .name = std::graphics::api_name::opengl_compatibility, .version = std::version{ .major = 2, .minor = 1 } };
compile_time std::graphics::api vulkan = std::graphics::api{
    .name = std::graphics::api_name::vulkan, .version = std::version{ .major = 1, .minor = 3 } };
```

The same constant makes a window (`wnd::opengl::config{ .api = opengl }`,
`sys.create_vulkan_window(config, vulkan)`) and picks the functions
(`std::graphics::opengl::functions<opengl>`, `std::graphics::vulkan::functions<vulkan>`). Calling a
command the version doesn't have, like `glBegin` on `opengl`, is a compile error.

**The loop** (`application::run`): poll the window system once for every window (or wait, while
paused) → the main window's events through the key bindings, then the box → every other window's
events → draw the main window → draw the tool windows that need it → frames per second in the title.

## What it shows

| Window | How it's made | What to look at |
|---|---|---|
| main | `sys.create_gl_window(..., sharing::join(group))`, OpenGL Core 3.3, redrawn every frame | a box you move with the keys and the mouse |
| inspectors (Ctrl+N) | tool windows of the main window, `redraw::on_demand`, `sharing::automatic` | fps and the box's place as bars, redrawn once a second |
| sandbox (Ctrl+Shift+N) | OpenGL Compatibility 2.1, `sharing::independent` | a share group of its own, with its own `functions<opengl_legacy>` |
| Vulkan (Ctrl+K) | `sys.create_vulkan_window(..., vulkan)`, the program's own VkInstance | the title shows the surface size from `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` |

Every OpenGL window has a small marker in its top-right corner: **green** when the texture made in
the share group's own context exists in that window's context (`glIsTexture`), **red** when it
doesn't. The main window and the inspectors are green, the sandbox is red: that's sharing, visible.

| Key | Action |
|---|---|
| W A S D or the arrows | move the box |
| right mouse button (held) | move the box with the mouse: the cursor is captured |
| mouse wheel | resize the box |
| P | pause: the loop sleeps in `sys.wait_for` |
| F11 | fullscreen (needs a window manager) |
| Ctrl+N | open an inspector (Esc in it closes it) |
| Ctrl+Shift+N | open or close the sandbox |
| Ctrl+K | open or close the Vulkan window |
| Esc | quit |

## Language features on the way

- **A window's kind is its type:** `gl_window` and `vulkan_window` derive from the `open class
  window`; `begin_frame()` exists only on a `gl_window`.
- **A current context is a value:** `wnd::opengl::frame` borrows the window while it is drawn;
  `present()` consumes it. `sandbox_window::draw` shows why the sandbox is a class: the frame
  borrows `this.win` while `this.gl` stays usable.
- **Struct values as template arguments:** `std::graphics::opengl::functions<opengl>`; the drawing
  helpers are `template<std::graphics::api A>` functions that work for both OpenGL versions, with A
  deduced from the argument.
- **Bindings are data, actions are types:** `@wnd::default_key` annotations on the `action` enum,
  read by `wnd::bindings<action>::defaults()`; two actions on the same default key are a compile error.
- **RAII:** closing a window is destroying it (`inspectors.retain(...)`, `sandbox = std::none`).
  `vulkan_view` declares its fields loader, instance, window, so they're destroyed window (and its
  surface) first, instance second, loader last: the order Vulkan needs, with no destructor code.
