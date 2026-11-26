# Metaprogramming

Metaprogramming in Edax is **ordinary compile-time code** (see [Compile-Time Evaluation](compile-time.md)). There is no macro language, no token pasting and no code in strings. Three things are added on top of `compile_time` functions and `if compile_time`:

1. **Types are values** at compile time: `compile_time type t = u32;`.
2. **Reflection:** `std::meta` describes a type's fields and alternatives.
3. **`for compile_time`:** a loop that is written once and copied once per element, so each copy can work with a different type.

Together they cover generic printing, comparison, hashing, serialization and enum ↔ string conversion. **Annotations** (see [Annotations](annotations.md)) attach data to declarations, check them, and generate methods for annotated types. Compile-time code never creates new types or free functions: every name in a program has a written declaration.

## Rules

### Types as values
`type` is a built-in type whose values are types. It exists only at compile time.
```edax
compile_time type id = u32;

compile_time type counter_type(usize max) {
    return if (max <= 255) { u8 } else { u32 };
}

compile_time type small = counter_type(100);
small count = 0;                        // a u8
std::darray<small> history = default;   // a type value works wherever a type is written
```
- A type is written in an expression the same way as in a declaration: `u32`, `point`, `T`, `std::option<u32>`. Reference types (`T&`) cannot be written as values.
- A type value is used **as a type through a name**: a `compile_time` constant, a template parameter, or a parameter of a `compile_time` function. To use a type stored somewhere else (like `f.type` below), give it a name first: `compile_time type ft = f.type;`.
- **An alias names the type it was given** (`compile_time type vec2i32 = vec2<i32>;`). It builds values and reaches static members like the type itself: `vec2i32{ .x = 1, .y = 2 }`, `vec2i32::zero()`, `number::max` for `compile_time type number = u16;`. Aliases of aliases and aliases in namespaces (`geo::vec2u8{ ... }`) work the same way. The alias is looked up in the namespace it was declared in.
- Type values support `==` and `!=`. `std::type_name<T>()` gives the spelling (see [Checks](../errors/checks.md)).
- A template parameter `T` is a type value, so `T` can be passed to a `compile_time` function that takes a `type`.

**Compile-time-only types.** `type`, the `std::meta` types below, and every struct with a field of one of them exist only at compile time. They can be used in `compile_time` constants, in `compile_time` functions, and as `for compile_time` loop variables. Using one in run-time code (a run-time variable, a parameter of a run-time function, a field of a run-time struct) is a compile error. Their parts that are ordinary values can be used at run time: `f.name` is a `std::static_str`.

### Reflection: `std::meta`
All functions are `compile_time` and take the type as a template argument, like `std::is_integer<T>()` (see [Compile-Time Evaluation](compile-time.md)).

| Function | Result |
|---|---|
| `std::meta::fields<T>()` | `std::span<std::meta::field>`: the fields of a struct or class, in declaration order |
| `std::meta::alternatives<T>()` | `std::span<std::meta::alternative>`: the alternatives of an enum, in declaration order |
| `std::meta::is_struct<T>()`, `is_class<T>()`, `is_enum<T>()` | `bool` |
| `std::meta::result_of<F>()` | `type`: what calling the lambda or callable type `F` returns (`std::thread::spawn` uses it to name `handle<R>`) |
| `std::meta::parameters_of<F>()` | `std::span<std::meta::parameter>`: the parameters of the lambda or callable type `F`, with `name`, `type` (without the reference), `passing` and `index`. An ECS reads a system's components and whether it writes them from `[](mut position& p, velocity& v) { ... }` |
| `std::returns<F, R>()` | `bool`: whether calling `F` gives an `R`. For entry checks of functions taking callables (`darray::sort_by` uses it), where `std::meta::result_of<F>()` can't be written as a template argument |
| `std::meta::is_view<T>()` | `bool`: whether `T` borrows (a reference, a view, a lambda with reference captures, see [Ownership, Copies, Moves and References](../memory/ownership-and-references.md)) |
| `std::meta::methods<T>()` | `std::span<std::meta::function>`: the methods of a type (visible ones, see privacy below), including generated ones (see [Annotations](annotations.md)) |
| `std::meta::interfaces<T>()` | `std::span<type>`: the interfaces `T` implements, directly or through its bases |
| `std::meta::template_args<T>()` | `std::span<type>`: the type arguments of a template instance (`std::darray<u32>` → `u32`); empty for other types |
| `std::meta::annotation<A, T>()`, `std::meta::has_annotation<A, T>()` | Annotation `A` on type `T` (see [Annotations](annotations.md)) |
| `std::meta::function_annotation<A>(where)`, `type_annotation<A>(where)`, `file_annotation<A>(where)` | `std::option<A>`: annotation `A` on the function a `std::call_site` is in, on that function's type, or on its file (see [Functions](../functions/functions.md), [Annotations](annotations.md)) |
| `std::meta::name_of(value)` | `std::static_str`: the name of the enum alternative `value` holds. Works at run time too |
| `std::meta::from_name<E>(text)` | `std::option<E>`: the payload-free alternative of `E` called `text`. Works at run time too |
| `std::meta::functions_with<A>()` | `std::span<std::meta::function>`: every free function carrying annotation `A` (below) |
| `std::meta::types_with<A>()` | `std::span<type>`: every type carrying annotation `A` (below) |
| `std::is_integer<T>()`, `std::is_float<T>()`, `std::is_same<A, B>()`, `std::size_of<T>()`, `std::align_of<T>()`, `std::type_name<T>()` | As in 15 and 23 |

