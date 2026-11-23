// The graphics API mapper (see graphics_registry.hpp): Khronos registries -> std modules.
//
// The registries are XML. A small reader turns them into a tree; the generators walk the
// features (API versions) and extensions to find which commands, constants and types exist in
// which version, and write Edax declarations for them:
//
//   - C types become Edax types: GLenum -> u32, `const char*` -> std::raw_ptr<u8>, Vulkan
//     handles -> u64, enums -> i32, bitmasks -> u32 / u64, function pointers -> std::raw_ptr<u8>.
//   - Constants keep their registry names (GL_COLOR_BUFFER_BIT, VK_SUCCESS): the names every
//     tutorial, the specifications and the drivers' messages use.
//   - Vulkan structs become Edax structs with C layout; sType is already filled in.
//   - Each command becomes an `unsafe` method of functions<A>. Its entry check compares the
//     program's API (A) with the versions that have the command, so calling a command the
//     version doesn't have is a compile error that says which version has it.

#include "edax/graphics_registry.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace edax {

std::string_view registry_generator_stamp() { return __DATE__ " " __TIME__; }


namespace {

// ---- a small XML reader ---------------------------------------------------------------------

struct XmlNode {
    std::string                                      name;   // empty: a text node
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XmlNode>                             children;
    std::string                                      text;   // a text node's text

    const std::string* attr(std::string_view key) const {
        for (const auto& [k, v] : attrs) {
            if (k == key) return &v;
        }
        return nullptr;
    }
    std::string attr_or(std::string_view key, std::string_view fallback = "") const {
        const std::string* v = attr(key);
        return v ? *v : std::string(fallback);
    }
    bool is(std::string_view n) const { return name == n; }
    // The first child element named `n`.
    const XmlNode* child(std::string_view n) const {
        for (const XmlNode& c : children) {
            if (c.name == n) return &c;
        }
        return nullptr;
    }
    // All text below this node, in order (`<comment>` elements left out).
    std::string all_text() const {
        if (name.empty()) return text;
        std::string out;
        for (const XmlNode& c : children) {
            if (c.name != "comment") out += c.all_text();
        }
        return out;
    }
};

class XmlReader {
public:
    explicit XmlReader(std::string_view text) : s_(text) {}

    XmlNode document() {
        XmlNode root;
        root.name = "#document";
        read_children(root, "");
        return root;
    }

private:
    [[noreturn]] void fail(std::string_view what) const {
        throw std::runtime_error(std::format("registry XML: {} at byte {}", what, pos_));
    }
    bool starts(std::string_view p) const { return s_.substr(pos_, p.size()) == p; }
    void skip_past(std::string_view end) {
        std::size_t at = s_.find(end, pos_);
        if (at == std::string_view::npos) fail(std::format("missing '{}'", end));
        pos_ = at + end.size();
    }
    static std::string decode(std::string_view raw) {
        std::string out;
        out.reserve(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != '&') {
                out += raw[i];
                continue;
            }
            std::size_t semi = raw.find(';', i);
            if (semi == std::string_view::npos) {
                out += raw[i];
                continue;
            }
            std::string_view ent = raw.substr(i + 1, semi - i - 1);
            if (ent == "lt") out += '<';
            else if (ent == "gt") out += '>';
            else if (ent == "amp") out += '&';
            else if (ent == "quot") out += '"';
            else if (ent == "apos") out += '\'';
            else if (!ent.empty() && ent[0] == '#') {
                unsigned long code = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')
                                         ? std::stoul(std::string(ent.substr(2)), nullptr, 16)
                                         : std::stoul(std::string(ent.substr(1)));
                if (code < 0x80) out += static_cast<char>(code);   // the registries only use ASCII here
            } else {
                out += std::string(raw.substr(i, semi - i + 1));
            }
            i = semi;
        }
        return out;
    }
    std::string read_name() {
        std::size_t start = pos_;
        while (pos_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_' || s_[pos_] == ':' ||
                                    s_[pos_] == '-' || s_[pos_] == '.')) {
            ++pos_;
        }
        if (start == pos_) fail("expected a name");
        return std::string(s_.substr(start, pos_ - start));
    }
    void skip_space() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }
    // Reads the children of `parent` up to `</end>` (or the end of the text for the document).
    void read_children(XmlNode& parent, std::string_view end) {
        while (pos_ < s_.size()) {
            if (starts("<!--")) {
                skip_past("-->");
            } else if (starts("<?")) {
                skip_past("?>");
            } else if (starts("<!")) {
                skip_past(">");
            } else if (starts("</")) {
                pos_ += 2;
                std::string closing = read_name();
                if (closing != end) fail(std::format("'</{}>' closes '<{}>'", closing, end));
                skip_space();
                if (!starts(">")) fail("expected '>'");
                ++pos_;
                return;
            } else if (starts("<")) {
                ++pos_;
                XmlNode e;
                e.name = read_name();
                for (;;) {
                    skip_space();
                    if (starts("/>")) {
                        pos_ += 2;
                        break;
                    }
                    if (starts(">")) {
                        ++pos_;
                        read_children(e, e.name);
                        break;
                    }
                    std::string key = read_name();
                    skip_space();
                    if (!starts("=")) fail("expected '='");
                    ++pos_;
                    skip_space();
                    char quote = s_[pos_];
                    if (quote != '"' && quote != '\'') fail("expected a quoted value");
                    std::size_t close = s_.find(quote, pos_ + 1);
                    if (close == std::string_view::npos) fail("unterminated value");
                    e.attrs.emplace_back(key, decode(s_.substr(pos_ + 1, close - pos_ - 1)));
                    pos_ = close + 1;
                }
                parent.children.push_back(std::move(e));
            } else {
                std::size_t next = s_.find('<', pos_);
                if (next == std::string_view::npos) next = s_.size();
                XmlNode t;
                t.text = decode(s_.substr(pos_, next - pos_));
                pos_   = next;
                parent.children.push_back(std::move(t));
            }
        }
        if (!end.empty()) fail(std::format("'<{}>' is never closed", end));
    }

    std::string_view s_;
    std::size_t      pos_ = 0;
};

// ---- shared helpers ---------------------------------------------------------------------------

// Edax's keywords: a C parameter with one of these names gets a `_` appended.
bool is_keyword(std::string_view w) {
    static const std::set<std::string_view> words = {
        "import", "namespace", "struct", "class", "interface", "enum", "template", "type", "virtual", "override",
        "abstract", "operator", "public", "protected", "private", "static", "extern", "mut", "move", "this", "view",
        "compile_time", "run_time", "deduce", "check", "thread_local", "if", "else", "for", "while", "break",
        "continue", "return", "match", "true", "false", "nullptr", "default", "auto", "cast", "bit", "unsafe", "bool",
        "char", "void", "never", "i8", "i16", "i32", "i64", "i128", "isize", "u8", "u16", "u32", "u64", "u128",
        "usize", "f32", "f64"};
    return words.count(w) != 0;
}

std::string param_name(std::string name) { return is_keyword(name) ? name + "_" : name; }

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    std::size_t              start = 0;
    while (start <= s.size()) {
        std::size_t at = s.find(sep, start);
        if (at == std::string_view::npos) at = s.size();
        out.emplace_back(s.substr(start, at - start));
        start = at + 1;
    }
    return out;
}

bool list_has(std::string_view list, std::string_view item) {
    for (const std::string& part : split(list, ',')) {
        if (part == item) return true;
    }
    return false;
}

// A C declaration split into tokens: identifiers, `*`, `[`, `]`, numbers and `:`.
std::vector<std::string> c_tokens(std::string_view text) {
    std::vector<std::string> out;
    std::size_t              i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t start = i;
            while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_')) ++i;
            out.emplace_back(text.substr(start, i - start));
        } else {
            out.emplace_back(1, c);
            ++i;
        }
    }
    return out;
}

// A C type as the registries write it: `const char* const*` with an array suffix.
struct CType {
    std::string              base;         // "GLchar", "VkInstanceCreateInfo", "void"
    bool                     base_const = false;
    std::vector<bool>        const_after;  // per `*`: whether `const` follows it
    std::vector<std::string> array;        // `[4]`, `[VK_UUID_SIZE]`: outermost first
    int                      bit_field = 0;

    int pointers() const { return static_cast<int>(const_after.size()); }
};

