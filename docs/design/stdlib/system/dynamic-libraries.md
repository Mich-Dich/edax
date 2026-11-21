# Dynamic Libraries and Plugins

A program can use code from shared libraries in three ways:

| Kind | Loaded | Declared in `build.rifts` | Used through |
|---|---|---|---|
| **Shared library** | When the program starts, by the OS | `g.add_shared_library(...)` / a shared `c_target`, then `g.link` | Ordinary imports and calls, as for a static library |
| **Rift plugin** | At run time, chosen by the program | `g.add_plugin(...)` | `std::plugin<I>`: an object implementing an interface `I` |
| **C library** | At run time, chosen by the program | Nothing (any C shared library) | `std::c_library`: named C functions, called in `unsafe` |

A program may load **several plugins of the same interface** (renderers, importers, scripting back ends) and any number of C libraries, side by side.

## Rules

### Rift plugins: `std::plugin<I>`
A plugin provides one object implementing an interface `I` that the program and the plugin both import.

**In the plugin** (a target added with `g.add_plugin`):
```rift
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
```rift
std::result<std::plugin<renderer_api::renderer>, std::plugin_error> loaded =
    std::plugin<renderer_api::renderer>::load(std::filesystem::executable_dir() / "plugins" / "vulkan.so");

mut std::plugin<renderer_api::renderer> vulkan = loaded?;
vulkan->draw(frame);                        // the plugin's object, through the interface (dynamic dispatch)
```
- **`std::plugin<I>::load(path)`** is a static function of the type that describes what is loaded, so the result's type says what the library must provide.
- Loading checks that the library's entry class implements **this** `I`, built from the same compiled interface of `I`'s module (a hash), and with the same `riftc` version. Any mismatch is `std::error(std::plugin_error::incompatible)` with the reason, never a crash at the first call.
- `*p` / `p->` give the object as `I&` (`mut I&` through a `mut` plugin). These are references into the plugin, so they can't outlive it (see [Ownership, Copies, Moves and References](../../language/memory/ownership-and-references.md)).
- **Destroying the `std::plugin` destroys its object and unloads the library.** Values the plugin returned that own memory from the plugin's code must be destroyed first; the borrow rules enforce this for everything that borrows the plugin.
- Rift types (strings, containers, results) can cross the boundary, because both sides are built from the same compiled interfaces.
- Errors: `std::plugin_error` is `not_found`, `not_a_plugin`, `incompatible`, `platform_error`.

### C libraries: `std::c_library`
```rift
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

### Requirements
- The target must have the `dynamic_libraries` capability (see [Rift as a Build System](../../build/build-system.md)); otherwise `load` is a compile error.
- `std::plugin` and `std::c_library` are `std::sendable` but not `std::shareable`: one thread owns a loaded library at a time.

## Rationale
- **Loading lives on the type that is loaded:** `std::plugin<I>::load` and `std::c_library::load` say *what* you get, not just "a library". A Rift plugin is checked against its interface when it's loaded, so a wrong plugin is an error value, not undefined behaviour.
- **Two run-time kinds:** C libraries need nothing from their authors and stay unchecked, so they're `unsafe` at every call. Rift plugins carry their interface's identity, so calls into them are safe and fully typed.
- **Unloading by destruction, protected by borrows:** an unloaded library is a classic source of crashes (a function pointer into freed code). Making everything taken from a library a view of it turns that into a compile error.
