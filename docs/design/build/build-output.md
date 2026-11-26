# Build Output and Diagnostics

What `edaxc build`, `run` and `test` print for a `build.ed` project: a **checklist** with a line per target, each target's **details** when asked for, every warning and error, and a **last line** that names `build.log`, where everything is written. On a terminal the lines of the targets still running are **live**: rewritten in place under what is finished.

The second half of the page is about errors: how edaxc keeps a broken file from producing a wall of follow-up errors.

## Rules

### The checklist
```
edaxc build: debug, 4 targets
 ✓ build.ed       19 targets                                          1.2 s
 ✓ stats          2 units, 912 B                                      0.1 s  build/c/libstats.a
 ▸ windowing      ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━╺━━━━━━━━    6/7       7.7 s  link 0.2 s
 · hello          waiting
```
- The first line names the command, the mode and the number of targets: the ones asked for (by name, or every program / test) and the C/C++ libraries they link.
- **`build.ed`** is the first line: the time to evaluate it (and its dependencies' build scripts), the number of targets and dependencies.
- **Each target** has a mark: `·` waiting, `▸` running, `✓` done, `=` up to date, `✗` failed, `-` not built. A line waits with the reason (`waits for gluttony`).
- **A unit** is a piece of a target's work, counted when it is **finished**, not when it starts:

  | Target | Its units |
  |---|---|
  | Edax program, test or plugin | Each of its source files (done once checked), and the phases `read`, `check`, `code`, `link` |
  | C/C++ library | Each source file (compiled in parallel), then `archive` (or `link` for a shared library) |

- **A running line** shows a bar, `done/total`, the time so far, and the units running now by name, slowest first (`link 6.1 s`): the progress doesn't sit on `44/45` without saying why.
- **The bar is weighted** by the last build's unit times (`build/<mode>/.edax/timings`), so a link that takes half the time counts for half the bar. Without a previous build every unit weighs the same.
- **A finished line** says what was made: `45 units, 556.67 KiB` (the units and the size of the sources), the warnings and errors, the time, and the file built.

### Failures don't stop the build
- A target that fails is `✗` with its error count; the targets that don't need it are still built (like `make -k`). A target that links a failed one is `- not built: needs broken`.
- `--stop-on-error` starts no target after the first failure (`not built: the build stopped at the first error`).
- The last line becomes `build failed: 1 error, 0 warnings   details: build/debug/.edax/logs/build.log`, and edaxc exits with status 1.

### The last line
```
built 5 targets in 21.3 s (1 up to date): 0 errors, 2 warnings   details: build/debug/.edax/logs/build.log
```
Warnings and errors are printed in full as they happen, but a terminal keeps only so much; the last line says where all of it is.

### Live lines and plain lines
| `--progress=` | Output |
|---|---|
| `automatic` (default) | `live` when stdout is a terminal, `lines` otherwise |
| `live` | The running and waiting lines are rewritten in place (at most every 100 ms, only when they changed), cut to the terminal's width and height; finished lines and diagnostics are printed above them once and stay. Each redraw is one write that overwrites the lines in place, without erasing them first, so nothing flickers (as for [`std::log::status`](../stdlib/logging/output.md)) |
| `lines` | Each target is printed once, when it finishes; no escape codes (CI, pipes, editors) |
| `quiet` | Diagnostics and the last line only |

- Under a running target, the `sources` detail (below) shows its table with marks; when the table doesn't fit on the screen, only the running rows are shown. When the target finishes, its whole table is printed once.
- **`edaxc run`** writes the build output to **stderr**, so the program's stdout is its own.
- `edaxc test` prints `PASS`/`FAIL` per test as the tests run, then the last line, then `N of M tests passed`.
- A single file (`edaxc build hello.ed`, no `build.ed`) prints only its `built '...'` line.

### Details
A **detail** is shown under the line it belongs to. Every detail is always written to `build.log`; the console shows the ones asked for.

| Detail | Under | Shows |
|---|---|---|
| `sources` | Each target | `source file details: 45 source files, total size 556.67 KiB`, then each file with its lines, size, mark and time |
| `units` | Each target | `phases:` the phases after the source files, with their times |
| `commands` | Each C/C++ target | The compiler command of each file and the archive / link command |
| `packages` | `build.ed` | Each package or dependency, its tag and directory |
| `toolchain` | The first line | edaxc and LLVM versions, the C compiler and linker, the target |
| `timings` | The last line | The 10 slowest units of the build |

```
 ✓ app            5 units, 67 B                                       0.6 s  build/debug/app
    source file details: 1 source file, total size 67 B
     ✓      6 lines       67 B   src/app.ed    0.2 s
    phases:
     ✓ read          0.3 s
     ✓ check         0.2 s
     ✓ code          0.0 s
     ✓ link          0.0 s
```
- Paths are relative to the target's root; the columns are sized to the table's widest value. An up-to-date unit has no time.

### `build.log`
- Every build writes `build.log` into the hidden `.edax/logs/` of the output folder of the program it builds (`build/<mode>/.edax/logs/build.log` by default; see "Output folders" in [Edax as a Build System](build-system.md)) and keeps the previous 5 next to it as `build.1.log` (the newest) to `build.5.log`. Like the timings and each target's up-to-date stamps (`.edax/stamps/<file>.inputs` in the target's folder), the logs are edaxc's own files: hidden, but next to what was built.
- It has everything the console has, without colors, plus every detail, every command, and **every** diagnostic: also the ones the error limit (below) leaves off the console.
- `--no-log-file` (or `.log_file = false`) writes none; the last line then says `(no build.log)`.

