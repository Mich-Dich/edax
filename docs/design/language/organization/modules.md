# Headers, Sources and Modules

## Rules

### Module = header + source
- `geometry.edh` (header) + `geometry.ed` (source) form the module **`geometry`**.
- Directories become `::` in module names: `shapes/circle.edh` is module `shapes::circle`, with paths relative to the project root.
- A module is a unit of compilation, **not** a namespace. Its declarations live in the namespaces they're declared in (`namespace geometry { ... }`, see [Namespaces](namespaces.md)) and are always used by their namespace path: `geometry::circle`. There is no `using`; there are file-local namespace aliases (see [Namespaces](namespaces.md)).
- A `.ed` automatically sees its own `.edh`, so you don't import it.
- Inside a module, declaration order doesn't matter.

### `import`
```edax
import std;                 // the whole standard library
import std::filesystem;     // only std::filesystem
import geometry;            // ./geometry.edh
```
`import` names a module by its file path. The names it brings are reached through their namespaces (see [Namespaces](namespaces.md)): `std::filesystem::path`. The narrower import only reduces what the compiler loads.

### What goes where
| Item | `.edh` | `.ed` |
|---|---|---|
| Function | Declaration `T f(...);` | Definition with body |
| Struct | All fields + method declarations | `struct` block with method bodies only (no new fields) |
| Class | `public:` / `protected:` members, destructor declaration | `class` block: method bodies + `private:` members |
| Enum | Complete | - |
| Interface | Complete, with method declarations | `interface` block with the bodies of default methods only |
| Template | Declaration | Body (see [Templates](../compile-time/templates.md)) |
| `compile_time` constant | Complete, with its value | - |
| `static` variable | Complete, with its initializer | - |
| Module-local items | - | Marked `private` |

**Headers never contain function bodies.** Headers never contain `check`s either: checks are written only in the `.ed` (see [Checks](../errors/checks.md)). **Method bodies are only ever defined inside the `class`/`struct` block in the `.ed`.** There are no out-of-line `void test::f() { }` definitions.

### Class merge rules
| Situation | Result |
|---|---|
| Header declares `void f();`, source defines it in the class block | ✓ Merged |
| Header declares `void f();`, source never defines it | Error (at compile time of the source) |
| Source defines a `public`/`protected` member the header doesn't declare | Error |
| Source adds `private:` methods or fields | ✓ Allowed |
| Header contains `private:` | Error |
| Source signature differs from header (including `mut`) | Error |

**Layout:** header data members in header order, then private fields in source order.

### `private` at file scope
In a `.ed` **that has a `.edh`**, every top-level item not declared in the header must be marked `private`; otherwise it's an error. In a standalone `.ed` (no header) nothing can be exported, so `private` isn't needed.
```edax
// geometry.ed
private f64 square(f64 x) { return x * x; }       // module-local helper
f64 geometry::distance(...)                        // ERROR: no out-of-line definitions
```

### What importers see
An importer sees a module through its `.edh` only:

- every name from the `.edh`, and nothing from the `.ed` (private names can't be reached),
- the **complete layout** of each class (size, alignment, offsets), with private member *names* hidden,
- whether each type is a copy type and whether it has a destructor,
- template bodies and `compile_time` function bodies, so importers can instantiate and evaluate them (see [Templates](../compile-time/templates.md), [Compile-Time Evaluation](../compile-time/compile-time.md)),
- every function's entry checks and every type-body check, so they run at the importer's calls and instantiations (see [Checks](../errors/checks.md)).

```edax
// other.ed
import test;
test::test t = test::test{};        // OK: the size is in the interface
t.private_member_x = 5;             // ERROR: private member not visible
```

### How modules are compiled
- **The front end sees the whole program.** Each build parses and checks every module the program imports (its own, its libraries', `std` and generated modules) together; what an importer may use is limited as above, but nothing is read from a separate interface file. The generated graphics modules (`std::opengl`, `std::vulkan`, made from the XML registries) are kept on disk (`~/.cache/edax/modules`), so their generation is paid once.
- **Machine code is reused per unit.** An unoptimized build splits the program into units (each module of the program, a few per library module, a type's methods together) and keeps each unit's object code in the object cache, under a hash of its code (see [Build Modes](../../build/build-modes.md), "The object cache"). A unit is compiled again only when its code changed: changing a method body recompiles that module's unit, and the units that inline one of its small functions; changing a private field changes the layout, which changes the code of the importers that use the type.
- **`development` builds are compiled in units through LLVM's ThinLTO**, which imports the functions worth inlining across units; their objects are cached the same way.
- **`release` builds compile the program as one unit**, so everything can be inlined into everything.
- A module of a shared Edax library is compiled into the library, once; programs only declare what they use of it (generic instances are made in the program, see [Dynamic Libraries and Plugins](../../stdlib/system/dynamic-libraries.md)).

> **Planned:** compiled module interfaces on disk, so that an unchanged module isn't parsed and checked again. Measured today, the whole front end of a large program (the windowing example with the OpenGL module) takes about 0.75 s, of which checking the bodies of `std` is too small to measure; the cost is in instantiating templates (re-parsing each template for its instance: 22%) and parsing, which interfaces wouldn't remove. They wait until the front end is a large share of a build.

### Import graph
- A module's layout comes from its source, which must be checked first. So **module imports must form an acyclic graph**, counting imports from both `.edh` and `.ed` files.

## Rationale
- **What the split is for:** a readable header with only the public interface, private names that can't be reached, and **no PIMPL indirection**. It is not a compile firewall. Layout changes still recompile dependents, and the doc says so openly.
- **No cycles:** this is the honest cost of putting the full layout into what importers see. Cycles between modules are usually a design smell anyway.
- **One place for method bodies:** inside the type's block in the `.ed`. Out-of-line `test::f()` definitions would be a second way.
- **Required `private`:** mirrors the class rule that every public member must be in the header. You can't accidentally export something.
