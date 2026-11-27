#include "edax/thin_lto.hpp"

#include "llvm/Analysis/ModuleSummaryAnalysis.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/LTO/LTO.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Support/Caching.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>

// Edax's own passes in ThinLTO's post-link pipeline. LLVM's LTO library gives no hook for them,
// except the statically registered pass plugins it runs on every pipeline: on Debian/Ubuntu's
// LLVM that is Polly, which edaxc doesn't link (the packages don't ship it by default). This
// stand-in for it registers Edax's passes while a thin_link runs (CMake checks that LLVM has the
// hook: EDAX_HAVE_LTO_PASS_HOOK).
namespace edax {
namespace {
std::function<void(llvm::PassBuilder&)>* g_post_link_passes = nullptr;
}
} // namespace edax

__attribute__((weak)) llvm::PassPluginLibraryInfo getPollyPluginInfo() {
    return {LLVM_PLUGIN_API_VERSION, "edax", "1", [](llvm::PassBuilder& pb) {
                if (edax::g_post_link_passes && *edax::g_post_link_passes) (*edax::g_post_link_passes)(pb);
            }};
}

namespace edax {

bool thin_prelink(const ThinLtoJob& job, const std::string& ir, const std::string& path, std::string& error) {
    llvm::LLVMContext  ctx;
    llvm::SMDiagnostic err;
    std::unique_ptr<llvm::Module> mod =
        llvm::parseIR(llvm::MemoryBufferRef(llvm::StringRef(ir.data(), ir.size()), "edax-unit"), err, ctx);
    if (!mod) {
        error = "internal error: a unit's LLVM IR does not parse";
        return false;
    }
    std::unique_ptr<llvm::TargetMachine> machine(job.machine());
    mod->setDataLayout(machine->createDataLayout());
    llvm::LoopAnalysisManager          lam;
    llvm::FunctionAnalysisManager      fam;
    llvm::CGSCCAnalysisManager         cgam;
    llvm::ModuleAnalysisManager        mam;
    llvm::PassInstrumentationCallbacks pic;
    llvm::StandardInstrumentations     si(ctx, false);
    si.registerCallbacks(pic, &mam);
    llvm::PipelineTuningOptions tuning;
    tuning.SLPVectorization = true;   // as in the backend's pipeline_tuning()
    llvm::PassBuilder pb(machine.get(), tuning, std::nullopt, &pic);
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    if (job.passes) job.passes(pb);
    // No pre-link optimization: inlining inside a unit before the cross-unit imports changes the
    // order the whole-program inliner would use, and loses alias information the vectorizer needs
    // (bench/particles ran 7x slower). The post-link pipeline optimizes each unit with its imports.
    llvm::ModulePassManager mpm;
    mpm.run(*mod, mam);
    // Written next to its final name and renamed: another edaxc may be writing it too.
    std::string temporary = path + ".tmp" + std::to_string(llvm::sys::Process::getProcessId()) + "-" +
                            std::to_string(reinterpret_cast<std::uintptr_t>(&ctx));
    {
        std::error_code ec;
        llvm::raw_fd_ostream file(temporary, ec, llvm::sys::fs::OF_None);
        if (ec) {
            error = "cannot write " + temporary + ": " + ec.message();
            return false;
        }
        const llvm::ModuleSummaryIndex& index = mam.getResult<llvm::ModuleSummaryIndexAnalysis>(*mod);
        llvm::WriteBitcodeToFile(*mod, file, false, &index, /*GenerateHash=*/true);
    }
    if (std::error_code ec = llvm::sys::fs::rename(temporary, path)) {
        error = "cannot write " + path + ": " + ec.message();
        llvm::sys::fs::remove(temporary);
        return false;
    }
    return true;
}

bool thin_link(const ThinLtoJob& job, const std::vector<std::string>& bitcode, std::vector<std::string>& objects, std::size_t& compiled,
               std::string& error) {
    {   // Import functions up to 300 instructions (LLVM's default is 100): the units aren't
        // optimized before the summary is made, so their functions count larger than they are.
        static std::once_flag once;
        std::call_once(once, [] {
            const char* argv[] = {"edaxc", "-import-instr-limit=300"};
            llvm::cl::ParseCommandLineOptions(2, argv);
        });
    }
    llvm::lto::Config conf;
    conf.CPU = job.cpu;
    for (llvm::StringRef f : llvm::split(llvm::StringRef(job.features), ',')) {
        if (!f.empty()) conf.MAttrs.push_back(f.str());
    }
    conf.OptLevel                  = job.opt_level;
    conf.PTO.SLPVectorization      = job.opt_level > 1;   // off by default in LLVM; clang and lld turn it on (see pipeline_tuning)
    conf.CGOptLevel                = job.opt_level == 1 ? llvm::CodeGenOptLevel::Less : llvm::CodeGenOptLevel::Aggressive;
    conf.RelocModel                = llvm::Reloc::PIC_;
    conf.DefaultTriple             = job.triple;
    conf.HasWholeProgramVisibility = true;
    std::function<void(llvm::PassBuilder&)> post_link = job.post_link_passes;
    g_post_link_passes = &post_link;
    struct Reset {
        ~Reset() { g_post_link_passes = nullptr; }
    } reset;
    llvm::lto::LTO lto(std::move(conf), llvm::lto::createInProcessThinBackend(llvm::heavyweight_hardware_concurrency()));
    std::vector<std::unique_ptr<llvm::MemoryBuffer>> buffers;
    std::set<std::string>                            defined;
    for (const std::string& path : bitcode) {
        llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer = llvm::MemoryBuffer::getFile(path);
        if (!buffer) {
            error = "cannot read " + path;
            return false;
        }
        buffers.push_back(std::move(*buffer));
        llvm::Expected<std::unique_ptr<llvm::lto::InputFile>> input = llvm::lto::InputFile::create(buffers.back()->getMemBufferRef());
        if (!input) {
            error = "internal error: ThinLTO input: " + llvm::toString(input.takeError());
            return false;
        }
        std::vector<llvm::lto::SymbolResolution> resolutions;
        for (const llvm::lto::InputFile::Symbol& sym : (*input)->symbols()) {
            llvm::lto::SymbolResolution r;
            bool definition                = !sym.isUndefined();
            r.Prevailing                   = definition && defined.insert(sym.getName().str()).second;
            r.FinalDefinitionInLinkageUnit = definition;
            // Hidden symbols belong to the program's units: no other object refers to them.
            r.VisibleToRegularObj = sym.getVisibility() != llvm::GlobalValue::HiddenVisibility;
            resolutions.push_back(r);
        }
        if (llvm::Error e = lto.add(std::move(*input), resolutions)) {
            error = "internal error: ThinLTO: " + llvm::toString(std::move(e));
            return false;
        }
    }
    std::vector<std::string> outputs(lto.getMaxTasks());
    auto output_path = [&](std::size_t task) { return job.work_dir + "/thin-" + std::to_string(task) + ".o"; };
    compiled = 0;
    auto add_buffer = [&](std::size_t task, const llvm::Twine&, std::unique_ptr<llvm::MemoryBuffer> mb) {
        std::error_code ec;
        llvm::raw_fd_ostream file(output_path(task), ec, llvm::sys::fs::OF_None);
        if (!ec) file << mb->getBuffer();
        outputs[task] = output_path(task);
    };
    llvm::Expected<llvm::FileCache> cache = llvm::localCache("ThinLTO", "Thin", job.cache_dir, add_buffer);
    if (!cache) {
        error = "cannot use the ThinLTO cache: " + llvm::toString(cache.takeError());
        return false;
    }
    auto add_stream = [&](std::size_t task, const llvm::Twine&) -> llvm::Expected<std::unique_ptr<llvm::CachedFileStream>> {
        std::error_code ec;
        auto stream = std::make_unique<llvm::raw_fd_ostream>(output_path(task), ec, llvm::sys::fs::OF_None);
        if (ec) return llvm::createStringError(ec, "cannot write " + output_path(task));
        outputs[task] = output_path(task);
        ++compiled;
        return std::make_unique<llvm::CachedFileStream>(std::move(stream));
    };
    if (llvm::Error e = lto.run(add_stream, *cache)) {
        error = "internal error: ThinLTO: " + llvm::toString(std::move(e));
        return false;
    }
    std::error_code ec;
    for (const std::string& o : outputs) {
        if (!o.empty() && std::filesystem::file_size(o, ec) > 0 && !ec) objects.push_back(o);
    }
    return true;
}

} // namespace edax
