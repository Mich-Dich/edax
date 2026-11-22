# Graphics APIs: `std::graphics`

A program says which graphics API it uses with two things: the API's **name** and its **version**. The same value makes the window ([Windowing](../windowing/windowing.md)) and picks the API's functions. The functions aren't written by hand: edaxc makes them from the **Khronos registries** (`gl.xml`, `vk.xml`), which describe every command, constant and type of every version of OpenGL, OpenGL ES and Vulkan.

## Rules

### An API is a name and a version
```edax
namespace std::graphics {
    enum api_name : u8 { opengl_core, opengl_compatibility, opengl_es, vulkan }

    struct api {
        api_name     name = api_name::opengl_core;
        std::version version = std::version{ .major = 3, .minor = 3 };
    }
}
```
```edax
compile_time std::graphics::api gl_33 = std::graphics::api{
    .name = std::graphics::api_name::opengl_core, .version = std::version{ .major = 3, .minor = 3 } };
compile_time std::graphics::api vk_14 = std::graphics::api{
    .name = std::graphics::api_name::vulkan, .version = std::version{ .major = 1, .minor = 4, .patch = 320 } };
```
- The version is a [`std::version`](../core/utilities.md#versions). An `api` prints as `OpenGL Core 3.3.0`, `Vulkan 1.4.320`.
- The names are the API families: `opengl_core` (the core profile, 3.2 and later), `opengl_compatibility` (the compatibility profile, every version), `opengl_es` (2.0 and later) and `vulkan`.
- `std::graphics::name_text(name)` is the family's name as text, `std::graphics::is_opengl(name)` whether it is one of the OpenGL families.
- An `api` is declared `compile_time` and used everywhere the program needs it: the window's config and the functions' template argument. The program states its API once.

### The functions: `std::graphics::opengl` and `std::graphics::vulkan`
Each API family has a module that edaxc writes from its registry:

| Module | Registry | Contents |
|---|---|---|
| `std::graphics::opengl` | `gl.xml` | Every `GL_*` constant of OpenGL 1.0-4.6 and OpenGL ES 2.0-3.2; `functions<A>`, with a method for every command |
| `std::graphics::vulkan` | `vk.xml` | The constants, types and structs of Vulkan 1.0-1.4 and the extensions `VK_KHR_surface`, `VK_KHR_swapchain`, `VK_KHR_xlib_surface` and `VK_EXT_debug_utils`; `functions<A>` |

```edax
namespace gl = std::graphics::opengl;

mut std::windowing::opengl::frame frame = win.begin_frame();
gl::functions<gl_33> f = gl::functions<gl_33>::load(frame)?;
unsafe {
    f.glClearColor(0.1, 0.2, 0.3, 1.0);
    f.glClear(gl::GL_COLOR_BUFFER_BIT);
}
frame.present();
```
- `functions<A>` takes the program's `api` as its template argument (a struct value parameter, see [Templates](../../language/compile-time/templates.md)). A different version is a different type: a program with an OpenGL 3.3 window and an OpenGL 2.1 window has `functions<gl_33>` and `functions<gl_21>`.
- A module is made the first time a program names it, and only then. It is loaded like the rest of `std` (`import std;`).
- `edaxc registry opengl` and `edaxc registry vulkan` print the module's header: every declaration, with the versions that have each command.

### Versions are checked when the program is compiled
Every method has an entry check (see [Checks](../../language/errors/checks.md)) that compares `A` with the versions that have the command. A command the program's version doesn't have is a **compile error at the call**, naming the versions that have it:
```edax
compile_time std::graphics::api gl_21 = std::graphics::api{
    .name = std::graphics::api_name::opengl_compatibility, .version = std::version{ .major = 2, .minor = 1 } };

void setup(gl::functions<gl_21>& f) {
    mut u32 vao = 0;
    unsafe { f.glGenVertexArrays(1, mut &vao); }
    // error: glGenVertexArrays isn't in OpenGL Compatibility 2.1: it is in OpenGL Core 3.2+,
    //        OpenGL Compatibility 3.0+, OpenGL ES 3.0+
}
```
- A command removed from the core profile is an error there too: `glBegin` in `functions<gl_33>` "isn't in OpenGL Core 3.3: it is in OpenGL Compatibility 1.0+".
- An API that doesn't exist is an error where `functions<A>` is named: "OpenGL Core 3.7.0 doesn't exist; the versions are 3.2, 3.3, 4.0, ...". OpenGL versions have patch 0; a Vulkan patch may be up to the registry's header version.
- `std::graphics::opengl::functions<A>` with a Vulkan `A` (and the other way round) is an error that names the right module.

### Loading
`load` looks up exactly the commands `A` has, from a **function source**:
```edax
namespace std::graphics {
    interface function_source {
        std::option<std::raw_ptr<u8>> proc_address(std::str name);
    }
}
```
- An OpenGL context that is current is a source: `std::windowing::opengl::frame` and `opengl::current`. On GLX the addresses are the same in every context, so functions loaded once serve every window of the API.
- The Vulkan loader is a source: `std::windowing::vulkan::loader`. Vulkan loads in its usual three steps:

| Step | Loads |
|---|---|
| `functions<A>::load(loader)` | `vkGetInstanceProcAddr` and the global commands (`vkCreateInstance`, `vkEnumerateInstance*`) |
| `f.load_instance(instance) mut` | The instance's commands, and its devices' through the instance |
| `f.load_device(device) mut` | Optional: the device's commands, called without the instance's dispatch |

- `load` and `load_instance` return `std::result<_, std::string>`: the error is the name of a command of version `A` that the driver doesn't have.
- An extension's command may be missing (the instance didn't enable the extension): it stays unloaded, and calling it panics with its name. So does calling a command whose step hasn't run.

