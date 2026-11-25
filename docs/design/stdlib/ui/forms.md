# UIs from types: annotations, forms, `inspect`

Editors made from a type's fields by reflection ([draft 35](../../drafts/35-ui.md) §7). The annotations and forms are in `std::ui` (any front-end can use them); `inspect` and `field` are methods of [`im::scope`](immediate.md).

## Rules

### Three levels
| Level | Code |
|---|---|
| Whole type | `w.inspect("player", mut player)` (under a header), `w.inspect_fields(mut player)` (in place) |
| One field | `w.field(mut player, "movement.speed")`: the path is checked at compile time |
| By hand | `w.slider("Speed", mut player.movement.speed, 0.0, 20.0)` |

### What a field becomes
| Field type | Widget |
|---|---|
| `bool` | The theme's bool widget (`controls.boolean`); `@ui::widget` `checkbox` or `toggle` |
| Integers, floats | A slider with `@ui::range`, else a drag field (`@ui::step` per pixel); `@ui::widget{ .kind = drag }` with a range: a bounded number field |
| `std::string` | A text field |
| Enum without payloads | Segments up to 4 alternatives, a combo above; `@ui::widget` `segmented` or `combo` |
| Struct, class | A tree node with its fields; `@ui::flatten`: the fields in place |
| Enum with payloads | A combo for the alternative, the payload's editor below it (a new alternative gets a default payload) |
| `std::math::vec2/3/4` | One number field per component in a row, marked x red, y green, z blue |
| `std::math::color` | A swatch and r, g, b, a fields in 0..1 |
| `std::option<T>` | A checkbox (set or none) and `T`'s editor while it is set |
| `std::darray<T>` | A tree node with an editor per element ("#0", "#1", ...), "×" to remove one, "Add" for a default element; elements are at the path `name[]` (`.range("items[].weight", ...)`) |
| A type implementing `im::inspectable` | Its own `inspect(mut im::scope&)` |
| Anything else | Its type name, read only |

A field's label is its name with `_` as spaces and a capital first letter (`top_speed` → "Top speed"), or `@ui::label`; a unit is shown after it ("Top speed (km/h)").

### Annotations
| Annotation | On | Effect | Checked |
|---|---|---|---|
| `@ui::label{ .text }` | field, alternative, method | Shown name | |
| `@ui::tooltip{ .text }` | field, method | Hover text | |
| `@ui::range{ .min, .max }` | field | Slider bounds | number field; `min <= max` |
| `@ui::step{ .value }` | field | Drag step | number field; > 0 |
| `@ui::unit{ .text }` | field | After the label | number field |
| `@ui::widget{ .kind }` | field | Another widget | the kind fits the type |
| `@ui::group{ .name }` | field | Fields with the same group under one section, where the first is | |
| `@ui::read_only`, `@ui::hidden`, `@ui::flatten` | field | As named | |
| `@ui::action{ .label }` | method | A button calling the method (no parameters) | |

A check that fails is an error at the annotation, naming the field and its type.

### Forms: `ui::form<T>`
A form changes the annotations for one call. `ui::form<T>::annotated()` changes nothing; each method changes one field and returns the form:

| Method | Changes |
|---|---|
| `label`, `tooltip`, `unit`, `group` | Presentation |
| `range(f, min, max)`, `min`, `max`, `step` | Limits (number fields) |
| `show`, `hide`, `hide_all`, `read_only`, `editable` | Visibility; after `hide_all()` only what `show` names (and what is inside or around it) |
| `widget(f, kind)` | Another widget |
| `rule(f, text_rule)`, `rule_number(f, number_rule)` | A rule on a text or number field: its message shows under the field |
| `rule_across(f, check)` | A rule over the whole value, shown under `f` |
| `visible_if(f, condition)` | `f` only while `condition(value)` holds |
| `range_all(min, max)`, `step_all(s)` | Every number field of `T` and of the types inside it |

- `f` is a path: `"speed"`, `"tuning.gear_ratio"`. It is checked at compile time: a missing field is an error at the call, with the nearest name; so is a range on a text field or a text rule on a number field.
- `ui::rules::` has `not_empty`, `min_length(n)`, `max_length(n)`, `email`, `at_least(v)`, `at_most(v)`.
- A form is a run-time value (its strings and rules live on the heap); built per frame it costs a few small allocations, built once it costs nothing per frame.

```edax
ui::form<vehicle> player = ui::form<vehicle>::annotated()
    .hide_all()
    .show("paint")
    .show("tuning.gear_ratio")
    .range("tuning.gear_ratio", 2.5, garage.max_ratio());       // known only at run time
ui::edit e = w.inspect("your car", mut car, player);
```

