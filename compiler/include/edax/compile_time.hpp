#pragma once
// Compile-time evaluation (spec 15, 23, 26).
//
// The evaluator runs checked code inside the compiler: the initializers of `compile_time`
// constants, the conditions of `if compile_time`, the ranges of `for compile_time`, and every
// `compile_time` function they call. It works directly on the checked AST, so it sees exactly
// the types the checker computed.
//
// Values are plain data (integers, floats, bools, chars, strings, types, structs, enums, lists)
// plus the reflection handles of `std::meta` (a field or an alternative of a reflected type).
//
// Errors:
//   E4200  a value that is not known at compile time (a run-time variable, a non-compile_time
//          function, I/O)
//   E4201  the evaluation itself failed (overflow, division by zero, out-of-bounds index, a
//          panic, too many steps or too deep recursion)
// Code that depends on a template parameter that is not known yet (the generic check of a
// template) gives "unknown" without an error; the instance check evaluates it again.

#include "edax/diagnostics.hpp"
#include "edax/symbols.hpp"
#include "edax/types.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace edax::ct {

__extension__ typedef __int128 i128;

// Exact f128 arithmetic for the evaluator (spec 34), on IEEE binary128 bit patterns. Implemented
// with LLVM's APFloat in the backend, so the front end needs no 128-bit float of its own.
namespace quad {
u128        from_text(std::string_view decimal);
u128        from_double(double v);
u128        from_integer(u128 bits, bool is_signed, int width);
double      to_double(u128 q);
u128        apply(char op, u128 a, u128 b);   // + - * / %
int         compare(u128 a, u128 b);          // -1, 0, 1, or 2 when unordered
std::string to_text(u128 q);                  // the shortest text that reads back exactly
// Toward zero into an integer of `width` bits; false when NaN or out of range. `exact`: no fraction.
bool        to_integer(u128 q, bool is_signed, int width, u128& out, bool& exact);
} // namespace quad

struct Value {
    enum class Kind { void_, integer, floating, boolean, character, string, type, aggregate, enumeration, list, field, alternative, function, parameter, file };

    Kind               kind      = Kind::void_;
    const types::Type* type      = nullptr;   // the Edax type of the value
    u128               integer   = 0;         // integers: two's complement, masked to the type's width
    double             floating  = 0.0;
    // f128: the exact IEEE binary128 bits (`floating` then holds the nearest double, for the
    // code that only reads doubles). Unset: the value is exactly `floating`.
    std::optional<u128> quad;
    bool               boolean   = false;
    char32_t           character = 0;
    std::string        string;                 // strings: the bytes
    const types::Type* type_value = nullptr;   // type: the type it denotes; field/alternative: the reflected type
    const sema::Symbol* symbol   = nullptr;   // enumeration: its alternative; field: the field; alternative: the alternative;
                                               // function: the method; parameter: the function it belongs to
    std::vector<Value> elements;               // aggregate: fields in declaration order; list; enumeration: 0 or 1 payload
    std::optional<Span> location;              // a std::source_location: the code it names (spec 23)
    std::size_t         index = 0;             // parameter: its position

    static Value of_integer(u128 bits, const types::Type* t);
    static Value of_bool(bool b, const types::Type* t);
    static Value of_type(const types::Type* denoted, const types::Type* type_type);

    // The integer as a signed 128-bit number (sign-extended from the type's width when signed).
    [[nodiscard]] i128 as_signed() const;
};

// The f128 bits of a floating value: exact when it has them, else from its double.
inline u128 quad_of(const Value& v) { return v.quad ? *v.quad : quad::from_double(v.floating); }

// What build.ed describes (spec 25): filled in by the methods of `std::build::graph` while
// the interpreter runs `build(g)`.
struct BuildTarget {
    enum class Kind { executable, library, test, c_library, system_library, foreign, plugin };
    Kind                                     kind = Kind::executable;
    std::string                              name;
    std::string                              root;             // Edax: the main .ed file
    std::string                              language = "c";   // C targets: "c" or "cpp"
    bool                                     shared = false;   // C targets: a shared library (g.add_shared_library)
    std::string                              standard;          // "c17", "cpp20", ...
    std::vector<std::size_t>                 links;             // targets this one links
    std::vector<std::string>                 sources;           // C targets
    std::vector<std::pair<std::string, bool>> include_dirs;     // dir, shared with linkers
    std::vector<std::string>                 user_include_dirs; // `visibility::users`: only for what links it
    std::vector<std::pair<std::string, std::string>> defines;
    std::map<std::string, std::shared_ptr<const Value>> settings;
    std::vector<sema::FileSetting>           file_settings;     // `g.set_for_files(t, pattern, name, value)`
    const sema::Symbol*                      mode = nullptr;    // `g.set_mode(t, ...)`
    std::shared_ptr<const Value>             mode_optimization;   // that mode's optimization() (a std::build::optimization)
    Span                                     span;              // the call that added it
    std::vector<std::size_t>                 steps;             // `g.depends_on(t, s)`
    std::vector<std::size_t>                 after;             // `g.depends_on(t, first)`: built before, not linked
    std::string                              output_dir;        // `g.output_dir(t, pattern)` ("": the project's pattern)
    std::string                              project;           // the name of the project that declares it (its folder)
    // The folder its output goes to: `output_dir` (or the project's pattern) with the
    // placeholders filled in, absolute; set by the driver.
    std::string                              output_path;
    std::vector<std::string>                 details;           // `g.details(t, [...])`
    bool                                     has_details = false;
    std::string                              library_root;      // system libraries: `-L<root>/lib`
    std::vector<std::string>                 prebuilt;          // C libraries: files linked as they are (cmake_external)
    // Edax targets: the directories of the shared Edax libraries it links (their modules are
    // compiled there, not here); set by the driver.
    std::vector<std::string>                 shared_library_dirs;
    std::size_t                              dependency = 0;    // foreign: 1 + the dependency's index
    std::string                              base;              // the root its paths are relative to ("" = the project's)
    struct CImport {
        std::vector<std::string> headers;
        std::string              module;
        std::string              ns;
        Span                     span;
    };
    std::vector<CImport>                     c_imports;         // C targets: g.import_c_headers
    struct CppWrap {
        std::vector<std::string> headers, classes, templates;
        std::string              module;
        std::string              ns;
        Span                     span;
    };
    std::vector<CppWrap>                     cpp_wraps;         // C++ targets: g.wrap_cpp
};