// Parses the type part of a declaration (`const GLchar *const*`, `char name[256]` without the name).
CType parse_ctype(const std::vector<std::string>& tokens) {
    CType t;
    bool  in_array = false;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const std::string& tok = tokens[i];
        if (in_array) {
            if (tok == "]") in_array = false;
            else t.array.push_back(tok);
            continue;
        }
        if (tok == "[") {
            in_array = true;
        } else if (tok == ":") {
            if (i + 1 < tokens.size()) t.bit_field = std::stoi(tokens[i + 1]);
            break;
        } else if (tok == "const") {
            if (t.const_after.empty()) t.base_const = true;
            else t.const_after.back() = true;
        } else if (tok == "*") {
            t.const_after.push_back(false);
        } else if (tok == "struct" || tok == "unsigned" || tok == "signed") {
            if (tok != "struct") t.base = t.base.empty() ? tok : t.base + " " + tok;
        } else if (t.base.empty() || t.base == "unsigned" || t.base == "signed") {
            t.base = t.base.empty() ? tok : t.base + " " + tok;
        }
    }
    return t;
}

std::string wrap_pointers(std::string pointee, const CType& t) {
    bool pointee_mut = !t.base_const;
    for (int i = 0; i < t.pointers(); ++i) {
        pointee     = pointee_mut ? std::format("std::raw_ptr<mut {}>", pointee) : std::format("std::raw_ptr<{}>", pointee);
        pointee_mut = !t.const_after[static_cast<std::size_t>(i)];
    }
    return pointee;
}

// The text of a default value for a field of Edax type `t`.
std::string zero_of(const std::string& t) {
    if (t.starts_with("std::raw_ptr")) return "nullptr";
    if (t == "f32" || t == "f64") return "0.0";
    if (t == "u8" || t == "u16" || t == "u32" || t == "u64" || t == "usize" || t == "i8" || t == "i16" || t == "i32" ||
        t == "i64" || t == "isize") {
        return "0";
    }
    return "default";
}

std::string version_text(int major, int minor) { return std::format("{}.{}", major, minor); }

std::pair<int, int> parse_version(std::string_view number) {
    std::vector<std::string> parts = split(number, '.');
    return {std::stoi(parts[0]), parts.size() > 1 ? std::stoi(parts[1]) : 0};
}

// The rest of a generated file's first comment: where it came from.
std::string generated_note(std::string_view registry, std::string_view version) {
    return std::format("// Generated by edaxc from the Khronos registry {} ({}), embedded in edaxc from\n"
                       "// library/std/graphics/registry. Don't edit: a new registry file is a new version of this module.\n"
                       "// `edaxc registry {}` prints this text.\n",
                       registry, version, registry == "gl.xml" ? "opengl" : "vulkan");
}

// ---- OpenGL ---------------------------------------------------------------------------------------

// The Edax type of a GL scalar type; empty when it isn't one.
std::string gl_scalar(std::string_view c) {
    static const std::map<std::string_view, std::string_view> table = {
        {"GLenum", "u32"},      {"GLboolean", "u8"},   {"GLbitfield", "u32"},   {"GLbyte", "i8"},      {"GLubyte", "u8"},
        {"GLshort", "i16"},     {"GLushort", "u16"},   {"GLint", "i32"},        {"GLuint", "u32"},     {"GLclampx", "i32"},
        {"GLsizei", "i32"},     {"GLfloat", "f32"},    {"GLclampf", "f32"},     {"GLdouble", "f64"},   {"GLclampd", "f64"},
        {"GLchar", "u8"},       {"GLcharARB", "u8"},   {"GLhalf", "u16"},       {"GLhalfARB", "u16"},  {"GLfixed", "i32"},
        {"GLintptr", "isize"},  {"GLintptrARB", "isize"}, {"GLsizeiptr", "isize"}, {"GLsizeiptrARB", "isize"},
        {"GLint64", "i64"},     {"GLint64EXT", "i64"}, {"GLuint64", "u64"},     {"GLuint64EXT", "u64"}, {"GLhalfNV", "u16"},
        {"GLvdpauSurfaceNV", "isize"}, {"GLhandleARB", "u32"}, {"void", "u8"},  {"char", "u8"},        {"int", "i32"},
        {"float", "f32"},       {"double", "f64"},     {"_cl_context", "u8"},   {"_cl_event", "u8"},
    };
    auto it = table.find(c);
    return it == table.end() ? std::string() : std::string(it->second);
}

// GL types that are pointers themselves.
bool gl_pointer_type(std::string_view c) {
    return c == "GLsync" || c == "GLDEBUGPROC" || c == "GLDEBUGPROCARB" || c == "GLDEBUGPROCKHR" || c == "GLDEBUGPROCAMD" ||
           c == "GLeglImageOES" || c == "GLeglClientBufferEXT" || c == "GLVULKANPROCNV";
}

std::optional<std::string> gl_type(const CType& t) {
    if (gl_pointer_type(t.base)) {
        std::string inner = t.base == "GLsync" || t.base.starts_with("GLegl") ? "std::raw_ptr<mut u8>" : "std::raw_ptr<u8>";
        if (t.pointers() == 0) return inner;
        CType rest       = t;
        rest.base_const  = true;   // a pointer type's pointee: read-only from here
        return wrap_pointers(inner, rest);
    }
    std::string scalar = gl_scalar(t.base);
    if (scalar.empty()) return std::nullopt;
    if (t.base == "void" && t.pointers() == 0) return "void";
    return wrap_pointers(scalar, t);
}

struct GlParam {
    std::string name;
    std::string type;        // the C function's Edax type
    std::string wrapper;     // what the method takes (u32 for a GLint that holds an enum)
};

struct GlCommand {
    std::string          name;
    std::string          result;
    std::vector<GlParam> params;
    bool                 ok = true;
    std::string          why;
};

constexpr int gl_version_count = 19;   // 1.0 ... 4.6
constexpr int es_version_count = 4;    // 2.0, 3.0, 3.1, 3.2

} // namespace

