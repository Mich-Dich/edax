# Node editor: `std::ui::nodes`

Draft 35 §13 (roadmap M6) in two layers:

| Layer | What the program writes | For |
|---|---|---|
| **`nodes::editor`** (§13.1, the basics) | Its own graph data; each frame it declares frames, nodes (with pins and bodies) and links, and applies the edits the editor reports | Graphs in an existing data model: an ECS, a behaviour tree, a dialogue system |
| **`nodes::graph<K>`** (§13.2, [below](#nodesgraphk)) | One enum of node kinds; pins, parameters, the add-node menu, type checks, copy and paste, undo and saving come from reflection | New graphs: materials, effects, logic, audio |

Both draw with the same styles. `example/editor` builds the car paint's material on `nodes::graph<K>` and compiles it to GLSL whenever it changes.

## Rules

### Making an editor
- **`w.node_editor(name, mut view, height)`** makes an `editor` across the scope, `height` pixels high (0: the rest of the panel).
- **The program owns a `nodes::view_state`** for each editor. It holds:
  - the view: `pan` and `zoom`;
  - the selection: `selected` nodes and `selected_links`;
  - the look: `look`, a `nodes::style` (see [Styles](#styles));
  - what the editor needs between frames: drags, the nodes' heights, the pins that had links.
- **Order inside an editor:**
  1. frames (`ed.frame`);
  2. nodes (`ed.node`), each with its pins and an optional body;
  3. links (`ed.link`);
  4. `ed.finish()`, which ends the editor and returns the `nodes::edits`.
- **Nodes:** `ed.node(id, mut position, title, nodes::node_look{ .color, .symbol, .width, .variant })` returns a `node_view`.
  - The position is in canvas units and is the program's own variable; dragging moves it.
  - The style's node painter draws the card: in the default style, the title and an icon tile in the node's colour. `variant` is free for the program's own painters (which drawing to use for this node).
- **Pins and bodies:** these are rows of the node, top to bottom, in the order they are declared.
  - `n.input(pin, label, nodes::pin_style{ .color, .shape })` puts a pin on the left edge; `n.output(...)` puts one on the right edge.
  - A pin's shape is `circle`, `square`, `triangle` or `diamond`. It is filled when a link uses it, and the output's colour colours the link.
  - `n.input_widget(pin, label, style)` is an input with a widget after its label while nothing is linked to it (the value used when the pin is unconnected). It returns a scope for that widget; while the pin is linked, the scope does nothing and only the label shows.
  - `n.body()` returns an ordinary `im::scope`, so any widget works in a node.
  - **Widgets in a node keep their own drags.** A press on a node moves it only when no widget in it (a slider, a field, the chevron) took the press; the editor decides this at `finish()`, after every widget had its turn.
- **Collapsing:** the chevron in a node's header collapses it to its header; its pins then sit on the header's edges and keep their links, and its body and inline widgets do nothing. Collapsed nodes are in `view.collapsed` (the program can set them too).
- **Links:** `ed.link(id, from, to)` takes an output pin and an input pin, both declared this frame. `ed.link_via(id, from, to, mut via)` adds reroute points (canvas units, in order from the output); the editor adds, moves and removes them as the user asks and reports the link in `edits.rerouted`. Stubs and curve handles are only at pins, so a link bends freely through its points. Links are routed by the style (curves, subway lines or straight lines) into polylines, and drawn under the nodes (`draw_list::insert` puts them before the nodes' triangles).
- **Frames:** `ed.frame(id, mut area, title, tint)` is a coloured comment box. Dragging its title moves it and the nodes inside it, and its corner resizes it.

### Zoom
The wheel zooms at the cursor from 0.35× to 2.5×. While the editor is open, the theme's density is multiplied by the zoom, so every widget in a node body (text, sliders, fields) is drawn at the zoom. The theme is restored at `finish()`, or when the editor ends without it.

### Input
| Input | Does |
|---|---|
| Drag a node | Moves it, with the rest of the selection |
| Click a node (Ctrl or Shift: add or remove) | Selects it |
| Drag from a pin to another pin | Reports `edits.created` (always from an output to an input) |
| Drag from a pin to the empty canvas | Reports `edits.dropped`: the pin and the place, for an add-node menu that connects the new node |
| Drag on the canvas | Box selection of the nodes and links the box touches (Ctrl or Shift keep the selection); while the box is dragged, what it would select is highlighted |
| Click a link (Ctrl or Shift: add or remove) | Selects it |
| Alt and a click on a link | Adds a reroute point there (links from `link_via`) |
| Drag a reroute point / Alt and a click on it | Moves it / removes it |
| Click a node's chevron | Collapses or opens it |
| Middle button, or Alt and the left button (not on a link) | Pans |
| Wheel | Zooms at the cursor |
| Right click, or Space | Reports `edits.menu_at` |
| Delete or Backspace | Reports the selected nodes and links in `deleted_nodes` and `deleted_links` |
| Ctrl+A | Selects every node and link |
| F | Fits the view to the selection (or to every node) |
| Ctrl+D | Reports `edits.duplicate` |

- **Arranging:** setting `view.arrange_next` to a `nodes::arrangement` lines up the selected nodes (two or more) in the next frame: `left`, `center`, `right`, `top`, `middle`, `bottom` align their edges or centres, `spread_x` and `spread_y` put the same space between them. The nodes' positions change and are reported in `edits.moved`.
- **A minimap** in the bottom-right corner shows the nodes (the selected ones in the accent) and the view.
- **`ui::input` has the right and middle buttons and the modifier keys** (`shift`, `control`, `alt`), and `ui::key` has `d` and `f`; `ui::input_of` fills them from the window.

### Styles
`view.look` is a `nodes::style`: sizes at zoom 1, how links run, the grid, an optional theme, and one painter per part. Everything can be changed; a program can start from a preset and change any field.

| Field | What it sets |
|---|---|
| `header_height`, `pin_row`, `pin_radius`, `padding` | Node sizes (the header, a pin's row, the pins, the space around a body) |
| `link` (`nodes::link_look`) | `shape` (`curve`, `metro`, `straight`), `round_corners` and `corner_radius`, `stub` (the straight run out of a pin), `width`, `halo` |
| `grid`, `grid_step`, `major_every` | The canvas grid: `none`, `lines`, `dots` or `crosses`, its spacing, and every how many marks one is stronger |
| `minimap` | Whether the minimap is shown |
| `canvas_theme` | A theme for everything inside the editor, the widgets in node bodies too (a light canvas in a dark program); none: the scope's |
| `draw` (`nodes::painter_set`) | The painters: `canvas`, `node`, `pin`, `pin_label`, `link`, `frame`, `selection_box`, `reroute` |

- **Painters** are `std::function<void(mut ui::painter&, X_part&)>`, like `ui::skin`'s. Each gets what the editor decided, in screen coordinates:
  - `node_part`: the card's bounds and header, the title, the `node_look`, `selected`, `preview` (inside the selection box being dragged), `hover`, the zoom, `collapsed` and the `chevron` button (`painters::chevron` draws the default one);
  - `link_part`: the routed polyline, its colour, the `link_look`, `selected`, `preview`, `hovered`, `dragging` (the link being drawn);
  - `pin_part`, `pin_label_part`, `frame_part`, `canvas_part` (bounds, origin, grid) and `box_part`.
- **Presets:** `style::studio()` (the default), `metro()`, `blueprint()`, `neon()`, `paper()`. Their painters are public in `nodes::painters` (and `painters::metro`, `::blueprint`, `::neon`, `::paper`), so a program's painter can draw something of its own for some nodes and call a preset's for the rest.

| Preset | Look |
|---|---|
| `studio` | Neutral cards with a tinted header, curved links with a halo, a line grid |
| `metro` | A subway map: thick lines with rounded corners and a casing, white stations for pins, the node's colour as a bar and as a line badge around its icon, a dot grid |
| `blueprint` | A technical drawing: white lines on blue, sharp-cornered subway links, crop marks, capitals in the mono face, dashed frames and previews |
| `neon` | Glowing outlines and links on black |
| `paper` | Cream paper, black outlines, hard offset shadows, flat colour headers; a light theme inside a dark program |

- **Subway links** (`link_shape::metro`) only run in the 8 directions: up, up-right, right, down-right, down, down-left, left and up-left. A link leaves its output to the right and enters its input from the left (after `stub`), across and at 45° when it is wide enough, steeper at 45°, straight down and at 45° again when it isn't, and around below or between the nodes when it goes backwards. With `round_corners`, every corner is an arc of `corner_radius` (smaller where the runs beside it are short).
- **`draw_list::polyline(points, width, colour)`** draws a link in one piece with mitred joints, so translucent halos don't darken where segments meet.

```
view.look = nodes::style::metro();
view.look.link.corner_radius = 24.0;
view.look.draw.node = [](mut ui::painter& p, nodes::node_part& n) {
    if (n.look.variant == 1) {
        p.list.fill(n.bounds, ui::color::hex(0xfde68a), 2.0);          // a sticky note
        _ = p.text(n.bounds.origin() + ui::vec2{ .x = 8.0, .y = 6.0 }, n.title, p.tokens.style(ui::text_role::body, ui::color::black()));
    } else {
        nodes::painters::metro::node(mut p, n);
    }
};
```

### What the program applies
- **The editor never changes the graph itself,** apart from the positions it is given. The program decides what to accept: refusing a link whose pin types don't match, replacing the link that already goes into an input, never deleting the output node.
- **`nodes::topological_order(nodes, edges)`** sorts node ids so every edge goes forward (Kahn's algorithm). It returns `std::error(id)` with a node on a cycle, so a compiler can report the cycle at that node.

## `nodes::graph<K>`

### Kinds, pins and parameters
- **The kinds are the alternatives of an enum `K`**, each with a struct payload. `@nodes::kind{ .title, .category, .color, .symbol, .width, .removable }` on the payload gives its title (default: the alternative's name), the add-node menu's group, the header's colour (`0xRRGGBB`), icon and width, and whether the user may delete it (`false` for a graph's output).
- **`@nodes::in` and `@nodes::out` fields are pins**, typed by the field's type; their labels come from `@ui::label` or the name. Pin colours come from the type: the numbers, `bool` and the `std::math` types have their own, others get one from their type id.
- **The other fields are parameters**, edited in the node's body with the editors of `inspect` (`@ui::range`, `@ui::label`, forms of the type, enums as segments or combos).
- **Unlinked inputs keep a value:** a number or `bool` input shows its editor in its row (a slider with `@ui::range`); other unlinked inputs (a `vec3`) are edited in the body. A linked input shows only its label.
- **Links** go from an output to an input of the same type, or where a function marked `@nodes::converts` takes the output's type and returns the input's (`m::vec3 splat(f32 x)` lets an `f32` go into a `vec3`). The program converts when it evaluates. A link into an input replaces the one there.
- **Pins are numbered** node id × 64 + field index, so a kind has at most 64 fields.

### The graph
`nodes::graph<K>` is plain data and works without an editor (a build step that compiles `.graph` files):

| Member | Does |
|---|---|
| `make()`, `add(value, at)`, `remove(id)`, `add_frame(title, area, color)` | Builds a graph; `remove` takes the node's links too |
| `connect(from, "output", to, "input")` | Links two fields by name; the error says why not ("a vec2 output can't go into a vec3 input") |
| `nodes`, `links`, `frames`, `index_of(id)` | The data: `graph_node<K>` (`id`, `value`, `at`), `graph_link` (`from`, `to` pins, `via` reroute points), `graph_frame` |
| `source_of(node, "input")` | The node and output linked into an input, and that output's type id (to know when to convert) |
| `topological_order()` | Node ids with every node after its inputs' nodes; the error names a node on a cycle |
| `to_text()`, `from_text(text)` | Readable text, below; std::filesystem writes and reads it (`write_text(path, g.to_text().as_str())`) |
| `undo()`, `redo()` | The editor's history |
| `view` | The editor's `view_state`: view, selection, collapsed nodes, style |

### The editor
`nodes::graph_editor(mut scope, mut graph)` draws the whole editor across the rest of the scope and changes the graph as the user edits it. It returns a `graph_edit`: `changed` (anything that can change what the graph computes, also while a slider is dragged), `committed` (a finished change was recorded as an undo step) and `problem` (why a request was refused, for a status line).

| Input | Does |
|---|---|
| Right click, Space, or a link dropped on the canvas | Opens the add-node menu there: the kinds by category; typing filters them (letters in order, case ignored), Backspace deletes, Enter adds the first, Escape or a click outside closes. For a dropped link it lists only the kinds with a pin the link fits, and links the new node |
| Ctrl+C, Ctrl+X, Ctrl+V | Copies the selected nodes and the links between them as text into the clipboard, cuts them (not the undeletable ones), pastes them (new ids, 40 units down and right, selected) |
| Ctrl+D | Duplicates the selection |
| Ctrl+Z, Ctrl+Y or Ctrl+Shift+Z | Undoes and redoes. Every finished change is one step: a drag counts when the button is let go, typing when the field is left |
| Delete | Deletes the selection; a kind with `.removable = false` stays, with a `problem` saying so |

Everything else (links, reroute points, frames, collapsing, arranging, styles) is the basics editor's.

### Text
```
edax_graph 1
node 3 mix 260.0 60.0 { a = { x = 0.0 y = 0.0 z = 0.0 } b = { x = 0.0 y = 0.0 z = 0.0 } t = 0.25 mode = add }
node 4 output 500.0 80.0 { color = { x = 0.0 y = 0.0 z = 0.0 } roughness = 0.5 name = "car \"paint\"" }
link 9 3 result 4 color 1 470.0 290.0
frame 5 0.0 0.0 220.0 300.0 15026253 "Inputs"
```
- One line per node (id, kind, place, every field but the outputs, by name), link (id, the two nodes and fields by name, the reroute points) and frame (id, area, colour, title).
- A node's fields may be numbers, `bool`, `std::string`, enums without payloads (by name) and structs of those. Another type is a compile error naming it.
- Reading is forgiving where files age: a field missing from the text keeps its default, an unknown one is skipped. An unknown kind or pin is an error naming the line.

### Example
`example/editor` (`editor/graph.ed*`), shortened:
```
@nodes::kind{ .title = "Mix", .category = "Math", .color = 0x3e8ef7, .symbol = ui::icon::filter, .width = 220.0 }
struct mix_node {
    @nodes::in m::vec3 a = default;
    @nodes::in m::vec3 b = default;
    @nodes::in @ui::range{ .min = 0.0, .max = 1.0 } f32 t = 0.5;     // a slider while unlinked
    @nodes::out m::vec3 result = default;
}
enum material_node : u16 { time(time_node), uv(uv_node), color(color_node), noise(noise_node), mix(mix_node), output(output_node) }

@nodes::converts
m::vec3 splat(f32 x) { return m::vec3{ .x = x, .y = x, .z = x }; }

// every frame
nodes::graph_edit e = nodes::graph_editor(mut s, mut mat.paint);
if (e.changed) { compile(mut mat); }

// compiling: the nodes in order, each input from its link or its own value
for (u64 id : mat.paint.topological_order()?) {
    match (mat.paint.nodes[mat.paint.index_of(id)?].value) {
        material_node::mix(p) => out.push_str(f"vec3 n{id} = mix({input(mat, id, "a", vec3_literal(p.a))}, ...);"),
        ...
    }
}
```

## Example: the basics
A graph the program keeps itself, shortened (`tests/action/run/std_ui_nodes.ed` is complete):

```
nodes::edits e = {
    mut nodes::editor ed = s.node_editor("material", mut m.view, 0.0);
    ed.frame(9000, mut m.frame, "Base color", ui::color::hex(0xe5484d));
    for (usize i : 0..m.nodes.length()) {
        node shown = m.nodes[i];
        mut nodes::node_view n = ed.node(shown.id, mut m.nodes[i].at, title_of(shown.kind),
                                         nodes::node_look{ .color = tint_of(shown.kind), .symbol = icon_of(shown.kind) });
        n.output(pin(shown.id, 4), "vec3", nodes::pin_style{ .color = type_color(value_type::vec3) });
        n.input(pin(shown.id, 0), "a", nodes::pin_style{ .color = type_color(value_type::vec3) });
        {
            mut im::scope field = n.input_widget(pin(shown.id, 2), "t", nodes::pin_style{ .color = type_color(value_type::scalar) });
            changed = field.slider("", mut m.nodes[i].amount, 0.0, 1.0) || changed;   // while t is unlinked
        }
    }
    for (usize k : 0..m.links.length()) {
        ed.link_via(m.links[k].id, m.links[k].from, m.links[k].to, mut m.links[k].via);
    }
    ed.finish()
};
for (nodes::link_request r : e.created) {
    if (accepts(pin_type(m, r.from), pin_type(m, r.to))) { connect(mut m, r.from, r.to); changed = true; }
}
if (changed) { compile(mut m); }   // topological_order, then one GLSL line per node
```

## Not yet
- **From §13.2:** groups collapsed into one node (subgraphs), finding a node, `nodes::custom_body` (a kind drawing its own body: a colour ramp, a curve, a preview), `evaluate(evaluator)` for interpreted graphs, and forms per graph.
- **Two graph editors in one scope** share their ids (the editor's id comes from its call site in the library).
- **Open question 11** (graph evaluation: only an interpreter interface, or also a build step) is still open; the graph being plain data allows both.
- **In the example:** the material is compiled to GLSL text, but the shader isn't used to draw the car yet.

## Rationale
- **Kinds as an enum, pins as annotated fields.** A kind is one struct written once: the editor, the text format, the type checks and the program's compiler (`match` on the payload) all read the same declaration, so adding a kind is adding one alternative.
- **Conversions as annotated functions.** `@nodes::converts` functions are found with `std::meta::functions_with`, so the allowed conversions are the functions the program evaluates with, and a conversion is added by writing it.
- **Undo as text snapshots.** The graph's text is already needed for saving and copying, it captures everything a change can touch (values, links, reroute points, frames), and it can't go out of step with the data. One step per finished change keeps a slider drag from filling the history.
- **Edits, not changes.** Graphs live in very different data models (an ECS, a behaviour tree, a material), and only the program knows what a valid link is. Reporting requests keeps undo with the program's data and makes type checks the program's own code.
- **Zoom through the theme's density.** Every widget already scales with the density, so node bodies zoom without a second layout or a transformed draw list, and text is still drawn from the distance-field atlas at its new size.
- **Styles as data plus painters.** Sizes and routing are data (so they can be tweaked, switched at run time and shown in a settings bar), and the drawing is functions (so a style can look like anything, as `ui::skin` does for widgets). The editor's behaviour never depends on the style: hit tests use the routed polylines whatever their shape.
- **Presses decided at the end.** A node is declared before the widgets in it, so when it sees a press it can't know yet whether a slider in its body will take it. Deciding at `finish()`, from the widget that was last under the cursor, gives the innermost widget the press, as a click on a slider in a window does.
- **Reroute points as positions.** They are part of the program's link data (saved and undone with it), and the editor changes them as it changes node positions, rather than reporting requests for something so small.
- **Box selection by touch.** The box selects what it touches, nodes and links alike (a link need not be inside the box completely), and shows it while it is dragged, so what the release will do is never a surprise.
- **Links under the nodes by inserting triangles.** Links are declared after the nodes because their ends are the pins' positions of this frame. Inserting their triangles at the mark taken before the first node draws them under the nodes, without a frame of lag and without a second layer.
