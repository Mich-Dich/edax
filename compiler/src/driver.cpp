#include "rift/driver.hpp"

#include "rift/ast_printer.hpp"
#include "rift/check.hpp"
#include "rift/compile_time.hpp"
#include "rift/llvm_backend.hpp"
#include "rift/diagnostics.hpp"
#include "rift/lexer.hpp"
#include "rift/modules.hpp"
#include "rift/ownership.hpp"
#include "rift/parser.hpp"
#include "rift/resolve.hpp"
#include "rift/source.hpp"
#include "rift/std_library.hpp"

#include <cstdlib>
#include <functional>
#include <set>
#include <deque>
#include <algorithm>
#include <filesystem>
#include <format>
#include <map>
#include <fstream>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <optional>
#include <ostream>
#include <vector>

extern char** environ;   // POSIX: the process environment, passed on to child processes

#ifndef RIFT_VERSION
#define RIFT_VERSION "0.0.0-dev"
#endif

namespace rift {

namespace {

constexpr std::string_view usage_text =
    "usage: riftc <command> [options] [file]\n"
    "\n"
    "commands:\n"
    "  lex <file>            print the token stream of a .rifts/.rifth file\n"
    "  parse <file>          print the syntax tree of one file\n"
    "  check <file>          check a module and everything it imports\n"
    "  symbols <file>        print the module's merged symbol table (header + source)\n"
    "  typed <file>          print the syntax tree with the type of every expression\n"
    "  build <file>          compile a program to build/<mode>/<name> (next to the file)\n"
    "  build [targets]       build the programs of ./build.rifts (or the targets named)\n"
    "  run <file> [-- args]  build a program in a temporary directory and run it\n"
    "  run <target>          build and run a program of ./build.rifts\n"
    "  test [tests]          build and run the tests of ./build.rifts\n"
    "  emit-llvm <file>      print the LLVM IR the program compiles to\n"
    "  explain [code|id]     describe a diagnostic code or a warning id, or list all codes\n"
    "\n"
    "options:\n"
    "  --diagnostic-format=human|short   diagnostic layout (default: human)\n"
    "  --color=auto|always|never         colored diagnostics (default: auto)\n"
    "  --root=DIR                        project root for imports and build.rifts (default: the file's\n"
    "                                    directory; for build.rifts the current directory)\n"
    "  --stop-after=PHASE                lex, parse, resolve, types or ownership\n"
    "  --mode=MODE                       debug (default), release, release_with_debug_info, or a mode\n"
    "                                    of build.rifts\n"
    "  --warn=ID=off|error|on            switch a warning of a check, e.g. --warn=gfx::deprecated=off\n"
    "  -o FILE, --output=FILE            where 'build' writes the executable\n"
    "  -DNAME=VALUE                      an option of build.rifts\n"
    "  --list                            build: list build.rifts' targets, modes and options\n"
    "  --compile-commands                build: write compile_commands.json for the C/C++ targets\n"
    "  --out-dir=DIR                     where a build.rifts build goes (default: <root>/build)\n"
    "  --bin-dir=DIR                     where a build.rifts build puts its programs (default: <out-dir>/<mode>)\n"
    "  --cc=COMPILER                     linker for build/run (default: $RIFT_CC, else cc)\n"
    "  --all-functions                   generate every function, not only those main reaches (testing)\n"
    "  -h, --help                        show this help\n"
    "  -V, --version                     show the compiler version\n"
    "\n"
    "exit codes: 0 success, 1 errors in input, 2 bad command line, 3 internal error;\n"
    "            'run' exits with the program's own status (128 + signal if it crashed)\n";

enum class Phase { lex, parse, resolve, types, ownership };

struct Options {
    std::string_view command;
    std::string_view input;
    RenderOptions    render;
    bool             color_auto = true;
    std::string      root;
    Phase            stop_after = Phase::ownership;
    std::string      mode       = "debug";
    std::string      output;
    std::string      cc;
    bool             all_functions = false;   // --all-functions (testing the backend)
    std::vector<std::string> program_args;   // `run ... -- args`
    std::map<std::string, std::string> warn;  // --warn=gfx::deprecated=off|error|on (spec 23)
    std::vector<std::string> targets;         // build.rifts targets named on the command line
    std::map<std::string, std::string> defines;   // -Dname=value (spec 25)
    bool             list = false;             // build --list
    bool             compile_commands = false; // build --compile-commands
    bool             project = false;          // the command works on build.rifts
    std::string      out_dir;                  // --out-dir: where a project's build goes (default <root>/build)
    std::string      bin_dir;                  // --bin-dir: where a project's programs go (default <out>/<mode>)
};

constexpr int process_not_found = -1;

/// Runs a program and waits for it. Returns its exit status (128 + signal number if it was
/// killed), or process_not_found. With `captured`, stdout and stderr are collected there.
int run_process(const std::vector<std::string>& cmd, std::string* captured, const std::string& cwd = {}) {
    std::vector<char*> argv;
    for (const std::string& a : cmd) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    int pipe_fds[2] = {-1, -1};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
    if (captured) {
        if (pipe(pipe_fds) != 0) return process_not_found;
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
        posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
    }
    pid_t         pid = 0;
    int           rc  = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (captured) {
        close(pipe_fds[1]);
        if (rc == 0) {
            char    buf[4096];
            ssize_t n = 0;
            while ((n = read(pipe_fds[0], buf, sizeof buf)) > 0) captured->append(buf, static_cast<std::size_t>(n));
        }
        close(pipe_fds[0]);
    }
    if (rc != 0) return process_not_found;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return process_not_found;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

std::filesystem::path make_temp_dir() {
    std::string pattern = (std::filesystem::temp_directory_path() / "riftc-run-XXXXXX").string();
    if (!mkdtemp(pattern.data())) return {};
    return pattern;
}

ExitCode usage_error(DriverStreams& s, std::string_view message) {
    s.err << "riftc: error: " << message << "\nRun 'riftc --help' for usage.\n";
    return ExitCode::usage_error;
}

/// Parses options after the command. Returns an exit code on failure.
std::optional<ExitCode> parse_options(std::span<const std::string_view> args, Options& opts,
                                      DriverStreams& s) {
    bool builds = opts.command == "build" || opts.command == "run" || opts.command == "emit-llvm" || opts.command == "test";
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (arg == "--" && opts.command == "run") {
            for (++i; i < args.size(); ++i) opts.program_args.emplace_back(args[i]);
            break;
        }
        if (arg.starts_with("--warn=")) {
            std::string_view spec = arg.substr(7);
            std::size_t      eq   = spec.rfind('=');
            std::string_view lvl  = eq == std::string_view::npos ? std::string_view{} : spec.substr(eq + 1);
            if (eq == std::string_view::npos || eq == 0 || (lvl != "off" && lvl != "error" && lvl != "on")) {
                return usage_error(s, std::format("'--warn' takes a diagnostic and a level: --warn=gfx::deprecated=off|error|on, not '{}'", spec));
            }
            opts.warn[std::string(spec.substr(0, eq))] = std::string(lvl);
            continue;
        }
        if (builds && arg.starts_with("--mode=")) {
            opts.mode = std::string(arg.substr(7));   // user modes are checked against build.rifts
            if (opts.mode.empty()) return usage_error(s, "'--mode=' needs a mode name");
            continue;
        }
        if (builds && arg.starts_with("-D")) {
            std::string_view spec = arg.substr(2);
            std::size_t      eq   = spec.find('=');
            if (eq == std::string_view::npos || eq == 0) return usage_error(s, std::format("'-D' takes name=value, not '{}'", spec));
            opts.defines[std::string(spec.substr(0, eq))] = std::string(spec.substr(eq + 1));
            continue;
        }
        if (builds && arg.starts_with("--bin-dir=")) {
            opts.bin_dir = std::string(arg.substr(10));
            continue;
        }
        if (builds && arg.starts_with("--out-dir=")) {
            opts.out_dir = std::string(arg.substr(10));
            continue;
        }
        if (opts.command == "build" && arg == "--compile-commands") {
            opts.compile_commands = true;
            continue;
        }
        if (opts.command == "build" && arg == "--list") {
            opts.list = true;
            continue;
        }
        if (builds && (arg == "-o" || arg.starts_with("--output="))) {
            if (arg == "-o") {
                if (i + 1 >= args.size()) return usage_error(s, "'-o' needs a file name");
                opts.output = std::string(args[++i]);
            } else {
                opts.output = std::string(arg.substr(9));
            }
            continue;
        }
        if (builds && arg.starts_with("--cc=")) {
            opts.cc = std::string(arg.substr(5));
            continue;
        }
        if (builds && arg == "--all-functions") {
            opts.all_functions = true;
            continue;
        }
        if (arg == "--diagnostic-format=human") {
            opts.render.format = DiagnosticFormat::human;
        } else if (arg == "--diagnostic-format=short") {
            opts.render.format = DiagnosticFormat::short_form;
        } else if (arg == "--color=always") {
            opts.render.color = true, opts.color_auto = false;
        } else if (arg == "--color=never") {
            opts.render.color = false, opts.color_auto = false;
        } else if (arg == "--color=auto") {
            opts.color_auto = true;
        } else if (arg.starts_with("--root=")) {
            opts.root = std::string(arg.substr(7));
        } else if (arg == "--stop-after=lex") {
            opts.stop_after = Phase::lex;
        } else if (arg == "--stop-after=parse") {
            opts.stop_after = Phase::parse;
        } else if (arg == "--stop-after=resolve") {
            opts.stop_after = Phase::resolve;
        } else if (arg == "--stop-after=types") {
            opts.stop_after = Phase::types;
        } else if (arg == "--stop-after=ownership") {
            opts.stop_after = Phase::ownership;
        } else if (arg.starts_with("-") && arg != "-") {
            return usage_error(s, std::format("unknown option '{}'", arg));
        } else if (opts.input.empty() && (arg.ends_with(".rifts") || arg.ends_with(".rifth") || !builds)) {
            opts.input = arg;
        } else if (builds && opts.input.empty() && opts.command != "emit-llvm") {
            opts.targets.emplace_back(arg);   // a target of build.rifts
        } else {
            return usage_error(s, std::format("unexpected extra argument '{}'", arg));
        }
    }
    // `riftc build`, `riftc run app`, `riftc test`: the project's build.rifts (spec 25).
    opts.project = builds && opts.command != "emit-llvm" && opts.input.empty();
    if (opts.project && opts.command == "run" && opts.targets.size() != 1) {
        return usage_error(s, "'run' takes a program: a .rifts file, or one target of build.rifts");
    }
    if (!opts.project && (opts.list || opts.compile_commands || !opts.defines.empty())) {
        return usage_error(s, "'--list', '--compile-commands' and '-D' work with build.rifts: run them without a file");
    }
    if (!opts.project && opts.mode != "debug" && opts.mode != "release" && opts.mode != "release_with_debug_info") {
        return usage_error(s, std::format("unknown build mode '{}' (debug, release, release_with_debug_info; "
                                          "other modes are defined in build.rifts)", opts.mode));
    }
    if (opts.input.empty() && !opts.project) {
        return usage_error(s, std::format("'{}' needs an input file", opts.command));
    }
    if (opts.color_auto) {
        opts.render.color = s.err_is_terminal;
    }
    return std::nullopt;
}

std::string escape_for_display(std::string_view text) {
    std::string out;
    for (char c : text) {
        switch (c) {
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        case '\0': out += "\\0"; break;
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        default: out.push_back(c);
        }
    }
    return out;
}

void dump_tokens(std::ostream& out, const SourceFile& file, const std::vector<Token>& tokens) {
    for (const Token& t : tokens) {
        LineCol     lc  = file.line_col(t.span.begin);
        std::string pos = std::format("{}:{}", lc.line, lc.column);
        std::string line = std::format("{:<8}{:<24}{}", pos, token_kind_name(t.kind), t.text);
        switch (t.kind) {
        case TokenKind::int_literal:
            line += std::format("  value={}", to_string(t.int_value));
            break;
        case TokenKind::char_literal:
        case TokenKind::string_literal:
        case TokenKind::raw_string_literal:
        case TokenKind::format_string_literal:
        case TokenKind::doc_comment:
            line += std::format("  value=\"{}\"", escape_for_display(t.value));
            break;
        default:
            break;
        }
        if (!t.suffix.empty()) {
            line += std::format("  suffix={}", t.suffix);
        }
        while (!line.empty() && line.back() == ' ') {
            line.pop_back();
        }
        out << line << '\n';
    }
}

/// The backend: LLVM IR text (emit-llvm), then an executable: the IR is compiled to an object file
/// for the host and linked with the system linker (--cc, RIFT_CC or cc).
ExitCode run_backend(const Options& opts, sema::Program& program, SourceManager& sources, DiagnosticEngine& diags,
                          DriverStreams& s) {
    auto setting = [&](const char* name) {
        auto it = program.mode_settings.find(name);
        return it == program.mode_settings.end() ? std::string() : it->second;
    };
    LlvmOptions llvm_options{opts.all_functions, setting("panic") == "trap",
                             setting("debug_info") == "full" || setting("debug_info") == "line_tables"};
    std::string ir = generate_llvm_ir(program, sources, diags, llvm_options);
    diags.sort_by_location();
    render_all(s.err, diags, sources, opts.render);
    if (diags.has_errors()) return ExitCode::compile_error;
    if (opts.command == "emit-llvm") {
        s.out << ir;
        return ExitCode::success;
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path        input(opts.input);
    fs::path        temp_dir;
    fs::path        exe;
    if (opts.command == "run") {
        temp_dir = make_temp_dir();
        if (temp_dir.empty()) {
            s.err << "riftc: error[E0004]: cannot create a temporary directory\n";
            return ExitCode::compile_error;
        }
        exe = temp_dir / input.stem();
    } else if (opts.output.empty()) {
        fs::path root = opts.root.empty() ? input.parent_path() : fs::path(opts.root);
        exe           = root / "build" / opts.mode / input.stem();
    } else {
        exe = opts.output;
    }
    if (!exe.parent_path().empty()) fs::create_directories(exe.parent_path(), ec);

    std::string linker = opts.cc;
    if (linker.empty()) {
        const char* env = std::getenv("RIFT_CC");
        linker          = env && *env ? env : "cc";
    }
    std::string error;
    CodegenOptions codegen;
    codegen.optimization = setting("optimization").empty() ? (opts.mode == "debug" ? "none" : "speed") : setting("optimization");
    codegen.debug_info   = setting("debug_info") == "full" || setting("debug_info") == "line_tables";
    if (!build_executable(ir, exe.string(), linker, error, codegen)) {
        s.err << std::format("riftc: internal compiler error: {}\n", error);
        if (!temp_dir.empty()) fs::remove_all(temp_dir, ec);
        return ExitCode::internal_error;
    }
    if (opts.command == "build") {
        s.out << std::format("built '{}' ({})\n", exe.string(), opts.mode);
        return ExitCode::success;
    }
    s.out.flush();
    s.err.flush();
    std::vector<std::string> run_cmd = {exe.string()};
    run_cmd.insert(run_cmd.end(), opts.program_args.begin(), opts.program_args.end());
    int program_status = run_process(run_cmd, nullptr);
    fs::remove_all(temp_dir, ec);
    return static_cast<ExitCode>(program_status);
}

ExitCode run_frontend(const Options& opts, DriverStreams& s) {
    std::string input(opts.input);
    if (!input.ends_with(".rifts") && !input.ends_with(".rifth")) {
        s.err << std::format("riftc: error[E0002]: '{}': {}\n", input,
                             code_title(DiagCode::E0002));
        return ExitCode::compile_error;
    }

    SourceManager    sources;
    DiagnosticEngine diags;

    if (opts.command == "lex" || opts.command == "parse" || opts.stop_after == Phase::lex) {
        // Single-file commands: no imports are followed.
        std::string           reason;
        std::optional<FileId> id = sources.load(input, reason);
        if (!id) {
            s.err << std::format("riftc: error[E0001]: cannot read '{}': {}\n", input, reason);
            return ExitCode::compile_error;
        }
        const SourceFile&  file   = sources.file(*id);
        std::vector<Token> tokens = tokenize(file, *id, diags);
        if (opts.command == "lex") {
            dump_tokens(s.out, file, tokens);
        } else if (!diags.has_errors() && opts.stop_after != Phase::lex) {
            ast::File tree = parse(file, *id, tokens, diags);
            ast::print(s.out, tree);
        }
        render_all(s.err, diags, sources, opts.render);
        return diags.has_errors() ? ExitCode::compile_error : ExitCode::success;
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(input, ec)) {
        s.err << std::format("riftc: error[E0001]: cannot read '{}': cannot open file\n", input);
        return ExitCode::compile_error;
    }
    // Each phase only runs on input the previous phase accepted, so errors don't cascade.
    sema::Program program = load_program(input, [&] { LoadOptions o; o.root = opts.root; return o; }(), sources, diags);
    bool resolved = false;
    bool typed    = false;
    if (!diags.has_errors() && opts.stop_after >= Phase::resolve) {
        resolve_program(program, diags);
        resolved = true;
    }
    program.build_mode = opts.mode;
    program.warn_levels = opts.warn;
    if (!diags.has_errors() && opts.stop_after >= Phase::types) {
        check_types(program, sources, diags);
        typed = true;
    }
    if (!diags.has_errors() && opts.stop_after >= Phase::ownership) {
        check_ownership(program, diags);
    }
    if ((opts.command == "build" || opts.command == "run" || opts.command == "emit-llvm") && !diags.has_errors()) {
        return run_backend(opts, program, sources, diags, s);
    }
    if (opts.command == "symbols" && resolved && program.entry) {
        print_symbols(s.out, *program.entry, sources);
    }
    if (opts.command == "typed" && typed && program.entry) {
        if (program.entry->header) ast::print(s.out, program.entry->header->ast, true);
        if (program.entry->source) ast::print(s.out, program.entry->source->ast, true);
    }
    diags.sort_by_location();
    render_all(s.err, diags, sources, opts.render);
    return diags.has_errors() ? ExitCode::compile_error : ExitCode::success;
}

// --- build.rifts (spec 25) ------------------------------------------------------------------

/// The mode's settings that relax a rule of the built-in modes, for the note every such build
/// prints (spec 24).
std::string relaxed_rules(const std::map<std::string, std::string>& ms) {
    auto get = [&](const char* k) {
        auto it = ms.find(k);
        return it == ms.end() ? std::string() : it->second;
    };
    std::vector<std::string> parts;
    if (get("overflow") == "wrap") parts.push_back("overflow wraps");
    if (get("shifts") == "mask") parts.push_back("shift counts are masked");
    if (get("panic") == "trap") parts.push_back("panics trap");
    if (get("bounds") == "unchecked") parts.push_back("bounds checks off");
    if (get("run_time_checks") == "none") parts.push_back("run_time checks off");
    if (get("run_time_checks") == "gated_only") parts.push_back("only mode-gated run_time checks");
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) out += (i ? ", " : "") + parts[i];
    if (!out.empty() && get("allow_undefined_behavior") == "true") out += " - undefined behaviour is allowed";
    return out;
}

/// Compiles and links one Rift target of build.rifts. Returns the executable's path, or empty.
std::string build_rift_target(const Options& opts, const std::filesystem::path& root, const std::filesystem::path& out,
                              const ct::BuildTarget& t, const std::string& mode, const std::vector<std::string>& libraries,
                              const std::vector<std::string>& extra_roots, DriverStreams& s, bool& failed) {
    namespace fs = std::filesystem;
    fs::path base  = t.base.empty() ? root : fs::path(t.base);
    fs::path entry = (base / t.root).lexically_normal();
    std::error_code ec;
    if (!fs::is_regular_file(entry, ec)) {
        s.err << std::format("riftc: error[E0001]: target '{}': cannot read '{}'\n", t.name, entry.string());
        failed = true;
        return {};
    }
    SourceManager    sources;
    DiagnosticEngine diags;
    LoadOptions      load;
    load.root         = base.lexically_normal().string();
    load.build_script = (base / "build.rifts").lexically_normal().string();
    load.extra_roots  = extra_roots;
    sema::Program program = load_program(entry.string(), load, sources, diags);
    if (!diags.has_errors()) resolve_program(program, diags);
    program.build_mode     = mode;
    program.warn_levels    = opts.warn;
    program.build_settings = t.settings;
    if (!diags.has_errors()) check_types(program, sources, diags);
    if (!diags.has_errors()) check_ownership(program, diags);
    std::string ir;
    auto setting = [&](const char* name) {
        auto it = program.mode_settings.find(name);
        return it == program.mode_settings.end() ? std::string() : it->second;
    };
    if (!diags.has_errors()) ir = generate_llvm_ir(program, sources, diags, LlvmOptions{false, setting("panic") == "trap",
                                                                         setting("debug_info") == "full" || setting("debug_info") == "line_tables"});
    diags.sort_by_location();
    render_all(s.err, diags, sources, opts.render);
    if (diags.has_errors()) {
        failed = true;
        return {};
    }
    fs::path exe = opts.bin_dir.empty() ? out / mode / t.name : fs::path(opts.bin_dir) / t.name;
    fs::create_directories(exe.parent_path(), ec);
    std::string linker = opts.cc;
    if (linker.empty()) {
        const char* env = std::getenv("RIFT_CC");
        linker          = env && *env ? env : "cc";
    }
    CodegenOptions codegen;
    codegen.optimization = setting("optimization").empty() ? "none" : setting("optimization");
    codegen.debug_info   = setting("debug_info") == "full" || setting("debug_info") == "line_tables";
    std::string error;
    if (!build_executable(ir, exe.string(), linker, error, codegen, libraries)) {
        s.err << std::format("riftc: error: target '{}': {}\n", t.name, error);
        failed = true;
        return {};
    }
    return exe.string();
}

/// The compile commands of the C targets, for `--compile-commands` (clangd).
struct CompileCommand {
    std::string directory;
    std::string file;
    std::vector<std::string> arguments;
};

std::string json_string(std::string_view text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

/// Compiles a C/C++ target to `<out>/c/lib<name>.a`. Returns the archive, or empty.
std::string build_c_target(const std::filesystem::path& root, const std::filesystem::path& out, const ct::BuildGraph& g,
                           const ct::BuildTarget& t, const std::string& optimization, DriverStreams& s, bool& failed,
                           std::vector<CompileCommand>* commands) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path base = t.base.empty() ? root : fs::path(t.base);
    fs::path dir  = out / "c" / t.name;
    fs::create_directories(dir, ec);
    bool        cpp      = t.language == "cpp";
    std::string compiler = cpp ? "c++" : "cc";
    std::vector<std::string> objects;
    for (std::size_t i = 0; i < t.sources.size(); ++i) {
        fs::path src = fs::path(t.sources[i]).is_absolute() ? fs::path(t.sources[i]) : base / t.sources[i];
        fs::path obj = dir / (src.stem().string() + "." + std::to_string(i) + ".o");
        std::vector<std::string> cmd = {compiler, "-c", src.string(), "-o", obj.string(), "-fPIC"};
        if (!t.standard.empty()) {
            std::string std_ = t.standard;
            if (std_.starts_with("cpp")) std_ = "c++" + std_.substr(3);
            cmd.push_back("-std=" + std_);
        }
        cmd.push_back(optimization == "none" ? "-O0" : optimization == "size" ? "-Os" : "-O2");
        auto add_dirs = [&](const ct::BuildTarget& from, bool only_shared) {
            fs::path from_base = from.base.empty() ? root : fs::path(from.base);
            for (const auto& [d, shared] : from.include_dirs) {
                if (!only_shared || shared) cmd.push_back("-I" + (fs::path(d).is_absolute() ? fs::path(d) : from_base / d).string());
            }
        };
        add_dirs(t, false);
        for (std::size_t l : t.links) add_dirs(g.targets[l], true);
        for (const auto& [name, value] : t.defines) cmd.push_back("-D" + name + "=" + value);
        if (commands) commands->push_back(CompileCommand{base.string(), src.string(), cmd});
        // Up to date: the object is newer than its source (headers are not tracked yet).
        if (fs::exists(obj, ec) && fs::last_write_time(obj, ec) >= fs::last_write_time(src, ec)) {
            objects.push_back(obj.string());
            continue;
        }
        std::string output;
        int status = run_process(cmd, &output);
        if (status != 0) {
            s.err << output << std::format("riftc: error: target '{}': compiling '{}' failed\n", t.name, t.sources[i]);
            failed = true;
            return {};
        }
        objects.push_back(obj.string());
    }
    fs::path archive = out / "c" / ("lib" + t.name + ".a");
    fs::remove(archive, ec);
    std::vector<std::string> ar = {"ar", "rcs", archive.string()};
    ar.insert(ar.end(), objects.begin(), objects.end());
    std::string output;
    if (run_process(ar, &output) != 0) {
        s.err << output << std::format("riftc: error: target '{}': 'ar' failed\n", t.name);
        failed = true;
        return {};
    }
    return archive.string();
}

/// Runs a step unless its outputs exist and are newer than its inputs (spec 25).
bool run_step(const std::filesystem::path& root, const ct::BuildStep& st, DriverStreams& s) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto at = [&](const std::string& p) { return fs::path(p).is_absolute() ? fs::path(p) : root / p; };
    bool stale = st.outputs.empty();
    fs::file_time_type oldest = fs::file_time_type::max();
    for (const std::string& o : st.outputs) {
        if (!fs::exists(at(o), ec)) stale = true;
        else oldest = std::min(oldest, fs::last_write_time(at(o), ec));
    }
    for (const std::string& i : st.inputs) {
        if (!fs::exists(at(i), ec)) {
            s.err << std::format("riftc: error: step '{}': its input '{}' does not exist\n", st.name, i);
            return false;
        }
        if (fs::last_write_time(at(i), ec) > oldest) stale = true;
    }
    if (!stale) return true;
    for (const std::string& o : st.outputs) fs::create_directories(at(o).parent_path(), ec);
    std::vector<std::string> cmd = {st.program};
    cmd.insert(cmd.end(), st.arguments.begin(), st.arguments.end());
    std::string output;
    int status = run_process(cmd, &output, root.string());
    if (status != 0) {
        s.err << output << std::format("riftc: error: step '{}' failed (exit status {})\n", st.name, status);
        return false;
    }
    for (const std::string& o : st.outputs) {
        if (!fs::exists(at(o), ec)) {
            s.err << std::format("riftc: error: step '{}' did not write its declared output '{}'\n", st.name, o);
            return false;
        }
    }
    s.out << std::format("ran step '{}'\n", st.name);
    return true;
}

/// One evaluated build.rifts: its graph and the program that owns its symbols.
struct EvaluatedBuild {
    std::filesystem::path root;
    ct::BuildGraph        graph;
    SourceManager         sources;
    sema::Program         program;
};

/// Runs `<root>/build.rifts`' build(g) with the compile-time interpreter (spec 25).
bool evaluate_build(EvaluatedBuild& b, const Options& opts, const std::filesystem::path& out, DriverStreams& s,
                    std::map<std::string, std::string>& lock, std::vector<std::string>& lock_errors) {
    namespace fs = std::filesystem;
    b.graph.given   = opts.defines;
    std::error_code abs_ec;
    b.graph.root    = fs::absolute(b.root, abs_ec).lexically_normal().string();
    b.graph.out_dir = out.string();
    b.graph.fetch   = [&, out](const ct::BuildDependency& d, std::string& error) -> std::string {
        std::error_code ec;
        fs::path dir;
        std::string key, hash;
        if (d.kind == "local") {
            dir = fs::weakly_canonical(fs::path(d.source).is_absolute() ? fs::path(d.source) : b.root / d.source, ec);
            if (!fs::is_directory(dir, ec)) {
                error = "no such directory";
                return {};
            }
            return dir.string();   // a directory under the user's control: not locked
        } else {
            std::string repo = fs::path(d.source).stem().string();
            dir = out / "deps" / (repo + "-" + d.commit.substr(0, 12));
            if (!fs::is_directory(dir, ec)) {
                std::string output;
                fs::create_directories(dir.parent_path(), ec);
                if (run_process({"git", "clone", "--quiet", d.source, dir.string()}, &output) != 0 ||
                    run_process({"git", "-C", dir.string(), "checkout", "--quiet", d.commit}, &output) != 0) {
                    fs::remove_all(dir, ec);
                    error = "git failed: " + output;
                    return {};
                }
            }
            key = "git " + d.source + " " + d.commit;
            std::string tree;
            run_process({"git", "-C", dir.string(), "rev-parse", "HEAD^{tree}"}, &tree);
            while (!tree.empty() && (tree.back() == '\n' || tree.back() == '\r')) tree.pop_back();
            hash = tree;
        }
        auto it = lock.find(key);
        if (it != lock.end() && it->second != hash) {
            lock_errors.push_back(std::format("the dependency '{}' changed: build.lock has {}, found {} (delete its line to accept)",
                                              d.source, it->second, hash));
        }
        lock[key] = hash;
        return dir.string();
    };
    DiagnosticEngine diags;
    fs::path script = (b.root / "build.rifts").lexically_normal();
    b.program = load_program(script.string(), [&] { LoadOptions o; o.root = b.root.lexically_normal().string(); return o; }(), b.sources, diags);
    if (!diags.has_errors()) resolve_program(b.program, diags);
    b.program.build_mode  = opts.mode;
    b.program.warn_levels = opts.warn;
    b.program.build_graph = &b.graph;
    if (!diags.has_errors()) check_types(b.program, b.sources, diags);
    diags.sort_by_location();
    render_all(s.err, diags, b.sources, opts.render);
    return !diags.has_errors();
}

ExitCode run_project(const Options& opts, DriverStreams& s) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path root = opts.root.empty() ? fs::path(".") : fs::path(opts.root);   // as written: diagnostics show it
    if (!fs::is_regular_file(root / "build.rifts", ec)) {
        return usage_error(s, std::format("no build.rifts in '{}': name a .rifts file, or write a build.rifts (spec 25)",
                                          opts.root.empty() ? root.string() : opts.root));
    }
    fs::path out = opts.out_dir.empty() ? root / "build" : fs::absolute(opts.out_dir, ec);

