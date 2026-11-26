#include "project.hpp"
#include "edax/driver.hpp"
#include "edax/llvm_backend.hpp"
#include "edax_test.hpp"

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <cstdlib>

using edax_test::Project;

namespace {

// Generated LLVM IR for a one-file program, plus the diagnostic codes.
struct Generated {
    std::string ir;
    std::string codes;
};

Generated generate(const std::map<std::string, std::string>& files) {
    Project p(files);
    std::string ir = p.diags.has_errors() ? std::string() : edax::generate_llvm_ir(p.program, p.sources, p.diags);
    // Each Project lives in its own temporary directory; locations spell it out.
    for (std::size_t at; (at = ir.find(p.root.string())) != std::string::npos;) ir.replace(at, p.root.string().size(), "ROOT");
    return Generated{ir, p.codes()};
}

Generated generate(const std::string& main) { return generate({{"main.ed", main}}); }

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

} // namespace

TEST_CASE("codegen: a program gets a C main that calls the Edax main") {
    Generated g = generate("u32 main() { return 0; }");
    CHECK_EQ(g.codes, std::string());
    CHECK(contains(g.ir, "define internal i32 @r_4main4main()"));
    CHECK(contains(g.ir, "define i32 @main(i32 %0, ptr %1)"));
    CHECK(contains(g.ir, "call i32 @edax_exit_status("));
}

TEST_CASE("codegen: only functions reachable from main are generated") {
    Generated g = generate("u32 used() { return 1; }\nu32 unused() { return 2; }\nu32 main() { return used(); }");
    CHECK(contains(g.ir, "@r_4main4used"));
    CHECK(!contains(g.ir, "@r_4main6unused"));
}

TEST_CASE("codegen: names are length-prefixed per module, namespace, type and overload") {
    Generated g = generate({{"main.ed", "import geo::shapes;\nu32 main() { return geo::f(1u32) + geo::f(true); }\n"},
                            {"geo/shapes.edh", "namespace geo {\nu32 f(u32 x);\nu32 f(bool b);\n}\n"},
                            {"geo/shapes.ed", "namespace geo {\nu32 f(u32 x) { return x; }\nu32 f(bool b) { return 0; }\n}\n"}});
    CHECK_EQ(g.codes, std::string());
    CHECK(contains(g.ir, "@r_3geo6shapes3geo1f_0("));
    CHECK(contains(g.ir, "@r_3geo6shapes3geo1f_1("));
}

TEST_CASE("codegen: methods take their object by pointer") {
    Generated g = generate("struct c { u32 v = 0; void inc() mut { this.v += 1; } }\n"
                           "u32 main() { mut c x = c{}; x.inc(); return x.v; }");
    CHECK_EQ(g.codes, std::string());
    CHECK(contains(g.ir, "define internal void @r_4main1c3inc(ptr"));
    CHECK(contains(g.ir, "call void @r_4main1c3inc(ptr"));
}

TEST_CASE("codegen: integer arithmetic is checked, wrapping operators are not") {
    Generated g = generate("u32 main() { u32 a = 1; u32 b = a + 2; u32 c = a +% 2; return b - c; }");
    CHECK(contains(g.ir, "@llvm.uadd.with.overflow.i32"));
    CHECK(contains(g.ir, "@llvm.usub.with.overflow.i32"));
    CHECK(contains(g.ir, "add i32 "));
}

TEST_CASE("codegen: operands with side effects are evaluated left to right") {
    Generated g = generate("u32 f(u32 x) { return x; }\nu32 main() { return f(1) + f(2); }");
    // Both calls are emitted in order before the addition.
    std::size_t first  = g.ir.find("call i32 @r_4main1f(i32 1)");
    std::size_t second = g.ir.find("call i32 @r_4main1f(i32 2)");
    CHECK(first != std::string::npos);
    CHECK(second != std::string::npos);
    CHECK(first < second);
}

TEST_CASE("codegen: missing main and unsupported constructs are errors") {
    CHECK_EQ(generate("u32 helper() { return 0; }").codes, std::string("E6002"));
    CHECK_EQ(generate("void main() { auto f = [](u32 x) { return x; }; }").codes, std::string());   // lambdas generate
    CHECK_EQ(generate("extern(std::build::language::cpp) void f();\nvoid main() { unsafe { f(); } }").codes, std::string("E6001"));
    // Unsupported code that main never reaches is fine.
    CHECK_EQ(generate("extern(std::build::language::cpp) void f();\nvoid unused() { unsafe { f(); } }\nvoid main() { }").codes,
             std::string());
}

