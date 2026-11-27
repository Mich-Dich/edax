#pragma once
// Name resolution (M3): builds every module's symbol table, merges headers with sources
// (spec 09), checks class hierarchies (spec 07) and resolves every name in every body
// (spec 03, 06, 10, 12, 18). Resolved symbols are stored in the AST (`symbol` fields).

#include "edax/diagnostics.hpp"
#include "edax/symbols.hpp"

#include <iosfwd>
#include <set>
#include <string>

namespace edax {

void resolve_program(sema::Program& program, DiagnosticEngine& diags);

// The names directly in `std` that the compiler provides itself (println, size_of, raw_ptr, ...):
// an undeclared `std::x` is an error unless it is one of them, or in std::meta, std::raw,
// std::simd or std::c_library. A new builtin `std::` name must be added here.
const std::set<std::string>& compiler_std_names();

// Resolves an instance of the template type `templ` (spec 14): `decl` is a fresh copy of the
// template's declaration, which must outlive the program. Returns the instance's symbol (not
// registered in the module); `params` receives the symbols of its template parameters.
sema::Symbol* resolve_instance(sema::Program& program, DiagnosticEngine& diags, const sema::Symbol* templ,
                               ast::Decl& decl, std::vector<sema::Symbol*>& params);

// Resolves an instance of the function template `templ` (spec 14): `decl` is a fresh copy of the
// template's function, signature and body included. Same contract as resolve_instance.
// For a method template, `self` is its class: the copy is resolved as a method.
sema::Symbol* resolve_function_instance(sema::Program& program, DiagnosticEngine& diags, const sema::Symbol* templ,
                                        ast::FunctionDecl& decl, std::vector<sema::Symbol*>& params, sema::Symbol* self = nullptr);

// `edaxc symbols`: the merged module interface, members in layout order.
void print_symbols(std::ostream& out, const sema::Module& module, const SourceManager& sources);

// `edaxc expand <file> <type>` (spec 29): the type as written, followed by every member its
// annotations generate, written out. False when the program has no such type.
bool print_expansion(std::ostream& out, const sema::Program& program, const SourceManager& sources, std::string_view type);

} // namespace edax
