#include "edax/modules.hpp"

#include "edax/ast_printer.hpp"
#include "edax/graphics_registry.hpp"
#include "edax/lexer.hpp"
#include "edax/parser.hpp"
#include "edax/std_library.hpp"

#include <filesystem>
#include <format>
#include <map>
#include <mutex>
#include <system_error>

namespace edax {

namespace fs = std::filesystem;

namespace {

std::string display_path(const fs::path& p) {
    std::string s = p.lexically_normal().generic_string();
    return s.empty() ? "." : s;
}

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

std::string root_or_default(const std::string& entry, const std::string& root) {
    if (!root.empty()) {
        return root;
    }
    fs::path parent = fs::path(entry).parent_path();
    return parent.empty() ? "." : parent.generic_string();
}

class Loader {
public:
    Loader(std::string root, SourceManager& sources, DiagnosticEngine& diags, std::vector<std::string> extra_roots = {})
        : root_(std::move(root)), extra_roots_(std::move(extra_roots)), sources_(sources), diags_(diags) {}

    sema::Program run(const std::string& entry, const std::string& build_script = {}) {
        fs::path    entry_path(entry);
        std::string name = module_name_for(entry, root_);
        auto        module = std::make_unique<sema::Module>();
        module->name       = name;

        fs::path header = entry_path, source = entry_path;
        header.replace_extension(".edh");
        source.replace_extension(".ed");
        if (entry_path.extension() == ".edh" || is_file(header)) {
            load_file(*module, header, /*is_header=*/true);
        }
        if (entry_path.extension() == ".ed" || is_file(source)) {
            load_file(*module, source, /*is_header=*/false);
        }

        sema::Module* raw = module.get();
        program_.modules.emplace(name, std::move(module));
        program_.entry = raw;
        state_[name]   = State::visiting;
        stack_.push_back(name);
        process_imports(*raw);
        stack_.pop_back();
        state_[name] = State::done;
        if (!build_script.empty() && !program_.modules.count("build")) {
            auto script  = std::make_unique<sema::Module>();
            script->name = "build";
            load_file(*script, fs::path(build_script), /*is_header=*/false);
            sema::Module* s = script.get();
            program_.modules.emplace("build", std::move(script));
            state_["build"] = State::visiting;
            stack_.push_back("build");
            process_imports(*s);
            stack_.pop_back();
            state_["build"] = State::done;
        }
        load_named_std();
        return std::move(program_);
    }

private:
    enum class State { visiting, done };

    void load_file(sema::Module& module, const fs::path& path, bool is_header) {
        std::string           reason;
        std::optional<FileId> id = sources_.load(display_path(path), reason);
        if (!id) {
            return;   // the driver already checked the entry file; imports are checked before
        }
        parse_into(module, *id, is_header);
    }

    void parse_into(sema::Module& module, FileId id_value, bool is_header) {
        std::optional<FileId> id = id_value;
        sema::ModuleFile file{*id, {}};
        file.ast.file = *id;
        std::size_t errors_before = diags_.error_count();
        std::vector<Token> tokens = tokenize(sources_.file(*id), *id, diags_);
        if (diags_.error_count() == errors_before) {
            file.ast = parse(sources_.file(*id), *id, tokens, diags_);
        }
        program_.file_tokens[*id] = std::move(tokens);
        (is_header ? module.header : module.source) = std::move(file);
    }

    static std::vector<const ast::ImportDecl*> imports_of(const std::optional<sema::ModuleFile>& file) {
        std::vector<const ast::ImportDecl*> out;
        if (file) {
            for (const ast::DeclPtr& d : file->ast.decls) {
                if (d->kind == ast::DeclKind::import) {
                    out.push_back(&d->as<ast::ImportDecl>());
                }
            }
        }
        return out;
    }

    void process_imports(sema::Module& module) {
        for (int pass = 0; pass < 2; ++pass) {
            bool header = pass == 0;
            for (const ast::ImportDecl* imp : imports_of(header ? module.header : module.source)) {
                std::string name = ast::to_string(imp->path);
                if (imp->path.segments.front().name == "std") {
                    (header ? module.header_imports_std : module.source_imports_std) = true;
                    load_std();
                    continue;
                }
                if (name == module.name) {
                    diags_.report(DiagCode::E3004, imp->span,
                                  std::format("module '{}' imports itself", name), "remove this import")
                        .notes.push_back(Note{"a .ed file sees its own .edh automatically", std::nullopt});
                    continue;
                }
                if (sema::Module* target = load_module(name, *imp)) {
                    (header ? module.header_imports : module.source_imports).push_back(target);
                }
            }
        }
    }

