# Logging: `std::log`

`std::log` writes messages from any thread to any number of **sinks**: the console, files, an in-game console, the network. A log call is an ordinary function call:
```edax
std::log::info(f"loaded {count} meshes in {elapsed.as_milliseconds()} ms");
```
- The call knows where it is written (file, line, function, module, build target) through an `implicit` parameter (see [Functions](../../language/functions/functions.md)).
- Whether the call exists is decided at compile time from the levels of its function, type, file and build target (below). A removed call costs nothing: no code, no string in the binary, and its f-string holes are never evaluated.
- Calls that exist are filtered again at run time by the sinks' levels (below).

## Rules

### Levels
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
| `std::log::error(f"...")` | `error` | Never dropped when the queue is full (see [Logging Output: Records, Patterns, Sinks, the Session](output.md)) |
| `std::log::fatal(f"...")` | `fatal` | Writes every queued record and flushes every sink before it returns. Never removed. It doesn't end the program |

- The message is a `std::format_args`: an `f"..."` or a plain string literal. Its holes and specs are checked at compile time (see [Strings](../text/strings.md)).
- `off` as a level removes every call except `fatal`.
- `std::log::write(lvl, message)` logs at a level given as a compile-time argument; the functions above call it.
- `fatal` is for failures the program may still handle (reset a subsystem, retry, shut down cleanly); to end the program, call `std::panic` after it.
- Recoverable errors are logged with `std::log::error(...)` and returned with `std::error(...)` (see [Error Handling](../../language/errors/error-handling.md)). Conditions are checked with `std::log::validate` and `std::log::assert` (below). A `run_time check` (see [Checks](../../language/errors/checks.md)) is the language's assert: it panics, and its panic is logged (see [Logging Output: Records, Patterns, Sinks, the Session](output.md)).

### Compile-time levels: which calls exist
Every call has a **compile-time level**, taken from the first of these that applies (the most specific wins):

| # | Where it's set | How |
|---|---|---|
| 1 | The enclosing **function** | `@std::log::level_override{ .level = ... }` on the function |
| 2 | The enclosing **type** (all its methods) | `@std::log::level_override{ .level = ... }` on the type |
| 3 | The **file**, in the file itself | `@std::log::level_override{ .level = ... };` at the top of the file (a file annotation, see [Annotations](../../language/compile-time/annotations.md)) |
| 4 | The **file**, from `build.ed` | `std::log::set_level(mut g, target, "<glob>", level)` |
| 5 | The command line | `-Dlog_level=<level>`, declared by `std::log::add_options(mut g, target)` |
| 6 | The **target**, from `build.ed` | `std::log::set_level(mut g, target, level)` |
| 7 | The **build mode** | `trace` in modes based on `std::build::mode::debug`, `error` in modes based on `std::build::mode::release` |

A call whose level is below its compile-time level is **removed**. `std::log::level_at(where)` is the `compile_time` function that computes the level; it is ordinary library code.

**In the file:**
```edax
// renderer/vulkan/pipeline.ed
import std;
import gfx::vulkan_device;

@std::log::level_override{ .level = std::log::level::trace };

namespace gfx::vulkan {
    ...
}
```

**In `build.ed`:**
```edax
compile_time void build(mut std::build::graph& g) {
    std::build::target renderer = g.add_library(std::build::edax_target{ .name = "renderer", .root = "renderer/lib.ed" });
    std::log::add_options(mut g, renderer);                                                   // -Dlog_level=<level>
    std::log::set_level(mut g, renderer, std::log::level::info);                              // the target's level
    std::log::set_level(mut g, renderer, "vulkan/pipeline*.ed", std::log::level::debug);  // some files
}
```
- The glob is relative to the directory of the target's root file. A glob that matches no file is a build error.
- The per-file form records a per-file setting (see [Edax as a Build System](../../build/build-system.md)), so changing it recompiles only the matching files.
- `-Dlog_level` must name a level; anything else is an error.

**The annotation:**
```edax
namespace std::log {
    annotation(std::meta::target::file, std::meta::target::type, std::meta::target::function)
    struct level_override {
        std::log::level level = std::log::level::trace;
    }
}
```