GeneratedStdModule generate_opengl_module(std::string_view gl_xml) {
    XmlNode doc = XmlReader(gl_xml).document();
    const XmlNode* registry = doc.child("registry");
    if (!registry) throw std::runtime_error("gl.xml: no <registry>");

    // Constants: name -> value and type, for the api the constant is for.
    struct GlEnum {
        std::string value;
        std::string type;   // "", "u", "ull"
        std::string api;
    };
    std::map<std::string, std::vector<GlEnum>> enums;
    std::set<std::string>                      enum_groups;   // groups that name enums: params of those types hold one
    for (const XmlNode& block : registry->children) {
        if (!block.is("enums")) continue;
        for (const XmlNode& e : block.children) {
            if (!e.is("enum")) continue;
            enums[e.attr_or("name")].push_back(GlEnum{e.attr_or("value"), e.attr_or("type"), e.attr_or("api")});
            for (const std::string& g : split(e.attr_or("group"), ',')) {
                if (!g.empty()) enum_groups.insert(g);
            }
        }
    }

    // Commands.
    std::map<std::string, GlCommand> commands;
    if (const XmlNode* cmds = registry->child("commands")) {
        for (const XmlNode& c : cmds->children) {
            if (!c.is("command")) continue;
            const XmlNode* proto = c.child("proto");
            if (!proto) continue;
            GlCommand cmd;
            std::string ret_text;
            for (const XmlNode& part : proto->children) {
                if (part.is("name")) cmd.name = part.all_text();
                else ret_text += part.all_text() + " ";
            }
            CType ret = parse_ctype(c_tokens(ret_text));
            if (auto r = gl_type(ret)) cmd.result = *r;
            else cmd.ok = false, cmd.why = "result " + ret.base;
            for (const XmlNode& p : c.children) {
                if (!p.is("param")) continue;
                std::string text, name;
                for (const XmlNode& part : p.children) {
                    if (part.is("name")) name = part.all_text();
                    else text += part.all_text() + " ";
                }
                CType pt = parse_ctype(c_tokens(text));
                if (!pt.array.empty()) {   // `GLfloat v[4]` in a parameter is a pointer
                    pt.const_after.push_back(false);
                    pt.array.clear();
                }
                std::optional<std::string> t = gl_type(pt);
                if (!t) {
                    cmd.ok  = false;
                    cmd.why = "parameter type " + pt.base;
                    continue;
                }
                GlParam gp{param_name(name), *t, *t};
                std::string group = p.attr_or("group");
                // A GLint that holds an enum (glTexImage2D's internalformat): the method takes the
                // u32 of the GL_* constant and passes its bits on.
                if ((*t == "i32" || *t == "u32") && pt.pointers() == 0 && pt.base != "GLenum" && !group.empty() &&
                    enum_groups.count(group)) {
                    gp.wrapper = "u32";
                }
                cmd.params.push_back(gp);
            }
            commands[cmd.name] = std::move(cmd);
        }
    }

    // Features: which commands exist in which version and profile. Processed in order, as the
    // registry describes them: a feature adds what it requires and drops what it removes.
    std::map<std::string, std::uint32_t> core_mask, compat_mask, es_mask;
    std::set<std::string>                used_enums;
    std::vector<std::pair<int, int>>     gl_versions, es_versions;
    {
        std::set<std::string> core, compat, es;
        for (const XmlNode& f : registry->children) {
            if (!f.is("feature")) continue;
            std::string api = f.attr_or("api");
            if (api != "gl" && api != "gles2") continue;
            auto [major, minor] = parse_version(f.attr_or("number"));
            bool desktop = api == "gl";
            for (const XmlNode& part : f.children) {
                if (!part.is("require") && !part.is("remove")) continue;
                std::string profile = part.attr_or("profile");
                for (const XmlNode& item : part.children) {
                    std::string name = item.attr_or("name");
                    if (item.is("enum")) {
                        if (part.is("require")) used_enums.insert(name);
                        continue;
                    }
                    if (!item.is("command")) continue;
                    if (!desktop) {
                        if (part.is("require")) es.insert(name);
                        else es.erase(name);
                        continue;
                    }
                    if (part.is("require")) {
                        if (profile != "core") compat.insert(name);
                        if (profile != "compatibility") core.insert(name);
                    } else {
                        if (profile != "compatibility") core.erase(name);
                        if (profile != "core") compat.erase(name);
                    }
                }
            }
            if (desktop) {
                int bit = static_cast<int>(gl_versions.size());
                gl_versions.emplace_back(major, minor);
                bool has_core = major > 3 || (major == 3 && minor >= 2);   // core profiles start at 3.2
                for (const std::string& n : compat) compat_mask[n] |= 1u << bit;
                if (has_core) {
                    for (const std::string& n : core) core_mask[n] |= 1u << bit;
                }
            } else {
                int bit = static_cast<int>(es_versions.size());
                es_versions.emplace_back(major, minor);
                for (const std::string& n : es) es_mask[n] |= 1u << bit;
            }
        }
    }
    if (gl_versions.size() != gl_version_count || es_versions.size() != es_version_count) {
        throw std::runtime_error(std::format("gl.xml: expected {} OpenGL and {} OpenGL ES versions, found {} and {}",
                                             gl_version_count, es_version_count, gl_versions.size(), es_versions.size()));
    }

    GeneratedStdModule out;
    out.registry_version = std::format("OpenGL {}, OpenGL ES {}", version_text(gl_versions.back().first, gl_versions.back().second),
                                       version_text(es_versions.back().first, es_versions.back().second));

    std::ostringstream h, s;
    h << "// The Edax standard library: module std::graphics::opengl, OpenGL and OpenGL ES.\n//\n"
      << generated_note("gl.xml", out.registry_version)
      << "//\n// Every GL_* constant of the core versions, and functions<A>: a method for every command of\n"
         "// OpenGL 1.0-4.6 and OpenGL ES 2.0-3.2. Calling a command that the program's API version A\n"
         "// doesn't have is a compile error naming the versions that have it.\n//\n"
         "//     compile_time std::graphics::api gl_33 = std::graphics::api{\n"
         "//         .name = std::graphics::api_name::opengl_core, .version = std::version{ .major = 3, .minor = 3 } };\n"
         "//     mut std::windowing::opengl::frame frame = win.begin_frame();\n"
         "//     std::graphics::opengl::functions<gl_33> gl = std::graphics::opengl::functions<gl_33>::load(frame).expect(\"GL\");\n"
         "//     unsafe { gl.glClear(std::graphics::opengl::GL_COLOR_BUFFER_BIT); }\n\n"
      << "namespace std::graphics::opengl {\n";

    // Constants.
    h << "    // ---- constants ------------------------------------------------------------------------------\n\n";
    for (const std::string& name : used_enums) {
        auto it = enums.find(name);
        if (it == enums.end()) continue;
        const GlEnum* pick = nullptr;
        for (const GlEnum& e : it->second) {
            if (e.api.empty() || e.api == "gl") {
                pick = &e;
                break;
            }
        }
        if (!pick) pick = &it->second.front();
        std::string type = pick->type == "ull" ? "u64" : "u32";
        if (name == "GL_TRUE" || name == "GL_FALSE") type = "u8";   // GLboolean
        h << std::format("    compile_time {} {} = {};\n", type, name, pick->value);
        ++out.constants;
    }

    // The version tables, used by the checks.
    h << "\n    // ---- versions ---------------------------------------------------------------------------------\n\n"
         "    // Whether the registry has OpenGL API `a` (a version of OpenGL, OpenGL ES, a valid profile).\n"
         "    compile_time bool known(std::graphics::api a);\n"
         "    // Whether a command with these version masks (bit i: the i-th version of the registry)\n"
         "    // exists in `a`.\n"
         "    compile_time run_time bool has(std::graphics::api a, u32 core, u32 compatibility, u32 es);\n"
         "    // The versions the registry knows, as text: what the checks print.\n"
         "    compile_time std::static_str versions(std::graphics::api_name name);\n\n";

    s << "// The Edax standard library: bodies of module std::graphics::opengl (see opengl.edh).\n//\n"
      << generated_note("gl.xml", out.registry_version) << "\nnamespace std::graphics::opengl {\n";
    auto version_list = [](const std::vector<std::pair<int, int>>& v, int from) {
        std::string text;
        for (std::size_t i = static_cast<std::size_t>(from); i < v.size(); ++i) text += (text.empty() ? "" : ", ") + version_text(v[i].first, v[i].second);
        return text;
    };
    auto index_body = [](const std::vector<std::pair<int, int>>& v) {
        std::string body;
        for (std::size_t i = 0; i < v.size(); ++i) {
            body += std::format("        if (v.major == {} && v.minor == {}) {{ return {}; }}\n", v[i].first, v[i].second, i);
        }
        return body + "        return 32;\n";
    };
    s << "    // The bit of a version in the masks below, or 32 when the registry has no such version.\n"
      << "    private compile_time run_time u32 gl_index(std::version v) {\n" << index_body(gl_versions) << "    }\n\n"
      << "    private compile_time run_time u32 es_index(std::version v) {\n" << index_body(es_versions) << "    }\n\n"
      << "    compile_time bool known(std::graphics::api a) {\n"
         "        if (a.version.patch != 0) { return false; }\n"
         "        return match (a.name) {\n"
         "            std::graphics::api_name::opengl_core          => gl_index(a.version) < 32 && a.version.at_least(std::version{ .major = 3, .minor = 2 }),\n"
         "            std::graphics::api_name::opengl_compatibility => gl_index(a.version) < 32,\n"
         "            std::graphics::api_name::opengl_es            => es_index(a.version) < 32,\n"
         "            std::graphics::api_name::vulkan               => false,\n"
         "        };\n"
         "    }\n\n"
         "    compile_time run_time bool has(std::graphics::api a, u32 core, u32 compatibility, u32 es) {\n"
         "        return match (a.name) {\n"
         "            std::graphics::api_name::opengl_core          => gl_index(a.version) < 32 && ((core >> gl_index(a.version)) & 1) != 0,\n"
         "            std::graphics::api_name::opengl_compatibility => gl_index(a.version) < 32 && ((compatibility >> gl_index(a.version)) & 1) != 0,\n"
         "            std::graphics::api_name::opengl_es            => es_index(a.version) < 32 && ((es >> es_index(a.version)) & 1) != 0,\n"
         "            std::graphics::api_name::vulkan               => false,\n"
         "        };\n"
         "    }\n\n"
      << "    compile_time std::static_str versions(std::graphics::api_name name) {\n"
         "        return match (name) {\n"
      << std::format("            std::graphics::api_name::opengl_core          => \"{}\",\n",
                     version_list(gl_versions, static_cast<int>(std::find(gl_versions.begin(), gl_versions.end(), std::make_pair(3, 2)) - gl_versions.begin())))
      << std::format("            std::graphics::api_name::opengl_compatibility => \"{}\",\n", version_list(gl_versions, 0))
      << std::format("            std::graphics::api_name::opengl_es            => \"{}\",\n", version_list(es_versions, 0))
      << "            std::graphics::api_name::vulkan               => \"none\",\n"
         "        };\n"
         "    }\n\n";

    // Where a command exists, as text for the check's message.
    auto first_bit = [](std::uint32_t m) { return m ? std::countr_zero(m) : -1; };
    auto where = [&](std::uint32_t core, std::uint32_t compat, std::uint32_t es) {
        std::vector<std::string> parts;
        auto range = [&](std::uint32_t mask, const std::vector<std::pair<int, int>>& v, std::string_view label) {
            int first = first_bit(mask);
            if (first < 0) return;
            int last = 31 - std::countl_zero(mask);
            bool until_latest = last == static_cast<int>(v.size()) - 1;
            std::string text = std::format("{} {}", label, version_text(v[static_cast<std::size_t>(first)].first, v[static_cast<std::size_t>(first)].second));
            text += until_latest ? "+" : std::format(" to {}", version_text(v[static_cast<std::size_t>(last)].first, v[static_cast<std::size_t>(last)].second));
            parts.push_back(text);
        };
        range(core, gl_versions, "OpenGL Core");
        range(compat, gl_versions, "OpenGL Compatibility");
        range(es, es_versions, "OpenGL ES");
        std::string text;
        for (const std::string& p : parts) text += (text.empty() ? "" : ", ") + p;
        return text;
    };

    // The class.
    std::vector<const GlCommand*> emitted;
    for (const auto& [name, cmd] : commands) {
        if (!cmd.ok) continue;
        if (!core_mask.count(name) && !compat_mask.count(name) && !es_mask.count(name)) continue;
        emitted.push_back(&cmd);
    }
    out.commands = emitted.size();

    h << "    // ---- functions ---------------------------------------------------------------------------------\n\n"
         "    // The commands of OpenGL API `A`. Load them with a context current (std::windowing::opengl::frame\n"
         "    // or ::current): load() looks up exactly the commands A has. A method A doesn't have is a\n"
         "    // compile error at the call. The methods are `unsafe`: they call into the driver, which\n"
         "    // trusts every pointer and size, and need the context they were loaded for to be current.\n"
         "    template<std::graphics::api A>\n"
         "    class functions {\n"
         "    public:\n"
         "        // Every command of A from `from`; error(name): the driver doesn't have `name`.\n"
         "        static std::result<functions<A>, std::string> load(std::graphics::function_source& from);\n\n";
    // The tables load() walks: the commands' names and, per command, its version masks.
    std::string names, core_list, compat_list, es_list;
    for (const GlCommand* c : emitted) {
        names += (names.empty() ? "" : " ") + c->name;
        core_list += std::format("{}0x{:x}", core_list.empty() ? "" : ", ", core_mask.count(c->name) ? core_mask[c->name] : 0u);
        compat_list += std::format("{}0x{:x}", compat_list.empty() ? "" : ", ", compat_mask.count(c->name) ? compat_mask[c->name] : 0u);
        es_list += std::format("{}0x{:x}", es_list.empty() ? "" : ", ", es_mask.count(c->name) ? es_mask[c->name] : 0u);
    }
    s << "    // The commands in the order of functions<A>'s table, and the versions that have each one.\n"
      << std::format("    private compile_time std::static_str command_names = \"{}\";\n", names)
      << std::format("    private compile_time std::array<u32, {}> core_masks = [{}];\n", emitted.size(), core_list)
      << std::format("    private compile_time std::array<u32, {}> compatibility_masks = [{}];\n", emitted.size(), compat_list)
      << std::format("    private compile_time std::array<u32, {}> es_masks = [{}];\n\n", emitted.size(), es_list)
      << "    template<std::graphics::api A>\n"
         "    class functions {\n"
         "        compile_time check (std::graphics::is_opengl(A.name))\n"
         "            else f\"std::graphics::opengl::functions: {std::graphics::name_text(A.name)} is not an OpenGL API (see std::graphics::vulkan)\";\n"
         "        compile_time check (!std::graphics::is_opengl(A.name) || known(A))\n"
         "            else f\"{std::graphics::name_text(A.name)} {A.version.major}.{A.version.minor}.{A.version.patch} doesn't exist; the versions are {versions(A.name)} (patch 0)\";\n"
         "    public:\n"
         "        static std::result<functions<A>, std::string> load(std::graphics::function_source& from) {\n"
         "            mut functions<A> f = functions<A>{};\n"
         "            std::darray<std::str> names = command_names.split(' ');\n"
         "            for (usize i : 0..names.len()) {\n"
         "                if (has(A, core_masks[i], compatibility_masks[i], es_masks[i])) {\n"
         "                    match (from.proc_address(names[i])) {\n"
         "                        std::some(address) => { f.p[i] = address; },\n"
         "                        std::none          => return std::error(std::string::from(names[i])),\n"
         "                    }\n"
         "                }\n"
         "            }\n"
         "            return std::ok(move f);\n"
         "        }\n\n";
    std::size_t slot = 0;
    for (const GlCommand* c : emitted) {
        std::uint32_t core = core_mask.count(c->name) ? core_mask[c->name] : 0;
        std::uint32_t compat = compat_mask.count(c->name) ? compat_mask[c->name] : 0;
        std::uint32_t es = es_mask.count(c->name) ? es_mask[c->name] : 0;
        std::string cond = std::format("has(A, 0x{:x}, 0x{:x}, 0x{:x})", core, compat, es);
        std::string params, args, ctypes;
        for (const GlParam& p : c->params) {
            params += (params.empty() ? "" : ", ") + p.wrapper + " " + p.name;
            ctypes += (ctypes.empty() ? "" : ", ") + p.type;
            std::string arg = p.wrapper == p.type ? p.name : std::format("cast::truncate<{}>({})", p.type, p.name);
            args += (args.empty() ? "" : ", ") + arg;
        }
        std::string place = where(core, compat, es);
        h << std::format("        // {}\n        unsafe {} {}({});\n", place, c->result, c->name, params);
        std::string call = std::format("cast::reinterpret<std::fn_ptr<{}({})>>(this.p[{}])({})", c->result, ctypes, slot, args);
        s << std::format("        unsafe {} {}({}) {{\n"
                         "            compile_time check ({})\n"
                         "                else f\"{} isn't in {{std::graphics::name_text(A.name)}} {{A.version.major}}.{{A.version.minor}}: it is in {}\";\n"
                         "            {}{};\n"
                         "        }}\n\n",
                         c->result, c->name, params, cond, c->name, place, c->result == "void" ? "" : "return ", call);
        ++slot;
    }
    h << "    }\n}\n";
    s << "    private:\n" << std::format("        std::array<std::raw_ptr<u8>, {}> p = default;\n", emitted.size()) << "    }\n}\n";
    out.header = h.str();
    out.source = s.str();
    return out;
}

