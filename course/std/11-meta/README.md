# Std lesson 11: Reflection and annotations (`std::meta`)

C++ programmers write serializers, property editors and `enum` ↔ string tables by hand, or with macros, or with a code generator, because the language can't look at a type's fields. Edax can, at compile time. **Types are values**, `std::meta` describes a type's fields and alternatives, and `for compile_time` copies a loop body once per field so each copy can work with a different type. **Annotations** attach data and checks to declarations, and can generate methods. The `std::derive` annotations you've used are written this way, in ordinary Edax.

**You will learn:** `std::meta::fields`, `alternatives`, `name_of` / `from_name`, `for compile_time` and `f.get(value)`, compile-time type values (`compile_time type ft = f.type;`), declaring annotations, reading them, checking with them, and generating members.

**You will build:** a generic settings serializer that writes and reads any struct (`exercise.ed`).

Examples are in [`examples/`](examples); `python3 course/check.py s11 --examples` checks them.

---

## 1. Walking fields

```edax
template<type T>
std::string describe(T& value) {
    mut std::string out = std::format(f"{std::type_name<T>()}(");
    for compile_time (std::meta::field f : std::meta::fields<T>()) {
        if compile_time (f.index > 0) { out.push_str(", "); }
        out.push_str(std::format(f"{f.name}={f.get(value)}"));
    }
    out.push_str(")");
    return out;
}
```

- `std::meta::fields<T>()` is a `std::span<std::meta::field>`, computed at compile time. A field handle has `name`, `type`, `index` and `offset`.
- **`for compile_time`** copies the body once per field. In each copy `f` is a compile-time constant, so `f.get(value)` has that field's own type (a `std::string` in one copy, a `u32` in the next) and each copy is type-checked on its own. A normal `for` couldn't do that, because one body would need one type.
- `f.get(value)` is a reference to that field; `f.get(mut value)` a mutable one, which you can assign to.
- **Privacy is respected**: you see the fields visible where the code is written. There's no reflective way around `private`.
- Reflection runs at compile time. To use a result at run time, put it in a `compile_time` constant: `compile_time usize field_count = std::meta::fields<player>().length();`.

Example: [`fields.ed`](examples/fields.ed).

## 2. Types as values

Inside the loop, `f.type` is the field's type as a **value of type `type`**. To use it as a type, give it a name first:

```edax
compile_time type ft = f.type;
if compile_time (std::meta::is_enum<ft>()) { ... }
std::option<ft> parsed = v.parse<ft>();
```

Useful questions about a type: `std::is_integer<T>()`, `std::is_float<T>()`, `std::is_same<A, B>()`, `std::implements<T, I>()`, `std::meta::is_struct<T>()`, `is_class`, `is_enum`, `std::size_of<T>()`, `std::type_name<T>()`. All compile-time, all usable in `if compile_time`.

## 3. Enums

| Call | Gives |
|---|---|
| `std::meta::name_of(value)` | `"high"` for `quality::high`; works at run time |
| `std::meta::from_name<E>(text)` | `std::option<E>`; works at run time |
| `std::meta::alternatives<E>()` | handles with `name`, `index`, `has_payload`, `payload` |

That's the `enum` ↔ string table every C++ codebase writes by hand, with nothing to keep in sync.

Example: [`enums.ed`](examples/enums.ed).

## 4. Annotations: data, checks, generated code

An annotation is a struct declared with `annotation(targets)`:

```edax
annotation(std::meta::target::field) struct label {
    std::static_str text;
}
annotation(std::meta::target::field) struct hidden { }

struct audio_settings {
    @label{ .text = "Master volume" } u32 volume = 80;
    @hidden u64 cache_key = 0;
}
```

| An annotation can | How |
|---|---|
| **carry data** for generic code | `f.has<hidden>()`, `f.annotation<label>()` (an `std::option`) |
| **check** where it's written | `compile_time check` inside it; `annotation` is its value, `annotated` what it's on |
| **generate members** of the type it's on | a `generate { ... }` block (target `type`) |
| **wrap** a function body | a `wrap { ... }` block with `std::meta::wrapped_body()` (target `function`) |

It can never change code it isn't written on: every effect starts at a visible `@`.

A check runs once per place the annotation is written, and fails **at the annotation**:

```edax
annotation(std::meta::target::field) struct range {
    i64 min;
    i64 max;
    compile_time check (annotation.min <= annotation.max)
        else f"@range: min ({annotation.min}) is larger than max ({annotation.max})";
}
```
```
error[E4210]: @range: min (100) is larger than max (0)
```

