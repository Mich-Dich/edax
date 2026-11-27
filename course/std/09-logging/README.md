# Std lesson 9: Logging (`std::log`)

In C++, logging is a library decision (spdlog, glog, your own) and its cost is a guess: does a disabled `LOG_DEBUG << expensive()` still call `expensive()`? `std::log` answers those questions in the language's terms. A log call is an ordinary function call. Whether it **exists** is decided at compile time, per function, type, file and build target, and a removed call leaves no code at all. Calls that exist are filtered at run time with one atomic load, and their f-string holes run only if the message is actually written. One thread owns all the outputs, so sinks never need locks.

**You will learn:** the levels, a session and its sinks (console, file, memory), patterns, compile-time levels with `@std::log::level_override`, `validate` and `assert`, live status lines, and what happens to the log on a panic.

**You will build:** the logging of an asset importer, with a console and a "problems panel" (`exercise.ed`).

Examples are in [`examples/`](examples); `python3 course/check.py s9 --examples` checks them.

---

## 1. Calls and levels

```edax
std::log::info(f"loaded {count} meshes in {elapsed.as_milliseconds()} ms");
```

| Call | Level | Note |
|---|---|---|
| `std::log::trace(...)`, `debug`, `info`, `warn` | as named | |
| `std::log::error(...)` | `error` | never dropped when the queue is full |
| `std::log::fatal(...)` | `fatal` | writes and flushes **everything** before it returns; never removed; doesn't end the program |

- The message is a `std::format_args` (std lesson 4): an f-string or a plain literal. Holes and specs are checked at compile time, and a message ending in `\n` gets a warning (sinks add the line break).
- Every call knows where it is (file, line, function, module, target) through an `implicit` parameter: nothing to pass, no macros.
- Records logged **before** a session starts are kept (up to 256) and written once the first sink is added, so start-up messages aren't lost.

## 2. The session and its sinks

```edax
mut std::log::session logs = std::log::start(std::log::options{});
_ = logs.add(std::log::console::make(), std::log::sink_options{
    .min_level = std::log::level::debug,
    .pattern   = std::log::pattern::from("{level:<5} {file_name}:{line} {message}"),
});
```

- `start` launches **one thread**, named `log`, which owns every sink. A log call formats its message on the calling thread and queues it; the log thread renders and writes. Sinks need no locks, and slow output never blocks the caller (unless the queue is full: `when_full::wait`, the default, or `drop`).
- Each sink has its own **minimum level**, **pattern**, filter and colours, so each sink is a channel: a console from `info`, a file with everything, a panel with only problems.
- `logs.flush()` returns once everything sent before it is written. **Destroying the session** writes the rest, flushes, and joins the log thread: shutdown happens exactly once, at the end of the session's scope.
- Because writing is asynchronous, a `std::println` right after a log call can come out first. Flush before printing directly to the same stream.

| Sink | Writes to |
|---|---|
| `std::log::console::make()`, `make_with(std::log::colors::off)` | stdout; `error` and `fatal` to **stderr**. Colours when both are terminals (`automatic`) |
| `std::log::file::create(file_options)` | a file kept open, with flushing, rotation by size or day, and `json_lines` output |
| `std::log::memory::make(history)` | a `std::log::history` ring of the last lines; `snapshot()` reads it from any thread (an in-game console) |
| your own type implementing `std::log::sink` | anything: network, GUI |

Examples: [`basics.ed`](examples/basics.ed), [`sinks.ed`](examples/sinks.ed).

## 3. Patterns

A pattern is an f-string over the record's fields: `{message}`, `{level}`, `{time}`, `{date}`, `{millis}`, `{thread}`, `{target}`, `{module}`, `{file}`, `{file_name}`, `{function}`, `{function_name}`, `{line}`, `{column}`, and `{color}` ... `{/color}` around the coloured part. Fields take alignment and width: `{level:<5}`.

`std::log::pattern::from("...")` is `compile_time run_time` with an entry check, so a typo in a literal pattern is a compile error at the literal:

```
error[E4210]: std::log::pattern: unknown field: '{levle}' (fields: message, level, time, date, millis, ...)
```

Ready-made: `std::log::patterns::minimal` (`INFO  loaded 12 meshes`), `standard`, `source`, `engine`, `detailed`, `file`.

Example: [`err_logging.ed`](examples/err_logging.ed).

## 4. Which calls exist: compile-time levels

Every call has a compile-time level; a call below it is **removed** (no code, no string in the binary, holes never evaluated). The most specific setting wins:

| # | Set on | How |
|---|---|---|
| 1 | the function | `@std::log::level_override{ .level = ... }` before it |
| 2 | the type | the same annotation on the type |
| 3 | the file | `@std::log::level_override{ .level = ... };` at the top of the file |
| 4 | files of a target, from `build.ed` | `std::log::set_level(mut g, target, "glob*.ed", level)` |
| 5 | the command line | `-Dlog_level=debug`, after `std::log::add_options(mut g, target)` |
| 6 | the target, from `build.ed` | `std::log::set_level(mut g, target, level)` |
| 7 | the build mode | `trace` in debug-based modes, `error` in release-based modes |

So a chatty subsystem can be silenced at compile time without touching its code (rows 4–6), or keep its debug output in one file you're working on (rows 3–4). Note the release default: `info` calls are removed from release builds unless you say otherwise.