// ---- Vulkan -------------------------------------------------------------------------------------

namespace {

// The extensions std::graphics::vulkan describes besides the core versions: what a program
// needs to show something in a window and to debug.
const std::vector<std::string_view>& vulkan_extensions() {
    static const std::vector<std::string_view> list = {"VK_KHR_surface", "VK_KHR_swapchain", "VK_KHR_xlib_surface",
                                                       "VK_EXT_debug_utils"};
    return list;
}

struct VkMember {
    std::string name;
    CType       ctype;
    std::string values;   // sType's value
};

struct VkTypeInfo {
    std::string           category;   // struct, union, handle, enum, bitmask, basetype, funcpointer, ...
    std::string           alias;
    std::string           base;       // basetype / bitmask: the type it is
    std::string           requires_;  // bitmask: its FlagBits enum
    std::vector<VkMember> members;
    bool                  dispatchable = false;
    bool                  bitmask64 = false;
};

struct VkEnumValue {
    std::string name;
    std::string value;   // the Edax literal, or empty for an alias
    std::string alias;
    std::string type;    // the Edax type
};

struct VkCommandInfo {
    std::string           name;
    std::string           alias;
    CType                 result;
    std::vector<VkMember> params;
};

class VulkanGenerator {
public:
    explicit VulkanGenerator(const XmlNode& registry) : r_(registry) {}

