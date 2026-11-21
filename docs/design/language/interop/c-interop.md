# C and C++ Interop

Edax calls C and C can call Edax through `extern(std::build::language::c)` declarations. Every call into C is `unsafe`. C headers are turned into Edax headers by a build step, and C++ classes are reached through generated C wrappers. C/C++ code is compiled and linked by the build system (see [Edax as a Build System](../../build/build-system.md)).

## Rules

### `extern(language)`
`extern` takes the language as a parameter, the same way `run_time(std::build::mode::debug)`
takes a build mode (see [Checks](../errors/checks.md)). The language is an alternative of `std::build::language` (see [Edax as a Build System](../../build/build-system.md)),
written out in full like every std name: `extern(std::build::language::c)`. There is no implicit namespace; a shorter name is a
`compile_time` constant the user declares: `compile_time std::build::language c = std::build::language::c;` then `extern(c)`.
```edax
extern(std::build::language::c) i32 puts(std::raw_ptr<u8> text);          // one function

extern(std::build::language::c) {                                         // several at once
    std::raw_ptr<mut u8> malloc(usize size);
    void free(std::raw_ptr<mut u8> p);
}

unsafe {
    i32 written = puts(message.as_ptr());
}
```
- **Without a body**, an `extern(std::build::language::c)` function is **imported**: it is defined in C and is always `unsafe` to call.
- **With a body**, it is **exported**: it is Edax code that C can call under its plain name (`int is_even(int)` for `extern(std::build::language::c) bool is_even(i32 x) { ... }`). Edax code calls it like any function, without `unsafe`. A header can declare it and the source define it, as for any function (see [Headers, Sources and Modules](../organization/modules.md)).
- An `extern` block applies its modifiers (`private`, `unsafe`) to every declaration inside; it holds only function declarations.
- Their parameters and results are numbers, `bool`, `char`, simple enums, `std::raw_ptr`, `std::fn_ptr` (C callbacks) and structs whose fields are all of these. Structs have C's layout (see [Structs](../user-types/structs.md)) and are passed by value following the platform's C calling convention.
- `text.as_ptr()` gives the bytes of a `std::str` as a `std::raw_ptr<u8>`. A `std::str` is not NUL-terminated; a literal for C is written `c"..."` (see [Lexical Structure](../basics/lexical.md)), which is.
- `extern` only accepts `std::build::language::c`. C++ is reached through generated C wrappers (below), never through C++'s own ABI.
- If a foreign function can throw a C++ exception, it must be wrapped so the exception can't cross into Edax. Letting one through aborts the program.

### Importing C headers
A build step turns C headers into a Edax header of `extern` declarations:
```edax
g.import_c_headers(renderer, std::build::c_import{
    .headers   = ["vulkan/vulkan.h"],
    .module    = "vulkan_c",                      // the generated vulkan_c.edh
    .namespace = "vk",                            // its declarations go into namespace vk
});
```
```edax
import vulkan_c;
unsafe {
    vk::VkResult r = vk::vkCreateInstance(mut &info, nullptr, mut &instance);
}
```
| C declaration | Becomes |
|---|---|
| Function | An `extern(std::build::language::c)` declaration |
| `struct` | A struct with C's layout |
| `union` | A struct with the union's size and alignment; its members are read and written through `unsafe` accessor methods |
| `enum` | A simple enum with the same underlying type |
| `typedef` | A `compile_time type` alias (see [Metaprogramming](../compile-time/metaprogramming.md)) |
| `#define` of a number or string literal | A `compile_time` constant |
| Function pointer type | `std::fn_ptr<R(A...)>` |
| Function-like macro, `inline` function, bit field | Not translated; listed in the step's report with the reason |

- The step runs the system's C compiler front end on the headers with the target's include directories and defines, so the result matches what C code in the same target sees.
- The generated header is an ordinary file in the build directory; `edaxc expand` isn't needed to read it.

### C++ classes through generated wrappers
C++ can't be called directly. `g.wrap_cpp` generates, for the classes it is given, C wrapper functions (compiled into the C++ target) and a Edax header with a class per C++ class:
```edax
g.wrap_cpp(physics, std::build::cpp_wrap{
    .headers   = ["jolt/Physics/PhysicsSystem.h"],
    .classes   = ["JPH::PhysicsSystem", "JPH::BodyInterface"],
    .templates = ["JPH::Ref<JPH::Shape>"],             // template instances to wrap, written out
    .module    = "jolt",
    .namespace = "jolt",
});
```
- Each wrapped class becomes a Edax `class` that owns the C++ object through a pointer: its constructors become `static` `make` functions, its destructor the Edax destructor, its public methods methods (`const` methods are non-`mut`, others `mut`).
- Wrapped calls are safe to call from Edax: the generated wrapper is the `unsafe` boundary, checked once by the generator.
- **Exceptions:** every wrapper catches C++ exceptions. A method that may throw returns `std::result<T, std::cpp_exception>` (with the exception's `what()` text); one declared `noexcept` returns `T`.
- Templates are wrapped only for the instances listed. Operators, overloaded functions and default arguments get distinct Edax names, listed in the step's report.


## Rationale
- **Generated headers instead of hand-written ones:** large C APIs (Vulkan, GLFW) are usable at once and stay in sync with the C headers. What can't be translated faithfully is reported, never guessed.
- **C++ only through C wrappers:** C++'s ABI (name mangling, exceptions, templates) differs per compiler. A generated C layer keeps the boundary simple and makes exceptions into ordinary error values.
- **`extern(std::build::language::c)`, not `extern "C"`:** the language is a name the language already has (`std::build::language::c`), not a string the compiler has to recognize. It reads like the other modifiers with a parameter (`run_time(std::build::mode::debug)`), a typo is a compile error instead of an unknown string, and the block form removes the repetition of long C headers.
- **Full names inside the parentheses:** a bare `extern(c)` or `run_time(debug)` would make the compiler look the name up in a namespace the code never mentions. Writing the full name keeps principle 1 (explicit) and the rule that std names are always `std::`-qualified (see [The Standard Library](../../stdlib/README.md)).
