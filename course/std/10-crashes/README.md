# Std lesson 10: Panics and crash handling

Language lesson 4 said it plainly: a panic ends the process, with no unwinding. That makes a panic the last thing a program does, so it had better be useful: it should say **where**, **in which function**, **what** failed, **with which values**, and leave the log on disk. Edax panics do all of that, stop a debugger right on the failing line, and give your program one hook to record what it needs before the end.

**You will learn:** how to read a panic message, which failures panic and which tool to use instead, the panic hook and `std::panic_info`, combining your hook with `std::log`'s, the debugger stop and the mode setting that controls it, and thread names in reports.

**You will build:** a crash reporter for a program whose worker thread reads outside a grid (`exercise.ed`).

Examples are in [`examples/`](examples); `python3 course/check.py s10 --examples` checks them.

---

## 1. Reading a panic

```
panic_message.ed:4:33: panic in area: integer overflow in 'w * h' (u32): 70000 * 70000
```

| Part | Means |
|---|---|
| `panic_message.ed:4:33` | the exact place: file, line, column |
| `in area` | the function: `geo::polygon::area`, `lambda in main`, `~buffer` |
| `integer overflow in 'w * h' (u32)` | what failed, as source text, with the type |
| `70000 * 70000` | the values that made it fail |

Bounds failures carry their values too (`index 7 out of bounds (length 6)`), and a failed `run_time check` adds a note with the condition:

```
hook.ed:20:21: panic in main: ratio 1/0: the divisor is zero
note: the check `b != 0` is at hook.ed:11:21
```

That one is an **entry check** (language lesson 8): it's reported at the **caller's** line and in the caller's function, because the caller passed the bad argument. The note points at the check.

Examples: [`panic_message.ed`](examples/panic_message.ed), [`expect.ed`](examples/expect.ed).

## 2. What panics, and what to use instead

| Situation | Tool | On failure |
|---|---|---|
| the caller can do something about it (missing file, bad input) | return a `std::result` / `std::option` | a value the caller handles |
| bad input you want logged and skipped | `std::log::validate(c, f"...") else { ... }` | an `error` record, returns `false` |
| an invariant that should hold, but recovering is possible | `std::log::assert(c, f"...") else { ... }` | a `fatal` record, a debugger stop, returns `false` |
| "this can't fail here" | `.expect(f"why")` | a panic with your reason |
| a precondition / invariant whose failure leaves nothing safe | `run_time check (c) else f"...";` | a panic |
| arithmetic overflow, an index out of range | (automatic) | a panic with the values |
| "stop now" | `std::panic(f"...")` | a panic |

Panics are for bugs. `expect` is the honest version of C++'s `*opt` or `.value()`: it still stops, but the message says what you assumed.

## 3. The panic hook

```edax
void on_panic(std::panic_info& info) { ... }

std::option<std::fn_ptr<void(std::panic_info&)>> previous =
    std::set_panic_hook([](std::panic_info& info) { on_panic(info); });
```

`std::panic_info` is a view with:

| Field | Holds |
|---|---|
| `message` | the panic's text (for a failed check, followed by its `note:` line) |
| `where` | a `std::call_site`: `file`, `file_name`, `line`, `column`, `function`, ... |
| `function` | the function as the panic prints it |
| `thread_name` | the name of the panicking thread |

Rules:
- There is **one** hook per program. `set_panic_hook` installs yours and **returns the previous one**.
- The hook runs on the panicking thread, **before** the panic is printed, at most once per process. A panic inside the hook, or in another thread meanwhile, goes straight to the end.
- It can record (print, write a file, flush a log). It **can't** stop the process from ending.
- It takes a `std::fn_ptr`, so it captures nothing: a capture-less lambda that calls your function.

### Working with `std::log`

`std::log::start` installs its own hook, which logs the panic as a `fatal` record and flushes every sink. Installing it **replaces** a hook you set earlier. So: start the log first, then install yours, keep the one it returns, and call it from yours:

```edax
private static mut std::option<std::fn_ptr<void(std::panic_info&)>> log_hook = std::none;

void mine(std::panic_info& info) {
    ... // your report
    unsafe { if (log_hook is std::some(h)) { h(info); } }
}

unsafe { log_hook = std::set_panic_hook([](std::panic_info& info) { mine(info); }); }
```

(`static mut` needs `unsafe` at every access, language lesson 1; here only the hook and its installer touch it.)

