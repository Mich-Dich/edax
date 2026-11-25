# Std lesson 8: Files and the environment

File I/O is where error handling stops being theoretical: files are missing, directories aren't writable, a path points at a folder. In Edax every I/O operation returns a `std::result` with a small `std::io_error` enum, a file is a value that closes itself, and paths are values too. The library also catches one classic bug at compile time: a relative path literal that only works when the program is started from the right directory.

**You will learn:** `std::file` (`open`, `create`, `append`, `lines`, `read_all`, `write`, `flush`), `std::io_error`, `std::filesystem::path` and `/`, `executable_dir()` and the relative-path warning, `create_directories`, `main`'s arguments, environment variables and terminal checks.

**You will build:** a settings file that is created with defaults, changed and read back (`exercise.ed`).

Examples are in [`examples/`](examples); `python3 course/check.py s8 --examples` checks them. `namespace fs = std::filesystem;` keeps names short.

---

## 1. Files

| Call | Gives |
|---|---|
| `std::file::open(path)` | `std::result<std::file, std::io_error>`: an existing file, for reading |
| `std::file::create(path)` | a new or emptied file, for writing |
| `std::file::append(path)` | a file for writing at its end, created if missing |
| `f.lines() mut` | a stream of `std::str` lines without the line break |
| `f.read_all() mut` | `std::result<std::string, std::io_error>`: the rest of the file |
| `f.write(text) mut` | `std::result<usize, std::io_error>`: the bytes written |
| `f.flush() mut` | `std::result<void, std::io_error>` |

- **A file is closed when it is destroyed** (RAII), so it can't be used after closing or forgotten open. Write a function's file handling with `?` and the file closes on every path out, early returns included.
- Every result must be used (language lesson 4). For `write`, whose byte count you rarely need, `_ = out.write(...)?;` says "check the error, drop the count".
- `path` is a `std::filesystem::path&` or any text.

```edax
std::result<void, std::io_error> write_notes(fs::path& at) {
    mut std::file out = std::file::create(at)?;
    _ = out.write("first line\n")?;
    return std::ok();
}   // out is destroyed: flushed and closed
```

Example: [`files.ed`](examples/files.ed).

## 2. Errors: `std::io_error`

`std::io_error` is a simple enum: `not_found`, `permission_denied`, `already_exists`, `is_directory`, `invalid_data`, `other`. Simple enums print their alternative's name (`f"{e}"` gives `not_found`), and you can match on the ones you handle:

```edax
match (load(p)) {
    std::ok(s) => ...,
    std::error(std::io_error::not_found) => ...,      // a case you expect: create defaults
    std::error(e) => { return std::error(e); },       // everything else goes up
}
```

That's the shape of "load or create": the *expected* failure is handled where it means something, and the rest travels up with its type intact.

**Returning a local inside `std::ok(...)` needs `move`.** Only `return local;` moves implicitly (language lesson 2); `return std::ok(local);` builds a new value from `local`, so it's `return std::ok(move local);`. The error message says so.

Example: [`errors.ed`](examples/errors.ed).

## 3. Paths, and where files are

| Call | Gives |
|---|---|
| `fs::path::from(text)` | a path (allocates) |
| `dir / "name"` | a new path with `name` appended |
| `p.parent()`, `p.file_name()`, `p.extension()`, `p.as_str()` | parts |
| `fs::executable_dir()` | the directory of the running program |
| `fs::create_directories(p)` | creates it and any missing parents |
| `fs::path::relative(text)` | a path that is relative **on purpose** |

**The relative-path warning.** `std::file::open("settings.toml")` opens the file relative to the *working directory*: whatever directory the program was started from. That's almost never what you mean, and it's the bug where a program works from the IDE and not from a shortcut. A literal relative path gets a warning:

```
warning[W4210]: "settings.toml" is relative to the working directory, not to the executable
  = note: use 'std::filesystem::executable_dir() / name', or 'std::filesystem::path::relative(name)' when the working directory is meant
  = note: warning id: std::relative_path
```

This warning isn't built into the compiler: it's an **entry check** in `std::file::open` (language lesson 8), library code you could have written yourself. Paths starting with `/`, `./` or `../` aren't warned about, and neither are run-time values (a path the user typed is the user's choice).

Example: [`relative.ed`](examples/relative.ed).

## 4. The environment