struct BuildStep {
    std::string              name;
    std::string              program;
    std::vector<std::string> arguments;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    Span                     span;
};

struct BuildDependency {
    std::string kind;     // "git", "local" or "archive"
    std::string source;   // the url or the directory
    std::string commit;
    std::string dir;      // where it is on this machine
    // A package (g.package): fetched into `path` of the project, built per `build`.
    bool                                             is_package = false;
    std::string                                      name;
    std::string                                      path;
    std::string                                      tag;
    std::string                                      sha256;
    std::string                                      build = "automatic";   // automatic, edax, cmake, headers_only, script, cmake_external
    std::string                                      build_arg;             // headers_only: the include directory; script: its file
    std::vector<std::string>                         external_outputs;      // cmake_external: libraries, relative to the install dir
    std::vector<std::string>                         external_include_dirs; // cmake_external: include directories, likewise
    std::map<std::string, std::size_t>               exports;               // script: `p.export_target(t, name)`
    std::vector<std::pair<std::string, std::string>> options;
    std::vector<std::pair<std::string, std::size_t>> uses;      // find_package name → target index
    std::vector<std::string>                         patches;
    Span                                             span;
};

struct BuildOption {
    std::string name;
    std::string help;
    std::string type;     // the type's name, for --list
    std::string value;    // as text, for --list
};

// `g.output(std::build::output{...})`: how the build reports itself (build-output.md).
struct BuildOutput {
    bool                     set = false;
    std::string              progress;     // "automatic", "live", "lines", "quiet"
    std::vector<std::string> details;
    bool                     log_file = true;
    unsigned                 keep_logs = 5;
    std::string              log_dir;      // a g.output_dir pattern ("": the project's output folder)
    std::string              target_line, done_line, source_row, last_line, failed_line;   // "": the default
};

struct BuildGraph {
    BuildOutput                               output;
    std::vector<BuildTarget>                  targets;
    std::vector<const sema::Symbol*>          modes;      // `g.add_mode(...)`
    std::vector<BuildOption>                  options;
    std::map<std::string, std::string>        given;      // -Dname=value
    std::map<std::string, std::string>        env_read;   // recorded inputs
    std::string                               root;       // the project root (absolute)
    std::string                               out_dir;    // the build directory (absolute)
    std::string                               output_dir; // `g.output_dir(pattern)` ("": the default)
    std::vector<BuildStep>                    steps;
    std::vector<BuildDependency>              dependencies;
    std::map<std::string, std::vector<std::string>> globs;   // recorded inputs
    std::map<std::string, std::string>        programs;   // find_program: name → path
    // Fetches a dependency (`git clone` of the pinned commit, or a local directory) and returns
    // its directory; empty with `error` set on failure. Set by the driver.
    std::function<std::string(const BuildDependency&, std::string& error)> fetch;
};

// The placeholders of a `g.output_dir` pattern, in the order the docs list them: build, name,
// project, mode, kind, platform, os, arch, cpu.
const std::vector<std::string>& output_dir_placeholders();

// What is wrong with a `g.output_dir` pattern (an unknown placeholder, an unclosed `{`, nothing
// left), or "" when it is fine.
std::string check_output_dir(std::string_view pattern);

// `pattern` with every `{placeholder}` replaced by its value from `values` (a missing one by "").
std::string expand_output_dir(std::string_view pattern, const std::map<std::string, std::string>& values);

// Source-like text of a value, for messages: `42`, `"abc"`, `u32`, `point{ .x = 1.0 }`.
std::string to_string(const Value& v);

// Equality of two values of the same type (spec 26: also for type values).
bool equal(const Value& a, const Value& b);

