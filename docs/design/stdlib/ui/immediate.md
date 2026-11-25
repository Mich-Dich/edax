# Immediate-mode widgets: `std::ui::immediate`

The immediate-mode front-end of [`std::ui`](core.md) ([draft 35](../../drafts/35-ui.md) §10 and §11). A program describes its widgets every frame; the library keeps what must last (positions, open sections, animations) by widget id. The module is loaded when a program names `std::ui::immediate`; `std::ui::platform` (input from a `std::windowing` window) when it names `std::ui` and `std::windowing`.

## Rules

### Context and frame
- `im::context::create(theme)` makes a context: per-id state, windows, animations, the theme, the skin, the fonts and the finished frame's draw list. One context per OS window.
- `ctx.begin_frame(input, size)` returns an `im::frame`, a view that borrows the context mutably; `f.finish()` consumes it and stacks the windows into `ctx.draw_data()`. Drawing: `renderer.draw(frame, ctx.draw_data(), ctx.fonts())`.
- `ui::input` is plain data: cursor, left button down/pressed/released, scroll, `dt`, the keys that went down or repeated (`ui::key_press`: a `ui::key`, shift, control) and the text typed. `ui::input_of(win.input(), events.as_span(), 0.0)` fills it from a window and the frame's events (keys with repeats, typed characters); tests fill it by hand. `dt` 0 lets the context measure the time between frames.
- **Routing (`std::ui::routing`):** `im::router` is the context as a `std::windowing::input_layer`. `wnd::route(mut events, mut layer, win.input())` with `mut im::router layer = im::router{ .ctx = ctx }` takes key and text events while the UI wants the keyboard and mouse buttons and the wheel while it wants the mouse; the keys and text it takes are added to the next frame's input, and the game sees what is left (draft 35 §10.2).
- `ctx.wants_mouse()`: the cursor is over a window of the UI or a widget holds the mouse. `ctx.wants_keyboard()`: a field is being typed into, or the keyboard moves the focus. The game leaves the mouse or the keys alone then; check `wants_keyboard()` *before* the frame's events, so an Escape that stops typing doesn't also quit.
- `ctx.duplicate_ids()` counts ids used twice in the last frame. `ctx.clipboard_text()` / `set_clipboard_text(s)`: what text fields copy and paste (inside the program until `std::windowing` has a clipboard).

### Changing the look at run time
- `ctx.set_theme(t)` fades to `t` over `t.motion.slow` (colours in OKLab, sizes and radii linearly, the shapes of controls switch half way); `set_theme_now(t)` switches at once. `ctx.theme()` is the theme in use.
- `ctx.skin()` is the context's `ui::skin`: assign a painter to a slot (`ctx.skin().slider_knob = ...`) or a whole skin (`ctx.skin() = ui::skin::standard()`). See [core.md, Changing the look](core.md#changing-the-look).

### Keyboard
- **Tab** and **Shift+Tab** move the focus through the widgets in the order they were made in the last frame; **Enter** or **Space** presses the focused widget (a button clicks, a checkbox flips, a section opens, a number or text field starts typing); **arrows** change focused sliders, numbers and combos (Shift: ten steps) and open or close tree nodes; **Escape** closes an open list, else stops typing, else drops the focus.
- Pressing a widget with the mouse focuses it; pressing where there is no widget drops the focus. The focus ring (`interaction.focused`, drawn by the painters) shows only while the keyboard is used, as `:focus-visible` on the web.
- Own widgets get all of this from `interact`: its `response` has `clicked` (also for Enter/Space) and `has_focus`.

### Windows and scopes
- `f.window(title, start)` is a window that is dragged by its title bar, brought to the front by a press anywhere on it and **resized by the grip** in its bottom-right corner; `start` places it the first time. With height 0 it is as high as its content; with a height given (or once resized) it keeps its height and its content **scrolls** (wheel, or the bar on its right). Each window draws into its own list; `finish` stacks them, front window last.
- A container returns an `im::scope`, a view that borrows its parent mutably; widgets are its methods and it ends at its `}`. `section(label, open_at_first)` (collapsible, animated chevron), `tree(label, open_at_first)` (a smaller node with a guide line along its children), `card()` (a raised box), `row()` (side by side), `id(key)` (ids include `key`, for loops).
- `grid(columns)` places its widgets in equal columns, left to right, then down; `available_width()` in it is a cell.
- `scroll(height)` is a part `height` pixels high whose content scrolls: the wheel over it (the innermost area under the cursor takes it), or dragging the bar's thumb. The offset stays in range when the content shrinks.
- **`styled(theme)`** draws its widgets with another theme (denser, other colours, square corners, checkboxes for bools) and gives the parent's theme back at its `}`: a style change can't be left pushed (draft 35 §3.3).
  ```edax
  {
      mut ui::theme compact = w.theme();
      compact.density = 0.85;
      compact.controls.boolean = ui::bool_widget::checkbox;
      mut im::scope small = w.styled(compact);
      _ = small.boolean("Cast shadows", mut settings.shadows);
  }
  ```