### Configuring the output in `build.ed`
```edax
namespace std::build {
    enum progress { automatic, live, lines, quiet }
    enum detail { sources, units, commands, packages, toolchain, timings }

    view struct output {
        std::build::progress          progress = std::build::progress::automatic;
        std::span<std::build::detail> details  = [];     // for every target
        bool                          log_file  = true;
        u32                           keep_logs = 5;
        // The lines (below); "" is the default.
        std::static_str target_line = "";
        std::static_str done_line   = "";
        std::static_str source_row  = "";
        std::static_str last_line   = "";
        std::static_str failed_line = "";
    }

    class graph {
    public:
        compile_time void output(std::build::output o) mut;
        compile_time void details(std::build::target t, std::span<std::build::detail> d) mut;
    }
}
```
```edax
g.output(std::build::output{ .details = [std::build::detail::sources] });
g.details(app, [std::build::detail::sources, std::build::detail::units]);   // one target differs
```
- `g.output` sets the project's defaults, `g.details(target, ...)` one target's details.
- **The command line overrides both:** `--progress=...`, `--details=sources,units,...|all|none` (`none`: no details, whatever `build.ed` says), `--no-log-file`, `--stop-on-error`.

### The lines are patterns
Each line is a pattern with `{name}` placeholders; `{name:<12}` pads to 12 columns on the left, `{name:>8}` on the right, `{name:<*}` to the widest value of the table, `{bar:40}` draws a 40-column bar; `{{` is a `{`.

| Pattern | Default | Placeholders |
|---|---|---|
| `target_line` (running) | `" {mark} {target:<*} {bar:40} {done:>4}/{total:<4} {elapsed:>8}  {running}"` | `mark target bar percent done total elapsed running` |
| `done_line` | `" {mark} {target:<*} {summary:<48} {elapsed:>8}  {output}"` | `mark target summary elapsed output` |
| `source_row` | `"     {mark} {lines:>12} {size:>10}   {file:<*}  {time:>7}"` | `mark lines size file time` |
| `last_line` | `"built {targets} in {elapsed} ({up_to_date} up to date): {errors}, {warnings}   details: {log}"` | `targets elapsed up_to_date errors warnings log` |
| `failed_line` | `"build failed: {errors}, {warnings}   details: {log}"` | the same |

Trailing spaces are removed from every line.

### Live lines in Edax programs: `std::log::status`
edaxc draws its checklist with its own C++ code; an Edax program gets the same kind of live lines from `std::log::status` (see [Logging output](../stdlib/logging/output.md)).

## Diagnostics

### One mistake, one error
A missing `}` or `)` makes the rest of a file mean something else, and a parser that reads on reports every follow-up as a new error. Before parsing, edaxc checks the file's brackets `( [ {` and, when they don't match, **repairs them where the indentation says they belong**, reports that once, and parses the repaired file:

```
error[E2016]: unclosed '{'
   --> windowing/application.ed:185:35
    |
185 |                 .size  = wnd::size{ .width = 480,
    |                                   ^ this '{' is never closed
    |
186 |             }, vulkan).map_error([](wnd::error e) { return std::format(f"no Vulkan window: {e}"); })?;
    |             -
    = note: this '}' on line 186 lines up with the '{' on line 183, so it closes that one
```
(Without the repair, this one deleted `.height = 320 },` gave seven errors, the first of them on line 5.)

- **A closer at the start of a line** closes the opener whose line has the same indentation; the openers left open in between are the ones reported.
- **An unclosed `(` or `[`** is closed at the end of its own line's statement: before the `;` or the block's `{` on that line.
- **A block still open at the end of the file** is closed where the code goes back to the opener's indentation.
- **A closer with nothing open** is left to the parser, which reports it where it is (`expected a declaration, found '}'`).
- At most 3 bracket errors are reported per file, and the parser's own errors at the repaired places are dropped: they would only describe the repair.
- A file whose brackets match is never changed, however it is indented.

### The error limit
- After the first **20** errors, the rest are counted, not shown:
  ```
  ... and 14 more errors not shown: all of them are in build/debug/.edax/logs/build.log
  ```
  (`--error-limit=0 shows them all` when there is no `build.log`, as for a single file.)
- `--error-limit=N` changes the limit; `0` shows every error. Warnings after the limit are counted too.
- In a project build, each target's diagnostics are printed above the checklist; the target's line counts them, so the closing `N errors, M warnings` line of a single-file build is left out.
- `--diagnostic-format=short` (one line per diagnostic, for tools) has no limit.

## Rationale
- **A checklist, not a scrolling log:** during a build a developer wants to see what is left and what is slow; after it, what was built and what went wrong. One line per target answers both on one screen.
- **Finished counts and named running units:** a counter of started jobs sits on `[122/123]` while the slowest job runs. The names and times of what is running are the honest progress, and they point at the slow file.
- **The detail table as the target's own checklist:** marks and times on each source row turn a file list into progress.
- **Keep going after a failure:** one broken library shouldn't hide the errors of the other targets; one build shows them all.
- **`build.log` and the last line:** a terminal forgets; the log doesn't. Naming it in the last line means a warning that scrolled away is one `less build/debug/.edax/logs/build.log` away, so the console can stay short.
- **`run` on stderr:** the program's output can be piped without the build's lines in it.
- **Repairing brackets by indentation:** the parser can't know which `}` was meant, but the person who indented the code told it. Indentation is a hint only for errors: a file with matching brackets means exactly what its brackets say.
- **The error limit:** the first errors are the ones to fix; the hundredth follow-up error overflows the terminal and hides them. Nothing is lost: `build.log` has the rest.