    // build.lock: each dependency's content hash (spec 25).
    std::map<std::string, std::string> lock;
    {
        std::ifstream in(root / "build.lock");
        std::string   line;
        while (std::getline(in, line)) {
            std::size_t at = line.rfind(' ');
            if (at != std::string::npos && !line.starts_with("#")) lock[line.substr(0, at)] = line.substr(at + 1);
        }
    }
    std::map<std::string, std::string> lock_before = lock;
    std::vector<std::string> lock_errors;

    // 1. Run build.rifts' build(g) with the compile-time interpreter.
    std::deque<EvaluatedBuild> builds;
    EvaluatedBuild& main_build = builds.emplace_back();
    main_build.root = root;
    if (!evaluate_build(main_build, opts, out, s, lock, lock_errors)) return ExitCode::compile_error;
    ct::BuildGraph& graph = main_build.graph;
    sema::Program&  program = main_build.program;

    // Targets of dependencies: their own build.rifts, evaluated in the same mode; their
    // targets join this graph (with paths relative to their root).
    for (std::size_t d = 0; d < graph.dependencies.size(); ++d) {
        bool used = false;
        for (const ct::BuildTarget& t : graph.targets) used = used || (t.kind == ct::BuildTarget::Kind::foreign && t.dependency == d + 1);
        if (!used) continue;
        fs::path dep_root = graph.dependencies[d].dir;
        if (!fs::is_regular_file(dep_root / "build.rifts", ec)) {
            s.err << std::format("riftc: error: the dependency '{}' has no build.rifts; describe its sources in this build.rifts "
                                 "(g.glob_in) instead of g.target_of\n", graph.dependencies[d].source);
            return ExitCode::compile_error;
        }
        EvaluatedBuild& dep = builds.emplace_back();
        dep.root = dep_root;
        Options dep_opts = opts;
        dep_opts.defines.clear();
        if (!evaluate_build(dep, dep_opts, out / "deps-build" / dep_root.filename(), s, lock, lock_errors)) return ExitCode::compile_error;
        std::size_t offset = graph.targets.size();
        for (ct::BuildTarget t : dep.graph.targets) {
            for (std::size_t& l : t.links) l += offset;
            t.steps.clear();
            if (t.base.empty()) t.base = dep_root.string();
            t.name = graph.dependencies[d].source + ":" + t.name;
            graph.targets.push_back(std::move(t));
        }
        for (ct::BuildTarget& t : graph.targets) {
            if (t.kind != ct::BuildTarget::Kind::foreign || t.dependency != d + 1 || !t.links.empty()) continue;
            for (std::size_t i = 0; i < dep.graph.targets.size(); ++i) {
                if (dep.graph.targets[i].name == t.name) t.links.push_back(offset + i);
            }
            if (t.links.empty()) {
                s.err << std::format("riftc: error: the dependency '{}' has no target '{}'\n", graph.dependencies[d].source, t.name);
                return ExitCode::compile_error;
            }
        }
    }
    if (!lock_errors.empty()) {
        for (const std::string& e : lock_errors) s.err << "riftc: error: " << e << "\n";
        return ExitCode::compile_error;
    }
    if (lock != lock_before) {
        std::ofstream lock_out(root / "build.lock");
        lock_out << "# Written by riftc (spec 25): the content hash of each dependency. Keep it in version control.\n";
        for (const auto& [key, hash] : lock) lock_out << key << " " << hash << "\n";
    }

