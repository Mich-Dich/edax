# 31 - Logging (`std::log`)

> **Status:** proposal (2026-11-19). Nothing here is implemented yet. It needs three language additions (section 9) and a few small std additions (section 10).
> **Source:** the GLT engine logger (C++, plugin `logger_default`), reviewed in the appendix.

`std::log` writes messages from any thread to any number of **sinks** (console, files, an in-game console, the network). A log call is an ordinary function call:
```edax
std::log::info(f"loaded {count} meshes in {elapsed.as_milliseconds()} ms");
```
There are no macros. Everything the C++ macros did is done by the language:

| The C++ macro did | Edax does it with |
|---|---|
| Capture file, line, function (`std::source_location::current()`) | A **`std::call_site` parameter**, filled in by the compiler (section 9.1) |
| Capture the module name (`GLT_MODULE_NAME`) | The call site knows its file, module, namespace and **build target** |
| Remove calls below the level at compile time (`LOG_LEVEL_ENABLED`) | A **`compile_time` parameter** and `if compile_time` (section 9.2) |
| Not evaluate the arguments of a removed call | **Lazy f-string holes** (section 9.3) |
| Check the format string | f-strings are already checked at compile time (see 19) |

## Rules

### 1. Levels
```edax
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
| `std::log::fatal(f"...")` | `fatal` | Returns **`never`**: logs, flushes every sink, then panics with the same message. Never compiled out |

- The message is a `std::format_args` (an `f"..."` or a plain literal). There is no separate argument list, so a wrong number of arguments can't be written.
- `fatal` replaces the C++ `logged_exception` and the "log fatal, then break/throw" part of `ASSERT`. Recoverable errors are `std::log::error(...)` plus `return std::error(...)` (see 11).

### 2. Which calls exist: compile-time levels
Every call has a **compile-time level**, decided for the file (and function) it is written in. A call below that level is **removed completely**: no code, no string in the binary, and its f-string holes are never evaluated.

The level is the first of these that applies (most specific wins):

| # | Source | Example |
|---|---|---|
| 1 | Annotation on the enclosing **function** | `@std::log::level_override{ .level = std::log::level::trace }` |
| 2 | Annotation on the enclosing **type** (applies to its methods) | the same, on a class |
| 3 | **Per-file** setting in `build.ed` (glob, relative to the target root) | `std::log::set_level(mut g, renderer, "vulkan/*.ed", std::log::level::trace);` |
| 4 | **Per-target** setting in `build.ed` | `std::log::set_level(mut g, renderer, std::log::level::info);` |
| 5 | **Mode default** | `trace` for modes based on `std::build::mode::debug`, `error` for modes based on `release` |

```edax
// build.ed
compile_time void build(mut std::build::graph& g) {
    std::build::target renderer = g.add_library(std::build::edax_target{ .name = "renderer", .root = "renderer" });
    std::log::set_level(mut g, renderer, std::log::level::info);                              // global default for the target
    std::log::set_level(mut g, renderer, "vulkan/pipeline*.ed", std::log::level::trace);  // override for some files
}
```
- `std::log::set_level` are `compile_time` functions of the standard library. They record **per-file settings** (section 10), so changing one file's level recompiles only that file.
- A file glob that matches no file of the target is a build error (`std::log: "vulkan/pipline*.ed" matches no file of target 'renderer'`).
- `std::log::add_options(mut g, target)` declares the command-line option `-Dlog_level=<level>`, which replaces row 4 for that build.

### 3. Run-time filtering
Calls that exist can still be filtered while the program runs:

- Each sink has a **minimum level** (section 5), which can be changed at run time.
- Before formatting, a call checks the **run-time minimum**: the lowest minimum of all sinks, kept in one `std::atomic<u8>`. That's one relaxed load and one branch. Below it, the message is not formatted (its holes are not evaluated, section 9.3).

### 4. Records and patterns
A sink receives a **record**:
```edax
namespace std::log {
    view struct record {
        std::log::level       level;
        std::call_site        where;        // file, line, function, module, target (static data, see 9.1)
        u64                   thread_id;
        std::str              thread_name;  // the name given at spawn, or set_current_name (see 10)
        std::time::wall_time  time;         // when the call was made, on the calling thread
        std::str              message;      // the formatted message, without a trailing newline
    }
}
```

A **pattern** turns a record into a line. It uses the f-string syntax of 19, with record fields as the names:
```edax
private compile_time std::log::pattern console_pattern = std::log::pattern::from(
    "{color}[{time}.{millis}] [{target} {level:<5} {thread}]{/color} {file_name}:{line} {message}");