- Popups (a combo's list) and tooltips are drawn in an **overlay** over every window; while a list is open, the windows under it don't react to the mouse.
- `scope.open()` is false for a collapsed section; its widgets return at once.
- Layout is a column (`row()`: a row) with the theme's gap; widgets that fill take the width left (`available_width()`). Windows and cards are as high as their content in the last frame, as in ImGui: a new window has its size from the second frame on.

### Sizes (§4)
- `s.width(size)` sizes the next widget (or container) of the scope: `ui::px(n)` pixels, `ui::fit` what the widget needs, `ui::percent(p)` of the parent's width, `ui::fill(weight)` a share of what the other widgets of a row leave, by weight (in a column or a grid cell: all of it).
  ```edax
  mut im::scope bar = w.row();
  if (bar.width(ui::fill(1.0)).button("Save", ui::variant::primary)) { ... }   // takes what is left
  _ = bar.button("Cancel");                                                      // fits its label
  ```
- A row measures its fixed widths and fill weights in one frame and shares the rest in the next, like `fit` (one pass, as ImGui).
- Rows keep these numbers by id: rows made from one call site in a helper need an `id(key)` scope each (the duplicate-id count shows it).

### Ids
- Every widget takes `implicit compile_time std::call_site where`; its id hashes the scope's id, the call site and the label. Two widgets with one id in a frame count in `duplicate_ids()`.

### Widgets
| Call | Is |
|---|---|
| `text(f"...")`, `muted(f"...")`, `heading(title)` | Text in the body style (wrapped to the width), muted, or the title style |
| `button(label)`, `button(label, ui::variant::primary)` | `@std::must_use`; true in the frame it was clicked. Variants: `primary`, `secondary`, `ghost`, `outline`, `danger`, `success` |
| `checkbox(label, mut b)`, `toggle(label, mut b)` | True in the frame `b` changed |
| `boolean(label, mut b)` | A toggle or a checkbox, as `theme.controls.boolean` says |
| `slider(label, mut v, min, max)` | Any number type; the value is shown with two decimals (floats) or whole |
| `number(label, mut v, min, max)`, `drag(label, mut v, speed)` | A number field: dragged sideways, typed after a click (Enter takes it, Escape drops it), arrows step; `number` stays in its range and shows how far it is |
| `input_text(label, mut text)` | A one-line text field (`ui::text_edit`): caret, selection with the mouse or Shift, Ctrl+arrows by word, Home/End, Ctrl+A/C/X/V, Ctrl+Z/Y; it scrolls to keep the caret in |
| `combo(label, mut index, items)` | One of `items` from a list below the field (above it when there is no room); up and down change it while focused |
| `choice(label, mut e)` | An enum's alternatives without payloads, by name (`std::meta::alternatives`) |
| `tooltip(text)` | Shows `text` next to the cursor when the widget before it has been hovered for 0.45 s |
| `progress(fraction)`, `separator()` | |
| `icon_button(icon, hint)`, `icon_toggle(icon, mut on, hint)`, `icon_label_button(icon, label, variant)`, `icon(icon, tint)`, `badge(icon, text, tint)` | Icons from `ui::icon` in buttons, toggles and pills ([tools and editors](studio.md)) |
| `chip(label, mut on)` | A small rounded toggle with a label, filled with the accent while on |
| `placeholder(text)` | A faint text in the next text field while it is empty |
| `spacer()`, `bar_separator()` | In a row: take the room left (push what follows to the end), a thin line between groups |
| `menu(label)` → scope with `menu_item(icon, label, shortcut)`, `menu_check(label, mut on)` | A flat button that opens a popup; hovering another menu while one is open switches |
| `available_height()` | The height left below the next widget in a panel or region (tables and log panels given 0 fill it) |

Every widget draws through the skin: the button is `skin.button(painter, button_part)`, the slider `slider_track` and `slider_knob`, and so on, so a replaced painter changes the widget everywhere.

### Own widgets (§11)
Own widgets are methods of the program's own interface, implemented for `im::scope`, and use the calls the library's widgets use:

| Call | Does |
|---|---|
| `s.make_id(key, where)` | The widget's id under this scope |
| `s.available_width()`, `s.allocate(size)` | The width left; reserves space and returns the rectangle |
| `s.interact(id, rect)` | Hover, press, click and the animated `hover`/`press` (an `im::response`) |
| `s.animate(id, slot, target)` | A value easing to `target` (0 to 1) with `motion.normal` |
| `s.state(id)` | The widget's `widget_state` between frames |
| `s.storage(id)` | The widget's `widget_storage` (numbers, text, points) between frames; its buffers are reused |
| `s.scroll_unused()`, `s.take_scroll()` | Whether the wheel is still free this frame; taking it (inner widgets first) |
| `s.region(id, rect, space)` | A part of the window at `rect` (not in the flow), clipped and scrolled: panes of splits |
| `f.panel(id, rect, order)` | A window without a title bar at `rect`, its own layer at `order` (below 16: under every window): docked windows |
| `s.painter()` | A `ui::painter` drawing into the scope's window with the current theme |
| `s.theme()` | The tokens in use |

## Example
```edax
namespace ui = std::ui;
namespace im = std::ui::immediate;

interface my_widgets {
    bool rating(std::str label, mut u32& stars, implicit compile_time std::call_site where) mut;
}

implement my_widgets for im::scope {
    bool rating(std::str label, mut u32& stars, implicit compile_time std::call_site where) mut override {
        ui::theme t = this.theme();
        ui::rect r = this.allocate(ui::vec2{ .x = this.available_width(), .y = 30.0 });
        mut bool changed = false;
        for (u32 i : 0..5) {
            ui::rect cell = ui::rect{ .x = r.right() - 110.0 + cast<f32>(cast::truncate<u16>(i)) * 22.0, .y = r.y, .width = 22.0, .height = 30.0 };
            if (this.interact(std::hash_combine(this.make_id(label, where), cast<u64>(i)), cell).clicked) {
                stars = i + 1;
                changed = true;
            }
        }
        mut ui::painter p = this.painter();
        _ = p.text(r.origin(), label, t.style(ui::text_role::body, t.colors.text));
        for (u32 i : 0..5) {
            ui::vec2 at = ui::vec2{ .x = r.right() - 99.0 + cast<f32>(cast::truncate<u16>(i)) * 22.0, .y = r.y + 15.0 };
            p.list.star(at, 10.0, 4.5, 5, 0.0, if (i < stars) { t.colors.warning } else { t.colors.border });
        }
        return changed;
    }
}

mut im::context ctx = im::context::create(ui::theme::dark());
...
{
    mut im::frame f = ctx.begin_frame(ui::input_of(win.input(), events.as_span(), 0.0), ui::window_size(win.size()));
    {
        mut im::scope w = f.window("Settings", ui::rect{ .x = 32.0, .y = 32.0, .width = 420.0 });
        {
            mut im::scope video = w.section("Video", true);
            _ = video.boolean("Vertical sync", mut settings.vsync);
            _ = video.slider("Speed", mut settings.speed, 0.0, 10.0);
        }
        _ = w.rating("Fun", mut settings.stars);
        if (w.button("Save", ui::variant::primary)) {
            save(settings);
        }
    }
    f.finish();
}
```
`example/ui_widgets` has every widget, a "Style" window that changes the tokens live (corners, knob shape, bools as toggles or checkboxes, tick shape, accent, light/dark) and a switch to the program's own painters (blocky buttons, star knobs).

`example/windowing` has a debug overlay made with this module and routed with `im::router` (the M2 dogfood).

## Not yet
`split` (two panes with a divider) is `w.split_x`/`split_y` of [docking](docking.md). The system clipboard and the IME area wait for `std::windowing` (M1).

## Rationale
- **Ids from call sites, scopes from views:** see draft 35 §10 and the M0 findings (`example/ui_spike/README.md`).
- **Painters receive parts:** widgets decide (layout, hit tests, values); painters draw. Swapping a painter can't break a widget's behaviour, and the same painter serves every place the widget is used.
- **Windows draw into their own lists:** bringing a window to the front reorders lists; nothing is drawn twice.
