# Inheritance and Interfaces

## Rules

### Inheritance
- A class can only be inherited from if it is declared **`open`**. A class without `open` is final.
- **Single base class** plus any number of interfaces: `class circle : shape, std::formattable`. The base class, if there is one, comes first.
- Inheritance is always public. There is no `private`/`protected` inheritance.
- A derived class that should itself be inheritable is also declared `open`: `open class circle : shape`.

### Virtual methods
| Keyword | Meaning | Allowed in |
|---|---|---|
| `virtual` | Introduces a new dynamically dispatched method | `open` classes only |
| `abstract` | A `virtual` method with no implementation. The class can't be instantiated | `open` classes only |
| `override` | Implements a method from a base class or interface | Any class or struct with that base or interface |

- `virtual` and `override` never appear on the same method.
- `override` is required. Matching a base method's name without it is an error, and so is `override` with no matching base method.
- The destructor of an `open` class is always virtual.
- To call the base implementation, write `this.shape::describe()`.

### Polymorphism and slicing
- Upcasting is implicit for references and pointers (`circle&` → `shape&`). Downcasting uses `cast::dynamic` (see [Casts](../basics/casts.md)).
- **Copying or moving a derived object into a base-typed value is an error** (no slicing). Polymorphism only works through `&`, `std::unique_ptr` and similar.
- When a derived class is constructed, base and derived public fields are listed together: `circle{ .name = ..., .radius = 1.0 }`.

### Interfaces
- An `interface` is a named set of method signatures, with no fields. A method may have a **default body** (below).
- Methods are written like class methods, with a trailing `mut` if they mutate.
- Implemented by classes **and structs** using `override`.
- Interfaces may extend other interfaces: `interface solid : shape_like { f64 volume(); }`.
- Interface methods may be `compile_time` (their implementations must be too); this is how build modes declare their settings (see [Build Modes](../../build/build-modes.md)).
- An interface may declare `compile_time static` methods (`std::implicit<T>`, see [Functions](../functions/functions.md)). They are implemented with `override`; such an interface is used as a base or a template constraint.
- **Default methods:** an interface method with a body is used by every implementer that doesn't `override` it.
  - The header declares the method like any other; its body is in the interface's block in the `.ed`, like a class's (see [Headers, Sources and Modules](../organization/modules.md)).
  - A default body can only use the interface's own methods (through `this`), since the interface has no fields.
  - Replacing a default still needs `override`.
  - If a type gets two defaults for the same signature (from two interfaces), it must `override` that method; otherwise it's an error naming both interfaces.
  ```edax
  interface sink {
      void write(std::str line) mut;
      bool colored() { return false; }          // default: most sinks don't write colors
  }
  ```
- Two ways to use an interface, each with a visible cost:

| Use | Dispatch | Cost |
|---|---|---|
| Template constraint `template<type T : shape_like>` (see [Templates](../compile-time/templates.md)) | Static | Zero; resolved at compile time |
| Interface reference/pointer: `shape_like& s`, `std::unique_ptr<shape_like>` | Dynamic | Fat pointer (object + table); one indirect call |

Implementing an interface does **not** change the object's layout. The dispatch table sits beside the pointer, not inside the object. Only `virtual` in a class hierarchy adds a vtable pointer to the object.

## Example
```edax
open class shape {
public:
    std::string name;
    virtual f64 area() abstract;
    virtual std::string describe();          // has an implementation in .ed
}

class circle : shape {
public:
    f64 radius = 0.0;
    f64 area() override;
    std::string describe() override;         // may call this.shape::describe()
}

class sealed_thing { }
class bad : sealed_thing { }                 // ERROR: sealed_thing is not open

interface printable {
    std::string to_text();
}

struct point : printable {
    f64 x = 0.0;
    f64 y = 0.0;
    std::string to_text() override;
}

void print_all(std::span<std::unique_ptr<printable>> items) {   // dynamic dispatch
    for (std::unique_ptr<printable>& item : items) {
        std::println(item->to_text());
    }
}

template<type T : printable>
void print_one(T& item) {                                       // static dispatch
    std::println(item.to_text());
}
```

### Who may implement an interface
The type's own declaration (its base list, or an annotation written on it that generates the implementation, see [Annotations](../compile-time/annotations.md)), or an `implement I for T { ... }` block in the module that declares `I` or the one that declares `T` (see [Templates](../compile-time/templates.md)). No third module can make an existing type implement an interface (see [Namespaces](../organization/namespaces.md)).

## Rationale
- **`virtual` only introduces, `override` only implements:** every method says exactly one thing, and a reader can tell a new dispatch point from an implementation.
- **`abstract` keyword:** in the header split, *every* method is body-less in the `.edh`, so "no body" can't mean "pure virtual". It needs its own word.
- **Single base class:** this avoids diamond inheritance. Interfaces cover the remaining multiple-inheritance cases.
- **Default methods with a mandatory `override` to replace them:** interfaces can grow without breaking implementers, and an implementation never replaces a default silently.
- **Fat pointers for interfaces:** structs can implement interfaces without becoming "non-plain" data, because their layout never changes.
- **No slicing:** it's a classic silent C++ bug, and here it's simply a compile error.