```
| Field | Value |
|---|---|
| `{message}` | The formatted message |
| `{level}` | `TRACE` ... `FATAL` (use `{level:<5}` to align) |
| `{time}`, `{date}`, `{millis}`, `{micros}` | `hh:mm:ss`, `yyyy-mm-dd`, `000`–`999`, `000000`–`999999` |
| `{thread}`, `{thread_id}` | The thread's name (its id if it has none), the id |
| `{target}`, `{module}`, `{namespace}` | Build target (`renderer`), module (`renderer::vulkan::pipeline`), namespace |
| `{file}`, `{file_name}` | Path relative to the project root, file name only |
| `{function}`, `{function_name}` | `gfx::pipeline::create`, `create` |
| `{line}`, `{column}` | Numbers |
| `{color}`, `{/color}` | Start/end of the level's color. Sinks without color (files) drop them |

- **`pattern::from(std::static_str)`** has a `compile_time check` entry (see 23): the text is parsed when the program is compiled, and an unknown field or a bad spec is an error **at the pattern literal**:
  ```
  error: std::log::pattern: unknown field 'mesage' (fields: message, level, time, ...)
   --> app/main.ed:12:62
  ```
- **`pattern::parse(std::str)`** returns `std::result<std::log::pattern, std::log::pattern_error>`, for patterns read from a config file at run time.
- A pattern is a fixed array of at most 32 segments, so it needs no heap and can be a `compile_time` constant. Rendering a record walks the segments; the pattern text is never parsed again.
- Sinks add the line break; a pattern doesn't end with one.

### 5. Sinks
A sink is anything that implements the interface:
```edax
namespace std::log {
    interface sink {
        void write(std::log::record& r) mut;
        void flush() mut;
    }
}
```
- Sinks are **owned by the log thread** (section 6). They're moved there when they're added, so they must be `std::sendable` (see 27) but never need to be shareable or locked. A sink that isn't sendable is a compile error naming the field (example 5 of 27).
- Every sink is added with **sink options**, which the log thread applies before calling `write`:
  ```edax
  struct sink_options {
      std::log::level   min_level = std::log::level::trace;
      std::log::pattern pattern   = std::log::default_pattern;
      std::log::filter  filter    = std::log::filter::all();     // e.g. filter::targets("renderer")
  }
  ```
  With filters, sinks are **channels**: `renderer.log` gets only the `renderer` target, `errors.log` gets everything from `warn` up, the console gets `info` and up.

The standard library provides:

| Sink | Writes to | Notes |
|---|---|---|
| `std::log::console` | stdout, `error`/`fatal` to stderr | Colors when the stream is a terminal (`.colors = auto / always / never`) |
| `std::log::file` | A file | Keeps the file open; buffers up to `.buffer_size` bytes; flushes at `.flush_at` level and above, when the queue runs empty, and on `flush()`. `.mode = truncate / append`. Writes a "log started / stopped" line with the date |
| `std::log::memory` | A ring of the last *n* records' lines | For an in-game console: `snapshot()` from any thread (the ring is a `std::shared_ptr<std::mutex<...>>` inside) |

Custom sinks (network, a GUI, the engine's own plugin) implement `std::log::sink`.

### 6. The log session
```edax
std::log::session logs = std::log::start(std::log::options{
    .queue_capacity = 16 * 1024,                     // records
    .when_full      = std::log::when_full::wait,     // or drop
});
std::log::sink_id console = logs.add(std::log::console::make(), std::log::sink_options{
    .min_level = std::log::level::info,
    .pattern   = console_pattern,
});
```
- `start` starts **one thread** named `log` that owns the sinks. Log calls on other threads only format the message and put it into a **bounded channel** (see 27): one atomic operation, no lock, no allocation for messages up to 200 bytes.
- **Settings travel through the same channel**: `logs.add(...)`, `logs.remove(id)`, `logs.set_level(id, level)`, `logs.set_pattern(id, pattern)` send a command, so a change applies exactly between the messages sent before and after it. The commands are a payload enum (see 08), not numbers.
- `logs.flush()` waits until everything sent before it is written and every sink is flushed.
- **Destroying the session** closes the channel; the log thread writes what is left, flushes every sink, destroys them, and is joined. Exactly once, on the thread that owns the session.
- Only one session can exist at a time. A second `start` panics with both locations.
- **Full queue:** `wait` (the default) blocks the caller until there is room. `drop` drops `trace` to `warn` records and counts them; the log thread writes `"[std::log] 120 messages dropped"` as soon as there is room. `error` and `fatal` always wait.

### 7. Before `start`, after the session, and on panics
- **Before `start`**, records go to a static buffer of 256 records (no heap; a `static` of synchronized type, see 27). `start` writes them into the sinks first, in order. If more arrive, the newest are counted and dropped, and the count is logged.
- **Without a session** (before the first or after the last), `fatal` and panics write the buffered records and their own message to stderr.
- **On a panic** (any thread, any cause: a failed `run_time check`, an overflow, `expect`, `fatal`): the panic message is logged as a `fatal` record, the log thread writes every queued record, flushes every sink, and then the process aborts. This replaces the GLT crash handler's `subscribe(logger::shutdown)`. It's a hook inside the standard library (open question 5 is about a user-defined panic handler).
- Panics during this flush are ignored, so a broken sink can't hide the original panic.

### 8. Checks the library makes
Because log calls are functions, the library can check them with ordinary entry checks (see 23). These are part of the proposal:

| Check | When | Kind / id |
|---|---|---|
| The message is an empty literal | compile time | error |
| The message ends with `\n` (sinks add the line break) | compile time | warning `std::log::trailing_newline` |
| A hole has a type without `std::formattable` | compile time | error with the custom message of 23, example 4 |
| `@std::log::level_override` is more verbose than the target's level, in a mode based on `release` | compile time | warning `std::log::verbose_override` (so a forgotten `trace` override doesn't ship) |
| A log call inside a `compile_time` function | compile time | error: "logging happens at run time; use std::compile_note" |
| A pattern literal with an unknown field or a bad spec | compile time | error at the literal |
| A per-file level glob that matches no file | build | error |
| `std::log::start` while a session exists | run time | panic, both locations |

## 9. Language additions this needs
Each of these is general. Logging is the first user, not the only one.

### 9.1 `std::call_site`
`std::call_site` is a standard library type for **the place a function was called from**. Like `std::static_str`, it points to static memory: it is a copy type, `std::sendable` and `std::shareable`, and it can be stored anywhere.

- **A parameter of type `std::call_site` is filled in by the compiler** with the location of the call. It must be the first parameter. The caller doesn't write it:
  ```edax
  void info(std::call_site where, std::format_args message);
  std::log::info(f"ready");                 // where = this line
  ```
- **Forwarding:** if the first argument of a call already has type `std::call_site`, it is passed as it is. That lets wrappers report *their* caller:
  ```edax
  void warn_slow(compile_time std::call_site where, std::time::duration d) {
      std::log::warn(where, f"took {d.as_milliseconds()} ms");     // reported at warn_slow's caller
  }
  ```
  `where` is `compile_time` here because `std::log::warn` needs a compile-time call site (9.2); a wrapper that only stores or prints the location can take a plain `std::call_site`.
- Methods, all returning static data: `file()`, `file_name()`, `line()`, `column()`, `function()`, `function_name()`, `module()`, `namespace_name()`, `target()`.
- Every `std::call_site` is a **compile-time value**, so it can be used with `compile_time` parameters (9.2) and with `std::meta::enclosing_function(where)` / `enclosing_type(where)`, which return the reflection handles of 26 (for reading annotations).
- **Cost:** one pointer argument, or nothing after inlining. The file name, function name etc. are computed by the compiler once per call site. The C++ logger allocated three strings per message for this (appendix, finding 3).
- Other uses: assertion helpers, profiler scopes, error values that record where they were made (`struct parse_failure { std::call_site where; ... }`), `expect`-like helpers in user code. `std::source_location` (spec 23, used by `std::location_of`) becomes `std::call_site`.

### 9.2 `compile_time` parameters (closes open question 18)
A parameter marked `compile_time` takes a **compile-time value** and is a compile-time constant in the body:
```edax
void write(compile_time std::call_site where, compile_time std::log::level lvl, std::format_args message) {
    if compile_time (lvl >= std::log::level_at(where)) {
        ...
    }
}
```
- The argument must be a compile-time value (the rule of 23, error E4200 at the argument otherwise).
- In the body it can be used wherever a compile-time value can: `if compile_time`, template arguments, `compile_time check`, calls of `compile_time` functions.
- The function is **instantiated once per distinct set of compile-time arguments**, like a template with value parameters (see 14). Value parameters only allow integers and `bool`; a `compile_time` parameter may have any compile-time-capable type (enums, `std::static_str`, `std::call_site`, structs of those).
- The same restrictions as for `compile_time` entry checks (see 23): its address can't be taken, and it can't be virtual.
- `compile_time` is part of the signature and is written in the header.

Open question 18 said a parameter modifier is needed only when the body must use the value at compile time; deciding whether a log call exists is that case.

### 9.3 Lazy f-string holes
An `f"..."` passed as a `std::format_args` is **not evaluated at the call**. Its holes are evaluated **when the callee formats it**: at most once, on the calling thread, before the callee returns. If the callee never formats it, the holes are never evaluated.
```edax
std::log::debug(f"tree: {tree.dump()}");   // dump() only runs if the debug record is formatted
```
- `std::format_args` stays a view (it can't escape the call, so everything the holes read is still alive) and becomes **move-only**: formatting consumes it, so it can't be formatted twice.
- A hole may not contain `move`, a `mut` argument, or a call of a `mut` method. Holes can't change visible state, so it never matters whether they ran. Error: "a hole is only evaluated if the message is formatted, so it can't change 'x'".
- For `std::println`, `std::format` and `std::panic` nothing changes: they always format exactly once.
- Without this rule, `if compile_time` in the callee could remove the *call's body* but not the evaluation of its arguments.

## 10. Small standard library additions
| Addition | For |
|---|---|
| `std::thread::set_current_name(std::str)`, `std::thread::current_name() → std::str` (stored in a 16-byte `thread_local`; `spawn` with `.name` sets it) | `{thread}`. Replaces `register_label_for_thread` |
| `std::time::wall_time` (`now()`, local `date()` / `time_of_day()` with milliseconds) | `{time}`, `{date}` |
| Per-file settings in `build.ed`: `g.set_for_files(target, glob, name, value)`, read with `std::build::setting_at<T>(std::call_site, name) → std::option<T>` (see 25) | Per-file levels; usable for any per-file configuration |
| `std::meta::enclosing_function(std::call_site)`, `enclosing_type(std::call_site)` | Reading `@std::log::level_override` (needs annotations, 29) |

## Examples

### 1. The library side
```edax
// std/log.ed (excerpt)
namespace std::log {
    @std::inline_always
    void info(compile_time std::call_site where, std::format_args message) {
        std::log::write(where, std::log::level::info, move message);
    }