    GeneratedStdModule run() {
        read_types();
        read_enums();
        read_commands();
        read_features();
        read_extensions();
        close_types();
        return write();
    }

private:
    static bool for_vulkan(const XmlNode& n) {
        const std::string* api = n.attr("api");
        return !api || list_has(*api, "vulkan");
    }

    void read_types() {
        const XmlNode* types = r_.child("types");
        if (!types) throw std::runtime_error("vk.xml: no <types>");
        for (const XmlNode& t : types->children) {
            if (!t.is("type") || !for_vulkan(t)) continue;
            VkTypeInfo info;
            info.category = t.attr_or("category");
            std::string name = t.attr_or("name");
            if (const XmlNode* n = t.child("name")) name = n->all_text();
            if (const XmlNode* p = t.child("proto"); p && info.category == "funcpointer") {
                if (const XmlNode* n = p->child("name")) name = n->all_text();
            }
            if (name.empty()) continue;
            info.alias     = t.attr_or("alias");
            info.requires_ = t.attr_or("requires");
            if (info.requires_.empty()) info.requires_ = t.attr_or("bitvalues");
            if (info.category == "basetype" || info.category == "bitmask") {
                if (const XmlNode* bt = t.child("type")) info.base = bt->all_text();
                info.bitmask64 = info.base == "VkFlags64";
            }
            if (info.category == "handle") {
                if (const XmlNode* bt = t.child("type")) info.dispatchable = bt->all_text() == "VK_DEFINE_HANDLE";
            }
            if (info.category == "struct" || info.category == "union") {
                for (const XmlNode& m : t.children) {
                    if (!m.is("member") || !for_vulkan(m)) continue;
                    VkMember vm;
                    std::string text;
                    for (const XmlNode& part : m.children) {
                        if (part.is("name")) {
                            vm.name = part.all_text();
                            text += " ";
                        } else if (!part.is("comment")) {
                            text += part.all_text() + " ";
                        }
                    }
                    // The text before the name is the type; after it, the array and bit-field.
                    vm.ctype  = parse_ctype(c_tokens(text));
                    vm.values = m.attr_or("values");
                    info.members.push_back(vm);
                }
            }
            types_[name] = info;
        }
    }

    void read_enums() {
        for (const XmlNode& block : r_.children) {
            if (!block.is("enums")) continue;
            std::string type_name = block.attr_or("name");
            std::string kind      = block.attr_or("type");
            if (kind == "bitmask" && block.attr_or("bitwidth") == "64") bitmask64_enums_.insert(type_name);
            for (const XmlNode& e : block.children) {
                if (!e.is("enum") || !for_vulkan(e)) continue;
                VkEnumValue v;
                v.name  = e.attr_or("name");
                v.alias = e.attr_or("alias");
                if (kind == "constants") {
                    std::string ctype = e.attr_or("type");
                    std::string raw   = e.attr_or("value");
                    v.type            = ctype == "uint64_t" ? "u64" : ctype == "float" ? "f32" : ctype == "int32_t" ? "i32" : "u32";
                    v.value           = constant_literal(raw, v.type);
                    constants_.push_back(v);
                    continue;
                }
                v.type = enum_value_type(type_name, kind);
                if (const std::string* bp = e.attr("bitpos")) v.value = std::format("0x{:x}", std::uint64_t{1} << std::stoi(*bp));
                else if (const std::string* val = e.attr("value")) v.value = *val;
                enum_values_[type_name].push_back(v);
            }
        }
    }

    std::string enum_value_type(const std::string& type_name, const std::string& kind) const {
        if (kind == "bitmask") return bitmask64_enums_.count(type_name) ? "u64" : "u32";
        return "i32";
    }

    static std::string constant_literal(std::string raw, const std::string& type) {
        raw.erase(std::remove(raw.begin(), raw.end(), '('), raw.end());
        raw.erase(std::remove(raw.begin(), raw.end(), ')'), raw.end());
        if (raw.starts_with("~")) {   // ~0U, ~1U, ~0ULL, ~2U: the complement
            std::string digits = raw.substr(1);
            while (!digits.empty() && (digits.back() == 'U' || digits.back() == 'L')) digits.pop_back();
            std::uint64_t n = std::stoull(digits);
            return type == "u64" ? std::format("0x{:x}", ~n) : std::format("0x{:x}", static_cast<std::uint32_t>(~n));
        }
        while (!raw.empty() && (raw.back() == 'U' || raw.back() == 'L' || raw.back() == 'F' || raw.back() == 'f')) raw.pop_back();
        if (type == "f32" && raw.find('.') == std::string::npos) raw += ".0";
        return raw;
    }

    void read_commands() {
        const XmlNode* cmds = r_.child("commands");
        if (!cmds) throw std::runtime_error("vk.xml: no <commands>");
        for (const XmlNode& c : cmds->children) {
            if (!c.is("command") || !for_vulkan(c)) continue;
            VkCommandInfo info;
            if (const std::string* alias = c.attr("alias")) {
                info.name  = c.attr_or("name");
                info.alias = *alias;
                commands_[info.name] = info;
                continue;
            }
            const XmlNode* proto = c.child("proto");
            if (!proto) continue;
            std::string ret;
            for (const XmlNode& part : proto->children) {
                if (part.is("name")) info.name = part.all_text();
                else ret += part.all_text() + " ";
            }
            info.result = parse_ctype(c_tokens(ret));
            for (const XmlNode& p : c.children) {
                if (!p.is("param") || !for_vulkan(p)) continue;
                VkMember m;
                std::string text;
                for (const XmlNode& part : p.children) {
                    if (part.is("name")) m.name = part.all_text();
                    else text += part.all_text() + " ";
                }
                m.ctype = parse_ctype(c_tokens(text));
                info.params.push_back(m);
            }
            commands_[info.name] = info;
        }
    }

    // A <require>'s items: types, enums (maybe extending a type) and commands.
    void take_require(const XmlNode& req, int major, int minor, int extnumber) {
        for (const XmlNode& item : req.children) {
            if (!for_vulkan(item)) continue;
            std::string name = item.attr_or("name");
            if (item.is("type")) {
                wanted_types_.insert(name);
            } else if (item.is("command")) {
                auto [it, fresh] = since_.try_emplace(name, std::make_pair(major, minor));
                if (!fresh && major >= 0 && (it->second.first < 0 || std::make_pair(major, minor) < it->second)) it->second = {major, minor};
                wanted_commands_.insert(name);
            } else if (item.is("enum")) {
                std::string extends = item.attr_or("extends");
                if (extends.empty()) {
                    if (item.attr("value") || item.attr("alias")) {   // an extension's own constant
                        std::string value = item.attr_or("value");
                        if (!value.empty() && value.front() == '"') {
                            ext_strings_.emplace_back(name, value.substr(1, value.size() - 2));
                        } else if (!value.empty() && (std::isdigit(static_cast<unsigned char>(value[0])) || value[0] == '-')) {
                            constants_.push_back(VkEnumValue{name, value, "", "u32"});
                        } else {
                            wanted_enums_.insert(name);
                        }
                    } else {
                        wanted_enums_.insert(name);
                    }
                    continue;
                }
                VkEnumValue v;
                v.name  = name;
                v.alias = item.attr_or("alias");
                v.type  = types_.count(extends) && bitmask_type_of_enum(extends) ? (bitmask64_enums_.count(extends) ? "u64" : "u32") : "i32";
                if (const std::string* bp = item.attr("bitpos")) {
                    v.value = std::format("0x{:x}", std::uint64_t{1} << std::stoi(*bp));
                } else if (const std::string* off = item.attr("offset")) {
                    int number = item.attr("extnumber") ? std::stoi(item.attr_or("extnumber")) : extnumber;
                    long long value = 1000000000LL + (number - 1) * 1000LL + std::stoll(*off);
                    if (item.attr_or("dir") == "-") value = -value;
                    v.value = std::to_string(value);
                } else if (const std::string* val = item.attr("value")) {
                    v.value = *val;
                }
                auto& list = enum_values_[extends];
                if (std::none_of(list.begin(), list.end(), [&](const VkEnumValue& e) { return e.name == v.name; })) list.push_back(v);
                wanted_types_.insert(extends);
            }
        }
    }

