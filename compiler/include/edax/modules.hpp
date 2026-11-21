#pragma once
// Module loading (spec 09): finds the header/source of the entry file, follows every `import`
// to `<root>/<a>/<b>.edh`, lexes and parses each file once, and rejects import cycles.
//
//   import geometry;          -> <root>/geometry.edh  (+ geometry.ed if present)
//   import shapes::circle;    -> <root>/shapes/circle.edh
//   import std; import std::x -> the standard library, built into edaxc (library/std/std.edh),
//                                loaded once as module "std". `std::` names it does not declare
//                                yet are accepted unchecked (the compiler knows them itself)

#include "edax/diagnostics.hpp"
#include "edax/symbols.hpp"

#include <string>
#include <vector>

namespace edax {

struct LoadOptions {
    std::string root;   // project root; empty = the entry file's directory
    // build.ed, loaded as module `build` next to the entry: its mode classes are what
    // `--mode=` and `run_time(build::profile)` name (spec 24, open question 31).
    std::string build_script;
    // More directories to find imported modules in, after `root`: the roots of dependencies
    // whose libraries a target links (spec 25).
    std::vector<std::string> extra_roots;
};

// Loads the entry module and its imports. Lexer, parser and import errors go to `diags`; the
// returned program contains whatever could be loaded.
sema::Program load_program(const std::string& entry_path, const LoadOptions& options,
                           SourceManager& sources, DiagnosticEngine& diags);

// "shapes/circle.edh" relative to the root -> "shapes::circle".
std::string module_name_for(const std::string& path, const std::string& root);

} // namespace edax
