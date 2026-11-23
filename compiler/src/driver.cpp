#include "edax/ast_walk.hpp"
#include "edax/driver.hpp"
#include "edax/build_report.hpp"

#include <charconv>
#include "edax/graphics_registry.hpp"

#include "edax/ast_printer.hpp"
#include "edax/c_import.hpp"
#include "edax/cmake_reader.hpp"
#include "edax/cpp_wrap.hpp"
#include "edax/definition.hpp"
#include "edax/check.hpp"
#include "edax/compile_time.hpp"
#include "edax/llvm_backend.hpp"
#include "edax/diagnostics.hpp"
#include "edax/lexer.hpp"
#include "edax/modules.hpp"
#include "edax/ownership.hpp"
#include "edax/parser.hpp"
#include "edax/resolve.hpp"
#include "edax/source.hpp"
#include "edax/std_library.hpp"

#include <cstdlib>
#include <functional>
#include <set>
#include <sstream>
#include <thread>
#include <atomic>
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
#include <regex>
#include <ostream>
#include <vector>

extern char** environ;   // POSIX: the process environment, passed on to child processes

#ifndef EDAX_VERSION
#define EDAX_VERSION "0.0.0-dev"
#endif

namespace edax {

namespace {

// The modes of std::build::mode (spec 24): selectable without build.ed.
// The mode's sanitize() (spec 34) for the C and C++ targets of a build.ed build.
std::string g_c_sanitize;

bool is_builtin_mode(const std::string& mode) {
    return mode == "debug" || mode == "development" || mode == "release" || mode == "release_with_debug_info";
}

constexpr std::string_view usage_text =
    "usage: edaxc <command> [options] [file]\n"
    "\n"
    "commands:\n"
    "  lex <file>            print the token stream of a .ed/.edh file\n"
    "  parse <file>          print the syntax tree of one file\n"
    "  check <file>          check a module and everything it imports\n"
    "  symbols <file>        print the module's merged symbol table (header + source)\n"
    "  typed <file>          print the syntax tree with the type of every expression\n"
    "  build <file>          compile a program to build/<mode>/<name> (next to the file)\n"
    "  build [targets]       build the programs of ./build.ed (or the targets named)\n"
    "  run <file> [-- args]  build a program in a temporary directory and run it\n"
    "  run <target>          build and run a program of ./build.ed\n"
    "  test [tests]          build and run the tests of ./build.ed\n"
    "  emit-llvm <file>      print the LLVM IR the program compiles to\n"
    "  insights <file>       list what the compiler does implicitly (stack, heap, drops, moves, ...) as JSON\n"
    "  expand <file> <type>  print a type with the members its annotations generate written out\n"
    "  explain-checks <file> the run-time checks of the file's code and whether the optimizer removes\n"
    "                        them (as in a release build)\n"
    "  simd-report <file> [function]\n"
    "                        what the vector code of each function becomes on the CPU level: hardware\n"
    "                        forms, split or emulated operations, masked tails, multiversion copies,\n"
    "                        must_vectorize loops\n"
    "  package list|fetch|status [name]\n"
    "                        the packages of ./build.ed: list them (version, path, how they're built,\n"
    "                        local changes), fetch them without building, or show those whose files\n"
    "                        differ from build.lock\n"
    "  package update <name> --tag <tag> | --commit <commit>\n"
    "                        change a git package's version in build.ed, then fetch it\n"
    "  package convert <name> [--compare] [--to=DIR]\n"
    "                        write packages/<name>.ed from the package's CMakeLists.txt (what the\n"
    "                        CMake reader can't translate becomes TODOs), or compare the one you have\n"
    "  definition <file> <line>:<column>\n"
    "                        where the name at that place is declared, as <file>:<line>:<column>\n"
    "                        (editors' go to definition)\n"
    "  explain [code|id]     describe a diagnostic code or a warning id, or list all codes\n"
    "  registry opengl|vulkan [--source]\n"
    "                        print std::graphics::opengl or ::vulkan, the module edaxc makes from the\n"
    "                        embedded Khronos registry (gl.xml, vk.xml): the header, or the source\n"
    "\n"
    "options:\n"
    "  --diagnostic-format=human|short   diagnostic layout (default: human)\n"
    "  --color=auto|always|never         colored diagnostics (default: auto)\n"
    "  --error-limit=N                   show the first N errors, count the rest (default: 20; 0: all)\n"
    "  --progress=automatic|live|lines|quiet\n"
    "                                    build.ed builds: live lines on a terminal, a line per finished\n"
    "                                    target otherwise (automatic), or only diagnostics and the last line\n"
    "  --details=sources,units,commands,packages,toolchain,timings|all|none\n"
    "                                    what the console shows under the targets (build.log has it all)\n"
    "  --no-log-file                     don't write build/<mode>/build.log\n"
    "  --stop-on-error                   don't start other targets after one fails\n"
    "  --root=DIR                        project root for imports and build.ed (default: the file's\n"
    "                                    directory; for build.ed the current directory)\n"
    "  --stop-after=PHASE                lex, parse, resolve, types or ownership\n"
    "  --mode=MODE                       debug (default), development, release, release_with_debug_info,\n"
    "                                    or a mode of build.ed\n"
    "  --warn=ID=off|error|on            switch a warning of a check, e.g. --warn=gfx::deprecated=off\n"
    "  -o FILE, --output=FILE            where 'build' writes the executable\n"
    "  -DNAME=VALUE                      an option of build.ed\n"
    "  --list                            build: list build.ed' targets, modes and options\n"
    "  --compile-commands                build: write compile_commands.json for the C/C++ targets\n"
    "  --out-dir=DIR                     where a build.ed build goes (default: <root>/build)\n"
    "  --bin-dir=DIR                     where a build.ed build puts its programs (default: <out-dir>/<mode>)\n"
    "  --cc=COMPILER                     linker for build/run (default: $EDAX_CC, else cc)\n"
    "  --target=PLATFORM                 build for another platform: x86_64-linux-gnu, aarch64-linux-gnu,\n"
    "                                    riscv64-linux-gnu (links with PLATFORM-gcc, else clang --target),\n"
    "                                    or a target description FILE.ed declaring\n"
    "                                    'compile_time std::build::platform target = ...;'\n"
    "  --cpu=LEVEL                       the CPU level: x86_64, x86_64_v2, x86_64_v3, x86_64_v4, armv8,\n"
    "                                    armv9, riscv64, riscv64_v (default: the architecture's first)\n"
    "  --all-functions                   generate every function, not only those main reaches (testing)\n"
    "  --profile-generate                build/run: instrument the program to record a profile for the mode's\n"
    "                                    profile_guided() (merge the .profraw files with llvm-profdata)\n"
    "  --hermetic                        build/run/package: a package's find_package that .uses doesn't\n"
    "                                    answer is an error, not a library of the build machine\n"
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
    bool             profile_generate = false; // build/run --profile-generate (profile-guided optimization)
    bool             hermetic = false;         // build/run --hermetic: packages use only what the project declares
    std::vector<std::string> program_args;   // `run ... -- args`
    std::map<std::string, std::string> warn;  // --warn=gfx::deprecated=off|error|on (spec 23)
    std::vector<std::string> targets;         // build.ed targets named on the command line
    std::map<std::string, std::string> defines;   // -Dname=value (spec 25)
    bool             list = false;             // build --list
    bool             compile_commands = false; // build --compile-commands
    bool             project = false;          // the command works on build.ed
    std::string      out_dir;                  // --out-dir: where a project's build goes (default <root>/build)
    std::string      bin_dir;                  // --bin-dir: where a project's programs go (default <out>/<mode>)
    std::string      target;                   // --target=<platform>: cross compiling (spec 25)
    std::string      cpu;                      // --cpu=<level>: the CPU level (build system, "Target platforms")
    // The build's output (docs/design/build/build-output.md).
    std::string      progress;                 // --progress=automatic|live|lines|quiet ("": build.ed's, else automatic)
    std::set<std::string> details;             // --details=...
    bool             details_given = false;
    bool             no_log_file = false;      // --no-log-file
    bool             stop_on_error = false;    // --stop-on-error
};

// Whether `program` can be started through PATH.
bool on_path(const std::string& program) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string_view rest = path;
    while (!rest.empty()) {
        std::size_t      colon = rest.find(':');
        std::string_view dir   = rest.substr(0, colon);
        std::error_code  ec;
        if (!dir.empty() && std::filesystem::exists(std::filesystem::path(dir) / program, ec)) return true;
        if (colon == std::string_view::npos) break;
        rest.remove_prefix(colon + 1);
    }
    return false;
}

// The C (or C++) compiler and linker driver, as words: --cc, $EDAX_CC, else `cc` for the host;
// for another platform `<platform>-gcc` when installed, else `clang --target=<platform>`.
std::vector<std::string> c_compiler(const Options& opts, bool cpp) {
    std::string given = opts.cc;
    if (given.empty()) {
        const char* env = std::getenv("EDAX_CC");
        if (env && *env) given = env;
    }
    if (!given.empty() && !cpp) {
        std::vector<std::string> words;
        std::istringstream       in(given);
        for (std::string w; in >> w;) words.push_back(w);
        return words;
    }
    if (opts.target.empty()) return {cpp ? "c++" : "cc"};
    std::string gnu = opts.target + (cpp ? "-g++" : "-gcc");
    if (on_path(gnu)) return {gnu};
    return {cpp ? "clang++" : "clang", "--target=" + opts.target};
}

// The linker command for build_executable (a shell command line).
std::string linker_command(const Options& opts) {
    std::string out;
    for (const std::string& w : c_compiler(opts, false)) out += (out.empty() ? "" : " ") + w;
    return out;
}

constexpr int process_not_found = -1;

// Runs a program and waits for it. Returns its exit status (128 + signal number if it was
// killed), or process_not_found. With `captured`, stdout and stderr are collected there.
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
    std::string pattern = (std::filesystem::temp_directory_path() / "edaxc-run-XXXXXX").string();
    if (!mkdtemp(pattern.data())) return {};
    return pattern;
}

ExitCode usage_error(DriverStreams& s, std::string_view message) {
    s.err << "edaxc: error: " << message << "\nRun 'edaxc --help' for usage.\n";
    return ExitCode::usage_error;
}

// Parses options after the command. Returns an exit code on failure.
std::optional<ExitCode> parse_options(std::span<const std::string_view> args, Options& opts,
                                      DriverStreams& s) {
    bool builds = opts.command == "build" || opts.command == "run" || opts.command == "emit-llvm" || opts.command == "test";
    bool targets = builds || opts.command == "check" || opts.command == "insights" || opts.command == "simd-report" ||
                   opts.command == "explain-checks";   // --target and --cpu
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
            opts.mode = std::string(arg.substr(7));   // user modes are checked against build.ed
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
        if (targets && arg.starts_with("--target=")) {
            opts.target = std::string(arg.substr(9));
            if (opts.target.empty()) return usage_error(s, "'--target=' needs a platform, e.g. --target=aarch64-linux-gnu");
            continue;
        }
        if (targets && arg.starts_with("--cpu=")) {
            opts.cpu = std::string(arg.substr(6));
            if (opts.cpu.empty()) return usage_error(s, "'--cpu=' needs a CPU level, e.g. --cpu=x86_64_v3");
            continue;
        }
        if (builds && arg.starts_with("--cc=")) {
            opts.cc = std::string(arg.substr(5));
            continue;
        }
        if (builds && arg.starts_with("--progress=")) {
            opts.progress = std::string(arg.substr(11));
            if (opts.progress != "automatic" && opts.progress != "live" && opts.progress != "lines" && opts.progress != "quiet") {
                return usage_error(s, std::format("'{}': the progress is automatic, live, lines or quiet", arg));
            }
            continue;
        }
        if (builds && arg.starts_with("--details=")) {
            opts.details_given = true;
            opts.details.clear();
            std::string_view list = arg.substr(10);
            while (!list.empty()) {
                std::size_t      comma = list.find(',');
                std::string_view d     = list.substr(0, comma);
                if (d == "all") {
                    for (const char* x : {"sources", "units", "commands", "packages", "toolchain", "timings"}) opts.details.insert(x);
                } else if (d == "sources" || d == "units" || d == "commands" || d == "packages" || d == "toolchain" || d == "timings") {
                    opts.details.insert(std::string(d));
                } else if (d != "none") {
                    return usage_error(s, std::format("'{}': the details are sources, units, commands, packages, toolchain, timings, all or none", d));
                }
                list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
            }
            continue;
        }
        if (builds && arg == "--no-log-file") {
            opts.no_log_file = true;
            continue;
        }
        if (builds && arg == "--stop-on-error") {
            opts.stop_on_error = true;
            continue;
        }
        if (builds && arg == "--all-functions") {
            opts.all_functions = true;
            continue;
        }
        if (builds && arg == "--profile-generate") {
            opts.profile_generate = true;
            continue;
        }
        if (builds && arg == "--hermetic") {
            opts.hermetic = true;
            continue;
        }
        if (arg.starts_with("--error-limit=")) {
            std::string_view n = arg.substr(14);
            std::size_t      v = 0;
            if (n.empty() || std::from_chars(n.data(), n.data() + n.size(), v).ec != std::errc{}) {
                return usage_error(s, std::format("'{}': the limit is a number (0: no limit)", arg));
            }
            opts.render.error_limit = v;
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
        } else if (opts.input.empty() && (arg.ends_with(".ed") || arg.ends_with(".edh") || !builds)) {
            opts.input = arg;
        } else if ((opts.command == "expand" || opts.command == "simd-report" || opts.command == "definition") && !opts.input.empty() &&
                   opts.targets.empty()) {
            opts.targets.emplace_back(arg);   // the type, or the function
        } else if (builds && opts.input.empty() && opts.command != "emit-llvm") {
            opts.targets.emplace_back(arg);   // a target of build.ed
        } else {
            return usage_error(s, std::format("unexpected extra argument '{}'", arg));
        }
    }
    // `edaxc build`, `edaxc run app`, `edaxc test`: the project's build.ed (spec 25).
    opts.project = builds && opts.command != "emit-llvm" && opts.input.empty();
    if (opts.project && opts.command == "run" && opts.targets.size() != 1) {
        return usage_error(s, "'run' takes a program: a .ed file, or one target of build.ed");
    }
    if (!opts.project && (opts.list || opts.compile_commands || !opts.defines.empty())) {
        return usage_error(s, "'--list', '--compile-commands' and '-D' work with build.ed: run them without a file");
    }
    if (!opts.project && !is_builtin_mode(opts.mode)) {
        return usage_error(s, std::format("unknown build mode '{}' (debug, development, release, release_with_debug_info; "
                                          "other modes are defined in build.ed)", opts.mode));
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

std::string json_string(std::string_view text) {
    std::string out = "\"";
    for (char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) out += std::format("\\u{:04x}", static_cast<unsigned>(c));
            else out += c;
        }
    }
    return out + "\"";
}

// `edaxc insights <file>`: what the compiler does implicitly in the file's code, as JSON for
// editors (the VS Code extension's code insights). Every function of the file is generated,
// whether `main` reaches it or not.
// `edaxc explain-checks <file>` (spec 34 §5): every run-time check in the file's code (bounds,
// overflow, entry checks of the functions it calls) and whether the optimizer removes it, as in a
// release build. Kept checks are the ones to look at: `std::assume` states a fact that removes
// them, `@std::no_checks_left` makes the build fail when one is left.
ExitCode run_explain_checks(const Options& opts, sema::Program& program, SourceManager& sources, DiagnosticEngine& diags, DriverStreams& s) {
    LlvmOptions lo;
    lo.debug_info = false;
    std::vector<CheckSite> sites = explain_checks(program, sources, diags, std::string(opts.input), lo);
    if (diags.has_errors()) {
        render_all(s.err, diags, sources, opts.render);
        return ExitCode::compile_error;
    }
    std::string file = std::filesystem::path(opts.input).lexically_normal().generic_string();
    struct Row {
        std::string   where;
        std::uint64_t line = 0, column = 0;
        const CheckSite* site = nullptr;
    };
    std::vector<Row> rows;
    for (const CheckSite& c : sites) {
        std::size_t colon2 = c.where.rfind(':');
        std::size_t colon1 = colon2 == std::string::npos ? std::string::npos : c.where.rfind(':', colon2 - 1);
        if (colon1 == std::string::npos) continue;
        std::string path = c.where.substr(0, colon1);
        if (std::filesystem::path(path).lexically_normal().generic_string() != file) continue;   // the file's own code
        rows.push_back(Row{c.where, std::stoull(c.where.substr(colon1 + 1, colon2 - colon1 - 1)), std::stoull(c.where.substr(colon2 + 1)), &c});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return std::tie(a.line, a.column) < std::tie(b.line, b.column); });
    std::size_t kept = 0;
    for (const Row& r : rows) kept += r.site->kept ? 1 : 0;
    s.out << std::format("{}: {} run-time check{}, {} removed by the optimizer, {} kept\n", file, rows.size(), rows.size() == 1 ? "" : "s",
                         rows.size() - kept, kept);
    for (const Row& r : rows) {
        std::string what;
        for (const std::string& w : r.site->what) what += (what.empty() ? "" : "; ") + w;
        s.out << std::format("  {:<8} {}  {}\n", r.site->kept ? "kept" : "removed", r.where, what);
    }
    return ExitCode::success;
}

