# Namespaces

Namespaces work like C++'s: `namespace a::b { ... }` blocks, reopenable, nestable, and independent of files. What's different is name lookup. In C++, code can **change what someone else's code calls** without touching it: through argument-dependent lookup, `using namespace`, a declaration in an inner scope, or by adding to `namespace std`. Rift's lookup rules make every one of those either impossible or a compile error.

**The rule behind the rules:** *adding a declaration anywhere can cause a compile error, but it can never silently change what existing code calls.*

## Rules

### 1. Declaring namespaces
```rift
namespace gfx::vulkan {
    class device { ... }
    device open_device(u32 index);
}

namespace gfx {
    namespace vulkan {                 // the same namespace as above
        void shutdown();
    }
}
```
- `namespace a::b { ... }` is short for `namespace a { namespace b { ... } }`. No `;` after the `}` (see [Lexical Structure](../basics/lexical.md)).
- A namespace can be **reopened** in any file of the same project (section 6), in headers and sources alike.
- Declarations outside any `namespace` block are in the **global namespace**, written `::name` when it has to be explicit. Only a program's own files may declare global names; a library must put everything in a namespace (section 6).
- A name is **either** a namespace or a declaration in its parent, never both: `namespace gfx::window { }` next to `class window` in `gfx` is an error.
- There are no anonymous namespaces (`private` does that job, see [Headers, Sources and Modules](modules.md)) and no inline namespaces.

### 2. Namespaces and modules are separate
- A **module** (a `.rifth`/`.rifts` pair, see [Headers, Sources and Modules](modules.md)) is a unit of compilation. `import` names a module by its **file path**.
- A **namespace** is where a name lives. Code always reaches names through their namespace path, whichever module declared them.
```rift
import gfx::vulkan_device;          // the file gfx/vulkan_device.rifth
gfx::vulkan::device d = gfx::vulkan::open_device(0);
```
- Importing a module makes its declarations visible under their namespaces. Without the import, the names don't exist for this file, even if the namespace is open in other files.
- **Convention, not rule:** keep file paths and namespaces parallel (`gfx/vulkan/device.rifth` declares into `gfx::vulkan`). Large projects benefit from splitting a namespace over several files, which is why it isn't enforced.
- `std` is a namespace declared by the standard library's modules; `import std;` loads them all.

### 3. Looking up a name
**Qualified names** (`a::b::c`) are looked up exactly where they say, nowhere else. A leading `::` starts at the global namespace.

**Unqualified names** (`c`) inside namespace `a::b`:

1. **Locals and parameters** of the current function. Members are always written `this.x` (see [Classes](../user-types/classes.md)), so they're never found here.
2. **The enclosing namespaces:** `a::b`, then `a`, then the global namespace. Each level is searched.
3. If the name exists at **more than one level**, it's an **ambiguity error**: write the full name.
```rift
namespace app {
    void log(std::str text);
    namespace net {
        void log(std::str text);
        void connect() {
            log("connecting");       // ERROR: 'log' is both app::net::log and app::log; write the one you mean
        }
    }
}
```
In C++ the inner `log` would silently win. If it was added later, existing calls change meaning without a warning. In Rift, the addition is a compile error at every call that became ambiguous.

- **Overload sets don't merge across levels.** All overloads of a name come from one namespace.
- Nothing else is searched: not the argument types' namespaces, not other imported namespaces.

### 4. What doesn't exist, and why
| C++ feature | Why it's a hijack | Rift |
|---|---|---|
| **Argument-dependent lookup** | `swap(a, b)` or `begin(x)` calls a function from `a`'s namespace that the caller never named. A library can take over calls by adding an overload next to its types | **No ADL.** A call finds only what the lookup above finds |
| `using namespace x;` | Brings in names that later collide with or outrank local ones | Not available |
| `using x::f;` declarations | Imports names into a scope | Not available |
| Adding to `namespace std` | Specializing `std::hash`, overloading `std::swap` changes how the standard library behaves for everyone | Each top-level namespace belongs to one project (section 6); `std` belongs to the standard library |
| Template specialization | A specialization in another file changes what a template does | No specialization (see [Templates](../compile-time/templates.md)); behaviour comes from the type's own interfaces |
| Dependent lookup at instantiation | A template body calls whatever `f` is visible *where it's used* | Template bodies resolve names where the **template** is written (section 5) |
| Operators found by ADL | Any header can add `operator==` for someone else's type | Operators are defined with the type (section 5) |