    // 2. The mode: a built-in one, or a class build.rifts added with g.add_mode.
    bool builtin = opts.mode == "debug" || opts.mode == "release" || opts.mode == "release_with_debug_info";
    bool added   = false;
    std::string names = "debug, release, release_with_debug_info";
    for (const sema::Symbol* m : graph.modes) {
        added = added || m->name == opts.mode;
        names += ", " + m->name;
    }
    if (!builtin && !added) return usage_error(s, std::format("unknown build mode '{}' ({})", opts.mode, names));
    for (const auto& [name, value] : opts.defines) {
        bool known = false;
        for (const ct::BuildOption& o : graph.options) known = known || o.name == name;
        if (!known) return usage_error(s, std::format("build.rifts has no option '{}' (riftc build --list)", name));
    }

    auto kind_name = [](ct::BuildTarget::Kind k) {
        switch (k) {
        case ct::BuildTarget::Kind::executable: return "program";
        case ct::BuildTarget::Kind::library: return "library";
        case ct::BuildTarget::Kind::test: return "test";
        case ct::BuildTarget::Kind::c_library: return "C library";
        case ct::BuildTarget::Kind::system_library: return "system library";
        case ct::BuildTarget::Kind::foreign: return "target of a dependency";
        }
        return "";
    };
    if (opts.list) {
        s.out << "targets:\n";
        for (const ct::BuildTarget& t : graph.targets) {
            if (!t.base.empty()) continue;
            s.out << std::format("  {:<20} {}{}\n", t.name, kind_name(t.kind), t.root.empty() ? "" : " (" + t.root + ")");
        }
        s.out << "modes:\n  " << names << "\n";
        if (!graph.options.empty()) s.out << "options:\n";
        for (const ct::BuildOption& o : graph.options) {
            s.out << std::format("  -D{}=<{}>  {} (now {})\n", o.name, o.type, o.help, o.value);
        }
        return ExitCode::success;
    }
    std::string relaxed = relaxed_rules(program.mode_settings);
    if (!relaxed.empty()) {
        const sema::Symbol* cls  = program.build_mode_class;
        const sema::Symbol* base = cls ? cls->base_class() : nullptr;
        s.err << std::format("note: building in mode '{}'{}: {}\n", opts.mode, base ? " (base: " + base->name + ")" : "", relaxed);
    }