```edax
struct field {                 // std::meta::field
    std::static_str name;
    type            type;      // the field's declared type
    usize           index;     // position in declaration order
    usize           offset;    // byte offset in the object (the layout of 13)
}

struct alternative {           // std::meta::alternative
    std::static_str name;
    usize           index;
    bool            has_payload;
    type            payload;   // the payload's type; `void` when there is none
}

struct function {              // std::meta::function
    std::static_str                 name;
    type                            result;
    std::span<std::meta::parameter> parameters;   // without `this`
    bool                            is_mut;       // ends with `mut` (see Classes)
    bool                            is_static;
    std::static_str                 namespace_name;   // `app::commands`; for free functions and methods alike
}

struct parameter {             // std::meta::parameter
    std::static_str name;
    type            type;
    std::meta::passing passing;   // by_value, by_reference, by_mut_reference
}
```
Every handle (`field`, `alternative`, `function`, `parameter`) also has `annotation<A>()` and `has<A>()` (see [Annotations](annotations.md)).

**Using a field.** `f.get(value)` is a reference to that field of `value`. With `mut` it is a mutable reference, written like any other mutable reference argument (see [Functions](../functions/functions.md)):
```edax
u32&     a = f.get(person);          // when f.type is u32
mut u32& b = f.get(mut person);
```

**Using an alternative.**
| Method | Result |
|---|---|
| `a.is(value)` | `bool`: `value` holds this alternative |
| `a.get(value)` | `std::option<P&>`: the payload if `value` holds this alternative (only when `has_payload`) |
| `a.make()` / `a.make(payload)` | A new value of the enum holding this alternative |

**Using a method.** `m.call(value, args...)` calls the method `m` on `value`. It is checked and compiled like the call `value.name(args...)`, so the arguments follow the method's parameters. For a `mut` method, write `m.call(mut value, ...)`; for a `static` one, `m.call(args...)`.

- `f`, `a` and `m` must be **compile-time values** (a `for compile_time` loop variable, a `compile_time` constant). The type of `f.get(value)` depends on which field `f` is, so the compiler must know it.
- **Privacy is respected.** `std::meta::fields<T>()` lists the fields that are visible where the call is written (see [Classes](../user-types/classes.md), [Headers, Sources and Modules](../organization/modules.md)). A generic function in another module sees the public fields of a class, and every field of a struct. There is no reflective way around `private`.
- **Finding annotated declarations:** `functions_with<A>()` and `types_with<A>()` search the modules of the current build target and the modules it imports, nothing else. The result is sorted by namespace path, then name, so it doesn't depend on file or import order. A free function from the list is called with `f.call(args...)`. This builds registries (commands, plugins, components) without listing their entries by hand:
  ```edax
  for compile_time (std::meta::function f : std::meta::functions_with<command>()) {
      if (input == f.annotation<command>().expect("found by functions_with").word) {
          f.call(mut state);
      }
  }
  ```
- Reflection is read-only. It cannot add, remove or rename fields, alternatives or functions. Adding methods to a type is done with an annotation's `generate` block (see [Annotations](annotations.md)), on the type's own declaration.

