# Files and the File System: `std::filesystem`

Every file operation goes through a **mount table**. A path's first parts choose a **source**: a folder on disk, a packed archive, files in memory, or a source the program writes itself. The same code reads `content/textures/rock.png` from the project folder during development and from `game.pak` in the shipped game; only what is mounted changes. A program that mounts nothing gets the operating system's file system, as everywhere else.

```edax
std::filesystem::table fs = std::filesystem::table::global();
if compile_time (build::shipping.value()) {
    std::filesystem::archive pak = std::filesystem::archive::load(std::filesystem::executable_dir() / "game.pak")?;
    std::filesystem::mounted content = fs.mount("content", move pak, std::filesystem::mount_options{});
} else {
    std::filesystem::mounted content = fs.mount("content", std::filesystem::native::at(project / "content"), std::filesystem::mount_options{});
}
std::filesystem::mounted saves = fs.mount("saves", std::filesystem::native::at(std::filesystem::user_data_dir("my_game")),
                                          std::filesystem::mount_options{ .writable = true });

std::darray<u8> png = std::filesystem::read_bytes(std::filesystem::path::from("content/textures/rock.png"))?;
```

## Rules

### Paths
- `std::filesystem::path` is text with `/` between parts. `p / "name"` appends a part, `p.normalized()` removes `.`, empty parts and `part/..`.
- A path that starts with `/` is an **operating-system path**: no mount table sees it (`executable_dir()`, `/proc/...`, `/usr/share/zoneinfo/...`).
- Every other path is a path **of a table**.
- Parts of the last name: `file_name()` (`"app.tar.gz"`), `extension()` (`"gz"`, `""` when none), `stem()` (`"app.tar"`), `has_extension("gz")`, `with_extension("zip")` (`""` removes it).

#### Absolute and relative paths in the type
Code that needs one kind says so in its types, and the compiler holds it to that:

| Type | Is | Made by |
|---|---|---|
| `std::filesystem::absolute_path` | a path that starts with `/` | `executable_dir()`, `user_data_dir(app)`, `user_config_dir(app)`, `temp_dir()`, `path::absolute("/...")`, `p.to_absolute()`, `cast<absolute_path>(p)` |
| `std::filesystem::relative_path` | a path a mount table resolves | `path::relative("...")`, `p.to_relative()`, `cast<relative_path>(p)` |
| `std::filesystem::path` | either | `path::from(text)` |

```edax
void install_default_sinks(mut std::log::session& log, std::filesystem::absolute_path& log_dir);

install_default_sinks(mut log, std::filesystem::executable_dir() / "log");   // an absolute_path
install_default_sinks(mut log, std::filesystem::path::from("log"));
// error: expected 'std::filesystem::absolute_path&', found 'std::filesystem::path'
std::filesystem::relative_path levels = std::filesystem::path::relative("/levels");
// error: "/levels" starts with '/': it is an absolute path; use 'path::absolute'
```
- Both are distinct types of `path` with a check (see [Templates](../../language/compile-time/templates.md), "Distinct types with a check"): `cast` and the constructors check, text known at compile time is checked there, and a path that fails at run time is a panic. `p.to_absolute()` and `p.to_relative()` ask instead (`std::option`).
- Every function that takes a `path&` takes both (lent read-only), and either moves into a `path`. They have `path`'s methods that only read.
- `/` keeps the kind: `absolute_path / "name"` and `absolute_path / relative_path` are absolute, `relative_path / "name"` and `relative_path / relative_path` relative; `relative_path / absolute_path` doesn't exist.

### Sources
A `std::filesystem::source` answers for the files below the place it is mounted. Its paths are relative to that place and normalized.

| Method | Does | Default |
|---|---|---|
| `open(at, mode)` | A `std::filesystem::stream` for `open_mode::read`, `write` (new or emptied) or `append` | required |
| `info(at)` | `file_info`: kind (`file`, `directory`, `link`, `other`), size, modified, read-only | required |
| `list(at)` | The entries of a directory, sorted by name | required |
| `create_directory`, `rename`, `remove` | Changes | `io_error::read_only` |
| `map(at)` | The file's bytes in memory (`region`) | required |
| `os_path(at)` | The operating-system path behind `at` (sources on disk) | `none` |

A source is shared by every thread that uses its table, so its methods don't change it (`std::filesystem::memory` locks inside).

| Source | What it is |
|---|---|
| `native::at(root)` | A folder on disk: reading, writing, mapping (`mmap`), watching. A writable mount creates its folder on the first write. `native::os()` takes paths as they are. |
| `archive::load(p)` | A read-only archive (below), mapped once; `map` hands out its files without copying. |
| `memory::make()` | Files in RAM, readable and writable: tests, generated data, `std::embed` blobs. `share()` is another handle to the same files. |
| your own | Anything that implements `source`: an encrypted pack, a network share, a plugin's source (`mount_shared`). |