// `edaxc simd-report <file> [<function>]` (SIMD, "Seeing what happens"): per function of the
// file, its vector types and their hardware form on the CPU level, the operations that are split
// or emulated, the masked tail steps of block loops, the multiversion copies, and the loops of
// `@std::simd::must_vectorize` functions with the vectorizer's verdict.
ExitCode run_simd_report(const Options& opts, sema::Program& program, SourceManager& sources, DiagnosticEngine& diags, DriverStreams& s) {
    using types::Kind;
    const TargetPlatform p    = target_platform();
    bool                 x86  = p.triple.starts_with("x86_64");
    bool                 arm  = p.triple.starts_with("aarch64");
    unsigned             reg  = p.vector_bytes;
    bool                 simd = p.has("simd");
    auto register_name = [&](unsigned bytes) -> std::string {
        if (x86) return bytes <= 16 ? "xmm" : bytes <= 32 ? "ymm" : "zmm";
        if (arm) return bytes <= 8 ? "d" : "q";
        return "v";
    };
    auto form = [&](const types::Type* t) -> std::string {
        unsigned bytes = static_cast<unsigned>(t->inner->bits / 8 * t->bits);
        if (t->is(Kind::mask)) bytes = static_cast<unsigned>(t->bits) * 4;
        if (!simd) return "computed lane by lane (the target has no vector registers)";
        if (bytes <= reg) return std::format("1 × {}", register_name(std::max(bytes, 16u)));
        return std::format("{} × {} (split)", (bytes + reg - 1) / reg, register_name(reg));
    };
    // Generate the program once to learn which must_vectorize loops vectorize.
    std::set<std::pair<FileId, std::uint32_t>> scalar_loops;
    bool must = false;
    if (program.entry && program.entry->source) {
        for (const ast::DeclPtr& d : program.entry->source->ast.decls) {
            if (d->kind == ast::DeclKind::function && sema::find_annotation(d->annotations, "std::simd::must_vectorize")) must = true;
        }
    }
    if (must) {
        DiagnosticEngine quiet;
        LlvmOptions      lo;
        lo.debug_info = true;
        (void)generate_llvm_ir(program, sources, quiet, lo);
        for (const Diagnostic& d : quiet.all()) {
            if (d.code == DiagCode::E6003) scalar_loops.insert({d.span.file, d.span.begin});
        }
    }
    auto line_of = [&](Span sp) { return sources.file(sp.file).line_col(sp.begin).line; };
    bool any = false;
    s.out << std::format("CPU level {} ({}), widest vector register {} bytes\n", p.cpu, p.triple, reg);
    std::vector<const ast::FunctionDecl*> fns;
    std::vector<std::string>              names;
    if (program.entry && program.entry->source) {
        for (const ast::DeclPtr& d : program.entry->source->ast.decls) {
            if (d->kind == ast::DeclKind::function) {
                fns.push_back(&d->as<ast::FunctionDecl>());
                names.push_back(d->name);
            }
            if (d->kind == ast::DeclKind::struct_ || d->kind == ast::DeclKind::class_) {
                for (const ast::DeclPtr& m : d->as<ast::TypeDecl>().members) {
                    if (m->kind != ast::DeclKind::function) continue;
                    fns.push_back(&m->as<ast::FunctionDecl>());
                    names.push_back(d->name + "::" + m->name);
                }
            }
        }
    }
    for (std::size_t i = 0; i < fns.size(); ++i) {
        const ast::FunctionDecl& f = *fns[i];
        if (!opts.targets.empty() && names[i] != opts.targets.front() && f.name != opts.targets.front()) continue;
        if (!f.body) continue;
        std::map<std::string, std::string> vectors;   // type → form
        std::vector<std::string>           operations, tails, loops;
        ast::walk_exprs(const_cast<ast::BlockExpr&>(*f.body), [&](ast::Expr& e) {
            const types::Type* t = e.type ? types::strip_ref(e.type) : nullptr;
            if (t && (t->is(Kind::vector) || t->is(Kind::mask))) vectors[types::to_string(t)] = form(t);
            if (e.kind == ast::ExprKind::binary && t && t->is(Kind::vector)) {
                auto& b = e.as<ast::BinaryExpr>();
                std::string what = types::to_string(t);
                if (t->inner->is_integer() && (b.op == TokenKind::slash || b.op == TokenKind::percent)) {
                    operations.push_back(std::format("line {}: {} {} is emulated: one lane at a time (no vector integer division)", line_of(e.span),
                                                     what, b.op == TokenKind::slash ? "/" : "%"));
                } else if (t->inner->is_integer() && t->inner->bits == 64 && b.op == TokenKind::star && x86 &&
                           !p.has_feature("avx512dq")) {
                    operations.push_back(std::format("line {}: {} * is emulated: three multiplies per lane pair (vpmullq needs AVX-512)",
                                                     line_of(e.span), what));
                } else if (form(t).ends_with("(split)")) {
                    operations.push_back(std::format("line {}: {} {} is split: one instruction per register", line_of(e.span), what,
                                                     token_kind_spelling(b.op)));
                }
            }
            if (e.kind == ast::ExprKind::call) {
                auto& c = e.as<ast::CallExpr>();
                if (c.callee->kind == ast::ExprKind::member) {
                    const std::string& m = c.callee->as<ast::MemberExpr>().name.segments.back().name;
                    if ((m == "gather" || m == "scatter") && x86 && !p.has_feature("avx2")) {
                        operations.push_back(std::format("line {}: {} is emulated: one load or store per lane (gathers need AVX2)", line_of(e.span), m));
                    }
                }
            }
            if (e.kind == ast::ExprKind::for_) {
                auto& fe = e.as<ast::ForExpr>();
                const types::Type* vt = fe.symbol && fe.symbol->type ? types::strip_ref(fe.symbol->type) : nullptr;
                if (vt && vt->is(Kind::user) && vt->symbol->name == "block") {
                    bool native = p.has_feature("avx512f") || p.has_feature("sve");
                    tails.push_back(std::format("line {}: the last block is one masked step ({})", line_of(fe.span),
                                                native ? "native masked loads and stores" : "a short sequence of loads, blends and stores"));
                } else if (sema::find_annotation(f.annotations, "std::simd::must_vectorize") && !fe.is_compile_time) {
                    bool scalar = scalar_loops.contains({fe.span.file, fe.span.begin});
                    loops.push_back(std::format("line {}: {}", line_of(fe.span), scalar ? "NOT vectorized (edaxc build reports why)" : "vectorized"));
                }
            }
            return true;
        });
        std::vector<std::string> copies;
        if (const ast::AnnotationUse* u = sema::find_annotation(f.annotations, "std::simd::multiversion"); u && u->value_symbol && u->value_symbol->ct_value) {
            const ct::Value& v = *u->value_symbol->ct_value;
            if (!v.elements.empty()) {
                for (const ct::Value& level : v.elements[0].elements) {
                    if (!level.symbol) continue;
                    unsigned bytes = cpu_level_vector_bytes(level.symbol->name);
                    copies.push_back(std::format("{} (vector registers of {} bytes)", level.symbol->name, bytes));
                }
                copies.push_back(std::format("{} (the build's own level, for CPUs below those)", p.cpu));
            }
        }
        if (vectors.empty() && operations.empty() && tails.empty() && loops.empty() && copies.empty()) continue;
        any = true;
        s.out << std::format("\n{} (line {})\n", names[i], line_of(f.span));
        if (!vectors.empty()) {
            s.out << "  vector types:\n";
            for (const auto& [t, how] : vectors) s.out << std::format("    {}: {}\n", t, how);
        }
        if (!operations.empty()) {
            s.out << "  split or emulated:\n";
            for (const std::string& o : operations) s.out << "    " << o << "\n";
        }
        if (!tails.empty()) {
            s.out << "  masked tails:\n";
            for (const std::string& t : tails) s.out << "    " << t << "\n";
        }
        if (!copies.empty()) {
            s.out << "  multiversion copies:\n";
            for (const std::string& c : copies) s.out << "    " << c << "\n";
        }
        if (!loops.empty()) {
            s.out << "  must_vectorize loops:\n";
            for (const std::string& l : loops) s.out << "    " << l << "\n";
        }
    }
    if (!any) s.out << (opts.targets.empty() ? "no vector code\n" : std::format("'{}' has no vector code\n", opts.targets.front()));
    diags.sort_by_location();
    render_all(s.err, diags, sources, opts.render);
    return diags.has_errors() ? ExitCode::compile_error : ExitCode::success;
}

ExitCode run_insights(const Options& opts, sema::Program& program, SourceManager& sources, DiagnosticEngine& diags,
                      DriverStreams& s) {
    auto setting = [&](const char* name) {
        auto it = program.mode_settings.find(name);
        return it == program.mode_settings.end() ? std::string() : it->second;
    };
    std::vector<Insight> insights;
    LlvmOptions          options;
    options.all_functions = true;
    options.panic_trap    = setting("panic") == "trap";
    options.insights      = &insights;
    generate_llvm_ir(program, sources, diags, options);
    diags.sort_by_location();
    render_all(s.err, diags, sources, opts.render);
    if (diags.has_errors()) return ExitCode::compile_error;
    s.out << "[\n";
    for (std::size_t i = 0; i < insights.size(); ++i) {
        const Insight&    in = insights[i];
        const SourceFile& f  = sources.file(in.span.file);
        LineCol           b  = f.line_col(in.span.begin);
        LineCol           e  = f.line_col(in.span.end);
        s.out << std::format("  {{\"file\": {}, \"line\": {}, \"column\": {}, \"endLine\": {}, \"endColumn\": {}, \"category\": {}, "
                             "\"kind\": {}, \"placement\": {}, \"message\": {}, \"detail\": {}}}{}\n",
                             json_string(f.path()), b.line, b.column, e.line, e.column, json_string(in.category), json_string(in.kind),
                             json_string(in.placement), json_string(in.message), json_string(in.detail), i + 1 < insights.size() ? "," : "");
    }
    s.out << "]\n";
    return ExitCode::success;
}

// The backend: LLVM IR text (emit-llvm), then an executable: the IR is compiled to an object file
// for the host and linked with the system linker (--cc, EDAX_CC or cc).
// Profile-guided optimization (build modes): `--profile-generate` makes the program write raw
// profiles next to itself; the mode's profile_guided() (relative to the project) is the merged
// profile to optimize with.
void set_profile_options(CodegenOptions& codegen, const Options& opts, const std::filesystem::path& exe, const std::string& profile,
                         const std::filesystem::path& base) {
    if (opts.profile_generate) {
        codegen.profile_generate = exe.string() + "-%p.profraw";
    } else if (!profile.empty()) {
        std::filesystem::path p(profile);
        codegen.profile_use = (p.is_absolute() ? p : base / p).lexically_normal().string();
    }
}

