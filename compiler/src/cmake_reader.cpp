// The CMake reader (packages, section 4). A small CMake interpreter: the language (variables,
// lists, conditions, loops, functions and macros, include and add_subdirectory) and the commands
// that describe targets. Install rules, tests and packaging are ignored; anything else stops the
// reader with its file and line.

#include "rift/cmake_reader.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <regex>
#include <set>
#include <sstream>

namespace rift {
namespace {

namespace fs = std::filesystem;

struct Arg {
    std::string text;
    bool        quoted = false;   // quoted and bracket arguments are not split at ';'
};

struct Command {
    std::string      name;   // lower case
    std::vector<Arg> args;
    std::string      file;
    int              line = 0;
};

struct Stop {
    std::string message;
};

// Splits a CMake file into commands; arguments keep `${...}` for evaluation.
std::vector<Command> parse(const std::string& text, const std::string& file) {
    std::vector<Command> out;
    std::size_t          i = 0;
    int                  line = 1;
    auto bracket_len = [&](std::size_t at) -> int {   // `[==[` → its number of '='
        if (text[at] != '[') return -1;
        std::size_t j = at + 1;
        while (j < text.size() && text[j] == '=') ++j;
        return j < text.size() && text[j] == '[' ? static_cast<int>(j - at - 1) : -1;
    };
    auto skip_bracket = [&](std::size_t& at, int eq) {
        std::string close = "]" + std::string(static_cast<std::size_t>(eq), '=') + "]";
        std::size_t end = text.find(close, at);
        std::string body = text.substr(at + static_cast<std::size_t>(eq) + 2, (end == std::string::npos ? text.size() : end) - at - static_cast<std::size_t>(eq) - 2);
        line += static_cast<int>(std::count(body.begin(), body.end(), '\n'));
        at = end == std::string::npos ? text.size() : end + close.size();
        if (!body.empty() && body[0] == '\n') body.erase(0, 1);
        return body;
    };
    while (i < text.size()) {
        char c = text[i];
        if (c == '\n') {
            ++line;
            ++i;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (c == '#') {
            if (int eq = bracket_len(i + 1); eq >= 0) {
                ++i;
                skip_bracket(i, eq);
            } else {
                while (i < text.size() && text[i] != '\n') ++i;
            }
            continue;
        }
        if (!std::isalpha(static_cast<unsigned char>(c)) && c != '_') throw Stop{std::format("{}:{}: unexpected '{}'", file, line, c)};
        Command cmd;
        cmd.file = file;
        cmd.line = line;
        while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_')) cmd.name += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i++])));
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        if (i >= text.size() || text[i] != '(') throw Stop{std::format("{}:{}: expected '(' after '{}'", file, line, cmd.name)};
        ++i;
        int depth = 1;
        while (i < text.size()) {
            c = text[i];
            if (c == '\n') {
                ++line;
                ++i;
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(c))) {
                ++i;
                continue;
            }
            if (c == '#') {
                if (int eq = bracket_len(i + 1); eq >= 0) {
                    ++i;
                    skip_bracket(i, eq);
                } else {
                    while (i < text.size() && text[i] != '\n') ++i;
                }
                continue;
            }
            if (c == ')') {
                ++i;
                if (--depth == 0) break;
                cmd.args.push_back(Arg{")", false});
                continue;
            }
            if (c == '(') {
                ++depth;
                ++i;
                cmd.args.push_back(Arg{"(", false});
                continue;
            }
            if (int eq = bracket_len(i); eq >= 0) {
                cmd.args.push_back(Arg{skip_bracket(i, eq), true});
                continue;
            }
            if (c == '"') {
                std::string v;
                ++i;
                while (i < text.size() && text[i] != '"') {
                    if (text[i] == '\\' && i + 1 < text.size()) {
                        char e = text[i + 1];
                        if (e == '\n') {   // a line continuation
                            ++line;
                            i += 2;
                            continue;
                        }
                        if (e == 'n') v += '\n';
                        else if (e == 't') v += '\t';
                        else if (e == '$' || e == '"' || e == '\\' || e == ';' || e == '(' || e == ')' || e == '#' || e == ' ') {
                            v += e == '$' ? "\x01" : std::string(1, e);   // an escaped '$' is not a reference
                        } else {
                            v += '\\';
                            v += e;
                        }
                        i += 2;
                        continue;
                    }
                    if (text[i] == '\n') ++line;
                    v += text[i++];
                }
                ++i;
                cmd.args.push_back(Arg{v, true});
                continue;
            }
            std::string v;
            int         inner = 0;   // `$<...>` and `${...}` may hold spaces? no: but parentheses nest
            while (i < text.size()) {
                char d = text[i];
                if (d == '\\' && i + 1 < text.size()) {
                    v += text[i + 1] == ';' ? std::string("\\;") : std::string(1, text[i + 1]);
                    i += 2;
                    continue;
                }
                if (d == '"' && !v.empty()) {   // `-DX="a b"`: a quote inside an unquoted argument
                    v += text[i++];
                    while (i < text.size() && text[i] != '"') v += text[i++];
                    if (i < text.size()) v += text[i++];
                    continue;
                }
                if (std::isspace(static_cast<unsigned char>(d)) || d == '"' || d == '#') break;
                if (d == '(') {
                    ++inner;
                } else if (d == ')') {
                    if (inner == 0) break;
                    --inner;
                }
                v += d;
                ++i;
            }
            cmd.args.push_back(Arg{v, false});
        }
        out.push_back(std::move(cmd));
    }
    return out;
}

std::vector<std::string> split_list(const std::string& s) {
    std::vector<std::string> out;
    if (s.empty()) return out;
    std::string cur;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == ';') {
            cur += ';';
            ++i;
        } else if (s[i] == ';') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += s[i];
        }
    }
    out.push_back(cur);
    return out;
}

std::string join(const std::vector<std::string>& v, const std::string& sep = ";") {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) out += (i ? sep : "") + v[i];
    return out;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool is_false_constant(const std::string& v) {
    std::string u = upper(v);
    return u.empty() || u == "0" || u == "OFF" || u == "NO" || u == "FALSE" || u == "N" || u == "IGNORE" || u == "NOTFOUND" ||
           u.ends_with("-NOTFOUND");
}

bool is_true_constant(const std::string& v) {
    std::string u = upper(v);
    if (u == "1" || u == "ON" || u == "YES" || u == "TRUE" || u == "Y") return true;
    return !u.empty() && std::all_of(u.begin(), u.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }) && u != "0";
}

int compare_versions(const std::string& a, const std::string& b) {
    std::istringstream x(a), y(b);
    while (true) {
        std::string pa, pb;
        bool        ha = static_cast<bool>(std::getline(x, pa, '.'));
        bool        hb = static_cast<bool>(std::getline(y, pb, '.'));
        if (!ha && !hb) return 0;
        long na = ha && !pa.empty() ? std::atol(pa.c_str()) : 0;
        long nb = hb && !pb.empty() ? std::atol(pb.c_str()) : 0;
        if (na != nb) return na < nb ? -1 : 1;
    }
}

struct Function {
    std::vector<std::string> params;
    std::vector<Command>     body;
    bool                     is_macro = false;
};

class Reader {
public:
    explicit Reader(const CMakeInput& in) : in_(in) {}

