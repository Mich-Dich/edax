# Dynamic Libraries and Plugins

A program can use code from shared libraries in three ways:

| Kind | Loaded | Declared in `build.ed` | Used through |
|---|---|---|---|
| **Shared library** | When the program starts, by the OS | `g.add_shared_library(...)` of an `edax_target` or a `c_target`, then `g.link` | Ordinary imports and calls, as for a static library |
| **Edax plugin** | At run time, chosen by the program | `g.add_plugin(...)` | `std::plugin<I>`: an object implementing an interface `I` |
| **C library** | At run time, chosen by the program | Nothing (any C shared library) | `std::c_library`: named C functions, called in `unsafe` |

A program may load **several plugins of the same interface** (renderers, importers, scripting back ends) and any number of C libraries, side by side.

## Rules

### Edax plugins: `std::plugin<I>`
A plugin provides one object implementing an interface `I` that the program and the plugin both import.

**In the plugin** (a target added with `g.add_plugin`):
```edax
import renderer_api;                       // declares interface renderer_api::renderer

namespace vulkan_backend {
    @std::plugin_entry
    class renderer : renderer_api::renderer {
    public:
        void draw(renderer_api::frame& f) mut override { ... }
        static renderer make() { ... }
    }
}
```
- Exactly one class is marked `@std::plugin_entry`. It implements the plugin's interface and has a `static` method `make()` that the loader calls.

**In the program:**
```edax
std::result<std::plugin<renderer_api::renderer>, std::plugin_error> loaded =
    std::plugin<renderer_api::renderer>::load(std::filesystem::executable_dir() / "plugins" / "vulkan.so");

mut std::plugin<renderer_api::renderer> vulkan = loaded?;
vulkan->draw(frame);                        // the plugin's object, through the interface (dynamic dispatch)
```
- **`std::plugin<I>::load(path)`** is a static function of the type that describes what is loaded, so the result's type says what the library must provide.
- Loading checks that the library's entry class implements **this** `I`, built from the same declaration of `I` (a hash of the interface: its methods and their types), and with the same `edaxc` version. Any mismatch is `std::error(std::plugin_error::incompatible)` with the reason, never a crash at the first call.
- `*p` / `p->` give the object as `I&` (`mut I&` through a `mut` plugin). These are references into the plugin, so they can't outlive it (see [Ownership, Copies, Moves and References](../../language/memory/ownership-and-references.md)).
- **Destroying the `std::plugin` destroys its object and unloads the library.** Values the plugin returned that own memory from the plugin's code must be destroyed first; the borrow rules enforce this for everything that borrows the plugin.
- Edax types (strings, containers, results) can cross the boundary, because both sides are built from the same modules by the same `edaxc`.
- Errors: `std::plugin_error` is `not_found`, `not_a_plugin`, `incompatible`, `platform_error`.

### Shared Edax libraries
```edax
std::build::target core = g.add_shared_library(std::build::edax_target{ .name = "core", .root = "core/core.ed" });
std::build::target app  = g.add_executable(std::build::edax_target{ .name = "app", .root = "app/main.ed" });
g.link(app, core);              // app imports core's modules; their code is in libcore.so
```
- The library's modules are those in the directory of its `.root` (and below). Their code is compiled once, into `lib<name>.so` in the programs' directory; programs find it there (`$ORIGIN`).
- Everything the modules define that isn't generic is in the library: functions, methods, `static` variables, vtables and type ids, so there is one of each (`cast::dynamic` works on objects made on either side). Its `static` variables are initialized when the library is loaded.
- **Generic code is made where it is used**: `core::biggest<u32>` or `std::darray<core::item>` are instantiated in the program, as for a static library, since the library can't know its users' type arguments.
- Each side has its own copy of the standard library's code, as a plugin has; Edax types cross the boundary because both sides are built from the same modules by the same `edaxc`, in the same mode.
- With shared libraries in the program, whole-program devirtualization is off (the library's code doesn't know the program's classes).
- A failed entry check of a library function names the caller's line for calls inside the library; for calls from a program it names the function only.

### C libraries: `std::c_library`
```edax
mut std::c_library vk = std::c_library::load("libvulkan.so.1")?;
std::c_library::function<std::raw_ptr<u8>(std::raw_ptr<u8>, std::raw_ptr<u8>)> get_proc =
    vk.function<std::raw_ptr<u8>(std::raw_ptr<u8>, std::raw_ptr<u8>)>(c"vkGetInstanceProcAddr")?;
unsafe {
    std::raw_ptr<u8> create = get_proc(nullptr, c"vkCreateInstance");
}
```
- `std::c_library::load(name)` uses the OS's search rules (a file name) or a path.
- `lib.function<R(A...)>(c"symbol")` looks up a C function by name and returns a `std::c_library::function<R(A...)>`. It is a **view** of the library, so it can't outlive it, and calling it is `unsafe`: the compiler can't check that the C function has that signature. Parameter and result types follow the C interop rules (see [C and C++ Interop](../../language/interop/c-interop.md)).
- A missing symbol is `std::error(std::c_library_error::symbol_not_found)`.
- Destroying the `std::c_library` unloads it.

### Hot reload
Any plugin can be replaced while the program runs:
```edax
mut std::plugin<game_api::game> game = std::plugin<game_api::game>::load(path)?;
...
game.reload(path)?;          // the new build of the same plugin, with the state carried over
```
- `reload` checks the new library like `load` (same interface hash, same edaxc) and makes the new build's object with its `make()`. On any error the old build stays loaded and in use.
- If the `@std::plugin_entry` class is also marked `@std::hot_reload_state`, its state is carried over **field by field by name**, using the layout table (name, offset, size, type hash) edaxc puts in each build's descriptor:
  - a field both builds have gets the old value; a field only the new build has keeps the value `make()` gave it; a field only the old build had is destroyed with the old object;
  - a field whose type changed refuses the reload: `std::plugin_error::state_changed`, before anything moved.
  - The values are moved, not copied: the old object, holding the new build's defaults, is destroyed by the old build, then the old library is unloaded.
- Only the entry object's own fields are migrated. A field's type is compared as a whole (its hash covers its layout), so a changed struct inside it counts as a changed type. Function pointers and interface pointers into the old library must not be kept in that state; everything else the plugin rebuilds itself.
- Values that borrow the old library must be gone first; the borrow rules enforce it, as for unloading.
- `std::filesystem::watch(dir)` reports changed files below a directory (`changes()` without waiting, `wait(ms)`), so a program can rebuild and reload when a source changes.

### Requirements
- The target must have the `dynamic_libraries` capability (see [Edax as a Build System](../../build/build-system.md)); otherwise `load` is a compile error.
- `std::plugin` and `std::c_library` are `std::sendable` but not `std::shareable`: one thread owns a loaded library at a time.

## Rationale
- **Loading lives on the type that is loaded:** `std::plugin<I>::load` and `std::c_library::load` say *what* you get, not just "a library". A Edax plugin is checked against its interface when it's loaded, so a wrong plugin is an error value, not undefined behaviour.
- **Two run-time kinds:** C libraries need nothing from their authors and stay unchecked, so they're `unsafe` at every call. Edax plugins carry their interface's identity, so calls into them are safe and fully typed.
- **Unloading by destruction, protected by borrows:** an unloaded library is a classic source of crashes (a function pointer into freed code). Making everything taken from a library a view of it turns that into a compile error.