ExitCode run_backend(const Options& opts, sema::Program& program, SourceManager& sources, DiagnosticEngine& diags,
                          DriverStreams& s) {
    auto setting = [&](const char* name) {
        auto it = program.mode_settings.find(name);
        return it == program.mode_settings.end() ? std::string() : it->second;
    };
    LlvmOptions llvm_options{opts.all_functions, setting("panic") == "trap",
                             setting("debug_info") == "full" || setting("debug_info") == "line_tables",
                             setting("debug_info") == "full"};
    std::string      ir;          // emit-llvm: the text
    GeneratedProgram generated;   // build, run: the module itself
    if (opts.command == "emit-llvm") ir = generate_llvm_ir(program, sources, diags, llvm_options);
    else generated = generate_llvm_program(program, sources, diags, llvm_options);
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
            s.err << "edaxc: error[E0004]: cannot create a temporary directory\n";
            return ExitCode::compile_error;
        }
        exe = temp_dir / input.stem();
    } else if (opts.output.empty()) {
        fs::path root = opts.root.empty() ? input.parent_path() : fs::path(opts.root);
        exe           = root / "build" / opts.mode / opts.target / input.stem();
    } else {
        exe = opts.output;
    }
    if (!exe.parent_path().empty()) fs::create_directories(exe.parent_path(), ec);

    std::string linker = linker_command(opts);
    std::string error;
    CodegenOptions codegen;
    codegen.optimization = setting("optimization").empty() ? (opts.mode == "debug" ? "none" : "speed") : setting("optimization");
    codegen.debug_info   = setting("debug_info") == "full" || setting("debug_info") == "line_tables";
    codegen.sanitize     = setting("sanitize").empty() ? "none" : setting("sanitize");
    if (codegen.sanitize != "none") linker += " -fsanitize=" + codegen.sanitize;
    set_profile_options(codegen, opts, exe, setting("profile_guided"), input.parent_path());
    if (!build_executable(generated, exe.string(), linker, error, codegen)) {
        if (error.starts_with("the linker")) {
            s.err << std::format("edaxc: error: {}\n", error);
            if (!opts.target.empty()) {
                s.err << std::format("note: linking for '{}' needs its C library: install a cross toolchain ({}-gcc) or pass --cc=...\n",
                                     opts.target, opts.target);
            }
            if (!temp_dir.empty()) fs::remove_all(temp_dir, ec);
            return ExitCode::compile_error;
        }
        s.err << std::format("edaxc: internal compiler error: {}\n", error);
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

// `edaxc definition <file> <line>:<column>` (editors' go to definition): prints
// `<file>:<line>:<column>` of the declaration of the name there, or nothing (exit 1). The
// program is checked as far as it goes; diagnostics are not printed.
ExitCode run_definition(const Options& opts, DriverStreams& s) {
    namespace fs = std::filesystem;
    if (opts.targets.empty()) return usage_error(s, "'definition' takes a file and a place: edaxc definition <file> <line>:<column>");
    const std::string& place = opts.targets.front();
    std::size_t colon = place.find(':');
    std::uint32_t line = 0, column = 0;
    try {
        line   = static_cast<std::uint32_t>(std::stoul(place.substr(0, colon)));
        column = colon == std::string::npos ? 1 : static_cast<std::uint32_t>(std::stoul(place.substr(colon + 1)));
    } catch (...) {
        return usage_error(s, std::format("'{}' is not a place: write <line>:<column> (both from 1)", place));
    }
    std::string      input(opts.input);
    SourceManager    sources;
    DiagnosticEngine diags;
    std::error_code  ec;
    fs::path         wanted = fs::weakly_canonical(input, ec);
    std::string      wanted_name;   // a file of the standard library: its name in edaxc ("<std>/log.ed")
    fs::path         std_dir = fs::weakly_canonical(EDAX_STD_DIR, ec);
    if (fs::path rel = wanted.lexically_relative(std_dir); !rel.empty() && !rel.string().starts_with("..")) {
        // Loaded through a program that imports the module: the library is compiled into edaxc.
        std::string module = rel.replace_extension().generic_string();
        for (std::size_t at; (at = module.find('/')) != std::string::npos;) module.replace(at, 1, "::");
        if (module != "std") module = "std::" + module;
        wanted_name = "<std>/" + wanted.lexically_relative(std_dir).generic_string();
        fs::path entry = fs::temp_directory_path(ec) / std::format("edaxc-definition-{}.ed", static_cast<long>(::getpid()));
        std::ofstream(entry) << "import std;\nimport " << module << ";\ni32 main() { return 0; }\n";
        input = entry.string();
    }
    sema::Program program = load_program(input, [&] { LoadOptions o; o.root = opts.root; return o; }(), sources, diags);
    if (!wanted_name.empty()) fs::remove(input, ec);
    if (!diags.has_errors()) resolve_program(program, diags);
    program.build_mode = opts.mode;
    if (!diags.has_errors()) check_types(program, sources, diags);
    // The file asked about, and the byte offset of the place.
    for (FileId id = 0; id < sources.count(); ++id) {
        const SourceFile& f = sources.file(id);
        if (wanted_name.empty() ? f.path().starts_with("<") || fs::weakly_canonical(f.path(), ec) != wanted : f.path() != wanted_name) continue;
        if (line == 0 || line > f.line_count()) return ExitCode::compile_error;
        std::uint32_t offset = f.line_start(line);
        std::string_view text = f.text();
        for (std::uint32_t c = 1; c < column && offset < text.size() && text[offset] != '\n'; ++c) {
            char32_t    cp  = 0;
            std::size_t len = decode_utf8(text, offset, cp);
            offset += static_cast<std::uint32_t>(len == 0 ? 1 : len);
        }
        std::optional<Span> at = find_definition(program, id, offset);
        if (!at) return ExitCode::compile_error;
        const SourceFile& target = sources.file(at->file);
        LineCol lc = target.line_col(at->begin);
        fs::path path(target.path());
        if (target.path().starts_with("<std>/")) {   // the library's files: in the tree edaxc was built from
            fs::path tree = fs::path(EDAX_STD_DIR) / target.path().substr(6);
            if (fs::exists(tree, ec)) path = tree;
        } else {
            path = fs::absolute(path, ec);
        }
        // Relative to the current directory when the file is below it (tests and messages stay short).
        path = path.lexically_normal();
        if (fs::path rel = path.lexically_relative(fs::current_path(ec)); !rel.empty() && !rel.string().starts_with("..")) path = rel;
        s.out << std::format("{}:{}:{}\n", path.string(), lc.line, lc.column);
        return ExitCode::success;
    }
    return ExitCode::compile_error;
}

ExitCode run_frontend(const Options& opts, DriverStreams& s) {
    std::string input(opts.input);
    if (!input.ends_with(".ed") && !input.ends_with(".edh")) {
        s.err << std::format("edaxc: error[E0002]: '{}': {}\n", input,
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
            s.err << std::format("edaxc: error[E0001]: cannot read '{}': {}\n", input, reason);
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
        s.err << std::format("edaxc: error[E0001]: cannot read '{}': cannot open file\n", input);
        return ExitCode::compile_error;
    }
    if (opts.command == "definition") return run_definition(opts, s);
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
    program.build_target = std::filesystem::path(input).stem().string();   // `{target}` without build.ed (spec 31)
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
    if (opts.command == "insights" && !diags.has_errors()) {
        return run_insights(opts, program, sources, diags, s);
    }
    if (opts.command == "simd-report" && !diags.has_errors()) {
        return run_simd_report(opts, program, sources, diags, s);
    }
    if (opts.command == "explain-checks" && !diags.has_errors()) {
        return run_explain_checks(opts, program, sources, diags, s);
    }
    if (opts.command == "symbols" && resolved && program.entry) {
        print_symbols(s.out, *program.entry, sources);
    }
    if (opts.command == "expand" && resolved && !diags.has_errors()) {
        if (opts.targets.empty()) return usage_error(s, "'expand' takes a file and a type: edaxc expand <file> <type>");
        if (!print_expansion(s.out, program, sources, opts.targets.front())) {
            s.err << std::format("edaxc: error: no type '{}' in '{}' or the modules it imports\n", opts.targets.front(), input);
            return ExitCode::compile_error;
        }
    }
    if (opts.command == "typed" && typed && program.entry) {
        if (program.entry->header) ast::print(s.out, program.entry->header->ast, true);
        if (program.entry->source) ast::print(s.out, program.entry->source->ast, true);
    }
    diags.sort_by_location();
    render_all(s.err, diags, sources, opts.render);
    return diags.has_errors() ? ExitCode::compile_error : ExitCode::success;
}

// --- build.ed (spec 25) ------------------------------------------------------------------

// The mode's settings that relax a rule of the built-in modes, for the note every such build
// prints (spec 24).
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

// The report of the project build running now (run_project), or null: the target builders
// report their progress to it.
BuildReport* g_report = nullptr;

std::string target_kind_text(ct::BuildTarget::Kind k) {
    switch (k) {
    case ct::BuildTarget::Kind::executable: return "program";
    case ct::BuildTarget::Kind::library: return "library";
    case ct::BuildTarget::Kind::test: return "test";
    case ct::BuildTarget::Kind::c_library: return "C library";
    case ct::BuildTarget::Kind::system_library: return "system library";
    case ct::BuildTarget::Kind::foreign: return "target of a dependency";
    case ct::BuildTarget::Kind::plugin: return "plugin";
    }
    return "";
}

// The checklist line of a target (added when it wasn't: a library built on the way), started.
std::optional<std::size_t> report_start(const ct::BuildTarget& t) {
    if (!g_report) return std::nullopt;
    std::optional<std::size_t> item = g_report->find(t.name);
    if (!item) item = g_report->add_item(t.name, target_kind_text(t.kind));
    if (!g_report->started(*item)) g_report->start(*item);
    return item;
}

// A path as the build output shows it: relative to the working directory when inside it.
std::string shown_path(const std::filesystem::path& p) {
    std::error_code       ec;
    std::filesystem::path rel = std::filesystem::relative(p, std::filesystem::current_path(ec), ec);
    if (!ec && !rel.empty() && !rel.string().starts_with("..")) return rel.string();
    return p.string();
}

std::string command_text(const std::vector<std::string>& cmd) {
    std::string text;
    for (const std::string& a : cmd) text += (text.empty() ? "" : " ") + a;
    return text;
}

// Compiles and links one Edax target of build.ed. Returns the executable's path, or empty.
std::string build_edax_target(const Options& opts, const std::filesystem::path& root, const std::filesystem::path& out,
                              const ct::BuildTarget& t, const std::string& mode, const std::vector<std::string>& libraries,
                              const std::vector<std::string>& extra_roots, DriverStreams& s, bool& failed) {
    namespace fs = std::filesystem;
    fs::path base  = t.base.empty() ? root : fs::path(t.base);
    fs::path entry = (base / t.root).lexically_normal();
    std::error_code ec;
    std::optional<std::size_t> item = report_start(t);
    auto fail = [&](std::size_t errors) {
        failed = true;
        if (item) {
            g_report->add_diagnostics(*item, errors, 0);
            g_report->finish(*item, BuildReport::State::failed, {});
        }
        return std::string();
    };
    if (!fs::is_regular_file(entry, ec)) {
        s.err << std::format("edaxc: error[E0001]: target '{}': cannot read '{}'\n", t.name, entry.string());
        return fail(1);
    }
    // Up to date (spec 25): the executable is newer than every file it was built from, and was
    // built the same way (mode, settings, libraries, linker, this edaxc).
    bool     plugin   = t.kind == ct::BuildTarget::Kind::plugin;   // `<bin>/plugins/<name>.so` (dynamic libraries)
    bool     shared_library = t.kind == ct::BuildTarget::Kind::library && t.shared;   // `<bin>/lib<name>.so`
    fs::path bin_dir  = opts.bin_dir.empty() ? out / mode / opts.target : fs::path(opts.bin_dir);
    fs::path exe_path = plugin ? bin_dir / "plugins" / (t.name + ".so") : shared_library ? bin_dir / ("lib" + t.name + ".so") : bin_dir / t.name;
    fs::path stamp    = exe_path;
    stamp += ".inputs";
    std::string signature = std::format("mode={} target={} cc={}{}", mode, opts.target, linker_command(opts),
                                        opts.profile_generate ? " profile-generate" : "");
    for (const auto& [name, value] : t.settings) signature += std::format(" -D{}={}", name, value ? ct::to_string(*value) : "?");
    for (const auto& [id, level] : opts.warn) signature += std::format(" --warn={}={}", id, level);
    for (const std::string& l : libraries) signature += " " + l;
    for (const std::string& r : extra_roots) signature += " root=" + r;
    for (const std::string& d : t.shared_library_dirs) signature += " shared=" + d;
    {
        auto edaxc_time = fs::last_write_time("/proc/self/exe", ec);
        if (!ec) signature += std::format(" edaxc={}", edaxc_time.time_since_epoch().count());
        ec.clear();
    }
    if (std::ifstream in(stamp); in && fs::exists(exe_path, ec)) {
        std::string line;
        bool        fresh = std::getline(in, line) && line == signature;
        auto        built = fs::last_write_time(exe_path, ec);
        while (fresh && std::getline(in, line)) {
            if (line.empty()) continue;
            std::error_code e2;
            if (!fs::exists(line, e2) || fs::last_write_time(line, e2) > built) fresh = false;
        }
        for (const std::string& l : libraries) {
            std::error_code e2;
            if (!l.starts_with("-") && (!fs::exists(l, e2) || fs::last_write_time(l, e2) > built)) fresh = false;
        }
        if (fresh) {
            if (item) g_report->finish(*item, BuildReport::State::up_to_date, "up to date", shown_path(exe_path));
            return exe_path.string();
        }
    }
    fs::remove(stamp, ec);
    // g.target_cpu(t, level) (spec 34): the whole target is checked and compiled for the level.
    struct CpuScope {
        std::string before = build_cpu();
        ~CpuScope() {
            std::string e;
            set_build_cpu(before, e);
        }
    } cpu_scope;
    if (auto it = t.settings.find("std::build::cpu"); it != t.settings.end() && it->second && it->second->symbol &&
                                                      it->second->symbol->name != "baseline") {
        std::string error;
        if (!set_build_cpu(it->second->symbol->name, error)) {
            std::string levels;
            for (const std::string& l : cpu_levels()) levels += (levels.empty() ? "" : ", ") + l;
            s.err << std::format("edaxc: error: target '{}': g.target_cpu: {} (levels for this target: {})\n", t.name, error, levels);
            return fail(1);
        }
    }
    // The phases are units of the target; its source files are units too, done once checked.
    std::size_t unit_read = 0, unit_check = 0, unit_code = 0, unit_link = 0;
    if (item) {
        unit_read  = g_report->add_unit(*item, "read");
        unit_check = g_report->add_unit(*item, "check");
        unit_code  = g_report->add_unit(*item, "code");
        unit_link  = g_report->add_unit(*item, "link");
        g_report->unit_start(*item, unit_read);
    }
    auto phase = [&](std::size_t done, std::optional<std::size_t> next, bool ok) {
        if (!item) return;
        g_report->unit_done(*item, done, ok);
        if (ok && next) g_report->unit_start(*item, *next);
    };
    SourceManager    sources;
    DiagnosticEngine diags;
    LoadOptions      load;
    load.root         = base.lexically_normal().string();
    load.build_script = (base / "build.ed").lexically_normal().string();
    load.extra_roots  = extra_roots;
    sema::Program program = load_program(entry.string(), load, sources, diags);
    if (!diags.has_errors()) resolve_program(program, diags);
    std::size_t source_units = 0;
    if (item) {   // the target's own files: not the standard library's, not build.ed
        std::vector<SourceInfo> files;
        std::string             script = fs::absolute(load.build_script, ec).lexically_normal().string();
        for (std::size_t id = 0; id < sources.count(); ++id) {
            const std::string& path = sources.file(static_cast<FileId>(id)).path();
            if (path.starts_with("<")) continue;
            fs::path absolute = fs::absolute(path, ec).lexically_normal();
            if (absolute.string() == script) continue;
            fs::path rel   = absolute.lexically_relative(fs::absolute(base, ec).lexically_normal());
            bool     under = !rel.empty() && !rel.string().starts_with("..");
            files.push_back(source_info(absolute, under ? rel.string() : absolute.string()));
        }
        std::sort(files.begin(), files.end(), [](const SourceInfo& a, const SourceInfo& b) { return a.path < b.path; });
        g_report->set_sources(*item, files);
        source_units = files.size();
        unit_read += source_units, unit_check += source_units, unit_code += source_units, unit_link += source_units;
        if (!diags.has_errors()) {
            for (std::size_t k = 0; k < source_units; ++k) g_report->unit_start(*item, k);
        }
    }
    bool read_ok = !diags.has_errors();
    phase(unit_read, unit_check, read_ok);
    program.build_mode     = mode;
    program.warn_levels    = opts.warn;
    program.build_settings = t.settings;
    program.file_settings  = t.file_settings;
    program.build_target   = t.name;
    program.is_plugin      = plugin;
    program.is_shared_library = shared_library;
    {   // shared Edax libraries: which modules are the library's (its root's directory) and which
        // are compiled in a shared library this target links
        auto module_dir = [&](const sema::Module& m) -> std::string {
            const std::optional<sema::ModuleFile>& f = m.source ? m.source : m.header;
            if (!f) return {};
            std::error_code e2;
            return fs::weakly_canonical(fs::absolute(sources.file(f->id).path(), e2), e2).generic_string();
        };
        auto under = [](const std::string& file, const std::string& dir) { return !dir.empty() && file.starts_with(dir + "/"); };
        std::string own_dir = shared_library ? fs::weakly_canonical(entry.parent_path(), ec).generic_string() : std::string();
        for (const auto& [name, m] : program.modules) {
            if (name == "build" || name == "std" || name.starts_with("std::")) continue;
            std::string file = module_dir(*m);
            if (shared_library && under(file, own_dir)) {
                program.library_modules.insert(m.get());
                continue;
            }
            for (const std::string& d : t.shared_library_dirs) {
                if (under(file, d)) program.shared_modules.insert(m.get());
            }
        }
    }
    if (!diags.has_errors()) check_types(program, sources, diags);
    if (!diags.has_errors()) check_ownership(program, diags);
    if (item && !diags.has_errors()) {
        for (std::size_t k = 0; k < source_units; ++k) g_report->unit_done(*item, k);
    }
    if (read_ok) phase(unit_check, unit_code, !diags.has_errors());
    GeneratedProgram generated;
    auto setting = [&](const char* name) {
        auto it = program.mode_settings.find(name);
        return it == program.mode_settings.end() ? std::string() : it->second;
    };
    if (!diags.has_errors()) generated = generate_llvm_program(program, sources, diags, LlvmOptions{false, setting("panic") == "trap",
                                                                         setting("debug_info") == "full" || setting("debug_info") == "line_tables",
                                                                         setting("debug_info") == "full"});
    if (item && !diags.has_errors()) phase(unit_code, unit_link, true);
    diags.sort_by_location();
    RenderOptions render = opts.render;
    if (g_report && opts.render.format == DiagnosticFormat::human) {
        // The console gets the first errors, build.log all of them.
        render.summary_line = false;
        render.full_log     = g_report->log_path_text();
        std::ostringstream shown, all;
        render_all(shown, diags, sources, render);
        RenderOptions everything = render;
        everything.error_limit   = 0;
        everything.color         = false;
        render_all(all, diags, sources, everything);
        if (!shown.str().empty()) g_report->print_console(shown.str(), true);
        if (!all.str().empty()) g_report->log_only(all.str());
    } else {
        render_all(s.err, diags, sources, render);
    }
    if (item) g_report->add_diagnostics(*item, 0, diags.warning_count());
    if (diags.has_errors()) return fail(diags.error_count());
    fs::path exe = exe_path;
    fs::create_directories(exe.parent_path(), ec);
    std::string linker = linker_command(opts);
    if (setting("link_time_optimization") == "true") linker += " -flto=auto";   // the C libraries carry LTO code (spec 24)
    CodegenOptions codegen;
    codegen.optimization = setting("optimization").empty() ? "none" : setting("optimization");
    codegen.debug_info   = setting("debug_info") == "full" || setting("debug_info") == "line_tables";
    codegen.sanitize     = setting("sanitize").empty() ? "none" : setting("sanitize");
    if (codegen.sanitize != "none") linker += " -fsanitize=" + codegen.sanitize;
    codegen.shared       = plugin || shared_library;
    set_profile_options(codegen, opts, exe, setting("profile_guided"), base);
    std::string error;
    if (!build_executable(generated, exe.string(), linker, error, codegen, libraries)) {
        s.err << std::format("edaxc: error: target '{}': {}\n", t.name, error);
        if (!opts.target.empty() && error.starts_with("the linker")) {
            s.err << std::format("note: linking for '{}' needs its C library: install a cross toolchain ({}-gcc) or pass --cc=...\n",
                                 opts.target, opts.target);
        }
        if (item) g_report->unit_done(*item, unit_link, false);
        return fail(1);
    }
    if (item) {
        g_report->unit_done(*item, unit_link);
        g_report->finish(*item, BuildReport::State::done, {}, shown_path(exe));
    }
    if (std::ofstream st(stamp); st) {   // what the next build compares with
        st << signature << "\n";
        for (std::size_t id = 0; id < sources.count(); ++id) {
            const std::string& path = sources.file(static_cast<FileId>(id)).path();
            std::error_code    e2;
            if (!path.starts_with("<") && fs::exists(path, e2)) st << fs::absolute(path, e2).lexically_normal().string() << "\n";
        }
        for (const std::string& f : program.embedded_files) st << f << "\n";   // std::embed (spec 34)
        if (auto it = program.mode_settings.find("profile_guided"); it != program.mode_settings.end() && !it->second.empty() && !opts.profile_generate) {
            fs::path p(it->second);
            st << fs::absolute(p.is_absolute() ? p : base / p, ec).lexically_normal().string() << "\n";   // the profile: an input
        }
        st << fs::absolute(load.build_script, ec).lexically_normal().string() << "\n";
    }
    return exe.string();
}

// The compile commands of the C targets, for `--compile-commands` (clangd).
struct CompileCommand {
    std::string directory;
    std::string file;
    std::vector<std::string> arguments;
};

// Compiles a C/C++ target to `<out>/c/lib<name>.a`. Returns the archive, or empty.
std::string build_c_target(const Options& opts, const std::filesystem::path& root, const std::filesystem::path& out,
                           const ct::BuildGraph& g, const ct::BuildTarget& t, const std::string& optimization, bool lto,
                           DriverStreams& s, bool& failed, std::vector<CompileCommand>* commands) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path base = t.base.empty() ? root : fs::path(t.base);
    fs::path dir  = out / "c" / opts.target / t.name;
    fs::create_directories(dir, ec);
    bool                     cpp      = t.language == "cpp";
    std::vector<std::string> compiler = c_compiler(opts, cpp);
    std::vector<std::string> objects;
    std::optional<std::size_t> item = report_start(t);
    if (item) {
        std::vector<SourceInfo> files;
        for (const std::string& src : t.sources) {
            fs::path p = fs::path(src).is_absolute() ? fs::path(src) : base / src;
            files.push_back(source_info(p, src));
        }
        g_report->set_sources(*item, files);
    }
    auto fail = [&]() {
        failed = true;
        if (item) {
            g_report->add_diagnostics(*item, 1, 0);
            g_report->finish(*item, BuildReport::State::failed, {});
        }
        return std::string();
    };
    // The commands: in build.log, and on the console with the `commands` detail.
    auto show_command = [&](const std::vector<std::string>& cmd) {
        if (!g_report) return;
        if (g_report->shows(t.name, "commands")) g_report->print("    " + command_text(cmd) + "\n", false);
        else g_report->log_only("    " + command_text(cmd));
    };
    struct Job {
        std::vector<std::string> cmd;
        std::string              source;
        std::string              output;   // compiler output
        int                      status = 0;
        std::size_t              unit   = 0;
        std::string              object;
    };
    std::vector<Job> jobs;   // the objects that are out of date: compiled in parallel
    for (std::size_t i = 0; i < t.sources.size(); ++i) {
        fs::path src = fs::path(t.sources[i]).is_absolute() ? fs::path(t.sources[i]) : base / t.sources[i];
        fs::path obj = dir / (src.stem().string() + "." + std::to_string(i) + ".o");
        std::vector<std::string> cmd = compiler;
        for (std::string a : {std::string("-c"), src.string(), std::string("-o"), obj.string(), std::string("-fPIC")}) cmd.push_back(a);
        if (lto) {   // link_time_optimization() (spec 24): fat objects, so the archive also links without LTO
            cmd.push_back("-flto=auto");
            cmd.push_back("-ffat-lto-objects");
        }
        if (!t.standard.empty()) {
            std::string std_ = t.standard;
            if (std_.starts_with("cpp")) std_ = "c++" + std_.substr(3);
            cmd.push_back("-std=" + std_);
        }
        cmd.push_back(optimization == "none" ? "-O0" : optimization == "basic" ? "-O1" : optimization == "size" ? "-Os" : "-O2");
        if (!g_c_sanitize.empty() && g_c_sanitize != "none") cmd.push_back("-fsanitize=" + g_c_sanitize);   // spec 34
        auto add_dirs = [&](const ct::BuildTarget& from, bool only_shared) {
            fs::path from_base = from.base.empty() ? root : fs::path(from.base);
            for (const auto& [d, shared] : from.include_dirs) {
                if (!only_shared || shared) cmd.push_back("-I" + (fs::path(d).is_absolute() ? fs::path(d) : from_base / d).string());
            }
            if (only_shared) {   // `visibility::users`: only for what links the target
                for (const std::string& d : from.user_include_dirs) {
                    cmd.push_back("-I" + (fs::path(d).is_absolute() ? fs::path(d) : from_base / d).string());
                }
            }
        };
        add_dirs(t, false);
        for (std::size_t l : t.links) add_dirs(g.targets[l], true);
        for (const auto& [name, value] : t.defines) cmd.push_back("-D" + name + "=" + value);
        if (commands) commands->push_back(CompileCommand{base.string(), src.string(), cmd});
        objects.push_back(obj.string());
        // Up to date: made by the same command, and newer than its source and every header it
        // included (the compiler's dependency file).
        std::string command_line;
        for (const std::string& a : cmd) command_line += a + "\n";
        fs::path deps = obj, recorded = obj;
        deps.replace_extension(".d");
        recorded.replace_extension(".cmd");
        auto up_to_date = [&] {
            if (!fs::exists(obj, ec)) return false;
            std::ifstream in(recorded, std::ios::binary);
            std::string   before = read_rest(in);
            if (before != command_line) return false;
            auto made = fs::last_write_time(obj, ec);
            if (fs::last_write_time(src, ec) > made) return false;
            std::ifstream d(deps);
            std::string   text = read_rest(d);
            std::size_t   colon = text.find(": ");
            if (colon == std::string::npos) return false;
            std::string file;
            auto check = [&] {
                if (file.empty()) return true;
                bool fresh = fs::exists(file, ec) && fs::last_write_time(file, ec) <= made;
                file.clear();
                return fresh;
            };
            for (std::size_t k = colon + 2; k < text.size(); ++k) {
                char ch = text[k];
                if (ch == '\\' && k + 1 < text.size() && text[k + 1] == ' ') {
                    file += ' ';
                    ++k;
                } else if (ch == '\\' && k + 1 < text.size() && text[k + 1] == '\n') {
                    ++k;
                } else if (ch == ' ' || ch == '\n' || ch == '\t') {
                    if (!check()) return false;
                } else {
                    file += ch;
                }
            }
            return check();
        };
        if (up_to_date()) {
            if (item) g_report->unit_up_to_date(*item, i);
            continue;
        }
        fs::remove(recorded, ec);
        std::vector<std::string> with_deps = cmd;
        for (std::string a : {std::string("-MMD"), std::string("-MF"), deps.string()}) with_deps.push_back(a);
        std::ofstream(recorded.string() + ".pending", std::ios::binary) << command_line;
        jobs.push_back(Job{with_deps, t.sources[i], {}, 0, i, obj.string()});
    }
    for (const Job& j : jobs) show_command(j.cmd);
    {
        std::atomic<std::size_t> next{0};
        unsigned                 workers = std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(), static_cast<unsigned>(jobs.size())));
        std::vector<std::thread> pool;
        for (unsigned w = 0; w < workers && !jobs.empty(); ++w) {
            pool.emplace_back([&] {
                for (std::size_t j; (j = next.fetch_add(1)) < jobs.size();) {
                    if (item) g_report->unit_start(*item, jobs[j].unit);
                    jobs[j].status = run_process(jobs[j].cmd, &jobs[j].output);
                    if (item) g_report->unit_done(*item, jobs[j].unit, jobs[j].status == 0);
                }
            });
        }
        for (std::thread& th : pool) th.join();
    }
    for (const Job& j : jobs) {   // the compilers' warnings are shown, and counted
        if (j.status != 0 || j.output.empty()) continue;
        s.err << j.output;
        std::size_t warnings = 0;
        for (std::size_t at = 0; (at = j.output.find("warning:", at)) != std::string::npos; at += 8) ++warnings;
        if (item) g_report->add_diagnostics(*item, 0, warnings);
    }
    for (const Job& j : jobs) {
        if (j.status == 0) continue;
        s.err << j.output << std::format("edaxc: error: target '{}': compiling '{}' failed\n", t.name, j.source);
        return fail();
    }
    for (const Job& j : jobs) {   // each object's command, for the next build's up-to-date check
        fs::path recorded = j.object;
        recorded.replace_extension(".cmd");
        fs::rename(recorded.string() + ".pending", recorded, ec);
    }
    std::size_t link_unit = item ? g_report->add_unit(*item, t.shared ? "link" : "archive") : 0;
    if (item) g_report->unit_start(*item, link_unit);
    auto finished = [&](const fs::path& made) {
        if (item) {
            g_report->unit_done(*item, link_unit);
            if (jobs.empty()) g_report->finish(*item, BuildReport::State::up_to_date, "up to date", shown_path(made));
            else g_report->finish(*item, BuildReport::State::done, {}, shown_path(made));
        }
        return made.string();
    };
    if (t.shared) {   // a shared library: lib<name>.so, and the programs find it next to it (rpath)
        fs::path library = out / "c" / opts.target / ("lib" + t.name + ".so");
        std::vector<std::string> link = compiler;
        link.push_back("-shared");
        link.push_back("-o");
        link.push_back(library.string());
        link.insert(link.end(), objects.begin(), objects.end());
        std::string output;
        show_command(link);
        if (run_process(link, &output) != 0) {
            s.err << output << std::format("edaxc: error: target '{}': linking the shared library failed\n", t.name);
            return fail();
        }
        return finished(library);
    }
    fs::path archive = out / "c" / opts.target / ("lib" + t.name + ".a");
    fs::remove(archive, ec);
    std::vector<std::string> ar = {"ar", "rcs", archive.string()};
    ar.insert(ar.end(), objects.begin(), objects.end());
    std::string output;
    show_command(ar);
    if (run_process(ar, &output) != 0) {
        s.err << output << std::format("edaxc: error: target '{}': 'ar' failed\n", t.name);
        return fail();
    }
    return finished(archive);
}

