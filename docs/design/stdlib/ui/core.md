# User interface core: `std::ui`

`std::ui` is the user-interface library of the standard library ([draft 35](../../drafts/35-ui.md), [roadmap](../../drafts/35-ui-roadmap.md)). This page describes its core, which the immediate-mode and retained front-ends build on: **themes**, **skins** (how widgets are drawn), **text** and the **draw list**, and the OpenGL renderer `std::ui::opengl`. The immediate-mode widgets are [immediate.md](immediate.md). The core knows nothing about immediate or retained mode and calls no graphics API: it turns shapes, text and images into a draw list, plain data that a renderer draws.

The module is loaded when a program names `std::ui`, together with `std::math`, `std::image` and `std::text::font`. `std::ui::opengl` is loaded when the program names `std::ui` and OpenGL, and brings `std::windowing` and `std::graphics::opengl`.

## Rules

### Names
`std::ui` uses `std::math`'s types under short names: `ui::vec2`, `ui::rect`, `ui::color` (`compile_time type` aliases). Coordinates are window units (pixels before the content scale), y down. Colours are sRGB with straight alpha; blending happens in sRGB, as in other UI toolkits.

### Themes
A `ui::theme` is a plain struct of **design tokens**; widgets ask it for a role (`colors.surface`, `space.m`, `radius.large`), never for a number (draft 35 §3.1).

| Field | Type | Holds |
|---|---|---|
| `mode` | `brightness` | `dark` or `light` |
| `colors` | `palette` | `background`, `surface`, `surface_raised`, `surface_sunken`, `primary`, `primary_hover`, `primary_pressed`, `on_primary`, `text`, `text_muted`, `text_faint`, `border`, `focus`, `success`, `warning`, `danger`, `shadow` |
| `typography` | `type_scale` | A `type_style` (face, size, line height) per `text_role`: `caption` 12, `body` 14, `label` 13 semibold, `title` 17, `heading` 22, `display` 32, `code` 13 mono |
| `space` | `spacing_scale` | `xs` 4, `s` 8, `m` 12, `l` 16, `xl` 24, `xxl` 32 |
| `radius` | `radius_scale` | `small` 4, `medium` 8, `large` 12, `full`; presets `rounded()`, `subtle()` (2/3/4/5), `sharp()` (all 0) |
| `shadow` | `elevation_scale` | `level1` (cards), `level2` (popups), `level3` (dialogs): offset, blur, opacity |
| `motion` | `motion_scale` | `fast`, `normal`, `slow` in seconds |
| `controls` | `controls_style` | The component tokens: sizes and shapes of controls (below) |
| `density` | `f32` | 1 comfortable, 0.8 compact, 1.2 touch; scales text sizes |

- **`theme::from_accent(color, brightness)`** derives every colour from one accent in OKLCH: surfaces tinted with its hue, `primary` at a lightness that reads on them, `text_muted` raised until it has WCAG AA contrast (4.5:1) on `surface`, `on_primary` white or near-black by contrast. `theme::dark()` and `light()` use the library's blue.
- **`theme::mix(a, b, t)`** interpolates two themes (colours in OKLab, sizes and radii linearly; the shapes in `controls` switch at `t = 0.5`): an animated switch between light and dark, or from rounded to sharp corners.
- `t.style(role, color)` gives the `text_style` of a role; `t.elevation(level)` the shadow of level 1-3.

### Changing the look
A look has two replaceable layers (draft 35 §3): the theme's **tokens**, and the **skin**: one painter function per kind of widget.

**Component tokens** (`theme.controls`):

| Field | Holds |
|---|---|
| `button` | `height` 34, `padding` 16, `raised` (shadow under primary/danger/success), `gradient` |
| `slider` | `track_height` 6, `knob_size` 18, `knob`: `knob_shape::circle`, `square`, `diamond`, `star`, `bar`; `fill_track` |
| `toggle` | `width` 40, `height` 22 |
| `checkbox` | `size` 18, `mark`: `tick_shape::tick`, `cross`, `square`, `dot` |
| `window` | `title_height` 36, `padding` 16, `elevation` 2, `separate_title` |
| `row_height`, `gap` | 30, 8 |
| `boolean` | `bool_widget::toggle` or `checkbox`: which widget edits a bool when the code doesn't say (`scope.boolean`, `inspect` in M3) |

Corners come from `radius`: `ui::radius_scale::sharp()` gives square buttons, cards, windows, checkboxes, and square toggles and tracks (their radius is `radius.full`).

