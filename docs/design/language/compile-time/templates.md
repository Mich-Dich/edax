# Templates

## Rules

### Declaring template parameters
Template parameters are **always declared** with `template<...>` in front of the item:
```rift
template<type T>
T larger(T a, T b) {
    return if (a > b) { a } else { b };
}

template<type T, usize N>
struct array { ... }

template<type K, type V>
class hash_map { ... }
```
| Parameter | Meaning |
|---|---|
| `type T` | Any type |
| `type T : iface` | A type that implements `iface` |
| `type T : iface_a & iface_b` | A type that implements both |
| `usize N`, `bool B`, ... | A compile-time value of that type |
| `type T = std::global_allocator` | A parameter with a **default**: it may be left out at the end of the argument list |
| `type... A` | A **pack**: any number of types (below) |

### Using templates
- **Types:** template arguments are always written out: `std::darray<employee>`, `std::array<u8, 16>`.
- **Functions:** arguments are deduced from the call arguments, or written explicitly:
  ```rift
  u32 m = larger(a, b);              // T = u32 from a and b
  u32 n = larger<u32>(3, 4);         // explicit; the literals take u32
  u32 o = larger(3, 4);              // ERROR: T can't be deduced from untyped literals
  ```
- **Deduction uses every argument that has a type**, including the result type of a lambda argument (`std::meta::result_of`, see [Metaprogramming](metaprogramming.md)). Untyped literals then take the deduced type. In `pool.reduce_chunks(data, 4096, 0.0, map, combine)` the result type comes from `map`'s result, and `0.0` becomes that type. A parameter fixed only by untyped literals is an error, as above.
- **`void` as a template argument:** `void` may be passed where the template uses the parameter only as a result or as an enum payload (`std::thread::handle<void>`, `std::result<void, E>`). Using it as a field or parameter type is an error at the instantiation. An alternative whose payload is `void` has no payload: `std::ok()` (see [Enums](../user-types/enums.md)).

### Parameter packs
A pack takes any number of types, and a function parameter of a pack type takes the matching arguments:
```rift
template<type... A>
void print_all(A&... values) {
    for compile_time (A& v : values) {          // one copy of the body per argument, each with its own type
        std::print(f"{v} ");
    }
    std::println("");
}

print_all(1u32, "two", 3.0);                    // A = u32, std::static_str, f64
```
- A pack can only be used in two ways: walked with `for compile_time` (see [Compile-Time Evaluation](compile-time.md)), where the loop variable's type `A` is the current element's type, and passed on whole as `values...` to another pack parameter.
- `values.len()` is the number of elements, a `compile_time usize`.
- The std callables are written with packs: `std::function<R(A...)>`, `std::fn_ptr<R(A...)>`.
- A pack is the last template parameter.

### Default template arguments
`template<type T, type A = std::global_allocator> class darray` can be used as `std::darray<u32>`: arguments with defaults may be left out from the end. A default may name earlier parameters.

### Method templates
A method can have its own template parameters, also in a class template. They are deduced from
the call's arguments like a function's:
```rift
template<type T>
class darray {
public:
    template<type F>
    void retain(F keep) mut;           // keep: a lambda, deduced at each call
}

employees.retain([](employee& e) { return e.is_senior(); });
```

### Constraints
```rift
template<type T : std::comparable>
T larger(T a, T b) { ... }
```
- The std constraints are structural: `std::comparable` (numbers, `char`, strings, types with `operator<=>`), `std::cloneable` (everything but classes without `clone()`), `std::formattable` (what an f-string can print).
- **Constrained** templates are checked **when they are defined**. The body may only use what the constraints provide. At the call site the compiler checks that `T` satisfies them, and errors point at the call: "`point` does not implement `std::comparable`".
- **Unconstrained** templates are checked when they are instantiated, and the error shows the instantiation chain (at most one screen). Constraints are recommended for every public template.
- A **method of a class template instance** only has to compile when it is used: `std::darray<std::unique_ptr<item>>` is fine, and only a call to its `clone()` is an error (at the call, with the reason in a note). Virtual methods and destructors always have to compile.

### No specialization
There is no template specialization. Behaviour that depends on the type is written with `if compile_time` (see [Compile-Time Evaluation](compile-time.md)):
```rift
template<type T>
std::string describe(T value) {
    if compile_time (std::is_integer<T>()) {
        return std::format(f"integer {value}");
    } else {
        return std::string::from("something else");
    }
}
```

### Compilation model
1. The template **declaration** is in the `.rifth`; the **body** is in the `.rifts`.
2. Compiling the `.rifts` stores the template body in the module's compiled interface (see [Headers, Sources and Modules](../organization/modules.md)).
3. An importer that uses `larger<u32>` **instantiates and type-checks at its own compile time**. Layout, `sizeof`, `compile_time` evaluation and `deduce` return types are all available there.
4. Identical instantiations from different files are **merged at link time**. Each unique instantiation ends up in the binary only once.

### Name lookup in template bodies
Names in a template body are looked up **where the template is written** (see [Namespaces](../organization/namespaces.md)), never where it's instantiated. Behaviour that depends on `T` comes from `T`'s members and from the constraints' interfaces, so code at the instantiation site can't change which function a template calls.

## Rationale
- **Declared parameters:** every name in a declaration is introduced somewhere a reader can see. `template<type T>` is familiar from C++, and `type` is shorter than `typename`.
- **Instantiation at compile time:** a file using `std::darray<employee>` needs its size immediately to put it on the stack, and `deduce` needs the return type to type-check the caller. Only the *duplicate machine code* can wait until link time. The header/source split is still kept, because the body travels in the compiled interface.
- **Interfaces as constraints:** one concept (interface) serves both dynamic dispatch and generic constraints, so there's nothing extra like C++ "concepts" to learn.
- **`if compile_time` instead of specialization:** all variants are in one place and can be read top to bottom.