### Mount tables
- `table::global()` is the table every call uses unless something else is chosen (below). `table::create(name)` makes another; `table::named(name)` finds one. A table is a copy type, a handle to a table that lives as long as the program.
- `t.mount(prefix, source, mount_options{ .priority, .writable })` returns a `mounted`. Dropping it (or `unmount()`) takes the source out; reads that already started finish first. It is `@std::must_use`.
- **Which mount answers:** the mounts with the longest prefix that covers the path (`content` covers `content` and `content/...`; `""` covers everything). Reads ask them by priority (higher first; for equal priorities, the newer mount first) until one has the file: mods and patches mounted over the base archive shadow its files. Writes go to the first writable one; when there is none, `io_error::read_only`.
- **Unmounted paths** go to the operating system (relative to the working directory). `t.set_fallback(false)` makes them `io_error::not_mounted` instead, as a shipped game may want.
- `list` merges the entries of every mount of the prefix (the higher priority wins for equal names) and shows mounts below the directory as directories. `info` of a path that only leads to mounts is a directory.
- `rename` works within one source (`io_error::not_supported` across sources).
- Threads may mount and read at the same time.

### Which table a call uses
| Level | How | For |
|---|---|---|
| The whole program | The global table | Mount `content/` once at start; every call sees it |
| A file | `@std::filesystem::use_table{ .name = "disk" };` after the imports | Code that always works on the raw folders |
| Files or a target, chosen in `build.ed` | `g.set_for_files(editor, "src/cooker/**", "std::filesystem::table", "disk");`, `g.set(mod_plugin, "std::filesystem::table", "mods");` | An asset cooker that reads raw files even in a game build; a plugin in its own sandbox |
| One call | The table's methods: `t.read_bytes(p)` | Tools that work on two trees at once |

The free functions (`std::filesystem::read_bytes(p)`, `std::file::open(p)`, ...) take `implicit compile_time std::call_site`, so the choice is made at compile time for the calling file; the code doesn't change. A table the program hasn't created is `io_error::not_mounted`. `std::filesystem::table_at()` gives the table of the calling file.

