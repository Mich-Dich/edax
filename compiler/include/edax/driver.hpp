#pragma once
// The edaxc command line, as a library function so unit tests can call it without a process.
//
//   edaxc lex <file>       print the token stream
//   edaxc check <file>     run every implemented front-end phase, report diagnostics
//   edaxc explain [code]   describe a diagnostic code (no code: list all)
//   edaxc --version | --help
//
// Options for lex/check:
//   --diagnostic-format=human|short   (default human)
//   --color=auto|always|never         (default auto: color only if stderr is a terminal)

#include <iosfwd>
#include <span>
#include <string>
#include <string_view>

namespace edax {

// Process exit codes. Stable: tests and scripts rely on them.
enum class ExitCode : int {
    success        = 0,   // no errors (warnings allowed)
    compile_error  = 1,   // the input has errors
    usage_error    = 2,   // bad command line
    internal_error = 3,   // compiler bug (uncaught exception, failed invariant)
};

struct DriverStreams {
    std::ostream& out;
    std::ostream& err;
    bool          err_is_terminal = false;
};

ExitCode run_driver(std::span<const std::string_view> args, DriverStreams streams);

// "edaxc 0.0.1"
std::string version_string();

} // namespace edax
