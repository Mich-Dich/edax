# Docking: `std::ui::dock`

A dockspace arranges a program's windows in splits and tab stacks around a central area of documents. The user rearranges them by dragging tabs; the arrangement is data that can be saved, compared and changed by code. Draft 35 §12 describes the plan; the roadmap calls this milestone M5. The module is built on [`std::ui::immediate`](immediate.md).

## Rules

### Windows are named, not placed
- **`space.window(name)`** returns a scope for the window's widgets, wherever the layout puts it.
  - The scope is closed (`open()` is false) when the window's tab isn't the one shown, and when the window is closed.
  - A window the layout doesn't know yet joins the first tab stack (none: a new stack right of everything).
- **`space.document(name)`** is the same for documents, which live in the central area.
- **Names are the ids of docked windows**, not call sites. That is why a layout can be saved and restored.
- Each docked window is a panel with its own layer, so widgets in it are hovered only when it is in front. Its scrolling and widget state belong to its name.

### The dockspace
- **`f.dockspace(name, mut ws)`** fills the frame. **`f.dockspace_in(name, mut ws, rect)`** fills a part of it, for example under a toolbar.
  - Both return a `dock::space`, which borrows the frame and the workspace until its `}`.
- **Tab stacks.**
  - A press on a tab shows it.
  - A tab's × closes the window. The × shows on the active tab and on hovered ones.
  - The stack that holds the last-pressed window has its tab underlined.
- **Splits** are resized by dragging the divider between their parts. A double-click returns the divider to the share the layout was made with.
- **The central area** holds documents and stays when it is empty.
- **Floating stacks** are windows of their own, in front of everything docked. Dragging the empty part of their bar moves them, and a press brings them to the front.
- **Layers:** the dockspace's tab bars and dividers are layer order 1 and docked panels are order 2. Ordinary windows start at 16 (`ctx.next_order`), so docked windows are always behind them.

### Dragging a tab
- **A drag starts once a pressed tab moves 6 px.** The tab then follows the cursor, and drop targets appear:
  - five on the stack under the cursor: centre (join the stack), left, right, top and bottom (split it in half);
  - one on a floating stack (join it);
  - four on the dockspace's edges (a column or row of a quarter, beside everything).
- **The hovered target shows a preview** of where the window will go.
- **Dropping on a target** moves the window.
- **Dropping elsewhere** makes it a floating window at the cursor. A floating window that is alone in its stack just moves there.
- **Dropping a window on its own stack, where it is the only window, changes nothing.**
- The tab rectangles of the last frame are kept (`ws.tab_of(name)`), so scripted input and tests can drag them.

### Closing and reopening
- **`layout.close(name)`** remembers where the window was:
  - the windows on the other side of the split it was in (or the other windows of its stack, with its tab position);
  - the side;
  - its share of the split;
  - or its floating rectangle.
- **`layout.reopen(name)`** puts it back beside the smallest part of the layout that still holds those windows. It splits that part on the same side with the same share, so closing and reopening restores the layout exactly.
  - When none of those windows is left, the window joins the first stack.
- **`w.window_menu(mut ws, start)`** shows a button for every closed window and a "Reset layout" button.

### The layout is data
- **A `dock::layout`** holds:
  - nodes: `split_x`, `split_y`, `tabs`, `central`;
  - floating stacks;
  - the closed windows' places.
- **Build one in code:**
  - `dock::tabs(["A", "B"])`, `dock::central()`, `dock::central_with(["Scene"])`;
  - `dock::split_x(ratio, first, second)` and `dock::split_y`, where `ratio` is the first part's share.
- **Change it by code:**
  - `dock_next_to(name, anchor, side, share)`, `dock_at_edge`, `float_window`;
  - `activate`, `remove`, `close`, `reopen`, `add`, `add_document`.
- **Ask it:** `find`, `contains`, `is_closed`, `windows()`, `central_node()`.
- **`to_text()` writes readable text** (one node per line, indented, names quoted), and `layout::from_text(text)` reads it back; none means the text isn't a layout. `same(other)` compares two layouts; `clone()` copies one.

```
edax-dock 1
split_x 0.2 0.2
  tabs 0 1 "Outliner"
  split_x 0.75 0.75
    split_y 0.7 0.7
      central 0 1 "Scene"
      tabs 0 2 "Console" "Frame time"
    tabs 0 2 "Inspector" "Theme"
end
```

### Workspaces
- **`dock::workspace::make(layout)`** keeps the layout in use (`current()`; `edit()` to change it), named layouts and what the dockspace remembers between frames.
- **Named layouts:**
  - `ws.save_as("Animation")` keeps the layout in use under a name.
  - `ws.switch_to("Layout")` keeps the current one under its name first, then switches.
  - `ws.reset(layout)` replaces the layout in use, for example from a "Reset layout" button.
