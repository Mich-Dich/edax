// edaxc entry point. All logic lives in edax::run_driver (compiler/src/driver.cpp) so it can be
// unit-tested; this file only adapts argv/streams and catches compiler bugs.

#include "edax/driver.hpp"

#include <exception>
#include <iostream>
#include <string_view>
#include <unistd.h>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string_view> args(argv + 1, argv + argc);
    try {
        edax::DriverStreams streams{std::cout, std::cerr, isatty(STDERR_FILENO) == 1, isatty(STDOUT_FILENO) == 1};
        return static_cast<int>(edax::run_driver(args, streams));
    } catch (const std::exception& e) {
        std::cerr << "edaxc: internal compiler error: " << e.what() << '\n';
    } catch (...) {
        std::cerr << "edaxc: internal compiler error: unknown exception\n";
    }
    return static_cast<int>(edax::ExitCode::internal_error);
}
