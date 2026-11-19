#include "rift/llvm_backend.hpp"
#include "rift/ast_walk.hpp"

#include "rift/ast_printer.hpp"
#include "rift/compile_time.hpp"
#include "rift/types.hpp"

#include <functional>

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
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAArch64.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <format>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace rift {
namespace {

using namespace ast;
using sema::Module;
using sema::Symbol;
using sema::SymbolKind;
using types::Kind;

/// The lines of `text`, each with its newline.
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
// riftc builds with LLVM 18 to 21; the few APIs that changed between them go through these.

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

llvm::TargetMachine* make_target_machine(const llvm::Target* target, const std::string& triple, const llvm::TargetOptions& options,
                                         llvm::CodeGenOptLevel level) {
#if LLVM_VERSION_MAJOR >= 21
    return target->createTargetMachine(llvm::Triple(triple), "generic", "", options, llvm::Reloc::PIC_, std::nullopt, level);
#else
    return target->createTargetMachine(triple, "generic", "", options, llvm::Reloc::PIC_, std::nullopt, level);
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
} // namespace rift

// The targets riftc can cross-compile for (spec 25: `--target=`), besides the host.
extern "C" {
void LLVMInitializeAArch64TargetInfo();
void LLVMInitializeAArch64Target();
void LLVMInitializeAArch64TargetMC();
void LLVMInitializeAArch64AsmPrinter();
void LLVMInitializeX86TargetInfo();
void LLVMInitializeX86Target();
void LLVMInitializeX86TargetMC();
void LLVMInitializeX86AsmPrinter();
void LLVMInitializeRISCVTargetInfo();
void LLVMInitializeRISCVTarget();
void LLVMInitializeRISCVTargetMC();
void LLVMInitializeRISCVAsmPrinter();
}

namespace rift {
namespace {

/// The platform being compiled for: empty for the host (spec 25, `--target=`).
std::string g_target_triple;

/// The LLVM triple riftc compiles for.
std::string target_triple() { return g_target_triple.empty() ? llvm::sys::getDefaultTargetTriple() : g_target_triple; }

void initialize_targets() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
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
}

/// The data layout of the target, which decides the sizes and alignments of enum payloads.
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

/// Length-prefixed name parts, as in the C backend: `r_3geo6shapes1f`.
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

/// The name parts that make a symbol unique: its module (a file) and its namespace (spec 30).
std::vector<std::string> symbol_scope(const sema::Symbol* s) {
    std::vector<std::string> out = split_module(s->module ? s->module->name : "");
    if (!s->ns.empty()) {
        for (std::string& part : split_module(s->ns)) out.push_back(std::move(part));
    }
    return out;
}

/// Thrown after an E6001 was reported for a construct the backend does not handle yet. The
/// function being generated is abandoned; the module is discarded because errors were reported.
struct Unsupported {};

class Gen {
public:
    Gen(sema::Program& program, const SourceManager& sources, DiagnosticEngine& diags, LlvmOptions options,
        llvm::LLVMContext& ctx)
        : program_(program), tc_(*program.types), sources_(sources), diags_(diags), options_(options), ctx_(ctx),
          mod_(std::make_unique<llvm::Module>("rift", ctx)), b_(ctx) {
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
        if (p.string().starts_with("<std>/")) {   // the library's files: in the tree riftc was built from, if it is still there
            std::filesystem::path tree = std::filesystem::path(RIFT_STD_DIR) / p.string().substr(6);
            p = std::filesystem::exists(tree, ec) ? tree : std::filesystem::path("/<std>") / p.string().substr(6);
        }
        std::filesystem::path abs = p.is_absolute() || p.string().starts_with("<") ? p : std::filesystem::absolute(p, ec);
        llvm::DIFile* f = dib_->createFile(abs.filename().string(), abs.parent_path().string());
        if (!unit_) {
            unit_ = dib_->createCompileUnit(llvm::dwarf::DW_LANG_C_plus_plus, f, "riftc " RIFT_VERSION, false, "", 0);
        }
        di_files_[id] = f;
        return f;
    }

    /// A DISubprogram for `fn`, written at `span`. With `full` debug info (spec 24) the function is
    /// named as written, `module::type::name`, which is also its linkage name.
    void debug_function(llvm::Function* fn, Span span, const std::string& name) {
        if (!dib_ || !fn) return;
        llvm::DIFile* file = di_file(span.file);
        unsigned      line = sources_.file(span.file).line_col(span.begin).line;
        llvm::DISubroutineType* type = dib_->createSubroutineType(dib_->getOrCreateTypeArray({}));
        std::string linkage = options_.debug_variables ? name : fn->getName().str();
        llvm::DISubprogram* sp = dib_->createFunction(file, name, linkage, file, line, type, line,
                                                      llvm::DINode::FlagPrototyped, llvm::DISubprogram::SPFlagDefinition);
        fn->setSubprogram(sp);
        // The prologue (storing the parameters) has no line, as in C/C++ at -O0: stepping into the
        // function stops at its first statement, with the parameters in place (spec 24).
        b_.SetCurrentDebugLocation(llvm::DILocation::get(ctx_, options_.debug_variables ? 0 : line, 0, sp));
    }

    // --- debug info for variables (spec 24: debug_info() == full) ---------------------------

    std::map<const types::Type*, llvm::DIType*> di_types_;
    std::map<std::string, llvm::DINamespace*>   di_namespaces_;

    /// The namespace of a module (`std`, `geometry`), nested for `std::filesystem`.
    llvm::DIScope* di_namespace(const std::string& module) {
        if (module.empty()) return unit_;
        if (auto it = di_namespaces_.find(module); it != di_namespaces_.end()) return it->second;
        std::size_t   cut    = module.rfind("::");
        llvm::DIScope* parent = cut == std::string::npos ? static_cast<llvm::DIScope*>(unit_) : di_namespace(module.substr(0, cut));
        auto* ns = dib_->createNameSpace(parent, cut == std::string::npos ? module : module.substr(cut + 2), false);
        di_namespaces_[module] = ns;
        return ns;
    }

    llvm::DIType* di_basic(const std::string& rift_name, const std::string& c_name, unsigned bits, unsigned encoding) {
        llvm::DIType* base = dib_->createBasicType(c_name, bits, encoding);
        return dib_->createTypedef(base, rift_name, nullptr, 0, unit_);
    }

    /// The DWARF type of a Rift type: built-in types are typedefs with their Rift names, structs
    /// and classes follow the backend's layout, simple enums are enumerations, enums with payloads
    /// are `struct { tag; union { ... } payload; }`.
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
        case Kind::float_: out = di_basic(t->name, t->bits == 32 ? "float" : "double", static_cast<unsigned>(t->bits), llvm::dwarf::DW_ATE_float); break;
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

    /// A struct (or class, enum with payloads, `std::str`, lambda) as a DWARF structure.
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
            llvm::Type* storage = st->getElementType(1);
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
            llvm::DIType* tag = dib_->createEnumerationType(scope, name + "::tag", nullptr, 0, dl.getTypeAllocSizeInBits(st->getElementType(0)),
                                                            8, dib_->getOrCreateArray(values), nullptr);
            member("tag", 0, tag);
            llvm::DIType* u = dib_->createUnionType(scope, name + "::payload", nullptr, 0, dl.getTypeAllocSizeInBits(storage),
                                                    static_cast<std::uint32_t>(dl.getABITypeAlign(storage).value() * 8), llvm::DINode::FlagZero,
                                                    dib_->getOrCreateArray(alts));
            member("payload", 1, u);
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

    /// Declares the variable stored at `slot` for the debugger.
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

    /// The debug location of the code generated next: `span` in the current function.
    void debug_at(Span span) {
        if (!dib_) return;
        llvm::DISubprogram* sp = cur_fn_ ? cur_fn_->getSubprogram() : nullptr;
        if (!sp || span.file != di_file_of(sp)) {
            if (!sp) b_.SetCurrentDebugLocation(llvm::DebugLoc());
            return;
        }
        LineCol lc = sources_.file(span.file).line_col(span.begin);
        b_.SetCurrentDebugLocation(llvm::DILocation::get(ctx_, lc.line, lc.column, sp));
    }

    FileId di_file_of(const llvm::DISubprogram* sp) {
        for (const auto& [id, f] : di_files_) {
            if (f == sp->getFile()) return id;
        }
        return static_cast<FileId>(-1);
    }

    /// Code made for one function while another was being generated can carry the other's
    /// locations; functions without a subprogram carry none, and calls need one.
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
        // `@std::test` functions of the entry file (spec 25, 29): `riftc test` runs them one by one.
        std::vector<const Symbol*> tests;
        if (program_.entry) {
            for (const Symbol* s : program_.entry->item_order) {
                if (s->kind == SymbolKind::function && s->decl && sema::find_annotation(s->decl->annotations, "std::test")) tests.push_back(s);
            }
        }
        Symbol* main = options_.all_functions || !tests.empty() ? find_main_quietly() : find_main();
        if (!main && !options_.all_functions && tests.empty()) return nullptr;
        declare_runtime();
        if (program_.entry && program_.entry->source) inspect_file_ = program_.entry->source->id;
        if (main) rift_main_ = function_of(main);
        std::vector<llvm::Function*> test_fns;
        for (const Symbol* t : tests) test_fns.push_back(function_of(t));
        // Rift functions exported to C exist even when no Rift code calls them: C may (spec 13).
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
                    if (s->kind != SymbolKind::function || s->function()->is_template) continue;
                    if (s->function()->is_compile_time && !s->function()->is_run_time) continue;   // never runs
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
            while (!global_queue_.empty()) {
                const Symbol* g = global_queue_.front();
                global_queue_.pop_front();
                gen_global(g);
            }
        }
        if (!tests.empty()) {
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

    /// A private constant holding `text` (NUL-terminated); its address is a `ptr`.
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

    /// Names as in the C backend: `r_<module parts><type><name>`, overloads numbered in order.
    std::string function_name(const Symbol* s) {
        std::vector<std::string> parts = symbol_scope(s);
        if (s->parent) {
            parts.push_back(s->parent->name);
            if (s->parent->template_of) parts.push_back("0i" + std::to_string(s->parent->instance_index));
        }
        parts.push_back(s->name == "~" + (s->parent ? s->parent->name : "") ? "drop" : s->name);
        if (s->template_of) parts.push_back("0i" + std::to_string(s->instance_index));
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

    /// The LLVM function for `s`, declared on first use; its body is generated later.
    llvm::Function* function_of(const Symbol* s) {
        if (auto it = fns_.find(s); it != fns_.end()) return it->second;
        const FunctionDecl* f = body_decl(s);
        if (!f) unsupported(s->span, "this kind of function");
        if (f->is_template && !s->template_of) unsupported(s->span, "calling templates");
        if (f->is_extern && f->abi == "cpp") unsupported(s->span, "'extern(std::build::language::cpp)' (C++ linkage)");
        if (f->is_extern && !f->body) return extern_function(s, *f);   // a C function: imported
        if (!f->body) unsupported(s->span, "functions without a body");
        // One module is the whole program: Rift functions are internal (LLVM may inline, specialize
        // or drop them); C sees only `main` and the exported wrappers. Rift never unwinds: a panic
        // aborts (spec 11).
        auto* fn = llvm::Function::Create(function_type(s), llvm::Function::InternalLinkage, function_name(s), mod_.get());
        fn->addFnAttr(llvm::Attribute::NoUnwind);
        const Symbol* annotated = s->template_of && s->template_of->kind == s->kind ? s->template_of : s;
        if (annotated->decl) {   // @std::inline_always, @std::inline_never, @std::cold (spec 29)
            const auto& uses = annotated->decl->annotations;
            if (sema::find_annotation(uses, "std::inline_always")) fn->addFnAttr(llvm::Attribute::AlwaysInline);
            if (sema::find_annotation(uses, "std::inline_never")) fn->addFnAttr(llvm::Attribute::NoInline);
            if (sema::find_annotation(uses, "std::cold")) {
                fn->addFnAttr(llvm::Attribute::Cold);
                fn->addFnAttr(llvm::Attribute::NoInline);
            }
        }
        fns_[s] = fn;
        queue_.push_back(s);
        if (f->is_extern) export_function(s, *f, fn);   // a Rift function that C can call
        return fn;
    }

    /// The LLVM type of the Rift function `s` (methods take `self` first). Also for abstract and
    /// interface methods, which have no body: dynamic calls use it.
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

    /// `extern(c)` with a body (spec 13): C calls it under its own name. The exported symbol is a
    /// wrapper that converts C's byte-sized `bool` and calls the Rift function.
    void export_function(const Symbol* s, const FunctionDecl& f, llvm::Function* target) {
        if (has_self(s)) unsupported(s->span, "exporting methods to C");
        llvm::Function*                  decl     = extern_function_type(s, f, f.name + ".export");
        llvm::Function*                  wrapper  = llvm::Function::Create(decl->getFunctionType(), llvm::Function::ExternalLinkage,
                                                                           f.name, mod_.get());
        wrapper->copyAttributesFrom(decl);
        decl->eraseFromParent();
        llvm::IRBuilderBase::InsertPoint saved = b_.saveIP();
        b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", wrapper));
        std::vector<llvm::Value*> args;
        for (unsigned i = 0; i < wrapper->arg_size(); ++i) {
            llvm::Value* a = wrapper->getArg(i);
            llvm::Type*  want = target->getFunctionType()->getParamType(i);
            args.push_back(want->isIntegerTy(1) ? b_.CreateICmpNE(a, llvm::ConstantInt::get(a->getType(), 0)) : a);
        }
        llvm::Value* r = b_.CreateCall(target, args);
        if (wrapper->getReturnType()->isVoidTy()) {
            b_.CreateRetVoid();
        } else {
            b_.CreateRet(r->getType()->isIntegerTy(1) ? b_.CreateZExt(r, wrapper->getReturnType()) : r);
        }
        b_.restoreIP(saved);
    }

    /// An `extern(c)` function (spec 13): declared under its own name with the C calling
    /// convention. Small integers are widened as C does; structs are passed by pointer only.
    llvm::Function* extern_function(const Symbol* s, const FunctionDecl& f) {
        llvm::Function* fn = mod_->getFunction(f.name);
        if (!fn) fn = extern_function_type(s, f, f.name);
        fns_[s] = fn;
        return fn;
    }

    /// A declaration with C's types for `f` (bool as a byte, small integers widened).
    llvm::Function* extern_function_type(const Symbol* s, const FunctionDecl& f, const std::string& name) {
        (void)s;
        auto c_type = [&](const types::Type* t, Span span) -> llvm::Type* {
            cur_ = span;
            if (t->is(Kind::bool_)) return llvm::Type::getInt8Ty(ctx_);
            if (t->is(Kind::user) && t->symbol->kind != SymbolKind::enum_) {
                unsupported(span, "passing structs by value to extern functions (pass a std::raw_ptr)");
            }
            if (t->is(Kind::reference)) unsupported(span, "references in extern functions (use std::raw_ptr)");
            return lower_type(t);
        };
        std::vector<llvm::Type*>        params;
        std::vector<const types::Type*> types;
        for (const Param& p : f.params) {
            const types::Type* t = tc_.lower(*p.type, nullptr);
            types.push_back(t);
            params.push_back(c_type(t, p.span));
        }
        const types::Type* rt  = f.result ? tc_.lower(*f.result, nullptr) : nullptr;
        llvm::Type*        ret = is_void(rt) ? llvm::Type::getVoidTy(ctx_) : c_type(rt, f.result->span);
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
                if (auto k = extend(types[i]); k != llvm::Attribute::None) fn->addParamAttr(i, k);
            }
            if (rt && !is_void(rt)) {
                if (auto k = extend(rt); k != llvm::Attribute::None) fn->addRetAttr(k);
            }
        }
        return fn;
    }

    /// A function's result type; `deduce` uses the type the checker found (spec 15).
    const types::Type* result_type(const FunctionDecl& f, const std::map<const Symbol*, const types::Type*>* subst) {
        if (f.result->kind == TypeKind::deduce) {
            if (!f.deduced_result) unsupported(f.result->span, "this 'deduce' function");
            return f.deduced_result;
        }
        return tc_.lower(*f.result, subst);
    }

    // --- lambdas (spec 18) ----------------------------------------------------------------

    /// A lambda's value: its captures, in the order written. A value capture holds the value, a
    /// reference capture the address; a lambda without captures holds one unused byte.
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
        auto* st = llvm::StructType::create(ctx_, fields, "closure");
        return closure_types_[t] = st;
    }

    /// The address of a local's object (through a reference local, what it refers to).
    llvm::Value* local_address(const Symbol* s, Span span) {
        auto it = locals_.find(s);
        if (it == locals_.end()) unsupported(span, "capturing this name");
        if (s->type && s->type->is(Kind::reference)) return b_.CreateLoad(ptr_type(), it->second);
        return it->second;
    }

    /// `[captures](params) { body }`: builds the closure value; the body becomes a function of its own.
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
                                          "rl_" + std::to_string(lambda_fns_.size()), mod_.get());
        lambda_fns_[&l] = fn;
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
        } catch (const Unsupported&) {
        }
    }

    /// Calls the lambda held by `callee` (its type says which lambda it is).
    llvm::Value* closure_call(Expr& callee, const types::Type* t, std::vector<Expr*> args) {
        const LambdaExpr* l   = tc_.lambda_of(t);
        llvm::Value*      env = object_address(callee);
        std::vector<llvm::Value*> values{env};
        for (std::size_t i = 0; i < args.size() && i + 1 < t->args.size(); ++i) {
            if (t->args[i]->is(Kind::reference)) {
                values.push_back(object_address(*args[i]));
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

    /// The LLVM type of a value of Rift type `t`. Booleans are `i1` in values; they are stored as
    /// `i8` in memory once structs are lowered (see the interop layout notes).
    llvm::Type* lower_type(const types::Type* t) {
        if (!t) unsupported(cur_, "values of an unknown type");
        switch (t->kind) {
        case Kind::int_: return llvm::IntegerType::get(ctx_, static_cast<unsigned>(t->bits));
        case Kind::float_: return t->bits == 32 ? llvm::Type::getFloatTy(ctx_) : llvm::Type::getDoubleTy(ctx_);
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
        case Kind::function: return llvm::StructType::get(ctx_, {ptr_type(), ptr_type(), ptr_type()});   // { captures, code, drop }
        case Kind::fn_ptr: return ptr_type();
        case Kind::array: return llvm::ArrayType::get(lower_type(t->inner), tc_.array_count(t));
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

    static constexpr const char* no_std = "the standard library is not written yet";

    // --- functions --------------------------------------------------------------------------

    void gen_function(const Symbol* s) {
        const FunctionDecl* f  = body_decl(s);
        llvm::Function*     fn = fns_[s];
        cur_                   = s->span;
        try {
            cur_fn_ = fn;
            debug_function(fn, s->span, options_.debug_variables ? display_name(s) : s->name);
            locals_.clear();
            loops_.clear();
            scopes_.clear();
            self_ = nullptr;
            start_inspecting(s->span);
            const auto* subst = sema::substitution_of(s);
            result_           = f->result ? result_type(*f, subst) : nullptr;
            llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx_, "entry", fn);
            b_.SetInsertPoint(entry);
            open_scope();
            unsigned offset = 0;
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
        } catch (const Unsupported&) {
            // Reported. The function keeps a partial body; the module is discarded.
        }
        inspect_ = false;
    }

    /// An alloca in the entry block, so that loops do not grow the stack.
    llvm::AllocaInst* entry_alloca(llvm::Type* t, const std::string& name) {
        llvm::BasicBlock& entry = cur_fn_->getEntryBlock();
        llvm::IRBuilder<> tmp(&entry, entry.begin());
        return tmp.CreateAlloca(t, nullptr, name);
    }

    bool terminated() const { return b_.GetInsertBlock()->getTerminator() != nullptr; }

    /// A setting of the build mode (spec 24), "" without the standard library.
    std::string mode_setting(const char* name) const {
        auto it = program_.mode_settings.find(name);
        return it == program_.mode_settings.end() ? std::string() : it->second;
    }

    /// After a `return`, `break` or `continue`: following code goes into a block nothing jumps to.
    void open_dead_block() { b_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "dead", cur_fn_)); }

    /// Panics with `message` at `span` unless `ok` holds (spec 16). `ok` is an i1.
    void check(llvm::Value* ok, Span span, const std::string& message) {
        if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(ok); c && c->isOne()) return;   // cannot fail
        if (inspect_) {
            insight(span, "checks", "check", "range", "can panic: " + message,
                    "A run-time check (spec 16, 23): when it fails, the program panics with this message. Checks that cannot fail "
                    "(a constant divisor, an exhaustive match) are not generated.");
        }
        llvm::BasicBlock* bad  = llvm::BasicBlock::Create(ctx_, "panic", cur_fn_);
        llvm::BasicBlock* good = llvm::BasicBlock::Create(ctx_, "ok", cur_fn_);
        b_.CreateCondBr(ok, good, bad);
        b_.SetInsertPoint(bad);
        b_.CreateCall(rift_panic_str_, {loc_global(span), message_bytes(message), llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), message.size())});
        b_.CreateUnreachable();
        b_.SetInsertPoint(good);
    }

    /// Where the current call of a function with run_time entry checks was made (null: unknown,
    /// e.g. through a function pointer). Set by the caller around the call (spec 23).
    llvm::GlobalVariable* call_site_global() {
        if (auto* g = mod_->getNamedGlobal("rift_call_site")) return g;
        auto* g = new llvm::GlobalVariable(*mod_, ptr_type(), false, llvm::GlobalValue::InternalLinkage,
                                           llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0)), "rift_call_site");
        g->setThreadLocalMode(llvm::GlobalValue::LocalExecTLSModel);   // each thread has its own call site (spec 27)
        return g;
    }

    /// Whether `s` starts with run_time checks that are compiled in.
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

    /// The source location `rift_loc` global for `span`: `{ file, line, column }`.
    llvm::Constant* loc_global(Span span) {
        auto key = std::make_pair(span.file, span.begin);
        if (auto it = locs_.find(key); it != locs_.end()) return it->second;
        const SourceFile& file = sources_.file(span.file);
        LineCol           lc   = file.line_col(span.begin);
        llvm::Constant*   path = string_global(file.path());
        llvm::Constant*   init = llvm::ConstantStruct::get(
            loc_type_, {path, llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), lc.line),
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), lc.column)});
        auto* g = new llvm::GlobalVariable(*mod_, loc_type_, true, llvm::GlobalValue::PrivateLinkage, init,
                                           std::format("rift_loc_{}", locs_.size()));
        locs_[key] = g;
        return g;
    }

    // --- statements -------------------------------------------------------------------------

    /// A block: its locals are destroyed at its end. The value is the tail expression's, or its
    /// address when the function returns a reference.
    llvm::Value* gen_block(BlockExpr& blk, bool as_address = false) {
        open_scope();
        for (StmtPtr& st : blk.statements) gen_stmt(*st);
        llvm::Value* v = nullptr;
        if (blk.tail) {
            bool address = as_address && !is_void(types::strip_ref(blk.tail->type));
            v = address ? object_address(*blk.tail) : gen_expr(*blk.tail);
            if (!terminated()) moved_out(*blk.tail);
        }
        drop_site_ = Span{blk.span.file, blk.span.end > blk.span.begin ? blk.span.end - 1 : blk.span.end, blk.span.end};
        drop_kind_ = "drop";
        close_scope();
        return v;
    }

    /// One statement. An unsupported construct is reported and the rest of the function still
    /// generates, so that every such construct gets its error.
    void gen_stmt(Stmt& s) {
        try {
            gen_stmt_inner(s);
        } catch (const Unsupported&) {
        }
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
                 : vt->is(Kind::reference)                           ? object_address(*v.init)
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

    /// The value of `e`, or null for an expression of type void or never.
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
            unsupported(e.span, "indexing");
        case ExprKind::call: return call_value(e.as<CallExpr>());
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

    llvm::Value* float_literal(const LiteralExpr& lit, const types::Type* t) {
        if (!t || !t->is_float()) unsupported(lit.span, "float literals of this type");
        std::string text;
        for (char c : lit.text) {
            if (c != '_') text += c;
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
        if (u.op_symbol) {   // a user-defined `-` or `~` (spec 16)
            if (has_self(u.op_symbol)) return call_symbol(u.op_symbol, object_address(*u.operand), {}, u.span);
            return call_symbol(u.op_symbol, nullptr, {u.operand.get()}, u.span);
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
            check(b_.CreateICmpNE(a, minimum), u.span, std::format("integer overflow in '-' ({})", t->name));
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

    /// A condition: a `bool`, or a raw pointer, which is true when it is not null (spec 02, 13).
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
        if (bx.op == TokenKind::amp_amp || bx.op == TokenKind::pipe_pipe) return short_circuit(bx);
        if (bx.op_symbol) return user_operator(bx);
        const types::Type* lt = types::strip_ref(bx.lhs->type);
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

    /// A binary operator the program defines (spec 16). `!=` is the negated `==`, and `<`, `<=`,
    /// `>`, `>=` compare the `std::ordering` from `<=>` with zero.
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

    /// A user-defined `operator==` for `t`, or null.
    static const Symbol* equality_operator(const types::Type* t) {
        if (!t || !t->is(Kind::user)) return nullptr;
        auto it = t->symbol->members.find("==");
        if (it == t->symbol->members.end()) return nullptr;
        for (const Symbol* m : it->second) {
            if (m->function() && m->function()->is_operator) return m;
        }
        return nullptr;
    }

    /// Whether the objects at `pa` and `pb` (both of type `t`) are equal: what a defaulted
    /// `operator==` does, field by field (spec 16).
    llvm::Value* values_equal(const types::Type* t, llvm::Value* pa, llvm::Value* pb, Span span) {
        if (t && t->is(Kind::user) && t->symbol->kind != SymbolKind::enum_) {
            if (const Symbol* op = equality_operator(t); op && !body_decl(op)->is_defaulted) {
                return b_.CreateCall(function_of(op), {pa, pb});
            }
            llvm::StructType* st  = llvm::cast<llvm::StructType>(user_type(t->symbol));
            llvm::Value*      all = llvm::ConstantInt::getTrue(ctx_);
            for (const Symbol* f : fields_of(t->symbol)) {
                unsigned i = field_index(f);
                llvm::Value* e = values_equal(field_type(f), b_.CreateStructGEP(st, pa, i), b_.CreateStructGEP(st, pb, i), span);
                all = b_.CreateAnd(all, e);
            }
            return all;
        }
        llvm::Type*  lt = lower_type(t);
        llvm::Value* a  = b_.CreateLoad(lt, pa);
        llvm::Value* b  = b_.CreateLoad(lt, pb);
        if (is_str(t)) return str_equal(a, b, true);
        if (lt->isFloatingPointTy()) return b_.CreateFCmpOEQ(a, b);
        if (lt->isIntegerTy()) return b_.CreateICmpEQ(a, b);
        unsupported(span, std::format("comparing values of type '{}'", types::to_string(t)));
    }

    /// `&&` and `||`: the right operand is evaluated only when needed.
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

    /// Arithmetic and bit operations on two values of type `t` (the left operand's type). `rt` is
    /// the right operand's type, which matters for shifts. Checked as spec 16 requires.
    llvm::Value* arith(TokenKind op, const types::Type* t, llvm::Value* a, llvm::Value* b, const types::Type* rt,
                       Span span) {
        if (!t) unsupported(span, "this operation");
        if (t->is_float()) {
            switch (op) {
            case TokenKind::plus: return b_.CreateFAdd(a, b);
            case TokenKind::minus: return b_.CreateFSub(a, b);
            case TokenKind::star: return b_.CreateFMul(a, b);
            case TokenKind::slash: return b_.CreateFDiv(a, b);
            case TokenKind::percent: return b_.CreateFRem(a, b);
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
                                             std::format("integer overflow in '+' ({})", name));
        case TokenKind::minus: return checked(llvm::Intrinsic::ssub_with_overflow, llvm::Intrinsic::usub_with_overflow, s, a, b,
                                              span, std::format("integer overflow in '-' ({})", name));
        case TokenKind::star: return checked(llvm::Intrinsic::smul_with_overflow, llvm::Intrinsic::umul_with_overflow, s, a, b,
                                             span, std::format("integer overflow in '*' ({})", name));
        case TokenKind::slash: {
            check(b_.CreateICmpNE(b, llvm::ConstantInt::get(b->getType(), 0)), span, "division by zero");
            if (!s) return b_.CreateUDiv(a, b);
            llvm::Value* minimum = llvm::ConstantInt::get(ctx_, llvm::APInt::getSignedMinValue(static_cast<unsigned>(t->bits)));
            llvm::Value* minus_one = llvm::ConstantInt::getAllOnesValue(b->getType());
            check(b_.CreateNot(b_.CreateAnd(b_.CreateICmpEQ(a, minimum), b_.CreateICmpEQ(b, minus_one))), span,
                  std::format("integer overflow in '/' ({})", name));
            return b_.CreateSDiv(a, b);
        }
        case TokenKind::percent: {
            check(b_.CreateICmpNE(b, llvm::ConstantInt::get(b->getType(), 0)), span, "division by zero in '%'");
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
            check(b_.CreateICmpULT(count, llvm::ConstantInt::get(count->getType(), static_cast<std::uint64_t>(t->bits))),
                  span, std::format("shift count out of range for {}", name));
            llvm::Value* amount = b_.CreateZExtOrTrunc(count, a->getType());
            if (op == TokenKind::shl) return b_.CreateShl(a, amount);
            return s ? b_.CreateAShr(a, amount) : b_.CreateLShr(a, amount);
        }
        default: break;
        }
        unsupported(span, std::format("'{}' on {}", token_kind_spelling(op), types::to_string(t)));
    }

    /// `a op b` with an overflow intrinsic: panics with `message` on overflow.
    llvm::Value* checked(llvm::Intrinsic::ID signed_id, llvm::Intrinsic::ID unsigned_id, bool s, llvm::Value* a,
                         llvm::Value* b, Span span, const std::string& message) {
        llvm::Function* f = intrinsic_declaration(mod_.get(), s ? signed_id : unsigned_id, {a->getType()});
        llvm::Value*    r = b_.CreateCall(f, {a, b});
        check(b_.CreateNot(b_.CreateExtractValue(r, 1)), span, message);
        return b_.CreateExtractValue(r, 0);
    }

    llvm::Value* if_value(IfExpr& i, const types::Type* t) {
        if (i.is_compile_time) {   // only the branch the checker chose exists (spec 15)
            if (i.taken == 0) return gen_block(*i.then_block);
            if (i.taken == 1) return gen_expr(*i.else_branch);
            if (i.taken == 2) return nullptr;
            unsupported(i.span, "'if compile_time' with a condition that is not known");
        }
        llvm::Value*      cond     = truth(*i.condition);
        bool              has_value = !is_void(t);
        llvm::AllocaInst* slot      = has_value ? entry_alloca(lower_type(types::strip_ref(t)), "if.value") : nullptr;
        llvm::BasicBlock* then_bb   = llvm::BasicBlock::Create(ctx_, "then", cur_fn_);
        llvm::BasicBlock* else_bb   = i.else_branch ? llvm::BasicBlock::Create(ctx_, "else", cur_fn_) : nullptr;
        llvm::BasicBlock* end_bb    = llvm::BasicBlock::Create(ctx_, "endif", cur_fn_);
        b_.CreateCondBr(cond, then_bb, else_bb ? else_bb : end_bb);
        b_.SetInsertPoint(then_bb);
        llvm::Value* tv = gen_block(*i.then_block);
        if (!terminated()) {
            if (slot && tv) b_.CreateStore(tv, slot);
            b_.CreateBr(end_bb);
        }
        if (else_bb) {
            b_.SetInsertPoint(else_bb);
            llvm::Value* ev = gen_expr(*i.else_branch);
            if (!terminated()) {
                if (slot && ev) b_.CreateStore(ev, slot);
                b_.CreateBr(end_bb);
            }
        }
        b_.SetInsertPoint(end_bb);
        return slot ? b_.CreateLoad(lower_type(types::strip_ref(t)), slot) : nullptr;
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
              : result_ && result_->is(Kind::reference)                              ? object_address(*r.value)
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
        record_drops(0);
        drop_kind_ = "drop";
        destroy_from(0);
        if (v && !rt->isVoidTy()) b_.CreateRet(v);
        else b_.CreateRetVoid();
        open_dead_block();
        return nullptr;
    }

    /// A method or static function of a std type, by name (`string::as_str`, `string::from`).
    const Symbol* std_member(const types::Type* t, const std::string& name) {
        if (!t || !t->is(Kind::user)) return nullptr;
        auto it = t->symbol->members.find(name);
        return it == t->symbol->members.end() ? nullptr : it->second.front();
    }

    /// The implicit conversions of spec 19. A temporary `std::string` that is read as a
    /// `std::str` lives until the end of the enclosing block.
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
        // literal_to_string: `std::string::from(literal)`
        const types::Type* target = nullptr;
        if (const Symbol* sym = program_.std_module ? program_.std_module->item("std::string") : nullptr) {
            target = tc_.user(sym);
        }
        const Symbol* from = std_member(target, "from");
        if (!from) unsupported(e.span, "this std::string");
        return b_.CreateCall(function_of(from), {gen_expr(e)});
    }

    /// A pointer (or a smart pointer holding one) seen as a pointer to a base class or an
    /// interface: the same address, plus the interface's table when the target is an interface.
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

    /// A C library function the `std::raw` intrinsics call.
    llvm::FunctionCallee libc(const char* name, llvm::Type* result, std::vector<llvm::Type*> params) {
        return mod_->getOrInsertFunction(name, llvm::FunctionType::get(result, params, false));
    }

    /// The bytes `n` elements of `t` take; panics on overflow.
    llvm::Value* byte_count(const types::Type* t, llvm::Value* n, Span span) {
        auto*        i64  = llvm::Type::getInt64Ty(ctx_);
        std::uint64_t size = mod_->getDataLayout().getTypeAllocSize(lower_type(t));
        llvm::Value* r    = b_.CreateBinaryIntrinsic(llvm::Intrinsic::umul_with_overflow, b_.CreateZExtOrTrunc(n, i64),
                                                     llvm::ConstantInt::get(i64, size));
        check(b_.CreateNot(b_.CreateExtractValue(r, 1)), span, "allocation size overflows");
        return b_.CreateExtractValue(r, 0);
    }

    /// The `std::raw` memory intrinsics (spec 13).
    /// A `std::memory_order` argument as an LLVM ordering. An order that isn't a constant (the
    /// library's checks make it one) becomes sequentially consistent, which is always correct.
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

    /// std::raw's atomics, futex and spin hint (spec 27). `bool` is a byte in memory.
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
        if (name == "futex_wait" || name == "futex_wake") {
            llvm::Value* p = gen_expr(*c.args[0]);
            llvm::Value* n = gen_expr(*c.args[1]);
            b_.CreateCall(runtime(name == "futex_wait" ? "rift_futex_wait" : "rift_futex_wake", vd, {ptr_type(), i32}), {p, n});
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

    llvm::Value* raw_call(CallExpr& c, const std::string& name) {
        if (name.starts_with("atomic_") || name.starts_with("futex_") || name == "spin_hint") return thread_raw(c, name);
        auto* ptr = ptr_type();
        auto* i64 = llvm::Type::getInt64Ty(ctx_);
        const types::Type* pt = types::strip_ref(c.type);
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
        if (name == "str_from") {
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

    llvm::Value* call_value(CallExpr& c) {
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
                unsupported(callee.span, std::format("'{}'", to_string(pe.path)), no_std);
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
                if (inspect_) {
                    insight(callee_name(c), "dispatch", "interface", "range",
                            std::format("interface call: {}::{} through a table", types::to_string(dispatched), name),
                            "The object is reached through an interface reference (an object pointer and a table, spec 07): the call loads "
                            "the method's address from the table, so it can't be inlined.");
                }
                llvm::Value* table  = b_.CreateExtractValue(fat, 1);
                llvm::Value* target = b_.CreateLoad(ptr_type(), b_.CreateConstGEP1_32(ptr_type(), table, itable_header + static_cast<unsigned>(slot)));
                std::vector<Expr*> args;
                for (ExprPtr& a : c.args) args.push_back(a.get());
                return call_symbol(s, b_.CreateExtractValue(fat, 0), args, c.span, false, target);
            }
            const types::Type* target = me.deref_symbol ? types::strip_ref(tc_.lower(*body_decl(me.deref_symbol)->result,
                                                                                      sema::substitution_of(me.deref_symbol)))
                                                        : obj;
            if (s && s->kind == SymbolKind::method && (!me.arrow || me.deref_symbol) && me.name.segments.size() == 1 && target &&
                target->is(Kind::user) && has_vtable(target->symbol) && has_self(s)) {
                int slot = vtable_slot(target->symbol, s);   // a virtual method: the object's own implementation
                if (slot >= 0) {
                    if (inspect_) {
                        insight(callee_name(c), "dispatch", "virtual", "range",
                                std::format("virtual call: {}::{} through the vtable", types::to_string(target), name),
                                "An `open` class hierarchy (spec 07): the call loads the most derived implementation from the object's "
                                "vtable. Call `object.base::method()` to call one implementation directly.");
                    }
                    llvm::Value* object = member_object(me);
                    std::vector<Expr*> args;
                    for (ExprPtr& a : c.args) args.push_back(a.get());
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
            if (name == "len" && c.args.empty() && is_str(obj)) return b_.CreateExtractValue(gen_expr(*me.object), 1);
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

    /// Calls the function `s` with `in` as its arguments (after the receiver, for a method). A
    /// reference result is read through unless `want_address`, which returns the address itself.
    llvm::Value* call_symbol(const Symbol* s, llvm::Value* receiver, const std::vector<Expr*>& in, Span span,
                             bool want_address = false, llvm::Value* indirect = nullptr) {
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
                args.push_back(object_address(*in[i]));
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
        if (tracked) b_.CreateStore(loc_global(span), call_site_global());
        llvm::Value* call = indirect ? b_.CreateCall(function_type(s), indirect, args)   // a virtual or interface call
                                     : b_.CreateCall(function_of(s), args);
        if (tracked) b_.CreateStore(llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0)), call_site_global());
        if (inspect_ && !indirect) record_call(s, fns_.contains(s) ? fns_[s] : nullptr, call_ast, call_ast ? callee_name(*call_ast) : span);
        const types::Type* result = f->result ? tc_.lower(*f->result, sema::substitution_of(s)) : nullptr;
        if (imported && result && result->is(Kind::bool_)) return b_.CreateICmpNE(call, llvm::ConstantInt::get(call->getType(), 0));
        if (want_address) return call;
        if (result && result->is(Kind::reference) && is_interface(result->inner)) return call;   // the fat pointer
        if (result && result->is(Kind::reference)) return b_.CreateLoad(lower_type(types::strip_ref(result)), call);
        return call;
    }

    /// The runtime functions the generated code calls. Their bodies are in llvm_runtime.cpp and
    /// are appended to the module text by generate_llvm_ir.
    void declare_runtime() {
        auto* i32 = llvm::Type::getInt32Ty(ctx_);
        auto* i64 = llvm::Type::getInt64Ty(ctx_);
        auto* i128 = llvm::Type::getInt128Ty(ctx_);
        auto* ptr = llvm::PointerType::get(ctx_, 0);
        auto* vd  = llvm::Type::getVoidTy(ctx_);
        loc_type_ = llvm::StructType::get(ctx_, {ptr, i32, i32});
        rift_panic_str_ = llvm::Function::Create(llvm::FunctionType::get(vd, {ptr, ptr, i64}, false),
                                                 llvm::Function::ExternalLinkage, "rift_panic_str", mod_.get());
        rift_panic_str_->setDoesNotReturn();
        rift_exit_status_ = llvm::Function::Create(llvm::FunctionType::get(i32, {i128, ptr}, false),
                                                   llvm::Function::ExternalLinkage, "rift_exit_status", mod_.get());
    }

    /// `message` as a constant byte array (not NUL-terminated), for `rift_panic_str`.
    llvm::Constant* message_bytes(const std::string& message) {
        llvm::Constant* init = llvm::ConstantDataArray::getString(ctx_, message, false);
        return new llvm::GlobalVariable(*mod_, init->getType(), true, llvm::GlobalValue::PrivateLinkage, init, ".msg");
    }

    /// The C entry point of a program with `@std::test` functions (spec 25, 29). `riftc test` runs
    /// it with an environment variable:
    ///   RIFT_TEST_LIST=1     prints "rift-tests:" and the tests' names, one per line;
    ///   RIFT_TEST_RUN=name   runs that test (exit 0 when it returns; a panic aborts, exit 2: no such test).
    /// Without either it runs `main` if there is one, else every test in order.
    void emit_test_entry(const std::vector<const Symbol*>& tests, const std::vector<llvm::Function*>& fns, const Symbol* main) {
        auto* i32 = llvm::Type::getInt32Ty(ctx_);
        auto* ptr = llvm::PointerType::get(ctx_, 0);
        if (main) {   // the program's own entry, renamed
            emit_entry(main);
            mod_->getFunction("main")->setName("rift_program_main");
        }
        llvm::Function*   init  = emit_init_globals();
        llvm::Function*   entry = llvm::Function::Create(llvm::FunctionType::get(i32, {i32, ptr}, false), llvm::Function::ExternalLinkage,
                                                         "main", mod_.get());
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", entry));
        llvm::FunctionCallee strcmp = mod_->getOrInsertFunction("strcmp", llvm::FunctionType::get(i32, {ptr, ptr}, false));
        llvm::FunctionCallee puts   = mod_->getOrInsertFunction("puts", llvm::FunctionType::get(i32, {ptr}, false));
        llvm::FunctionCallee getenv = mod_->getOrInsertFunction("getenv", llvm::FunctionType::get(ptr, {ptr}, false));
        llvm::Value* null = llvm::ConstantPointerNull::get(ptr);
        // RIFT_TEST_LIST
        llvm::BasicBlock* list     = llvm::BasicBlock::Create(ctx_, "list", entry);
        llvm::BasicBlock* not_list = llvm::BasicBlock::Create(ctx_, "not_list", entry);
        b.CreateCondBr(b.CreateICmpNE(b.CreateCall(getenv, {string_global("RIFT_TEST_LIST")}), null), list, not_list);
        b.SetInsertPoint(list);
        b.CreateCall(puts, {string_global("rift-tests:")});
        for (const Symbol* t : tests) b.CreateCall(puts, {string_global(t->name)});
        b.CreateRet(llvm::ConstantInt::get(i32, 0));
        // RIFT_TEST_RUN=name
        b.SetInsertPoint(not_list);
        llvm::Value*      name    = b.CreateCall(getenv, {string_global("RIFT_TEST_RUN")});
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
            b.CreateRet(b.CreateCall(mod_->getFunction("rift_program_main"), {entry->getArg(0), entry->getArg(1)}));
            return;
        }
        b.CreateCall(init);
        for (std::size_t i = 0; i < tests.size(); ++i) {
            b.CreateCall(fns[i]);
            b.CreateCall(puts, {string_global("test " + tests[i]->name + " ... ok")});
        }
        b.CreateRet(llvm::ConstantInt::get(i32, 0));
    }

    /// The C entry point: runs Rift's `main` and returns its status.
    void emit_entry(const Symbol* main) {
        const FunctionDecl* f      = body_decl(main);
        auto*               i32    = llvm::Type::getInt32Ty(ctx_);
        auto*               ptr    = llvm::PointerType::get(ctx_, 0);
        llvm::Function*     entry  = llvm::Function::Create(llvm::FunctionType::get(i32, {i32, ptr}, false),
                                                            llvm::Function::ExternalLinkage, "main", mod_.get());
        llvm::BasicBlock*   block  = llvm::BasicBlock::Create(ctx_, "entry", entry);
        llvm::IRBuilder<>   b(block);
        // The arguments as std::str (checked to be UTF-8); raw for std::env::raw_args (spec 04).
        llvm::FunctionCallee main_args = mod_->getOrInsertFunction("rift_main_args", llvm::FunctionType::get(ptr, {i32, ptr}, false));
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
        llvm::CallInst*     call   = b.CreateCall(rift_main_, main_in);
        b.CreateCall(mod_->getOrInsertFunction("free", llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {ptr}, false)), {array});
        const types::Type*  result = f->result ? tc_.lower(*f->result, sema::substitution_of(main)) : nullptr;
        if (result && result->is_integer()) {
            llvm::Value* wide = result->is_signed ? b.CreateSExt(call, llvm::Type::getInt128Ty(ctx_))
                                                  : b.CreateZExt(call, llvm::Type::getInt128Ty(ctx_));
            b.CreateRet(b.CreateCall(rift_exit_status_, {wide, loc_global(main->span)}));
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

    /// Methods take `self` (a pointer to the object) as their first parameter.
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

    /// Every field of an object, the base's first (spec 07).
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

    /// Whether two methods have the same parameter types (an override of a slot).
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

    /// The virtual slots of a class: the methods that introduced them, root first.
    std::vector<const Symbol*> vtable_slots(const Symbol* s) {
        std::vector<const Symbol*> out;
        if (const Symbol* base = s->base_class()) out = vtable_slots(base);
        for (const Symbol* m : methods_of(s)) {
            const FunctionDecl* f = m->function();
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

    /// The slot of `method` in the hierarchy of `s`, or -1.
    int vtable_slot(const Symbol* s, const Symbol* method) {
        std::vector<const Symbol*> slots = vtable_slots(s);
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (same_signature(slots[i], method)) return static_cast<int>(i);
        }
        return -1;
    }

    /// The implementation of `method` (by signature) for objects of class `s`: the most derived one.
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

    /// The vtable of class `s`.
    llvm::Constant* vtable_of(const Symbol* s) {
        if (auto it = vtables_.find(s); it != vtables_.end()) return it->second;
        std::vector<const Symbol*> slots = vtable_slots(s);
        auto* array_type = llvm::ArrayType::get(ptr_type(), vtable_header + slots.size());
        std::vector<std::string> parts = symbol_scope(s);
        parts.push_back(s->name);
        if (s->template_of) parts.push_back("0i" + std::to_string(s->instance_index));
        auto* g = new llvm::GlobalVariable(*mod_, array_type, true, llvm::GlobalValue::InternalLinkage, nullptr,
                                           "rv_" + mangle_parts(parts));
        vtables_[s] = g;   // before the entries: a class's methods may need its own vtable
        std::vector<llvm::Constant*> entries;
        const Symbol* base = s->base_class();
        entries.push_back(base ? vtable_of(base) : llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0)));
        entries.push_back(drop_function(tc_.user(s)));
        for (const Symbol* slot : slots) entries.push_back(function_constant(implementation(s, slot)));
        g->setInitializer(llvm::ConstantArray::get(array_type, entries));
        return g;
    }

    /// A virtual call: the function in `slot` of the object's vtable.
    llvm::Value* virtual_function(llvm::Value* object, int slot) {
        llvm::Value* vtable = b_.CreateLoad(ptr_type(), object);
        llvm::Value* entry  = b_.CreateConstGEP1_32(ptr_type(), vtable, vtable_header + static_cast<unsigned>(slot));
        return b_.CreateLoad(ptr_type(), entry);
    }

    /// The methods of an interface, those of the interfaces it extends first.
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

    /// An interface table: { drop, type id, methods..., tables of the extended interfaces... }.
    static constexpr unsigned itable_header = 2;

    std::map<const Symbol*, llvm::GlobalVariable*> type_ids_;

    /// A global whose address stands for type `s` (cast::dynamic from an interface).
    llvm::Constant* type_id(const Symbol* s) {
        if (auto it = type_ids_.find(s); it != type_ids_.end()) return it->second;
        std::vector<std::string> parts = symbol_scope(s);
        parts.push_back(s->name);
        if (s->template_of) parts.push_back("0i" + std::to_string(s->instance_index));
        auto* g = new llvm::GlobalVariable(*mod_, llvm::Type::getInt8Ty(ctx_), true, llvm::GlobalValue::InternalLinkage,
                                           llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), 0), "rt_" + mangle_parts(parts));
        type_ids_[s] = g;
        return g;
    }

    std::map<std::pair<const Symbol*, int>, llvm::Function*> thunks_;

    /// An interface table entry for a class with a vtable: calls the object's own override.
    llvm::Function* virtual_thunk(const Symbol* impl, int slot) {
        auto key = std::make_pair(impl, slot);
        if (auto it = thunks_.find(key); it != thunks_.end()) return it->second;
        llvm::FunctionType* type = function_type(impl);
        auto* fn = llvm::Function::Create(type, llvm::Function::InternalLinkage,
                                          std::string(function_of(impl)->getName()) + ".dispatch", mod_.get());
        thunks_[key] = fn;
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

    /// The table of interface `iface` for objects of type `s`.
    llvm::Constant* itable_of(const Symbol* s, const Symbol* iface) {
        auto key = std::make_pair(s, iface);
        if (auto it = itables_.find(key); it != itables_.end()) return it->second;
        std::vector<const Symbol*> ms = interface_methods(iface);
        std::vector<const Symbol*> extended = interface_bases(iface);
        auto* array_type = llvm::ArrayType::get(ptr_type(), itable_header + ms.size() + extended.size());
        std::vector<std::string> parts = symbol_scope(s);
        parts.push_back(s->name);
        if (s->template_of) parts.push_back("0i" + std::to_string(s->instance_index));
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

    /// The interfaces `i` extends directly.
    static std::vector<const Symbol*> interface_bases(const Symbol* i) {
        std::vector<const Symbol*> out;
        for (const Symbol* b : i->bases) {
            if (b->kind == SymbolKind::interface) out.push_back(b);
        }
        return out;
    }

    /// The path of extended interfaces from `from` to `to` (empty when `to` is not one of them).
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

    /// `{ object, table of from }` as `{ object, table of to }`: each extended interface's table
    /// is stored after the methods (spec 07).
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

    /// The fat pointer `{ object, table }` of an expression whose type is an interface (or a
    /// reference to one): what an interface place's "address" is.
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
                return b_.CreateLoad(fat, b_.CreateStructGEP(user_type(f->parent), member_object(m), field_index(f)));
            }
            break;
        }
        case ExprKind::block: return gen_block(e.as<BlockExpr>(), true);
        default: return gen_expr(e);   // a call returning `iface&` returns the fat pointer
        }
        unsupported(e.span, "this interface reference");
    }

    /// An interface reference `{ object, table }` for `e`, used as `iface&` (spec 07).
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

    /// Whether the enum `e` is laid out as `{ tag, payload storage }`: some alternative declares a
    /// payload (a `void` one too, so that `std::option<void>` has the layout of every option).
    static bool has_payloads(const Symbol* e) {
        for (const Symbol* a : e->member_order) {
            if (a->alternative && a->alternative->payload) return true;
        }
        return false;
    }

    /// Whether the alternative `a` carries a value: it has a payload type, and that type is not
    /// `void` (`std::ok()` of a `std::result<void, E>`, spec 08).
    bool carries(const Symbol* a) {
        if (!a || !a->alternative || !a->alternative->payload) return false;
        return !payload_of(a)->is(Kind::void_);
    }

    const types::Type* field_type(const Symbol* f) {
        return tc_.lower(*f->decl->as<VariableDecl>().type, sema::substitution_of(f));
    }

    unsigned field_index(const Symbol* f) {
        std::vector<const Symbol*> fields = fields_of(f->parent);
        auto                       it     = std::find(fields.begin(), fields.end(), f);
        if (it == fields.end()) unsupported(f->span, "this field");
        return static_cast<unsigned>(it - fields.begin()) + f->parent->layout_prefix();
    }

    /// The LLVM type of a struct, class or enum. Structs keep the C layout of their fields (in
    /// declaration order); an empty struct holds one byte, as in C. An enum without payloads is its
    /// tag; with payloads it is `{ tag, [n x iK] }`, the payload storage sized and aligned for the
    /// largest payload on the host.
    llvm::Type* user_type(const Symbol* s) {
        if (s->decl && s->decl->is_template && !s->template_of) unsupported(s->span, "templates used without arguments");
        if (auto it = user_types_.find(s); it != user_types_.end()) return it->second;
        if (s->kind == SymbolKind::enum_) return enum_type(s);
        if (!is_struct_symbol(s)) unsupported(s->span, "values of this type");
        auto* st       = llvm::StructType::create(ctx_, s->name);
        user_types_[s] = st;
        std::vector<llvm::Type*> elems;
        if (const Symbol* base = s->base_class()) elems.push_back(user_type(base));   // the base object first (spec 07)
        else if (s->has_vtable_pointer()) elems.push_back(ptr_type());               // the vtable
        for (const Symbol* f : fields_of(s)) {
            cur_ = f->span;
            elems.push_back(lower_type(field_type(f)));
        }
        if (elems.empty()) elems.push_back(llvm::Type::getInt8Ty(ctx_));
        st->setBody(elems);
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
        auto* st       = llvm::StructType::create(ctx_, e->name);
        user_types_[e] = st;
        llvm::Type* elem = llvm::IntegerType::get(ctx_, align * 8);
        st->setBody({tag, llvm::ArrayType::get(elem, (size + align - 1) / align)});
        return st;
    }

    /// Tag value of each alternative (explicit values, otherwise previous + 1).
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

    /// The value of an alternative: its tag, or the enum with `payload` stored in it.
    llvm::Value* alternative_value(const Symbol* alt, llvm::Value* payload) {
        const Symbol* e   = alt->parent;
        llvm::Value*  tag = tag_constant(alt);
        if (!has_payloads(e)) return tag;
        llvm::Type*       et   = user_type(e);
        llvm::AllocaInst* slot = entry_alloca(et, "alternative");
        b_.CreateStore(tag, b_.CreateStructGEP(et, slot, 0));
        if (payload) b_.CreateStore(payload, b_.CreateStructGEP(et, slot, 1));
        return b_.CreateLoad(et, slot);
    }

    /// A struct value from the fields given in a literal; the others take their declared
    /// initializer, or the default of their type.
    llvm::Value* build_struct(const Symbol* s, const std::map<std::string, llvm::Value*>& given) {
        llvm::Type*       st   = user_type(s);
        llvm::AllocaInst* slot = entry_alloca(st, "value");
        for (const Symbol* f : all_fields(s)) {   // the base's fields too: `circle{ .name = ..., .radius = ... }`
            cur_ = f->span;
            llvm::Value* v = nullptr;
            if (auto it = given.find(f->name); it != given.end()) {
                v = it->second;
            } else {
                const auto& d = f->decl->as<VariableDecl>();
                v             = d.init ? gen_expr(*d.init) : default_value(field_type(f), f->span);
            }
            b_.CreateStore(v, b_.CreateStructGEP(user_type(f->parent), slot, field_index(f)));   // a base is at offset 0
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
        if (t && (t->is_integer() || t->is_float() || t->is(Kind::bool_) || t->is(Kind::char_) || t->is(Kind::pointer))) {
            return llvm::Constant::getNullValue(lower_type(t));
        }
        unsupported(where, std::format("'default' for '{}'", t ? types::to_string(t) : "unknown"));
    }

    bool is_place(const Expr& e) const {
        switch (e.kind) {
        case ExprKind::path: {
            const Symbol* s = e.as<PathExpr>().symbol;
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

    /// The address of the object a place denotes. A reference (or `this`) denotes the object it
    /// refers to, so its address is the pointer it holds.
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
            const Symbol* f = m.symbol;
            if (!f || f->kind != SymbolKind::field || f->is_static) unsupported(e.span, "this member access");
            llvm::Value* at = b_.CreateStructGEP(user_type(f->parent), member_object(m), field_index(f));
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

    /// The object a member access reads from: `p->x` on a smart pointer goes through its `operator*`.
    llvm::Value* member_object(MemberExpr& m) {
        if (m.deref_symbol) return call_symbol(m.deref_symbol, object_address(*m.object), {}, m.span, true);
        return object_address(*m.object);
    }

    /// The address of the object `e` denotes: a place, a pointer or reference value, or a temporary.
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

    /// The drop flags of owned temporaries, by their slot: whoever takes a temporary over (a
    /// match, a loop) clears its flag.
    std::map<const llvm::Value*, llvm::AllocaInst*> temporaries_;

    /// Hands the temporary at `slot` (if it is one) to a new owner.
    void release_temporary(const llvm::Value* slot) {
        if (auto it = temporaries_.find(slot); it != temporaries_.end()) b_.CreateStore(llvm::ConstantInt::getFalse(ctx_), it->second);
    }

    /// The value of a place expression (a reference is read through).
    llvm::Value* value_of_place(Expr& e) {
        return b_.CreateLoad(lower_type(types::strip_ref(e.type)), place(e));
    }

    struct Loop {
        llvm::BasicBlock* exit;
        llvm::BasicBlock* cont;
    };

    // --- strings, output and formatting (spec 19) ------------------------------------------

    llvm::Type* ptr_type() { return llvm::PointerType::get(ctx_, 0); }

    /// A `str` value: the bytes and their length, `{ ptr, i64 }`.
    llvm::StructType* str_type() { return llvm::StructType::get(ctx_, {ptr_type(), llvm::Type::getInt64Ty(ctx_)}); }

    /// The runtime's output buffer, `{ ptr, i64, i64 }`.
    llvm::StructType* buffer_type() {
        return llvm::StructType::get(ctx_, {ptr_type(), llvm::Type::getInt64Ty(ctx_), llvm::Type::getInt64Ty(ctx_)});
    }

    static bool is_str(const types::Type* t) {
        return t && (t->is(Kind::static_str) || (t->is(Kind::external) && t->name == "std::str"));
    }

    /// A `str` constant with the bytes of `text`.
    llvm::Constant* str_constant(const std::string& text) {
        llvm::Constant* bytes = message_bytes(text);
        return llvm::ConstantStruct::get(str_type(), {bytes, llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), text.size())});
    }

    /// A function of the runtime (llvm_runtime.cpp), declared here and defined in the appended IR.
    llvm::Function* runtime(const char* name, llvm::Type* result, std::vector<llvm::Type*> params) {
        if (llvm::Function* f = mod_->getFunction(name)) return f;
        return llvm::Function::Create(llvm::FunctionType::get(result, params, false), llvm::Function::ExternalLinkage, name, mod_.get());
    }

    /// The `rift_spec` of a format hole: `{ align, plus, width, precision, radix }` (spec 19).
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

    /// The rift_stream for stdout (`err` false) or stderr.
    llvm::Value* stream(bool err) {
        return b_.CreateCall(runtime("rift_stream", ptr_type(), {llvm::Type::getInt1Ty(ctx_)}),
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

    /// std::print / println / eprint / eprintln / panic. Output is written straight to the stream;
    /// panics print the message with the location of the call (spec 11).
    void std_output(CallExpr& c) {
        std::string name    = std_name(c);
        bool        panic   = name == "panic";
        bool        newline = name.ends_with("ln");
        bool        err     = name.starts_with("e") || panic;
        if (c.args.size() != 1) unsupported(c.span, std::format("'std::{}' with {} arguments", name, c.args.size()));
        Expr& arg = *c.args[0];
        auto* i64 = llvm::Type::getInt64Ty(ctx_);
        auto* i1  = llvm::Type::getInt1Ty(ctx_);
        auto* loc = llvm::PointerType::get(ctx_, 0);
        const types::Type* at = types::strip_ref(arg.type);
        if ((arg.kind == ExprKind::string_literal && arg.as<LiteralExpr>().token_kind == TokenKind::format_string_literal) ||
            (at && at->is(Kind::external) && at->name == "std::format_args")) {
            if (!panic) record_format(callee_name(c));
            llvm::AllocaInst* buf = text_into_buffer(arg);
            if (panic) {
                b_.CreateCall(runtime("rift_panic_buf", llvm::Type::getVoidTy(ctx_), {loc, loc}), {loc_global(c.span), buf});
                b_.CreateUnreachable();
                open_dead_block();
            } else {
                b_.CreateCall(runtime("rift_flush", llvm::Type::getVoidTy(ctx_), {loc, loc, i1}),
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
            b_.CreateCall(runtime("rift_panic_str", llvm::Type::getVoidTy(ctx_), {loc, loc, i64}), {loc_global(c.span), data, len});
            b_.CreateUnreachable();
            open_dead_block();
        } else {
            b_.CreateCall(runtime("rift_write_str", llvm::Type::getVoidTy(ctx_), {loc, loc, i64, i1}),
                          {stream(err), data, len, llvm::ConstantInt::getBool(ctx_, newline)});
        }
    }

    /// `std::format_args` (spec 19): a view of an f-string that is not formatted yet: a function
    /// that writes the text into a buffer, and the addresses of the holes' values it reads.
    llvm::StructType* format_args_type() {
        if (!format_args_type_) format_args_type_ = llvm::StructType::create(ctx_, {ptr_type(), ptr_type()}, "rift.format_args");
        return format_args_type_;
    }
    llvm::StructType* format_args_type_ = nullptr;

    /// An f-string used as a value (spec 19, 31): a function that formats it, and the places its
    /// holes read. The holes are evaluated only when (and if) the receiver formats it, on the
    /// calling thread while the call is running, so every local they read is still alive.
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
                                          "rift_format", mod_.get());
        fn->addFnAttr(llvm::Attribute::NoUnwind);
        llvm::IRBuilderBase::InsertPoint saved_ip     = b_.saveIP();
        llvm::DebugLoc                   saved_loc    = b_.getCurrentDebugLocation();
        llvm::Function*                  saved_fn     = cur_fn_;
        llvm::Value*                     saved_self   = self_;
        auto                             saved_locals = locals_;
        auto                             saved_scopes = scopes_;
        auto                             saved_loops  = loops_;
        auto                             saved_owned  = owned_;
        const types::Type*               saved_result = result_;
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
                b_.CreateCall(runtime("rift_buf_put", vd, {ptr_type(), ptr_type(), i64}),
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

    /// The text of an f-string or a `std::format_args` value, in a new runtime buffer.
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

    /// The runtime buffer with the text of the f-string `lit`. Its bytes are on the heap.
    llvm::AllocaInst* format_into_buffer(LiteralExpr& lit) {
        auto*             i64 = llvm::Type::getInt64Ty(ctx_);
        auto*             p   = ptr_type();
        llvm::AllocaInst* buf = entry_alloca(buffer_type(), "buffer");
        b_.CreateStore(llvm::ConstantAggregateZero::get(buffer_type()), buf);
        for (FormatPart& part : lit.format_parts) {
            if (part.expr) {
                format_hole(buf, part);
            } else {
                b_.CreateCall(runtime("rift_buf_put", llvm::Type::getVoidTy(ctx_), {p, p, i64}),
                              {buf, message_bytes(part.text), llvm::ConstantInt::get(i64, part.text.size())});
            }
        }
        return buf;
    }

    /// `std::trailing_zeros` and friends: LLVM's bit intrinsics (cttz/ctlz defined for 0).
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

    /// `std::checked_add` and `std::saturating_add` and their sub/mul forms (spec 16).
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

    /// `std::format(f"...")`: the text as a new `std::string` (spec 19).
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

    /// Whether `t` is the library's `std::string` class.
    bool is_string(const types::Type* t) const {
        if (!t || !t->is(Kind::user) || !program_.std_module) return false;
        return program_.std_module->item("std::string") == t->symbol;
    }

    /// The name of the alternative of the simple enum `e` whose tag is `tag`: a simple enum prints
    /// as its alternative's name (spec 19).
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

    /// The text of a `std::str` or `std::string` expression, as a `std::str`.
    llvm::Value* text_value(Expr& e) {
        const types::Type* t = types::strip_ref(e.type);
        if (!is_string(t) || e.conversion == Expr::Conversion::string_to_str) return gen_expr(e);
        e.conversion = Expr::Conversion::string_to_str;   // the same borrow as an implicit conversion
        llvm::Value* v = gen_expr(e);
        e.conversion = Expr::Conversion::none;
        return v;
    }

    /// One `{expr:spec}` hole of an f-string, formatted into `buf`.
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
                b_.CreateCall(runtime("rift_fmt_u", vd, {p, i128, i1, p}),
                              {buf, b_.CreateZExt(v, i128), llvm::ConstantInt::getFalse(ctx_), spec});
            } else {
                b_.CreateCall(runtime("rift_fmt_i", vd, {p, i128, p}), {buf, b_.CreateSExt(v, i128), spec});
            }
        } else if (t && t->is_float()) {
            bool single = t->bits == 32;
            llvm::Value* d = single ? b_.CreateFPExt(v, llvm::Type::getDoubleTy(ctx_)) : v;
            b_.CreateCall(runtime("rift_fmt_f", vd, {p, llvm::Type::getDoubleTy(ctx_), i1, p}),
                          {buf, d, llvm::ConstantInt::getBool(ctx_, single), spec});
        } else if (t && t->is(Kind::bool_)) {
            b_.CreateCall(runtime("rift_fmt_bool", vd, {p, i1, p}), {buf, v, spec});
        } else if (t && t->is(Kind::char_)) {
            b_.CreateCall(runtime("rift_fmt_char", vd, {p, llvm::Type::getInt32Ty(ctx_), p}), {buf, v, spec});
        } else if (is_str(t) || is_string(t)) {
            b_.CreateCall(runtime("rift_fmt_str", vd, {p, p, llvm::Type::getInt64Ty(ctx_), p}),
                          {buf, b_.CreateExtractValue(v, 0), b_.CreateExtractValue(v, 1), spec});
        } else {
            unsupported(e.span, std::format("formatting values of type '{}'", t ? types::to_string(t) : "unknown"));
        }
    }

    /// The `void format(mut std::string& out)` method of a type (std::formattable, spec 19).
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

    /// Formats the value at `addr` of type `t` into `buf` (spec 19, 29): through the type's
    /// `format` method, or for an enum with `@std::derive::formattable` as `name(payload)`.
    /// Everything else is read and formatted like an f-string hole.
    void format_at(llvm::Value* buf, llvm::Value* addr, const types::Type* t, llvm::Constant* spec, Span span) {
        auto* vd  = llvm::Type::getVoidTy(ctx_);
        auto* p   = ptr_type();
        auto* i64 = llvm::Type::getInt64Ty(ctx_);
        auto put_text = [&](llvm::Value* target, const std::string& text) {
            b_.CreateCall(runtime("rift_buf_put", vd, {p, p, i64}),
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
            b_.CreateCall(function_of(method), {addr, tmp});
            llvm::Value* text = b_.CreateCall(function_of(as_str), {tmp});
            b_.CreateCall(runtime("rift_fmt_str", vd, {p, p, i64, p}),
                          {buf, b_.CreateExtractValue(text, 0), b_.CreateExtractValue(text, 1), spec});
            drop_value(st, tmp);
            return;
        }
        if (t->is_user(SymbolKind::enum_) && has_payloads(t->symbol)) {
            llvm::AllocaInst* inner = entry_alloca(buffer_type(), "enum_text");
            b_.CreateStore(llvm::ConstantAggregateZero::get(buffer_type()), inner);
            llvm::Type*  et  = lower_type(t);
            llvm::Value* tag = b_.CreateLoad(et->getStructElementType(0), b_.CreateStructGEP(et, addr, 0));
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
                        format_at(inner, b_.CreateStructGEP(et, addr, 1), pt, plain_spec(), span);
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
            b_.CreateCall(runtime("rift_fmt_str", vd, {p, p, i64, p}), {buf, data, len, spec});
            b_.CreateCall(libc("free", vd, {p}), {data});
            return;
        }
        // A plain value: read it and format it like a hole.
        auto* i1   = llvm::Type::getInt1Ty(ctx_);
        auto* i128 = llvm::Type::getInt128Ty(ctx_);
        if (is_string(t)) {
            const Symbol* as_str = std_member(t, "as_str");
            llvm::Value*  text   = b_.CreateCall(function_of(as_str), {addr});
            b_.CreateCall(runtime("rift_fmt_str", vd, {p, p, i64, p}),
                          {buf, b_.CreateExtractValue(text, 0), b_.CreateExtractValue(text, 1), spec});
            return;
        }
        llvm::Value* v = b_.CreateLoad(lower_type(t), addr);
        if (t->is_user(SymbolKind::enum_)) {
            v = enum_name(t->symbol, v);
            b_.CreateCall(runtime("rift_fmt_str", vd, {p, p, i64, p}), {buf, b_.CreateExtractValue(v, 0), b_.CreateExtractValue(v, 1), spec});
        } else if (t->is_integer()) {
            if (!t->is_signed) {
                b_.CreateCall(runtime("rift_fmt_u", vd, {p, i128, i1, p}), {buf, b_.CreateZExt(v, i128), llvm::ConstantInt::getFalse(ctx_), spec});
            } else {
                b_.CreateCall(runtime("rift_fmt_i", vd, {p, i128, p}), {buf, b_.CreateSExt(v, i128), spec});
            }
        } else if (t->is_float()) {
            bool single = t->bits == 32;
            llvm::Value* d = single ? b_.CreateFPExt(v, llvm::Type::getDoubleTy(ctx_)) : v;
            b_.CreateCall(runtime("rift_fmt_f", vd, {p, llvm::Type::getDoubleTy(ctx_), i1, p}),
                          {buf, d, llvm::ConstantInt::getBool(ctx_, single), spec});
        } else if (t->is(Kind::bool_)) {
            b_.CreateCall(runtime("rift_fmt_bool", vd, {p, i1, p}), {buf, b_.CreateTrunc(v, i1), spec});
        } else if (t->is(Kind::char_)) {
            b_.CreateCall(runtime("rift_fmt_char", vd, {p, llvm::Type::getInt32Ty(ctx_), p}), {buf, v, spec});
        } else if (is_str(t)) {
            b_.CreateCall(runtime("rift_fmt_str", vd, {p, p, i64, p}), {buf, b_.CreateExtractValue(v, 0), b_.CreateExtractValue(v, 1), spec});
        } else {
            unsupported(span, std::format("formatting values of type '{}'", types::to_string(t)));
        }
    }

    // --- match, for and casts -----------------------------------------------------------------

    llvm::Value* str_equal(llvm::Value* a, llvm::Value* b, bool eq) {
        auto* i64 = llvm::Type::getInt64Ty(ctx_);
        llvm::Value* same = b_.CreateCall(runtime("rift_str_eq", llvm::Type::getInt1Ty(ctx_), {ptr_type(), i64, ptr_type(), i64}),
                                          {b_.CreateExtractValue(a, 0), b_.CreateExtractValue(a, 1), b_.CreateExtractValue(b, 0),
                                           b_.CreateExtractValue(b, 1)});
        return eq ? same : b_.CreateNot(same);
    }

    /// Continues when `ok` holds; otherwise goes to `fail` (the next arm of a match).
    void require(llvm::Value* ok, llvm::BasicBlock* fail) {
        llvm::BasicBlock* cont = llvm::BasicBlock::Create(ctx_, "pattern", cur_fn_);
        b_.CreateCondBr(ok, cont, fail);
        b_.SetInsertPoint(cont);
    }

    const types::Type* payload_of(const Symbol* alt) {
        return tc_.lower(*alt->alternative->payload, sema::substitution_of(alt));
    }

    /// The field of a struct type named `name`, or null.
    static const Symbol* field_symbol(const Symbol* type, const std::string& name) {
        auto it = type->members.find(name);
        if (it == type->members.end() || it->second.front()->kind != SymbolKind::field) return nullptr;
        return it->second.front();
    }

    /// Checks the pattern `p` against the object at `addr` of type `t`. Falls through when it
    /// matches (binding its names) and branches to `fail` otherwise. Parts of a value are only read
    /// once the tests above them have passed, as in the C backend.
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
            llvm::Type*  et  = user_type(e);
            llvm::Value* tag = b_.CreateLoad(enum_tag_type(e), b_.CreateStructGEP(et, addr, 0));
            require(b_.CreateICmpEQ(tag, tag_constant(alt)), fail);
            if (ep.payload && carries(alt)) {
                match_pattern(*ep.payload, b_.CreateStructGEP(et, addr, 1), payload_of(alt), fail);
            }
            return;
        }
        case PatternKind::struct_: {
            auto& sp = p.as<StructPattern>();
            if (!t->is(Kind::user) || !is_struct_symbol(t->symbol)) unsupported(p.span, "this pattern");
            llvm::Type* st = user_type(t->symbol);
            for (FieldPattern& fp : sp.fields) {
                const Symbol* f = field_symbol(t->symbol, fp.name);
                if (!f || !fp.pattern) continue;
                match_pattern(*fp.pattern, b_.CreateStructGEP(st, addr, field_index(f)), field_type(f), fail);
            }
            return;
        }
        case PatternKind::error: unsupported(p.span, "this pattern");
        }
    }

    /// `match`: the arms are tried in order; the first whose pattern (and guard) matches gives the
    /// value. If none matches, the program panics (spec 07).
    llvm::Value* match_value(MatchExpr& m, const types::Type* t) {
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
        llvm::AllocaInst*  slot      = has_value ? entry_alloca(lower_type(types::strip_ref(t)), "match.value") : nullptr;
        llvm::BasicBlock*  end_bb    = llvm::BasicBlock::Create(ctx_, "match.end", cur_fn_);
        for (MatchArm& arm : m.arms) {
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
            llvm::Value* v = gen_expr(*arm.body);
            if (!terminated() && slot && v) {
                b_.CreateStore(v, slot);
                moved_out(*arm.body);   // `std::ok(list) => move list`: the value leaves the arm
            }
            close_scope();
            if (!terminated()) b_.CreateBr(end_bb);
            b_.SetInsertPoint(next);
        }
        check(llvm::ConstantInt::getFalse(ctx_), m.span, "no match arm matched");
        b_.CreateBr(end_bb);
        b_.SetInsertPoint(end_bb);
        llvm::Value* result = slot ? b_.CreateLoad(lower_type(types::strip_ref(t)), slot) : nullptr;
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

    /// Compares two `std::str` byte by byte: negative, zero or positive (an i64).
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

    /// `a op b` for the ordering and equality operators on integers, chars, bools, enums and floats.
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

    /// `bit(n)`: the integer with only bit `n` set; `n` must be below the bit width (spec 16).
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
        check(in, b.span, std::format("bit index out of range for {}", t->name));
        return b_.CreateShl(llvm::ConstantInt::get(lt, 1), b_.CreateTrunc(n, lt));
    }

    /// `for x in a..b` and `for x in a..=b` over integers. The inclusive form uses a flag so that
    /// `a..=MAX` never overflows (as in the C backend).
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
        b_.CreateBr(cond_bb);
        b_.SetInsertPoint(exit_bb);
        return nullptr;
    }

    /// `for (x : c)` over a container (`len()` and `operator[]`) or a stream (`next()`), spec 10.
    /// A temporary iterable lives until the loop ends.
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
            llvm::Type*  ot  = opt->getAllocatedType();
            llvm::Value* tag = b_.CreateLoad(enum_tag_type(some->parent), b_.CreateStructGEP(ot, opt, 0));
            b_.CreateCondBr(b_.CreateICmpEQ(tag, tag_constant(some)), body_bb, exit_bb);
            b_.SetInsertPoint(body_bb);
            element = b_.CreateStructGEP(ot, opt, 1);
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
        b_.CreateBr(cond_bb);
        b_.SetInsertPoint(exit_bb);
        close_scope();
        return nullptr;
    }

    /// Clamps the integer value `v` (of type `from`) into the range of `to`: `cast::saturate`.
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

    /// Float to integer with saturation: NaN gives 0, out-of-range values give the bounds.
    llvm::Value* saturate_float(llvm::Value* v, const types::Type* to) {
        auto*    dt   = llvm::Type::getDoubleTy(ctx_);
        llvm::Value* f = b_.CreateFPExt(v, dt);
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

    /// A conversion between numeric types, as in C: widening follows the source's signedness.
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

    llvm::Value* cast_value(CastExpr& c) {
        const types::Type* to   = types::strip_ref(c.type);
        const types::Type* from = types::strip_ref(c.operand->type);
        const std::string& variant = c.variant;
        if (variant == "checked" && types::is_std_instance(to, "option")) return checked_cast(c, from, to);
        if (variant == "dynamic") return dynamic_cast_value(c, types::strip_ref(to->symbol->template_args[0]), to);
        if (variant == "checked" || variant == "dynamic") {
            unsupported(c.span, std::format("'cast::{}' (it returns std::option)", variant));
        }
        llvm::Value* v = gen_expr(*c.operand);
        if (variant == "reinterpret") {
            if (to->is(Kind::pointer) || from->is(Kind::pointer)) return convert(v, from, to);
            return b_.CreateBitCast(v, lower_type(to));
        }
        if (variant == "saturate" && to->is_integer()) {
            if (from->is_float()) return saturate_float(v, to);
            if (from->is_integer()) return saturate_integer(v, from, to);
        }
        // cast<T> (lossless), cast::truncate (keeps the low bits) and cast::round are conversions.
        return convert(v, from, to);
    }

    // --- checks (spec 23) -----------------------------------------------------------------

    /// `run_time check (c) else msg;` (and the run-time part of `compile_time run_time`): panics
    /// with the message at the check when the condition is false. Compile-time checks and checks
    /// of other build modes generate nothing.
    void run_time_check(Check& c, Span span) {
        if (!c.run_active || !c.condition) return;
        llvm::Value*      ok   = truth(*c.condition);
        llvm::BasicBlock* fail = llvm::BasicBlock::Create(ctx_, "check.failed", cur_fn_);
        llvm::BasicBlock* pass = llvm::BasicBlock::Create(ctx_, "check.ok", cur_fn_);
        b_.CreateCondBr(ok, pass, fail);
        b_.SetInsertPoint(fail);
        if (c.is_entry) {   // an entry check names the caller's line, then the check (spec 23)
            llvm::Value* site  = b_.CreateLoad(ptr_type(), call_site_global());
            llvm::Value* check = loc_global(c.condition->span);
            llvm::Value* at    = b_.CreateSelect(b_.CreateIsNotNull(site), site, check);
            const SourceFile& file = sources_.file(c.condition->span.file);
            LineCol lc = file.line_col(c.condition->span.begin);
            bool in_std = file.path().starts_with("<std>");   // the library's checks: the caller's line is enough
            panic_with(*c.message, c.condition->span, at,
                       in_std ? std::string() : std::format("\nnote: the check is at {}:{}:{}", file.path(), lc.line, lc.column));
        } else {
            panic_with(*c.message, c.condition->span);
        }
        b_.SetInsertPoint(pass);
        (void)span;
    }

    // --- compile-time values (spec 15, 26) ---------------------------------------------------

    /// The constant for a value computed at compile time; null for values that exist only at
    /// compile time (types, reflection data, lists).
    llvm::Value* materialize(const ct::Value& v, Span where) {
        const types::Type* t = types::strip_ref(v.type);
        switch (v.kind) {
        case ct::Value::Kind::integer: {
            auto* lt = llvm::cast<llvm::IntegerType>(lower_type(t));
            return llvm::ConstantInt::get(ctx_, wide(v.integer, lt->getBitWidth()));
        }
        case ct::Value::Kind::floating: return llvm::ConstantFP::get(lower_type(t), v.floating);
        case ct::Value::Kind::boolean: return llvm::ConstantInt::getBool(ctx_, v.boolean);
        case ct::Value::Kind::character:
            return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), static_cast<std::uint64_t>(v.character));
        case ct::Value::Kind::string: return str_constant(v.string);
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

    /// A compile-time value as an LLVM constant of type `lt` (for the initial value of a
    /// thread_local); null when it has a part that isn't plain data.
    llvm::Constant* constant_of(const ct::Value& v, llvm::Type* lt) {
        const types::Type* t = types::strip_ref(v.type);
        switch (v.kind) {
        case ct::Value::Kind::integer: return llvm::ConstantInt::get(ctx_, wide(v.integer, lt->getIntegerBitWidth()));
        case ct::Value::Kind::floating: return llvm::ConstantFP::get(lt, v.floating);
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

    /// The address of the field a reflected `std::meta::field` names, in `object` (spec 26).
    llvm::Value* field_address(const ct::Value& field, Expr& object) {
        const Symbol* f = field.symbol;
        return b_.CreateStructGEP(user_type(f->parent), object_address(object), field_index(f));
    }

    /// `f.get(value)`, `a.is(value)`, `a.get(value)`, `a.make(...)`.
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
        llvm::Value* tag  = has_payloads(e) ? b_.CreateLoad(enum_tag_type(e), b_.CreateStructGEP(user_type(e), addr, 0))
                                            : b_.CreateLoad(enum_tag_type(e), addr);
        llvm::Value* is   = b_.CreateICmpEQ(tag, tag_constant(alt));
        if (name == "is") return is;
        const Symbol* option = types::strip_ref(static_cast<Expr&>(c).type)->symbol;   // a.get(value): std::option<P&>
        llvm::Value*  some   = alternative_value(alternative(option, "some"), b_.CreateStructGEP(user_type(e), addr, 1));
        llvm::Value*  none   = alternative_value(alternative(option, "none"), nullptr);
        return b_.CreateSelect(is, some, none);
    }

    // --- std::option and std::result (spec 11) -----------------------------------------------

    /// The alternative `name` of the enum (instance) `e`.
    static const Symbol* alternative(const Symbol* e, const std::string& name) {
        return e->members.at(name).front();
    }

    /// The alternative that carries the value: `some` or `ok`.
    static const Symbol* good_alternative(const Symbol* e) {
        return alternative(e, e->members.count("ok") ? "ok" : "some");
    }

    /// Stores the value of `operand` (an option or result, consumed) in a fresh slot and tests
    /// whether it holds `some` / `ok`.
    std::pair<llvm::AllocaInst*, llvm::Value*> unpack(Expr& operand, const Symbol* e) {
        llvm::Type*       et   = user_type(e);
        llvm::AllocaInst* slot = entry_alloca(et, "unpacked");
        b_.CreateStore(gen_expr(operand), slot);
        moved_out(operand);
        llvm::Value* tag = b_.CreateLoad(enum_tag_type(e), b_.CreateStructGEP(et, slot, 0));
        return {slot, b_.CreateICmpEQ(tag, tag_constant(good_alternative(e)))};
    }

    /// The payload of alternative `alt`, read from the enum at `slot`.
    llvm::Value* payload_at(llvm::AllocaInst* slot, const Symbol* alt) {
        if (!carries(alt)) return nullptr;   // a `void` payload
        return b_.CreateLoad(lower_type(payload_of(alt)), b_.CreateStructGEP(user_type(alt->parent), slot, 1));
    }

    /// Destroys the object at `addr` if its type needs it (a value that is thrown away).
    void drop_value(const types::Type* t, llvm::Value* addr) {
        if (needs_drop(t)) b_.CreateCall(drop_function(t), {addr});
    }

    /// Branches on `good`; `on_good` / `on_bad` produce a value of type `t` (or end the block).
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

    /// `r?`: the `ok` / `some` value, or return the error / `none` from the function.
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
            record_drops(0);
            drop_kind_ = "drop";
            destroy_from(0);
            b_.CreateRet(r);
            return nullptr;
        });
    }

    /// value_or, expect and or_error (map_error needs function values). The receiver is consumed;
    /// a part that is not used is destroyed (spec 11, 12).
    llvm::Value* std_method(CallExpr& c, MemberExpr& me, const types::Type* t, const std::string& name) {
        const Symbol*      e     = t->symbol;
        if (types::is_std_query_method(name)) {   // the tag, read in place
            llvm::Value* addr = object_address(*me.object);
            llvm::Value* tag  = b_.CreateLoad(enum_tag_type(e), b_.CreateStructGEP(user_type(e), addr, 0));
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
                llvm::Value* arg = ft->args[0]->is(Kind::reference) ? b_.CreateStructGEP(user_type(e), slot, 1)
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
            if (error) drop_value(payload_of(error), b_.CreateStructGEP(user_type(e), slot, 1));
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

    /// Panics with `message` (a string or an f-string) at `where`.
    void panic_with(Expr& message, Span where, llvm::Value* at = nullptr, const std::string& suffix = {}) {
        auto* p   = ptr_type();
        auto* i64 = llvm::Type::getInt64Ty(ctx_);
        auto* vd  = llvm::Type::getVoidTy(ctx_);
        llvm::Value* loc = at ? at : loc_global(where);
        if (!suffix.empty()) {   // the message, then a note: through a buffer
            llvm::AllocaInst* buf = entry_alloca(buffer_type(), "buffer");
            b_.CreateStore(llvm::ConstantAggregateZero::get(buffer_type()), buf);
            if (message.kind == ExprKind::string_literal && message.as<LiteralExpr>().token_kind == TokenKind::format_string_literal) {
                for (FormatPart& part : message.as<LiteralExpr>().format_parts) {
                    if (part.expr) format_hole(buf, part);
                    else b_.CreateCall(runtime("rift_buf_put", vd, {p, p, i64}),
                                       {buf, message_bytes(part.text), llvm::ConstantInt::get(i64, part.text.size())});
                }
            } else {
                llvm::Value* v = gen_expr(message);
                b_.CreateCall(runtime("rift_buf_put", vd, {p, p, i64}), {buf, b_.CreateExtractValue(v, 0), b_.CreateExtractValue(v, 1)});
            }
            b_.CreateCall(runtime("rift_buf_put", vd, {p, p, i64}),
                          {buf, message_bytes(suffix), llvm::ConstantInt::get(i64, suffix.size())});
            b_.CreateCall(runtime("rift_panic_buf", vd, {p, p}), {loc, buf});
            b_.CreateUnreachable();
            return;
        }
        if (message.kind == ExprKind::string_literal && message.as<LiteralExpr>().token_kind == TokenKind::format_string_literal) {
            llvm::AllocaInst* buf = entry_alloca(buffer_type(), "buffer");
            b_.CreateStore(llvm::ConstantAggregateZero::get(buffer_type()), buf);
            for (FormatPart& part : message.as<LiteralExpr>().format_parts) {
                if (part.expr) {
                    format_hole(buf, part);
                } else {
                    b_.CreateCall(runtime("rift_buf_put", vd, {p, p, i64}),
                                  {buf, message_bytes(part.text), llvm::ConstantInt::get(i64, part.text.size())});
                }
            }
            b_.CreateCall(runtime("rift_panic_buf", vd, {p, p}), {loc, buf});
        } else {
            llvm::Value* v = gen_expr(message);
            b_.CreateCall(runtime("rift_panic_str", vd, {p, p, i64}),
                          {loc, b_.CreateExtractValue(v, 0), b_.CreateExtractValue(v, 1)});
        }
        b_.CreateUnreachable();
    }

    /// `cast::checked<T>(x)`: `std::some` of the converted value when it is exactly the same
    /// number, otherwise `std::none` (spec 17).
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
            llvm::Value* wide = f->getType() == dt ? f : b_.CreateFPExt(f, dt);
            return b_.CreateAnd(b_.CreateFCmpOGE(wide, llvm::ConstantFP::get(dt, low)),
                                b_.CreateFCmpOLT(wide, llvm::ConstantFP::get(dt, high)));
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
            llvm::Value* whole = b_.CreateFCmpOEQ(v, b_.CreateUnaryIntrinsic(llvm::Intrinsic::trunc, v));
            exact     = b_.CreateAnd(in, whole);
            llvm::Value* safe = b_.CreateSelect(exact, v, llvm::ConstantFP::get(v->getType(), 0.0));
            converted = convert(safe, from, to);
        } else {                           // float -> float: NaN stays NaN; otherwise it must round-trip
            converted = convert(v, from, to);
            exact = b_.CreateOr(b_.CreateFCmpUNO(v, v), b_.CreateFCmpOEQ(convert(converted, to, from), v));
        }
        return option_of(option, converted, exact);
    }

    /// `cast::dynamic<derived&>(base)`: walks the object's vtables, from its own class to the root,
    /// looking for the target class's vtable (spec 07, 17).
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

    /// `some(value)` when `present`, else `none`, of the option type `option`.
    llvm::Value* option_of(const types::Type* option, llvm::Value* value, llvm::Value* present) {
        const Symbol*     e    = option->symbol;
        llvm::Type*       et   = user_type(e);
        llvm::AllocaInst* slot = entry_alloca(et, "option");
        b_.CreateStore(llvm::Constant::getNullValue(et), slot);
        b_.CreateStore(b_.CreateSelect(present, tag_constant(alternative(e, "some")), tag_constant(alternative(e, "none"))),
                       b_.CreateStructGEP(et, slot, 0));
        b_.CreateStore(value, b_.CreateStructGEP(et, slot, 1));
        return b_.CreateLoad(et, slot);
    }

    // --- globals --------------------------------------------------------------------------

    std::map<const Symbol*, llvm::GlobalVariable*> globals_;
    std::map<const Symbol*, llvm::Function*>       global_inits_;
    std::deque<const Symbol*>                      global_queue_;

    /// The storage of a global variable. Its initializer runs at startup (rift_init_globals).
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
        if (v.storage == Storage::thread_local_) {   // one copy per thread at a fixed offset from the thread pointer (spec 27)
            gv->setThreadLocalMode(llvm::GlobalValue::LocalExecTLSModel);
        }
        globals_[g] = gv;
        global_queue_.push_back(g);
        return gv;
    }

    /// Generates the initializer of a global as a function that stores its value.
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

    /// The modules in the order they are initialized: imported modules first (as in the C backend).
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

    /// rift_init_globals: runs the initializers of the globals the program uses, module by module.
    llvm::Function* emit_init_globals() {
        llvm::Function* fn = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {}, false),
                                                    llvm::Function::InternalLinkage, "rift_init_globals", mod_.get());
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
    struct DropScope {
        std::vector<OwnedLocal> locals;
        bool                    loop = false;   // the body of a loop: break and continue stop here
    };
    std::vector<DropScope>                       scopes_;
    std::map<const Symbol*, OwnedLocal>          owned_;
    std::map<const Symbol*, bool>                needs_drop_memo_;
    std::map<const Symbol*, llvm::Function*>     drop_fns_;
    std::deque<const Symbol*>                    drop_queue_;
    std::map<const Symbol*, llvm::Function*>     clone_fns_;
    std::set<const Expr*>                        converting_;
    std::deque<const Symbol*>                    clone_queue_;
    const types::Type*                           result_ = nullptr;   // the result of the function

    /// A type needs a destructor when it has one, or when a field or payload does.
    bool needs_drop(const types::Type* t) {
        if (t && t->is(Kind::function)) return true;   // frees its captures
        if (t && t->is(Kind::array)) return needs_drop(t->inner);
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

    /// The function that destroys a value of user type `t`; it is generated later.
    llvm::Function* drop_function(const types::Type* t) {
        if (t->is(Kind::function)) return function_drop();
        if (t->is(Kind::closure)) return closure_drop(t);
        if (t->is(Kind::array)) return array_function(t, false);
        const Symbol* s = t->symbol;
        if (auto it = drop_fns_.find(s); it != drop_fns_.end()) return it->second;
        std::vector<std::string> parts = symbol_scope(s);
        parts.push_back(s->name);
        if (s->template_of) parts.push_back("0i" + std::to_string(s->instance_index));
        auto* fn = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {llvm::PointerType::get(ctx_, 0)}, false),
                                          llvm::Function::InternalLinkage, "rd_" + mangle_parts(parts), mod_.get());
        drop_fns_[s] = fn;
        drop_queue_.push_back(s);
        return fn;
    }

    /// Destroys a `std::function`: destroys its captures (the closure's drop function, if it has
    /// one) and frees them.
    llvm::Function* function_drop() {
        if (llvm::Function* f = mod_->getFunction("rift_function_drop")) return f;
        auto* fn = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {ptr_type()}, false),
                                          llvm::Function::InternalLinkage, "rift_function_drop", mod_.get());
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

    /// Destroys (`clone` false: `(ptr)`) or clones (`(ptr dst, ptr src)`) every element of an
    /// inline array, in a loop.
    llvm::Function* array_function(const types::Type* t, bool clone) {
        auto key = std::make_pair(t, clone);
        if (auto it = array_fns_.find(key); it != array_fns_.end()) return it->second;
        std::vector<llvm::Type*> params{ptr_type()};
        if (clone) params.push_back(ptr_type());
        auto* fn = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), params, false),
                                          llvm::Function::InternalLinkage,
                                          (clone ? "rac_" : "rad_") + std::to_string(array_fns_.size()), mod_.get());
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

    std::map<const types::Type*, llvm::Function*> closure_drops_;

    /// Destroys the value captures of a lambda that need it.
    llvm::Function* closure_drop(const types::Type* t) {
        if (auto it = closure_drops_.find(t); it != closure_drops_.end()) return it->second;
        auto* fn = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {ptr_type()}, false),
                                          llvm::Function::InternalLinkage, "rld_" + std::to_string(closure_drops_.size()), mod_.get());
        closure_drops_[t] = fn;
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

    /// A lambda as a `std::function` (its captures move to the heap) or as a `std::fn_ptr` (a
    /// function without the captures argument), spec 18.
    llvm::Value* callable_from_lambda(Expr& e, const types::Type* to) {
        const types::Type*     ct = types::strip_ref(e.type);
        const ast::LambdaExpr* l  = tc_.lambda_of(ct);
        llvm::Value*           v  = gen_expr(e);
        llvm::Function*        fn = lambda_function(*l);
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

    /// Calls a `std::function` or `std::fn_ptr` value.
    llvm::Value* callable_call(Expr& callee, const types::Type* t, const std::vector<Expr*>& args) {
        llvm::Value* v = gen_expr(callee);
        std::vector<llvm::Value*> values;
        std::vector<llvm::Type*>  params;
        llvm::Value* target = v;
        if (t->is(Kind::function)) {
            values.push_back(b_.CreateExtractValue(v, 0));
            params.push_back(ptr_type());
            target = b_.CreateExtractValue(v, 1);
        }
        for (std::size_t i = 0; i < args.size() && i + 1 < t->args.size(); ++i) {
            if (t->args[i]->is(Kind::reference)) {
                values.push_back(object_address(*args[i]));
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

    /// `x.clone()` for the object of type `t` at `src` (spec 05, 12): copy types are copied, a class
    /// calls its `clone()` method, and structs and enums clone what they contain.
    llvm::Value* clone_value(const types::Type* t, llvm::Value* src) {
        if (!needs_drop(t)) return b_.CreateLoad(lower_type(t), src);
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

    /// The function that clones a struct or enum of type `t` into place: `(ptr dst, ptr src)`.
    llvm::Function* clone_function(const types::Type* t) {
        const Symbol* s = t->symbol;
        if (auto it = clone_fns_.find(s); it != clone_fns_.end()) return it->second;
        std::vector<std::string> parts = symbol_scope(s);
        parts.push_back(s->name);
        if (s->template_of) parts.push_back("0i" + std::to_string(s->instance_index));
        auto* ptr = llvm::PointerType::get(ctx_, 0);
        auto* fn  = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {ptr, ptr}, false),
                                           llvm::Function::InternalLinkage, "rc_" + mangle_parts(parts), mod_.get());
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
            llvm::Value*      tag  = b_.CreateLoad(enum_tag_type(s), b_.CreateStructGEP(lt, src, 0));
            llvm::BasicBlock* done = llvm::BasicBlock::Create(ctx_, "clone.done", fn);
            llvm::SwitchInst* sw   = b_.CreateSwitch(tag, done);
            for (const Symbol* a : s->member_order) {
                if (!carries(a)) continue;
                const types::Type* p = payload_of(a);
                if (!needs_drop(p)) continue;
                llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx_, "clone.case", fn);
                sw->addCase(tag_constant(a), bb);
                b_.SetInsertPoint(bb);
                b_.CreateStore(clone_value(p, b_.CreateStructGEP(lt, src, 1)), b_.CreateStructGEP(lt, dst, 1));
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

    /// Runs the destructor (if any), then destroys the fields in reverse declaration order, or the
    /// live payload of an enum.
    void gen_drop(const Symbol* s) {
        llvm::Function*                  fn       = drop_fns_[s];
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
            llvm::Type*  et  = user_type(s);
            llvm::Value* tag = b_.CreateLoad(enum_tag_type(s), b_.CreateStructGEP(et, self, 0));
            llvm::BasicBlock* done = llvm::BasicBlock::Create(ctx_, "drop.done", fn);
            llvm::SwitchInst* sw   = b_.CreateSwitch(tag, done);
            for (const Symbol* a : s->member_order) {
                if (!carries(a)) continue;
                const types::Type* p = payload_of(a);
                if (!needs_drop(p)) continue;
                llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx_, "drop.case", fn);
                sw->addCase(tag_constant(a), bb);
                b_.SetInsertPoint(bb);
                b_.CreateCall(drop_function(p), {b_.CreateStructGEP(et, self, 1)});
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

    /// Ends the innermost scope: its owned locals are destroyed, last declared first.
    void close_scope() {
        if (scopes_.empty()) return;
        if (!terminated()) {
            record_drops(scopes_.size() - 1);
            destroy_from(scopes_.size() - 1);
        }
        scopes_.pop_back();
    }

    /// Destroys the owned locals of the scopes from `first` inwards without leaving them (before
    /// `return`, `break` and `continue`).
    void destroy_from(std::size_t first) {
        for (std::size_t i = scopes_.size(); i-- > first;) {
            const auto& locals = scopes_[i].locals;
            for (auto it = locals.rbegin(); it != locals.rend(); ++it) drop_if(it->flag, it->drop, it->slot);
        }
    }

    /// The index of the innermost loop body (0 when there is none).
    std::size_t loop_scope() const {
        for (std::size_t i = scopes_.size(); i-- > 0;) {
            if (scopes_[i].loop) return i;
        }
        return 0;
    }

    /// Calls `drop` on the object at `addr` when `flag` is set.
    void drop_if(llvm::AllocaInst* flag, llvm::Function* drop, llvm::Value* addr) {
        llvm::BasicBlock* yes = llvm::BasicBlock::Create(ctx_, "drop", cur_fn_);
        llvm::BasicBlock* no  = llvm::BasicBlock::Create(ctx_, "drop.done", cur_fn_);
        b_.CreateCondBr(b_.CreateLoad(llvm::Type::getInt1Ty(ctx_), flag), yes, no);
        b_.SetInsertPoint(yes);
        b_.CreateCall(drop, {addr});
        b_.CreateBr(no);
        b_.SetInsertPoint(no);
    }

    /// Declares the owned local `sym` (stored at `slot`). `owns_now` is true for parameters.
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

    /// The owned local that `e` names, looking through `move`; null for anything else.
    const OwnedLocal* owned_path(const Expr& e) {
        const Expr* x = &e;
        if (x->kind == ExprKind::unary && x->as<UnaryExpr>().op == UnaryOp::move) x = x->as<UnaryExpr>().operand.get();
        if (x->kind != ExprKind::path) return nullptr;
        auto it = owned_.find(x->as<PathExpr>().symbol);
        return it == owned_.end() ? nullptr : &it->second;
    }

    /// After a value was moved out of `e`, the local no longer owns it.
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

    /// Evaluates `e` for its effects. A value that needs a destructor is destroyed at once.
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

    // --- insights (`riftc insights`): what the compiler does implicitly ---------------------
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
    /// What is known about the capacity of a local `darray` / `string` (spec 20).
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

    /// `std::darray<u32>::push`, `geometry::length`: how a function is shown (with its namespace).
    std::string display_name(const Symbol* s) {
        if (!s) return "?";
        if (s->parent && (s->parent->kind == SymbolKind::struct_ || s->parent->kind == SymbolKind::class_ ||
                          s->parent->kind == SymbolKind::enum_)) {
            return types::qualified_name(tc_.user(s->parent)) + "::" + s->name;
        }
        return s->key();
    }

    /// The span of the name a call is made through: `push` in `list.push(7)`, `from` in
    /// `std::string::from(x)`.
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

    /// The value of an integer constant expression, if it is one.
    static std::optional<std::uint64_t> constant_count(const Expr& e) {
        if (e.kind == ExprKind::int_literal) return static_cast<std::uint64_t>(e.as<LiteralExpr>().int_value);
        if (e.constant && e.constant->kind == ct::Value::Kind::integer) return static_cast<std::uint64_t>(e.constant->integer);
        return std::nullopt;
    }

    /// How often code at this point runs per run of the function: the product of the constant
    /// trip counts of the open loops (nullopt when one is not constant).
    std::optional<std::uint64_t> repeat() const {
        std::uint64_t n = 1;
        for (const auto& t : trips_) {
            if (!t) return std::nullopt;
            n *= *t;
        }
        return n;
    }

    /// The tracked capacity of the local container that `e` names, or null.
    Capacity* tracked(const Expr& e) {
        if (e.kind != ExprKind::path) return nullptr;
        auto it = caps_.find(e.as<PathExpr>().symbol);
        return it == caps_.end() ? nullptr : &it->second;
    }

    bool is_darray(const types::Type* t) const {
        if (!t || !t->is(Kind::user) || !program_.std_module) return false;
        return t->symbol->template_of == program_.std_module->item("std::darray");
    }

    /// A local container starts with a known capacity: `default`, `with_capacity(n)`, `from(literal)`.
    void track_init(const Symbol* var, const Expr* init) {
        if (!inspect_ || !var) return;
        const types::Type* t = var->type;
        bool text = is_string(t);
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

    /// The certain-allocation message of a call on a tracked container, "" for "may allocate", or
    /// "none" when it surely does not allocate. Updates what is known.
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
        static const std::set<std::string> reading{"len", "is_empty", "capacity", "get", "contains", "as_span", "as_str", "clone",
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

    /// A call of `s` (as `fn`) at `name_span` in the inspected code.
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
            bool text = s && s->parent && is_string(tc_.user(s->parent));
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

    /// A new function (or lambda) of the inspected file starts.
    void start_inspecting(Span span) {
        inspect_    = options_.insights && span.file == inspect_file_;
        cond_depth_ = 0;
        trips_.clear();
        caps_.clear();
        moved_.clear();
        decl_depth_.clear();
    }

    /// Using the place `e` by value as a `t` copies it: shown when that is more than 16 bytes.
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

    /// `std::println(f"...")` / `std::format(f"...")`: the text is built in a buffer.
    void record_format(Span at) {
        if (!inspect_) return;
        insight(at, "temporaries", "temporary", "range",
                std::format("temporary: the text being formatted ({} on the stack)", bytes_text(byte_size(buffer_type()))),
                "An unnamed stack slot whose address is used (passed to a function, or indexed), so it stays in memory in optimized "
                "builds too. Like every slot of the function it is reserved when the function is entered. It describes a buffer whose "
                "bytes are on the heap.");
        insight(at, "heap", "alloc", "range", "allocates: the formatted text",
                "Formatting writes the text into a heap buffer that grows as needed (`rift_buf_put` → `realloc`) and is freed once "
                "the text is written or copied into the `std::string`.");
    }

    /// The owned local `sym` is no longer owned at this point.
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

    /// The stack frame of the function just generated (`fn`, body `body`).
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

    /// Whether the address of the slot `v` (or of a part of it) leaves plain loads and stores: then
    /// it stays in memory after optimization; otherwise LLVM keeps it in registers (SROA, mem2reg).
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
        case Expr::Conversion::to_callable:
            insight(e.span, "conversions", "to-callable", "range",
                    std::format("lambda → {}", types::qualified_name(e.converted_to)),
                    "Implicit (spec 18): a `std::function` keeps the lambda's captures on the heap; a `std::fn_ptr` is just the code.");
            break;
        default: break;
        }
    }

    /// Calls reach the allocator (or `free`) through these functions; computed once at the end.
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
        std::set<std::string> alloc_targets{"malloc", "realloc", "calloc", "rift_buf_put", "rift_fmt_u", "rift_fmt_i", "rift_fmt_f",
                                            "rift_fmt_str", "rift_fmt_bool", "rift_fmt_char"};
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
    llvm::Function*                                   rift_main_ = nullptr;
    llvm::Function*                                   rift_panic_str_ = nullptr;
    llvm::Function*                                   rift_exit_status_ = nullptr;
    llvm::Value*                                      self_ = nullptr;
    llvm::StructType*                                 loc_type_ = nullptr;
    std::map<const Symbol*, llvm::Function*>          fns_;
    std::deque<const Symbol*>                         queue_;
    std::map<const LambdaExpr*, llvm::Function*>      lambda_fns_;
    std::deque<const LambdaExpr*>                     lambda_queue_;
    std::map<const types::Type*, llvm::StructType*>   closure_types_;
    std::map<const Symbol*, llvm::Value*>             locals_;   // the storage of each local (lambda captures: their place in the closure)
    std::map<std::pair<FileId, std::uint32_t>, llvm::Constant*> locs_;
    std::vector<Loop>                                 loops_;
    std::set<std::tuple<FileId, std::uint32_t, std::string>>    reported_;
};

} // namespace

std::string generate_llvm_ir(sema::Program& program, const SourceManager& sources, DiagnosticEngine& diags,
                             LlvmOptions options) {
    llvm::LLVMContext ctx;
    Gen               gen(program, sources, diags, options, ctx);
    std::unique_ptr<llvm::Module> mod = gen.run();
    if (!mod) return {};
    std::string           printed;
    llvm::raw_string_ostream module_out(printed);
    mod->print(module_out, nullptr);
    module_out.flush();
    // The runtime defines the functions and intrinsics it shares with the module; the module's
    // declarations of them would clash with those.
    std::string runtime_text = llvm_runtime_ir();
    {   // the futex system call's number on the host (riftc compiles for the machine it runs on)
        llvm::Triple      host(target_triple());
        const std::string number = host.isAArch64() || host.isRISCV() ? "98" : "202";
        for (std::size_t at; (at = runtime_text.find("@@SYS_FUTEX@@")) != std::string::npos;) runtime_text.replace(at, 13, number);
    }
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
    if (options.panic_trap) {   // every panic ends in rift_panic_buf: make it a trap
        const std::string head = "define internal void @rift_panic_buf(ptr %loc, ptr %msg) noreturn cold {";
        std::size_t at = runtime_text.find(head);
        if (at != std::string::npos) {
            runtime_text.replace(at, head.size(), head + "\n  call void @llvm.trap()\n  unreachable\n}\n"
                                                  "define internal void @rift_panic_buf.message(ptr %loc, ptr %msg) noreturn cold {");
            runtime_text += "\ndeclare void @llvm.trap() cold noreturn nounwind\n";
        }
    }
    return text + "\n" + runtime_text;
}

bool build_executable(std::string_view ir, const std::string& out_path, const std::string& linker, std::string& error,
                      const CodegenOptions& codegen, const std::vector<std::string>& extra_inputs) {
    initialize_targets();
    llvm::LLVMContext ctx;
    llvm::SMDiagnostic err;
    std::unique_ptr<llvm::Module> mod = llvm::parseIR(llvm::MemoryBufferRef(llvm::StringRef(ir.data(), ir.size()), "rift.ll"), err, ctx);
    if (!mod) {
        std::string text;
        llvm::raw_string_ostream os(text);
        err.print("riftc", os);
        error = "internal error: the generated LLVM IR does not parse\n" + text;
        return false;
    }
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
    llvm::TargetOptions options;
    bool optimize = codegen.optimization != "none";
    std::unique_ptr<llvm::TargetMachine> machine(make_target_machine(
        target, triple_name, options, optimize ? llvm::CodeGenOptLevel::Aggressive : llvm::CodeGenOptLevel::None));
    set_target_triple(*mod, triple_name);
    mod->setDataLayout(machine->createDataLayout());
    if (optimize) {   // the mode's optimization() (spec 24): LLVM's standard pipeline
        llvm::LoopAnalysisManager     lam;
        llvm::FunctionAnalysisManager fam;
        llvm::CGSCCAnalysisManager    cgam;
        llvm::ModuleAnalysisManager   mam;
        llvm::PassBuilder             pb(machine.get());
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        llvm::OptimizationLevel level = codegen.optimization == "size" ? llvm::OptimizationLevel::Os : llvm::OptimizationLevel::O2;
        llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(level);
        mpm.run(*mod, mam);
    }

    std::string object_path = out_path + ".o";
    std::error_code ec;
    {
        llvm::raw_fd_ostream file(object_path, ec, llvm::sys::fs::OF_None);
        if (ec) {
            error = "cannot write " + object_path + ": " + ec.message();
            return false;
        }
        llvm::legacy::PassManager pm;
        if (machine->addPassesToEmitFile(pm, file, nullptr, llvm::CodeGenFileType::ObjectFile)) {
            error = "the target cannot emit object files";
            return false;
        }
        pm.run(*mod);
    }
    std::string command = linker + " -o '" + out_path + "' '" + object_path + "'";
    for (const std::string& input : extra_inputs) command += " '" + input + "'";
    command += " -lm";
    int         status  = std::system(command.c_str());
    llvm::sys::fs::remove(object_path);
    if (status != 0) {
        error = "the linker '" + linker + "' failed";
        return false;
    }
    return true;
}

bool set_build_target(const std::string& platform, std::string& error) {
    if (platform.empty()) {
        g_target_triple.clear();
        return true;
    }
    llvm::Triple t(llvm::Triple::normalize(platform));
    if (!t.isArch64Bit() || !(t.isAArch64() || t.getArch() == llvm::Triple::x86_64 || t.getArch() == llvm::Triple::riscv64)) {
        error = std::format("'{}' is not a supported platform: riftc compiles for 64-bit x86-64, AArch64 and RISC-V", platform);
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
        error = std::format("this riftc's LLVM can't generate code for '{}': {}", platform, lookup_error);
        g_target_triple.clear();
        return false;
    }
    return true;
}

bool target_is_host() { return g_target_triple.empty() || llvm::Triple(g_target_triple).getArch() == llvm::Triple(llvm::sys::getDefaultTargetTriple()).getArch(); }

} // namespace rift