// `g.import_c_headers` (C interop): preprocesses the headers of each import of C target `t`
// with its include directories and defines, translates them and writes
// `<out>/c_import/<target>/<module>.edh` (and an empty `.ed`) plus a report. Returns the
// directory, a module root of the Edax targets that link `t`. Written again only when the
// translation changes, so the programs' caches stay valid.
std::string import_c_target(const Options& opts, const std::filesystem::path& root, const std::filesystem::path& out,
                            const ct::BuildGraph& g, const ct::BuildTarget& t, DriverStreams& s, bool& failed) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path base = t.base.empty() ? root : fs::path(t.base);
    fs::path dir  = out / "c_import" / t.name;
    fs::create_directories(dir, ec);
    std::vector<std::string> dirs;
    auto add_dirs = [&](const ct::BuildTarget& from, bool only_shared) {
        fs::path from_base = from.base.empty() ? root : fs::path(from.base);
        for (const auto& [d, shared] : from.include_dirs) {
            if (!only_shared || shared) dirs.push_back((fs::path(d).is_absolute() ? fs::path(d) : from_base / d).lexically_normal().string());
        }
        if (only_shared) {
            for (const std::string& d : from.user_include_dirs) dirs.push_back((fs::path(d).is_absolute() ? fs::path(d) : from_base / d).lexically_normal().string());
        }
    };
    add_dirs(t, false);
    for (std::size_t l : t.links) add_dirs(g.targets[l], true);
    for (const ct::BuildTarget::CImport& imp : t.c_imports) {
        // One translation unit including every header, preprocessed with -dD (keeps #defines).
        fs::path unit = dir / (imp.module + ".import.c");
        {
            std::ofstream u(unit);
            for (const std::string& h : imp.headers) u << "#include \"" << h << "\"\n";
        }
        std::vector<std::string> cmd = c_compiler(opts, t.language == "cpp");
        cmd.push_back("-E");
        cmd.push_back("-dD");
        if (t.language == "cpp") cmd.push_back("-xc");
        for (const std::string& d : dirs) cmd.push_back("-I" + d);
        cmd.push_back("-I" + base.string());
        for (const auto& [name, value] : t.defines) cmd.push_back("-D" + name + "=" + value);
        cmd.push_back(unit.string());
        std::string output;
        if (run_process(cmd, &output) != 0) {
            s.err << output << std::format("edaxc: error: target '{}': preprocessing the headers of '{}' failed\n", t.name, imp.module);
            failed = true;
            return {};
        }
        CImportOptions o;
        o.headers      = imp.headers;
        o.include_dirs = dirs;
        o.include_dirs.push_back(base.lexically_normal().string() + "/");
        o.ns           = imp.ns;
        {   // The macros' values: preprocessed again with a line per macro to expand it.
            std::ofstream(unit, std::ios::app) << c_macro_probe(output, o);
            std::vector<std::string> probe = cmd;
            probe.erase(std::find(probe.begin(), probe.end(), "-dD"));
            std::string expanded;
            if (run_process(probe, &expanded) == 0) o.macro_values = c_macro_values(expanded);
        }
        CImportResult r = import_c_headers(output, o);
        auto write_if_changed = [&](const fs::path& p, const std::string& text) {
            std::ifstream in(p);
            std::string   old = read_rest(in);
            if (in && old == text) return;
            std::ofstream(p) << text;
        };
        write_if_changed(dir / (imp.module + ".edh"), r.header);
        write_if_changed(dir / (imp.module + ".ed"), r.source);
        std::string report = std::format("# g.import_c_headers: {} ({} functions)\n", imp.module, r.functions);
        report += r.skipped.empty() ? "# everything was translated\n" : "# not translated:\n";
        for (const auto& [name, why] : r.skipped) report += std::format("{}: {}\n", name, why);
        write_if_changed(dir / (imp.module + ".report.txt"), report);
        fs::remove(unit, ec);
    }
    return dir.string();
}

// `g.wrap_cpp` (C interop): reads the headers of each wrap of C++ target `t` with clang's JSON
// AST, writes `<out>/cpp_wrap/<target>/<module>.edh`/`.ed`, the C wrappers' source (added
// to `extra_sources`, compiled into `t`) and a report. Returns the module root.
std::string wrap_cpp_target(const Options&, const std::filesystem::path& root, const std::filesystem::path& out,
                            const ct::BuildGraph& g, const ct::BuildTarget& t, DriverStreams& s, bool& failed,
                            std::vector<std::string>& extra_sources) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path base = t.base.empty() ? root : fs::path(t.base);
    fs::path dir  = out / "cpp_wrap" / t.name;
    fs::create_directories(dir, ec);
    std::vector<std::string> flags;
    auto add_dirs = [&](const ct::BuildTarget& from, bool only_shared) {
        fs::path from_base = from.base.empty() ? root : fs::path(from.base);
        for (const auto& [d, shared] : from.include_dirs) {
            if (!only_shared || shared) flags.push_back("-I" + (fs::path(d).is_absolute() ? fs::path(d) : from_base / d).string());
        }
        if (only_shared) {
            for (const std::string& d : from.user_include_dirs) flags.push_back("-I" + (fs::path(d).is_absolute() ? fs::path(d) : from_base / d).string());
        }
    };
    add_dirs(t, false);
    for (std::size_t l : t.links) add_dirs(g.targets[l], true);
    flags.push_back("-I" + base.string());
    for (const auto& [name, value] : t.defines) flags.push_back("-D" + name + "=" + value);
    std::string standard = t.standard.starts_with("cpp") ? "c++" + t.standard.substr(3) : "c++20";
    std::string clang    = "clang++";
    if (const char* env = std::getenv("EDAX_CLANGXX"); env && *env) clang = env;
    for (const ct::BuildTarget::CppWrap& w : t.cpp_wraps) {
        CppWrapOptions o{w.headers, w.classes, w.templates, w.module, w.ns};
        fs::path probe = dir / (w.module + ".probe.cpp");
        std::ofstream(probe) << cpp_wrap_probe(o);
        std::vector<std::string> dumps;
        std::vector<std::string> names = w.classes;
        names.insert(names.end(), w.templates.begin(), w.templates.end());
        for (const std::string& name : names) {
            std::vector<std::string> cmd = {clang, "-x", "c++", "-std=" + standard, "-fsyntax-only", "-Xclang", "-ast-dump=json",
                                            "-Xclang", "-ast-dump-filter=" + cpp_wrap_filter(name)};
            cmd.insert(cmd.end(), flags.begin(), flags.end());
            cmd.push_back(probe.string());
            std::string dump;
            int status = run_process(cmd, &dump);
            if (status != 0) {
                if (status == process_not_found) {
                    s.err << std::format("edaxc: error: target '{}': g.wrap_cpp reads C++ headers with clang, and '{}' was not found "
                                         "(install clang, or set EDAX_CLANGXX)\n", t.name, clang);
                } else {
                    s.err << dump.substr(0, dump.find("\n{")) << std::format("edaxc: error: target '{}': clang could not read the headers of '{}'\n", t.name, w.module);
                }
                failed = true;
                return {};
            }
            dumps.push_back(std::move(dump));
        }
        fs::remove(probe, ec);
        CppWrapResult r = cpp_wrap_generate(o, dumps);
        if (!r.error.empty()) {
            s.err << std::format("edaxc: error: target '{}': {}\n", t.name, r.error);
            failed = true;
            return {};
        }
        auto write_if_changed = [&](const fs::path& p, const std::string& text) {
            std::ifstream in(p);
            std::string   old = read_rest(in);
            if (in && old == text) return;
            std::ofstream(p) << text;
        };
        write_if_changed(dir / (w.module + ".edh"), r.header);
        write_if_changed(dir / (w.module + ".ed"), r.source);
        write_if_changed(dir / (w.module + "_wrappers.cpp"), r.wrappers);
        std::string report = std::format("# g.wrap_cpp: {}\n", w.module);
        report += r.report.empty() ? "# everything was wrapped under its own name\n" : "";
        for (const auto& [what, why] : r.report) report += std::format("{}: {}\n", what, why);
        write_if_changed(dir / (w.module + ".report.txt"), report);
        extra_sources.push_back((dir / (w.module + "_wrappers.cpp")).string());
    }
    return dir.string();
}

// The files of the project and of its build directory, with their modification time and size.
using FileSnapshot = std::map<std::string, std::pair<std::filesystem::file_time_type, std::uintmax_t>>;

FileSnapshot snapshot_files(const std::filesystem::path& root, const std::filesystem::path& out) {
    namespace fs = std::filesystem;
    FileSnapshot    files;
    std::error_code ec;
    for (const fs::path& top : {root, out}) {
        if (!fs::is_directory(top, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(top, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_directory(ec)) {
                std::string name = it->path().filename().string();
                if (name == ".git" || name == "deps" || name == "deps-build") it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            std::string key = fs::weakly_canonical(it->path(), ec).string();
            files[key]      = {fs::last_write_time(it->path(), ec), fs::file_size(it->path(), ec)};
        }
    }
    return files;
}

// Runs a step unless its outputs exist and are newer than its inputs (spec 25).
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
            s.err << std::format("edaxc: error: step '{}': its input '{}' does not exist\n", st.name, i);
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
        s.err << output << std::format("edaxc: error: step '{}' failed (exit status {})\n", st.name, status);
        return false;
    }
    for (const std::string& o : st.outputs) {
        if (!fs::exists(at(o), ec)) {
            s.err << std::format("edaxc: error: step '{}' did not write its declared output '{}'\n", st.name, o);
            return false;
        }
    }
    s.out << std::format("ran step '{}'\n", st.name);
    return true;
}

// A user target description (build system, "Target platforms"): a .ed file declaring
// `compile_time std::build::platform target = std::build::platform{ ... };`. Its triple, operating
// system, CPU level and capabilities replace the platform's.
std::optional<ExitCode> load_target_description(const Options& opts, DriverStreams& s) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_regular_file(opts.target, ec)) return usage_error(s, std::format("no target description '{}'", opts.target));
    SourceManager    sources;
    DiagnosticEngine diags;
    sema::Program    program = load_program(opts.target, LoadOptions{}, sources, diags);
    if (!diags.has_errors()) resolve_program(program, diags);
    if (!diags.has_errors()) check_types(program, sources, diags);
    const sema::Symbol* target = program.entry ? program.entry->item("target") : nullptr;
    if (!diags.has_errors() && (!target || !target->ct_value)) {
        diags.report(DiagCode::E4216, Span{program.entry && program.entry->source ? program.entry->source->id : 0, 0, 0},
                     "the target description declares no 'target'")
            .notes.push_back(Note{"write 'compile_time std::build::platform target = std::build::platform{ .triple = \"...\", ... };'", {}});
    }
    diags.sort_by_location();
    render_all(s.err, diags, sources, opts.render);
    if (diags.has_errors()) return ExitCode::compile_error;
    const ct::Value& v = *target->ct_value;
    TargetPlatform   p;
    std::size_t      i = 0;
    for (const sema::Symbol* m : v.type->symbol->member_order) {
        if (m->kind != sema::SymbolKind::field || m->is_static || i >= v.elements.size()) continue;
        const ct::Value& f = v.elements[i++];
        if (m->name == "triple") p.triple = f.string;
        if (m->name == "os" && f.symbol) p.os = f.symbol->name;
        if (m->name == "cpu" && f.symbol && f.symbol->name != "baseline") p.cpu = f.symbol->name;
        if (m->name == "capabilities") {
            for (const ct::Value& c : f.elements) {
                if (c.symbol) p.capabilities.push_back(c.symbol->name);
            }
        }
    }
    if (std::string error; !set_target_description(p, error)) return usage_error(s, std::format("{}: {}", opts.target, error));
    return std::nullopt;
}

// One evaluated build.ed: its graph and the program that owns its symbols.
struct EvaluatedBuild {
    std::filesystem::path root;
    ct::BuildGraph        graph;
    SourceManager         sources;
    sema::Program         program;
};

// Runs `<root>/build.ed`' build(g) with the compile-time interpreter (spec 25).
// The targets of a package that has no build.ed (packages, section 3): read from its
// CMakeLists.txt, or one header-only target. They join the graph; the package's foreign targets
// (`pkg.target("name")`) link them. Returns an error message, or "".
std::string command_output(const std::vector<std::string>& cmd, int* status = nullptr);

