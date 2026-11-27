#include "edax/simd_intrinsics.hpp"
#include "edax/llvm_backend.hpp"
#include "edax/lower_aggregates.hpp"
#include "edax/thin_lto.hpp"
#include "edax/std_library.hpp"
#include "edax/ast_walk.hpp"

#include "edax/ast_printer.hpp"
#include "edax/compile_time.hpp"
#include "edax/types.hpp"

#include <functional>
#include <regex>

#include <filesystem>
#include "llvm/ADT/APSInt.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAArch64.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/PGOOptions.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Transforms/Instrumentation/AddressSanitizer.h"
#include "llvm/Transforms/Instrumentation/ThreadSanitizer.h"
#include "llvm/Support/BLAKE3.h"
#include "llvm/Support/CachePruning.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/IPO/ArgumentPromotion.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <thread>
#include <tuple>
#include <vector>

#ifdef EDAX_VERSION
#define EDAX_VERSION_TEXT EDAX_VERSION
#else
#define EDAX_VERSION_TEXT "0.0.0-dev"
#endif

namespace edax {
namespace {

using namespace ast;
using sema::Module;
using sema::Symbol;
using sema::SymbolKind;
using types::Kind;

// The lines of `text`, each with its newline.
std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t                   start = 0;
    while (start < text.size()) {
        std::size_t end  = text.find('\n', start);
        std::size_t next = end == std::string_view::npos ? text.size() : end + 1;
        out.push_back(text.substr(start, next - start));
        start = next;
    }
    return out;
}

// --- LLVM versions -------------------------------------------------------------------------
// edaxc builds with LLVM 18 to 21; the few APIs that changed between them go through these.

llvm::Function* intrinsic_declaration(llvm::Module* m, llvm::Intrinsic::ID id, llvm::ArrayRef<llvm::Type*> types = {}) {
#if LLVM_VERSION_MAJOR >= 20
    return llvm::Intrinsic::getOrInsertDeclaration(m, id, types);
#else
    return llvm::Intrinsic::getDeclaration(m, id, types);
#endif
}

const llvm::Target* lookup_target(const std::string& triple, std::string& error) {
#if LLVM_VERSION_MAJOR >= 21
    return llvm::TargetRegistry::lookupTarget(llvm::Triple(triple), error);
#else
    return llvm::TargetRegistry::lookupTarget(triple, error);
#endif
}

// The LLVM CPU name and feature string of the CPU level being compiled for.
std::pair<std::string, std::string> llvm_cpu_and_features();

llvm::TargetMachine* make_target_machine(const llvm::Target* target, const std::string& triple, const llvm::TargetOptions& options,
                                         llvm::CodeGenOptLevel level) {
    auto [cpu, features] = llvm_cpu_and_features();
#if LLVM_VERSION_MAJOR >= 21
    return target->createTargetMachine(llvm::Triple(triple), cpu, features, options, llvm::Reloc::PIC_, std::nullopt, level);
#else
    return target->createTargetMachine(triple, cpu, features, options, llvm::Reloc::PIC_, std::nullopt, level);
#endif
}

void set_target_triple(llvm::Module& m, const std::string& triple) {
#if LLVM_VERSION_MAJOR >= 21
    m.setTargetTriple(llvm::Triple(triple));
#else
    m.setTargetTriple(triple);
#endif
}

} // namespace
} // namespace edax

// The targets edaxc can cross-compile for (spec 25: `--target=`), besides the host.
extern "C" {
void LLVMInitializeAArch64TargetInfo();
void LLVMInitializeAArch64Target();
void LLVMInitializeAArch64TargetMC();
void LLVMInitializeAArch64AsmPrinter();
void LLVMInitializeAArch64AsmParser();
void LLVMInitializeX86TargetInfo();
void LLVMInitializeX86Target();
void LLVMInitializeX86TargetMC();
void LLVMInitializeX86AsmPrinter();
void LLVMInitializeX86AsmParser();
void LLVMInitializeRISCVTargetInfo();
void LLVMInitializeRISCVTarget();
void LLVMInitializeRISCVTargetMC();
void LLVMInitializeRISCVAsmPrinter();
void LLVMInitializeRISCVAsmParser();
}

namespace edax {
namespace {

// The platform being compiled for: empty for the host (spec 25, `--target=`).
std::string g_target_triple;

// The LLVM triple edaxc compiles for.
std::string target_triple() { return g_target_triple.empty() ? llvm::sys::getDefaultTargetTriple() : g_target_triple; }

// A CPU level (build system, "Target platforms"; SIMD, "Choosing widths and CPU levels").
struct CpuLevel {
    const char*              name;
    llvm::Triple::ArchType   arch;
    const char*              llvm_cpu;
    const char*              llvm_features;
    std::vector<std::string> features;
    unsigned                 vector_bytes;
};

const std::vector<CpuLevel>& cpu_level_table() {
    static const std::vector<std::string> v1 = {"sse", "sse2"};
    static const std::vector<std::string> v2 = [] {
        std::vector<std::string> f = v1;
        f.insert(f.end(), {"sse3", "ssse3", "sse4_1", "sse4_2", "popcnt"});
        return f;
    }();
    static const std::vector<std::string> v3 = [] {
        std::vector<std::string> f = v2;
        f.insert(f.end(), {"avx", "avx2", "fma", "bmi1", "bmi2", "f16c", "lzcnt"});
        return f;
    }();
    static const std::vector<std::string> v4 = [] {
        std::vector<std::string> f = v3;
        f.insert(f.end(), {"avx512f", "avx512bw", "avx512cd", "avx512dq", "avx512vl"});
        return f;
    }();
    static const std::vector<CpuLevel> table = {
        {"x86_64", llvm::Triple::x86_64, "x86-64", "", v1, 16},
        {"x86_64_v2", llvm::Triple::x86_64, "x86-64-v2", "", v2, 16},
        {"x86_64_v3", llvm::Triple::x86_64, "x86-64-v3", "", v3, 32},
        {"x86_64_v4", llvm::Triple::x86_64, "x86-64-v4", "", v4, 64},
        {"armv8", llvm::Triple::aarch64, "generic", "+neon", {"neon"}, 16},
        {"armv9", llvm::Triple::aarch64, "generic", "+neon,+v9a,+sve,+sve2", {"neon", "sve", "sve2"}, 16},
        {"riscv64", llvm::Triple::riscv64, "generic-rv64", "+m,+a,+f,+d,+c", {}, 8},
        {"riscv64_v", llvm::Triple::riscv64, "generic-rv64", "+m,+a,+f,+d,+c,+v", {"rvv"}, 16},
    };
    return table;
}

// The CPU level chosen with `--cpu=` or a target description; empty: the architecture's baseline.
std::string g_cpu_level;

// Capabilities, operating system and triple from a user target description; empty: the platform's own.
std::optional<TargetPlatform> g_description;

const CpuLevel* find_cpu_level(const std::string& name) {
    for (const CpuLevel& l : cpu_level_table()) {
        if (name == l.name) return &l;
    }
    return nullptr;
}

// The level a multiversion copy is checked and generated for (SIMD, "Choosing widths").
std::string g_level_override;

// The level in effect: the chosen one, else the first of the target's architecture.
const CpuLevel* current_cpu_level() {
    if (const CpuLevel* l = find_cpu_level(g_level_override)) return l;
    if (const CpuLevel* l = find_cpu_level(g_cpu_level)) return l;
    llvm::Triple t(target_triple());
    for (const CpuLevel& l : cpu_level_table()) {
        if (l.arch == t.getArch()) return &l;
    }
    return nullptr;
}

// The number of `level` among the target's levels (1 the first); 0 when it isn't one.
int cpu_level_number(const std::string& level) {
    llvm::Triple t(target_triple());
    int          n = 0;
    for (const CpuLevel& l : cpu_level_table()) {
        if (l.arch != t.getArch()) continue;
        ++n;
        if (level == l.name) return n;
    }
    return 0;
}

std::pair<std::string, std::string> llvm_cpu_and_features() {
    const CpuLevel* l = current_cpu_level();
    if (!l) return {"generic", ""};
    return {l->llvm_cpu, l->llvm_features};
}

void initialize_targets() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();   // inline assembly: cpuid for @std::simd::multiversion
    static bool cross = false;
    if (g_target_triple.empty() || cross) return;
    cross = true;
    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmPrinter();
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();
    LLVMInitializeRISCVTargetInfo();
    LLVMInitializeRISCVTarget();
    LLVMInitializeRISCVTargetMC();
    LLVMInitializeRISCVAsmPrinter();
    LLVMInitializeAArch64AsmParser();
    LLVMInitializeX86AsmParser();
    LLVMInitializeRISCVAsmParser();
}

// The data layout of the target, which decides the sizes and alignments of enum payloads.
llvm::DataLayout host_data_layout() {
    initialize_targets();
    std::string triple = target_triple();
    std::string error;
    const llvm::Target* target = lookup_target(triple, error);
    if (!target) llvm::report_fatal_error("internal error: no LLVM target for the host: " + llvm::Twine(error));
    std::unique_ptr<llvm::TargetMachine> machine(make_target_machine(target, triple, llvm::TargetOptions(), llvm::CodeGenOptLevel::Default));
    return machine->createDataLayout();
}

// --- names ----------------------------------------------------------------------------------

// FNV-1a: short, stable hashes for names (not for the object cache's keys, which use BLAKE3).
std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t h = 1469598103934665603ull;
    for (char ch : text) {
        auto c = static_cast<unsigned char>(ch);
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// Length-prefixed name parts, as in the C backend: `r_3geo6shapes1f`.
std::string mangle_parts(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& p : parts) out += std::to_string(p.size()) + p;
    return out;
}

std::vector<std::string> split_module(const std::string& name) {
    std::vector<std::string> out;
    std::size_t              start = 0;
    while (true) {
        std::size_t pos = name.find("::", start);
        out.push_back(name.substr(start, pos - start));
        if (pos == std::string::npos) break;
        start = pos + 2;
    }
    return out;
}

// The name parts that make a symbol unique: its module (a file) and its namespace (spec 30).
std::vector<std::string> symbol_scope(const sema::Symbol* s) {
    std::vector<std::string> out = split_module(s->module ? s->module->name : "");
    if (!s->ns.empty()) {
        for (std::string& part : split_module(s->ns)) out.push_back(std::move(part));
    }
    return out;
}

// Thrown after an E6001 was reported for a construct the backend does not handle yet. The
// function being generated is abandoned; the module is discarded because errors were reported.
struct Unsupported {};

class Gen {
public:
    Gen(sema::Program& program, const SourceManager& sources, DiagnosticEngine& diags, LlvmOptions options,
        llvm::LLVMContext& ctx)
        : program_(program), tc_(*program.types), sources_(sources), diags_(diags), options_(options), ctx_(ctx),
          mod_(std::make_unique<llvm::Module>("edax", ctx)), b_(ctx) {
        mod_->setDataLayout(host_data_layout());
        if (options_.debug_info) {
            dib_ = std::make_unique<llvm::DIBuilder>(*mod_);
            mod_->addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
            mod_->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 5);
        }
    }

    // --- debug info (spec 24: the mode's debug_info()) -----------------------------------------

    std::unique_ptr<llvm::DIBuilder>     dib_;
    llvm::DICompileUnit*                 unit_ = nullptr;
    std::map<FileId, llvm::DIFile*>      di_files_;

    llvm::DIFile* di_file(FileId id) {
        if (auto it = di_files_.find(id); it != di_files_.end()) return it->second;
        std::filesystem::path p(sources_.file(id).path());
        std::error_code ec;
        if (p.string().starts_with("<std>/")) {   // the library's files: installed with edaxc, or in the tree it was built from, if it is still there
            std::filesystem::path tree = std::filesystem::path(std_source_dir()) / p.string().substr(6);
            p = std::filesystem::exists(tree, ec) ? tree : std::filesystem::path("/<std>") / p.string().substr(6);
        }
        std::filesystem::path abs = p.is_absolute() || p.string().starts_with("<") ? p : std::filesystem::absolute(p, ec);
        llvm::DIFile* f = dib_->createFile(abs.filename().string(), abs.parent_path().string());
        if (!unit_) {
            unit_ = dib_->createCompileUnit(llvm::dwarf::DW_LANG_C_plus_plus, f, "edaxc " EDAX_VERSION, false, "", 0);
        }
        di_files_[id] = f;
        return f;
    }

    // A DISubprogram for `fn`, written at `span`. With `full` debug info (spec 24) the function is
    // named as written, `module::type::name`, which is also its linkage name.
    void debug_function(llvm::Function* fn, Span span, const std::string& name) {
        if (!dib_ || !fn) return;
        llvm::DIFile* file = di_file(span.file);
        unsigned      line = sources_.file(span.file).line_col(span.begin).line;
        llvm::DISubroutineType* type = dib_->createSubroutineType(dib_->getOrCreateTypeArray({}));
        std::string linkage = options_.debug_variables ? name : fn->getName().str();
        // A function the debugger steps through has no line at all: the function's start is
        // written with its scope line.
        bool through = step_through(fn, span.file);
        unsigned shown = through ? 0 : line;
        llvm::DISubprogram* sp = dib_->createFunction(file, name, linkage, file, shown, type, shown,
                                                      llvm::DINode::FlagPrototyped, llvm::DISubprogram::SPFlagDefinition);
        fn->setSubprogram(sp);
        // The prologue (storing the parameters) has no line, as in C/C++ at -O0: stepping into the
        // function stops at its first statement, with the parameters in place (spec 24).
        bool no_line = options_.debug_variables || through;
        b_.SetCurrentDebugLocation(llvm::DILocation::get(ctx_, no_line ? 0 : line, 0, sp));
    }

    // --- debug info for variables (spec 24: debug_info() == full) ---------------------------

    std::map<const types::Type*, llvm::DIType*> di_types_;
    std::map<std::string, llvm::DINamespace*>   di_namespaces_;

    // The namespace of a module (`std`, `geometry`), nested for `std::filesystem`.
    llvm::DIScope* di_namespace(const std::string& module) {
        if (module.empty()) return unit_;
        if (auto it = di_namespaces_.find(module); it != di_namespaces_.end()) return it->second;
        std::size_t   cut    = module.rfind("::");
        llvm::DIScope* parent = cut == std::string::npos ? static_cast<llvm::DIScope*>(unit_) : di_namespace(module.substr(0, cut));
        auto* ns = dib_->createNameSpace(parent, cut == std::string::npos ? module : module.substr(cut + 2), false);
        di_namespaces_[module] = ns;
        return ns;
    }

    llvm::DIType* di_basic(const std::string& edax_name, const std::string& c_name, unsigned bits, unsigned encoding) {
        llvm::DIType* base = dib_->createBasicType(c_name, bits, encoding);
        return dib_->createTypedef(base, edax_name, nullptr, 0, unit_);
    }

    // The DWARF type of a Edax type: built-in types are typedefs with their Edax names, structs
    // and classes follow the backend's layout, simple enums are enumerations, enums with payloads
    // are `struct { tag; union { ... } payload; }`.
    llvm::DIType* di_type(const types::Type* t) {
        if (!t) return nullptr;
        if (auto it = di_types_.find(t); it != di_types_.end()) return it->second;
        const llvm::DataLayout& dl = mod_->getDataLayout();
        llvm::DIType* out = nullptr;
        switch (t->kind) {
        case Kind::int_: {
            std::string c = t->bits == 8 ? "char" : t->bits == 16 ? "short" : t->bits == 32 ? "int" : t->bits == 64 ? "long" : "__int128";
            out = di_basic(t->name, (t->is_signed ? "" : "unsigned ") + c, static_cast<unsigned>(t->bits),
                           t->is_signed ? llvm::dwarf::DW_ATE_signed : llvm::dwarf::DW_ATE_unsigned);
            break;
        }
        case Kind::float_: out = di_basic(t->name, t->bits == 16 ? "_Float16" : t->bits == 32 ? "float" : "double", static_cast<unsigned>(t->bits), llvm::dwarf::DW_ATE_float); break;
        case Kind::bool_: out = di_basic("bool", "bool", 8, llvm::dwarf::DW_ATE_boolean); break;
        case Kind::char_: out = di_basic("char", "char32_t", 32, llvm::dwarf::DW_ATE_UTF); break;
        case Kind::reference:
            if (is_interface(t->inner)) {   // an object and its interface's table
                llvm::DIType* p = dib_->createPointerType(nullptr, 64);
                out = dib_->createStructType(unit_, types::to_string(t), nullptr, 0, 128, 64, llvm::DINode::FlagZero, nullptr,
                                             dib_->getOrCreateArray({dib_->createMemberType(unit_, "object", nullptr, 0, 64, 64, 0, llvm::DINode::FlagZero, p),
                                                                     dib_->createMemberType(unit_, "table", nullptr, 0, 64, 64, 64, llvm::DINode::FlagZero, p)}));
                break;
            }
            out = dib_->createReferenceType(llvm::dwarf::DW_TAG_reference_type, di_type(t->inner), 64);
            break;
        case Kind::pointer:
            if (is_interface(t->inner)) out = dib_->createUnspecifiedType(types::to_string(t));
            else out = dib_->createPointerType(di_type(t->inner), 64, 0, std::nullopt, types::to_string(t));
            break;
        case Kind::array: {
            llvm::Type* lt = lower_type(t);
            llvm::Metadata* range = dib_->getOrCreateSubrange(0, static_cast<std::int64_t>(lt->getArrayNumElements()));
            out = dib_->createArrayType(dl.getTypeAllocSizeInBits(lt), static_cast<std::uint32_t>(dl.getABITypeAlign(lt).value() * 8),
                                        di_type(t->inner), dib_->getOrCreateArray({range}));
            break;
        }
        case Kind::user:
            if (t->symbol->kind == SymbolKind::interface) {
                out = dib_->createUnspecifiedType(types::to_string(t));
                break;
            }
            out = di_struct(t);
            break;
        case Kind::tuple: {   // a struct whose members are named by position
            auto*                      st = llvm::cast<llvm::StructType>(lower_type(t));
            const llvm::StructLayout*  sl = dl.getStructLayout(st);
            std::vector<llvm::Metadata*> members;
            for (std::size_t i = 0; i < t->args.size(); ++i) {
                llvm::Type* et = st->getElementType(static_cast<unsigned>(i));
                members.push_back(dib_->createMemberType(unit_, std::to_string(i), nullptr, 0, dl.getTypeAllocSizeInBits(et),
                                                         static_cast<std::uint32_t>(dl.getABITypeAlign(et).value() * 8),
                                                         sl->getElementOffsetInBits(static_cast<unsigned>(i)), llvm::DINode::FlagZero,
                                                         di_type(t->args[i])));
            }
            out = dib_->createStructType(unit_, types::to_string(t), nullptr, 0, dl.getTypeAllocSizeInBits(st),
                                         static_cast<std::uint32_t>(dl.getABITypeAlign(st).value() * 8), llvm::DINode::FlagZero,
                                         nullptr, dib_->getOrCreateArray(members));
            break;
        }
        case Kind::static_str:
        case Kind::external:
        case Kind::closure:
        case Kind::function:
            out = di_struct(t);
            break;
        default: out = dib_->createUnspecifiedType(types::to_string(t)); break;
        }
        di_types_[t] = out;
        return out;
    }

    // A struct (or class, enum with payloads, `std::str`, lambda) as a DWARF structure.
    llvm::DIType* di_struct(const types::Type* t) {
        llvm::Type* lt = lower_type(t);
        const llvm::DataLayout& dl = mod_->getDataLayout();
        std::string ns   = t->is(Kind::user) ? t->symbol->ns : "";
        std::string name = types::to_string(t);
        if (!ns.empty() && name.starts_with(ns + "::")) name = name.substr(ns.size() + 2);
        llvm::DIScope* scope = di_namespace(t->is(Kind::user) ? ns : (t->is(Kind::external) || t->is(Kind::static_str) ? "std" : ""));
        if (t->is(Kind::static_str)) name = "static_str";
        if (t->is(Kind::external) && name.starts_with("std::")) name = name.substr(5);
        std::uint64_t size  = lt->isSized() ? dl.getTypeAllocSizeInBits(lt).getFixedValue() : std::uint64_t{0};
        std::uint32_t align = lt->isSized() ? static_cast<std::uint32_t>(dl.getABITypeAlign(lt).value() * 8) : 8;
        if (t->is(Kind::user) && t->symbol->kind == SymbolKind::enum_ && !has_payloads(t->symbol)) {
            std::vector<llvm::Metadata*> values;
            for (const Symbol* a : t->symbol->member_order) {
                if (!a->alternative) continue;
                values.push_back(dib_->createEnumerator(a->name, llvm::APSInt(tag_constant(a)->getValue(), true)));
            }
            return dib_->createEnumerationType(scope, name, nullptr, 0, size, align, dib_->getOrCreateArray(values),
                                               di_type(tc_.lower(*t->symbol->decl->as<EnumDecl>().tag_type, sema::substitution_of(t->symbol))));
        }
        llvm::DICompositeType* fwd = dib_->createReplaceableCompositeType(llvm::dwarf::DW_TAG_structure_type, name, scope, nullptr, 0, 0,
                                                                          size, align);
        di_types_[t] = fwd;   // a field may refer back to the type (through a pointer)
        std::vector<llvm::Metadata*> members;
        auto* st = llvm::dyn_cast<llvm::StructType>(lt);
        const llvm::StructLayout* layout = st && !st->isOpaque() ? dl.getStructLayout(st) : nullptr;
        auto member = [&](const std::string& n, unsigned index, llvm::DIType* type) {
            if (!layout || index >= st->getNumElements() || !type) return;
            llvm::Type* et = st->getElementType(index);
            members.push_back(dib_->createMemberType(fwd, n, nullptr, 0, dl.getTypeAllocSizeInBits(et),
                                                     static_cast<std::uint32_t>(dl.getABITypeAlign(et).value() * 8),
                                                     layout->getElementOffsetInBits(index), llvm::DINode::FlagZero, type));
        };
        if (t->is(Kind::static_str) || (t->is(Kind::external) && t->name == "std::str")) {
            member("data", 0, dib_->createPointerType(di_type(tc_.builtin("u8")), 64));
            member("len", 1, di_type(tc_.builtin("usize")));
        } else if (t->is(Kind::user) && t->symbol->kind == SymbolKind::enum_ && layout) {
            const Symbol* e = t->symbol;
            std::vector<llvm::Metadata*> alts;
            bool        niche   = tc_.niche_enum(e).has_value();   // no tag field (spec 34)
            llvm::Type* storage = st->getElementType(niche ? 0 : 1);
            for (const Symbol* a : e->member_order) {
                if (!carries(a)) continue;
                llvm::DIType* pt = di_type(payload_of(a));
                llvm::Type*   pl = lower_type(payload_of(a));
                alts.push_back(dib_->createMemberType(nullptr, a->name, nullptr, 0, dl.getTypeAllocSizeInBits(pl),
                                                      static_cast<std::uint32_t>(dl.getABITypeAlign(pl).value() * 8), 0, llvm::DINode::FlagZero, pt));
            }
            std::vector<llvm::Metadata*> values;
            for (const Symbol* a : e->member_order) {
                if (a->alternative) values.push_back(dib_->createEnumerator(a->name, llvm::APSInt(tag_constant(a)->getValue(), true)));
            }
            if (!niche) {
                llvm::DIType* tag = dib_->createEnumerationType(scope, name + "::tag", nullptr, 0, dl.getTypeAllocSizeInBits(st->getElementType(0)),
                                                                8, dib_->getOrCreateArray(values), nullptr);
                member("tag", 0, tag);
            }
            llvm::DIType* u = dib_->createUnionType(scope, name + "::payload", nullptr, 0, dl.getTypeAllocSizeInBits(storage),
                                                    static_cast<std::uint32_t>(dl.getABITypeAlign(storage).value() * 8), llvm::DINode::FlagZero,
                                                    dib_->getOrCreateArray(alts));
            member("payload", niche ? 0 : 1, u);
        } else if (t->is(Kind::user) && t->symbol->decl) {
            const Symbol* s = t->symbol;
            unsigned index = 0;
            if (const Symbol* base = s->base_class()) {
                llvm::DIType* bt = di_type(tc_.user(base));
                members.push_back(dib_->createInheritance(fwd, bt, 0, 0, llvm::DINode::FlagPublic));
                index = 1;
            } else if (s->has_vtable_pointer()) {
                member("_vptr", 0, dib_->createPointerType(nullptr, 64));
                index = 1;
            }
            for (const Symbol* f : fields_of(s)) {
                if (types::is_union(s)) {   // every member at offset 0
                    llvm::DIType* ft = di_type(field_type(f));
                    if (ft) members.push_back(dib_->createMemberType(fwd, f->name, nullptr, 0, ft->getSizeInBits(), 0, 0, llvm::DINode::FlagZero, ft));
                    continue;
                }
                member(f->name, field_index(f), di_type(field_type(f)));
            }
            (void)index;
        } else if (t->is(Kind::closure)) {
            if (const LambdaExpr* l = tc_.lambda_of(t)) {
                unsigned i = 0;
                for (const Capture& c : l->captures) {
                    const types::Type* ct = c.symbol && c.symbol->type ? c.symbol->type : nullptr;
                    if (ct) member(c.name, i, di_type(c.kind == CaptureKind::ref || c.kind == CaptureKind::mut_ref ? tc_.reference(types::strip_ref(ct), c.kind == CaptureKind::mut_ref) : ct));
                    ++i;
                }
            }
        }
        llvm::DICompositeType* real = dib_->createStructType(scope, name, nullptr, 0, size, align, llvm::DINode::FlagZero, nullptr,
                                                             dib_->getOrCreateArray(members));
        dib_->replaceTemporary(llvm::TempDIType(fwd), real);
        di_types_[t] = real;
        return real;
    }

    // Declares the variable stored at `slot` for the debugger.
    void debug_variable(llvm::Value* slot, const std::string& name, const types::Type* t, Span at, unsigned arg = 0, bool artificial = false) {
        if (!dib_ || !options_.debug_variables || !slot || !cur_fn_ || name.empty()) return;
        llvm::DISubprogram* sp = cur_fn_->getSubprogram();
        if (!sp || at.file != di_file_of(sp)) return;
        llvm::DIType* type = di_type(t);
        if (!type) return;
        if (artificial) type = dib_->createArtificialType(type);
        unsigned line = sources_.file(at.file).line_col(at.begin).line;
        llvm::DILocalVariable* v = arg ? dib_->createParameterVariable(sp, name, arg, sp->getFile(), line, type, true)
                                       : dib_->createAutoVariable(sp, name, sp->getFile(), line, type, true);
        llvm::Instruction* after = llvm::dyn_cast<llvm::Instruction>(slot);
        llvm::BasicBlock*  block = b_.GetInsertBlock();
        if (!block) return;
        dib_->insertDeclare(slot, v, dib_->createExpression(), llvm::DILocation::get(ctx_, line, 0, sp), block);
        (void)after;
    }

    // The debug location of the code generated next: `span` in the current function. In the
    // library's optimized code the line is 0, which debuggers step through (see step_through).
    void debug_at(Span span) {
        if (!dib_) return;
        llvm::DISubprogram* sp = cur_fn_ ? cur_fn_->getSubprogram() : nullptr;
        if (!sp || span.file != di_file_of(sp)) {
            if (!sp) b_.SetCurrentDebugLocation(llvm::DebugLoc());
            return;
        }
        if (step_through(cur_fn_, span.file)) {
            b_.SetCurrentDebugLocation(llvm::DILocation::get(ctx_, 0, 0, sp));
            return;
        }
        LineCol lc = sources_.file(span.file).line_col(span.begin);
        b_.SetCurrentDebugLocation(llvm::DILocation::get(ctx_, lc.line, lc.column, sp));
    }

    // Whether the debugger steps through `fn` without stopping: the standard library's code when the
    // mode optimizes it (std_optimization(), spec 24 and 34). Its statements have line 0, which
    // lldb and gdb never stop at, so stepping out of a lambda that std::sort called goes back to the
    // program's code instead of through the sort; backtraces still name the function. A mode with
    // `std_optimization() == none` steps through the library line by line.
    std::unordered_map<const llvm::Function*, bool> step_through_;
    bool step_through(const llvm::Function* fn, FileId file) {
        if (!fn) return false;
        auto [it, fresh] = step_through_.try_emplace(fn, false);
        if (fresh) {
            llvm::StringRef level = fn->hasFnAttribute("edax-optimize") ? fn->getFnAttribute("edax-optimize").getValueAsString() : "";
            it->second = !level.empty() && level != "none" && sources_.file(file).path().starts_with("<std>");
        }
        return it->second;
    }

    FileId di_file_of(const llvm::DISubprogram* sp) {
        for (const auto& [id, f] : di_files_) {
            if (f == sp->getFile()) return id;
        }
        return static_cast<FileId>(-1);
    }

    // Code made for one function while another was being generated can carry the other's
    // locations; functions without a subprogram carry none, and calls need one.
    void fix_debug_locations() {
        for (llvm::Function& fn : *mod_) {
            llvm::DISubprogram* sp = fn.getSubprogram();
            for (llvm::Instruction& inst : llvm::instructions(fn)) {
                const llvm::DebugLoc& loc = inst.getDebugLoc();
                if (!sp) {
                    if (loc) inst.setDebugLoc(llvm::DebugLoc());
                    continue;
                }
                if (!loc || loc->getScope()->getSubprogram() != sp) inst.setDebugLoc(llvm::DILocation::get(ctx_, sp->getLine(), 0, sp));
            }
        }
    }

    std::unique_ptr<llvm::Module> run() {
        try {
            return build();
        } catch (const Unsupported&) {
            return nullptr;   // reported at the construct that threw
        }
    }

private:
    std::unique_ptr<llvm::Module> build() {
        // `@std::test` functions of the entry file (spec 25, 29): `edaxc test` runs them one by one.
        std::vector<const Symbol*> tests;
        if (program_.entry) {
            for (const Symbol* s : program_.entry->item_order) {
                if (s->kind == SymbolKind::function && s->decl && sema::find_annotation(s->decl->annotations, "std::test")) tests.push_back(s);
            }
        }
        bool    no_main = program_.is_plugin || program_.is_shared_library;
        Symbol* main = options_.all_functions || !tests.empty() || no_main ? find_main_quietly() : find_main();
        if (!main && !options_.all_functions && tests.empty() && !no_main) return nullptr;
        declare_runtime();
        if (program_.entry && program_.entry->source) inspect_file_ = program_.entry->source->id;
        if (main && !no_main) edax_main_ = function_of(main);
        if (program_.is_shared_library) {   // everything the library's modules define, for its programs
            std::function<void(const Symbol*)> add = [&](const Symbol* s) {
                if (generic(s)) return;
                if (s->kind == SymbolKind::function || s->kind == SymbolKind::method) {
                    const FunctionDecl* f = body_decl(s);
                    if (!f || !f->body || f->is_template || f->is_abstract || (f->is_compile_time && !f->is_run_time)) return;
                    function_of(s);
                } else if (s->kind == SymbolKind::global && s->decl && s->decl->kind == DeclKind::variable &&
                           s->decl->as<VariableDecl>().storage != Storage::compile_time) {
                    global_of(s);
                } else if ((s->kind == SymbolKind::class_ || s->kind == SymbolKind::struct_ || s->kind == SymbolKind::interface) && s->decl &&
                           !s->decl->is_template) {
                    if (s->kind != SymbolKind::interface) {   // one type id and vtable, the library's
                        type_id(s);
                        if (has_vtable(s)) vtable_of(s);
                    }
                    for (const auto& [name, members] : s->members) {
                        for (const Symbol* m : members) add(m);
                    }
                }
            };
            for (const Module* m : program_.library_modules) {
                for (const Symbol* s : m->item_order) add(s);
            }
        }
        if (program_.is_plugin && program_.plugin_class) emit_plugin_entry();
        std::vector<llvm::Function*> test_fns;
        for (const Symbol* t : tests) test_fns.push_back(function_of(t));
        // Edax functions exported to C exist even when no Edax code calls them: C may (spec 13).
        for (auto& [name, m] : program_.modules) {
            for (Symbol* s : m->item_order) {
                if (s->kind != SymbolKind::function) continue;
                const FunctionDecl* f = body_decl(s);
                if (f && f->is_extern && f->body && !f->is_template) function_of(s);
            }
        }
        if (options_.all_functions) {
            for (auto& [name, m] : program_.modules) {
                for (Symbol* s : m->item_order) {
                    const FunctionDecl* f = s->kind == SymbolKind::function ? s->function() : nullptr;
                    if (!f || f->is_template) continue;
                    if (f->is_compile_time && !f->is_run_time) continue;   // never runs
                    function_of(s);
                }
            }
        }
        while (!queue_.empty() || !global_queue_.empty() || !drop_queue_.empty() || !lambda_queue_.empty() ||
               !clone_queue_.empty()) {
            while (!clone_queue_.empty()) {
                const Symbol* d = clone_queue_.front();
                clone_queue_.pop_front();
                gen_clone(d);
            }
            while (!lambda_queue_.empty()) {
                const LambdaExpr* l = lambda_queue_.front();
                lambda_queue_.pop_front();
                gen_lambda(*l);
            }
            while (!drop_queue_.empty()) {
                const Symbol* d = drop_queue_.front();
                drop_queue_.pop_front();
                gen_drop(d);
            }
            while (!queue_.empty()) {
                const Symbol* s = queue_.front();
                queue_.pop_front();
                gen_function(s);
            }
            while (!site_queue_.empty()) {
                const Symbol* s = site_queue_.front();
                site_queue_.pop_front();
                gen_function(s, site_fns_[s]);
            }
            while (!global_queue_.empty()) {
                const Symbol* g = global_queue_.front();
                global_queue_.pop_front();
                gen_global(g);
            }
        }
        if (!packed_types_.empty()) unalign_packed_accesses();
        if (program_.is_shared_library) {
            // No `main`: the library's globals are initialized when it is loaded.
            llvm::Function* init = emit_init_globals();
            llvm::appendToGlobalCtors(*mod_, init, 65535);
        } else if (program_.is_plugin) {
            // A plugin has no `main`: the library's only entry is edax_plugin_entry.
        } else if (!tests.empty()) {
            emit_test_entry(tests, test_fns, main);
        } else if (main) {
            emit_entry(main);
        } else {
            // Without a `main` (only with all_functions) the module still links: `main` returns 0.
            llvm::Function* entry = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getInt32Ty(ctx_), {}, false),
                                                           llvm::Function::ExternalLinkage, "main", mod_.get());
            b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", entry));
            b_.CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0));
        }
        if (diags_.has_errors()) return nullptr;
        if (dib_) {
            fix_debug_locations();
            dib_->finalize();
        }
        finish_insights();
        return std::move(mod_);
    }

    // --- diagnostics ------------------------------------------------------------------------

    [[noreturn]] void unsupported(Span span, const std::string& what, const std::string& why = {}) {
        if (reported_.insert({span.file, span.begin, what}).second) {
            Diagnostic& d = diags_.report(DiagCode::E6001, span, std::format("code generation does not support {} yet", what),
                                          "not supported yet");
            if (!why.empty()) d.notes.push_back(Note{why, std::nullopt});
        }
        throw Unsupported{};
    }

    // A call whose name resolves to nothing: the checker let it through because it looked like
    // one the compiler provides, but nothing here knows it either.
    [[noreturn]] void unrecognised(Span span, const std::string& name) {
        if (reported_.insert({span.file, span.begin, name}).second) {
            Diagnostic& d = diags_.report(DiagCode::E3020, span, std::format("'{}' is not recognised", name), "not found");
            if (name.starts_with("std::")) {
                d.notes.push_back(Note{"neither the standard library compiled into this edaxc nor the compiler declares it; "
                                       "if you changed library/std, rebuild edaxc", std::nullopt});
            }
        }
        throw Unsupported{};
    }

    // A private constant holding `text` (NUL-terminated); its address is a `ptr`.
    llvm::Constant* string_global(const std::string& text) {
        llvm::Constant* init = llvm::ConstantDataArray::getString(ctx_, text);
        return new llvm::GlobalVariable(*mod_, init->getType(), true, llvm::GlobalValue::PrivateLinkage, init, ".str");
    }

    // --- program structure ------------------------------------------------------------------

    static const FunctionDecl* body_decl(const Symbol* s) {
        if (s->definition && s->definition->kind == DeclKind::function) return &s->definition->as<FunctionDecl>();
        return s->function();
    }

    Symbol* find_main_quietly() {
        if (!program_.entry) return nullptr;
        auto it = program_.entry->items.find("main");
        if (it == program_.entry->items.end()) return nullptr;
        for (Symbol* s : it->second) {
            const FunctionDecl* f = s->kind == SymbolKind::function ? body_decl(s) : nullptr;
            if (f && f->params.size() <= 1) return s;
        }
        return nullptr;
    }

    // `ptr @edax_plugin_entry()`: the plugin's descriptor (std::plugin_descriptor in std.ed):
    // a magic number, the hash of its interface, the edaxc version and its `create` function.
    void emit_plugin_entry() {
        auto* i64 = llvm::Type::getInt64Ty(ctx_);
        auto* st  = llvm::StructType::get(ctx_, {i64, i64, ptr_type(), ptr_type()});
        // create(out): the object on the heap, made by T::make(), written to `out` as a
        // std::raw_ptr<mut I>: the object and T's table for I.
        auto* create = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {ptr_type()}, false),
                                              llvm::Function::InternalLinkage, "edax.plugin.create", mod_.get());
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", create));
            llvm::Type*     t     = user_type(program_.plugin_class);
            std::uint64_t   size  = mod_->getDataLayout().getTypeAllocSize(t).getFixedValue();
            llvm::Value*    obj   = b.CreateCall(libc("malloc", ptr_type(), {i64}), {llvm::ConstantInt::get(i64, size ? size : 1)});
            llvm::Value*    value = b.CreateCall(function_of(program_.plugin_make), {});
            b.CreateStore(value, obj);
            auto*        fat   = llvm::StructType::get(ctx_, {ptr_type(), ptr_type()});
            llvm::Value* as_i  = b.CreateInsertValue(b.CreateInsertValue(llvm::UndefValue::get(fat), obj, 0),
                                                     itable_of(program_.plugin_class, program_.plugin_interface), 1);
            b.CreateStore(as_i, create->getArg(0));
            b.CreateRetVoid();
        }
        // Hot reload (dynamic libraries): an entry class marked @std::hot_reload_state carries a table
        // of its fields, { name, offset, size, type hash } each, so that `reload` can move the
        // state of the old build's object into the new one field by field, by name.
        llvm::Constant* layout = llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0));
        const Symbol*   cls    = program_.plugin_class;
        if (cls->decl && sema::find_annotation(cls->decl->annotations, "std::hot_reload_state")) {
            const llvm::StructLayout* sl    = mod_->getDataLayout().getStructLayout(llvm::cast<llvm::StructType>(user_type(cls)));
            auto*                     entry = llvm::StructType::get(ctx_, {ptr_type(), i64, i64, i64});
            std::vector<llvm::Constant*> rows;
            for (const Symbol* f : fields_of(cls)) {
                const types::Type* ft    = field_type(f);
                llvm::Type*        lt    = lower_type(ft);
                std::uint64_t      size  = mod_->getDataLayout().getTypeAllocSize(lt).getFixedValue();
                std::uint64_t      hash  = fnv1a(type_key(ft) + std::format("/{}", size));
                rows.push_back(llvm::ConstantStruct::get(entry, {string_global(f->name), llvm::ConstantInt::get(i64, sl->getElementOffset(field_index(f))),
                                                                 llvm::ConstantInt::get(i64, size), llvm::ConstantInt::get(i64, hash)}));
            }
            rows.push_back(llvm::ConstantStruct::get(entry, {llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0)), llvm::ConstantInt::get(i64, 0),
                                                             llvm::ConstantInt::get(i64, 0), llvm::ConstantInt::get(i64, 0)}));   // the end
            auto* array = llvm::ArrayType::get(entry, rows.size());
            layout = new llvm::GlobalVariable(*mod_, array, true, llvm::GlobalValue::InternalLinkage, llvm::ConstantArray::get(array, rows),
                                              "edax.plugin.state_layout");
        }
        auto* st2 = llvm::StructType::get(ctx_, {i64, i64, ptr_type(), ptr_type(), ptr_type()});
        llvm::Constant* fields[] = {llvm::ConstantInt::get(i64, 0x5249465450474C32ull), llvm::ConstantInt::get(i64, program_.plugin_interface_hash),
                                    string_global(EDAX_VERSION_TEXT), create, layout};
        auto* descriptor = new llvm::GlobalVariable(*mod_, st2, true, llvm::GlobalValue::InternalLinkage, llvm::ConstantStruct::get(st2, fields),
                                                    "edax.plugin.descriptor");
        (void)st;
        auto* entry = llvm::Function::Create(llvm::FunctionType::get(ptr_type(), {}, false), llvm::Function::ExternalLinkage,
                                             "edax_plugin_entry", mod_.get());
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", entry));
        b.CreateRet(descriptor);
    }

    Symbol* find_main() {
        Module* entry = program_.entry;
        if (entry) {
            if (auto it = entry->items.find("main"); it != entry->items.end()) {
                for (Symbol* s : it->second) {
                    if (s->kind == SymbolKind::function) {
                        const FunctionDecl* f = body_decl(s);
                        if (f && f->params.size() > 1) unsupported(f->params[1].span, "more than one parameter of 'main'");
                        return s;
                    }
                }
            }
        }
        Span where{};
        if (entry && entry->source) where = Span{entry->source->id, 0, 0};
        else if (entry && entry->header) where = Span{entry->header->id, 0, 0};
        diags_.report(DiagCode::E6002, where, "the program has no 'main' function", "no 'main'")
            .notes.push_back(Note{"a program starts at 'void main()' or 'u32 main()' in the entry file", std::nullopt});
        return nullptr;
    }

    // --- stable names (object cache) ------------------------------------------------------------
    //
    // Every name in the module comes from what it names, never from the order things were
    // generated in: the same function gets the same name, and so the same IR text, in every build
    // that contains it. build_executable's object cache relies on that.

    std::map<const Symbol*, std::string> symbol_keys_;

    // What a type is, written out: template arguments by their own keys, a lambda by where it is
    // written (types::to_string would make all lambdas of one signature equal).
    std::string type_key(const types::Type* t) {
        if (!t) return "?";
        switch (t->kind) {
        case Kind::reference: return (t->is_mut ? "mut " : "") + type_key(t->inner) + "&";
        case Kind::user:
        case Kind::pending: return t->symbol ? symbol_key(t->symbol) : t->name;
        case Kind::closure: {
            const LambdaExpr* l = tc_.lambda_of(t);
            return l ? std::format("lambda@{}:{}", sources_.file(l->span.file).path(), l->span.begin) : "lambda";
        }
        default: {
            std::string out = std::format("{}:{}:{}:{}{}{}", static_cast<int>(t->kind), t->name, t->bits, t->is_signed ? "s" : "",
                                          t->is_size ? "z" : "", t->is_mut ? "m" : "");
            if (t->inner) out += "(" + type_key(t->inner) + ")";
            for (const types::Type* a : t->args) out += "," + type_key(a);
            return out;
        }
        }
    }

    // The full name of a symbol with the arguments of every instance on the way.
    std::string symbol_key(const Symbol* s) {
        if (auto it = symbol_keys_.find(s); it != symbol_keys_.end()) return it->second;
        std::string out;
        if (s->parent) {
            out = symbol_key(s->parent) + "::";
        } else {
            for (const std::string& p : symbol_scope(s)) out += p + "::";
        }
        out += s->name;
        if (s->pack_origin) out += std::format("#{}", s->pack_count);
        if (s->template_of) {
            out += "<";
            for (const types::Type* a : s->template_args) out += type_key(a) + ",";
            out += ">";
        }
        if (!s->cpu_level.empty()) out += "@" + s->cpu_level;
        return symbol_keys_[s] = out;
    }

    static std::string short_hash(std::string_view text) { return std::format("{:016x}", fnv1a(text)); }

    // The name part of an instance: a hash of its arguments (it was the order instances were made in).
    std::string instance_tag(const Symbol* s) {
        std::string args;
        for (const types::Type* a : s->template_args) args += type_key(a) + ",";
        return "0i" + short_hash(args);
    }

    // The name of a function generated for a piece of `cur_fn_`'s body (a lambda, a format
    // string): the enclosing function and the place.
    std::string local_helper_name(std::string_view prefix, Span span) {
        std::string where = std::format("{}@{}:{}", cur_fn_ ? cur_fn_->getName().str() : std::string(), sources_.file(span.file).path(),
                                        span.begin);
        return std::string(prefix) + short_hash(where);
    }

    // --- object-cache units --------------------------------------------------------------------
    //
    // build_executable compiles a debug build in units and caches each unit's object code. A unit
    // is a group of functions that change together: one per module of the program; the standard
    // library's functions are spread over a few units per module, with a type's methods kept
    // together so that they still inline into each other. Instances are grouped by the instance
    // that owns them.
    std::string unit_of(const Symbol* s) {
        const Symbol* top      = s;
        bool          instance = s->template_of != nullptr;
        while (top->parent) {
            top      = top->parent;
            instance = instance || top->template_of;
        }
        const sema::Module* m       = s->module;
        bool                library = m && m->project == "std";
        std::string         base    = std::string(library ? "std/" : "user/") + (m ? m->name : "?");
        if (instance) return std::format("{}/i{}", base, fnv1a(symbol_key(top)) % 16);
        if (!library) return base;
        return std::format("{}/p{}", base, fnv1a(symbol_key(top)) % 8);
    }

    static void set_unit(llvm::Function* fn, const std::string& unit) {
        if (fn && !unit.empty()) fn->addFnAttr("edax-unit", unit);
    }

    // A helper made for a function belongs to that function's unit.
    static void inherit_unit(llvm::Function* fn, const llvm::Function* from) {
        if (from && from->hasFnAttribute("edax-unit")) set_unit(fn, from->getFnAttribute("edax-unit").getValueAsString().str());
    }

    // Names as in the C backend: `r_<module parts><type><name>`, overloads numbered in order.
    std::string function_name(const Symbol* s) {
        std::vector<std::string> parts = symbol_scope(s);
        if (s->parent) {
            parts.push_back(s->parent->name);
            if (s->parent->template_of) parts.push_back(instance_tag(s->parent));
        }
        parts.push_back(s->name == "~" + (s->parent ? s->parent->name : "") ? "drop" : s->name);
        if (s->template_of) parts.push_back(instance_tag(s));
        std::string name = "r_" + mangle_parts(parts);
        const auto&   table = s->parent ? s->parent->members : s->module->items;
        const Symbol* entry = s;   // an instance is named after its template's overload
        while (entry->template_of && entry->template_of->kind == entry->kind) entry = entry->template_of;
        if (auto it = table.find(s->parent ? s->name : s->key()); it != table.end() && it->second.size() > 1) {
            auto pos = std::find(it->second.begin(), it->second.end(), entry);
            name += std::format("_{}", pos - it->second.begin());
        }
        return name;
    }

    // The LLVM function for `s`, declared on first use; its body is generated later.
    // Shared Edax libraries: whether `s` is compiled in a shared library this program links (the
    // program declares it), or exported by the shared library being built. Generic instances are
    // made where they are used, as always.
    static bool generic(const Symbol* s) { return s->template_of || sema::substitution_of(s); }
    const Module* owner_module(const Symbol* s) const {
        for (; s; s = s->parent) {
            if (s->module) return s->module;
        }
        return nullptr;
    }
    // The TLS model of this module's own thread-locals: at a fixed offset from the thread pointer in
    // a program; through the dynamic loader in a plugin or shared library, which may be loaded
    // (dlopen) after the program's threads exist (local-exec can't be linked into a shared object).
    llvm::GlobalValue::ThreadLocalMode own_tls_model() const {
        return program_.is_plugin || program_.is_shared_library ? llvm::GlobalValue::LocalDynamicTLSModel
                                                                : llvm::GlobalValue::LocalExecTLSModel;
    }
    bool from_shared_library(const Symbol* s) const {
        return !program_.shared_modules.empty() && !generic(s) && program_.shared_modules.contains(owner_module(s));
    }
    bool exported_by_library(const Symbol* s) const {
        return program_.is_shared_library && !generic(s) && program_.library_modules.contains(owner_module(s));
    }
    // A global (vtable, type id, variable) of a shared library: declared here, or exported.
    void share_global(llvm::GlobalVariable* g, const Symbol* s) {
        // Not dso_local (an internal global was): the program and the library reach the one copy
        // through the GOT, so they agree on its address.
        if (from_shared_library(s)) {
            g->setLinkage(llvm::GlobalValue::ExternalLinkage);
            g->setInitializer(nullptr);
            g->setDSOLocal(false);
        } else if (exported_by_library(s)) {
            g->setLinkage(llvm::GlobalValue::ExternalLinkage);
            g->setVisibility(llvm::GlobalValue::DefaultVisibility);
            g->setDSOLocal(false);
        }
    }

    llvm::Function* function_of(const Symbol* s) {
        if (auto it = fns_.find(s); it != fns_.end()) return it->second;
        const FunctionDecl* f = body_decl(s);
        if (!f) unsupported(s->span, "this kind of function");
        if (f->is_template && !s->template_of) unsupported(s->span, "calling templates");
        if (f->is_extern && f->abi == "cpp") unsupported(s->span, "'extern(std::build::language::cpp)' (C++ linkage)");
        if (f->is_extern && !f->body) return extern_function(s, *f);   // a C function: imported
        if (!f->body) unsupported(s->span, "functions without a body");
        if (from_shared_library(s)) {   // compiled in a shared Edax library the program links
            auto* fn = llvm::Function::Create(function_type(s), llvm::Function::ExternalLinkage, function_name(s), mod_.get());
            fn->addFnAttr(llvm::Attribute::NoUnwind);
            fns_[s] = fn;
            return fn;
        }
        // One module is the whole program: Edax functions are internal (LLVM may inline, specialize
        // or drop them); C sees only `main` and the exported wrappers. Edax never unwinds: a panic
        // aborts (spec 11).
        auto* fn = llvm::Function::Create(function_type(s), llvm::Function::InternalLinkage, function_name(s), mod_.get());
        fn->addFnAttr(llvm::Attribute::NoUnwind);
        set_unit(fn, unit_of(s));
        const Symbol* annotated = s->template_of && s->template_of->kind == s->kind ? s->template_of : s;
        if (annotated->decl) {   // @std::inline_always, @std::inline_never, @std::cold (spec 29)
            const auto& uses = annotated->decl->annotations;
            if (sema::find_annotation(uses, "std::inline_always")) fn->addFnAttr(llvm::Attribute::AlwaysInline);
            if (sema::find_annotation(uses, "std::inline_never")) fn->addFnAttr(llvm::Attribute::NoInline);
            if (sema::find_annotation(uses, "std::no_checks_left")) program_.no_checks_left.emplace_back(s->span, fn->getName().str());
            if (sema::find_annotation(uses, "std::cold")) {
                fn->addFnAttr(llvm::Attribute::Cold);
                fn->addFnAttr(llvm::Attribute::NoInline);
            }
        }
        {   // @std::optimize, or g.optimize_files (spec 34): the build step reads this attribute
            const Symbol* own = s->version_of ? s->version_of : annotated;
            std::string   level = own->optimize_level.empty() ? file_optimization(s->span) : own->optimize_level;
            // The standard library's code (spec 34): the mode's std_optimization(), unless build.ed
            // or an annotation says otherwise.
            if (level.empty() && is_std_function(s)) level = mode_setting("std_optimization");
            if (!level.empty()) fn->addFnAttr("edax-optimize", level);
        }
        if (!s->cpu_level.empty()) {   // a multiversion copy: code for its CPU level
            if (const CpuLevel* l = find_cpu_level(s->cpu_level)) {
                fn->addFnAttr("target-cpu", l->llvm_cpu);
                if (*l->llvm_features) fn->addFnAttr("target-features", l->llvm_features);
            }
        }
        add_alias_attributes(s, *f, fn);
        if (borrows_from_this(s, *f)) fn->addFnAttr("edax-borrows-this");
        if (has_run_time_entry_checks(s)) fn->addFnAttr("edax-name", display_name(s));   // edaxc explain-checks
        fns_[s] = fn;
        queue_.push_back(s);
        if (f->is_extern) export_function(s, *f, fn);   // a Edax function that C can call
        if (exported_by_library(s)) {   // the shared Edax library being built: programs call it
            fn->setLinkage(llvm::GlobalValue::ExternalLinkage);
            fn->setVisibility(llvm::GlobalValue::DefaultVisibility);
            fn->setDSOLocal(false);
        }
        return fn;
    }

    // Fields of a packed struct may be misaligned (spec 34): every load and store through a
    // pointer into one is marked as aligned to 1 byte.
    void unalign_packed_accesses() {
        auto into_packed = [&](llvm::Value* p) {
            for (int depth = 0; p && depth < 8; ++depth) {
                auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(p);
                if (!gep) return false;
                if (auto* st = llvm::dyn_cast<llvm::StructType>(gep->getSourceElementType()); st && packed_types_.contains(st)) return true;
                p = gep->getPointerOperand();
            }
            return false;
        };
        for (llvm::Function& fn : *mod_) {
            for (llvm::BasicBlock& bb : fn) {
                for (llvm::Instruction& in : bb) {
                    if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&in); ld && into_packed(ld->getPointerOperand())) ld->setAlignment(llvm::Align(1));
                    if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&in); st && into_packed(st->getPointerOperand())) st->setAlignment(llvm::Align(1));
                }
            }
        }
    }

    // Whether `s` is the standard library's code (its module, or an instance of one of its templates).
    static bool is_std_function(const Symbol* s) {
        const Symbol* parent = s->parent;
        for (const Symbol* o : std::initializer_list<const Symbol*>{s, s->template_of, s->version_of, parent, parent ? parent->template_of : nullptr}) {
            if (o && o->module && o->module->project == "std") return true;
        }
        return false;
    }

    // The optimization build.ed gave the file of `where` with `g.optimize_files` (spec 34), or "".
    std::string file_optimization(Span where) {
        if (program_.file_settings.empty()) return "";
        if (auto it = file_levels_.find(where.file); it != file_levels_.end()) return it->second;
        std::error_code ec;
        std::string     path  = std::filesystem::weakly_canonical(std::filesystem::path(sources_.file(where.file).path()), ec).generic_string();
        std::string     level;
        std::string text = sources_.file(where.file).path();
        for (const sema::FileSetting& fs : program_.file_settings) {
            if (fs.name != "std::build::optimization" || !fs.value || !fs.value->symbol) continue;
            if (!fs.std_regex.empty()) {   // `<std>/graphics/**`
                if (text.starts_with("<std>/") && std::regex_match(text.substr(6), std::regex(fs.std_regex))) level = fs.value->symbol->name;
                continue;
            }
            if (std::find(fs.files.begin(), fs.files.end(), path) != fs.files.end()) level = fs.value->symbol->name;
        }
        file_levels_[where.file] = level;
        return level;
    }

    // What the aliasing rule guarantees (Ownership: aliasing rule), told to LLVM (spec 34 §1.1):
    // - a `mut T&` (and the `this` of a `mut` method) is the only way to reach its object while the
    //   call runs: `noalias`;
    // - a `T&` (and the `this` of a non-`mut` method) sees an object that does not change during the
    //   call: `noalias readonly`, but only for *frozen* types, whose bytes can't change through an
    //   immutable reference (synchronized classes such as std::atomic change through `T&`).
    // Interface references are fat values, not pointers; they get nothing.
    void add_alias_attributes(const Symbol* s, const FunctionDecl& f, llvm::Function* fn) {
        if (!alias_attributes_) return;
        const auto* subst = sema::substitution_of(s);
        unsigned    index = 0;
        auto mark = [&](unsigned i, const types::Type* object, bool is_mut) {
            if (i >= fn->arg_size() || !fn->getArg(i)->getType()->isPointerTy()) return;
            if (is_mut) {
                fn->addParamAttr(i, llvm::Attribute::NoAlias);
            } else if (is_frozen(object)) {
                fn->addParamAttr(i, llvm::Attribute::NoAlias);
                fn->addParamAttr(i, llvm::Attribute::ReadOnly);
            }
        };
        if (has_self(s)) {
            if (!f.is_destructor && !f.is_move && s->parent && is_struct_symbol(s->parent)) {
                types::Type self{};
                self.kind   = Kind::user;
                self.symbol = s->parent;
                if (f.is_mut || is_frozen_symbol(s->parent)) mark(0, &self, f.is_mut);
            }
            index = 1;
        }
        for (const Param& p : f.params) {
            const types::Type* t = tc_.lower(*p.type, subst);
            if (t && t->is(Kind::reference) && t->inner && !is_interface(t->inner)) mark(index, t->inner, t->is_mut);
            if (t && !t->is(Kind::reference)) mark_span(fn, index, t);
            ++index;
        }
    }

    // A span parameter borrows its elements like a reference (Ownership: a view keeps its borrow):
    // a `std::span<mut T>` is the only way to reach them while the call runs, and a `std::span<T>` of
    // a frozen `T` sees elements that don't change. The span itself is a value, so LLVM's `noalias`
    // can't say it; add_alias_scopes gives the accesses through it a scope of their own instead.
    std::map<const llvm::Function*, std::map<unsigned, bool>> span_params_;   // argument index -> mutable
    void mark_span(llvm::Function* fn, unsigned i, const types::Type* t) {
        if (i >= fn->arg_size() || !t->is(Kind::user) || t->symbol->template_args.empty()) return;
        if (types::is_std_instance(t, "span_mut")) {
            span_params_[fn][i] = true;
        } else if (types::is_std_instance(t, "span") && is_frozen(t->symbol->template_args[0])) {
            span_params_[fn][i] = false;
        } else {
            return;
        }
        fn->addParamAttr(i, llvm::Attribute::get(ctx_, "edax-span", span_params_[fn][i] ? "mut" : "read"));
    }

    // A method whose result is a reference into `this`: it takes no other reference (spec 34 §1:
    // `xs[i]` reaches the memory `xs` owns).
    bool borrows_from_this(const Symbol* s, const FunctionDecl& f) {
        if (!alias_attributes_ || !has_self(s) || !f.result || f.is_extern) return false;
        const auto*        subst = sema::substitution_of(s);
        const types::Type* r     = tc_.lower(*f.result, subst);
        if (!r || !r->is(Kind::reference) || is_interface(r->inner)) return false;
        for (const Param& p : f.params) {
            const types::Type* t = tc_.lower(*p.type, subst);
            if (!t || t->is(Kind::reference) || t->is(Kind::pointer) || t->is(Kind::closure) || t->is(Kind::fn_ptr)) return false;
            const Symbol* ts = t->is(Kind::user) ? t->symbol : nullptr;
            while (ts && !ts->decl && ts->template_of) ts = ts->template_of;
            if (ts && ts->decl && ts->decl->kind == ast::DeclKind::struct_ && ts->decl->as<TypeDecl>().is_view) return false;   // holds references
        }
        return true;
    }

    // The aliasing rule reaches what a reference owns (spec 34 §1): two `mut T&` parameters are
    // different objects with different heap buffers, so `a[i]` and `b[j]` never overlap. LLVM's
    // `noalias` only covers the bytes of the object itself; the loads and stores of the function
    // whose address comes from one parameter (directly, through a local reference, or as the result
    // of a method of it that returns a reference into `this`) are put in that parameter's alias
    // scope and marked as not aliasing the other parameters' scopes. The scopes survive inlining,
    // which is where they matter: the buffer pointers are loaded from the objects there.
    void add_alias_scopes(llvm::Function* fn) {
        if (!alias_attributes_) return;
        std::vector<llvm::Argument*> args;
        const std::map<unsigned, bool>* spans = nullptr;
        if (auto it = span_params_.find(fn); it != span_params_.end()) spans = &it->second;
        auto span_of = [&](const llvm::Argument* a) -> const bool* {
            if (!spans) return nullptr;
            auto it = spans->find(a->getArgNo());
            return it == spans->end() ? nullptr : &it->second;
        };
        // Whether only reads go through the argument: `T&` of a frozen type, `std::span<T>`.
        auto reads_only = [&](const llvm::Argument* a) {
            if (const bool* m = span_of(a)) return !*m;
            return a->onlyReadsMemory();
        };
        for (llvm::Argument& a : fn->args()) {
            if ((a.hasNoAliasAttr() && a.getType()->isPointerTy()) || span_of(&a)) args.push_back(&a);
        }
        if (args.size() < 2) return;
        std::map<llvm::Value*, llvm::Argument*> roots;
        std::function<llvm::Argument*(llvm::Value*, int)> root = [&](llvm::Value* v, int depth) -> llvm::Argument* {
            if (!v || depth > 12) return nullptr;
            if (auto it = roots.find(v); it != roots.end()) return it->second;
            llvm::Argument* out = nullptr;
            if (auto* a = llvm::dyn_cast<llvm::Argument>(v)) {
                out = a->hasNoAliasAttr() ? a : nullptr;
            } else if (auto* span_slot = llvm::dyn_cast<llvm::AllocaInst>(v)) {
                // A span parameter's slot (stored once from the argument): its elements and fields.
                llvm::Argument* from = nullptr;
                bool            ok   = true;
                for (llvm::User* u : span_slot->users()) {
                    if (auto* st = llvm::dyn_cast<llvm::StoreInst>(u)) {
                        auto* arg = llvm::dyn_cast<llvm::Argument>(st->getValueOperand());
                        if (st->getPointerOperand() != span_slot || from || !arg) ok = false;
                        else from = arg;
                    }
                }
                if (ok && from && span_of(from)) out = from;
            } else if (auto* g = llvm::dyn_cast<llvm::GetElementPtrInst>(v)) {
                out = root(g->getPointerOperand(), depth + 1);
            } else if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(v)) {
                // A parameter's or local reference's slot: an alloca stored exactly once.
                if (auto* slot = llvm::dyn_cast<llvm::AllocaInst>(ld->getPointerOperand()); slot && ld->getType()->isPointerTy()) {
                    llvm::StoreInst* only = nullptr;
                    bool             ok   = true;
                    for (llvm::User* u : slot->users()) {
                        if (auto* st = llvm::dyn_cast<llvm::StoreInst>(u)) {
                            if (st->getPointerOperand() != slot || only) ok = false;
                            else only = st;
                        } else if (!llvm::isa<llvm::LoadInst>(u) && !llvm::isa<llvm::DbgInfoIntrinsic>(u)) {
                            ok = false;
                        }
                    }
                    if (ok && only) out = root(only->getValueOperand(), depth + 1);
                }
            } else if (auto* call = llvm::dyn_cast<llvm::CallInst>(v)) {
                llvm::Function* callee = call->getCalledFunction();
                if (callee && callee->hasFnAttribute("edax-borrows-this") && call->arg_size() > 0) out = root(call->getArgOperand(0), depth + 1);
            }
            roots[v] = out;
            return out;
        };
        llvm::MDBuilder                       md(ctx_);
        llvm::MDNode*                         domain = md.createAnonymousAliasScopeDomain(fn->getName());
        std::map<llvm::Argument*, llvm::MDNode*> scope;
        for (llvm::Argument* a : args) scope[a] = md.createAnonymousAliasScope(domain, std::format("{}.arg{}", fn->getName().str(), a->getArgNo()));
        for (llvm::BasicBlock& bb : *fn) {
            for (llvm::Instruction& in : bb) {
                llvm::Value* addr = nullptr;
                if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&in)) addr = ld->getPointerOperand();
                else if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&in)) addr = st->getPointerOperand();
                if (!addr || llvm::isa<llvm::AllocaInst>(addr)) continue;
                llvm::Argument* a = root(addr, 0);
                if (!a) continue;
                std::vector<llvm::Metadata*> others;
                for (llvm::Argument* b : args) {
                    // Two read-only parameters may be the same object; they only read, so nothing is lost.
                    if (b != a && !(reads_only(a) && reads_only(b))) others.push_back(scope[b]);
                }
                if (others.empty()) continue;
                in.setMetadata(llvm::LLVMContext::MD_alias_scope, llvm::MDNode::get(ctx_, {scope[a]}));
                in.setMetadata(llvm::LLVMContext::MD_noalias, llvm::MDNode::get(ctx_, others));
            }
        }
    }

    // A type whose bytes never change through an immutable reference: numbers, bool, char, raw and
    // function pointers, SIMD values, and structs, classes, enums, tuples and arrays made only of
    // these. A type marked @std::interior_mutable (std::atomic, std::mutex, ...) is not frozen, and
    // neither is anything that contains one.
    bool is_frozen(const types::Type* t) {
        if (!t) return false;
        switch (t->kind) {
            case Kind::int_: case Kind::float_: case Kind::bool_: case Kind::char_: case Kind::pointer:
            case Kind::fn_ptr: case Kind::vector: case Kind::mask: case Kind::static_str: return true;
            case Kind::array: return is_frozen(t->inner);
            case Kind::tuple:
                for (const types::Type* e : t->args) {
                    if (!is_frozen(e)) return false;
                }
                return true;
            case Kind::user: return t->symbol && is_frozen_symbol(t->symbol);
            default: return false;
        }
    }

    bool is_frozen_symbol(const Symbol* s) {
        if (auto it = frozen_memo_.find(s); it != frozen_memo_.end()) return it->second;
        frozen_memo_[s] = false;   // guards recursion; a type can't contain itself by value
        bool result     = false;
        if (s->kind == SymbolKind::enum_) {
            result = true;
            for (const Symbol* a : s->member_order) {
                if (carries(a) && !is_frozen(payload_of(a))) result = false;
            }
        } else if (s->kind == SymbolKind::struct_ || s->kind == SymbolKind::class_) {
            const Symbol* d = s;
            while (d && !d->decl && d->template_of) d = d->template_of;
            bool marked = (d && d->decl && sema::find_annotation(d->decl->annotations, "std::interior_mutable")) ||
                          (s->template_of && s->template_of->decl && sema::find_annotation(s->template_of->decl->annotations, "std::interior_mutable"));
            result = !marked;
            if (const Symbol* base = s->base_class(); base && !is_frozen_symbol(base)) result = false;
            for (const Symbol* fld : fields_of(s)) {
                if (!is_frozen(field_type(fld))) result = false;
            }
        }
        frozen_memo_[s] = result;
        return result;
    }

    // The LLVM type of the Edax function `s` (methods take `self` first). Also for abstract and
    // interface methods, which have no body: dynamic calls use it.
    llvm::FunctionType* function_type(const Symbol* s) {
        const FunctionDecl*      f     = body_decl(s);
        const auto*              subst = sema::substitution_of(s);
        std::vector<llvm::Type*> params;
        if (has_self(s)) params.push_back(llvm::PointerType::get(ctx_, 0));
        for (const Param& p : f->params) {
            cur_ = p.span;
            params.push_back(lower_type(tc_.lower(*p.type, subst)));
        }
        cur_ = s->span;
        llvm::Type* ret = f->result ? lower_type(result_type(*f, subst)) : llvm::Type::getVoidTy(ctx_);
        return llvm::FunctionType::get(ret, params, false);
    }

    // `extern(c)` with a body (spec 13): C calls it under its own name. The exported symbol is a
    // wrapper that converts C's byte-sized `bool` and calls the Edax function.
    void export_function(const Symbol* s, const FunctionDecl& f, llvm::Function* target) {
        if (has_self(s)) unsupported(s->span, "exporting methods to C");
        llvm::Function*                  decl     = extern_function_type(s, f, f.name + ".export");
        llvm::Function*                  wrapper  = llvm::Function::Create(decl->getFunctionType(), llvm::Function::ExternalLinkage,
                                                                           f.name, mod_.get());
        wrapper->copyAttributesFrom(decl);
        CSignature sig = std::move(c_sigs_[decl]);
        c_sigs_.erase(decl);
        decl->eraseFromParent();
        llvm::IRBuilderBase::InsertPoint saved = b_.saveIP();
        b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", wrapper));
        std::vector<llvm::Value*> args;
        for (unsigned i = 0; i < sig.params.size(); ++i) {
            const CArg& c    = sig.params[i];
            llvm::Type* want = target->getFunctionType()->getParamType(i);
            if (c.kind == CArg::Kind::direct) {   // the struct from its registers
                std::vector<llvm::Value*> parts;
                for (unsigned k = 0; k < c.parts.size(); ++k) parts.push_back(wrapper->getArg(c.first + k));
                args.push_back(c_join(c, parts));
            } else if (c.kind == CArg::Kind::memory) {
                args.push_back(b_.CreateLoad(c.edax, wrapper->getArg(c.first)));
            } else {
                llvm::Value* a = wrapper->getArg(c.first);
                args.push_back(want->isIntegerTy(1) ? b_.CreateICmpNE(a, llvm::ConstantInt::get(a->getType(), 0)) : a);
            }
        }
        llvm::Value* r = b_.CreateCall(target, args);
        if (sig.result.kind == CArg::Kind::memory) {
            b_.CreateStore(r, wrapper->getArg(0));
            b_.CreateRetVoid();
        } else if (sig.result.kind == CArg::Kind::direct) {
            std::vector<llvm::Value*> parts = c_split(sig.result, r);
            if (parts.size() == 1) {
                b_.CreateRet(parts[0]);
            } else {
                llvm::Value* agg = llvm::UndefValue::get(wrapper->getReturnType());
                for (unsigned k = 0; k < parts.size(); ++k) agg = b_.CreateInsertValue(agg, parts[k], k);
                b_.CreateRet(agg);
            }
        } else if (wrapper->getReturnType()->isVoidTy()) {
            b_.CreateRetVoid();
        } else {
            b_.CreateRet(r->getType()->isIntegerTy(1) ? b_.CreateZExt(r, wrapper->getReturnType()) : r);
        }
        b_.restoreIP(saved);
    }

    // An `extern(c)` function (spec 13): declared under its own name with the C calling
    // convention. Small integers are widened and structs passed as C does (c_classify).
    llvm::Function* extern_function(const Symbol* s, const FunctionDecl& f) {
        llvm::Function* fn = mod_->getFunction(f.name);
        if (!fn) fn = extern_function_type(s, f, f.name);
        fns_[s] = fn;
        return fn;
    }

    // A declaration with C's types for `f` (bool as a byte, small integers widened).
    llvm::Function* extern_function_type(const Symbol* s, const FunctionDecl& f, const std::string& name) {
        (void)s;
        auto c_type = [&](const types::Type* t, Span span) -> llvm::Type* {
            cur_ = span;
            if (t->is(Kind::bool_)) return llvm::Type::getInt8Ty(ctx_);
            if (t->is(Kind::user) && t->symbol->kind != SymbolKind::enum_ && t->symbol->kind != SymbolKind::struct_) {
                unsupported(span, "passing classes by value to extern functions (pass a std::raw_ptr)");
            }
            if (t->is(Kind::reference)) unsupported(span, "references in extern functions (use std::raw_ptr)");
            return lower_type(t);
        };
        CSignature                      sig;
        std::vector<llvm::Type*>        params;
        std::vector<const types::Type*> types;
        const types::Type* rt  = f.result ? tc_.lower(*f.result, nullptr) : nullptr;
        if (rt && !is_void(rt)) {
            sig.result = c_classify(c_type(rt, f.result->span), true, f.result->span);
            if (sig.result.kind == CArg::Kind::memory) params.push_back(ptr_type());   // sret
        }
        for (const Param& p : f.params) {
            const types::Type* t = tc_.lower(*p.type, nullptr);
            types.push_back(t);
            CArg a = c_classify(c_type(t, p.span), false, p.span);
            a.first = static_cast<unsigned>(params.size());
            if (a.kind == CArg::Kind::direct) params.insert(params.end(), a.parts.begin(), a.parts.end());
            else if (a.kind == CArg::Kind::memory) params.push_back(ptr_type());
            else params.push_back(a.edax);
            sig.params.push_back(std::move(a));
        }
        llvm::Type* ret = !rt || is_void(rt) || sig.result.kind == CArg::Kind::memory ? llvm::Type::getVoidTy(ctx_)
                        : sig.result.kind == CArg::Kind::direct
                            ? (sig.result.parts.size() == 1 ? sig.result.parts[0] : llvm::StructType::get(ctx_, sig.result.parts))
                            : sig.result.edax;
        llvm::Function* fn = nullptr;
        {
            fn = llvm::Function::Create(llvm::FunctionType::get(ret, params, false), llvm::Function::ExternalLinkage,
                                        name, mod_.get());
            auto extend = [&](const types::Type* t) -> llvm::Attribute::AttrKind {
                if (t->is(Kind::bool_)) return llvm::Attribute::ZExt;
                if (t->is_integer() && t->bits < 32) return t->is_signed ? llvm::Attribute::SExt : llvm::Attribute::ZExt;
                return llvm::Attribute::None;
            };
            for (unsigned i = 0; i < types.size(); ++i) {
                const CArg& a = sig.params[i];
                if (a.kind == CArg::Kind::plain) {
                    if (auto k = extend(types[i]); k != llvm::Attribute::None) fn->addParamAttr(a.first, k);
                } else if (a.kind == CArg::Kind::memory && a.byval) {
                    fn->addParamAttr(a.first, llvm::Attribute::getWithByValType(ctx_, a.edax));
                    fn->addParamAttr(a.first, llvm::Attribute::getWithAlignment(ctx_, llvm::Align(8)));
                }
            }
            if (rt && !is_void(rt)) {
                if (sig.result.kind == CArg::Kind::memory) {
                    fn->addParamAttr(0, llvm::Attribute::getWithStructRetType(ctx_, sig.result.edax));
                    fn->addParamAttr(0, llvm::Attribute::NoAlias);
                } else if (sig.result.kind == CArg::Kind::plain) {
                    if (auto k = extend(rt); k != llvm::Attribute::None) fn->addRetAttr(k);
                }
            }
        }
        c_sigs_[fn] = std::move(sig);
        return fn;
    }

    // --- the C calling convention for structs (C interop) -------------------------------------

    // How one parameter or the result of an extern function is passed in C.
    struct CArg {
        enum class Kind { plain, direct, memory };
        Kind                       kind = Kind::plain;
        llvm::Type*                edax = nullptr;   // the Edax (LLVM) type of the value
        std::vector<llvm::Type*>   parts;            // direct: the registers' types, one per eightbyte
        bool                       byval = false;    // memory: a `byval` copy (x86-64); else a pointer to a copy (AArch64)
        unsigned                   first = 0;        // the index of its first LLVM parameter
    };
    struct CSignature {
        std::vector<CArg> params;
        CArg              result;
    };

    // The leaves of `t` (scalars and vector elements) with their byte offsets.
    void c_leaves(llvm::Type* t, std::uint64_t at, std::vector<std::pair<llvm::Type*, std::uint64_t>>& out) {
        const llvm::DataLayout& dl = mod_->getDataLayout();
        if (auto* st = llvm::dyn_cast<llvm::StructType>(t)) {
            const llvm::StructLayout* sl = dl.getStructLayout(st);
            for (unsigned i = 0; i < st->getNumElements(); ++i) c_leaves(st->getElementType(i), at + sl->getElementOffset(i), out);
        } else if (auto* at_ = llvm::dyn_cast<llvm::ArrayType>(t)) {
            std::uint64_t size = dl.getTypeAllocSize(at_->getElementType());
            for (std::uint64_t i = 0; i < at_->getNumElements(); ++i) c_leaves(at_->getElementType(), at + i * size, out);
        } else {
            out.emplace_back(t, at);
        }
    }

    // Classifies a value of LLVM type `t` for the target's C calling convention: x86-64 System V
    // (eightbytes of class INTEGER or SSE, more than 16 bytes in memory) or AAPCS64 (homogeneous
    // float aggregates in vector registers, up to 16 bytes in general registers, more through a
    // pointer to a copy).
    CArg c_classify(llvm::Type* t, bool result, Span span) {
        CArg a;
        a.edax = t;
        if (!t->isStructTy() && !t->isArrayTy()) return a;
        const llvm::DataLayout& dl   = mod_->getDataLayout();
        std::uint64_t           size = dl.getTypeAllocSize(t);
        std::vector<std::pair<llvm::Type*, std::uint64_t>> leaves;
        c_leaves(t, 0, leaves);
        llvm::Triple triple(target_triple());
        if (triple.getArch() == llvm::Triple::x86_64 && !triple.isOSWindows()) {
            if (size > 16 || size == 0) {
                a.kind  = CArg::Kind::memory;
                a.byval = !result;
                return a;
            }
            a.kind = CArg::Kind::direct;
            for (std::uint64_t e = 0; e * 8 < size; ++e) {
                std::uint64_t bytes = std::min<std::uint64_t>(8, size - e * 8);
                bool          sse = true;
                int           floats = 0;
                bool          is_double = false;
                for (const auto& [lt, off] : leaves) {
                    if (off / 8 != e) continue;
                    llvm::Type* scalar = lt->getScalarType();
                    if (!scalar->isFloatingPointTy()) sse = false;
                    if (scalar->isFloatTy()) floats += lt->isVectorTy() ? static_cast<int>(llvm::cast<llvm::FixedVectorType>(lt)->getNumElements()) : 1;
                    if (scalar->isDoubleTy()) is_double = true;
                }
                if (sse && is_double) a.parts.push_back(llvm::Type::getDoubleTy(ctx_));
                else if (sse && floats >= 2) a.parts.push_back(llvm::FixedVectorType::get(llvm::Type::getFloatTy(ctx_), 2));
                else if (sse && floats == 1 && bytes <= 4) a.parts.push_back(llvm::Type::getFloatTy(ctx_));
                else if (sse) a.parts.push_back(llvm::Type::getDoubleTy(ctx_));
                else a.parts.push_back(llvm::Type::getIntNTy(ctx_, static_cast<unsigned>(bytes * 8)));
            }
            return a;
        }
        if (triple.getArch() == llvm::Triple::aarch64) {
            llvm::Type* first = leaves.empty() ? nullptr : leaves[0].first;
            bool hfa = first && first->isFloatingPointTy() && leaves.size() <= 4;
            for (const auto& [lt, off] : leaves) hfa = hfa && lt == first;
            if (hfa) {
                a.kind = CArg::Kind::direct;
                a.parts.push_back(llvm::ArrayType::get(first, leaves.size()));
                return a;
            }
            if (size > 16) {
                a.kind = CArg::Kind::memory;   // a pointer to a copy; a result goes through x8 (sret)
                return a;
            }
            a.kind = CArg::Kind::direct;
            if (dl.getABITypeAlign(t).value() == 16) a.parts.push_back(llvm::Type::getInt128Ty(ctx_));
            else if (size <= 8) a.parts.push_back(llvm::Type::getInt64Ty(ctx_));
            else a.parts.push_back(llvm::ArrayType::get(llvm::Type::getInt64Ty(ctx_), 2));
            return a;
        }
        unsupported(span, std::format("passing structs by value to C on {} (pass a std::raw_ptr)", triple.getArchName().str()));
    }

    // The C registers of the struct value `v` (stored to memory and read back as `a.parts`).
    std::vector<llvm::Value*> c_split(const CArg& a, llvm::Value* v) {
        llvm::AllocaInst* buffer = c_buffer(a);
        b_.CreateStore(v, buffer);
        std::vector<llvm::Value*> out;
        std::uint64_t offset = 0;
        for (llvm::Type* part : a.parts) {
            llvm::Value* at = b_.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(ctx_), buffer, offset);
            out.push_back(b_.CreateLoad(part, at));
            offset += mod_->getDataLayout().getTypeAllocSize(part);
        }
        return out;
    }

    // The struct value from its C registers.
    llvm::Value* c_join(const CArg& a, const std::vector<llvm::Value*>& parts) {
        llvm::AllocaInst* buffer = c_buffer(a);
        std::uint64_t offset = 0;
        for (llvm::Value* part : parts) {
            llvm::Value* at = b_.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(ctx_), buffer, offset);
            b_.CreateStore(part, at);
            offset += mod_->getDataLayout().getTypeAllocSize(part->getType());
        }
        return b_.CreateLoad(a.edax, buffer);
    }

    // A buffer that holds both the struct and its registers (which may be larger: a 12-byte
    // struct travels in two 8-byte registers).
    llvm::AllocaInst* c_buffer(const CArg& a) {
        const llvm::DataLayout& dl = mod_->getDataLayout();
        std::uint64_t size = dl.getTypeAllocSize(a.edax);
        std::uint64_t regs = 0;
        for (llvm::Type* p : a.parts) regs += dl.getTypeAllocSize(p);
        llvm::AllocaInst* buffer = entry_alloca(llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx_), std::max(size, regs)), "c_abi");
        buffer->setAlignment(llvm::Align(16));
        return buffer;
    }

    // Calls the extern function `fn` with Edax values: structs are passed and returned as C does.
    llvm::Value* c_call(llvm::Function* fn, const std::vector<llvm::Value*>& edax_args) {
        auto it = c_sigs_.find(fn);
        if (it == c_sigs_.end()) return b_.CreateCall(fn, edax_args);
        const CSignature& sig = it->second;
        std::vector<llvm::Value*> args;
        llvm::AllocaInst* sret = nullptr;
        if (sig.result.kind == CArg::Kind::memory) {
            sret = entry_alloca(sig.result.edax, "c_result");
            args.push_back(sret);
        }
        for (std::size_t i = 0; i < edax_args.size() && i < sig.params.size(); ++i) {
            const CArg& a = sig.params[i];
            if (a.kind == CArg::Kind::direct) {
                for (llvm::Value* part : c_split(a, edax_args[i])) args.push_back(part);
            } else if (a.kind == CArg::Kind::memory) {
                llvm::AllocaInst* copy = entry_alloca(a.edax, "c_arg");
                b_.CreateStore(edax_args[i], copy);
                args.push_back(copy);
            } else {
                args.push_back(edax_args[i]);
            }
        }
        llvm::CallInst* call = b_.CreateCall(fn, args);
        call->setAttributes(fn->getAttributes());
        if (sret) return b_.CreateLoad(sig.result.edax, sret);
        if (sig.result.kind == CArg::Kind::direct) {
            std::vector<llvm::Value*> parts;
            if (sig.result.parts.size() == 1) parts.push_back(call);
            else {
                for (unsigned k = 0; k < sig.result.parts.size(); ++k) parts.push_back(b_.CreateExtractValue(call, k));
            }
            return c_join(sig.result, parts);
        }
        return call;
    }

    // A function's result type; `deduce` uses the type the checker found (spec 15).
    const types::Type* result_type(const FunctionDecl& f, const std::map<const Symbol*, const types::Type*>* subst) {
        if (f.result->kind == TypeKind::deduce) {
            if (!f.deduced_result) unsupported(f.result->span, "this 'deduce' function");
            return f.deduced_result;
        }
        return tc_.lower(*f.result, subst);
    }

    // --- lambdas (spec 18) ----------------------------------------------------------------

    // A lambda's value: its captures, in the order written. A value capture holds the value, a
    // reference capture the address; a lambda without captures holds one unused byte.
    llvm::StructType* closure_type(const types::Type* t) {
        if (auto it = closure_types_.find(t); it != closure_types_.end()) return it->second;
        const LambdaExpr* l = tc_.lambda_of(t);
        std::vector<llvm::Type*> fields;
        for (const Capture& c : l->captures) {
            const types::Type* ct = c.symbol ? c.symbol->type : nullptr;
            if (!ct) unsupported(c.span, "this capture");
            fields.push_back(ct->is(Kind::reference) ? ptr_type() : lower_type(ct));
        }
        if (fields.empty()) fields.push_back(llvm::Type::getInt8Ty(ctx_));
        auto* st = llvm::StructType::create(ctx_, fields, "closure." + short_hash(type_key(t)));
        return closure_types_[t] = st;
    }

    // The address of a local's object (through a reference local, what it refers to).
    llvm::Value* local_address(const Symbol* s, Span span) {
        auto it = locals_.find(s);
        if (it == locals_.end()) unsupported(span, "capturing this name");
        if (s->type && s->type->is(Kind::reference)) return b_.CreateLoad(ptr_type(), it->second);
        return it->second;
    }

    // `[captures](params) { body }`: builds the closure value; the body becomes a function of its own.
    // `[a, b, c]` as the std::array, std::raw::array or std::span its context gave it. A span's
    // elements are static data when they are all constants, else a temporary array in the frame.
    llvm::Value* list_value(ListExpr& l) {
        const types::Type* t = types::strip_ref(static_cast<Expr&>(l).type);
        if (!t) unsupported(l.span, "this list literal");
        const types::Type* element = nullptr;
        bool               to_array = false, to_span = false;
        if (t->is(Kind::array)) {
            element  = t->inner;
            to_array = true;
        } else if (types::is_std_instance(t, "array") && t->symbol->template_args.size() == 2) {
            element  = t->symbol->template_args[0];
            to_array = true;
        } else if ((types::is_std_instance(t, "span") || types::is_std_instance(t, "span_mut")) &&
                   t->symbol->template_args.size() == 1) {
            element = t->symbol->template_args[0];
            to_span = true;
        }
        if (t->is(Kind::vector)) {   // `f32x8 ramp = [0.0, 1.0, ...]`
            llvm::Value* v = llvm::UndefValue::get(lower_type(t));
            for (std::size_t i = 0; i < l.elements.size(); ++i) v = b_.CreateInsertElement(v, gen_expr(*l.elements[i]), static_cast<std::uint64_t>(i));
            return v;
        }
        if (!element) unsupported(l.span, std::format("a list literal of type '{}'", types::to_string(t)));
        llvm::Type*                 et = lower_type(element);
        auto*                       at = llvm::ArrayType::get(et, l.elements.size());
        std::vector<llvm::Value*>   values;
        bool                        constant = true;
        for (ExprPtr& x : l.elements) {
            values.push_back(gen_expr(*x));
            moved_out(*x);
            constant = constant && llvm::isa<llvm::Constant>(values.back());
        }
        llvm::Value* array = nullptr;
        if (constant) {
            std::vector<llvm::Constant*> cs;
            for (llvm::Value* v : values) cs.push_back(llvm::cast<llvm::Constant>(v));
            array = llvm::ConstantArray::get(at, cs);
        } else {
            array = llvm::UndefValue::get(at);
            for (std::size_t i = 0; i < values.size(); ++i) array = b_.CreateInsertValue(array, values[i], {static_cast<unsigned>(i)});
        }
        if (to_array) {
            if (t->is(Kind::array)) return array;
            return build_struct(t->symbol, {{"items", array}});
        }
        (void)to_span;
        llvm::Value* data = nullptr;
        if (constant && !types::is_std_instance(t, "span_mut")) {
            auto* g = new llvm::GlobalVariable(*mod_, at, true, llvm::GlobalValue::PrivateLinkage, llvm::cast<llvm::Constant>(array), "list");
            data    = g;
        } else {
            llvm::AllocaInst* slot = entry_alloca(at, "list");
            b_.CreateStore(array, slot);
            data = slot;
        }
        llvm::Value* count = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), l.elements.size());
        return build_struct(t->symbol, {{"data", data}, {"count", count}});
    }

    llvm::Value* lambda_value(LambdaExpr& l) {
        const types::Type* t  = static_cast<Expr&>(l).type;
        llvm::StructType*  st = closure_type(t);
        llvm::AllocaInst*  slot = entry_alloca(st, "closure");
        b_.CreateStore(llvm::Constant::getNullValue(st), slot);
        for (std::size_t i = 0; i < l.captures.size(); ++i) {
            Capture&     c = l.captures[i];
            llvm::Value* v = nullptr;
            if (c.kind == CaptureKind::init) {
                v = gen_expr(*c.init);
                moved_out(*c.init);
            } else if (c.kind == CaptureKind::ref || c.kind == CaptureKind::mut_ref) {
                v = local_address(c.captured, c.span);
            } else {
                v = b_.CreateLoad(lower_type(c.symbol->type), local_address(c.captured, c.span));
                if (c.kind == CaptureKind::move) {
                    if (auto it = owned_.find(c.captured); it != owned_.end()) b_.CreateStore(llvm::ConstantInt::getFalse(ctx_), it->second.flag);
                }
            }
            b_.CreateStore(v, b_.CreateStructGEP(st, slot, static_cast<unsigned>(i)));
        }
        lambda_function(l);
        return b_.CreateLoad(st, slot);
    }

    llvm::Function* lambda_function(const LambdaExpr& l) {
        if (auto it = lambda_fns_.find(&l); it != lambda_fns_.end()) return it->second;
        const types::Type* t = static_cast<const Expr&>(l).type;
        std::vector<llvm::Type*> params{ptr_type()};
        for (std::size_t i = 0; i + 1 < t->args.size(); ++i) params.push_back(lower_type(t->args[i]));
        llvm::Type* ret = is_void(t->args.back()) ? llvm::Type::getVoidTy(ctx_) : lower_type(t->args.back());
        auto* fn = llvm::Function::Create(llvm::FunctionType::get(ret, params, false), llvm::Function::InternalLinkage,
                                          local_helper_name("rl_", l.span), mod_.get());
        fn->addFnAttr(llvm::Attribute::NoUnwind);
        inherit_unit(fn, cur_fn_);
        {   // a lambda is optimized like the code around it: its file's level, the library's own level (spec 34)
            std::string level = file_optimization(l.span);
            if (level.empty() && sources_.file(l.span.file).path().starts_with("<std>")) level = mode_setting("std_optimization");
            if (!level.empty()) fn->addFnAttr("edax-optimize", level);
        }
        // The aliasing rule holds at a lambda's calls as at a function's (spec 34 §1).
        if (alias_attributes_) {
            for (std::size_t i = 0; i + 1 < t->args.size(); ++i) {
                const types::Type* p = t->args[i];
                unsigned           at = static_cast<unsigned>(i) + 1;
                if (p->is(Kind::reference) && p->inner && !is_interface(p->inner)) {
                    if (p->is_mut) {
                        fn->addParamAttr(at, llvm::Attribute::NoAlias);
                    } else if (is_frozen(p->inner)) {
                        fn->addParamAttr(at, llvm::Attribute::NoAlias);
                        fn->addParamAttr(at, llvm::Attribute::ReadOnly);
                    }
                } else if (!p->is(Kind::reference)) {
                    mark_span(fn, at, p);
                }
            }
        }
        lambda_fns_[&l] = fn;
        if (std::string outer = function_name(cur_fn_); !outer.empty()) fn_names_[fn] = "lambda in " + outer;
        else fn_names_[fn] = "lambda";
        lambda_queue_.push_back(&l);
        return fn;
    }

    void gen_lambda(const LambdaExpr& l) {
        llvm::Function*    fn = lambda_fns_[&l];
        const types::Type* t  = static_cast<const Expr&>(l).type;
        cur_                  = l.span;
        try {
            cur_fn_ = fn;
            debug_function(fn, l.span, "lambda");
            start_inspecting(l.span);
            locals_.clear();
            loops_.clear();
            scopes_.clear();
            owned_.clear();
            self_   = nullptr;
            result_ = t->args.back();
            b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn));
            open_scope();
            llvm::Value*      env = fn->getArg(0);
            llvm::StructType* st  = closure_type(t);
            for (std::size_t i = 0; i < l.captures.size(); ++i) {
                if (l.captures[i].symbol) locals_[l.captures[i].symbol] = b_.CreateStructGEP(st, env, static_cast<unsigned>(i));
            }
            for (std::size_t i = 0; i < l.params.size(); ++i) {
                llvm::Argument*   a    = fn->getArg(static_cast<unsigned>(i) + 1);
                llvm::AllocaInst* slot = entry_alloca(a->getType(), l.params[i].name);
                b_.CreateStore(a, slot);
                if (l.params[i].symbol) locals_[l.params[i].symbol] = slot;
            }
            llvm::Value* v = gen_block(*l.body);
            if (!terminated()) {
                record_drops(0);
                destroy_from(0);
                if (fn->getReturnType()->isVoidTy()) b_.CreateRetVoid();
                else if (v) b_.CreateRet(v);
                else b_.CreateUnreachable();
            }
            scopes_.clear();
            add_alias_scopes(fn);
        } catch (const Unsupported&) {
        }
    }

    // Calls the lambda held by `callee` (its type says which lambda it is).
    llvm::Value* closure_call(Expr& callee, const types::Type* t, std::vector<Expr*> args) {
        const LambdaExpr* l   = tc_.lambda_of(t);
        llvm::Value*      env = object_address(callee);
        std::vector<llvm::Value*> values{env};
        for (std::size_t i = 0; i < args.size() && i + 1 < t->args.size(); ++i) {
            if (t->args[i]->is(Kind::reference)) {
                values.push_back(reference_to(*args[i]));
            } else {
                values.push_back(gen_expr(*args[i]));
                moved_out(*args[i]);
            }
        }
        llvm::Value* r = b_.CreateCall(lambda_function(*l), values);
        if (t->args.back()->is(Kind::reference)) return b_.CreateLoad(lower_type(types::strip_ref(t->args.back())), r);
        return r;
    }

    // --- types ------------------------------------------------------------------------------

    // The LLVM type of a value of Edax type `t`. Booleans are `i1` in values; they are stored as
    // `i8` in memory once structs are lowered (see the interop layout notes).
    llvm::Type* lower_type(const types::Type* t) {
        if (!t) unsupported(cur_, "values of an unknown type");
        switch (t->kind) {
        case Kind::int_: return llvm::IntegerType::get(ctx_, static_cast<unsigned>(t->bits));
        case Kind::float_: return t->bits == 16 ? llvm::Type::getHalfTy(ctx_) : t->bits == 32 ? llvm::Type::getFloatTy(ctx_) : t->bits == 128 ? llvm::Type::getFP128Ty(ctx_) : llvm::Type::getDoubleTy(ctx_);
        case Kind::bool_: return llvm::Type::getInt1Ty(ctx_);
        case Kind::char_: return llvm::Type::getInt32Ty(ctx_);
        case Kind::void_:
        case Kind::never: return llvm::Type::getVoidTy(ctx_);
        case Kind::reference:
            if (is_interface(t->inner)) return llvm::StructType::get(ctx_, {ptr_type(), ptr_type()});   // { object, table }
            return llvm::PointerType::get(ctx_, 0);
        case Kind::pointer:
            if (is_interface(t->inner)) return llvm::StructType::get(ctx_, {ptr_type(), ptr_type()});   // { object, table }
            return llvm::PointerType::get(ctx_, 0);
        case Kind::user: return user_type(t->symbol);
        case Kind::static_str: return str_type();
        case Kind::closure: return closure_type(t);
        case Kind::function:
            if (t->bits > 0) {   // std::inline_function (spec 34): { captures inline, code, drop }
                return llvm::StructType::get(ctx_, {llvm::ArrayType::get(llvm::Type::getInt64Ty(ctx_), static_cast<unsigned>(t->bits / 8)),
                                                    ptr_type(), ptr_type()});
            }
            return llvm::StructType::get(ctx_, {ptr_type(), ptr_type(), ptr_type()});   // { captures, code, drop }
        case Kind::fn_ptr: return ptr_type();
        case Kind::array: return llvm::ArrayType::get(lower_type(t->inner), tc_.array_count(t));
        case Kind::vector: return llvm::FixedVectorType::get(lower_type(t->inner), static_cast<unsigned>(t->bits));
        case Kind::mask: return llvm::FixedVectorType::get(llvm::Type::getInt1Ty(ctx_), static_cast<unsigned>(t->bits));
        case Kind::tuple: {
            std::vector<llvm::Type*> elements;
            for (const types::Type* e : t->args) elements.push_back(lower_type(e));
            return llvm::StructType::get(ctx_, elements);
        }
        case Kind::external:
            if (t->name == "std::str") return str_type();
            if (t->name == "std::format_args") return format_args_type();
            if (t->name == "std::ordering") return llvm::Type::getInt8Ty(ctx_);
            break;
        default: break;
        }
        unsupported(cur_, std::format("values of type '{}'", types::to_string(t)));
    }

    static bool is_void(const types::Type* t) { return !t || t->is(Kind::void_) || t->is(Kind::never); }

    // --- functions --------------------------------------------------------------------------

    // The body of a `@std::simd::multiversion` function: the first call asks the CPU which copy
    // to use (the highest level it has, else the build's own) and keeps it; every call calls it.
    void gen_dispatcher(const Symbol* s) {
        llvm::Function* fn    = fns_[s];
        auto*           ptr   = ptr_type();
        auto*           cache = new llvm::GlobalVariable(*mod_, ptr, false, llvm::GlobalValue::InternalLinkage,
                                                         llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0)),
                                                         fn->getName() + ".version");
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx_, "entry", fn);
        llvm::BasicBlock* pick  = llvm::BasicBlock::Create(ctx_, "pick", fn);
        llvm::BasicBlock* call  = llvm::BasicBlock::Create(ctx_, "call", fn);
        b_.SetInsertPoint(entry);
        llvm::LoadInst* known = b_.CreateLoad(ptr, cache);
        known->setAtomic(llvm::AtomicOrdering::Monotonic);
        b_.CreateCondBr(b_.CreateIsNull(known), pick, call);
        b_.SetInsertPoint(pick);
        llvm::Function* has_level = runtime("edax_cpu_has_level", llvm::Type::getInt1Ty(ctx_), {llvm::Type::getInt32Ty(ctx_)});
        // The listed levels from the last (the highest, as written) down; the build's own copy last.
        std::vector<const Symbol*> listed(s->versions.begin(), s->versions.end() - 1);
        llvm::Value* chosen = function_of(s->versions.back());
        for (const Symbol* v : listed) {
            llvm::Value* has = b_.CreateCall(has_level, {llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), static_cast<std::uint64_t>(cpu_level_number(v->cpu_level)))});
            chosen = b_.CreateSelect(has, function_of(v), chosen);
        }
        llvm::StoreInst* keep = b_.CreateStore(chosen, cache);
        keep->setAtomic(llvm::AtomicOrdering::Monotonic);
        keep->setAlignment(llvm::Align(8));
        b_.CreateBr(call);
        b_.SetInsertPoint(call);
        llvm::PHINode* target = b_.CreatePHI(ptr, 2);
        target->addIncoming(known, entry);
        target->addIncoming(chosen, pick);
        std::vector<llvm::Value*> args;
        for (llvm::Argument& a : fn->args()) args.push_back(&a);
        llvm::CallInst* r = b_.CreateCall(fn->getFunctionType(), target, args);
        if (fn->getReturnType()->isVoidTy()) b_.CreateRetVoid();
        else b_.CreateRet(r);
    }

    // `variant`: the copy of a function with run_time entry checks that takes its caller's
    // location as a last parameter (see site_variant_of).
    void gen_function(const Symbol* s, llvm::Function* variant = nullptr) {
        if (!s->versions.empty() && !variant) {
            gen_dispatcher(s);
            return;
        }
        const FunctionDecl* f  = body_decl(s);
        llvm::Function*     fn = variant ? variant : fns_[s];
        site_arg_              = variant ? variant->getArg(static_cast<unsigned>(variant->arg_size() - 1)) : nullptr;
        auto* saved_insights   = options_.insights;
        if (variant) options_.insights = nullptr;   // insights describe each function once
        cur_                   = s->span;
        {   // @std::simd::must_vectorize: its plain loops are tagged and checked after optimizing
            const Symbol* annotated = s->version_of ? s->version_of : (s->template_of && s->template_of->kind == s->kind ? s->template_of : s);
            must_vectorize_ = annotated->decl && sema::find_annotation(annotated->decl->annotations, "std::simd::must_vectorize");
            // @std::float_mode (spec 34): fast-math flags on the float operations of this body.
            // Without it: strict IEEE (no contraction, no reordering).
            llvm::FastMathFlags fmf;
            unsigned            mode = annotated->float_mode;
            if (mode & sema::float_mode_contract) fmf.setAllowContract();
            if (mode & sema::float_mode_reassociate) fmf.setAllowReassoc();
            if (mode & sema::float_mode_finite_only) {
                fmf.setNoNaNs();
                fmf.setNoInfs();
            }
            if (mode & sema::float_mode_no_signed_zeros) fmf.setNoSignedZeros();
            if (mode & sema::float_mode_reciprocal) {
                fmf.setAllowReciprocal();
                fmf.setApproxFunc();
            }
            b_.setFastMathFlags(fmf);
        }
        try {
            cur_fn_ = fn;
            fn_names_[fn] = display_name(s);
            debug_function(fn, s->span, options_.debug_variables ? display_name(s) : s->name);
            locals_.clear();
            loops_.clear();
            scopes_.clear();
            self_ = nullptr;
            start_inspecting(s->span);
            const auto* subst = sema::substitution_of(s);
            cur_subst_        = subst;
            result_           = f->result ? result_type(*f, subst) : nullptr;
            llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx_, "entry", fn);
            b_.SetInsertPoint(entry);
            open_scope();
            unsigned offset = 0;
            this_owned_.reset();
            if (has_self(s)) {
                self_   = fn->getArg(0);
                offset  = 1;
                if (options_.debug_variables && dib_ && s->parent) {   // `this`, for the debugger
                    llvm::AllocaInst* this_slot = entry_alloca(self_->getType(), "this");
                    b_.CreateStore(self_, this_slot);
                    debug_variable(this_slot, "this", tc_.reference(tc_.user(s->parent), true), s->span, 1, true);
                }
                const types::Type* st = s->parent ? tc_.user(s->parent) : nullptr;
                if (f->is_move && st && needs_drop(st)) {   // a consuming method destroys what is left of `this` (spec 06)
                    llvm::AllocaInst* flag = entry_alloca(llvm::Type::getInt1Ty(ctx_), "df_this");
                    b_.CreateStore(llvm::ConstantInt::getTrue(ctx_), flag);
                    scopes_.back().locals.push_back(OwnedLocal{self_, flag, drop_function(st)});
                    this_owned_ = OwnedLocal{self_, flag, drop_function(st), nullptr, st};   // `return this;` moves it out
                }
            }
            for (std::size_t i = 0; i < f->params.size(); ++i) {
                const Param& p = f->params[i];
                cur_           = p.span;
                llvm::Argument*   a    = fn->getArg(static_cast<unsigned>(i) + offset);
                llvm::AllocaInst* slot = entry_alloca(a->getType(), p.name);
                b_.CreateStore(a, slot);
                if (!p.symbol) continue;
                locals_[p.symbol] = slot;
                const types::Type* pt = tc_.lower(*p.type, subst);
                if (inspect_) slot_info_[slot] = SlotInfo{p.name, types::to_string(pt), "parameter"};
                debug_variable(slot, p.name, pt, p.span, static_cast<unsigned>(i + 1 + offset));
                if (needs_drop(pt)) own(p.symbol, slot, pt, true);
            }
            llvm::Value* v = gen_block(*f->body, result_ && result_->is(Kind::reference));
            if (!terminated()) {
                record_drops(0);
                destroy_from(0);
                if (fn->getReturnType()->isVoidTy()) b_.CreateRetVoid();
                else if (v) b_.CreateRet(v);
                else b_.CreateUnreachable();
            }
            scopes_.clear();
            record_frame(fn, s, *f->body);
            add_alias_scopes(fn);
        } catch (const Unsupported&) {
            // Reported. The function keeps a partial body; the module is discarded.
        }
        inspect_          = false;
        site_arg_         = nullptr;
        options_.insights = saved_insights;
    }

    // A function with run_time entry checks reports its caller's line when a check fails (spec 23).
    // Direct calls go to this copy, which takes the caller's location as a hidden last parameter:
    // after inlining it is a constant on the failure path, so the hot path has no extra stores
    // (spec 34). Indirect calls and the library's own calls use the original, which reads the
    // thread's call site (null: the check's own line).
    llvm::Function* site_variant_of(const Symbol* s) {
        if (auto it = site_fns_.find(s); it != site_fns_.end()) return it->second;
        llvm::Function*          original = function_of(s);
        llvm::FunctionType*      ft       = original->getFunctionType();
        std::vector<llvm::Type*> params(ft->param_begin(), ft->param_end());
        params.push_back(ptr_type());
        auto* fn = llvm::Function::Create(llvm::FunctionType::get(ft->getReturnType(), params, false), llvm::Function::InternalLinkage,
                                          original->getName() + ".at", mod_.get());
        fn->copyAttributesFrom(original);
        site_fns_[s] = fn;
        site_queue_.push_back(s);
        return fn;
    }

    // An alloca in the entry block, so that loops do not grow the stack.
    llvm::AllocaInst* entry_alloca(llvm::Type* t, const std::string& name) {
        llvm::Function*   in    = b_.GetInsertBlock() ? b_.GetInsertBlock()->getParent() : cur_fn_;
        llvm::BasicBlock& entry = in->getEntryBlock();
        llvm::IRBuilder<> tmp(&entry, entry.begin());
        return tmp.CreateAlloca(t, nullptr, name);
    }

    bool terminated() const { return b_.GetInsertBlock()->getTerminator() != nullptr; }

    // A setting of the build mode (spec 24), "" without the standard library.
    std::string mode_setting(const char* name) const {
        auto it = program_.mode_settings.find(name);
        return it == program_.mode_settings.end() ? std::string() : it->second;
    }

    // After a `return`, `break` or `continue`: following code goes into a block nothing jumps to.
    void open_dead_block() { b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "dead", cur_fn_)); }

    // A piece of the run-time part of a panic message: text, or an integer formatted when the
    // check fails (the operands of an overflow, an index).
    struct PanicPart {
        std::string  text;
        llvm::Value* value     = nullptr;
        bool         is_signed = false;
    };

    static PanicPart part(std::string text) { return PanicPart{std::move(text), nullptr, false}; }
    static PanicPart part(llvm::Value* v, bool is_signed) { return PanicPart{{}, v, is_signed}; }

    // The source text of `span` for a panic message: one line (runs of white space become one
    // space), at most 48 characters.
    std::string snippet(Span span) const {
        std::string_view text = sources_.file(span.file).text();
        if (span.begin >= span.end || span.end > text.size()) return {};
        std::string out;
        bool        space = false;
        for (char ch : text.substr(span.begin, span.end - span.begin)) {
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                space = !out.empty();
                continue;
            }
            if (static_cast<unsigned char>(ch) < 0x20) continue;   // no control characters in a message
            if (space) out += ' ';
            space = false;
            out += ch;
        }
        while (!out.empty() && (out.back() == ';' || out.back() == ' ')) out.pop_back();   // an assignment's span ends with `;`
        if (out.size() > 48) out = out.substr(0, 45) + "...";
        return out;
    }

    // Panics with `message` at `span` unless `ok` holds (spec 16). `ok` is an i1.
    void check(llvm::Value* ok, Span span, const std::string& message) { check(ok, span, message, {}); }

    // The same, with `values` appended to the message when the check fails: `summary` is what
    // the check says before it runs (insights, `edaxc explain-checks`), the values are added to
    // it at run time.
    void check(llvm::Value* ok, Span span, const std::string& summary, const std::vector<PanicPart>& values) {
        if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(ok); c && c->isOne()) return;   // cannot fail
        if (inspect_) {
            insight(span, "checks", "check", "range", "can panic: " + summary,
                    "A run-time check (spec 16, 23): when it fails, the program panics with this message. Checks that cannot fail "
                    "(a constant divisor, an exhaustive match) are not generated.");
        }
        llvm::BasicBlock* bad  = llvm::BasicBlock::Create(ctx_, "panic", cur_fn_);
        llvm::BasicBlock* good = llvm::BasicBlock::Create(ctx_, "ok", cur_fn_);
        b_.CreateCondBr(ok, good, bad);
        b_.SetInsertPoint(bad);
        llvm::DebugLoc saved_loc = b_.getCurrentDebugLocation();
        debug_at(span);   // a debugger stops here: at the expression that failed
        if (options_.panic_trap) {
            b_.CreateCall(intrinsic_declaration(mod_.get(), llvm::Intrinsic::trap, {}));
            b_.CreateUnreachable();
        } else if (values.empty()) {
            b_.CreateCall(edax_panic_str_, {loc_global(span), message_bytes(summary), llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), summary.size())});
            end_panic();
        } else {
            std::string               text = summary;
            std::vector<llvm::Value*> numbers;
            for (const PanicPart& v : values) {
                if (!v.value) {
                    text += v.text;
                    continue;
                }
                text += v.is_signed ? '\x01' : '\x02';
                numbers.push_back(widen_for_panic(v.value, v.is_signed));
            }
            panic_values(loc_global(span), text, numbers, summary);
            end_panic();
        }
        b_.SetCurrentDebugLocation(saved_loc);
        b_.SetInsertPoint(good);
    }

    // An integer as the runtime's i128 for edax_panic_values.
    llvm::Value* widen_for_panic(llvm::Value* v, bool is_signed) {
        auto* i128 = llvm::Type::getInt128Ty(ctx_);
        return is_signed ? b_.CreateSExtOrTrunc(v, i128) : b_.CreateZExtOrTrunc(v, i128);
    }

    // Calls edax_panic_values: `text` with \x01 (signed) and \x02 (unsigned) where the values go,
    // at most two. One call, so a check's failure path doesn't make its function look big to
    // the inliner. `summary` is the message before it runs (edaxc explain-checks).
    void panic_values(llvm::Value* loc, const std::string& text, const std::vector<llvm::Value*>& numbers, const std::string& summary) {
        auto* p    = ptr_type();
        auto* i64  = llvm::Type::getInt64Ty(ctx_);
        auto* i128 = llvm::Type::getInt128Ty(ctx_);
        llvm::Value* zero = llvm::ConstantInt::get(i128, 0);
        llvm::CallInst* call = b_.CreateCall(runtime("edax_panic_values", llvm::Type::getVoidTy(ctx_), {p, p, i64, i128, i128}),
                                             {loc, message_bytes(text), llvm::ConstantInt::get(i64, text.size()),
                                              numbers.size() > 0 ? numbers[0] : zero, numbers.size() > 1 ? numbers[1] : zero});
        call->setMetadata("edax.message", llvm::MDNode::get(ctx_, llvm::MDString::get(ctx_, summary)));
    }

    // The end of a panic block, after the runtime printed the panic: the runtime returns only
    // when a debugger is attached, and then the program stops right here, at the line that
    // panicked (`stop_debugger_on_panic()`, spec 24). Continuing ends the process.
    void end_panic() {
        bool stop = mode_setting("stop_debugger_on_panic") != "false";
        if (stop) b_.CreateCall(intrinsic_declaration(mod_.get(), llvm::Intrinsic::debugtrap, {}));
        b_.CreateCall(runtime("edax_panic_end", llvm::Type::getVoidTy(ctx_), {llvm::Type::getInt1Ty(ctx_)}),
                      {llvm::ConstantInt::getBool(ctx_, stop)});
        b_.CreateUnreachable();
    }

    // Where the current call of a function with run_time entry checks was made (null: unknown,
    // e.g. through a function pointer). Set by the caller around the call (spec 23).
    llvm::GlobalVariable* call_site_global() {
        if (auto* g = mod_->getNamedGlobal("edax_call_site")) return g;
        auto* g = new llvm::GlobalVariable(*mod_, ptr_type(), false, llvm::GlobalValue::InternalLinkage,
                                           llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0)), "edax_call_site");
        g->setThreadLocalMode(own_tls_model());   // each thread has its own call site (spec 27)
        return g;
    }

    // Whether `s` starts with run_time checks that are compiled in.
    bool has_run_time_entry_checks(const Symbol* s) {
        const FunctionDecl* f = body_decl(s);
        if (!f || !f->body) return false;
        for (const StmtPtr& st : f->body->statements) {
            if (st->kind == StmtKind::check) {
                const Check& k = st->as<CheckStmt>().check;
                if (k.is_run_time && k.run_active && k.condition) return true;
                continue;
            }
            if (st->kind != StmtKind::expression || st->as<ExprStmt>().expr->kind != ExprKind::if_) break;
            const auto& gate = st->as<ExprStmt>().expr->as<IfExpr>();   // `if compile_time` around checks
            if (!gate.is_compile_time || gate.taken != 0) break;
            for (const StmtPtr& inner : gate.then_block->statements) {
                if (inner->kind != StmtKind::check) return false;
                const Check& k = inner->as<CheckStmt>().check;
                if (k.is_run_time && k.run_active && k.condition && k.is_entry) return true;
            }
        }
        return false;
    }

    // How panics name the function being generated: `geo::polygon::area`, `lambda in main`
    // ("" for the compiler's own helpers).
    std::string function_name(const llvm::Function* fn) const {
        auto it = fn ? fn_names_.find(fn) : fn_names_.end();
        return it == fn_names_.end() ? std::string() : it->second;
    }

    // The source location `edax_loc` global for `span`: `{ file, line, column, function }`. The
    // function is the one being generated (a C string, or null), which panics name.
    llvm::Constant* loc_global(Span span) {
        std::string function = function_name(cur_fn_);
        auto key = std::make_tuple(span.file, span.begin, function);
        if (auto it = locs_.find(key); it != locs_.end()) return it->second;
        const SourceFile& file = sources_.file(span.file);
        LineCol           lc   = file.line_col(span.begin);
        llvm::Constant*   path = string_global(file.path());
        llvm::Constant*   name = function.empty() ? static_cast<llvm::Constant*>(llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0)))
                                                  : string_global(function);
        llvm::Constant*   init = llvm::ConstantStruct::get(
            loc_type_, {path, llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), lc.line),
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), lc.column), name});
        auto* g = new llvm::GlobalVariable(*mod_, loc_type_, true, llvm::GlobalValue::PrivateLinkage, init,
                                           std::format("edax_loc_{}", locs_.size()));
        locs_[key] = g;
        return g;
    }

    // --- statements -------------------------------------------------------------------------

    // A block: its locals are destroyed at its end. The value is the tail expression's, or its
    // address when the function returns a reference.
    llvm::Value* gen_block(BlockExpr& blk, bool as_address = false) {
        open_scope();
        for (StmtPtr& st : blk.statements) gen_stmt(*st);
        llvm::Value* v = nullptr;
        if (blk.tail) {
            debug_at(blk.tail->span);   // a tail without ';' is a line of its own for a debugger
            bool address = as_address && !is_void(types::strip_ref(blk.tail->type));
            v = address ? object_address(*blk.tail) : gen_expr(*blk.tail);
            if (!terminated()) moved_out(*blk.tail);
        }
        drop_site_ = Span{blk.span.file, blk.span.end > blk.span.begin ? blk.span.end - 1 : blk.span.end, blk.span.end};
        drop_kind_ = "drop";
        close_scope();
        return v;
    }

    // One statement. An unsupported construct is reported and the rest of the function still
    // generates, so that every such construct gets its error.
    void gen_stmt(Stmt& s) {
        std::size_t depth = scopes_.size();
        std::size_t mark  = depth ? scopes_.back().locals.size() : 0;
        try {
            gen_stmt_inner(s);
        } catch (const Unsupported&) {
        }
        end_of_statement(s, depth, mark);
    }

    // Temporaries are destroyed at the end of the full statement (spec 11), so a lock guard
    // in `bool ok = m.try_lock().is_some();` is unlocked right away. The temporaries of a variable's
    // initializer that may borrow from them (a view, a reference, a struct) live until the end
    // of the block instead.
    void end_of_statement(Stmt& s, std::size_t depth, std::size_t mark) {
        if (depth == 0 || scopes_.size() != depth) return;
        if (s.kind == StmtKind::variable) {
            const Symbol*      sym = s.as<VariableStmt>().symbol;
            const types::Type* t   = sym ? sym->type : nullptr;
            bool plain = t && (t->is_integer() || t->is_float() || t->is(Kind::bool_) || t->is(Kind::char_));
            if (!plain) return;
        }
        if (s.kind == StmtKind::destructure) return;
        std::vector<OwnedLocal>& locals = scopes_.back().locals;
        if (mark >= locals.size()) return;
        std::vector<OwnedLocal> ending, kept;
        for (std::size_t i = mark; i < locals.size(); ++i) {
            bool temporary = !locals[i].sym && temporaries_.contains(locals[i].slot);
            (temporary ? ending : kept).push_back(locals[i]);
        }
        if (ending.empty()) return;
        if (!terminated()) {
            for (auto it = ending.rbegin(); it != ending.rend(); ++it) drop_if(it->flag, it->drop, it->slot);
        }
        locals.resize(mark);
        for (OwnedLocal& o : kept) locals.push_back(o);
    }

    void gen_stmt_inner(Stmt& s) {
        cur_ = s.span;
        debug_at(s.span);
        switch (s.kind) {
        case StmtKind::variable: return variable_stmt(s.as<VariableStmt>());
        case StmtKind::assign: return assign_stmt(s.as<AssignStmt>());
        case StmtKind::discard: discard(*s.as<DiscardStmt>().value); return;
        case StmtKind::expression: discard(*s.as<ExprStmt>().expr); return;
        case StmtKind::check: return run_time_check(s.as<CheckStmt>().check, s.span);
        case StmtKind::destructure: return destructure_stmt(s.as<DestructureStmt>());
        }
    }

    // `(T1 a, T2 b) = expr;`: the tuple's elements move into new variables; elements given to
    // `_` are destroyed.
    void destructure_stmt(DestructureStmt& d) {
        const types::Type* t = types::strip_ref(d.init->type);
        if (!t || !t->is(Kind::tuple)) unsupported(d.span, "destructuring this value");
        if (d.by_reference) {   // `for (auto (a, b) : list)`: the names refer into the element
            llvm::Value* base = reference_to(*d.init);
            llvm::Type*  lt   = lower_type(t);
            for (std::size_t i = 0; i < d.bindings.size(); ++i) {
                DestructureBinding& b = d.bindings[i];
                if (b.wildcard || !b.symbol) continue;
                llvm::Value*      part = b_.CreateStructGEP(lt, base, static_cast<unsigned>(i));
                bool              ref  = b.symbol->type->is(Kind::reference);
                llvm::AllocaInst* slot = entry_alloca(ref ? ptr_type() : lower_type(b.symbol->type), b.name);
                locals_[b.symbol]      = slot;
                debug_variable(slot, b.name, b.symbol->type, b.span);
                if (inspect_) slot_info_[slot] = SlotInfo{b.name, types::to_string(b.symbol->type), "variable"};
                b_.CreateStore(ref ? part : b_.CreateLoad(lower_type(b.symbol->type), part), slot);
            }
            return;
        }
        llvm::Value* value = gen_expr(*d.init);
        moved_out(*d.init);
        for (std::size_t i = 0; i < d.bindings.size(); ++i) {
            DestructureBinding& b  = d.bindings[i];
            const types::Type*  et = t->args[i];
            llvm::Value*        el = b_.CreateExtractValue(value, {static_cast<unsigned>(i)});
            if (b.wildcard || !b.symbol) {
                if (needs_drop(et)) {
                    llvm::AllocaInst* tmp = entry_alloca(lower_type(et), "discarded");
                    b_.CreateStore(el, tmp);
                    b_.CreateCall(drop_function(et), {tmp});
                }
                continue;
            }
            llvm::AllocaInst* slot = entry_alloca(lower_type(b.symbol->type), b.name);
            locals_[b.symbol]      = slot;
            debug_variable(slot, b.name, b.symbol->type, b.span);
            if (inspect_) slot_info_[slot] = SlotInfo{b.name, types::to_string(b.symbol->type), "variable"};
            bool owns = needs_drop(b.symbol->type);
            if (owns) own(b.symbol, slot, b.symbol->type, false);
            b_.CreateStore(el, slot);
            if (owns) set_flag(b.symbol, true);
        }
    }

    void variable_stmt(VariableStmt& v) {
        cur_ = v.name_span;
        if (v.storage == Storage::compile_time) return;   // a constant: its uses are constants (spec 15)
        if (v.storage != Storage::local) unsupported(v.name_span, "static local variables");
        if (!v.symbol || !v.symbol->type) unsupported(v.name_span, "variables of an unknown type");
        // The initializer first: if it is unsupported, that is the error to show (not its type).
        llvm::Value* init = nullptr;
        if (v.init) {
            const types::Type* vt = v.symbol->type;
            init = vt->is(Kind::reference) && is_interface(vt->inner) ? interface_reference(*v.init, vt->inner->symbol)
                 : vt->is(Kind::reference)                           ? reference_to(*v.init)
                                                                      : gen_expr(*v.init);
        }
        if (v.init && !v.symbol->type->is(Kind::reference)) moved_out(*v.init);
        llvm::AllocaInst* slot = entry_alloca(lower_type(v.symbol->type), v.name);
        locals_[v.symbol]      = slot;
        debug_variable(slot, v.name, v.symbol->type, v.name_span);
        if (inspect_) {
            slot_info_[slot] = SlotInfo{v.name, types::to_string(v.symbol->type), "variable"};
            track_init(v.symbol, v.init.get());
            if (v.init) record_copy(*v.init, v.symbol->type);
        }
        bool owns = needs_drop(v.symbol->type);
        if (owns) own(v.symbol, slot, v.symbol->type, false);
        if (init) b_.CreateStore(init, slot);
        if (owns && init) set_flag(v.symbol, true);
    }

    void assign_stmt(AssignStmt& a) {
        cur_ = a.span;
        if (!is_place(*a.target)) unsupported(a.target->span, "assignments to this place");
        const types::Type* dt = types::strip_ref(a.target->type);
        if (a.op == TokenKind::eq && needs_drop(dt)) {
            // The new value is computed first; then the old one is destroyed and replaced.
            llvm::Value* nv = gen_expr(*a.value);
            moved_out(*a.value);
            if (const OwnedLocal* o = owned_path(*a.target)) {
                drop_if(o->flag, o->drop, o->slot);
                b_.CreateStore(nv, o->slot);
                b_.CreateStore(llvm::ConstantInt::getTrue(ctx_), o->flag);
            } else {
                llvm::Value* addr = place(*a.target);
                b_.CreateCall(drop_function(dt), {addr});
                b_.CreateStore(nv, addr);
            }
            return;
        }
        if (a.op != TokenKind::eq && a.op_symbol) {   // spec 16: `operator+=`, or `a = a + b`
            const FunctionDecl* f = body_decl(a.op_symbol);
            if (!f) unsupported(a.span, "an operator without a body");
            bool compound = f->name != token_kind_spelling(compound_operator(a.op));
            llvm::Value* addr = place(*a.target);
            if (compound) {
                if (has_self(a.op_symbol)) call_symbol(a.op_symbol, addr, {a.value.get()}, a.span);
                else unsupported(a.span, "a free 'operator" + f->name + "'");
                return;
            }
            llvm::Value* nv = has_self(a.op_symbol) ? call_symbol(a.op_symbol, addr, {a.value.get()}, a.span)
                                                    : call_symbol(a.op_symbol, nullptr, {a.target.get(), a.value.get()}, a.span);
            const types::Type* tt = types::strip_ref(a.target->type);
            if (needs_drop(tt)) b_.CreateCall(drop_function(tt), {addr});
            b_.CreateStore(nv, addr);
            return;
        }
        llvm::Value*       addr = place(*a.target);
        const types::Type* t    = types::strip_ref(a.target->type);
        if (a.distinct_underlying) t = a.distinct_underlying;   // the value is at offset 0 (spec 34)
        llvm::Value*       v    = nullptr;
        if (a.op == TokenKind::eq) {
            v = gen_expr(*a.value);
        } else {
            TokenKind op = compound_operator(a.op);
            if (op == TokenKind::error) unsupported(a.span, "this assignment operator");
            llvm::Value* cur = b_.CreateLoad(lower_type(t), addr);
            llvm::Value* rhs = gen_expr(*a.value);
            v = arith(op, t, cur, rhs, a.value->type, a.span);
        }
        b_.CreateStore(v, addr);
    }

    static TokenKind compound_operator(TokenKind op) {
        switch (op) {
        case TokenKind::plus_eq: return TokenKind::plus;
        case TokenKind::minus_eq: return TokenKind::minus;
        case TokenKind::star_eq: return TokenKind::star;
        case TokenKind::slash_eq: return TokenKind::slash;
        case TokenKind::percent_eq: return TokenKind::percent;
        case TokenKind::amp_eq: return TokenKind::amp;
        case TokenKind::pipe_eq: return TokenKind::pipe;
        case TokenKind::caret_eq: return TokenKind::caret;
        case TokenKind::shl_eq: return TokenKind::shl;
        case TokenKind::shr_eq: return TokenKind::shr;
        case TokenKind::plus_percent_eq: return TokenKind::plus_percent;
        case TokenKind::minus_percent_eq: return TokenKind::minus_percent;
        case TokenKind::star_percent_eq: return TokenKind::star_percent;
        default: return TokenKind::error;
        }
    }

    // --- expressions ------------------------------------------------------------------------

    // The value of `e`, or null for an expression of type void or never.
    llvm::Value* gen_expr(Expr& e) {
        if (e.conversion != Expr::Conversion::none && !converting_.count(&e)) {
            converting_.insert(&e);
            llvm::Value* v = implicit_conversion(e);
            converting_.erase(&e);
            return v;
        }
        cur_ = e.span;
        if (e.constant) {   // computed at compile time (spec 15, 26)
            if (llvm::Value* c = materialize(*e.constant, e.span)) return c;
            unsupported(e.span, "a value that exists only at compile time");
        }
        switch (e.kind) {
        case ExprKind::int_literal: return int_literal(e.as<LiteralExpr>(), e.type);
        case ExprKind::float_literal: return float_literal(e.as<LiteralExpr>(), e.type);
        case ExprKind::bool_literal:
            return llvm::ConstantInt::getBool(ctx_, e.as<LiteralExpr>().bool_value);
        case ExprKind::char_literal: {
            const std::string& text = e.as<LiteralExpr>().value;
            char32_t           cp   = 0;
            if (text.empty() || decode_utf8(text, 0, cp) == 0) cp = text.empty() ? 0 : static_cast<unsigned char>(text[0]);
            return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), static_cast<std::uint64_t>(cp));
        }
        case ExprKind::path: return path_value(e);
        case ExprKind::member:
        case ExprKind::this_: return value_of_place(e);
        case ExprKind::struct_literal: return struct_literal(e.as<StructLiteralExpr>());
        case ExprKind::tuple: {
            const types::Type* t = types::strip_ref(e.type);
            if (!t || !t->is(Kind::tuple)) unsupported(e.span, "this tuple");
            llvm::Value* v = llvm::UndefValue::get(lower_type(t));
            auto&        te = e.as<TupleExpr>();
            for (std::size_t i = 0; i < te.elements.size(); ++i) {
                if (i < t->args.size() && t->args[i]->is(Kind::reference)) {   // `(mut this.a, this.b)`: addresses (spec 34)
                    v = b_.CreateInsertValue(v, reference_to(*te.elements[i]), {static_cast<unsigned>(i)});
                    continue;
                }
                llvm::Value* el = gen_expr(*te.elements[i]);
                moved_out(*te.elements[i]);
                v = b_.CreateInsertValue(v, el, {static_cast<unsigned>(i)});
            }
            return v;
        }
        case ExprKind::list: return list_value(e.as<ListExpr>());
        case ExprKind::lambda: return lambda_value(e.as<LambdaExpr>());
        case ExprKind::match: {
            ++cond_depth_;
            llvm::Value* v = match_value(e.as<MatchExpr>(), e.type);
            --cond_depth_;
            return v;
        }
        case ExprKind::bit: return bit_value(e.as<BitExpr>(), e.type);
        case ExprKind::for_: return for_loop(e.as<ForExpr>());
        case ExprKind::cast: return cast_value(e.as<CastExpr>());
        case ExprKind::try_: return try_value(e.as<TryExpr>());
        case ExprKind::nullptr_literal: return llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0));
        case ExprKind::string_literal: {
            auto& lit = e.as<LiteralExpr>();
            if (lit.token_kind == TokenKind::format_string_literal) return format_args_value(lit);
            if (lit.token_kind == TokenKind::c_string_literal) return message_bytes(lit.value + std::string(1, '\0'));
            return str_constant(lit.value);
        }
        case ExprKind::default_: return default_value(types::strip_ref(e.type), e.span);
        case ExprKind::unary:
            if (e.as<UnaryExpr>().op == UnaryOp::deref) return value_of_place(e);
            return unary_value(e.as<UnaryExpr>(), e.type);
        case ExprKind::binary: return binary_value(e.as<BinaryExpr>());
        case ExprKind::index:
            if (e.as<IndexExpr>().op_symbol) return value_of_place(e);
            if (const types::Type* vt = types::strip_ref(e.as<IndexExpr>().object->type); vt && vt->is(Kind::vector)) {   // `v[i]`
                llvm::Value* v = gen_expr(*e.as<IndexExpr>().object);
                auto*        i64 = llvm::Type::getInt64Ty(ctx_);
                llvm::Value* i = b_.CreateZExtOrTrunc(gen_expr(*e.as<IndexExpr>().index), i64);
                check(b_.CreateICmpULT(i, llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(vt->bits))), e.span, "lane index out of bounds",
                      {part(": "), part(i, false), part(std::format(" (lanes: {})", vt->bits))});
                return b_.CreateExtractElement(v, i);
            }
            unsupported(e.span, "indexing");
        case ExprKind::call: {
            llvm::Value* v = call_value(e.as<CallExpr>());
            // A call of a function returning `never` (std::unreachable, an exit helper): nothing
            // runs after it, like after std::panic.
            if (e.type && e.type->is(Kind::never) && b_.GetInsertBlock() && !b_.GetInsertBlock()->getTerminator()) {
                b_.CreateUnreachable();
                open_dead_block();
                return nullptr;
            }
            return v;
        }
        case ExprKind::block: return gen_block(e.as<BlockExpr>());
        case ExprKind::if_: {
            bool real = !e.as<IfExpr>().is_compile_time;
            cond_depth_ += real;
            llvm::Value* v = if_value(e.as<IfExpr>(), e.type);
            cond_depth_ -= real;
            return v;
        }
        case ExprKind::while_: {
            trips_.push_back(std::nullopt);
            llvm::Value* v = while_loop(e.as<WhileExpr>());
            trips_.pop_back();
            return v;
        }
        case ExprKind::return_: return return_value(e.as<ReturnExpr>());
        case ExprKind::break_:
            if (loops_.empty()) unsupported(e.span, "'break' outside a loop");
            drop_site_ = e.span;
            drop_kind_ = "exit-drop";
            record_drops(loop_scope());
            drop_kind_ = "drop";
            destroy_from(loop_scope());
            b_.CreateBr(loops_.back().exit);
            open_dead_block();
            return nullptr;
        case ExprKind::continue_:
            if (loops_.empty()) unsupported(e.span, "'continue' outside a loop");
            drop_site_ = e.span;
            drop_kind_ = "exit-drop";
            record_drops(loop_scope());
            drop_kind_ = "drop";
            destroy_from(loop_scope());
            b_.CreateBr(loops_.back().cont);
            open_dead_block();
            return nullptr;
        default: unsupported(e.span, "this expression");
        }
    }

    llvm::Value* int_literal(const LiteralExpr& lit, const types::Type* t) {
        if (!t || !t->is_integer()) unsupported(lit.span, "integer literals of this type");
        std::uint64_t words[2] = {static_cast<std::uint64_t>(lit.int_value),
                                  static_cast<std::uint64_t>(lit.int_value >> 64)};
        llvm::APInt   value    = llvm::APInt(128, words).zextOrTrunc(static_cast<unsigned>(t->bits));
        return llvm::ConstantInt::get(ctx_, value);
    }

    // A compile-time float: f128 values keep their exact binary128 bits.
    llvm::Constant* float_constant(const ct::Value& v, llvm::Type* lt) {
        if (lt->isFP128Ty()) {
            u128          q        = ct::quad_of(v);
            std::uint64_t words[2] = {static_cast<std::uint64_t>(q), static_cast<std::uint64_t>(q >> 64)};
            return llvm::ConstantFP::get(ctx_, llvm::APFloat(llvm::APFloat::IEEEquad(), llvm::APInt(128, words)));
        }
        return llvm::ConstantFP::get(lt, v.floating);
    }

    llvm::Value* float_literal(const LiteralExpr& lit, const types::Type* t) {
        if (!t || !t->is_float()) unsupported(lit.span, "float literals of this type");
        std::string text;
        for (char c : lit.text) {
            if (c != '_') text += c;
        }
        if (t->bits == 128) {   // f128: the literal's full precision, not a double's
            std::string digits = text.substr(0, text.find('f'));   // without the suffix
            ct::Value   v;
            v.quad = ct::quad::from_text(digits);
            return float_constant(v, lower_type(t));
        }
        return llvm::ConstantFP::get(lower_type(t), std::strtod(text.c_str(), nullptr));
    }

    llvm::Value* path_value(Expr& e) {
        const Symbol* sym = e.as<PathExpr>().symbol;
        if (!sym) unsupported(e.span, "this name");
        if (sym->kind == SymbolKind::alternative) {
            if (carries(sym)) unsupported(e.span, "enum alternatives with a payload used without a value");
            return alternative_value(sym, nullptr);
        }
        if (sym->kind == SymbolKind::global) return value_of_place(e);
        if (sym->kind != SymbolKind::local || !locals_.count(sym)) {
            unsupported(e.span, "this name", "only local variables, parameters and enum alternatives are supported so far");
        }
        return value_of_place(e);
    }

    llvm::Value* unary_value(UnaryExpr& u, const types::Type* t) {
        if (const types::Type* d = u.distinct_result) {   // `-d` on a distinct type (spec 34)
            u.distinct_result = nullptr;
            const types::Type* under = types::strip_ref(u.operand->type);
            llvm::Value* v = unary_value(u, under);
            u.distinct_result = d;
            return b_.CreateInsertValue(llvm::UndefValue::get(lower_type(d)), v, {0});
        }
        if (u.op_symbol) {   // a user-defined `-` or `~` (spec 16)
            if (has_self(u.op_symbol)) return call_symbol(u.op_symbol, object_address(*u.operand), {}, u.span);
            return call_symbol(u.op_symbol, nullptr, {u.operand.get()}, u.span);
        }
        if (const types::Type* vt = types::strip_ref(u.operand->type); vt && (vt->is(Kind::vector) || vt->is(Kind::mask)) &&
            (u.op == UnaryOp::negate || u.op == UnaryOp::bit_not || u.op == UnaryOp::logical_not)) {
            llvm::Value* v = gen_expr(*u.operand);
            if (u.op != UnaryOp::negate) return b_.CreateNot(v);
            if (vt->inner->is_float()) return b_.CreateFNeg(v);
            llvm::Value* minimum = vec_splat(vt, llvm::ConstantInt::get(ctx_, llvm::APInt::getSignedMinValue(static_cast<unsigned>(vt->inner->bits))));
            check_lanes(b_.CreateICmpEQ(v, minimum), u.span, std::format("integer overflow in '-' ({})", types::to_string(vt)));
            return b_.CreateNeg(v);
        }
        if (u.op == UnaryOp::negate) {
            if (u.operand->kind == ExprKind::int_literal) {
                // `-128i8`: the literal is negated as a constant, so the minimum does not overflow.
                auto* lit = llvm::cast<llvm::ConstantInt>(int_literal(u.operand->as<LiteralExpr>(), t));
                return llvm::ConstantInt::get(ctx_, llvm::APInt(lit->getBitWidth(), 0) - lit->getValue());
            }
            llvm::Value* a = gen_expr(*u.operand);
            if (t && t->is_float()) return b_.CreateFNeg(a);
            if (!t || !t->is_integer()) unsupported(u.span, "'-' on this type");
            llvm::Value* minimum = llvm::ConstantInt::get(ctx_, llvm::APInt::getSignedMinValue(static_cast<unsigned>(t->bits)));
            check(b_.CreateICmpNE(a, minimum), u.span, overflow_message(u.span, "-", t->name), {part(": -("), part(a, true), part(")")});
            return b_.CreateSub(llvm::ConstantInt::get(a->getType(), 0), a);
        }
        if (u.op == UnaryOp::logical_not) return b_.CreateNot(truth(*u.operand));
        if (u.op == UnaryOp::bit_not) return b_.CreateNot(gen_expr(*u.operand));
        if (u.op == UnaryOp::address_of || u.op == UnaryOp::address_of_mut) {   // spec 13
            return is_place(*u.operand) ? place(*u.operand) : object_address(*u.operand);
        }
        if (u.op == UnaryOp::deref) return b_.CreateLoad(lower_type(t), gen_expr(*u.operand));
        if (u.op == UnaryOp::move || u.op == UnaryOp::mut_marker) return gen_expr(*u.operand);
        unsupported(u.span, "this unary operator");
    }

    // A condition: a `bool`, or a raw pointer, which is true when it is not null (spec 02, 13).
    llvm::Value* truth(Expr& e) {
        llvm::Value* v = gen_expr(e);
        if (inspect_ && (v->getType()->isPointerTy() || (v->getType()->isIntegerTy() && !v->getType()->isIntegerTy(1)))) {
            const types::Type* t = types::strip_ref(e.type);
            insight(e.span, "conversions", "condition", "range",
                    std::format("{} → bool ({})", t ? types::to_string(t) : "?", v->getType()->isPointerTy() ? "≠ nullptr" : "≠ 0"),
                    "A condition converts an integer or a `std::raw_ptr` to `bool` (spec 02): true when it is not zero / not `nullptr`.");
        }
        if (v->getType()->isPointerTy()) return b_.CreateICmpNE(v, llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0)));
        if (v->getType()->isIntegerTy() && !v->getType()->isIntegerTy(1)) {   // an integer in a condition: != 0 (spec 02)
            return b_.CreateICmpNE(v, llvm::ConstantInt::get(v->getType(), 0));
        }
        return v;
    }

    llvm::Value* binary_value(BinaryExpr& bx) {
        if (const types::Type* d = bx.distinct_result) {   // distinct-type arithmetic (spec 34): on the underlying values
            const types::Type* saved = bx.type;
            bx.distinct_result = nullptr;
            bx.type            = bx.lhs->type;
            llvm::Value* v     = binary_value(bx);
            bx.type            = saved;
            bx.distinct_result = d;
            return b_.CreateInsertValue(llvm::UndefValue::get(lower_type(d)), v, {0});
        }
        if (bx.op == TokenKind::amp_amp || bx.op == TokenKind::pipe_pipe) return short_circuit(bx);
        if (bx.op_symbol) return user_operator(bx);
        const types::Type* lt = types::strip_ref(bx.lhs->type);
        if (lt && (lt->is(Kind::vector) || lt->is(Kind::mask))) return vector_binary(bx, lt);
        if (lt && lt->is(Kind::tuple)) {   // element by element
            llvm::Value* pa = object_address(*bx.lhs);
            llvm::Value* pb = object_address(*bx.rhs);
            if (bx.op == TokenKind::eq_eq) return values_equal(lt, pa, pb, bx.span);
            if (bx.op == TokenKind::bang_eq) return b_.CreateNot(values_equal(lt, pa, pb, bx.span));
            llvm::Value* order = tuple_order(lt, pa, pb, bx.span);
            llvm::Value* zero  = llvm::ConstantInt::get(order->getType(), 0);
            switch (bx.op) {
            case TokenKind::spaceship: return order;
            case TokenKind::less: return b_.CreateICmpSLT(order, zero);
            case TokenKind::less_eq: return b_.CreateICmpSLE(order, zero);
            case TokenKind::greater: return b_.CreateICmpSGT(order, zero);
            case TokenKind::greater_eq: return b_.CreateICmpSGE(order, zero);
            default: unsupported(bx.span, "this operator on tuples");
            }
        }
        llvm::Value*       a  = gen_expr(*bx.lhs);
        llvm::Value*       b  = gen_expr(*bx.rhs);
        switch (bx.op) {
        case TokenKind::spaceship: {   // std::ordering: -1, 0 or 1
            llvm::Type* i8   = llvm::Type::getInt8Ty(ctx_);
            llvm::Value* lo  = compare_op(TokenKind::less, a, b, lt, bx.span);
            llvm::Value* hi  = compare_op(TokenKind::greater, a, b, lt, bx.span);
            return b_.CreateSelect(lo, llvm::ConstantInt::getSigned(i8, -1),
                                   b_.CreateSelect(hi, llvm::ConstantInt::get(i8, 1), llvm::ConstantInt::get(i8, 0)));
        }
        case TokenKind::eq_eq:
        case TokenKind::bang_eq: {
            bool eq = bx.op == TokenKind::eq_eq;
            if (lt && lt->is_float()) return eq ? b_.CreateFCmpOEQ(a, b) : b_.CreateFCmpUNE(a, b);
            if (is_str(lt)) return str_equal(a, b, eq);
            if (a->getType()->isIntegerTy() || a->getType()->isPointerTy()) return eq ? b_.CreateICmpEQ(a, b) : b_.CreateICmpNE(a, b);
            unsupported(bx.span, "comparing these values");
        }
        case TokenKind::less:
        case TokenKind::less_eq:
        case TokenKind::greater:
        case TokenKind::greater_eq: {
            if (lt && lt->is_float()) {
                switch (bx.op) {
                case TokenKind::less: return b_.CreateFCmpOLT(a, b);
                case TokenKind::less_eq: return b_.CreateFCmpOLE(a, b);
                case TokenKind::greater: return b_.CreateFCmpOGT(a, b);
                default: return b_.CreateFCmpOGE(a, b);
                }
            }
            if (is_str(lt)) return compare_op(bx.op, a, b, lt, bx.span);   // byte-wise, like memcmp (spec 19)
            if (!lt || !(lt->is_integer() || lt->is(Kind::char_))) unsupported(bx.span, "comparing these values");
            bool s = lt->is_integer() ? lt->is_signed : false;
            switch (bx.op) {
            case TokenKind::less: return s ? b_.CreateICmpSLT(a, b) : b_.CreateICmpULT(a, b);
            case TokenKind::less_eq: return s ? b_.CreateICmpSLE(a, b) : b_.CreateICmpULE(a, b);
            case TokenKind::greater: return s ? b_.CreateICmpSGT(a, b) : b_.CreateICmpUGT(a, b);
            default: return s ? b_.CreateICmpSGE(a, b) : b_.CreateICmpUGE(a, b);
            }
        }
        default: return arith(bx.op, lt, a, b, bx.rhs->type, bx.span);
        }
    }

    // A binary operator the program defines (spec 16). `!=` is the negated `==`, and `<`, `<=`,
    // `>`, `>=` compare the `std::ordering` from `<=>` with zero.
    llvm::Value* user_operator(BinaryExpr& bx) {
        const Symbol*       s = bx.op_symbol;
        const FunctionDecl* f = body_decl(s);
        llvm::Value*        r = nullptr;
        if (f && f->is_defaulted) {
            r = values_equal(types::strip_ref(bx.lhs->type), object_address(*bx.lhs), object_address(*bx.rhs), bx.span);
        } else if (has_self(s)) {
            r = call_symbol(s, object_address(*bx.lhs), {bx.rhs.get()}, bx.span);
        } else {
            r = call_symbol(s, nullptr, {bx.lhs.get(), bx.rhs.get()}, bx.span);
        }
        switch (bx.op) {
        case TokenKind::bang_eq: return b_.CreateNot(r);
        case TokenKind::less:
        case TokenKind::less_eq:
        case TokenKind::greater:
        case TokenKind::greater_eq: {
            llvm::Value* zero = llvm::ConstantInt::get(r->getType(), 0);
            switch (bx.op) {
            case TokenKind::less: return b_.CreateICmpSLT(r, zero);
            case TokenKind::less_eq: return b_.CreateICmpSLE(r, zero);
            case TokenKind::greater: return b_.CreateICmpSGT(r, zero);
            default: return b_.CreateICmpSGE(r, zero);
            }
        }
        default: return r;
        }
    }

    // A user-defined `operator==` for `t`, or null.
    static const Symbol* equality_operator(const types::Type* t) {
        if (!t || !t->is(Kind::user)) return nullptr;
        auto it = t->symbol->members.find("==");
        if (it == t->symbol->members.end()) return nullptr;
        for (const Symbol* m : it->second) {
            if (m->function() && m->function()->is_operator) return m;
        }
        return nullptr;
    }

    // Whether the objects at `pa` and `pb` (both of type `t`) are equal: what a defaulted
    // `operator==` does, field by field (spec 16).
    // `<=>` on tuples: the first element that differs decides (a std::ordering: -1, 0 or 1).
    llvm::Value* tuple_order(const types::Type* t, llvm::Value* pa, llvm::Value* pb, Span span) {
        llvm::Type*  i8     = llvm::Type::getInt8Ty(ctx_);
        llvm::Value* result = llvm::ConstantInt::get(i8, 0);
        llvm::Type*  st     = lower_type(t);
        for (std::size_t i = 0; i < t->args.size(); ++i) {
            const types::Type* et = t->args[i];
            llvm::Value*       ea = b_.CreateStructGEP(st, pa, static_cast<unsigned>(i));
            llvm::Value*       eb = b_.CreateStructGEP(st, pb, static_cast<unsigned>(i));
            llvm::Value*       o  = nullptr;
            if (et->is(Kind::tuple)) {
                o = tuple_order(et, ea, eb, span);
            } else {
                llvm::Value* a  = b_.CreateLoad(lower_type(et), ea);
                llvm::Value* b  = b_.CreateLoad(lower_type(et), eb);
                llvm::Value* lo = compare_op(TokenKind::less, a, b, et, span);
                llvm::Value* hi = compare_op(TokenKind::greater, a, b, et, span);
                o = b_.CreateSelect(lo, llvm::ConstantInt::getSigned(i8, -1),
                                    b_.CreateSelect(hi, llvm::ConstantInt::get(i8, 1), llvm::ConstantInt::get(i8, 0)));
            }
            result = b_.CreateSelect(b_.CreateICmpNE(result, llvm::ConstantInt::get(i8, 0)), result, o);
        }
        return result;
    }

    llvm::Value* values_equal(const types::Type* t, llvm::Value* pa, llvm::Value* pb, Span span) {
        if (t && t->is(Kind::tuple)) {
            llvm::Type*  st  = lower_type(t);
            llvm::Value* all = llvm::ConstantInt::getTrue(ctx_);
            for (std::size_t i = 0; i < t->args.size(); ++i) {
                unsigned k = static_cast<unsigned>(i);
                all = b_.CreateAnd(all, values_equal(t->args[i], b_.CreateStructGEP(st, pa, k), b_.CreateStructGEP(st, pb, k), span));
            }
            return all;
        }
        if (t && t->is(Kind::user) && t->symbol->kind != SymbolKind::enum_) {
            if (const Symbol* op = equality_operator(t); op && !body_decl(op)->is_defaulted) {
                return b_.CreateCall(function_of(op), {pa, pb});
            }
            llvm::StructType* st  = llvm::cast<llvm::StructType>(user_type(t->symbol));
            if (types::is_union(t->symbol)) {   // a union: the same bytes (spec 34)
                std::uint64_t size = tc_.layout(t).size;
                auto*         i64  = llvm::Type::getInt64Ty(ctx_);
                llvm::Value*  r    = b_.CreateCall(libc("memcmp", llvm::Type::getInt32Ty(ctx_), {ptr_type(), ptr_type(), i64}),
                                                   {pa, pb, llvm::ConstantInt::get(i64, size)});
                return b_.CreateICmpEQ(r, llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0));
            }
            llvm::Value*      all = llvm::ConstantInt::getTrue(ctx_);
            for (const Symbol* f : fields_of(t->symbol)) {
                unsigned i = field_index(f);
                llvm::Value* e = values_equal(field_type(f), b_.CreateStructGEP(st, pa, i), b_.CreateStructGEP(st, pb, i), span);
                all = b_.CreateAnd(all, e);
            }
            return all;
        }
        if (t && t->is_user(SymbolKind::enum_) && has_payloads(t->symbol)) {
            // Equal when the alternatives are the same and so are their payloads (spec 16).
            const Symbol*     es    = t->symbol;
            llvm::Value*      ta    = load_tag(es, pa);
            llvm::Value*      tb    = load_tag(es, pb);
            llvm::Function*   fn    = b_.GetInsertBlock()->getParent();
            llvm::BasicBlock* done  = llvm::BasicBlock::Create(ctx_, "enum_eq.done", fn);
            llvm::BasicBlock* check = llvm::BasicBlock::Create(ctx_, "enum_eq.payload", fn);
            std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> incoming;
            incoming.emplace_back(llvm::ConstantInt::getFalse(ctx_), b_.GetInsertBlock());
            b_.CreateCondBr(b_.CreateICmpEQ(ta, tb), check, done);
            b_.SetInsertPoint(check);
            for (const Symbol* alt : t->symbol->member_order) {
                if (!alt->alternative || !carries(alt)) continue;
                llvm::BasicBlock* yes  = llvm::BasicBlock::Create(ctx_, "enum_eq.alt", fn);
                llvm::BasicBlock* next = llvm::BasicBlock::Create(ctx_, "enum_eq.next", fn);
                b_.CreateCondBr(b_.CreateICmpEQ(ta, tag_constant(alt)), yes, next);
                b_.SetInsertPoint(yes);
                llvm::Value* same = values_equal(payload_of(alt), payload_address(es, pa), payload_address(es, pb), span);
                incoming.emplace_back(same, b_.GetInsertBlock());
                b_.CreateBr(done);
                b_.SetInsertPoint(next);
            }
            incoming.emplace_back(llvm::ConstantInt::getTrue(ctx_), b_.GetInsertBlock());   // no payload: same tag
            b_.CreateBr(done);
            b_.SetInsertPoint(done);
            llvm::PHINode* phi = b_.CreatePHI(llvm::Type::getInt1Ty(ctx_), static_cast<unsigned>(incoming.size()));
            for (auto& [v, bb] : incoming) phi->addIncoming(v, bb);
            return phi;
        }
        llvm::Type*  lt = lower_type(t);
        llvm::Value* a  = b_.CreateLoad(lt, pa);
        llvm::Value* b  = b_.CreateLoad(lt, pb);
        if (is_str(t)) return str_equal(a, b, true);
        if (lt->isFloatingPointTy()) return b_.CreateFCmpOEQ(a, b);
        if (lt->isIntegerTy()) return b_.CreateICmpEQ(a, b);
        unsupported(span, std::format("comparing values of type '{}'", types::to_string(t)));
    }

    // `&&` and `||`: the right operand is evaluated only when needed.
    llvm::Value* short_circuit(BinaryExpr& bx) {
        bool and_op = bx.op == TokenKind::amp_amp;
        llvm::Value* a = truth(*bx.lhs);
        llvm::BasicBlock* from   = b_.GetInsertBlock();
        llvm::BasicBlock* rhs_bb = llvm::BasicBlock::Create(ctx_, "sc.rhs", cur_fn_);
        llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(ctx_, "sc.end", cur_fn_);
        if (and_op) b_.CreateCondBr(a, rhs_bb, end_bb);
        else b_.CreateCondBr(a, end_bb, rhs_bb);
        b_.SetInsertPoint(rhs_bb);
        ++cond_depth_;
        llvm::Value*      r        = truth(*bx.rhs);
        --cond_depth_;
        llvm::BasicBlock* rhs_done = b_.GetInsertBlock();
        b_.CreateBr(end_bb);
        b_.SetInsertPoint(end_bb);
        llvm::PHINode* phi = b_.CreatePHI(llvm::Type::getInt1Ty(ctx_), 2);
        phi->addIncoming(llvm::ConstantInt::getBool(ctx_, !and_op), from);
        phi->addIncoming(r, rhs_done);
        return phi;
    }

    // Arithmetic and bit operations on two values of type `t` (the left operand's type). `rt` is
    // the right operand's type, which matters for shifts. Checked as spec 16 requires.
    llvm::Value* arith(TokenKind op, const types::Type* t, llvm::Value* a, llvm::Value* b, const types::Type* rt,
                       Span span) {
        if (!t) unsupported(span, "this operation");
        if (t->is_float()) {
            switch (op) {
            case TokenKind::plus: return b_.CreateFAdd(a, b);
            case TokenKind::minus: return b_.CreateFSub(a, b);
            case TokenKind::star: return b_.CreateFMul(a, b);
            case TokenKind::slash: return b_.CreateFDiv(a, b);
            case TokenKind::percent:
                if (a->getType()->isFP128Ty()) return b_.CreateCall(libc("fmodf128", a->getType(), {a->getType(), a->getType()}), {a, b});
                return b_.CreateFRem(a, b);
            default: unsupported(span, std::format("this operator on {}", types::to_string(t)));
            }
        }
        if (!t->is_integer()) {
            if (t->is(Kind::bool_) && (op == TokenKind::amp || op == TokenKind::pipe || op == TokenKind::caret)) {
                if (op == TokenKind::amp) return b_.CreateAnd(a, b);
                if (op == TokenKind::pipe) return b_.CreateOr(a, b);
                return b_.CreateXor(a, b);
            }
            unsupported(span, std::format("this operator on {}", types::to_string(t)));
        }
        bool        s    = t->is_signed;
        std::string name = t->name;
        switch (op) {
        case TokenKind::plus_percent: return b_.CreateAdd(a, b);     // wrapping operators (spec 16)
        case TokenKind::minus_percent: return b_.CreateSub(a, b);
        case TokenKind::star_percent: return b_.CreateMul(a, b);
        default: break;
        }
        if (mode_setting("overflow") == "wrap") {   // a mode that wraps (spec 24)
            if (op == TokenKind::plus) return b_.CreateAdd(a, b);
            if (op == TokenKind::minus) return b_.CreateSub(a, b);
            if (op == TokenKind::star) return b_.CreateMul(a, b);
        }
        switch (op) {
        case TokenKind::plus: return checked(llvm::Intrinsic::sadd_with_overflow, llvm::Intrinsic::uadd_with_overflow, s, a, b, span,
                                             overflow_message(span, "+", name), "+");
        case TokenKind::minus: return checked(llvm::Intrinsic::ssub_with_overflow, llvm::Intrinsic::usub_with_overflow, s, a, b,
                                              span, overflow_message(span, "-", name), "-");
        case TokenKind::star: return checked(llvm::Intrinsic::smul_with_overflow, llvm::Intrinsic::umul_with_overflow, s, a, b,
                                             span, overflow_message(span, "*", name), "*");
        case TokenKind::slash: {
            check(b_.CreateICmpNE(b, llvm::ConstantInt::get(b->getType(), 0)), span, division_by_zero_message(span, "/"));
            if (!s) return b_.CreateUDiv(a, b);
            llvm::Value* minimum = llvm::ConstantInt::get(ctx_, llvm::APInt::getSignedMinValue(static_cast<unsigned>(t->bits)));
            llvm::Value* minus_one = llvm::ConstantInt::getAllOnesValue(b->getType());
            check(b_.CreateNot(b_.CreateAnd(b_.CreateICmpEQ(a, minimum), b_.CreateICmpEQ(b, minus_one))), span,
                  overflow_message(span, "/", name), {part(": "), part(a, true), part(" / "), part(b, true)});
            return b_.CreateSDiv(a, b);
        }
        case TokenKind::percent: {
            check(b_.CreateICmpNE(b, llvm::ConstantInt::get(b->getType(), 0)), span, division_by_zero_message(span, "%"));
            if (!s) return b_.CreateURem(a, b);
            // MIN % -1 is 0; LLVM's srem would be undefined there, so the divisor is replaced by 1.
            llvm::Value* minus_one = llvm::ConstantInt::getAllOnesValue(b->getType());
            llvm::Value* is_m1     = b_.CreateICmpEQ(b, minus_one);
            llvm::Value* safe      = b_.CreateSelect(is_m1, llvm::ConstantInt::get(b->getType(), 1), b);
            return b_.CreateSelect(is_m1, llvm::ConstantInt::get(b->getType(), 0), b_.CreateSRem(a, safe));
        }
        case TokenKind::plus_percent: return b_.CreateAdd(a, b);
        case TokenKind::minus_percent: return b_.CreateSub(a, b);
        case TokenKind::star_percent: return b_.CreateMul(a, b);
        case TokenKind::amp: return b_.CreateAnd(a, b);
        case TokenKind::pipe: return b_.CreateOr(a, b);
        case TokenKind::caret: return b_.CreateXor(a, b);
        case TokenKind::shl:
        case TokenKind::shr: {
            // The count is compared as a 128-bit value, so a negative count is out of range too.
            if (mode_setting("shifts") == "mask") {   // the count & (width - 1) (spec 24)
                llvm::Value* amount = b_.CreateAnd(b_.CreateZExtOrTrunc(b, a->getType()),
                                                   llvm::ConstantInt::get(a->getType(), static_cast<std::uint64_t>(t->bits - 1)));
                if (op == TokenKind::shl) return b_.CreateShl(a, amount);
                return s ? b_.CreateAShr(a, amount) : b_.CreateLShr(a, amount);
            }
            llvm::Value* count = rt && rt->is_signed ? b_.CreateSExtOrTrunc(b, llvm::Type::getInt128Ty(ctx_))
                                                     : b_.CreateZExtOrTrunc(b, llvm::Type::getInt128Ty(ctx_));
            std::string text = snippet(span);
            check(b_.CreateICmpULT(count, llvm::ConstantInt::get(count->getType(), static_cast<std::uint64_t>(t->bits))),
                  span, text.empty() ? std::format("shift count out of range for {}", name)
                                     : std::format("shift count out of range for {} in '{}'", name, text),
                  {part(": "), part(count, rt && rt->is_signed), part(std::format(" (must be below {})", t->bits))});
            llvm::Value* amount = b_.CreateZExtOrTrunc(count, a->getType());
            if (op == TokenKind::shl) return b_.CreateShl(a, amount);
            return s ? b_.CreateAShr(a, amount) : b_.CreateLShr(a, amount);
        }
        default: break;
        }
        unsupported(span, std::format("'{}' on {}", token_kind_spelling(op), types::to_string(t)));
    }

    // `a op b` with an overflow intrinsic: panics with `message` and the operands on overflow.
    llvm::Value* checked(llvm::Intrinsic::ID signed_id, llvm::Intrinsic::ID unsigned_id, bool s, llvm::Value* a,
                         llvm::Value* b, Span span, const std::string& message, const char* symbol) {
        llvm::Function* f = intrinsic_declaration(mod_.get(), s ? signed_id : unsigned_id, {a->getType()});
        llvm::Value*    r = b_.CreateCall(f, {a, b});
        check(b_.CreateNot(b_.CreateExtractValue(r, 1)), span, message,
              {part(": "), part(a, s), part(std::format(" {} ", symbol)), part(b, s)});
        return b_.CreateExtractValue(r, 0);
    }

    // "integer overflow in 'high - low' (i16)": the expression as written, or the operator.
    std::string overflow_message(Span span, const char* symbol, const std::string& type) const {
        std::string text = snippet(span);
        return std::format("integer overflow in '{}' ({})", text.empty() ? std::string(symbol) : text, type);
    }

    std::string division_by_zero_message(Span span, const char* symbol) const {
        std::string text = snippet(span);
        return std::format("division by zero in '{}'", text.empty() ? std::string(symbol) : text);
    }

    // `as_address`: every branch is a place (places_only), and the result is its address.
    llvm::Value* if_value(IfExpr& i, const types::Type* t, bool as_address = false) {
        if (i.is_compile_time) {   // only the branch the checker chose exists (spec 15)
            if (i.taken == 0) return gen_block(*i.then_block);
            if (i.taken == 1) return gen_expr(*i.else_branch);
            if (i.taken == 2) return nullptr;
            unsupported(i.span, "'if compile_time' with a condition that is not known");
        }
        llvm::Value*      cond     = truth(*i.condition);
        bool              has_value = !is_void(t);
        llvm::AllocaInst* slot      = has_value ? entry_alloca(as_address ? ptr_type() : lower_type(types::strip_ref(t)), "if.value") : nullptr;
        llvm::BasicBlock* then_bb   = llvm::BasicBlock::Create(ctx_, "then", cur_fn_);
        llvm::BasicBlock* else_bb   = i.else_branch ? llvm::BasicBlock::Create(ctx_, "else", cur_fn_) : nullptr;
        llvm::BasicBlock* end_bb    = llvm::BasicBlock::Create(ctx_, "endif", cur_fn_);
        b_.CreateCondBr(cond, then_bb, else_bb ? else_bb : end_bb);
        b_.SetInsertPoint(then_bb);
        llvm::Value* tv = gen_block(*i.then_block, as_address);
        if (!terminated()) {
            if (slot && tv) b_.CreateStore(tv, slot);
            b_.CreateBr(end_bb);
        }
        if (else_bb) {
            b_.SetInsertPoint(else_bb);
            llvm::Value* ev = as_address ? object_address(*i.else_branch) : gen_expr(*i.else_branch);
            if (!terminated()) {
                if (slot && ev) b_.CreateStore(ev, slot);
                b_.CreateBr(end_bb);
            }
        }
        b_.SetInsertPoint(end_bb);
        return slot ? b_.CreateLoad(as_address ? ptr_type() : lower_type(types::strip_ref(t)), slot) : nullptr;
    }

    llvm::Value* while_loop(WhileExpr& w) {
        llvm::BasicBlock* cond_bb = llvm::BasicBlock::Create(ctx_, "while.cond", cur_fn_);
        llvm::BasicBlock* body_bb = llvm::BasicBlock::Create(ctx_, "while.body", cur_fn_);
        llvm::BasicBlock* exit_bb = llvm::BasicBlock::Create(ctx_, "while.end", cur_fn_);
        b_.CreateBr(cond_bb);
        b_.SetInsertPoint(cond_bb);
        b_.CreateCondBr(truth(*w.condition), body_bb, exit_bb);
        b_.SetInsertPoint(body_bb);
        loops_.push_back(Loop{exit_bb, cond_bb});
        open_scope(true);
        gen_block(*w.body);
        close_scope();
        loops_.pop_back();
        if (!terminated()) b_.CreateBr(cond_bb);
        b_.SetInsertPoint(exit_bb);
        return nullptr;
    }

    llvm::Value* return_value(ReturnExpr& r) {
        llvm::Type* rt = cur_fn_->getReturnType();
        llvm::Value* v = nullptr;
        if (r.value) {
            v = result_ && result_->is(Kind::reference) && is_interface(result_->inner) ? interface_reference(*r.value, result_->inner->symbol)
              : result_ && result_->is(Kind::reference)                              ? reference_to(*r.value)
                                                                                       : gen_expr(*r.value);
            moved_out(*r.value);
            if (inspect_ && r.value->kind == ExprKind::path && owned_.contains(r.value->as<PathExpr>().symbol)) {
                const std::string& n = r.value->as<PathExpr>().symbol->name;
                insight(r.value->span, "moves", "implicit-move", "range", "implicit move of " + n,
                        std::format("A move is a bitwise copy of {}. The destination owns the `{}` now; `{}` can't be used afterwards "
                                    "and is not dropped at the end of its scope (spec 12). Returning a local moves it without `move`.",
                                    bytes_text(type_bytes(types::strip_ref(r.value->type))), types::to_string(types::strip_ref(r.value->type)), n));
            }
        }
        drop_site_ = r.span;
        drop_kind_ = "exit-drop";
        (void)rt;
        leave_function(v);
        open_dead_block();
        return nullptr;
    }

    // A method or static function of a std type, by name (`string::as_str`, `string::from`).
    const Symbol* std_member(const types::Type* t, const std::string& name) {
        if (!t || !t->is(Kind::user)) return nullptr;
        auto it = t->symbol->members.find(name);
        return it == t->symbol->members.end() ? nullptr : it->second.front();
    }

    // The implicit conversions of spec 19. A temporary `std::string` that is read as a
    // `std::str` lives until the end of the enclosing block.
    llvm::Value* implicit_conversion(Expr& e) {
        cur_ = e.span;
        record_conversion(e);
        if (e.conversion == Expr::Conversion::string_to_str) {
            const types::Type* st   = types::strip_ref(e.type);
            const Symbol*      view = std_member(st, "as_str");
            if (!view) unsupported(e.span, "this std::string");
            llvm::Value* addr = object_address(e);   // a temporary lives until the end of the block
            return b_.CreateCall(function_of(view), {addr});
        }
        if (e.conversion == Expr::Conversion::to_callable) return callable_from_lambda(e, e.converted_to);
        if (e.conversion == Expr::Conversion::pointer_upcast) {   // the same pointer, seen as its base (spec 07)
            return upcast_value(gen_expr(e), types::strip_ref(e.type), e.converted_to, e.span);
        }
        if (e.conversion == Expr::Conversion::distinct_value) {   // the value inside a distinct type (spec 34)
            llvm::Value* v = gen_expr(e);
            if (e.type->is(types::Kind::reference)) v = b_.CreateLoad(lower_type(types::strip_ref(e.type)), v);
            return b_.CreateExtractValue(v, {0});
        }
        // literal_to_string: `std::string::from(literal)`
        const types::Type* target = nullptr;
        if (const Symbol* sym = program_.std_module ? program_.std_module->item("std::string") : nullptr) {
            target = tc_.user(sym);
        }
        const Symbol* from = std_member(target, "from");
        if (!from) unsupported(e.span, "this std::string");
        return b_.CreateCall(function_of(from), {gen_expr(e)});
    }

    // A pointer (or a smart pointer holding one) seen as a pointer to a base class or an
    // interface: the same address, plus the interface's table when the target is an interface.
    llvm::Value* upcast_value(llvm::Value* v, const types::Type* from, const types::Type* to, Span span) {
        if (from == to) return v;
        if (from->is(Kind::pointer) && to->is(Kind::pointer)) {
            if (!is_interface(to->inner)) return v;
            if (is_interface(from->inner)) return convert_interface(v, from->inner->symbol, to->inner->symbol, span);
            auto*        fat = llvm::StructType::get(ctx_, {ptr_type(), ptr_type()});
            llvm::Value* out = b_.CreateInsertValue(llvm::UndefValue::get(fat), v, 0);
            return b_.CreateInsertValue(out, itable_of(from->inner->symbol, to->inner->symbol), 1);
        }
        if (from->is(Kind::user) && to->is(Kind::user)) {   // std::unique_ptr<circle> → std::unique_ptr<shape>: field by field
            std::vector<const Symbol*> ff = all_fields(from->symbol);
            std::vector<const Symbol*> tf = all_fields(to->symbol);
            if (ff.size() != tf.size()) unsupported(span, "this conversion");
            llvm::Value* out = llvm::UndefValue::get(lower_type(to));
            for (std::size_t i = 0; i < ff.size(); ++i) {
                llvm::Value* field = b_.CreateExtractValue(v, {field_index(ff[i])});
                out = b_.CreateInsertValue(out, upcast_value(field, field_type(ff[i]), field_type(tf[i]), span), {field_index(tf[i])});
            }
            return out;
        }
        return v;
    }

    // A C library function the `std::raw` intrinsics call.
    llvm::FunctionCallee libc(const char* name, llvm::Type* result, std::vector<llvm::Type*> params) {
        return mod_->getOrInsertFunction(name, llvm::FunctionType::get(result, params, false));
    }

    // The bytes `n` elements of `t` take; panics on overflow.
    llvm::Value* byte_count(const types::Type* t, llvm::Value* n, Span span) {
        auto*        i64  = llvm::Type::getInt64Ty(ctx_);
        std::uint64_t size = mod_->getDataLayout().getTypeAllocSize(lower_type(t));
        llvm::Value* r    = b_.CreateBinaryIntrinsic(llvm::Intrinsic::umul_with_overflow, b_.CreateZExtOrTrunc(n, i64),
                                                     llvm::ConstantInt::get(i64, size));
        check(b_.CreateNot(b_.CreateExtractValue(r, 1)), span, "allocation size overflows");
        return b_.CreateExtractValue(r, 0);
    }

    // The `std::raw` memory intrinsics (spec 13).
    // A `std::memory_order` argument as an LLVM ordering. An order that isn't a constant (the
    // library's checks make it one) becomes sequentially consistent, which is always correct.
    llvm::AtomicOrdering ordering(Expr& e, char use) {
        llvm::Value* v = gen_expr(e);
        auto*        c = llvm::dyn_cast<llvm::ConstantInt>(v);
        if (!c) return llvm::AtomicOrdering::SequentiallyConsistent;
        switch (c->getZExtValue()) {
        case 0: return llvm::AtomicOrdering::Monotonic;
        case 1: return use == 's' ? llvm::AtomicOrdering::Monotonic : llvm::AtomicOrdering::Acquire;
        case 2: return use == 'l' || use == 'f' ? llvm::AtomicOrdering::Monotonic : llvm::AtomicOrdering::Release;
        case 3: return use == 'l' || use == 'f' ? llvm::AtomicOrdering::Acquire
                     : use == 's'               ? llvm::AtomicOrdering::Release
                                                : llvm::AtomicOrdering::AcquireRelease;
        default: return llvm::AtomicOrdering::SequentiallyConsistent;
        }
    }

    // std::raw's atomics, futex and spin hint (spec 27). `bool` is a byte in memory.
    llvm::Value* thread_raw(CallExpr& c, const std::string& name) {
        auto* i32 = llvm::Type::getInt32Ty(ctx_);
        auto* vd  = llvm::Type::getVoidTy(ctx_);
        if (name == "spin_hint") {
            llvm::Triple triple(target_triple());
            if (triple.isX86()) b_.CreateCall(intrinsic_declaration(mod_.get(), llvm::Intrinsic::x86_sse2_pause));
            else if (triple.isAArch64()) b_.CreateCall(intrinsic_declaration(mod_.get(), llvm::Intrinsic::aarch64_hint), {llvm::ConstantInt::get(i32, 1)});
            return nullptr;
        }
        if (name == "atomic_fence") {
            b_.CreateFence(ordering(*c.args[0], 'r'));
            return nullptr;
        }
        if (name == "futex_wait_for") {
            llvm::Value* p  = gen_expr(*c.args[0]);
            llvm::Value* n  = gen_expr(*c.args[1]);
            llvm::Value* ns = gen_expr(*c.args[2]);
            b_.CreateCall(runtime("edax_futex_wait_for", vd, {ptr_type(), i32, llvm::Type::getInt64Ty(ctx_)}), {p, n, ns});
            return nullptr;
        }
        if (name == "futex_wait" || name == "futex_wake") {
            llvm::Value* p = gen_expr(*c.args[0]);
            llvm::Value* n = gen_expr(*c.args[1]);
            b_.CreateCall(runtime(name == "futex_wait" ? "edax_futex_wait" : "edax_futex_wake", vd, {ptr_type(), i32}), {p, n});
            return nullptr;
        }
        const types::Type* vt     = types::strip_ref(c.args[0]->type)->inner;
        bool               is_bool = vt->is(Kind::bool_);
        llvm::Type*        mem    = is_bool ? llvm::Type::getInt8Ty(ctx_) : lower_type(vt);
        llvm::Align        align(mod_->getDataLayout().getTypeStoreSize(mem).getFixedValue());
        llvm::Value*       p      = gen_expr(*c.args[0]);
        auto to_mem   = [&](llvm::Value* v) { return is_bool ? b_.CreateZExt(v, mem) : v; };
        auto from_mem = [&](llvm::Value* v) { return is_bool ? b_.CreateICmpNE(v, llvm::ConstantInt::get(mem, 0)) : v; };
        if (name == "atomic_load") {
            llvm::LoadInst* l = b_.CreateAlignedLoad(mem, p, align);
            l->setAtomic(ordering(*c.args[1], 'l'));
            return from_mem(l);
        }
        if (name == "atomic_store") {
            llvm::Value*     v  = to_mem(gen_expr(*c.args[1]));
            llvm::StoreInst* st = b_.CreateAlignedStore(v, p, align);
            st->setAtomic(ordering(*c.args[2], 's'));
            return nullptr;
        }
        if (name == "atomic_compare_exchange") {
            llvm::Value* expected_at = gen_expr(*c.args[1]);
            llvm::Value* desired     = to_mem(gen_expr(*c.args[2]));
            llvm::AtomicOrdering ok  = ordering(*c.args[3], 'r');
            llvm::AtomicOrdering bad = ordering(*c.args[4], 'f');
            llvm::Value* weak        = gen_expr(*c.args[5]);
            llvm::Value* expected    = b_.CreateLoad(mem, expected_at);
            auto*        x           = b_.CreateAtomicCmpXchg(p, expected, desired, align, ok, bad);
            if (auto* w = llvm::dyn_cast<llvm::ConstantInt>(weak); w && w->isOne()) x->setWeak(true);
            b_.CreateStore(b_.CreateExtractValue(x, 0), expected_at);   // the value that was there
            return b_.CreateExtractValue(x, 1);
        }
        llvm::AtomicRMWInst::BinOp op = name == "atomic_exchange"  ? llvm::AtomicRMWInst::Xchg
                                      : name == "atomic_fetch_add" ? llvm::AtomicRMWInst::Add
                                      : name == "atomic_fetch_sub" ? llvm::AtomicRMWInst::Sub
                                      : name == "atomic_fetch_and" ? llvm::AtomicRMWInst::And
                                      : name == "atomic_fetch_or"  ? llvm::AtomicRMWInst::Or
                                                                   : llvm::AtomicRMWInst::Xor;
        llvm::Value* v = to_mem(gen_expr(*c.args[1]));
        return from_mem(b_.CreateAtomicRMW(op, p, v, align, ordering(*c.args[2], 'r')));
    }

    // A raw SIMD intrinsic (SIMD, "Raw intrinsics"): the LLVM intrinsic, with the arguments and
    // the result reinterpreted between Edax's vector types and LLVM's.
    llvm::Value* simd_intrinsic(CallExpr& c, const simd::Intrinsic& in) {
        auto llvm_of = [&](const simd::Intrinsic::LlvmType& t) -> llvm::Type* {
            llvm::Type* lane = t.is_float ? (t.lane_bits == 64 ? llvm::Type::getDoubleTy(ctx_) : llvm::Type::getFloatTy(ctx_))
                                          : static_cast<llvm::Type*>(llvm::IntegerType::get(ctx_, static_cast<unsigned>(t.lane_bits)));
            return t.lanes ? static_cast<llvm::Type*>(llvm::FixedVectorType::get(lane, static_cast<unsigned>(t.lanes))) : lane;
        };
        std::vector<llvm::Type*>  params;
        std::vector<llvm::Value*> args;
        for (std::size_t i = 0; i < in.llvm_params.size(); ++i) {
            llvm::Type*  want = llvm_of(in.llvm_params[i]);
            llvm::Value* v    = gen_expr(*c.args[i]);
            if (want->isVectorTy()) v = b_.CreateBitCast(v, want);
            else if (want->isIntegerTy()) v = b_.CreateZExtOrTrunc(v, want);
            params.push_back(want);
            args.push_back(v);
        }
        llvm::Type*         result = llvm_of(in.llvm_result);
        llvm::FunctionCallee f     = mod_->getOrInsertFunction(std::string(in.llvm_name), llvm::FunctionType::get(result, params, false));
        llvm::Value*        r      = b_.CreateCall(f, args);
        llvm::Type*         edax   = lower_type(c.type);
        if (edax->isVectorTy()) return b_.CreateBitCast(r, edax);
        if (edax->isIntegerTy() && r->getType()->isIntegerTy()) return b_.CreateZExtOrTrunc(r, edax);
        return r;
    }

    // The std function std::raw's `name` goes through when build.ed chose a global allocator
    // (`raw_heap_allocate`, ...), or null for the C library's heap.
    const Symbol* chosen_heap(const std::string& name) {
        if (!program_.std_module || !program_.build_settings.contains("std::global_allocator")) return nullptr;
        return program_.std_module->item("std::raw_heap_" + name);
    }

    llvm::Value* raw_call(CallExpr& c, const std::string& name) {
        if (name.starts_with("atomic_") || name.starts_with("futex_") || name == "spin_hint") return thread_raw(c, name);
        if (name == "assume") {   // spec 34: the optimizer may rely on the condition
            b_.CreateAssumption(gen_expr(*c.args[0]));
            return nullptr;
        }
        if (name == "prefetch_read" || name == "prefetch_write") {   // spec 34: llvm.prefetch(address, rw, locality 3, data)
            auto* i32 = llvm::Type::getInt32Ty(ctx_);
            b_.CreateCall(intrinsic_declaration(mod_.get(), llvm::Intrinsic::prefetch, {ptr_type()}),
                          {gen_expr(*c.args[0]), llvm::ConstantInt::get(i32, name == "prefetch_write" ? 1 : 0), llvm::ConstantInt::get(i32, 3),
                           llvm::ConstantInt::get(i32, 1)});
            return nullptr;
        }
        if (name == "unreachable") {
            b_.CreateUnreachable();
            open_dead_block();
            return nullptr;
        }
        if (name == "bit_cast") {   // the same bits as another plain type (spec 34)
            llvm::Value* v  = gen_expr(*c.args[0]);
            llvm::Type*  to = lower_type(types::strip_ref(c.type));
            if (v->getType()->isPointerTy() && !to->isPointerTy() && c.args[0]->type && c.args[0]->type->is(Kind::reference)) {
                return b_.CreateLoad(to, v);
            }
            if (v->getType()->isIntOrIntVectorTy() || v->getType()->isFPOrFPVectorTy()) {
                if (to->isIntOrIntVectorTy() || to->isFPOrFPVectorTy()) return b_.CreateBitCast(v, to);
            }
            llvm::AllocaInst* slot = entry_alloca(v->getType(), "bits");
            b_.CreateStore(v, slot);
            return b_.CreateLoad(to, slot);
        }
        auto* ptr = ptr_type();
        auto* i64 = llvm::Type::getInt64Ty(ctx_);
        const types::Type* pt = types::strip_ref(c.type);
        const Symbol* heap = (name == "allocate" || name == "reallocate" || name == "free") ? chosen_heap(name) : nullptr;
        if (heap) {
            // build.ed chose a global allocator (allocators): std::raw goes through it.
            llvm::Function* f = function_of(heap);
            if (name == "free") {
                llvm::Value* p = gen_expr(*c.args[0]);
                if (p->getType()->isStructTy()) p = b_.CreateExtractValue(p, 0);
                b_.CreateCall(f, {p});
                return nullptr;
            }
            llvm::Value* old   = name == "reallocate" ? gen_expr(*c.args[0]) : nullptr;
            llvm::Value* bytes = byte_count(pt->inner, gen_expr(*c.args.back()), c.span);
            return old ? b_.CreateCall(f, {old, bytes}) : b_.CreateCall(f, {bytes});
        }
        if (name == "allocate" || name == "reallocate") {
            llvm::Value* old   = name == "reallocate" ? gen_expr(*c.args[0]) : nullptr;
            llvm::Value* bytes = byte_count(pt->inner, gen_expr(*c.args.back()), c.span);
            bytes = b_.CreateSelect(b_.CreateICmpEQ(bytes, llvm::ConstantInt::get(i64, 0)), llvm::ConstantInt::get(i64, 1), bytes);
            llvm::Value* p = old ? b_.CreateCall(libc("realloc", ptr, {ptr, i64}), {old, bytes})
                                 : b_.CreateCall(libc("malloc", ptr, {i64}), {bytes});
            check(b_.CreateICmpNE(p, llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0))), c.span, "out of memory");
            return p;
        }
        if (name == "free") {
            llvm::Value* p = gen_expr(*c.args[0]);
            if (p->getType()->isStructTy()) p = b_.CreateExtractValue(p, 0);   // a pointer to an interface
            b_.CreateCall(libc("free", llvm::Type::getVoidTy(ctx_), {ptr}), {p});
            return nullptr;
        }
        if (name == "str_from" || name == "static_str_from") {
            llvm::Value* p = gen_expr(*c.args[0]);
            llvm::Value* n = b_.CreateZExtOrTrunc(gen_expr(*c.args[1]), i64);
            llvm::Value* v = llvm::UndefValue::get(str_type());
            return b_.CreateInsertValue(b_.CreateInsertValue(v, p, 0), n, 1);
        }
        const types::Type* target = types::strip_ref(c.args[0]->type);
        if (name == "read") return b_.CreateLoad(lower_type(target->inner), gen_expr(*c.args[0]));
        if (name == "write") {
            llvm::Value* p = gen_expr(*c.args[0]);
            llvm::Value* v = gen_expr(*c.args[1]);
            moved_out(*c.args[1]);
            b_.CreateStore(v, p);
            return nullptr;
        }
        if (name == "drop") {
            llvm::Value* p = gen_expr(*c.args[0]);
            const types::Type* t = target->inner;
            if (is_interface(t)) {   // the table's first entry destroys the object (null: nothing to do)
                llvm::Value* object = b_.CreateExtractValue(p, 0);
                llvm::Value* drop   = b_.CreateLoad(ptr_type(), b_.CreateExtractValue(p, 1));
                auto* yes  = llvm::BasicBlock::Create(ctx_, "idrop", cur_fn_);
                auto* done = llvm::BasicBlock::Create(ctx_, "idrop.done", cur_fn_);
                b_.CreateCondBr(b_.CreateIsNotNull(drop), yes, done);
                b_.SetInsertPoint(yes);
                b_.CreateCall(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {ptr_type()}, false), drop, {object});
                b_.CreateBr(done);
                b_.SetInsertPoint(done);
            } else if (t->is(Kind::user) && has_vtable(t->symbol)) {   // the object's own class decides (a virtual destructor)
                llvm::Value* vtable = b_.CreateLoad(ptr_type(), p);
                llvm::Value* drop   = b_.CreateLoad(ptr_type(), b_.CreateConstGEP1_32(ptr_type(), vtable, 1));
                b_.CreateCall(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {ptr_type()}, false), drop, {p});
            } else if (needs_drop(t)) {
                b_.CreateCall(drop_function(t), {p});
            }
            return nullptr;
        }
        // copy(to, from, n): memmove, so the ranges may overlap
        llvm::Value* to    = gen_expr(*c.args[0]);
        llvm::Value* from  = gen_expr(*c.args[1]);
        llvm::Value* bytes = byte_count(target->inner, gen_expr(*c.args[2]), c.span);
        b_.CreateMemMove(to, llvm::MaybeAlign(), from, llvm::MaybeAlign(), bytes);
        return nullptr;
    }

    // Where `return` goes inside a wrapped function body (annotations, section 8): the body's
    // value slot and the code after `std::meta::wrapped_body()`, not the function's exit.
    struct Redirect {
        llvm::Function*   fn    = nullptr;
        llvm::AllocaInst* slot  = nullptr;
        llvm::BasicBlock* cont  = nullptr;
        std::size_t       depth = 0;   // the scopes open when the body started
    };
    std::vector<Redirect> redirects_;

    const Redirect* active_redirect() const {
        return !redirects_.empty() && redirects_.back().fn == cur_fn_ ? &redirects_.back() : nullptr;
    }

    // `std::meta::wrapped_body()`: the function's own body, generated in place.
    llvm::Value* wrapped_body(CallExpr& c) {
        const types::Type* t    = c.type;
        llvm::AllocaInst*  slot = is_void(t) ? nullptr : entry_alloca(lower_type(t), "wrapped.value");
        llvm::BasicBlock*  cont = llvm::BasicBlock::Create(ctx_, "wrapped.end", cur_fn_);
        redirects_.push_back(Redirect{cur_fn_, slot, cont, scopes_.size()});
        llvm::Value* v = gen_block(*c.wrapped);
        if (!terminated()) {
            if (slot && v) b_.CreateStore(v, slot);
            b_.CreateBr(cont);
        }
        redirects_.pop_back();
        b_.SetInsertPoint(cont);
        return slot ? b_.CreateLoad(lower_type(t), slot) : nullptr;
    }

    // Leaves the function with `v` (null for none) after destroying every open scope, or, inside a
    // wrapped body, leaves the body.
    void leave_function(llvm::Value* v) {
        if (const Redirect* r = active_redirect()) {
            record_drops(r->depth);
            drop_kind_ = "drop";
            destroy_from(r->depth);
            if (r->slot && v) b_.CreateStore(v, r->slot);
            b_.CreateBr(r->cont);
            return;
        }
        record_drops(0);
        drop_kind_ = "drop";
        destroy_from(0);
        if (v && !cur_fn_->getReturnType()->isVoidTy()) b_.CreateRet(v);
        else b_.CreateRetVoid();
    }

    // --- SIMD ------------------------------------------------------------------------------------

    llvm::Value* vec_splat(const types::Type* vt, llvm::Value* x) {
        return b_.CreateVectorSplat(static_cast<unsigned>(vt->bits), x);
    }

    // Panics with `message` when any lane of the i1 vector `bad` is set.
    void check_lanes(llvm::Value* bad, Span span, const std::string& message) {
        check(b_.CreateNot(b_.CreateOrReduce(bad)), span, message);
    }

    llvm::Value* vector_binary(BinaryExpr& bx, const types::Type* t) {
        llvm::Value* a = gen_expr(*bx.lhs);
        llvm::Value* b = gen_expr(*bx.rhs);
        TokenKind    op = bx.op;
        if (t->is(Kind::mask)) {
            if (op == TokenKind::amp) return b_.CreateAnd(a, b);
            if (op == TokenKind::pipe) return b_.CreateOr(a, b);
            return b_.CreateXor(a, b);
        }
        const types::Type* lane = t->inner;
        bool               flt  = lane->is_float();
        bool               sgn  = lane->is_integer() && lane->is_signed;
        switch (op) {
        case TokenKind::eq_eq: return flt ? b_.CreateFCmpOEQ(a, b) : b_.CreateICmpEQ(a, b);
        case TokenKind::bang_eq: return flt ? b_.CreateFCmpUNE(a, b) : b_.CreateICmpNE(a, b);
        case TokenKind::less: return flt ? b_.CreateFCmpOLT(a, b) : sgn ? b_.CreateICmpSLT(a, b) : b_.CreateICmpULT(a, b);
        case TokenKind::less_eq: return flt ? b_.CreateFCmpOLE(a, b) : sgn ? b_.CreateICmpSLE(a, b) : b_.CreateICmpULE(a, b);
        case TokenKind::greater: return flt ? b_.CreateFCmpOGT(a, b) : sgn ? b_.CreateICmpSGT(a, b) : b_.CreateICmpUGT(a, b);
        case TokenKind::greater_eq: return flt ? b_.CreateFCmpOGE(a, b) : sgn ? b_.CreateICmpSGE(a, b) : b_.CreateICmpUGE(a, b);
        default: break;
        }
        if (flt) {
            switch (op) {
            case TokenKind::plus: return b_.CreateFAdd(a, b);
            case TokenKind::minus: return b_.CreateFSub(a, b);
            case TokenKind::star: return b_.CreateFMul(a, b);
            case TokenKind::slash: return b_.CreateFDiv(a, b);
            default: unsupported(bx.span, "this operator on float vectors");
            }
        }
        std::string name = types::to_string(t);
        switch (op) {
        case TokenKind::plus_percent: return b_.CreateAdd(a, b);
        case TokenKind::minus_percent: return b_.CreateSub(a, b);
        case TokenKind::star_percent: return b_.CreateMul(a, b);
        case TokenKind::amp: return b_.CreateAnd(a, b);
        case TokenKind::pipe: return b_.CreateOr(a, b);
        case TokenKind::caret: return b_.CreateXor(a, b);
        case TokenKind::shl:
        case TokenKind::shr: {
            llvm::Value* width = vec_splat(t, llvm::ConstantInt::get(lower_type(lane), static_cast<std::uint64_t>(lane->bits)));
            check_lanes(b_.CreateICmpUGE(b, width), bx.span, std::format("shift count too large ({})", name));
            if (op == TokenKind::shl) return b_.CreateShl(a, b);
            return sgn ? b_.CreateAShr(a, b) : b_.CreateLShr(a, b);
        }
        default: break;
        }
        bool wrap = mode_setting("overflow") == "wrap";
        auto with_overflow = [&](llvm::Intrinsic::ID sid, llvm::Intrinsic::ID uid, const char* sym) -> llvm::Value* {
            llvm::Function* f = intrinsic_declaration(mod_.get(), sgn ? sid : uid, {a->getType()});
            llvm::Value*    r = b_.CreateCall(f, {a, b});
            check_lanes(b_.CreateExtractValue(r, 1), bx.span, overflow_message(bx.span, sym, name));
            return b_.CreateExtractValue(r, 0);
        };
        switch (op) {
        case TokenKind::plus: return wrap ? b_.CreateAdd(a, b) : with_overflow(llvm::Intrinsic::sadd_with_overflow, llvm::Intrinsic::uadd_with_overflow, "+");
        case TokenKind::minus: return wrap ? b_.CreateSub(a, b) : with_overflow(llvm::Intrinsic::ssub_with_overflow, llvm::Intrinsic::usub_with_overflow, "-");
        case TokenKind::star: return wrap ? b_.CreateMul(a, b) : with_overflow(llvm::Intrinsic::smul_with_overflow, llvm::Intrinsic::umul_with_overflow, "*");
        case TokenKind::slash:
        case TokenKind::percent: {
            check_lanes(b_.CreateICmpEQ(b, llvm::Constant::getNullValue(b->getType())), bx.span, division_by_zero_message(bx.span, "/"));
            if (!sgn) return op == TokenKind::slash ? b_.CreateUDiv(a, b) : b_.CreateURem(a, b);
            llvm::Value* minimum = vec_splat(t, llvm::ConstantInt::get(ctx_, llvm::APInt::getSignedMinValue(static_cast<unsigned>(lane->bits))));
            llvm::Value* m1      = llvm::Constant::getAllOnesValue(b->getType());
            check_lanes(b_.CreateAnd(b_.CreateICmpEQ(a, minimum), b_.CreateICmpEQ(b, m1)), bx.span, overflow_message(bx.span, "/", name));
            return op == TokenKind::slash ? b_.CreateSDiv(a, b) : b_.CreateSRem(a, b);
        }
        default: unsupported(bx.span, "this operator on vectors");
        }
    }

    // A float vector reduced as a fixed pairwise tree: the same result on every target.
    llvm::Value* pairwise(llvm::Value* v, unsigned n, const std::function<llvm::Value*(llvm::Value*, llvm::Value*)>& f) {
        while (n > 1) {
            unsigned half = n / 2;
            std::vector<int> lo, hi;
            for (unsigned i = 0; i < half; ++i) {
                lo.push_back(static_cast<int>(i));
                hi.push_back(static_cast<int>(i + half));
            }
            v = f(b_.CreateShuffleVector(v, lo), b_.CreateShuffleVector(v, hi));
            n = half;
        }
        return b_.CreateExtractElement(v, static_cast<std::uint64_t>(0));
    }

    llvm::Value* vector_iota(const types::Type* vt) {
        std::vector<llvm::Constant*> lanes;
        llvm::Type* lt = lower_type(vt->inner);
        for (int i = 0; i < vt->bits; ++i) {
            lanes.push_back(vt->inner->is_float() ? llvm::ConstantFP::get(lt, static_cast<double>(i))
                                                  : llvm::ConstantInt::get(lt, static_cast<std::uint64_t>(i)));
        }
        return llvm::ConstantVector::get(lanes);
    }

    // The elements pointer and the length of a span value.
    std::pair<llvm::Value*, llvm::Value*> span_parts(Expr& e) {
        const types::Type* st = types::strip_ref(e.type);
        llvm::Value*       addr = object_address(e);
        llvm::Type*        lt   = user_type(st->symbol);
        const Symbol*      data = nullptr;
        const Symbol*      count = nullptr;
        for (const Symbol* f : all_fields(st->symbol)) {
            if (f->name == "data") data = f;
            if (f->name == "count") count = f;
        }
        if (!data || !count) unsupported(e.span, "this span");
        return {b_.CreateLoad(ptr_type(), b_.CreateStructGEP(lt, addr, field_index(data))),
                b_.CreateLoad(llvm::Type::getInt64Ty(ctx_), b_.CreateStructGEP(lt, addr, field_index(count)))};
    }

    // `f32x8::splat(x)` and the other static functions of vectors and masks.
    llvm::Value* vector_static(CallExpr& c, const types::Type* vt, const std::string& name) {
        auto* i64 = llvm::Type::getInt64Ty(ctx_);
        if (vt->is(Kind::mask)) {   // prefix(k): lanes below k
            llvm::Value* k = b_.CreateZExtOrTrunc(gen_expr(*c.args[0]), i64);
            std::vector<llvm::Constant*> idx;
            for (int i = 0; i < vt->bits; ++i) idx.push_back(llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(i)));
            return b_.CreateICmpULT(llvm::ConstantVector::get(idx), b_.CreateVectorSplat(static_cast<unsigned>(vt->bits), k));
        }
        llvm::Type* lt = lower_type(vt);
        if (name == "splat") return vec_splat(vt, gen_expr(*c.args[0]));
        if (name == "iota") return vector_iota(vt);
        if (name == "from_bits") return b_.CreateBitCast(gen_expr(*c.args[0]), lt);
        if (name == "load" || name == "load_unchecked") {
            auto [data, len] = span_parts(*c.args[0]);
            llvm::Value* i   = b_.CreateZExtOrTrunc(gen_expr(*c.args[1]), i64);
            if (name == "load") {
                llvm::Value* end = b_.CreateAdd(i, llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(vt->bits)));
                check(b_.CreateAnd(b_.CreateICmpULE(end, len), b_.CreateICmpUGE(end, i)), c.span, "vector load out of bounds");
            }
            return b_.CreateAlignedLoad(lt, b_.CreateGEP(lower_type(vt->inner), data, i), llvm::Align(1));
        }
        if (name == "gather" || name == "gather_unchecked") {
            auto [data, len] = span_parts(*c.args[0]);
            llvm::Value* idx = gen_expr(*c.args[1]);
            llvm::Value* wide = b_.CreateZExtOrTrunc(idx, llvm::FixedVectorType::get(i64, static_cast<unsigned>(vt->bits)));
            if (name == "gather") check_lanes(b_.CreateICmpUGE(wide, b_.CreateVectorSplat(static_cast<unsigned>(vt->bits), len)), c.span, "gather index out of bounds");
            llvm::Value* ptrs = b_.CreateGEP(lower_type(vt->inner), data, wide);
            return b_.CreateMaskedGather(lt, ptrs, llvm::Align(1));
        }
        unsupported(c.span, "this vector function");
    }

    // `s.load(at)` / `s.store(at, v)`: one block; lanes past the end are 0 and are not written.
    llvm::Value* span_block(CallExpr& c, MemberExpr& me, const std::string& name) {
        auto* i64  = llvm::Type::getInt64Ty(ctx_);
        auto [data, len] = span_parts(*me.object);
        const types::Type* bt   = types::strip_ref(c.args[0]->type);
        llvm::Value*       baddr = object_address(*c.args[0]);
        llvm::Type*        blt  = user_type(bt->symbol);
        unsigned si = 0, ci = 0, ai = 0;
        for (const Symbol* f : all_fields(bt->symbol)) {
            if (f->name == "start") si = field_index(f);
            if (f->name == "count") ci = field_index(f);
            if (f->name == "active") ai = field_index(f);
        }
        llvm::Value* start  = b_.CreateLoad(i64, b_.CreateStructGEP(blt, baddr, si));
        llvm::Value* count  = b_.CreateLoad(i64, b_.CreateStructGEP(blt, baddr, ci));
        const types::Type* vt = types::strip_ref(name == "load" ? c.type : c.args[1]->type);
        llvm::Type*  mt     = llvm::FixedVectorType::get(llvm::Type::getInt1Ty(ctx_), static_cast<unsigned>(vt->bits));
        llvm::Value* active = b_.CreateLoad(mt, b_.CreateStructGEP(blt, baddr, ai));
        llvm::Value* end    = b_.CreateAdd(start, count);
        check(b_.CreateAnd(b_.CreateICmpULE(end, len), b_.CreateICmpUGE(end, start)), c.span, "vector block out of bounds");
        llvm::Value* at     = b_.CreateGEP(lower_type(vt->inner), data, start);
        if (name == "load") {
            return b_.CreateMaskedLoad(lower_type(vt), at, llvm::Align(1), active, llvm::Constant::getNullValue(lower_type(vt)));
        }
        b_.CreateMaskedStore(gen_expr(*c.args[1]), at, llvm::Align(1), active);
        return nullptr;
    }

    // Methods of vectors and masks.
    // The methods of numbers (spec 34). min/max ignore a NaN operand (IEEE minNum/maxNum);
    // integer abs and a clamp with lo > hi panic.
    llvm::Value* number_method(CallExpr& c, MemberExpr& me, const types::Type* t, const std::string& name) {
        llvm::Value* v   = gen_expr(*me.object);
        if (v->getType()->isPointerTy()) v = b_.CreateLoad(lower_type(t), v);
        auto arg = [&](std::size_t i) { return gen_expr(*c.args[i]); };
        llvm::Type* ty = v->getType();
        if (t->is_integer()) {
            bool sgn = t->is_signed;
            if (name == "abs") {
                if (!sgn) return v;
                llvm::Value* minimum = llvm::ConstantInt::get(ty, llvm::APInt::getSignedMinValue(static_cast<unsigned>(t->bits)));
                check(b_.CreateICmpNE(v, minimum), c.span, overflow_message(c.span, "abs", t->name), {part(": abs("), part(v, true), part(")")});
                return b_.CreateBinaryIntrinsic(llvm::Intrinsic::abs, v, llvm::ConstantInt::getFalse(ctx_));
            }
            if (name == "min") return b_.CreateBinaryIntrinsic(sgn ? llvm::Intrinsic::smin : llvm::Intrinsic::umin, v, arg(0));
            if (name == "max") return b_.CreateBinaryIntrinsic(sgn ? llvm::Intrinsic::smax : llvm::Intrinsic::umax, v, arg(0));
            if (name == "clamp") {
                llvm::Value* lo = arg(0);
                llvm::Value* hi = arg(1);
                check(sgn ? b_.CreateICmpSLE(lo, hi) : b_.CreateICmpULE(lo, hi), c.span, "clamp with lo > hi",
                      {part(": lo "), part(lo, sgn), part(", hi "), part(hi, sgn)});
                llvm::Value* up = b_.CreateBinaryIntrinsic(sgn ? llvm::Intrinsic::smax : llvm::Intrinsic::umax, v, lo);
                return b_.CreateBinaryIntrinsic(sgn ? llvm::Intrinsic::smin : llvm::Intrinsic::umin, up, hi);
            }
            unsupported(c.span, "this integer method");
        }
        bool f32 = t->bits <= 32;
        auto unary = [&](llvm::Intrinsic::ID id) { return b_.CreateUnaryIntrinsic(id, v); };
        auto libm  = [&](const char* name64, const char* name32, std::vector<llvm::Value*> args) -> llvm::Value* {
            if (t->bits == 16) {   // f16: the f32 function, then back
                auto* f = llvm::Type::getFloatTy(ctx_);
                for (llvm::Value*& a : args) a = b_.CreateFPExt(a, f);
                std::vector<llvm::Type*> params(args.size(), f);
                return b_.CreateFPTrunc(b_.CreateCall(libc(name32, f, params), args), ty);
            }
            std::vector<llvm::Type*> params(args.size(), ty);
            if (t->bits == 128) return b_.CreateCall(libc((std::string(name64) + "f128").c_str(), ty, params), args);   // glibc's binary128 libm
            return b_.CreateCall(libc(f32 ? name32 : name64, ty, params), args);
        };
        if (t->bits == 128) {
            // f128: LLVM would call the x87 `long double` functions (`sqrtl`) on x86-64, so the
            // binary128 functions are called by name (glibc's `sqrtf128`, ...).
            static const std::map<std::string_view, std::pair<const char*, int>> quad = {
                {"sqrt", {"sqrtf128", 1}},   {"floor", {"floorf128", 1}}, {"ceil", {"ceilf128", 1}},
                {"round", {"roundf128", 1}}, {"trunc", {"truncf128", 1}}, {"sin", {"sinf128", 1}},
                {"cos", {"cosf128", 1}},     {"exp", {"expf128", 1}},     {"exp2", {"exp2f128", 1}},
                {"log", {"logf128", 1}},     {"log2", {"log2f128", 1}},   {"log10", {"log10f128", 1}},
                {"pow", {"powf128", 2}},     {"min", {"fminf128", 2}},    {"max", {"fmaxf128", 2}},
                {"mul_add", {"fmaf128", 3}}};
            if (auto q = quad.find(name); q != quad.end()) {
                std::vector<llvm::Value*> args{v};
                for (int i = 1; i < q->second.second; ++i) args.push_back(arg(static_cast<std::size_t>(i - 1)));
                std::vector<llvm::Type*> params(args.size(), ty);
                return b_.CreateCall(libc(q->second.first, ty, params), args);
            }
        }
        if (name == "sqrt") return unary(llvm::Intrinsic::sqrt);
        if (name == "abs") return unary(llvm::Intrinsic::fabs);
        if (name == "floor") return unary(llvm::Intrinsic::floor);
        if (name == "ceil") return unary(llvm::Intrinsic::ceil);
        if (name == "round") return unary(llvm::Intrinsic::round);
        if (name == "trunc") return unary(llvm::Intrinsic::trunc);
        if (name == "sin") return unary(llvm::Intrinsic::sin);
        if (name == "cos") return unary(llvm::Intrinsic::cos);
        if (name == "exp") return unary(llvm::Intrinsic::exp);
        if (name == "exp2") return unary(llvm::Intrinsic::exp2);
        if (name == "log") return unary(llvm::Intrinsic::log);
        if (name == "log2") return unary(llvm::Intrinsic::log2);
        if (name == "log10") return unary(llvm::Intrinsic::log10);
        if (name == "tan") return libm("tan", "tanf", {v});
        if (name == "asin") return libm("asin", "asinf", {v});
        if (name == "acos") return libm("acos", "acosf", {v});
        if (name == "atan") return libm("atan", "atanf", {v});
        if (name == "atan2") return libm("atan2", "atan2f", {v, arg(0)});
        if (name == "pow") return b_.CreateBinaryIntrinsic(llvm::Intrinsic::pow, v, arg(0));
        if (name == "copysign") return b_.CreateBinaryIntrinsic(llvm::Intrinsic::copysign, v, arg(0));
        if (name == "min") return b_.CreateBinaryIntrinsic(llvm::Intrinsic::minnum, v, arg(0));
        if (name == "max") return b_.CreateBinaryIntrinsic(llvm::Intrinsic::maxnum, v, arg(0));
        if (name == "mul_add") {   // always one fused operation, whatever @std::float_mode says
            llvm::Value* b = arg(0);
            llvm::Value* d = arg(1);
            return b_.CreateIntrinsic(llvm::Intrinsic::fma, {ty}, {v, b, d});
        }
        if (name == "clamp") {
            llvm::Value* lo = arg(0);
            llvm::Value* hi = arg(1);
            check(b_.CreateFCmpOLE(lo, hi), c.span, "clamp with lo > hi (or a NaN bound)");
            if (t->bits == 128) {   // no libcall needed: compares and selects
                llvm::Value* up = b_.CreateSelect(b_.CreateFCmpOLT(v, lo), lo, v);
                return b_.CreateSelect(b_.CreateFCmpOGT(up, hi), hi, up);
            }
            return b_.CreateBinaryIntrinsic(llvm::Intrinsic::minnum, b_.CreateBinaryIntrinsic(llvm::Intrinsic::maxnum, v, lo), hi);
        }
        llvm::Value* inf = llvm::ConstantFP::getInfinity(ty);
        if (name == "is_nan") return b_.CreateFCmpUNO(v, v);
        if (name == "is_infinite") return b_.CreateFCmpOEQ(unary(llvm::Intrinsic::fabs), inf);
        if (name == "is_finite") return b_.CreateFCmpOLT(unary(llvm::Intrinsic::fabs), inf);
        unsupported(c.span, "this float method");
    }

    llvm::Value* vector_method(CallExpr& c, MemberExpr& me, const types::Type* t, const std::string& name) {
        auto*        i64 = llvm::Type::getInt64Ty(ctx_);
        llvm::Value* v   = gen_expr(*me.object);
        unsigned     n   = static_cast<unsigned>(t->bits);
        if (t->is(Kind::mask)) {
            if (name == "any") return b_.CreateOrReduce(v);
            if (name == "all") return b_.CreateAndReduce(v);
            if (name == "none") return b_.CreateNot(b_.CreateOrReduce(v));
            if (name == "count") {
                llvm::Value* bits = b_.CreateBitCast(v, llvm::IntegerType::get(ctx_, n));
                llvm::Value* pop  = b_.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, bits);
                return b_.CreateZExtOrTrunc(pop, i64);
            }
            if (name == "first") {
                const types::Type* opt = types::strip_ref(c.type);
                llvm::Value* bits = b_.CreateBitCast(v, llvm::IntegerType::get(ctx_, n));
                llvm::Value* tz   = b_.CreateBinaryIntrinsic(llvm::Intrinsic::cttz, bits, llvm::ConstantInt::getFalse(ctx_));
                llvm::Value* some = alternative_value(alternative(opt->symbol, "some"), b_.CreateZExtOrTrunc(tz, i64));
                llvm::Value* none = alternative_value(alternative(opt->symbol, "none"), nullptr);
                return b_.CreateSelect(b_.CreateOrReduce(v), some, none);
            }
            if (name == "select") return b_.CreateSelect(v, gen_expr(*c.args[0]), gen_expr(*c.args[1]));
            unsupported(c.span, "this mask method");
        }
        const types::Type* lane = t->inner;
        bool flt = lane->is_float(), sgn = lane->is_integer() && lane->is_signed;
        auto arg = [&](std::size_t i) { return gen_expr(*c.args[i]); };
        auto bin = [&](llvm::Intrinsic::ID id, llvm::Value* a, llvm::Value* b) { return b_.CreateBinaryIntrinsic(id, a, b); };
        if (name == "min") return flt ? bin(llvm::Intrinsic::minnum, v, arg(0)) : bin(sgn ? llvm::Intrinsic::smin : llvm::Intrinsic::umin, v, arg(0));
        if (name == "max") return flt ? bin(llvm::Intrinsic::maxnum, v, arg(0)) : bin(sgn ? llvm::Intrinsic::smax : llvm::Intrinsic::umax, v, arg(0));
        if (name == "clamp") {
            llvm::Value* lo = arg(0);
            llvm::Value* hi = arg(1);
            llvm::Value* x  = flt ? bin(llvm::Intrinsic::maxnum, v, lo) : bin(sgn ? llvm::Intrinsic::smax : llvm::Intrinsic::umax, v, lo);
            return flt ? bin(llvm::Intrinsic::minnum, x, hi) : bin(sgn ? llvm::Intrinsic::smin : llvm::Intrinsic::umin, x, hi);
        }
        if (name == "abs") {
            if (flt) return b_.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, v);
            if (!sgn) return v;
            return bin(llvm::Intrinsic::abs, v, llvm::ConstantInt::getFalse(ctx_));
        }
        if (name == "mul_add") {
            llvm::Value* b = arg(0);
            llvm::Value* cc = arg(1);
            if (flt) return b_.CreateIntrinsic(llvm::Intrinsic::fma, {v->getType()}, {v, b, cc});
            return b_.CreateAdd(b_.CreateMul(v, b), cc);
        }
        if (name == "sqrt") return b_.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, v);
        if (name == "floor") return b_.CreateUnaryIntrinsic(llvm::Intrinsic::floor, v);
        if (name == "ceil") return b_.CreateUnaryIntrinsic(llvm::Intrinsic::ceil, v);
        if (name == "round") return b_.CreateUnaryIntrinsic(llvm::Intrinsic::round, v);
        if (name == "trunc") return b_.CreateUnaryIntrinsic(llvm::Intrinsic::trunc, v);
        if (name == "saturating_add") return bin(sgn ? llvm::Intrinsic::sadd_sat : llvm::Intrinsic::uadd_sat, v, arg(0));
        if (name == "saturating_sub") return bin(sgn ? llvm::Intrinsic::ssub_sat : llvm::Intrinsic::usub_sat, v, arg(0));
        if (name == "reduce_add") {
            if (flt) return pairwise(v, n, [&](llvm::Value* a, llvm::Value* b) { return b_.CreateFAdd(a, b); });
            return b_.CreateAddReduce(v);
        }
        if (name == "reduce_mul") {
            if (flt) return pairwise(v, n, [&](llvm::Value* a, llvm::Value* b) { return b_.CreateFMul(a, b); });
            return b_.CreateMulReduce(v);
        }
        if (name == "reduce_min") {
            if (flt) return pairwise(v, n, [&](llvm::Value* a, llvm::Value* b) { return bin(llvm::Intrinsic::minnum, a, b); });
            return sgn ? b_.CreateIntMinReduce(v, true) : b_.CreateIntMinReduce(v, false);
        }
        if (name == "reduce_max") {
            if (flt) return pairwise(v, n, [&](llvm::Value* a, llvm::Value* b) { return bin(llvm::Intrinsic::maxnum, a, b); });
            return sgn ? b_.CreateIntMaxReduce(v, true) : b_.CreateIntMaxReduce(v, false);
        }
        if (name == "reduce_and") return b_.CreateAndReduce(v);
        if (name == "reduce_or") return b_.CreateOrReduce(v);
        if (name == "reduce_xor") return b_.CreateXorReduce(v);
        if (name == "with") {
            llvm::Value* i = b_.CreateZExtOrTrunc(arg(0), i64);
            check(b_.CreateICmpULT(i, llvm::ConstantInt::get(i64, n)), c.span, "lane index out of bounds",
                  {part(": "), part(i, false), part(std::format(" (lanes: {})", n))});
            return b_.CreateInsertElement(v, arg(1), i);
        }
        if (name == "to_bits") return b_.CreateBitCast(v, lower_type(types::strip_ref(c.type)));
        if (name == "reverse") {
            std::vector<int> order;
            for (unsigned i = 0; i < n; ++i) order.push_back(static_cast<int>(n - 1 - i));
            return b_.CreateShuffleVector(v, order);
        }
        if (name == "rotate_lanes") {   // lane i takes lane (i + k) % n
            llvm::Value* k = b_.CreateURem(b_.CreateZExtOrTrunc(arg(0), i64), llvm::ConstantInt::get(i64, n));
            std::vector<llvm::Constant*> idx;
            for (unsigned i = 0; i < n; ++i) idx.push_back(llvm::ConstantInt::get(i64, i));
            llvm::Value* from = b_.CreateURem(b_.CreateAdd(llvm::ConstantVector::get(idx), b_.CreateVectorSplat(n, k)),
                                              b_.CreateVectorSplat(n, llvm::ConstantInt::get(i64, n)));
            llvm::Value* out = llvm::UndefValue::get(v->getType());
            for (unsigned i = 0; i < n; ++i) {
                out = b_.CreateInsertElement(out, b_.CreateExtractElement(v, b_.CreateExtractElement(from, static_cast<std::uint64_t>(i))),
                                             static_cast<std::uint64_t>(i));
            }
            return out;
        }
        if (name == "interleave_low" || name == "interleave_high") {
            std::vector<int> order;
            unsigned base = name == "interleave_low" ? 0 : n / 2;
            for (unsigned i = 0; i < n / 2; ++i) {
                order.push_back(static_cast<int>(base + i));
                order.push_back(static_cast<int>(n + base + i));
            }
            return b_.CreateShuffleVector(v, arg(0), order);
        }
        if (name == "shuffle" || name == "shuffle_with") {
            std::vector<int> order;
            for (ExprPtr& x : c.args.back()->as<ListExpr>().elements) order.push_back(static_cast<int>(x->as<LiteralExpr>().int_value));
            if (name == "shuffle") return b_.CreateShuffleVector(v, order);
            return b_.CreateShuffleVector(v, arg(0), order);
        }
        if (name == "split") {
            std::vector<int> lo, hi;
            for (unsigned i = 0; i < n / 2; ++i) {
                lo.push_back(static_cast<int>(i));
                hi.push_back(static_cast<int>(i + n / 2));
            }
            llvm::Value* tup = llvm::UndefValue::get(lower_type(types::strip_ref(c.type)));
            tup = b_.CreateInsertValue(tup, b_.CreateShuffleVector(v, lo), {0});
            return b_.CreateInsertValue(tup, b_.CreateShuffleVector(v, hi), {1});
        }
        if (name == "store" || name == "store_unchecked" || name == "scatter" || name == "scatter_unchecked") {
            Expr* target = c.args[0].get();
            if (target->kind == ExprKind::unary && target->as<UnaryExpr>().op == UnaryOp::mut_marker) target = target->as<UnaryExpr>().operand.get();
            auto [data, len] = span_parts(*target);
            if (name.starts_with("store")) {
                llvm::Value* i = b_.CreateZExtOrTrunc(arg(1), i64);
                if (name == "store") {
                    llvm::Value* end = b_.CreateAdd(i, llvm::ConstantInt::get(i64, n));
                    check(b_.CreateAnd(b_.CreateICmpULE(end, len), b_.CreateICmpUGE(end, i)), c.span, "vector store out of bounds");
                }
                b_.CreateAlignedStore(v, b_.CreateGEP(lower_type(lane), data, i), llvm::Align(1));
                return nullptr;
            }
            llvm::Value* wide = b_.CreateZExtOrTrunc(arg(1), llvm::FixedVectorType::get(i64, n));
            if (name == "scatter") check_lanes(b_.CreateICmpUGE(wide, b_.CreateVectorSplat(n, len)), c.span, "scatter index out of bounds");
            b_.CreateMaskedScatter(v, b_.CreateGEP(lower_type(lane), data, wide), llvm::Align(1));
            return nullptr;
        }
        unsupported(c.span, "this vector method");
    }

    llvm::Value* call_value(CallExpr& c) {
        if (c.wrapped) return wrapped_body(c);
        if (c.callee->kind == ExprKind::member) {
            auto&              me = c.callee->as<MemberExpr>();
            const types::Type* ot = types::strip_ref(me.object->type);
            if (ot && (ot->is(Kind::vector) || ot->is(Kind::mask))) return vector_method(c, me, ot, me.name.segments.back().name);
            if (ot && types::is_number_method(ot, me.name.segments.back().name)) return number_method(c, me, ot, me.name.segments.back().name);
            if (ot && (types::is_std_instance(ot, "span") || types::is_std_instance(ot, "span_mut")) && !c.args.empty() &&
                (me.name.segments.back().name == "load" || me.name.segments.back().name == "store")) {
                const types::Type* at = types::strip_ref(c.args[0]->type);
                if (at && at->is(Kind::user) && at->symbol->template_of && at->symbol->template_of->name == "block") {
                    return span_block(c, me, me.name.segments.back().name);
                }
            }
        }
        if (c.callee->kind == ExprKind::path && !c.callee->as<PathExpr>().symbol && c.callee->type &&
            (c.callee->type->is(Kind::vector) || c.callee->type->is(Kind::mask))) {
            return vector_static(c, c.callee->type, c.callee->as<PathExpr>().path.segments.back().name);
        }
        if (c.callee->kind == ExprKind::path && !c.callee->as<PathExpr>().symbol) {
            const auto& segs = c.callee->as<PathExpr>().path.segments;
            if (segs.size() == 3 && segs[0].name == "std" && segs[1].name == "simd" && segs[2].name == "join") {
                llvm::Value*     lo = gen_expr(*c.args[0]);
                llvm::Value*     hi = gen_expr(*c.args[1]);
                std::vector<int> order;
                unsigned         n  = static_cast<unsigned>(types::strip_ref(c.args[0]->type)->bits);
                for (unsigned i = 0; i < 2 * n; ++i) order.push_back(static_cast<int>(i));
                return b_.CreateShuffleVector(lo, hi, order);
            }
        }
        if (is_std_output(c)) {
            std_output(c);
            return nullptr;
        }
        if (std_name(c) == "format") return std_format(c);
        if (std::string n = std_name(c); n.starts_with("checked_") || n.starts_with("saturating_")) return arith_builtin(c, n);
        if (std::string n = std_name(c); n == "trailing_zeros" || n == "leading_zeros" || n == "count_ones" || n == "rotate_left" ||
                                         n == "rotate_right") {
            return bit_builtin(c, n);
        }
        Expr& callee = *c.callee;
        if (callee.kind == ExprKind::path) {
            const auto&   pe = callee.as<PathExpr>();
            const Symbol* s  = pe.symbol;
            if (!s) {
                if (std::string r = ct::raw_intrinsic(c); !r.empty()) return raw_call(c, r);
                if (ct::builtin_name(c) == "at_compile_time") return llvm::ConstantInt::getFalse(ctx_);   // spec 15: the compiled program
                const auto& segs = pe.path.segments;
                if (segs.size() == 4 && segs[0].name == "std" && segs[1].name == "simd") {
                    if (const simd::Intrinsic* in = simd::find_intrinsic(segs[2].name, segs[3].name)) return simd_intrinsic(c, *in);
                }
                if (segs.size() == 2 && segs[0].name == "std" && (segs[1].name == "size_of" || segs[1].name == "align_of") &&
                    segs[1].template_args.size() == 1 && segs[1].template_args[0].type) {
                    // A type whose layout the checker couldn't fold (a lambda): LLVM knows it.
                    const types::Type* t   = tc_.lower(*segs[1].template_args[0].type, cur_subst_);
                    llvm::Type*        lt  = lower_type(t);
                    const auto&        dl  = mod_->getDataLayout();
                    std::uint64_t      v   = segs[1].name == "size_of" ? dl.getTypeAllocSize(lt).getFixedValue() : dl.getABITypeAlign(lt).value();
                    return llvm::ConstantInt::get(lower_type(c.type), v);
                }
                unrecognised(callee.span, to_string(pe.path));
            }
            if (s->kind == SymbolKind::alternative) {
                if (c.args.size() == 1 && !carries(s)) gen_expr(*c.args[0]);   // a `void` payload: only its effects (spec 08)
                if (c.args.size() != 1 || !carries(s)) return alternative_value(s, nullptr);
                const types::Type* pt = payload_of(s);
                llvm::Value* payload = pt->is(Kind::reference) ? object_address(*c.args[0])   // `std::option<T&>`
                                                               : gen_expr(*c.args[0]);
                moved_out(*c.args[0]);
                return alternative_value(s, payload);
            }
            if (s->kind == SymbolKind::function || s->kind == SymbolKind::method) {
                llvm::Value* receiver = nullptr;
                if (has_self(s)) {
                    if (!self_) unsupported(c.span, "calling a method without a receiver");
                    receiver = self_;
                }
                return call_function(s, receiver, c);
            }
            const types::Type* ct = types::strip_ref(callee.type);
            if (ct && (ct->is(Kind::function) || ct->is(Kind::fn_ptr))) {
                std::vector<Expr*> args;
                for (ExprPtr& a : c.args) args.push_back(a.get());
                return callable_call(callee, ct, args);
            }
            if (ct && ct->is(Kind::closure)) {
                std::vector<Expr*> args;
                for (ExprPtr& a : c.args) args.push_back(a.get());
                return closure_call(callee, ct, args);
            }
            unsupported(c.span, std::format("calling function values (of type {})", ct ? types::to_string(ct) : "?"));
        }
        if (callee.kind == ExprKind::member) {
            auto&                 me   = callee.as<MemberExpr>();
            const Symbol*         s    = me.symbol;
            const types::Type*    obj  = types::strip_ref(me.object->type);
            std::string           name = me.name.segments.back().name;
            if (me.object->constant && (me.object->constant->kind == ct::Value::Kind::field ||
                                        me.object->constant->kind == ct::Value::Kind::alternative)) {
                return meta_call(c, *me.object->constant, name);
            }
            if (s && s->kind == SymbolKind::field) {   // a stored std::function / std::fn_ptr
                std::vector<Expr*> args;
                for (ExprPtr& a : c.args) args.push_back(a.get());
                return callable_call(callee, types::strip_ref(callee.type), args);
            }
            if (obj && obj->is(Kind::pointer) && !me.arrow && name == "offset") {   // spec 13
                const types::Type* nt = types::strip_ref(c.args.at(0)->type);
                llvm::Value* base = gen_expr(*me.object);
                llvm::Value* n    = gen_expr(*c.args[0]);
                n = nt && nt->is_integer() && !nt->is_signed ? b_.CreateZExtOrTrunc(n, llvm::Type::getInt64Ty(ctx_))
                                                             : b_.CreateSExtOrTrunc(n, llvm::Type::getInt64Ty(ctx_));
                return b_.CreateGEP(lower_type(obj->inner), base, n);
            }
            if (s && s->kind == SymbolKind::function) {   // a std::str method: a library function, text first
                std::vector<Expr*> args{me.object.get()};
                for (ExprPtr& a : c.args) args.push_back(a.get());
                return call_symbol(s, nullptr, args, c.span);
            }
            const types::Type* dispatched = !me.arrow ? obj
                                          : me.deref_symbol ? types::strip_ref(tc_.lower(*body_decl(me.deref_symbol)->result,
                                                                                         sema::substitution_of(me.deref_symbol)))
                                          : obj && obj->is(Kind::pointer) ? obj->inner : nullptr;
            if (s && s->kind == SymbolKind::method && is_interface(dispatched)) {   // through a fat pointer
                int slot = interface_slot(dispatched->symbol, s);
                if (slot < 0) unsupported(c.span, "this interface call");
                llvm::Value* fat    = !me.arrow       ? interface_reference(*me.object, dispatched->symbol)
                                    : me.deref_symbol ? member_object(me)
                                                      : gen_expr(*me.object);
                const Symbol* only  = single_implementation(dispatched->symbol, s);
                if (inspect_ && only) {
                    insight(callee_name(c), "dispatch", "devirtualized", "range",
                            std::format("direct call: {} is the only implementation of {}::{}", display_name(only),
                                        types::to_string(dispatched), name),
                            "The whole program is one module, so the compiler sees every type that implements the interface: with one "
                            "implementation the call goes straight to it and can be inlined (spec 34 §8). A second implementing type "
                            "makes it a table call again.");
                } else if (inspect_) {
                    insight(callee_name(c), "dispatch", "interface", "range",
                            std::format("interface call: {}::{} through a table", types::to_string(dispatched), name),
                            "The object is reached through an interface reference (an object pointer and a table, spec 07): the call loads "
                            "the method's address from the table, so it can't be inlined.");
                }
                std::vector<Expr*> args;
                for (ExprPtr& a : c.args) args.push_back(a.get());
                if (only) return call_symbol(only, b_.CreateExtractValue(fat, 0), args, c.span);   // spec 34 §8: one implementing type
                llvm::Value* table  = b_.CreateExtractValue(fat, 1);
                if (c.hint_symbol && reaches(c.hint_symbol, dispatched->symbol)) {   // @std::dispatch_hint<T>
                    if (const Symbol* impl = implementation(c.hint_symbol, s)) {
                        speculation_ = Speculation{b_.CreateICmpEQ(table, itable_of(c.hint_symbol, dispatched->symbol)), function_of(impl)};
                        if (inspect_) {
                            insight(callee_name(c), "dispatch", "speculative", "range",
                                    std::format("guarded direct call to {} (@std::dispatch_hint), the table call otherwise", display_name(impl)),
                                    "The object's interface table is compared with the hinted class's: on a match the call goes "
                                    "straight to its implementation and can be inlined (performance §8).");
                        }
                    }
                }
                llvm::Value* target = b_.CreateLoad(ptr_type(), b_.CreateConstGEP1_32(ptr_type(), table, itable_header + static_cast<unsigned>(slot)));
                return call_symbol(s, b_.CreateExtractValue(fat, 0), args, c.span, false, target);
            }
            const types::Type* target = me.deref_symbol ? types::strip_ref(tc_.lower(*body_decl(me.deref_symbol)->result,
                                                                                      sema::substitution_of(me.deref_symbol)))
                                                        : obj;
            if (s && s->kind == SymbolKind::method && (!me.arrow || me.deref_symbol) && me.name.segments.size() == 1 && target &&
                target->is(Kind::user) && has_vtable(target->symbol) && has_self(s)) {
                int slot = vtable_slot(target->symbol, s);   // a virtual method: the object's own implementation
                if (slot >= 0) {
                    const Symbol* only = single_implementation(target->symbol, s);
                    if (inspect_ && only) {
                        insight(callee_name(c), "dispatch", "devirtualized", "range",
                                std::format("direct call: {} is the only implementation in the program", display_name(only)),
                                "The whole program is one module, so the compiler sees every class derived from this one: no class "
                                "overrides the method, so the call goes straight to it and can be inlined (spec 34 §8).");
                    } else if (inspect_) {
                        insight(callee_name(c), "dispatch", "virtual", "range",
                                std::format("virtual call: {}::{} through the vtable", types::to_string(target), name),
                                "An `open` class hierarchy (spec 07): the call loads the most derived implementation from the object's "
                                "vtable. Call `object.base::method()` to call one implementation directly.");
                    }
                    llvm::Value* object = member_object(me);
                    std::vector<Expr*> args;
                    for (ExprPtr& a : c.args) args.push_back(a.get());
                    if (only) return call_symbol(only, object, args, c.span);   // spec 34 §8: one target
                    if (c.hint_symbol && c.hint_symbol->kind == SymbolKind::class_ && reaches(c.hint_symbol, target->symbol)) {
                        if (const Symbol* impl = implementation(c.hint_symbol, s)) {   // @std::dispatch_hint<T>
                            llvm::Value* vtable = b_.CreateLoad(ptr_type(), object);
                            speculation_ = Speculation{b_.CreateICmpEQ(vtable, vtable_of(c.hint_symbol)), function_of(impl)};
                            if (inspect_) {
                                insight(callee_name(c), "dispatch", "speculative", "range",
                                        std::format("guarded direct call to {} (@std::dispatch_hint), the vtable call otherwise",
                                                    display_name(impl)),
                                        "The object's vtable is compared with the hinted class's: on a match the call goes straight "
                                        "to its implementation and can be inlined (performance §8).");
                            }
                        }
                    }
                    return call_symbol(s, object, args, c.span, false, virtual_function(object, slot));
                }
            }
            if (s && s->kind == SymbolKind::method) {
                if (me.deref_symbol) return call_function(s, has_self(s) ? member_object(me) : nullptr, c);
                if (me.arrow) return call_function(s, has_self(s) ? gen_expr(*me.object) : nullptr, c);
                llvm::Value* object = has_self(s) ? object_address(*me.object) : nullptr;
                llvm::Value* result = call_function(s, object, c);
                if (const FunctionDecl* fd = body_decl(s); fd && fd->is_move && object) {   // the method owns it now (spec 06)
                    moved_out(*me.object);
                    release_temporary(object);
                }
                return result;
            }
            if (!s && types::is_std_builtin_method(obj, name)) {
                llvm::Value* v = std_method(c, me, obj, name);
                // `std::option<T&>`: value_or/expect give the reference; the value is read through it
                if (v && v->getType()->isPointerTy() && c.type && c.type->is(Kind::reference) &&
                    !is_interface(c.type->inner)) {
                    return b_.CreateLoad(lower_type(types::strip_ref(c.type)), v);
                }
                return v;
            }
            if (name == "clone" && c.args.empty() && obj && !s) return clone_value(obj, object_address(*me.object));
            if (name == "length" && c.args.empty() && is_str(obj)) return b_.CreateExtractValue(gen_expr(*me.object), 1);
            if (name == "as_ptr" && c.args.empty() && is_str(obj)) return b_.CreateExtractValue(gen_expr(*me.object), 0);
            if (name == "is_empty" && c.args.empty() && is_str(obj)) {
                return b_.CreateICmpEQ(b_.CreateExtractValue(gen_expr(*me.object), 1), llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0));
            }
            unsupported(c.span, std::format("the method '{}' on '{}'", name, obj ? types::to_string(obj) : "an unknown type"));
        }
        // Any other place holding a callable: `handlers[i](x)`, `this.table.on_done(x)`. (A temporary
        // std::function, `make()(x)`, would need dropping after the call: not yet.)
        if (const types::Type* ct = types::strip_ref(callee.type);
            ct && (ct->is(Kind::fn_ptr) || (ct->is(Kind::function) && is_place(callee)))) {
            std::vector<Expr*> args;
            for (ExprPtr& a : c.args) args.push_back(a.get());
            return callable_call(callee, ct, args);
        }
        if (const types::Type* ct = types::strip_ref(callee.type); ct && (ct->is(Kind::function) || ct->is(Kind::fn_ptr) || ct->is(Kind::closure))) {
            std::vector<Expr*> args;   // calling a value: `make()(x)` (spec 18)
            for (ExprPtr& a : c.args) args.push_back(a.get());
            return ct->is(Kind::closure) ? closure_call(callee, ct, args) : callable_call(callee, ct, args);
        }
        unsupported(c.span, "calling this expression");
    }

    llvm::Value* call_function(const Symbol* s, llvm::Value* receiver, CallExpr& c) {
        std::vector<Expr*> args;
        for (const ExprPtr& a : c.args) args.push_back(a.get());
        call_expr_ = &c;
        return call_symbol(s, receiver, args, c.span);
    }

    // Calls the function `s` with `in` as its arguments (after the receiver, for a method). A
    // reference result is read through unless `want_address`, which returns the address itself.
    // `@std::dispatch_hint<T>` (performance §8): the next dynamic call compares the object's table
    // with T's and calls T's implementation directly when they match.
    struct Speculation {
        llvm::Value*    guard  = nullptr;   // the object's table is T's
        llvm::Function* direct = nullptr;   // T's implementation
    };
    Speculation speculation_;

    llvm::Value* call_symbol(const Symbol* s, llvm::Value* receiver, const std::vector<Expr*>& in, Span span,
                             bool want_address = false, llvm::Value* indirect = nullptr) {
        Speculation speculate = std::exchange(speculation_, Speculation{});
        const CallExpr* call_ast = std::exchange(call_expr_, nullptr);   // set by call_function: the call being made
        const FunctionDecl* f = body_decl(s);
        if (!f) unsupported(span, "this function");
        if ((f->is_template && !s->template_of) || (s->parent && s->parent->decl && s->parent->decl->is_template && !s->parent->template_of)) {
            unsupported(span, "calling templates");
        }
        if (f->is_abstract && !indirect) unsupported(span, "calling an abstract method directly");
        std::vector<llvm::Value*> args;
        if (receiver) args.push_back(receiver);
        for (std::size_t i = 0; i < in.size() && i < f->params.size(); ++i) {
            const types::Type* pt = tc_.lower(*f->params[i].type, sema::substitution_of(s));
            if (pt->is(Kind::reference) && is_interface(pt->inner)) {
                args.push_back(interface_reference(*in[i], pt->inner->symbol));
            } else if (pt->is(Kind::reference)) {
                args.push_back(reference_to(*in[i]));
            } else {
                args.push_back(gen_expr(*in[i]));
                moved_out(*in[i]);
                if (inspect_) record_copy(*in[i], pt);
            }
        }
        bool            imported = f->is_extern && !f->body;
        if (imported) {   // C's bool is a byte
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (args[i]->getType()->isIntegerTy(1)) args[i] = b_.CreateZExt(args[i], llvm::Type::getInt8Ty(ctx_));
            }
        }
        bool tracked = !indirect && has_run_time_entry_checks(s);
        bool by_arg  = tracked && !imported && !f->is_extern && s->versions.empty() && !s->version_of && !from_shared_library(s);
        if (tracked && !by_arg) b_.CreateStore(loc_global(span), call_site_global());
        llvm::Value* call = nullptr;
        if (by_arg) {   // the caller's location as a hidden argument (site_variant_of)
            std::vector<llvm::Value*> with_site = args;
            with_site.push_back(loc_global(span));
            call = b_.CreateCall(site_variant_of(s), with_site);
        } else if (indirect && speculate.guard) {   // guarded direct call, the dynamic call for other classes
            llvm::Function*   fn     = cur_fn_ ? cur_fn_ : b_.GetInsertBlock()->getParent();
            llvm::BasicBlock* direct = llvm::BasicBlock::Create(ctx_, "hint.direct", fn);
            llvm::BasicBlock* other  = llvm::BasicBlock::Create(ctx_, "hint.dynamic", fn);
            llvm::BasicBlock* done   = llvm::BasicBlock::Create(ctx_, "hint.done", fn);
            b_.CreateCondBr(speculate.guard, direct, other, llvm::MDBuilder(ctx_).createBranchWeights(15, 1));
            b_.SetInsertPoint(direct);
            llvm::Value* a = b_.CreateCall(speculate.direct->getFunctionType(), speculate.direct, args);
            b_.CreateBr(done);
            b_.SetInsertPoint(other);
            llvm::Value* d = b_.CreateCall(function_type(s), indirect, args);
            b_.CreateBr(done);
            b_.SetInsertPoint(done);
            if (d->getType()->isVoidTy()) {
                call = d;
            } else {
                llvm::PHINode* phi = b_.CreatePHI(d->getType(), 2);
                phi->addIncoming(a, direct);
                phi->addIncoming(d, other);
                call = phi;
            }
        } else {
            call = indirect ? b_.CreateCall(function_type(s), indirect, args)   // a virtual or interface call
                 : imported ? c_call(function_of(s), args)                       // C's calling convention
                            : b_.CreateCall(function_of(s), args);
        }
        if (tracked && !by_arg) b_.CreateStore(llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0)), call_site_global());
        if (inspect_ && !indirect) record_call(s, fns_.contains(s) ? fns_[s] : nullptr, call_ast, call_ast ? callee_name(*call_ast) : span);
        const types::Type* result = f->result ? tc_.lower(*f->result, sema::substitution_of(s)) : nullptr;
        if (imported && result && result->is(Kind::bool_)) return b_.CreateICmpNE(call, llvm::ConstantInt::get(call->getType(), 0));
        if (want_address) return call;
        if (result && result->is(Kind::reference) && is_interface(result->inner)) return call;   // the fat pointer
        if (result && result->is(Kind::reference)) return b_.CreateLoad(lower_type(types::strip_ref(result)), call);
        return call;
    }

    // The runtime functions the generated code calls. Their bodies are in llvm_runtime.cpp and
    // are appended to the module text by generate_llvm_ir.
    void declare_runtime() {
        auto* i32 = llvm::Type::getInt32Ty(ctx_);
        auto* i64 = llvm::Type::getInt64Ty(ctx_);
        auto* i128 = llvm::Type::getInt128Ty(ctx_);
        auto* ptr = llvm::PointerType::get(ctx_, 0);
        auto* vd  = llvm::Type::getVoidTy(ctx_);
        loc_type_ = llvm::StructType::get(ctx_, {ptr, i32, i32, ptr});
        edax_panic_str_ = llvm::Function::Create(llvm::FunctionType::get(vd, {ptr, ptr, i64}, false),
                                                 llvm::Function::ExternalLinkage, "edax_panic_str", mod_.get());
        edax_panic_str_->addFnAttr(llvm::Attribute::Cold);   // the failure path of every check: kept out of the hot code
        edax_exit_status_ = llvm::Function::Create(llvm::FunctionType::get(i32, {i128, ptr}, false),
                                                   llvm::Function::ExternalLinkage, "edax_exit_status", mod_.get());
    }

    // `message` as a constant byte array (not NUL-terminated), for `edax_panic_str`.
    llvm::Constant* message_bytes(const std::string& message) {
        llvm::Constant* init = llvm::ConstantDataArray::getString(ctx_, message, false);
        return new llvm::GlobalVariable(*mod_, init->getType(), true, llvm::GlobalValue::PrivateLinkage, init, ".msg");
    }

    // The C entry point of a program with `@std::test` functions (spec 25, 29). `edaxc test` runs
    // it with an environment variable:
    //   EDAX_TEST_LIST=1     prints "edax-tests:" and the tests' names, one per line;
    //   EDAX_TEST_RUN=name   runs that test (exit 0 when it returns; a panic aborts, exit 2: no such test).
    // Without either it runs `main` if there is one, else every test in order.
    void emit_test_entry(const std::vector<const Symbol*>& tests, const std::vector<llvm::Function*>& fns, const Symbol* main) {
        auto* i32 = llvm::Type::getInt32Ty(ctx_);
        auto* ptr = llvm::PointerType::get(ctx_, 0);
        if (main) {   // the program's own entry, renamed
            emit_entry(main);
            mod_->getFunction("main")->setName("edax_program_main");
        }
        llvm::Function*   init  = emit_init_globals();
        llvm::Function*   entry = llvm::Function::Create(llvm::FunctionType::get(i32, {i32, ptr}, false), llvm::Function::ExternalLinkage,
                                                         "main", mod_.get());
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", entry));
        llvm::FunctionCallee strcmp = mod_->getOrInsertFunction("strcmp", llvm::FunctionType::get(i32, {ptr, ptr}, false));
        llvm::FunctionCallee puts   = mod_->getOrInsertFunction("puts", llvm::FunctionType::get(i32, {ptr}, false));
        llvm::FunctionCallee getenv = mod_->getOrInsertFunction("getenv", llvm::FunctionType::get(ptr, {ptr}, false));
        llvm::Value* null = llvm::ConstantPointerNull::get(ptr);
        // EDAX_TEST_LIST
        llvm::BasicBlock* list     = llvm::BasicBlock::Create(ctx_, "list", entry);
        llvm::BasicBlock* not_list = llvm::BasicBlock::Create(ctx_, "not_list", entry);
        b.CreateCondBr(b.CreateICmpNE(b.CreateCall(getenv, {string_global("EDAX_TEST_LIST")}), null), list, not_list);
        b.SetInsertPoint(list);
        b.CreateCall(puts, {string_global("edax-tests:")});
        for (const Symbol* t : tests) b.CreateCall(puts, {string_global(t->name)});
        b.CreateRet(llvm::ConstantInt::get(i32, 0));
        // EDAX_TEST_RUN=name
        b.SetInsertPoint(not_list);
        llvm::Value*      name    = b.CreateCall(getenv, {string_global("EDAX_TEST_RUN")});
        llvm::BasicBlock* run_one = llvm::BasicBlock::Create(ctx_, "run_one", entry);
        llvm::BasicBlock* plain   = llvm::BasicBlock::Create(ctx_, "plain", entry);
        b.CreateCondBr(b.CreateICmpNE(name, null), run_one, plain);
        b.SetInsertPoint(run_one);
        b.CreateCall(init);
        for (std::size_t i = 0; i < tests.size(); ++i) {
            llvm::BasicBlock* yes  = llvm::BasicBlock::Create(ctx_, "test", entry);
            llvm::BasicBlock* next = llvm::BasicBlock::Create(ctx_, "next", entry);
            b.CreateCondBr(b.CreateICmpEQ(b.CreateCall(strcmp, {name, string_global(tests[i]->name)}), llvm::ConstantInt::get(i32, 0)), yes, next);
            b.SetInsertPoint(yes);
            b.CreateCall(fns[i]);
            b.CreateRet(llvm::ConstantInt::get(i32, 0));
            b.SetInsertPoint(next);
        }
        b.CreateRet(llvm::ConstantInt::get(i32, 2));   // no such test
        // Neither: the program's main, or every test in order.
        b.SetInsertPoint(plain);
        if (main) {
            b.CreateRet(b.CreateCall(mod_->getFunction("edax_program_main"), {entry->getArg(0), entry->getArg(1)}));
            return;
        }
        b.CreateCall(init);
        for (std::size_t i = 0; i < tests.size(); ++i) {
            b.CreateCall(fns[i]);
            b.CreateCall(puts, {string_global("test " + tests[i]->name + " ... ok")});
        }
        b.CreateRet(llvm::ConstantInt::get(i32, 0));
    }

    // The C entry point: runs Edax's `main` and returns its status.
    void emit_entry(const Symbol* main) {
        const FunctionDecl* f      = body_decl(main);
        auto*               i32    = llvm::Type::getInt32Ty(ctx_);
        auto*               ptr    = llvm::PointerType::get(ctx_, 0);
        llvm::Function*     entry  = llvm::Function::Create(llvm::FunctionType::get(i32, {i32, ptr}, false),
                                                            llvm::Function::ExternalLinkage, "main", mod_.get());
        llvm::BasicBlock*   block  = llvm::BasicBlock::Create(ctx_, "entry", entry);
        llvm::IRBuilder<>   b(block);
        // The arguments as std::str (checked to be UTF-8); raw for std::env::raw_args (spec 04).
        llvm::FunctionCallee main_args = mod_->getOrInsertFunction("edax_main_args", llvm::FunctionType::get(ptr, {i32, ptr}, false));
        llvm::Value*        array  = b.CreateCall(main_args, {entry->getArg(0), entry->getArg(1)});
        b.CreateCall(emit_init_globals());
        std::vector<llvm::Value*> main_in;
        if (!f->params.empty()) {   // main(std::span<std::str> args): { data, count }
            llvm::Type*  span = lower_type(tc_.lower(*f->params[0].type, nullptr));
            llvm::Value* v    = llvm::UndefValue::get(span);
            v = b.CreateInsertValue(v, array, 0);
            v = b.CreateInsertValue(v, b.CreateSExt(entry->getArg(0), llvm::Type::getInt64Ty(ctx_)), 1);
            main_in.push_back(v);
        }
        llvm::CallInst*     call   = b.CreateCall(edax_main_, main_in);
        b.CreateCall(mod_->getOrInsertFunction("free", llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {ptr}, false)), {array});
        const types::Type*  result = f->result ? tc_.lower(*f->result, sema::substitution_of(main)) : nullptr;
        if (result && result->is_integer()) {
            llvm::Value* wide = result->is_signed ? b.CreateSExt(call, llvm::Type::getInt128Ty(ctx_))
                                                  : b.CreateZExt(call, llvm::Type::getInt128Ty(ctx_));
            b.CreateRet(b.CreateCall(edax_exit_status_, {wide, loc_global(main->span)}));
        } else if (result && result->is(Kind::user)) {
            b.CreateRet(b.CreateZExt(call, i32));   // std::exit_code: success = 0, failure = 1
        } else {
            b.CreateRet(llvm::ConstantInt::get(i32, 0));
        }
    }

    // --- structs, enums and places ----------------------------------------------------------

    std::map<const Symbol*, llvm::Type*> user_types_;

    static bool is_struct_symbol(const Symbol* s) {
        return s->kind == SymbolKind::struct_ || s->kind == SymbolKind::class_;
    }

    // Methods take `self` (a pointer to the object) as their first parameter.
    static bool has_self(const Symbol* s) {
        const FunctionDecl* f = body_decl(s);
        return s->parent && f && !f->is_static;
    }

    static std::vector<const Symbol*> fields_of(const Symbol* s) {
        std::vector<const Symbol*> out;
        for (const Symbol* m : s->member_order) {
            if (m->kind == SymbolKind::field && !m->is_static) out.push_back(m);
        }
        return out;
    }

    // Every field of an object, the base's first (spec 07).
    static std::vector<const Symbol*> all_fields(const Symbol* s) {
        std::vector<const Symbol*> out;
        if (const Symbol* base = s->base_class()) out = all_fields(base);
        for (const Symbol* f : fields_of(s)) out.push_back(f);
        return out;
    }

    // --- dynamic dispatch (spec 07) -----------------------------------------------------------
    //
    // A class hierarchy whose root is `open` has a vtable pointer at offset 0. A class's vtable is
    // { parent vtable (null at the root), drop function, the virtual methods... }; the slots of the
    // methods are those of the base, then the ones the class introduces with `virtual`. An
    // interface reference is a fat pointer { object, table }: the table lists the implementations
    // of the interface's methods for the object's type (spec 07: the object's layout is unchanged).

    static constexpr unsigned vtable_header = 2;   // parent vtable, drop

    static bool has_vtable(const Symbol* s) {
        for (; s; s = s->base_class()) {
            if (s->has_vtable_pointer()) return true;
        }
        return false;
    }

    // Whether two methods have the same parameter types (an override of a slot).
    bool same_signature(const Symbol* a, const Symbol* b) {
        const FunctionDecl* fa = body_decl(a);
        const FunctionDecl* fb = body_decl(b);
        if (!fa || !fb || a->name != b->name || fa->params.size() != fb->params.size()) return false;
        for (std::size_t i = 0; i < fa->params.size(); ++i) {
            if (tc_.lower(*fa->params[i].type, sema::substitution_of(a)) != tc_.lower(*fb->params[i].type, sema::substitution_of(b))) {
                return false;
            }
        }
        return true;
    }

    static std::vector<const Symbol*> methods_of(const Symbol* s) {
        std::vector<const Symbol*> out;
        for (const Symbol* m : s->member_order) {
            if (m->kind == SymbolKind::method && m->function() && !m->function()->is_destructor) out.push_back(m);
        }
        return out;
    }

    // The virtual slots of a class: the methods that introduced them, root first.
    std::vector<const Symbol*> vtable_slots(const Symbol* s) {
        std::vector<const Symbol*> out;
        if (const Symbol* base = s->base_class()) out = vtable_slots(base);
        for (const Symbol* m : methods_of(s)) {
            const FunctionDecl* f = m->function();   // methods_of only returns methods with a declaration
            if (!f) continue;
            bool introduces = f->is_virtual || f->is_abstract;
            // In a class hierarchy with a vtable, implementing an interface method makes it
            // dispatched dynamically too: a derived class may override it again (spec 07).
            if (!introduces && f->is_override && has_vtable(s)) {
                introduces = true;
                for (const Symbol* slot : out) introduces = introduces && !same_signature(slot, m);
            }
            if (introduces) out.push_back(m);
        }
        // The methods of the interfaces it implements that it leaves to derived classes: they are
        // abstract here, and `this.sound()` in the class dispatches through the vtable.
        if (has_vtable(s)) {
            for (const Symbol* b : s->bases) {
                if (b->kind != SymbolKind::interface) continue;
                for (const Symbol* m : interface_methods(b)) {
                    bool covered = false;
                    for (const Symbol* slot : out) covered = covered || same_signature(slot, m);
                    if (!covered) out.push_back(m);
                }
            }
        }
        return out;
    }

    // The slot of `method` in the hierarchy of `s`, or -1.
    int vtable_slot(const Symbol* s, const Symbol* method) {
        std::vector<const Symbol*> slots = vtable_slots(s);
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (same_signature(slots[i], method)) return static_cast<int>(i);
        }
        return -1;
    }

    // The implementation of `method` (by signature) for objects of class `s`: the most derived one.
    const Symbol* implementation(const Symbol* s, const Symbol* method) {
        for (; s; s = s->base_class()) {
            for (const Symbol* m : methods_of(s)) {
                const FunctionDecl* f = body_decl(m);
                if (same_signature(m, method) && f && f->body) return m;
            }
        }
        return nullptr;
    }

    llvm::Constant* function_constant(const Symbol* m) {
        return m ? static_cast<llvm::Constant*>(function_of(m)) : llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0));
    }

    std::map<const Symbol*, llvm::GlobalVariable*> vtables_;

    // The vtable of class `s`.
    llvm::Constant* vtable_of(const Symbol* s) {
        if (auto it = vtables_.find(s); it != vtables_.end()) return it->second;
        std::vector<const Symbol*> slots = vtable_slots(s);
        auto* array_type = llvm::ArrayType::get(ptr_type(), vtable_header + slots.size());
        std::vector<std::string> parts = symbol_scope(s);
        parts.push_back(s->name);
        if (s->template_of) parts.push_back(instance_tag(s));
        auto* g = new llvm::GlobalVariable(*mod_, array_type, true, llvm::GlobalValue::InternalLinkage, nullptr,
                                           "rv_" + mangle_parts(parts));
        vtables_[s] = g;   // before the entries: a class's methods may need its own vtable
        if (from_shared_library(s)) {   // one vtable per class: the shared library's
            share_global(g, s);
            return g;
        }
        if (exported_by_library(s)) share_global(g, s);
        std::vector<llvm::Constant*> entries;
        const Symbol* base = s->base_class();
        entries.push_back(base ? vtable_of(base) : llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0)));
        entries.push_back(drop_function(tc_.user(s)));
        for (const Symbol* slot : slots) entries.push_back(function_constant(implementation(s, slot)));
        g->setInitializer(llvm::ConstantArray::get(array_type, entries));
        return g;
    }

    // --- devirtualization (spec 34 §8) --------------------------------------------------------
    // The whole program is one module, so the compiler sees every class and every implementation
    // of an interface. A call whose possible targets are one function is a direct call (it can be
    // inlined). Not in a plugin, nor in a program that loads plugins: their classes are unknown.

    std::vector<const Symbol*> all_types_;
    bool                       types_listed_ = false;
    bool                       open_world_   = false;

    void list_types() {
        if (types_listed_) return;
        types_listed_ = true;
        open_world_   = program_.is_plugin || program_.is_shared_library || !program_.shared_modules.empty();
        for (const auto& [name, m] : program_.modules) {
            for (const Symbol& sym : m->storage) {
                if (sym.kind != SymbolKind::class_ && sym.kind != SymbolKind::struct_ && sym.kind != SymbolKind::enum_) continue;
                if (sym.decl && sym.decl->is_template && !sym.template_of) continue;   // a template, not a type
                if (sym.template_of && sym.template_of->name == "plugin" && sym.template_of->ns == "std") open_world_ = true;
                all_types_.push_back(&sym);
            }
        }
    }

    // Whether `t` is `root` or derives from it (classes), or implements it (interfaces).
    static bool reaches(const Symbol* t, const Symbol* root) {
        if (t == root) return true;
        for (const Symbol* b : t->bases) {
            if (b && reaches(b, root)) return true;
        }
        return false;
    }

    std::map<std::pair<const Symbol*, const Symbol*>, const Symbol*> devirtualized_;

    // The one implementation of `method` that a call through `root` (a class or an interface) can
    // reach, or null when there are several or the program can't know.
    const Symbol* single_implementation(const Symbol* root, const Symbol* method) {
        if (!alias_attributes_) return nullptr;   // without optimization: as written
        auto key = std::make_pair(root, method);
        if (auto it = devirtualized_.find(key); it != devirtualized_.end()) return it->second;
        list_types();
        const Symbol* only = nullptr;
        bool          many = open_world_;
        for (const Symbol* t : all_types_) {
            if (many) break;
            if (!reaches(t, root)) continue;
            const Symbol* impl = implementation(t, method);
            if (!impl) continue;   // abstract here: no object of this exact class exists
            if (only && only != impl) many = true;
            only = impl;
        }
        const Symbol* out = many ? nullptr : only;
        devirtualized_[key] = out;
        return out;
    }

    // A virtual call: the function in `slot` of the object's vtable.
    llvm::Value* virtual_function(llvm::Value* object, int slot) {
        llvm::Value* vtable = b_.CreateLoad(ptr_type(), object);
        llvm::Value* entry  = b_.CreateConstGEP1_32(ptr_type(), vtable, vtable_header + static_cast<unsigned>(slot));
        return b_.CreateLoad(ptr_type(), entry);
    }

    // The methods of an interface, those of the interfaces it extends first.
    static std::vector<const Symbol*> interface_methods(const Symbol* i) {
        std::vector<const Symbol*> out;
        for (const Symbol* b : i->bases) {
            if (b->kind == SymbolKind::interface) {
                for (const Symbol* m : interface_methods(b)) out.push_back(m);
            }
        }
        for (const Symbol* m : methods_of(i)) out.push_back(m);
        return out;
    }

    int interface_slot(const Symbol* iface, const Symbol* method) {
        std::vector<const Symbol*> ms = interface_methods(iface);
        for (std::size_t i = 0; i < ms.size(); ++i) {
            if (same_signature(ms[i], method)) return static_cast<int>(i);
        }
        return -1;
    }

    // An interface table: { drop, type id, methods..., tables of the extended interfaces... }.
    static constexpr unsigned itable_header = 2;

    std::map<const Symbol*, llvm::GlobalVariable*> type_ids_;

    // A global whose address stands for type `s` (cast::dynamic from an interface).
    llvm::Constant* type_id(const Symbol* s) {
        if (auto it = type_ids_.find(s); it != type_ids_.end()) return it->second;
        std::vector<std::string> parts = symbol_scope(s);
        parts.push_back(s->name);
        if (s->template_of) parts.push_back(instance_tag(s));
        auto* g = new llvm::GlobalVariable(*mod_, llvm::Type::getInt8Ty(ctx_), true, llvm::GlobalValue::InternalLinkage,
                                           llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 0), "rt_" + mangle_parts(parts));
        type_ids_[s] = g;
        share_global(g, s);   // one type id per type across a shared library and its programs
        return g;
    }

    std::map<std::pair<const Symbol*, int>, llvm::Function*> thunks_;

    // An interface table entry for a class with a vtable: calls the object's own override.
    llvm::Function* virtual_thunk(const Symbol* impl, int slot) {
        auto key = std::make_pair(impl, slot);
        if (auto it = thunks_.find(key); it != thunks_.end()) return it->second;
        llvm::FunctionType* type = function_type(impl);
        auto* fn = llvm::Function::Create(type, llvm::Function::InternalLinkage,
                                          std::format("{}.dispatch{}", function_of(impl)->getName().str(), slot), mod_.get());
        thunks_[key] = fn;
        inherit_unit(fn, function_of(impl));
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", fn));
        llvm::Value* object = fn->getArg(0);
        llvm::Value* vtable = b.CreateLoad(ptr_type(), object);
        llvm::Value* target = b.CreateLoad(ptr_type(), b.CreateConstGEP1_32(ptr_type(), vtable, vtable_header + static_cast<unsigned>(slot)));
        std::vector<llvm::Value*> args;
        for (llvm::Argument& a : fn->args()) args.push_back(&a);
        llvm::CallInst* call = b.CreateCall(type, target, args);
        if (type->getReturnType()->isVoidTy()) b.CreateRetVoid();
        else b.CreateRet(call);
        return fn;
    }

    std::map<std::pair<const Symbol*, const Symbol*>, llvm::GlobalVariable*> itables_;

    // The table of interface `iface` for objects of type `s`.
    llvm::Constant* itable_of(const Symbol* s, const Symbol* iface) {
        auto key = std::make_pair(s, iface);
        if (auto it = itables_.find(key); it != itables_.end()) return it->second;
        std::vector<const Symbol*> ms = interface_methods(iface);
        std::vector<const Symbol*> extended = interface_bases(iface);
        auto* array_type = llvm::ArrayType::get(ptr_type(), itable_header + ms.size() + extended.size());
        std::vector<std::string> parts = symbol_scope(s);
        parts.push_back(s->name);
        if (s->template_of) parts.push_back(instance_tag(s));
        parts.push_back(iface->name);
        auto* g = new llvm::GlobalVariable(*mod_, array_type, true, llvm::GlobalValue::InternalLinkage, nullptr,
                                           "ri_" + mangle_parts(parts));
        itables_[key] = g;
        std::vector<llvm::Constant*> entries;
        entries.push_back(drop_function(tc_.user(s)));   // what a std::unique_ptr<iface> destroys with
        // The type, for cast::dynamic: null when the object has a vtable (that says its class).
        entries.push_back(has_vtable(s) ? static_cast<llvm::Constant*>(llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0)))
                                        : type_id(s));
        for (const Symbol* m : ms) {
            const Symbol* impl = implementation(s, m);
            if (!impl) unsupported(s->span, std::format("'{}' without an implementation of '{}'", s->name, m->name));
            int slot = has_vtable(s) ? vtable_slot(s, m) : -1;
            entries.push_back(slot >= 0 ? virtual_thunk(impl, slot) : function_constant(impl));
        }
        for (const Symbol* b : extended) entries.push_back(itable_of(s, b));
        g->setInitializer(llvm::ConstantArray::get(array_type, entries));
        return g;
    }

    static bool is_interface(const types::Type* t) { return t && t->is(Kind::user) && t->symbol->kind == SymbolKind::interface; }

    // The interfaces `i` extends directly.
    static std::vector<const Symbol*> interface_bases(const Symbol* i) {
        std::vector<const Symbol*> out;
        for (const Symbol* b : i->bases) {
            if (b->kind == SymbolKind::interface) out.push_back(b);
        }
        return out;
    }

    // The path of extended interfaces from `from` to `to` (empty when `to` is not one of them).
    static bool interface_path(const Symbol* from, const Symbol* to, std::vector<std::size_t>& path) {
        if (from == to) return true;
        std::vector<const Symbol*> bases = interface_bases(from);
        for (std::size_t k = 0; k < bases.size(); ++k) {
            path.push_back(k);
            if (interface_path(bases[k], to, path)) return true;
            path.pop_back();
        }
        return false;
    }

    // `{ object, table of from }` as `{ object, table of to }`: each extended interface's table
    // is stored after the methods (spec 07).
    llvm::Value* convert_interface(llvm::Value* fat, const Symbol* from, const Symbol* to, Span span) {
        if (from == to) return fat;
        std::vector<std::size_t> path;
        if (!interface_path(from, to, path)) unsupported(span, "converting between unrelated interfaces");
        llvm::Value* table = b_.CreateExtractValue(fat, 1);
        const Symbol* at = from;
        for (std::size_t k : path) {
            unsigned index = static_cast<unsigned>(itable_header + interface_methods(at).size() + k);
            table = b_.CreateLoad(ptr_type(), b_.CreateConstGEP1_32(ptr_type(), table, index));
            at = interface_bases(at)[k];
        }
        return b_.CreateInsertValue(fat, table, 1);
    }

    // The fat pointer `{ object, table }` of an expression whose type is an interface (or a
    // reference to one): what an interface place's "address" is.
    llvm::Value* interface_fat(Expr& e) {
        auto* fat = llvm::StructType::get(ctx_, {ptr_type(), ptr_type()});
        switch (e.kind) {
        case ExprKind::unary: {
            auto& u = e.as<UnaryExpr>();
            if (u.op == UnaryOp::mut_marker || u.op == UnaryOp::move) return interface_fat(*u.operand);
            if (u.op == UnaryOp::deref) {
                if (u.op_symbol) return call_symbol(u.op_symbol, object_address(*u.operand), {}, e.span, true);
                return gen_expr(*u.operand);   // a `std::raw_ptr<iface>` is the fat pointer itself
            }
            break;
        }
        case ExprKind::path: {
            const Symbol* sym = e.as<PathExpr>().symbol;
            auto it = sym ? locals_.find(sym) : locals_.end();
            if (it != locals_.end()) return b_.CreateLoad(fat, it->second);
            break;
        }
        case ExprKind::member: {
            auto& m = e.as<MemberExpr>();
            const Symbol* f = m.symbol;
            if (f && f->kind == SymbolKind::field && !f->is_static) {
                return b_.CreateLoad(fat, field_address(f, member_object(m)));
            }
            break;
        }
        case ExprKind::block: return gen_block(e.as<BlockExpr>(), true);
        default: return gen_expr(e);   // a call returning `iface&` returns the fat pointer
        }
        unsupported(e.span, "this interface reference");
    }

    // An interface reference `{ object, table }` for `e`, used as `iface&` (spec 07).
    llvm::Value* interface_reference(Expr& e, const Symbol* iface) {
        auto*              fat = llvm::StructType::get(ctx_, {ptr_type(), ptr_type()});
        const types::Type* t   = types::strip_ref(e.type);
        if (is_interface(t)) return convert_interface(interface_fat(e), t->symbol, iface, e.span);
        if (!t || !t->is(Kind::user)) unsupported(e.span, "this interface reference");
        llvm::Value* object = object_address(e);
        llvm::Value* v      = llvm::UndefValue::get(fat);
        v = b_.CreateInsertValue(v, object, 0);
        return b_.CreateInsertValue(v, itable_of(t->symbol, iface), 1);
    }

    // Whether the enum `e` is laid out as `{ tag, payload storage }`: some alternative declares a
    // payload (a `void` one too, so that `std::option<void>` has the layout of every option).
    static bool has_payloads(const Symbol* e) {
        for (const Symbol* a : e->member_order) {
            if (a->alternative && a->alternative->payload) return true;
        }
        return false;
    }

    // Whether the alternative `a` carries a value: it has a payload type, and that type is not
    // `void` (`std::ok()` of a `std::result<void, E>`, spec 08).
    bool carries(const Symbol* a) {
        if (!a || !a->alternative || !a->alternative->payload) return false;
        return !payload_of(a)->is(Kind::void_);
    }

    const types::Type* field_type(const Symbol* f) {
        return tc_.lower(*f->decl->as<VariableDecl>().type, sema::substitution_of(f));
    }

    // The address of field `f` in the object at `object` (a union's members are all at its start).
    llvm::Value* field_address(const Symbol* f, llvm::Value* object) {
        if (types::is_union(f->parent)) return object;
        return b_.CreateStructGEP(user_type(f->parent), object, field_index(f));
    }

    unsigned field_index(const Symbol* f) {
        std::vector<const Symbol*> fields = fields_of(f->parent);
        auto                       it     = std::find(fields.begin(), fields.end(), f);
        if (it == fields.end()) unsupported(f->span, "this field");
        return static_cast<unsigned>(it - fields.begin()) + f->parent->layout_prefix();
    }

    // The LLVM type of a struct, class or enum. Structs keep the C layout of their fields (in
    // declaration order); an empty struct takes no space (unlike C). An enum without payloads is its
    // tag; with payloads it is `{ tag, [n x iK] }`, the payload storage sized and aligned for the
    // largest payload on the host.
    llvm::Type* user_type(const Symbol* s) {
        if (s->decl && s->decl->is_template && !s->template_of) unsupported(s->span, "templates used without arguments");
        if (auto it = user_types_.find(s); it != user_types_.end()) return it->second;
        if (s->kind == SymbolKind::enum_) return enum_type(s);
        if (!is_struct_symbol(s)) unsupported(s->span, "values of this type");
        auto* st       = llvm::StructType::create(ctx_, s->name + "." + short_hash(symbol_key(s)));
        user_types_[s] = st;
        if (types::is_union(s)) {   // spec 34: storage for the largest member, aligned for the strictest
            types::Layout l    = tc_.layout(tc_.user(s));
            std::uint64_t unit = std::max<std::uint64_t>(l.align, 1);
            llvm::Type*   elem = unit <= 8 ? static_cast<llvm::Type*>(llvm::IntegerType::get(ctx_, static_cast<unsigned>(unit * 8)))
                                           : llvm::FixedVectorType::get(llvm::Type::getInt32Ty(ctx_), static_cast<unsigned>(unit / 4));
            st->setBody(llvm::ArrayRef<llvm::Type*>{llvm::ArrayType::get(elem, l.size / unit)});
            return st;
        }
        std::vector<llvm::Type*> elems;
        if (const Symbol* base = s->base_class()) elems.push_back(user_type(base));   // the base object first (spec 07)
        else if (s->has_vtable_pointer()) elems.push_back(ptr_type());               // the vtable
        for (const Symbol* f : fields_of(s)) {
            cur_ = f->span;
            elems.push_back(lower_type(field_type(f)));
        }
        if (std::uint64_t want = types::requested_alignment(s); want > 1 && !elems.empty()) {
            // @std::layout{ .align = N } (spec 34): a trailing zero-length array of an N-aligned
            // type raises the struct's alignment (and rounds its size) without moving any field.
            llvm::Type* unit = want <= 8 ? static_cast<llvm::Type*>(llvm::IntegerType::get(ctx_, static_cast<unsigned>(want * 8)))
                                         : llvm::FixedVectorType::get(llvm::Type::getInt32Ty(ctx_), static_cast<unsigned>(want / 4));
            elems.push_back(llvm::ArrayType::get(unit, 0));
        }
        bool packed = types::is_packed(s);   // @std::layout{ .packed } (spec 34): fields at byte offsets
        if (packed) packed_types_.insert(st);
        st->setBody(elems, packed);   // an empty struct takes no space: `std::global_allocator` in a container costs nothing
        return st;
    }

    llvm::Type* enum_tag_type(const Symbol* e) {
        return lower_type(tc_.lower(*e->decl->as<EnumDecl>().tag_type, sema::substitution_of(e)));
    }

    llvm::Type* enum_type(const Symbol* e) {
        llvm::Type* tag = enum_tag_type(e);
        if (!has_payloads(e)) {
            user_types_[e] = tag;
            return tag;
        }
        const llvm::DataLayout& layout = mod_->getDataLayout();
        std::uint64_t           size   = 0;
        unsigned                align  = 1;
        for (const Symbol* a : e->member_order) {
            if (!carries(a)) continue;
            cur_ = a->span;
            llvm::Type* p = lower_type(tc_.lower(*a->alternative->payload, sema::substitution_of(a)));
            size          = std::max<std::uint64_t>(size, layout.getTypeAllocSize(p).getFixedValue());
            align         = std::max(align, static_cast<unsigned>(layout.getABITypeAlign(p).value()));
        }
        auto* st       = llvm::StructType::create(ctx_, e->name + "." + short_hash(symbol_key(e)));
        user_types_[e] = st;
        llvm::Type* elem = llvm::IntegerType::get(ctx_, align * 8);
        if (tc_.niche_enum(e)) st->setBody(llvm::ArrayRef<llvm::Type*>{llvm::ArrayType::get(elem, (size + align - 1) / align)});   // no tag (spec 34)
        else st->setBody({tag, llvm::ArrayType::get(elem, (size + align - 1) / align)});
        return st;
    }

    // --- tags and payloads (an enum with a niche has no tag field, spec 34) ---------------

    // The index of the payload storage in the enum's struct.
    unsigned payload_index(const Symbol* e) { return tc_.niche_enum(e) ? 0 : 1; }

    // The address of the payload of the enum at `addr`.
    llvm::Value* payload_address(const Symbol* e, llvm::Value* addr) { return b_.CreateStructGEP(user_type(e), addr, payload_index(e)); }

    // The address and type of the niche of the enum at `addr`.
    std::pair<llvm::Value*, llvm::Type*> niche_address(const types::NicheEnum& n, llvm::Value* addr) {
        llvm::Value* at = b_.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(ctx_), addr, n.niche.offset);
        return {at, llvm::IntegerType::get(ctx_, static_cast<unsigned>(n.niche.size * 8))};
    }

    // The tag of the enum at `addr`.
    llvm::Value* load_tag(const Symbol* e, llvm::Value* addr) {
        if (!has_payloads(e)) return b_.CreateLoad(enum_tag_type(e), addr);
        if (std::optional<types::NicheEnum> n = tc_.niche_enum(e)) {
            auto [at, it] = niche_address(*n, addr);
            llvm::Value* set = b_.CreateICmpNE(b_.CreateLoad(it, at), llvm::ConstantInt::get(it, n->niche.value));
            return b_.CreateSelect(set, tag_constant(n->with), tag_constant(n->without));
        }
        return b_.CreateLoad(enum_tag_type(e), b_.CreateStructGEP(user_type(e), addr, 0));
    }

    // Stores the tag of the enum at `addr`. With a niche, the payload is stored first: `with`
    // leaves it as it is, `without` writes the niche's value.
    void store_tag(const Symbol* e, llvm::Value* addr, llvm::Value* tag) {
        if (std::optional<types::NicheEnum> n = tc_.niche_enum(e)) {
            auto [at, it] = niche_address(*n, addr);
            llvm::Value* zero = llvm::ConstantInt::get(it, n->niche.value);   // the niche's value
            if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(tag)) {
                if (c == tag_constant(n->without)) b_.CreateStore(zero, at);
                return;
            }
            b_.CreateStore(b_.CreateSelect(b_.CreateICmpEQ(tag, tag_constant(n->without)), zero, b_.CreateLoad(it, at)), at);
            return;
        }
        b_.CreateStore(tag, b_.CreateStructGEP(user_type(e), addr, 0));
    }

    // Tag value of each alternative (explicit values, otherwise previous + 1).
    std::map<const Symbol*, u128> tag_values(const Symbol* e) {
        std::map<const Symbol*, u128> out;
        u128                          next = 0;
        for (const Symbol* a : e->member_order) {
            if (!a->alternative) continue;
            if (const Expr* v = a->alternative->value.get()) {
                bool negative = false;
                if (v->kind == ExprKind::unary && v->as<UnaryExpr>().op == UnaryOp::negate) {
                    negative = true;
                    v        = v->as<UnaryExpr>().operand.get();
                }
                if (v->kind != ExprKind::int_literal) unsupported(v->span, "enum tags that are not integer literals");
                u128 m = v->as<LiteralExpr>().int_value;
                next   = negative ? u128{0} - m : m;
            }
            out[a] = next;
            next   = next + 1;
        }
        return out;
    }

    static llvm::APInt wide(u128 v, unsigned bits) {
        std::uint64_t words[2] = {static_cast<std::uint64_t>(v), static_cast<std::uint64_t>(v >> 64)};
        return llvm::APInt(128, words).zextOrTrunc(bits);
    }

    llvm::ConstantInt* tag_constant(const Symbol* alt) {
        const Symbol* e    = alt->parent;
        auto          bits = llvm::cast<llvm::IntegerType>(enum_tag_type(e))->getBitWidth();
        return llvm::ConstantInt::get(ctx_, wide(tag_values(e)[alt], bits));
    }

    // The value of an alternative: its tag, or the enum with `payload` stored in it.
    llvm::Value* alternative_value(const Symbol* alt, llvm::Value* payload) {
        const Symbol* e   = alt->parent;
        llvm::Value*  tag = tag_constant(alt);
        if (!has_payloads(e)) return tag;
        llvm::Type*       et   = user_type(e);
        llvm::AllocaInst* slot = entry_alloca(et, "alternative");
        if (payload) b_.CreateStore(payload, payload_address(e, slot));
        store_tag(e, slot, tag);
        return b_.CreateLoad(et, slot);
    }

    // A struct value from the fields given in a literal; the others take their declared
    // initializer, or the default of their type.
    llvm::Value* build_struct(const Symbol* s, const std::map<std::string, llvm::Value*>& given) {
        llvm::Type*       st   = user_type(s);
        llvm::AllocaInst* slot = entry_alloca(st, "value");
        if (types::is_union(s)) b_.CreateStore(llvm::Constant::getNullValue(st), slot);   // the bytes past the member are zero
        for (const Symbol* f : all_fields(s)) {   // the base's fields too: `circle{ .name = ..., .radius = ... }`
            cur_ = f->span;
            llvm::Value* v = nullptr;
            if (auto it = given.find(f->name); it != given.end()) {
                v = it->second;
            } else {
                const auto& d = f->decl->as<VariableDecl>();
                v             = d.init ? gen_expr(*d.init) : default_value(field_type(f), f->span);
            }
            if (types::is_union(s) && !given.contains(f->name)) continue;   // a union: the one member the literal sets
            b_.CreateStore(v, field_address(f, slot));   // a base is at offset 0
        }
        if (has_vtable(s)) b_.CreateStore(vtable_of(s), slot);   // the vtable pointer is at offset 0
        return b_.CreateLoad(st, slot);
    }

    llvm::Value* struct_literal(StructLiteralExpr& sl) {
        const types::Type* t = types::strip_ref(static_cast<Expr&>(sl).type);
        if (!t || !t->is(Kind::user) || !is_struct_symbol(t->symbol)) {
            unsupported(sl.span, std::format("building values of type '{}'", t ? types::to_string(t) : "unknown"));
        }
        // The given fields are evaluated in the order written, then the rest.
        std::map<std::string, llvm::Value*> given;
        for (FieldInit& fi : sl.fields) {
            const Symbol* f = nullptr;
            for (const Symbol* candidate : all_fields(t->symbol)) {
                if (candidate->name == fi.name) f = candidate;
            }
            const types::Type* ft = f ? field_type(f) : nullptr;
            if (ft && ft->is(Kind::reference)) {   // a reference field binds to the place (spec 12)
                given[fi.name] = is_interface(ft->inner) ? interface_reference(*fi.value, ft->inner->symbol) : object_address(*fi.value);
                continue;
            }
            given[fi.name] = gen_expr(*fi.value);
            moved_out(*fi.value);
        }
        return build_struct(t->symbol, given);
    }

    llvm::Value* default_value(const types::Type* t, Span where) {
        if (t && t->is(Kind::user) && is_struct_symbol(t->symbol)) return build_struct(t->symbol, {});
        if (types::is_std_instance(t, "option")) return alternative_value(alternative(t->symbol, "none"), nullptr);
        if (t && t->is(Kind::array)) {   // every element's default
            llvm::Value* element = default_value(t->inner, where);
            if (auto* c = llvm::dyn_cast<llvm::Constant>(element)) {
                return llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(lower_type(t)),
                                                std::vector<llvm::Constant*>(tc_.array_count(t), c));
            }
            llvm::Value* v = llvm::UndefValue::get(lower_type(t));
            for (std::uint64_t i = 0; i < tc_.array_count(t); ++i) {
                v = b_.CreateInsertValue(v, i == 0 ? element : default_value(t->inner, where), {static_cast<unsigned>(i)});
            }
            return v;
        }
        if (t && (t->is_integer() || t->is_float() || t->is(Kind::bool_) || t->is(Kind::char_) || t->is(Kind::pointer) ||
                  t->is(Kind::vector) || t->is(Kind::mask))) {
            return llvm::Constant::getNullValue(lower_type(t));
        }
        unsupported(where, std::format("'default' for '{}'", t ? types::to_string(t) : "unknown"));
    }

    bool is_place(const Expr& e) const {
        switch (e.kind) {
        case ExprKind::path: {
            const Symbol* s = e.as<PathExpr>().symbol;
            // A compile_time local has no storage: its value is materialized where it is used.
            if (s && s->kind == SymbolKind::local && e.constant && !locals_.count(s)) return false;
            return s && (s->kind == SymbolKind::local || s->kind == SymbolKind::global);
        }
        case ExprKind::this_:
        case ExprKind::member: return true;
        case ExprKind::index: return e.as<IndexExpr>().op_symbol != nullptr;
        case ExprKind::unary: return e.as<UnaryExpr>().op == UnaryOp::deref;
        case ExprKind::call: {   // `f.get(value)` (spec 26), or a call that returns a reference
            if (e.type && e.type->is(Kind::reference) && !is_interface(e.type->inner)) return true;
            const auto& c = e.as<CallExpr>();
            if (c.callee->kind != ExprKind::member) return false;
            const auto& object = *c.callee->as<MemberExpr>().object;
            return object.constant && object.constant->kind == ct::Value::Kind::field;
        }
        default: return false;
        }
    }

    // The address of the object a place denotes. A reference (or `this`) denotes the object it
    // refers to, so its address is the pointer it holds.
    llvm::Value* place(Expr& e) {
        switch (e.kind) {
        case ExprKind::path: {
            const Symbol* s = e.as<PathExpr>().symbol;
            if (s && s->kind == SymbolKind::global) return global_of(s);
            if (!s || s->kind != SymbolKind::local) unsupported(e.span, "this place");
            auto it = locals_.find(s);
            if (it == locals_.end()) unsupported(e.span, "this name");
            if (s->type && s->type->is(Kind::reference)) return b_.CreateLoad(llvm::PointerType::get(ctx_, 0), it->second);
            return it->second;
        }
        case ExprKind::this_:
            if (!self_) unsupported(e.span, "'this' outside a method");
            return self_;
        case ExprKind::member: {
            auto&         m = e.as<MemberExpr>();
            if (const types::Type* tt = types::strip_ref(m.object->type); tt && tt->is(Kind::tuple)) {   // `t.0`
                unsigned i = static_cast<unsigned>(std::stoul(m.name.segments.back().name));
                return b_.CreateStructGEP(lower_type(tt), member_object(m), i);
            }
            const Symbol* f = m.symbol;
            if (!f || f->kind != SymbolKind::field || f->is_static) unsupported(e.span, "this member access");
            llvm::Value* at = field_address(f, member_object(m));
            const types::Type* ft = field_type(f);
            if (ft && ft->is(Kind::reference)) return b_.CreateLoad(ptr_type(), at);   // a reference field holds the address
            return at;
        }
        case ExprKind::unary: {
            auto& u = e.as<UnaryExpr>();
            if (u.op != UnaryOp::deref) break;
            if (u.op_symbol) return call_symbol(u.op_symbol, object_address(*u.operand), {}, e.span, true);   // `*p`: operator*
            return gen_expr(*u.operand);
        }
        case ExprKind::index: {   // `operator[]` returns a reference: the element's address (spec 16)
            auto& i = e.as<IndexExpr>();
            if (!i.op_symbol) break;
            return call_symbol(i.op_symbol, object_address(*i.object), {i.index.get()}, e.span, true);
        }
        case ExprKind::call: {   // `f.get(value)`: the field (spec 26)
            auto& c = e.as<CallExpr>();
            if (c.callee->kind == ExprKind::member) {
                Expr& object = *c.callee->as<MemberExpr>().object;
                if (object.constant && object.constant->kind == ct::Value::Kind::field && c.args.size() == 1) {
                    return field_address(*object.constant, *c.args[0]);
                }
            }
            // A call returning a reference (`m.get_or_insert(k, 0)`): the address it returned. The
            // call is generated reading through it; that read is dropped again.
            if (e.type && e.type->is(Kind::reference) && !is_interface(e.type->inner)) {
                llvm::Value* v = gen_expr(e);
                if (auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(v); load && load->use_empty()) {
                    llvm::Value* address = load->getPointerOperand();
                    load->eraseFromParent();
                    return address;
                }
            }
            break;
        }
        default: break;
        }
        unsupported(e.span, "this place");
    }

    // The object a member access reads from: `p->x` on a smart pointer goes through its `operator*`.
    llvm::Value* member_object(MemberExpr& m) {
        if (m.deref_symbol) return call_symbol(m.deref_symbol, object_address(*m.object), {}, m.span, true);
        return object_address(*m.object);
    }

    // The address of the object `e` denotes: a place, a pointer or reference value, or a temporary.
    // The address a returned `T&` refers to. object_address reads a raw pointer's value (the
    // object it points to); a reference to a place that *holds* a raw pointer (`*p` of a
    // `std::raw_ptr<std::raw_ptr<T>>`, an element of a darray of pointers) is that place's address.
    llvm::Value* reference_to(Expr& e) {
        if (e.kind == ExprKind::unary) {   // `f(mut p)` with a pointer `p`: the place, not the pointer
            UnaryOp op = e.as<UnaryExpr>().op;
            if (op == UnaryOp::mut_marker || op == UnaryOp::move) return reference_to(*e.as<UnaryExpr>().operand);
        }
        if (e.type && e.type->is(Kind::pointer) && is_place(e)) return place(e);
        return object_address(e);
    }

    llvm::Value* object_address(Expr& e) {
        if (e.kind == ExprKind::unary) {
            UnaryOp op = e.as<UnaryExpr>().op;
            if (op == UnaryOp::mut_marker || op == UnaryOp::move) return object_address(*e.as<UnaryExpr>().operand);
        }
        if (e.type && e.type->is(Kind::pointer)) return gen_expr(e);
        if (is_interface(types::strip_ref(e.type))) return interface_fat(e);
        if (e.kind == ExprKind::block) return gen_block(e.as<BlockExpr>(), true);   // `unsafe { *p }`
        bool reflected = e.constant && e.kind == ExprKind::member && !e.as<MemberExpr>().symbol;   // `p.passing` (spec 26)
        if (!reflected && e.type && e.type->is(Kind::reference)) return place(e);
        if (!reflected && is_place(e)) return place(e);
        // `return match (w) { a => this.first, b => this.second };` from a function returning a
        // reference: the address of the place the taken branch names, not a copy of its value.
        if (e.kind == ExprKind::match && places_only(e)) {
            ++cond_depth_;
            llvm::Value* v = match_value(e.as<MatchExpr>(), e.type, true);
            --cond_depth_;
            return v;
        }
        if (e.kind == ExprKind::if_ && !e.as<IfExpr>().is_compile_time && places_only(e)) {
            ++cond_depth_;
            llvm::Value* v = if_value(e.as<IfExpr>(), e.type, true);
            --cond_depth_;
            return v;
        }
        llvm::Value*      v    = gen_expr(e);
        llvm::AllocaInst* slot = entry_alloca(v->getType(), "temporary");
        b_.CreateStore(v, slot);
        // A temporary that owns something lives until the end of the enclosing block.
        const types::Type* t = types::strip_ref(e.type);
        if (needs_drop(t) && !scopes_.empty() && !owned_path(e)) {
            llvm::AllocaInst* flag = entry_alloca(llvm::Type::getInt1Ty(ctx_), "df_temporary");
            b_.CreateStore(llvm::ConstantInt::getTrue(ctx_), flag);
            scopes_.back().locals.push_back(OwnedLocal{slot, flag, drop_function(t)});
            temporaries_[slot] = flag;
        }
        return slot;
    }

    // Whether every value `e` can produce is a place (or doesn't produce one: `return`, a panic):
    // a match or an if whose branches name variables, fields or elements.
    bool places_only(Expr& e) {
        if (e.type && e.type->is(Kind::never)) return true;
        if (is_place(e)) return true;
        if (e.kind == ExprKind::block) {
            BlockExpr& b = e.as<BlockExpr>();
            return b.statements.empty() && b.tail && places_only(*b.tail);
        }
        if (e.kind == ExprKind::match) {
            for (MatchArm& arm : e.as<MatchExpr>().arms) {
                if (!arm.body || !places_only(*arm.body)) return false;
            }
            return true;
        }
        if (e.kind == ExprKind::if_) {
            IfExpr& i = e.as<IfExpr>();
            if (i.is_compile_time || !i.else_branch || !i.then_block || !i.then_block->tail) return false;
            return places_only(*i.then_block->tail) && places_only(*i.else_branch);
        }
        return false;
    }

    // The drop flags of owned temporaries, by their slot: whoever takes a temporary over (a
    // match, a loop) clears its flag.
    std::map<const llvm::Value*, llvm::AllocaInst*> temporaries_;

    // Hands the temporary at `slot` (if it is one) to a new owner.
    void release_temporary(const llvm::Value* slot) {
        if (auto it = temporaries_.find(slot); it != temporaries_.end()) b_.CreateStore(llvm::ConstantInt::getFalse(ctx_), it->second);
    }

    // The value of a place expression (a reference is read through).
    llvm::Value* value_of_place(Expr& e) {
        return b_.CreateLoad(lower_type(types::strip_ref(e.type)), place(e));
    }

    struct Loop {
        llvm::BasicBlock* exit;
        llvm::BasicBlock* cont;
    };

    // --- strings, output and formatting (spec 19) ------------------------------------------

    llvm::Type* ptr_type() { return llvm::PointerType::get(ctx_, 0); }

    // A `str` value: the bytes and their length, `{ ptr, i64 }`.
    llvm::StructType* str_type() { return llvm::StructType::get(ctx_, {ptr_type(), llvm::Type::getInt64Ty(ctx_)}); }

    // The runtime's output buffer, `{ ptr, i64, i64 }`.
    llvm::StructType* buffer_type() {
        return llvm::StructType::get(ctx_, {ptr_type(), llvm::Type::getInt64Ty(ctx_), llvm::Type::getInt64Ty(ctx_)});
    }

    static bool is_str(const types::Type* t) {
        return t && (t->is(Kind::static_str) || (t->is(Kind::external) && t->name == "std::str"));
    }

    // A `str` constant with the bytes of `text`.
    llvm::Constant* str_constant(const std::string& text) {
        llvm::Constant* bytes = message_bytes(text);
        return llvm::ConstantStruct::get(str_type(), {bytes, llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), text.size())});
    }

    // A function of the runtime (llvm_runtime.cpp), declared here and defined in the appended IR.
    llvm::Function* runtime(const char* name, llvm::Type* result, std::vector<llvm::Type*> params) {
        if (llvm::Function* f = mod_->getFunction(name)) return f;
        llvm::Function* f = llvm::Function::Create(llvm::FunctionType::get(result, params, false), llvm::Function::ExternalLinkage, name, mod_.get());
        if (std::string_view(name).starts_with("edax_panic")) {   // panics are rare; they return only to stop a debugger
            if (std::string_view(name) == "edax_panic_end") f->setDoesNotReturn();
            f->addFnAttr(llvm::Attribute::Cold);
        }
        return f;
    }

    // The `edax_spec` of a format hole: `{ align, plus, width, precision, radix }` (spec 19).
    llvm::Constant* spec_constant(char align, bool plus, int width, int precision, char radix) {
        auto* i8  = llvm::Type::getInt8Ty(ctx_);
        auto* i32 = llvm::Type::getInt32Ty(ctx_);
        llvm::Constant* init = llvm::ConstantStruct::getAnon(
            ctx_, {llvm::ConstantInt::get(i8, static_cast<std::uint8_t>(align)), llvm::ConstantInt::getBool(ctx_, plus),
                   llvm::ConstantInt::getSigned(i32, width), llvm::ConstantInt::getSigned(i32, precision),
                   llvm::ConstantInt::get(i8, static_cast<std::uint8_t>(radix))});
        return new llvm::GlobalVariable(*mod_, init->getType(), true, llvm::GlobalValue::PrivateLinkage, init, ".spec");
    }

    llvm::Constant* plain_spec() { return spec_constant(0, false, -1, -1, 0); }

    // The edax_stream for stdout (`err` false) or stderr.
    llvm::Value* stream(bool err) {
        return b_.CreateCall(runtime("edax_stream", ptr_type(), {llvm::Type::getInt1Ty(ctx_)}),
                             {llvm::ConstantInt::getBool(ctx_, err)});
    }

    static std::string std_name(const CallExpr& c) {
        if (c.callee->kind != ExprKind::path) return {};
        const auto& pe = c.callee->as<PathExpr>();
        if (pe.symbol || pe.path.segments.size() != 2 || pe.path.segments[0].name != "std") return {};
        return pe.path.segments[1].name;
    }

    static bool is_std_output(const CallExpr& c) {
        std::string n = std_name(c);
        return n == "print" || n == "println" || n == "eprint" || n == "eprintln" || n == "panic";
    }

    // std::print / println / eprint / eprintln / panic. Output is written straight to the stream;
    // panics print the message with the location of the call (spec 11).
    void std_output(CallExpr& c) {
        std::string name    = std_name(c);
        bool        panic   = name == "panic";
        bool        newline = name.ends_with("ln");
        bool        err     = name.starts_with("e") || panic;
        if (c.args.size() != 1) unsupported(c.span, std::format("'std::{}' with {} arguments", name, c.args.size()));
        Expr& arg = *c.args[0];
        if (panic) {
            debug_at(c.span);   // a debugger stops at the call of std::panic
            if (options_.panic_trap) {   // panic() == trap: no message (spec 24)
                b_.CreateCall(intrinsic_declaration(mod_.get(), llvm::Intrinsic::trap, {}));
                b_.CreateUnreachable();
                open_dead_block();
                return;
            }
        }
        auto* i64 = llvm::Type::getInt64Ty(ctx_);
        auto* i1  = llvm::Type::getInt1Ty(ctx_);
        auto* loc = llvm::PointerType::get(ctx_, 0);
        const types::Type* at = types::strip_ref(arg.type);
        if ((arg.kind == ExprKind::string_literal && arg.as<LiteralExpr>().token_kind == TokenKind::format_string_literal) ||
            (at && at->is(Kind::external) && at->name == "std::format_args")) {
            if (!panic) record_format(callee_name(c));
            llvm::AllocaInst* buf = text_into_buffer(arg);
            if (panic) {
                b_.CreateCall(runtime("edax_panic_buf", llvm::Type::getVoidTy(ctx_), {loc, loc}), {loc_global(c.span), buf});
                end_panic();
                open_dead_block();
            } else {
                b_.CreateCall(runtime("edax_flush", llvm::Type::getVoidTy(ctx_), {loc, loc, i1}),
                              {stream(err), buf, llvm::ConstantInt::getBool(ctx_, newline)});
            }
            return;
        }
        const types::Type* t = types::strip_ref(arg.type);
        if (!is_str(t) && !is_string(t)) {
            unsupported(arg.span, std::format("'std::{}' with a value of type '{}'", name, t ? types::to_string(t) : "unknown"));
        }
        llvm::Value* v = text_value(arg);
        llvm::Value* data = b_.CreateExtractValue(v, 0);
        llvm::Value* len  = b_.CreateExtractValue(v, 1);
        if (panic) {
            b_.CreateCall(runtime("edax_panic_str", llvm::Type::getVoidTy(ctx_), {loc, loc, i64}), {loc_global(c.span), data, len});
            end_panic();
            open_dead_block();
        } else {
            b_.CreateCall(runtime("edax_write_str", llvm::Type::getVoidTy(ctx_), {loc, loc, i64, i1}),
                          {stream(err), data, len, llvm::ConstantInt::getBool(ctx_, newline)});
        }
    }

    // `std::format_args` (spec 19): a view of an f-string that is not formatted yet: a function
    // that writes the text into a buffer, and the addresses of the holes' values it reads.
    llvm::StructType* format_args_type() {
        if (!format_args_type_) format_args_type_ = llvm::StructType::create(ctx_, {ptr_type(), ptr_type()}, "edax.format_args");
        return format_args_type_;
    }
    llvm::StructType* format_args_type_ = nullptr;

    // An f-string used as a value (spec 19, 31): a function that formats it, and the places its
    // holes read. The holes are evaluated only when (and if) the receiver formats it, on the
    // calling thread while the call is running, so every local they read is still alive.
    llvm::Value* format_args_value(LiteralExpr& lit) {
        // What the holes read from the enclosing function: locals (their storage) and `this`.
        std::vector<const Symbol*> captured;
        std::set<const Symbol*>    seen;
        bool                       uses_self = false;
        auto capture = [&](const Symbol* s) {
            if (s && locals_.count(s) && seen.insert(s).second) captured.push_back(s);
        };
        for (FormatPart& part : lit.format_parts) {
            if (!part.expr) continue;
            ast::walk_exprs(*part.expr, [&](Expr& x) {
                if (x.kind == ExprKind::path) capture(x.as<PathExpr>().symbol);
                if (x.kind == ExprKind::this_) uses_self = true;
                if (x.kind == ExprKind::member && x.as<MemberExpr>().symbol && !x.as<MemberExpr>().object) uses_self = true;
                if (x.kind == ExprKind::lambda) {
                    for (const Capture& c : x.as<LambdaExpr>().captures) capture(c.captured);
                }
                return true;
            });
        }
        if (self_) uses_self = true;   // `this.x` in a hole reaches the object through self_
        std::vector<llvm::Type*> fields(captured.size() + 1, ptr_type());
        llvm::StructType*        env_t = llvm::StructType::get(ctx_, fields);
        llvm::AllocaInst*        env   = entry_alloca(env_t, "format_env");
        for (std::size_t i = 0; i < captured.size(); ++i) {
            b_.CreateStore(locals_.at(captured[i]), b_.CreateStructGEP(env_t, env, static_cast<unsigned>(i)));
        }
        b_.CreateStore(uses_self && self_ ? self_ : static_cast<llvm::Value*>(llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0))),
                       b_.CreateStructGEP(env_t, env, static_cast<unsigned>(captured.size())));
        auto* vd = llvm::Type::getVoidTy(ctx_);
        auto* fn = llvm::Function::Create(llvm::FunctionType::get(vd, {ptr_type(), ptr_type()}, false), llvm::Function::InternalLinkage,
                                          local_helper_name("edax_format_", lit.span), mod_.get());
        fn->addFnAttr(llvm::Attribute::NoUnwind);
        inherit_unit(fn, cur_fn_);
        llvm::IRBuilderBase::InsertPoint saved_ip     = b_.saveIP();
        llvm::DebugLoc                   saved_loc    = b_.getCurrentDebugLocation();
        llvm::Function*                  saved_fn     = cur_fn_;
        llvm::Value*                     saved_self   = self_;
        auto                             saved_locals = locals_;
        auto                             saved_scopes = scopes_;
        auto                             saved_loops  = loops_;
        auto                             saved_owned  = owned_;
        const types::Type*               saved_result = result_;
        fn_names_[fn] = function_name(saved_fn);   // an f-string's helper: panics name the function it is in
        cur_fn_ = fn;
        b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn));
        b_.SetCurrentDebugLocation(llvm::DebugLoc());
        if (dib_ && saved_fn->getSubprogram()) debug_function(fn, lit.span, "f-string");
        locals_.clear();
        scopes_.clear();
        loops_.clear();
        owned_.clear();
        llvm::Value* in = fn->getArg(1);
        for (std::size_t i = 0; i < captured.size(); ++i) {
            locals_[captured[i]] = b_.CreateLoad(ptr_type(), b_.CreateStructGEP(env_t, in, static_cast<unsigned>(i)));
        }
        self_ = saved_self ? b_.CreateLoad(ptr_type(), b_.CreateStructGEP(env_t, in, static_cast<unsigned>(captured.size()))) : nullptr;
        open_scope();
        llvm::Value* buf = fn->getArg(0);
        auto*        i64 = llvm::Type::getInt64Ty(ctx_);
        for (FormatPart& part : lit.format_parts) {
            if (part.expr) {
                format_hole(buf, part);
            } else {
                b_.CreateCall(runtime("edax_buf_put", vd, {ptr_type(), ptr_type(), i64}),
                              {buf, message_bytes(part.text), llvm::ConstantInt::get(i64, part.text.size())});
            }
        }
        close_scope();
        if (!terminated()) b_.CreateRetVoid();
        cur_fn_ = saved_fn;
        self_   = saved_self;
        locals_ = saved_locals;
        scopes_ = saved_scopes;
        loops_  = saved_loops;
        owned_  = saved_owned;
        result_ = saved_result;
        b_.restoreIP(saved_ip);
        b_.SetCurrentDebugLocation(saved_loc);
        llvm::Value* v = llvm::UndefValue::get(format_args_type());
        v              = b_.CreateInsertValue(v, fn, 0);
        return b_.CreateInsertValue(v, env, 1);
    }

    // The text of an f-string or a `std::format_args` value, in a new runtime buffer.
    llvm::AllocaInst* text_into_buffer(Expr& e) {
        if (e.kind == ExprKind::string_literal && e.as<LiteralExpr>().token_kind == TokenKind::format_string_literal) {
            return format_into_buffer(e.as<LiteralExpr>());
        }
        llvm::Value*      args = gen_expr(e);
        llvm::AllocaInst* buf  = entry_alloca(buffer_type(), "buffer");
        b_.CreateStore(llvm::ConstantAggregateZero::get(buffer_type()), buf);
        auto* vd = llvm::Type::getVoidTy(ctx_);
        b_.CreateCall(llvm::FunctionType::get(vd, {ptr_type(), ptr_type()}, false), b_.CreateExtractValue(args, 0),
                      {buf, b_.CreateExtractValue(args, 1)});
        return buf;
    }

    // The runtime buffer with the text of the f-string `lit`. Its bytes are on the heap.
    llvm::AllocaInst* format_into_buffer(LiteralExpr& lit) {
        auto*             i64 = llvm::Type::getInt64Ty(ctx_);
        auto*             p   = ptr_type();
        llvm::AllocaInst* buf = entry_alloca(buffer_type(), "buffer");
        b_.CreateStore(llvm::ConstantAggregateZero::get(buffer_type()), buf);
        for (FormatPart& part : lit.format_parts) {
            if (part.expr) {
                format_hole(buf, part);
            } else {
                b_.CreateCall(runtime("edax_buf_put", llvm::Type::getVoidTy(ctx_), {p, p, i64}),
                              {buf, message_bytes(part.text), llvm::ConstantInt::get(i64, part.text.size())});
            }
        }
        return buf;
    }

    // `std::trailing_zeros` and friends: LLVM's bit intrinsics (cttz/ctlz defined for 0).
    llvm::Value* bit_builtin(CallExpr& c, const std::string& name) {
        llvm::Value* x  = gen_expr(*c.args.at(0));
        auto*        u32 = llvm::Type::getInt32Ty(ctx_);
        if (name == "rotate_left" || name == "rotate_right") {
            llvm::Value* n = b_.CreateZExtOrTrunc(gen_expr(*c.args.at(1)), x->getType());
            return b_.CreateIntrinsic(name == "rotate_left" ? llvm::Intrinsic::fshl : llvm::Intrinsic::fshr, {x->getType()}, {x, x, n});
        }
        llvm::Value* r = nullptr;
        if (name == "count_ones") {
            r = b_.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, x);
        } else {
            r = b_.CreateIntrinsic(name == "trailing_zeros" ? llvm::Intrinsic::cttz : llvm::Intrinsic::ctlz, {x->getType()},
                                   {x, llvm::ConstantInt::getFalse(ctx_)});
        }
        return b_.CreateZExtOrTrunc(r, u32);
    }

    // `std::checked_add` and `std::saturating_add` and their sub/mul forms (spec 16).
    llvm::Value* arith_builtin(CallExpr& c, const std::string& name) {
        const types::Type* t = types::strip_ref(c.args.at(0)->type);
        if (!t || !t->is_integer()) unsupported(c.span, std::format("'std::{}' on this type", name));
        llvm::Value* a  = gen_expr(*c.args[0]);
        llvm::Value* b  = gen_expr(*c.args[1]);
        bool         s  = t->is_signed;
        std::string  op = name.substr(name.find('_') + 1);
        if (name.starts_with("saturating")) {
            if (op == "add") return b_.CreateBinaryIntrinsic(s ? llvm::Intrinsic::sadd_sat : llvm::Intrinsic::uadd_sat, a, b);
            if (op == "sub") return b_.CreateBinaryIntrinsic(s ? llvm::Intrinsic::ssub_sat : llvm::Intrinsic::usub_sat, a, b);
            // mul: the exact product, or the bound on the side of its sign
            llvm::Value* r   = b_.CreateBinaryIntrinsic(s ? llvm::Intrinsic::smul_with_overflow : llvm::Intrinsic::umul_with_overflow, a, b);
            llvm::Value* ovf = b_.CreateExtractValue(r, 1);
            unsigned     w   = a->getType()->getIntegerBitWidth();
            llvm::Value* bound;
            if (s) {
                llvm::Value* neg = b_.CreateXor(b_.CreateICmpSLT(a, llvm::ConstantInt::get(a->getType(), 0)),
                                                b_.CreateICmpSLT(b, llvm::ConstantInt::get(b->getType(), 0)));
                bound = b_.CreateSelect(neg, llvm::ConstantInt::get(ctx_, llvm::APInt::getSignedMinValue(w)),
                                        llvm::ConstantInt::get(ctx_, llvm::APInt::getSignedMaxValue(w)));
            } else {
                bound = llvm::ConstantInt::get(ctx_, llvm::APInt::getMaxValue(w));
            }
            return b_.CreateSelect(ovf, bound, b_.CreateExtractValue(r, 0));
        }
        llvm::Intrinsic::ID id = op == "add" ? (s ? llvm::Intrinsic::sadd_with_overflow : llvm::Intrinsic::uadd_with_overflow)
                               : op == "sub" ? (s ? llvm::Intrinsic::ssub_with_overflow : llvm::Intrinsic::usub_with_overflow)
                                             : (s ? llvm::Intrinsic::smul_with_overflow : llvm::Intrinsic::umul_with_overflow);
        llvm::Value* r = b_.CreateBinaryIntrinsic(id, a, b);
        return option_of(types::strip_ref(c.type), b_.CreateExtractValue(r, 0), b_.CreateNot(b_.CreateExtractValue(r, 1)));
    }

    // `std::format(f"...")`: the text as a new `std::string` (spec 19).
    llvm::Value* std_format(CallExpr& c) {
        const types::Type* at = c.args.size() == 1 ? types::strip_ref(c.args[0]->type) : nullptr;
        bool literal = c.args.size() == 1 && c.args[0]->kind == ExprKind::string_literal &&
                       c.args[0]->as<LiteralExpr>().token_kind == TokenKind::format_string_literal;
        if (!literal && !(at && at->is(Kind::external) && at->name == "std::format_args")) {
            unsupported(c.span, "'std::format' without an f-string");
        }
        const Symbol* sym = program_.std_module ? program_.std_module->item("std::string") : nullptr;
        const Symbol* from = sym ? std_member(tc_.user(sym), "from") : nullptr;
        if (!from) unsupported(c.span, "'std::format' without the standard library");
        record_format(callee_name(c));
        llvm::AllocaInst* buf  = text_into_buffer(*c.args[0]);
        llvm::Value*      data = b_.CreateLoad(ptr_type(), b_.CreateStructGEP(buffer_type(), buf, 0));
        llvm::Value*      len  = b_.CreateLoad(llvm::Type::getInt64Ty(ctx_), b_.CreateStructGEP(buffer_type(), buf, 1));
        llvm::Value*      text = b_.CreateInsertValue(b_.CreateInsertValue(llvm::UndefValue::get(str_type()), data, 0), len, 1);
        llvm::Value*      out  = b_.CreateCall(function_of(from), {text});
        b_.CreateCall(libc("free", llvm::Type::getVoidTy(ctx_), {ptr_type()}), {data});
        return out;
    }

    // Whether `t` is the library's `std::string` class, or a `std::string_with<A>` (text that
    // converts to `std::str` and formats the same way).
    bool is_string(const types::Type* t) const {
        if (!t || !t->is(Kind::user) || !program_.std_module) return false;
        return program_.std_module->item("std::string") == t->symbol || types::is_std_instance(t, "string_with");
    }

    // The name of the alternative of the simple enum `e` whose tag is `tag`: a simple enum prints
    // as its alternative's name (spec 19).
    llvm::Value* enum_name(const Symbol* e, llvm::Value* tag) {
        auto*        i64  = llvm::Type::getInt64Ty(ctx_);
        llvm::Value* text = llvm::ConstantAggregateZero::get(str_type());
        for (const Symbol* a : e->member_order) {
            if (!a->alternative) continue;
            llvm::Value* name = llvm::ConstantStruct::get(str_type(), {message_bytes(a->name), llvm::ConstantInt::get(i64, a->name.size())});
            text = b_.CreateSelect(b_.CreateICmpEQ(tag, tag_constant(a)), name, text);
        }
        return text;
    }

    // The text of a `std::str` or `std::string` expression, as a `std::str`.
    llvm::Value* text_value(Expr& e) {
        const types::Type* t = types::strip_ref(e.type);
        if (!is_string(t) || e.conversion == Expr::Conversion::string_to_str) return gen_expr(e);
        e.conversion = Expr::Conversion::string_to_str;   // the same borrow as an implicit conversion
        llvm::Value* v = gen_expr(e);
        e.conversion = Expr::Conversion::none;
        return v;
    }

    // One `{expr:spec}` hole of an f-string, formatted into `buf`.
    void format_hole(llvm::Value* buf, FormatPart& part) {
        Expr&                e    = *part.expr;
        const types::Type*   t    = types::strip_ref(e.type);
        llvm::Constant*      spec = part.spec.present
                                        ? spec_constant(part.spec.align, part.spec.plus, part.spec.width, part.spec.precision, part.spec.radix)
                                        : plain_spec();
        bool simple_enum = t && t->is_user(SymbolKind::enum_) && !has_payloads(t->symbol);
        if (t && t->is(Kind::user) && !simple_enum && !is_string(t)) {   // std::formattable, @std::derive::formattable
            format_at(buf, object_address(e), t, spec, e.span);
            return;
        }
        if (!t || !(t->is_integer() || t->is_float() || t->is(Kind::bool_) || t->is(Kind::char_) || is_str(t) || is_string(t) ||
                    simple_enum)) {
            unsupported(e.span, std::format("formatting values of type '{}'", t ? types::to_string(t) : "unknown"));
        }
        llvm::Value*         v    = simple_enum ? enum_name(t->symbol, gen_expr(e)) : text_value(e);
        if (simple_enum) t = tc_.external("std::str");
        auto*                i1   = llvm::Type::getInt1Ty(ctx_);
        auto*                i128 = llvm::Type::getInt128Ty(ctx_);
        auto*                vd   = llvm::Type::getVoidTy(ctx_);
        auto*                p    = ptr_type();
        if (t && t->is_integer()) {
            if (!t->is_signed || part.spec.radix) {   // unsigned, or two's complement bits in another base
                b_.CreateCall(runtime("edax_fmt_u", vd, {p, i128, i1, p}),
                              {buf, b_.CreateZExt(v, i128), llvm::ConstantInt::getFalse(ctx_), spec});
            } else {
                b_.CreateCall(runtime("edax_fmt_i", vd, {p, i128, p}), {buf, b_.CreateSExt(v, i128), spec});
            }
        } else if (t && t->is_float()) {
            bool single = t->bits <= 32;   // f16 prints through f32
            llvm::Value* d = single ? b_.CreateFPExt(v, llvm::Type::getDoubleTy(ctx_)) : v;
            if (t->bits == 128) b_.CreateCall(runtime("edax_fmt_q", vd, {p, v->getType(), p}), {buf, v, spec});   // f128: its own digits
            else b_.CreateCall(runtime("edax_fmt_f", vd, {p, llvm::Type::getDoubleTy(ctx_), llvm::Type::getInt8Ty(ctx_), p}),
                               {buf, d, llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), static_cast<std::uint64_t>(t->bits)), spec});
        } else if (t && t->is(Kind::bool_)) {
            b_.CreateCall(runtime("edax_fmt_bool", vd, {p, i1, p}), {buf, v, spec});
        } else if (t && t->is(Kind::char_)) {
            b_.CreateCall(runtime("edax_fmt_char", vd, {p, llvm::Type::getInt32Ty(ctx_), p}), {buf, v, spec});
        } else if (is_str(t) || is_string(t)) {
            b_.CreateCall(runtime("edax_fmt_str", vd, {p, p, llvm::Type::getInt64Ty(ctx_), p}),
                          {buf, b_.CreateExtractValue(v, 0), b_.CreateExtractValue(v, 1), spec});
        } else {
            unsupported(e.span, std::format("formatting values of type '{}'", t ? types::to_string(t) : "unknown"));
        }
    }

    // The `void format(mut std::formatter& out)` method of a type (std::formattable).
    static const Symbol* format_method(const Symbol* type) {
        for (const Symbol* t = type; t; t = t->base_class()) {
            auto it = t->members.find("format");
            if (it == t->members.end()) continue;
            for (const Symbol* m : it->second) {
                const FunctionDecl* f = m->function();
                if (m->kind == SymbolKind::method && f && !m->is_static && f->params.size() == 1) return m;
            }
        }
        return nullptr;
    }

    // Formats the value at `addr` of type `t` into `buf` (spec 19, 29): through the type's
    // `format` method, or for an enum with `@std::derive::formattable` as `name(payload)`.
    // Everything else is read and formatted like an f-string hole.
    void format_at(llvm::Value* buf, llvm::Value* addr, const types::Type* t, llvm::Constant* spec, Span span) {
        auto* vd  = llvm::Type::getVoidTy(ctx_);
        auto* p   = ptr_type();
        auto* i64 = llvm::Type::getInt64Ty(ctx_);
        auto put_text = [&](llvm::Value* target, const std::string& text) {
            b_.CreateCall(runtime("edax_buf_put", vd, {p, p, i64}),
                          {target, message_bytes(text), llvm::ConstantInt::get(i64, text.size())});
        };
        if (t->is(Kind::user) && t->symbol->kind != SymbolKind::enum_ && !is_string(t)) {
            const Symbol* method = format_method(t->symbol);
            const Symbol* str    = program_.std_module ? program_.std_module->item("std::string") : nullptr;
            const Symbol* as_str = str ? std_member(tc_.user(str), "as_str") : nullptr;
            if (!method || !as_str) unsupported(span, std::format("formatting values of type '{}'", types::to_string(t)));
            const types::Type* st  = tc_.user(str);
            llvm::AllocaInst*  tmp = entry_alloca(lower_type(st), "formatted");
            b_.CreateStore(llvm::Constant::getNullValue(lower_type(st)), tmp);   // an empty string
            // A std::formatter: the string to write to and the hole's spec (std::format_spec has
            // the runtime spec's layout).
            const Symbol* fsym = program_.std_module ? program_.std_module->item("std::formatter") : nullptr;
            if (!fsym) unsupported(span, "std::formatter");
            llvm::Type*       ft    = user_type(fsym);
            llvm::AllocaInst* fmt   = entry_alloca(ft, "formatter");
            unsigned          out_i = 0, spec_i = 0;
            for (const Symbol* f : all_fields(fsym)) {
                if (f->name == "out") out_i = field_index(f);
                if (f->name == "hole") spec_i = field_index(f);
            }
            llvm::Type* spec_t = llvm::cast<llvm::StructType>(ft)->getElementType(spec_i);
            b_.CreateStore(tmp, b_.CreateStructGEP(ft, fmt, out_i));
            b_.CreateStore(b_.CreateLoad(spec_t, spec), b_.CreateStructGEP(ft, fmt, spec_i));
            b_.CreateCall(function_of(method), {addr, fmt});
            llvm::Value* text = b_.CreateCall(function_of(as_str), {tmp});
            b_.CreateCall(runtime("edax_fmt_str", vd, {p, p, i64, p}),
                          {buf, b_.CreateExtractValue(text, 0), b_.CreateExtractValue(text, 1), spec});
            drop_value(st, tmp);
            return;
        }
        if (t->is_user(SymbolKind::enum_) && has_payloads(t->symbol)) {
            llvm::AllocaInst* inner = entry_alloca(buffer_type(), "enum_text");
            b_.CreateStore(llvm::ConstantAggregateZero::get(buffer_type()), inner);
            llvm::Value* tag = load_tag(t->symbol, addr);
            llvm::Function* fn   = b_.GetInsertBlock()->getParent();
            llvm::BasicBlock* done = llvm::BasicBlock::Create(ctx_, "fmt.done", fn);
            for (const Symbol* a : t->symbol->member_order) {
                if (!a->alternative) continue;
                llvm::BasicBlock* yes  = llvm::BasicBlock::Create(ctx_, "fmt.alt", fn);
                llvm::BasicBlock* next = llvm::BasicBlock::Create(ctx_, "fmt.next", fn);
                b_.CreateCondBr(b_.CreateICmpEQ(tag, tag_constant(a)), yes, next);
                b_.SetInsertPoint(yes);
                put_text(inner, a->name);
                if (a->alternative->payload) {
                    const types::Type* pt = tc_.lower(*a->alternative->payload, sema::substitution_of(a));
                    if (!pt->is(Kind::void_)) {
                        put_text(inner, "(");
                        format_at(inner, payload_address(t->symbol, addr), pt, plain_spec(), span);
                        put_text(inner, ")");
                    }
                }
                b_.CreateBr(done);
                b_.SetInsertPoint(next);
            }
            b_.CreateBr(done);
            b_.SetInsertPoint(done);
            llvm::Value* data = b_.CreateLoad(p, b_.CreateStructGEP(buffer_type(), inner, 0));
            llvm::Value* len  = b_.CreateLoad(i64, b_.CreateStructGEP(buffer_type(), inner, 1));
            b_.CreateCall(runtime("edax_fmt_str", vd, {p, p, i64, p}), {buf, data, len, spec});
            b_.CreateCall(libc("free", vd, {p}), {data});
            return;
        }
        // A plain value: read it and format it like a hole.
        auto* i1   = llvm::Type::getInt1Ty(ctx_);
        auto* i128 = llvm::Type::getInt128Ty(ctx_);
        if (is_string(t)) {
            const Symbol* as_str = std_member(t, "as_str");
            llvm::Value*  text   = b_.CreateCall(function_of(as_str), {addr});
            b_.CreateCall(runtime("edax_fmt_str", vd, {p, p, i64, p}),
                          {buf, b_.CreateExtractValue(text, 0), b_.CreateExtractValue(text, 1), spec});
            return;
        }
        llvm::Value* v = b_.CreateLoad(lower_type(t), addr);
        if (t->is_user(SymbolKind::enum_)) {
            v = enum_name(t->symbol, v);
            b_.CreateCall(runtime("edax_fmt_str", vd, {p, p, i64, p}), {buf, b_.CreateExtractValue(v, 0), b_.CreateExtractValue(v, 1), spec});
        } else if (t->is_integer()) {
            if (!t->is_signed) {
                b_.CreateCall(runtime("edax_fmt_u", vd, {p, i128, i1, p}), {buf, b_.CreateZExt(v, i128), llvm::ConstantInt::getFalse(ctx_), spec});
            } else {
                b_.CreateCall(runtime("edax_fmt_i", vd, {p, i128, p}), {buf, b_.CreateSExt(v, i128), spec});
            }
        } else if (t->is_float()) {
            bool single = t->bits <= 32;   // f16 prints through f32
            llvm::Value* d = single ? b_.CreateFPExt(v, llvm::Type::getDoubleTy(ctx_)) : v;
            if (t->bits == 128) b_.CreateCall(runtime("edax_fmt_q", vd, {p, v->getType(), p}), {buf, v, spec});   // f128: its own digits
            else b_.CreateCall(runtime("edax_fmt_f", vd, {p, llvm::Type::getDoubleTy(ctx_), llvm::Type::getInt8Ty(ctx_), p}),
                               {buf, d, llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), static_cast<std::uint64_t>(t->bits)), spec});
        } else if (t->is(Kind::bool_)) {
            b_.CreateCall(runtime("edax_fmt_bool", vd, {p, i1, p}), {buf, b_.CreateTrunc(v, i1), spec});
        } else if (t->is(Kind::char_)) {
            b_.CreateCall(runtime("edax_fmt_char", vd, {p, llvm::Type::getInt32Ty(ctx_), p}), {buf, v, spec});
        } else if (is_str(t)) {
            b_.CreateCall(runtime("edax_fmt_str", vd, {p, p, i64, p}), {buf, b_.CreateExtractValue(v, 0), b_.CreateExtractValue(v, 1), spec});
        } else {
            unsupported(span, std::format("formatting values of type '{}'", types::to_string(t)));
        }
    }

    // --- match, for and casts -----------------------------------------------------------------

    llvm::Value* str_equal(llvm::Value* a, llvm::Value* b, bool eq) {
        auto* i64 = llvm::Type::getInt64Ty(ctx_);
        llvm::Value* same = b_.CreateCall(runtime("edax_str_eq", llvm::Type::getInt1Ty(ctx_), {ptr_type(), i64, ptr_type(), i64}),
                                          {b_.CreateExtractValue(a, 0), b_.CreateExtractValue(a, 1), b_.CreateExtractValue(b, 0),
                                           b_.CreateExtractValue(b, 1)});
        return eq ? same : b_.CreateNot(same);
    }

    // Continues when `ok` holds; otherwise goes to `fail` (the next arm of a match).
    void require(llvm::Value* ok, llvm::BasicBlock* fail) {
        llvm::BasicBlock* cont = llvm::BasicBlock::Create(ctx_, "pattern", cur_fn_);
        b_.CreateCondBr(ok, cont, fail);
        b_.SetInsertPoint(cont);
    }

    const types::Type* payload_of(const Symbol* alt) {
        return tc_.lower(*alt->alternative->payload, sema::substitution_of(alt));
    }

    // The field of a struct type named `name`, or null.
    static const Symbol* field_symbol(const Symbol* type, const std::string& name) {
        auto it = type->members.find(name);
        if (it == type->members.end() || it->second.front()->kind != SymbolKind::field) return nullptr;
        return it->second.front();
    }

    // Checks the pattern `p` against the object at `addr` of type `t`. Falls through when it
    // matches (binding its names) and branches to `fail` otherwise. Parts of a value are only read
    // once the tests above them have passed, as in the C backend.
    void match_pattern(Pattern& p, llvm::Value* addr, const types::Type* t, llvm::BasicBlock* fail) {
        switch (p.kind) {
        case PatternKind::wildcard: return;
        case PatternKind::binding: {
            const Symbol* s = p.as<BindingPattern>().symbol;
            if (s) pending_bindings_.push_back(PendingBinding{s, addr, t, p.span});   // bound once every test passed
            return;
        }
        case PatternKind::literal: {
            Expr&        lit = *p.as<LiteralPattern>().literal;
            llvm::Value* v   = b_.CreateLoad(lower_type(t), addr);
            llvm::Value* l   = gen_expr(lit);
            if (is_str(t)) return require(str_equal(v, l, true), fail);
            if (v->getType()->isFloatingPointTy()) return require(b_.CreateFCmpOEQ(v, l), fail);
            require(b_.CreateICmpEQ(v, l), fail);
            return;
        }
        case PatternKind::range: {
            auto&        r = p.as<RangePattern>();
            llvm::Value* v = b_.CreateLoad(lower_type(t), addr);
            if (r.lo) require(compare_op(TokenKind::greater_eq, v, gen_expr(*r.lo), t, p.span), fail);
            if (r.hi) require(compare_op(r.inclusive ? TokenKind::less_eq : TokenKind::less, v, gen_expr(*r.hi), t, p.span), fail);
            return;
        }
        case PatternKind::enum_alternative: {
            auto&         ep  = p.as<EnumPattern>();
            const Symbol* alt = ep.symbol;
            if (!alt || !alt->parent || alt->kind != SymbolKind::alternative) unsupported(p.span, "this pattern (standard library enums)");
            const Symbol* e = alt->parent;
            if (!has_payloads(e)) {
                require(b_.CreateICmpEQ(b_.CreateLoad(enum_tag_type(e), addr), tag_constant(alt)), fail);
                return;
            }
            llvm::Value* tag = load_tag(e, addr);
            require(b_.CreateICmpEQ(tag, tag_constant(alt)), fail);
            if (ep.payload && carries(alt)) {
                match_pattern(*ep.payload, payload_address(e, addr), payload_of(alt), fail);
            }
            return;
        }
        case PatternKind::struct_: {
            auto& sp = p.as<StructPattern>();
            if (!t->is(Kind::user) || !is_struct_symbol(t->symbol)) unsupported(p.span, "this pattern");
            for (FieldPattern& fp : sp.fields) {
                const Symbol* f = field_symbol(t->symbol, fp.name);
                if (!f || !fp.pattern) continue;
                match_pattern(*fp.pattern, field_address(f, addr), field_type(f), fail);
            }
            return;
        }
        case PatternKind::tuple: {
            auto& tp = p.as<TuplePattern>();
            if (!t || !t->is(Kind::tuple)) unsupported(p.span, "this pattern");
            llvm::Type* st = lower_type(t);
            for (std::size_t i = 0; i < tp.elements.size(); ++i) {
                match_pattern(*tp.elements[i], b_.CreateStructGEP(st, addr, static_cast<unsigned>(i)), t->args[i], fail);
            }
            return;
        }
        case PatternKind::error: unsupported(p.span, "this pattern");
        }
    }

    // `match`: the arms are tried in order; the first whose pattern (and guard) matches gives the
    // value. If none matches, the program panics (spec 07).
    // `as_address`: every arm is a place (places_only), and the result is its address.
    llvm::Value* match_value(MatchExpr& m, const types::Type* t, bool as_address = false) {
        Expr* scrutinee = m.scrutinee.get();
        bool  borrow_mut = false;   // `match (mut x)`: the bindings are the parts of x (spec 10)
        if (scrutinee->kind == ExprKind::unary && scrutinee->as<UnaryExpr>().op == UnaryOp::mut_marker) {
            scrutinee  = scrutinee->as<UnaryExpr>().operand.get();
            borrow_mut = true;
        }
        const types::Type* st   = types::strip_ref(scrutinee->type);
        // A temporary (or `move x`) is owned by the match: its bindings take their parts, and what
        // no binding took is destroyed at the end. A place is borrowed: bindings name its parts.
        bool owning = needs_drop(st) && !is_place(*scrutinee) && !(scrutinee->type && scrutinee->type->is(Kind::reference));
        open_scope();
        llvm::Value*      addr       = object_address(*scrutinee);
        llvm::AllocaInst* owned_flag = nullptr;
        if (owning) {
            moved_out(*scrutinee);
            release_temporary(addr);
            if (!llvm::isa<llvm::AllocaInst>(addr)) unsupported(scrutinee->span, "matching on this value");
            owned_flag = entry_alloca(llvm::Type::getInt1Ty(ctx_), "df_scrutinee");
            b_.CreateStore(llvm::ConstantInt::getTrue(ctx_), owned_flag);
            scopes_.back().locals.push_back(OwnedLocal{llvm::cast<llvm::AllocaInst>(addr), owned_flag, drop_function(st)});
        }
        bool               has_value = !is_void(t) && !(t && t->is_unknown());
        llvm::AllocaInst*  slot      = has_value ? entry_alloca(as_address ? ptr_type() : lower_type(types::strip_ref(t)), "match.value") : nullptr;
        llvm::BasicBlock*  end_bb    = llvm::BasicBlock::Create(ctx_, "match.end", cur_fn_);
        // Each arm's test is at the match, not at the arm before it: an arm's body then starts a
        // line of its own, where a debugger stops.
        llvm::DebugLoc match_loc = b_.getCurrentDebugLocation();
        for (MatchArm& arm : m.arms) {
            b_.SetCurrentDebugLocation(match_loc);
            llvm::BasicBlock* next = llvm::BasicBlock::Create(ctx_, "arm.next", cur_fn_);
            llvm::BasicBlock* body = llvm::BasicBlock::Create(ctx_, "arm.body", cur_fn_);
            pending_bindings_.clear();
            match_pattern(*arm.pattern, addr, st, next);
            std::vector<PendingBinding> binds = std::move(pending_bindings_);
            pending_bindings_.clear();
            open_scope();
            bool took = false;
            for (const PendingBinding& pb : binds) {
                if (borrow_mut) {
                    locals_[pb.symbol] = pb.addr;   // writes go to the matched value
                } else if (!needs_drop(pb.type)) {   // a copy
                    llvm::Type*       lt = lower_type(pb.type);
                    llvm::AllocaInst* v  = entry_alloca(lt, pb.symbol->name);
                    b_.CreateStore(b_.CreateLoad(lt, pb.addr), v);
                    locals_[pb.symbol] = v;
                } else if (!owning) {
                    locals_[pb.symbol] = pb.addr;   // names the part of the borrowed value
                } else {
                    if (arm.guard) unsupported(pb.span, "a guard on an arm that takes a value with a destructor");
                    if (arm.pattern->kind == PatternKind::struct_) {
                        unsupported(pb.span, "taking a part of a struct with a destructor in a pattern");
                    }
                    llvm::Type*       lt = lower_type(pb.type);
                    llvm::AllocaInst* v  = entry_alloca(lt, pb.symbol->name);
                    b_.CreateStore(b_.CreateLoad(lt, pb.addr), v);
                    locals_[pb.symbol] = v;
                    own(pb.symbol, v, pb.type, true);
                    took = true;
                }
            }
            if (took) b_.CreateStore(llvm::ConstantInt::getFalse(ctx_), owned_flag);
            if (arm.guard) {
                b_.CreateCondBr(truth(*arm.guard), body, next);
            } else {
                b_.CreateBr(body);
            }
            b_.SetInsertPoint(body);
            if (arm.body->kind != ExprKind::block) debug_at(arm.body->span);   // `pattern => expression,`: its own line
            llvm::Value* v = as_address ? object_address(*arm.body) : gen_expr(*arm.body);
            if (!terminated() && slot && v) {
                b_.CreateStore(v, slot);
                if (!as_address) moved_out(*arm.body);   // `std::ok(list) => move list`: the value leaves the arm
            }
            close_scope();
            if (!terminated()) b_.CreateBr(end_bb);
            b_.SetInsertPoint(next);
        }
        check(llvm::ConstantInt::getFalse(ctx_), m.span, "no match arm matched");
        b_.CreateBr(end_bb);
        b_.SetInsertPoint(end_bb);
        llvm::Value* result = slot ? b_.CreateLoad(as_address ? ptr_type() : lower_type(types::strip_ref(t)), slot) : nullptr;
        close_scope();
        return result;
    }

    struct PendingBinding {
        const Symbol*      symbol;
        llvm::Value*       addr;
        const types::Type* type;
        Span               span;
    };
    std::vector<PendingBinding> pending_bindings_;

    // Compares two `std::str` byte by byte: negative, zero or positive (an i64).
    llvm::Value* str_compare(llvm::Value* a, llvm::Value* b) {
        auto*        i64  = llvm::Type::getInt64Ty(ctx_);
        auto*        i32  = llvm::Type::getInt32Ty(ctx_);
        llvm::Value* la   = b_.CreateExtractValue(a, 1);
        llvm::Value* lb   = b_.CreateExtractValue(b, 1);
        llvm::Value* n    = b_.CreateSelect(b_.CreateICmpULT(la, lb), la, lb);
        llvm::Value* c    = b_.CreateCall(libc("memcmp", i32, {ptr_type(), ptr_type(), i64}),
                                          {b_.CreateExtractValue(a, 0), b_.CreateExtractValue(b, 0), n});
        llvm::Value* diff = b_.CreateSub(la, lb);
        return b_.CreateSelect(b_.CreateICmpEQ(c, llvm::ConstantInt::get(i32, 0)), diff, b_.CreateSExt(c, i64));
    }

    // `a op b` for the ordering and equality operators on integers, chars, bools, enums and floats.
    llvm::Value* compare_op(TokenKind op, llvm::Value* a, llvm::Value* b, const types::Type* t, Span span) {
        if (a->getType()->isFloatingPointTy()) {
            switch (op) {
            case TokenKind::less: return b_.CreateFCmpOLT(a, b);
            case TokenKind::less_eq: return b_.CreateFCmpOLE(a, b);
            case TokenKind::greater: return b_.CreateFCmpOGT(a, b);
            case TokenKind::greater_eq: return b_.CreateFCmpOGE(a, b);
            default: unsupported(span, "comparing these values");
            }
        }
        if (a->getType() == str_type()) {   // std::str: bytes in order, then the shorter first
            a = str_compare(a, b);
            b = llvm::ConstantInt::get(a->getType(), 0);
            t = nullptr;
            switch (op) {
            case TokenKind::less: return b_.CreateICmpSLT(a, b);
            case TokenKind::less_eq: return b_.CreateICmpSLE(a, b);
            case TokenKind::greater: return b_.CreateICmpSGT(a, b);
            case TokenKind::greater_eq: return b_.CreateICmpSGE(a, b);
            default: return b_.CreateICmpEQ(a, b);
            }
        }
        if (!a->getType()->isIntegerTy()) unsupported(span, "comparing these values");
        bool s = t && t->is_integer() && t->is_signed;
        switch (op) {
        case TokenKind::less: return s ? b_.CreateICmpSLT(a, b) : b_.CreateICmpULT(a, b);
        case TokenKind::less_eq: return s ? b_.CreateICmpSLE(a, b) : b_.CreateICmpULE(a, b);
        case TokenKind::greater: return s ? b_.CreateICmpSGT(a, b) : b_.CreateICmpUGT(a, b);
        case TokenKind::greater_eq: return s ? b_.CreateICmpSGE(a, b) : b_.CreateICmpUGE(a, b);
        default: return b_.CreateICmpEQ(a, b);
        }
    }

    // `bit(n)`: the integer with only bit `n` set; `n` must be below the bit width (spec 16).
    llvm::Value* bit_value(BitExpr& b, const types::Type* type) {
        const types::Type* t = types::strip_ref(type);
        if (!t || !t->is_integer()) unsupported(b.span, "'bit(n)' without an integer type");
        llvm::Type* lt   = lower_type(t);
        llvm::Value* n   = b_.CreateSExtOrTrunc(gen_expr(*b.operand), llvm::Type::getInt128Ty(ctx_));
        llvm::Value* in  = b_.CreateAnd(b_.CreateICmpSGE(n, llvm::ConstantInt::get(n->getType(), 0)),
                                        b_.CreateICmpSLT(n, llvm::ConstantInt::get(n->getType(), static_cast<std::uint64_t>(t->bits))));
        if (mode_setting("shifts") == "mask") {
            return b_.CreateShl(llvm::ConstantInt::get(lt, 1),
                                b_.CreateAnd(b_.CreateTrunc(n, lt), llvm::ConstantInt::get(lt, static_cast<std::uint64_t>(t->bits - 1))));
        }
        check(in, b.span, std::format("bit index out of range for {}", t->name),
              {part(": "), part(n, true), part(std::format(" (must be 0 to {})", t->bits - 1))});
        return b_.CreateShl(llvm::ConstantInt::get(lt, 1), b_.CreateTrunc(n, lt));
    }

    // `for x in a..b` and `for x in a..=b` over integers. The inclusive form uses a flag so that
    // `a..=MAX` never overflows (as in the C backend).
    llvm::Value* for_loop(ForExpr& f) {
        if (f.unrolled) {   // `for compile_time`: the copies run one after the other (spec 26)
            llvm::BasicBlock* exit_bb = llvm::BasicBlock::Create(ctx_, "unrolled.end", cur_fn_);
            for (BlockPtr& copy : f.copies) {
                llvm::BasicBlock* next = llvm::BasicBlock::Create(ctx_, "unrolled.next", cur_fn_);
                loops_.push_back(Loop{exit_bb, next});
                open_scope(true);
                gen_block(*copy);
                close_scope();
                loops_.pop_back();
                if (!terminated()) b_.CreateBr(next);
                b_.SetInsertPoint(next);
            }
            b_.CreateBr(exit_bb);
            b_.SetInsertPoint(exit_bb);
            return nullptr;
        }
        if (f.is_compile_time) unsupported(f.span, "'for compile_time' over elements that are not known");
        if (f.len_symbol || f.next_symbol) return for_each(f);
        Expr* it = f.iterable.get();
        if (it->kind != ExprKind::range) unsupported(it->span, "'for' over anything but a range 'a..b'");
        auto&              r  = it->as<RangeExpr>();
        const types::Type* t  = f.symbol && f.symbol->type ? types::strip_ref(f.symbol->type) : nullptr;
        if (!t || !t->is_integer()) unsupported(f.span, "'for' over values of this type");
        llvm::Type*        lt = lower_type(t);
        llvm::Value*       lo = gen_expr(*r.lo);
        llvm::Value*       hi = gen_expr(*r.hi);
        llvm::AllocaInst*  iv = entry_alloca(lt, "for.i");
        llvm::AllocaInst*  go = r.inclusive ? entry_alloca(llvm::Type::getInt1Ty(ctx_), "for.go") : nullptr;
        b_.CreateStore(lo, iv);
        if (go) b_.CreateStore(compare_op(TokenKind::less_eq, lo, hi, t, f.span), go);
        llvm::BasicBlock* cond_bb = llvm::BasicBlock::Create(ctx_, "for.cond", cur_fn_);
        llvm::BasicBlock* body_bb = llvm::BasicBlock::Create(ctx_, "for.body", cur_fn_);
        llvm::BasicBlock* cont_bb = llvm::BasicBlock::Create(ctx_, "for.next", cur_fn_);
        llvm::BasicBlock* exit_bb = llvm::BasicBlock::Create(ctx_, "for.end", cur_fn_);
        b_.CreateBr(cond_bb);
        b_.SetInsertPoint(cond_bb);
        llvm::Value* more = go ? b_.CreateLoad(llvm::Type::getInt1Ty(ctx_), go)
                               : compare_op(TokenKind::less, b_.CreateLoad(lt, iv), hi, t, f.span);
        b_.CreateCondBr(more, body_bb, exit_bb);
        b_.SetInsertPoint(body_bb);
        if (f.symbol) {
            llvm::AllocaInst* var = entry_alloca(lt, f.symbol->name);
            b_.CreateStore(b_.CreateLoad(lt, iv), var);
            locals_[f.symbol] = var;
            if (inspect_) slot_info_[var] = SlotInfo{f.symbol->name, types::to_string(t), "loop variable"};
            debug_variable(var, f.symbol->name, t, f.name_span);
        }
        std::optional<std::uint64_t> trips;
        if (auto* l = llvm::dyn_cast<llvm::ConstantInt>(lo)) {
            if (auto* h = llvm::dyn_cast<llvm::ConstantInt>(hi)) {
                llvm::APInt d = h->getValue() - l->getValue();
                bool empty = t->is_signed ? h->getValue().slt(l->getValue()) : h->getValue().ult(l->getValue());
                trips = empty ? 0 : d.getZExtValue() + (r.inclusive ? 1 : 0);
            }
        }
        loops_.push_back(Loop{exit_bb, cont_bb});
        trips_.push_back(trips);
        open_scope(true);
        gen_block(*f.body);
        close_scope();
        trips_.pop_back();
        loops_.pop_back();
        if (!terminated()) b_.CreateBr(cont_bb);
        b_.SetInsertPoint(cont_bb);
        llvm::Value* cur = b_.CreateLoad(lt, iv);
        llvm::Value* one = llvm::ConstantInt::get(lt, 1);
        if (go) {
            llvm::Value* more_after = b_.CreateICmpNE(cur, hi);
            b_.CreateStore(b_.CreateSelect(more_after, b_.CreateAdd(cur, one), cur), iv);
            b_.CreateStore(more_after, go);
        } else {
            b_.CreateStore(b_.CreateAdd(cur, one), iv);
        }
        tag_loop(b_.CreateBr(cond_bb), f.span);
        b_.SetInsertPoint(exit_bb);
        return nullptr;
    }

    // In a `@std::simd::must_vectorize` function, marks the back edge of a plain loop with
    // `!{!"edax.must_vectorize", i32 n}` in its loop metadata: after optimizing, the loops that
    // were vectorized carry `llvm.loop.isvectorized` too.
    void tag_loop(llvm::Instruction* back_edge, Span span) {
        if (!must_vectorize_) return;
        auto* i32 = llvm::Type::getInt32Ty(ctx_);
        llvm::Metadata* tag[] = {llvm::MDString::get(ctx_, "edax.must_vectorize"),
                                 llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i32, program_.must_vectorize_loops.size()))};
        llvm::TempMDTuple self = llvm::MDTuple::getTemporary(ctx_, {});
        llvm::Metadata* ops[] = {self.get(), llvm::MDNode::get(ctx_, tag)};
        llvm::MDNode* id = llvm::MDNode::getDistinct(ctx_, ops);
        id->replaceOperandWith(0, id);
        back_edge->setMetadata(llvm::LLVMContext::MD_loop, id);
        program_.must_vectorize_loops.emplace_back(span, cur_fn_->getName().str());
    }

    bool must_vectorize_ = false;   // the function being generated is @std::simd::must_vectorize

    // `for (x : c)` over a container (`length()` and `operator[]`) or a stream (`next()`), spec 10.
    // A temporary iterable lives until the loop ends.
    llvm::Value* for_each(ForExpr& f) {
        auto*              i64 = llvm::Type::getInt64Ty(ctx_);
        const types::Type* ct  = types::strip_ref(f.iterable->type);
        open_scope();
        llvm::Value* obj = object_address(*f.iterable);   // a temporary iterable lives until the loop ends
        (void)ct;
        const types::Type* vt = f.symbol ? f.symbol->type : nullptr;
        llvm::BasicBlock* cond_bb = llvm::BasicBlock::Create(ctx_, "each.cond", cur_fn_);
        llvm::BasicBlock* body_bb = llvm::BasicBlock::Create(ctx_, "each.body", cur_fn_);
        llvm::BasicBlock* cont_bb = llvm::BasicBlock::Create(ctx_, "each.next", cur_fn_);
        llvm::BasicBlock* exit_bb = llvm::BasicBlock::Create(ctx_, "each.end", cur_fn_);
        llvm::AllocaInst* iv      = entry_alloca(i64, "each.i");
        llvm::Value*      len     = nullptr;
        llvm::AllocaInst* opt     = nullptr;
        const Symbol*     some    = nullptr;
        if (f.len_symbol) {
            len = b_.CreateCall(function_of(f.len_symbol), {obj});
            b_.CreateStore(llvm::ConstantInt::get(i64, 0), iv);
        } else {
            const FunctionDecl* nf = body_decl(f.next_symbol);
            const types::Type*  rt = tc_.lower(*nf->result, sema::substitution_of(f.next_symbol));
            opt  = entry_alloca(lower_type(rt), "each.option");
            for (const Symbol* a : rt->symbol->member_order) {
                if (a->name == "some") some = a;
            }
        }
        b_.CreateBr(cond_bb);
        b_.SetInsertPoint(cond_bb);
        llvm::Value* element = nullptr;   // the element's address
        if (len) {
            b_.CreateCondBr(b_.CreateICmpULT(b_.CreateLoad(i64, iv), len), body_bb, exit_bb);
            b_.SetInsertPoint(body_bb);
            element = b_.CreateCall(function_of(f.index_symbol), {obj, b_.CreateLoad(i64, iv)});
        } else {
            b_.CreateStore(b_.CreateCall(function_of(f.next_symbol), {obj}), opt);
            llvm::Value* tag = load_tag(some->parent, opt);
            b_.CreateCondBr(b_.CreateICmpEQ(tag, tag_constant(some)), body_bb, exit_bb);
            b_.SetInsertPoint(body_bb);
            element = payload_address(some->parent, opt);
        }
        loops_.push_back(Loop{exit_bb, cont_bb});
        trips_.push_back(std::nullopt);
        open_scope(true);
        if (f.symbol && vt) {
            if (vt->is(Kind::reference)) {
                llvm::AllocaInst* var = entry_alloca(ptr_type(), f.symbol->name);
                b_.CreateStore(element, var);
                locals_[f.symbol] = var;
            } else {
                llvm::AllocaInst* var = entry_alloca(lower_type(vt), f.symbol->name);
                b_.CreateStore(b_.CreateLoad(lower_type(vt), element), var);
                locals_[f.symbol] = var;
                if (f.next_symbol && needs_drop(vt)) own(f.symbol, var, vt, true);   // each element is owned
            }
        }
        gen_block(*f.body);
        close_scope();
        trips_.pop_back();
        loops_.pop_back();
        if (!terminated()) b_.CreateBr(cont_bb);
        b_.SetInsertPoint(cont_bb);
        if (len) b_.CreateStore(b_.CreateAdd(b_.CreateLoad(i64, iv), llvm::ConstantInt::get(i64, 1)), iv);
        llvm::Instruction* back = b_.CreateBr(cond_bb);
        if (len) tag_loop(back, f.span);   // over a container: a plain loop (a stream, like blocks, is not)
        b_.SetInsertPoint(exit_bb);
        close_scope();
        return nullptr;
    }

    // Clamps the integer value `v` (of type `from`) into the range of `to`: `cast::saturate`.
    llvm::Value* saturate_integer(llvm::Value* v, const types::Type* from, const types::Type* to) {
        auto* i128 = llvm::Type::getInt128Ty(ctx_);
        llvm::Value* w = from->is_signed ? b_.CreateSExt(v, i128) : b_.CreateZExt(v, i128);
        unsigned bits = static_cast<unsigned>(to->bits);
        if (to->is_signed) {
            llvm::APInt low  = llvm::APInt::getSignedMinValue(bits).sext(128);
            llvm::APInt high = llvm::APInt::getSignedMaxValue(bits).sext(128);
            w = b_.CreateSelect(b_.CreateICmpSLT(w, llvm::ConstantInt::get(ctx_, low)), llvm::ConstantInt::get(ctx_, low), w);
            w = b_.CreateSelect(b_.CreateICmpSGT(w, llvm::ConstantInt::get(ctx_, high)), llvm::ConstantInt::get(ctx_, high), w);
        } else {
            llvm::APInt high = llvm::APInt::getMaxValue(bits).zext(128);
            w = b_.CreateSelect(b_.CreateICmpSLT(w, llvm::ConstantInt::get(i128, 0)), llvm::ConstantInt::get(i128, 0), w);
            w = b_.CreateSelect(b_.CreateICmpSGT(w, llvm::ConstantInt::get(ctx_, high)), llvm::ConstantInt::get(ctx_, high), w);
        }
        return b_.CreateTrunc(w, lower_type(to));
    }

    // Float to integer with saturation: NaN gives 0, out-of-range values give the bounds.
    llvm::Value* saturate_float(llvm::Value* v, const types::Type* to) {
        llvm::Type*  dt = v->getType()->isFP128Ty() ? v->getType() : llvm::Type::getDoubleTy(ctx_);
        llvm::Value* f  = v->getType() == dt ? v : b_.CreateFPExt(v, dt);
        unsigned bits = static_cast<unsigned>(to->bits);
        double   low  = to->is_signed ? -std::ldexp(1.0, static_cast<int>(bits) - 1) : 0.0;
        double   high = to->is_signed ? std::ldexp(1.0, static_cast<int>(bits) - 1) : std::ldexp(1.0, static_cast<int>(bits));
        llvm::Type* lt = lower_type(to);
        llvm::Value* converted = to->is_signed ? b_.CreateFPToSI(f, lt) : b_.CreateFPToUI(f, lt);
        llvm::Value* min_v = to->is_signed ? llvm::ConstantInt::get(lt, llvm::APInt::getSignedMinValue(bits))
                                           : llvm::ConstantInt::get(lt, 0);
        llvm::Value* max_v = to->is_signed ? llvm::ConstantInt::get(lt, llvm::APInt::getSignedMaxValue(bits))
                                           : llvm::ConstantInt::get(lt, llvm::APInt::getMaxValue(bits));
        llvm::Value* r = b_.CreateSelect(b_.CreateFCmpOGE(f, llvm::ConstantFP::get(dt, high)), max_v, converted);
        r = b_.CreateSelect(b_.CreateFCmpOLE(f, llvm::ConstantFP::get(dt, low)), min_v, r);
        return b_.CreateSelect(b_.CreateFCmpUNO(f, f), llvm::ConstantInt::get(lt, 0), r);
    }

    // A conversion between numeric types, as in C: widening follows the source's signedness.
    llvm::Value* convert(llvm::Value* v, const types::Type* from, const types::Type* to) {
        llvm::Type* lt = lower_type(to);
        if (from->is(Kind::pointer) || to->is(Kind::pointer)) {
            if (to->is(Kind::pointer) && from->is(Kind::pointer)) return v;
            if (to->is(Kind::pointer)) return b_.CreateIntToPtr(v, lt);
            return b_.CreatePtrToInt(v, lt);
        }
        if (to->is_integer() || to->is(Kind::char_)) {
            if (from->is_float()) {
                bool s = to->is_integer() && to->is_signed;
                return s ? b_.CreateFPToSI(v, lt) : b_.CreateFPToUI(v, lt);
            }
            if (!v->getType()->isIntegerTy()) unsupported(cur_, "this conversion");
            unsigned fb = v->getType()->getIntegerBitWidth();
            unsigned tb = lt->getIntegerBitWidth();
            if (tb < fb) return b_.CreateTrunc(v, lt);
            if (tb > fb) return (from->is_integer() && from->is_signed) ? b_.CreateSExt(v, lt) : b_.CreateZExt(v, lt);
            return v;
        }
        if (to->is_float()) {
            if (from->is_float()) {
                if (from->bits == to->bits) return v;
                return from->bits < to->bits ? b_.CreateFPExt(v, lt) : b_.CreateFPTrunc(v, lt);
            }
            bool s = from->is_integer() && from->is_signed;
            return s ? b_.CreateSIToFP(v, lt) : b_.CreateUIToFP(v, lt);
        }
        if (to->is(Kind::bool_) && from->is(Kind::bool_)) return v;
        unsupported(cur_, std::format("converting to '{}'", types::to_string(to)));
    }

    static bool is_distinct(const types::Type* t) {
        return t && t->is(Kind::user) && t->symbol && t->symbol->decl && t->symbol->kind == SymbolKind::struct_ &&
               t->symbol->decl->as<TypeDecl>().is_distinct;
    }

    llvm::Value* cast_value(CastExpr& c) {
        const types::Type* to   = types::strip_ref(c.type);
        const types::Type* from = types::strip_ref(c.operand->type);
        const std::string& variant = c.variant;
        if (variant == "checked" && types::is_std_instance(to, "option")) return checked_cast(c, from, to);
        if (variant == "dynamic") return dynamic_cast_value(c, types::strip_ref(to->symbol->template_args[0]), to);
        if (variant == "checked" || variant == "dynamic") {
            unsupported(c.span, std::format("'cast::{}' (it returns std::option)", variant));
        }
        if (variant.empty() && (is_distinct(to) || is_distinct(from))) {   // a distinct type and its underlying type (spec 34)
            llvm::Value* v = gen_expr(*c.operand);
            if (v->getType()->isPointerTy() && !lower_type(from)->isPointerTy()) v = b_.CreateLoad(lower_type(from), v);
            if (is_distinct(to)) return b_.CreateInsertValue(llvm::UndefValue::get(lower_type(to)), v, {0});
            return b_.CreateExtractValue(v, {0});
        }
        llvm::Value* v = gen_expr(*c.operand);
        if (from->is(Kind::vector) && to->is(Kind::vector)) {   // lane by lane
            llvm::Value* out = llvm::UndefValue::get(lower_type(to));
            for (int i = 0; i < to->bits; ++i) {
                llvm::Value* lane = b_.CreateExtractElement(v, static_cast<std::uint64_t>(i));
                llvm::Value* r    = variant == "saturate" && to->inner->is_integer()
                                        ? (from->inner->is_float() ? saturate_float(lane, to->inner) : saturate_integer(lane, from->inner, to->inner))
                                        : convert(lane, from->inner, to->inner);
                out = b_.CreateInsertElement(out, r, static_cast<std::uint64_t>(i));
            }
            return out;
        }
        if (variant == "reinterpret") {
            if (to->is(Kind::pointer) || from->is(Kind::pointer)) return convert(v, from, to);
            return b_.CreateBitCast(v, lower_type(to));
        }
        if (variant == "saturate" && to->is_integer()) {
            if (from->is_float()) return saturate_float(v, to);
            if (from->is_integer()) return saturate_integer(v, from, to);
        }
        if (variant == "truncate" && from->is_float() && to->is_integer()) {
            // The CPU's own conversion (one instruction), toward zero. A value that doesn't fit gives
            // an unspecified integer: frozen, so it is some integer and not undefined behaviour.
            llvm::Type* lt = lower_type(to);
            return b_.CreateFreeze(to->is_signed ? b_.CreateFPToSI(v, lt) : b_.CreateFPToUI(v, lt));
        }
        // cast<T> (lossless), cast::truncate (keeps the low bits) and cast::round are conversions.
        return convert(v, from, to);
    }

    // --- checks (spec 23) -----------------------------------------------------------------

    // `run_time check (c) else msg;` (and the run-time part of `compile_time run_time`): panics
    // with the message at the check when the condition is false. Compile-time checks and checks
    // of other build modes generate nothing.
    void run_time_check(Check& c, Span span) {
        if (!c.run_active || !c.condition) return;
        llvm::Value*      ok   = truth(*c.condition);
        llvm::BasicBlock* fail = llvm::BasicBlock::Create(ctx_, "check.failed", cur_fn_);
        llvm::BasicBlock* pass = llvm::BasicBlock::Create(ctx_, "check.ok", cur_fn_);
        b_.CreateCondBr(ok, pass, fail);
        b_.SetInsertPoint(fail);
        if (c.is_entry) {   // an entry check names the caller's line, then the check (spec 23)
            llvm::Value* site  = site_arg_ ? site_arg_ : static_cast<llvm::Value*>(b_.CreateLoad(ptr_type(), call_site_global()));
            llvm::Value* check = loc_global(c.condition->span);
            llvm::Value* at    = b_.CreateSelect(b_.CreateIsNotNull(site), site, check);
            const SourceFile& file = sources_.file(c.condition->span.file);
            LineCol lc = file.line_col(c.condition->span.begin);
            bool in_std = file.path().starts_with("<std>");   // the library's checks: the caller's line is enough
            panic_with(*c.message, c.condition->span, at,
                       in_std ? std::string()
                              : std::format("\nnote: the check `{}` is at {}:{}:{}", snippet(c.condition->span), file.path(), lc.line, lc.column));
        } else {
            bool        in_std = sources_.file(c.condition->span.file).path().starts_with("<std>");
            std::string text   = snippet(c.condition->span);
            panic_with(*c.message, c.condition->span, nullptr,
                       in_std || text == "false" ? std::string() : std::format("\nnote: the check `{}` failed", text));
        }
        b_.SetInsertPoint(pass);
        (void)span;
    }

    // --- compile-time values (spec 15, 26) ---------------------------------------------------

    // The constant for a value computed at compile time; null for values that exist only at
    // compile time (types, reflection data, lists).
    llvm::Value* materialize(const ct::Value& v, Span where) {
        const types::Type* t = types::strip_ref(v.type);
        switch (v.kind) {
        case ct::Value::Kind::integer: {
            auto* lt = llvm::cast<llvm::IntegerType>(lower_type(t));
            return llvm::ConstantInt::get(ctx_, wide(v.integer, lt->getBitWidth()));
        }
        case ct::Value::Kind::floating: return float_constant(v, lower_type(t));
        case ct::Value::Kind::boolean: return llvm::ConstantInt::getBool(ctx_, v.boolean);
        case ct::Value::Kind::character:
            return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), static_cast<std::uint64_t>(v.character));
        case ct::Value::Kind::string:
            if (t && types::is_std_instance(t, "span")) {   // std::embed (spec 34): the bytes in static memory
                llvm::Constant* bytes = llvm::ConstantDataArray::getString(ctx_, v.string, false);
                auto* g = new llvm::GlobalVariable(*mod_, bytes->getType(), true, llvm::GlobalValue::PrivateLinkage, bytes, "embed");
                g->setAlignment(llvm::Align(16));
                return build_struct(t->symbol, {{"data", g}, {"count", llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), v.string.size())}});
            }
            return str_constant(v.string);
        case ct::Value::Kind::enumeration: {
            const Symbol* alt = alternative(t->symbol, v.symbol->name);
            llvm::Value*  payload = nullptr;
            if (!v.elements.empty()) {
                payload = materialize(v.elements[0], where);
                if (!payload) return nullptr;
            }
            return alternative_value(alt, payload);
        }
        case ct::Value::Kind::aggregate: {
            std::map<std::string, llvm::Value*> given;
            std::size_t i = 0;
            for (const Symbol* f : fields_of(t->symbol)) {
                if (i >= v.elements.size()) break;
                llvm::Value* x = materialize(v.elements[i++], where);
                if (!x) return nullptr;
                given[f->name] = x;
            }
            return build_struct(t->symbol, given);
        }
        default: return nullptr;
        }
    }

    // A compile-time value as an LLVM constant of type `lt` (for the initial value of a
    // thread_local); null when it has a part that isn't plain data.
    llvm::Constant* constant_of(const ct::Value& v, llvm::Type* lt) {
        const types::Type* t = types::strip_ref(v.type);
        switch (v.kind) {
        case ct::Value::Kind::integer: return llvm::ConstantInt::get(ctx_, wide(v.integer, lt->getIntegerBitWidth()));
        case ct::Value::Kind::floating: return float_constant(v, lt);
        case ct::Value::Kind::boolean: return llvm::ConstantInt::getBool(ctx_, v.boolean);
        case ct::Value::Kind::character: return llvm::ConstantInt::get(lt, static_cast<std::uint64_t>(v.character));
        case ct::Value::Kind::string: return str_constant(v.string);
        case ct::Value::Kind::enumeration:
            if (!t || !t->is(Kind::user) || has_payloads(t->symbol)) return nullptr;
            return tag_constant(alternative(t->symbol, v.symbol->name));
        case ct::Value::Kind::list: {   // a std::raw::array
            auto* at = llvm::dyn_cast<llvm::ArrayType>(lt);
            if (!at || at->getNumElements() != v.elements.size()) return nullptr;
            std::vector<llvm::Constant*> parts;
            for (const ct::Value& e : v.elements) {
                llvm::Constant* c = constant_of(e, at->getElementType());
                if (!c) return nullptr;
                parts.push_back(c);
            }
            return llvm::ConstantArray::get(at, parts);
        }
        case ct::Value::Kind::aggregate: {
            auto* st = llvm::dyn_cast<llvm::StructType>(lt);
            if (!st || !t || !t->is(Kind::user) || has_vtable(t->symbol) || t->symbol->base_class()) return nullptr;
            std::vector<llvm::Constant*> parts;
            std::vector<const Symbol*>   fields = fields_of(t->symbol);
            if (fields.size() != st->getNumElements() || fields.size() != v.elements.size()) {
                return fields.empty() ? llvm::Constant::getNullValue(lt) : nullptr;
            }
            for (std::size_t i = 0; i < fields.size(); ++i) {
                llvm::Constant* c = constant_of(v.elements[i], st->getElementType(static_cast<unsigned>(i)));
                if (!c) return nullptr;
                parts.push_back(c);
            }
            return llvm::ConstantStruct::get(st, parts);
        }
        default: return nullptr;
        }
    }

    // The address of the field a reflected `std::meta::field` names, in `object` (spec 26).
    llvm::Value* field_address(const ct::Value& field, Expr& object) {
        const Symbol* f = field.symbol;
        return field_address(f, object_address(object));
    }

    // `f.get(value)`, `a.is(value)`, `a.get(value)`, `a.make(...)`.
    llvm::Value* meta_call(CallExpr& c, const ct::Value& handle, const std::string& name) {
        if (handle.kind == ct::Value::Kind::field) {
            llvm::Value* addr = field_address(handle, *c.args.at(0));
            return b_.CreateLoad(lower_type(types::strip_ref(static_cast<Expr&>(c).type)), addr);
        }
        const Symbol* e   = handle.type_value->symbol;
        const Symbol* alt = alternative(e, handle.symbol->name);
        if (name == "make") {
            llvm::Value* payload = c.args.empty() ? nullptr : gen_expr(*c.args[0]);
            if (!c.args.empty()) moved_out(*c.args[0]);
            return alternative_value(alt, payload);
        }
        llvm::Value* addr = object_address(*c.args.at(0));
        llvm::Value* tag  = load_tag(e, addr);
        llvm::Value* is   = b_.CreateICmpEQ(tag, tag_constant(alt));
        if (name == "is") return is;
        const Symbol* option = types::strip_ref(static_cast<Expr&>(c).type)->symbol;   // a.get(value): std::option<P&>
        llvm::Value*  some   = alternative_value(alternative(option, "some"), payload_address(e, addr));
        llvm::Value*  none   = alternative_value(alternative(option, "none"), nullptr);
        return b_.CreateSelect(is, some, none);
    }

    // --- std::option and std::result (spec 11) -----------------------------------------------

    // The alternative `name` of the enum (instance) `e`.
    static const Symbol* alternative(const Symbol* e, const std::string& name) {
        return e->members.at(name).front();
    }

    // The alternative that carries the value: `some` or `ok`.
    static const Symbol* good_alternative(const Symbol* e) {
        return alternative(e, e->members.count("ok") ? "ok" : "some");
    }

    // Stores the value of `operand` (an option or result, consumed) in a fresh slot and tests
    // whether it holds `some` / `ok`.
    std::pair<llvm::AllocaInst*, llvm::Value*> unpack(Expr& operand, const Symbol* e) {
        llvm::Type*       et   = user_type(e);
        llvm::AllocaInst* slot = entry_alloca(et, "unpacked");
        b_.CreateStore(gen_expr(operand), slot);
        moved_out(operand);
        llvm::Value* tag = load_tag(e, slot);
        return {slot, b_.CreateICmpEQ(tag, tag_constant(good_alternative(e)))};
    }

    // The payload of alternative `alt`, read from the enum at `slot`.
    llvm::Value* payload_at(llvm::AllocaInst* slot, const Symbol* alt) {
        if (!carries(alt)) return nullptr;   // a `void` payload
        return b_.CreateLoad(lower_type(payload_of(alt)), payload_address(alt->parent, slot));
    }

    // Destroys the object at `addr` if its type needs it (a value that is thrown away).
    void drop_value(const types::Type* t, llvm::Value* addr) {
        if (needs_drop(t)) b_.CreateCall(drop_function(t), {addr});
    }

    // Branches on `good`; `on_good` / `on_bad` produce a value of type `t` (or end the block).
    llvm::Value* branch_value(llvm::Value* good, const types::Type* t, const std::function<llvm::Value*()>& on_good,
                              const std::function<llvm::Value*()>& on_bad) {
        bool              none   = !t || t->is(Kind::void_);   // `std::option<void>`: only the branch matters
        llvm::AllocaInst* out    = none ? nullptr : entry_alloca(lower_type(t), "value");
        llvm::BasicBlock* yes    = llvm::BasicBlock::Create(ctx_, "good", cur_fn_);
        llvm::BasicBlock* no     = llvm::BasicBlock::Create(ctx_, "bad", cur_fn_);
        llvm::BasicBlock* done   = llvm::BasicBlock::Create(ctx_, "joined", cur_fn_);
        b_.CreateCondBr(good, yes, no);
        for (auto [block, make] : {std::pair{yes, &on_good}, std::pair{no, &on_bad}}) {
            b_.SetInsertPoint(block);
            llvm::Value* v = (*make)();
            if (!terminated()) {
                if (out && v) b_.CreateStore(v, out);
                b_.CreateBr(done);
            }
        }
        b_.SetInsertPoint(done);
        return out ? b_.CreateLoad(lower_type(t), out) : nullptr;
    }

    // `r?`: the `ok` / `some` value, or return the error / `none` from the function.
    llvm::Value* try_value(TryExpr& t) {
        const Symbol*      e     = types::strip_ref(t.operand->type)->symbol;
        auto [slot, good]        = unpack(*t.operand, e);
        const Symbol*      ok    = good_alternative(e);
        const types::Type* value = payload_of(ok);
        const Symbol*      ret   = types::strip_ref(result_)->symbol;   // the function's own option / result
        return branch_value(good, value, [&] { return payload_at(slot, ok); }, [&]() -> llvm::Value* {
            llvm::Value* r = ret->members.count("error")
                                 ? alternative_value(alternative(ret, "error"), payload_at(slot, alternative(e, "error")))
                                 : alternative_value(alternative(ret, "none"), nullptr);
            drop_site_ = t.span;
            drop_kind_ = "exit-drop";
            leave_function(r);
            return nullptr;
        });
    }

    // value_or, expect and or_error (map_error needs function values). The receiver is consumed;
    // a part that is not used is destroyed (spec 11, 12).
    llvm::Value* std_method(CallExpr& c, MemberExpr& me, const types::Type* t, const std::string& name) {
        const Symbol*      e     = t->symbol;
        if (types::is_std_query_method(name)) {   // the tag, read in place
            llvm::Value* addr = object_address(*me.object);
            llvm::Value* tag  = load_tag(e, addr);
            llvm::Value* good = b_.CreateICmpEQ(tag, tag_constant(good_alternative(e)));
            return name == "is_some" || name == "is_ok" ? good : b_.CreateNot(good);
        }
        if (name == "map_error") {   // ok stays ok; the error goes through the lambda (spec 11)
            auto [slot, good]       = unpack(*me.object, e);
            const Symbol*      ok   = good_alternative(e);
            const Symbol*      err  = alternative(e, "error");
            const types::Type* rt   = types::strip_ref(static_cast<Expr&>(c).type);
            const types::Type* ft   = types::strip_ref(c.args[0]->type);
            llvm::AllocaInst*  fslot = entry_alloca(lower_type(ft), "map_error.f");
            b_.CreateStore(gen_expr(*c.args[0]), fslot);
            moved_out(*c.args[0]);
            return branch_value(good, rt, [&] {
                return alternative_value(alternative(rt->symbol, "ok"), payload_at(slot, ok));
            }, [&] {
                llvm::Value* arg = ft->args[0]->is(Kind::reference) ? payload_address(e, slot)
                                                                    : payload_at(slot, err);
                llvm::Value* e2  = b_.CreateCall(lambda_function(*tc_.lambda_of(ft)), {fslot, arg});
                return alternative_value(alternative(rt->symbol, "error"), e2);
            });
        }
        auto [slot, good]        = unpack(*me.object, e);
        const Symbol*      ok    = good_alternative(e);
        const types::Type* value = payload_of(ok);
        const Symbol*      error = e->members.count("error") ? alternative(e, "error") : nullptr;
        auto drop_error = [&] {
            if (error) drop_value(payload_of(error), payload_address(e, slot));
        };
        Expr& arg = *c.args[0];
        if (name == "expect") {
            return branch_value(good, value, [&] { return payload_at(slot, ok); }, [&]() -> llvm::Value* {
                panic_with(arg, me.name.span);
                return nullptr;
            });
        }
        // value_or and or_error: the argument is evaluated first, whichever way it goes.
        if (name == "value_or" && value->is(Kind::reference)) {   // `std::option<T&>`: the default is a place too
            llvm::Value* fallback = object_address(arg);
            return branch_value(good, value, [&] { return payload_at(slot, ok); }, [&] { return fallback; });
        }
        const types::Type* at = types::strip_ref(arg.type);
        llvm::AllocaInst*  a  = entry_alloca(lower_type(at), "argument");
        b_.CreateStore(gen_expr(arg), a);
        moved_out(arg);
        if (name == "value_or") {
            return branch_value(good, value, [&] {
                drop_value(at, a);
                return payload_at(slot, ok);
            }, [&] {
                drop_error();
                return b_.CreateLoad(lower_type(at), a);
            });
        }
        const types::Type* rt = types::strip_ref(static_cast<Expr&>(c).type);   // or_error: result<T, E2>
        return branch_value(good, rt, [&] {
            drop_value(at, a);
            return alternative_value(alternative(rt->symbol, "ok"), payload_at(slot, ok));
        }, [&] {
            drop_error();
            return alternative_value(alternative(rt->symbol, "error"), b_.CreateLoad(lower_type(at), a));
        });
    }

    // Panics with `message` (a string or an f-string) at `where`.
    void panic_with(Expr& message, Span where, llvm::Value* at = nullptr, const std::string& suffix = {}) {
        auto* p   = ptr_type();
        auto* i64 = llvm::Type::getInt64Ty(ctx_);
        auto* vd  = llvm::Type::getVoidTy(ctx_);
        debug_at(where);   // a debugger stops at the line that panicked
        if (options_.panic_trap) {   // panic() == trap: no message (spec 24)
            b_.CreateCall(intrinsic_declaration(mod_.get(), llvm::Intrinsic::trap, {}));
            b_.CreateUnreachable();
            return;
        }
        llvm::Value* loc = at ? at : loc_global(where);
        if (message.kind == ExprKind::string_literal && message.as<LiteralExpr>().token_kind == TokenKind::format_string_literal) {
            // Holes that are all plain integers (at most two, as the library's bounds checks
            // have): one call of edax_panic_values, which keeps the failure path small.
            auto& parts = message.as<LiteralExpr>().format_parts;
            std::size_t holes = 0;
            bool        plain = true;
            for (FormatPart& part : parts) {
                if (!part.expr) {
                    plain = plain && part.text.find_first_of("\x01\x02") == std::string::npos;
                    continue;
                }
                const types::Type* t = types::strip_ref(part.expr->type);
                ++holes;
                plain = plain && t && t->is_integer() && t->bits <= 128 && !part.spec.present;
            }
            if (plain && holes <= 2) {
                std::string               text, summary;
                std::vector<llvm::Value*> numbers;
                for (FormatPart& part : parts) {
                    if (!part.expr) {
                        text += part.text;
                        summary += part.text;
                        continue;
                    }
                    bool is_signed = types::strip_ref(part.expr->type)->is_signed;
                    text += is_signed ? '\x01' : '\x02';
                    summary += "{" + snippet(part.expr->span) + "}";
                    numbers.push_back(widen_for_panic(gen_expr(*part.expr), is_signed));
                }
                panic_values(loc, text + suffix, numbers, summary);
                end_panic();
                return;
            }
        }
        if (!suffix.empty()) {   // the message, then a note: through a buffer
            llvm::AllocaInst* buf = entry_alloca(buffer_type(), "buffer");
            b_.CreateStore(llvm::ConstantAggregateZero::get(buffer_type()), buf);
            if (message.kind == ExprKind::string_literal && message.as<LiteralExpr>().token_kind == TokenKind::format_string_literal) {
                for (FormatPart& part : message.as<LiteralExpr>().format_parts) {
                    if (part.expr) format_hole(buf, part);
                    else b_.CreateCall(runtime("edax_buf_put", vd, {p, p, i64}),
                                       {buf, message_bytes(part.text), llvm::ConstantInt::get(i64, part.text.size())});
                }
            } else {
                llvm::Value* v = gen_expr(message);
                b_.CreateCall(runtime("edax_buf_put", vd, {p, p, i64}), {buf, b_.CreateExtractValue(v, 0), b_.CreateExtractValue(v, 1)});
            }
            b_.CreateCall(runtime("edax_buf_put", vd, {p, p, i64}),
                          {buf, message_bytes(suffix), llvm::ConstantInt::get(i64, suffix.size())});
            b_.CreateCall(runtime("edax_panic_buf", vd, {p, p}), {loc, buf});
            end_panic();
            return;
        }
        if (message.kind == ExprKind::string_literal && message.as<LiteralExpr>().token_kind == TokenKind::format_string_literal) {
            llvm::AllocaInst* buf = entry_alloca(buffer_type(), "buffer");
            b_.CreateStore(llvm::ConstantAggregateZero::get(buffer_type()), buf);
            for (FormatPart& part : message.as<LiteralExpr>().format_parts) {
                if (part.expr) {
                    format_hole(buf, part);
                } else {
                    b_.CreateCall(runtime("edax_buf_put", vd, {p, p, i64}),
                                  {buf, message_bytes(part.text), llvm::ConstantInt::get(i64, part.text.size())});
                }
            }
            b_.CreateCall(runtime("edax_panic_buf", vd, {p, p}), {loc, buf});
        } else {
            llvm::Value* v = gen_expr(message);
            b_.CreateCall(runtime("edax_panic_str", vd, {p, p, i64}),
                          {loc, b_.CreateExtractValue(v, 0), b_.CreateExtractValue(v, 1)});
        }
        end_panic();
    }

    // `cast::checked<T>(x)`: `std::some` of the converted value when it is exactly the same
    // number, otherwise `std::none` (spec 17).
    llvm::Value* checked_cast(CastExpr& c, const types::Type* from, const types::Type* option) {
        const types::Type* to = option->symbol->template_args[0];
        if (from->is_integer() && to->is(Kind::char_)) {   // a Unicode scalar value: not a surrogate, at most 0x10FFFF
            llvm::Value* v    = gen_expr(*c.operand);
            auto*        i128 = llvm::Type::getInt128Ty(ctx_);
            llvm::Value* w    = from->is_signed ? b_.CreateSExt(v, i128) : b_.CreateZExt(v, i128);
            llvm::Value* ok   = b_.CreateAnd(b_.CreateICmpSGE(w, llvm::ConstantInt::get(i128, 0)),
                                             b_.CreateICmpSLE(w, llvm::ConstantInt::get(i128, 0x10FFFF)));
            llvm::Value* sur  = b_.CreateAnd(b_.CreateICmpSGE(w, llvm::ConstantInt::get(i128, 0xD800)),
                                             b_.CreateICmpSLE(w, llvm::ConstantInt::get(i128, 0xDFFF)));
            ok = b_.CreateAnd(ok, b_.CreateNot(sur));
            return option_of(option, b_.CreateTrunc(w, llvm::Type::getInt32Ty(ctx_)), ok);
        }
        if (!from->is_numeric() || !to->is_numeric()) unsupported(c.span, "'cast::checked' between these types");
        llvm::Value* v         = gen_expr(*c.operand);
        llvm::Value* converted = nullptr;
        llvm::Value* exact     = nullptr;
        auto*        dt        = llvm::Type::getDoubleTy(ctx_);
        // [low, high) of an integer type, as doubles (both are powers of two, so exact).
        auto range_of = [&](const types::Type* it, llvm::Value* f) {
            double low  = it->is_signed ? -std::ldexp(1.0, it->bits - 1) : 0.0;
            double high = std::ldexp(1.0, it->is_signed ? it->bits - 1 : it->bits);
            llvm::Type*  wt   = f->getType()->isFP128Ty() ? f->getType() : dt;
            llvm::Value* wide = f->getType() == wt ? f : b_.CreateFPExt(f, wt);
            return b_.CreateAnd(b_.CreateFCmpOGE(wide, llvm::ConstantFP::get(wt, low)),
                                b_.CreateFCmpOLT(wide, llvm::ConstantFP::get(wt, high)));
        };
        if (from->is_integer() && to->is_integer()) {
            converted = convert(v, from, to);
            exact     = b_.CreateICmpEQ(convert(converted, to, from), v);
            if (from->is_signed && !to->is_signed) {
                exact = b_.CreateAnd(exact, b_.CreateICmpSGE(v, llvm::ConstantInt::get(v->getType(), 0)));
            } else if (!from->is_signed && to->is_signed) {
                exact = b_.CreateAnd(exact, b_.CreateICmpSGE(converted, llvm::ConstantInt::get(converted->getType(), 0)));
            }
        } else if (from->is_integer()) {   // int -> float: the float must convert back to the same integer
            converted = convert(v, from, to);
            llvm::BasicBlock* test = b_.GetInsertBlock();
            llvm::BasicBlock* back = llvm::BasicBlock::Create(ctx_, "checked.back", cur_fn_);
            llvm::BasicBlock* done = llvm::BasicBlock::Create(ctx_, "checked.done", cur_fn_);
            llvm::Value*      in   = range_of(from, converted);
            b_.CreateCondBr(in, back, done);
            b_.SetInsertPoint(back);
            llvm::Value* same = b_.CreateICmpEQ(convert(converted, to, from), v);
            b_.CreateBr(done);
            b_.SetInsertPoint(done);
            llvm::PHINode* phi = b_.CreatePHI(llvm::Type::getInt1Ty(ctx_), 2);
            phi->addIncoming(llvm::ConstantInt::getFalse(ctx_), test);
            phi->addIncoming(same, back);
            exact = phi;
        } else if (to->is_integer()) {     // float -> int: in range, and a whole number
            llvm::Value* in    = range_of(to, v);
            llvm::Value* cut   = v->getType()->isFP128Ty() ? b_.CreateCall(libc("truncf128", v->getType(), {v->getType()}), {v})
                                                               : b_.CreateUnaryIntrinsic(llvm::Intrinsic::trunc, v);
            llvm::Value* whole = b_.CreateFCmpOEQ(v, cut);
            exact     = b_.CreateAnd(in, whole);
            llvm::Value* safe = b_.CreateSelect(exact, v, llvm::ConstantFP::get(v->getType(), 0.0));
            converted = convert(safe, from, to);
        } else {                           // float -> float: NaN stays NaN; otherwise it must round-trip
            converted = convert(v, from, to);
            exact = b_.CreateOr(b_.CreateFCmpUNO(v, v), b_.CreateFCmpOEQ(convert(converted, to, from), v));
        }
        return option_of(option, converted, exact);
    }

    // `cast::dynamic<derived&>(base)`: walks the object's vtables, from its own class to the root,
    // looking for the target class's vtable (spec 07, 17).
    llvm::Value* dynamic_cast_value(CastExpr& c, const types::Type* target, const types::Type* option) {
        llvm::Value*      object = object_address(*c.operand);
        llvm::BasicBlock* test  = llvm::BasicBlock::Create(ctx_, "dynamic.test", cur_fn_);
        llvm::BasicBlock* up    = llvm::BasicBlock::Create(ctx_, "dynamic.up", cur_fn_);
        llvm::BasicBlock* found = llvm::BasicBlock::Create(ctx_, "dynamic.found", cur_fn_);
        llvm::BasicBlock* none  = llvm::BasicBlock::Create(ctx_, "dynamic.none", cur_fn_);
        llvm::BasicBlock* done  = llvm::BasicBlock::Create(ctx_, "dynamic.done", cur_fn_);
        llvm::AllocaInst* cur   = entry_alloca(ptr_type(), "dynamic.vtable");
        if (is_interface(types::strip_ref(c.operand->type))) {   // from an interface: its table has the type
            llvm::Value* table = b_.CreateExtractValue(object, 1);
            object             = b_.CreateExtractValue(object, 0);
            llvm::Value* id    = b_.CreateLoad(ptr_type(), b_.CreateConstGEP1_32(ptr_type(), table, 1));
            if (!has_vtable(target->symbol)) {
                b_.CreateCondBr(b_.CreateICmpEQ(id, type_id(target->symbol)), found, none);
            } else {
                llvm::BasicBlock* walk = llvm::BasicBlock::Create(ctx_, "dynamic.walk", cur_fn_);
                b_.CreateCondBr(b_.CreateIsNull(id), walk, none);   // null: the object has a vtable
                b_.SetInsertPoint(walk);
            }
        }
        if (!b_.GetInsertBlock()->getTerminator()) {
            b_.CreateStore(b_.CreateLoad(ptr_type(), object), cur);
            b_.CreateBr(test);
        }
        llvm::Constant* want = has_vtable(target->symbol) ? vtable_of(target->symbol)
                                                          : llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0));
        b_.SetInsertPoint(test);
        llvm::Value* v = b_.CreateLoad(ptr_type(), cur);
        b_.CreateCondBr(b_.CreateICmpEQ(v, want), found, up);
        b_.SetInsertPoint(up);
        llvm::Value* parent = b_.CreateLoad(ptr_type(), v);   // entry 0: the base class's vtable
        b_.CreateStore(parent, cur);
        b_.CreateCondBr(b_.CreateICmpEQ(parent, llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0))), none, test);
        b_.SetInsertPoint(found);
        llvm::Value* yes = option_of(option, object, llvm::ConstantInt::getTrue(ctx_));
        llvm::BasicBlock* found_end = b_.GetInsertBlock();
        b_.CreateBr(done);
        b_.SetInsertPoint(none);
        llvm::Value* no = option_of(option, object, llvm::ConstantInt::getFalse(ctx_));
        llvm::BasicBlock* none_end = b_.GetInsertBlock();
        b_.CreateBr(done);
        b_.SetInsertPoint(done);
        llvm::PHINode* phi = b_.CreatePHI(yes->getType(), 2);
        phi->addIncoming(yes, found_end);
        phi->addIncoming(no, none_end);
        return phi;
    }

    // `some(value)` when `present`, else `none`, of the option type `option`.
    llvm::Value* option_of(const types::Type* option, llvm::Value* value, llvm::Value* present) {
        const Symbol*     e    = option->symbol;
        llvm::Type*       et   = user_type(e);
        llvm::AllocaInst* slot = entry_alloca(et, "option");
        b_.CreateStore(llvm::Constant::getNullValue(et), slot);
        b_.CreateStore(value, payload_address(e, slot));
        store_tag(e, slot, b_.CreateSelect(present, tag_constant(alternative(e, "some")), tag_constant(alternative(e, "none"))));
        return b_.CreateLoad(et, slot);
    }

    // --- globals --------------------------------------------------------------------------

    std::map<const Symbol*, llvm::GlobalVariable*> globals_;
    std::map<const Symbol*, llvm::Function*>       global_inits_;
    std::deque<const Symbol*>                      global_queue_;

    // The storage of a global variable. Its initializer runs at startup (edax_init_globals).
    llvm::GlobalVariable* global_of(const Symbol* g) {
        if (auto it = globals_.find(g); it != globals_.end()) return it->second;
        cur_ = g->span;
        const auto&              v     = g->decl->as<VariableDecl>();
        const types::Type*       vt    = tc_.lower(*v.type, sema::substitution_of(g));
        if (needs_drop(vt)) unsupported(v.span, "global variables that need a destructor");
        llvm::Type*              t     = lower_type(vt);
        std::vector<std::string> parts = symbol_scope(g);
        parts.push_back(g->name);
        auto* gv = new llvm::GlobalVariable(*mod_, t, false, llvm::GlobalValue::InternalLinkage,
                                            llvm::Constant::getNullValue(t), "rg_" + mangle_parts(parts));
        if (v.storage == Storage::thread_local_) {   // one copy per thread (spec 27)
            gv->setThreadLocalMode(own_tls_model());
        }
        globals_[g] = gv;
        if (from_shared_library(g) || exported_by_library(g)) {   // one copy, in the shared library
            share_global(gv, g);
            if (v.storage == Storage::thread_local_) {
                gv->setThreadLocalMode(from_shared_library(g) ? llvm::GlobalValue::InitialExecTLSModel
                                                              : llvm::GlobalValue::GeneralDynamicTLSModel);
            }
            if (from_shared_library(g)) return gv;   // initialized by the library when it is loaded
        }
        global_queue_.push_back(g);
        return gv;
    }

    // Generates the initializer of a global as a function that stores its value.
    void gen_global(const Symbol* g) {
        const auto& v    = g->decl->as<VariableDecl>();
        cur_             = g->span;
        llvm::GlobalVariable* gv = globals_[g];
        if (v.storage == Storage::thread_local_) {   // the initial value is a constant: no code runs per thread
            if (v.init) {
                llvm::Constant* c = v.init->constant ? constant_of(*v.init->constant, gv->getValueType()) : nullptr;
                if (!c) unsupported(v.init->span, "this initial value of a thread_local (it must be a constant)");
                gv->setInitializer(c);
            }
            return;
        }
        llvm::Function* init = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {}, false),
                                                      llvm::Function::InternalLinkage, "init_" + gv->getName().str(), mod_.get());
        llvm::IRBuilderBase::InsertPoint saved_ip = b_.saveIP();
        llvm::Function*                  saved_fn = cur_fn_;
        llvm::Value*                     saved_self = self_;
        auto                             saved_locals = locals_;
        cur_fn_ = init;
        self_   = nullptr;
        locals_.clear();
        loops_.clear();
        b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", init));
        if (v.init) b_.CreateStore(gen_expr(*v.init), gv);
        b_.CreateRetVoid();
        cur_fn_ = saved_fn;
        self_   = saved_self;
        locals_ = saved_locals;
        b_.restoreIP(saved_ip);
        global_inits_[g] = init;
    }

    // The modules in the order they are initialized: imported modules first (as in the C backend).
    std::vector<const Module*> module_order() {
        std::vector<const Module*> out;
        std::set<const Module*>    seen;
        std::function<void(const Module*)> visit = [&](const Module* m) {
            if (!m || !seen.insert(m).second) return;
            for (const Module* i : m->header_imports) visit(i);
            for (const Module* i : m->source_imports) visit(i);
            out.push_back(m);
        };
        visit(program_.entry);
        for (auto& [name, m] : program_.modules) visit(m.get());
        return out;
    }

    // edax_init_globals: runs the initializers of the globals the program uses, module by module.
    llvm::Function* emit_init_globals() {
        llvm::Function* fn = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {}, false),
                                                    llvm::Function::InternalLinkage, "edax_init_globals", mod_.get());
        llvm::IRBuilderBase::InsertPoint saved_ip = b_.saveIP();
        b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn));
        for (const Module* m : module_order()) {
            for (const Symbol* s : m->item_order) {
                if (auto it = global_inits_.find(s); it != global_inits_.end()) b_.CreateCall(it->second);
            }
        }
        b_.CreateRetVoid();
        b_.restoreIP(saved_ip);
        return fn;
    }

    // --- destructors (spec 12) -------------------------------------------------------------
    //
    // A local whose type needs a destructor has a flag: set when the local takes its value,
    // cleared when the value is moved out. Leaving a scope, `return`, `break` and `continue`
    // destroy the owned locals that still hold a value, innermost scope first and in reverse
    // declaration order. A panic aborts without running destructors (spec 11).

    struct OwnedLocal {
        llvm::Value*      slot;   // the variable (or `this` of a consuming method)
        llvm::AllocaInst* flag;   // i1: nonzero while the variable owns its value
        llvm::Function*   drop;   // destroys a value of the variable's type
        const Symbol*      sym  = nullptr;
        const types::Type* type = nullptr;
    };
    // `this` of the consuming method being generated, owned by it (spec 06).
    std::optional<OwnedLocal> this_owned_;
    struct DropScope {
        std::vector<OwnedLocal> locals;
        bool                    loop = false;   // the body of a loop: break and continue stop here
    };
    std::vector<DropScope>                       scopes_;
    std::map<const Symbol*, OwnedLocal>          owned_;
    std::map<const Symbol*, bool>                needs_drop_memo_;
    std::map<const Symbol*, bool>                frozen_memo_;
    std::map<const Symbol*, llvm::Function*>     site_fns_;
    std::deque<const Symbol*>                    site_queue_;
    llvm::Value*                                 site_arg_ = nullptr;
    std::map<std::uint32_t, std::string>         file_levels_;
    std::set<llvm::StructType*>                  packed_types_;   // @std::layout{ .packed } structs
    bool                                         alias_attributes_ = true;
    std::map<const Symbol*, llvm::Function*>     drop_fns_;
    std::map<llvm::Function*, CSignature>        c_sigs_;   // extern functions: how C passes their structs
    std::deque<const Symbol*>                    drop_queue_;
    std::map<const Symbol*, llvm::Function*>     clone_fns_;
    std::set<const Expr*>                        converting_;
    std::deque<const Symbol*>                    clone_queue_;
    const types::Type*                           result_ = nullptr;   // the result of the function

    // A type needs a destructor when it has one, or when a field or payload does.
    bool needs_drop(const types::Type* t) {
        if (t && t->is(Kind::function)) return true;   // frees its captures
        if (t && t->is(Kind::array)) return needs_drop(t->inner);
        if (t && t->is(Kind::tuple)) {
            for (const types::Type* e : t->args) {
                if (needs_drop(e)) return true;
            }
            return false;
        }
        if (t && t->is(Kind::closure)) {               // a lambda owns its value captures (spec 18)
            const LambdaExpr* l = tc_.lambda_of(t);
            if (!l) return false;
            for (const Capture& c : l->captures) {
                if (c.symbol && c.symbol->type && !c.symbol->type->is(Kind::reference) && needs_drop(c.symbol->type)) return true;
            }
            return false;
        }
        if (!t || !t->is(Kind::user) || !t->symbol) return false;
        return symbol_needs_drop(t->symbol);
    }

    bool symbol_needs_drop(const Symbol* s) {
        if (auto it = needs_drop_memo_.find(s); it != needs_drop_memo_.end()) return it->second;
        needs_drop_memo_[s] = false;   // a type cannot contain itself by value; this guards the scan
        bool result         = false;
        if (s->kind == SymbolKind::enum_) {
            for (const Symbol* a : s->member_order) {
                if (a->alternative && a->alternative->payload && needs_drop(payload_of(a))) result = true;
            }
        } else {
            for (const auto& [name, overloads] : s->members) {
                for (const Symbol* m : overloads) {
                    if (m->function() && m->function()->is_destructor) result = true;
                }
            }
            for (const Symbol* f : fields_of(s)) {
                if (needs_drop(field_type(f))) result = true;
            }
            if (const Symbol* base = s->base_class(); base && symbol_needs_drop(base)) result = true;
        }
        needs_drop_memo_[s] = result;
        return result;
    }

    // The function that destroys a value of user type `t`; it is generated later.
    llvm::Function* drop_function(const types::Type* t) {
        if (t->is(Kind::function)) return t->bits > 0 ? inline_function_drop(t) : function_drop();
        if (t->is(Kind::closure)) return closure_drop(t);
        if (t->is(Kind::array)) return array_function(t, false);
        if (t->is(Kind::tuple)) return tuple_function(t, false);
        const Symbol* s = t->symbol;
        if (auto it = drop_fns_.find(s); it != drop_fns_.end()) return it->second;
        std::vector<std::string> parts = symbol_scope(s);
        parts.push_back(s->name);
        if (s->template_of) parts.push_back(instance_tag(s));
        auto* fn = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {llvm::PointerType::get(ctx_, 0)}, false),
                                          llvm::Function::InternalLinkage, "rd_" + mangle_parts(parts), mod_.get());
        set_unit(fn, unit_of(s));
        if (is_std_function(s)) {   // a library type's destructor: optimized like the library (spec 34)
            if (std::string level = mode_setting("std_optimization"); !level.empty()) fn->addFnAttr("edax-optimize", level);
        }
        drop_fns_[s] = fn;
        drop_queue_.push_back(s);
        return fn;
    }

    // Destroys a `std::inline_function`: destroys its captures in place (spec 34).
    llvm::Function* inline_function_drop(const types::Type* t) {
        std::string name = "edax_inline_function_drop." + std::to_string(t->bits);
        if (llvm::Function* f = mod_->getFunction(name)) return f;
        auto* fn = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {ptr_type()}, false),
                                          llvm::Function::InternalLinkage, name, mod_.get());
        auto*             entry = llvm::BasicBlock::Create(ctx_, "entry", fn);
        auto*             call  = llvm::BasicBlock::Create(ctx_, "drop", fn);
        auto*             done  = llvm::BasicBlock::Create(ctx_, "done", fn);
        llvm::IRBuilder<> b(entry);
        auto*             st   = llvm::cast<llvm::StructType>(lower_type(t));
        llvm::Value*      drop = b.CreateLoad(ptr_type(), b.CreateStructGEP(st, fn->getArg(0), 2));
        b.CreateCondBr(b.CreateICmpNE(drop, llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0))), call, done);
        b.SetInsertPoint(call);
        b.CreateCall(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {ptr_type()}, false), drop, {b.CreateStructGEP(st, fn->getArg(0), 0)});
        b.CreateBr(done);
        b.SetInsertPoint(done);
        b.CreateRetVoid();
        return fn;
    }

    // Destroys a `std::function`: destroys its captures (the closure's drop function, if it has
    // one) and frees them.
    llvm::Function* function_drop() {
        if (llvm::Function* f = mod_->getFunction("edax_function_drop")) return f;
        auto* fn = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {ptr_type()}, false),
                                          llvm::Function::InternalLinkage, "edax_function_drop", mod_.get());
        auto*             entry = llvm::BasicBlock::Create(ctx_, "entry", fn);
        auto*             call  = llvm::BasicBlock::Create(ctx_, "drop", fn);
        auto*             done  = llvm::BasicBlock::Create(ctx_, "free", fn);
        llvm::IRBuilder<> b(entry);
        auto*             st   = llvm::StructType::get(ctx_, {ptr_type(), ptr_type(), ptr_type()});
        llvm::Value*      env  = b.CreateLoad(ptr_type(), b.CreateStructGEP(st, fn->getArg(0), 0));
        llvm::Value*      drop = b.CreateLoad(ptr_type(), b.CreateStructGEP(st, fn->getArg(0), 2));
        b.CreateCondBr(b.CreateICmpNE(drop, llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0))), call, done);
        b.SetInsertPoint(call);
        b.CreateCall(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {ptr_type()}, false), drop, {env});
        b.CreateBr(done);
        b.SetInsertPoint(done);
        b.CreateCall(libc("free", llvm::Type::getVoidTy(ctx_), {ptr_type()}), {env});
        b.CreateRetVoid();
        return fn;
    }

    std::map<std::pair<const types::Type*, bool>, llvm::Function*> array_fns_;

    // Destroys (`clone` false: `(ptr)`) or clones (`(ptr dst, ptr src)`) every element of an
    // inline array, in a loop.
    llvm::Function* array_function(const types::Type* t, bool clone) {
        auto key = std::make_pair(t, clone);
        if (auto it = array_fns_.find(key); it != array_fns_.end()) return it->second;
        std::vector<llvm::Type*> params{ptr_type()};
        if (clone) params.push_back(ptr_type());
        auto* fn = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), params, false),
                                          llvm::Function::InternalLinkage,
                                          (clone ? "rac_" : "rad_") + short_hash(type_key(t)), mod_.get());
        array_fns_[key] = fn;
        llvm::IRBuilderBase::InsertPoint saved = b_.saveIP();
        llvm::Function*                  saved_fn = cur_fn_;
        cur_fn_ = fn;
        auto*        i64   = llvm::Type::getInt64Ty(ctx_);
        llvm::Type*  et    = lower_type(t->inner);
        auto*        entry = llvm::BasicBlock::Create(ctx_, "entry", fn);
        auto*        loop  = llvm::BasicBlock::Create(ctx_, "loop", fn);
        auto*        body  = llvm::BasicBlock::Create(ctx_, "body", fn);
        auto*        done  = llvm::BasicBlock::Create(ctx_, "done", fn);
        b_.SetInsertPoint(entry);
        llvm::AllocaInst* iv = b_.CreateAlloca(i64);
        b_.CreateStore(llvm::ConstantInt::get(i64, 0), iv);
        b_.CreateBr(loop);
        b_.SetInsertPoint(loop);
        llvm::Value* i = b_.CreateLoad(i64, iv);
        b_.CreateCondBr(b_.CreateICmpULT(i, llvm::ConstantInt::get(i64, tc_.array_count(t))), body, done);
        b_.SetInsertPoint(body);
        llvm::Value* dst = b_.CreateGEP(et, fn->getArg(0), i);
        if (clone) {
            b_.CreateStore(clone_value(t->inner, b_.CreateGEP(et, fn->getArg(1), i)), dst);
        } else if (needs_drop(t->inner)) {
            b_.CreateCall(drop_function(t->inner), {dst});
        }
        b_.CreateStore(b_.CreateAdd(i, llvm::ConstantInt::get(i64, 1)), iv);
        b_.CreateBr(loop);
        b_.SetInsertPoint(done);
        b_.CreateRetVoid();
        cur_fn_ = saved_fn;
        b_.restoreIP(saved);
        return fn;
    }

    // Destroys (or, with `clone`, clones into place) the elements of a tuple of type `t`.
    llvm::Function* tuple_function(const types::Type* t, bool clone) {
        auto key = std::make_pair(t, clone);
        if (auto it = tuple_fns_.find(key); it != tuple_fns_.end()) return it->second;
        std::vector<llvm::Type*> params{ptr_type()};
        if (clone) params.push_back(ptr_type());
        auto* fn = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), params, false),
                                          llvm::Function::InternalLinkage,
                                          (clone ? "rtc_" : "rtd_") + short_hash(type_key(t)), mod_.get());
        tuple_fns_[key] = fn;
        llvm::IRBuilderBase::InsertPoint saved    = b_.saveIP();
        llvm::DebugLoc                   saved_loc = b_.getCurrentDebugLocation();
        llvm::Function*                  saved_fn = cur_fn_;
        cur_fn_ = fn;
        b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn));
        b_.SetCurrentDebugLocation(llvm::DebugLoc());
        llvm::Type* st = lower_type(t);
        for (std::size_t i = 0; i < t->args.size(); ++i) {
            const types::Type* et  = t->args[i];
            llvm::Value*       dst = b_.CreateStructGEP(st, fn->getArg(0), static_cast<unsigned>(i));
            if (clone) {
                b_.CreateStore(clone_value(et, b_.CreateStructGEP(st, fn->getArg(1), static_cast<unsigned>(i))), dst);
            } else if (needs_drop(et)) {
                b_.CreateCall(drop_function(et), {dst});
            }
        }
        b_.CreateRetVoid();
        cur_fn_ = saved_fn;
        b_.restoreIP(saved);
        b_.SetCurrentDebugLocation(saved_loc);
        return fn;
    }
    std::map<std::pair<const types::Type*, bool>, llvm::Function*> tuple_fns_;

    std::map<const types::Type*, llvm::Function*> closure_drops_;

    // Destroys the value captures of a lambda that need it.
    llvm::Function* closure_drop(const types::Type* t) {
        if (auto it = closure_drops_.find(t); it != closure_drops_.end()) return it->second;
        auto* fn = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {ptr_type()}, false),
                                          llvm::Function::InternalLinkage, "rld_" + short_hash(type_key(t)), mod_.get());
        closure_drops_[t] = fn;
        if (const LambdaExpr* l = tc_.lambda_of(t); l && lambda_fns_.count(l)) inherit_unit(fn, lambda_fns_.at(l));
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", fn));
        llvm::StructType* st = closure_type(t);
        const LambdaExpr* l  = tc_.lambda_of(t);
        for (std::size_t i = 0; i < l->captures.size(); ++i) {
            const types::Type* ct = l->captures[i].symbol ? l->captures[i].symbol->type : nullptr;
            if (!ct || ct->is(Kind::reference) || !needs_drop(ct)) continue;
            b.CreateCall(drop_function(ct), {b.CreateStructGEP(st, fn->getArg(0), static_cast<unsigned>(i))});
        }
        b.CreateRetVoid();
        return fn;
    }

    // A lambda as a `std::function` (its captures move to the heap) or as a `std::fn_ptr` (a
    // function without the captures argument), spec 18.
    llvm::Value* callable_from_lambda(Expr& e, const types::Type* to) {
        const types::Type*     ct = types::strip_ref(e.type);
        const ast::LambdaExpr* l  = tc_.lambda_of(ct);
        llvm::Value*           v  = gen_expr(e);
        llvm::Function*        fn = lambda_function(*l);
        if (to->is(Kind::function) && to->bits > 0) {   // std::inline_function: the captures in place (spec 34)
            llvm::Type*       ft   = lower_type(to);
            llvm::AllocaInst* slot = entry_alloca(ft, "inline_function");
            b_.CreateStore(llvm::Constant::getNullValue(ft), slot);
            b_.CreateStore(v, b_.CreateStructGEP(ft, slot, 0));
            b_.CreateStore(fn, b_.CreateStructGEP(ft, slot, 1));
            llvm::Value* drop = needs_drop(ct) ? static_cast<llvm::Value*>(closure_drop(ct))
                                               : llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0));
            b_.CreateStore(drop, b_.CreateStructGEP(ft, slot, 2));
            return b_.CreateLoad(ft, slot);
        }
        if (to->is(Kind::function)) {
            std::uint64_t size = std::max<std::uint64_t>(mod_->getDataLayout().getTypeAllocSize(v->getType()), 1);
            llvm::Value*  env  = b_.CreateCall(libc("malloc", ptr_type(), {llvm::Type::getInt64Ty(ctx_)}),
                                               {llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), size)});
            check(b_.CreateICmpNE(env, llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0))), e.span, "out of memory");
            b_.CreateStore(v, env);
            llvm::Value* r = llvm::UndefValue::get(lower_type(to));
            r = b_.CreateInsertValue(r, env, 0);
            r = b_.CreateInsertValue(r, fn, 1);
            llvm::Value* drop = needs_drop(ct) ? static_cast<llvm::Value*>(closure_drop(ct))
                                               : llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0));
            return b_.CreateInsertValue(r, drop, 2);
        }
        std::string name = fn->getName().str() + ".ptr";
        if (llvm::Function* w = mod_->getFunction(name)) return w;
        std::vector<llvm::Type*> params(fn->getFunctionType()->param_begin() + 1, fn->getFunctionType()->param_end());
        auto* w = llvm::Function::Create(llvm::FunctionType::get(fn->getReturnType(), params, false), llvm::Function::InternalLinkage,
                                         name, mod_.get());
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", w));
        std::vector<llvm::Value*> args{llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0))};
        for (llvm::Argument& a : w->args()) args.push_back(&a);
        llvm::Value* r = b.CreateCall(fn, args);
        if (w->getReturnType()->isVoidTy()) b.CreateRetVoid();
        else b.CreateRet(r);
        return w;
    }

    // Calls a `std::function` or `std::fn_ptr` value.
    llvm::Value* callable_call(Expr& callee, const types::Type* t, const std::vector<Expr*>& args) {
        llvm::Value* v = gen_expr(callee);
        std::vector<llvm::Value*> values;
        std::vector<llvm::Type*>  params;
        llvm::Value* target = v;
        if (t->is(Kind::function) && t->bits > 0) {   // the captures are in the value: call with their address
            llvm::AllocaInst* slot = entry_alloca(v->getType(), "inline_function.call");
            b_.CreateStore(v, slot);
            values.push_back(b_.CreateStructGEP(v->getType(), slot, 0));
            params.push_back(ptr_type());
            target = b_.CreateExtractValue(v, 1);
        } else if (t->is(Kind::function)) {
            values.push_back(b_.CreateExtractValue(v, 0));
            params.push_back(ptr_type());
            target = b_.CreateExtractValue(v, 1);
        }
        for (std::size_t i = 0; i < args.size() && i + 1 < t->args.size(); ++i) {
            if (t->args[i]->is(Kind::reference)) {
                values.push_back(reference_to(*args[i]));
                params.push_back(ptr_type());
            } else {
                values.push_back(gen_expr(*args[i]));
                params.push_back(lower_type(t->args[i]));
                moved_out(*args[i]);
            }
        }
        const types::Type* rt  = t->args.back();
        llvm::Type*        ret = is_void(rt) ? llvm::Type::getVoidTy(ctx_) : rt->is(Kind::reference) ? ptr_type() : lower_type(rt);
        llvm::Value* r = b_.CreateCall(llvm::FunctionType::get(ret, params, false), target, values);
        if (rt->is(Kind::reference)) return b_.CreateLoad(lower_type(types::strip_ref(rt)), r);
        return r;
    }

    // `x.clone()` for the object of type `t` at `src` (spec 05, 12): copy types are copied, a class
    // calls its `clone()` method, and structs and enums clone what they contain.
    llvm::Value* clone_value(const types::Type* t, llvm::Value* src) {
        if (!needs_drop(t)) return b_.CreateLoad(lower_type(t), src);
        if (t->is(Kind::tuple)) {
            llvm::Value* dst = entry_alloca(lower_type(t), "clone");
            b_.CreateCall(tuple_function(t, true), {dst, src});
            return b_.CreateLoad(lower_type(t), dst);
        }
        if (t->is(Kind::array)) {
            llvm::Value* dst = entry_alloca(lower_type(t), "clone");
            b_.CreateCall(array_function(t, true), {dst, src});
            return b_.CreateLoad(lower_type(t), dst);
        }
        const Symbol* s = t->symbol;
        if (auto it = s->members.find("clone"); it != s->members.end()) {
            for (const Symbol* m : it->second) {
                const FunctionDecl* f = body_decl(m);
                if (f && f->params.empty() && !f->is_static) return b_.CreateCall(function_of(m), {src});
            }
        }
        llvm::Type*  lt  = lower_type(t);
        llvm::Value* dst = entry_alloca(lt, "clone");
        b_.CreateCall(clone_function(t), {dst, src});
        return b_.CreateLoad(lt, dst);
    }

    // The function that clones a struct or enum of type `t` into place: `(ptr dst, ptr src)`.
    llvm::Function* clone_function(const types::Type* t) {
        const Symbol* s = t->symbol;
        if (auto it = clone_fns_.find(s); it != clone_fns_.end()) return it->second;
        std::vector<std::string> parts = symbol_scope(s);
        parts.push_back(s->name);
        if (s->template_of) parts.push_back(instance_tag(s));
        auto* ptr = llvm::PointerType::get(ctx_, 0);
        auto* fn  = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {ptr, ptr}, false),
                                           llvm::Function::InternalLinkage, "rc_" + mangle_parts(parts), mod_.get());
        set_unit(fn, unit_of(s));
        if (is_std_function(s)) {   // optimized like the library (spec 34)
            if (std::string level = mode_setting("std_optimization"); !level.empty()) fn->addFnAttr("edax-optimize", level);
        }
        clone_fns_[s] = fn;
        clone_queue_.push_back(s);
        return fn;
    }

    void gen_clone(const Symbol* s) {
        llvm::Function*                  fn       = clone_fns_[s];
        inspect_                                  = false;
        llvm::IRBuilderBase::InsertPoint saved_ip = b_.saveIP();
        llvm::Function*                  saved_fn = cur_fn_;
        cur_fn_ = fn;
        llvm::Value* dst = fn->getArg(0);
        llvm::Value* src = fn->getArg(1);
        b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn));
        llvm::Type* lt = user_type(s);
        if (s->kind == SymbolKind::enum_) {
            b_.CreateStore(b_.CreateLoad(lt, src), dst);   // the tag and plain payloads
            llvm::Value*      tag  = load_tag(s, src);
            llvm::BasicBlock* done = llvm::BasicBlock::Create(ctx_, "clone.done", fn);
            llvm::SwitchInst* sw   = b_.CreateSwitch(tag, done);
            for (const Symbol* a : s->member_order) {
                if (!carries(a)) continue;
                const types::Type* p = payload_of(a);
                if (!needs_drop(p)) continue;
                llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx_, "clone.case", fn);
                sw->addCase(tag_constant(a), bb);
                b_.SetInsertPoint(bb);
                b_.CreateStore(clone_value(p, payload_address(s, src)), payload_address(s, dst));
                b_.CreateBr(done);
            }
            b_.SetInsertPoint(done);
        } else {
            for (const Symbol* f : fields_of(s)) {
                unsigned i = field_index(f);
                b_.CreateStore(clone_value(field_type(f), b_.CreateStructGEP(lt, src, i)), b_.CreateStructGEP(lt, dst, i));
            }
        }
        b_.CreateRetVoid();
        cur_fn_ = saved_fn;
        b_.restoreIP(saved_ip);
    }

    // Runs the destructor (if any), then destroys the fields in reverse declaration order, or the
    // live payload of an enum.
    void gen_drop(const Symbol* s) {
        llvm::Function*                  fn       = drop_fns_[s];
        fn_names_[fn] = "~" + types::qualified_name(tc_.user(s));
        inspect_                                  = false;
        llvm::IRBuilderBase::InsertPoint saved_ip = b_.saveIP();
        llvm::Function*                  saved_fn = cur_fn_;
        llvm::Value*                     saved_self = self_;
        auto                             saved_locals = locals_;
        auto                             saved_scopes = scopes_;
        auto                             saved_loops  = loops_;
        cur_fn_ = fn;
        self_   = nullptr;
        locals_.clear();
        scopes_.clear();
        loops_.clear();
        llvm::Value* self = fn->getArg(0);
        b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", fn));
        if (s->kind == SymbolKind::enum_) {
            llvm::Value* tag = load_tag(s, self);
            llvm::BasicBlock* done = llvm::BasicBlock::Create(ctx_, "drop.done", fn);
            llvm::SwitchInst* sw   = b_.CreateSwitch(tag, done);
            for (const Symbol* a : s->member_order) {
                if (!carries(a)) continue;
                const types::Type* p = payload_of(a);
                if (!needs_drop(p)) continue;
                llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx_, "drop.case", fn);
                sw->addCase(tag_constant(a), bb);
                b_.SetInsertPoint(bb);
                b_.CreateCall(drop_function(p), {payload_address(s, self)});
                b_.CreateBr(done);
            }
            b_.SetInsertPoint(done);
        } else {
            llvm::Type* st = user_type(s);
            for (const auto& [mname, overloads] : s->members) {
                for (const Symbol* m : overloads) {
                    if (m->function() && m->function()->is_destructor) b_.CreateCall(function_of(m), {self});
                }
            }
            std::vector<const Symbol*> fields = fields_of(s);
            for (auto it = fields.rbegin(); it != fields.rend(); ++it) {
                const types::Type* ft = field_type(*it);
                if (needs_drop(ft)) b_.CreateCall(drop_function(ft), {b_.CreateStructGEP(st, self, field_index(*it))});
            }
            if (const Symbol* base = s->base_class()) {   // then the base part (spec 07)
                const types::Type* bt = tc_.user(base);
                if (needs_drop(bt)) b_.CreateCall(drop_function(bt), {self});
            }
        }
        b_.CreateRetVoid();
        cur_fn_    = saved_fn;
        self_      = saved_self;
        locals_    = saved_locals;
        scopes_    = saved_scopes;
        loops_     = saved_loops;
        b_.restoreIP(saved_ip);
    }

    void open_scope(bool loop = false) { scopes_.push_back(DropScope{{}, loop}); }

    // Ends the innermost scope: its owned locals are destroyed, last declared first.
    void close_scope() {
        if (scopes_.empty()) return;
        if (!terminated()) {
            record_drops(scopes_.size() - 1);
            destroy_from(scopes_.size() - 1);
        }
        scopes_.pop_back();
    }

    // Destroys the owned locals of the scopes from `first` inwards without leaving them (before
    // `return`, `break` and `continue`).
    void destroy_from(std::size_t first) {
        for (std::size_t i = scopes_.size(); i-- > first;) {
            const auto& locals = scopes_[i].locals;
            for (auto it = locals.rbegin(); it != locals.rend(); ++it) drop_if(it->flag, it->drop, it->slot);
        }
    }

    // The index of the innermost loop body (0 when there is none).
    std::size_t loop_scope() const {
        for (std::size_t i = scopes_.size(); i-- > 0;) {
            if (scopes_[i].loop) return i;
        }
        return 0;
    }

    // Calls `drop` on the object at `addr` when `flag` is set.
    void drop_if(llvm::AllocaInst* flag, llvm::Function* drop, llvm::Value* addr) {
        llvm::BasicBlock* yes = llvm::BasicBlock::Create(ctx_, "drop", cur_fn_);
        llvm::BasicBlock* no  = llvm::BasicBlock::Create(ctx_, "drop.done", cur_fn_);
        b_.CreateCondBr(b_.CreateLoad(llvm::Type::getInt1Ty(ctx_), flag), yes, no);
        b_.SetInsertPoint(yes);
        b_.CreateCall(drop, {addr});
        b_.CreateBr(no);
        b_.SetInsertPoint(no);
    }

    // Declares the owned local `sym` (stored at `slot`). `owns_now` is true for parameters.
    void own(const Symbol* sym, llvm::AllocaInst* slot, const types::Type* t, bool owns_now) {
        llvm::AllocaInst* flag = entry_alloca(llvm::Type::getInt1Ty(ctx_), "df_" + sym->name);
        b_.CreateStore(llvm::ConstantInt::getBool(ctx_, owns_now), flag);
        OwnedLocal o{slot, flag, drop_function(t), sym, t};
        if (inspect_) decl_depth_[sym] = {cond_depth_, trips_.size()};
        scopes_.back().locals.push_back(o);
        owned_[sym] = o;
    }

    void set_flag(const Symbol* sym, bool value) {
        b_.CreateStore(llvm::ConstantInt::getBool(ctx_, value), owned_[sym].flag);
    }

    // The owned local that `e` names, looking through `move`; null for anything else.
    const OwnedLocal* owned_path(const Expr& e) {
        const Expr* x = &e;
        if (x->kind == ExprKind::unary && x->as<UnaryExpr>().op == UnaryOp::move) x = x->as<UnaryExpr>().operand.get();
        if (x->kind == ExprKind::this_ && this_owned_) return &*this_owned_;   // `this` of a consuming method
        if (x->kind != ExprKind::path) return nullptr;
        auto it = owned_.find(x->as<PathExpr>().symbol);
        return it == owned_.end() ? nullptr : &it->second;
    }

    // After a value was moved out of `e`, the local no longer owns it.
    void moved_out(const Expr& e) {
        if (e.conversion == Expr::Conversion::string_to_str) return;   // a borrow, not a move
        if (const OwnedLocal* o = owned_path(e)) {
            b_.CreateStore(llvm::ConstantInt::getFalse(ctx_), o->flag);
            if (inspect_ && o->sym) {
                if (e.kind == ExprKind::unary) {
                    const std::string& n = o->sym->name;
                    insight(e.span, "moves", "move", "range", std::format("move {0}: ownership leaves {0}", n),
                            std::format("A move is a bitwise copy of {}. The destination owns the `{}` now; `{}` can't be used afterwards "
                                        "and is not dropped at the end of its scope (spec 12).",
                                        bytes_text(type_bytes(o->type)), types::to_string(o->type), n));
                }
                record_move(o->sym);
            }
        }
    }

    // Evaluates `e` for its effects. A value that needs a destructor is destroyed at once.
    void discard(Expr& e) {
        if (!needs_drop(e.type) || e.kind == ExprKind::path) {
            gen_expr(e);
            return;
        }
        llvm::Value* v = gen_expr(e);
        if (!v || terminated()) return;
        llvm::AllocaInst* slot = entry_alloca(v->getType(), "discarded");
        b_.CreateStore(v, slot);
        b_.CreateCall(drop_function(e.type), {slot});
    }

    // --- insights (`edaxc insights`): what the compiler does implicitly ---------------------
    //
    // While a function of the inspected file is generated, the hooks below record events: stack
    // slots, calls, drops, moves, copies, conversions, dispatch and checks. Calls and drops only
    // become heap insights at the end (finish_insights), once the whole module exists and it is
    // known which functions reach the allocator.

    struct SlotInfo {
        std::string name;
        std::string type;
        std::string what;   // parameter, variable, loop variable, temporary, drop flag
    };
    // What is known about the capacity of a local `darray` / `string` (spec 20).
    struct Capacity {
        bool        known = false;
        bool        text  = false;   // a std::string (inline up to 23 bytes)
        std::uint64_t cap = 0, len_min = 0, len_max = 0;
    };
    struct PendingAlloc {
        Span            span;
        llvm::Function* fn = nullptr;     // what is called (reaches the allocator, or not)
        std::string     message;          // "" = "may allocate"; otherwise the certain allocation
        bool            in_loop = false;  // a growing call in a loop: suggest reserve()
        std::string     display;          // the called function's name
    };
    struct PendingDrop {
        Span            span;
        std::string     kind;   // drop, exit-drop
        std::string     var;
        std::string     type;
        llvm::Function* drop = nullptr;
        bool            maybe_moved = false;
        bool            no_heap = false;    // known to own no heap memory (empty, or text inline)
        std::uint64_t   size = 0;
    };

    bool                                        inspect_ = false;
    FileId                                      inspect_file_{};
    int                                         cond_depth_ = 0;
    std::vector<std::optional<std::uint64_t>>   trips_;   // per open loop: its constant trip count
    std::map<const llvm::AllocaInst*, SlotInfo> slot_info_;
    std::map<const Symbol*, Capacity>           caps_;
    std::map<const Symbol*, std::pair<int, std::size_t>> decl_depth_;   // cond depth and loop depth at declaration
    std::map<const Symbol*, int>                moved_;   // 1: moved on every path, 2: maybe
    std::vector<PendingAlloc>                   allocs_;
    std::vector<PendingDrop>                    drops_;
    std::vector<Insight>                        found_;
    Span                                        drop_site_{};
    std::string                                 drop_kind_ = "drop";

    void insight(Span span, std::string category, std::string kind, std::string placement, std::string message,
                 std::string detail) {
        found_.push_back(Insight{std::move(category), std::move(kind), span, std::move(placement), std::move(message), std::move(detail)});
    }

    std::uint64_t byte_size(llvm::Type* t) { return mod_->getDataLayout().getTypeAllocSize(t).getFixedValue(); }
    std::uint64_t type_bytes(const types::Type* t) { return t ? byte_size(lower_type(t)) : 0; }

    static std::string bytes_text(std::uint64_t n) { return std::format("{} B", n); }

    // `std::darray<u32>::push`, `geometry::length`: how a function is shown (with its namespace).
    std::string display_name(const Symbol* s) {
        if (!s) return "?";
        if (const FunctionDecl* f = s->function(); f && f->is_extern) return s->name;   // a C function: C's own name
        if (s->parent && (s->parent->kind == SymbolKind::struct_ || s->parent->kind == SymbolKind::class_ ||
                          s->parent->kind == SymbolKind::enum_)) {
            return types::qualified_name(tc_.user(s->parent)) + "::" + s->name;
        }
        return s->key();
    }

    // The span of the name a call is made through: `push` in `list.push(7)`, `from` in
    // `std::string::from(x)`.
    static Span callee_name(const CallExpr& c) {
        const Expr& callee = *c.callee;
        if (callee.kind == ExprKind::member && !callee.as<MemberExpr>().name.segments.empty()) {
            return callee.as<MemberExpr>().name.segments.back().span;
        }
        if (callee.kind == ExprKind::path && !callee.as<PathExpr>().path.segments.empty()) {
            return callee.as<PathExpr>().path.segments.back().span;
        }
        return callee.span;
    }

    // The value of an integer constant expression, if it is one.
    static std::optional<std::uint64_t> constant_count(const Expr& e) {
        if (e.kind == ExprKind::int_literal) return static_cast<std::uint64_t>(e.as<LiteralExpr>().int_value);
        if (e.constant && e.constant->kind == ct::Value::Kind::integer) return static_cast<std::uint64_t>(e.constant->integer);
        return std::nullopt;
    }

    // How often code at this point runs per run of the function: the product of the constant
    // trip counts of the open loops (nullopt when one is not constant).
    std::optional<std::uint64_t> repeat() const {
        std::uint64_t n = 1;
        for (const auto& t : trips_) {
            if (!t) return std::nullopt;
            n *= *t;
        }
        return n;
    }

    // The tracked capacity of the local container that `e` names, or null.
    Capacity* tracked(const Expr& e) {
        if (e.kind != ExprKind::path) return nullptr;
        auto it = caps_.find(e.as<PathExpr>().symbol);
        return it == caps_.end() ? nullptr : &it->second;
    }

    bool is_darray(const types::Type* t) const {
        if (!t || !t->is(Kind::user) || !program_.std_module) return false;
        return t->symbol->template_of == program_.std_module->item("std::darray");
    }

    // A local container starts with a known capacity: `default`, `with_capacity(n)`, `from(literal)`.
    void track_init(const Symbol* var, const Expr* init) {
        if (!inspect_ || !var) return;
        const types::Type* t = var->type;
        bool text = is_string(t) && !types::is_std_instance(t, "string_with");   // 23 bytes inline: std::string only
        if (!text && !is_darray(t)) return;
        Capacity c;
        c.text = text;
        if (!init || init->kind == ExprKind::default_) {
            c.known = true;   // `default`
            c.cap   = text ? 23 : 0;
        } else if (init->kind == ExprKind::call) {
            const auto& call = init->as<CallExpr>();
            std::string name = call.callee->kind == ExprKind::path && !call.callee->as<PathExpr>().path.segments.empty()
                                   ? call.callee->as<PathExpr>().path.segments.back().name : "";
            if (name == "with_capacity" && call.args.size() == 1) {
                if (auto n = constant_count(*call.args[0])) {
                    c.known = true;
                    c.cap   = text ? std::max<std::uint64_t>(*n, 23) : *n;
                }
            } else if (name == "from" && text && call.args.size() == 1 && call.args[0]->kind == ExprKind::string_literal) {
                std::uint64_t n = call.args[0]->as<LiteralExpr>().value.size();
                c.known = true;
                c.cap   = n <= 23 ? 23 : std::max<std::uint64_t>(n, 46);
                c.len_min = c.len_max = n;
            }
        }
        caps_[var] = c;
    }

    // The certain-allocation message of a call on a tracked container, "" for "may allocate", or
    // "none" when it surely does not allocate. Updates what is known.
    std::string capacity_effect(Capacity& c, const std::string& method, const CallExpr& call) {
        std::optional<std::uint64_t> times = repeat();
        bool                         sure  = cond_depth_ == 0;
        std::uint64_t                most  = times.value_or(0);
        std::uint64_t                least = sure && times ? *times : 0;
        auto grow_text = [](std::uint64_t cap, std::uint64_t need) { return std::max({cap * 2, std::uint64_t{32}, need}); };
        if (!c.known) return "";
        if (method == "push" || method == "insert" || (method == "push_str" && c.text)) {
            std::uint64_t add = 1;
            if (method == "push_str") {
                if (call.args.size() != 1 || call.args[0]->kind != ExprKind::string_literal) {
                    c.known = false;
                    return "";
                }
                add = call.args[0]->as<LiteralExpr>().value.size();
            } else if (method == "push" && c.text) {
                add = 4;   // a char: up to 4 bytes of UTF-8
            }
            if (!times) {
                c.known = false;
                return "";
            }
            if (c.len_max + most * add <= c.cap) {
                c.len_max += most * add;
                c.len_min += least * add;
                return "none";
            }
            if (most == 1 && least == 1 && c.len_min == c.len_max && !c.text && c.len_min == c.cap) {
                std::uint64_t next = c.cap == 0 ? 4 : c.cap * 2;
                std::string   m    = std::format("allocates: full at {}, capacity {} → {}", c.len_min, c.cap, next);
                c.cap = next;
                c.len_min += 1;
                c.len_max += 1;
                return m;
            }
            if (most == 1 && least == 1 && c.len_min == c.len_max && c.text) {
                std::uint64_t next = grow_text(c.cap, c.len_min + add);
                std::string   m    = std::format("allocates: capacity {} → {}", c.cap, next);
                c.cap = next;
                c.len_min += add;
                c.len_max += add;
                return m;
            }
            c.known = false;
            return "";
        }
        if (method == "reserve" && call.args.size() == 1) {
            auto n = constant_count(*call.args[0]);
            if (!n || cond_depth_ > 0 || !trips_.empty()) {
                c.known = false;
                return "";
            }
            if (*n <= c.cap) return "none";
            std::uint64_t next = c.text ? grow_text(c.cap, *n) : *n;
            std::string   m    = std::format("allocates: capacity {} → {}", c.cap, next);
            c.cap = next;
            return m;
        }
        static const std::set<std::string> reading{"length", "is_empty", "capacity", "get", "contains", "as_span", "as_str", "clone",
                                                   "sort", "sort_by", "sort_by_key", "get_mut"};
        if (reading.contains(method)) return "none";
        if (method == "clear") {
            c.len_min = c.len_max = 0;
            return "none";
        }
        if (method == "pop" || method == "remove" || method == "swap_remove") {
            if (c.len_min > 0) c.len_min -= 1;
            if (c.len_max > 0 && cond_depth_ == 0 && trips_.empty()) c.len_max -= 1;
            return "none";
        }
        if (method == "truncate" || method == "retain") {
            c.len_min = 0;
            return "none";
        }
        c.known = false;
        return "";
    }

    // A call of `s` (as `fn`) at `name_span` in the inspected code.
    void record_call(const Symbol* s, llvm::Function* fn, const CallExpr* call, Span name_span) {
        if (!inspect_ || !fn) return;
        PendingAlloc a{name_span, fn, "", false, display_name(s)};
        std::string method = s ? s->name : "";
        bool growing = method == "push" || method == "push_str" || method == "insert" || method == "resize";
        if (call && call->callee->kind == ExprKind::member) {
            const auto& me = call->callee->as<MemberExpr>();
            if (Capacity* c = tracked(*me.object)) {
                std::string effect = capacity_effect(*c, method, *call);
                if (effect == "none") return;
                a.message = effect;
            }
        } else if (call && call->callee->kind == ExprKind::path && (method == "with_capacity" || method == "from") &&
                   call->args.size() == 1) {
            bool text = s && s->parent && is_string(tc_.user(s->parent)) && !s->parent->template_of;
            if (method == "with_capacity") {
                if (auto n = constant_count(*call->args[0])) {
                    if (*n == 0 || (text && *n <= 23)) return;
                    a.message = std::format("allocates: capacity {}", *n);
                }
            } else if (text && call->args[0]->kind == ExprKind::string_literal) {
                std::uint64_t n = call->args[0]->as<LiteralExpr>().value.size();
                if (n <= 23) return;   // stored inline (small-string optimization)
                a.message = std::format("allocates: {} B for {} B of text", std::max<std::uint64_t>(n, 46), n);
            }
        }
        a.in_loop = growing && !trips_.empty() && a.message.empty();
        allocs_.push_back(std::move(a));
    }

    const CallExpr* call_expr_ = nullptr;

    // A new function (or lambda) of the inspected file starts.
    void start_inspecting(Span span) {
        inspect_    = options_.insights && span.file == inspect_file_;
        cond_depth_ = 0;
        trips_.clear();
        caps_.clear();
        moved_.clear();
        decl_depth_.clear();
    }

    // Using the place `e` by value as a `t` copies it: shown when that is more than 16 bytes.
    void record_copy(const Expr& e, const types::Type* t) {
        if (!inspect_ || !t || t->is(Kind::reference) || needs_drop(t)) return;
        if (e.kind != ExprKind::path && e.kind != ExprKind::member && e.kind != ExprKind::index) return;
        if (e.kind == ExprKind::path && !locals_.contains(e.as<PathExpr>().symbol)) return;
        std::uint64_t n = type_bytes(t);
        if (n <= 16) return;
        std::string name = types::qualified_name(t);
        insight(e.span, "copies", "copy", "range", std::format("copies {} ({})", bytes_text(n), name),
                std::format("`{0}` is a copy type: using it by value copies all {1} bytes. Pass a reference (`{0}&`) to avoid the copy.", name, n));
    }

    // `std::println(f"...")` / `std::format(f"...")`: the text is built in a buffer.
    void record_format(Span at) {
        if (!inspect_) return;
        insight(at, "temporaries", "temporary", "range",
                std::format("temporary: the text being formatted ({} on the stack)", bytes_text(byte_size(buffer_type()))),
                "An unnamed stack slot whose address is used (passed to a function, or indexed), so it stays in memory in optimized "
                "builds too. Like every slot of the function it is reserved when the function is entered. It describes a buffer whose "
                "bytes are on the heap.");
        insight(at, "heap", "alloc", "range", "allocates: the formatted text",
                "Formatting writes the text into a heap buffer that grows as needed (`edax_buf_put` → `realloc`) and is freed once "
                "the text is written or copied into the `std::string`.");
    }

    // The owned local `sym` is no longer owned at this point.
    void record_move(const Symbol* sym) {
        if (!inspect_ || !sym) return;
        auto d = decl_depth_.find(sym);
        bool always = d != decl_depth_.end() && d->second.first == cond_depth_ && d->second.second == trips_.size();
        int& m = moved_[sym];
        if (always) m = 1;
        else if (m != 1) m = 2;
        caps_.erase(sym);
    }

    void record_drops(std::size_t first) {
        if (!inspect_) return;
        llvm::BasicBlock* here = b_.GetInsertBlock();
        if (here != &cur_fn_->getEntryBlock() && llvm::pred_empty(here)) return;   // after return/break: never runs
        for (std::size_t i = scopes_.size(); i-- > first;) {
            const auto& locals = scopes_[i].locals;
            for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
                if (!it->sym) continue;
                int moved = moved_.contains(it->sym) ? moved_[it->sym] : 0;
                if (moved == 1) continue;
                PendingDrop d{drop_site_, drop_kind_, it->sym->name, it->type ? types::qualified_name(it->type) : "?", it->drop, moved == 2};
                if (auto c = caps_.find(it->sym); c != caps_.end() && c->second.known) {
                    d.no_heap = c->second.text ? c->second.cap <= 23 : c->second.cap == 0;
                }
                drops_.push_back(std::move(d));
            }
        }
    }

    // The stack frame of the function just generated (`fn`, body `body`).
    void record_frame(llvm::Function* fn, const Symbol* s, const BlockExpr& body) {
        if (!inspect_) return;
        std::uint64_t total = 0, kept = 0;
        std::vector<std::string> named;
        std::uint64_t temporaries = 0, temp_bytes = 0, flags = 0, flag_bytes = 0;
        std::string rows;
        std::vector<llvm::AllocaInst*> slots;
        for (llvm::Instruction& inst : fn->getEntryBlock()) {
            if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(&inst)) slots.push_back(a);
        }
        std::reverse(slots.begin(), slots.end());   // entry_alloca puts each new slot first
        for (llvm::AllocaInst* a : slots) {
            std::uint64_t n    = byte_size(a->getAllocatedType());
            bool          reg  = !address_used(a);
            SlotInfo      info = slot_info_.contains(a) ? slot_info_[a] : SlotInfo{a->getName().str(), "", "temporary"};
            if (a->getName().starts_with("df_")) info.what = "drop flag";
            total += n;
            if (!reg) kept += n;
            if (info.what == "drop flag") {
                ++flags;
                flag_bytes += n;
            } else if (info.what == "temporary") {
                ++temporaries;
                temp_bytes += n;
            } else {
                named.push_back(std::format("{} {}", info.name, bytes_text(n)));
            }
            rows += std::format("| `{}` | `{}` | {} | {} | {} |\n", info.name, info.type, n, info.what, reg ? "registers" : "stays in memory");
        }
        std::string list;
        for (std::size_t i = 0; i < named.size() && i < 6; ++i) list += (list.empty() ? "" : " · ") + named[i];
        if (named.size() > 6) list += std::format(" · …{} more", named.size() - 6);
        if (temporaries) list += std::format("{}{} {} {}", list.empty() ? "" : " · ", temporaries, temporaries == 1 ? "temporary" : "temporaries", bytes_text(temp_bytes));
        if (flags) list += std::format("{}{} drop {} {}", list.empty() ? "" : " · ", flags, flags == 1 ? "flag" : "flags", bytes_text(flag_bytes));
        Span at{body.span.file, body.span.begin, body.span.begin + 1};
        insight(at, "stack", "frame", "above",
                std::format("stack frame of {}: {} reserved on entry, about {} optimized{}{}", s->name, bytes_text(total), bytes_text(kept),
                            list.empty() ? "" : " — ", list),
                "Every stack slot of a function is reserved when it is entered (one adjustment of the stack pointer), whichever scope "
                "declares it: a variable declared near the end of a block has its slot from the start, and slots are not resized as "
                "scopes open and close. Sizes are before alignment padding. Optimized builds keep only the slots whose address is used "
                "(passed by reference, indexed); the others live in registers.\n\n| slot | type | bytes | what | optimized |\n"
                "|---|---|---|---|---|\n" + rows);
    }

    // Whether the address of the slot `v` (or of a part of it) leaves plain loads and stores: then
    // it stays in memory after optimization; otherwise LLVM keeps it in registers (SROA, mem2reg).
    static bool address_used(const llvm::Value* v) {
        for (const llvm::User* u : v->users()) {
            if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(u)) {
                if (!gep->hasAllConstantIndices() || address_used(gep)) return true;
            } else if (auto* st = llvm::dyn_cast<llvm::StoreInst>(u)) {
                if (st->getValueOperand() == v) return true;   // the address itself is stored
            } else if (auto* call = llvm::dyn_cast<llvm::CallBase>(u)) {
                const llvm::Function* f = call->getCalledFunction();
                if (!f || !f->isIntrinsic()) return true;
            } else if (!llvm::isa<llvm::LoadInst>(u)) {
                return true;
            }
        }
        return false;
    }

    void record_conversion(const Expr& e) {
        if (!inspect_) return;
        switch (e.conversion) {
        case Expr::Conversion::string_to_str:
            insight(e.span, "conversions", "string-to-str", "range", "std::string → std::str (a view, no copy)",
                    "Implicit (spec 02): the `std::str` points at the string's bytes (`std::string::as_str`).");
            break;
        case Expr::Conversion::literal_to_string: {
            std::size_t n = e.kind == ExprKind::string_literal ? e.as<LiteralExpr>().value.size() : 0;
            insight(e.span, "conversions", "literal-to-string", "range", "literal → std::string",
                    std::format("Implicit (spec 19, warning W4002): `std::string::from` copies the {} bytes of text{}.", n,
                                n <= 23 ? " into the string itself (up to 23 bytes are stored inline)" : " to the heap"));
            break;
        }
        case Expr::Conversion::pointer_upcast:
            insight(e.span, "conversions", "upcast", "range",
                    std::format("{} → {} (upcast)", types::qualified_name(types::strip_ref(e.type)), types::qualified_name(e.converted_to)),
                    "Implicit (spec 07): the same address, seen as a base class or an interface (an interface adds its table).");
            break;
        case Expr::Conversion::distinct_value:
            insight(e.span, "conversions", "distinct-value", "range",
                    std::format("{} → {}", types::qualified_name(types::strip_ref(e.type)), types::qualified_name(e.converted_to)),
                    "Implicit (spec 34): a distinct type with a check gives up its guarantee and becomes its underlying type; no code.");
            break;
        case Expr::Conversion::to_callable:
            insight(e.span, "conversions", "to-callable", "range",
                    std::format("lambda → {}", types::qualified_name(e.converted_to)),
                    "Implicit (spec 18): a `std::function` keeps the lambda's captures on the heap; a `std::fn_ptr` is just the code.");
            break;
        default: break;
        }
    }

    // Calls reach the allocator (or `free`) through these functions; computed once at the end.
    std::map<const llvm::Function*, const llvm::Function*> next_alloc_, next_free_;
    std::map<const llvm::Function*, std::string>          fn_display_;

    void reach(const std::set<std::string>& targets, std::map<const llvm::Function*, const llvm::Function*>& next) {
        // Backwards breadth-first search from the targets: next[f] is f's callee on a shortest path.
        std::map<const llvm::Function*, std::vector<const llvm::Function*>> callers;
        for (llvm::Function& f : *mod_) {
            for (llvm::Instruction& i : llvm::instructions(f)) {
                if (auto* call = llvm::dyn_cast<llvm::CallBase>(&i)) {
                    if (const llvm::Function* g = call->getCalledFunction()) callers[g].push_back(&f);
                }
            }
        }
        std::deque<const llvm::Function*> work;
        for (llvm::Function& f : *mod_) {
            if (targets.contains(f.getName().str())) {
                next[&f] = nullptr;
                work.push_back(&f);
            }
        }
        while (!work.empty()) {
            const llvm::Function* g = work.front();
            work.pop_front();
            for (const llvm::Function* f : callers[g]) {
                if (next.contains(f)) continue;
                next[f] = g;
                work.push_back(f);
            }
        }
    }

    std::string chain(const llvm::Function* f, const std::map<const llvm::Function*, const llvm::Function*>& next) {
        std::string out;
        for (int guard = 0; f && guard < 12; ++guard) {
            auto d = fn_display_.find(f);
            std::string name = d != fn_display_.end() ? d->second : f->getName().str();
            out += (out.empty() ? "" : " → ") + std::format("`{}`", name);
            auto it = next.find(f);
            f = it == next.end() ? nullptr : it->second;
        }
        return out;
    }

    void finish_insights() {
        if (!options_.insights) return;
        for (const auto& [s, fn] : fns_) fn_display_[fn] = display_name(s);
        for (const auto& [s, fn] : drop_fns_) fn_display_[fn] = "~" + types::qualified_name(tc_.user(s));
        // The runtime's buffer functions are only declared here; their bodies grow with realloc.
        std::set<std::string> alloc_targets{"malloc", "realloc", "calloc", "edax_buf_put", "edax_fmt_u", "edax_fmt_i", "edax_fmt_f", "edax_fmt_q",
                                            "edax_fmt_str", "edax_fmt_bool", "edax_fmt_char"};
        reach(alloc_targets, next_alloc_);
        reach({"free"}, next_free_);
        const std::string maybe = "\n\nWhether it does depends on the run (e.g. `push` only grows a full buffer).";
        for (const PendingAlloc& a : allocs_) {
            if (!next_alloc_.contains(a.fn)) continue;
            std::string detail = "Reaches the allocator: " + chain(a.fn, next_alloc_) + maybe;
            std::string message = !a.message.empty() ? std::format("{} ({})", a.message, a.display)
                                : a.in_loop          ? std::format("may allocate in a loop — reserve() before it ({})", a.display)
                                                     : std::format("may allocate ({})", a.display);
            if (a.in_loop) {
                detail += "\n\nThis grows a container inside a loop: each time it is full it reallocates and moves everything. When "
                          "the final size is known, `reserve(n)` (or `with_capacity(n)`) before the loop allocates once.";
            }
            insight(a.span, "heap", "alloc", "range", message, detail);
        }
        // Drops: all drops of a site first, then their frees, in the order they run.
        std::size_t i = 0;
        while (i < drops_.size()) {
            std::size_t j = i;
            while (j < drops_.size() && drops_[j].span.begin == drops_[i].span.begin && drops_[j].span.file == drops_[i].span.file &&
                   drops_[j].kind == drops_[i].kind) {
                ++j;
            }
            for (std::size_t k = i; k < j; ++k) {
                const PendingDrop& d     = drops_[k];
                bool               frees = !d.no_heap && next_free_.contains(d.drop);
                insight(d.span, "drops", d.kind, "above",
                        std::format("drop {}: {}{}{}", d.var, d.type, d.maybe_moved ? " (if not moved)" : "", frees ? " · frees heap memory" : ""),
                        std::format("`{}` owns a `{}`. Leaving the scope runs its destructor here (locals are destroyed in reverse "
                                    "declaration order, spec 12).{}",
                                    d.var, d.type, d.maybe_moved ? " It may have been moved on another path: then its drop flag is clear and nothing runs." : ""));
            }
            for (std::size_t k = i; k < j; ++k) {
                const PendingDrop& d = drops_[k];
                if (d.no_heap || !next_free_.contains(d.drop)) continue;
                insight(d.span, "heap", "free", "above", std::format("frees the heap memory of {}", d.var),
                        "Its destructor releases heap memory: " + chain(d.drop, next_free_));
            }
            i = j;
        }
        std::stable_sort(found_.begin(), found_.end(), [&](const Insight& a, const Insight& b) {
            auto pa = sources_.file(a.span.file).line_col(a.span.begin);
            auto pb = sources_.file(b.span.file).line_col(b.span.begin);
            return std::tie(pa.line, pa.column) < std::tie(pb.line, pb.column);
        });
        // Same span, same message (e.g. a statement generated twice): shown once.
        std::vector<Insight> unique;
        std::set<std::tuple<std::uint32_t, std::uint32_t, std::string, std::string>> seen;
        for (Insight& in : found_) {
            if (seen.insert({in.span.begin, in.span.end, in.kind, in.message}).second) unique.push_back(std::move(in));
        }
        *options_.insights = std::move(unique);
    }

    // --- state ------------------------------------------------------------------------------

    sema::Program&                                    program_;
    types::TypeContext&                               tc_;
    const SourceManager&                              sources_;
    DiagnosticEngine&                                 diags_;
    LlvmOptions                                       options_;
    llvm::LLVMContext&                                ctx_;
    std::unique_ptr<llvm::Module>                     mod_;
    llvm::IRBuilder<>                                 b_;
    Span                                              cur_{};
    llvm::Function*                                   cur_fn_ = nullptr;
    const std::map<const Symbol*, const types::Type*>* cur_subst_ = nullptr;   // the template arguments of the function being generated
    llvm::Function*                                   edax_main_ = nullptr;
    llvm::Function*                                   edax_panic_str_ = nullptr;
    llvm::Function*                                   edax_exit_status_ = nullptr;
    llvm::Value*                                      self_ = nullptr;
    llvm::StructType*                                 loc_type_ = nullptr;
    std::map<const Symbol*, llvm::Function*>          fns_;
    std::deque<const Symbol*>                         queue_;
    std::map<const LambdaExpr*, llvm::Function*>      lambda_fns_;
    std::deque<const LambdaExpr*>                     lambda_queue_;
    std::map<const types::Type*, llvm::StructType*>   closure_types_;
    std::map<const Symbol*, llvm::Value*>             locals_;   // the storage of each local (lambda captures: their place in the closure)
    std::map<std::tuple<FileId, std::uint32_t, std::string>, llvm::Constant*> locs_;
    std::map<const llvm::Function*, std::string>                         fn_names_;   // see function_name
    std::vector<Loop>                                 loops_;
    std::set<std::tuple<FileId, std::uint32_t, std::string>>    reported_;
};

} // namespace

// Conditional stores to an address the same iteration already stored to become unconditional
// stores of a select (spec 34): `if (py[i] < 0.0) { py[i] = -py[i]; vy[i] = -vy[i] * 0.8; }` after
// `py[i] += ...; vy[i] *= ...;`. The earlier store shows that the address is valid and written by
// this thread, so storing again can't fault or race; the loop then has no branch and vectorizes
// without masked stores (LLVM itself speculates at most one such store). Runs where the
// vectorizer starts.
// Spec 34 §1 after inlining: the elements of a span parameter (`"edax-span"`, see mark_span) are
// reached only through it while the function runs. add_alias_scopes tags the accesses a function
// makes itself; this pass runs once callees are inlined into it (a system's lambda into the loop
// that calls it per element) and tags the accesses whose address comes from a span parameter's
// data pointer, so the loop vectorizes without a run-time overlap check. It runs before the
// function is inlined into its callers, and the scopes stay with the accesses after that.
struct SpanAliasScopes : llvm::PassInfoMixin<SpanAliasScopes> {
    static llvm::Attribute span_mark(const llvm::Argument* a) {
        return a->getParent()->getAttributes().getParamAttr(a->getArgNo(), "edax-span");
    }

    static llvm::Argument* root(llvm::Value* v) {
        for (int depth = 0; v && depth < 16; ++depth) {
            if (auto* a = llvm::dyn_cast<llvm::Argument>(v)) return span_mark(a).isValid() ? a : nullptr;
            if (auto* g = llvm::dyn_cast<llvm::GetElementPtrInst>(v)) {
                v = g->getPointerOperand();
            } else if (auto* e = llvm::dyn_cast<llvm::ExtractValueInst>(v)) {
                if (e->getNumIndices() != 1 || e->getIndices()[0] != 0) return nullptr;   // the data pointer is field 0
                v = e->getAggregateOperand();
            } else {
                return nullptr;
            }
        }
        return nullptr;
    }

    llvm::PreservedAnalyses run(llvm::Function& fn, llvm::FunctionAnalysisManager&) {
        std::vector<llvm::Argument*> spans;
        for (llvm::Argument& a : fn.args()) {
            if (span_mark(&a).isValid()) spans.push_back(&a);
        }
        if (spans.size() < 2) return llvm::PreservedAnalyses::all();
        auto writes = [](llvm::Argument* a) { return span_mark(a).getValueAsString() == "mut"; };
        llvm::LLVMContext&                       ctx = fn.getContext();
        llvm::MDBuilder                          md(ctx);
        llvm::MDNode*                            domain = md.createAnonymousAliasScopeDomain(fn.getName().str() + ".spans");
        std::map<llvm::Argument*, llvm::MDNode*> scope;
        for (llvm::Argument* a : spans) scope[a] = md.createAnonymousAliasScope(domain, std::format("{}.span{}", fn.getName().str(), a->getArgNo()));
        bool changed = false;
        for (llvm::BasicBlock& bb : fn) {
            for (llvm::Instruction& in : bb) {
                llvm::Value* addr = nullptr;
                if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&in)) addr = ld->getPointerOperand();
                else if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&in)) addr = st->getPointerOperand();
                llvm::Argument* a = addr ? root(addr) : nullptr;
                if (!a) continue;
                std::vector<llvm::Metadata*> others;
                for (llvm::Argument* b : spans) {
                    if (b != a && (writes(a) || writes(b))) others.push_back(scope[b]);   // two read-only spans may overlap
                }
                if (others.empty()) continue;
                in.setMetadata(llvm::LLVMContext::MD_alias_scope,
                               llvm::MDNode::concatenate(in.getMetadata(llvm::LLVMContext::MD_alias_scope), llvm::MDNode::get(ctx, {scope[a]})));
                in.setMetadata(llvm::LLVMContext::MD_noalias,
                               llvm::MDNode::concatenate(in.getMetadata(llvm::LLVMContext::MD_noalias), llvm::MDNode::get(ctx, others)));
                changed = true;
            }
        }
        return changed ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
    }
};

struct ConditionalStoreSelect : llvm::PassInfoMixin<ConditionalStoreSelect> {
    static bool same_address(llvm::Value* a, llvm::Value* b) {
        if (a == b) return true;
        auto* ga = llvm::dyn_cast<llvm::GetElementPtrInst>(a);
        auto* gb = llvm::dyn_cast<llvm::GetElementPtrInst>(b);
        return ga && gb && ga->isIdenticalTo(gb);
    }

    // A store of a `type` to `addr` in `p`, with nothing after it that could free memory.
    static bool stored_before(llvm::BasicBlock* p, llvm::Value* addr, llvm::Type* type) {
        bool found = false;
        for (llvm::Instruction& in : *p) {
            if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&in)) {
                if (!st->isVolatile() && !st->isAtomic() && st->getValueOperand()->getType() == type && same_address(st->getPointerOperand(), addr)) {
                    found = true;
                }
            } else if (auto* call = llvm::dyn_cast<llvm::CallBase>(&in)) {
                if (!llvm::isa<llvm::DbgInfoIntrinsic>(call) && !call->onlyReadsMemory()) found = false;
            }
        }
        return found;
    }

    static bool convert(llvm::BasicBlock* b) {
        llvm::BasicBlock* p = b->getSinglePredecessor();
        if (!p || p == b) return false;
        auto* branch = llvm::dyn_cast<llvm::BranchInst>(p->getTerminator());
        auto* out    = llvm::dyn_cast<llvm::BranchInst>(b->getTerminator());
        if (!branch || !branch->isConditional() || !out || out->isConditional()) return false;
        llvm::BasicBlock* join    = out->getSuccessor(0);
        bool              on_true = branch->getSuccessor(0) == b;
        if ((on_true ? branch->getSuccessor(1) : branch->getSuccessor(0)) != join || join == b) return false;
        std::vector<llvm::StoreInst*>   stores;
        std::vector<llvm::Instruction*> moved;
        unsigned                        cost = 0;
        for (llvm::Instruction& in : *b) {
            if (&in == out || llvm::isa<llvm::DbgInfoIntrinsic>(&in)) continue;
            if (++cost > 16) return false;
            if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&in)) {
                if (st->isVolatile() || st->isAtomic()) return false;
                auto* ptr = llvm::dyn_cast<llvm::Instruction>(st->getPointerOperand());
                if (ptr && ptr->getParent() == b) return false;   // the address must exist before the branch
                if (!stored_before(p, st->getPointerOperand(), st->getValueOperand()->getType())) return false;
                stores.push_back(st);
                continue;
            }
            if (llvm::isa<llvm::PHINode>(&in) || in.mayReadOrWriteMemory() || !llvm::isSafeToSpeculativelyExecute(&in)) return false;
            moved.push_back(&in);
        }
        if (stores.empty()) return false;
        for (llvm::PHINode& phi : join->phis()) {
            if (phi.getBasicBlockIndex(p) < 0 || phi.getBasicBlockIndex(b) < 0) return false;
        }
        llvm::Value*      taken = branch->getCondition();
        llvm::IRBuilder<> ir(branch);
        if (!on_true) taken = ir.CreateNot(taken);
        for (llvm::Instruction* in : moved) in->moveBefore(*p, branch->getIterator());
        // The stores keep their order; each reads the value it replaces just before it.
        for (llvm::StoreInst* st : stores) {
            llvm::Value*    addr = st->getPointerOperand();
            llvm::LoadInst* old  = ir.CreateAlignedLoad(st->getValueOperand()->getType(), addr, st->getAlign());
            old->copyMetadata(*st, {llvm::LLVMContext::MD_alias_scope, llvm::LLVMContext::MD_noalias, llvm::LLVMContext::MD_tbaa});
            llvm::StoreInst* now = ir.CreateAlignedStore(ir.CreateSelect(taken, st->getValueOperand(), old), addr, st->getAlign());
            now->copyMetadata(*st);
            st->eraseFromParent();
        }
        for (llvm::PHINode& phi : join->phis()) {
            llvm::Value* sel = ir.CreateSelect(taken, phi.getIncomingValueForBlock(b), phi.getIncomingValueForBlock(p));
            phi.setIncomingValueForBlock(p, sel);
            phi.removeIncomingValue(b, false);
        }
        ir.CreateBr(join);
        branch->eraseFromParent();
        b->dropAllReferences();
        b->eraseFromParent();
        return true;
    }

    llvm::PreservedAnalyses run(llvm::Function& fn, llvm::FunctionAnalysisManager&) {
        if (fn.hasOptNone()) return llvm::PreservedAnalyses::all();
        bool changed = false;
        for (bool again = true; again;) {
            again = false;
            for (llvm::BasicBlock& b : fn) {
                if (convert(&b)) {
                    changed = again = true;
                    break;
                }
            }
        }
        return changed ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
    }
};

// How LLVM's pipeline is tuned, in every pipeline that optimizes. LLVM's own default leaves the
// SLP vectorizer off (clang turns it on from -O2): it is the pass that turns the four multiplies
// of a vec4 operation into one, which the code of std::math (and any struct of floats) relies on.
llvm::PipelineTuningOptions pipeline_tuning() {
    llvm::PipelineTuningOptions pto;
    pto.SLPVectorization = true;
    return pto;
}

// The passes Edax adds to LLVM's pipeline (in every pipeline that optimizes, so the vectorizer
// reports match the build).
void register_edax_passes(llvm::PassBuilder& pb) {
    // Spec 34 §2: a `T&` of a small frozen type is passed as its value (all Edax functions are
    // internal, so every caller changes with it). LLVM runs this only at -O3.
    pb.registerCGSCCOptimizerLateEPCallback([](llvm::CGSCCPassManager& cgpm, llvm::OptimizationLevel) {
        cgpm.addPass(llvm::ArgumentPromotionPass(4));   // up to 4 scalars: a 16-byte vec4
    });
    pb.registerScalarOptimizerLateEPCallback([](llvm::FunctionPassManager& fpm, llvm::OptimizationLevel) {
        fpm.addPass(SpanAliasScopes());
    });
    pb.registerVectorizerStartEPCallback([](llvm::FunctionPassManager& fpm, llvm::OptimizationLevel) {
        fpm.addPass(ConditionalStoreSelect());
        fpm.addPass(llvm::InstCombinePass());
    });
}

// The loop vectorizer's remarks: which loops it vectorized and why the others stayed scalar.
class VectorizerRemarks : public llvm::DiagnosticHandler {
public:
    struct Remark {
        std::string function;
        unsigned    line = 0;   // 0: no location
        bool        passed = false;
        std::string text;
    };
    std::vector<Remark> remarks;

    bool handleDiagnostics(const llvm::DiagnosticInfo& d) override {
        if (const auto* r = llvm::dyn_cast<llvm::DiagnosticInfoOptimizationBase>(&d)) {
            if (r->getPassName() == "loop-vectorize") {
                Remark m;
                m.function = r->getFunction().getName().str();
                m.passed   = r->isPassed();
                m.text     = r->getMsg();
                if (r->isLocationAvailable()) m.line = r->getLocation().getLine();
                remarks.push_back(std::move(m));
            }
            return true;
        }
        return false;
    }
    bool isAnalysisRemarkEnabled(llvm::StringRef pass) const override { return pass == "loop-vectorize"; }
    bool isMissedOptRemarkEnabled(llvm::StringRef pass) const override { return pass == "loop-vectorize"; }
    bool isPassedOptRemarkEnabled(llvm::StringRef pass) const override { return pass == "loop-vectorize"; }
};

// `@std::simd::must_vectorize` (SIMD, "Requiring vectorized code"): optimizes a copy of the
// module as a release build would (the annotated functions kept out of line, so their loops
// stay theirs) and reports each of their plain loops the vectorizer left scalar, with its
// reasons. Loops the optimizer removed or fully unrolled pass.
bool check_vectorized(llvm::Module& original, const sema::Program& program, const SourceManager& sources, DiagnosticEngine& diags) {
    std::unique_ptr<llvm::Module> mod = llvm::CloneModule(original);
    llvm::LLVMContext&            ctx = mod->getContext();
    std::string triple_name = target_triple();
    std::string lookup_error;
    const llvm::Target* target = lookup_target(triple_name, lookup_error);
    if (!target) return true;
    std::unique_ptr<llvm::TargetMachine> machine(make_target_machine(target, triple_name, llvm::TargetOptions(), llvm::CodeGenOptLevel::Aggressive));
    set_target_triple(*mod, triple_name);
    mod->setDataLayout(machine->createDataLayout());
    std::set<std::string> checked;
    for (const auto& [span, fn] : program.must_vectorize_loops) checked.insert(fn);
    for (llvm::Function& fn : *mod) {
        if (!checked.contains(fn.getName().str())) continue;
        fn.removeFnAttr(llvm::Attribute::AlwaysInline);
        fn.addFnAttr(llvm::Attribute::NoInline);
    }
    auto  handler = std::make_unique<VectorizerRemarks>();
    auto* seen    = handler.get();
    std::unique_ptr<llvm::DiagnosticHandler> saved = ctx.getDiagnosticHandler();
    ctx.setDiagnosticHandler(std::move(handler));
    {
        llvm::LoopAnalysisManager     lam;
        llvm::FunctionAnalysisManager fam;
        llvm::CGSCCAnalysisManager    cgam;
        llvm::ModuleAnalysisManager   mam;
        llvm::PassBuilder             pb(machine.get(), pipeline_tuning());
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        register_edax_passes(pb);
        llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
        mpm.run(*mod, mam);
    }
    std::vector<VectorizerRemarks::Remark> remarks = std::move(seen->remarks);

    ctx.setDiagnosticHandler(std::move(saved));
    bool ok = true;
    for (const auto& [span, fn] : program.must_vectorize_loops) {
        unsigned first  = sources.file(span.file).line_col(span.begin).line;
        unsigned last   = sources.file(span.file).line_col(span.end).line;
        bool     passed = false, missed = false;
        std::vector<std::string> why;
        for (const auto& r : remarks) {
            if (r.function != fn || (r.line && (r.line < first || r.line > last))) continue;
            if (r.passed && r.text.starts_with("vectorized loop")) passed = true;
            if (r.passed || !r.text.starts_with("loop not vectorized")) continue;
            missed = true;
            std::string reason = r.text.size() > 21 && r.text.starts_with("loop not vectorized: ") ? r.text.substr(21) : "";
            if (!reason.empty()) reason[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(reason[0])));   // LLVM 20 capitalizes
            if (!reason.empty() && std::find(why.begin(), why.end(), reason) == why.end()) why.push_back(reason);
        }
        if (passed || !missed) continue;
        ok = false;
        Diagnostic& d = diags.report(DiagCode::E6003, span,
                                     "this loop does not become vector code, but its function is '@std::simd::must_vectorize'",
                                     "not vectorized");
        for (const std::string& w : why) d.notes.push_back(Note{"the vectorizer: " + w, std::nullopt});
        d.notes.push_back(Note{"write it with vector types and std::simd::blocks, or remove what keeps it scalar", std::nullopt});
    }
    return ok;
}

// `@std::no_checks_left` (spec 34): optimizes a copy of the module as a release build would, with
// the annotated functions kept out of line, and reports every panic path left in them: a bounds,
// overflow or other check the optimizer could not prove away. Each note is the check's place.
bool check_no_checks_left(llvm::Module& original, const sema::Program& program, DiagnosticEngine& diags) {
    std::unique_ptr<llvm::Module> mod = llvm::CloneModule(original);
    std::string triple_name = target_triple();
    std::string lookup_error;
    const llvm::Target* target = lookup_target(triple_name, lookup_error);
    if (!target) return true;
    std::unique_ptr<llvm::TargetMachine> machine(make_target_machine(target, triple_name, llvm::TargetOptions(), llvm::CodeGenOptLevel::Aggressive));
    set_target_triple(*mod, triple_name);
    mod->setDataLayout(machine->createDataLayout());
    std::map<std::string, Span> wanted;
    for (const auto& [span, fn] : program.no_checks_left) {
        wanted[fn]         = span;
        wanted[fn + ".at"] = span;   // the copy direct calls use (caller's location as an argument)
    }
    for (llvm::Function& fn : *mod) {
        if (!wanted.contains(fn.getName().str())) continue;
        fn.removeFnAttr(llvm::Attribute::AlwaysInline);
        fn.addFnAttr(llvm::Attribute::NoInline);
    }
    {
        llvm::LoopAnalysisManager     lam;
        llvm::FunctionAnalysisManager fam;
        llvm::CGSCCAnalysisManager    cgam;
        llvm::ModuleAnalysisManager   mam;
        llvm::PassBuilder             pb(machine.get(), pipeline_tuning());
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        register_edax_passes(pb);
        llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
        mpm.run(*mod, mam);
    }
    // A panic's first argument is its location: { file, line, column }.
    auto where = [](llvm::Value* v) -> std::string {
        auto* g = llvm::dyn_cast<llvm::GlobalVariable>(v->stripPointerCasts());
        auto* c = g && g->hasInitializer() ? llvm::dyn_cast<llvm::ConstantStruct>(g->getInitializer()) : nullptr;
        if (!c || c->getNumOperands() < 3) return "";
        std::string file;
        if (auto* fg = llvm::dyn_cast<llvm::GlobalVariable>(c->getOperand(0)->stripPointerCasts()); fg && fg->hasInitializer()) {
            if (auto* data = llvm::dyn_cast<llvm::ConstantDataSequential>(fg->getInitializer()); data && data->isString()) {
                file = data->getAsString().str();
                while (!file.empty() && file.back() == '\0') file.pop_back();
            }
        }
        auto* line = llvm::dyn_cast<llvm::ConstantInt>(c->getOperand(1));
        auto* col  = llvm::dyn_cast<llvm::ConstantInt>(c->getOperand(2));
        if (!line || !col) return file;
        return std::format("{}:{}:{}", file, line->getZExtValue(), col->getZExtValue());
    };
    bool ok = true;
    std::set<std::string> reported;
    for (llvm::Function& fn : *mod) {
        auto it = wanted.find(fn.getName().str());
        if (it == wanted.end() || fn.isDeclaration()) continue;
        std::vector<std::string> places;
        for (llvm::BasicBlock& bb : fn) {
            for (llvm::Instruction& in : bb) {
                auto* call = llvm::dyn_cast<llvm::CallInst>(&in);
                llvm::Function* callee = call ? call->getCalledFunction() : nullptr;
                if (!callee || !callee->getName().starts_with("edax_panic") || callee->getName() == "edax_panic_end" ||
                    call->arg_size() == 0) {
                    continue;
                }
                if (call->arg_size() > 1) {   // std::assume and std::unreachable are checks the program asked for
                    auto* mg = llvm::dyn_cast<llvm::GlobalVariable>(call->getArgOperand(1)->stripPointerCasts());
                    auto* md = mg && mg->hasInitializer() ? llvm::dyn_cast<llvm::ConstantDataSequential>(mg->getInitializer()) : nullptr;
                    if (md && md->isString() && (md->getAsString().starts_with("assumption failed (std::assume)") ||
                                                 md->getAsString().starts_with("reached code marked unreachable"))) {
                        continue;
                    }
                }
                std::string p = where(call->getArgOperand(0));
                if (std::find(places.begin(), places.end(), p) == places.end()) places.push_back(p);
            }
        }
        std::string key = std::format("{}:{}", it->second.file, it->second.begin);
        if (places.empty() || reported.contains(key)) continue;
        reported.insert(key);
        ok = false;
        Diagnostic& d = diags.report(DiagCode::E6004, it->second,
                                     std::format("{} run-time check{} left in a '@std::no_checks_left' function", places.size(),
                                                 places.size() == 1 ? " is" : "s are"),
                                     "checks left");
        for (const std::string& p : places) d.notes.push_back(Note{p.empty() ? "a check" : "a check at " + p, std::nullopt});
        d.notes.push_back(Note{"state what the optimizer can't see (std::assume), iterate instead of indexing, or use the unchecked forms in unsafe code",
                               std::nullopt});
    }
    return ok;
}

// `i1 @edax_cpu_has_level(i32 id)`: whether the running CPU has CPU level `id` of the target's
// architecture (1 the first, as in the table of levels): what `@std::simd::multiversion` picks
// its copy with. x86-64 asks `cpuid` and `xgetbv`; AArch64 and RISC-V ask the kernel (getauxval).
std::string cpu_detection_ir() {
    llvm::Triple t(target_triple());
    if (t.getArch() == llvm::Triple::x86_64) {
        return R"(
define internal i1 @edax_cpu_has_level(i32 %id) {
entry:
  %first = icmp ule i32 %id, 1
  br i1 %first, label %yes, label %v2
v2:
  %c1 = call { i32, i32, i32, i32 } asm "cpuid", "={ax},={bx},={cx},={dx},{ax},{cx},~{dirflag},~{fpsr},~{flags}"(i32 1, i32 0)
  %ecx1 = extractvalue { i32, i32, i32, i32 } %c1, 2
  %m2 = and i32 %ecx1, 9961985
  %ok2 = icmp eq i32 %m2, 9961985
  br i1 %ok2, label %after2, label %no
after2:
  %is2 = icmp eq i32 %id, 2
  br i1 %is2, label %yes, label %v3
v3:
  %m3 = and i32 %ecx1, 943722496
  %ok3a = icmp eq i32 %m3, 943722496
  br i1 %ok3a, label %v3b, label %no
v3b:
  %x = call { i32, i32 } asm "xgetbv", "={ax},={dx},{cx},~{dirflag},~{fpsr},~{flags}"(i32 0)
  %xcr0 = extractvalue { i32, i32 } %x, 0
  %c7 = call { i32, i32, i32, i32 } asm "cpuid", "={ax},={bx},={cx},={dx},{ax},{cx},~{dirflag},~{fpsr},~{flags}"(i32 7, i32 0)
  %ebx7 = extractvalue { i32, i32, i32, i32 } %c7, 1
  %ce = call { i32, i32, i32, i32 } asm "cpuid", "={ax},={bx},={cx},={dx},{ax},{cx},~{dirflag},~{fpsr},~{flags}"(i32 -2147483647, i32 0)
  %ecxe = extractvalue { i32, i32, i32, i32 } %ce, 2
  %ymm = and i32 %xcr0, 6
  %ymm.ok = icmp eq i32 %ymm, 6
  %b3 = and i32 %ebx7, 296
  %b3.ok = icmp eq i32 %b3, 296
  %lz = and i32 %ecxe, 32
  %lz.ok = icmp ne i32 %lz, 0
  %ok3b = and i1 %ymm.ok, %b3.ok
  %ok3 = and i1 %ok3b, %lz.ok
  br i1 %ok3, label %after3, label %no
after3:
  %is3 = icmp eq i32 %id, 3
  br i1 %is3, label %yes, label %v4
v4:
  %zmm = and i32 %xcr0, 230
  %zmm.ok = icmp eq i32 %zmm, 230
  %b4 = and i32 %ebx7, -805109760
  %b4.ok = icmp eq i32 %b4, -805109760
  %ok4 = and i1 %zmm.ok, %b4.ok
  %is4 = icmp eq i32 %id, 4
  %ok4id = and i1 %ok4, %is4
  br i1 %ok4id, label %yes, label %no
yes:
  ret i1 true
no:
  ret i1 false
}
)";
    }
    if (t.isAArch64()) {   // armv9: SVE2 (AT_HWCAP2 = 26, HWCAP2_SVE2 = bit 1)
        return R"(
declare i64 @getauxval(i64)
define internal i1 @edax_cpu_has_level(i32 %id) {
entry:
  %first = icmp ule i32 %id, 1
  br i1 %first, label %yes, label %v9
v9:
  %caps = call i64 @getauxval(i64 26)
  %sve2 = and i64 %caps, 2
  %ok = icmp ne i64 %sve2, 0
  ret i1 %ok
yes:
  ret i1 true
}
)";
    }
    if (t.isRISCV()) {   // riscv64_v: the V extension (AT_HWCAP = 16, bit 'v' - 'a' = 21)
        return R"(
declare i64 @getauxval(i64)
define internal i1 @edax_cpu_has_level(i32 %id) {
entry:
  %first = icmp ule i32 %id, 1
  br i1 %first, label %yes, label %v
v:
  %caps = call i64 @getauxval(i64 16)
  %bit = and i64 %caps, 2097152
  %ok = icmp ne i64 %bit, 0
  ret i1 %ok
yes:
  ret i1 true
}
)";
    }
    return "\ndefine internal i1 @edax_cpu_has_level(i32 %id) {\nentry:\n  %ok = icmp ule i32 %id, 1\n  ret i1 %ok\n}\n";
;
}


namespace {
// Appends what identifies `c` (its kind, type and contents; other globals by name) to `key`:
// equal keys are equal constants. Hashing the raw data instead of the printed IR keeps large
// constants (embedded data) cheap.
void constant_key(const llvm::Constant* c, std::map<const llvm::Type*, std::string>& types, std::string& key) {
    auto add_raw = [&](const void* data, std::size_t size) { key.append(static_cast<const char*>(data), size); };
    auto add_int = [&](std::uint64_t v) { add_raw(&v, sizeof v); };
    auto add_type = [&](const llvm::Type* t) {
        auto [it, fresh] = types.try_emplace(t);
        if (fresh) {
            llvm::raw_string_ostream out(it->second);
            t->print(out);
        }
        key += it->second;
        key += ';';
    };
    auto add_apint = [&](const llvm::APInt& v) {
        add_int(v.getBitWidth());
        add_raw(v.getRawData(), v.getNumWords() * sizeof(std::uint64_t));
    };
    add_int(c->getValueID());
    add_type(c->getType());
    if (const auto* g = llvm::dyn_cast<llvm::GlobalValue>(c); g && g->hasName()) {
        key += g->getName();
        key += ';';
    } else if (const auto* d = llvm::dyn_cast<llvm::ConstantDataSequential>(c)) {
        llvm::StringRef raw = d->getRawDataValues();
        add_int(raw.size());
        key += raw;
    } else if (const auto* i = llvm::dyn_cast<llvm::ConstantInt>(c)) {
        add_apint(i->getValue());
    } else if (const auto* f = llvm::dyn_cast<llvm::ConstantFP>(c)) {
        add_apint(f->getValueAPF().bitcastToAPInt());
    } else if (llvm::isa<llvm::ConstantAggregate>(c) || llvm::isa<llvm::ConstantExpr>(c)) {
        if (const auto* e = llvm::dyn_cast<llvm::ConstantExpr>(c)) {
            add_int(e->getOpcode());
            add_int(e->getRawSubclassOptionalData());
            if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(e)) add_type(gep->getSourceElementType());
        }
        add_int(c->getNumOperands());
        for (const llvm::Use& op : c->operands()) constant_key(llvm::cast<llvm::Constant>(op.get()), types, key);
    } else if (!llvm::isa<llvm::ConstantAggregateZero>(c) && !llvm::isa<llvm::ConstantPointerNull>(c) &&
               !llvm::isa<llvm::UndefValue>(c)) {
        std::string text;   // the rest (block addresses, unnamed globals, ...): as printed
        llvm::raw_string_ostream out(text);
        c->print(out);
        out.flush();
        key += text;
        key += ';';
    }
}

// Private constants (string literals, messages, format specs, source locations) are named after
// their contents: `.str.<hash>` instead of `.str.17`, so that a function's IR text doesn't change
// when another function adds a string (the object cache, build_executable). Equal constants are
// merged. A constant is named after the constants it points to, so those come first.
void name_constants_by_content(llvm::Module& mod) {
    std::vector<llvm::GlobalVariable*> pending;
    for (llvm::GlobalVariable& g : mod.globals()) {
        if (g.hasPrivateLinkage() && g.isConstant() && g.hasInitializer()) pending.push_back(&g);
    }
    std::set<const llvm::GlobalVariable*> waiting(pending.begin(), pending.end());
    auto refers_to_waiting = [&](const llvm::Constant* c) {
        std::vector<const llvm::Constant*> work{c};
        while (!work.empty()) {
            const llvm::Constant* x = work.back();
            work.pop_back();
            if (const auto* g = llvm::dyn_cast<llvm::GlobalVariable>(x)) {
                if (waiting.count(g)) return true;
                continue;
            }
            for (const llvm::Use& op : x->operands()) {
                if (const auto* oc = llvm::dyn_cast<llvm::Constant>(op.get())) work.push_back(oc);
            }
        }
        return false;
    };
    std::map<std::string, llvm::GlobalVariable*> by_name;
    std::map<const llvm::Type*, std::string>     type_names;
    bool progress = true;
    while (!pending.empty() && progress) {
        progress = false;
        std::vector<llvm::GlobalVariable*> later;
        for (llvm::GlobalVariable* g : pending) {
            if (refers_to_waiting(g->getInitializer())) {
                later.push_back(g);
                continue;
            }
            std::string text;
            constant_key(g->getInitializer(), type_names, text);
            text += std::format("{} {}", g->hasGlobalUnnamedAddr() ? "u" : "", g->getAlign().valueOrOne().value());
            std::string base = g->getName().str();
            while (!base.empty() && (std::isdigit(static_cast<unsigned char>(base.back())) || base.back() == '.' || base.back() == '_')) {
                base.pop_back();
            }
            std::string name = std::format("{}.{:016x}", base, fnv1a(text));
            if (auto it = by_name.find(name); it != by_name.end()) {   // the same contents: one constant
                g->replaceAllUsesWith(it->second);
                g->eraseFromParent();
            } else {
                g->setName(name);
                by_name[name] = g;
            }
            waiting.erase(g);
            progress = true;
        }
        pending = std::move(later);
    }
}
} // namespace

namespace {
// A few calls are made with the callee's declared type while an argument has another type of the
// same meaning (an interface reference for a plain pointer, ...). The printed IR spells each call
// with its arguments' types, and the parser accepts that, so the text form was always valid: this
// gives in-memory calls the same function type the text would have.
void spell_calls_like_text(llvm::Module& mod) {
    for (llvm::Function& fn : mod) {
        for (llvm::Instruction& i : llvm::make_early_inc_range(llvm::instructions(fn))) {
            auto* call = llvm::dyn_cast<llvm::CallInst>(&i);
            if (!call || call->getFunctionType()->isVarArg()) continue;
            llvm::FunctionType* ft = call->getFunctionType();
            bool same = ft->getNumParams() == call->arg_size();
            for (unsigned k = 0; same && k < call->arg_size(); ++k) same = ft->getParamType(k) == call->getArgOperand(k)->getType();
            if (same) continue;
            std::vector<llvm::Type*>  types;
            std::vector<llvm::Value*> args;
            for (llvm::Value* a : call->args()) {
                types.push_back(a->getType());
                args.push_back(a);
            }
            auto* nc = llvm::CallInst::Create(llvm::FunctionType::get(ft->getReturnType(), types, false), call->getCalledOperand(), args, "",
                                              call);
            nc->takeName(call);
            nc->setAttributes(call->getAttributes());
            nc->setCallingConv(call->getCallingConv());
            nc->setTailCallKind(call->getTailCallKind());
            nc->setDebugLoc(call->getDebugLoc());
            call->replaceAllUsesWith(nc);
            call->eraseFromParent();
        }
    }
}

// The checked program as an LLVM module (without the runtime), or null after errors.
std::unique_ptr<llvm::Module> generate_module(llvm::LLVMContext& ctx, sema::Program& program, const SourceManager& sources,
                                              DiagnosticEngine& diags, LlvmOptions options) {
    Gen                           gen(program, sources, diags, options, ctx);
    std::unique_ptr<llvm::Module> mod = gen.run();
    if (!mod) return nullptr;
    spell_calls_like_text(*mod);
    lower_large_aggregates(*mod);   // large structs in memory: instruction selection splits SSA aggregates
    name_constants_by_content(*mod);
    if (!program.must_vectorize_loops.empty() && !check_vectorized(*mod, program, sources, diags)) return nullptr;
    if (!program.no_checks_left.empty() && !check_no_checks_left(*mod, program, diags)) return nullptr;
    return mod;
}

// The runtime's IR (llvm_runtime.cpp) for this build: the host's futex number, the CPU detection,
// and with `panic() == trap` a trap in edax_panic_buf.
std::string runtime_text_for(const LlvmOptions& options) {
    std::string runtime_text = llvm_runtime_ir();
    {   // the futex system call's number on the host (edaxc compiles for the machine it runs on)
        llvm::Triple      host(target_triple());
        const std::string number = host.isAArch64() || host.isRISCV() ? "98" : "202";
        for (std::size_t at; (at = runtime_text.find("@@SYS_FUTEX@@")) != std::string::npos;) runtime_text.replace(at, 13, number);
    }
    runtime_text += cpu_detection_ir();
    if (options.panic_trap) {   // every panic ends in edax_panic_buf: make it a trap
        const std::string head = "define internal void @edax_panic_buf(ptr %loc, ptr %msg) cold {";
        std::size_t at = runtime_text.find(head);
        if (at != std::string::npos) {
            runtime_text.replace(at, head.size(), head + "\n  call void @llvm.trap()\n  unreachable\n}\n"
                                                  "define internal void @edax_panic_buf.message(ptr %loc, ptr %msg) cold {");
            runtime_text += "\ndeclare void @llvm.trap() cold noreturn nounwind\n";
        }
    }
    return runtime_text;
}

// Links the runtime into `mod`. The two refer to each other's internal symbols (the text form is
// one module): those are external while linking and internal again afterwards.
bool link_runtime(llvm::Module& mod, const std::string& runtime_text, std::string& error) {
    llvm::SMDiagnostic err;
    std::unique_ptr<llvm::Module> rt =
        llvm::parseIR(llvm::MemoryBufferRef(llvm::StringRef(runtime_text.data(), runtime_text.size()), "edax-runtime.ll"), err, mod.getContext());
    if (!rt) {
        std::string text;
        llvm::raw_string_ostream os(text);
        err.print("edaxc", os);
        error = "internal error: the runtime's LLVM IR does not parse\n" + text;
        return false;
    }
    std::vector<std::string> local;
    auto open_up = [&](llvm::Module& defining, llvm::Module& using_) {
        for (llvm::GlobalValue& gv : defining.global_values()) {
            if (gv.isDeclaration() || !gv.hasLocalLinkage()) continue;
            llvm::GlobalValue* other = using_.getNamedValue(gv.getName());
            if (!other || !other->isDeclaration()) continue;
            local.push_back(gv.getName().str());
            gv.setLinkage(llvm::GlobalValue::ExternalLinkage);
        }
    };
    open_up(*rt, mod);
    open_up(mod, *rt);
    rt->setDataLayout(mod.getDataLayout());
    rt->setTargetTriple(mod.getTargetTriple());
    if (llvm::Linker::linkModules(mod, std::move(rt))) {
        error = "internal error: the runtime does not link with the program";
        return false;
    }
    for (const std::string& name : local) {
        if (llvm::GlobalValue* gv = mod.getNamedValue(name)) gv->setLinkage(llvm::GlobalValue::InternalLinkage);
    }
    return true;
}
} // namespace

struct GeneratedProgram::Impl {
    llvm::LLVMContext             ctx;
    std::unique_ptr<llvm::Module> mod;
};

GeneratedProgram::GeneratedProgram() = default;
GeneratedProgram::~GeneratedProgram() = default;
GeneratedProgram::GeneratedProgram(GeneratedProgram&&) noexcept = default;
GeneratedProgram& GeneratedProgram::operator=(GeneratedProgram&&) noexcept = default;

GeneratedProgram generate_llvm_program(sema::Program& program, const SourceManager& sources, DiagnosticEngine& diags,
                                       LlvmOptions options) {
    GeneratedProgram out;
    out.impl_      = std::make_unique<GeneratedProgram::Impl>();
    out.impl_->mod = generate_module(out.impl_->ctx, program, sources, diags, options);
    if (!out.impl_->mod) {
        out.impl_.reset();
        return out;
    }
    std::string error;
    if (!link_runtime(*out.impl_->mod, runtime_text_for(options), error)) {
        diags.report(DiagCode::E6001, Span{}, error, "");
        out.impl_.reset();
    }
    return out;
}

std::string generate_llvm_ir(sema::Program& program, const SourceManager& sources, DiagnosticEngine& diags,
                             LlvmOptions options) {
    llvm::LLVMContext             ctx;
    std::unique_ptr<llvm::Module> mod = generate_module(ctx, program, sources, diags, options);
    if (!mod) return {};
    std::string           printed;
    llvm::raw_string_ostream module_out(printed);
    mod->print(module_out, nullptr);
    module_out.flush();
    // The runtime defines the functions and intrinsics it shares with the module; the module's
    // declarations of them would clash with those.
    std::string runtime_text = runtime_text_for(options);
    std::set<std::string> provided;
    for (std::string_view line : split_lines(runtime_text)) {
        if (!line.starts_with("define ") && !line.starts_with("declare ")) continue;
        std::size_t at = line.find('@');
        if (at == std::string_view::npos) continue;
        std::size_t end = line.find('(', at);
        provided.insert(std::string(line.substr(at + 1, end - at - 1)));
    }
    std::string text;
    for (std::string_view line : split_lines(printed)) {
        if (line.starts_with("declare ")) {
            std::size_t at  = line.find('@');
            std::size_t end = at == std::string_view::npos ? at : line.find('(', at);
            if (end != std::string_view::npos && provided.count(std::string(line.substr(at + 1, end - at - 1)))) continue;
        }
        text.append(line);
    }
    return text + "\n" + runtime_text;
}

namespace {
// The location a panic or a site variant was given: "file:line:column", or "".
std::string location_text(llvm::Value* v) {
    auto* g = llvm::dyn_cast<llvm::GlobalVariable>(v->stripPointerCasts());
    auto* c = g && g->hasInitializer() ? llvm::dyn_cast<llvm::ConstantStruct>(g->getInitializer()) : nullptr;
    if (!c || c->getNumOperands() < 3) return "";
    std::string file;
    if (auto* fg = llvm::dyn_cast<llvm::GlobalVariable>(c->getOperand(0)->stripPointerCasts()); fg && fg->hasInitializer()) {
        if (auto* data = llvm::dyn_cast<llvm::ConstantDataSequential>(fg->getInitializer()); data && data->isString()) {
            file = data->getAsString().str();
            while (!file.empty() && file.back() == '\0') file.pop_back();
        }
    }
    auto* line = llvm::dyn_cast<llvm::ConstantInt>(c->getOperand(1));
    auto* col  = llvm::dyn_cast<llvm::ConstantInt>(c->getOperand(2));
    if (!line || !col || file.empty()) return "";
    return std::format("{}:{}:{}", file, line->getZExtValue(), col->getZExtValue());
}

std::string message_text(llvm::CallInst* call) {
    if (auto* md = call->getMetadata("edax.message")) {   // a message with values added at run time: what it says before
        if (auto* text = llvm::dyn_cast<llvm::MDString>(md->getOperand(0))) return text->getString().str();
    }
    if (call->arg_size() < 2) return "";
    auto* mg = llvm::dyn_cast<llvm::GlobalVariable>(call->getArgOperand(1)->stripPointerCasts());
    auto* md = mg && mg->hasInitializer() ? llvm::dyn_cast<llvm::ConstantDataSequential>(mg->getInitializer()) : nullptr;
    if (!md || !md->isString()) return "";
    std::string text = md->getAsString().str();
    if (call->arg_size() > 2) {
        if (auto* n = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(2)); n && n->getZExtValue() <= text.size()) text.resize(n->getZExtValue());
    }
    while (!text.empty() && text.back() == '\0') text.pop_back();
    return text;
}

// Every check of the module by the location it reports: panics with a constant location, and
// calls of functions with entry checks that take the caller's location (`.at` copies).
std::map<std::string, std::set<std::string>> check_sites(llvm::Module& mod) {
    std::map<std::string, std::set<std::string>> out;
    for (llvm::Function& fn : mod) {
        for (llvm::BasicBlock& bb : fn) {
            for (llvm::Instruction& in : bb) {
                auto* call = llvm::dyn_cast<llvm::CallInst>(&in);
                llvm::Function* callee = call ? call->getCalledFunction() : nullptr;
                if (!callee || call->arg_size() == 0) continue;
                if (callee->getName().starts_with("edax_panic") && callee->getName() != "edax_panic_end") {
                    std::string where = location_text(call->getArgOperand(0));
                    if (!where.empty()) out[where].insert(message_text(call));
                } else if (callee->getName().ends_with(".at") && callee->hasFnAttribute("edax-name")) {
                    std::string where = location_text(call->getArgOperand(call->arg_size() - 1));
                    if (!where.empty()) out[where].insert("the checks of " + callee->getFnAttribute("edax-name").getValueAsString().str());
                }
            }
        }
    }
    return out;
}
} // namespace

std::vector<CheckSite> explain_checks(sema::Program& program, const SourceManager& sources, DiagnosticEngine& diags,
                                      const std::string& file, LlvmOptions options) {
    initialize_targets();
    options.debug_info = true;   // which functions are the file's
    llvm::LLVMContext ctx;
    Gen               gen(program, sources, diags, options, ctx);
    std::unique_ptr<llvm::Module> mod = gen.run();
    if (!mod) return {};
    // The file's own functions are looked at on their own, for any arguments: they are not
    // inlined into each other (what they call is, as in a build).
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string     wanted = fs::weakly_canonical(fs::path(file), ec).generic_string();
    for (llvm::Function& fn : *mod) {
        llvm::DISubprogram* sp = fn.getSubprogram();
        if (!sp || fn.isDeclaration()) continue;
        fs::path p = fs::path(sp->getDirectory().str()) / sp->getFilename().str();
        if (fs::weakly_canonical(p, ec).generic_string() != wanted) continue;
        fn.removeFnAttr(llvm::Attribute::AlwaysInline);
        fn.addFnAttr(llvm::Attribute::NoInline);
        if (fn.getName() != "main") fn.setLinkage(llvm::GlobalValue::ExternalLinkage);   // callers unknown: no constants propagated in
    }
    std::map<std::string, std::set<std::string>> before = check_sites(*mod);
    std::string triple_name = target_triple();
    std::string lookup_error;
    const llvm::Target* target = lookup_target(triple_name, lookup_error);
    if (!target) return {};
    std::unique_ptr<llvm::TargetMachine> machine(make_target_machine(target, triple_name, llvm::TargetOptions(), llvm::CodeGenOptLevel::Aggressive));
    set_target_triple(*mod, triple_name);
    mod->setDataLayout(machine->createDataLayout());
    {
        llvm::LoopAnalysisManager     lam;
        llvm::FunctionAnalysisManager fam;
        llvm::CGSCCAnalysisManager    cgam;
        llvm::ModuleAnalysisManager   mam;
        llvm::PassBuilder             pb(machine.get(), pipeline_tuning());
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        register_edax_passes(pb);
        llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
        mpm.run(*mod, mam);
    }
    std::map<std::string, std::set<std::string>> after = check_sites(*mod);
    std::vector<CheckSite> out;
    for (const auto& [where, what] : before) {
        CheckSite site;
        site.where = where;
        auto kept  = after.find(where);
        site.kept  = kept != after.end();
        for (const std::string& w : site.kept ? kept->second : what) {
            if (!w.empty()) site.what.push_back(w);
        }
        out.push_back(std::move(site));
    }
    return out;
}

std::string llvm_version() { return LLVM_VERSION_STRING; }

// --- building: optimization, object code, the object cache ------------------------------------

namespace {

int optimization_rank(const std::string& l) { return l == "basic" ? 1 : l == "size" ? 2 : l == "speed" ? 3 : 0; }

// The highest optimization level a function of `mod` asks for ("none" when all are at none);
// functions without their own level are at `default_level` (the mode's).
std::string highest_level(const llvm::Module& mod, const std::string& default_level = "none") {
    std::string highest = "none";
    for (const llvm::Function& fn : mod) {
        if (fn.isDeclaration() || fn.hasAvailableExternallyLinkage() || fn.hasFnAttribute(llvm::Attribute::OptimizeNone)) continue;
        std::string l = fn.hasFnAttribute("edax-optimize") ? fn.getFnAttribute("edax-optimize").getValueAsString().str() : default_level;
        if (optimization_rank(l) > optimization_rank(highest)) highest = l;
    }
    return highest;
}

// Optimizes `mod` at `level` (with the sanitizer passes) and writes its object code to `path`.
bool emit_object(llvm::Module& mod, const llvm::Target* target, const std::string& triple_name, const std::string& level, bool asan,
                 bool tsan, const std::string& path, std::string& error, const std::string& profile_generate = {},
                 const std::string& profile_use = {}) {
    bool optimize = level != "none";
    bool pgo      = !profile_generate.empty() || !profile_use.empty();
    std::unique_ptr<llvm::TargetMachine> machine(make_target_machine(
        target, triple_name, llvm::TargetOptions(),
        !optimize ? llvm::CodeGenOptLevel::None : level == "basic" ? llvm::CodeGenOptLevel::Less : llvm::CodeGenOptLevel::Aggressive));
    set_target_triple(mod, triple_name);
    mod.setDataLayout(machine->createDataLayout());
    if (optimize || asan || tsan || pgo) {   // the mode's optimization() (spec 24), sanitizer passes (spec 34), PGO
        llvm::LoopAnalysisManager     lam;
        llvm::FunctionAnalysisManager fam;
        llvm::CGSCCAnalysisManager    cgam;
        llvm::ModuleAnalysisManager   mam;
        // The standard instrumentations make the passes skip `optnone` functions (@std::optimize).
        llvm::PassInstrumentationCallbacks pic;
        llvm::StandardInstrumentations     si(mod.getContext(), false);
        si.registerCallbacks(pic, &mam);
        std::optional<llvm::PGOOptions> pgo_options;
        if (!profile_generate.empty()) {   // counters in every function, written at exit
            pgo_options = llvm::PGOOptions(profile_generate, "", "", "", llvm::vfs::getRealFileSystem(), llvm::PGOOptions::IRInstr);
        } else if (!profile_use.empty()) {
            pgo_options = llvm::PGOOptions(profile_use, "", "", "", llvm::vfs::getRealFileSystem(), llvm::PGOOptions::IRUse);
        }
        llvm::PassBuilder             pb(machine.get(), pipeline_tuning(), pgo_options, &pic);
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        // speed: O2 (O3 measured slower on bench/: it stops inlining std::string::push); basic: O1,
        // the `development` mode; size: Os.
        llvm::OptimizationLevel pipeline = level == "size"    ? llvm::OptimizationLevel::Os
                                         : level == "basic"   ? llvm::OptimizationLevel::O1
                                                              : llvm::OptimizationLevel::O2;
        register_edax_passes(pb);
        if (asan || tsan) {
            auto add_sanitizer = [asan](llvm::ModulePassManager& mpm) {
                if (asan) {
                    mpm.addPass(llvm::AddressSanitizerPass(llvm::AddressSanitizerOptions{}));
                } else {
                    mpm.addPass(llvm::ModuleThreadSanitizerPass());
                    mpm.addPass(llvm::createModuleToFunctionPassAdaptor(llvm::ThreadSanitizerPass()));
                }
            };
#if LLVM_VERSION_MAJOR >= 20   // the callback also gets the LTO phase
            pb.registerOptimizerLastEPCallback(
                [add_sanitizer](llvm::ModulePassManager& mpm, llvm::OptimizationLevel, llvm::ThinOrFullLTOPhase) { add_sanitizer(mpm); });
#else
            pb.registerOptimizerLastEPCallback([add_sanitizer](llvm::ModulePassManager& mpm, llvm::OptimizationLevel) { add_sanitizer(mpm); });
#endif
        }
        llvm::ModulePassManager mpm = optimize ? pb.buildPerModuleDefaultPipeline(pipeline) : pb.buildO0DefaultPipeline(llvm::OptimizationLevel::O0);
        mpm.run(mod, mam);
    }
    std::error_code ec;
    llvm::raw_fd_ostream file(path, ec, llvm::sys::fs::OF_None);
    if (ec) {
        error = "cannot write " + path + ": " + ec.message();
        return false;
    }
    llvm::legacy::PassManager pm;
    if (machine->addPassesToEmitFile(pm, file, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        error = "the target cannot emit object files";
        return false;
    }
    pm.run(mod);
    return true;
}

// The object cache (debug builds). A program is compiled in units (the "edax-unit" attribute the
// generator puts on each function, see Gen::unit_of): each unit becomes a module of its own, and
// its object code is stored under a hash of that module's IR text and of everything else that
// decides the code (edaxc, LLVM, target, CPU, optimization, sanitizer). A unit whose text is the
// same as in an earlier build - the standard library's, almost always - isn't compiled again.
// The key is the content itself, so whole-program decisions (devirtualization, what `main`
// reaches, optimization levels) can't make a cached object stale: if they change the code, they
// change the text.
//
// Only builds whose own code is unoptimized use it: across units nothing is inlined, which an
// unoptimized program doesn't do anyway (the library still inlines within its units). Optimized
// builds stay one module.
//
// The directory: $EDAX_CACHE_DIR, else $XDG_CACHE_HOME/edax/objects, else ~/.cache/edax/objects;
// EDAX_CACHE_DIR=off turns the cache off. It is pruned to 2 GB, and of objects unused for 30 days.
// EDAX_CACHE_STATS=1 prints a line about each build's units to stderr (2: also the units compiled),
// EDAX_CACHE_DUMP=<dir> writes each unit's IR there.
std::string object_cache_dir() {
    const char* own = std::getenv("EDAX_CACHE_DIR");
    if (own && (std::string_view(own) == "off" || std::string_view(own) == "0")) return {};
    std::filesystem::path dir;
    if (own && *own) {
        dir = own;
    } else if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
        dir = std::filesystem::path(xdg) / "edax" / "objects";
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        dir = std::filesystem::path(home) / ".cache" / "edax" / "objects";
    } else {
        return {};
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!std::filesystem::is_directory(dir, ec)) return {};
    return dir.string();
}

// What decides the object code besides the IR text. __DATE__/__TIME__: a rebuilt backend (its
// passes may have changed) doesn't reuse the objects of the previous one.
std::string object_cache_salt(const std::string& triple_name, const std::string& level, bool asan, bool tsan) {
    auto [cpu, features] = llvm_cpu_and_features();
    return std::format("edax-object-cache 1\nedaxc {} {} {}\nllvm {}\n{}\n{}\n{}\n{} {}{}\npic\n", EDAX_VERSION, __DATE__, __TIME__,
                       LLVM_VERSION_STRING, triple_name, cpu, features, level, asan ? "asan" : "", tsan ? "tsan" : "");
}

std::string hex_digest(llvm::ArrayRef<std::uint8_t> bytes) {
    std::string out;
    for (std::uint8_t b : bytes) out += std::format("{:02x}", b);
    return out;
}

// The unit of a function or global: the "edax-unit" attribute; "shared" for what the generator
// made without one (the runtime, `main`, helpers of tuples and arrays), "globals" for variables,
// vtables and type ids (one definition each). Private constants (strings, source locations) have
// no unit: each unit gets its own copy of those it uses.
const std::string no_unit;

struct Units {
    std::map<const llvm::GlobalValue*, std::string> of;
    std::map<std::string, std::vector<const llvm::GlobalValue*>> members;   // ordered: the build is deterministic

    static bool copied(const llvm::GlobalValue& gv) {
        const auto* g = llvm::dyn_cast<llvm::GlobalVariable>(&gv);
        return g && g->hasPrivateLinkage() && g->isConstant() && g->hasInitializer();
    }

    explicit Units(llvm::Module& mod) {
        for (llvm::Function& fn : mod) {
            if (fn.isDeclaration()) continue;
            std::string u = fn.hasFnAttribute("edax-unit") ? fn.getFnAttribute("edax-unit").getValueAsString().str() : "shared";
            of[&fn] = u;
            members[u].push_back(&fn);
        }
        for (llvm::GlobalVariable& g : mod.globals()) {
            if (g.isDeclaration()) continue;
            if (copied(g)) {
                of[&g] = no_unit;
                continue;
            }
            of[&g] = "globals";
            members["globals"].push_back(&g);
        }
    }

    // Small optimized functions of other units that an optimized unit calls get a copy of their
    // body in that unit (available_externally), so that they still inline there: the library's
    // code calls across its units all the time (an instance's methods, allocation, hashing).
    // The copy is part of the unit's text, so a change to the original changes the unit's key.
    std::map<std::string, std::set<const llvm::Function*>> imports;

    std::string default_level = "none";   // the mode's optimization(): functions without their own level

    bool optimized(const llvm::Function& fn) const {
        if (fn.isDeclaration() || fn.hasFnAttribute(llvm::Attribute::OptimizeNone)) return false;
        std::string level = fn.hasFnAttribute("edax-optimize") ? fn.getFnAttribute("edax-optimize").getValueAsString().str() : default_level;
        return optimization_rank(level) > 0;
    }

    void find_imports() {
        constexpr unsigned small = 120;   // instructions of unoptimized IR
        for (const auto& [gv, unit] : of) {
            const auto* caller = llvm::dyn_cast<llvm::Function>(gv);
            if (!caller || !optimized(*caller)) continue;
            for (const llvm::Instruction& i : llvm::instructions(*caller)) {
                const auto* call = llvm::dyn_cast<llvm::CallBase>(&i);
                const llvm::Function* callee = call ? call->getCalledFunction() : nullptr;
                if (!callee || !optimized(*callee) || callee->hasFnAttribute(llvm::Attribute::NoInline) ||
                    callee->getInstructionCount() > small) {
                    continue;
                }
                auto it = of.find(callee);
                if (it != of.end() && it->second != unit) imports[unit].insert(callee);
            }
        }
    }

    // The functions and globals the body of `fn` refers to (through constants and copied constants).
    void references(const llvm::Function& fn, std::set<const llvm::GlobalValue*>& out) const {
        std::vector<const llvm::Value*> work;
        for (const llvm::Instruction& i : llvm::instructions(fn)) {
            for (const llvm::Use& op : i.operands()) work.push_back(op.get());
        }
        std::set<const llvm::Value*> seen;
        while (!work.empty()) {
            const llvm::Value* v = work.back();
            work.pop_back();
            if (!seen.insert(v).second) continue;
            if (const auto* g = llvm::dyn_cast<llvm::GlobalValue>(v)) {
                auto it = of.find(g);
                if (it != of.end() && it->second.empty()) {
                    work.push_back(llvm::cast<llvm::GlobalVariable>(g)->getInitializer());
                } else {
                    out.insert(g);
                }
            } else if (const auto* c = llvm::dyn_cast<llvm::Constant>(v)) {
                for (const llvm::Use& op : c->operands()) work.push_back(op.get());
            }
        }
    }

    // The units that use `v` (through constants and copied constants).
    void users(const llvm::Value* v, std::set<std::string>& out) const {
        for (const llvm::User* u : v->users()) {
            if (const auto* i = llvm::dyn_cast<llvm::Instruction>(u)) {
                auto it = of.find(i->getFunction());
                if (it != of.end()) out.insert(it->second);
            } else if (const auto* g = llvm::dyn_cast<llvm::GlobalValue>(u)) {
                auto it = of.find(g);
                if (it == of.end()) continue;
                if (it->second.empty()) users(g, out);
                else out.insert(it->second);
            } else if (llvm::isa<llvm::Constant>(u)) {
                users(u, out);
            }
        }
    }
};

// Makes the module of one unit: its own functions and globals, declarations of what it uses from
// other units, copies of the private constants it uses. The compile unit of the debug info gets
// the unit's first file and no list of all enums, so that it doesn't depend on the rest.
// A unit's module as bitcode: the key of its cached object, and what a worker parses into its own
// context. Writing bitcode is several times faster than printing the IR as text.
std::string unit_bitcode(const llvm::Module& part) {
    std::string bytes;
    llvm::raw_string_ostream out(bytes);
    llvm::WriteBitcodeToFile(part, out);
    out.flush();
    return bytes;
}

std::unique_ptr<llvm::Module> unit_module(llvm::Module& whole, const Units& units, const std::string& unit) {
    llvm::ValueToValueMapTy map;
    const std::set<const llvm::Function*>* imported = nullptr;
    if (auto it = units.imports.find(unit); it != units.imports.end()) imported = &it->second;
    std::unique_ptr<llvm::Module> part = llvm::CloneModule(whole, map, [&](const llvm::GlobalValue* gv) {
        auto it = units.of.find(gv);
        if (it != units.of.end() && (it->second == unit || it->second.empty())) return true;
        const auto* fn = llvm::dyn_cast<llvm::Function>(gv);
        return fn && imported && imported->count(fn);
    });
    if (imported) {
        for (const llvm::Function* fn : *imported) {
            auto* copy = llvm::cast<llvm::Function>(map[fn]);
            copy->setLinkage(llvm::GlobalValue::AvailableExternallyLinkage);
            copy->setVisibility(llvm::GlobalValue::DefaultVisibility);
        }
    }
    for (bool erased = true; erased;) {   // what the unit doesn't use (copies point to copies)
        erased = false;
        for (llvm::GlobalVariable& g : llvm::make_early_inc_range(part->globals())) {
            if (g.use_empty() && (g.isDeclaration() || g.hasPrivateLinkage())) {
                g.eraseFromParent();
                erased = true;
            }
        }
    }
    for (llvm::Function& fn : llvm::make_early_inc_range(*part)) {
        if (fn.isDeclaration() && fn.use_empty()) fn.eraseFromParent();
    }
    // In name order: the whole module's order is the order things were generated in, which
    // depends on the rest of the program.
    std::vector<llvm::GlobalVariable*> globals;
    for (llvm::GlobalVariable& g : part->globals()) globals.push_back(&g);
    std::sort(globals.begin(), globals.end(), [](auto* a, auto* b) { return a->getName() < b->getName(); });
    for (llvm::GlobalVariable* g : globals) {
        g->removeFromParent();
        part->insertGlobalVariable(g);
    }
    std::vector<llvm::Function*> functions;
    for (llvm::Function& fn : *part) functions.push_back(&fn);
    std::sort(functions.begin(), functions.end(), [](auto* a, auto* b) { return a->getName() < b->getName(); });
    for (llvm::Function* fn : functions) {
        fn->removeFromParent();
        part->getFunctionList().push_back(fn);
    }
    if (llvm::NamedMDNode* cus = part->getNamedMetadata("llvm.dbg.cu")) {
        llvm::DIFile* first = nullptr;
        std::string   first_name;
        for (llvm::Function& fn : *part) {
            llvm::DISubprogram* sp = fn.isDeclaration() ? nullptr : fn.getSubprogram();
            if (sp && sp->getFile() && (!first || fn.getName() < first_name)) {
                first      = sp->getFile();
                first_name = fn.getName().str();
            }
        }
        if (!first) {
            part->eraseNamedMetadata(cus);
        } else {
            for (llvm::MDNode* node : cus->operands()) {
                auto* cu = llvm::dyn_cast<llvm::DICompileUnit>(node);
                if (!cu || !cu->isDistinct()) continue;
                cu->replaceEnumTypes(nullptr);
                cu->replaceOperandWith(0, first);
            }
        }
    }
    return part;
}

// Compiles `mod` unit by unit through the cache; `objects` gets the object files to link.
bool compile_units(llvm::Module& mod, const llvm::Target* target, const std::string& triple_name, const std::string& cache_dir,
                   const std::string& default_level, bool asan, bool tsan, std::vector<std::string>& objects, std::string& error) {
    Units units(mod);
    units.default_level = default_level;
    // A function or global another unit uses becomes visible to the other objects of the
    // program, and only to them (hidden).
    for (const auto& [gv, unit] : units.of) {
        if (unit.empty() || !gv->hasLocalLinkage()) continue;
        std::set<std::string> used_by;
        units.users(gv, used_by);
        used_by.erase(unit);
        if (used_by.empty()) continue;
        auto* g = const_cast<llvm::GlobalValue*>(gv);
        g->setLinkage(llvm::GlobalValue::ExternalLinkage);
        g->setVisibility(llvm::GlobalValue::HiddenVisibility);
    }
    // What an imported copy refers to is reached from the importing unit as well.
    units.find_imports();
    for (const auto& [unit, fns] : units.imports) {
        std::set<const llvm::GlobalValue*> refs;
        for (const llvm::Function* fn : fns) {
            refs.insert(fn);
            units.references(*fn, refs);
        }
        for (const llvm::GlobalValue* gv : refs) {
            auto it = units.of.find(gv);
            if (it == units.of.end() || it->second.empty() || it->second == unit || !gv->hasLocalLinkage()) continue;
            auto* g = const_cast<llvm::GlobalValue*>(gv);
            g->setLinkage(llvm::GlobalValue::ExternalLinkage);
            g->setVisibility(llvm::GlobalValue::HiddenVisibility);
        }
    }
    struct Miss {
        std::string text;
        std::string path;
        std::string level;
        std::string unit;
    };
    std::vector<Miss> misses;
    std::size_t       hits = 0;
    for (const auto& [unit, members] : units.members) {
        std::unique_ptr<llvm::Module> part = unit_module(mod, units, unit);
        std::string level = highest_level(*part, default_level);
        std::string text = unit_bitcode(*part);
        if (const char* dump = std::getenv("EDAX_CACHE_DUMP"); dump && *dump) {   // each unit's IR, to see why one changed
            std::string name = unit;
            std::replace_if(name.begin(), name.end(), [](char c) { return c == '/' || c == ':'; }, '_');
            std::error_code dump_ec;
            llvm::raw_fd_ostream dump_out(std::string(dump) + "/" + name + ".ll", dump_ec);
            part->print(dump_out, nullptr);
        }
        llvm::BLAKE3 hasher;
        hasher.update(object_cache_salt(triple_name, level, asan, tsan));
        hasher.update(text);
        std::string path = (std::filesystem::path(cache_dir) / ("llvmcache-" + hex_digest(hasher.final()) + ".o")).string();
        std::error_code ec;
        if (std::filesystem::file_size(path, ec) > 0 && !ec) {
            ++hits;
            // Used: pruning keeps it (pruneCache goes by the modification time).
            std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), ec);
        } else {
            misses.push_back(Miss{std::move(text), path, level, unit});
        }
        objects.push_back(path);
    }
    // The units that aren't cached yet, in parallel: each worker has its own LLVM context.
    std::vector<std::string> errors(misses.size());
    std::atomic<std::size_t> next{0};
    auto work = [&] {
        for (std::size_t i; (i = next.fetch_add(1)) < misses.size();) {
            const Miss& m = misses[i];
            llvm::LLVMContext ctx;
            llvm::SMDiagnostic err;
            std::unique_ptr<llvm::Module> part =
                llvm::parseIR(llvm::MemoryBufferRef(llvm::StringRef(m.text.data(), m.text.size()), "edax-unit.bc"), err, ctx);
            if (!part) {
                std::string text;
                llvm::raw_string_ostream os(text);
                err.print("edaxc", os);
                errors[i] = "internal error: a unit's LLVM IR does not parse\n" + text;
                continue;
            }
            // Written next to its final name and renamed: another edaxc may be writing it too.
            std::string temporary = std::format("{}.tmp{}-{}", m.path, llvm::sys::Process::getProcessId(), i);
            if (!emit_object(*part, target, triple_name, m.level, asan, tsan, temporary, errors[i])) {
                llvm::sys::fs::remove(temporary);
                continue;
            }
            if (std::error_code ec = llvm::sys::fs::rename(temporary, m.path)) {
                errors[i] = "cannot write " + m.path + ": " + ec.message();
                llvm::sys::fs::remove(temporary);
            }
        }
    };
    unsigned threads = std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(), static_cast<unsigned>(misses.size())));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work);
    work();
    for (std::thread& t : pool) t.join();
    for (const std::string& e : errors) {
        if (!e.empty()) {
            error = e;
            return false;
        }
    }
    if (const char* stats = std::getenv("EDAX_CACHE_STATS"); stats && *stats && std::string_view(stats) != "0") {
        llvm::errs() << std::format("edaxc: object cache: {} units, {} cached, {} compiled\n", units.members.size(), hits, misses.size());
        if (std::string_view(stats) == "2") {   // which, and where their IR is (to see why a unit changed)
            for (const Miss& m : misses) llvm::errs() << std::format("  compiled {} ({})\n", m.unit, m.path);
        }
    }
    if (llvm::Expected<llvm::CachePruningPolicy> policy =
            llvm::parseCachePruningPolicy("prune_interval=1h:prune_after=720h:cache_size_bytes=2g")) {
        llvm::pruneCache(cache_dir, *policy);
    } else {
        llvm::consumeError(policy.takeError());
    }
    return true;
}

// Optimized builds through the cache (ThinLTO). Each unit is optimized on its own up to the
// point where whole-program information matters (LLVM's ThinLTO pre-link pipeline), and the
// result, bitcode with a summary of what it defines and calls, is cached under the hash of the
// unit's IR. LLVM's ThinLTO then imports the functions worth inlining across units, internalizes
// what no other unit uses, optimizes each unit again and makes its object code; that step has a
// cache of its own, keyed by the unit and everything it imported. An edit recompiles the units it
// changed and the units that imported from them.
// EDAX_THINLTO=off keeps optimized builds in one module.
bool compile_thin(llvm::Module& mod, const llvm::Target* target, const std::string& triple_name, const std::string& cache_dir,
                  const std::string& level, const std::string& work_dir, std::vector<std::string>& objects, std::string& error) {
    Units units(mod);
    units.default_level = level;
    for (const auto& [gv, unit] : units.of) {   // what another unit uses: visible to the program's objects only
        if (unit.empty() || !gv->hasLocalLinkage()) continue;
        std::set<std::string> used_by;
        units.users(gv, used_by);
        used_by.erase(unit);
        if (used_by.empty()) continue;
        auto* g = const_cast<llvm::GlobalValue*>(gv);
        g->setLinkage(llvm::GlobalValue::ExternalLinkage);
        g->setVisibility(llvm::GlobalValue::HiddenVisibility);
    }
    ThinLtoJob job;
    job.triple    = triple_name;
    std::tie(job.cpu, job.features) = llvm_cpu_and_features();
    job.opt_level = level == "basic" ? 1 : 2;
    std::filesystem::path thin_dir = std::filesystem::path(cache_dir) / "thinlto";
    // LLVM's key for a ThinLTO object doesn't cover Edax's passes: the objects of each edaxc
    // build (and level) go to a directory of their own.
    {
        llvm::BLAKE3 hasher;
        hasher.update(object_cache_salt(triple_name, level, false, false));
        job.cache_dir = (thin_dir / ("objects-" + hex_digest(hasher.final()).substr(0, 16))).string();
    }
    job.work_dir  = work_dir;
    job.machine   = [target, triple_name, &job] {
        return make_target_machine(target, triple_name, llvm::TargetOptions(),
                                   job.opt_level == 1 ? llvm::CodeGenOptLevel::Less : llvm::CodeGenOptLevel::Aggressive);
    };
    job.passes           = [](llvm::PassBuilder& pb) { register_edax_passes(pb); };
    job.post_link_passes = [](llvm::PassBuilder& pb) { register_edax_passes(pb); };
    std::string salt = object_cache_salt(triple_name, level, false, false) + "thinlto-unit 2\n";
    struct Part {
        std::string text;
        std::string path;
    };
    std::vector<Part>        parts;
    std::vector<std::size_t> missing;
    std::error_code          ec;
    std::filesystem::create_directories(thin_dir, ec);
    for (const auto& [unit, members] : units.members) {
        std::unique_ptr<llvm::Module> part = unit_module(mod, units, unit);
        Part p;
        p.text = unit_bitcode(*part);
        llvm::BLAKE3 hasher;
        hasher.update(salt);
        hasher.update(p.text);
        p.path = (thin_dir / ("unit-" + hex_digest(hasher.final()) + ".bc")).string();
        if (std::filesystem::file_size(p.path, ec) > 0 && !ec) {
            std::filesystem::last_write_time(p.path, std::filesystem::file_time_type::clock::now(), ec);
        } else {
            missing.push_back(parts.size());
        }
        parts.push_back(std::move(p));
    }
    std::vector<std::string> errors(missing.size());
    std::atomic<std::size_t> next{0};
    auto work = [&] {
        for (std::size_t i; (i = next.fetch_add(1)) < missing.size();) {
            thin_prelink(job, parts[missing[i]].text, parts[missing[i]].path, errors[i]);
        }
    };
    unsigned threads = std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(), static_cast<unsigned>(missing.size())));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work);
    work();
    for (std::thread& t : pool) t.join();
    for (const std::string& e : errors) {
        if (!e.empty()) {
            error = e;
            return false;
        }
    }
    std::vector<std::string> bitcode;
    for (const Part& p : parts) bitcode.push_back(p.path);
    std::size_t compiled = 0;
    if (!thin_link(job, bitcode, objects, compiled, error)) return false;
    if (const char* stats = std::getenv("EDAX_CACHE_STATS"); stats && *stats && std::string_view(stats) != "0") {
        llvm::errs() << std::format("edaxc: ThinLTO: {} units, {} optimized again\n", parts.size(), missing.size());
    }
    if (llvm::Expected<llvm::CachePruningPolicy> policy =
            llvm::parseCachePruningPolicy("prune_interval=1h:prune_after=720h:cache_size_bytes=2g")) {
        llvm::pruneCache(thin_dir.string(), *policy);
        for (const auto& entry : std::filesystem::directory_iterator(thin_dir, ec)) {
            if (entry.is_directory(ec) && entry.path().filename().string().starts_with("objects-")) {
                llvm::pruneCache(entry.path().string(), *policy);
                std::filesystem::remove(entry.path(), ec);   // only when it is empty
            }
        }
    } else {
        llvm::consumeError(policy.takeError());
    }
    return true;
}

} // namespace

namespace {
bool build_module(std::unique_ptr<llvm::Module> mod, const std::string& out_path, const std::string& linker, std::string& error,
                  const CodegenOptions& codegen, const std::vector<std::string>& extra_inputs);
} // namespace

bool build_executable(std::string_view ir, const std::string& out_path, const std::string& linker, std::string& error,
                      const CodegenOptions& codegen, const std::vector<std::string>& extra_inputs) {
    initialize_targets();
    llvm::LLVMContext ctx;
    llvm::SMDiagnostic err;
    std::unique_ptr<llvm::Module> mod = llvm::parseIR(llvm::MemoryBufferRef(llvm::StringRef(ir.data(), ir.size()), "edax.ll"), err, ctx);
    if (!mod) {
        std::string text;
        llvm::raw_string_ostream os(text);
        err.print("edaxc", os);
        error = "internal error: the generated LLVM IR does not parse\n" + text;
        return false;
    }
    return build_module(std::move(mod), out_path, linker, error, codegen, extra_inputs);
}

bool build_executable(GeneratedProgram& program, const std::string& out_path, const std::string& linker, std::string& error,
                      const CodegenOptions& codegen, const std::vector<std::string>& extra_inputs) {
    if (!program.impl_ || !program.impl_->mod) {
        error = "internal error: no generated program";
        return false;
    }
    initialize_targets();
    return build_module(std::move(program.impl_->mod), out_path, linker, error, codegen, extra_inputs);
}

namespace {
bool build_module(std::unique_ptr<llvm::Module> mod, const std::string& out_path, const std::string& linker, std::string& error,
                  const CodegenOptions& codegen, const std::vector<std::string>& extra_inputs) {
    std::string bad;
    llvm::raw_string_ostream verify_out(bad);
    if (llvm::verifyModule(*mod, &verify_out)) {
        error = "internal error: the generated LLVM IR is invalid\n" + verify_out.str();
        return false;
    }
    std::string triple_name = target_triple();
    std::string lookup_error;
    const llvm::Target* target = lookup_target(triple_name, lookup_error);
    if (!target) {
        error = "no LLVM target for " + triple_name + ": " + lookup_error;
        return false;
    }
    // Per-function optimization (spec 34): `@std::optimize` and g.optimize_files mark functions
    // with "edax-optimize". The module runs the pipeline of the highest level asked for; functions
    // that asked for `none` (or, in an unoptimized build, didn't ask) are kept as written.
    std::string level = codegen.optimization;
    bool        own_code_optimized = optimization_rank(level) > 0;   // the program's own functions (not the library's)
    {
        std::string highest = level;
        bool        any     = false;
        for (llvm::Function& fn : *mod) {
            if (!fn.hasFnAttribute("edax-optimize")) continue;
            any = true;
            std::string l = fn.getFnAttribute("edax-optimize").getValueAsString().str();
            if (optimization_rank(l) > optimization_rank(highest)) highest = l;
        }
        if (any) {
            for (llvm::Function& fn : *mod) {
                if (fn.isDeclaration()) continue;
                std::string own = fn.hasFnAttribute("edax-optimize") ? fn.getFnAttribute("edax-optimize").getValueAsString().str() : level;
                bool library = fn.hasFnAttribute("edax-unit") && fn.getFnAttribute("edax-unit").getValueAsString().starts_with("std/");
                if (!library && optimization_rank(own) > 0) own_code_optimized = true;
                if (optimization_rank(own) == 0 && optimization_rank(highest) > 0) {
                    fn.removeFnAttr(llvm::Attribute::AlwaysInline);
                    fn.addFnAttr(llvm::Attribute::OptimizeNone);
                    fn.addFnAttr(llvm::Attribute::NoInline);
                } else if (own == "size") {
                    fn.addFnAttr(llvm::Attribute::OptimizeForSize);
                }
            }
            level = highest;
        }
    }
    bool asan = codegen.sanitize == "address";
    bool tsan = codegen.sanitize == "thread";
    if (asan || tsan) {   // the mode's sanitize() (spec 34): every Edax function is instrumented
        for (llvm::Function& fn : *mod) {
            if (fn.isDeclaration()) continue;
            fn.addFnAttr(asan ? llvm::Attribute::SanitizeAddress : llvm::Attribute::SanitizeThread);
        }
    }

    // Profile-guided optimization: the instrumented program needs LLVM's profile runtime.
    std::string profile_runtime;
    if (!codegen.profile_generate.empty()) {
        llvm::Triple t(triple_name);
        std::string  archive = std::format("libclang_rt.profile-{}.a", t.getArchName().str());
        std::string  found;
        for (std::string clang : {std::string("clang"), std::format("clang-{}", LLVM_VERSION_MAJOR)}) {
            std::string command = std::format("{} -print-file-name={} 2>/dev/null", clang, archive);
            if (FILE* p = popen(command.c_str(), "r")) {
                char buffer[4096];
                std::string text;
                while (std::fgets(buffer, sizeof buffer, p)) text += buffer;
                pclose(p);
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
                if (text.find('/') != std::string::npos && std::filesystem::exists(text)) {
                    found = text;
                    break;
                }
            }
        }
        if (found.empty()) {
            error = std::format("--profile-generate needs LLVM's profile runtime ({}): install compiler-rt for LLVM {} "
                                "(e.g. libclang-rt-{}-dev)", archive, LLVM_VERSION_MAJOR, LLVM_VERSION_MAJOR);
            return false;
        }
        profile_runtime = found;
    }
    if (!codegen.profile_use.empty() && !std::filesystem::exists(codegen.profile_use)) {
        error = std::format("the profile '{}' named by profile_guided() does not exist (record one with --profile-generate, "
                            "then merge it with llvm-profdata)", codegen.profile_use);
        return false;
    }
    bool pgo = !codegen.profile_generate.empty() || !codegen.profile_use.empty();

    // Unoptimized programs go through the object cache; shared libraries (plugins) don't yet:
    // their thread-locals can't be reached across objects the way an executable's can.
    // Optimized programs stay one module: compiled in units (with the small callees copied in)
    // bench/ ran 1.1-3.5x slower (string_build 3.5x, text_parse 1.4x, hash_int 1.3x), because
    // whole-program inlining, argument promotion and dead-function removal stop at units.
    std::vector<std::string> objects;
    bool                     temporary = true;
    std::string              cache_dir = !own_code_optimized && !codegen.shared && !pgo ? object_cache_dir() : std::string();
    std::string thin_dir;
    {
        // `basic` (the development mode): through the cache. `speed` stays one module unless
        // EDAX_THINLTO=on: in units, bench/ ran up to 1.5x slower (text_parse 1.5x, vec_push_sum
        // 1.25x, hash_int 1.15x), because functions another unit calls stay external.
        const char*      thin = std::getenv("EDAX_THINLTO");
        std::string_view how  = thin ? thin : "";
        bool             on   = how == "on" || how == "1" ? (level == "speed" || level == "basic")
                              : how == "off" || how == "0" ? false
                                                           : level == "basic";
        if (on && own_code_optimized && !codegen.shared && !pgo && !asan && !tsan) thin_dir = object_cache_dir();
    }
    if (!thin_dir.empty()) {
        set_target_triple(*mod, triple_name);
        std::unique_ptr<llvm::TargetMachine> machine(make_target_machine(target, triple_name, llvm::TargetOptions(), llvm::CodeGenOptLevel::Aggressive));
        mod->setDataLayout(machine->createDataLayout());
        std::string work = out_path + ".thin";
        std::filesystem::create_directories(work);
        if (!compile_thin(*mod, target, triple_name, thin_dir, level, work, objects, error)) return false;
    } else if (!cache_dir.empty()) {
        set_target_triple(*mod, triple_name);
        std::unique_ptr<llvm::TargetMachine> machine(make_target_machine(target, triple_name, llvm::TargetOptions(), llvm::CodeGenOptLevel::None));
        mod->setDataLayout(machine->createDataLayout());
        if (!compile_units(*mod, target, triple_name, cache_dir, codegen.optimization, asan, tsan, objects, error)) return false;
        temporary = false;
    } else {
        std::string object_path = out_path + ".o";
        if (!emit_object(*mod, target, triple_name, level, asan, tsan, object_path, error, codegen.profile_generate, codegen.profile_use)) {
            return false;
        }
        objects.push_back(object_path);
    }
    std::string command = linker + (codegen.shared ? " -shared" : "") + " -o '" + out_path + "'";
    for (const std::string& object : objects) command += " '" + object + "'";
    for (const std::string& input : extra_inputs) command += " '" + input + "'";
    if (!profile_runtime.empty()) command += " -u __llvm_profile_runtime '" + profile_runtime + "'";
    command += " -lm -ldl";
    int status = std::system(command.c_str());
    if (temporary) {
        for (const std::string& object : objects) llvm::sys::fs::remove(object);
    }
    if (status != 0) {
        error = "the linker '" + linker + "' failed";
        return false;
    }
    return true;
}
} // namespace

bool set_build_target(const std::string& platform, std::string& error) {
    if (platform.empty()) {
        g_target_triple.clear();
        return true;
    }
    llvm::Triple t(llvm::Triple::normalize(platform));
    if (!t.isArch64Bit() || !(t.isAArch64() || t.getArch() == llvm::Triple::x86_64 || t.getArch() == llvm::Triple::riscv64)) {
        error = std::format("'{}' is not a supported platform: edaxc compiles for 64-bit x86-64, AArch64 and RISC-V", platform);
        return false;
    }
    if (!t.isOSLinux()) {
        error = std::format("'{}' is not a supported platform: the standard library targets Linux", platform);
        return false;
    }
    g_target_triple = t.str();
    initialize_targets();
    std::string lookup_error;
    if (!lookup_target(g_target_triple, lookup_error)) {
        error = std::format("this edaxc's LLVM can't generate code for '{}': {}", platform, lookup_error);
        g_target_triple.clear();
        return false;
    }
    return true;
}

bool TargetPlatform::has(const std::string& capability) const {
    return std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end();
}

bool TargetPlatform::has_feature(const std::string& feature) const {
    return std::find(features.begin(), features.end(), feature) != features.end();
}

namespace {
TargetPlatform platform_for(const std::string& triple_name, const CpuLevel* level) {
    llvm::Triple   t(triple_name);
    TargetPlatform p;
    p.triple = t.str();
    p.os     = t.isOSLinux() ? "linux" : t.isOSWindows() ? "windows" : t.isMacOSX() ? "macos" : "none";
    if (level) {
        p.cpu          = level->name;
        p.features     = level->features;
        p.vector_bytes = level->vector_bytes;
    }
    if (p.os != "none") p.capabilities = {"os", "threads", "heap", "files", "dynamic_libraries"};
    if (level && (!level->features.empty() || t.getArch() == llvm::Triple::x86_64)) p.capabilities.push_back("simd");
    // Apple's ARM cores move 128-byte lines; x86-64 and the other ARM and RISC-V cores 64.
    p.cache_line = t.isAArch64() && t.isOSDarwin() ? 128 : 64;
    return p;
}
} // namespace

const TargetPlatform& target_platform() {
    static TargetPlatform current;
    current = platform_for(target_triple(), current_cpu_level());
    if (g_description) {
        current.os           = g_description->os;
        current.capabilities = g_description->capabilities;
        if (g_description->cache_line != 0) current.cache_line = g_description->cache_line;
    }
    return current;
}

TargetPlatform host_platform() {
    std::string host = llvm::sys::getDefaultTargetTriple();
    llvm::Triple t(host);
    const CpuLevel* level = nullptr;
    for (const CpuLevel& l : cpu_level_table()) {
        if (l.arch == t.getArch()) {
            level = &l;
            break;
        }
    }
    return platform_for(host, level);
}

bool set_build_cpu(const std::string& level, std::string& error) {
    if (level.empty()) {
        g_cpu_level.clear();
        return true;
    }
    const CpuLevel* l = find_cpu_level(level);
    if (!l) {
        error = std::format("'{}' is not a CPU level edaxc knows", level);
        return false;
    }
    llvm::Triple t(target_triple());
    if (l->arch != t.getArch()) {
        error = std::format("the CPU level '{}' is not for '{}'", level, t.str());
        return false;
    }
    g_cpu_level = level;
    return true;
}

const std::string& build_cpu() { return g_cpu_level; }

std::vector<std::string> cpu_levels() {
    llvm::Triple             t(target_triple());
    std::vector<std::string> out;
    for (const CpuLevel& l : cpu_level_table()) {
        if (l.arch == t.getArch()) out.push_back(l.name);
    }
    return out;
}

std::vector<std::string> cpu_level_features(const std::string& level) {
    const CpuLevel* l = find_cpu_level(level);
    return l ? l->features : std::vector<std::string>{};
}

std::string set_cpu_level_override(const std::string& level) {
    std::string before = g_level_override;
    g_level_override   = level;
    return before;
}

unsigned cpu_level_vector_bytes(const std::string& level) {
    const CpuLevel* l = find_cpu_level(level);
    return l ? l->vector_bytes : 16;
}

bool set_target_description(const TargetPlatform& description, std::string& error) {
    llvm::Triple t(llvm::Triple::normalize(description.triple));
    std::string  before = g_target_triple;
    g_target_triple     = t.str();
    initialize_targets();
    std::string lookup_error;
    if (!lookup_target(t.str(), lookup_error)) {
        error = std::format("this edaxc's LLVM can't generate code for '{}': {}", description.triple, lookup_error);
        g_target_triple = before;
        return false;
    }
    if (!set_build_cpu(description.cpu, error)) return false;
    g_description = description;
    return true;
}

bool target_is_host() { return g_target_triple.empty() || llvm::Triple(g_target_triple).getArch() == llvm::Triple(llvm::sys::getDefaultTargetTriple()).getArch(); }

} // namespace edax

namespace edax::ct::quad {
namespace {
llvm::APFloat make(u128 q) {
    std::uint64_t words[2] = {static_cast<std::uint64_t>(q), static_cast<std::uint64_t>(q >> 64)};
    return llvm::APFloat(llvm::APFloat::IEEEquad(), llvm::APInt(128, words));
}
u128 bits(const llvm::APFloat& f) {
    llvm::APInt i = f.bitcastToAPInt();
    return (static_cast<u128>(i.getRawData()[1]) << 64) | i.getRawData()[0];
}
} // namespace

u128 from_text(std::string_view decimal) {
    llvm::APFloat f(llvm::APFloat::IEEEquad());
    auto r = f.convertFromString(decimal, llvm::APFloat::rmNearestTiesToEven);
    if (!r) {
        llvm::consumeError(r.takeError());
        return from_double(std::strtod(std::string(decimal).c_str(), nullptr));
    }
    return bits(f);
}

u128 from_double(double v) {
    llvm::APFloat f(v);
    bool lost = false;
    f.convert(llvm::APFloat::IEEEquad(), llvm::APFloat::rmNearestTiesToEven, &lost);
    return bits(f);
}

u128 from_integer(u128 value, bool is_signed, int width) {
    std::uint64_t words[2] = {static_cast<std::uint64_t>(value), static_cast<std::uint64_t>(value >> 64)};
    llvm::APInt   i(128, words);
    if (width < 128) i = is_signed ? i.trunc(static_cast<unsigned>(width)).sext(128) : i.trunc(static_cast<unsigned>(width)).zext(128);
    llvm::APFloat f(llvm::APFloat::IEEEquad());
    f.convertFromAPInt(i, is_signed, llvm::APFloat::rmNearestTiesToEven);
    return bits(f);
}

double to_double(u128 q) {
    llvm::APFloat f    = make(q);
    bool          lost = false;
    f.convert(llvm::APFloat::IEEEdouble(), llvm::APFloat::rmNearestTiesToEven, &lost);
    return f.convertToDouble();
}

u128 apply(char op, u128 a, u128 b) {
    llvm::APFloat x = make(a);
    llvm::APFloat y = make(b);
    auto          rm = llvm::APFloat::rmNearestTiesToEven;
    switch (op) {
    case '+': x.add(y, rm); break;
    case '-': x.subtract(y, rm); break;
    case '*': x.multiply(y, rm); break;
    case '/': x.divide(y, rm); break;
    case '%': x.mod(y); break;
    default: break;
    }
    return bits(x);
}

int compare(u128 a, u128 b) {
    switch (make(a).compare(make(b))) {
    case llvm::APFloat::cmpLessThan: return -1;
    case llvm::APFloat::cmpEqual: return 0;
    case llvm::APFloat::cmpGreaterThan: return 1;
    default: return 2;
    }
}

bool to_integer(u128 q, bool is_signed, int width, u128& out, bool& exact) {
    llvm::APFloat  f = make(q);
    if (f.isNaN()) return false;
    llvm::APSInt   i(static_cast<unsigned>(width), !is_signed);
    bool           is_exact = false;
    auto           st = f.convertToInteger(i, llvm::APFloat::rmTowardZero, &is_exact);
    if (st & llvm::APFloat::opInvalidOp) return false;
    exact            = is_exact;
    llvm::APInt wide = is_signed ? i.sext(128) : i.zext(128);
    out              = (static_cast<u128>(wide.getRawData()[1]) << 64) | wide.getRawData()[0];
    if (width < 128) out &= (u128{1} << width) - 1;
    return true;
}

std::string to_text(u128 q) {
    llvm::APFloat f = make(q);
    if (f.isNaN()) return "NaN";
    if (f.isInfinity()) return f.isNegative() ? "-inf" : "inf";
    llvm::SmallString<64> out;
    f.toString(out, 0, 0, false);   // the shortest digits that read back exactly
    std::string text(out.str());
    if (text.find_first_of(".eEn") == std::string::npos) text += ".0";
    return text;
}
} // namespace edax::ct::quad