// How the checker connects the evaluator to code it has not checked yet.
struct Hooks {
    // Makes sure a function's body is checked (on demand: it may be declared later). False if
    // it cannot be (for example because checking it is in progress: a cycle).
    std::function<bool(const sema::Symbol* function)> check_function;
    // Makes sure a field's default value is checked (a struct declared later or elsewhere).
    std::function<void(const sema::Symbol* field)> check_field;
    // The value of a `compile_time` constant (global or local), evaluating it if needed. Null
    // when it has no value (an error was reported, or it depends on a template parameter).
    std::function<std::shared_ptr<const Value>(const sema::Symbol* constant)> constant_value;
    // Lowers a type written in the code being checked (template parameters substituted).
    std::function<const types::Type*(const ast::TypeExpr& t)> lower;
    // Whether a compile-time value depends on a template parameter that is not known yet (the
    // generic check of a template): then the evaluation gives "unknown" without an error.
    std::function<bool(const sema::Symbol* s)> depends_on_template;
    // What a template parameter stands for in the code being checked (null when unknown).
    std::function<const types::Type*(const sema::Symbol* param)> substitute;
    // The fields of a struct/class that are visible to code in the checker's current module
    // (spec 26: reflection respects privacy).
    std::function<std::vector<const sema::Symbol*>(const types::Type* t)> visible_fields;
    // The methods of a type that code in the checker's current module can call: no destructors,
    // operators or method templates (spec 26).
    std::function<std::vector<const sema::Symbol*>(const types::Type* t)> visible_methods;
    // `std::implements<T, I>()`: whether `t` satisfies the interface or constraint `iface`
    // (user interfaces, std::comparable, std::sendable, ...).
    std::function<bool(const types::Type* t, const types::Type* iface)> implements;
    // Whether values of `t` borrow something (a reference, a view, a lambda with reference
    // captures, spec 12).
    std::function<bool(const types::Type* t)> is_view;
};

class Evaluator {
public:
    Evaluator(sema::Program& program, DiagnosticEngine& diags, Hooks hooks);
    ~Evaluator();

    // Evaluates a checked expression. Returns null when it could not be evaluated: then either an
    // error was reported, or `unknown()` is true (it depends on a template parameter).
    std::shared_ptr<const Value> evaluate(const ast::Expr& e, std::string_view why);
    [[nodiscard]] bool unknown() const { return unknown_; }

    // Where an evaluation runs, for the checks of spec 23.
    struct Context {
        // Code inside this function (an entry check): its parameters are read through `parameter`,
        // and types are lowered with its template substitution.
        const sema::Symbol* function = nullptr;
        // The value of a parameter of `function` (the call's argument); null when it is not known
        // at compile time (then the error, if any, was reported by the callback).
        std::function<std::shared_ptr<const Value>(const sema::Symbol* param)> parameter;
        // Whether the argument of a parameter of `function` is known at compile time, without
        // reporting anything (`std::is_compile_time(param)`).
        std::function<bool(const sema::Symbol* param)> parameter_known;
        // Where the argument of a parameter of `function` is written (`std::location_of(param)`).
        std::function<std::optional<Span>(const sema::Symbol* param)> parameter_site;
        // The text of a literal argument (`std::literal_text(param)`, spec 31): a string literal's
        // value, or an f-string's text with `{}` for each hole; none for other arguments.
        std::function<std::optional<std::string>(const sema::Symbol* param)> parameter_text;
        // Report nothing; give up silently instead (`compile_time run_time` checks).
        bool quiet = false;
        // Where `std::compile_error` / `std::compile_warning` report (the call of an entry check).
        std::optional<Span> site;
    };
    std::shared_ptr<const Value> evaluate_in(const ast::Expr& e, std::string_view why, Context context);

    // Runs the compile_time function `fn` with `args` (build.ed' `build(g)`, spec 25). Null
    // when it fails (the error is reported).
    std::shared_ptr<const Value> call(const sema::Symbol* fn, std::vector<Value> args, Span call_site);

    // The value of a setting of a build mode: runs the mode class's method `name` (spec 24).
    // Null when it fails (the error is reported).
    std::shared_ptr<const Value> mode_setting(const sema::Symbol* mode_class, const std::string& name);

    // The values a `for compile_time` iterates over: the elements of a list, or the integers of
    // a range. Null as for `evaluate`.
    std::optional<std::vector<Value>> elements(const ast::Expr& iterable);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool                  unknown_ = false;
};

// The `std::meta` function a call names (`fields`, `alternatives`, `is_struct`, ...) or the trait
// (`is_integer`, `size_of`, ...), or empty: `std::meta::fields<T>()` -> "meta::fields".
// The hash of an interface that a plugin and the program loading it must agree on: its name
// and its methods' signatures (dynamic libraries).
std::uint64_t interface_hash(const sema::Symbol* iface);

std::string builtin_name(const ast::CallExpr& c);

// The name of the `std::raw` memory intrinsic `c` calls (`"allocate"`, `"write"`, ...), or empty
// (spec 13). These run only at run time.
std::string raw_intrinsic(const ast::CallExpr& c);

} // namespace edax::ct
