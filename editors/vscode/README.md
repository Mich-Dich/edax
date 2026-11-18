# Rift for VS Code

Syntax highlighting, build, and step-through debugging for the Rift language (`.rifts` source,
`.rifth` header).

## Install

```sh
code --install-extension rift-language-0.5.0.vsix
```

Or in VS Code: Extensions view → `…` menu → **Install from VSIX…**. Debugging uses
[CodeLLDB](https://marketplace.visualstudio.com/items?itemName=vadimcn.vscode-lldb)
(`vadimcn.vscode-lldb`); VS Code installs it along with this extension.

## Debugging

Open a `.rifts` file and press **F5** (or use the ▷ / 🐞 buttons in the editor title). The
extension builds the file with `riftc build --mode=debug`, shows compile errors in **Problems**,
and starts the debugger. It works like C++ in VS Code:

- Breakpoints in `.rifts` files, Step Over / Into / Out, Continue, Run to Cursor.
- **Step Into enters the standard library**: `list.push(7)` opens `library/std/std.rifts` at
  `darray<T>::push`. The call stack shows Rift names (`std::darray<u32>::push`, `geometry::area`).
- **Variables and Watch**: locals, parameters, `this`, loop variables and `match` bindings, with
  Rift types (`u32`, `std::string`, `geometry::point`). Standard library values are shown by
  content:

  | Value | Shown as |
  |---|---|
  | `std::string`, `std::str`, string literals | `"text"` |
  | `std::darray<T>`, `std::span<T>`, `std::array<T, N>` | `size=3`, then `[0]`, `[1]`, … |
  | `std::hash_map<K, V>`, `std::tree_map<K, V>` | `size=2`, then `["key"] = value` |
  | `std::unique_ptr<T>`, `std::shared_ptr<T>` | the pointee's fields; `strong=2 weak=0` |
  | `std::option<T>`, `std::result<T, E>`, any enum with payloads | `some(42)`, `error(bad_number)`, `word("hi")` |
  | `char` | `'R'` |

Debug a target of a `build.rifts` project with a launch configuration (**Run → Add
Configuration… → Rift: Debug a build.rifts target**):

```jsonc
{
    "type": "rift",
    "request": "launch",
    "name": "Debug app",
    "target": "app",            // or "program": "${file}" for a single file
    "args": [],
    "cwd": "${workspaceFolder}",
    "mode": "debug"             // release_with_debug_info: optimized, variables may be <optimized out>
}
```

| Setting | Default | Meaning |
|---|---|---|
| `rift.compilerPath` | `""` | `riftc` to use. Empty: `build/compiler/riftc` in a workspace folder, else `riftc` on `PATH` |
| `rift.standardLibraryPath` | `""` | `library/std` sources, only needed if the checkout riftc was built from has moved |
| `rift.debug.buildMode` | `debug` | Build mode for debugging |
| `rift.debug.stepIntoStandardLibrary` | `true` | Step Into enters `std::` functions |

Variables need `debug_info()` to be `full` (the `debug` mode). `line_tables` gives breakpoints,
stepping and call stacks only.

### Without VS Code

The same works in a terminal:

```sh
riftc build hello.rifts                     # -> build/debug/hello
lldb build/debug/hello
(lldb) command script import editors/vscode/lldb/rift_formatters.py
(lldb) settings set target.process.thread.step-avoid-regexp ''
(lldb) b hello.rifts:12
(lldb) run
```

`gdb build/debug/hello` works too (breakpoints, stepping, `info locals`), without the formatters.

## Code insights

Off until you turn them on: the eye button in the editor title (**Rift: Toggle Code Insights**),
or **Rift: Choose Code Insights…** (also the *Insights* item in the status bar) to pick what to
show. Insights show what the compiler does that the source doesn't spell out:

| Category | Shown as | Default |
|---|---|---|
| Stack frames | a line above each function: every slot it reserves **on entry**, whichever scope declares it (variables, temporaries, drop flags) | on |
| Heap memory | highlighted calls that may allocate or free, with the call chain to `malloc` on hover (`list.push(7)`, `std::string::from`, formatting) | on |
| Drops | a line above `}` (and above `return` / `break` / `continue`): which destructors run there, in which order, conditional ones, and whether they free heap memory | on |
| Moves | `move x` and returning a local: ownership leaves the variable, it won't be dropped | on |
| Large copies | copy types over 16 bytes passed or assigned by value | on |
| Temporaries | unnamed stack slots (values being built, f-string buffers, …) | off |
| Implicit conversions | `std::string → std::str`, literal → `std::string` (allocates), upcasts, `u32 → bool` in conditions, lambda → `std::function` | on |
| Dynamic dispatch | virtual and interface calls through a table | on |
| Run-time checks | overflow, shift and division checks that can panic | off |

At the end of a line each fact is a short tag, most important first: `alloc` (`alloc↻` in a loop),
`free`, `drop`, `copy24` (bytes), `move`, `virt` / `dyn`, `chk`, `conv`, `tmp`. Hover a tag for what
it stands for, the explanation, and the code it is about underlined:

```text
85 |     shapes.push(shape::dot);
   |            ^^^^
```

Insights follow the program, not just the code on one line:

- **Capacity**: a local `darray`, `string` or `hash_map` is followed from `default` /
  `with_capacity(n)` through `reserve(n)`, pushes (also in loops with constant bounds) and its
  other methods. A `push` that fits the capacity it already has shows nothing; one that surely
  grows says so (`allocates: full at 4, capacity 4 → 8`).
- **Text**: `std::println` / `std::format` with an f-string build the text in a heap buffer (a
  24-byte buffer header on the stack); short `std::string`s (up to 23 bytes) are stored inline and
  never allocate. Panic messages don't count.
- **Drops**: a variable moved on every path has no drop; one moved in a branch says "(if not moved)".
- **Temporaries and stack frames**: only slots whose address is used are shown as `tmp`; the frame
  line also says how much stays in memory once optimized.
- **Checks**: checks that cannot fail (`k % 4`, an exhaustive `match`) are not shown.

Each category is a setting (`rift.insights.show.<category>`). `rift.insights.virtualLines` puts
frames and drops at the end of the line instead of on lines of their own; `rift.insights.inlineLabels`
switches the tags off (the highlights keep their hovers). Insights refresh
when the file is saved. They come from `riftc insights <file>`, which prints them as JSON.

## Highlighting

- Comments: `//`, `/* */`, and `///` doc comments
- Strings: `"..."` with escapes, raw `r"..."`, C strings `c"..."`, and format strings `f"..."` with
  `{expr}` / `{expr:spec}` interpolation and `{{ }}` escapes
- Char literals, including `'\u{1F600}'`
- Numbers: decimal, `0x`, `0b`, `0o`, `_` separators, floats, and type suffixes (`42u32`, `1.0f32`)
- Keywords, built-in types (`i8`…`u128`, `usize`, `f32`, `f64`, `bool`, `char`, `void`, `never`)
- Declarations: `struct` / `class` / `interface` / `enum` names, base lists, `open class`,
  `view struct`, destructors `~name()`, `operator==` etc.
- `template<type T : iface>` parameters, `cast<T>` and `cast::truncate/saturate/round/checked/dynamic/reinterpret`
- Namespaces (`std::`, `module::`), function calls and definitions, type names in declarations and
  `T{...}` construction
- Operators including wrapping `+% -% *%`, ranges `..` / `..=`, `<=>`, `=>`, `->`, and `?`
- Access labels `public:` / `protected:` / `private:`, attributes `@name`

## How it fits together

| Piece | Where |
|---|---|
| Debug info (DWARF 5: lines, variables, types) | `compiler/src/llvm_backend.cpp`, section "variable debug info" |
| Launch configuration → `riftc build` → CodeLLDB | `extension.js` |
| How LLDB shows Rift values | `lldb/rift_formatters.py` |
| Code insights (`riftc insights` → decorations, CodeLens) | `insights.js`; the facts: `compiler/src/llvm_backend.cpp`, section "insights" |
| Highlighting | `syntaxes/rift.tmLanguage.json` (keywords mirror `compiler/include/rift/token_kinds.def`) |

Repackage after changes:

```sh
npx @vscode/vsce package --allow-missing-repository
```
