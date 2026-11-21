# Logging Output: Records, Patterns, Sinks, the Session

What happens to a log call after it is made: it becomes a record, the log thread renders it with each sink's pattern and writes it. The calls and levels are in logging.

## Rules

### Records and patterns
A sink receives a **record**:
```edax
namespace std::log {
    view struct record {
        std::log::level      level;
        std::call_site       where;         // file, line, function, module, target (see Functions)
        std::thread::id      thread_id;
        std::str             thread_name;   // set at spawn or with std::thread::set_current_name
        std::time::wall_time time;          // when the call was made
        std::str             message;       // the formatted message, without a line break
        bool                 from_panic;    // the message of a panic: the runtime prints it after the hook
        std::span<std::log::field> fields;  // the message's holes, as name and value (below)
    }
}
```

A **pattern** turns a record into one line. It uses the f-string syntax of 19 with record fields as names:
```edax
private compile_time std::log::pattern console_pattern = std::log::pattern::from(
    "{color}[{time}.{millis}] [{target} {level:<5} {thread}]{/color} {file_name}:{line} {message}");
```
| Field | Value |
|---|---|
| `{message}` | The formatted message |
| `{level}` | `TRACE` ... `FATAL` |
| `{time}`, `{date}`, `{millis}`, `{micros}` | `hh:mm:ss`, `yyyy-mm-dd`, `000`–`999`, `000000`–`999999` (local time) |
| `{thread}`, `{thread_id}` | The thread's name (its id if it has none), the id |
| `{target}`, `{module}`, `{namespace}` | Build target (`renderer`), module (`renderer::vulkan::pipeline`), namespace (`gfx::vulkan`) |
| `{file}`, `{file_name}` | The file as edaxc was given it, the file name only |
| `{function}`, `{function_name}` | `gfx::vulkan::create_pipeline`, `create_pipeline` |
| `{line}`, `{column}` | Numbers |
| `{color}`, `{/color}` | Start and end of the level's color. Sinks without color drop them |

- A field may have an alignment and a width: `{level:<5}`, `{thread:>10}`, `{level:^7}`. `{{` and `}}` are braces.
- **`std::log::pattern::from(std::static_str)`** is `compile_time run_time` and checks its text with a `compile_time run_time check` entry (see [Checks](../../language/errors/checks.md)). An unknown field or a bad spec in a literal is a compile error at the literal:
  ```
  error: std::log::pattern: unknown field: '{levle}' (fields: message, level, time, ...)
   --> app/main.ed:12:62
  ```
- **Ready-made patterns** in `std::log::patterns`:

  | Pattern | Example line |
  |---|---|
  | `minimal` | `INFO  loaded 12 meshes` |
  | `standard` (the default) | `[14:33:10.887] [INFO ] main: loaded 12 meshes` |
  | `source` | `[14:33:10.887] [INFO ] scene.ed:42 loaded 12 meshes` |
  | `engine` | `[14:33:10.887] [renderer INFO  main - scene.ed:load:42] loaded 12 meshes` |
  | `detailed` | `[2026-11-19 14:33:10.887412] [renderer INFO  main] gfx::load (src/scene.ed:42) loaded 12 meshes` |
  | `file` | `[2026-11-19 14:33:10.887] [renderer INFO  main] src/scene.ed:42 loaded 12 meshes` (no colors) |

  All but `file` wrap the time and level in `{color}` ... `{/color}`.
- A pattern owns its text (a `std::string`). Built at compile time it lives in static memory (see [Compile-Time Evaluation](../../language/compile-time/compile-time.md)), so a pattern can be a `compile_time` constant.
- **Patterns from run-time text** (a config file): `std::log::pattern::parse(text) → std::result<std::log::pattern, std::log::pattern_error>` runs the same checks at run time.
- Rendering scans the text once per record, byte by byte.
- Sinks add the line break.

**Structured fields.** Every hole of the message is also in `record.fields`, as a `std::log::field`:
```edax
namespace std::log {
    view struct field {
        std::static_str name;      // the hole's expression as written: `count`, `mesh.name`, `elapsed.as_milliseconds()`
        std::str        value;     // the hole's formatted text, with its spec applied
    }
}
```
`std::log::info(f"loaded {count} meshes in {ms} ms")` gives the fields `count` and `ms`. The names come from the compiler (they are static data); the values are the text already formatted for the message, so fields cost no extra formatting. Sinks that write machine-readable logs use them.