    // 3. The targets to build: those named, else every program (build) or test (test).
    std::vector<std::size_t> selected;
    for (const std::string& name : opts.targets) {
        bool found = false;
        for (std::size_t i = 0; i < graph.targets.size(); ++i) {
            if (graph.targets[i].name == name) selected.push_back(i), found = true;
        }
        if (!found) return usage_error(s, std::format("build.rifts has no target '{}' (riftc build --list)", name));
    }
    if (opts.targets.empty()) {
        for (std::size_t i = 0; i < graph.targets.size(); ++i) {
            ct::BuildTarget::Kind k = graph.targets[i].kind;
            if (!graph.targets[i].base.empty()) continue;
            bool want = opts.command == "test" ? k == ct::BuildTarget::Kind::test
                      : opts.compile_commands ? k == ct::BuildTarget::Kind::c_library
                                              : k == ct::BuildTarget::Kind::executable;
            if (want) selected.push_back(i);
        }
    }
    std::string optimization = program.mode_settings["optimization"];
    std::vector<CompileCommand> commands;
    std::map<std::size_t, std::string> archives;
    std::set<std::size_t> steps_done;
    bool failed = false;
    // What a target links, directly or through other targets: archives, -l flags, and the
    // roots of dependencies whose Rift modules it imports.
    std::function<void(std::size_t, std::vector<std::string>&, std::vector<std::string>&, std::set<std::size_t>&)> collect =
        [&](std::size_t i, std::vector<std::string>& libs, std::vector<std::string>& roots, std::set<std::size_t>& seen) {
            for (std::size_t l : graph.targets[i].links) {
                if (!seen.insert(l).second) continue;
                const ct::BuildTarget& used = graph.targets[l];
                if (used.kind == ct::BuildTarget::Kind::c_library) {
                    if (!archives.count(l)) archives[l] = build_c_target(root, out, graph, used, optimization, s, failed, &commands);
                    if (!archives[l].empty()) libs.push_back(archives[l]);
                } else if (used.kind == ct::BuildTarget::Kind::system_library) {
                    if (!used.library_root.empty()) libs.push_back("-L" + (fs::path(used.library_root) / "lib").string());
                    libs.push_back("-l" + used.name);
                } else if (!used.base.empty()) {
                    if (std::find(roots.begin(), roots.end(), used.base) == roots.end()) roots.push_back(used.base);
                }
                collect(l, libs, roots, seen);
            }
        };
    auto run_steps = [&](std::size_t i) {
        for (std::size_t st : graph.targets[i].steps) {
            if (!steps_done.insert(st).second) continue;
            if (!run_step(root, graph.steps[st], s)) failed = true;
        }
    };
    int tests_failed = 0;
    for (std::size_t i : selected) {
        const ct::BuildTarget& t = graph.targets[i];
        run_steps(i);
        if (failed) return ExitCode::compile_error;
        if (t.kind == ct::BuildTarget::Kind::c_library) {
            if (!archives.count(i)) archives[i] = build_c_target(root, out, graph, t, optimization, s, failed, &commands);
            if (!archives[i].empty() && opts.command == "build" && !opts.compile_commands) {
                s.out << std::format("built '{}' ({})\n", archives[i], opts.mode);
            }
            continue;
        }
        if (t.kind != ct::BuildTarget::Kind::executable && t.kind != ct::BuildTarget::Kind::test) {
            if (opts.command == "run") return usage_error(s, std::format("'{}' is a {}, not a program", t.name, kind_name(t.kind)));
            continue;   // a Rift library is compiled into the programs that import it
        }
        std::vector<std::string> libs, roots;
        std::set<std::size_t>    seen;
        collect(i, libs, roots, seen);
        if (failed) return ExitCode::compile_error;
        if (opts.compile_commands) continue;
        std::string mode = t.mode ? t.mode->name : opts.mode;
        bool        target_failed = false;
        std::string exe = build_rift_target(opts, root, out, t, mode, libs, roots, s, target_failed);
        if (target_failed) return ExitCode::compile_error;
        if (opts.command == "build") {
            s.out << std::format("built '{}' ({})\n", exe, mode);
        } else if (opts.command == "run") {
            s.out.flush();
            s.err.flush();
            std::vector<std::string> cmd = {exe};
            cmd.insert(cmd.end(), opts.program_args.begin(), opts.program_args.end());
            return static_cast<ExitCode>(run_process(cmd, nullptr));
        } else {   // test
            std::string output;
            int status = run_process({exe}, &output);
            s.out << std::format("{} {}\n", status == 0 ? "PASS" : "FAIL", t.name);
            if (status != 0) {
                s.out << output;
                ++tests_failed;
            }
        }
    }
    if (failed) return ExitCode::compile_error;
    if (opts.compile_commands) {
        fs::create_directories(out, ec);
        std::ofstream json(out / "compile_commands.json");
        json << "[\n";
        for (std::size_t i = 0; i < commands.size(); ++i) {
            json << "  {\"directory\": " << json_string(commands[i].directory) << ", \"file\": " << json_string(commands[i].file)
                 << ", \"arguments\": [";
            for (std::size_t a = 0; a < commands[i].arguments.size(); ++a) json << (a ? ", " : "") << json_string(commands[i].arguments[a]);
            json << "]}" << (i + 1 < commands.size() ? "," : "") << "\n";
        }
        json << "]\n";
        s.out << std::format("wrote '{}' ({} files)\n", (out / "compile_commands.json").string(), commands.size());
    }
    if (opts.command == "test") {
        std::size_t total = selected.size();
        s.out << std::format("{} of {} tests passed\n", total - static_cast<std::size_t>(tests_failed), total);
        return tests_failed == 0 ? ExitCode::success : ExitCode::compile_error;
    }
    return ExitCode::success;
}

