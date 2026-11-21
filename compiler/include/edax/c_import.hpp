#pragma once
// Importing C headers (C interop, "Importing C headers"): `g.import_c_headers(target, spec)`.

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace edax {

struct CImportOptions {
    std::vector<std::string> headers;        // the headers to translate (as written in the spec)
    std::vector<std::string> include_dirs;   // the C target's include directories
    std::string              ns;             // the namespace of the generated declarations
    std::map<std::string, std::string> macro_values;   // object-like macros, expanded (c_macro_values)
};

struct CImportResult {
    std::string                                      header;     // the generated `.edh`
    std::string                                      source;     // its `.ed` (union accessors)
    std::vector<std::pair<std::string, std::string>> skipped;    // name, why it isn't translated
    int                                              functions = 0;
};

// Lines to append to the headers' translation unit so that the preprocessor expands each
// object-like macro of theirs: `#define LZMA_FILTER_X86 LZMA_VLI_C(0x04)` becomes `0x04UL`.
std::string c_macro_probe(const std::string& preprocessed, const CImportOptions& options);
// The expansions, from the preprocessed probe.
std::map<std::string, std::string> c_macro_values(const std::string& probe_output);

// Translates the output of `cc -E -dD` of the headers. Only declarations from the listed
// headers (or files under the include directories) are translated; system headers are not.
CImportResult import_c_headers(const std::string& preprocessed, const CImportOptions& options);

} // namespace edax