TEST_CASE("codegen: output is deterministic") {
    std::string src = "struct a { u32 x = 0; }\nstruct b { a inner = default; }\nenum e : u8 { one, two(u32) }\n"
                      "u32 main() { b v = b{}; e w = e::two(3); return v.inner.x; }";
    // Names are hashes of what they name; string constants hold the (temporary) project path,
    // so their hashes differ between the two projects.
    auto without_hashes = [](const std::string& ir) { return std::regex_replace(ir, std::regex(R"(\.[0-9a-f]{16})"), ".H"); };
    CHECK_EQ(without_hashes(generate(src).ir), without_hashes(generate(src).ir));
}

TEST_CASE("codegen: names come from what they name, not from the order they were made in") {
    // Adding a function that uses a new string and a new instance doesn't rename anything in main.
    std::string body = "u32 main() { std::darray<u32> xs = default; std::println(\"one\"); return 0; }";
    Generated   a    = generate("import std;\n" + body);
    Generated   b    = generate("import std;\nvoid other() { std::darray<u8> ys = default; std::println(\"two\"); }\n" + body);
    auto main_of = [](const std::string& ir) {
        std::size_t at = ir.find("@r_4main4main(");
        return at == std::string::npos ? std::string() : ir.substr(at, ir.find("\n}\n", at) - at);
    };
    CHECK(!main_of(a.ir).empty());
    CHECK_EQ(main_of(a.ir), main_of(b.ir));
}

TEST_CASE("codegen: a struct type holds the types of its fields") {
    Generated g = generate("struct outer { inner i = default; }\nstruct inner { u32 x = 0; }\n"
                           "u32 main() { outer o = outer{}; return o.i.x; }");
    // Struct types are named `<name>.<hash of the full name>`.
    CHECK(std::regex_search(g.ir, std::regex(R"(%inner\.[0-9a-f]{16} = type \{ i32 \})")));
    CHECK(std::regex_search(g.ir, std::regex(R"(%outer\.[0-9a-f]{16} = type \{ %inner\.[0-9a-f]{16} \})")));
}

// --- the driver: build and run ------------------------------------------------------------

namespace {

struct DriverRun {
    int         status;
    std::string out;
    std::string err;
};

DriverRun drive(std::vector<std::string> args) {
    std::vector<std::string_view> views(args.begin(), args.end());
    std::ostringstream            out, err;
    int status = static_cast<int>(edax::run_driver(views, edax::DriverStreams{out, err, false}));
    return DriverRun{status, out.str(), err.str()};
}

std::filesystem::path write_program(const std::string& name, const std::string& text) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "edax_unit_codegen";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / name) << text;
    return dir / name;
}

} // namespace

TEST_CASE("driver: build writes build/<mode>/<name> next to the file") {
    std::filesystem::path src = write_program("tool.ed", "u32 main() { return 0; }\n");
    std::filesystem::remove_all(src.parent_path() / "build");
    DriverRun r = drive({"build", src.string(), "--mode=release"});
    CHECK_EQ(r.status, 0);
    CHECK(std::filesystem::exists(src.parent_path() / "build" / "release" / "tool"));
    std::filesystem::remove_all(src.parent_path());
}

TEST_CASE("driver: run returns the program's exit status") {
    std::filesystem::path src = write_program("seven.ed", "u32 main() { return 7; }\n");
    CHECK_EQ(drive({"run", src.string()}).status, 7);
    std::filesystem::remove_all(src.parent_path());
}

TEST_CASE("driver: a debug build reuses the object code of units that didn't change") {
    namespace fs = std::filesystem;
    fs::path cache = fs::temp_directory_path() / "edax_unit_object_cache";
    fs::remove_all(cache);
    const char* saved = std::getenv("EDAX_CACHE_DIR");
    std::string saved_value = saved ? saved : "";
    setenv("EDAX_CACHE_DIR", cache.string().c_str(), 1);
    auto objects = [&] {
        std::set<std::string> out;
        for (const fs::directory_entry& e : fs::directory_iterator(cache)) {
            if (e.path().filename().string().starts_with("llvmcache-") && e.path().extension() == ".o") out.insert(e.path().filename().string());
        }
        return out;
    };
    std::string program = "import std;\nu32 main() { mut std::darray<u32> xs = default; xs.push(5); return xs[0]; }\n";
    fs::path src = write_program("cached.ed", program);
    CHECK_EQ(drive({"run", src.string()}).status, 5);
    std::set<std::string> first = objects();
    CHECK(first.size() > 1);   // the program's unit and the library's units
    CHECK_EQ(drive({"run", src.string()}).status, 5);
    CHECK(objects() == first);   // all reused
    // A change to the program's own code: only its unit is compiled again.
    std::ofstream(src) << "import std;\nu32 main() { mut std::darray<u32> xs = default; xs.push(6); return xs[0]; }\n";
    CHECK_EQ(drive({"run", src.string()}).status, 6);
    CHECK_EQ(objects().size(), first.size() + 1);
    // Release builds are one module, without the cache.
    CHECK_EQ(drive({"run", src.string(), "--mode=release"}).status, 6);
    CHECK_EQ(objects().size(), first.size() + 1);
    if (saved) setenv("EDAX_CACHE_DIR", saved_value.c_str(), 1);
    else unsetenv("EDAX_CACHE_DIR");
    fs::remove_all(cache);
    fs::remove_all(src.parent_path());
}