### `for compile_time`
```edax
for compile_time (std::meta::field f : std::meta::fields<T>()) {
    ...
}
```
- The range is a compile-time value: a `std::span` of compile-time values, or `a..b` / `a..=b` with compile-time bounds.
- The body is **copied once per element**, in order. In each copy the loop variable is a compile-time constant with that element's value, and each copy is type-checked on its own. This is how one loop body can work with fields of different types.
- `break` leaves the remaining copies; `continue` goes on with the next copy. `return` returns from the function. Both may sit inside run-time `if`s.
- The loop is a statement with no value.
- Inside a `compile_time` function, an ordinary `for` over `std::meta::fields<T>()` also runs at compile time. Its loop variable is not a compile-time constant, though, so it can read `f.name` or `f.index` but cannot call `f.get(...)`. Use `for compile_time` when the body depends on the element's type.

### Type ids
`std::meta::type_id<T>()` is a `u64` that identifies `T`: a hash of its fully qualified name with template arguments, the same in every module and every plugin built from the same interfaces. It is a compile-time value usable at run time. Two types with the same id are a link error naming both.

### Run-time type information, when asked for
> **Planned:** `@std::reflect`, `std::meta::runtime_info` and compile-time values as static data are not implemented yet.

`@std::reflect` on a type (or a call of `std::meta::runtime_info<T>()`) makes the compiler emit a **static, read-only table**: name, size, alignment, type id, fields (name, offset, type id, annotations that are `@std::reflect` themselves), methods marked `@std::reflect` (callable through it), enum alternatives. Editors, property grids, serializers, undo and network replication read it. Types that don't ask cost nothing.

Compile-time values that hold texts, function pointers and lists become static data the same way, so lookup tables and baked curves are written as compile-time code.

### Types whose fields come from another type
A struct or class template may declare fields with `for compile_time` over the fields of one of its type parameters. Each field written in the block becomes one field per field of that type, named after it:
```edax
template<type T, type A = std::global_allocator>
struct soa_columns {
    for compile_time (std::meta::field f : std::meta::fields<T>()) {
        std::darray<f.type, A> f.name = default;   // one column per field of T, with T's field name
    }
}

mut soa_columns<particle> ps = default;
ps.position.push(p0);                             // `position` is declared in `particle`
```
- The source is `std::meta::fields<P>()` where `P` is a type parameter of the template, and an instance's argument for it must be a struct or class. Its fields are taken in declaration order; of a class only the `public` ones (a library's template never sees private fields).
- Inside the block, each field's type may use `f.type` anywhere a type goes (`std::darray<f.type, A>`, `soa<f.type>`), its name is `f.name`, and it may have an initial value that doesn't depend on `f`.
- The generated fields take the generator's place among the written fields: fields written before it come first, fields written after it last.
- A generated name that equals a written field's, or a second field in one block (both would be named `f.name`), is an error of the instance that asks for it.
- Methods of the template reach the generated fields through reflection on the instance itself: `for compile_time (std::meta::field col : std::meta::fields<soa_columns<T, A>>())`, `col.get(mut this)`.
- Generators nest: `soa<f.type>` in one template, `std::darray<f.type>` in `soa`, gives a column per field of each component (the `ecs_soa` benchmark).
- **Planned:** written prefixes and suffixes (`f.name ~ "_dirty"`, open question 9), `@f.annotations` to copy the field's annotations, "go to definition" landing on the source field, and generated fields in `edaxc expand`.
- This is how SoA containers, ECS tables, GPU mirrors of CPU structs (`gpu_mirror<T>` inserting std140 padding) and replication deltas are written as library code.

### Embedding files
`std::embed("shaders/blit.spv")` is the file's bytes as a `std::span<u8>` in static memory; `std::embed_text("help.txt")` is a `std::static_str`. The path is relative to the source file and is a recorded input of the build, so changing the file rebuilds what embeds it.

### Seeing generated code
`edaxc expand <file> <type>` prints a type with every annotation-generated member, every `for compile_time` unrolled and every generated field written out.

## Examples