    bool bitmask_type_of_enum(const std::string& enum_name) const {
        // FlagBits enums are declared in <enums type="bitmask">; read_enums noted the 64-bit ones.
        for (const XmlNode& block : r_.children) {
            if (block.is("enums") && block.attr_or("name") == enum_name) return block.attr_or("type") == "bitmask";
        }
        return false;
    }

    void read_features() {
        for (const XmlNode& f : r_.children) {
            if (!f.is("feature") || !list_has(f.attr_or("api"), "vulkan")) continue;
            auto [major, minor] = parse_version(f.attr_or("number"));
            versions_.insert({major, minor});
            for (const XmlNode& req : f.children) {
                if (req.is("require")) take_require(req, major, minor, 0);
            }
        }
    }

    bool depends_ok(std::string_view expr) const {
        // `,` is or, `+` is and, parentheses group. Core versions are all here; the listed
        // extensions are.
        std::size_t pos = 0;
        std::function<bool()> any_of, all_of, atom;
        any_of = [&]() {
            bool v = all_of();
            while (pos < expr.size() && expr[pos] == ',') {
                ++pos;
                bool w = all_of();
                v      = v || w;
            }
            return v;
        };
        all_of = [&]() {
            bool v = atom();
            while (pos < expr.size() && expr[pos] == '+') {
                ++pos;
                bool w = atom();
                v      = v && w;
            }
            return v;
        };
        atom = [&]() {
            if (pos < expr.size() && expr[pos] == '(') {
                ++pos;
                bool v = any_of();
                if (pos < expr.size() && expr[pos] == ')') ++pos;
                return v;
            }
            std::size_t start = pos;
            while (pos < expr.size() && expr[pos] != ',' && expr[pos] != '+' && expr[pos] != ')') ++pos;
            std::string name(expr.substr(start, pos - start));
            if (name.starts_with("VK_VERSION_")) return true;
            if (name.find("::") != std::string::npos) return false;   // a feature bit
            return std::find(vulkan_extensions().begin(), vulkan_extensions().end(), name) != vulkan_extensions().end();
        };
        return any_of();
    }

    void read_extensions() {
        const XmlNode* exts = r_.child("extensions");
        if (!exts) return;
        for (const XmlNode& e : exts->children) {
            if (!e.is("extension")) continue;
            std::string name = e.attr_or("name");
            if (std::find(vulkan_extensions().begin(), vulkan_extensions().end(), name) == vulkan_extensions().end()) continue;
            int number = std::stoi(e.attr_or("number"));
            for (const XmlNode& req : e.children) {
                if (!req.is("require") || !for_vulkan(req)) continue;
                if (const std::string* dep = req.attr("depends"); dep && !depends_ok(*dep)) continue;
                take_require(req, -1, 0, number);   // extension commands: declared for every version
            }
        }
    }

    // The types the wanted types and commands need, recursively.
    void close_types() {
        std::vector<std::string> todo(wanted_types_.begin(), wanted_types_.end());
        for (const std::string& c : wanted_commands_) {
            const VkCommandInfo* info = command(c);
            if (!info) continue;
            todo.push_back(info->result.base);
            for (const VkMember& p : info->params) todo.push_back(p.ctype.base);
        }
        while (!todo.empty()) {
            std::string t = todo.back();
            todo.pop_back();
            if (!types_.count(t) || closed_.count(t)) continue;
            closed_.insert(t);
            const VkTypeInfo& info = types_[t];
            if (!info.alias.empty()) todo.push_back(info.alias);
            if (!info.base.empty()) todo.push_back(info.base);
            if (!info.requires_.empty()) todo.push_back(info.requires_);
            for (const VkMember& m : info.members) todo.push_back(m.ctype.base);
        }
    }

    const VkCommandInfo* command(const std::string& name) const {
        auto it = commands_.find(name);
        if (it == commands_.end()) return nullptr;
        if (!it->second.alias.empty()) return command(it->second.alias);
        return &it->second;
    }

    // The Edax type of a type name, without pointers.
    std::string scalar(const std::string& c) const {
        static const std::map<std::string_view, std::string_view> table = {
            {"void", "u8"},      {"char", "u8"},       {"float", "f32"},     {"double", "f64"},   {"int8_t", "i8"},
            {"uint8_t", "u8"},   {"int16_t", "i16"},   {"uint16_t", "u16"},  {"int32_t", "i32"},  {"uint32_t", "u32"},
            {"int64_t", "i64"},  {"uint64_t", "u64"},  {"size_t", "usize"},  {"int", "i32"},      {"Display", "u8"},
            {"Window", "u64"},   {"VisualID", "u64"},
        };
        if (auto it = table.find(c); it != table.end()) return std::string(it->second);
        auto t = types_.find(c);
        if (t == types_.end()) return "u8";   // an opaque platform type, only seen behind a pointer
        const VkTypeInfo& info = t->second;
        if (!info.alias.empty()) return scalar(info.alias);
        if (info.category == "handle") return "u64";
        if (info.category == "enum") return bitmask_type_of_enum(c) ? (bitmask64_enums_.count(c) ? "u64" : "u32") : "i32";
        if (info.category == "bitmask") return info.bitmask64 ? "u64" : "u32";
        if (info.category == "basetype") return info.base.empty() ? "u8" : scalar(info.base);
        if (info.category == "funcpointer") return "std::raw_ptr<u8>";
        if (info.category == "struct" || info.category == "union") return c;
        return "u8";
    }

    std::string array_count(const std::string& n) const {
        if (!n.empty() && std::isdigit(static_cast<unsigned char>(n[0]))) return n;
        for (const VkEnumValue& c : constants_) {
            if (c.name == n) return c.value;
        }
        return n;
    }

    std::string edax_type(const CType& t) const {
        std::string base = scalar(t.base);
        if (t.pointers() == 0) {
            if (t.base == "void") return "void";
            std::string out = base;
            for (auto it = t.array.rbegin(); it != t.array.rend(); ++it) out = std::format("std::raw::array<{}, {}>", out, array_count(*it));
            return out;
        }
        if (base == "std::raw_ptr<u8>") {   // a pointer to a function pointer
            CType rest      = t;
            rest.base_const = true;
            return wrap_pointers(base, rest);
        }
        return wrap_pointers(base, t);
    }

    // The size and alignment of an Edax-mapped C type, for unions.
    std::pair<std::size_t, std::size_t> layout(const CType& t) const {
        std::pair<std::size_t, std::size_t> one;
        if (t.pointers() > 0) one = {8, 8};
        else one = layout_of_name(t.base);
        std::size_t count = 1;
        for (const std::string& a : t.array) count *= static_cast<std::size_t>(std::stoull(array_count(a)));
        return {one.first * count, one.second};
    }

    std::pair<std::size_t, std::size_t> layout_of_name(const std::string& name) const {
        std::string s = scalar(name);
        if (s == "u8" || s == "i8") return {1, 1};
        if (s == "u16" || s == "i16") return {2, 2};
        if (s == "u32" || s == "i32" || s == "f32") return {4, 4};
        if (s == "u64" || s == "i64" || s == "f64" || s == "usize" || s.starts_with("std::raw_ptr")) return {8, 8};
        auto it = types_.find(name);
        if (it == types_.end()) return {1, 1};
        const VkTypeInfo& info = it->second;
        if (!info.alias.empty()) return layout_of_name(info.alias);
        std::size_t size = 0, align = 1;
        for (const VkMember& m : info.members) {
            auto [ms, ma] = layout(m.ctype);
            align         = std::max(align, ma);
            if (info.category == "union") {
                size = std::max(size, ms);
            } else {
                size = (size + ma - 1) / ma * ma + ms;
            }
        }
        size = (size + align - 1) / align * align;
        return {size, align};
    }

    std::string field_default(const VkMember& m, const std::string& type, const std::set<std::string>& written) const {
        if (!m.values.empty() && m.name == "sType" && written.count(m.values)) return m.values;
        return zero_of(type);
    }