A `generate` block adds methods as if the type's author had written them; inside, `this` is the object and `annotated` its type:

```edax
annotation(std::meta::target::type) struct resettable {
    generate {
        void reset() mut {
            for compile_time (std::meta::field f : std::meta::fields<annotated>()) {
                f.get(mut this) = default;
            }
        }
    }
}
```

`edaxc expand file.ed counters` prints the type with its generated members written out. Now read `library/std/derive.edh`: `formattable`, `hashable` and `ordered` are 20 lines each, written exactly like this.

Examples: [`annotations.ed`](examples/annotations.ed), [`err_annotations.ed`](examples/err_annotations.ed), [`generate.ed`](examples/generate.ed).

---

## C++ → Edax at a glance

| C++ | Edax |
|---|---|
| Boost.PFR / macros to list fields | `std::meta::fields<T>()` |
| `magic_enum::enum_name(e)` | `std::meta::name_of(e)` |
| `magic_enum::enum_cast<E>(s)` | `std::meta::from_name<E>(s)` |
| `[[attributes]]` (ignored by the compiler) | annotations that carry data, check and generate |
| a code generator for `operator<<` | `@std::derive::formattable` |
| `std::conditional_t`, `decltype` games | `compile_time type t = ...;` |
| C++26 reflection `^^T` / `[: :]` | `std::meta::*` + `for compile_time` |

---

## Your goal

`exercise.ed` defines a `graphics` settings struct with annotations: `@key` gives a field another name in the text, `@skip` leaves one out. Write the generic `to_text` and `from_text` (they must work for any struct, not just `graphics`), and the check in `@key`, so that it prints:

```
resolution_width = 1280
resolution_height = 720
gamma = 2.2
vsync = true
shadows = medium
2 lines unused
resolution_width = 2560
resolution_height = 720
gamma = 1.8
vsync = false
shadows = high
```

(Of the six input lines, `colour = blue` matches no field, and `shadows = epic` isn't a `quality`.)

<details>
<summary>Hint 1: the name of a field in the text</summary>

`compile_time std::static_str name = f.annotation<key>().value_or(key{ .text = f.name }).text;`
</details>

<details>
<summary>Hint 2: <code>to_text</code></summary>

Inside `for compile_time (std::meta::field f : std::meta::fields<T>())` and `if compile_time (!f.has<skip>())`: `compile_time type ft = f.type;` then `if compile_time (std::meta::is_enum<ft>())` use `std::meta::name_of(f.get(value))`, else `f.get(value)`, in `out.push_str(std::format(f"{name} = ...\n"))`.
</details>

<details>
<summary>Hint 3: <code>from_text</code></summary>

The run-time loop goes over lines; inside it, the `for compile_time` loop goes over fields, and a run-time `if (name == field_name)` picks the one. Then `if (v.parse<ft>() is std::some(parsed)) { f.get(mut value) = parsed; used = true; }`, or `std::meta::from_name<ft>(v)` for enums.
</details>

<details>
<summary>Hint 4: the check in <code>@key</code></summary>

`compile_time check (!annotation.text.is_empty()) else "@key: the name can't be empty";` in the annotation's body.
</details>

---

## Try this afterwards

1. Write `@key{ .text = "" }` on a field.
2. Add a field `std::string title = default;` to `graphics`. Does `to_text` still compile? And `from_text`?
3. Call `to_text` on a struct from another lesson, for example `audio_settings`.
4. Run `build/compiler/edaxc expand course/std/11-meta/examples/generate.ed counters`.

<details>
<summary>What happens</summary>

1. `error[E4210]: @key: the name can't be empty`, at that annotation.
2. `to_text` does: a string can be formatted. `from_text` doesn't: `parse` only reads numbers and `bool`, and for `ft` = `std::string` the error is a confusing `expected 'std::option<std::string>', found 'std::option<f64>'`. Add a branch `if compile_time (std::is_same<ft, std::string>())` that assigns `std::string::from(v)`.
3. It works unchanged, because nothing in it names `graphics`.
4. `counters` printed with `reset()` and `format()` written out, as the compiler sees them.
</details>

## Recap

- `std::meta::fields<T>()` and `for compile_time` walk a type's fields with each field's own type; `f.get` reads and writes them.
- `compile_time type t = f.type;` turns a type value back into a type.
- `name_of` / `from_name` replace enum string tables.
- Annotations carry data, check where they're written, generate methods, and wrap bodies; `std::derive` is built from them.

Next: [std lesson 12](../12-sync/README.md), synchronization in depth.