### 1. Printing any struct
```edax
template<type T>
void describe(T& value) {
    compile_time check (std::meta::is_struct<T>()) else f"describe needs a struct, not {std::type_name<T>()}";
    std::print(f"{std::type_name<T>()} {{");
    for compile_time (std::meta::field f : std::meta::fields<T>()) {
        std::print(f" .{f.name} = {f.get(value)}");
    }
    std::println(" }");
}

struct point { f64 x; f64 y; }
describe(point{ .x = 1.0, .y = 2.5 });      // point { .x = 1.0 .y = 2.5 }
```

### 2. Field-by-field equality
```edax
template<type T>
bool fields_equal(T& a, T& b) {
    for compile_time (std::meta::field f : std::meta::fields<T>()) {
        if (f.get(a) != f.get(b)) {
            return false;
        }
    }
    return true;
}
```
Each copy compares one field with that field's own `!=`. A field type without `!=` is a compile error that points at this line and names the field's type.

### 3. Enum ↔ string
The standard library has this built in: `std::meta::name_of(value)` and `std::meta::from_name<E>(text)`. Written by hand, it is:
```edax
template<type E>
std::static_str name_of(E value) {
    for compile_time (std::meta::alternative a : std::meta::alternatives<E>()) {
        if (a.is(value)) {
            return a.name;
        }
    }
    std::panic("unreachable: every value holds one alternative");
}

template<type E>
std::option<E> parse_enum(std::str text) {
    for compile_time (std::meta::alternative a : std::meta::alternatives<E>()) {
        if compile_time (!a.has_payload) {
            if (text == a.name) {
                return std::some(a.make());
            }
        }
    }
    return std::none;
}
```

### 4. Choosing a type
```edax
compile_time usize max_players = 100;
compile_time type  player_id   = counter_type(max_players);   // u8

player_id next = 0;
counter_type(max_players) other = 0;   // ERROR: a type value is used as a type through a name
```

### 5. Calling every method with a given annotation
```edax
annotation(std::meta::target::function) struct command { std::static_str word; }

class shell {
public:
    @command{ .word = "status" } void show_status();
    @command{ .word = "reset" }  void reset() mut;
}

bool dispatch(mut shell& sh, std::str input) {
    for compile_time (std::meta::function m : std::meta::methods<shell>()) {
        if compile_time (m.has<command>()) {
            if (input == m.annotation<command>().expect("has").word) {
                if compile_time (m.is_mut) { m.call(mut sh); } else { m.call(sh); }
                return true;
            }
        }
    }
    return false;
}
```
The command table is built from the methods themselves: adding a command is adding one annotated method, and nothing else has to be kept in sync.

## Rationale
- **Ordinary compile-time code, not macros:** metaprograms use the same syntax, types and error messages as the rest of the language. A reader who knows Edax can read them, and the compiler can check them like any other code.
- **`type` as a value:** one more value kind instead of a second language for type computations. Functions such as `counter_type` read like normal code.
- **Type values are used through names:** a declaration `f.type x = ...;` or `counter_type(N) x = ...;` would make every type position also an expression position, which makes C++-like syntax ambiguous. Naming the type first keeps the grammar simple and the code readable.
- **`f.get(value)` instead of a splice operator:** no new symbol. It reads as what it does, and `mut` marks a mutable access the same way as everywhere else.
- **`for compile_time` mirrors `if compile_time`:** the same word says "decided by the compiler", and both type-check each copy or branch on its own.
- **Template-argument form for `std::meta`:** consistent with the traits of 15 (`std::is_integer<T>()`).
- **Privacy:** reflection that could read private fields would make `private` advisory. Listing only visible fields keeps 06 and 09 true.
- **Only methods are generated, and only through annotations:** most real uses (printing, comparing, hashing, serializing, enum names) only need to *read* types, plus derive-style methods on a type its author marked (see [Annotations](annotations.md)). Not generating whole types or free functions means every name has a declaration a reader can find. Code generation beyond that is a build step (see [Edax as a Build System](../../build/build-system.md)).
- **Registries from the import graph only:** `functions_with` sees exactly what the target compiles, so its result is deterministic and changes only when the code does.
- **Methods as handles with `m.call(...)`:** the reflected call is checked like a written one, so reflection never bypasses types, `mut` or privacy.
- **Generated fields with written names:** engines need SoA storage and mirrors of their structs. Taking every generated name from a declared field keeps "every name has a written declaration" true and keeps tools working.
- **RTTI on request:** C++ either has RTTI for everything or for nothing. Edax emits it for the types that ask, built from the same reflection compile-time code uses.
