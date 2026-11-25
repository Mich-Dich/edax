# `std::ui`: implementation status

The design is [draft 35](../design/drafts/35-ui.md); the plan is its [roadmap](../design/drafts/35-ui-roadmap.md). This file tracks what exists, in the format of `spec-34-status.md`. Legend: **done** (implemented and tested), **partial**, **planned**.

## Milestones
| Milestone | Status | Where | Tests |
|---|---|---|---|
| M0 API spike | **done** | `example/ui_spike` (throwaway; findings in its README) | `example/ui_spike/test.ed` (5 tests, no window), `--screenshot` run under Xvfb |
| M1 Foundations | **partial**: everything but the windowing additions and `std::format_into` | `library/std/{math,image,text/font,ui,ui/opengl}.ed*`, fonts and shaders built into edaxc; `example/ui_gallery` | `action/run/std_math`, `std_image`, `std_text_font`, `std_ui_draw_list`; the gallery's `--screenshot` under Xvfb |
| M2 Core and immediate mode | **done** except `split` (moved to M5): the style system (tokens, skins, painters), context/frame/scope, windows (drag, resize, scroll), sections, trees, cards, rows, grids, scroll areas, sizes, styled scopes, keyboard focus and navigation, text fields, numbers, combos, tooltips, routing, the own-widget API, the debug overlay of `example/windowing` | `library/std/ui.ed*` (style), `ui/immediate.ed*`, `ui/platform.ed*`; `example/ui_widgets` | `action/run/std_ui_skin`, `std_ui_immediate`, `std_ui_keyboard`, `std_ui_layout`; `ui_widgets --screenshot [--custom]` under Xvfb |
| M3 Reflection and forms | **done** (small items left below): annotations with their checks, `ui::form<T>` (overrides, compile-time paths incl. `list[]`, rules, `visible_if`, `range_all`/`step_all`), forms for types (`with_form`, `ctx.set_form`) with the precedence of §7.8.4, widget sets (`tool`, `touch`, `gamepad`), `@ui::field_widget`, `inspect` for every type of §7.2 but maps, `field`, `ui::edit`, `ui::undo_history`, tables (sorting, selection, virtualized), tabs, radio, segments, steppers | `library/std/ui.ed*` (annotations, forms), `ui/immediate.ed*`; `example/editor` | `action/run/std_ui_forms`, `std_ui_tables`, `std_ui_scoped_forms`, `action/check/ui_form_paths`, `ui_annotations`; `editor --screenshot [--fleet]` |
| M4 Plots and debug tools | planned | | |
| M5 Docking | planned | | |
| M6 Node editor | planned | | |
| M7 Widget catalogue and polish | planned | | |
| M8 Compile time and performance | planned | | |
| M9 Reach | planned | | |
| M10 Retained mode | planned | | |

## M1 in detail
| Item | Status | Where |
|---|---|---|
| `std::math`: `vec2/3/4`, `rect`, `color` (sRGB, OKLCH, WCAG contrast), `lerp`, `smoothstep` | done | `math.ed*` |
| `std::image`: `rgba8`, PNG decode (all colour types, 1-16 bits, `tRNS`; not interlaced), QOI decode/encode, PNG encode (stored blocks), `inflate_zlib` | done | `image.ed*` |
| `std::text::font`: TrueType (`glyf`, cmap 4/12, composite glyphs, `kern`), outlines, distance fields; DM Sans 400/600 and DejaVu Sans Mono built in | done | `text/font.ed*`, `text/fonts/` |
| `std::ui` core: themes (tokens, `from_accent`, `mix`), `fonts` (distance-field atlas, measuring, wrapping), `draw_list` (SDF boxes, borders, shadows, gradients, circles, lines, images, text, clipping) | done | `ui.ed*` |
| `std::ui::opengl::renderer<A>`: one shader, one draw call per command, HiDPI scaling, `upload`, `read_pixels` | done | `ui/opengl.ed*`, `ui/shader.{vert,frag}` |
| `std::span<T>::as_ptr()`, `std::str::byte(i)` | done | `std.ed*` |
| Windowing: clipboard text, IME area | planned | |
| `std::format_into(mut std::string&, std::format_args)` | planned (needs a backend change: formatting writes into a runtime buffer) | |
| Screenshot tests in CI (Mesa llvmpipe under Xvfb) | planned; works by hand: `ui_gallery --screenshot` | |