### What the registries become
Names are the registry's own: `glTexImage2D`, `GL_TEXTURE_2D`, `vkCreateInstance`, `VkInstanceCreateInfo`, `VK_SUCCESS`. They are the names of the specifications, every tutorial and the drivers' messages.

| In the registry | In Edax |
|---|---|
| `GLenum`, `GLbitfield`, `GLuint` / `GLint`, `GLsizei` / `GLfloat` / `GLboolean` | `u32` / `i32` / `f32` / `u8` |
| A `GLint` parameter that holds an enum (`glTexImage2D`'s `internalformat`) | `u32`: the `GL_*` constant, its bits passed on |
| `GL_*` constants | `u32` (`u64` for the 64-bit ones, `u8` for `GL_TRUE` / `GL_FALSE`) |
| `const T*` / `T*` | `std::raw_ptr<T>` / `std::raw_ptr<mut T>`; `void*` is `std::raw_ptr<mut u8>` |
| Vulkan handles (`VkInstance`, `VkBuffer`, ...) | `u64` |
| Vulkan enums / flags | `i32` / `u32` (`u64` for 64-bit flags); their values are constants of that type |
| Vulkan structs | Structs with C layout and the registry's field names; `sType` is already set |
| Vulkan unions (`VkClearValue`) | A struct of the union's size and alignment (`data`); the members are listed in the header |
| Function pointers (`PFN_*`, `GLDEBUGPROC`) | `std::raw_ptr<u8>` |
| Fixed arrays (`char deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE]`) | `std::raw::array<u8, 256>` |

```edax
namespace vk = std::graphics::vulkan;

vk::VkApplicationInfo application = vk::VkApplicationInfo{ .apiVersion = vk::make_version(vk_14.version) };
vk::VkInstanceCreateInfo info = vk::VkInstanceCreateInfo{
    .pApplicationInfo        = &application,
    .enabledExtensionCount   = 2,
    .ppEnabledExtensionNames = cast::reinterpret<std::raw_ptr<std::raw_ptr<u8>>>(&extensions),
};
```
- `vk::make_version(v)` is `VK_MAKE_API_VERSION(0, major, minor, patch)`; `vk::version_of(packed)` turns a packed version back into a `std::version`.

### The methods are `unsafe`
A graphics API trusts every handle, pointer and size it is given, and an OpenGL command needs the context it was loaded for to be current. The methods are therefore `unsafe` (see [Raw Pointers and `unsafe`](../../language/memory/raw-pointers-and-unsafe.md)). Safe wrappers (a texture that deletes itself, a frame that is the proof of a current context) are written on top, as `std::windowing` does for contexts.

### The registries
The registries are files of the library: `library/std/graphics/registry/gl.xml` and `vk.xml`, embedded in edaxc when it is built. A new version of OpenGL or Vulkan is a new registry file, not new code: the modules are made from whatever the registry describes.

## Rationale
- **A name and a version, nothing else:** "OpenGL Core 3.3" and "Vulkan 1.4.320" are how people say which API they use. One value states it once; the window and the functions can't disagree about the version, because they read the same constant.
- **Generated from the registries:** hand-written bindings for every version of OpenGL and Vulkan are thousands of declarations that go out of date with each release. The Khronos registries are the source the C headers themselves are generated from; reading them keeps the bindings complete and correct by construction.
- **Version errors at compile time:** the classic failure is a program that calls a function the context's version doesn't have and crashes on a null pointer on some machines. The registry knows which version has each command, so the compiler can refuse the call, and say which version would have it.
- **Every command declared, the version checked per call:** declaring only the commands of one version would need a generated type per version, which the language doesn't have (see [Metaprogramming](../../language/compile-time/metaprogramming.md)). One class per family with checked methods gives the same errors, and `load` still looks up only what the version has.
- **The registry's names:** renaming `glTexImage2D` to `tex_image_2d` would make every tutorial, specification and driver message harder to use. The names stay; only the types become Edax's.
- **Handles as `u64`:** Vulkan's handles are 64-bit values on every platform Edax targets; one integer type keeps them interchangeable with `std::windowing` (`create_surface(u64 instance)`).