**Painters and skins.** A `ui::painter` is a view of a draw list, the fonts and the theme (`p.list`, `p.faces`, `p.tokens`; `p.text`, `p.centered_text`, `p.measure`). A `ui::skin` holds one `std::function<void(mut ui::painter&, X_part&)>` per kind of widget: `button`, `checkbox`, `toggle`, `slider_track`, `slider_knob`, `progress`, `section_header`, `tree_node`, `window`, `card`, `field` (the frame of text, number and combo fields), `popup` (a list's box), `list_item`, `tooltip`, `scrollbar`. The window painter also draws the resize grip (`window_part.grip`). The part (`button_part`, `check_part`, `slider_part`, `field_part`, `item_part`, ...) is what the widget decided: its rectangle, label, value, and an `interaction` (`hovered`, `pressed`, `focused`, `disabled`, and `hover`/`press` animated from 0 to 1).
- `skin::standard()` uses the functions of `ui::paint`, which read the tokens. A program replaces any slot; the widget keeps its behaviour (hit testing, values, ids), only its drawing changes, everywhere it is used.
- A replacement can call the library's painters and add to them (`ui::paint::knob(mut p, ui::knob_shape::star, ...)`).
- Painters only draw; they get the part by immutable reference and the painter by `mut` reference (`mut T&` parameters in callable types, [lambdas](../../language/functions/lambdas.md)).

```edax
mut ui::skin& s = ctx.skin();
s.slider_knob = [](mut ui::painter& p, ui::slider_part& k) {
    ui::color gold = p.tokens.colors.warning;
    p.list.circle(k.knob_center(), 10.0 + 4.0 * k.state.hover, gold.with_alpha(0.18));
    ui::paint::knob(mut p, ui::knob_shape::star, k.knob_center(), 20.0, gold, gold.lighter(-0.2));
};
```

### Sizes
`ui::length` says how wide a widget is along a layout: `ui::px(n)`, `ui::fit`, `ui::fill(weight)`, `ui::percent(p)` (draft 35 §4; used by `scope.width`, see [immediate.md](immediate.md)).

### Input and text editing
- `ui::input` is one frame of input as plain data (see [immediate.md](immediate.md)); `in.pressed(key)` and `in.pressed_with_control(key)` look for a key. `ui::key` has only the keys the UI uses (Tab, Enter, Escape, Space, Backspace, Delete, arrows, Home, End, Page Up/Down, and A C V X Y Z for shortcuts); `std::ui::platform`'s `key_of` maps a window's key to it.
- `ui::text_edit` is the editing state of a one-line field: `cursor` and `anchor` (byte offsets on character boundaries; the selection lies between them), undo and redo steps (the text before each change, at most 100), and the field's scroll. `apply(mut text, input, mut clipboard)` applies a frame's keys and typed text and returns whether the text changed; Enter, Escape and Tab are left to the widget. It is independent of the immediate layer, so a retained text box can use it too.

### Text: `ui::fonts`
- `ui::fonts::make()` holds the three faces of the library (`font_face::regular`, `semibold`, `mono`); `fonts::from_faces(...)` takes others.
- Glyphs are rendered into an **atlas** of distance fields the first time they are drawn, at `glyph_atlas_size` (40) pixels with `glyph_atlas_spread` (5), so one atlas serves every size. The atlas is 1024 pixels wide and grows downwards; glyphs keep their pixel positions, and `version()` changes when glyphs were added so renderers know to upload it again.
- `measure(text, style)` (width and line height), `line_height(style)`, `baseline(style)` (from the top of a line), `advance(face, previous, c, size)` (with kerning), `metrics(face)`.
- `wrap(text, style, width)` breaks at spaces so no line is wider than `width` (a longer word gets a line of its own) and at `'\n'`; it returns `text_line`s (byte ranges and widths).
- Text is UTF-8; `ui::char_at(text, i)` decodes one character (invalid bytes become U+FFFD).

### The draw list
`ui::draw_list` is everything a frame draws, in order: `vertices`, `indices` and `commands`. A **command** is a run of triangles with one clip rectangle and one texture; shapes don't sample a texture, so they join any command, and a frame of shapes and text is usually one command per clip rectangle.

| Method | Draws |
|---|---|
| `begin(size)`, `clear()` | A new frame (capacity is kept: a frame like the last one doesn't allocate) |
| `push_clip(r)`, `pop_clip()`, `current_clip()` | Clipping to the intersection of the pushed rectangles |
| `fill(r, color, radius)` | A box with rounded corners (radius 0: square; `radius.full`: a pill) |
| `fill_gradient(r, top, bottom, radius)` | A box with a vertical gradient |
| `stroke(r, color, radius, width)` | The outline of a box, inside it |
| `shadow(r, radius, shadow_style, color)` | A soft drop shadow of a box |
| `circle(center, radius, color)`, `circle_stroke(...)` | |
| `line(from, to, width, color)` | A line with round caps |
| `image(texture, r, uv, tint, radius)` | A texture, optionally with rounded corners |
| `fill_polygon(points, color)`, `stroke_polygon(points, closed, width, color)` | Convex and star-shaped polygons (fanned from the centroid, edges anti-aliased by a fringe of transparent vertices); outlines with round joints |
| `star(center, outer, inner, points, rotation, color)`, `regular_polygon(center, radius, sides, rotation, color)`, `tick(r, width, color)` | Stars, triangles, diamonds, hexagons, check marks |
| `append(other)` | Another list's commands after this list's (windows stacked by the immediate layer) |
| `text(mut fonts, at, text, style)` | One line; returns its width |
| `paragraph(mut fonts, at, width, text, style)` | Wrapped text; returns the height |

- **One vertex format:** position, texture coordinates, colour and the shape's parameters (position relative to its centre, half size, radius, stroke width, blur, `shape_kind`). Rounded boxes, outlines, circles, lines and shadows are signed-distance shapes evaluated per pixel by the shader: four vertices each, anti-aliased, sharp at any scale. Polygons are `shape_kind::solid`: the vertex colour only.
- `ui::texture` is a renderer's handle and size; `texture::font_atlas()` stands for the atlas, id 0 for white.
- The list is plain data: tests check it without a GPU (`tests/action/run/std_ui_draw_list.ed`).

### The OpenGL renderer: `std::ui::opengl::renderer<A>`
- `renderer<A>::make(frame)` loads the OpenGL functions of API `A` (OpenGL 3.3 core or newer) from a `std::windowing::opengl::frame`, compiles the shader and makes the buffers. The shader sources are `library/std/ui/shader.vert` and `shader.frag`, built into edaxc.
- `draw(frame, list, fonts)` draws over what the frame shows: uploads the atlas when `fonts.version()` changed, then one `glDrawElements` per command with its scissor rectangle. Output is premultiplied alpha. The list is in window units; the renderer scales to the framebuffer, so HiDPI windows are drawn at full resolution.
- `upload(rgba8)` makes a texture (linear filtering, clamped), `release(texture)` frees it, `read_pixels(frame)` reads the frame back as an `rgba8` (screenshots and tests), `functions()` gives the program the loaded functions.

## Example
```edax
namespace ui = std::ui;

ui::theme t = ui::theme::from_accent(ui::color::hex(0x4F7CFF), ui::brightness::dark);
mut ui::fonts fonts = ui::fonts::make();
mut ui::draw_list list = default;

list.begin(ui::vec2{ .x = 1280.0, .y = 720.0 });
ui::rect card = ui::rect{ .x = 40.0, .y = 40.0, .width = 360.0, .height = 200.0 };
list.shadow(card, t.radius.large, t.elevation(2), t.colors.shadow);
list.fill(card, t.colors.surface, t.radius.large);
_ = list.text(mut fonts, ui::vec2{ .x = 64.0, .y = 64.0 }, "Player", t.style(ui::text_role::title, t.colors.text));

mut wnd::opengl::frame frame = win.begin_frame();
renderer.draw(frame, list, fonts);
frame.present();
```
`example/ui_gallery` draws a whole card in both themes of one accent.

## Rationale
- **A core without a mode:** the immediate and the retained front-end (draft 35 §1) both end in a draw list; everything that makes a UI look right is written once.
- **Signed-distance shapes:** the modern look (rounded corners, borders, soft shadows) costs four vertices and no tessellation, and stays sharp when scaled.
- **Tokens, not numbers:** a theme change restyles every widget consistently, and `from_accent` makes a readable theme from the one colour a project usually has.
- **Plain data between core and renderer:** another renderer (Vulkan, a game engine's own) needs only the draw list's format.
- **Tokens and painters, not style stacks:** ImGui's look is a few dozen colours and sizes; anything else (a star knob, a checkbox instead of a checkmark-in-a-frame, hard shadows) means writing a new widget. Here the shapes of controls are tokens, and every widget's drawing is a replaceable function that gets a description of the widget (its part), so a different look never means a different widget.
- **Parts instead of widget internals:** a painter sees what it needs to draw (bounds, value, animated hover/press), not the widget's state; widgets can change how they work without breaking painters.
