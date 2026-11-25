# Plots and debug tools: `std::ui::plot`, `std::ui::debug`

Charts for immediate-mode UIs, made for frame times, sensor logs and large data sets. Debug panels for the program's performance and its log. Both are built on [`std::ui::immediate`](immediate.md) and use only its own-widget API. Draft 35 §8 and §9 describe the plan; the roadmap calls this milestone M4.

## Rules

### Making a plot
- **`w.plot(title, height)`** returns a `plot::scope`. The scope borrows `w` until its `}`.
  - It spans the scope's width. Its height is `height` times the theme's density.
  - An empty title leaves no title row.
- **Series calls collect; the `}` draws.**
  - The calls are `line`, `area`, `scatter`, `bars`, `series`, `hline` and `vline`.
  - The ranges of all series are known only at the `}`, so the plot fits them and draws them there.
- **Data is read in place from `std::span<T>`, for any number type.**
  - The x value is the index (`p.line("cpu", ys)`), a second span (`p.line("pos", xs, ys)`), or a lambda over rows (`p.line_of("hp", units, [](unit& u) { return u.health; })`).
  - `plot::history<T, N>` keeps the last `N` values. `as_span()` views them, oldest first, without a copy: every value is stored twice, so the view is always contiguous.
- **A plot holds at most 16 series and lines (`plot::max_entries`).** Further calls are ignored.
- **Colours** come from the palette:
  - Hues are spread around the theme's primary colour, at its lightness and chroma (`plot::palette_color`).
  - `p.color(c)` sets the next series' colour.
  - Lines take their colour as an argument: `p.hline("60 fps", 16.7, p.theme().colors.warning)`.

| Call | Draws |
|---|---|
| `line(name, ys)`, `line(name, xs, ys)`, `line_of(name, rows, value)` | A line; NaN values are gaps |
| `area(name, ys)`, `area(name, xs, ys)` | A line with the area down to 0 filled |
| `scatter(name, ys)`, `scatter(name, xs, ys)` | Points (every point; not reduced) |
| `bars(name, ys)` | A bar per value at x = 0, 1, 2, … |
| `series(name, rows)` | One line per `@plot::series` field against the `@plot::x` field (or the index) |
| `hline(name, y, color)`, `vline(name, x, color)` | A line across the plot, labelled at its end |

### Axes
- **`p.axis_x(plot::axis{ ... })` and `p.axis_y(...)`** set an axis.
  - `.label`: the y label (usually a unit) goes after the title; the x label goes under the ticks.
  - `.min` and `.max` fix the range that fitting uses.
  - `.limit_min` and `.limit_max` are hard limits: no pan, zoom or fit goes past them. A pan stops at a limit, a zoom out stops at the width between two limits, and a fitted range is cut there.
    - Only positive x: `p.axis_x(plot::axis{ .limit_min = std::some(0.0) })`. Only negative y: `p.axis_y(plot::axis{ .limit_max = std::some(0.0) })`.
    - On a logarithmic axis the limits are values, not powers of ten.
    - `plot::clamp_range(low, high, limit_min, limit_max)` applies the same rule to any range.
  - `.kind = plot::scale::logarithmic` works on the y axis. Its ticks are powers of ten, and values ≤ 0 become gaps.
- **Ticks are 1, 2 or 5 times a power of ten** (`plot::ticks::for_range`). Their number comes from the plot's size and the labels' widths.
- **Labels** use `plot::format_number`: the decimals the step needs, and powers of ten for very large and very small values (`1.23e9`).
- **Units from reflection:** a `@plot::series{ .unit = "m" }` field labels the y axis unless the program set a label.

### Interaction
- **Pan:** drag the plot. **Zoom:** turn the wheel around the cursor, on both axes.
- **Fit again:** double-click, or the **Reset** button that shows in the plot's top-right corner while the view is moved. After a pan or zoom the plot keeps its range; until then it follows the data.
  - From code: `p.moved()` tells whether the view was moved, and `p.reset_view()` makes it follow the data again (for example from a toolbar button for several plots).
- **Hover:** shows a line at the cursor's x, the nearest point of every series and their values in a box. `p.hovered()` gives the cursor in plot coordinates.
  - The box stays steady while values change: its width grows in steps of 8 px and never shrinks while the cursor stays on the plot, and it keeps its side of the cursor while it fits there. Both reset when the cursor leaves.
- **Legend:** shown when a plot has two or more series; `p.legend(false)` turns it off. A click on an entry hides or shows its series.
- **Shared x axis:** `p.link_x("group")` makes plots with the same group share their x range, so panning or zooming one moves all.
- The wheel goes to the innermost widget that wants it: a plot inside a scroll area zooms, and the area doesn't scroll (`scope.scroll_unused()`, `take_scroll()`).

### Large data
- **Each series is reduced to what the plot's pixel columns can show.**
  - Few points are kept as they are.
  - With more than four per column, each column keeps its first, lowest, highest and last point, in their order.
  - Peaks are never lost, and the line looks the same.
- **Only the points in view are kept**, plus one neighbour on each side so lines reach the edges.
- **Index-x series reduce fastest.** When x is the index (`line(name, ys)`, `area`, histories), a column is a slice and needs no division per point.
  - Reducing 1 000 000 `f32` values to 1 000 columns takes about 1.4 ms (`bench/plot_decimate`). A plain loop doing the same in C++ takes 4.4 ms.
- **The reduced points are kept in the plot's `widget_storage`**, cleared and refilled each frame. Once the buffers are large enough, drawing a plot allocates nothing.
- `plot::decimate` and `plot::decimate_indexed` are public, for programs that draw series themselves.

