# Command-line arguments

`std::args` reads a program's command line into a struct. The struct is the description: one field per argument, its type says what the argument accepts, its initial value is the default, and annotations add names, help texts and limits. `import std;` is enough; the module loads when a file names `std::args`.

## Rules

### Parsing
| Name | Result |
|---|---|
| `std::args::parse<T>(args)` | `std::result<T, std::args::error>`: the command line `args` (as `main` gets it, the executable first) read into a `T` |
| `std::args::help<T>(program)` | `std::string`: the help, as `--help` prints it |
| `e.report()` on a `std::args::error` | Prints it and gives `main`'s exit code: the help and the version go to the output (success), a wrong command line goes to the error output with the usage (failure) |
| `e.kind` | `std::args::stop::help`, `version` or `invalid`; `e.message` says what is wrong, `e.text` is the help, the version line or the usage |

`T` is a struct, and **every field has an initial value** (`std::filesystem::path project = default;`): `parse` starts from `T`'s default and fills in what the command line gives.

### Fields
| Field | On the command line |
|---|---|
| No annotation, `bool` | A flag: `--name` |
| No annotation, another type | An option: `--name value`, `--name=value` |
| `@std::args::option{ .name, .short_name, .help, .value_name, .required }` | An option with another name, a one-letter form (`-t 8`, `-t8`), help |
| `@std::args::flag{ .name, .short_name, .help }` | A flag; on a `bool` it sets `true`, on an integer each use counts (`-vvv` is 3) |
| `@std::args::positional{ .help, .value_name, .required }` | An argument by position, in field order. Required unless `.required = false` or the field is a `std::option<T>`; a `std::darray<T>` takes all remaining ones |
| `@std::args::range{ .min, .max }` | Numbers outside are an error |
| `@std::args::subcommand` | On a payload enum (or a `std::option` of one): each alternative is a subcommand (`tool build ...`), its payload struct holds the subcommand's arguments. Required unless it is a `std::option` |
| `@std::args::command{ .name, .help }` | On an alternative of a subcommand enum: its name and help |
| `@std::args::flatten` | On a struct field: its arguments are given as the outer struct's (shared groups like `logging_options`) |
| `@std::args::skip` | Not an argument; keeps its initial value |
| `@std::args::program{ .name, .about, .version }` | On the struct: the name in the help (default: the executable's file name), a line about the program, and `--version` |

Names on the command line are the field's or alternative's name with `-` for `_` (`log_file` is `--log-file`, `size_optimized` is `size-optimized`).

### Types
| Field type | Accepts |
|---|---|
| Integers, floats | Numbers (`--scale -0.5` works: a value may start with `-`) |
| `bool` | As a flag: nothing; as an option or positional: `true`/`false`, `yes`/`no`, `on`/`off`, `1`/`0` |
| `std::string`, `std::filesystem::path` | Any text |
| Enums without payloads | An alternative's name |
| `std::option<T>` | `T`; not given: `std::none` |
| `std::darray<T>` | `T`, once per use of the option (or all remaining positional arguments) |

Other types are compile errors, at the annotation (`@std::args::flag works on bool fields ... 'name' is a std::string`) or at the `parse` call for a field without one (`the field 'settings' can't hold an argument; mark it @std::args::skip or @std::args::flatten`).

### The command line
- `--name value`, `--name=value`, `-n value`, `-nvalue`; flags grouped as `-vq`, also before a short option with its value (`-vt8`).
- `--` ends the options: everything after it is positional.
- `-h`/`--help` anywhere prints the help; `--version` with `@std::args::program{ .version = ... }`.
- After a subcommand's name, the arguments are the subcommand's; the outer struct's options come before it.
- Mistakes name the argument and, when another name is close, suggest it: `unknown option '--treads' (did you mean '--threads'?)`, `--renderer must be one of vulkan, opengl or software, found 'opengel' (did you mean 'opengl'?)`.

## Examples
```edax
import std;

enum render_api : u8 { vulkan, opengl, software }

@std::args::program{ .about = "Gluttony game editor", .version = "0.3.1" }
struct options {
    @std::args::positional{ .help = "Path to the project file" }
    std::filesystem::path project_path = default;

    @std::args::flag{ .short_name = "v", .help = "More log output" }
    bool verbose = false;

    @std::args::option{ .short_name = "t", .help = "Worker threads (0: one per core)" }
    @std::args::range{ .min = 0.0, .max = 64.0 }
    u32 threads = 0;

    @std::args::option{ .help = "Graphics backend" }
    render_api renderer = render_api::vulkan;

    @std::args::option{ .help = "Write the log here" }
    std::option<std::filesystem::path> log_file = std::none;

    @std::args::option{ .name = "plugin", .help = "Load a plugin" }
    std::darray<std::string> plugins = default;
}

std::exit_code main(std::span<std::str> args) {
    options opts = match (std::args::parse<options>(args)) {
        std::ok(o) => move o,
        std::error(e) => return e.report(),
    };
    // ...
    return std::exit_code::success;
}
```
`gluttony --help` prints:
```
Gluttony game editor

usage: gluttony [options] <project-path>

Arguments:
  <project-path>   Path to the project file

Options:
  -v, --verbose             More log output
  -t, --threads <n>         Worker threads (0: one per core) [default: 0]
      --renderer <choice>   Graphics backend [default: vulkan] [vulkan, opengl or software]
      --log-file <path>     Write the log here
      --plugin <text>       Load a plugin (repeatable)
  -h, --help                Show this help
      --version             Show the version
```

Subcommands:
```edax
struct build_options {
    @std::args::flag{ .short_name = "r" } bool release = false;
    @std::args::positional{ .required = false } std::darray<std::string> targets = default;
}

enum command : u8 {
    @std::args::command{ .help = "Build targets" }
    build(build_options),
    @std::args::command{ .help = "Remove build output" }
    clean_all,
}

struct cli {
    @std::args::flatten logging_options log = default;
    @std::args::subcommand command cmd = command::clean_all;
}
// tool -v build -r app lib    tool clean-all    tool build --help
```

## Rationale
- **The struct is the specification:** names come from fields, so a misspelled name is a compile error instead of a lookup that fails at run time (`get<T>(parsed, "name")`), and the values arrive typed. It is the same idea as UIs from types (`ui::form<T>`): reflection over fields and annotations, no registration.
- **Initial values as defaults:** one place for the default, shown in the help. Reflection can't see whether a field has an initial value, so required arguments are said explicitly (`.required`), positional ones are required by default.
- **The error is a value:** `--help` and `--version` come back as an `error` that prints and gives `success`, so `main` keeps one exit path and the library never ends the process.
- **Library only:** `std::args` is Edax code over `std::meta`; the compiler only registers the module (`compiler/CMakeLists.txt`, `modules.cpp`).

## Not yet
- Options of the outer struct after a subcommand's name.
- Own value types (a static `from_arg(std::str)`), and a run-time `std::args::parser` for arguments only known while running (plugins adding their own).
- Integer limits in the "expects a whole number" message: code generation doesn't support `V::min` on a template parameter yet.