    CMakeResult run() {
        try {
            scopes_.emplace_back();
            fs::path root = fs::path(in_.root).lexically_normal();
            set("CMAKE_SOURCE_DIR", root.string());
            set("CMAKE_BINARY_DIR", in_.out_dir);
            set("PROJECT_SOURCE_DIR", root.string());
            set("CMAKE_BUILD_TYPE", in_.build_type);
            set("CMAKE_SYSTEM_NAME", in_.system_name);
            set("CMAKE_HOST_SYSTEM_NAME", in_.system_name);
            set("CMAKE_C_COMPILER_ID", "GNU");
            set("CMAKE_CXX_COMPILER_ID", "GNU");
            set("CMAKE_SIZEOF_VOID_P", "8");
            set("CMAKE_VERSION", "3.28.0");
            set("CMAKE_C_COMPILER_LOADED", "1");
            set("CMAKE_CXX_COMPILER_LOADED", "1");
            set("PROJECT_IS_TOP_LEVEL", "ON");
            set("CMAKE_STATIC_LIBRARY_PREFIX", "lib");
            set("CMAKE_STATIC_LIBRARY_SUFFIX", ".a");
            set("CMAKE_SHARED_LIBRARY_PREFIX", "lib");
            set("CMAKE_SHARED_LIBRARY_SUFFIX", ".so");
            set("CMAKE_EXECUTABLE_SUFFIX", "");
            if (in_.system_name == "Windows") set("WIN32", "1");
            else set("UNIX", "1");
            if (in_.system_name == "Darwin") set("APPLE", "1");
            if (in_.system_name == "Linux") set("LINUX", "1");
            for (const auto& [k, v] : in_.options) cache_[k] = v;
            directory(root);
        } catch (const Stop& s) {
            result_.error = s.message;
        }
        result_.targets.clear();
        for (const std::string& n : order_) {
            CMakeTarget t = targets_.at(n);
            if (t.standard.empty() && standards_.contains(n)) {
                const auto& [c, cpp] = standards_.at(n);
                if (t.language == "cpp" && !cpp.empty()) t.standard = "cpp" + cpp;
                else if (t.language == "c" && !c.empty()) t.standard = "c" + c;
            }
            if (t.standard.starts_with("c") && !t.standard.starts_with("cpp") && t.language == "cpp") t.standard.clear();
            result_.targets.push_back(std::move(t));
        }
        return result_;
    }

private:
    const CMakeInput&                                   in_;
    CMakeResult                                         result_;
    std::vector<std::map<std::string, std::string>>     scopes_;
    std::map<std::string, std::string>                  cache_;
    std::map<std::string, Function>                     functions_;
    std::map<std::string, CMakeTarget>                  targets_;
    std::vector<std::string>                            order_;
    std::map<std::string, bool>                         checks_;   // cached check results
    std::map<std::string, std::pair<std::string, std::string>> standards_;   // CMAKE_C/CXX_STANDARD when a target was added
    std::vector<std::string>                            dir_includes_;   // include_directories()
    std::vector<std::string>                            dir_defines_;    // add_definitions()
    bool                                                returning_ = false;
    bool                                                breaking_ = false;

    // --- variables ------------------------------------------------------------------------------