std::string add_package_targets(const Options& opts, const std::filesystem::path&, const std::filesystem::path& out,
                                ct::BuildGraph& graph, std::size_t d, DriverStreams& s, const std::map<std::string, std::string>& mode) {
    namespace fs = std::filesystem;
    std::error_code ec;
    (void)s;
    const ct::BuildDependency dep = graph.dependencies[d];
    fs::path dir = dep.dir;
    std::map<std::string, std::size_t> names;   // the package's target names → graph index
    std::size_t first = graph.targets.size();
    // CMAKE_BUILD_TYPE from the mode's settings (build modes, packages): unoptimized is Debug;
    // optimized is Release, MinSizeRel for `size`, RelWithDebInfo with debug info.
    auto setting = [&](const char* name) {
        auto it = mode.find(name);
        return it == mode.end() ? std::string() : it->second;
    };
    std::string optimization = setting("optimization").empty() ? (opts.mode == "debug" ? "none" : "speed") : setting("optimization");
    bool        debug_info   = !setting("debug_info").empty() && setting("debug_info") != "none";
    std::string build_type   = optimization == "none" ? "Debug" : debug_info ? "RelWithDebInfo" : optimization == "size" ? "MinSizeRel" : "Release";
    if (dep.build == "cmake_external") {   // real CMake as a build step (packages, section 4): the last resort
        if (!fs::is_regular_file(dir / "CMakeLists.txt", ec)) return std::format("the package '{}' has no CMakeLists.txt", dep.name);
        fs::path work    = out / "packages" / dep.name / ("cmake-" + build_type);
        fs::path build   = work / "build";
        fs::path install = work / "install";
        std::vector<std::string> configure = {"cmake", "-S", dir.string(), "-B", build.string(), "-DCMAKE_BUILD_TYPE=" + build_type,
                                              "-DCMAKE_INSTALL_PREFIX=" + install.string(), "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
                                              "-DCMAKE_C_COMPILER=" + c_compiler(opts, false).front(),
                                              "-DCMAKE_CXX_COMPILER=" + c_compiler(opts, true).front()};
        for (const auto& [k, v] : dep.options) configure.push_back("-D" + k + "=" + v);
        // The step's inputs: every file of the package (size and time) and the command.
        std::uint64_t h = 1469598103934665603ull;
        auto mix = [&](std::string_view text) {
            for (char ch : text) h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ull;
            h = (h ^ 0xff) * 1099511628211ull;
        };
        for (const std::string& a : configure) mix(a);
        std::vector<std::string> files;
        for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_directory(ec) && it->path().filename().string().starts_with(".")) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            auto when = fs::last_write_time(it->path(), ec).time_since_epoch().count();
            files.push_back(std::format("{} {} {}", it->path().lexically_relative(dir).generic_string(), it->file_size(ec), when));
        }
        std::sort(files.begin(), files.end());
        for (const std::string& f : files) mix(f);
        std::string stamp = std::format("{:016x}", h);
        std::string before;
        {
            std::ifstream in(work / "stamp");
            std::getline(in, before);
        }
        bool complete = true;
        for (const std::string& o : dep.external_outputs) complete = complete && fs::exists(install / o, ec);
        if (before != stamp || !complete) {
            if (command_output({"sh", "-c", "command -v cmake"}).empty()) {
                return std::format("the package '{}' is built with real CMake (cmake_external), but cmake is not on the PATH", dep.name);
            }
            fs::remove(work / "stamp", ec);
            fs::create_directories(work, ec);
            for (const std::vector<std::string>& cmd : {configure, std::vector<std::string>{"cmake", "--build", build.string(), "--parallel"},
                                                        std::vector<std::string>{"cmake", "--install", build.string()}}) {
                std::string output;
                if (run_process(cmd, &output) != 0) {
                    if (output.size() > 4000) output = "..." + output.substr(output.size() - 4000);
                    return std::format("the package '{}': '{} {}' failed (cmake_external):\n{}", dep.name, cmd[0], cmd[1], output);
                }
            }
            for (const std::string& o : dep.external_outputs) {
                if (!fs::exists(install / o, ec)) {
                    return std::format("the package '{}': CMake installed no '{}' (cmake_external's .outputs, relative to {})", dep.name, o,
                                       install.string());
                }
            }
            std::ofstream(work / "stamp") << stamp << "\n";
        }
        ct::BuildTarget t;
        t.kind = ct::BuildTarget::Kind::c_library;
        t.name = dep.name + ":" + dep.name;
        t.base = dir.string();
        t.span = dep.span;
        for (const std::string& o : dep.external_outputs) t.prebuilt.push_back((install / o).string());
        for (const std::string& i : dep.external_include_dirs) t.include_dirs.emplace_back((install / i).lexically_normal().string(), true);
        names[dep.name] = graph.targets.size();
        graph.targets.push_back(std::move(t));
    }
    bool cmake = dep.build == "cmake" || (dep.build == "automatic" && fs::is_regular_file(dir / "CMakeLists.txt", ec));
    if (dep.build == "cmake_external") {
    } else if (cmake) {
        if (!fs::is_regular_file(dir / "CMakeLists.txt", ec)) return std::format("the package '{}' has no CMakeLists.txt", dep.name);
        CMakeInput in;
        in.root    = dir.string();
        in.out_dir = (out / "packages" / dep.name).string();
        for (const auto& [k, v] : dep.options) in.options[k] = v;
        for (const auto& [k, t] : dep.uses) in.uses[k] = std::to_string(t);
        in.build_type  = build_type;
        in.system_name = target_platform().os == "windows" ? "Windows" : target_platform().os == "macos" ? "Darwin" : "Linux";
        in.compiles    = [&](const std::string& source) {
            fs::path probe = out / "packages" / (dep.name + ".check.c");
            fs::create_directories(probe.parent_path(), ec);
            std::ofstream(probe) << source;
            std::vector<std::string> cmd = c_compiler(opts, false);
            cmd.insert(cmd.end(), {"-fsyntax-only", probe.string()});
            std::string output;
            bool ok = run_process(cmd, &output) == 0;
            fs::remove(probe, ec);
            return ok;
        };
        CMakeResult r = read_cmake(in);
        if (!r.error.empty()) {
            std::string shown = r.error;   // the file relative to the project
            std::string prefix = dir.lexically_normal().string() + "/";
            if (shown.starts_with(prefix)) shown = (dep.kind == "local" ? dep.source : dep.path) + "/" + shown.substr(prefix.size());
            return std::format("the package '{}': {}\nnote: the CMake reader stops at what it can't translate, never guessing; describe the "
                               "package in packages/{}.ed (`edaxc package convert {}` writes one to start from)", dep.name, shown, dep.name, dep.name);
        }
        std::map<std::string, std::string> alias;
        for (const CMakeTarget& c : r.targets) {
            if (c.kind == CMakeTarget::Kind::alias) alias[c.name] = c.alias_of;
            if (c.kind == CMakeTarget::Kind::executable || c.kind == CMakeTarget::Kind::alias) continue;
            names[c.name] = graph.targets.size();
            ct::BuildTarget t;
            t.kind     = ct::BuildTarget::Kind::c_library;
            t.name     = dep.name + ":" + c.name;
            t.language = c.language;
            t.standard = c.standard;
            t.shared   = c.kind == CMakeTarget::Kind::shared_library;
            t.sources  = c.sources;
            t.base     = dir.string();
            t.span     = dep.span;
            for (const auto& [i, vis] : c.include_dirs) {
                if (vis == "users") t.user_include_dirs.push_back(i);
                else t.include_dirs.emplace_back(i, vis == "shared");
            }
            for (const auto& [def, vis] : c.defines) {
                std::size_t eq = def.find('=');
                t.defines.emplace_back(def.substr(0, eq), eq == std::string::npos ? "1" : def.substr(eq + 1));
            }
            graph.targets.push_back(std::move(t));
        }
        for (const auto& [a, target] : alias) {
            if (names.contains(target)) names[a] = names[target];
        }
        // Links: the package's own targets, `.uses`, else system libraries.
        for (const CMakeTarget& c : r.targets) {
            if (!names.contains(c.name) || c.kind == CMakeTarget::Kind::alias) continue;
            std::size_t self = names[c.name];
            for (const auto& [l, vis] : c.links) {
                std::size_t to = 0;
                bool        found = false;
                if (auto it = names.find(l); it != names.end()) {
                    to    = it->second;
                    found = true;
                }
                for (const auto& [u, t] : dep.uses) {
                    std::string bare = l.substr(0, l.find("::"));
                    if (!found && (u == l || u == bare)) {
                        to    = t;
                        found = true;
                    }
                }
                if (!found && opts.hermetic) {   // packages, section 5
                    return std::format("the package '{}': its target '{}' links '{}', which no '.uses' of the package answers "
                                       "(--hermetic: a build uses only what the project declares)", dep.name, c.name, l);
                }
                if (!found) {   // a system library looked up on the build machine
                    ct::BuildTarget sys;
                    sys.kind = ct::BuildTarget::Kind::system_library;
                    sys.name = l.substr(l.find("::") == std::string::npos ? 0 : l.rfind("::") + 2);
                    sys.span = dep.span;
                    graph.targets.push_back(sys);
                    to = graph.targets.size() - 1;
                }
                graph.targets[self].links.push_back(to);
            }
        }
    } else {   // headers only: one target named after the package
        std::string include = dep.build_arg;
        if (include.empty()) include = fs::is_directory(dir / "include", ec) ? "include" : ".";
        ct::BuildTarget t;
        t.kind = ct::BuildTarget::Kind::c_library;
        t.name = dep.name + ":" + dep.name;
        t.base = dir.string();
        t.span = dep.span;
        t.include_dirs.emplace_back((dir / include).lexically_normal().string(), true);
        names[dep.name] = graph.targets.size();
        graph.targets.push_back(std::move(t));
    }
    (void)first;
    for (ct::BuildTarget& t : graph.targets) {
        if (t.kind != ct::BuildTarget::Kind::foreign || t.dependency != d + 1 || !t.links.empty()) continue;
        auto it = names.find(t.name);
        if (it == names.end()) {
            std::string list;
            for (const auto& [n, i] : names) list += (list.empty() ? "" : ", ") + n;
            return std::format("the package '{}' has no target '{}' (its targets: {})", dep.name, t.name, list.empty() ? "none" : list);
        }
        t.links.push_back(it->second);
    }
    return {};
}