### Sinks
```edax
namespace std::log {
    interface sink {
        bool colored();                                    // whether `{color}` is written
        void write(std::log::record& r, std::str line) mut; // `line`: the record in the sink's pattern
        void flush() mut;
    }

    struct sink_options {
        std::log::level   min_level = std::log::level::trace;
        std::log::pattern pattern   = std::log::default_pattern;
        std::log::filter  filter    = std::log::filter{};   // std::log::filter::all(), std::log::filter::targets("renderer")
        std::log::palette palette   = std::log::default_palette;
    }
}
```

**Colors per level.** A sink that writes colors (`colored()`) renders `{color}` with its `palette`, one `style` per level:
```edax
namespace std::log {
    enum color : u8 {
        none,                                                       // the terminal's own color
        black, red, green, yellow, blue, magenta, cyan, white,
        gray, bright_red, bright_green, bright_yellow, bright_blue, bright_magenta, bright_cyan, bright_white,
        indexed(u8),                                                // the 256-color table
        rgb(std::log::rgb),                                         // 24-bit: std::log::rgb{ .r, .g, .b }
    }

    struct style {
        std::log::color foreground = std::log::color::none;
        std::log::color background = std::log::color::none;
        bool            bold = false;
        bool            underline = false;
    }

    struct palette {
        std::log::style trace = std::log::style{ .foreground = std::log::color::gray };
        std::log::style debug = std::log::style{ .foreground = std::log::color::bright_blue };
        std::log::style info  = std::log::style{ .foreground = std::log::color::bright_green };
        std::log::style warn  = std::log::style{ .foreground = std::log::color::yellow };
        std::log::style error = std::log::style{ .foreground = std::log::color::red };
        std::log::style fatal = std::log::style{ .foreground = std::log::color::black, .background = std::log::color::red, .bold = true };
    }
}
```
```edax
_ = logs.add(std::log::console::make(), std::log::sink_options{
    .pattern = std::log::patterns::engine,
    .palette = std::log::palette{ .info = std::log::style{ .foreground = std::log::color::rgb(std::log::rgb{ .r = 255, .g = 128, .b = 0 }) } },
});
```
- A sink is added to the session with its options (below). The log thread applies `min_level` and `filter`, renders the pattern, and calls `write`. So each sink is a **channel**: a file that only gets the `renderer` target, a history that gets everything from `warn` up, a console that gets `info` and up.
- Sinks are **owned by the log thread**. They're moved there when added, so they must be `std::sendable` (see [Threads](../concurrency/threads.md)). They are never shared, so they need no locks.

The standard library provides:

| Sink | Writes to | Options and behaviour |
|---|---|---|
| `std::log::console::make()`, `make_with(colors)` | stdout; `error` and `fatal` to stderr | `std::log::colors::automatic` (colors when stdout and stderr are terminals), `always`, `off`. Skips panic records, which the runtime prints itself. `flush()` flushes the C streams |
| `std::log::file::create(file_options&)` | A file, kept open; missing directories above it are created | `.path`, `.mode = truncate / append`, `.flush_at` (default `warn`: those records are flushed at once), `.buffer_size` (default 64 KiB: flushed when that much is pending), `.rotation` (below), `.format = text / json_lines` (`json_lines`: one JSON object per record, with the time, level, location, message and each field as a key). Writes "log started" / "log stopped" records with the date and time |
| `std::log::memory::make(history&)` | A `std::log::history` | `history::make(capacity)` keeps the last lines; `snapshot()` copies them from any thread (an in-game console) |

Any other destination (network, GUI, an engine plugin) is a type implementing `std::log::sink`.

**Rotation** of a file sink is set with `.rotation`:
```edax
namespace std::log {
    struct size_rotation {
        u64 max_bytes;          // start a new file when the current one would grow past this
        u32 keep = 5;           // older files to keep: app.1.log (newest) ... app.5.log
    }
    enum rotation : u8 {
        none,                   // the default: one file
        by_size(std::log::size_rotation),
        daily(u32),             // a new file at local midnight; the payload is how many old files to keep
    }
}
```
Rotation happens on the log thread between two records, so a record is never split across files.

