# Rift for VS Code

Syntax highlighting for the Rift language (spec v0.3) — `.rifts` source and `.rifth` header files.

## Install

```sh
code --install-extension rift-language-0.3.0.vsix
```

Or in VS Code: Extensions view → `…` menu → **Install from VSIX…**

For development, open this folder in VS Code and press **F5** to launch an Extension Development Host.

## What gets highlighted

- Comments: `//`, `/* */`, and `///` doc comments
- Strings: `"..."` with escapes, raw `r"..."`, C strings `c"..."`, and format strings `f"..."` with `{expr}` / `{expr:spec}` interpolation and `{{ }}` escapes
- Char literals, including `'\u{1F600}'`
- Numbers: decimal, `0x`, `0b`, `0o`, `_` separators, floats, and type suffixes (`42u32`, `1.0f32`)
- Keywords, built-in types (`i8`…`u128`, `usize`, `f32`, `f64`, `bool`, `char`, `void`, `never`)
- Declarations: `struct` / `class` / `interface` / `enum` names, base lists, `open class`, `view struct`, destructors `~name()`, `operator==` etc.
- `template<type T : iface>` parameters, `cast<T>` and `cast::truncate/saturate/round/checked/dynamic/reinterpret`
- Namespaces (`std::`, `module::`), function calls and definitions, type names in declarations and `T{...}` construction
- Operators including wrapping `+% -% *%`, ranges `..` / `..=`, `<=>`, `=>`, `->`, and `?`
- Access labels `public:` / `protected:` / `private:`, attributes `@name`

The editor also gets comment toggling, bracket matching, auto-closing pairs, and `///` continuation on Enter.

## Updating the grammar

The keyword list mirrors `compiler/include/rift/token_kinds.def`. When the lexer gains a keyword, add it to the matching group in `syntaxes/rift.tmLanguage.json`, then repackage:

```sh
npx @vscode/vsce package --allow-missing-repository
```



install with:
```bash
code --install-extension editors/vscode/rift-language-0.3.0.vsix
```
