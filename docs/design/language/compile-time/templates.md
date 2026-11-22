# Templates

## Rules

### Declaring template parameters
Template parameters are **always declared** with `template<...>` in front of the item:
```edax
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
| `usize N`, `bool B`, `std::version V`, ... | A compile-time value of that type: an integer, a `bool`, an enum or a struct whose fields are compile-time values |
| `type T = std::global_allocator` | A parameter with a **default**: it may be left out at the end of the argument list |
| `type... A` | A **pack**: any number of types (below) |

### Using templates
- **Types:** template arguments are always written out: `std::darray<employee>`, `std::array<u8, 16>`.
- **Functions:** arguments are deduced from the call arguments, or written explicitly:
  ```edax
  u32 m = larger(a, b);              // T = u32 from a and b
  u32 n = larger<u32>(3, 4);         // explicit; the literals take u32
  u32 o = larger(3, 4);              // ERROR: T can't be deduced from untyped literals
  ```
- **Deduction uses every argument that has a type**, including the result type of a lambda argument (`std::meta::result_of`, see [Metaprogramming](metaprogramming.md)). Untyped literals then take the deduced type. In `pool.reduce_chunks(data, 4096, 0.0, map, combine)` the result type comes from `map`'s result, and `0.0` becomes that type. A parameter fixed only by untyped literals is an error, as above.
- **Value arguments** are compile-time values, usually named constants: `std::graphics::opengl::functions<gl_33>` with `compile_time std::graphics::api gl_33 = ...;`. Two arguments are the same instance when their values are equal. A function deduces a value parameter from an argument's type like a type parameter: `void draw(functions<A>& f)` called with a `functions<gl_33>` has `A = gl_33`. Inside the template the parameter is a compile-time value: `A.version.major` can be read by checks and `if compile_time`.
- **`void` as a template argument:** `void` may be passed where the template uses the parameter only as a result or as an enum payload (`std::thread::handle<void>`, `std::result<void, E>`). Using it as a field or parameter type is an error at the instantiation. An alternative whose payload is `void` has no payload: `std::ok()` (see [Enums](../user-types/enums.md)).

### Parameter packs
A pack takes any number of types, and a function parameter of a pack type takes the matching arguments:
```edax
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
```edax
template<type T>
class darray {
public:
    template<type F>
    void retain(F keep) mut;           // keep: a lambda, deduced at each call
}

employees.retain([](employee& e) { return e.is_senior(); });
```

### Constraints
```edax
template<type T : std::comparable>
T larger(T a, T b) { ... }
```
- The std constraints are structural: `std::comparable` (numbers, `char`, strings, types with `operator<=>`), `std::cloneable` (everything but classes without `clone()`), `std::formattable` (what an f-string can print).
- **Constrained** templates are checked **when they are defined**. The body may only use what the constraints provide. At the call site the compiler checks that `T` satisfies them, and errors point at the call: "`point` does not implement `std::comparable`".
- **Unconstrained** templates are checked when they are instantiated, and the error shows the instantiation chain (at most one screen). Constraints are recommended for every public template.
- A **method of a class template instance** only has to compile when it is used: `std::darray<std::unique_ptr<item>>` is fine, and only a call to its `clone()` is an error (at the call, with the reason in a note). Virtual methods and destructors always have to compile.

### No specialization
There is no template specialization. Behaviour that depends on the type is written with `if compile_time` (see [Compile-Time Evaluation](compile-time.md)):
```edax
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
1. The template **declaration** is in the `.edh`; the **body** is in the `.ed`.
2. Compiling the `.ed` stores the template body in the module's compiled interface (see [Headers, Sources and Modules](../organization/modules.md)).
3. An importer that uses `larger<u32>` **instantiates and type-checks at its own compile time**. Layout, `sizeof`, `compile_time` evaluation and `deduce` return types are all available there.
4. Identical instantiations from different files are **merged at link time**. Each unique instantiation ends up in the binary only once.

### Name lookup in template bodies
Names in a template body are looked up **where the template is written** (see [Namespaces](../organization/namespaces.md)), never where it's instantiated. Behaviour that depends on `T` comes from `T`'s members and from the constraints' interfaces, so code at the instantiation site can't change which function a template calls.

### Associated types
An interface can declare a type that each implementer chooses:
```edax
interface iterable {
    type element;
    std::option<element> next() mut;
}