Then, at run time, each sink's minimum filters what exists; the lowest of them is one `std::atomic<u8>`, so a filtered call costs one load and one branch.

## 5. `validate` and `assert`

| | `std::log::validate` | `std::log::assert` | `run_time check` |
|---|---|---|---|
| For | conditions that may fail: bad input, a missing file | conditions that must hold: a bug | failures that leave nothing safe to do |
| On failure | logs at `error` | logs at `fatal` (flushed at once), stops in the debugger if attached | panics |
| Then | returns `false` | returns `false` | the program ends |

Both are `@std::check_result`: the caller must handle the `false`, with `else`:

```edax
std::log::validate(id < count, f"texture {id} is out of range ({count} textures)") else {
    return false;
}
```

The condition is evaluated in every build mode; only the messages follow the log levels.

Example: [`validate.ed`](examples/validate.ed).

## 6. Live status lines

```edax
mut std::log::status progress = logs.status();
for (...) {
    progress.set(f"importing [{i + 1}/{n}] {name}");   // rewritten in place, under the scrolling log
    ...
}
progress.finish(f"imported {k} of {n} assets");        // becomes one ordinary info record
```

On a terminal, the line at the bottom is redrawn in place (rate-limited, never flickering, cut to the terminal's width). On a pipe or in a file there are no live lines at all, and `finish` (and `print`) write ordinary `info` records. The same code works in CI and in a terminal without an `if`.

## 7. On a panic

`start` installs the panic hook (next lesson): on a panic in any thread, the hook logs the panic message as a `fatal` record, makes the log thread write and flush everything queued, and only then lets the runtime print the panic and end the process. The last lines before a crash are therefore never lost in a queue.

---

## C++ → Edax at a glance

| C++ (spdlog-style) | Edax |
|---|---|
| `spdlog::info("{} meshes", n)` | `std::log::info(f"{n} meshes")` |
| `SPDLOG_ACTIVE_LEVEL` macro | compile-time levels: annotation, `build.ed`, mode |
| `spdlog::set_pattern("%l %v")` | `std::log::pattern::from("{level} {message}")`, checked at compile time |
| async logger + thread pool | the session's log thread |
| `spdlog::shutdown()` | the session's destructor |
| `assert` | `run_time check` (ends) or `std::log::assert` (logs, returns `false`) |
| a progress bar library | `std::log::status` |

---

## Your goal

`exercise.ed` imports five assets. One is empty (an error), one is huge (a warning). The console shows everything from `info`; a "problems panel", a `memory` sink, collects everything from `warn`. Fill in the TODOs so that stdout is:

```
INFO  importing 5 assets
WARN  huge.png is 66 MiB, importing anyway
INFO  imported 4 of 5 assets
problems panel (2):
  import_asset: tree.png is empty
  import_asset: huge.png is 66 MiB, importing anyway
```

(The console also writes `ERROR tree.png is empty`, to stderr. `checking ...` must not appear anywhere: remove those calls at compile time, without deleting them.)

<details>
<summary>Hint 1: the sinks</summary>

`logs.add(std::log::console::make_with(std::log::colors::off), std::log::sink_options{ .min_level = std::log::level::info, .pattern = console_pattern });` and the same with `std::log::memory::make(problems)`, `warn` and `panel_pattern`.
</details>

<details>
<summary>Hint 2: <code>import_asset</code></summary>

`@std::log::level_override{ .level = std::log::level::warn }` before the function. Inside: `std::log::validate(a.bytes > 0, f"{a.name} is empty") else { return false; }`, then the size check with `std::log::warn`.
</details>

<details>
<summary>Hint 3: the status line</summary>

`mut std::log::status progress = logs.status();` before the loop, `progress.set(f"...")` in it, `progress.finish(f"imported {imported} of {assets.length()} assets");` after it. On the checker's pipe only the `finish` line shows.
</details>

---

## Try this afterwards

1. Run the solution in a terminal (`build/compiler/edaxc run course/std/09-logging/solution.ed`). What's different?
2. Remove the `level_override`, and set the console's minimum to `debug`.
3. Build with `--mode=release`. Which lines are left?
4. Write `std::log::info("done\n");`.

<details>
<summary>What happens</summary>

1. Colours are off (`make_with(colors::off)`), but the status line now appears and is rewritten in place while the assets are imported, and the `ERROR` line shows up among the others (stderr and stdout share the terminal).
2. Five `DEBUG checking ...` lines appear: the calls exist again.
3. First a compile warning, `std::log::verbose_override`: the annotation makes `import_asset` log at `warn` in a release build, which the library wants you to notice. Then `INFO  importing 5 assets` is gone (release-based modes remove `info` calls by default), while the warning and error stay. `INFO  imported 4 of 5 assets` stays too: `progress.finish` isn't a log call that can be removed, it's a method of the status that always writes its record.
4. A warning, `std::log::trailing_newline`: the sink adds the line break itself.
</details>

## Recap

- Log calls are function calls; compile-time levels decide whether they exist at all.
- One session, one log thread, many sinks; each sink has its level, pattern and colours.
- Patterns are checked at compile time; `validate` and `assert` log and return; `status` draws live lines only where it can.
- On a panic, the log is flushed before the process ends.

Next: [std lesson 10](../10-crashes/README.md), panics and crash handling.