### Run-time filtering
- Every sink has a **minimum level** (see [Logging Output: Records, Patterns, Sinks, the Session](output.md)) that can be changed while the program runs.
- The lowest minimum of all sinks is kept in one `std::atomic<u8>`, the **run-time minimum**. A call below it returns after one relaxed load and one branch, without formatting its message (its holes aren't evaluated, see [Formatting and Printing](../text/formatting.md)).

### Asserts and validation
```edax
namespace std::log {
    @std::check_result bool validate(bool condition, std::format_args failure, implicit compile_time std::call_site where);
    @std::check_result bool validate(bool condition, std::format_args success, std::format_args failure, implicit compile_time std::call_site where);
    @std::check_result bool assert(bool condition, std::format_args failure, implicit compile_time std::call_site where);
    @std::check_result bool assert(bool condition, std::format_args success, std::format_args failure, implicit compile_time std::call_site where);
}
```
| | `std::log::validate` | `std::log::assert` | `run_time check` (see [Checks](../../language/errors/checks.md)) |
|---|---|---|---|
| For | Conditions that may fail at run time: bad input, a missing file | Conditions that must hold: a broken invariant, a bug | Conditions whose failure leaves nothing safe to do |
| On failure | Logs `failure` at `error` | Logs `failure` at `fatal`, written and flushed at once; stops in the debugger if one is attached (`SIGTRAP`) | Panics: the program ends |
| Returns | `condition` | `condition` | — |
| In release modes | Checked | Checked | As the mode says (see [Build Modes](../../build/build-modes.md)) |

- The condition is evaluated in every build mode. The messages follow the log levels like any log call: `success` is a `trace` record, `failure` an `error` (validate) or `fatal` (assert) record, and holes of a message that isn't logged are never evaluated (see [Formatting and Printing](../text/formatting.md)).
- Both return the condition and are marked `@std::check_result`, so the caller handles the failure with **`else`** (see [Control Flow](../../language/basics/control-flow.md)):
  ```edax
  bool bind(texture& t) {
      std::log::validate(t.is_valid(), f"bound texture {t.id}", f"texture {t.id} is invalid") else {
          return false;
      }
      ...
  }
  ```
- Neither ends the program, so a reset or retry can follow. A failure that must stop the program is a `run_time check`.

### Compile-time checks
The log functions check their calls with entry checks (see [Checks](../../language/errors/checks.md)), and `std::log::pattern::from` checks its text:

| Check | Kind / id |
|---|---|
| The message is an empty literal | error |
| The message ends with `\n` | warning `std::log::trailing_newline` |
| A hole's type doesn't implement `std::formattable` | error: "'T' can't be printed in an f-string" (see [Strings](../text/strings.md)) |
| A hole changes something (see [Formatting and Printing](../text/formatting.md)) | error |
| A pattern literal with an unknown field, a bad spec, or an unmatched brace | error at the literal |
| A `@std::log::level_override` on a function or type more verbose than `error`, in a mode based on `std::build::mode::release` | warning `std::log::verbose_override` (an annotation check, see [Annotations](../../language/compile-time/annotations.md)) |
| A per-file glob in `build.ed` that matches no file | build error |
| `-Dlog_level=` with something that isn't a level | build error |

The message checks read the literal text of the argument with `std::literal_text(message)` (see [Checks](../../language/errors/checks.md)).

## Examples

### The library side
```edax
// std/log.ed (excerpt)
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
        // ... the type's and the file's annotation, then build.ed' settings, then the mode
        return std::log::mode_default();
    }
}
```

### A file with its own level
```edax
// renderer/vulkan/pipeline.ed
import std;
import gfx::vulkan_device;

@std::log::level_override{ .level = std::log::level::debug };

namespace gfx::vulkan {
    std::result<pipeline, vk::result> create_pipeline(device& dev, pipeline_desc& desc) {
        std::log::debug(f"creating '{desc.name}' with {desc.stages.length()} stages");
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

### Checks and validation
```edax
run_time check (!error) else f"argument error: {error.message()}";            // panics; the panic is logged (see the logging output page)

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

### Compile errors
```edax
std::log::info(f"loaded {count} meshes\n");
std::log::warn("");
std::log::info(f"{mesh}");
```
```
warning: the message ends with '\n'; sinks add the line break
 --> scene.ed:4:5
  = note: warning id: std::log::trailing_newline
error: std::log: the message is empty
 --> scene.ed:5:5
error: 'gfx::mesh' can't be printed in an f-string
 --> scene.ed:6:22
```

## Rationale
- **Log calls are functions:** they type-check like any call, their errors fit on one screen (principle 9), and they behave like any other statement in `if`/`else`.
- **Levels at several places, most specific wins:** the build sets defaults for whole targets and groups of files; a file, type or function can state its own level where the code is. The release-mode warning keeps a temporary `trace` override from shipping unnoticed.
- **`level_at` is library code:** the order of the rules is a Edax function anyone can read, built from general tools (annotations around a call site, per-file settings).
- **Two filters:** compile-time levels decide what is in the binary and cost nothing; sink levels decide what is written and cost one load.
- **`fatal` doesn't end the program:** whether a failure is the end depends on the program (an engine may reset a subsystem); `fatal` only guarantees the record is on disk. Ending the program is `std::panic`, visible at the call.
- **Three ways to check a condition:** `validate` for expected failures, `assert` for bugs the program can survive, `run_time check` for those it can't. The two log functions return the condition, so the recovery is ordinary code in an `else` block (see [Control Flow](../../language/basics/control-flow.md)) instead of a macro argument.
