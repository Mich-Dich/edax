# 29 - Annotations

An **annotation** attaches compile-time information to a declaration:
```rift
@std::derive::formattable
struct sensor_reading {
    @json::name{ .text = "id" } u32 sensor_id;
    @range{ .min = -40, .max = 125 } i16 temperature;
    @json::skip u64 cache_key = 0;
}
```
Annotations are ordinary structs, declared with `annotation`. An annotation can do three things, and nothing else:

| Capability | Example | Section |
|---|---|---|
| **Carry data** that generic code reads through reflection | `@json::name{ .text = "id" }` read by a serializer | 3 |
| **Check** the declaration it's written on | `@range` rejects a non-integer field | 4 |
| **Generate members** for the type it's written on | `@std::derive::formattable` adds a `format` method | 5 |

An annotation can never change code it isn't written on. Every effect starts at a visible `@` on the affected declaration.

## Rules

### 1. Declaring an annotation
```rift
annotation(std::meta::target::field) struct range {
    i64 min;
    i64 max;
}
```
- `annotation(targets...)` before `struct` makes the struct an annotation. The parameters list where it may be written, in the same `keyword(parameter)` form as `run_time(std::build::mode::debug)` (see 23):

| `std::meta::target::...` | Written on |
|---|---|
| `type` | struct, class, enum, interface |
| `field` | a field of a struct or class |
| `function` | a function or method |
| `parameter` | a function parameter |
| `alternative` | an enum alternative |
| `file` | a `.rifts` file: `@name{ ... };` after the imports (see 31). Its checks don't run yet |

- Fields must be compile-time-capable types: numbers, `bool`, `char`, `std::static_str`, `type`, simple enums, other structs of these, `std::span`s of these.
- An annotation is otherwise a normal struct: it has a namespace (see 30), it can be `private` to its module, and it can have default field values.

### 2. Writing an annotation
```rift
@range{ .min = 0, .max = 100 }       // with values: the T{...} form of 03
@json::skip                          // all fields have defaults: just the name
```
- An annotation stands directly before what it annotates: before the declaration (type, function, method), before the field's type, before the parameter's type, or before the alternative's name.
- The values are compile-time values (see 15).
- Several annotations may stand on one declaration, each with its own `@`. The same annotation twice on one declaration is an error.
- Annotations belong to the declaration, so they're written where the declaration is written. For a declaration in a `.rifth`, that's the header (see 09); the `.rifts` doesn't repeat them.
- Writing an annotation on a target it doesn't allow is an error naming the allowed targets.

### 3. Reading annotations
Reflection (see 26) can ask any declaration for its annotations. All of these are `compile_time`:

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
An annotation may contain `compile_time` checks (see 23). They run **once for every place the annotation is written**, and they can see what it is written on:

| Name (only inside an annotation) | Is |
|---|---|
| `annotation` | The annotation's value as written at that place: `annotation.min` |
| `annotated` | What it's written on: a `type` value (target `type`), or a `std::meta::field` / `function` / `parameter` / `alternative` |

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
- **Headers and modules:** the generated members are part of the type's interface. They're in the compiled module interface (see 09) because the annotation is on the declaration. Importers see them without seeing the annotation's code.

### 6. Annotations in the standard library
| Annotation | On | Effect |
|---|---|---|
| `@std::derive::formattable` | type | Implements `std::formattable`: `point { x: 1.0, y: 2.5 }` for structs, the alternative's name (and payload) for enums |
| `@std::derive::hashable` | type | Implements `std::hashable` (`u64 hash()`) from all fields, for `std::hash_map` keys |
| `@std::derive::ordered` | type | Generates `operator<=>` comparing fields in declaration order (like `operator== = default` in 16, for `<=>`) |
| `@std::deprecated{ .message = "..." }` | type, function, field, alternative | A warning at every use, at the use, with the message. Id `std::deprecated` (see 23) |
| `@std::allow{ .id = gfx::deprecated }` | function | Silences one warning id inside that function (closes open question 19) |
| `@std::must_use` | function, type | Ignoring the result is an error (as for `std::result`, see 11). Discard with `_ =` |
| `@std::inline_always`, `@std::inline_never` | function | Overrides the optimizer's inlining decision |
| `@std::cold` | function | The function is rarely called (error paths). The optimizer moves it, and branches to it, out of the hot path |
| `@std::test` | function | A test: `riftc test` finds and runs every `@std::test` function of the test targets (see 25). The function takes no parameters and returns `void` |

The `derive` annotations are ordinary annotations with `generate` blocks, written in Rift. They're in `std` because they're needed often, not because they're special.

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
- **`generate` writes members, not text:** the block is real Rift code, type-checked once in the annotation and again per use, with normal error messages. It is the safe subset of "generating declarations" (open question 27): it extends a type its author chose to extend, and can't create new names elsewhere.
- **No fields from `generate`:** generated fields would change layouts based on code far away and break the merge rules of 09. Methods are enough for derive-style uses.
- **`riftc expand`:** generated code you can't see is generated code you can't trust or debug.
- **Built-in behaviour as library annotations:** deprecation, warning control, inlining and test discovery would otherwise each need their own syntax. As annotations they share one form, and `std::deprecated` is an ordinary annotation with a check.