// The output of a command, without its trailing newline.
std::string command_output(const std::vector<std::string>& cmd, int* status) {
    std::string out;
    int         st = run_process(cmd, &out);
    if (status) *status = st;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

// Fetches a package (packages, section 2) into its `.path` and returns its directory. The
// version is pinned: a tag is resolved to its commit once and recorded in build.lock with the
// files' content hash. Files changed under the package's path are never overwritten.
std::string fetch_package(const std::filesystem::path& root, const std::filesystem::path& out, const ct::BuildDependency& d,
                          std::map<std::string, std::string>& lock, std::vector<std::string>& lock_errors, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (d.kind == "local") {
        fs::path dir = fs::weakly_canonical(fs::path(d.source).is_absolute() ? fs::path(d.source) : root / d.source, ec);
        if (!fs::is_directory(dir, ec)) {
            error = std::format("no directory '{}'", d.source);
            return {};
        }
        return dir.string();   // under the user's control: not locked
    }
    fs::path    dir = fs::absolute(root / d.path, ec).lexically_normal();
    std::string key = "package " + d.name;
    std::string locked = lock.contains(key) ? lock[key] : "";
    // One line per package: `package <name> <kind>|<source>|<commit or sha256>|<content hash>|<patches hash>`.
    std::string l_kind, l_source, l_version, l_hash, l_patches;
    {
        std::istringstream locked_in(locked);
        std::getline(locked_in, l_kind, '|');
        std::getline(locked_in, l_source, '|');
        std::getline(locked_in, l_version, '|');
        std::getline(locked_in, l_hash, '|');
        std::getline(locked_in, l_patches, '|');
    }
    // The patches' content, part of the lock.
    std::string patch_text;
    for (const std::string& p : d.patches) {
        std::ifstream in(root / p);
        if (!in) {
            error = std::format("the patch '{}' doesn't exist", p);
            return {};
        }
        std::stringstream ss;
        ss << in.rdbuf();
        patch_text += ss.str();
    }
    std::uint64_t fnv = 0xcbf29ce484222325ull;   // FNV-1a: the same on every machine
    for (char ch : patch_text) fnv = (fnv ^ static_cast<unsigned char>(ch)) * 0x100000001b3ull;
    std::string patches_hash = patch_text.empty() ? "-" : std::format("{:016x}", fnv);
    auto apply_patches = [&](const fs::path& at) -> bool {
        for (const std::string& p : d.patches) {
            std::string output;
            if (run_process({"git", "-C", at.string(), "apply", "--whitespace=nowarn", fs::absolute(root / p).string()}, &output) != 0) {
                error = std::format("the patch '{}' no longer applies: {}", p, output);
                return false;
            }
        }
        return true;
    };
    std::string version, hash;
    if (d.kind == "git") {
        std::string commit = d.commit;
        if (commit.empty()) {   // a tag: its commit, from build.lock or the remote
            if (l_kind == "git" && l_source == d.source + "@" + d.tag) commit = l_version;
            if (commit.empty()) {
                int         st = 0;
                std::string refs = command_output({"git", "ls-remote", d.source, "refs/tags/" + d.tag, "refs/tags/" + d.tag + "^{}"}, &st);
                std::string peeled, plain;
                std::istringstream lines(refs);
                for (std::string line; std::getline(lines, line);) {
                    std::string sha = line.substr(0, line.find('\t'));
                    if (line.ends_with("^{}")) peeled = sha;
                    else plain = sha;
                }
                commit = peeled.empty() ? plain : peeled;
                if (st != 0 || commit.empty()) {
                    error = std::format("the tag '{}' doesn't exist in {}", d.tag, d.source);
                    return {};
                }
            }
        }
        bool exists = fs::exists(dir / ".git", ec);
        std::string head = exists ? command_output({"git", "-C", dir.string(), "rev-parse", "HEAD"}) : "";
        if (exists && head == commit) {
            // At the locked version: any change besides the patches is the user's.
            std::string diff = command_output({"git", "-C", dir.string(), "diff", "HEAD"});
            std::string clean = command_output({"git", "-C", dir.string(), "status", "--porcelain", "--untracked-files=no"});
            if (d.patches.empty() && !clean.empty()) {
                error = std::format("files under '{}' differ from the locked version:\n{}\n(commit them as a patch in .patches, revert them, or use std::build::local while you work on them)",
                                    d.path, clean);
                return {};
            }
        } else {
            if (exists) {
                std::string clean = command_output({"git", "-C", dir.string(), "status", "--porcelain", "--untracked-files=no"});
                if (!clean.empty() && d.patches.empty()) {
                    error = std::format("files under '{}' differ from the locked version and would be overwritten:\n{}", d.path, clean);
                    return {};
                }
                std::string output;
                run_process({"git", "-C", dir.string(), "checkout", "--quiet", "--", "."}, &output);
            } else if (fs::exists(dir, ec) && !fs::is_empty(dir, ec)) {
                error = std::format("'{}' exists and is not a git checkout of the package", d.path);
                return {};
            } else {
                fs::create_directories(dir, ec);
                std::string output;
                if (run_process({"git", "-C", dir.string(), "init", "--quiet"}, &output) != 0 ||
                    run_process({"git", "-C", dir.string(), "remote", "add", "origin", d.source}, &output) != 0) {
                    error = "git failed: " + output;
                    return {};
                }
            }
            std::string output;
            if (run_process({"git", "-C", dir.string(), "fetch", "--quiet", "--depth", "1", "origin", commit}, &output) != 0 &&
                run_process({"git", "-C", dir.string(), "fetch", "--quiet", "origin"}, &output) != 0) {
                error = "git fetch failed: " + output;
                return {};
            }
            if (run_process({"git", "-C", dir.string(), "checkout", "--quiet", "--detach", commit}, &output) != 0) {
                error = "git checkout failed: " + output;
                return {};
            }
            if (!apply_patches(dir)) return {};
        }
        version = commit;
        hash    = command_output({"git", "-C", dir.string(), "rev-parse", "HEAD^{tree}"});
        std::string source = d.tag.empty() ? d.source : d.source + "@" + d.tag;
        std::string entry  = std::format("git|{}|{}|{}|{}", source, version, hash, patches_hash);
        if (!locked.empty() && l_kind == "git" && l_source == source && l_version == version && l_hash != hash) {
            lock_errors.push_back(std::format("the package '{}' changed: build.lock has {}, found {} for the same commit", d.name, l_hash, hash));
        }
        lock[key] = entry;
        return dir.string();
    }
    // An archive: downloaded, checked against its SHA-256, unpacked into the path.
    fs::path marker = dir / ".edaxc-package";
    {
        std::ifstream in(marker);
        std::string   have;
        in >> have;
        if (have == d.sha256 + patches_hash) {
            lock[key] = std::format("archive|{}|{}|{}|{}", d.source, d.sha256, d.sha256, patches_hash);
            return dir.string();
        }
    }
    if (fs::exists(dir, ec) && !fs::is_empty(dir, ec)) {
        error = std::format("'{}' exists and doesn't hold this archive; remove it to fetch the package again", d.path);
        return {};
    }
    fs::path download = out / "package-downloads" / (d.name + "-" + d.sha256.substr(0, 12) + fs::path(d.source).filename().string());
    fs::create_directories(download.parent_path(), ec);
    std::string output;
    if (!fs::exists(download, ec) && run_process({"curl", "-fsSL", "-o", download.string(), d.source}, &output) != 0) {
        error = "download failed: " + output;
        fs::remove(download, ec);
        return {};
    }
    std::string sum = command_output({"sha256sum", download.string()});
    sum             = sum.substr(0, sum.find(' '));
    if (sum != d.sha256) {
        fs::remove(download, ec);
        error = std::format("the archive's SHA-256 is {}, not the {} build.ed names", sum, d.sha256);
        return {};
    }
    fs::path unpack = out / "package-downloads" / (d.name + ".unpack");
    fs::remove_all(unpack, ec);
    fs::create_directories(unpack, ec);
    bool zip = download.string().ends_with(".zip");
    if (run_process(zip ? std::vector<std::string>{"unzip", "-q", download.string(), "-d", unpack.string()}
                        : std::vector<std::string>{"tar", "-xf", download.string(), "-C", unpack.string()}, &output) != 0) {
        error = "unpacking failed: " + output;
        return {};
    }
    fs::path top = unpack;   // an archive with one top directory: its contents
    std::vector<fs::path> entries;
    for (const auto& e : fs::directory_iterator(unpack, ec)) entries.push_back(e.path());
    if (entries.size() == 1 && fs::is_directory(entries[0], ec)) top = entries[0];
    fs::create_directories(dir.parent_path(), ec);
    fs::remove(dir, ec);
    fs::rename(top, dir, ec);
    if (ec) {
        error = "cannot move the unpacked files: " + ec.message();
        return {};
    }
    fs::remove_all(unpack, ec);
    if (!d.patches.empty()) {   // `git apply` works outside a repository too
        if (!apply_patches(dir)) return {};
    }
    std::ofstream(marker) << d.sha256 + patches_hash << "\n";
    lock[key] = std::format("archive|{}|{}|{}|{}", d.source, d.sha256, d.sha256, patches_hash);
    return dir.string();
}

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
        if (d.is_package) return fetch_package(b.root, out, d, lock, lock_errors, error);
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
    fs::path script = (b.root / "build.ed").lexically_normal();
    LoadOptions load;
    load.root = b.root.lexically_normal().string();
    {   // package descriptions (packages, section 3): packages/**/*.ed
        std::error_code ec;
        fs::path dir = b.root / "packages";
        if (fs::is_directory(dir, ec)) {
            for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (it->is_regular_file(ec) && it->path().extension() == ".ed") load.extra_modules.push_back(it->path().lexically_normal().string());
            }
            std::sort(load.extra_modules.begin(), load.extra_modules.end());
        }
    }
    b.program = load_program(script.string(), load, b.sources, diags);
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
    auto began = std::chrono::steady_clock::now();
    fs::path root = opts.root.empty() ? fs::path(".") : fs::path(opts.root);   // as written: diagnostics show it
    if (!fs::is_regular_file(root / "build.ed", ec)) {
        return usage_error(s, std::format("no build.ed in '{}': name a .ed file, or write a build.ed (spec 25)",
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

    // 1. Run build.ed' build(g) with the compile-time interpreter.
    std::deque<EvaluatedBuild> builds;
    EvaluatedBuild& main_build = builds.emplace_back();
    main_build.root = root;
    if (!evaluate_build(main_build, opts, out, s, lock, lock_errors)) return ExitCode::compile_error;
    ct::BuildGraph& graph = main_build.graph;
    sema::Program&  program = main_build.program;

    // Targets of dependencies: their own build.ed, evaluated in the same mode; their
    // targets join this graph (with paths relative to their root).
    for (std::size_t d = 0; d < graph.dependencies.size(); ++d) {
        bool used = false;
        for (const ct::BuildTarget& t : graph.targets) used = used || (t.kind == ct::BuildTarget::Kind::foreign && t.dependency == d + 1);
        if (!used) continue;
        fs::path dep_root = graph.dependencies[d].dir;
        const ct::BuildDependency& dependency = graph.dependencies[d];
        bool has_build = fs::is_regular_file(dep_root / "build.ed", ec);
        if (dependency.is_package && (dependency.build == "cmake" || dependency.build == "headers_only" || dependency.build == "cmake_external" ||
                                      (dependency.build == "automatic" && !has_build))) {
            std::string error = add_package_targets(opts, root, out, graph, d, s, program.mode_settings);
            if (!error.empty()) {
                s.err << "edaxc: error: " << error << "\n";
                return ExitCode::compile_error;
            }
            continue;
        }
        if (dependency.is_package && !has_build) {
            s.err << std::format("edaxc: error: the package '{}' has no build.ed (.build = std::build::package_build::edax)\n", dependency.name);
            return ExitCode::compile_error;
        }
        if (!has_build) {
            s.err << std::format("edaxc: error: the dependency '{}' has no build.ed; describe its sources in this build.ed "
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
            t.name = (graph.dependencies[d].is_package ? graph.dependencies[d].name : graph.dependencies[d].source) + ":" + t.name;
            graph.targets.push_back(std::move(t));
        }
        for (ct::BuildTarget& t : graph.targets) {
            if (t.kind != ct::BuildTarget::Kind::foreign || t.dependency != d + 1 || !t.links.empty()) continue;
            for (std::size_t i = 0; i < dep.graph.targets.size(); ++i) {
                if (dep.graph.targets[i].name == t.name) t.links.push_back(offset + i);
            }
            if (t.links.empty()) {
                s.err << std::format("edaxc: error: the dependency '{}' has no target '{}'\n", graph.dependencies[d].source, t.name);
                return ExitCode::compile_error;
            }
        }
    }
    if (!lock_errors.empty()) {
        for (const std::string& e : lock_errors) s.err << "edaxc: error: " << e << "\n";
        return ExitCode::compile_error;
    }
    if (lock != lock_before) {
        std::ofstream lock_out(root / "build.lock");
        lock_out << "# Written by edaxc (spec 25): the content hash of each dependency. Keep it in version control.\n";
        for (const auto& [key, hash] : lock) lock_out << key << " " << hash << "\n";
    }

    // 2. The mode: a built-in one, or a class build.ed added with g.add_mode.
    bool builtin = is_builtin_mode(opts.mode);
    bool added   = false;
    std::string names = "debug, development, release, release_with_debug_info";
    for (const sema::Symbol* m : graph.modes) {
        added = added || m->name == opts.mode;
        names += ", " + m->name;
    }
    if (!builtin && !added) return usage_error(s, std::format("unknown build mode '{}' ({})", opts.mode, names));
    for (const auto& [name, value] : opts.defines) {
        bool known = false;
        for (const ct::BuildOption& o : graph.options) known = known || o.name == name;
        if (!known) return usage_error(s, std::format("build.ed has no option '{}' (edaxc build --list)", name));
    }

    auto kind_name = [](ct::BuildTarget::Kind k) { return target_kind_text(k); };
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
        if (!found) return usage_error(s, std::format("build.ed has no target '{}' (edaxc build --list)", name));
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
    // Plugins are built with the programs: a program loads them from its `plugins` directory.
    if (!opts.compile_commands && opts.command != "test") {
        std::vector<std::size_t> plugins;
        for (std::size_t i = 0; i < graph.targets.size(); ++i) {
            if (graph.targets[i].kind == ct::BuildTarget::Kind::plugin && graph.targets[i].base.empty() &&
                std::find(selected.begin(), selected.end(), i) == selected.end()) {
                plugins.push_back(i);
            }
        }
        selected.insert(selected.begin(), plugins.begin(), plugins.end());
        // Shared Edax libraries are built before the programs that link them.
        std::vector<std::size_t> shared;
        std::function<void(std::size_t, std::set<std::size_t>&)> find_shared = [&](std::size_t i, std::set<std::size_t>& seen) {
            for (std::size_t l : graph.targets[i].links) {
                if (!seen.insert(l).second) continue;
                find_shared(l, seen);
                const ct::BuildTarget& lib = graph.targets[l];
                if (lib.kind == ct::BuildTarget::Kind::library && lib.shared && std::find(shared.begin(), shared.end(), l) == shared.end()) {
                    shared.push_back(l);
                }
            }
        };
        std::set<std::size_t> seen_shared;
        for (std::size_t i : selected) find_shared(i, seen_shared);
        for (std::size_t i : selected) {
            if (graph.targets[i].kind == ct::BuildTarget::Kind::library && graph.targets[i].shared &&
                std::find(shared.begin(), shared.end(), i) == shared.end()) {
                shared.push_back(i);
            }
        }
        std::erase_if(selected, [&](std::size_t i) { return std::find(shared.begin(), shared.end(), i) != shared.end(); });
        selected.insert(selected.begin(), shared.begin(), shared.end());
    }

    // The build output (docs/design/build/build-output.md): the checklist, build.log. Set by
    // build.ed's g.output(...) and g.details(...), overridden by the command line.
    std::ostream                real_out(s.out.rdbuf()), real_err(s.err.rdbuf());
    std::optional<BuildReport>  report;
    struct ReportEnder {
        std::optional<BuildReport>& r;
        bool                        ended = false;
        ~ReportEnder() {
            if (r && !ended) r->end(false);
            g_report = nullptr;
        }
    } ender{report};
    if (!opts.compile_commands) {
        const ct::BuildOutput& wanted = graph.output;
        ReportOptions          ro;
        std::string progress = !opts.progress.empty() ? opts.progress : !wanted.progress.empty() ? wanted.progress : "automatic";
        ro.progress = progress == "live" ? Progress::live : progress == "lines" ? Progress::lines : progress == "quiet" ? Progress::quiet : Progress::automatic;
        if (opts.details_given) {
            ro.details                   = opts.details;
            ro.details_from_command_line = true;
        } else {
            ro.details.insert(wanted.details.begin(), wanted.details.end());
            for (const ct::BuildTarget& t : graph.targets) {
                if (t.has_details) ro.target_details[t.name] = std::set<std::string>(t.details.begin(), t.details.end());
            }
        }
        ro.log_file    = wanted.log_file && !opts.no_log_file;
        ro.keep_logs   = wanted.keep_logs;
        ro.target_line = wanted.target_line;
        ro.done_line   = wanted.done_line;
        ro.source_row  = wanted.source_row;
        ro.last_line   = wanted.last_line;
        ro.failed_line = wanted.failed_line;
        ro.color       = opts.render.color && s.out_is_terminal;
        // The checklist: each selected target after the C/C++ libraries it links.
        std::vector<std::size_t>                  order;
        std::function<void(std::size_t, bool)>    add = [&](std::size_t i, bool selected_target) {
            if (std::find(order.begin(), order.end(), i) != order.end()) return;
            for (std::size_t l : graph.targets[i].links) add(l, false);
            ct::BuildTarget::Kind k = graph.targets[i].kind;
            if (selected_target || k == ct::BuildTarget::Kind::c_library) order.push_back(i);
        };
        for (std::size_t i : selected) add(i, true);
        std::string title = std::format("edaxc {}: {}, {} target{}", opts.command, opts.mode, order.size(), order.size() == 1 ? "" : "s");
        // `run`: the program's output is stdout, so the build's goes to stderr (as cargo's does).
        bool to_err = opts.command == "run";
        if (to_err) ro.color = opts.render.color;
        report.emplace(ro, to_err ? real_err : real_out, real_err, to_err ? s.err_is_terminal : s.out_is_terminal,
                       out / opts.mode / "build.log", out / opts.mode / ".edax" / "timings", title);
        g_report = &*report;
        std::string toolchain = std::format("    toolchain: {}, LLVM {}, C compiler {}, linker {}{}\n", std::string("edaxc ") + EDAX_VERSION, llvm_version(),
                                            command_text(c_compiler(opts, false)), linker_command(opts),
                                            opts.target.empty() ? std::string() : ", target " + opts.target);
        if (report->shows("", "toolchain")) report->print(toolchain, false);
        else report->log_only(toolchain);
        std::size_t script = report->add_item("build.ed", "build.ed");
        std::string summary = std::format("{} target{}", graph.targets.size(), graph.targets.size() == 1 ? "" : "s");
        if (!graph.dependencies.empty()) summary += std::format(", {} dependenc{}", graph.dependencies.size(), graph.dependencies.size() == 1 ? "y" : "ies");
        report->start(script);
        report->finish(script, BuildReport::State::done, summary, {},
                       std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count());
        std::string packages;   // the `packages` detail
        for (const ct::BuildDependency& d : graph.dependencies) {
            packages += std::format("    {} {}{}: {}\n", d.is_package ? "package" : "dependency", d.is_package ? d.name : d.source,
                                    d.tag.empty() ? std::string() : " " + d.tag, d.dir);
        }
        if (!packages.empty()) {
            if (report->shows("build.ed", "packages")) report->print(packages, false);
            else report->log_only(packages);
        }
        for (std::size_t i : order) {
            std::size_t item = report->add_item(graph.targets[i].name, kind_name(graph.targets[i].kind));
            for (std::size_t l : graph.targets[i].links) {
                if (std::find(order.begin(), order.end(), l) != order.end()) {
                    report->set_waiting_text(item, "waits for " + graph.targets[l].name);
                    break;
                }
            }
        }
        report->capture(s.out, s.err);
    }
    std::string optimization = program.mode_settings["optimization"];
    g_c_sanitize             = program.mode_settings["sanitize"];
    bool        lto          = program.mode_settings["link_time_optimization"] == "true";
    // A target with its own mode (`g.set_mode`, build system): its code is optimized as that mode says.
    auto own_optimization = [&](const ct::BuildTarget& target) {
        const ct::Value* v = target.mode_optimization.get();
        return v && v->symbol ? v->symbol->name : optimization;
    };
    std::vector<CompileCommand> commands;
    std::map<std::size_t, std::string> archives;
    std::map<std::size_t, std::string> imports;   // g.import_c_headers: the module root
    std::map<std::size_t, std::string> wraps;     // g.wrap_cpp: the module root
    std::set<std::size_t> steps_done;
    std::map<std::size_t, std::string> shared_built;   // shared Edax libraries: their lib<name>.so
    bool failed = false;
    // What a target links, directly or through other targets: archives, -l flags, and the
    // roots of dependencies whose Edax modules it imports.
    std::function<void(std::size_t, std::vector<std::string>&, std::vector<std::string>&, std::set<std::size_t>&)> collect =
        [&](std::size_t i, std::vector<std::string>& libs, std::vector<std::string>& roots, std::set<std::size_t>& seen) {
            for (std::size_t l : graph.targets[i].links) {
                if (!seen.insert(l).second) continue;
                const ct::BuildTarget& used = graph.targets[l];
                if (used.kind == ct::BuildTarget::Kind::c_library) {
                    if (!used.c_imports.empty()) {   // its headers as a Edax module
                        if (!imports.count(l)) imports[l] = import_c_target(opts, root, out, graph, used, s, failed);
                        if (!imports[l].empty() && std::find(roots.begin(), roots.end(), imports[l]) == roots.end()) roots.push_back(imports[l]);
                    }
                    if (!used.cpp_wraps.empty() && !wraps.count(l)) {   // its C++ classes as a Edax module
                        std::vector<std::string> extra;
                        wraps[l] = wrap_cpp_target(opts, root, out, graph, used, s, failed, extra);
                        if (!failed && !archives.count(l)) {
                            ct::BuildTarget with_wrappers = used;
                            with_wrappers.sources.insert(with_wrappers.sources.end(), extra.begin(), extra.end());
                            archives[l] = build_c_target(opts, root, out, graph, with_wrappers, own_optimization(with_wrappers), lto, s, failed, &commands);
                        }
                    }
                    if (wraps.count(l) && !wraps[l].empty() && std::find(roots.begin(), roots.end(), wraps[l]) == roots.end()) roots.push_back(wraps[l]);
                    if (!archives.count(l)) archives[l] = build_c_target(opts, root, out, graph, used, own_optimization(used), lto, s, failed, &commands);
                    if (!archives[l].empty()) libs.push_back(archives[l]);
                    libs.insert(libs.end(), used.prebuilt.begin(), used.prebuilt.end());   // cmake_external's outputs
                    // C++ code needs its runtime: the program is linked by the C compiler driver.
                    if (used.language == "cpp" && !archives[l].empty()) libs.push_back("-lstdc++");
                    if (used.shared && !archives[l].empty()) libs.push_back("-Wl,-rpath," + fs::path(archives[l]).parent_path().string());
                } else if (used.kind == ct::BuildTarget::Kind::library && used.shared) {   // a shared Edax library
                    if (auto it = shared_built.find(l); it != shared_built.end() && !it->second.empty()) {
                        libs.push_back(it->second);
                        libs.push_back("-Wl,-rpath,$ORIGIN");
                    }
                    if (!used.base.empty() && std::find(roots.begin(), roots.end(), used.base) == roots.end()) roots.push_back(used.base);
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
    // The steps of the selected targets run in parallel, each once its inputs that other steps
    // write are written (spec 25). Their output is printed in the order the graph lists them.
    if (!opts.compile_commands && !opts.list) {
        std::vector<std::size_t> todo;
        for (std::size_t i : selected) {
            for (std::size_t st : graph.targets[i].steps) {
                if (std::find(todo.begin(), todo.end(), st) == todo.end()) todo.push_back(st);
            }
        }
        std::sort(todo.begin(), todo.end());
        auto writes = [&](std::size_t a, std::size_t b) {   // step a writes an input of step b
            for (const std::string& o : graph.steps[a].outputs) {
                for (const std::string& in : graph.steps[b].inputs) {
                    if (fs::path(o).lexically_normal() == fs::path(in).lexically_normal()) return true;
                }
            }
            return false;
        };
        std::vector<std::ostringstream> outs(todo.size()), errs(todo.size());
        std::vector<int>                state(todo.size(), 0);   // 0 waiting, 1 done ok, 2 failed
        while (std::find(state.begin(), state.end(), 0) != state.end() && !failed) {
            std::vector<std::size_t> ready;
            for (std::size_t k = 0; k < todo.size(); ++k) {
                if (state[k] != 0) continue;
                bool blocked = false;
                for (std::size_t j = 0; j < todo.size(); ++j) {
                    if (j != k && state[j] != 1 && writes(todo[j], todo[k])) blocked = true;
                }
                if (!blocked) ready.push_back(k);
            }
            if (ready.empty()) {   // a cycle between steps
                s.err << "edaxc: error: the steps' inputs and outputs form a cycle\n";
                failed = true;
                break;
            }
            std::vector<std::thread> pool;
            std::vector<char>        ok(todo.size(), 0);
            FileSnapshot             before = snapshot_files(root, out);
            for (std::size_t k : ready) {
                pool.emplace_back([&, k] {
                    DriverStreams local{outs[k], errs[k]};
                    ok[k] = run_step(root, graph.steps[todo[k]], local) ? 1 : 0;
                });
            }
            for (std::thread& th : pool) th.join();
            {   // Steps are verified, not sandboxed (spec 25): any file they wrote must be declared.
                FileSnapshot             after = snapshot_files(root, out);
                std::set<std::string>    declared;
                std::vector<std::string> step_names;
                for (std::size_t k : ready) {
                    step_names.push_back("'" + graph.steps[todo[k]].name + "'");
                    for (const std::string& o : graph.steps[todo[k]].outputs) {
                        fs::path p = fs::path(o).is_absolute() ? fs::path(o) : root / o;
                        declared.insert(fs::weakly_canonical(p, ec).string());
                    }
                }
                std::vector<std::string> undeclared;
                for (const auto& [file, stamp] : after) {
                    auto it = before.find(file);
                    if ((it == before.end() || it->second != stamp) && !declared.contains(file)) undeclared.push_back(file);
                }
                for (const auto& [file, stamp] : before) {
                    if (!after.contains(file) && !declared.contains(file)) undeclared.push_back(file + " (deleted)");
                }
                if (!undeclared.empty()) {
                    std::sort(undeclared.begin(), undeclared.end());
                    std::string who;
                    for (std::size_t n = 0; n < step_names.size(); ++n) who += (n ? (n + 1 == step_names.size() ? " or " : ", ") : "") + step_names[n];
                    s.err << std::format("edaxc: error: step {} changed files it doesn't declare as outputs:\n", who);
                    for (const std::string& f : undeclared) {
                        std::error_code rel_ec;
                        fs::path shown = fs::path(f).lexically_relative(fs::weakly_canonical(root, rel_ec));
                        bool inside    = !shown.empty() && !shown.string().starts_with("..");
                        s.err << "  " << (inside ? shown.string() : f) << "\n";
                    }
                    s.err << "note: add them with g.add_outputs(step, [...]), or make the step write only its outputs\n";
                    failed = true;
                }
            }
            for (std::size_t k : ready) {
                s.out << outs[k].str();
                s.err << errs[k].str();
                state[k] = ok[k] ? 1 : 2;
                if (!ok[k]) failed = true;
                steps_done.insert(todo[k]);
            }
        }
        if (failed) return ExitCode::compile_error;
    }
    int         tests_failed = 0;
    std::size_t tests_run    = 0;
    // A failed target doesn't stop the others (like make -k); what needs it isn't built.
    // --stop-on-error starts nothing after the first failure.
    bool any_failed = false;
    std::function<std::optional<std::string>(std::size_t, std::set<std::size_t>&)> failed_dependency =
        [&](std::size_t i, std::set<std::size_t>& seen) -> std::optional<std::string> {
        for (std::size_t l : graph.targets[i].links) {
            if (!seen.insert(l).second) continue;
            if (archives.count(l) && archives[l].empty()) return graph.targets[l].name;
            if (auto deeper = failed_dependency(l, seen)) return deeper;
        }
        return std::nullopt;
    };
    auto not_built = [&](const ct::BuildTarget& t, const std::string& why) {
        any_failed = true;
        if (!report) return;
        std::optional<std::size_t> item = report->find(t.name);
        if (item) report->finish(*item, BuildReport::State::skipped, why);
    };
    for (std::size_t i : selected) {
        const ct::BuildTarget& t = graph.targets[i];
        if (any_failed && opts.stop_on_error) {
            not_built(t, "not built: the build stopped at the first error");
            continue;
        }
        run_steps(i);
        if (failed) return ExitCode::compile_error;
        if (t.kind == ct::BuildTarget::Kind::c_library) {
            if (!archives.count(i)) archives[i] = build_c_target(opts, root, out, graph, t, own_optimization(t), lto, s, failed, &commands);
            if (failed) any_failed = true, failed = false;
            continue;
        }
        bool plugin = t.kind == ct::BuildTarget::Kind::plugin || (t.kind == ct::BuildTarget::Kind::library && t.shared);
        if (plugin && opts.command == "run" && opts.targets.size() == 1 && opts.targets[0] == t.name) {
            return usage_error(s, std::format("'{}' is a {}, not a program", t.name, t.shared ? "shared library" : "plugin"));
        }
        if (!plugin && t.kind != ct::BuildTarget::Kind::executable && t.kind != ct::BuildTarget::Kind::test) {
            if (opts.command == "run") return usage_error(s, std::format("'{}' is a {}, not a program", t.name, kind_name(t.kind)));
            continue;   // a Edax library is compiled into the programs that import it
        }
        std::vector<std::string> libs, roots;
        std::set<std::size_t>    seen;
        collect(i, libs, roots, seen);
        if (opts.compile_commands) {
            if (failed) any_failed = true, failed = false;
            continue;
        }
        {
            std::set<std::size_t> visited;
            std::optional<std::string> needs = failed_dependency(i, visited);
            if (needs || failed) {
                failed = false;
                not_built(t, needs ? "not built: needs " + *needs : std::string("not built"));
                continue;
            }
        }
        std::string mode          = t.mode ? t.mode->name : opts.mode;
        bool        target_failed = false;
        // Edax libraries it links that have their own mode (`g.set_mode`): a program is compiled
        // as one, so their files get that mode's optimization, like `g.optimize_files`.
        ct::BuildTarget with_modes = t;
        {   // shared Edax libraries it links, directly or through others: their modules' code is theirs
            std::set<std::size_t>    linked;
            std::vector<std::size_t> todo(t.links.begin(), t.links.end());
            while (!todo.empty()) {
                std::size_t l = todo.back();
                todo.pop_back();
                if (!linked.insert(l).second || l >= graph.targets.size()) continue;
                const ct::BuildTarget& lib = graph.targets[l];
                todo.insert(todo.end(), lib.links.begin(), lib.links.end());
                if (lib.kind != ct::BuildTarget::Kind::library || !lib.shared) continue;
                fs::path dir = ((lib.base.empty() ? root : fs::path(lib.base)) / lib.root).parent_path();
                with_modes.shared_library_dirs.push_back(fs::weakly_canonical(dir, ec).generic_string());
            }
        }
        {
            std::set<std::size_t>    linked;
            std::vector<std::size_t> todo(t.links.begin(), t.links.end());
            while (!todo.empty()) {
                std::size_t l = todo.back();
                todo.pop_back();
                if (!linked.insert(l).second || l >= graph.targets.size()) continue;
                const ct::BuildTarget& lib = graph.targets[l];
                todo.insert(todo.end(), lib.links.begin(), lib.links.end());
                if (lib.kind != ct::BuildTarget::Kind::library || !lib.mode_optimization) continue;
                sema::FileSetting fsetting;
                fsetting.name  = "std::build::optimization";
                fsetting.value = lib.mode_optimization;
                fs::path dir = ((lib.base.empty() ? root : fs::path(lib.base)) / lib.root).parent_path();
                std::error_code walk_ec;
                for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, walk_ec);
                     it != fs::recursive_directory_iterator(); it.increment(walk_ec)) {
                    if (walk_ec) break;
                    std::string ext = it->path().extension().string();
                    if (ext == ".ed" || ext == ".edh") fsetting.files.push_back(fs::weakly_canonical(it->path(), walk_ec).generic_string());
                }
                with_modes.file_settings.push_back(std::move(fsetting));
            }
        }
        std::string exe           = build_edax_target(opts, root, out, with_modes, mode, libs, roots, s, target_failed);
        if (target_failed) {
            any_failed = true;
            continue;
        }
        if (t.kind == ct::BuildTarget::Kind::library && t.shared) {
            shared_built[i] = exe;
            continue;
        }
        if (plugin || opts.command == "build") continue;
        if (opts.command == "run") {
            if (any_failed) continue;
            if (report) report->end(true), ender.ended = true;
            s.out.flush();
            s.err.flush();
            std::vector<std::string> cmd = {exe};
            cmd.insert(cmd.end(), opts.program_args.begin(), opts.program_args.end());
            return static_cast<ExitCode>(run_process(cmd, nullptr));
        }
        // test: a program whose main returns 0, or its `@std::test` functions one by one (spec 29)
        std::string output;
        int status = run_process({"env", "EDAX_TEST_LIST=1", exe}, &output);
        if (status == 0 && output.starts_with("edax-tests:\n")) {
            std::vector<std::string> test_names;
            std::istringstream       lines(output.substr(std::string_view("edax-tests:\n").size()));
            for (std::string line; std::getline(lines, line);) {
                if (!line.empty()) test_names.push_back(line);
            }
            for (const std::string& name : test_names) {
                std::string one;
                int         st = run_process({"env", "EDAX_TEST_RUN=" + name, exe}, &one);
                s.out << std::format("{} {}::{}\n", st == 0 ? "PASS" : "FAIL", t.name, name);
                if (st != 0) {
                    s.out << one;
                    ++tests_failed;
                }
                ++tests_run;
            }
            continue;
        }
        ++tests_run;
        s.out << std::format("{} {}\n", status == 0 ? "PASS" : "FAIL", t.name);
        if (status != 0) {
            s.out << output;
            ++tests_failed;
        }
    }
    if (report) {
        report->end(!any_failed && !failed);
        ender.ended = true;
    }
    if (any_failed) return ExitCode::compile_error;
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
        std::size_t total = tests_run;
        s.out << std::format("{} of {} tests passed\n", total - static_cast<std::size_t>(tests_failed), total);
        return tests_failed == 0 ? ExitCode::success : ExitCode::compile_error;
    }
    return ExitCode::success;
}

// The doc comment of `compile_time std::diagnostic <name>` in a module's text, or nothing.
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
        while (first > 0 && trim(lines[first - 1]).starts_with("//")) --first;
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

// `edaxc explain gfx::deprecated`: the doc comment of a declared warning id (spec 23). The std
// library's ids come from edaxc itself; others from `gfx.edh` (or `.ed`) in the current
// directory, the project root.
ExitCode explain_id(std::string_view id, DriverStreams& s) {
    // An id is the diagnostic's namespace path (spec 23, 30): `gfx::deprecated`, `legacy`.
    std::size_t      split = id.rfind("::");
    std::string_view ns    = split == std::string_view::npos ? std::string_view() : id.substr(0, split);
    std::string_view name  = split == std::string_view::npos ? id : id.substr(split + 2);
    std::optional<std::string> doc;
    bool in_std = ns == "std" || ns.starts_with("std::");
    if (in_std) {
        for (const StdModule& m : std_modules()) {
            if (!doc) doc = diagnostic_doc(m.header_text, name);
            if (!doc) doc = diagnostic_doc(m.source_text, name);
        }
    } else {
        // The project's files (the current directory and below), the module named like the namespace first.
        namespace fs = std::filesystem;
        std::vector<fs::path> files;
        std::string           guess(ns);
        for (std::size_t at = guess.find("::"); at != std::string::npos; at = guess.find("::", at)) guess.replace(at, 2, "/");
        if (!guess.empty()) {
            files.push_back(guess + ".edh");
            files.push_back(guess + ".ed");
        }
        std::error_code ec;
        for (fs::recursive_directory_iterator it(".", fs::directory_options::skip_permission_denied, ec), end; it != end && !ec;
             it.increment(ec)) {
            if (it->is_directory(ec) && (it->path().filename() == "build" || it->path().filename().string().starts_with("."))) {
                it.disable_recursion_pending();
                continue;
            }
            std::string ext = it->path().extension().string();
            if (ext == ".edh" || ext == ".ed") files.push_back(it->path());
        }
        for (const fs::path& f : files) {
            std::ifstream in(f);
            if (!in) continue;
            std::string text = read_rest(in);
            if (!ns.empty() && text.find("namespace " + std::string(ns.substr(0, ns.find("::")))) == std::string::npos) continue;
            doc = diagnostic_doc(text, name);
            if (doc) break;
        }
    }
    if (!doc) return usage_error(s, std::format("unknown diagnostic id '{}' (no 'compile_time std::diagnostic {}' found)", id, name));
    s.out << std::format("{} (warning id)\n{}", id, *doc);
    return ExitCode::success;
}

ExitCode run_explain(std::span<const std::string_view> args, DriverStreams& s) {
    if (args.empty()) {
#define EDAX_DIAG(code, severity, title) \
    s.out << std::format("{}  {:<8} {}\n", #code, #severity, title);
#include "edax/diagnostic_codes.def"
#undef EDAX_DIAG
        return ExitCode::success;
    }
    if (args.size() > 1) {
        return usage_error(s, "'explain' takes at most one diagnostic code");
    }
    if (args[0].find("::") != std::string_view::npos) return explain_id(args[0], s);
    std::optional<DiagCode> code = code_from_name(args[0]);
    if (!code) {
        if (!args[0].empty() && !(args[0][0] == 'E' || args[0][0] == 'W') ) return explain_id(args[0], s);   // a global id
        return usage_error(s, std::format("unknown diagnostic code '{}'", args[0]));
    }
    s.out << std::format("{} ({}): {}\n", code_name(*code), to_string(code_severity(*code)),
                         code_title(*code));
    return ExitCode::success;
}

} // namespace

// --- edaxc package convert (packages, section 4) ---------------------------------------------
// Reads a package's CMakeLists.txt for each platform, leniently, and writes what it found as a
// package description: ordinary build-API calls, platform differences as `if`s on
// g.target_platform().os, and every command the reader couldn't translate as a TODO.
struct ConvertedPackage {
    std::string description;   // packages/<name>.ed
    std::string report;        // packages/<name>.convert.md
    std::size_t targets = 0;
    std::size_t todos   = 0;
    std::string generated_from;   // the configured files to copy (the host's), or ""
};

static ConvertedPackage convert_cmake_package(const Options& opts, const ct::BuildDependency& dep, const std::filesystem::path& out,
                                       const std::string& generated_dir /* "packages/<name>.generated" */) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = fs::path(dep.dir).lexically_normal();
    struct Platform {
        const char* os;       // std::build::os
        const char* system;   // CMAKE_SYSTEM_NAME
        CMakeResult result;
        fs::path    out_dir;
    };
    std::vector<Platform> platforms = {{"linux", "Linux", {}, {}}, {"windows", "Windows", {}, {}}, {"macos", "Darwin", {}, {}}};
    for (Platform& p : platforms) {
        CMakeInput in;
        in.root       = dir.string();
        p.out_dir     = out / "packages" / dep.name / (std::string("convert-") + p.os);
        fs::remove_all(p.out_dir, ec);
        in.out_dir    = p.out_dir.string();
        in.build_type = "Release";
        in.system_name = p.system;
        in.lenient    = true;
        for (const auto& [k, v] : dep.options) in.options[k] = v;
        for (const auto& [k, t] : dep.uses) in.uses[k] = std::to_string(t);
        in.compiles = [&](const std::string& source) {   // answered by the host's compiler
            fs::path probe = out / "packages" / (dep.name + ".check.c");
            fs::create_directories(probe.parent_path(), ec);
            std::ofstream(probe) << source;
            std::vector<std::string> cmd = c_compiler(opts, false);
            cmd.insert(cmd.end(), {"-fsyntax-only", probe.string()});
            std::string output;
            bool ok = run_process(cmd, &output) == 0;
            fs::remove(probe, ec);
            return ok;
        };
        p.result = read_cmake(in);
    }
    const Platform& host = platforms[0];
    auto shown_file = [&](const std::string& file) {
        std::string rel = fs::path(file).lexically_normal().lexically_relative(dir).generic_string();
        return rel.empty() || rel.starts_with("..") ? file : rel;
    };
    auto ident = [](const std::string& name) {
        std::string r = "t_";
        for (char ch : name) r += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
        return r;
    };
    auto quote = [](const std::string& text) {
        std::string r = "\"";
        for (char ch : text) {
            if (ch == '"' || ch == '\\') r += '\\';
            r += ch;
        }
        return r + "\"";
    };
    // A path the reader found, as an expression of the description.
    auto path_expr = [&](const std::string& path, const Platform& p) -> std::string {
        fs::path full = fs::path(path).lexically_normal();
        std::string in_package = full.lexically_relative(dir).generic_string();
        if (!in_package.empty() && !in_package.starts_with("..")) return std::format("p.at({})", quote(in_package));
        std::string generated = full.lexically_relative(p.out_dir).generic_string();
        if (!generated.empty() && !generated.starts_with("..")) {
            return quote(generated == "." ? generated_dir : generated_dir + "/" + generated);
        }
        return quote(path);
    };
    // Each target's lines (without indentation), per platform: what all platforms share is
    // written once, the rest under `if`s.
    struct Lines {
        std::string              declaration;
        std::vector<std::string> body;
        std::vector<std::string> files;   // sources in the package, quoted
        std::string              file;
        int                      line = 0;
    };
    std::vector<std::string> order;
    std::map<std::string, std::map<std::string, Lines>> per;   // target → os → lines
    std::map<std::string, std::string> alias_of;
    std::vector<std::string> notes;   // for the report
    std::set<std::string>    executables;
    for (const Platform& p : platforms) {
        std::set<std::string> names;
        for (const CMakeTarget& t : p.result.targets) names.insert(t.name);
        for (const CMakeTarget& t : p.result.targets) {
            if (t.kind == CMakeTarget::Kind::alias) {
                alias_of[t.name] = t.alias_of;
                continue;
            }
            if (t.kind == CMakeTarget::Kind::executable) {
                executables.insert(t.name);
                continue;
            }
            if (std::find(order.begin(), order.end(), t.name) == order.end()) order.push_back(t.name);
            Lines& l = per[t.name][p.os];
            l.file = t.file;
            l.line = t.line;
            std::string standard = t.standard.empty() ? "" : std::format(", .standard = std::build::cpp_standard::{}", t.standard);
            l.declaration = std::format("std::build::target {} = g.{}(std::build::c_target{{ .name = {}, .language = std::build::language::{}{} }});",
                                        ident(t.name), t.kind == CMakeTarget::Kind::shared_library ? "add_shared_library" : "add_c_library",
                                        quote(t.name), t.language, standard);
            std::vector<std::string> package_files, other_files;
            for (const std::string& f : t.sources) {
                std::string e = path_expr(f, p);
                if (e.starts_with("p.at(")) package_files.push_back(e.substr(5, e.size() - 6));
                else other_files.push_back(e);
            }
            l.files = package_files;
            for (const std::string& f : other_files) l.body.push_back(std::format("g.add_sources({}, {});", ident(t.name), f));
            for (const auto& [i, vis] : t.include_dirs) {
                l.body.push_back(std::format("g.add_include_dir({}, {}, std::build::visibility::{});", ident(t.name), path_expr(i, p), vis));
            }
            for (const auto& [def, vis] : t.defines) {
                std::size_t eq = def.find('=');
                l.body.push_back(std::format("g.define({}, {}, {});", ident(t.name), quote(def.substr(0, eq)),
                                             quote(eq == std::string::npos ? "1" : def.substr(eq + 1))));
            }
            for (const auto& [link, vis] : t.links) {
                std::string to = alias_of.contains(link) ? alias_of[link] : link;
                bool own = names.contains(to);
                bool used = false;
                for (const auto& [u, idx] : dep.uses) used = used || u == link || u == link.substr(0, link.find("::"));
                if (own) {
                    l.body.push_back(std::format("g.link({}, {});", ident(t.name), ident(to)));
                } else if (used) {
                    l.body.push_back(std::format("// TODO(convert): links '{}', which build.ed's .uses answers: link that target to "
                                                 "this package's users in build.ed", link));
                } else {
                    std::string lib = link.substr(link.find("::") == std::string::npos ? 0 : link.rfind("::") + 2);
                    if (lib.starts_with("-l")) lib = lib.substr(2);
                    l.body.push_back(std::format("g.link({}, g.add_system_library(std::build::system_library{{ .name = {} }}));",
                                                 ident(t.name), quote(lib)));
                }
            }
            for (const auto& [o, vis] : t.options) notes.push_back(std::format("`{}`: the compile option `{}` ({}) is not carried over: the "
                                                                               "build mode sets warnings and optimization", t.name, o, vis));
        }
    }
    // Events in reading order: targets and TODOs, by file then line.
    std::map<std::string, std::size_t> file_index;
    for (const Platform& p : platforms) {
        for (const std::string& f : p.result.files) file_index.emplace(fs::path(f).lexically_normal().string(), file_index.size());
    }
    auto position = [&](const std::string& file, int line) {
        auto it = file_index.find(fs::path(file).lexically_normal().string());
        return std::make_pair(it == file_index.end() ? file_index.size() : it->second, line);
    };
    struct Todo {
        CMakeTodo                todo;
        std::vector<std::string> platforms;
    };
    std::vector<Todo> todos;
    for (const Platform& p : platforms) {
        for (const CMakeTodo& t : p.result.todos) {
            auto it = std::find_if(todos.begin(), todos.end(), [&](const Todo& o) { return o.todo.file == t.file && o.todo.line == t.line; });
            if (it == todos.end()) todos.push_back(Todo{t, {p.os}});
            else it->platforms.push_back(p.os);
        }
    }
    for (const Platform& p : platforms) {   // a reader that stopped anyway (a file it can't read)
        if (!p.result.error.empty()) {
            CMakeTodo t;
            t.message = p.result.error;
            auto it = std::find_if(todos.begin(), todos.end(), [&](const Todo& o) { return o.todo.message == t.message; });
            if (it == todos.end()) todos.push_back(Todo{t, {p.os}});
            else it->platforms.push_back(p.os);
        }
    }
    struct Event {
        std::pair<std::size_t, int> at;
        bool                        is_target;
        std::size_t                 index;
    };
    std::vector<Event> events;
    for (std::size_t i = 0; i < order.size(); ++i) {
        const Lines& any = per[order[i]].begin()->second;
        events.push_back(Event{position(any.file, any.line), true, i});
    }
    for (std::size_t i = 0; i < todos.size(); ++i) events.push_back(Event{position(todos[i].todo.file, todos[i].todo.line), false, i});
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.at < b.at; });

    std::string ns;
    for (char ch : dep.name) ns += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
    std::string d;
    d += std::format("// packages/{}.ed: the package '{}', described for edaxc's build (packages, section 3).\n", dep.name, dep.name);
    d += std::format("// Written by `edaxc package convert {}` from its CMakeLists.txt; finish the TODO(convert)s, then it is yours.\n", dep.name);
    d += "import std;\n\n";
    d += std::format("namespace build::packages::{} {{\n", ns);
    d += "    compile_time void describe(mut std::build::graph& g, std::build::package& p) {\n";
    bool uses_os = false;
    std::string body;
    auto os_condition = [](const std::vector<std::string>& oses) {
        std::string c;
        for (const std::string& o : oses) c += (c.empty() ? "" : " || ") + std::format("os == std::build::os::{}", o);
        return c;
    };
    auto add_block = [&](const std::vector<std::string>& oses, const std::vector<std::string>& lines, const std::string& indent) {
        if (lines.empty()) return;
        if (oses.size() == platforms.size()) {
            for (const std::string& l : lines) body += indent + l + "\n";
            return;
        }
        uses_os = true;
        body += indent + "if (" + os_condition(oses) + ") {\n";
        for (const std::string& l : lines) body += indent + "    " + l + "\n";
        body += indent + "}\n";
    };
    bool first = true;
    for (const Event& e : events) {
        if (!first) body += "\n";
        first = false;
        if (!e.is_target) {
            const Todo& t = todos[e.index];
            std::string where = t.todo.file.empty() ? "" : std::format("{}:{}: ", shown_file(t.todo.file), t.todo.line);
            body += std::format("        // TODO(convert): {}{}", where, t.todo.message);
            if (t.platforms.size() != platforms.size()) {
                std::string list;
                for (const std::string& o : t.platforms) list += (list.empty() ? "" : ", ") + o;
                body += std::format(" (read for {})", list);
            }
            body += "\n";
            std::istringstream text(t.todo.text);
            for (std::string line; std::getline(text, line);) body += "        //     " + line + "\n";
            continue;
        }
        const std::string& name = order[e.index];
        std::map<std::string, Lines>& by_os = per[name];
        std::vector<std::string> present;
        for (const Platform& p : platforms) {
            if (by_os.contains(p.os)) present.push_back(p.os);
        }
        const Lines& base = by_os.at(present.front());
        std::string indent = "        ";
        bool wrapped = present.size() != platforms.size();
        if (wrapped) {   // a target only some platforms have
            uses_os = true;
            body += indent + "if (" + os_condition(present) + ") {\n";
            indent += "    ";
        }
        body += indent + std::format("// {}:{}\n", shown_file(base.file), base.line);
        body += indent + base.declaration + "\n";
        // Lines every present platform has, in the first platform's order; then each group of
        // platforms' own lines.
        auto add_sources = [&](const std::vector<std::string>& files) {
            std::string list;
            for (const std::string& f : files) list += (list.empty() ? "" : ", ") + f;
            return std::format("g.add_sources({}, p.files([{}]));", ident(name), list);
        };
        std::vector<std::string> common_files;
        for (const std::string& f : base.files) {
            bool everywhere = true;
            for (const std::string& o : present) {
                const std::vector<std::string>& fs_ = by_os.at(o).files;
                everywhere = everywhere && std::find(fs_.begin(), fs_.end(), f) != fs_.end();
            }
            if (everywhere) common_files.push_back(f);
        }
        if (!common_files.empty()) body += indent + add_sources(common_files) + "\n";
        for (const std::string& o : present) {   // the other sources as a line of each platform's own
            std::vector<std::string> extra;
            for (const std::string& f : by_os.at(o).files) {
                if (std::find(common_files.begin(), common_files.end(), f) == common_files.end()) extra.push_back(f);
            }
            if (!extra.empty()) by_os.at(o).body.insert(by_os.at(o).body.begin(), add_sources(extra));
        }
        std::vector<std::string> common;
        for (const std::string& l : base.body) {
            bool everywhere = true;
            for (const std::string& o : present) {
                const std::vector<std::string>& b = by_os.at(o).body;
                everywhere = everywhere && std::find(b.begin(), b.end(), l) != b.end();
            }
            if (everywhere) common.push_back(l);
        }
        for (const std::string& l : common) body += indent + l + "\n";
        std::map<std::vector<std::string>, std::vector<std::string>> groups;   // lines → platforms
        std::map<std::string, std::vector<std::string>> own;
        for (const std::string& o : present) {
            for (const std::string& l : by_os.at(o).body) {
                if (std::find(common.begin(), common.end(), l) == common.end()) own[o].push_back(l);
            }
        }
        for (const auto& [o, lines] : own) groups[lines].push_back(o);
        for (const auto& [lines, oses] : groups) {
            std::vector<std::string> ordered;   // platforms in their usual order
            for (const Platform& p : platforms) {
                if (std::find(oses.begin(), oses.end(), p.os) != oses.end()) ordered.push_back(p.os);
            }
            if (wrapped && ordered.size() == present.size()) {
                for (const std::string& l : lines) body += indent + l + "\n";
            } else if (wrapped) {
                body += indent + "if (" + os_condition(ordered) + ") {\n";
                for (const std::string& l : lines) body += indent + "    " + l + "\n";
                body += indent + "}\n";
            } else {
                add_block(ordered, lines, indent);
            }
        }
        body += indent + std::format("p.export_target({});\n", ident(name));
        for (const auto& [a, to] : alias_of) {
            if (to == name) body += indent + std::format("p.export_target({}, {});\n", ident(name), quote(a));
        }
        if (wrapped) body += "        }\n";
    }
    if (uses_os) d += "        std::build::os os = g.target_platform().os;\n";
    if (!body.empty()) d += (uses_os ? "\n" : "") + body;
    d += "    }\n}\n";

    // The report.
    std::string r;
    r += std::format("# Converting the package '{}'\n\n", dep.name);
    r += std::format("`edaxc package convert {}` read the package's CMake files for linux, windows and macos and wrote "
                     "`packages/{}.ed`. This report says what each command became.\n\n", dep.name, dep.name);
    r += "## Options\n\n";
    if (dep.options.empty()) r += "None (each option at its default).\n";
    for (const auto& [k, v] : dep.options) r += std::format("- `{}` = `{}`\n", k, v);
    r += "\nThe files were read with `CMAKE_BUILD_TYPE=Release`; feature checks (`check_include_file`, ...) were answered by this "
         "machine's compiler for every platform.\n\n## Files read\n\n";
    for (const std::string& f : host.result.files) r += std::format("- `{}`\n", shown_file(f));
    r += "\n## Commands\n\n| Where | Command | Became |\n|---|---|---|\n";
    for (const auto& [where, became] : host.result.commands) {
        std::string w = where;
        std::size_t colon = w.find(':', w.find(':') + 1);
        std::string loc = colon == std::string::npos ? w : w.substr(0, colon);
        std::string cmd = colon == std::string::npos ? "" : w.substr(colon + 2);
        std::size_t c2 = loc.rfind(':');
        if (c2 != std::string::npos) loc = shown_file(loc.substr(0, c2)) + loc.substr(c2);
        std::string b = became;
        for (std::string from : {host.out_dir.string() + "//", host.out_dir.string() + "/", host.out_dir.string()}) {
            for (std::size_t at; (at = b.find(from)) != std::string::npos;) b.replace(at, from.size(), generated_dir + (from.ends_with("/") ? "/" : ""));
        }
        for (char& ch : b) {
            if (ch == '|') ch = '/';
        }
        r += std::format("| `{}` | `{}` | {} |\n", loc, cmd, b);
    }
    r += "\n## TODOs\n\n";
    if (todos.empty()) r += "None: the description is complete.\n";
    for (const Todo& t : todos) {
        std::string where = t.todo.file.empty() ? "" : std::format("`{}:{}` ", shown_file(t.todo.file), t.todo.line);
        r += std::format("- {}{}", where, t.todo.message);
        if (t.platforms.size() != platforms.size()) {
            std::string list;
            for (const std::string& o : t.platforms) list += (list.empty() ? "" : ", ") + o;
            r += std::format(" (read for {})", list);
        }
        r += ". The reader doesn't translate it: write what it does with build-API calls (a `g.add_step` for a command "
             "that makes files), or leave it out if the build doesn't need it.\n";
    }
    if (!executables.empty()) {
        r += "\n## Left out\n\n";
        for (const std::string& e : executables) r += std::format("- the executable `{}` (a package provides libraries)\n", e);
    }
    if (!notes.empty()) {
        r += executables.empty() ? "\n## Left out\n\n" : "";
        for (const std::string& n : notes) r += "- " + n + "\n";
    }
    if (!fs::is_empty(host.out_dir, ec) && fs::is_directory(host.out_dir, ec)) {
        r += std::format("\n## Configured files\n\n`configure_file` outputs are copied to `{}` (as configured for linux with the "
                         "options above). Re-run the converter when the options change.\n", generated_dir);
    }
    ConvertedPackage c;
    c.description    = d;
    c.report         = r;
    c.targets        = order.size();
    c.todos          = todos.size();
    c.generated_from = fs::is_directory(host.out_dir, ec) && !fs::is_empty(host.out_dir, ec) ? host.out_dir.string() : "";
    return c;
}