### Operations
On a table, and as free functions on the calling file's table: `exists`, `is_file`, `is_directory`, `is_link` (false when it doesn't exist; `is_file` and `is_directory` follow links), `info`, `list`, `read_bytes`, `read_text` (`invalid_data` when not UTF-8), `write_bytes`, `write_text` (create or empty), `create_directories`, `rename`, `remove` (a file or an empty directory), `map` (a `mapped`: the bytes without copying), `watch` (sources on disk; changes come back as the table's paths), `os_path`, `walk` and `walk_all` (below). And: `read_link(p)` (on disk), `executable_dir()`, `user_data_dir(app)`, `user_config_dir(app)`, `temp_dir()`, and `dynamic_library_extension()` (`"so"`, `"dll"` or `"dylib"` for the platform being built for). `entry` and `file_info` have `is_file()`, `is_directory()` and `is_link()`.

### Walking a directory tree
`walk(dir)` gives every entry below `dir`, as a stream for `for` (see [Control Flow](../../language/basics/control-flow.md)): it reads one directory at a time, so the tree is never loaded at once.
```edax
for (std::filesystem::walk_entry<std::filesystem::absolute_path> e : std::filesystem::walk(plugin_dir).expect("plugins")) {
    if (e.is_file() && e.path.has_extension(std::filesystem::dynamic_library_extension())) { load(e.path); }
}

mut std::filesystem::walker<std::filesystem::relative_path> w = std::filesystem::walk(content, std::filesystem::walk_options{
    .order = std::filesystem::walk_order::breadth_first, .include_hidden = true })?;
for (std::filesystem::walk_entry<std::filesystem::relative_path> e : w) {
    if (e.is_directory() && e.name() == "drafts") { w.skip_children(); }
}
for (std::filesystem::walk_problem& p : w.problems()) { std::log::warn(f"can't read {p.path.as_str()}: {p.error}"); }
```
- A `walk_entry<P>` has the `path` (`root / ... / name`), its `kind`, its `depth` (0: directly in the root), and `is_file()`, `is_directory()`, `is_link()`, `name()`. `P` is the kind of path the walk started from (`path`, `absolute_path` or `relative_path`), and every entry's path has it; `walk_entry` alone is `walk_entry<path>`.
- `walk_options`:

| Field | Default | Does |
|---|---|---|
| `max_depth` | no limit | 0: only the root's own entries, like `list` |
| `order` | `depth_first` | `depth_first`: a directory, then everything below it, then the next entry. `breadth_first`: depth 0, then depth 1, ... The entries of one directory come sorted by name in both |
| `include_directories` | `true` | Whether directories are reported; they are entered either way |
| `include_hidden` | `false` | Names starting with `.` and everything below them (`.git`, the build's `.edax`) |
| `follow_links` | `false` | Enter symbolic links to directories (on disk). A link to a directory above it is not entered, and at most 8 links are followed on one way down; a link that isn't entered is reported as `link` |

- `w.skip_children()` doesn't enter the directory `next()` returned last.
- Only the root is an error (`walk(dir)` returns `std::result`). A directory below it that can't be read is skipped and kept in `w.problems()`.
- `walk_all(dir, options)` returns every entry at once (a `darray`), for sorting, counting or two passes.
- A walk goes through the mount tables like every other call: walking `content` on an archive walks the archive, and mounts below a directory are directories in it.

### `std::file`, `std::writer` and rotating files
- `std::file::open/create/append(path)` go through the calling file's table. A file reads and writes through its source's stream: `read`, `read_at(offset, into)` (positional, safe from several threads: many assets out of one archive at once), `read_bytes`, `read_all`, `lines`, `write`, `write_bytes`, `flush`, `size`.
- `std::writer` is what text is written to in order (`write`, `flush`): `std::file`, `std::filesystem::rotating_file`, `std::stdout()`, `std::stderr()`, or the program's own. Log sinks write to any writer (`std::log::writer_sink`).
- `std::filesystem::rotating_file::open(path, rotating_options{ .rotation, .append })` continues in a new file by size (`rotation::by_size(size_rotation{ .max_bytes, .keep })`) or by day (`rotation::daily(keep)`): `app.log` becomes `app.1.log`, and so on. It creates missing directories and works on any writable source.

### Errors
`io_error`: `not_found`, `permission_denied`, `already_exists`, `is_directory`, `not_directory`, `invalid_data`, `read_only`, `not_supported`, `not_mounted`, `other`.

### Archives
- `edaxc pack <folder> <archive> [--exclude=GLOB]...` packs a folder. In `build.ed`, `g.add_archive(std::build::archive_spec{ .name = "game.pak", .from = "content", .exclude = ["**/*.psd"] })` is a step that packs again whenever a file changes; `g.depends_on(game, step)` builds it with the game. `.output_dir` places it like a target (`g.output_dir` patterns; `{name}` is the archive's name without its extension).
- Format (little-endian): a 32-byte header (`EDAXPAK1`, `u32` version 1, `u32` count, `u64` index offset, `u64` index size); the files, each at a multiple of 16; the index, sorted by hash then path: per file `u64` FNV-1a 64 hash of the path, `u64` offset, `u64` size, `u32` path length, `u32` flags (0: stored as is), the path padded to a multiple of 8. A lookup is a binary search by hash.
- An archive is a source like any other: it can itself be loaded from a mount (an archive inside an archive).

## Not yet
- `for (auto e : ...)`: the loop variable of a walk is written with its type.
- Compression per file (the flags field), and archives appended to the executable.
- Asynchronous reads (io_uring); `read_at` with the thread pool covers streaming for now.
- Windows paths and sources.
- Programs with plugins: each plugin has its own copy of std's state, so a plugin doesn't see the host's tables yet (the host can hand a source over with `mount_shared`).

## Rationale
- **One table for every call:** an engine's code names files once (`content/...`); where they come from is decided at start, by build mode or configuration, not at every call. Shipping, modding and patching are mounting, not code changes.
- **The choice per file at compile time:** the same mechanism as log levels (`implicit std::call_site` and build settings): no global switch to flip at run time, no parameter to pass through every function, and code that must see the raw folders (a cooker) says so in `build.ed`.
- **The operating system as the fallback:** programs that don't care about any of this work as everywhere else; a shipped game turns the fallback off and can't read stray files by accident.
- **The kind of a path in its type:** a parameter named `absolute_path_to_log_dir` promises nothing; `absolute_path&` does, and the compiler checks every caller. The kinds are distinct types of `path` with a check, so every existing function that takes a `path&` takes them unchanged.
- **Walking as a stream:** `for` already goes over anything with `next()`, so a walk needs no iterator protocol; reading one directory at a time keeps memory at the depth of the tree, not its size.
- **Absolute paths bypass the tables:** the system's own files (`/proc`, the time-zone database, the executable's folder) are never shadowed by a mount.
- **Sources hand out streams and regions, not file descriptors:** archives and memory work like disk, and other modules (the log, the UI, time zones) read and write through std::filesystem instead of calling the C library themselves.