### Small charts
- **`w.sparkline(values)`** draws a line the height of a row, without axes, in a text row or a table cell.
- **`w.stat(label, value, trend, plot::stat_options{ ... })`** draws a card with:
  - a label and a big number with `.decimals` decimals;
  - a `.unit` after the number;
  - a filled sparkline of `trend` under it.

  With `.warn_above`, the number and the sparkline turn to the warning colour above that value.
- **`w.gauge(label, fraction, value, caption, tint)`** draws a 240° arc filled to `fraction` (0..1) with a soft glow, `value` in big type in its middle, `caption` under it and `label` above in capitals.
- **Areas fade out:** `area` series and filled sparklines go from their colour at the line to nearly transparent at the ground (`draw_list::fill_between_gradient`).

### Debug tools: `std::ui::debug`
- **`dbg::frame_stats`** keeps the last 300 frame times, the last 120 FPS values and the last frame's draw data.
  - `stats.record(ctx)` takes them from the context after a frame.
  - `record_seconds(s)` takes a frame time the program measured.
  - `record_work(ms)` takes the program's own work in a frame.
- **`f.perf_overlay(mut stats)`** shows FPS and frame-time cards, a frame-time plot with 60 and 30 fps lines, and the vertex and draw-call counts. It is a window in the top-right corner.
- **`w.perf_panel(mut stats)`** shows the same inside a window of the program.
- **`dbg::log_sink::create(capacity)`** returns a `std::log` sink and a `dbg::log_feed`.
  - The sink keeps the newest `capacity` lines, rendered with the sink's pattern, together with their levels.
  - The feed reads them from any thread.
- **`f.log_view(title, mut feed, rect)` and `w.log_panel(mut feed, height)`** show the log:
  - a search field (case-insensitive), a Clear button, and the lowest level shown (`dbg::log_filter`);
  - the count of warnings and errors;
  - the lines in their levels' colours, with errors on a red band.

  The newest line is at the bottom, and the view stays there unless it is scrolled up.
- `std::ui::debug` loads `std::ui::plot` and `std::log` when a program names it.

### Own widgets: what the plots use
The plots are written with the own-widget API of `std::ui::immediate`, plus these calls:

| Call | Does |
|---|---|
| `s.storage(id)` | The widget's `widget_storage`: numbers, text and points it keeps between frames, with buffers that are reused |
| `s.scroll_unused()`, `s.take_scroll()` | Whether the wheel is still free this frame, and taking it |
| `ctx.clock` | Seconds since the context was made (double-clicks, timers) |

`ui::draw_list::fill_between(upper, lower, color)` fills the band between two polylines without seams. Areas under lines use it.

## Example
```edax
namespace ui = std::ui;
namespace plot = std::ui::plot;
namespace dbg = std::ui::debug;

struct sample {
    @plot::x                     f64 time = 0.0;
    @plot::series{ .unit = "m" } f64 altitude = 0.0;
    @plot::series                f64 speed = 0.0;
    u32 id = 0;                                     // not plotted
}

mut plot::history<f32, 600> cpu_ms = default;
cpu_ms.push(frame_ms);
{
    mut plot::scope p = w.plot("Frame time", 200.0);
    p.axis_y(plot::axis{ .label = "ms", .min = std::some(0.0), .max = std::some(25.0), .limit_min = std::some(0.0) });
    p.line("cpu", cpu_ms.as_span());
    p.hline("60 fps", 16.7, p.theme().colors.warning);
}
{
    mut plot::scope p = w.plot("Flight", 180.0);
    p.axis_x(plot::axis{ .label = "seconds", .limit_min = std::some(0.0) });
    p.series("", flight_log.as_span());             // altitude and speed against time
}
w.stat("CPU", cpu_ms.mean(), cpu_ms.as_span(), plot::stat_options{ .unit = "ms", .warn_above = std::some(16.7) });

f.perf_overlay(mut stats);
f.log_view("Log", mut feed, ui::rect{ .x = 650.0, .y = 650.0, .width = 700.0, .height = 300.0 });
```
`example/ui_plots` shows every kind, the debug tools, linked plots and a logarithmic axis. `--screenshot out.png` writes one frame.

## Not yet
- Time axes (labels from `std::time`), category axes, more than one y axis and subplots.
- `stairs`, `bar_groups`, `stacked_bars`, `histogram`, `heatmap`, `error_bars`, `candles`, `pie`.
- `region`, `text_at`, `marker`, and the draggable `drag_line`, `drag_point` and `drag_rect`.
- Box zoom, and Shift to zoom only one axis (the input has no modifier state yet).
- Reducing `scatter` series.
- `texture_viewer`, and memory in `perf_overlay` (no allocator statistics yet).

## Rationale
- **Collect, then draw at the `}`.** Fitting needs the range of every series, and ImPlot's "set up axes first" order is easy to get wrong. A scope that draws when it ends needs no setup phase: axes and series can come in any order.
- **Reduce per column, not by sampling.** Sampling every n-th point hides spikes, and a frame-time spike is exactly what a frame-time plot is for. Four points per column draw the same picture as all of them.
- **Spans in, nothing copied.** The program owns its data; the plot reads it once per frame and keeps only the reduced points, in buffers it reuses.
- **Limits are separate from `.min`/`.max`.** `.min`/`.max` say what fitting shows; the limits say where the view may go at all. A frame-time plot can fit 0 to 25 ms and still let the user zoom, but never below 0.
- **The hover box is steadied in the library.** Its width came from the widest value text, so a changing number resized it every frame; with the box left of the cursor that moved its left edge too. A program can't fix that from outside, so the plot keeps the width and the side.
- **The x range is kept per plot or per group.** Linked plots share one stored range. The range is ordinary widget storage, so it survives as long as the context does.
- **Debug tools are a module of their own.** They depend on `std::log`, which a program that only draws charts doesn't need loaded.
