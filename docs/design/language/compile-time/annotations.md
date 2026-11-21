# Annotations

An **annotation** attaches compile-time information to a declaration:
```rift
@std::derive::formattable
struct sensor_reading {
    @json::name{ .text = "id" } u32 sensor_id;
    @range{ .min = -40, .max = 125 } i16 temperature;
    @json::skip u64 cache_key = 0;
}
```
Annotations are ordinary structs, declared with `annotation`. An annotation can do four things, and nothing else:

| Capability | Example | Section |
|---|---|---|
| **Carry data** that generic code reads through reflection | `@json::name{ .text = "id" }` read by a serializer | 3 |
| **Check** the declaration it's written on | `@range` rejects a non-integer field | 4 |
| **Generate members** for the type it's written on | `@std::derive::formattable` adds a `format` method | 5 |
| **Wrap the body** of the function it's written on | `@timed` measures every call | 8 |

An annotation can never change code it isn't written on. Every effect starts at a visible `@` on the affected declaration.

## Rules

### 1. Declaring an annotation
```rift
annotation(std::meta::target::field) struct range {
    i64 min;
    i64 max;
}
```
- `annotation(targets...)` before `struct` makes the struct an annotation. The parameters list where it may be written, in the same `keyword(parameter)` form as `run_time(std::build::mode::debug)` (see [Checks](../errors/checks.md)):

| `std::meta::target::...` | Written on |
|---|---|
| `type` | struct, class, enum, interface |
| `field` | a field of a struct or class |
| `function` | a function or method |
| `parameter` | a function parameter |
| `alternative` | an enum alternative |
| `file` | a `.rifts` file: `@name{ ... };` after the imports (section 7) |

- Fields must be compile-time-capable types: numbers, `bool`, `char`, `std::static_str`, `type`, simple enums, other structs of these, `std::span`s of these.
- An annotation is otherwise a normal struct: it has a namespace (see [Namespaces](../organization/namespaces.md)), it can be `private` to its module, and it can have default field values.

### 2. Writing an annotation
```rift
@range{ .min = 0, .max = 100 }       // with values: the T{...} form of 03
@json::skip                          // all fields have defaults: just the name
```
- An annotation stands directly before what it annotates: before the declaration (type, function, method), before the field's type, before the parameter's type, or before the alternative's name.
- The values are compile-time values (see [Compile-Time Evaluation](compile-time.md)).
- Several annotations may stand on one declaration, each with its own `@`. The same annotation twice on one declaration is an error.
- Annotations belong to the declaration, so they're written where the declaration is written. For a declaration in a `.rifth`, that's the header (see [Headers, Sources and Modules](../organization/modules.md)); the `.rifts` doesn't repeat them.
- Writing an annotation on a target it doesn't allow is an error naming the allowed targets.
- **On a statement**, only `@std::allow` may be written: `@std::allow{ .id = gfx::deprecated } draw_quad(1.0, 2.0);` silences that warning for that statement alone.

### 3. Reading annotations
Reflection (see [Metaprogramming](metaprogramming.md)) can ask any declaration for its annotations. All of these are `compile_time`:

| Call | Result |
|---|---|
| `std::meta::annotation<A, T>()` | `std::option<A>`: annotation `A` on type `T` |
| `f.annotation<A>()` on a `std::meta::field`, `function`, `parameter` or `alternative` | `std::option<A>` |
| `f.has<A>()` (and `std::meta::has_annotation<A, T>()` for types) | `bool` |

```rift
for compile_time (std::meta::field f : std::meta::fields<T>()) {
    if compile_time (!f.has<json::skip>()) {
        compile_time std::static_str key = f.annotation<json::name>().value_or(json::name{ .text = f.name }).text;
        out.write_key(key);
        out.write_value(f.get(value));
    }
}
```

### 4. Checks in annotations
An annotation may contain `compile_time` checks (see [Checks](../errors/checks.md)). They run **once for every place the annotation is written**, and they can see what it is written on:

| Name (only inside an annotation) | Is |
|---|---|
| `annotation` | The annotation's value as written at that place: `annotation.min` |
| `annotated` | What it's written on: a `type` value (target `type`), or a `std::meta::field` / `function` / `parameter` / `alternative` / `file` |

```rift
annotation(std::meta::target::field) struct range {
    i64 min;
    i64 max;

    compile_time check (annotation.min <= annotation.max)
        else f"@range: min ({annotation.min}) is larger than max ({annotation.max})";
    compile_time check {
        compile_time type t = annotated.type;
        if (!std::is_integer<t>()) {
            std::compile_error(f"@range works on integer fields; '{annotated.name}' is a {std::type_name<t>()}");
        }
    }
}
```
A failed check is reported **at the annotation**, in the user's code, with a note pointing to the check (the location rule of 23).