template<type C : iterable>
usize count_matching(mut C& items, C::element& wanted) { ... }
```
- An implementer writes `type element = u32;` in its body (a template can write `type element = T;`). In a template constrained by the interface, `C::element` is that type; outside, `counter::element` and `repeat<f64>::element` name it.
- Any struct or class may declare `type name = T;`, used as `name` in its body and `owner::name` outside. Only an interface may leave the type out (`type element;`); a type implementing the interface that doesn't give it is an error.

### Implementing an interface for an existing type
```edax
implement geometry::point for math::vec3 {
    f64 x() override { return cast<f64>(this.x); }
    ...
}
```
- `implement I for T { ... }` makes `T` implement `I` without changing `T`'s declaration. It is written in the module that declares `I` **or** the one that declares `T`, never a third one, so two libraries can't give competing implementations.
- The methods can only use `T`'s public members.

### Operators as interfaces
`std::addable<R>`, `std::subtractable<R>`, `std::multipliable<R>`, `std::dividable<R>` and `std::negatable` are implemented by writing the operator. Generic math code constrains on them: `template<type V : std::addable<V> & std::multipliable<f32>>`.
- `T : std::addable<R>` holds when `T` has `operator+` taking an `R` (a member or a free operator). Numbers and SIMD vectors implement them for their own type (a vector also with its lane type); a distinct type with `@std::derive::arithmetic` for `d + d`, `d - d`, `d * k`, `d / k`, `d / d`; unsigned integers aren't `std::negatable`.
- `R` may name the template's parameters: in `template<type V : std::addable<V>>` it is the argument given for `V`.

### Value packs and text arguments
- `template<usize... N>` takes any number of values: `tensor<f32, 4, 4>`. It is walked with `for compile_time` like a type pack. `N.len()` counts it and `product<N...>()` passes it on to another value pack. In a type template, the members are checked once per argument count, in the instances.
- `template<std::static_str S>` takes a text: `std::name<"player">`, `units::quantity<f32, "m/s">`. Two instances are the same when the texts are equal.

### Distinct types
`distinct type meters = f32;` is a new type with `f32`'s layout. Nothing converts implicitly: `cast<meters>(x)` and `cast<f32>(m)` are the only conversions (also at compile time), and only between the type and its underlying type. `meters{...}` is an error: the value has no named fields.
```edax
@std::derive::arithmetic
distinct type meters = f32;
@std::derive::arithmetic
distinct type seconds = f32;

meters lap   = cast<meters>(400.0);
meters total = lap * 3.0 + lap;          // meters: scaled by f32, added to meters
f32    ratio = total / lap;              // meters / meters is a plain number
total *= 0.5;
meters wrong = total + cast<seconds>(1.0);   // ERROR: no '+' for meters and seconds
meters area  = lap * lap;                    // ERROR: meters * meters isn't meters
```
- Without an annotation a distinct type has no operators. `@std::derive::arithmetic` gives it its underlying type's arithmetic, kept inside the type:

| Expression | Result |
|---|---|
| `d + d`, `d - d`, `d % d`, `-d` | the distinct type |
| `d * k`, `k * d`, `d / k` (`k` of the underlying type, or a literal) | the distinct type |
| `d / d` | the underlying type (a ratio) |
| `==`, `!=`, `<`, `<=`, `>`, `>=`, `<=>` between two `d` | `bool` / `std::ordering` |
| `+=`, `-=`, `%=` with a `d`; `*=`, `/=` with a `k` | changes `d` |

- The operations are the underlying type's own, with its overflow checks; there is no call and no cost.
- A distinct type can be written in a header and used as a field, parameter or template argument like any struct.

### Library error frames
`@std::diagnostic_frame{ .hide = true }` on a template of a library drops its frames from instantiation chains in errors, so a user sees the call in their code and the check that failed, not the library's internals.

## Rationale
- **Declared parameters:** every name in a declaration is introduced somewhere a reader can see. `template<type T>` is familiar from C++, and `type` is shorter than `typename`.
- **Instantiation at compile time:** a file using `std::darray<employee>` needs its size immediately to put it on the stack, and `deduce` needs the return type to type-check the caller. Only the *duplicate machine code* can wait until link time. The header/source split is still kept, because the body travels in the compiled interface.
- **Interfaces as constraints:** one concept (interface) serves both dynamic dispatch and generic constraints, so there's nothing extra like C++ "concepts" to learn.
- **`if compile_time` instead of specialization:** all variants are in one place and can be read top to bottom.
- **Associated types and retroactive implementations:** these are what generic libraries the size of Boost need (graphs, geometry, ranges). The orphan rule (`implement` only next to the interface or the type) keeps them coherent.
- **Distinct types:** units and ids are where C++ code mixes up `float`s. A distinct type costs nothing at run time and turns the mix-up into a compile error.