### The session
```edax
mut std::log::session logs = std::log::start(std::log::options{
    .queue_capacity = 16 * 1024,                     // records
    .when_full      = std::log::when_full::wait,     // or drop
});
std::log::sink_id console = logs.add(std::log::console::make(), std::log::sink_options{
    .min_level = std::log::level::info,
    .pattern   = console_pattern,
});
```
- `start` starts **one thread** named `log`, which owns the sinks. A log call formats its message on the calling thread and puts the record into a **bounded channel** (see [Threads](../concurrency/threads.md)). The message is a `std::string`: messages up to 23 bytes are stored inline, longer ones allocate once.
- **Changes travel through the same channel:** `logs.add(...)`, `logs.remove(id)`, `logs.set_level(id, level)`, `logs.set_pattern(id, pattern)` and `logs.set_palette(id, palette)` send a command (a payload enum, see [Enums](../../language/user-types/enums.md)). A change applies exactly between the records sent before and after it. `add`, `remove` and `set_level` are `mut`: the session also keeps the run-time minimum.
- `logs.flush()` returns once everything sent before it is written and every sink is flushed.
- **Destroying the session** closes the channel. The log thread writes what's left, flushes and destroys every sink, and is joined. Log calls go to the early buffer again (below).
- One session exists at a time. A second `start` panics.
- **Full queue:** with `wait` (the default) the caller waits for room. With `drop`, records from `trace` to `warn` are dropped and counted, and the next record that gets through is followed by `"[std::log] 120 messages dropped"`. `error` and `fatal` always wait.

### Before the session, after it, and on panics
- **Before `start`**, records go to a static buffer of 256 records, behind a spin lock. `start` sends them to the log thread first, in order; the number of records that didn't fit is logged as a warning.
- **The log thread keeps the records that arrive while it has no sink** (at most 256, the early ones included) and writes them to **each sink when it is added**. A session started and given its sinks at once therefore loses nothing.
- **Without a session**, `fatal`, failed asserts and panics print the buffered records on stderr.
- **On a panic** (any thread, any cause: a failed `run_time check`, an overflow, `expect`): the runtime first calls the panic hook (see [Error Handling](../../language/errors/error-handling.md)), which `start` installs. The hook logs the panic message as a `fatal` record, makes the log thread write every queued record and flush every sink, and returns; then the runtime prints the panic, so it is the last line, and the process aborts. The hook runs once (a panic inside it doesn't run it again), and a panic on the log thread itself doesn't wait for the log thread. Panics run no destructors (see [Error Handling](../../language/errors/error-handling.md)), which is why this is a hook.
- `fatal` and a failed `assert` log their record and wait until it and everything before it is written and flushed; then they return.

## Examples

### A program's `main`
```edax
// app/main.ed
import std;
import engine;

private compile_time std::log::pattern console_pattern = std::log::pattern::from(
    "{color}[{time}.{millis}]{/color} [{color}{target} {level:<5} {thread} - {file_name}:{function_name}:{line}{/color}] {message}");
private compile_time std::log::pattern file_pattern = std::log::pattern::from(
    "[{date} {time}.{millis}] [{target} {level:<5} {thread}] {file}:{line} {message}");

std::exit_code main(std::span<std::str> args) {
    std::thread::set_current_name("main");
    std::log::info(f"starting with {args.len()} arguments");                 // kept until a sink exists

    mut std::log::session logs = std::log::start(std::log::options{ .queue_capacity = 16 * 1024 });
    _ = logs.add(std::log::console::make(), std::log::sink_options{
        .min_level = std::log::level::info,
        .pattern   = console_pattern,
    });
    std::log::file_options file_options = std::log::file_options{
        .path     = std::filesystem::executable_dir() / "logs" / "gluttony.log",
        .flush_at = std::log::level::warn,
    };
    _ = logs.add(std::log::file::create(file_options).expect("can't open the log file"),
                 std::log::sink_options{ .pattern = file_pattern });

    mut engine::application app = engine::application::make(args);
    app.run();
    return std::exit_code::success;
}   // app is destroyed, then logs: every record written, every sink flushed, the log thread joined
```

## Rationale
- **One thread owns the sinks:** sinks need no locks and can't race; turning records into lines happens off the calling thread.
- **Changes through the queue:** a new pattern or level applies in message order, and the commands are typed.
- **Colors belong to the sink:** a palette per sink, so a terminal and an in-game console can color the same records differently; files get none.
- **The session is a value:** destroying it flushes and joins, so shutdown happens exactly once. Panics flush through a hook in the runtime, because they run no destructors.
