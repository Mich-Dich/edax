#pragma once
// The standard library's Edax source, compiled into edaxc (spec 20: the library is written in
// Edax where possible). The text comes from library/std/ at build time.

#include <span>
#include <string>
#include <string_view>

namespace edax {

// One module of the standard library: a header and a source (spec 09).
struct StdModule {
    std::string_view name;          // "std", "std::filesystem"
    std::string_view header_path;   // shown in diagnostics: "<std>/std.edh"
    std::string_view header_text;
    std::string_view source_path;
    std::string_view source_text;
};

// The library's modules; the first is `std` itself, which the others build on.
std::span<const StdModule> std_modules();

// A binary file of the standard library (a font), by its path below library/std
// ("text/fonts/DMSans-Regular.ttf"); a view with no data when there is none. `std::embed` in a
// std module reads these instead of the disk.
std::string_view std_resource(std::string_view path);

// The folder with the standard library's source (std.edh, std.ed, ...), for debug info and
// `edaxc definition`; the library itself is compiled into edaxc. An installed edaxc
// (`cmake --install`) uses `<prefix>/share/edax/std` next to its `bin`; otherwise the tree it was
// built from (`library/std`), which may no longer exist.
const std::string& std_source_dir();

} // namespace edax
