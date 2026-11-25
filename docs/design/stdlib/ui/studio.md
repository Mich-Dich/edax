# Tools and editors: the studio look, icons, toolbars and menus

What `std::ui` offers for dense, clear tool UIs: a theme and a skin made for them, a set of line icons, and the widgets of an editor's chrome (icon buttons, chips, toolbars, menus, badges, gauges, status bars). `example/editor` uses all of it: a 3D scene view and a node graph drawn with the draw list, docked into cards.

## Rules

### The studio look
- **`ui::theme::studio(accent, mode)`** is a theme for tools:
  - Neutral near-black greys that aren't tinted by the accent (dark: background `#09090b`, panels `#141417`, raised `#1d1d21`, sunken `#0e0e11`, borders `#26262c`).
  - The accent is used only for what is chosen, active or focused, and it glows softly there (primary buttons, chips, the focused tab's underline, progress).
  - 13 px body text, 12 px semibold labels, 11 px letter-spaced capitals for groups, 24 px rows and buttons, a 4 px gap, radii of 4, 8 and 10 px.
  - Flat buttons (no gradient, no shadow), checkboxes for bools, short animations.
  - Docked stacks are rounded cards 6 px apart with 34 px tab bars inside them.
- **`ui::skin::studio()`** holds the painters that go with it. Every one of them is in `ui::paint::studio`, so a program can reuse them one by one.
  - Hairline borders, and chevrons from the icon set.
  - Section headers are letter-spaced capitals ("TRANSFORM") with a hairline to the right; fields are sunken boxes with an accent ring while typing.
  - Docked stacks are cards (`dock_stack`); the active tab is underlined, in the accent with a glow when its stack has the focus; tabs show their window's icon.
  - Segmented controls slide an accent thumb; progress bars are accent gradients.
  - Selected rows are tinted with the accent, and scroll bars are thin.
- **To use both:**

  ```
  mut im::context ctx = im::context::create(ui::theme::studio(ui::color::hex(0x3b82f6), ui::brightness::dark));
  ctx.skin() = ui::skin::studio();
  ```
- **The dock's sizes are theme tokens:** `controls.window.tab_height` (default 30) and `controls.window.dock_gap` (default 4).

### Type and drawing
- **Letter spacing:** `type_style.tracking` and `text_style.tracking` add space after each character, in ems. The text role `text_role::overline` (11 px semibold, 0.08 em) is for group labels; `ui::upper(text)` writes them in capitals.
- **Draw list additions:**
  - `fill_gradient_x(r, left, right, radius)` for horizontal gradients;
  - `glow(r, radius, color, size)` for a soft light around a box;
  - `fill_between_gradient(upper, lower, top, bottom)` for areas that fade out (plots use it);
  - `dashed_line(from, to, width, dash, gap, color)`;
  - `bezier(a, b, c, d, width, color)` for curves such as graph links.

### Icons
- **`ui::icon`** is a set of 52 line icons (plus `icon::none`). They are drawn as vectors, so they are sharp at any size and coloured like text.
  - Actions: `close`, `plus`, `minus`, `checkmark`, `chevron_*`, `search`, `filter`, `more`, `menu`, `settings`.
  - Playback: `play`, `pause`, `stop`, `step`.
  - Visibility: `eye`, `eye_off`, `lock`, `unlock`.
  - Things: `folder`, `file`, `code`, `cube`, `light`, `camera`, `layers`, `grid`, `list`, `car`, `globe`, `bolt`.
  - Editing: `undo`, `redo`, `save`, `refresh`, `trash`, `copy`.
  - States: `info`, `warning`, `error`.
  - Tools: `cursor`, `translate`, `rotate`, `scale`.
  - Panels: `terminal`, `chart`, `history`, `pin`, `maximize`, `layout`.
- **`list.draw_icon(icon, box, color)`** fits an icon into the square centred in `box`. The icons are designed on a 16 × 16 grid with 1.5 px strokes.
- **`list.arc(center, radius, from, to, width, color)`** draws an arc (0 degrees is right, 90 is down).

### Toolbars and status bars
- **`f.panel(id, rect, order, inset)`** is a panel with its own inset: a menu bar, a toolbar or a status bar is a panel with a `row()` in it. `example/editor` uses a 40 px top bar and a 24 px status bar around `f.dockspace_in`.
- **Icon buttons:**
  - `w.icon_button(icon, hint)` is a square button that stays flat until hovered. `hint` is its tooltip and its id.
  - `w.icon_toggle(icon, mut on, hint)` stays pressed (accent) while `on`. Use it for tools and view options.
  - `w.icon_label_button(icon, label, variant)` is a button with an icon before its label.
- **Chips:** `w.chip(label, mut on)` is a small rounded toggle with a label ("Grid", "Wireframe"), filled with the accent while on, for view options over a viewport.
- **Placeholders:** `w.placeholder("Search objects…")` gives the next text field a faint text shown while it is empty.
- **Small pieces:**
  - `w.icon(icon, tint)` puts an icon in the flow.
  - `w.badge(icon, text, tint)` is a small pill for counts and states, like the console's warnings and errors.
- **Spacing in a row:**
  - `row.spacer()` takes the room the row's other widgets leave. One spacer pushes what follows to the end of the row; two spacers centre a group.
  - `row.bar_separator()` draws a thin vertical line between groups.

### Menus
- **`row.menu(label)`** is a flat button that opens a popup below it. The scope it returns holds the items and is open while the popup is.
  - While one menu is open, hovering another menu's button opens that one, as in a menu bar.
  - A press elsewhere or Escape closes it.
  - The popup is as wide as its widest item.
- **Items:**
  - `menu_item(icon, label, shortcut)` returns true when clicked, and the menu closes. Use `icon::none` for no icon and an empty shortcut for none.
  - `menu_check(label, mut on)` flips `on` and leaves the menu open.
  - `separator()` draws a line between groups.
- **Example:**

  ```
  {
      mut im::scope file = bar.menu("File");
      if (file.menu_item(ui::icon::save, "Save", "Ctrl+S")) { save(); }
      file.separator();
      _ = file.menu_check("Autosave", mut autosave);
  }
  ```

### Filling a panel
- **`w.available_height()`** is the height left below the next widget in the panel or region the scope is in. In windows sized by their content it is 0.
- **Lists that fill the rest:** `w.table(label, rows, 0.0)` and `w.log_panel(mut feed, 0.0)` fill the rest of the panel.

### Changes to widgets that every look gets
- **Tables size their columns from their content.** Each column is as wide as its title and the widest cell seen so far.
  - When the table is too narrow for all of that, titles are shortened before cells are, and text that doesn't fit ends with "…".
  - Numbers are right-aligned, and bools show a check mark.
  - The sort direction shows as a chevron, and thin lines separate the headers.
- **Enums in forms:**
  - Their combo boxes show labels ("Racing red", not `racing_red`).
  - An enum whose names don't fit side by side in the row becomes a combo box instead of squeezed segments.
- **Fields without a label:** `input_text("", ...)` and the other form rows have no label column when the label is empty, so the field takes the whole row.
- **The console's log panel** has one toolbar: search, lowest level shown, warning and error badges, and Clear.

## Example

The top bar of `example/editor` holds menus on the left, tools and play controls in the middle (between two spacers), and undo, redo and Save on the right:

```
mut im::scope top = f.panel(std::hash("top bar"), ui::rect{ .width = size.x, .height = 40.0 }, 3, 8.0);
mut im::scope bar = top.row();
bar.icon(ui::icon::bolt, t.colors.primary);
{
    mut im::scope file = bar.menu("File");
    if (file.menu_item(ui::icon::save, "Save", "Ctrl+S")) { std::log::info(f"scene saved"); }
}
bar.spacer();
for (usize i : 0..4) {
    mut bool on = state.tool == kinds[i];
    if (bar.icon_toggle(tools[i], mut on, hints[i])) { state.tool = kinds[i]; }
}
bar.bar_separator();
_ = bar.icon_toggle(ui::icon::play, mut state.playing, "Play (Ctrl+P)");
bar.spacer();
if (bar.icon_label_button(ui::icon::save, "Save", ui::variant::primary)) { save(); }
```

### Gauges and cards
- **`w.gauge(label, fraction, value, caption, tint)`** (std::ui::plot) is a 240° arc filled to `fraction` with a soft glow, the value in big type in its middle, the caption under it and the label above in capitals.
- **Docked stacks are cards:** the `dock_stack` painter draws a stack's background behind its tab bar and its content (`ui::dock_stack_part`: bounds, bar, content, focused, floating), and the content panel has none of its own. The standard painter fills the content as before.
- **Tab icons:** `ws.set_icon("Console", ui::icon::terminal)` shows an icon before a window's title.

## Not yet
- Submenus, keyboard navigation in menus, and context menus.
- Columns that can be resized or hidden, and a filter row for tables.
- An icon font, or icons loaded from SVG files.

## Rationale
- **A second theme and skin, not a new default.**
  - The standard look is roomy and suits small tools, settings and games.
  - An editor shows many more values at once, so it needs 24 px rows, neutral colours that don't compete with the content, and an accent used only for state. Making that a separate theme and skin keeps the standard look as it is.
- **Icons as vectors in the draw list.** One shader already draws lines, circles and polygons with anti-aliasing, so icons cost no textures and no font, and they stay sharp at every size and DPI.
- **Menus are scopes.** The items are ordinary code in a block, as for sections and windows, so a menu can show what the program knows at that moment, such as the closed windows in a Window menu.
- **Columns from content.** Equal column widths cut short values that need little room and long names that need more. Measuring only the rows in view keeps a table of 100 000 rows as cheap as before. Widths only grow, so scrolling doesn't make columns jump.
