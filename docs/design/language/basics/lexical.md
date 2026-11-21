# Lexical Structure

## Rules

### Files
| Extension | Meaning |
|---|---|
| `.rifts` | Source file (implementation) |
| `.rifth` | Header file (interface), see [Headers, Sources and Modules](../organization/modules.md) |

- Source text is UTF-8.
- A `.rifts` file without a matching `.rifth` is a standalone file. It can't be imported, which suits a program's `main` file.

### Comments
```rift
// line comment
/* block comment, does not nest */
```
There is no separate doc-comment syntax. The comment lines directly above a declaration describe it: `riftc explain` prints them for diagnostic ids (see [Checks](../errors/checks.md)), and tools show them for any name.

### Identifiers and naming
| Category | Convention | Enforced? |
|---|---|---|
| Keywords | `snake_case` | Yes (compiler) |
| Built-in types | `snake_case` | Yes (compiler) |
| Standard library | `snake_case`, written out in full (see [The Standard Library](../../stdlib/README.md)) | Yes (stdlib authoring rule) |
| User-defined names | Any casing | No |

Identifiers are ASCII letters, digits and `_`, and must not start with a digit. `_` alone is the wildcard (in patterns, and in `_ = expr;`).

### Keywords
```
import  namespace  struct  class  interface  enum  template  type  annotation
virtual  override  abstract  operator
public  protected  private  static  extern
mut  move  this  view
compile_time  run_time  deduce  check  thread_local
if  else  for  while  break  continue  return  match
true  false  nullptr  default  auto  cast  bit  unsafe
```
**Contextual keywords** are keywords only in one position and ordinary names everywhere else:

| Word | Keyword when | See |
|---|---|---|
| `open` | directly before `class` or `struct` (`open class shape`) | [Inheritance and interfaces](../user-types/inheritance-and-interfaces.md) |
| `implicit` | directly before a parameter's type (`implicit std::call_site where`) | [Functions](../functions/functions.md) |
| `annotated`, `generate`, `wrap` | inside an `annotation` declaration | [Annotations](../compile-time/annotations.md) |

`@` starts an annotation (see [Annotations](../compile-time/annotations.md)).

A keyword may be used as the name of a field or method, because such names are always reached through `.` or `::`: `std::meta::field` has a field called `type`, read as `f.type`.

Built-in type names, which are reserved like keywords:
```
bool  char  void  never
i8 i16 i32 i64 i128 isize
u8 u16 u32 u64 u128 usize
f32 f64
```
The SIMD vector types `<number type>x<lanes>` (`f32x8`, `u8x32`, `i64x4`, ...) are reserved the same way (see [SIMD](../simd/simd.md)).

### Literals
| Kind | Examples | Notes |
|---|---|---|
| Integer | `42`, `0xFF`, `0b1010`, `0o17`, `1_000_000` | `_` separators are allowed anywhere after the first digit |
| Integer with suffix | `42u32`, `0xFFu8`, `7usize` | Suffix = any integer type name |
| Float | `1.0`, `0.5`, `1e-3`, `2.5e10` | A float must contain `.` or `e` |
| Float with suffix | `1.0f32`, `3.14f64` | |
| Bool | `true`, `false` | |
| Char | `'a'`, `'\n'`, `'\u{1F600}'` | Type `char` |
| String | `"hello\n"` | Type `std::static_str`, never interpolated (see [Strings](../../stdlib/text/strings.md)) |
| Raw string | `r"C:\path\no\escapes"` | No escape processing |
| C string | `c"r+b"` | A NUL-terminated `std::raw_ptr<u8>` for C functions (see [Raw Pointers and `unsafe`](../memory/raw-pointers-and-unsafe.md)); escapes as in `"..."`, but `\0` is an error |
| Format string | `f"x = {x}"` | Interpolated (see [Strings](../../stdlib/text/strings.md)) |
| Null pointer | `nullptr` | Only for `std::raw_ptr` (see [Raw Pointers and `unsafe`](../memory/raw-pointers-and-unsafe.md)) |

Unsuffixed literals take their type from context (see [Built-in Types and Conversions](types.md)).

### Statements, `;` and braces
- `;` ends **statements** and **declarations without a body** (`void f();` in a header).
- A `}` that closes a type, function, block or control-flow statement is **never** followed by `;`.
- A declaration whose initializer ends in `}` still ends with `;`, because the declaration is the statement:
  ```rift
  u32 x = match (code) { 0 => 1, _ => 2, };
  auto f = [](u32 x) { return x + 1; };
  ```
- Braces are **mandatory** for `if`, `else`, `for`, `while`, `match` and every function body.

## Examples
```rift
struct point {
    f64 x = 0.0;
    f64 y = 0.0;
}

enum color : u8 {
    red = 0,
    green,
}

void log(std::str message);    // header declaration: ';' ends it
```

## Rationale
- **No `;` after types:** C needs it for `struct x {...} instance;`. Rift doesn't have that form, so the `;` only causes errors when it's forgotten.
- **Mandatory braces:** this rules out goto-fail-style bugs where an unbraced second line looks like part of the `if`. It's also required for `if` to work as an expression (see [Control Flow](control-flow.md)).
- **Contextual keywords:** words like `open` and `implicit` are useful as names elsewhere (`std::file::open`), so they're reserved only where they mean something.