## M2 in detail
| Item | Status | Where |
|---|---|---|
| Component tokens `theme.controls`: button, slider (knob shape: circle, square, diamond, star, bar), toggle, checkbox (mark: tick, cross, square, dot), window, row height, gap, `boolean` (toggle or checkbox) | done | `ui.edh` |
| Corner presets `radius_scale::rounded()`, `subtle()`, `sharp()`; `theme::mix` carries `controls` | done | `ui.ed` |
| Polygons in the draw list: `fill_polygon` (anti-aliased fringe, `shape_kind::solid`), `stroke_polygon`, `star`, `regular_polygon`, `tick`, `append` | done | `ui.ed`, `ui/shader.frag` |
| `ui::painter`, the parts (`button_part`, `check_part`, `toggle_part`, `slider_part`, `progress_part`, `header_part`, `window_part`, `card_part`, `interaction`), `ui::skin` with `standard()`, the painters `ui::paint::*` | done | `ui.ed*` |
| `im::context` (state by id, animations, theme fade with `set_theme`, skin, fonts, duplicate-id count, `wants_mouse`), `im::frame` (windows: drag, bring to front, one list each, stacked in `finish`) | done | `ui/immediate.ed*` |
| `im::scope`: `section`, `card`, `row`, `id`; `text`/`muted`/`heading` (wrapped), `button` (variants), `checkbox`, `toggle`, `boolean`, `slider<T>`, `progress`, `separator` | done | `ui/immediate.ed*` |
| Own-widget API (§11): `make_id`, `available_width`, `allocate`, `interact`, `animate`, `state`, `painter`, `theme` | done (an own `rating` widget in `example/ui_widgets` uses only these) | `ui/immediate.ed*` |
| `ui::input`, `ui::input_of(input_state, events, dt)`, `ui::window_size(size)` | done | `ui.edh`, `ui/platform.ed*` |
| Keyboard: `ui::key`, `key_press`, keys and typed text in `ui::input` (`input_of` reads them from the frame's events, with repeats); Tab/Shift+Tab focus order, Enter/Space press, arrows, Escape; the focus ring only while the keyboard is used; `wants_keyboard` | done | `ui.edh`, `ui/platform.ed*`, `ui/immediate.ed*` |
| `ui::text_edit` (caret, selection, word moves, clipboard, undo/redo, UTF-8) and `input_text` | done; the clipboard is the program's own until windowing has one | `ui.ed*`, `ui/immediate.ed*` |
| `number`, `drag` (drag, click to type, arrows), `combo`, `choice<E>` (enums by reflection), `tree`, `tooltip`, `styled` scopes (§3.3); the overlay layer for popups and tooltips | done | `ui/immediate.ed*` |
| Painters `tree_node`, `field`, `popup`, `list_item`, `tooltip` | done | `ui.ed*` |
| Debug overlay in `example/windowing` (dogfood) | done | `example/windowing/application.ed` |
| Sizes `ui::length` (`px`, `fit`, `fill`, `percent`) and `scope.width`; `fill` shares a row by weight from the last frame's measures | done | `ui.ed*`, `ui/immediate.ed*` |
| `grid(columns)`, `scroll(height)` with the wheel and a draggable bar (painter `scrollbar`); windows resized by a grip, then scrolling | done | `ui/immediate.ed*` |
| `im::router`: the context as a `wnd::input_layer` for `wnd::route`; taken keys and text go to the next frame | done; `example/windowing` routes its overlay | `ui/routing.ed*`, `modules.cpp`, `compiler/CMakeLists.txt` |
| `split` | moved to M5 (docking splits areas) | |

## M3 in detail
| Item | Status | Where |
|---|---|---|
| `@ui::label`, `tooltip`, `range`, `step`, `unit`, `widget`, `group`, `read_only`, `hidden`, `flatten`, `action`; checks: numbers only for range/step/unit, `min <= max`, step > 0, a widget that fits the type | done | `ui.edh` (declarations), `ui.ed` (checks) |
| Compile-time field paths: `class_of_path<T>`, `nearest_field<T>` (edit distance), `require_path<T>` (errors at the caller) | done | `ui.ed*` |
| `ui::form<T>`: `annotated`, `label`, `tooltip`, `unit`, `group`, `range`, `min`, `max`, `step`, `show`, `hide`, `hide_all`, `read_only`, `editable`, `widget`, `rule` (text), `rule_number`, `rule_across`, `visible_if`; built at run time | done | `ui.ed*` |
| `ui::verdict`, `ui::rules::not_empty`, `min_length`, `max_length`, `email`, `at_least`, `at_most` | done | `ui.ed*` |
| `ui::edit` (changed, committed after the drag or the typing ends, valid, path) | done | `ui.ed*`, `ui/immediate.ed` (`run_inspect`) |
| `scope.inspect(label, mut v[, form])`, `inspect_fields`, `field(mut v, "a.b")`; type → widget: bool, numbers (slider with a range, drag field), `std::string`, payload-free enums (segments up to 4, combo above), structs (tree nodes, `@ui::flatten`), groups as sections, `@ui::action` buttons, `im::inspectable` | done | `ui/immediate.ed*` |
| `segmented<E>` (painter `segments`) | done | `ui/immediate.ed*`, `ui.ed` |
| Lists (`darray`: add, remove; paths `name[]`), `std::option`, `std::math` vectors and colours, enums with payloads in `inspect` | done (reordering by drag: planned; maps: planned) | `ui/immediate.ed*` |
| `ui::snapshot<T>`, `ui::undo_history<T>`, `scope.undo_shortcuts` | done (snapshots; field diffs planned) | `ui.ed*`, `ui/immediate.ed*` |
| `w.table(label, span, height)`: columns from fields, sorting, selection, keyboard, virtualized rows (100 000 rows tested); `table_selection` | done (filter, column widths from `@ui::column`, editing cells: planned) | `ui/immediate.ed*` |
| `tabs<E>`, `radio<E>` (painters `tab`, `tab_bar`, `radio`, `table_header`, `table_row`) | done | `ui/immediate.ed*`, `ui.ed*` |
| Forms for types: `with_form` scopes, `ctx.set_form`, paths relative to the type, precedence context < scope < call; `range_all`, `step_all` | done | `ui.ed*` (`apply_overrides`), `ui/immediate.ed*` |
| `ui::widget_set`, `widget_sets::tool/touch/gamepad`, `with_widgets`, `stepper` | done | `ui.ed*`, `ui/immediate.ed*` |
| `@ui::field_widget{ .for_type = T }` | done | `ui.edh`, `ui/immediate.ed` |
| Still open in M3: `.nested(path, form)`, `@ui::column` widths, a table filter and editable cells, maps in `inspect`, dragging list elements to reorder | planned | |

## Compiler changes made for the UI
| Change | Where | Tests |
|---|---|---|
| Members of `implement` blocks (and annotation-generated members) are resolved with their template scope: `compile_time` and `implicit compile_time` parameters work there (was E4160 "cannot deduce template parameter '?'") | `resolve.cpp` `resolve_decl` | `action/run/implement_implicit_parameters` |
| A qualified `compile_time` constant is a template value argument, also through a namespace alias (`functions<ui::gl_33>`, was E3022 "is a variable, not a type") | `resolve.cpp` `names_value`, `names_qualified_value` | `action/run/qualified_value_arguments` |
| A `compile_time type` alias reached through a namespace alias builds values and reaches static members (`ui::vec2{ ... }` with `namespace ui = std::ui;`, was E3033 "has no field") | `resolve.cpp` `expand_type_alias_head` | `action/run/type_alias_through_namespace_alias` |
| Returning a `match` or `if` whose branches name places from a function returning a reference gives the place, not a copy that is destroyed (was a double free) | `llvm_backend.cpp` `object_address`, `places_only`, `match_value` / `if_value` with `as_address` | `action/run/match_reference_return` |
| Binary files of the library built into edaxc: `std::embed` / `std::embed_text` in a std module read them (fonts, shaders) | `compiler/CMakeLists.txt` (`EDAX_STD_RESOURCES`), `std_library.cpp.in` (`std_resource`), `check.cpp` `infer_embed` | the std tests above |
| An annotation's struct may have fields whose value is a type: `type for_type = void;` is a field there (elsewhere it declares an associated type), and isn't "compile-time only" in a struct whose values exist only at compile time anyway | `parser.cpp` (annotation bodies), `check.cpp` (`annotation_type`) | `action/run/std_ui_scoped_forms` (`@ui::field_widget`) |
| `f.call(...)` on a function from `std::meta::functions_with` works when the function is in a module the calling code doesn't import (the library calling the program's `@ui::field_widget`): the overloads of a reflected function fall back to the function itself | `check.cpp` (path callee overloads) | `action/run/std_ui_scoped_forms` |
| `a.get(mut value)` on a reflected alternative gives the payload as a mutable reference (`std::option<mut P&>`), so editors change payloads in place | `check.cpp` (meta `get`) | `action/run/std_ui_tables` (inspect on an enum with payloads) |
| Entry checks of a call (`compile_time check` in the callee) are no longer run while a template's generic body is checked, only in its instances: `order.sort_by(lambda)` in a template failed `std::returns<F, bool>()` with the lambda's result not known yet | `check.cpp` (`run_entry_checks`), `compile_time.cpp` (`result_of` of an unknown result is unknown) | `action/run/std_ui_tables` (the table sorts) |
| A consuming method returning `this` (`form<T> range(...) move { ...; return this; }`, builders) moved the object out but still destroyed it at the end (double free); `return this` now clears `this`'s drop flag | `llvm_backend.cpp` (`this_owned_`, `owned_path`) | `action/run/consuming_builder` |
| Checking a template whose `.edh` and `.ed` both name its parameters: callable types built from them (`std::function<bool(T&)>` in a field and in a method) didn't match (E4010 "expected 'std::function<bool(T&)>', found 'std::function<bool(T&)>'"); they are now equal up to the parameters' names | `check.cpp` (`same_up_to_params` in `assign_cost`) | `action/run/std_ui_forms` (forms keep such functions) |
| `@std::diagnostic_frame{ .hide = true }` also moves errors raised in a hidden `compile_time` helper the template calls (its annotation on the `.edh` declaration, its body in the `.ed`) to the user's call | `check.cpp` (`in_hidden_helper`) | `action/check/ui_form_paths` |
| Function types take `mut T&` parameters: `std::function<void(mut painter&, part&)>`, `std::fn_ptr<void(mut u32&)>` (was a parse error), so stored painters can draw into what they are given | `parser.cpp` (function type parameters) | `action/run/callable_mut_parameters` |
| `this.items[i].n += 1` in a `mut` method (a field of an element of the object's own container) was rejected with E4021 "in a non-mut method": the element's reference type came from the non-mut `operator[]`; elements are now as mutable as their container, as for locals | `check.cpp` `place_of` (member of an index) | `action/run/this_element_assignment` |
| On-demand loading of `std::ui::immediate` (when named) and `std::ui::platform` (`std::ui` with windowing or OpenGL) | `modules.cpp`, `compiler/CMakeLists.txt` | `action/run/std_ui_immediate`, `example/ui_widgets` |
| On-demand loading of `std::math`, `std::image`, `std::text::font`, `std::ui` (when a program names `std::ui` or the module) and `std::ui::opengl` (`std::ui` and "opengl"; brings windowing and the GL module) | `modules.cpp` `on_demand_triggers`, `ui_opengl_named`, `wanted` | the std tests and examples |

## Open items found so far
| Item | Kind | For |
|---|---|---|
| `std::format_into(mut std::string&, std::format_args)` | std + backend | M2 (text without an allocation per call) |
| "in the instance ..." notes print a whole `std::call_site` value | compiler message | - |
| E5023's note about atomics and mutexes also shows in single-threaded code | compiler message | - |
| A method body in the `.ed` must spell its parameter types as the header does: `wnd::opengl::frame&` (an alias) doesn't match `std::windowing::opengl::frame&` (E3014) | compiler | - |
| A painter call can't read `p.tokens...` in its own arguments: `paint::knob(mut p, ..., p.tokens.colors.warning)` is E5020 (the `mut p` argument excludes other uses); copy the colour first. A two-phase borrow (arguments read before the mutable borrow starts) would allow it | language (borrowing) | own painters |
| Only the modules of an interface or of a type may implement one for the other, so std::ui::immediate can't make its context an input layer without depending on windowing: `std::ui::routing` adds a small view class (`im::router`) instead | language rule, by design | - |
| Names in nested std namespaces clash with `std::` names (`std::ui::immediate::layout` vs `std::layout`, E3060): the immediate layer calls its layout records `flow` | language rule, by design | - |
| A local reference bound to an `if` of places (`stored_form& f = if (c) { a[i] } else { b[i] };`) is E6001 in code generation; returning such an `if` works | backend | - |
| `match (f.annotation<A>()) { ... }` directly on a reflected annotation is E6001 in code generation ("does not support this place"): bind it to a local first | backend | - |
| Goldens of `action/check` tests that show a note in a std file record its line numbers, so they change with the library | tests | - |
| No adjacent string literal concatenation and no multi-line raw strings: long texts (shaders) go to files and `std::embed_text` | language | - |
