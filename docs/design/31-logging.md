# 31 - Logging (`std::log`)

`std::log` writes messages from any thread to any number of **sinks**: the console, files, an in-game console, the network. A log call is an ordinary function call:
```rift
std::log::info(f"loaded {count} meshes in {elapsed.as_milliseconds()} ms");
```
- The call knows where it is written (file, line, function, module, build target) through an `implicit` parameter (section 10.1).
- Whether the call exists is decided at compile time from the levels of its function, type, file and build target (section 2). A removed call costs nothing: no code, no string in the binary, and its f-string holes are never evaluated.
- Calls that exist are filtered again at run time by the sinks' levels (section 3).

## Rules

### 1. Levels
```rift
namespace std::log {
    enum level : u8 { trace, debug, info, warn, error, fatal, off }
}
```
| Function | Level | Notes |
|---|---|---|
| `std::log::trace(f"...")` | `trace` | |
| `std::log::debug(f"...")` | `debug` | |
| `std::log::info(f"...")` | `info` | |
| `std::log::warn(f"...")` | `warn` | |
| `std::log::error(f"...")` | `error` | Never dropped when the queue is full (section 6) |
| `std::log::fatal(f"...")` | `fatal` | Writes every queued record and flushes every sink before it returns. Never removed. It doesn't end the program |

- The message is a `std::format_args`: an `f"..."` or a plain string literal. Its holes and specs are checked at compile time (see 19).
- `off` as a level removes every call except `fatal`.
- `std::log::write(lvl, message)` logs at a level given as a compile-time argument; the functions above call it.
- `fatal` is for failures the program may still handle (reset a subsystem, retry, shut down cleanly); to end the program, call `std::panic` after it.
- Recoverable errors are logged with `std::log::error(...)` and returned with `std::error(...)` (see 11). Conditions are checked with `std::log::validate` and `std::log::assert` (section 8). A `run_time check` (see 23) is the language's assert: it panics, and its panic is logged (section 7).

### 2. Compile-time levels: which calls exist
Every call has a **compile-time level**, taken from the first of these that applies (the most specific wins):

| # | Where it's set | How |
|---|---|---|
| 1 | The enclosing **function** | `@std::log::level_override{ .level = ... }` on the function |
| 2 | The enclosing **type** (all its methods) | `@std::log::level_override{ .level = ... }` on the type |
| 3 | The **file**, in the file itself | `@std::log::level_override{ .level = ... };` at the top of the file (section 10.5) |
| 4 | The **file**, from `build.rifts` | `std::log::set_level(mut g, target, "<glob>", level)` |
| 5 | The command line | `-Dlog_level=<level>`, declared by `std::log::add_options(mut g, target)` |
| 6 | The **target**, from `build.rifts` | `std::log::set_level(mut g, target, level)` |
| 7 | The **build mode** | `trace` in modes based on `std::build::mode::debug`, `error` in modes based on `std::build::mode::release` |

A call whose level is below its compile-time level is **removed**. `std::log::level_at(where)` is the `compile_time` function that computes the level; it is ordinary library code (section 11).

**In the file:**
```rift
// renderer/vulkan/pipeline.rifts
import std;
import gfx::vulkan_device;

@std::log::level_override{ .level = std::log::level::trace };

namespace gfx::vulkan {
    ...
}
```

**In `build.rifts`:**
```rift
compile_time void build(mut std::build::graph& g) {
    std::build::target renderer = g.add_library(std::build::rift_target{ .name = "renderer", .root = "renderer/lib.rifts" });
    std::log::add_options(mut g, renderer);                                                   // -Dlog_level=<level>
    std::log::set_level(mut g, renderer, std::log::level::info);                              // the target's level
    std::log::set_level(mut g, renderer, "vulkan/pipeline*.rifts", std::log::level::debug);  // some files
}
```
- The glob is relative to the directory of the target's root file. A glob that matches no file is a build error.
- The per-file form records a per-file setting (see 25), so changing it recompiles only the matching files.
- `-Dlog_level` must name a level; anything else is an error.

**The annotation:**
```rift
namespace std::log {
    annotation(std::meta::target::file, std::meta::target::type, std::meta::target::function)
    struct level_override {
        std::log::level level = std::log::level::trace;
    }
}
```