    @std::inline_always
    void write(compile_time std::call_site where, compile_time std::log::level lvl, std::format_args message) {
        if compile_time (lvl >= std::log::level_at(where)) {                     // decided per call site
            if (cast<u8>(lvl) >= run_time_minimum.load(std::memory_order::relaxed)) {
                std::log::submit(where, lvl, move message);                      // formats, enqueues
            }
        }
    }

    compile_time std::log::level level_at(std::call_site where) {
        std::option<std::meta::function> f = std::meta::enclosing_function(where);
        if (f.is_some() && f.expect("is_some").has<std::log::level_override>()) {
            return f.expect("is_some").annotation<std::log::level_override>().expect("has").level;
        }
        // ... the enclosing type, then std::build::setting_at<std::log::level>(where, "std::log::level"),
        //     then the mode default
    }

    @std::cold
    never fatal(std::call_site where, std::format_args message) {
        std::string text = std::format(move message);           // formatted once; allocating is fine here
        std::log::submit_text(where, std::log::level::fatal, text);
        std::log::flush_all_for_panic();                         // the queue and every sink (section 7)
        std::panic(f"{text}");
    }
}
```

### 2. The engine's `main` (the C++ `entry_point.cpp`)
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
    std::log::info(f"starting with {args.len()} arguments");                 // buffered until start()

    std::log::session logs = std::log::start(std::log::options{ .queue_capacity = 16 * 1024 });
    _ = logs.add(std::log::console::make(), std::log::sink_options{
        .min_level = std::log::level::info,
        .pattern   = console_pattern,
    });
    std::log::file main_log = std::log::file::create(std::log::file_options{
        .path     = std::filesystem::executable_dir() / "logs" / "gluttony.log",
        .flush_at = std::log::level::warn,                                   // the C++ buffer threshold
    }).expect("can't open the log file");
    _ = logs.add(std::unique_ptr<std::log::file>::make(move main_log), std::log::sink_options{ .pattern = file_pattern });

    mut engine::application app = engine::application::make(args);
    app.run();
    return std::exit_code::success;
}   // app, then logs: every record written, every sink flushed, the log thread joined
```
Compared with the C++ version: no `install_logger_functions`, no `register_label_for_thread`, no crash-handler subscription, no `shutdown()` call to forget.