TEST_CASE("driver: emit-llvm prints the module") {
    std::filesystem::path src = write_program("tiny.ed", "void main() { }\n");
    DriverRun r = drive({"emit-llvm", src.string()});
    CHECK_EQ(r.status, 0);
    CHECK(contains(r.out, "define internal void @r_4tiny4main()"));
    CHECK(contains(r.out, "define i32 @main(i32 %0, ptr %1)"));
    std::filesystem::remove_all(src.parent_path());
}

TEST_CASE("driver: a missing linker is reported") {
    std::filesystem::path src = write_program("nocc.ed", "void main() { }\n");
    DriverRun r = drive({"run", src.string(), "--cc=/nonexistent/cc"});
    CHECK(r.status != 0);
    CHECK(contains(r.err, "linker"));
    std::filesystem::remove_all(src.parent_path());
}

// --- panics: function names, values, the debugger stop (crash handling, phases A and D) ----------

TEST_CASE("codegen: a panic's location names the function it happens in") {
    Generated g = generate("u32 area(u32 w, u32 h) { return w * h; }\nu32 main() { return area(70000, 70000); }");
    CHECK_EQ(g.codes, std::string());
    CHECK(contains(g.ir, "c\"area\\00\""));
    CHECK(std::regex_search(g.ir, std::regex(R"(@edax_loc\S* = private constant \{ ptr, i32, i32, ptr \} \{ ptr @\S+, i32 1, i32 \d+, ptr @\S+ \})")));
}

TEST_CASE("codegen: an overflow panic carries the expression and both operands") {
    Generated g = generate("u32 area(u32 w, u32 h) { return w * h; }\nu32 main() { return area(70000, 70000); }");
    // The template: the message, then \01 / \02 where the signed / unsigned operands go.
    CHECK(contains(g.ir, "integer overflow in 'w * h' (u32): \\02 * \\02"));
    CHECK(contains(g.ir, "call void @edax_panic_values("));
    CHECK(contains(g.ir, "!edax.message"));
}

TEST_CASE("codegen: a panic stops an attached debugger at the panicking line, then ends the process") {
    Generated g = generate("i32 neg(i32 x) { return -x; }\nu32 main() { return cast::truncate<u32>(neg(5)); }");
    CHECK_EQ(g.codes, std::string());
    CHECK(std::regex_search(g.ir, std::regex(R"(call void @edax_panic_values\([^\n]*\n\s*call void @llvm\.debugtrap\(\)[^\n]*\n\s*call void @edax_panic_end\(i1 true\))")));
}

TEST_CASE("codegen: stop_debugger_on_panic() == false leaves out the debugger stop") {
    Project p({{"main.ed", "i32 neg(i32 x) { return -x; }\nu32 main() { return cast::truncate<u32>(neg(5)); }"}});
    REQUIRE(!p.diags.has_errors());
    p.program.mode_settings["stop_debugger_on_panic"] = "false";
    std::string ir = edax::generate_llvm_ir(p.program, p.sources, p.diags);
    CHECK(std::regex_search(ir, std::regex(R"(call void @edax_panic_values\([^\n]*\n\s*call void @edax_panic_end\(i1 false\))")));
    CHECK(!std::regex_search(ir, std::regex(R"(call void @edax_panic_values\([^\n]*\n\s*call void @llvm\.debugtrap)")));
}

// --- debug info: where a debugger stops (spec 24) ----------------------------------------------

namespace {

// The IR of `main` with DWARF line tables, `std_optimization` as given.
std::string debug_ir(const std::string& main, const std::string& std_optimization) {
    Project p({{"main.ed", main}});
    REQUIRE(!p.diags.has_errors());
    p.program.mode_settings["std_optimization"] = std_optimization;
    edax::LlvmOptions options;
    options.debug_info = true;
    return edax::generate_llvm_ir(p.program, p.sources, p.diags, options);
}

// The line of the !dbg location on the first line of `ir` that matches `pattern`, or -1.
int line_of_call(const std::string& ir, const std::string& pattern) {
    std::smatch m;
    if (!std::regex_search(ir, m, std::regex(pattern + R"([^\n]*!dbg !(\d+))"))) return -1;
    std::smatch l;
    if (!std::regex_search(ir, l, std::regex("!" + m[1].str() + R"( = !DILocation\(line: (\d+))"))) return -1;
    return std::stoi(l[1].str());
}

} // namespace