**Aliases** replace `using` for the one good use, shortening long names:
```rift
namespace vk = gfx::vulkan;            // file-local alias
vk::device d = vk::open_device(0);
```
- An alias is visible only in the file that declares it, and is never exported, not even from a header.
- An alias may not have the same name as anything already visible (no shadowing).
- Type aliases already exist: `compile_time type id = u32;` (see [Metaprogramming](../compile-time/metaprogramming.md)).

### 5. Behaviour belongs to the type
Customization in C++ is done with free functions found by ADL, specializations and operators added from anywhere. In Rift, **only the type's own declaration can define how the type behaves**:

- **Methods** are declared in the type (see [Structs](../user-types/structs.md), [Classes](../user-types/classes.md)).
- **Interfaces** are implemented in the type's base list (see [Inheritance and Interfaces](../user-types/inheritance-and-interfaces.md)), or by an annotation written on the type (see [Annotations](../compile-time/annotations.md)). No other module can make an existing type implement an interface.
- **Operators** are either methods of the type, or free `operator` functions declared **in the same namespace and the same module** as one of their operand types (see [Operators](../basics/operators.md)). A different module can't add an operator for a type it didn't declare.
- **Templates** call behaviour through their constraints (`T : std::hashable` → `value.hash()`, see [Templates](../compile-time/templates.md)) and through names visible where the template is written. Which overload a template body calls never depends on where it's instantiated.

So the answer to "what does `a == b` do?" is always in the declaration of `a`'s or `b`'s type, and the answer to "which `hash` does `std::hash_map` call?" is always the key type's `hash()` method.

### 6. Who may declare into a namespace
- Each **top-level namespace** belongs to the project that declares it. A project is a target set of one `build.rifts`, or the standard library (see [Rift as a Build System](../../build/build-system.md)).
- Modules of the same project may reopen and extend their namespaces freely. A module of **another** project that declares into it is a compile error:
  ```
  error: namespace 'std' belongs to the standard library; declarations can't be added to it
  ```
- If two dependencies both declare the same top-level namespace (two libraries called `json`), the build reports it as soon as both are linked into one target. It never becomes a silent merge.
- Libraries may not declare into the global namespace. A program's own files may, which keeps small programs short.

## Examples

### 1. A library and its user
```rift
// mathlib/vector.rifth
namespace mathlib {
    struct vec3 {
        f32 x;
        f32 y;
        f32 z;
        vec3 operator+(vec3& other);
        f32  length();
    }
    f32 dot(vec3& a, vec3& b);
}
```
```rift
// game/physics.rifts
import mathlib::vector;

namespace game::physics {
    namespace math = mathlib;                   // file-local alias

    f32 speed(math::vec3& velocity) {
        return velocity.length();               // a method: found on the type
    }

    f32 alignment(math::vec3& a, math::vec3& b) {
        return math::dot(a, b);                 // a free function: written with its namespace
        // dot(a, b) would be an error: 'dot' isn't visible here (no ADL)
    }
}
```

### 2. The C++ hijack that can't happen
```cpp
// C++: a user type in its own namespace
namespace audio { struct buffer { ... }; void swap(buffer&, buffer&); }   // added in version 2 of the library

template<typename T> void reorder(T& a, T& b) { using std::swap; swap(a, b); }
// reorder(buf1, buf2) used std::swap in version 1 and calls audio::swap in version 2, without any change in the caller
```
```rift
// Rift: the template body says which function it calls
template<type T>
void reorder(mut T& a, mut T& b) {
    std::swap(mut a, mut b);      // always std::swap; audio can't change that
}
```

### 3. Two levels, one name
```rift
namespace app {
    std::str version();
    namespace cli {
        void print_help() {
            std::println(version());          // OK: only app::version exists
        }
    }
}

// later, someone adds app::cli::version():
// → compile error in print_help: 'version' is both app::cli::version and app::version
```

## Rationale
- **C++ syntax, stricter lookup:** the block form, nesting, reopening and `a::b` paths are exactly C++'s, so nothing new has to be learned to *write* namespaces. Only the lookup is stricter, and its strictness only ever shows up as an error you can fix by writing more of the name.
- **No ADL:** the convenience of ADL (`swap`, `begin`, operators) is exactly what makes calls depend on code the caller never named. Rift gets the convenience back elsewhere: methods are found on the value, operators are defined with their types, and generic code uses interfaces.
- **Ambiguity instead of hiding:** with hiding, adding a declaration can change behaviour silently. With an error it can't. The price is sometimes writing `app::log` where `log` would have been enough.
- **Namespaces separate from files:** that's C++'s model, and it lets one namespace span many files. Keeping them separate from modules also means a name never changes because a file moved.
- **Ownership of top-level namespaces:** nobody can add to `std` or to another library's namespace, so a library's behaviour is defined by that library alone.