### 3. A module that logs
```edax
// renderer/vulkan/pipeline.ed
namespace gfx::vulkan {
    std::result<pipeline, vk::result> create_pipeline(device& dev, pipeline_desc& desc) {
        std::log::trace(f"creating '{desc.name}' with {desc.stages.len()} stages");   // gone unless this file's level is trace
        vk::result r = dev.create_graphics_pipeline(desc);
        if (r != vk::result::success) {
            std::log::error(f"failed to create the '{desc.name}' pipeline: {r}");
            return std::error(r);
        }
        return std::ok(pipeline::from(dev, desc));
    }

    @std::log::level_override{ .level = std::log::level::trace }               // while debugging this one function
    void rebuild_swapchain(mut device& dev) { ... }
}
```

### 4. What the C++ assert/validate macros become
```edax
run_time check (!error) else f"argument error: {error.message()}";            // ASSERT: panics, logged as fatal (section 7)

if (!texture.is_valid()) {                                                    // VALIDATE(expr, return false, ...)
    std::log::error(f"texture '{name}' is invalid");
    return false;                                                             // the control flow is visible
}
```

### 5. Errors at compile time
```edax
std::log::info(f"loaded {count} meshes\n");
std::log::info(f"loaded {cuont} meshes");
std::log::info(f"{mesh}");
```
```
warning[std::log::trailing_newline]: the message ends with '\n'; sinks add the line break
 --> scene.ed:4:41
error: unknown name 'cuont'
 --> scene.ed:5:29
error: std::log: 'gfx::mesh' can't be logged
 --> scene.ed:6:22
note: implement std::formattable for it (or use @std::derive::formattable), or log one of its fields
```