    // Standard library modules that are loaded only when the program names them: checking them
    // costs time, and a program that never names them can't be affected by them (spec 31:
    // std::log). Returns the name a program's text must contain, or an empty view for the
    // modules every `import std;` loads. `std::windowing` and its parts (std::windowing::x11, ...)
    // all load together when a file names `std::windowing`.
    // std::graphics comes with std::windowing, which uses it (std modules' own text doesn't count
    // as naming a module).
    static std::vector<std::string_view> on_demand_triggers(std::string_view name) {
        if (name == "std::log") return {name};
        if (name == "std::windowing" || name.starts_with("std::windowing::")) return {"std::windowing"};
        if (name == "std::graphics") return {"std::graphics", "std::windowing"};
        if (name == "std::graphics::opengl" || name == "std::graphics::vulkan") return {name};
        return {};
    }
    static bool on_demand(std::string_view name) { return !on_demand_triggers(name).empty(); }

    // Whether a file of the program (not of std) contains one of `triggers`.
    bool program_names(const std::vector<std::string_view>& triggers) const {
        for (const auto& [mname, mod] : program_.modules) {
            if (mod->project == "std") continue;
            for (const auto* f : {&mod->header, &mod->source}) {
                if (!*f) continue;
                std::string_view text = sources_.file((*f)->id).text();
                for (std::string_view t : triggers) {
                    if (text.find(t) != std::string_view::npos) return true;
                }
            }
        }
        return false;
    }

    // Loads the on-demand std modules that some loaded file names.
    void load_named_std() {
        if (!program_.std_module) return;
        for (const StdModule& m : std_modules()) {
            if (!on_demand(m.name) || program_.modules.count(std::string(m.name))) continue;
            if (program_names(on_demand_triggers(m.name))) add_std_module(m);
        }
        // The modules made from the Khronos registries (graphics_registry.cpp), made the first time
        // a program names them.
        for (std::string_view name : {std::string_view("std::graphics::opengl"), std::string_view("std::graphics::vulkan")}) {
            if (program_.modules.count(std::string(name)) || !program_names(on_demand_triggers(name))) continue;
            if (const StdModule* m = generated_std_module(name)) add_std_module(*m);
        }
    }

    // std::graphics::opengl or ::vulkan, made from its registry once per process. Null (and an
    // error) when the registry can't be read.
    const StdModule* generated_std_module(std::string_view name) {
        struct Made {
            GeneratedStdModule text;
            StdModule          module;
            std::string        error;
        };
        static std::map<std::string, Made, std::less<>> made;
        static std::mutex                                 lock;
        std::lock_guard<std::mutex>                       guard(lock);
        auto it = made.find(name);
        if (it == made.end()) {
            Made m;
            bool vulkan = name == "std::graphics::vulkan";
            try {
                m.text = vulkan ? generate_vulkan_module(std_registry("vk.xml")) : generate_opengl_module(std_registry("gl.xml"));
            } catch (const std::exception& e) {
                m.error = e.what();
            }
            it = made.emplace(std::string(name), std::move(m)).first;
            Made& stored = it->second;
            stored.module = StdModule{vulkan ? "std::graphics::vulkan" : "std::graphics::opengl",
                                      vulkan ? "<std>/graphics/vulkan.edh" : "<std>/graphics/opengl.edh", stored.text.header,
                                      vulkan ? "<std>/graphics/vulkan.ed" : "<std>/graphics/opengl.ed", stored.text.source};
        }
        if (!it->second.error.empty()) {
            diags_.report(DiagCode::E3001, Span{}, std::format("cannot make module '{}': {}", name, it->second.error),
                          "the embedded Khronos registry could not be read");
            return nullptr;
        }
        return &it->second.module;
    }

    void add_std_module(const StdModule& m) {
        auto module  = std::make_unique<sema::Module>();
        module->name = std::string(m.name);
        module->project = "std";
        parse_into(*module, sources_.add(std::string(m.header_path), std::string(m.header_text)), true);
        parse_into(*module, sources_.add(std::string(m.source_path), std::string(m.source_text)), false);
        module->header_imports_std = module->source_imports_std = true;
        state_[module->name] = State::done;
        program_.modules.emplace(module->name, std::move(module));
    }

