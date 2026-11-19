#pragma once
// LLVM backend. A checked program becomes LLVM IR, which LLVM compiles to an object file for the
// host; the system linker turns that into an executable. No C code is generated.
//
// Only this backend's translation units include LLVM headers. Everything in this header is plain
// C++, so the front end, the driver and the unit tests do not depend on LLVM.
//
// Layout rules for C/C++ interop (see claude/compiler_status.md, "Interop layout"): Rift types
// that have a C counterpart use the same size, alignment and field order as the C compiler's
// layout for the matching C type, and references and raw pointers are plain LLVM `ptr`.

#include "rift/diagnostics.hpp"
#include "rift/source.hpp"
#include "rift/symbols.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace rift {

/// Something the compiler does that the source does not spell out (or only hints at): a stack
/// slot, a heap allocation, a drop at the end of a scope, a move, a copy, a temporary, an implicit
/// conversion, a virtual call, a run-time check. `riftc insights` lists them for editors.
struct Insight {
    std::string category;    // stack, heap, drops, moves, copies, temporaries, conversions, dispatch, checks
    std::string kind;        // finer: frame, alloc, free, drop, move, copy, ...
    Span        span;        // where it happens
    std::string placement;   // "range": highlight span; "above": a line of its own above span's line;
                             // "after": text at the end of span's line
    std::string message;     // one line
    std::string detail;      // more, for a hover (Markdown)
};

struct LlvmOptions {
    /// Generate every non-template function in the entry module, not only those reachable from
    /// `main`. For testing the backend on code that is never run.
    bool all_functions = false;
    /// `panic()` returning `trap` (spec 24): every panic is a trap instruction, without a message.
    bool panic_trap = false;
    /// The mode's debug_info() is not `none`: DWARF line tables for every Rift function.
    bool debug_info = false;
    /// The mode's debug_info() is `full`: also locals, parameters, `this` and their types, so
    /// that a debugger can show variables (needs debug_info).
    bool debug_variables = false;
    /// When set, the backend lists what it does implicitly here (`riftc insights`).
    std::vector<Insight>* insights = nullptr;
};

/// How the object code is made (the mode's settings, spec 24).
struct CodegenOptions {
    std::string optimization = "none";   // "none", "size", "speed"
    bool        debug_info   = false;    // (the IR carries it: LlvmOptions::debug_info)
};

/// Lowers the checked program to textual LLVM IR (LLVM 18 syntax). Returns an empty string when
/// errors were added to `diags`.
std::string generate_llvm_ir(sema::Program& program, const SourceManager& sources, DiagnosticEngine& diags,
                             LlvmOptions options = {});

/// Compiles LLVM IR text to an object file for the host and links an executable at `out_path`
/// with `linker` (for example "cc"). Returns false and sets `error` on failure.
/// Compiles for `platform` (an LLVM triple such as "aarch64-linux-gnu", spec 25 `--target=`);
/// empty: the host. False with `error` set when riftc can't compile for it.
bool set_build_target(const std::string& platform, std::string& error);

/// Whether programs built for the current target can run here.
bool target_is_host();

bool build_executable(std::string_view ir, const std::string& out_path, const std::string& linker, std::string& error,
                      const CodegenOptions& codegen = {}, const std::vector<std::string>& extra_inputs = {});

/// The runtime every generated module is linked with, as LLVM IR text (llvm_runtime.cpp).
std::string llvm_runtime_ir();

} // namespace rift