/// The doc comment of `compile_time std::diagnostic <name>` in a module's text, or nothing.
std::optional<std::string> diagnostic_doc(std::string_view text, std::string_view name) {
    std::vector<std::string_view> lines;
    for (std::size_t at = 0; at <= text.size();) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        lines.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    auto trim = [](std::string_view l) {
        while (!l.empty() && (l.front() == ' ' || l.front() == '\t')) l.remove_prefix(1);
        while (!l.empty() && (l.back() == ' ' || l.back() == '\t' || l.back() == '\r')) l.remove_suffix(1);
        return l;
    };
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string_view l = trim(lines[i]);
        if (!l.starts_with("compile_time") || l.find("std::diagnostic") == std::string_view::npos) continue;
        std::size_t after = l.find("std::diagnostic") + std::string_view("std::diagnostic").size();
        std::string_view rest = trim(l.substr(after));
        if (!rest.starts_with(name)) continue;
        std::string_view tail = trim(rest.substr(name.size()));
        if (!tail.starts_with("=") && !tail.starts_with(";")) continue;
        std::size_t first = i;
        while (first > 0 && trim(lines[first - 1]).starts_with("///")) --first;
        std::string doc;
        for (std::size_t k = first; k < i; ++k) {
            std::string_view d = trim(lines[k]).substr(3);
            if (!d.empty() && d.front() == ' ') d.remove_prefix(1);
            doc += std::string(d) + "\n";
        }
        return doc.empty() ? std::string("(no doc comment)\n") : doc;
    }
    return std::nullopt;
}