### 5. Generating members
An annotation with target `type` may contain one **`generate` block**. Its contents are added to every type the annotation is written on, **as if the type's author had written them in the type**:
```rift
annotation(std::meta::target::type) struct hashable {
    generate : std::hashable {
        u64 hash() override {
            mut u64 h = std::hash_seed();
            for compile_time (std::meta::field f : std::meta::fields<annotated>()) {
                h = std::hash_combine(h, std::hash(f.get(this)));
            }
            return h;
        }
    }
}
```
- **`generate : interfaces { members }`.** The optional interface list is added to the type's base list, and the members (methods, `static` methods, operators) are added to the type. Only methods: a `generate` block can't add fields.
- **Inside the block** `this` is the annotated object, exactly as in a hand-written method. `annotated` is the annotated type and `annotation` the annotation's values.
- **Generated members are members.** They follow every rule of 05/06: they're public in a struct, and in a class they go into the section they're written under (`public:` by default). They see the type's private fields, because the type's author opted in by writing the annotation.
- **Conflicts are errors:** a generated member with the same signature as a written one, or as one from another annotation, is an error that names both.
- **Generic types:** on a template type, the block is generated per instance, and `annotated` is the instance.
- **Errors in generated code** are reported at the `@` that caused it, with a note into the `generate` block.
- **You can see the result:** `riftc expand <file> <type>` prints the type with every generated member written out.
- **Headers and modules:** the generated members are part of the type's interface. They're in the compiled module interface (see [Headers, Sources and Modules](../organization/modules.md)) because the annotation is on the declaration. Importers see them without seeing the annotation's code.

### 6. Annotations in the standard library
| Annotation | On | Effect |
|---|---|---|
| `@std::derive::formattable` | type | Implements `std::formattable`: `point { x: 1.0, y: 2.5 }` for structs, the alternative's name (and payload) for enums |
| `@std::derive::hashable` | type | Implements `std::hashable` (`u64 hash()`) from all fields, for `std::hash_map` keys |
| `@std::derive::ordered` | type | Generates `operator<=>` comparing fields in declaration order (like `operator== = default` in 16, for `<=>`) |
| `@std::deprecated{ .message = "..." }` | type, function, field, alternative | A warning at every use, at the use, with the message. Id `std::deprecated` (see [Checks](../errors/checks.md)) |
| `@std::allow{ .id = gfx::deprecated }` | function, statement | Silences one warning id inside that function, or in that one statement |
| `@std::plugin_entry` | class | The class a Rift plugin provides (see [Dynamic Libraries and Plugins](../../stdlib/system/dynamic-libraries.md)) |
| `@std::simd::multiversion{ .levels = [...] }` | function | Compiled once per CPU level; the best one is chosen at the first call (see [SIMD](../simd/simd.md)) |
| `@std::simd::must_vectorize` | function | Every plain `for` loop in it must become vector code, or the build fails with the reason |
| `@std::check_result` | function | The function returns `bool` and reports its own failure; a call to it may be followed by `else { ... }` (see [Control Flow](../basics/control-flow.md)) |
| `@std::must_use` | function, type | Ignoring the result is an error (as for `std::result`, see [Error Handling](../errors/error-handling.md)). Discard with `_ =` |
| `@std::inline_always`, `@std::inline_never` | function | Overrides the optimizer's inlining decision |
| `@std::cold` | function | The function is rarely called (error paths). The optimizer moves it, and branches to it, out of the hot path |
| `@std::test` | function | A test: `riftc test` finds and runs every `@std::test` function of the test targets (see [Rift as a Build System](../../build/build-system.md)). The function takes no parameters and returns `void` |

The `derive` annotations are ordinary annotations with `generate` blocks, written in Rift. They're in `std` because they're needed often, not because they're special.

### 7. File annotations
An annotation followed by `;` at file scope **annotates the file**:
```rift
import std;

@std::log::level_override{ .level = std::log::level::trace };
```
- File annotations stand after the imports and before the first declaration.
- Only in `.rifts` files: they describe the code written in that file. A header can't have them.
- The annotation must allow the target `std::meta::target::file` (see above).
- Compile-time code reads them with `std::meta::file_annotation<A>(where)`, where `where` is a `std::call_site` in that file (see [Functions](../functions/functions.md), [Metaprogramming](metaprogramming.md)).
- An annotation's checks run for a file annotation like for any other target; `annotated` is then a `std::meta::file` with the file's `name` and `module`.

