#pragma once
// The CMake reader (packages, section 4): evaluates a package's CMakeLists.txt to find its
// targets without running CMake. What it can't translate stops it with the command, its file and
// line; it never guesses.

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace rift {

struct CMakeTarget {
    enum class Kind { static_library, shared_library, object_library, interface_library, executable, alias };
    Kind        kind = Kind::static_library;
    std::string name;
    std::string alias_of;   // alias
    std::string language = "c";   // "cpp" when it has C++ sources
    std::string standard;         // "c11", "cpp17", ... from features or CMAKE_<LANG>_STANDARD
    std::vector<std::string> sources;   // absolute
    // (value, visibility): "shared" (PUBLIC), "own" (PRIVATE), "users" (INTERFACE).
    std::vector<std::pair<std::string, std::string>> include_dirs;
    std::vector<std::pair<std::string, std::string>> defines;   // "NAME" or "NAME=value"
    std::vector<std::pair<std::string, std::string>> options;
    std::vector<std::pair<std::string, std::string>> links;     // a target, a `.uses` name, or a system library
    std::string file;   // where it was added
    int         line = 0;
};

struct CMakeInput {
    std::string                        root;      // the package's directory
    std::string                        out_dir;   // for configure_file outputs
    std::map<std::string, std::string> options;   // the package's `.options`
    std::map<std::string, std::string> uses;      // find_package names answered by `.uses` (name → anything)
    std::string                        build_type = "Debug";
    std::string                        system_name = "Linux";
    // Runs the C compiler for a check (`check_include_file`, ...): true when `source` compiles.
    std::function<bool(const std::string& source)> compiles;
};

struct CMakeResult {
    std::vector<CMakeTarget>                         targets;
    std::vector<std::string>                         files;      // the CMake files read
    std::vector<std::pair<std::string, std::string>> commands;   // a command (file:line) and what it became
    std::vector<std::string>                         system_libraries;   // find_package/find_library not in `.uses`
    std::string                                      error;      // "file:line: message"
};

CMakeResult read_cmake(const CMakeInput& input);

} // namespace rift
