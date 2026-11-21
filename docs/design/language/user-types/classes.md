# Classes

A `class` provides **encapsulation**: hidden state, invariants and resource ownership. Inheritance is covered in 07, and the header/source merge in 09.

## Rules

### Access sections
C++-style labels: `public:`, `protected:`, `private:`. Public and protected members are declared in the `.edh`; private members exist only in the `.ed` (see [Headers, Sources and Modules](../organization/modules.md)).

### Methods and `this`
- `this` is an implicit, immutable reference to the object. **Members are always accessed through `this.`**: write `this.value`, not `value`.
- A method that changes the object ends with **`mut`**:
  ```edax
  void inc() mut { this.value += 1; }
  ```
  Inside a `mut` method, `this` is a mutable reference.
- `mut` methods can only be called on `mut` variables or through `mut` references:
  ```edax
  counter a = counter{};
  a.inc();                 // ERROR: a is immutable
  mut counter b = counter{};
  b.inc();                 // OK
  ```
- A non-`mut` method cannot call a `mut` method on `this`.
- **`static` methods** have no `this` and are called as `counter::starting_at(5)`.
- A method that **consumes** the object ends with **`move`**: `R join() move;`. It takes ownership of `this`, so it can be called only on an owned value (a local, a by-value parameter or a temporary), and the variable counts as moved afterwards (see [Ownership, Copies, Moves and References](../memory/ownership-and-references.md), [Threads](../../stdlib/concurrency/threads.md)).

### Construction
- `T{ .field = value }` is allowed **wherever every field without a default is visible**. Outside the class's own module, private fields therefore always take their defaults.
- If the private fields have no defaults, or the class has invariants to enforce, provide `static` factory methods. Inside the class's `.ed` all fields are visible.

### Destructor
- `~name()` runs when the object is destroyed (see [Ownership, Copies, Moves and References](../memory/ownership-and-references.md) for when). It is implicitly `mut`.
- It is declared in the `.edh` because whether a class has a destructor affects how it can be copied, and importers need to know that.
- A class with a destructor is never a copy type.

### Nested types
A struct, class, enum or interface may be declared **inside** a struct or class. It is a member of the outer type and is named through it:
```edax
class thread_pool {
public:
    struct options {
        usize threads;
        std::static_str name = "pool";
    }
    static thread_pool make(options config);
}

std::thread_pool::options cfg = std::thread_pool::options{ .threads = 8 };
```
- The access section decides who can name it: a nested type under `private:` is visible only inside the outer type's module (see [Headers, Sources and Modules](../organization/modules.md)).
- A nested type is an ordinary type: it has no hidden link to an object of the outer type, and it may use the outer type's private members, like any member.
- A template's nested types are per instance: `std::thread_pool::task<R>` is a nested template.

### Moving and copying
- Moving an object is always a **bitwise move**. There are no user-defined move constructors.
- Copy type: only if all fields are copy types and there is no destructor. Otherwise the class must implement `clone()` explicitly (interface `std::cloneable`) to be cloneable.

## Example

```edax
// counter.edh
namespace counting {
    class counter {
    public:
        u32  get();
        void inc() mut;
        static counter starting_at(u32 value);
        ~counter();
    }
}
```

```edax
// counter.ed
namespace counting {
    class counter {
    public:
        u32 get() {
            return this.value;
        }
        void inc() mut {
            this.value += 1;
        }
        static counter starting_at(u32 value) {
            return counter{ .value = value };     // private field is visible here
        }
        ~counter() {
            std::println(f"counter dropped at {this.value}");
        }
    private:
        u32 value = 0;
    }
}
```

```edax
// main.ed
import counter;                               // the module: counter.edh (see Headers, Sources and Modules)

std::exit_code main() {
    mut counting::counter c = counting::counter::starting_at(10);   // the namespace (see Namespaces)
    c.inc();
    std::println(f"{c.get()}");               // 11
    return std::exit_code::success;
}                                             // destructor runs here
```

## Rationale
- **Trailing `mut`:** it works like C++'s trailing `const`, but inverted to match immutable-by-default. The marker is on the declaration, so it shows up in the header where users of the class read it.
- **Explicit `this.`:** you can always tell a member from a local or a parameter. This is also why parameters can share a name with a field (`value` vs `this.value`).
- **Bitwise moves only:** moving can never run user code or fail, so `move` costs exactly what it looks like. The trade-off is that a type can't hold a pointer into itself; such structures use indices or a heap allocation.