## Rationale
- **No macros, three general features:** each C++ macro trick (location capture, compile-out, skipping argument evaluation) becomes a feature with one job that other code can use too. A log call is a function call, so it type-checks, has one-screen errors (principle 9), and can't break an `if`/`else`.
- **Levels in `build.ed`, not `#define`s:** a per-file level in C++ only works if it's defined before the header is first included; a precompiled header that includes `logger.h` silently ignores it (appendix, finding 10). In Edax the level is a setting the compiler reads per call site, so it can't be shadowed by include order, and a typo in a glob is an error.
- **Two filters:** compile-time levels decide what exists in the binary (free), sink levels decide what is written (one load). That is the split the C++ logger had between `LOG_LEVEL_ENABLED` and the buffer threshold, made explicit.
- **One log thread owning the sinks:** sinks don't need locks, can't race (they're only sendable, never shared), and formatting into a line happens off the hot thread. The calling thread only formats the message.
- **Settings through the queue:** keeps the good idea of the C++ logger (a format change applies in message order) and makes it type-safe with a payload enum.
- **`fatal` returns `never`:** a fatal log that the program continues after is a contradiction. With `never`, the compiler knows the branch ends.
- **The session is a value:** destruction flushes, so shutdown can't be forgotten or done twice. Panics flush through a std hook, because panics don't run destructors (see 11).

## Open questions
| # | Topic | Notes |
|---|---|---|
| 41 | **Per-file level in the source** | Levels can be set per file in `build.ed` and per function/type with an annotation. A file-level annotation would need a new annotation target (29) |
| 42 | **Structured logging** | Holes know their expression text at compile time, so a JSON sink could write `"count": 5` from `f"{count}"` with no extra syntax |
| 43 | **Plugins / dynamic libraries** | Edax has no dynamic-library design yet. With `std::log` in std, an engine plugin contributes sinks instead of replacing the logger |
| 44 | **Log rotation** | `std::log::file` with size/day rotation, or leave it to custom sinks |

---

## Appendix: the GLT C++ logger

### What maps to what
| C++ (`GLT::logger`, `logger_default`) | `std::log` |
|---|---|
| `LOG(info, "x {}", a)`, `LOG_Master` | `std::log::info(f"x {a}")` |
| `GLT_MODULE_NAME` | `{target}` / `{module}` from `std::call_site` |
| `owned_source_location` | `std::call_site` (static, no allocation) |
| `LOG_LEVEL_ENABLED` per translation unit | Per-file / per-target levels in `build.ed`, `@std::log::level_override` |
| `logger_functions` table, `install_logger_functions`, `use_default_logger_functions` | Sinks added to a session |
| Default implementation's `s_log_buffer` + `drain_log_buffer` | The static early buffer (section 7) |
| `CMD_UPDATE_FORMAT` ... `CMD_UNREGISTER_THREAD_LABEL` (int + reused fields) | Command payload enum |
| `$B$T:$J$E...` format, parsed per message | `std::log::pattern`, parsed once, checked at compile time |
| `set_buffer_threshold`, `set_buffer_size`, `flush_buffer` | File sink `.flush_at`, `.buffer_size`, `logs.flush()` |
| `register_label_for_thread` | Thread names (`.name` at spawn, `set_current_name`) |
| `logged_exception`, `LOGGED_EXCEPTION` | `std::log::fatal` (`never`), or `error` + `std::error(...)` |
| `ASSERT`, `ASSERT_S` | `run_time check` (the panic is logged) |
| `VALIDATE` | `if` + `std::log::error` + the command, written out |
| `crash_handler::subscribe(logger::shutdown)` | Panic hook in std |

### Findings in the C++ code
1. **Bug: `set_buffer_size` ignores its argument.** The command handler sets `s_buffer_size = message.location.line()`, and the location is `std::source_location::current()` inside `set_buffer_size` (the real value is commented out), so the buffer size becomes that line number (~297).
2. **`message_data` has `const` members**, so `std::move(queue.front())` copies every string: each message is copied at least twice (queue → local queue → processing).
3. **Three allocations per message for the location and module** (`owned_source_location` copies file and function names, plus `module_name`), and `get_filename` runs `strrchr` per message.
4. **Data races:** `s_use_buffer` is a plain `bool` written by `drain_log_buffer` before it takes the lock and read by every logging thread; `s_severity_level_buffering_threshold` is read under `s_queue_mutex` but written under `s_general_mutex`; `get_format()` reads `s_format_current` without a lock; `g_logger` is replaced while threads may log (the comment says "safe as long as called before any logging threads run", which `on_unload` doesn't guarantee).
5. **The log file is opened and closed for every write** (`WRITE_TO_FILE`, `OPEN_FILE` in `process_log_message`), and a failed open calls `std::quick_exit(1)`.
6. **Formatting and console/file output happen while `s_general_mutex` is held**, and the format string is re-parsed character by character for every message.
7. **Color codes end up in the log file:** `$B`/`$E` are applied to the one formatted string that goes to both console and file.
8. **`unregister_label_for_thread` builds a warning that is never written** (the command handler ignores the message).
9. **Macro hazards:** `LOG_Master` expands to `{ ... }`, so `if (x) LOG(info, "a"); else ...` doesn't compile; `ASSERT_3`'s `if (expr) LOG(...) else {...}` is a dangling-else trap when used inside another `if`; the Windows and Linux `_ASSERT_PICK` lists differ (`ASSERT_BAD` once vs twice); on Linux an `ASSERT` logs the failure twice (the `fatal` log and again in `logged_exception`).
10. **Per-file `LOG_LEVEL_ENABLED` depends on include order:** the `#ifndef` lives in `logger.h`, which `pch.h` likely includes first, so a `#define LOG_LEVEL_ENABLED` after `#include "util/pch.h"` is ignored (and redefinition warnings only appear if the value differs).
11. **Commands share the queue with messages but not the type:** `set_buffer_threshold` smuggles its value in `msg_sev`, `set_buffer_size` in the line number (finding 1).
12. **`process_queue` polls every 100 ms** (`wait_for`) even when idle.
