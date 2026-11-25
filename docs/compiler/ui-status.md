# `std::ui`: implementation status

The design is [draft 35](../design/drafts/35-ui.md); the plan is its [roadmap](../design/drafts/35-ui-roadmap.md). This file tracks what exists, in the format of `spec-34-status.md`. Legend: **done** (implemented and tested), **partial**, **planned**.

## Milestones
| Milestone | Status | Where | Tests |
|---|---|---|---|
| M0 API spike | **done** | `example/ui_spike` (throwaway; findings in its README) | `example/ui_spike/test.ed` (5 tests, no window), `--screenshot` run under Xvfb |
| M1 Foundations | **partial**: everything but the windowing additions and `std::format_into` | `library/std/{math,image,text/font,ui,ui/opengl}.ed*`, fonts and shaders built into edaxc; `example/ui_gallery` | `action/run/std_math`, `std_image`, `std_text_font`, `std_ui_draw_list`; the gallery's `--screenshot` under Xvfb |
| M2 Core and immediate mode | **done** (`split` came with M5): the style system (tokens, skins, painters), context/frame/scope, windows (drag, resize, scroll), sections, trees, cards, rows, grids, scroll areas, sizes, styled scopes, keyboard focus and navigation, text fields, numbers, combos, tooltips, routing, the own-widget API, the debug overlay of `example/windowing` | `library/std/ui.ed*` (style), `ui/immediate.ed*`, `ui/platform.ed*`; `example/ui_widgets` | `action/run/std_ui_skin`, `std_ui_immediate`, `std_ui_keyboard`, `std_ui_layout`; `ui_widgets --screenshot [--custom]` under Xvfb |
| M3 Reflection and forms | **done** (small items left below): annotations with their checks, `ui::form<T>` (overrides, compile-time paths incl. `list[]`, rules, `visible_if`, `range_all`/`step_all`), forms for types (`with_form`, `ctx.set_form`) with the precedence of §7.8.4, widget sets (`tool`, `touch`, `gamepad`), `@ui::field_widget`, `inspect` for every type of §7.2 but maps, `field`, `ui::edit`, `ui::undo_history`, tables (sorting, selection, virtualized), tabs, radio, segments, steppers | `library/std/ui.ed*` (annotations, forms), `ui/immediate.ed*`; `example/editor` | `action/run/std_ui_forms`, `std_ui_tables`, `std_ui_scoped_forms`, `action/check/ui_form_paths`, `ui_annotations`; `editor --screenshot` (the editor is docked since M5) |
| M4 Plots and debug tools | **done** (items left below): `std::ui::plot` (lines, areas, scatter, bars, h/v lines, reflection, histories, ticks, logarithmic y, legend, hover, pan/zoom/fit, linked x, reduction per pixel column), sparklines, stat cards; `std::ui::debug` (`perf_overlay`/`perf_panel`, `log_sink` + `log_view`/`log_panel`) | `library/std/ui/{plot,debug}.ed*`; `example/ui_plots` | `action/run/std_ui_plot`, `std_ui_debug`, `action/check/ui_plot_annotations`; `bench/plot_decimate`; `ui_plots --screenshot` under Xvfb |
| M5 Docking | **done** (items left below): `std::ui::dock` layouts as data (splits, tab stacks, central area, floating stacks, closed windows' places; text; built and changed in code), workspaces (named layouts, save/load), dockspaces (tab bars, close buttons, dividers with double-click reset, drag and drop onto 5 stack and 4 edge targets with previews, floating stacks), reopening in the last place, splits inside windows | `library/std/ui/dock.ed*`, `ui/immediate.ed*` (`panel`, `region`), `ui.ed*` (dock painters); `example/editor` | `action/run/std_ui_dock` (180 scripted drops: every window onto every target); `editor --screenshot [--drag]` under Xvfb |
| M6 Node editor | **done** (items left below): `std::ui::nodes::editor` (§13.1: canvas, nodes with pins and `im::scope` bodies at the zoom, links under the nodes, frames, selection of nodes and links by clicks and the box with a preview, dragging, link drags with drops on the canvas, keys, minimap, styles with 5 presets and curve, subway or straight links, collapsing, reroute points, arranging, inline pin widgets, `nodes::edits`, `topological_order`); `nodes::graph<K>` (§13.2: kinds from an enum, pins from `@nodes::in/out`, parameters edited like `inspect`, type checks with `@nodes::converts`, the add-node menu with search and link filtering, copy/cut/paste as text, duplicate, undo/redo, saving and loading text); the car paint's material in `example/editor` on `graph<K>`, compiled to GLSL on change | `library/std/ui/nodes.ed*`, `ui.ed*` (`draw_list::insert`, `polyline`, input buttons and modifiers); `example/editor/graph.ed*` | `action/run/std_ui_nodes`, `std_ui_graph`; `editor --screenshot --graph [--nodes=…] [--box] [--add]` |
| M7 Widget catalogue and polish | planned | | |
| M8 Compile time and performance | planned | | |
| M9 Reach | planned | | |
| M10 Retained mode | planned | | |

## M6 in detail
| Item | Status | Where |
|---|---|---|
| `w.node_editor(name, mut view_state, height)` → `nodes::editor`: `frame`, `node` → `node_view` (`input`, `output`, `body`), `link`, `to_screen`/`to_canvas`, `finish` → `nodes::edits` (`created`, `deleted_links`, `deleted_nodes`, `moved`, `dropped`, `menu_at`, `duplicate`) | done | `ui/nodes.ed*` |
| Pin shapes (circle, square, triangle, diamond), filled when linked; links coloured by their output, the selected one in the accent, drawn under the nodes (`draw_list::insert`) | done | `ui/nodes.ed`, `ui.ed` |
| Zoom at the cursor through the theme's density (bodies' widgets scale); grid; minimap; fit (F) | done | `ui/nodes.ed` |
| `ui::input`: right and middle buttons, held Shift/Ctrl/Alt; keys `d`, `f` | done | `ui.edh`, `ui/platform.ed` |
| `nodes::topological_order` (Kahn; the error names a node on a cycle) | done | `ui/nodes.ed` |
| Styles: `nodes::style` in `view_state.look` (sizes, `link_look`, grid kinds, `canvas_theme`, painters for canvas, node, pin, pin label, link, frame, selection box); presets `studio`, `metro`, `blueprint`, `neon`, `paper`; their painters public in `nodes::painters`; `node_look.variant` for a program's own painters | done | `ui/nodes.ed*` |
| Link shapes: `curve`, `metro` (8 directions: across and 45°, steep runs, backwards around; rounded corners with a radius, or sharp), `straight`; links as polylines in `view_state.path` (hit tests for every shape) | done | `ui/nodes.ed` |
| Selecting links: several (`selected_links`), Shift/Ctrl clicks, the box selects the links it touches; Delete and Ctrl+A include links | done | `ui/nodes.ed` |
| The box highlights what it would select while it is dragged (`node_part.preview`, `link_part.preview`) | done | `ui/nodes.ed` |
| `draw_list::polyline` (one strip, mitred joints, anti-aliased) | done | `ui.ed*` |
| Example: a bar to switch the style, the link shape, round corners and the radius; `--nodes=…` and `--box` screenshots | done | `example/editor/graph.ed*`, `main.ed` |
| Presses on nodes and frames are decided at `finish()`: a widget in a node (a slider) keeps its drag instead of moving the node | done | `ui/nodes.ed` |
| Collapsing (header chevron, `view.collapsed`, pins on the header's edges); `n.input_widget` (a widget in an input's row while unlinked); `scope.hidden_region` | done | `ui/nodes.ed*`, `ui/immediate.ed*` |
| Reroute points: `ed.link_via(…, mut via)`, Alt-click on a link adds one, drag moves, Alt-click removes; `edits.rerouted`; stubs only at pins; `reroute` painter | done | `ui/nodes.ed*` |
| Arranging the selection: `view.arrange_next` (align edges, centres, middles; the same space across or down) | done | `ui/nodes.ed*` |
| Example: pins labelled with Edax types (`f32`, `vec2`, `vec3`), Mix `t` / Multiply `factor` / Output `roughness` as inline sliders, an Arrange menu, Time collapsed, one reroute point | done | `example/editor/graph.ed*` |
| Example: material graph (types checked, links into an input replaced, add-node menu for right clicks and dropped links, GLSL made on change, the Shader tab) | done | `example/editor/graph.ed*`, `main.ed` |
| `nodes::graph<K>`: `@nodes::kind` (title, category, colour, icon, width, removable), `@nodes::in`, `@nodes::out`, `@nodes::converts` (found with `functions_with`); pins typed by field type (colours by type), parameters with `field` editors, unlinked number/bool inputs inline, other unlinked inputs in the body | done | `ui/nodes.ed*` |
| `graph<K>` data: `make`, `add`, `remove`, `connect` (by field names, type-checked), `add_frame`, `index_of`, `source_of`, `topological_order`; `to_text`/`from_text`/`save`/`load` (readable, by name; missing fields keep defaults, unknown ones are skipped) | done | `ui/nodes.ed*` |
| `graph_editor`: the add-node menu (right click, Space, dropped links; typing filters, Enter adds, a dropped link lists only fitting kinds and is connected), Ctrl+C/X/V, Ctrl+D, Ctrl+Z/Y (one step per finished change, text snapshots), undeletable kinds, `graph_edit` (`changed`, `committed`, `problem`); `view_state.area` | done | `ui/nodes.ed*` |
| Example: the material on `graph<material_node>` (8 kinds, `splat` conversion, the bar with undo, redo, save and load to `car_paint.graph`), GLSL from `topological_order` and `source_of` | done | `example/editor/graph.ed*` |
| Subgraphs, find node, `nodes::custom_body`, `evaluate()`, forms per graph; two graph editors in one scope (shared ids); open question 11; using the GLSL to draw the car | planned | |