- **Saving and loading:**
  - `ws.save(path)` writes the layout in use and the named layouts.
  - `dock::workspace::load(path)` reads them; none means the file is missing or not a workspace.
  - `ws.save_if_changed(path)` writes only after a change.
- **Hot reload:** a workspace kept in a plugin's state marked `@std::hot_reload_state` keeps its layout over a reload.

### Splits inside a window
- **`w.split_x(name, ratio, height)` and `split_y`** give two panes with a divider the mouse drags; a double-click resets it. They use the same divider painter as the dockspace.
- `s.first()` and `s.second()` return the panes' scopes, one after the other (each borrows the split until its `}`). `s.ratio()` is the first pane's share.

### Look
- The parts come from the skin (`ui::skin`) and can be replaced like every other painter:
  - `dock_tab` (`ui::dock_tab_part`)
  - `dock_bar` (the strip behind the tabs)
  - `dock_stack` (`ui::dock_stack_part`: a stack's background behind its tab bar and content; cards in the studio skin)
  - `divider` (`ui::divider_part`)
  - `drop_target` (`ui::drop_target_part`, with the preview of the result)

- **Tab icons:** `ws.set_icon(name, icon)` shows an icon before a window's title; `ws.icon_of(name)` reads it. Icons aren't saved with the layout.
- **Sizes from the theme:** the tab bar is `controls.window.tab_height` high and the parts of a split are `controls.window.dock_gap` apart.

### Own windows and panes: what docking uses
`std::ui::immediate` gained two calls for this:

| Call | Does |
|---|---|
| `f.panel(id, rect, order)` | A window without a title bar: its own layer at `order`, the surface colour behind it, its content clipped and scrolled |
| `s.region(id, rect, space)` | A part of a window at `rect`, not placed in the flow, clipped and scrolled, with `space` around its content |

## Example
```edax
namespace dock = std::ui::dock;

dock::layout default_layout() {
    return dock::split_x(0.2, dock::tabs(["Outliner"]),
           dock::split_x(0.72, dock::split_y(0.64, dock::central_with(["Scene"]), dock::tabs(["Console", "Frame time"])),
                               dock::tabs(["Designer", "Player", "Windows"])));
}

mut dock::workspace ws = dock::workspace::load("editor.layout").value_or(dock::workspace::make(default_layout()));
dock::layout start = default_layout();
while (running) {
    {
        mut im::frame f = ctx.begin_frame(input, size);
        {
            mut dock::space space = f.dockspace("editor", mut ws);
            {
                mut im::scope scene = space.document("Scene");
                if (scene.open()) {
                    draw_scene(mut scene, car);
                }
            }
            {
                mut im::scope w = space.window("Designer");
                if (w.open()) {
                    _ = w.inspect("vehicle", mut car, designer_form);
                }
            }
            {
                mut im::scope w = space.window("Windows");
                if (w.open()) {
                    w.window_menu(mut ws, start);
                }
            }
        }
        f.finish();
    }
    _ = ws.save_if_changed("editor.layout");
}
```
`example/editor` is the docked editor of the mockup. `--screenshot out.png` writes one frame, and `--screenshot out.png --drag` writes one while a tab is dragged over the scene's left target.

## Not yet
- A menu bar (M7). For now, `window_menu` lives in a docked window.
- Shift to drop floating: `ui::input` has no modifier state yet, so a drop on no target floats the window.
- Tab overflow menus, pinned tabs, closing with the middle button, and reordering tabs within a stack.
- Resizing floating stacks.
- Compile-time layouts (`compile_time dock::layout`). They need compile-time values as static data; layouts are built at run time, which costs a few small allocations once.
- Docking across operating-system windows (M9).

## Rationale
- **Names as ids.** A saved layout has to find its windows again in the next run, and call sites change with every edit of the code. A name is what the user sees on the tab anyway. This decides open question 1 for docked windows; other widgets keep call-site ids.
- **A layout is a tree in an array.** Nodes refer to their children by index, so a layout is plain data. It can be copied, compared, written as text and changed by code without pointers or ownership puzzles. Removed nodes are cleared and simply never reached again.
- **Remembering a closed window's place by its neighbours.** Node indices change when the tree changes, but the windows beside a closed one usually stay. The smallest part that still holds them is where it belongs.
- **Docked windows are panels, not windows with chrome.** The dockspace draws the tab bars, and each window's content is a clipped, scrolled panel with its own layer. Hovering and stacking then work as they do for ordinary windows.