/// `riftc explain gfx::deprecated`: the doc comment of a declared warning id (spec 23). The std
/// library's ids come from riftc itself; others from `gfx.rifth` (or `.rifts`) in the current
/// directory, the project root.
ExitCode explain_id(std::string_view id, DriverStreams& s) {
    std::size_t      split  = id.rfind("::");
    std::string_view module = id.substr(0, split);
    std::string_view name   = id.substr(split + 2);
    std::optional<std::string> doc;
    for (const StdModule& m : std_modules()) {
        if (m.name != module) continue;
        doc = diagnostic_doc(m.header_text, name);
        if (!doc) doc = diagnostic_doc(m.source_text, name);
    }
    if (!doc && module != "std" && !module.starts_with("std::")) {
        std::string path(module);
        for (std::size_t at = path.find("::"); at != std::string::npos; at = path.find("::", at)) path.replace(at, 2, "/");
        for (const char* ext : {".rifth", ".rifts"}) {
            std::ifstream in(path + ext);
            if (!in) continue;
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            doc = diagnostic_doc(text, name);
            if (doc) break;
        }
    }
    if (!doc) return usage_error(s, std::format("unknown diagnostic id '{}' (no 'compile_time std::diagnostic {}' in module '{}')",
                                                id, name, module));
    s.out << std::format("{} (warning id)\n{}", id, *doc);
    return ExitCode::success;
}

