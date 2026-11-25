#pragma once
// The standard library's Edax source, compiled into edaxc (spec 20: the library is written in
// Edax where possible). The text comes from library/std/ at build time.

#include <span>
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

} // namespace edax