## Editor look (after M5, ahead of M7)
| Item | Status | Where |
|---|---|---|
| `theme::studio(accent, mode)`: neutral greys, dense tokens (13 px text, 24 px rows, small radii, flat buttons); `skin::studio()` with the painters of `paint::studio` | done | `ui.ed*` |
| `ui::icon` (52 line icons and `none`), `draw_list::draw_icon`, `draw_list::arc` | done | `ui.ed*` |
| `icon_button`, `icon_toggle`, `icon_label_button`, `icon`, `badge`, `spacer`, `bar_separator`; `menu`, `menu_item`, `menu_check`; `available_height`; `frame.panel(id, rect, order, inset)` | done | `ui/immediate.ed*` |
| Tables: columns from content (titles shortened first, "…"), numbers right-aligned, bools as check marks, height 0 fills the panel | done | `ui/immediate.ed` |
| Enum combos show labels; enums whose names don't fit become combos; empty labels drop the label column | done | `ui/immediate.ed` |
| Dock: `controls.window.tab_height` and `dock_gap` tokens | done | `ui.edh`, `ui/dock.ed` |
| Log panel: one toolbar (search, level, warning and error badges, Clear), height 0 fills the panel | done | `ui/debug.ed` |
| `example/editor` redesigned: top bar (menus, tools, play), status bar, hierarchy (own widget), viewport with gizmos and overlay buttons, inspector with a transform | done | `example/editor` (`--screenshot [--drag] [--menu] [--green]`); `action/run/std_ui_studio` |
| Studio v2: near-black palette, radii 4/8/10, docked stacks as rounded cards 6 px apart (`dock_stack` painter, unfilled content panels), tab icons (`ws.set_icon`), overline section headers, accent glow on primary buttons, chips and the focused tab, accent segment thumbs, gradient progress | done | `ui.ed*`, `ui/dock.ed*`, `ui/immediate.ed*` |
| Text: `tracking` (letter spacing), `text_role::overline`, `ui::upper`; `scope.placeholder`; `scope.chip` | done | `ui.ed*`, `ui/immediate.ed*` |
| Draw list: `fill_gradient_x`, `glow`, `fill_between_gradient` (plots' areas fade out), `dashed_line`, `bezier` | done | `ui.ed*`, `ui/plot.ed` |
| `w.gauge` (std::ui::plot) | done | `ui/plot.ed*` |
| `example/editor` v2: a 3D scene view drawn with the draw list (perspective grid, shaded low-poly car sorted back to front, shadow, bounds, gizmos, orientation gizmo; orbit and zoom), a node graph document (cards, ports, curved links, dragging), document headers, inspector header card and summary, profiler gauges | done | `example/editor/{main,viewport,graph}.ed*` (`--screenshot [--drag] [--menu] [--graph] [--profiler] [--accent=…]`) |
| Submenus, keyboard in menus, context menus, resizable table columns | planned | |

## M5 in detail
| Item | Status | Where |
|---|---|---|
| `dock::layout`: nodes in an array (`split_x`, `split_y`, `tabs`, `central`), floating stacks, closed windows; `tabs`, `central`, `central_with`, `split_x`, `split_y`; `find`, `contains`, `windows`, `dock_next_to`, `dock_at`, `dock_at_edge`, `float_window`, `activate`, `remove` (empty stacks collapse), `add`, `add_document`; `clone`, `same` | done | `ui/dock.ed*` |
| `close` remembers the neighbours (the other side of the split, or the stack and the tab position), side and share; `reopen` splits the smallest part still holding them (exact round trip) | done | `ui/dock.ed*` |
| `to_text` / `from_text` (readable, indented, quoted names) | done | `ui/dock.ed*` |
| `dock::workspace`: `make`, `load`, `save`, `save_if_changed`, `current`/`edit`/`reset`, `save_as`, `switch_to`, `names`, `close`, `reopen`, `closed_windows`, `tab_of` | done | `ui/dock.ed*` |
| `f.dockspace`, `f.dockspace_in`, `space.window`, `space.document`: panels per window (ids from names), tab bars with close buttons, focus underline, dividers (drag, double-click), floating stacks (move, front), drag and drop (6 px threshold, ghost tab, 5 + 1 + 4 targets with previews, no target: floating) | done | `ui/dock.ed*` |
| `w.split_x/split_y(name, ratio, height)` → `first()`, `second()`, `ratio()` | done | `ui/dock.ed*` |
| `w.window_menu(mut ws, start)` (reopen closed windows, reset the layout) | done | `ui/dock.ed*` |
| Painters `dock_tab`, `dock_bar`, `divider`, `drop_target` in `ui::skin` | done | `ui.ed*` |
| `im::frame::panel(id, rect, order)`, `im::scope::region(id, rect, space)`; windows' orders start at 16 (docked layers below) | done | `ui/immediate.ed*` |
| Golden files record `<std>` line numbers as `<n>` (the library can change without touching check goldens) | done | `tests/run_tests.py`, `docs/testing.md` |
| A menu bar; Shift to float; tab overflow, pinning, middle-click, reordering in a stack; resizing floating stacks; compile-time layouts; several OS windows (M9) | planned | |

## M4 in detail
| Item | Status | Where |
|---|---|---|
| `plot::history<T, N>` (ring buffer, contiguous `as_span()` from storing each value twice), `min`/`max`/`mean`/`last` | done | `ui/plot.ed*` |
| `w.plot(title, height)` → `plot::scope`: collects series, fits and draws at its `}`; at most 16 entries | done | `ui/plot.ed*` |
| `line` (index, x/y spans, `line_of` with a lambda), `area`, `scatter`, `bars`, `hline`, `vline`, `color`, `legend`, `theme`, `range`, `hovered` | done | `ui/plot.ed*` |
| `series(name, rows)` by reflection: `@plot::x`, `@plot::series{ .unit }` (checked: number fields only) | done | `ui/plot.ed*` |
| Axes: labels, fixed `min`/`max`, ticks at 1/2/5 × 10^k sized to their labels, `format_number`, logarithmic y | done | `ui/plot.ed*` |
| Pan (drag), zoom (wheel around the cursor), fit (double-click), hover box with every series' value, legend toggles, `link_x(group)` | done | `ui/plot.ed*` |
| Axis limits `limit_min`/`limit_max` (pan, zoom and fit stop there; positive-only x, negative-only y), `plot::clamp_range` | done | `ui/plot.ed*` |
| Reset button while the view is moved, `p.moved()`, `p.reset_view()` | done | `ui/plot.ed*` |
| Steady hover box: width grows in 8 px steps and doesn't shrink while hovering, keeps its side of the cursor | done | `ui/plot.ed*` |
| Reduction per pixel column (`decimate`, `decimate_indexed`: first/lowest/highest/last, only what is in view plus the neighbours of its ends); index-x fast path: 1M `f32` in 1.4 ms (`bench/plot_decimate`, 0.32x of the same algorithm in C++) | done | `ui/plot.ed*` |
| No allocation per frame once buffers are large: points, names and line buffers in `widget_storage` | done | `ui/plot.ed`, `ui/immediate.edh` |
| `w.sparkline(values)`, `w.stat(label, value, trend, stat_options)` | done | `ui/plot.ed*` |
| `dbg::frame_stats`, `f.perf_overlay`, `w.perf_panel` | done | `ui/debug.ed*` |
| `dbg::log_sink` (a `std::log::sink`) + `log_feed` (thread-safe, ring of lines with levels, counts, clear), `f.log_view`, `w.log_panel` (level filter, search, colours, stays at the newest line) | done | `ui/debug.ed*` |
| `ui::draw_list::fill_between` (a band between two polylines, no seams) | done | `ui.ed*` |
| Own-widget API: `scope.storage(id)` (`widget_storage`), `scroll_unused`/`take_scroll`; `context.clock` | done | `ui/immediate.ed*` |
| Time and category axes, several y axes, subplots; `stairs`, grouped/stacked bars, histograms, heatmaps, error bars, candles, pies; `region`, `text_at`, drag handles; box zoom; reduced scatter; `texture_viewer`; memory in `perf_overlay` | planned | |

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
| `split` | done in M5: `w.split_x`/`split_y` (std::ui::dock) | `ui/dock.ed*` |

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
| A value argument is copied before the call starts (two-phase), so it may read a place the call borrows mutably: `ctx.state_of(ctx.open_menu)`, `v.push(v[0])`, `bump(mut c, c.hits)` (was E5020); reference arguments and views still may not overlap | `ownership.cpp` (`check_aliasing`, `copied`) | `action/run/two_phase_arguments`, `feature/12-ownership-references/disjoint_fields` |
| `view` and `check` are contextual keywords (before `class`/`struct`, after `compile_time`/`run_time`), so they name variables and functions: `mut im::scope view = bar.menu("View");` | `token_kinds.def`, `parser.cpp` (`is_word`) | `action/run/contextual_keywords`, `unit/test_lexer` |
| Destructuring assignment to places that exist: `(r.lo, r.hi) = split(x);`, `(a, b) = (b, a);`, `(_, r.hi) = split(x);` (was E4025) | `parser.cpp` (a hidden `auto (...)` declaration, then one assignment per place) | `action/run/destructuring_assignment` |
| A lambda without '->' in a template body (a method of a template struct, a function template) had a `void` result during the generic check, so `retain`/`sort_by` were instantiated with it ("'void' does not convert to bool"); a lambda's result now comes from its first `return` there too | `check.cpp` (`check_return`, `FunctionState::lambda`) | `action/run/template_lambda_result` |
| On-demand loading of `std::math`, `std::image`, `std::text::font`, `std::ui` (when a program names `std::ui` or the module) and `std::ui::opengl` (`std::ui` and "opengl"; brings windowing and the GL module) | `modules.cpp` `on_demand_triggers`, `ui_opengl_named`, `wanted` | the std tests and examples |

## Open items found so far
| Item | Kind | For |
|---|---|---|
| `std::format_into(mut std::string&, std::format_args)` | std + backend | M2 (text without an allocation per call) |
| A method body in the `.ed` must spell its parameter types as the header does: `wnd::opengl::frame&` (an alias) doesn't match `std::windowing::opengl::frame&` (E3014) | compiler | - |
| A painter call can't read `p.tokens...` in its own arguments: `paint::knob(mut p, ..., p.tokens.colors.warning)` is E5020 (the `mut p` argument excludes other uses); copy the colour first | language (borrowing) | fixed: value arguments are two-phase |
| Only the modules of an interface or of a type may implement one for the other, so std::ui::immediate can't make its context an input layer without depending on windowing: `std::ui::routing` adds a small view class (`im::router`) instead | language rule, by design | - |
| Names in nested std namespaces clash with `std::` names (`std::ui::immediate::layout` vs `std::layout`, E3060): the immediate layer calls its layout records `flow` | language rule, by design | - |
| A local reference bound to an `if` of places (`stored_form& f = if (c) { a[i] } else { b[i] };`) is E6001 in code generation; returning such an `if` works | backend | - |
| `match (f.annotation<A>()) { ... }` directly on a reflected annotation is E6001 in code generation ("does not support this place"): bind it to a local first | backend | - |
| Goldens of `action/check` tests that show a note in a std file recorded its line numbers, so they changed with the library | tests | fixed in M5: recorded as `<n>` |
| No adjacent string literal concatenation and no multi-line raw strings: long texts (shaders) go to files and `std::embed_text` | language | - |
| A lambda can't use the field of a `for compile_time` loop (`[f](usize i) { return f.get(rows[i]); }` is E4200 "needs a std::meta::field known at compile time", without the capture E3050); plotting by reflection copies the field's values into a buffer first | language (compile time) | `plot::scope::series` |
| Large fixed arrays on the stack (`std::array<vec2, 256>` filled in loops) make LLVM's SROA take seconds per function (a 4x slower build); reusable `darray` buffers avoid it | backend (compile time) | plots |
| No destructuring assignment to existing places | language | fixed: `(v.a, v.b) = f();` works |
| A tuple can't be formatted (`f"{pair}"` is E6001) | backend | - |
| `match` has no or-patterns (`a | b => ...`); the last arm is `_ => ...` instead | language | - |
| `check` and `view` were keywords everywhere | language | fixed: contextual; `move` stays reserved (icons are `checkmark` and `translate`) |
| A destructuring assignment couldn't skip an element | language | fixed: `(_, r.hi) = split(x);` |
| E5023's note mentioned atomics and mutexes for every borrow | compiler message | fixed: only for threads; otherwise it says to read what is needed first or to end the borrow in a block |
| "in the instance ..." notes printed a whole `std::call_site` value | compiler message | fixed: the implicit call site is left out |
| An integer can't be cast to an enum (`cast::truncate<ui::icon>(i)` is E4091); `std::meta::alternatives<E>()` and `a.make()` enumerate one | language | icon galleries |
