#pragma once
// C++ classes through generated wrappers (C interop): `g.wrap_cpp(target, std::build::cpp_wrap{...})`.
// The classes are read from clang's JSON AST of the headers; the generator writes C wrapper
// functions (compiled into the C++ target, catching every exception) and a Edax module with a
// class per C++ class.

#include <string>
#include <utility>
#include <vector>

namespace edax {

struct CppWrapOptions {
    std::vector<std::string> headers;     // as `#include "..."` sees them
    std::vector<std::string> classes;     // qualified: "JPH::PhysicsSystem"
    std::vector<std::string> templates;   // instances, written out: "JPH::Ref<JPH::Shape>"
    std::string              module;
    std::string              ns;
};

// The translation unit clang reads: the headers and an explicit instantiation of each listed
// template instance (so its members exist in the AST).
std::string cpp_wrap_probe(const CppWrapOptions& options);

// The name clang's `-ast-dump-filter` gets for a class or template instance.
std::string cpp_wrap_filter(const std::string& name);

struct CppWrapResult {
    std::string                                      header;    // <module>.edh
    std::string                                      source;    // <module>.ed
    std::string                                      wrappers;  // the C++ source of the C wrappers
    std::vector<std::pair<std::string, std::string>> report;    // what, why / its Edax name
    std::string                                      error;     // a class that isn't found
};

// Generates the wrappers from clang's JSON AST dumps, one per listed class and template (in
// the order of `classes`, then `templates`).
CppWrapResult cpp_wrap_generate(const CppWrapOptions& options, const std::vector<std::string>& dumps);

} // namespace edax