TEST_CASE("codegen: a block's tail expression has a line of its own") {
    std::string ir = debug_ir("bool check(u32 i) { return i > 1; }\n"
                              "u32 main() {\n"
                              "    mut u32 n = 0;\n"
                              "    for (u32 i : 0..3) {\n"
                              "        if (check(i)) {\n"
                              "            n += 1;\n"
                              "        }\n"
                              "    }\n"
                              "    return n;\n"
                              "}\n",
                              "none");
    CHECK_EQ(line_of_call(ir, R"(call i1 @\S*check)"), 5);
}

TEST_CASE("codegen: an arm's expression body and the next arm's test have their own lines") {
    std::string ir = debug_ir("import std; void one() { }\nvoid two() { }\n"
                              "u32 main() {\n"
                              "    std::option<u32> x = std::some(3);\n"
                              "    match (x) {\n"
                              "        std::some(_) => one(),\n"
                              "        std::none => two(),\n"
                              "    }\n"
                              "    return 0;\n"
                              "}\n",
                              "none");
    CHECK_EQ(line_of_call(ir, R"(call void @\S*one\()"), 6);
    CHECK_EQ(line_of_call(ir, R"(call void @\S*two\()"), 7);
    // The test of the second arm is at the match, so the first arm's body starts a line.
    CHECK_EQ(line_of_call(ir, R"(arm\.next:[^\n]*\n[^\n]*)"), 5);
}

TEST_CASE("codegen: the optimized standard library has line 0, which debuggers step through") {
    std::string program = "import std;\nu32 main() { mut std::darray<u32> xs = default; xs.push(5); return xs[0]; }\n";
    std::string fast = debug_ir(program, "speed");
    // LLVM leaves out `line:` when it is 0.
    std::regex push_with_line(R"(DISubprogram\(name: "push"[^\n]*line: \d+)");
    std::regex main_with_line(R"(DISubprogram\(name: "main"[^\n]*line: \d+)");
    CHECK(contains(fast, "DISubprogram(name: \"push\""));
    CHECK(!std::regex_search(fast, push_with_line));
    CHECK(std::regex_search(fast, main_with_line));
    // std_optimization() == none: the library's lines, to step through it.
    std::string plain = debug_ir(program, "none");
    CHECK(std::regex_search(plain, push_with_line));
}

// --- aliasing facts of spans and lambdas (spec 34 §1) --------------------------------------------

TEST_CASE("codegen: span parameters are marked and their accesses get alias scopes") {
    Generated g = generate("import std;\n"
                           "void k(std::span<mut f32> a, std::span<f32> b) { a[0] = b[0]; }\n"
                           "i32 main() { mut std::darray<f32> x = default; x.push(1.0); mut std::darray<f32> y = default; y.push(2.0);\n"
                           "    k(x.as_mut_span(), y.as_span()); return 0; }\n");
    CHECK_EQ(g.codes, std::string());
    CHECK(std::regex_search(g.ir, std::regex(R"(define internal void @\S*1k\(%span_mut\S* "edax-span"="mut" %0, %span\S* "edax-span"="read" %1\))")));
    // The function's own accesses through the span slots carry the scopes.
    std::smatch m;
    REQUIRE(std::regex_search(g.ir, m, std::regex(R"(define internal void @\S*1k\([^\n]*\{\n((?:[^\n]*\n)*?)\})")));
    CHECK(contains(m[1].str(), "!alias.scope"));
    CHECK(contains(m[1].str(), "!noalias"));
}

TEST_CASE("codegen: a span of a type that isn't frozen is not marked read-only") {
    Generated g = generate("import std;\n"
                           "void k(std::span<std::atomic<u32>> a, std::span<mut u32> b) { b[0] = a[0].load(std::memory_order::relaxed); }\n"
                           "i32 main() { return 0; }\n");
    CHECK_EQ(g.codes, std::string());
    CHECK(!std::regex_search(g.ir, std::regex(R"(%span\S* "edax-span"="read" %0)")));
}

TEST_CASE("codegen: a lambda's reference parameters are noalias like a function's") {
    Generated g = generate("import std;\n"
                           "i32 main() { mut u32 x = 1; u32 y = 2;\n"
                           "    auto set = [](mut u32& p, u32& v) { p = v; };\n"
                           "    set(mut x, y); std::println(f\"{x}\"); return 0; }\n");
    CHECK_EQ(g.codes, std::string());
    CHECK(std::regex_search(g.ir, std::regex(R"(define internal void @rl_\S*\(ptr %0, ptr noalias %1, ptr noalias readonly %2\))")));
}
