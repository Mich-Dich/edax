# Error Handling

## Rules

### Recoverable errors: `std::result` and `std::option`
- `std::result<T, E>` is used for operations that can fail. Its alternatives are `std::ok(x)` and `std::error(e)`.
- `std::option<T>` is used for values that may be absent. Its alternatives are `std::some(x)` and `std::none`.
- No exceptions: there is no `throw`, `try` or `catch`. Foreign code that may throw must be wrapped in `unsafe` (see [Raw Pointers and `unsafe`](../memory/raw-pointers-and-unsafe.md)).
- **Results must be used.** Ignoring a returned `std::result` is a compile error. To discard one on purpose, write `_ = f();`.

### Consuming a result
| Form | Effect |
|---|---|
| `match (r) { ... }` | Handle each case |
| `r?` | Unwrap `ok`, or `return` the error from the current function |
| `r.or_error(e2)` | Replace the error with `e2`; returns `result<T, E2>` |
| `r.map_error(f)` | Convert the error with a function or lambda |
| `r.value_or(x)` | `ok` value, or `x` |
| `r.expect(f"...")` | `ok` value, or **panic** with the message |
| `r.is_ok()`, `r.is_error()` (`o.is_some()`, `o.is_none()` on an option) | `bool`; looks without taking the value |

`?` requires the **error types to be identical**. There is no hidden conversion; convert with `.or_error` or `.map_error`:
```edax
std::result<employee, parse_error> parse_line(std::str line) {
    std::darray<std::str> parts = line.split(',');
    if (parts.length() != 3) {
        return std::error(parse_error::bad_line);
    }
    u32 age    = parts[1].parse<u32>().or_error(parse_error::bad_number)?;
    f64 salary = parts[2].parse<f64>().or_error(parse_error::bad_number)?;
    return std::ok(employee{
        .name   = std::string::from(parts[0]),
        .age    = age,
        .salary = salary,
    });
}
```
`?` on a `std::option` works the same way, but only inside functions that return `std::option`.

### Unrecoverable errors: panic
A **panic** prints a message and the source location to stderr, then **aborts** the process. There is no unwinding and destructors don't run. A panic in **any** thread aborts the whole process, so joining a thread never fails (see [Threads](../../stdlib/concurrency/threads.md)).

#### What a panic prints
```
geometry/area.ed:3:13: panic in geo::area: integer overflow in 'w * h' (u32): 70000 * 70000
```
- The location, then the **function** the panic happened in (`geo::polygon::area`, `std::darray<u32>::push`, `lambda in main`, `~texture` for a destructor).
- Checks the compiler makes add **what failed and the values involved**:

| Check | Message |
|---|---|
| Overflow of `+ - *` | `integer overflow in 'high - low' (i16): 32767 - -32768` |
| Overflow of `/`, unary `-`, `abs` | `... (i32): -2147483648 / -1`, `-(-2147483648)`, `abs(-2147483648)` |
| Division by zero | `division by zero in 'total / count'` |
| Shift count | `shift count out of range for u32 in '1u32 << s': 32 (must be below 32)` |
| `bit(n)`, a SIMD lane index, `clamp` | the index, or `lo` and `hi` |
| Index of `std::darray`, `std::array`, `std::span`, `std::bitset`, slices | `index 9 out of bounds (length 8)` |

- A failed `run_time check` prints its message, and a note quoting the condition: ``note: the check `x % 2 == 0` is at file:line:col`` for an entry check (whose panic names the caller's line), ``note: the check `...` failed`` otherwise. The standard library's own checks leave the note out.
- The expression is the source text, on one line, cut at 48 characters. The values are added when the check fails: the check itself costs what it cost before, and the failure path is one call of the runtime, so it doesn't change what the optimizer inlines or vectorizes.

#### Under a debugger
When a debugger is attached (on Linux: `TracerPid` in `/proc/self/status`, read when the panic happens, so attaching later counts), a panic prints its message and then **stops the debugger at the line that panicked**, in the function that panicked, with its locals and parameters in view. Continuing ends the process with abort's exit status (134), without a second stop inside `abort`.
- It happens in every built-in mode. A mode turns it off with `stop_debugger_on_panic()` returning `false` (see [Build Modes](../../build/build-modes.md)); a panic then raises `SIGABRT` as without a debugger.
- Without a debugger nothing changes: the check of `TracerPid` happens only after a panic.
- A failed **entry check** stops inside the function that has the check (for indexing: the library's `operator[]`); the frame above it is the caller's line, which the message names.
- With `panic()` returning `trap`, a panic is a trap instruction at the line that panicked, with no message.

These things panic:

| Trigger | See |
|---|---|
| `std::panic(f"...")` | - |
| A failed `run_time check` (or the run-time half of `compile_time run_time check`) | [Checks](checks.md) |
| Integer overflow, division by zero | [Operators](../basics/operators.md) |
| Shift count ≥ bit width, `bit(n)` out of range at run time | [Operators](../basics/operators.md) |
| Index out of bounds | [Sequences: `std::darray`, `std::array`, `std::span`, `std::bitset`](../../stdlib/containers/sequences.md) |
| `.expect(...)` on `error`/`none` | above |
| `main` returning an integer the OS can't represent | [Functions](../functions/functions.md) |

`std::panic` has return type `never`.

### The panic hook
```edax
namespace std {
    struct panic_info {
        std::str        message;
        std::call_site  where;          // where the panic happened (see Functions)
        std::static_str function;       // the function it happened in, as a panic prints it ("" if unknown)
        std::str        thread_name;
    }
    std::option<std::fn_ptr<void(std::panic_info&)>> set_panic_hook(std::fn_ptr<void(std::panic_info&)> hook);
}
```
- A program has at most one hook. `set_panic_hook` installs it and returns the previous one.
- On a panic, the runtime calls the hook on the panicking thread, then prints the panic and ends the process the way the build mode says (abort, or a CPU trap, see [Build Modes](../../build/build-modes.md)).
- The hook can record or flush (the logger writes its queue, see [Logging: `std::log`](../../stdlib/logging/logging.md)). It can't stop the process from ending.
- The hook runs at most once per process: a panic inside the hook, or in another thread while it runs, goes straight to the end.

## Rationale
- **No implicit error conversion:** a hidden conversion trait would decide which error the caller gets. `.or_error(parse_error::bad_number)` is short, and it shows exactly that.
- **Must-use results:** an ignored error is a silent bug. `_ =` makes ignoring it a visible decision.
- **Abort, not unwind:** there are no exceptions, so unwinding would only exist for panics. Abort keeps codegen simple and predictable. A build mode can replace the message + abort with a CPU trap (`panic()` returning `trap`, see [Build Modes](../../build/build-modes.md)), for example on embedded targets.
- **A hook, not a handler:** after a panic the program's state can't be trusted, so the hook can only record what happened; ending the process stays guaranteed.
