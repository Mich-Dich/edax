#pragma once
// Test helper: a throw-away project directory that is loaded, resolved and (optionally) type
// checked and ownership checked like `edaxc check` does.

#include "edax/check.hpp"
#include "edax/modules.hpp"
#include "edax/ownership.hpp"
#include "edax/resolve.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace edax_test {

enum class Stop { resolve, types, ownership };

struct Project {
    std::filesystem::path  root;
    edax::SourceManager    sources;
    edax::DiagnosticEngine diags;
    edax::sema::Program    program;

    explicit Project(const std::map<std::string, std::string>& files, Stop stop = Stop::ownership,
                     std::string entry = "main.ed") {
        static int counter = 0;
        root = std::filesystem::temp_directory_path() / ("edax_unit_project_" + std::to_string(++counter));
        std::filesystem::remove_all(root);
        for (const auto& [name, text] : files) {
            std::filesystem::path p = root / name;
            std::filesystem::create_directories(p.parent_path());
            std::ofstream(p) << text;
        }
        program = edax::load_program((root / entry).string(), [&] { edax::LoadOptions o; o.root = root.string(); return o; }(), sources, diags);
        if (!diags.has_errors()) edax::resolve_program(program, diags);
        if (!diags.has_errors() && stop != Stop::resolve) edax::check_types(program, sources, diags);
        if (!diags.has_errors() && stop == Stop::ownership) edax::check_ownership(program, diags);
    }
    ~Project() { std::filesystem::remove_all(root); }
    Project(const Project&)            = delete;
    Project& operator=(const Project&) = delete;

    std::string codes() const {
        std::string out;
        for (const edax::Diagnostic& d : diags.all()) {
            out += (out.empty() ? "" : " ") + std::string(edax::code_name(d.code));
        }
        return out;
    }

    edax::sema::Module& module(const std::string& name) { return *program.modules.at(name); }

    std::string symbols(const std::string& name) {
        std::ostringstream out;
        edax::print_symbols(out, module(name), sources);
        return out.str();
    }
};

} // namespace edax_test
