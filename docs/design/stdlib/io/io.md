# Files, Filesystem and the Environment

## Rules

### Console
`std::print`, `std::println`, `std::eprint` and `std::eprintln` write to stdout and stderr (see [Formatting and Printing](../text/formatting.md)).

### Files: `std::file`
| Call | Result |
|---|---|
| `std::file::open(path)` | `std::result<std::file, std::io_error>`: an existing file, for reading |
| `std::file::create(path)` | A new or emptied file, for writing |
| `std::file::append(path)` | A file for writing at its end (created if missing) |
| `f.lines() mut` | A stream of `std::str` lines without the line break (see [Control Flow](../../language/basics/control-flow.md), `for`) |
| `f.read_all() mut → std::result<std::string, std::io_error>` | The rest of the file |
| `f.write(text) mut → std::result<void, std::io_error>` | Writes the text |
| `f.flush() mut → std::result<void, std::io_error>` | Hands buffered data to the OS |

- `path` is a `std::filesystem::path&` or a `std::str`.
- A `std::file` is closed when it is destroyed.
- `std::io_error` is a simple enum: `not_found`, `permission_denied`, `already_exists`, `is_directory`, `invalid_data`, `other`. Simple enums print as the name of their alternative, so `f"{e}"` gives `not_found`.

### Paths: `std::filesystem`
| Call | Result |
|---|---|
| `std::filesystem::path::from(text)` | A path (allocates) |
| `std::filesystem::path::relative(text)` | A path marked as intentionally relative (no warning, below) |
| `p / "name"` | A new path with `name` appended |
| `p.parent()`, `p.file_name()`, `p.extension()`, `p.as_str()` | Parts of the path |
| `std::filesystem::executable_dir()` | The directory that contains the running executable |
| `std::filesystem::create_directories(p&) → std::result<void, std::io_error>` | Creates the directory and any missing parents |

### The relative-path warning
```edax
std::file in = std::file::open("employees.csv")?;                     // WARNING: relative path literal
std::file in = std::file::open(std::filesystem::executable_dir() / "employees.csv")?;   // OK
std::file in = std::file::open(std::filesystem::path::relative("employees.csv"))?;     // OK: explicitly relative
std::file in = std::file::open(user_path)?;                           // OK: run-time value, caller's choice
```
- `std::file::open` and `create` take a `std::filesystem::path&` or a `std::str`; a literal goes to the `std::str` overload. Paths starting with `/`, `./` or `../` are not warned about.
- The warning only applies to paths known at compile time, such as **string literals**.
- It isn't built into the compiler: it's an entry check of `std::file::open` in the standard library's source (see [Checks](../../language/errors/checks.md), example 7). Silence it with `--warn=std::relative_path=off`.

### Environment: `std::env`
- The program's arguments come to `main` as `std::span<std::str>` (see [Functions](../../language/functions/functions.md)).
- `std::env::raw_args()` returns copies of the arguments as bytes (`std::darray<std::darray<u8>>`), for arguments that aren't UTF-8.
- `std::env::variable(name) → std::option<std::string>` reads an environment variable.

## Rationale
- **Files are values:** closing is destruction, so a file can't be used after it is closed or forgotten open.
- **Relative-path warning as library code:** a literal relative path depends on the directory the program is started from, which is usually a bug. The warning is an entry check of `std::file::open`, not a compiler rule (see [Checks](../../language/errors/checks.md)).