### 8. Wrapping a function body
An annotation with target `function` may contain one **`wrap` block**. It replaces the body of every function the annotation is written on; inside it, **`std::meta::wrapped_body()`** runs the function's own body and gives its result:
```rift
annotation(std::meta::target::function) struct timed {
    std::static_str label = "";

    wrap {
        std::time::instant start = std::time::instant::now();
        if compile_time (annotated.result == void) {
            std::meta::wrapped_body();
            std::log::debug(f"{annotation.label}: {start.elapsed()}");
        } else {
            auto result = std::meta::wrapped_body();
            std::log::debug(f"{annotation.label}: {start.elapsed()}");
            return move result;
        }
    }
}

@timed{ .label = "load scene" }
std::result<scene, load_error> load_scene(std::str path) { ... }
```
- `std::meta::wrapped_body()` must be called **exactly once on every path** through the `wrap` block; anything else is an error at the annotation.
- The original body behaves as if it were a lambda: its `return` (and `?`) ends the body and gives `wrapped_body()` its value; the `wrap` block then decides what the function returns.
- The `wrap` block sees `annotation` and `annotated` (a `std::meta::function`), but not the parameters: it can't change the arguments the body sees.
- Several wrapping annotations on one function nest in the order they're written: the first is the outermost.
- Entry checks of the function (see [Checks](../errors/checks.md)) run before any `wrap` block.
- `riftc expand <file> <function>` shows the function with every `wrap` applied.

## Examples

### 1. One line instead of a hand-written method
```rift
@std::derive::formattable
@std::derive::hashable
struct grid_cell {
    i32 x;
    i32 y;
}

std::println(f"{cell}");                                  // grid_cell { x: 3, y: -1 }
mut std::hash_map<grid_cell, u32> visits = default;       // grid_cell is a key now
```

### 2. A JSON writer driven by annotations
```rift
namespace json {
    annotation(std::meta::target::field) struct name { std::static_str text; }
    annotation(std::meta::target::field) struct skip { }

    template<type T>
    void write_object(mut writer& out, T& value) {
        out.begin_object();
        for compile_time (std::meta::field f : std::meta::fields<T>()) {
            if compile_time (!f.has<json::skip>()) {
                compile_time std::static_str key = f.annotation<json::name>().value_or(json::name{ .text = f.name }).text;
                out.key(key);
                json::write_value(mut out, f.get(value));      // overloads for numbers, strings, nested objects
            }
        }
        out.end_object();
    }
}
```
```rift
struct sensor_reading {
    @json::name{ .text = "id" } u32 sensor_id;
    i16 temperature;
    @json::skip u64 cache_key = 0;
}
// json::write_object(mut out, reading) writes {"id": 7, "temperature": 21}
```

### 3. Validation generated from field annotations
```rift
annotation(std::meta::target::type) struct validated {
    generate {
        std::result<void, std::static_str> validate() {
            for compile_time (std::meta::field f : std::meta::fields<annotated>()) {
                if compile_time (f.has<range>()) {
                    compile_time range r = f.annotation<range>().expect("checked by has");
                    if (cast<i64>(f.get(this)) < r.min || cast<i64>(f.get(this)) > r.max) {
                        return std::error(f.name);                 // name of the first field out of range
                    }
                }
            }
            return std::ok();
        }
    }
}

@validated
struct motor_command {
    @range{ .min = -1000, .max = 1000 } i32 rpm;
    @range{ .min = 0, .max = 100 }      u8  duty_percent;
}
```
Each `@range` checks at compile time that its field is an integer (section 4); `@validated` generates one `validate()` method that checks the values at run time.

### 4. Tests
```rift
@std::test
void parses_negative_numbers() {
    run_time check ("-12".parse<i32>().value_or(0) == -12) else "parse failed";
}
```

## Rationale
- **Annotations are structs:** no new kind of thing to learn. They're declared, namespaced and documented like any type, and their values are written with the `T{...}` syntax everyone already knows.
- **Three capabilities, all visible:** data, checks and generated members. All of them start at an `@` on the declaration they affect, so reading a declaration tells you everything that applies to it. There is no global registration, no hidden pre-processing step, and an annotation can't reach code it isn't written on.
- **`generate` writes members, not text:** the block is real Rift code, type-checked once in the annotation and again per use, with normal error messages. It extends a type its author chose to extend, and can't create new names elsewhere: Rift doesn't generate whole types or free functions, so every name has a written declaration.
- **No fields from `generate`:** generated fields would change layouts based on code far away and break the merge rules of 09. Methods are enough for derive-style uses.
- **`riftc expand`:** generated code you can't see is generated code you can't trust or debug.
- **`wrap` sees the body, not the arguments:** tracing, timing and profiling need code before and after a call; they never need to change what the function computes. Hiding the parameters keeps a wrapped function's behaviour readable from its own body.
- **Only `@std::allow` on statements:** silencing one warning on one line is useful; arbitrary statement annotations would invite code generation inside function bodies.
- **Built-in behaviour as library annotations:** deprecation, warning control, inlining and test discovery would otherwise each need their own syntax. As annotations they share one form, and `std::deprecated` is an ordinary annotation with a check.