| Call | Gives |
|---|---|
| `std::exit_code main(std::span<std::str> args)` | the arguments; `args[0]` is the program |
| `std::env::variable(name)` | `std::option<std::string>` |
| `std::env::raw_args()` | the arguments as bytes, for ones that aren't UTF-8 |
| `std::env::is_terminal(std::env::stream::output)` | whether stdout is a terminal (not a file or a pipe) |
| `std::env::output_terminal_size()` | `std::option<terminal_size>`: `columns` and `rows` |

`is_terminal` decides whether colours and redrawn progress lines make sense. Under `check.py` the output is a pipe, so it's `false` there; `std::log` (next lesson) uses exactly this to decide on colours.

Example: [`environment.ed`](examples/environment.ed).

---

## C++ → Edax at a glance

| C++ | Edax |
|---|---|
| `std::ifstream in(p); if (!in) ...` | `std::file in = std::file::open(p)?;` |
| `std::getline(in, line)` loop | `for (std::str line : in.lines())` |
| `std::ofstream out(p, std::ios::app)` | `std::file::append(p)` |
| `errno` / `std::error_code` | `std::io_error` |
| `std::filesystem::path p = dir / "x";` | `fs::path p = dir / "x";` |
| `std::filesystem::create_directories` | `fs::create_directories` |
| `std::getenv("HOME")` (nullable `char*`) | `std::env::variable("HOME")` (an option) |
| `isatty(STDOUT_FILENO)` | `std::env::is_terminal(std::env::stream::output)` |
| `/proc/self/exe` tricks | `fs::executable_dir()` |

---

## Your goal

`exercise.ed` keeps two settings in a file next to the program. The first time, the file doesn't exist, so the defaults are written; then `volume` is changed, saved, and everything is read back. Fill in `load`, `save` and `load_or_create` so that it prints:

```
no settings yet: writing the defaults
2 settings
volume = 55
language = en
settings.txt: 49 bytes, 3 lines
missing.txt: not_found
```

(`edaxc run` builds the program in a fresh temporary directory, so every run starts without the file.)

<details>
<summary>Hint 1: <code>load</code></summary>

`mut std::file f = std::file::open(p)?;`, then `for (std::str line : f.lines())`: trim, skip empty lines and lines starting with `#`, and use `find("=")` and `slice` for the key and value (both trimmed, both copied into `std::string`s). End with `return std::ok(move out);`.
</details>

<details>
<summary>Hint 2: <code>save</code></summary>

`std::file::create(p)?`, then `_ = f.write(...)?;` for the comment and for each `std::format(f"{s.key} = {s.value}\n")`, then `f.flush()?;` and `return std::ok();`.
</details>

<details>
<summary>Hint 3: <code>load_or_create</code></summary>

`match (load(p))` with three arms: `std::ok(s)` returns `std::ok(move s)`; `std::error(std::io_error::not_found)` prints the message, builds the two defaults, `save(...)?`s them and returns them; `std::error(e)` returns `std::error(e)`.
</details>

---

## Try this afterwards

1. In `main`, open `"settings.txt"` instead of the path built from `executable_dir()`.
2. Make `save` forget the `?` after `f.write(...)`.
3. Call `load(fs::executable_dir())`.
4. Run the exercise twice with `edaxc build` and run the built program twice. What does the second run print first?

<details>
<summary>What happens</summary>

1. `warning[W4210]` at the literal, with the two ways to fix it.
2. `error[E4150]`: the result returned by `write` is ignored. `_ = f.write(...)` would compile, but then a full disk goes unnoticed: write the `?`.
3. Surprisingly, `std::ok` with no settings: on Linux, opening a directory succeeds, and its `lines()` stream is simply empty. `read_all()` is what reports `is_directory` (as [`errors.ed`](examples/errors.ed) shows). Code that must refuse directories should read with `read_all`, or check first.
4. The built program has a fixed directory, so the second run finds the file: no "writing the defaults" line, and `volume` is already 55.
</details>

## Recap

- I/O returns `std::result<..., std::io_error>`; `?` passes errors up; match the ones you expect.
- A `std::file` closes when destroyed.
- Paths are values; build them from `executable_dir()`, and a literal relative path warns.
- Arguments come to `main`; variables and terminal facts come from `std::env`.

Next: [std lesson 9](../09-logging/README.md), logging.
