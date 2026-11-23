#pragma once
// Optimized builds through the cache with LLVM's ThinLTO (build modes, "The object cache").
// Kept in a translation unit of its own: LLVM's LTO headers don't compile as C++26.

#include <functional>
#include <string>
#include <vector>

namespace llvm {
class PassBuilder;
class TargetMachine;
} // namespace llvm

namespace edax {

struct ThinLtoJob {
    std::string triple;
    std::string cpu;
    std::string features;    // comma-separated, as LLVM spells them
    unsigned    opt_level = 2;
    std::string cache_dir;   // the ThinLTO object cache
    std::string work_dir;    // where the objects of this link go
    std::function<llvm::TargetMachine*()>   machine;   // a new target machine (owned by the caller of it)
    std::function<void(llvm::PassBuilder&)> passes;             // Edax's own passes, for the pre-link pipeline
    std::function<void(llvm::PassBuilder&)> post_link_passes;   // and for the post-link pipeline
};

// Writes one unit (its IR text) with its summary as bitcode to `path`. Thread-safe.
bool thin_prelink(const ThinLtoJob& job, const std::string& ir_text, const std::string& path, std::string& error);

// Links the units' bitcode files with ThinLTO: imports, internalization, the post-link pipeline
// and code generation, through the cache. `objects` gets the object files.
bool thin_link(const ThinLtoJob& job, const std::vector<std::string>& bitcode, std::vector<std::string>& objects, std::size_t& compiled,
               std::string& error);

} // namespace edax
