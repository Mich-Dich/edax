#include "edax/driver.hpp"
#include "edax_test.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

using namespace edax;

namespace {

struct Run {
    ExitCode    code;
    std::string out;
    std::string err;
};

Run edaxc(std::vector<std::string_view> args) {
    std::ostringstream out, err;
    ExitCode           code = run_driver(args, DriverStreams{out, err, false});
    return Run{code, out.str(), err.str()};
}

// A temporary source file that is deleted again at the end of the test.
struct TempFile {
    std::filesystem::path path;
    TempFile(std::string_view name, std::string_view text)
        : path(std::filesystem::temp_directory_path() / name) {
        std::ofstream(path) << text;
    }
    ~TempFile() { std::filesystem::remove(path); }
    TempFile(const TempFile&)            = delete;
    TempFile& operator=(const TempFile&) = delete;
    std::string str() const { return path.string(); }
};

} // namespace

TEST_CASE("driver: --version") {
    Run r = edaxc({"--version"});
    CHECK_EQ(r.code, ExitCode::success);
    CHECK_EQ(r.out, version_string() + "\n");
    CHECK(r.out.starts_with("edaxc "));
}

TEST_CASE("driver: no arguments prints usage to stderr") {
    Run r = edaxc({});
    CHECK_EQ(r.code, ExitCode::usage_error);
    CHECK(r.err.starts_with("usage:"));
}

TEST_CASE("driver: unknown command and option") {
    CHECK_EQ(edaxc({"frobnicate"}).code, ExitCode::usage_error);
    CHECK_EQ(edaxc({"check", "--nope", "a.ed"}).code, ExitCode::usage_error);
    CHECK_EQ(edaxc({"check"}).code, ExitCode::usage_error);
    CHECK_EQ(edaxc({"check", "a.ed", "b.ed"}).code, ExitCode::usage_error);
}

TEST_CASE("driver: missing file and wrong extension") {
    Run missing = edaxc({"check", "/no/such/file.ed"});
    CHECK_EQ(missing.code, ExitCode::compile_error);
    CHECK(missing.err.find("E0001") != std::string::npos);
    Run wrong_ext = edaxc({"check", "main.cpp"});
    CHECK_EQ(wrong_ext.code, ExitCode::compile_error);
    CHECK(wrong_ext.err.find("E0002") != std::string::npos);
}

TEST_CASE("driver: check on a clean file is silent") {
    TempFile f("edax_driver_ok.ed", "u32 main() { return 0; }\n");
    Run      r = edaxc({"check", f.str()});
    CHECK_EQ(r.code, ExitCode::success);
    CHECK_EQ(r.out, std::string());
    CHECK_EQ(r.err, std::string());
}

TEST_CASE("driver: check reports errors with exit code 1") {
    TempFile f("edax_driver_bad.ed", "u32 x = 0x;\n");
    Run      r = edaxc({"check", "--diagnostic-format=short", f.str()});
    CHECK_EQ(r.code, ExitCode::compile_error);
    CHECK_EQ(r.err, f.str() + ":1:9: error[E1009]: expected hexadecimal digits after '0x'\n");
}

TEST_CASE("driver: warnings alone do not fail the build") {
    TempFile f("edax_driver_warn.ed", "/* a /* b */\n");
    Run      r = edaxc({"check", f.str()});
    CHECK_EQ(r.code, ExitCode::success);
    CHECK(r.err.find("warning[W1001]") != std::string::npos);
}

TEST_CASE("driver: lex dumps tokens to stdout") {
    TempFile f("edax_driver_lex.ed", "u32 x");
    Run      r = edaxc({"lex", f.str()});
    CHECK_EQ(r.code, ExitCode::success);
    CHECK_EQ(r.out, std::string("1:1     kw_u32                  u32\n"
                                "1:5     identifier              x\n"
                                "1:6     eof\n"));
}

TEST_CASE("driver: --color=always adds ANSI escapes") {
    TempFile f("edax_driver_color.ed", "$");
    CHECK(edaxc({"check", "--color=always", f.str()}).err.find("\x1b[") != std::string::npos);
    CHECK(edaxc({"check", "--color=never", f.str()}).err.find("\x1b[") == std::string::npos);
}

TEST_CASE("driver: explain") {
    Run one = edaxc({"explain", "E1003"});
    CHECK_EQ(one.code, ExitCode::success);
    CHECK_EQ(one.out, std::string("E1003 (error): unterminated string literal\n"));
    CHECK_EQ(edaxc({"explain", "E9999"}).code, ExitCode::usage_error);
    CHECK(edaxc({"explain"}).out.find("W1001") != std::string::npos);
}