Examples: [`hook.ed`](examples/hook.ed), [`with_log.ed`](examples/with_log.ed), [`chain.ed`](examples/chain.ed).

## 4. Under a debugger

When a debugger is attached (on Linux, `TracerPid` is read **when the panic happens**, so attaching later counts), a panic prints its message and then **stops the debugger at the line that panicked**, in the panicking function, with its locals and parameters in view: `area (w=70000, h=70000)`. Continuing ends the process with status 134, without a second stop inside `abort`.

- It happens in every built-in mode. A build mode turns it off by overriding `stop_debugger_on_panic()` to return `false` (language lesson 11); a panic then raises `SIGABRT` as without a debugger.
- Without a debugger nothing changes.
- A failed entry check stops inside the function that has the check (for indexing, the library's `operator[]`); one frame up is the caller's line, which the message names.

## 5. Threads

A panic in **any** thread ends the whole process (there's no "thread died" state to handle, so `join()` returns values directly). Name your threads, and reports say which one failed:

```edax
std::thread::set_current_name("main");
std::thread::spawn(std::thread::options{ .name = "loader" }, [move l]() { ... });
```

---

## C++ → Edax at a glance

| C++ | Edax |
|---|---|
| `std::terminate`, `abort()` | `std::panic(f"...")` |
| `std::set_terminate` | `std::set_panic_hook` |
| a signal handler for `SIGSEGV` to write a crash log | the panic hook (memory errors don't happen in safe code) |
| `assert(x)` (gone in release) | `run_time check (x) else "..."` (kept), or `run_time(std::build::mode::debug) check` |
| `opt.value()` | `opt.expect(f"why")` |
| `__builtin_trap()` + a debugger | automatic debugger stop at the failing line |
| `pthread_setname_np` | `std::thread::set_current_name`, `options{ .name }` |

---

## Your goal

`exercise.ed` reads tiles from a level on a worker thread, and one read is outside the grid. Install a panic hook that prints a short report (and writes it to `crash.txt` next to the program), and name the threads, so that stdout is:

```
tile (2, 1) is 1
crash report
  what:   index 7 out of bounds (length 6)
  where:  level::tile
  thread: loader
```

The process then ends with the panic, as it should. `check.py` compares only stdout.

<details>
<summary>Hint 1: the report</summary>

`std::str message = info.message.split('\n')[0];` drops a possible `note:` line. Build the text once with `std::format(f"crash report\n  what:   {message}\n  where:  {info.function}\n  thread: {info.thread_name}\n")` and `std::print` it.
</details>

<details>
<summary>Hint 2: the file, without panicking</summary>

`if (std::file::create(fs::executable_dir() / "crash.txt") is std::ok(f)) { mut std::file out = move f; _ = out.write(text); }`. Never `expect` in a hook: a second panic skips the rest of your hook.
</details>

<details>
<summary>Hint 3: installing it, naming the thread</summary>

`_ = std::set_panic_hook([](std::panic_info& info) { report(info); });` and `std::thread::spawn(std::thread::options{ .name = "loader" }, [move l]() { ... })`.
</details>

---

## Try this afterwards

1. Print `info.message` whole instead of its first line, and make `tile` an entry check (`run_time check (y * width + x < tiles.len()) else ...` as its first line).
2. Start a `std::log` session after installing your hook. Does your report still appear?
3. Run the solution under `gdb` (`edaxc build`, then `gdb ./build/debug/solution`, `run`). Where does it stop? Print `x` and `y`.
4. Call `std::panic("inside")` in your hook.

<details>
<summary>What happens</summary>

1. `where:  lambda in main`: an entry check is reported in the caller, here the thread's lambda. And `what:` now spans two lines, because the message carries the `note:` about the check.
2. No: `std::log::start` replaced your hook. Install yours after it and chain (section 3).
3. It stops in `level::tile` at the indexing line, after printing the panic; `p x` and `p y` show 1 and 2. `continue` ends the process.
4. The report is printed, then `panic in report: inside` ends the process: the hook's own panic goes straight to the end, so the rest of the hook (the file) never runs, and the original panic is never printed. Keep hooks panic-free.
</details>

## Recap

- A panic names the place, the function, the failed expression and its values.
- Results and `validate` for what can be handled; `expect`, `check` and panics for bugs.
- One hook, installed with `set_panic_hook`, which returns the previous one; chain with `std::log`'s.
- A debugger stops on the panicking line in every built-in mode, unless the mode says otherwise.

Next: [std lesson 11](../11-meta/README.md), reflection and annotations.
