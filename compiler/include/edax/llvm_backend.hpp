#pragma once
// LLVM backend. A checked program becomes LLVM IR, which LLVM compiles to an object file for the
// host; the system linker turns that into an executable. No C code is generated.
//
// Only this backend's translation units include LLVM headers. Everything in this header is plain
// C++, so the front end, the driver and the unit tests do not depend on LLVM.
//
// Layout rules for C/C++ interop (see claude/compiler_status.md, "Interop layout"): Edax types
// that have a C counterpart use the same size, alignment and field order as the C compiler's
// layout for the matching C type, and references and raw pointers are plain LLVM `ptr`.

#include "edax/diagnostics.hpp"
#include "edax/source.hpp"
#include "edax/symbols.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace edax {

// Something the compiler does that the source does not spell out (or only hints at): a stack
// slot, a heap allocation, a drop at the end of a scope, a move, a copy, a temporary, an implicit
// conversion, a virtual call, a run-time check. `edaxc insights` lists them for editors.
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
    // Generate every non-template function in the entry module, not only those reachable from
    // `main`. For testing the backend on code that is never run.
    bool all_functions = false;
    // `panic()` returning `trap` (spec 24): every panic is a trap instruction, without a message.
    bool panic_trap = false;
    // The mode's debug_info() is not `none`: DWARF line tables for every Edax function.
    bool debug_info = false;
    // The mode's debug_info() is `full`: also locals, parameters, `this` and their types, so
    // that a debugger can show variables (needs debug_info).
    bool debug_variables = false;
    // When set, the backend lists what it does implicitly here (`edaxc insights`).
    std::vector<Insight>* insights = nullptr;
};

// How the object code is made (the mode's settings, spec 24).
struct CodegenOptions {
    std::string optimization = "none";   // "none", "size", "speed"
    bool        debug_info   = false;    // (the IR carries it: LlvmOptions::debug_info)
    bool        shared       = false;    // a shared library (a plugin) instead of a program
    std::string sanitize     = "none";   // the mode's sanitize() (spec 34): "none", "address", "thread"
    // Profile-guided optimization (build modes): `edaxc build --profile-generate` instruments the
    // program to write raw profiles to this pattern (`%p` is the process id); the mode's
    // profile_guided() names a merged profile (.profdata) to optimize with.
    std::string profile_generate;
    std::string profile_use;
};

// The checked program as an LLVM module with the runtime linked in, ready for build_executable
// (no LLVM types here: the module stays inside the backend). Empty after errors.
class GeneratedProgram {
public:
    GeneratedProgram();
    ~GeneratedProgram();
    GeneratedProgram(GeneratedProgram&&) noexcept;
    GeneratedProgram& operator=(GeneratedProgram&&) noexcept;
    [[nodiscard]] bool empty() const { return !impl_; }

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

GeneratedProgram generate_llvm_program(sema::Program& program, const SourceManager& sources, DiagnosticEngine& diags,
                                       LlvmOptions options = {});

// Lowers the checked program to textual LLVM IR (LLVM 18 syntax). Returns an empty string when
// errors were added to `diags`.
std::string generate_llvm_ir(sema::Program& program, const SourceManager& sources, DiagnosticEngine& diags,
                             LlvmOptions options = {});

// A run-time check of the program (spec 34 §5, `edaxc explain-checks`): where it reports, what it
// checks, and whether the optimizer (-O2) kept it.
struct CheckSite {
    std::string              where;   // "file:line:column"
    std::vector<std::string> what;    // the messages, or "the checks of <function>"
    bool                     kept = false;
};

// Generates the program, optimizes it like a release build and lists its checks.
// `file`'s functions are not inlined into each other: each is looked at for any arguments.
std::vector<CheckSite> explain_checks(sema::Program& program, const SourceManager& sources, DiagnosticEngine& diags,
                                      const std::string& file, LlvmOptions options = {});

// Compiles LLVM IR text to an object file for the host and links an executable at `out_path`
// with `linker` (for example "cc"). Returns false and sets `error` on failure.
// Compiles for `platform` (an LLVM triple such as "aarch64-linux-gnu", spec 25 `--target=`);
// empty: the host. False with `error` set when edaxc can't compile for it.
bool set_build_target(const std::string& platform, std::string& error);

// Whether programs built for the current target can run here.
bool target_is_host();

// The platform being compiled for (build system, "Target platforms"): its triple, operating
// system, CPU level, the CPU features that level includes and the capabilities.
struct TargetPlatform {
    std::string              triple;         // "x86_64-pc-linux-gnu"
    std::string              os;             // "linux", "windows", "macos" or "none"
    std::string              cpu;            // the CPU level: "x86_64_v3", "armv8", ...
    std::vector<std::string> features;       // the level's CPU features: "avx2", "fma", "neon", ...
    std::vector<std::string> capabilities;   // "os", "threads", "heap", "files", "dynamic_libraries", "simd"
    unsigned                 vector_bytes = 16;   // the widest vector register of the level
    unsigned                 cache_line   = 64;   // bytes per cache line (a description's 0: the architecture's)
    bool has(const std::string& capability) const;
    bool has_feature(const std::string& feature) const;
};

// The current target platform.
const TargetPlatform& target_platform();

// The platform edaxc runs on (for `g.host()`).
TargetPlatform host_platform();

// Sets the CPU level (`--cpu=x86_64_v3`); empty: the baseline of the architecture. False with
// `error` set when the level doesn't exist or doesn't fit the target's architecture.
bool set_build_cpu(const std::string& level, std::string& error);

// The CPU level set with set_build_cpu (empty: the baseline).
const std::string& build_cpu();

// The CPU levels edaxc knows, for the architecture of the target (for messages).
std::vector<std::string> cpu_levels();

// The CPU features of a level, or none when it is unknown.
std::vector<std::string> cpu_level_features(const std::string& level);

// Checks and generates code for `level` instead of the build's CPU level (a multiversion copy,
// SIMD "Choosing widths"); empty: back to the build's. Returns the previous override.
std::string set_cpu_level_override(const std::string& level);

// The widest vector register of a level in bytes (16 when it is unknown).
unsigned cpu_level_vector_bytes(const std::string& level);

// A user target description (`--target=board.ed`): replaces the platform's triple,
// operating system, CPU level and capabilities. False with `error` set when edaxc can't compile
// for the triple or the level doesn't fit it.
bool set_target_description(const TargetPlatform& description, std::string& error);

// The LLVM version edaxc is built with ("18.1.3").
std::string llvm_version();

bool build_executable(std::string_view ir, const std::string& out_path, const std::string& linker, std::string& error,
                      const CodegenOptions& codegen = {}, const std::vector<std::string>& extra_inputs = {});
// The same for a generated module (no printing and parsing of the IR); consumes `program`.
bool build_executable(GeneratedProgram& program, const std::string& out_path, const std::string& linker, std::string& error,
                      const CodegenOptions& codegen = {}, const std::vector<std::string>& extra_inputs = {});

// The runtime every generated module is linked with, as LLVM IR text (llvm_runtime.cpp).
std::string llvm_runtime_ir();

} // namespace edax