    void set(const std::string& k, const std::string& v) { scopes_.back()[k] = v; }
    std::optional<std::string> lookup(const std::string& k) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (auto f = it->find(k); f != it->end()) return f->second;
        }
        if (auto f = cache_.find(k); f != cache_.end()) return f->second;
        return std::nullopt;
    }
    std::string get(const std::string& k) const { return lookup(k).value_or(""); }
    bool defined(const std::string& k) const { return lookup(k).has_value(); }
    void unset(const std::string& k) {
        scopes_.back().erase(k);
        // A variable unset in this scope must hide outer ones: CMake's scopes copy on entry.
    }

    std::string expand(const std::string& s, const Command& c) const {
        std::string out;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '{') {
                int         depth = 1;
                std::size_t j     = i + 2;
                std::string inner;
                while (j < s.size() && depth > 0) {
                    if (s[j] == '{') ++depth;
                    if (s[j] == '}' && --depth == 0) break;
                    inner += s[j++];
                }
                out += get(expand(inner, c));
                i = j;
                continue;
            }
            if (s[i] == '$' && s.compare(i, 5, "$ENV{") == 0) {
                std::size_t j = s.find('}', i);
                const char* e = std::getenv(s.substr(i + 5, j - i - 5).c_str());
                out += e ? e : "";
                i = j;
                continue;
            }
            if (s[i] == '$' && s.compare(i, 7, "$CACHE{") == 0) {
                std::size_t j = s.find('}', i);
                auto f = cache_.find(s.substr(i + 7, j - i - 7));
                out += f == cache_.end() ? "" : f->second;
                i = j;
                continue;
            }
            out += s[i] == '\x01' ? '$' : s[i];
        }
        return out;
    }

    // The arguments after expansion: unquoted ones split at ';' (an empty one disappears).
    std::vector<std::string> args(const Command& c) const {
        std::vector<std::string> out;
        for (const Arg& a : c.args) {
            std::string v = expand(a.text, c);
            if (a.quoted) {
                out.push_back(v);
                continue;
            }
            for (std::string& part : split_list(v)) {
                if (!part.empty()) out.push_back(std::move(part));
            }
        }
        return out;
    }

    [[noreturn]] void stop(const Command& c, const std::string& message) const {
        throw Stop{std::format("{}:{}: {}", c.file, c.line, message)};
    }

    void note(const Command& c, const std::string& became) {
        result_.commands.emplace_back(std::format("{}:{}: {}", c.file, c.line, c.name), became);
    }

    // --- files and directories ------------------------------------------------------------------

    std::vector<Command> load(const fs::path& file) {
        std::ifstream in(file);
        if (!in) throw Stop{std::format("cannot read {}", file.string())};
        std::stringstream ss;
        ss << in.rdbuf();
        result_.files.push_back(file.string());
        return parse(ss.str(), file.string());
    }

    void directory(const fs::path& dir) {
        std::string saved_src = get("CMAKE_CURRENT_SOURCE_DIR"), saved_list = get("CMAKE_CURRENT_LIST_DIR");
        std::vector<std::string> saved_includes = dir_includes_, saved_defines = dir_defines_;
        set("CMAKE_CURRENT_SOURCE_DIR", dir.string());
        set("CMAKE_CURRENT_LIST_DIR", dir.string());
        set("CMAKE_CURRENT_BINARY_DIR", (fs::path(in_.out_dir) / fs::relative(dir, in_.root)).lexically_normal().string());
        std::vector<Command> cmds = load(dir / "CMakeLists.txt");
        set("CMAKE_CURRENT_LIST_FILE", (dir / "CMakeLists.txt").string());
        run_block(cmds, 0, cmds.size());
        returning_ = false;
        set("CMAKE_CURRENT_SOURCE_DIR", saved_src);
        set("CMAKE_CURRENT_LIST_DIR", saved_list);
        dir_includes_ = saved_includes;
        dir_defines_  = saved_defines;
    }

    fs::path current_dir() const { return fs::path(get("CMAKE_CURRENT_SOURCE_DIR")); }

    std::string absolute(const std::string& p) const {
        fs::path path(p);
        return (path.is_absolute() ? path : current_dir() / path).lexically_normal().string();
    }

    // --- control flow ---------------------------------------------------------------------------

    // The index of the command that closes the block opened at `start` (endif/endforeach/...).
    std::size_t block_end(const std::vector<Command>& cmds, std::size_t start, const std::string& open, const std::string& close) const {
        int depth = 0;
        for (std::size_t i = start; i < cmds.size(); ++i) {
            if (cmds[i].name == open) ++depth;
            if (cmds[i].name == close && --depth == 0) return i;
        }
        stop(cmds[start], std::format("'{}' without '{}'", open, close));
    }

    void run_block(const std::vector<Command>& cmds, std::size_t from, std::size_t to) {
        for (std::size_t i = from; i < to && !returning_ && !breaking_; ++i) {
            const Command& c = cmds[i];
            if (c.name == "if") {
                std::size_t end = block_end(cmds, i, "if", "endif");
                // The branches: if / elseif / else at this depth.
                std::vector<std::size_t> marks = {i};
                int depth = 0;
                for (std::size_t j = i + 1; j < end; ++j) {
                    if (cmds[j].name == "if") ++depth;
                    if (cmds[j].name == "endif") --depth;
                    if (depth == 0 && (cmds[j].name == "elseif" || cmds[j].name == "else")) marks.push_back(j);
                }
                marks.push_back(end);
                for (std::size_t k = 0; k + 1 < marks.size(); ++k) {
                    const Command& head = cmds[marks[k]];
                    if (head.name == "else" || condition(head)) {
                        run_block(cmds, marks[k] + 1, marks[k + 1]);
                        break;
                    }
                }
                i = end;
                continue;
            }
            if (c.name == "foreach") {
                std::size_t end = block_end(cmds, i, "foreach", "endforeach");
                foreach_loop(cmds, i, end);
                i = end;
                continue;
            }
            if (c.name == "while") {
                std::size_t end = block_end(cmds, i, "while", "endwhile");
                int guard = 0;
                while (condition(c) && !returning_) {
                    if (++guard > 100000) stop(c, "a while loop that doesn't end");
                    run_block(cmds, i + 1, end);
                    if (breaking_) {
                        breaking_ = false;
                        break;
                    }
                }
                i = end;
                continue;
            }
            if (c.name == "function" || c.name == "macro") {
                std::size_t end = block_end(cmds, i, c.name, "end" + c.name);
                std::vector<std::string> a = args(c);
                if (a.empty()) stop(c, c.name + " without a name");
                Function f;
                f.params.assign(a.begin() + 1, a.end());
                f.body.assign(cmds.begin() + static_cast<std::ptrdiff_t>(i) + 1, cmds.begin() + static_cast<std::ptrdiff_t>(end));
                f.is_macro = c.name == "macro";
                functions_[lower(a[0])] = std::move(f);
                i = end;
                continue;
            }
            if (c.name == "break") {
                breaking_ = true;
                return;
            }
            if (c.name == "continue") return;
            if (c.name == "return") {
                returning_ = true;
                return;
            }
            command(c);
        }
    }

    void foreach_loop(const std::vector<Command>& cmds, std::size_t at, std::size_t end) {
        const Command&           c = cmds[at];
        std::vector<std::string> a = args(c);
        if (a.empty()) stop(c, "foreach without a variable");
        std::string              var = a[0];
        std::vector<std::string> items;
        if (a.size() >= 2 && a[1] == "RANGE") {
            long first = 0, last = 0, step = 1;
            if (a.size() == 3) last = std::atol(a[2].c_str());
            else {
                first = std::atol(a[2].c_str());
                last  = std::atol(a[3].c_str());
                if (a.size() > 4) step = std::atol(a[4].c_str());
            }
            for (long k = first; step > 0 ? k <= last : k >= last; k += step) items.push_back(std::to_string(k));
        } else if (a.size() >= 2 && a[1] == "IN") {
            std::string mode;
            for (std::size_t k = 2; k < a.size(); ++k) {
                if (a[k] == "LISTS" || a[k] == "ITEMS" || a[k] == "ZIP_LISTS") {
                    mode = a[k];
                    continue;
                }
                if (mode == "LISTS") {
                    for (std::string& x : split_list(get(a[k]))) items.push_back(x);
                } else {
                    items.push_back(a[k]);
                }
            }
        } else {
            items.assign(a.begin() + 1, a.end());
        }
        for (const std::string& item : items) {
            set(var, item);
            run_block(cmds, at + 1, end);
            if (breaking_) {
                breaking_ = false;
                break;
            }
            if (returning_) break;
        }
    }

    // --- conditions -----------------------------------------------------------------------------

    bool condition(const Command& c) {
        // Arguments are expanded, but an unquoted name is looked up as a variable.
        std::vector<std::pair<std::string, bool>> toks;   // text, quoted
        for (const Arg& a : c.args) {
            std::string v = expand(a.text, c);
            toks.emplace_back(v, a.quoted);
        }
        std::size_t p = 0;
        return or_expr(toks, p, c);
    }

    std::string value_of(const std::pair<std::string, bool>& t) const {
        if (t.second) return t.first;
        if (auto v = lookup(t.first)) return *v;
        return t.first;
    }

    bool truthy(const std::pair<std::string, bool>& t) const {
        if (is_true_constant(t.first)) return true;
        if (is_false_constant(t.first)) return false;
        if (t.second) return false;   // a quoted string that isn't a constant
        auto v = lookup(t.first);
        return v && !is_false_constant(*v);
    }

    bool or_expr(const std::vector<std::pair<std::string, bool>>& t, std::size_t& p, const Command& c) {
        bool v = and_expr(t, p, c);
        while (p < t.size() && !t[p].second && t[p].first == "OR") {
            ++p;
            bool r = and_expr(t, p, c);
            v      = v || r;
        }
        return v;
    }
    bool and_expr(const std::vector<std::pair<std::string, bool>>& t, std::size_t& p, const Command& c) {
        bool v = not_expr(t, p, c);
        while (p < t.size() && !t[p].second && t[p].first == "AND") {
            ++p;
            bool r = not_expr(t, p, c);
            v      = v && r;
        }
        return v;
    }
    bool not_expr(const std::vector<std::pair<std::string, bool>>& t, std::size_t& p, const Command& c) {
        if (p < t.size() && !t[p].second && t[p].first == "NOT") {
            ++p;
            return !not_expr(t, p, c);
        }
        return primary(t, p, c);
    }
    bool primary(const std::vector<std::pair<std::string, bool>>& t, std::size_t& p, const Command& c) {
        if (p >= t.size()) return false;
        if (!t[p].second && t[p].first == "(") {
            ++p;
            bool v = or_expr(t, p, c);
            if (p < t.size() && t[p].first == ")") ++p;
            return v;
        }
        static const std::set<std::string> unary = {"DEFINED", "EXISTS", "TARGET", "COMMAND", "IS_DIRECTORY", "IS_ABSOLUTE", "POLICY"};
        if (!t[p].second && unary.contains(t[p].first) && p + 1 < t.size()) {
            std::string op = t[p].first, x = t[p + 1].first;
            p += 2;
            if (op == "DEFINED") {
                if (x.starts_with("ENV{")) return std::getenv(x.substr(4, x.size() - 5).c_str()) != nullptr;
                if (x.starts_with("CACHE{")) return cache_.contains(x.substr(6, x.size() - 7));
                return defined(x);
            }
            if (op == "EXISTS") return fs::exists(x);
            if (op == "IS_DIRECTORY") return fs::is_directory(x);
            if (op == "IS_ABSOLUTE") return fs::path(x).is_absolute();
            if (op == "TARGET") return targets_.contains(x);
            if (op == "COMMAND") return functions_.contains(lower(x)) || known_command(lower(x));
            return true;   // POLICY
        }
        std::pair<std::string, bool> lhs = t[p++];
        static const std::set<std::string> binary = {"STREQUAL", "EQUAL", "LESS", "GREATER", "LESS_EQUAL", "GREATER_EQUAL", "MATCHES",
                                                     "VERSION_LESS", "VERSION_GREATER", "VERSION_EQUAL", "VERSION_LESS_EQUAL",
                                                     "VERSION_GREATER_EQUAL", "STRLESS", "STRGREATER", "IN_LIST"};
        if (p < t.size() && !t[p].second && binary.contains(t[p].first) && p + 1 < t.size()) {
            std::string op = t[p].first;
            std::string a = value_of(lhs), b = value_of(t[p + 1]);
            p += 2;
            if (op == "STREQUAL") return a == b;
            if (op == "STRLESS") return a < b;
            if (op == "STRGREATER") return a > b;
            if (op == "MATCHES") {
                try {
                    return std::regex_search(a, std::regex(b, std::regex::extended));
                } catch (...) {
                    stop(c, "a regular expression the reader can't read: " + b);
                }
            }
            if (op == "IN_LIST") {
                auto items = split_list(get(t[p - 1].first));
                return std::find(items.begin(), items.end(), a) != items.end();
            }
            if (op.starts_with("VERSION_")) {
                int r = compare_versions(a, b);
                if (op == "VERSION_LESS") return r < 0;
                if (op == "VERSION_GREATER") return r > 0;
                if (op == "VERSION_EQUAL") return r == 0;
                if (op == "VERSION_LESS_EQUAL") return r <= 0;
                return r >= 0;
            }
            double x = std::atof(a.c_str()), y = std::atof(b.c_str());
            if (op == "EQUAL") return x == y;
            if (op == "LESS") return x < y;
            if (op == "GREATER") return x > y;
            if (op == "LESS_EQUAL") return x <= y;
            return x >= y;
        }
        return truthy(lhs);
    }

    // --- generator expressions -----------------------------------------------------------------

    std::string genex(const std::string& s, const CMakeTarget* t, const Command& c, const std::string& language = "") {
        std::string out;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s.compare(i, 2, "$<") != 0) {
                out += s[i];
                continue;
            }
            int         depth = 1;
            std::size_t j     = i + 2;
            while (j < s.size() && depth > 0) {
                if (s.compare(j, 2, "$<") == 0) {
                    ++depth;
                    ++j;
                } else if (s[j] == '>' && --depth == 0) {
                    break;
                }
                ++j;
            }
            std::string inner = s.substr(i + 2, j - i - 2);
            out += genex_value(inner, t, c, language);
            i = j;
        }
        return out;
    }

    std::string genex_value(const std::string& raw, const CMakeTarget* t, const Command& c, const std::string& language) {
        std::size_t colon = raw.find(':');
        std::string name  = raw.substr(0, colon);
        std::string arg   = colon == std::string::npos ? "" : genex(raw.substr(colon + 1), t, c, language);
        if (name == "BUILD_INTERFACE") return arg;
        if (name == "INSTALL_INTERFACE" || name == "INSTALL_PREFIX") return "";
        if (name == "CONFIG") return lower(arg) == lower(in_.build_type) ? "1" : "0";
        if (name == "BOOL") return is_false_constant(arg) ? "0" : "1";
        if (name == "NOT") return arg == "1" ? "0" : "1";
        if (name == "AND" || name == "OR") {
            auto items = split_list(std::regex_replace(arg, std::regex(","), ";"));
            bool v = name == "AND";
            for (const std::string& x : items) v = name == "AND" ? v && x == "1" : v || x == "1";
            return v ? "1" : "0";
        }
        if (name == "STREQUAL" || name == "EQUAL") {
            std::size_t comma = arg.find(',');
            return arg.substr(0, comma) == arg.substr(comma + 1) ? "1" : "0";
        }
        if (name == "COMPILE_LANGUAGE" || name == "COMPILE_LANG_AND_ID") {
            std::string want = lower(arg.substr(0, arg.find(',')));
            std::string have = language.empty() ? (t ? t->language : "c") : language;
            return (want == "cxx" ? "cpp" : want) == have ? "1" : "0";
        }
        if (name == "PLATFORM_ID") return arg == in_.system_name ? "1" : "0";
        if (name == "C_COMPILER_ID" || name == "CXX_COMPILER_ID") return arg.empty() ? "GNU" : (arg == "GNU" ? "1" : "0");
        if (name == "TARGET_FILE" || name == "TARGET_LINKER_FILE") return arg;
        if (name == "TARGET_PROPERTY") return "";
        if (name.size() == 1 && (name == "0" || name == "1")) return name == "1" ? arg : "";
        if (name == "ANGLE-R") return ">";
        if (name == "COMMA") return ",";
        if (name == "SEMICOLON") return ";";
        stop(c, std::format("the generator expression '$<{}>' isn't understood by the CMake reader", name));
    }

    // --- commands -------------------------------------------------------------------------------

    static bool known_command(const std::string& n) {
        static const std::set<std::string> k = {
            "cmake_minimum_required", "project", "set", "unset", "option", "list", "string", "math", "message", "include",
            "add_subdirectory", "add_library", "add_executable", "target_sources", "target_include_directories",
            "target_compile_definitions", "target_compile_options", "target_compile_features", "target_link_libraries",
            "include_directories", "add_definitions", "add_compile_definitions", "add_compile_options", "file", "install", "export",
            "enable_testing", "add_test", "find_package", "find_library", "find_path", "find_program", "configure_file",
            "set_target_properties", "set_property", "get_filename_component", "cmake_policy", "mark_as_advanced",
            "check_include_file", "check_include_files", "check_symbol_exists", "check_function_exists", "check_c_source_compiles",
            "check_cxx_source_compiles", "check_type_size", "check_library_exists", "include_guard", "set_directory_properties",
            "cmake_parse_arguments", "cmake_path", "source_group", "pkg_check_modules", "target_link_options", "link_libraries",
            "enable_language", "get_property", "get_target_property", "define_property", "add_dependencies", "cpack_add_component",
            "write_basic_package_version_file", "configure_package_config_file", "check_c_compiler_flag", "check_cxx_compiler_flag",
            "target_precompile_headers", "set_source_files_properties", "separate_arguments", "site_name", "variable_watch"};
        return k.contains(n);
    }

    CMakeTarget& target(const Command& c, const std::string& name) {
        auto it = targets_.find(name);
        if (it == targets_.end()) stop(c, std::format("'{}' is not a target", name));
        if (it->second.kind == CMakeTarget::Kind::alias) return target(c, it->second.alias_of);
        return it->second;
    }

    // `PUBLIC a PRIVATE b ...` → (value, visibility).
    std::vector<std::pair<std::string, std::string>> with_visibility(const std::vector<std::string>& a, std::size_t from,
                                                                     const std::string& fallback = "shared") {
        std::vector<std::pair<std::string, std::string>> out;
        std::string vis = fallback;
        for (std::size_t i = from; i < a.size(); ++i) {
            if (a[i] == "PUBLIC") vis = "shared";
            else if (a[i] == "PRIVATE") vis = "own";
            else if (a[i] == "INTERFACE") vis = "users";
            else if (a[i] == "SYSTEM" || a[i] == "BEFORE" || a[i] == "AFTER") continue;
            else out.emplace_back(a[i], vis);
        }
        return out;
    }

    void add_sources(CMakeTarget& t, const std::vector<std::string>& files, const Command& c) {
        for (const std::string& raw : files) {
            std::string f = genex(raw, &t, c);
            if (f.empty()) continue;
            std::string ext = lower(fs::path(f).extension().string());
            if (ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".hxx" || ext == ".inl" || ext == ".def" || ext == ".rc" || ext == ".in") continue;
            if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c++") t.language = "cpp";
            else if (ext != ".c") stop(c, std::format("the source '{}' is in a language the build can't compile here", f));
            t.sources.push_back(absolute(f));
        }
    }

    bool check(const std::string& source) {
        if (auto it = checks_.find(source); it != checks_.end()) return it->second;
        bool ok = in_.compiles && in_.compiles(source);
        checks_[source] = ok;
        return ok;
    }

    void call_function(const Command& c, const Function& f) {
        std::vector<std::string> a = args(c);
        bool macro = f.is_macro;
        if (!macro) scopes_.emplace_back();
        std::map<std::string, std::string> saved;
        auto bind = [&](const std::string& k, const std::string& v) {
            if (macro) {
                if (!saved.contains(k)) saved[k] = get(k);
            }
            set(k, v);
        };
        for (std::size_t i = 0; i < f.params.size(); ++i) bind(f.params[i], i < a.size() ? a[i] : "");
        std::vector<std::string> rest(a.begin() + static_cast<std::ptrdiff_t>(std::min(a.size(), f.params.size())), a.end());
        bind("ARGN", join(rest));
        bind("ARGC", std::to_string(a.size()));
        bind("ARGV", join(a));
        for (std::size_t i = 0; i < a.size(); ++i) bind(std::format("ARGV{}", i), a[i]);
        run_block(f.body, 0, f.body.size());
        returning_ = false;
        if (!macro) {
            // PARENT_SCOPE writes went to the scope below (see `set`).
            scopes_.pop_back();
        } else {
            for (const auto& [k, v] : saved) set(k, v);
        }
    }

    void command(const Command& c) {
        if (auto f = functions_.find(c.name); f != functions_.end()) {
            Function copy = f->second;
            call_function(c, copy);
            return;
        }
        std::vector<std::string> a = args(c);
        const std::string&       n = c.name;
        if (n == "cmake_minimum_required" || n == "cmake_policy" || n == "mark_as_advanced" || n == "include_guard" ||
            n == "source_group" || n == "enable_language" || n == "define_property" || n == "site_name" || n == "variable_watch" ||
            n == "set_directory_properties" || n == "add_dependencies" || n == "target_precompile_headers" ||
            n == "set_source_files_properties") {
            return;
        }
        if (n == "install" || n == "export" || n == "enable_testing" || n == "add_test" || n == "cpack_add_component" ||
            n == "write_basic_package_version_file" || n == "configure_package_config_file") {
            note(c, "ignored");
            return;
        }
        if (n == "project") {
            if (a.empty()) stop(c, "project without a name");
            set("PROJECT_NAME", a[0]);
            set("CMAKE_PROJECT_NAME", a[0]);
            set("PROJECT_SOURCE_DIR", current_dir().string());
            set(a[0] + "_SOURCE_DIR", current_dir().string());
            set("PROJECT_BINARY_DIR", get("CMAKE_CURRENT_BINARY_DIR"));
            set(a[0] + "_BINARY_DIR", get("CMAKE_CURRENT_BINARY_DIR"));
            for (std::size_t i = 1; i + 1 < a.size(); ++i) {
                if (a[i] == "VERSION") {
                    std::string v = a[i + 1];
                    set("PROJECT_VERSION", v);
                    set(a[0] + "_VERSION", v);
                    auto parts = split_list(std::regex_replace(v, std::regex("\\."), ";"));
                    const char* names[] = {"MAJOR", "MINOR", "PATCH", "TWEAK"};
                    for (std::size_t k = 0; k < 4; ++k) {
                        set(std::string("PROJECT_VERSION_") + names[k], k < parts.size() ? parts[k] : "");
                        set(a[0] + "_VERSION_" + names[k], k < parts.size() ? parts[k] : "");
                    }
                }
            }
            return;
        }
        if (n == "set") {
            if (a.empty()) stop(c, "set without a variable");
            std::vector<std::string> values(a.begin() + 1, a.end());
            auto cache = std::find(values.begin(), values.end(), "CACHE");
            if (cache != values.end()) {   // set(X value CACHE TYPE "doc" [FORCE])
                std::string v = join(std::vector<std::string>(values.begin(), cache));
                bool force = std::find(values.begin(), values.end(), "FORCE") != values.end();
                if (!cache_.contains(a[0]) || (force && !in_.options.contains(a[0]))) cache_[a[0]] = v;
                return;
            }
            bool parent = !values.empty() && values.back() == "PARENT_SCOPE";
            if (parent) values.pop_back();
            if (parent && scopes_.size() >= 2) {
                scopes_[scopes_.size() - 2][a[0]] = join(values);
                return;
            }
            if (values.empty()) unset(a[0]);
            else set(a[0], join(values));
            return;
        }
        if (n == "unset") {
            if (!a.empty()) {
                if (a.size() > 1 && a[1] == "CACHE") cache_.erase(a[0]);
                else unset(a[0]);
            }
            return;
        }
        if (n == "option") {
            if (a.empty()) stop(c, "option without a name");
            if (!cache_.contains(a[0])) cache_[a[0]] = a.size() >= 3 ? a[2] : "OFF";
            return;
        }
        if (n == "message") {
            if (!a.empty() && (a[0] == "FATAL_ERROR" || a[0] == "SEND_ERROR")) {
                stop(c, "message(FATAL_ERROR): " + join(std::vector<std::string>(a.begin() + 1, a.end()), ""));
            }
            return;
        }
        if (n == "list") return list_command(c, a);
        if (n == "string") return string_command(c, a);
        if (n == "math") {
            if (a.size() < 3 || a[0] != "EXPR") stop(c, "math(EXPR var expression) is what the reader understands");
            set(a[1], std::to_string(arithmetic(a[2], c)));
            return;
        }
        if (n == "get_filename_component") {
            if (a.size() < 3) stop(c, "get_filename_component needs a variable, a file and a mode");
            fs::path p(a[1]);
            std::string m = a[2], v;
            if (m == "DIRECTORY" || m == "PATH") v = p.parent_path().string();
            else if (m == "NAME") v = p.filename().string();
            else if (m == "NAME_WE") v = p.stem().string();
            else if (m == "EXT" || m == "LAST_EXT") v = p.extension().string();
            else if (m == "ABSOLUTE" || m == "REALPATH") v = absolute(a[1]);
            else stop(c, "get_filename_component mode " + m);
            set(a[0], v);
            return;
        }
        if (n == "include") {
            if (a.empty()) return;
            std::string name = a[0];
            static const std::set<std::string> modules = {
                "GNUInstallDirs", "CMakePackageConfigHelpers", "CheckIncludeFile", "CheckIncludeFiles", "CheckSymbolExists",
                "CheckFunctionExists", "CheckCSourceCompiles", "CheckCXXSourceCompiles", "CheckTypeSize", "CheckLibraryExists",
                "CTest", "CPack", "FeatureSummary", "CMakeDependentOption", "CheckCCompilerFlag", "CheckCXXCompilerFlag",
                "FindPackageHandleStandardArgs", "CMakeParseArguments", "GenerateExportHeader", "TestBigEndian", "CheckLanguage"};
            if (modules.contains(name)) {
                if (name == "GNUInstallDirs") {
                    for (const char* d : {"BINDIR", "LIBDIR", "INCLUDEDIR", "DATADIR", "DATAROOTDIR", "DOCDIR", "MANDIR", "SYSCONFDIR"}) {
                        std::string v = std::string(d) == "BINDIR" ? "bin" : std::string(d) == "LIBDIR" ? "lib" : std::string(d) == "INCLUDEDIR" ? "include" : "share";
                        set(std::string("CMAKE_INSTALL_") + d, v);
                        set(std::string("CMAKE_INSTALL_FULL_") + d, "/usr/local/" + v);
                    }
                }
                if (name == "TestBigEndian") return;
                return;
            }
            fs::path file = absolute(name);
            if (!fs::exists(file)) {
                bool optional = std::find(a.begin(), a.end(), "OPTIONAL") != a.end();
                if (optional) return;
                stop(c, std::format("include({}): the reader knows no such module or file", name));
            }
            std::string saved = get("CMAKE_CURRENT_LIST_DIR");
            set("CMAKE_CURRENT_LIST_DIR", fs::path(file).parent_path().string());
            std::vector<Command> cmds = load(file);
            run_block(cmds, 0, cmds.size());
            returning_ = false;
            set("CMAKE_CURRENT_LIST_DIR", saved);
            return;
        }
        if (n == "add_subdirectory") {
            if (a.empty()) stop(c, "add_subdirectory without a directory");
            fs::path dir = absolute(a[0]);
            if (!fs::exists(dir / "CMakeLists.txt")) stop(c, std::format("add_subdirectory({}): no CMakeLists.txt there", a[0]));
            scopes_.emplace_back();
            directory(dir);
            scopes_.pop_back();
            return;
        }
        if (n == "add_library" || n == "add_executable") {
            if (a.empty()) stop(c, n + " without a name");
            CMakeTarget t;
            t.name = a[0];
            t.file = c.file;
            t.line = c.line;
            std::size_t i = 1;
            if (n == "add_executable") {
                t.kind = CMakeTarget::Kind::executable;
                while (i < a.size() && (a[i] == "WIN32" || a[i] == "MACOSX_BUNDLE" || a[i] == "EXCLUDE_FROM_ALL")) ++i;
                if (i < a.size() && (a[i] == "IMPORTED" || a[i] == "ALIAS")) stop(c, "imported or alias executables");
            } else {
                bool shared = is_true_constant(get("BUILD_SHARED_LIBS"));
                t.kind = shared ? CMakeTarget::Kind::shared_library : CMakeTarget::Kind::static_library;
                if (i < a.size() && a[i] == "ALIAS") {
                    if (i + 1 >= a.size()) stop(c, "add_library(... ALIAS) without the target");
                    t.kind     = CMakeTarget::Kind::alias;
                    t.alias_of = a[i + 1];
                    target(c, t.alias_of);
                    targets_[t.name] = t;
                    order_.push_back(t.name);
                    note(c, std::format("the alias {} of {}", t.name, t.alias_of));
                    return;
                }
                if (i < a.size() && (a[i] == "STATIC" || a[i] == "SHARED" || a[i] == "MODULE" || a[i] == "OBJECT" || a[i] == "INTERFACE")) {
                    t.kind = a[i] == "STATIC"      ? CMakeTarget::Kind::static_library
                           : a[i] == "OBJECT"      ? CMakeTarget::Kind::object_library
                           : a[i] == "INTERFACE"   ? CMakeTarget::Kind::interface_library
                                                   : CMakeTarget::Kind::shared_library;
                    ++i;
                }
                if (i < a.size() && a[i] == "IMPORTED") stop(c, "an IMPORTED library: describe it with .uses instead");
                if (i < a.size() && a[i] == "EXCLUDE_FROM_ALL") ++i;
            }
            standards_[t.name] = {get("CMAKE_C_STANDARD"), get("CMAKE_CXX_STANDARD")};
            for (const std::string& d : dir_includes_) t.include_dirs.emplace_back(d, "own");
            for (const std::string& d : dir_defines_) t.defines.emplace_back(d, "own");
            add_sources(t, std::vector<std::string>(a.begin() + static_cast<std::ptrdiff_t>(i), a.end()), c);
            if (targets_.contains(t.name)) stop(c, std::format("there is already a target '{}'", t.name));
            note(c, std::format("the target {}", t.name));
            targets_[t.name] = std::move(t);
            order_.push_back(a[0]);
            return;
        }
        if (n.starts_with("target_")) return target_command(c, a);
        if (n == "include_directories") {
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (a[i] == "SYSTEM" || a[i] == "BEFORE" || a[i] == "AFTER") continue;
                dir_includes_.push_back(absolute(genex(a[i], nullptr, c)));
            }
            return;
        }
        if (n == "add_definitions" || n == "add_compile_definitions") {
            for (std::string d : a) {
                if (d.starts_with("-D")) d = d.substr(2);
                else if (n == "add_definitions") continue;   // other flags
                dir_defines_.push_back(d);
            }
            return;
        }
        if (n == "add_compile_options" || n == "link_libraries" || n == "target_link_options") {
            note(c, "ignored (the build mode decides the compiler options)");
            return;
        }
        if (n == "file") return file_command(c, a);
        if (n == "find_package" || n == "pkg_check_modules") {
            if (a.empty()) return;
            std::string name = n == "pkg_check_modules" ? (a.size() > 1 ? a.back() : a[0]) : a[0];
            std::string prefix = n == "pkg_check_modules" ? a[0] : a[0];
            if (in_.uses.contains(name)) {
                set(prefix + "_FOUND", "TRUE");
                set(upper(prefix) + "_FOUND", "TRUE");
                set(prefix + "_LIBRARIES", name);
                set(upper(prefix) + "_LIBRARIES", name);
                set(prefix + "_INCLUDE_DIRS", "");
                note(c, std::format("{} from the package's .uses", name));
                return;
            }
            static const std::set<std::string> built_in = {"Threads", "PkgConfig"};
            if (built_in.contains(name)) {
                set(name + "_FOUND", "TRUE");
                set(upper(name) + "_FOUND", "TRUE");
                if (name == "Threads") set("CMAKE_THREAD_LIBS_INIT", "-pthread");
                return;
            }
            bool required = std::find(a.begin(), a.end(), "REQUIRED") != a.end();
            // A system library looked up on the build machine.
            std::string lib = lower(name);
            set(name + "_FOUND", "TRUE");
            set(upper(name) + "_FOUND", "TRUE");
            set(name + "_LIBRARIES", lib);
            set(upper(name) + "_LIBRARIES", lib);
            set(name + "_LIBRARY", lib);
            set(upper(name) + "_LIBRARY", lib);
            result_.system_libraries.push_back(lib);
            note(c, std::format("{}: the system library '{}'{}", name, lib, required ? "" : " (optional)"));
            return;
        }
        if (n == "find_library") {
            if (a.size() < 2) stop(c, "find_library needs a variable and a name");
            std::string lib = a[1] == "NAMES" && a.size() > 2 ? a[2] : a[1];
            set(a[0], lib);
            result_.system_libraries.push_back(lib);
            note(c, "the system library " + lib);
            return;
        }
        if (n == "find_path" || n == "find_program") {
            if (!a.empty()) set(a[0], a[0] + "-NOTFOUND");
            note(c, "not found (the reader doesn't search the machine)");
            return;
        }
        if (n == "configure_file") return configure_file(c, a);
        if (n == "set_target_properties") {
            std::size_t props = static_cast<std::size_t>(std::find(a.begin(), a.end(), "PROPERTIES") - a.begin());
            for (std::size_t i = 0; i < props; ++i) {
                for (std::size_t k = props + 1; k + 1 < a.size(); k += 2) property(c, target(c, a[i]), a[k], a[k + 1]);
            }
            return;
        }
        if (n == "set_property") {
            if (!a.empty() && a[0] == "TARGET") {
                std::size_t props = static_cast<std::size_t>(std::find(a.begin(), a.end(), "PROPERTY") - a.begin());
                for (std::size_t i = 1; i < props; ++i) {
                    if (a[i] == "APPEND" || a[i] == "APPEND_STRING") continue;
                    if (props + 1 < a.size()) property(c, target(c, a[i]), a[props + 1], props + 2 < a.size() ? a[props + 2] : "");
                }
            }
            return;
        }
        if (n == "get_property" || n == "get_target_property") {
            if (!a.empty()) set(a[0], "");
            return;
        }
        if (n == "cmake_parse_arguments") return parse_arguments(a);
        if (n == "check_include_file" || n == "check_include_files") {
            if (a.size() < 2) stop(c, n + " needs headers and a variable");
            std::string src;
            for (const std::string& h : split_list(a[0])) src += "#include <" + h + ">\n";
            set(a[1], check(src + "int main(void) { return 0; }\n") ? "1" : "");
            note(c, std::format("{} = {}", a[1], get(a[1]).empty() ? "no" : "yes"));
            return;
        }
        if (n == "check_symbol_exists" || n == "check_function_exists") {
            if (a.size() < 2) stop(c, n + " needs a symbol and a variable");
            std::string src;
            std::string var = n == "check_symbol_exists" ? a.back() : a[1];
            if (n == "check_symbol_exists" && a.size() >= 3) {
                for (const std::string& h : split_list(a[1])) src += "#include <" + h + ">\n";
                src += std::format("int main(void) {{ (void){}; return 0; }}\n", a[0]);
            } else {
                src = std::format("char {}(void);\nint main(void) {{ return (int){}(); }}\n", a[0], a[0]);
            }
            set(var, check(src) ? "1" : "");
            note(c, std::format("{} = {}", var, get(var).empty() ? "no" : "yes"));
            return;
        }
        if (n == "check_c_source_compiles" || n == "check_cxx_source_compiles") {
            if (a.size() < 2) stop(c, n + " needs a source and a variable");
            set(a[1], check(a[0]) ? "1" : "");
            return;
        }
        if (n == "check_type_size") {
            if (a.size() < 2) stop(c, "check_type_size needs a type and a variable");
            static const std::map<std::string, std::string> sizes = {{"int", "4"}, {"long", "8"}, {"long long", "8"}, {"short", "2"},
                                                                    {"void*", "8"}, {"size_t", "8"}, {"off_t", "8"}, {"char", "1"}, {"off64_t", "8"}};
            auto it = sizes.find(a[0]);
            set(a[1], it == sizes.end() ? "" : it->second);
            set("HAVE_" + a[1], it == sizes.end() ? "" : "1");
            return;
        }
        if (n == "check_c_compiler_flag" || n == "check_cxx_compiler_flag") {
            if (a.size() >= 2) set(a[1], "");   // the build mode decides the options
            return;
        }
        if (n == "check_library_exists") {
            if (a.size() >= 4) set(a[3], "1");
            return;
        }
        if (n == "cmake_path" || n == "separate_arguments") {
            if (a.size() >= 2 && a[0] == "SET") set(a[1], a.size() > 2 ? a[2] : "");
            else if (a.size() >= 2 && n == "separate_arguments") set(a[0], std::regex_replace(get(a[0]), std::regex(" +"), ";"));
            else stop(c, n + " in this form");
            return;
        }
        stop(c, std::format("the command '{}' isn't understood by the CMake reader", n));
    }

    void property(const Command& c, CMakeTarget& t, const std::string& name, const std::string& value) {
        if (name == "C_STANDARD") t.standard = "c" + value;
        else if (name == "CXX_STANDARD") t.standard = "cpp" + value;
        else if (name == "POSITION_INDEPENDENT_CODE" || name == "FOLDER" || name == "VERSION" || name == "SOVERSION" ||
                 name == "OUTPUT_NAME" || name == "C_STANDARD_REQUIRED" || name == "CXX_STANDARD_REQUIRED" || name == "C_EXTENSIONS" ||
                 name == "CXX_EXTENSIONS" || name == "PUBLIC_HEADER" || name == "EXPORT_NAME" || name == "DEFINE_SYMBOL" ||
                 name == "C_VISIBILITY_PRESET" || name == "CXX_VISIBILITY_PRESET" || name == "VISIBILITY_INLINES_HIDDEN" ||
                 name.ends_with("_OUTPUT_DIRECTORY") || name == "DEBUG_POSTFIX" || name == "PREFIX" || name == "LINKER_LANGUAGE") {
            return;
        } else if (name == "INTERFACE_INCLUDE_DIRECTORIES") {
            for (const std::string& d : split_list(value)) t.include_dirs.emplace_back(absolute(genex(d, &t, c)), "users");
        } else {
            stop(c, std::format("the target property '{}' isn't understood by the CMake reader", name));
        }
    }

    void target_command(const Command& c, const std::vector<std::string>& a) {
        const std::string& n = c.name;
        if (a.empty()) stop(c, n + " without a target");
        CMakeTarget& t = target(c, a[0]);
        if (n == "target_sources") {
            std::vector<std::string> files;
            for (auto& [f, vis] : with_visibility(a, 1)) {
                if (f == "FILE_SET" || f == "TYPE" || f == "BASE_DIRS" || f == "FILES" || f == "HEADERS") continue;
                if (vis != "users") files.push_back(f);
            }
            add_sources(t, files, c);
            return;
        }
        if (n == "target_include_directories") {
            for (auto& [d, vis] : with_visibility(a, 1, t.kind == CMakeTarget::Kind::interface_library ? "users" : "shared")) {
                std::string path = genex(d, &t, c);
                if (!path.empty()) t.include_dirs.emplace_back(absolute(path), vis);
            }
            return;
        }
        if (n == "target_compile_definitions") {
            for (auto& [d, vis] : with_visibility(a, 1)) {
                std::string def = genex(d, &t, c);
                if (def.starts_with("-D")) def = def.substr(2);
                if (!def.empty()) t.defines.emplace_back(def, vis);
            }
            return;
        }
        if (n == "target_compile_options") {
            note(c, "ignored (the build mode decides the compiler options)");
            return;
        }
        if (n == "target_compile_features") {
            for (auto& [f, vis] : with_visibility(a, 1)) {
                std::smatch m;
                if (std::regex_match(f, m, std::regex("(c|cxx)_std_([0-9]+)"))) t.standard = (m[1] == "c" ? "c" : "cpp") + m[2].str();
            }
            return;
        }
        if (n == "target_link_libraries") {
            for (auto& [l, vis] : with_visibility(a, 1)) {
                if (l == "debug" || l == "optimized" || l == "general" || l == "LINK_PUBLIC" || l == "LINK_PRIVATE") continue;
                std::string lib = genex(l, &t, c);
                if (lib.empty() || lib == "-pthread" || lib == "Threads::Threads" || lib == "m" || lib == "-lm") continue;
                if (lib.starts_with("-l")) lib = lib.substr(2);
                t.links.emplace_back(lib, vis);
            }
            return;
        }
        if (n == "target_link_options" || n == "target_precompile_headers") return;
        stop(c, std::format("the command '{}' isn't understood by the CMake reader", n));
    }

    void list_command(const Command& c, const std::vector<std::string>& a) {
        if (a.size() < 2) stop(c, "list needs a subcommand and a list");
        std::string op = a[0], var = a[1];
        std::vector<std::string> items = split_list(get(var));
        if (op == "APPEND") items.insert(items.end(), a.begin() + 2, a.end());
        else if (op == "PREPEND") items.insert(items.begin(), a.begin() + 2, a.end());
        else if (op == "REMOVE_ITEM") {
            for (std::size_t i = 2; i < a.size(); ++i) items.erase(std::remove(items.begin(), items.end(), a[i]), items.end());
        } else if (op == "REMOVE_DUPLICATES") {
            std::vector<std::string> seen;
            for (const std::string& x : items) {
                if (std::find(seen.begin(), seen.end(), x) == seen.end()) seen.push_back(x);
            }
            items = seen;
        } else if (op == "LENGTH") {
            set(a[2], std::to_string(items.size()));
            return;
        } else if (op == "GET") {
            std::vector<std::string> out;
            for (std::size_t i = 2; i + 1 < a.size(); ++i) {
                long k = std::atol(a[i].c_str());
                if (k < 0) k += static_cast<long>(items.size());
                if (k < 0 || static_cast<std::size_t>(k) >= items.size()) stop(c, "list(GET) index out of range");
                out.push_back(items[static_cast<std::size_t>(k)]);
            }
            set(a.back(), join(out));
            return;
        } else if (op == "FIND") {
            auto it = std::find(items.begin(), items.end(), a[2]);
            set(a[3], it == items.end() ? "-1" : std::to_string(it - items.begin()));
            return;
        } else if (op == "JOIN") {
            set(a[3], join(items, a[2]));
            return;
        } else if (op == "INSERT") {
            std::size_t k = static_cast<std::size_t>(std::atol(a[2].c_str()));
            items.insert(items.begin() + static_cast<std::ptrdiff_t>(std::min(k, items.size())), a.begin() + 3, a.end());
        } else if (op == "REVERSE") {
            std::reverse(items.begin(), items.end());
        } else if (op == "SORT") {
            std::sort(items.begin(), items.end());
        } else if (op == "FILTER") {
            if (a.size() < 5) stop(c, "list(FILTER var INCLUDE|EXCLUDE REGEX re)");
            std::regex re(a[4], std::regex::extended);
            bool include = a[2] == "INCLUDE";
            std::vector<std::string> out;
            for (const std::string& x : items) {
                if (std::regex_search(x, re) == include) out.push_back(x);
            }
            items = out;
        } else if (op == "TRANSFORM") {
            stop(c, "list(TRANSFORM) isn't understood by the CMake reader");
        } else {
            stop(c, "list(" + op + ") isn't understood by the CMake reader");
        }
        set(var, join(items));
    }

    void string_command(const Command& c, const std::vector<std::string>& a) {
        if (a.empty()) stop(c, "string without a subcommand");
        const std::string& op = a[0];
        auto need = [&](std::size_t k) {
            if (a.size() < k) stop(c, "string(" + op + ") needs more arguments");
        };
        if (op == "TOUPPER" || op == "TOLOWER") {
            need(3);
            set(a[2], op == "TOUPPER" ? upper(a[1]) : lower(a[1]));
        } else if (op == "APPEND" || op == "PREPEND") {
            need(2);
            std::string rest;
            for (std::size_t i = 2; i < a.size(); ++i) rest += a[i];
            set(a[1], op == "APPEND" ? get(a[1]) + rest : rest + get(a[1]));
        } else if (op == "CONCAT") {
            need(2);
            std::string v;
            for (std::size_t i = 2; i < a.size(); ++i) v += a[i];
            set(a[1], v);
        } else if (op == "REPLACE") {
            need(4);
            std::string input;
            for (std::size_t i = 4; i < a.size(); ++i) input += a[i];
            std::string out;
            if (a[1].empty()) out = input;
            else {
                std::size_t pos = 0, at;
                while ((at = input.find(a[1], pos)) != std::string::npos) {
                    out += input.substr(pos, at - pos) + a[2];
                    pos = at + a[1].size();
                }
                out += input.substr(pos);
            }
            set(a[3], out);
        } else if (op == "REGEX") {
            need(4);
            std::string input;
            if (a[1] == "REPLACE") {
                need(5);
                for (std::size_t i = 5; i < a.size(); ++i) input += a[i];
                std::string fmt = std::regex_replace(a[3], std::regex("\\\\([0-9])"), "$$$1");
                set(a[4], std::regex_replace(input, std::regex(a[2], std::regex::extended), fmt));
            } else if (a[1] == "MATCH" || a[1] == "MATCHALL") {
                for (std::size_t i = 4; i < a.size(); ++i) input += a[i];
                std::smatch m;
                std::regex  re(a[2], std::regex::extended);
                if (a[1] == "MATCH") {
                    set(a[3], std::regex_search(input, m, re) ? m[0].str() : "");
                } else {
                    std::vector<std::string> all;
                    for (auto it = std::sregex_iterator(input.begin(), input.end(), re); it != std::sregex_iterator(); ++it) all.push_back((*it)[0].str());
                    set(a[3], join(all));
                }
            } else {
                stop(c, "string(REGEX " + a[1] + ")");
            }
        } else if (op == "LENGTH") {
            need(3);
            set(a[2], std::to_string(a[1].size()));
        } else if (op == "SUBSTRING") {
            need(5);
            std::size_t from = static_cast<std::size_t>(std::atol(a[2].c_str()));
            long        len  = std::atol(a[3].c_str());
            set(a[4], from >= a[1].size() ? "" : a[1].substr(from, len < 0 ? std::string::npos : static_cast<std::size_t>(len)));
        } else if (op == "STRIP") {
            need(3);
            std::string v = a[1];
            while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.erase(0, 1);
            while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
            set(a[2], v);
        } else if (op == "FIND") {
            need(4);
            std::size_t at = a.size() > 4 && a[4] == "REVERSE" ? a[1].rfind(a[2]) : a[1].find(a[2]);
            set(a[3], at == std::string::npos ? "-1" : std::to_string(at));
        } else if (op == "COMPARE") {
            need(5);
            const std::string& m = a[1];
            bool r = m == "EQUAL" ? a[2] == a[3] : m == "NOTEQUAL" ? a[2] != a[3] : m == "LESS" ? a[2] < a[3] : a[2] > a[3];
            set(a[4], r ? "1" : "0");
        } else if (op == "JOIN") {
            need(3);
            set(a[2], join(std::vector<std::string>(a.begin() + 3, a.end()), a[1]));
        } else if (op == "TIMESTAMP" || op == "MAKE_C_IDENTIFIER") {
            need(3);
            std::string v = op == "TIMESTAMP" ? "19700101" : std::regex_replace(a[1], std::regex("[^A-Za-z0-9_]"), "_");
            set(op == "TIMESTAMP" ? a[1] : a[2], v);
        } else {
            stop(c, "string(" + op + ") isn't understood by the CMake reader");
        }
    }

    long arithmetic(const std::string& expr, const Command& c) {
        std::size_t p = 0;
        std::function<long()> sum, product, atom;
        auto ws = [&] { while (p < expr.size() && std::isspace(static_cast<unsigned char>(expr[p]))) ++p; };
        atom = [&]() -> long {
            ws();
            if (p < expr.size() && expr[p] == '(') {
                ++p;
                long v = sum();
                ws();
                ++p;
                return v;
            }
            if (p < expr.size() && expr[p] == '-') {
                ++p;
                return -atom();
            }
            if (p < expr.size() && expr[p] == '~') {
                ++p;
                return ~atom();
            }
            std::size_t start = p;
            while (p < expr.size() && std::isalnum(static_cast<unsigned char>(expr[p]))) ++p;
            if (start == p) stop(c, "math(EXPR): cannot read '" + expr + "'");
            return std::stol(expr.substr(start, p - start), nullptr, 0);
        };
        product = [&]() -> long {
            long v = atom();
            while (true) {
                ws();
                if (p < expr.size() && (expr[p] == '*' || expr[p] == '/' || expr[p] == '%')) {
                    char op = expr[p++];
                    long r  = atom();
                    v       = op == '*' ? v * r : r == 0 ? 0 : op == '/' ? v / r : v % r;
                } else {
                    return v;
                }
            }
        };
        sum = [&]() -> long {
            long v = product();
            while (true) {
                ws();
                if (p + 1 < expr.size() && (expr.compare(p, 2, "<<") == 0 || expr.compare(p, 2, ">>") == 0)) {
                    bool left = expr[p] == '<';
                    p += 2;
                    long r = product();
                    v      = left ? v << r : v >> r;
                } else if (p < expr.size() && (expr[p] == '+' || expr[p] == '-' || expr[p] == '&' || expr[p] == '|' || expr[p] == '^')) {
                    char op = expr[p++];
                    long r  = product();
                    v       = op == '+' ? v + r : op == '-' ? v - r : op == '&' ? (v & r) : op == '|' ? (v | r) : (v ^ r);
                } else {
                    return v;
                }
            }
        };
        return sum();
    }

    void file_command(const Command& c, const std::vector<std::string>& a) {
        if (a.empty()) stop(c, "file without a subcommand");
        const std::string& op = a[0];
        if (op == "GLOB" || op == "GLOB_RECURSE") {
            if (a.size() < 2) stop(c, "file(GLOB var patterns...)");
            std::vector<std::string> found;
            fs::path base = current_dir();
            std::size_t i = 2;
            bool relative = false;
            for (; i < a.size(); ++i) {
                if (a[i] == "CONFIGURE_DEPENDS" || a[i] == "LIST_DIRECTORIES") {
                    if (a[i] == "LIST_DIRECTORIES") ++i;
                    continue;
                }
                if (a[i] == "RELATIVE" && i + 1 < a.size()) {
                    base     = absolute(a[++i]);
                    relative = true;
                    continue;
                }
                fs::path pattern = absolute(a[i]);
                fs::path dir     = pattern.parent_path();
                std::string glob = pattern.filename().string();
                std::string re;
                for (char ch : glob) re += ch == '*' ? "[^/]*" : ch == '?' ? "." : ch == '.' ? "\\." : std::string(1, ch);
                std::regex  rx(re);
                std::error_code ec;
                auto take = [&](const fs::path& p) {
                    if (fs::is_regular_file(p, ec) && std::regex_match(p.filename().string(), rx)) {
                        found.push_back(relative ? fs::relative(p, base).string() : p.lexically_normal().string());
                    }
                };
                if (!fs::is_directory(dir, ec)) continue;
                if (op == "GLOB_RECURSE") {
                    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) take(e.path());
                } else {
                    for (const auto& e : fs::directory_iterator(dir, ec)) take(e.path());
                }
            }
            std::sort(found.begin(), found.end());
            set(a[1], join(found));
            note(c, std::format("{} files", found.size()));
            return;
        }
        if (op == "READ") {
            std::ifstream in(absolute(a.at(1)));
            std::stringstream ss;
            ss << in.rdbuf();
            set(a.at(2), ss.str());
            return;
        }
        if (op == "STRINGS") {
            std::ifstream in(absolute(a.at(1)));
            std::vector<std::string> lines;
            for (std::string l; std::getline(in, l);) lines.push_back(l);
            if (a.size() > 4 && a[3] == "REGEX") {
                std::regex re(a[4], std::regex::extended);
                std::vector<std::string> keep;
                for (const std::string& l : lines) {
                    if (std::regex_search(l, re)) keep.push_back(l);
                }
                lines = keep;
            }
            set(a.at(2), join(lines));
            return;
        }
        if (op == "TO_CMAKE_PATH" || op == "TO_NATIVE_PATH") {
            set(a.at(2), a.at(1));
            return;
        }
        if (op == "REAL_PATH") {
            set(a.at(2), absolute(a.at(1)));
            return;
        }
        if (op == "MAKE_DIRECTORY" || op == "WRITE" || op == "APPEND" || op == "COPY" || op == "INSTALL" || op == "REMOVE") {
            stop(c, "file(" + op + ") changes files while the build is read");
        }
        stop(c, "file(" + op + ") isn't understood by the CMake reader");
    }

    void configure_file(const Command& c, const std::vector<std::string>& a) {
        if (a.size() < 2) stop(c, "configure_file(input output)");
        std::ifstream in(absolute(a[0]));
        if (!in) stop(c, "configure_file: cannot read " + a[0]);
        bool at_only = std::find(a.begin(), a.end(), "@ONLY") != a.end();
        std::string out_text;
        for (std::string line; std::getline(in, line);) {
            std::smatch m;
            if (std::regex_match(line, m, std::regex("([ \\t]*)#([ \\t]*)cmakedefine01[ \\t]+([A-Za-z_0-9]+).*"))) {
                line = std::format("{}#{}define {} {}", m[1].str(), m[2].str(), m[3].str(), is_false_constant(get(m[3])) ? 0 : 1);
            } else if (std::regex_match(line, m, std::regex("([ \\t]*)#([ \\t]*)cmakedefine[ \\t]+([A-Za-z_0-9]+)(.*)"))) {
                line = is_false_constant(get(m[3])) ? std::format("/* #undef {} */", m[3].str())
                                                    : std::format("{}#{}define {}{}", m[1].str(), m[2].str(), m[3].str(), m[4].str());
            }
            std::string r;
            for (std::size_t i = 0; i < line.size(); ++i) {
                if (line[i] == '@') {
                    std::size_t j = line.find('@', i + 1);
                    if (j != std::string::npos && j > i + 1 &&
                        std::all_of(line.begin() + static_cast<std::ptrdiff_t>(i) + 1, line.begin() + static_cast<std::ptrdiff_t>(j),
                                    [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; })) {
                        r += get(line.substr(i + 1, j - i - 1));
                        i = j;
                        continue;
                    }
                }
                r += line[i];
            }
            out_text += (at_only ? r : expand(r, c)) + "\n";
        }
        fs::path output = fs::path(a[1]).is_absolute() ? fs::path(a[1]) : fs::path(get("CMAKE_CURRENT_BINARY_DIR")) / a[1];
        std::error_code ec;
        fs::create_directories(output.parent_path(), ec);
        std::ifstream old(output);
        std::stringstream ss;
        ss << old.rdbuf();
        if (!old || ss.str() != out_text) std::ofstream(output) << out_text;
        note(c, "wrote " + output.string());
    }

    void parse_arguments(const std::vector<std::string>& a) {
        // cmake_parse_arguments(prefix options one_value multi_value args...)
        if (a.size() < 4) return;
        std::size_t k = 0;
        bool parse_argv = a[0] == "PARSE_ARGV";
        if (parse_argv) k = 2;
        std::string prefix = a[k];
        auto opts  = split_list(a[k + 1]);
        auto one   = split_list(a[k + 2]);
        auto multi = split_list(a.size() > k + 3 ? a[k + 3] : "");
        std::vector<std::string> input = parse_argv ? split_list(get("ARGV")) : std::vector<std::string>(a.begin() + 4, a.end());
        for (const std::string& o : opts) set(prefix + "_" + o, "FALSE");
        for (const std::string& o : one) unset(prefix + "_" + o);
        for (const std::string& o : multi) unset(prefix + "_" + o);
        std::string current;
        std::vector<std::string> unparsed;
        for (const std::string& x : input) {
            if (std::find(opts.begin(), opts.end(), x) != opts.end()) {
                set(prefix + "_" + x, "TRUE");
                current.clear();
            } else if (std::find(one.begin(), one.end(), x) != one.end() || std::find(multi.begin(), multi.end(), x) != multi.end()) {
                current = x;
                set(prefix + "_" + x, "");
            } else if (!current.empty()) {
                std::string key = prefix + "_" + current;
                std::string v   = get(key);
                set(key, v.empty() ? x : v + ";" + x);
                if (std::find(one.begin(), one.end(), current) != one.end()) current.clear();
            } else {
                unparsed.push_back(x);
            }
        }
        set(prefix + "_UNPARSED_ARGUMENTS", join(unparsed));
    }
};

} // namespace

CMakeResult read_cmake(const CMakeInput& input) { return Reader(input).run(); }

} // namespace rift