    GeneratedStdModule write() {
        GeneratedStdModule out;
        int latest_major = versions_.rbegin()->first, latest_minor = versions_.rbegin()->second;
        std::string header_version;
        for (const XmlNode& t : r_.child("types")->children) {
            if (!t.is("type") || !for_vulkan(t)) continue;
            const XmlNode* n = t.child("name");
            if (n && n->all_text() == "VK_HEADER_VERSION") {
                std::string text = t.all_text();
                std::size_t at   = text.find("VK_HEADER_VERSION");
                header_version   = std::to_string(std::stoi(text.substr(at + 17)));
            }
        }
        out.registry_version = std::format("{}.{}.{}", latest_major, latest_minor, header_version);

        std::ostringstream h, s;
        h << "// The Edax standard library: module std::graphics::vulkan, Vulkan.\n//\n"
          << generated_note("vk.xml", out.registry_version)
          << "//\n// The constants, types and structs of Vulkan 1.0-" << latest_major << "." << latest_minor
          << " and of the extensions\n// ";
        for (std::string_view e : vulkan_extensions()) h << e << " ";
        h << "\n// and functions<A>, a method for every command. Names are the registry's. Handles are u64,\n"
             "// enums i32, flags u32 (or u64), function pointers std::raw_ptr<u8>. A struct's sType is filled in.\n"
             "// A command of a later version than A is a compile error at the call; an extension's command\n"
             "// is a run-time panic when the instance or device doesn't have it.\n//\n"
             "//     compile_time std::graphics::api vk_13 = std::graphics::api{\n"
             "//         .name = std::graphics::api_name::vulkan, .version = std::version{ .major = 1, .minor = 3 } };\n"
             "//     mut std::graphics::vulkan::functions<vk_13> vk = std::graphics::vulkan::functions<vk_13>::load(loader)?;\n"
             "//     ... vk.vkCreateInstance(...) ...\n"
             "//     vk.load_instance(instance)?;     // the instance's and its devices' commands\n\n"
          << "namespace std::graphics::vulkan {\n"
          << "    // ---- constants ------------------------------------------------------------------------------\n\n";
        std::set<std::string> written;
        auto constant = [&](const std::string& name, const std::string& type, const std::string& value) {
            if (!written.insert(name).second) return;
            h << std::format("    compile_time {} {} = {};\n", type, name, value);
            ++out.constants;
        };
        for (const VkEnumValue& c : constants_) constant(c.name, c.type, c.value);
        for (const auto& [name, value] : ext_strings_) {
            if (!written.insert(name).second) continue;
            h << std::format("    compile_time std::static_str {} = \"{}\";\n", name, value);
            ++out.constants;
        }
        // Enum values, type by type.
        std::map<std::string, std::string> value_of, alias_of;
        for (const auto& [type, values] : enum_values_) {
            for (const VkEnumValue& v : values) {
                if (!v.value.empty()) value_of[v.name] = v.value;
                if (!v.alias.empty()) alias_of[v.name] = v.alias;
            }
        }
        for (const auto& [type, values] : enum_values_) {
            if (!closed_.count(type)) continue;
            h << std::format("\n    // {}\n", type);
            for (const VkEnumValue& v : values) {
                std::string value  = v.value;
                std::string target = v.alias;
                for (int hops = 0; value.empty() && !target.empty() && hops < 8; ++hops) {
                    if (value_of.count(target)) value = value_of[target];
                    else target = alias_of.count(target) ? alias_of[target] : std::string();
                }
                if (value.empty()) continue;
                constant(v.name, v.type, value);
            }
        }

        // Structs and unions.
        h << "\n    // ---- structs ----------------------------------------------------------------------------------\n";
        for (const auto& [name, info] : types_) {
            if (!closed_.count(name) || (info.category != "struct" && info.category != "union") || !info.alias.empty()) continue;
            ++out.types;
            if (info.category == "union") {
                auto [size, align] = layout_of_name(name);
                std::string word = align >= 8 ? "u64" : align == 4 ? "u32" : align == 2 ? "u16" : "u8";
                h << std::format("\n    // The C union {} ({} bytes): its members share these bytes:\n", name, size);
                for (const VkMember& m : info.members) h << std::format("    //   {} {}\n", edax_type(m.ctype), m.name);
                h << std::format("    struct {} {{\n        std::raw::array<{}, {}> data = default;\n    }}\n", name, word, size / std::max<std::size_t>(align, 1));
                continue;
            }
            h << std::format("\n    struct {} {{\n", name);
            for (const VkMember& m : info.members) {
                std::string t = edax_type(m.ctype);
                if (m.ctype.bit_field) t = "u32";   // bit-fields: not in the core API
                h << std::format("        {} {} = {};\n", t, m.name, field_default(m, t, written));
            }
            h << "    }\n";
        }

        // The class.
        std::vector<std::string> emitted;
        for (const std::string& c : wanted_commands_) {
            if (command(c)) emitted.push_back(c);
        }
        std::sort(emitted.begin(), emitted.end());
        out.commands = emitted.size();
        auto level = [&](const std::string& name) {
            const VkCommandInfo* info = command(name);
            if (name == "vkGetInstanceProcAddr" || name == "vkCreateInstance" || name == "vkEnumerateInstanceVersion" ||
                name == "vkEnumerateInstanceExtensionProperties" || name == "vkEnumerateInstanceLayerProperties") {
                return 0;   // global
            }
            if (name == "vkGetDeviceProcAddr" || info->params.empty()) return 1;
            std::string first = info->params.front().ctype.base;
            if (first == "VkDevice" || first == "VkQueue" || first == "VkCommandBuffer") return 2;
            return 1;
        };

        h << "\n    // ---- versions ---------------------------------------------------------------------------------\n\n"
             "    // Whether the registry has Vulkan API `a`.\n"
             "    compile_time bool known(std::graphics::api a);\n"
             "    // Whether a command of Vulkan `major`.`minor` exists in `a` (0.0: an extension's command).\n"
             "    compile_time run_time bool has(std::graphics::api a, u32 major, u32 minor);\n"
             "    // VK_MAKE_API_VERSION(0, major, minor, patch): VkApplicationInfo::apiVersion.\n"
             "    compile_time run_time u32 make_version(std::version v);\n"
             "    // The std::version of a packed Vulkan version (vkEnumerateInstanceVersion, apiVersion).\n"
             "    compile_time run_time std::version version_of(u32 packed);\n\n"
             "    // ---- functions ---------------------------------------------------------------------------------\n\n"
             "    // The commands of Vulkan API `A`, loaded in three steps like with any Vulkan loader:\n"
             "    //   load(loader)            the global commands (vkCreateInstance, vkEnumerateInstance*)\n"
             "    //   load_instance(instance) the instance's commands, and its devices' through the instance\n"
             "    //   load_device(device)     optional: the device's commands without the instance's dispatch\n"
             "    // A core command of a later version than A is a compile error at the call; a command whose\n"
             "    // step hasn't run, or an extension the instance doesn't have, panics when called. The methods\n"
             "    // are `unsafe`: Vulkan trusts every handle, pointer and size it is given.\n"
             "    template<std::graphics::api A>\n"
             "    class functions {\n"
             "    public:\n"
             "        // vkGetInstanceProcAddr from `from` (std::windowing::vulkan::loader) and the global commands.\n"
             "        // error(name): the loader doesn't have `name`.\n"
             "        static std::result<functions<A>, std::string> load(std::graphics::function_source& from);\n"
             "        // The commands of `instance` (a VkInstance made with vkCreateInstance) and of its devices.\n"
             "        std::result<void, std::string> load_instance(u64 instance) mut;\n"
             "        // The commands of `device` (a VkDevice), called without the instance's dispatch.\n"
             "        std::result<void, std::string> load_device(u64 device) mut;\n\n";

        s << "// The Edax standard library: bodies of module std::graphics::vulkan (see vulkan.edh).\n//\n"
          << generated_note("vk.xml", out.registry_version) << "\nnamespace std::graphics::vulkan {\n"
          << "    compile_time bool known(std::graphics::api a) {\n"
          << std::format("        return a.name == std::graphics::api_name::vulkan && a.version.major == 1 && a.version.minor <= {}\n"
                         "            && (a.version.minor < {} || a.version.patch <= {});\n",
                         latest_minor, latest_minor, header_version)
          << "    }\n\n"
             "    compile_time run_time bool has(std::graphics::api a, u32 major, u32 minor) {\n"
             "        return a.version.at_least(std::version{ .major = major, .minor = minor });\n"
             "    }\n\n"
             "    compile_time run_time u32 make_version(std::version v) {\n"
             "        return (v.major << 22) | (v.minor << 12) | v.patch;\n"
             "    }\n\n"
             "    compile_time run_time std::version version_of(u32 packed) {\n"
             "        return std::version{ .major = (packed >> 22) & 0x7F, .minor = (packed >> 12) & 0x3FF, .patch = packed & 0xFFF };\n"
             "    }\n\n"
             "    template<std::graphics::api A>\n"
             "    class functions {\n"
             "        compile_time check (A.name == std::graphics::api_name::vulkan)\n"
             "            else f\"std::graphics::vulkan::functions: {std::graphics::name_text(A.name)} is not Vulkan (see std::graphics::opengl)\";\n"
          << std::format("        compile_time check (A.name != std::graphics::api_name::vulkan || known(A))\n"
                         "            else f\"Vulkan {{A.version.major}}.{{A.version.minor}}.{{A.version.patch}} isn't in the registry, which goes up to {}\";\n",
                         out.registry_version)
          << "    public:\n";

        std::ostringstream methods;
        auto since_of = [&](const std::string& name) {
            auto it = since_.find(name);
            return it == since_.end() || it->second.first < 0 ? std::make_pair(0, 0) : it->second;
        };
        std::string names, majors, minors, levels;
        std::size_t gipa = 0, gdpa = 0;
        for (std::size_t slot = 0; slot < emitted.size(); ++slot) {
            const std::string& name = emitted[slot];
            const VkCommandInfo* info = command(name);
            auto [maj, min] = since_of(name);
            int lvl = level(name);
            if (name == "vkGetInstanceProcAddr") gipa = slot;
            if (name == "vkGetDeviceProcAddr") gdpa = slot;
            names += (names.empty() ? "" : " ") + name;
            majors += std::format("{}{}", majors.empty() ? "" : ", ", maj);
            minors += std::format("{}{}", minors.empty() ? "" : ", ", min);
            levels += std::format("{}{}", levels.empty() ? "" : ", ", lvl);
            std::string cond = std::format("has(A, {}, {})", maj, min);
            std::string result = edax_type(info->result);
            std::string params, ctypes, args;
            for (const VkMember& p : info->params) {
                std::string t = edax_type(p.ctype);
                params += (params.empty() ? "" : ", ") + t + " " + param_name(p.name);
                ctypes += (ctypes.empty() ? "" : ", ") + t;
                args += (args.empty() ? "" : ", ") + param_name(p.name);
            }
            std::string place = maj ? std::format("Vulkan {}.{}", maj, min) : std::string("an extension");
            h << std::format("        // {}\n        unsafe {} {}({});\n", place, result, name, params);
            std::string step = lvl == 0 ? "load()" : lvl == 1 ? "load_instance()" : "load_instance() or load_device()";
            std::string call = std::format("cast::reinterpret<std::fn_ptr<{}({})>>(this.p[{}])({})", result, ctypes, slot, args);
            methods << std::format("        unsafe {} {}({}) {{\n", result, name, params);
            if (maj) {
                methods << std::format("            compile_time check ({})\n"
                                       "                else f\"{} is in Vulkan {}.{}; the program asks for Vulkan {{A.version.major}}.{{A.version.minor}}\";\n",
                                       cond, name, maj, min);
            }
            methods << std::format("            run_time check (this.p[{}] != nullptr) else \"{} isn't loaded: {}{}\";\n"
                                   "            {}{};\n"
                                   "        }}\n\n",
                                   slot, name, step, maj ? "" : " (and the extension enabled)", result == "void" ? "" : "return ", call);
        }
        h << "    }\n}\n";
        std::size_t n = emitted.size();
        // The tables the load steps walk: names, the version that has each command (0.0: an
        // extension's), and its level (0 global, 1 instance, 2 device).
        std::string tables = std::format("    private compile_time std::static_str command_names = \"{}\";\n", names) +
                             std::format("    private compile_time std::array<u32, {}> since_major = [{}];\n", n, majors) +
                             std::format("    private compile_time std::array<u32, {}> since_minor = [{}];\n", n, minors) +
                             std::format("    private compile_time std::array<u8, {}> levels = [{}];\n\n", n, levels);
        std::string text = s.str();
        std::string marker = "    template<std::graphics::api A>\n    class functions {\n        compile_time check (A.name";
        text.insert(text.find(marker), tables);
        s.str(text);
        s.seekp(0, std::ios::end);
        s << std::format(
                 "        static std::result<functions<A>, std::string> load(std::graphics::function_source& from) {{\n"
                 "            mut functions<A> f = functions<A>{{}};\n"
                 "            f.p[{0}] = match (from.proc_address(\"vkGetInstanceProcAddr\")) {{ std::some(p) => p, std::none => return std::error(std::string::from(\"vkGetInstanceProcAddr\")), }};\n"
                 "            std::darray<std::str> names = command_names.split(' ');\n"
                 "            for (usize i : 0..names.len()) {{\n"
                 "                if (levels[i] == 0 && i != {0} && has(A, since_major[i], since_minor[i])) {{\n"
                 "                    f.p[i] = f.lookup(0, names[i]);\n"
                 "                    if (!f.p[i] && since_major[i] != 0) {{ return std::error(std::string::from(names[i])); }}\n"
                 "                }}\n"
                 "            }}\n"
                 "            return std::ok(move f);\n"
                 "        }}\n\n"
                 "        std::result<void, std::string> load_instance(u64 instance) mut {{\n"
                 "            std::darray<std::str> names = command_names.split(' ');\n"
                 "            for (usize i : 0..names.len()) {{\n"
                 "                if (levels[i] != 0 && has(A, since_major[i], since_minor[i])) {{\n"
                 "                    this.p[i] = this.lookup(instance, names[i]);\n"
                 "                    if (!this.p[i] && since_major[i] != 0) {{ return std::error(std::string::from(names[i])); }}\n"
                 "                }}\n"
                 "            }}\n"
                 "            return std::ok();\n"
                 "        }}\n\n"
                 "        std::result<void, std::string> load_device(u64 device) mut {{\n"
                 "            run_time check (this.p[{1}] != nullptr) else \"load_device: call load_instance first\";\n"
                 "            std::darray<std::str> names = command_names.split(' ');\n"
                 "            for (usize i : 0..names.len()) {{\n"
                 "                if (levels[i] == 2 && has(A, since_major[i], since_minor[i])) {{\n"
                 "                    std::string c_name = std::format(f\"{{names[i]}}\\0\");\n"
                 "                    std::raw_ptr<u8> found = unsafe {{ cast::reinterpret<std::fn_ptr<std::raw_ptr<u8>(u64, std::raw_ptr<u8>)>>(this.p[{1}])(device, c_name.as_str().as_ptr()) }};\n"
                 "                    if (found) {{ this.p[i] = found; }}\n"
                 "                }}\n"
                 "            }}\n"
                 "            return std::ok();\n"
                 "        }}\n\n",
                 gipa, gdpa)
          << methods.str()
          << "    private:\n"
          << std::format("        std::raw_ptr<u8> lookup(u64 instance, std::str name) {{\n"
                         "            std::string c_name = std::format(f\"{{name}}\\0\");\n"
                         "            unsafe {{ return cast::reinterpret<std::fn_ptr<std::raw_ptr<u8>(u64, std::raw_ptr<u8>)>>(this.p[{}])(instance, c_name.as_str().as_ptr()); }}\n"
                         "        }}\n\n",
                         gipa)
          << std::format("        std::array<std::raw_ptr<u8>, {}> p = default;\n", n) << "    }\n}\n";
        out.header = h.str();
        out.source = s.str();
        return out;
    }

    const XmlNode&                                  r_;
    std::map<std::string, VkTypeInfo>               types_;
    std::map<std::string, std::vector<VkEnumValue>> enum_values_;
    std::set<std::string>                           bitmask64_enums_;
    std::vector<VkEnumValue>                        constants_;
    std::vector<std::pair<std::string, std::string>> ext_strings_;
    std::map<std::string, VkCommandInfo>            commands_;
    std::set<std::pair<int, int>>                   versions_;
    std::set<std::string>                           wanted_types_, wanted_enums_, wanted_commands_, closed_;
    std::map<std::string, std::pair<int, int>>      since_;
};

} // namespace

GeneratedStdModule generate_vulkan_module(std::string_view vk_xml) {
    XmlNode doc = XmlReader(vk_xml).document();
    const XmlNode* registry = doc.child("registry");
    if (!registry) throw std::runtime_error("vk.xml: no <registry>");
    return VulkanGenerator(*registry).run();
}

} // namespace edax