    // The standard library is built into edaxc (library/std/std.edh) and loaded once.
    void load_std() {
        if (program_.std_module) return;
        for (const StdModule& m : std_modules()) {
            if (on_demand(m.name)) continue;
            auto module  = std::make_unique<sema::Module>();
            module->name = std::string(m.name);
            module->project = "std";
            parse_into(*module, sources_.add(std::string(m.header_path), std::string(m.header_text)), true);
            parse_into(*module, sources_.add(std::string(m.source_path), std::string(m.source_text)), false);
            module->header_imports_std = module->source_imports_std = true;   // `std::` names, from every std module
            if (m.name == "std") program_.std_module = module.get();
            state_[module->name] = State::done;
            program_.modules.emplace(module->name, std::move(module));
        }
    }

    sema::Module* load_module(const std::string& name, const ast::ImportDecl& imp) {
        if (auto it = program_.modules.find(name); it != program_.modules.end()) {
            if (state_[name] == State::visiting) {
                std::string chain;
                bool        in_cycle = false;
                for (const std::string& m : stack_) {
                    in_cycle = in_cycle || m == name;
                    if (in_cycle) {
                        chain += m + " -> ";
                    }
                }
                diags_.report(DiagCode::E3002, imp.span, std::format("import cycle: {}{}", chain, name),
                              "this import closes the cycle")
                    .notes.push_back(Note{"module imports must form an acyclic graph, counting the "
                                          "imports of both .edh and .ed files",
                                          std::nullopt});
                return nullptr;
            }
            return it->second.get();
        }

        fs::path rel;
        for (const ast::PathSegment& seg : imp.path.segments) {
            rel /= seg.name;
        }
        fs::path base = root_;
        for (const std::string& extra : extra_roots_) {   // a dependency's module (spec 25)
            fs::path h = fs::path(root_) / rel;
            h += ".edh";
            fs::path e = fs::path(extra) / rel;
            e += ".edh";
            if (!is_file(h) && is_file(e)) base = extra;
        }
        fs::path header = base / rel;
        header += ".edh";
        fs::path source = base / rel;
        source += ".ed";
        if (!is_file(header)) {
            if (is_file(source)) {
                diags_.report(DiagCode::E3003, imp.path.span,
                              std::format("module '{}' has no header and cannot be imported", name),
                              std::format("found {} but no {}", display_path(source), display_path(header)))
                    .notes.push_back(Note{"a .ed without a .edh is a standalone file, like a program's main",
                                          std::nullopt});
            } else {
                diags_.report(DiagCode::E3001, imp.path.span, std::format("cannot find module '{}'", name),
                              std::format("looked for {}", display_path(header)));
            }
            return nullptr;
        }

        auto module     = std::make_unique<sema::Module>();
        module->name    = name;
        module->project = base == fs::path(root_) ? std::string() : fs::path(base).lexically_normal().generic_string();
        load_file(*module, header, true);
        if (is_file(source)) {
            load_file(*module, source, false);
        }
        sema::Module* raw = module.get();
        program_.modules.emplace(name, std::move(module));
        state_[name] = State::visiting;
        stack_.push_back(name);
        process_imports(*raw);
        stack_.pop_back();
        state_[name] = State::done;
        return raw;
    }

    std::string                   root_;
    std::vector<std::string>      extra_roots_;
    SourceManager&                sources_;
    DiagnosticEngine&             diags_;
    sema::Program                 program_;
    std::map<std::string, State>  state_;
    std::vector<std::string>      stack_;
};

} // namespace

std::string module_name_for(const std::string& path, const std::string& root) {
    fs::path rel = fs::path(path).lexically_normal().lexically_relative(fs::path(root).lexically_normal());
    if (rel.empty() || rel.generic_string().starts_with("..")) {
        rel = fs::path(path).filename();
    }
    rel.replace_extension();
    std::string name;
    for (const fs::path& part : rel) {
        if (part == ".") {
            continue;
        }
        name += (name.empty() ? "" : "::") + part.string();
    }
    return name;
}

sema::Program load_program(const std::string& entry_path, const LoadOptions& options,
                           SourceManager& sources, DiagnosticEngine& diags) {
    return Loader(root_or_default(entry_path, options.root), sources, diags, options.extra_roots).run(entry_path, options.build_script);
}

} // namespace edax