### Forms for types: `with_form`, `ctx.set_form`
- `w.with_form(ui::form<U>...)` returns a scope in which every `U` the editors show uses the form, wherever it is nested (a field of a field, an element of a list); `ctx.set_form(ui::form<U>...)` does it for the whole context (a later call for `U` replaces it). The form is moved in.
- Paths in these forms are relative to `U` (`"z"`, `"items[].weight"`); `hide_all`, `range_all` and `step_all` work too. Rules, `rule_across` and `visible_if` belong to the form of a call.
- **Precedence**, weakest first: the type's widget → its annotations → `ctx.set_form` → `with_form` scopes, inner over outer → the form passed to the call.
```edax
{
    mut im::scope tools = w.with_form(ui::form<vec3>::annotated().step_all(0.1).range_all(-100.0, 100.0));
    _ = tools.inspect("player", mut player);       // player.position and player.velocity use it
    _ = tools.inspect("camera", mut camera);
}
```

### Widget sets: `with_widgets`
`ui::widget_set` says which widget each kind of field becomes when its annotations and forms don't: `boolean` (toggle or checkbox), `ranged` and `unranged` numbers (`drag`, `slider` or `stepper`), `segment_limit` (enums up to it as segments, more as a combo), `radio_buttons`, and a `density` that scales the theme. `w.with_widgets(set)` uses it in a scope. The library's sets are `ui::widget_sets::tool()` (the default, compact), `touch()` (steppers, 1.25 density) and `gamepad()` (steppers and radio buttons); a set is a value, so a program copies one and changes single entries. `w.stepper(label, mut v, min, max, step)` is the stepper on its own.

### Widgets for types the program doesn't own: `@ui::field_widget`
A free function `ui::edit name(mut im::scope& s, std::str label, mut T& value)` with `@ui::field_widget{ .for_type = T }` is used for every `T` field that `inspect` and `field` show; the library finds it with `std::meta::functions_with` at compile time.
```edax
@ui::field_widget{ .for_type = physics::temperature }
ui::edit temperature_widget(mut im::scope& s, std::str label, mut physics::temperature& t) {
    mut f32 celsius = t.kelvin - 273.15;
    bool changed = s.slider(label, mut celsius, -40.0, 60.0);
    t.kelvin = celsius + 273.15;
    return ui::edit{ .changed = changed };
}
```

### Results: `ui::edit`
`inspect` and `field` return what happened this frame: `changed`, `committed` (the drag was released, Enter pressed, a toggle or button clicked; a change made while a drag is held or text is typed is committed when that ends), `valid` (no rule fails; `committed` stays false while one does) and `path` (the field that changed: `"tuning.gear_ratio"`, or an action's method name).

### Undo: `ui::undo_history<T>`
- `history.record(e, value)` after each editor: the first call keeps the starting state, each committed edit adds a step. `undo(mut value)` and `redo(mut value)` step through them; `w.undo_shortcuts(mut history, mut value)` binds Ctrl+Z and Ctrl+Y / Ctrl+Shift+Z while nothing is typed.
- Steps are snapshots made by `ui::snapshot<T>`: numbers, bools and enums copied, strings, lists and options copied element by element, structs field by field (they need a default). Field diffs for large types are planned.

### Tables, tabs, radio buttons
- `w.table(label, rows, height)`: one column per field of the row type (not `@ui::hidden`, headers as labels), a click on a header sorts by it (again: the other way, a third time: unsorted; numbers, bools, texts, enums by declaration order), a click selects a row (up and down move the selection while the table has the focus). Only the rows in view are drawn and hit-tested: 100 000 rows cost what twenty do. Returns the row clicked this frame; `w.table_selection(label)` the selected one.
- `w.tabs(mut page)`: one tab per alternative of an enum, the indicator slides; the program shows the page below. `w.radio(label, mut choice)`: radio buttons. Both read `@ui::label` on alternatives.

## Example
`example/editor` shows one `vehicle` with a designer's form and a player's form side by side, logs the committed edits with undo and redo, and lists a fleet of 10 000 cars in a table (`--fleet` for its screenshot).

## Rationale
- **Annotations are defaults, forms are per place:** one type is shown in several places with different rules (a designer's range and a player's, a field only an editor shows), and its annotations may be in a library the program can't change.
- **Paths as strings checked at compile time:** no new syntax, the errors name the nearest field, and renaming a field breaks the build where its old name is used.
- **`visible_if` in the form, not an annotation:** an annotation holds compile-time values only; a condition is a function of the whole value.