ExitCode run_explain(std::span<const std::string_view> args, DriverStreams& s) {
    if (args.empty()) {
#define RIFT_DIAG(code, severity, title) \
    s.out << std::format("{}  {:<8} {}\n", #code, #severity, title);
#include "rift/diagnostic_codes.def"
#undef RIFT_DIAG
        return ExitCode::success;
    }
    if (args.size() > 1) {
        return usage_error(s, "'explain' takes at most one diagnostic code");
    }
    if (args[0].find("::") != std::string_view::npos) return explain_id(args[0], s);
    std::optional<DiagCode> code = code_from_name(args[0]);
    if (!code) {
        return usage_error(s, std::format("unknown diagnostic code '{}'", args[0]));
    }
    s.out << std::format("{} ({}): {}\n", code_name(*code), to_string(code_severity(*code)),
                         code_title(*code));
    return ExitCode::success;
}

} // namespace

std::string version_string() { return std::string("riftc ") + RIFT_VERSION; }

ExitCode run_driver(std::span<const std::string_view> args, DriverStreams streams) {
    if (args.empty()) {
        streams.err << usage_text;
        return ExitCode::usage_error;
    }
    std::string_view command = args[0];
    if (command == "-h" || command == "--help" || command == "help") {
        streams.out << usage_text;
        return ExitCode::success;
    }
    if (command == "-V" || command == "--version") {
        streams.out << version_string() << '\n';
        return ExitCode::success;
    }
    if (command == "explain") {
        return run_explain(args.subspan(1), streams);
    }
    if (command == "lex" || command == "parse" || command == "check" || command == "symbols" || command == "typed" ||
        command == "build" || command == "run" || command == "emit-c" || command == "emit-llvm" || command == "test") {
        Options opts;
        opts.command = command;
        if (std::optional<ExitCode> failed = parse_options(args.subspan(1), opts, streams)) {
            return *failed;
        }
        if (opts.project) return run_project(opts, streams);
        if (command == "test") return usage_error(streams, "'test' runs the tests of build.rifts: run it without a file");
        return run_frontend(opts, streams);
    }
    return usage_error(streams, std::format("unknown command '{}'", command));
}

} // namespace rift