### 3. Run-time filtering
- Every sink has a **minimum level** (section 5) that can be changed while the program runs.
- The lowest minimum of all sinks is kept in one `std::atomic<u8>`, the **run-time minimum**. A call below it returns after one relaxed load and one branch, without formatting its message (its holes aren't evaluated, section 10.4).

### 4. Records and patterns
A sink receives a **record**:
```rift
namespace std::log {
    view struct record {
        std::log::level      level;
        std::call_site       where;         // file, line, function, module, target (section 10.2)
        u64                  thread_id;
        std::str             thread_name;   // set at spawn or with std::thread::set_current_name
        std::time::wall_time time;          // when the call was made
        std::str             message;       // the formatted message, without a line break
        bool                 from_panic;    // the message of a panic: the runtime prints it after the hook
    }
}
```

A **pattern** turns a record into one line. It uses the f-string syntax of 19 with record fields as names:
```rift
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
| `{file}`, `{file_name}` | The file as riftc was given it, the file name only |
| `{function}`, `{function_name}` | `gfx::vulkan::create_pipeline`, `create_pipeline` |
| `{line}`, `{column}` | Numbers |
| `{color}`, `{/color}` | Start and end of the level's color. Sinks without color drop them |

- A field may have an alignment and a width: `{level:<5}`, `{thread:>10}`, `{level:^7}`. `{{` and `}}` are braces.
- **`std::log::pattern::from(std::static_str)`** is `compile_time run_time` and checks its text with a `compile_time run_time check` entry (see 23). An unknown field or a bad spec in a literal is a compile error at the literal:
  ```
  error[E4210]: std::log::pattern: unknown field: '{levle}' (fields: message, level, time, ...)
   --> app/main.rifts:12:62
  ```
- **Ready-made patterns** in `std::log::patterns`:

  | Pattern | Example line |
  |---|---|
  | `minimal` | `INFO  loaded 12 meshes` |
  | `standard` (the default) | `[14:33:10.887] [INFO ] main: loaded 12 meshes` |
  | `source` | `[14:33:10.887] [INFO ] scene.rifts:42 loaded 12 meshes` |
  | `engine` | `[14:33:10.887] [renderer INFO  main - scene.rifts:load:42] loaded 12 meshes` |
  | `detailed` | `[2026-11-19 14:33:10.887412] [renderer INFO  main] gfx::load (src/scene.rifts:42) loaded 12 meshes` |
  | `file` | `[2026-11-19 14:33:10.887] [renderer INFO  main] src/scene.rifts:42 loaded 12 meshes` (no colors) |

  All but `file` wrap the time and level in `{color}` ... `{/color}`.
- A pattern holds its text (`std::static_str`), so it can be a `compile_time` constant. Rendering scans the text once per record, byte by byte.
- Sinks add the line break.

### 5. Sinks
```rift
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
```rift
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
```rift
_ = logs.add(std::log::console::make(), std::log::sink_options{
    .pattern = std::log::patterns::engine,
    .palette = std::log::palette{ .info = std::log::style{ .foreground = std::log::color::rgb(std::log::rgb{ .r = 255, .g = 128, .b = 0 }) } },
});
```
- A sink is added to the session with its options (section 6). The log thread applies `min_level` and `filter`, renders the pattern, and calls `write`. So each sink is a **channel**: a file that only gets the `renderer` target, a history that gets everything from `warn` up, a console that gets `info` and up.
- Sinks are **owned by the log thread**. They're moved there when added, so they must be `std::sendable` (see 27). They are never shared, so they need no locks.

The standard library provides:

| Sink | Writes to | Options and behaviour |
|---|---|---|
| `std::log::console::make()`, `make_with(colors)` | stdout; `error` and `fatal` to stderr | `std::log::colors::automatic` (colors when stdout and stderr are terminals), `always`, `off`. Skips panic records, which the runtime prints itself. `flush()` flushes the C streams |
| `std::log::file::create(file_options&)` | A file, kept open; missing directories above it are created | `.path`, `.mode = truncate / append`, `.flush_at` (default `warn`: those records are flushed at once), `.buffer_size` (default 64 KiB: flushed when that much is pending). Writes "log started" / "log stopped" lines with the date and time |
| `std::log::memory::make(history&)` | A `std::log::history` | `history::make(capacity)` keeps the last lines; `snapshot()` copies them from any thread (an in-game console) |

Any other destination (network, GUI, an engine plugin) is a type implementing `std::log::sink`.

### 6. The session
```rift
mut std::log::session logs = std::log::start(std::log::options{
    .queue_capacity = 16 * 1024,                     // records
    .when_full      = std::log::when_full::wait,     // or drop
});
std::log::sink_id console = logs.add(std::log::console::make(), std::log::sink_options{
    .min_level = std::log::level::info,
    .pattern   = console_pattern,
});
```
- `start` starts **one thread** named `log`, which owns the sinks. A log call formats its message on the calling thread and puts the record into a **bounded channel** (see 27). The message is a `std::string`: messages up to 23 bytes are stored inline, longer ones allocate once.
- **Changes travel through the same channel:** `logs.add(...)`, `logs.remove(id)`, `logs.set_level(id, level)`, `logs.set_pattern(id, pattern)` and `logs.set_palette(id, palette)` send a command (a payload enum, see 08). A change applies exactly between the records sent before and after it. `add`, `remove` and `set_level` are `mut`: the session also keeps the run-time minimum.
- `logs.flush()` returns once everything sent before it is written and every sink is flushed.
- **Destroying the session** closes the channel. The log thread writes what's left, flushes and destroys every sink, and is joined. Log calls go to the early buffer again (section 7).
- One session exists at a time. A second `start` panics.
- **Full queue:** with `wait` (the default) the caller waits for room. With `drop`, records from `trace` to `warn` are dropped and counted, and the next record that gets through is followed by `"[std::log] 120 messages dropped"`. `error` and `fatal` always wait.

### 7. Before the session, after it, and on panics
- **Before `start`**, records go to a static buffer of 256 records, behind a spin lock. `start` sends them to the log thread first, in order; the number of records that didn't fit is logged as a warning.
- **The log thread keeps the records that arrive while it has no sink** (at most 256, the early ones included) and writes them to **each sink when it is added**. A session started and given its sinks at once therefore loses nothing.
- **Without a session**, `fatal`, failed asserts and panics print the buffered records on stderr.
- **On a panic** (any thread, any cause: a failed `run_time check`, an overflow, `expect`): the runtime first calls a hook that `start` installs. The hook logs the panic message as a `fatal` record, makes the log thread write every queued record and flush every sink, and returns; then the runtime prints the panic, so it is the last line, and the process aborts. The hook runs once (a panic inside it doesn't run it again), and a panic on the log thread itself doesn't wait for the log thread. Panics run no destructors (see 11), which is why this is a hook.
- `fatal` and a failed `assert` log their record and wait until it and everything before it is written and flushed; then they return.

### 8. Asserts and validation
```rift
namespace std::log {
    bool validate(bool condition, std::format_args failure, implicit compile_time std::call_site where);
    bool validate(bool condition, std::format_args success, std::format_args failure, implicit compile_time std::call_site where);
    bool assert(bool condition, std::format_args failure, implicit compile_time std::call_site where);
    bool assert(bool condition, std::format_args success, std::format_args failure, implicit compile_time std::call_site where);
}
```
| | `std::log::validate` | `std::log::assert` | `run_time check` (see 23) |
|---|---|---|---|
| For | Conditions that may fail at run time: bad input, a missing file | Conditions that must hold: a broken invariant, a bug | Conditions whose failure leaves nothing safe to do |
| On failure | Logs `failure` at `error` | Logs `failure` at `fatal`, written and flushed at once; stops in the debugger if one is attached (`SIGTRAP`) | Panics: the program ends |
| Returns | `condition` | `condition` | — |
| In release modes | Checked | Checked | As the mode says (see 24) |

- The condition is evaluated in every build mode. The messages follow the log levels like any log call: `success` is a `trace` record, `failure` an `error` (validate) or `fatal` (assert) record, and holes of a message that isn't logged are never evaluated (section 10.4).
- Both return the condition, so the caller handles the failure with **`else`** (see 10):
  ```rift
  bool bind(texture& t) {
      std::log::validate(t.is_valid(), f"bound texture {t.id}", f"texture {t.id} is invalid") else {
          return false;
      }
      ...
  }
  ```
- Neither ends the program, so a reset or retry can follow. A failure that must stop the program is a `run_time check`.

### 9. Compile-time checks
The log functions check their calls with entry checks (see 23), and `std::log::pattern::from` checks its text:

| Check | Kind / id |
|---|---|
| The message is an empty literal | error |
| The message ends with `\n` | warning `std::log::trailing_newline` |
| A hole's type doesn't implement `std::formattable` | error: "'T' can't be printed in an f-string" (see 19) |
| A hole changes something (section 10.4) | error E4251 |
| A pattern literal with an unknown field, a bad spec, or an unmatched brace | error at the literal |
| A `@std::log::level_override` on a function or type more verbose than `error`, in a mode based on `std::build::mode::release` | warning `std::log::verbose_override` (an annotation check, see 29) |
| A per-file glob in `build.rifts` that matches no file | build error |
| `-Dlog_level=` with something that isn't a level | build error |

The message checks read the literal text of the argument with `std::literal_text(message)` (see 23).

## 10. Language additions

### 10.1 `implicit` parameters
The contextual keyword **`implicit`** marks a parameter whose argument the **compiler supplies at each call** when the caller leaves it out.
```rift
void info(std::format_args message, implicit std::call_site where);

std::log::info(f"ready");                 // where: supplied for this call
std::log::info(f"ready", where);          // where: written (forwarding)
```
- **Implicit parameters come last**, after every ordinary parameter (error E2037 otherwise). A call may leave out any number of them from the end. Writing one passes it like any argument.
- **The type decides the value.** The parameter's type must implement `std::implicit<T>`:
  ```rift
  namespace std {
      template<type T>
      interface implicit {
          compile_time static T from_call(std::call_site where);
      }
  }
  ```
  For each call that leaves the argument out, the compiler evaluates `T::from_call(<this call's site>)` at compile time and passes the result. A type without `compile_time static T from_call(std::call_site)` can't be left out (error E4250 at the call).
- A supplied value depends **only on where the call is**. It is a compile-time value, so it works with `compile_time` parameters (10.3).
- `implicit` and `compile_time` can be combined, in either order: `implicit compile_time std::call_site where`.
- `implicit` is part of the signature and is written in the header. Lambdas have no implicit parameters (error E2036).
- `implicit` is a keyword only before a parameter's type: `void f(implicit x)` is a parameter `x` of a type named `implicit`.

**Forwarding** keeps the original caller's location:
```rift
void warn_slow(std::time::duration d, implicit compile_time std::call_site where) {
    std::log::warn(f"took {d.as_milliseconds()} ms", where);     // reported at warn_slow's caller
}
```

**Another type:**
```rift
namespace profiler {
    struct zone : std::implicit<profiler::zone> {
        std::static_str name = "";
        u32             line = 0;

        compile_time static profiler::zone from_call(std::call_site where) override {
            return profiler::zone{ .name = where.function_name, .line = where.line };
        }
    }

    void begin(implicit profiler::zone z);
}

profiler::begin();          // a zone named after the calling function
```

Interfaces may declare **`compile_time static` methods** for this (see 07). They are implemented with `override` like any interface method, and such an interface is used as a base or a template constraint.

### 10.2 `std::call_site`
`std::call_site` is **the place a call is written**. The compiler builds one for every call that leaves out an implicit argument; every field is static data it computed:
```rift
namespace std {
    struct call_site : std::implicit<std::call_site> {
        std::static_str file;             // as riftc was given it: `renderer/vulkan/pipeline.rifts`
        std::static_str file_name;        // `pipeline.rifts`
        std::static_str function;         // `gfx::vulkan::create_pipeline`
        std::static_str function_name;    // `create_pipeline`
        std::static_str namespace_name;   // `gfx::vulkan`
        std::static_str module;           // `renderer::vulkan::pipeline`
        std::static_str target;           // the build target, or the main file's name without `.rifts`
        u32             line;
        u32             column;
    }
}
```
- A copy type of static data: `std::sendable`, `std::shareable`, storable anywhere.
- Every compiler-made `std::call_site` is a compile-time value. Besides its fields it knows the function and file it describes, which compile-time code reads with (see 26):

| Call (`compile_time` only) | Result |
|---|---|
| `std::meta::function_annotation<A>(where)` | `std::option<A>`: annotation `A` on the function the call is in |
| `std::meta::type_annotation<A>(where)` | `std::option<A>`: on that function's type |
| `std::meta::file_annotation<A>(where)` | `std::option<A>`: on that file (10.5) |
| `std::build::setting_at<T>(where, name)` | `std::option<T>`: what build.rifts passed to that file, else to the target (see 25) |

### 10.3 `compile_time` parameters
A parameter marked `compile_time` takes a **compile-time value** and is a compile-time constant in the body:
```rift
void write(compile_time std::log::level lvl, std::format_args message, implicit compile_time std::call_site where) {
    if compile_time (cast<u8>(lvl) >= cast<u8>(std::log::level_at(where))) {
        ...
    }
}
```
- The argument must be a compile-time value (error E4200 at the argument otherwise).
- In the body it can be used wherever a compile-time value can: `if compile_time`, template arguments, `compile_time` constants, calls of `compile_time` functions.
- The function is **instantiated once per distinct set of compile-time arguments**, like a template with value parameters (see 14). The parameter may have any compile-time-capable type: integers, `bool`, enums, `std::static_str`, structs of those such as `std::call_site`.
- The argument is still passed at run time; inside an instance every use of the parameter is the constant.
- `compile_time` is part of the signature and is written in the header. Lambdas have no `compile_time` parameters.

This closes open question 18.

### 10.4 Lazy f-string holes
An `f"..."` passed to a function's `std::format_args` parameter is **evaluated when the function formats it**, not at the call: at most once, on the calling thread, before the function returns. If the function never formats it, its holes are never evaluated.
```rift
std::log::debug(f"tree: {tree.dump()}");   // dump() runs only if the record is formatted
```
- `std::format_args` is a view (it can't outlive the call, so everything its holes read is alive) and **move-only**: `std::println`, `std::format` and `std::panic` consume it, and passing it on is `move message`.
- In an f-string passed to a function's `std::format_args` parameter, a hole may not contain `move`, a `mut` argument, or a call of a `mut` or consuming method (error E4251): whether a hole ran never changes visible state. `std::println`, `std::format` and `std::panic` format at once, so their f-strings have no such restriction.
- A plain string literal passed as a `std::format_args` is an f-string without holes.

### 10.5 File annotations
An annotation followed by `;` at file scope **annotates the file**:
```rift
import std;

@std::log::level_override{ .level = std::log::level::trace };
```
- File annotations stand after the imports and before the first declaration (error E2038).
- Only in `.rifts` files: they describe the code written in that file (error E3073 in a header).
- The annotation must allow the target `std::meta::target::file` (see 29).
- Compile-time code reads them with `std::meta::file_annotation<A>(where)` (10.2).
- The checks of an annotation don't run for file annotations yet.

## 11. Standard library additions
| Addition | Used for |
|---|---|
| `std::thread::set_current_name(std::str)`, `std::thread::current_name() → std::string` (cached per thread; `spawn` with `.name` names the thread too) | `{thread}` |
| `std::time::wall_time` (`now()`, `local() → std::time::civil_time`) | `{time}`, `{date}` |
| `std::file::append(path&)`, `std::file::flush()`, `std::filesystem::path::parent()`, `std::filesystem::create_directories(path&)` | The file sink |
| `g.set_for_files(target, glob, name, value)` and `std::build::setting_at<T>(where, name)` (see 25) | Row 4 of the level table; any per-file configuration |
| `std::meta::function_annotation`, `type_annotation`, `file_annotation` (see 26) | `level_at` |
| `std::literal_text(param)` (see 23) | The message checks |
| `slice` and `find` on strings at compile time (see 15) | Checking patterns |

riftc loads the module `std::log` only for programs whose files mention `std::log`; other programs don't pay for checking it.

## Examples

### 1. The library side
```rift
// std/log.rifts (excerpt)
namespace std::log {
    void info(std::format_args message, implicit compile_time std::call_site where) {
        compile_time check {
            if (std::literal_text(message).ends_with("\n")) {
                std::compile_warning(std::log::trailing_newline, "the message ends with '\\n'; sinks add the line break");
            }
        }
        std::log::write(std::log::level::info, move message, where);
    }

    void write(compile_time std::log::level lvl, std::format_args message, implicit compile_time std::call_site where) {
        if compile_time (cast<u8>(lvl) >= cast<u8>(std::log::level_at(where))) {           // per call site
            if (cast<u8>(lvl) >= minimum.load(std::memory_order::relaxed)) {               // one load
                std::log::submit(lvl, move message, where);                                // formats, queues
            }
        }
    }

    compile_time std::log::level level_at(std::call_site where) {
        std::option<std::log::level_override> f = std::meta::function_annotation<std::log::level_override>(where);
        if (f.is_some()) {
            return f.expect("is_some").level;
        }
        // ... the type's and the file's annotation, then build.rifts' settings, then the mode
        return std::log::mode_default();
    }
}
```

### 2. A program's `main`
```rift
// app/main.rifts
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

### 3. A file with its own level
```rift
// renderer/vulkan/pipeline.rifts
import std;
import gfx::vulkan_device;

@std::log::level_override{ .level = std::log::level::debug };

namespace gfx::vulkan {
    std::result<pipeline, vk::result> create_pipeline(device& dev, pipeline_desc& desc) {
        std::log::debug(f"creating '{desc.name}' with {desc.stages.len()} stages");
        vk::result r = dev.create_graphics_pipeline(desc);
        if (r != vk::result::success) {
            std::log::error(f"failed to create the '{desc.name}' pipeline: {r}");
            return std::error(r);
        }
        return std::ok(pipeline::from(dev, desc));
    }

    @std::log::level_override{ .level = std::log::level::trace }
    void rebuild_swapchain(mut device& dev) {
        std::log::trace(f"swapchain extent {dev.extent()}");                 // exists: this function is at trace
        ...
    }
}
```

### 4. Checks and validation
```rift
run_time check (!error) else f"argument error: {error.message()}";            // panics; the panic is logged (section 7)

std::log::validate(texture.is_valid(), f"texture '{name}' is invalid") else {   // error record
    return false;
}

std::log::assert(index < count, f"index {index} out of {count}") else {         // fatal record, debugger stops
    index = 0;
}

if (device_lost) {
    std::log::fatal("the GPU device was lost; recreating it");                   // flushed, then the program goes on
    renderer.reset();
}
```

### 5. Compile errors
```rift
std::log::info(f"loaded {count} meshes\n");
std::log::warn("");
std::log::info(f"{mesh}");
```
```
warning[W4210]: the message ends with '\n'; sinks add the line break
 --> scene.rifts:4:5
  = note: warning id: std::log::trailing_newline
error[E4210]: std::log: the message is empty
 --> scene.rifts:5:5
error[E4171]: 'gfx::mesh' can't be printed in an f-string
 --> scene.rifts:6:22
```

## Rationale
- **Log calls are functions:** they type-check like any call, their errors fit on one screen (principle 9), and they behave like any other statement in `if`/`else`.
- **`implicit` belongs to the parameter, the value to the type:** the keyword says "the caller may leave this out", visible in the signature and the header. The type says what is supplied, so the mechanism isn't tied to one struct. Values can only depend on where the call is, so `implicit` can't hide run-time values the way default arguments would (see 04).
- **Implicit parameters last:** a call reads like the call without them, and forwarding is just writing the argument.
- **Levels at several places, most specific wins:** the build sets defaults for whole targets and groups of files; a file, type or function can state its own level where the code is. The release-mode warning keeps a temporary `trace` override from shipping unnoticed.
- **`level_at` is library code:** the order of the rules is a Rift function anyone can read, built from general tools (annotations around a call site, per-file settings).
- **Two filters:** compile-time levels decide what is in the binary and cost nothing; sink levels decide what is written and cost one load.
- **One thread owns the sinks:** sinks need no locks and can't race; turning records into lines happens off the calling thread.
- **Changes through the queue:** a new pattern or level applies in message order, and the commands are typed.
- **`fatal` doesn't end the program:** whether a failure is the end depends on the program (an engine may reset a subsystem); `fatal` only guarantees the record is on disk. Ending the program is `std::panic`, visible at the call.
- **Three ways to check a condition:** `validate` for expected failures, `assert` for bugs the program can survive, `run_time check` for those it can't. The two log functions return the condition, so the recovery is ordinary code in an `else` block (see 10) instead of a macro argument.
- **Colors belong to the sink:** a palette per sink, so a terminal and an in-game console can color the same records differently; files get none.
- **The session is a value:** destroying it flushes and joins, so shutdown happens exactly once. Panics flush through a hook in the runtime, because they run no destructors.

## Open questions
| # | Topic | Notes |
|---|---|---|
| 41 | **Structured logging** | Holes know their expression text at compile time, so a JSON sink could write `"count": 5` from `f"{count}"` |
| 42 | **Dynamic libraries** | Rift has no design for them yet. A plugin contributes sinks to the session |
| 43 | **Log rotation** | By size or by day in `std::log::file`, or left to custom sinks |
| 44 | **Implicit parameters in lambdas** | Not allowed for now |
| 45 | **Patterns read at run time** | A pattern holds a `std::static_str`; one from a config file would need an owning pattern type |
| 46 | **Checks of file annotations** | An annotation's checks run on types, functions, fields, parameters and alternatives, not yet on files |