std::string version_string() { return std::string("edaxc ") + EDAX_VERSION; }

// `edaxc package list|fetch|status|update|convert [name]` (packages, section 8). Evaluating build.ed
// fetches the packages; this prints what it found.
ExitCode run_package(std::span<const std::string_view> args, DriverStreams& s) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (args.empty()) return usage_error(s, "edaxc package list|fetch|status|update|convert [name]");
    std::string sub(args[0]);
    if (sub != "list" && sub != "fetch" && sub != "status" && sub != "update" && sub != "convert") {
        return usage_error(s, std::format("unknown package command '{}'", sub));
    }
    Options opts;
    opts.command = "build";
    std::string only, new_tag, new_commit, to;
    bool        compare = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        std::string_view a = args[i];
        auto value = [&](std::string_view option, std::string& into) {   // `--tag=X` or `--tag X`
            if (a.starts_with(std::string(option) + "=")) {
                into = std::string(a.substr(option.size() + 1));
                return true;
            }
            if (a == option && i + 1 < args.size()) {
                into = std::string(args[++i]);
                return true;
            }
            return false;
        };
        if (a.starts_with("--root=")) opts.root = std::string(a.substr(7));
        else if (a.starts_with("--out-dir=")) opts.out_dir = std::string(a.substr(10));
        else if (a.starts_with("--mode=")) opts.mode = std::string(a.substr(7));
        else if (a == "--hermetic") opts.hermetic = true;
        else if (sub == "update" && value("--tag", new_tag)) continue;
        else if (sub == "update" && value("--commit", new_commit)) continue;
        else if (sub == "convert" && a == "--compare") compare = true;
        else if (sub == "convert" && value("--to", to)) continue;
        else if (!a.starts_with("-") && only.empty()) only = std::string(a);
        else return usage_error(s, std::format("unknown option '{}'", a));
    }
    if ((sub == "update" || sub == "convert") && only.empty()) return usage_error(s, std::format("edaxc package {}: which package?", sub));
    if (sub == "update" && new_tag.empty() == new_commit.empty()) {
        return usage_error(s, "edaxc package update NAME --tag TAG | --commit COMMIT");
    }
    fs::path root = opts.root.empty() ? fs::path(".") : fs::path(opts.root);
    if (!fs::is_regular_file(root / "build.ed", ec)) return usage_error(s, "no build.ed here (--root=DIR)");
    fs::path out = opts.out_dir.empty() ? root / "build" : fs::absolute(opts.out_dir, ec);
    std::map<std::string, std::string> lock;
    {
        std::ifstream in(root / "build.lock");
        for (std::string line; std::getline(in, line);) {
            std::size_t at = line.rfind(' ');
            if (at != std::string::npos && !line.starts_with("#")) lock[line.substr(0, at)] = line.substr(at + 1);
        }
    }
    std::map<std::string, std::string> before = lock;
    std::vector<std::string>           lock_errors;
    EvaluatedBuild b;
    b.root = root;
    if (!evaluate_build(b, opts, out, s, lock, lock_errors)) return ExitCode::compile_error;
    for (const std::string& e : lock_errors) s.err << "edaxc: error: " << e << "\n";
    if (!lock_errors.empty()) return ExitCode::compile_error;
    if (lock != before) {
        std::ofstream lock_out(root / "build.lock");
        lock_out << "# Written by edaxc (spec 25): the content hash of each dependency. Keep it in version control.\n";
        for (const auto& [key, hash] : lock) lock_out << key << " " << hash << "\n";
    }
    if (sub == "update" || sub == "convert") {
        const ct::BuildDependency* found = nullptr;
        for (const ct::BuildDependency& d : b.graph.dependencies) {
            if (d.is_package && d.name == only) found = &d;
        }
        if (!found) return usage_error(s, std::format("build.ed has no package '{}'", only));
        const ct::BuildDependency& d = *found;
        if (sub == "update") {   // edits the version in build.ed, then fetches it (packages, section 8)
            if (d.kind != "git") return usage_error(s, std::format("the package '{}' is not a git package: edit its .source in build.ed", d.name));
            const SourceFile& file = b.sources.file(d.span.file);
            std::string text(file.text());
            std::string call = text.substr(d.span.begin, d.span.end - d.span.begin);
            std::regex  version(R"re(\.(tag|commit)(\s*)=(\s*)"[^"]*")re");
            std::smatch m;
            std::ptrdiff_t matches = std::distance(std::sregex_iterator(call.begin(), call.end(), version), std::sregex_iterator());
            if (matches != 1 || !std::regex_search(call, m, version)) {
                return usage_error(s, std::format("cannot find the version of '{}' in {}: edit it there", d.name, file.path()));
            }
            std::string field = new_tag.empty() ? "commit" : "tag";
            std::string updated = std::format(".{}{}={}\"{}\"", field, m[2].str(), m[3].str(), new_tag.empty() ? new_commit : new_tag);
            call.replace(static_cast<std::size_t>(m.position(0)), static_cast<std::size_t>(m.length(0)), updated);
            text.replace(d.span.begin, d.span.end - d.span.begin, call);
            std::string old_version = d.commit.empty() ? "tag " + d.tag : "commit " + d.commit;
            {
                std::ofstream w(file.path());
                w << text;
            }
            s.out << std::format("{}: {} -> {} {} in {}\n", d.name, old_version, field, new_tag.empty() ? new_commit : new_tag, file.path());
            std::vector<std::string> again = {"fetch", d.name};
            if (!opts.root.empty()) again.push_back("--root=" + opts.root);
            if (!opts.out_dir.empty()) again.push_back("--out-dir=" + opts.out_dir);
            std::vector<std::string_view> views(again.begin(), again.end());
            return run_package(views, s);
        }
        // convert
        fs::path dir = d.dir;
        if (!fs::is_regular_file(dir / "CMakeLists.txt", ec)) return usage_error(s, std::format("the package '{}' has no CMakeLists.txt to convert", d.name));
        fs::path into = to.empty() ? root / "packages" : fs::path(to);
        std::string generated = "packages/" + d.name + ".generated";
        ConvertedPackage c = convert_cmake_package(opts, d, out, generated);
        fs::path description = into / (d.name + ".ed");
        if (compare) {   // the new conversion against the description you have
            if (!fs::is_regular_file(description, ec)) {
                return usage_error(s, std::format("there is no {} to compare with: run 'edaxc package convert {}' first", description.string(), d.name));
            }
            fs::path fresh = out / "packages" / d.name / (d.name + ".converted.ed");
            fs::create_directories(fresh.parent_path(), ec);
            std::ofstream(fresh) << c.description;
            std::string output;
            int status = run_process({"diff", "-u", "--label", description.string(), "--label", "converted from " + d.name + "/CMakeLists.txt",
                                      description.string(), fresh.string()}, &output);
            if (status == 0) s.out << std::format("{} matches the package's CMake files\n", description.string());
            else s.out << output;
            return ExitCode::success;
        }
        if (fs::exists(description, ec)) {
            return usage_error(s, std::format("{} exists: compare it with the package's CMake files with --compare, or delete it to "
                                              "convert again", description.string()));
        }
        fs::create_directories(into, ec);
        std::ofstream(description) << c.description;
        std::ofstream(into / (d.name + ".convert.md")) << c.report;
        if (!c.generated_from.empty()) {
            fs::path copy = into / (d.name + ".generated");
            fs::create_directories(copy, ec);
            fs::copy(c.generated_from, copy, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        }
        s.out << std::format("wrote {} ({} target{}, {} TODO{}) and {}\n", description.string(), c.targets, c.targets == 1 ? "" : "s", c.todos,
                             c.todos == 1 ? "" : "s", (into / (d.name + ".convert.md")).string());
        if (c.todos > 0) s.out << "finish the TODO(convert)s; from then on the package is built from the description\n";
        return ExitCode::success;
    }
    std::size_t shown = 0;
    for (const ct::BuildDependency& d : b.graph.dependencies) {
        if (!d.is_package || (!only.empty() && d.name != only)) continue;
        ++shown;
        fs::path dir = d.dir;
        std::string changes;
        if (fs::exists(dir / ".git", ec) && d.kind == "git") {
            std::string output;
            run_process({"git", "-C", dir.string(), "status", "--porcelain", "--untracked-files=no"}, &output);
            changes = output;
        }
        std::string how = d.build == "script" ? "script (" + d.build_arg + ")"
                        : d.build != "automatic" ? d.build
                        : fs::is_regular_file(dir / "build.ed", ec)    ? "edax (build.ed)"
                        : fs::is_regular_file(dir / "CMakeLists.txt", ec) ? "cmake (CMakeLists.txt, read by edaxc)"
                                                                           : "headers_only";
        std::string version = d.kind == "git" ? (d.commit.empty() ? d.tag + " = " : "") + (lock.contains("package " + d.name) ? [&] {
                                  std::string v = lock["package " + d.name];
                                  std::size_t a = v.find('|', v.find('|') + 1);
                                  return v.substr(a + 1, 12);
                              }() : d.commit)
                            : d.kind == "archive" ? "sha256 " + d.sha256.substr(0, 12) : "local";
        if (sub == "status") {
            if (!changes.empty() && d.patches.empty()) s.out << std::format("{}: files differ from build.lock:\n{}", d.name, changes);
            continue;
        }
        if (sub == "fetch") {
            s.out << std::format("fetched '{}' ({}) into {}\n", d.name, version, d.kind == "local" ? d.source : d.path);
            continue;
        }
        s.out << std::format("{}\n  source:  {} {}\n  version: {}\n  path:    {}\n  build:   {}\n", d.name, d.kind, d.source, version,
                             d.kind == "local" ? d.source : d.path, how);
        if (!d.patches.empty()) {
            s.out << "  patches:";
            for (const std::string& p : d.patches) s.out << " " << p;
            s.out << "\n";
        }
        s.out << (changes.empty() || !d.patches.empty() ? "  no local changes\n" : "  local changes:\n" + changes);
    }
    if (!only.empty() && shown == 0) return usage_error(s, std::format("build.ed has no package '{}'", only));
    if (shown == 0 && sub != "status") s.out << "build.ed declares no packages\n";
    return ExitCode::success;
}

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
    if (command == "package") {
        return run_package(args.subspan(1), streams);
    }
    if (command == "registry") {
        if (args.size() < 2 || (args[1] != "opengl" && args[1] != "vulkan") || (args.size() > 2 && args[2] != "--source")) {
            return usage_error(streams, "usage: edaxc registry opengl|vulkan [--source]");
        }
        try {
            GeneratedStdModule m = args[1] == "opengl" ? generate_opengl_module(std_registry("gl.xml")) : generate_vulkan_module(std_registry("vk.xml"));
            streams.out << (args.size() > 2 ? m.source : m.header);
            streams.err << std::format("{}: {} commands, {} constants, {} structs (registry: {})\n", args[1], m.commands, m.constants,
                                       m.types, m.registry_version);
        } catch (const std::exception& e) {
            streams.err << std::format("edaxc: error: the {} registry: {}\n", args[1], e.what());
            return ExitCode::compile_error;
        }
        return ExitCode::success;
    }
    if (command == "lex" || command == "parse" || command == "check" || command == "symbols" || command == "typed" ||
        command == "build" || command == "run" || command == "emit-llvm" || command == "test" ||
        command == "insights" || command == "expand" || command == "simd-report" || command == "definition" ||
        command == "explain-checks") {
        Options opts;
        opts.command = command;
        if (std::optional<ExitCode> failed = parse_options(args.subspan(1), opts, streams)) {
            return *failed;
        }
        if (opts.target.ends_with(".ed")) {
            if (std::optional<ExitCode> failed = load_target_description(opts, streams)) return *failed;
        } else if (std::string error; !set_build_target(opts.target, error)) {
            return usage_error(streams, error);
        }
        if (std::string error; !opts.cpu.empty() && !set_build_cpu(opts.cpu, error)) {
            std::string levels;
            for (const std::string& l : cpu_levels()) levels += (levels.empty() ? "" : ", ") + l;
            return usage_error(streams, std::format("{} (levels for this target: {})", error, levels));
        }
        if (command == "run" && !target_is_host()) {
            return usage_error(streams, std::format("a program built for '{}' can't run here; use 'build'", opts.target));
        }
        if (opts.project) return run_project(opts, streams);
        if (command == "test") return usage_error(streams, "'test' runs the tests of build.ed: run it without a file");
        return run_frontend(opts, streams);
    }
    return usage_error(streams, std::format("unknown command '{}'", command));
}

} // namespace edax
