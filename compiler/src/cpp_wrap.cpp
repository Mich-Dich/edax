// C++ classes through generated wrappers (C interop, "C++ classes through generated wrappers").
//
// clang reads the headers (`-ast-dump=json`, filtered to each listed class); this file turns each
// class's public constructors, destructor and methods into:
//   - a C wrapper per function (`extern "C"`, compiled into the C++ target) that catches every
//     exception and passes results through out-parameters;
//   - a Edax class that owns the C++ object through a pointer, whose methods call the wrappers.
// A function that may throw returns `std::result<T, std::cpp_exception>`; a `noexcept` one, `T`.

#include "edax/cpp_wrap.hpp"

#include <cctype>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>

namespace edax {
namespace {

// --- a small JSON reader (clang's AST dump) ------------------------------------------------------

struct Json {
    enum class Kind { null, boolean, number, string, array, object };
    Kind                                       kind = Kind::null;
    bool                                       boolean = false;
    std::string                                text;   // string, number
    std::vector<Json>                          items;
    std::vector<std::string>                   keys;     // an object's keys, and their values
    std::vector<Json>                          values;

    const Json* get(const std::string& key) const {
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) return &values[i];
        }
        return nullptr;
    }
    std::string str(const std::string& key) const {
        const Json* v = get(key);
        return v && v->kind == Kind::string ? v->text : std::string();
    }
    bool flag(const std::string& key) const {
        const Json* v = get(key);
        return v && v->kind == Kind::boolean && v->boolean;
    }
    const std::vector<Json>& inner() const {
        static const std::vector<Json> none;
        const Json* v = get("inner");
        return v && v->kind == Kind::array ? v->items : none;
    }
};

class JsonReader {
public:
    explicit JsonReader(const std::string& text) : s_(text) {}

    // The next top-level value (`-ast-dump-filter` prints several), or none at the end.
    std::optional<Json> next() {
        while (i_ < s_.size() && s_[i_] != '{') ++i_;
        if (i_ >= s_.size()) return std::nullopt;
        return value();
    }

private:
    const std::string& s_;
    std::size_t        i_ = 0;

    void space() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    Json value() {
        space();
        Json v;
        if (i_ >= s_.size()) return v;
        char c = s_[i_];
        if (c == '{') {
            v.kind = Json::Kind::object;
            ++i_;
            space();
            if (s_[i_] == '}') {
                ++i_;
                return v;
            }
            while (i_ < s_.size()) {
                space();
                std::string key = string();
                space();
                ++i_;   // ':'
                v.keys.push_back(std::move(key));
                v.values.push_back(value());
                space();
                if (s_[i_++] == '}') break;   // else ','
            }
        } else if (c == '[') {
            v.kind = Json::Kind::array;
            ++i_;
            space();
            if (s_[i_] == ']') {
                ++i_;
                return v;
            }
            while (i_ < s_.size()) {
                v.items.push_back(value());
                space();
                if (s_[i_++] == ']') break;
            }
        } else if (c == '"') {
            v.kind = Json::Kind::string;
            v.text = string();
        } else if (s_.compare(i_, 4, "true") == 0) {
            v.kind    = Json::Kind::boolean;
            v.boolean = true;
            i_ += 4;
        } else if (s_.compare(i_, 5, "false") == 0) {
            v.kind = Json::Kind::boolean;
            i_ += 5;
        } else if (s_.compare(i_, 4, "null") == 0) {
            i_ += 4;
        } else {
            v.kind = Json::Kind::number;
            std::size_t start = i_;
            while (i_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[i_])) || s_[i_] == '-' || s_[i_] == '.' || s_[i_] == '+')) ++i_;
            v.text = s_.substr(start, i_ - start);
        }
        return v;
    }
    std::string string() {
        std::string out;
        ++i_;   // '"'
        while (i_ < s_.size() && s_[i_] != '"') {
            if (s_[i_] == '\\' && i_ + 1 < s_.size()) {
                char e = s_[++i_];
                if (e == 'n') out += '\n';
                else if (e == 't') out += '\t';
                else if (e == 'u') {   // only ASCII appears in type names
                    out += static_cast<char>(std::stoi(s_.substr(i_ + 1, 4), nullptr, 16));
                    i_ += 4;
                } else out += e;
                ++i_;
                continue;
            }
            out += s_[i_++];
        }
        ++i_;
        return out;
    }
};

// --- types --------------------------------------------------------------------------------------

// How a C++ parameter or result crosses the wrapper.
struct Crossing {
    enum class Kind { none, scalar, string_in, string_out, object_in, object_out, unsupported };
    Kind        kind = Kind::none;
    std::string edax;      // the Edax type of the parameter or result
    std::string c;         // scalar: the C type
    std::string object;    // object_in/out: the wrapped class's key
    bool        is_mut = false;   // object_in: a non-const reference or pointer
    bool        is_pointer = false;
    std::string cpp_string;       // string_in: how the C++ function takes it
    std::string why;              // unsupported
};

std::string trim(std::string s) {
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::string without_spaces(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c != ' ') out += c;
    }
    return out;
}

// The last component of a qualified name, keeping template arguments: `JPH::Ref<JPH::Shape>`
// → `Ref<JPH::Shape>`.
std::string last_component(const std::string& name) {
    int         depth = 0;
    std::size_t cut   = 0;
    for (std::size_t i = 0; i + 1 < name.size(); ++i) {
        if (name[i] == '<') ++depth;
        if (name[i] == '>') --depth;
        if (depth == 0 && name[i] == ':' && name[i + 1] == ':') cut = i + 2;
    }
    return name.substr(cut);
}

// A Edax name for a C++ class or template instance: `Ref<JPH::Shape>` → `Ref_Shape`.
std::string edax_class_name(const std::string& qualified) {
    std::string last = last_component(qualified);
    std::string out;
    std::size_t lt = last.find('<');
    out = last.substr(0, lt);
    if (lt != std::string::npos) {
        std::string args = last.substr(lt + 1, last.rfind('>') - lt - 1);
        std::string word;
        for (char c : args + ",") {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                word += c;
            } else if (c == ':') {
                word.clear();   // keep only each argument's last component
            } else if (c == ',' || c == '<' || c == '>' || c == '*' || c == '&') {
                if (!word.empty()) out += "_" + word;
                word.clear();
                if (c == '*') out += "_ptr";
            } else if (c == ' ' && !word.empty() && (word == "unsigned" || word == "long" || word == "const")) {
                word += "_";
            }
        }
    }
    return out;
}

const std::map<std::string, std::pair<std::string, std::string>>& scalars() {
    static const std::map<std::string, std::pair<std::string, std::string>> m = {
        {"bool", {"bool", "bool"}},
        {"char", {"i8", "char"}},
        {"signed char", {"i8", "signed char"}},
        {"unsigned char", {"u8", "unsigned char"}},
        {"short", {"i16", "short"}},
        {"unsigned short", {"u16", "unsigned short"}},
        {"int", {"i32", "int"}},
        {"unsigned int", {"u32", "unsigned int"}},
        {"long", {"i64", "long"}},
        {"unsigned long", {"u64", "unsigned long"}},
        {"long long", {"i64", "long long"}},
        {"unsigned long long", {"u64", "unsigned long long"}},
        {"float", {"f32", "float"}},
        {"double", {"f64", "double"}},
    };
    return m;
}

struct Method {
    std::string              cpp_name;     // "add", "operator==", "" for constructors
    std::string              edax_name;
    std::vector<Crossing>    params;
    std::vector<std::string> param_names;
    std::vector<std::string> cpp_param_types;
    Crossing                 result;
    bool                     is_const = false, is_static = false, is_constructor = false, no_throw = false;
};

struct Class {
    std::string         qualified;   // as listed: "demo::Counter", "demo::Box<int>"
    std::string         key;         // its last component, without spaces: "Counter", "Box<int>"
    std::string         edax;        // its Edax name
    std::vector<Method> methods;
    bool                destructible = true;
};

const std::map<std::string, std::string>& operator_names() {
    static const std::map<std::string, std::string> m = {
        {"operator==", "equals"},   {"operator!=", "not_equals"}, {"operator<", "less"},       {"operator<=", "less_equal"},
        {"operator>", "greater"},   {"operator>=", "greater_equal"}, {"operator+", "plus"},    {"operator-", "minus"},
        {"operator*", "times"},     {"operator/", "divided"},     {"operator%", "remainder"}, {"operator[]", "at"},
        {"operator()", "call"},     {"operator+=", "add_assign"}, {"operator-=", "subtract_assign"},
        {"operator*=", "multiply_assign"}, {"operator/=", "divide_assign"}, {"operator<<", "shift_left"},
        {"operator>>", "shift_right"}, {"operator!", "not"},     {"operator++", "increment"}, {"operator--", "decrement"},
    };
    return m;
}

const std::set<std::string>& edax_keywords() {
    static const std::set<std::string> k = {
        "import", "struct", "class", "interface", "enum", "union", "namespace", "public", "private", "static", "mut", "move",
        "return", "if", "else", "while", "for", "match", "break", "continue", "true", "false", "nullptr", "this", "type",
        "template", "unsafe", "extern", "check", "compile_time", "run_time", "default", "view", "override", "open", "abstract",
        "operator", "cast", "bit", "auto", "deduce", "thread_local", "annotation"};
    return k;
}

std::string safe(const std::string& n) { return edax_keywords().contains(n) ? n + "_" : n; }

class Generator {
public:
    Generator(const CppWrapOptions& o) : options_(o) {}

    CppWrapResult run(const std::vector<std::string>& dumps) {
        std::vector<std::string> names = options_.classes;
        names.insert(names.end(), options_.templates.begin(), options_.templates.end());
        for (std::size_t i = 0; i < names.size(); ++i) {
            Class c;
            c.qualified = names[i];
            c.key       = without_spaces(last_component(names[i]));
            c.edax      = edax_class_name(names[i]);
            classes_.push_back(c);
        }
        for (std::size_t i = 0; i < names.size(); ++i) {
            const Json* decl = nullptr;
            std::vector<Json> found;
            static const std::string none;
            JsonReader reader(i < dumps.size() ? dumps[i] : none);
            while (auto j = reader.next()) found.push_back(std::move(*j));
            bool templ = i >= options_.classes.size();
            std::string want = last_component(names[i]);
            std::string base = want.substr(0, want.find('<'));
            for (const Json& j : found) {
                std::string kind = j.str("kind");
                if (j.str("name") != base || !j.get("definitionData")) continue;
                if (!templ && kind == "CXXRecordDecl") decl = &j;
                if (templ && kind == "ClassTemplateSpecializationDecl" && template_args_match(j, want)) decl = &j;
                if (decl) break;
            }
            if (!decl) {
                result_.error = std::format("g.wrap_cpp: '{}' is not a class defined in the headers", names[i]);
                return result_;
            }
            read_class(classes_[i], *decl);
        }
        write();
        return result_;
    }

private:
    const CppWrapOptions& options_;
    std::vector<Class>    classes_;
    CppWrapResult         result_;

    bool template_args_match(const Json& spec, const std::string& want) {
        std::string args = want.substr(want.find('<') + 1);
        args             = args.substr(0, args.rfind('>'));
        std::vector<std::string> wanted;
        int         depth = 0;
        std::string cur;
        for (char c : args) {
            if (c == '<') ++depth;
            if (c == '>') --depth;
            if (c == ',' && depth == 0) {
                wanted.push_back(without_spaces(cur));
                cur.clear();
            } else {
                cur += c;
            }
        }
        wanted.push_back(without_spaces(cur));
        std::vector<std::string> have;
        for (const Json& m : spec.inner()) {
            if (m.str("kind") != "TemplateArgument") continue;
            const Json* t = m.get("type");
            have.push_back(t ? without_spaces(t->str("qualType")) : std::string());
        }
        if (have.size() != wanted.size()) return false;
        for (std::size_t k = 0; k < have.size(); ++k) {
            if (have[k] != wanted[k] && last_component(have[k]) != last_component(wanted[k])) return false;
        }
        return true;
    }

    // The wrapped class a C++ type names (through const, & and *), or none.
    const Class* wrapped(const std::string& type) {
        std::string t = type;
        for (const char* q : {"const ", "struct ", "class "}) {
            for (std::size_t at; (at = t.find(q)) != std::string::npos;) t.erase(at, std::string(q).size());
        }
        while (!t.empty() && (t.back() == '&' || t.back() == '*' || t.back() == ' ')) t.pop_back();
        t = without_spaces(last_component(trim(t)));
        for (const Class& c : classes_) {
            if (c.key == t) return &c;
        }
        return nullptr;
    }

    Crossing crossing(const Json& type_json, bool result, const Class& self) {
        Crossing    x;
        std::string q = type_json.str("qualType");
        std::string d = type_json.str("desugaredQualType");
        std::string plain = trim(d.empty() ? q : d);
        if (plain == "void") return x;   // none
        static const std::map<std::string, std::string> typedefs = {
            {"size_t", "unsigned long"}, {"ssize_t", "long"}, {"ptrdiff_t", "long"}, {"intptr_t", "long"}, {"uintptr_t", "unsigned long"},
            {"int8_t", "signed char"}, {"int16_t", "short"}, {"int32_t", "int"}, {"int64_t", "long"},
            {"uint8_t", "unsigned char"}, {"uint16_t", "unsigned short"}, {"uint32_t", "unsigned int"}, {"uint64_t", "unsigned long"}};
        std::string bare = plain.starts_with("std::") ? plain.substr(5) : plain;
        if (std::string qb = trim(q.starts_with("std::") ? q.substr(5) : q); qb == "size_t") {   // written size_t: usize
            x.kind = Crossing::Kind::scalar;
            x.edax = "usize";
            x.c    = "size_t";
            return x;
        }
        if (auto it = typedefs.find(bare); it != typedefs.end()) plain = it->second;
        if (auto it = scalars().find(plain); it != scalars().end()) {
            x.kind = Crossing::Kind::scalar;
            x.edax = it->second.first;
            x.c    = it->second.second;
            return x;
        }
        std::string sq = without_spaces(q);
        bool is_string = sq == "std::string" || sq == "conststd::string&" || sq == "std::string_view" || sq == "conststd::string_view&" ||
                         sq == "constchar*" || sq == "std::basic_string<char>" || sq == "conststd::basic_string<char>&";
        if (is_string && !result) {
            x.kind       = Crossing::Kind::string_in;
            x.edax       = "std::str";
            x.cpp_string = sq == "constchar*" ? "c_string" : sq.find("string_view") != std::string::npos ? "view" : "string";
            return x;
        }
        if (result && (sq == "std::string" || sq == "conststd::string&" || sq == "std::basic_string<char>")) {
            x.kind = Crossing::Kind::string_out;
            x.edax = "std::string";
            return x;
        }
        if (const Class* c = wrapped(q)) {
            bool ref     = sq.ends_with("&");
            bool pointer = sq.ends_with("*");
            bool is_const = sq.starts_with("const");
            if (result) {
                if (ref || pointer) {
                    x.kind = Crossing::Kind::unsupported;
                    x.why  = std::format("returns a reference or pointer to '{}' (the Edax class owns its object)", c->qualified);
                    return x;
                }
                x.kind   = Crossing::Kind::object_out;
                x.edax   = c->edax;
                x.object = c->key;
                return x;
            }
            x.kind       = Crossing::Kind::object_in;
            x.object     = c->key;
            x.is_mut     = (ref || pointer) && !is_const;
            x.is_pointer = pointer;
            x.edax       = std::format("{}{}&", x.is_mut ? "mut " : "", c->edax);
            return x;
        }
        (void)self;
        x.kind = Crossing::Kind::unsupported;
        x.why  = std::format("the type '{}' has no Edax counterpart here", q);
        return x;
    }

    void read_class(Class& c, const Json& decl) {
        std::string tag    = decl.str("tagUsed");
        bool        public_ = tag != "class";
        std::map<std::string, int> seen;   // a Edax name used how often
        for (const Json& m : decl.inner()) {
            std::string kind = m.str("kind");
            if (kind == "AccessSpecDecl") {
                public_ = m.str("access") == "public";
                continue;
            }
            if (kind == "CXXDestructorDecl") {
                if (!public_ || m.str("explicitlyDeleted") == "true" || m.flag("explicitlyDeleted")) c.destructible = false;
                continue;
            }
            if (kind != "CXXMethodDecl" && kind != "CXXConstructorDecl") continue;
            if (!public_ || m.flag("isImplicit") || m.flag("explicitlyDeleted")) continue;
            std::string name = m.str("name");
            const Json* type = m.get("type");
            std::string signature = type ? type->str("qualType") : "";
            Method f;
            f.is_constructor = kind == "CXXConstructorDecl";
            f.cpp_name       = f.is_constructor ? "" : name;
            f.is_static      = m.str("storageClass") == "static";
            f.no_throw       = signature.find("noexcept") != std::string::npos && signature.find("noexcept(false)") == std::string::npos;
            std::size_t close = signature.rfind(')');
            f.is_const = close != std::string::npos && signature.find("const", close) != std::string::npos;
            if (f.is_constructor) {
                // A copy or move constructor: Edax classes are moved, not copied.
                bool copy = false;
                for (const Json& p : m.inner()) {
                    if (p.str("kind") == "ParmVarDecl" && p.get("type") && wrapped(p.get("type")->str("qualType")) == &c &&
                        m.inner().size() >= 1) {
                        copy = true;
                    }
                }
                int params = 0;
                for (const Json& p : m.inner()) params += p.str("kind") == "ParmVarDecl";
                if (copy && params == 1) continue;
            }
            std::string base = f.is_constructor ? "make" : name;
            if (name.starts_with("operator")) {
                auto it = operator_names().find(name);
                if (it == operator_names().end() || name == "operator=") {
                    if (name != "operator=") result_.report.emplace_back(c.qualified + "::" + name, "an operator without a Edax name: not wrapped");
                    continue;
                }
                base = it->second;
                result_.report.emplace_back(c.qualified + "::" + name, "wrapped as '" + base + "'");
            }
            base = safe(base);
            bool bad = false;
            std::size_t defaults = 0;
            for (const Json& p : m.inner()) {
                if (p.str("kind") != "ParmVarDecl") continue;
                const Json* pt = p.get("type");
                Crossing x = pt ? crossing(*pt, false, c) : Crossing{};
                if (x.kind == Crossing::Kind::unsupported || x.kind == Crossing::Kind::none) {
                    result_.report.emplace_back(c.qualified + "::" + (f.is_constructor ? last_component(c.qualified) : name),
                                                "not wrapped: " + (x.why.empty() ? "a parameter of an unknown type" : x.why));
                    bad = true;
                    break;
                }
                f.params.push_back(x);
                std::string pname = p.str("name");
                f.param_names.push_back(pname);
                f.cpp_param_types.push_back(pt->str("qualType"));
                if (p.get("init")) ++defaults;
            }
            if (bad) continue;
            if (!f.is_constructor && type) {
                Json rt;
                rt.kind = Json::Kind::object;
                std::string sig = type->str("qualType");
                std::string res = trim(sig.substr(0, sig.find('(')));
                Json text;
                text.kind = Json::Kind::string;
                text.text = res;
                rt.keys.push_back("qualType");
                rt.values.push_back(text);
                // The desugared result: from the desugared signature, when there is one.
                std::string dsig = type->str("desugaredQualType");
                if (!dsig.empty()) {
                    Json desugared;
                    desugared.kind = Json::Kind::string;
                    desugared.text = trim(dsig.substr(0, dsig.find('(')));
                    rt.keys.push_back("desugaredQualType");
                    rt.values.push_back(desugared);
                }
                f.result = crossing(rt, true, c);
                if (f.result.kind == Crossing::Kind::unsupported) {
                    result_.report.emplace_back(c.qualified + "::" + name, "not wrapped: " + f.result.why);
                    continue;
                }
            }
            int n = ++seen[base];
            f.edax_name = n == 1 ? base : std::format("{}_{}", base, n);
            if (n > 1) {
                result_.report.emplace_back(std::format("{}::{}({} parameters)", c.qualified, f.is_constructor ? last_component(c.qualified) : name, f.params.size()),
                                            "an overload, wrapped as '" + f.edax_name + "'");
            }
            c.methods.push_back(f);
            if (defaults > 0) {   // the call without the defaulted arguments
                Method g = f;
                g.params.resize(f.params.size() - defaults);
                g.param_names.resize(g.params.size());
                g.cpp_param_types.resize(g.params.size());
                g.edax_name = f.edax_name + "_default";
                result_.report.emplace_back(std::format("{}::{} with its default arguments", c.qualified, f.is_constructor ? last_component(c.qualified) : name),
                                            "wrapped as '" + g.edax_name + "'");
                c.methods.push_back(g);
            }
        }
    }

    std::string c_name(const Class& c, const std::string& what) const {
        return std::format("edax_{}_{}_{}", options_.module, c.edax, what);
    }

    // The C parameters of a wrapper and the C++ argument expressions.
    void c_params(const Method& f, std::vector<std::string>& params, std::vector<std::string>& args) {
        for (std::size_t i = 0; i < f.params.size(); ++i) {
            const Crossing& x = f.params[i];
            std::string     p = std::format("a{}", i);
            switch (x.kind) {
            case Crossing::Kind::scalar:
                params.push_back(x.c + " " + p);
                args.push_back(p);
                break;
            case Crossing::Kind::string_in:
                params.push_back("const char* " + p);
                params.push_back("size_t " + p + "_len");
                args.push_back(x.cpp_string == "view" ? std::format("std::string_view({0}, {0}_len)", p)
                               : x.cpp_string == "c_string" ? std::format("std::string({0}, {0}_len).c_str()", p)
                                                            : std::format("std::string({0}, {0}_len)", p));
                break;
            case Crossing::Kind::object_in: {
                const Class* c = nullptr;
                for (const Class& k : classes_) {
                    if (k.key == x.object) c = &k;
                }
                params.push_back(std::format("{}void* {}", x.is_mut ? "" : "const ", p));
                std::string cast = std::format("static_cast<{}{}*>({})", x.is_mut ? "" : "const ", c->qualified, p);
                args.push_back(x.is_pointer ? cast : "*" + cast);
                break;
            }
            default: break;
            }
        }
    }

    void write() {
        std::ostringstream h, src, cpp, ext;
        std::string        ind = options_.ns.empty() ? "" : "    ";
        h << "// Generated by edaxc (g.wrap_cpp): C++ classes through C wrappers. Do not edit.\nimport std;\n\n";
        src << "// Generated by edaxc (g.wrap_cpp): the classes call the C wrappers.\n";
        cpp << "// Generated by edaxc (g.wrap_cpp): C wrappers of C++ classes. Every wrapper catches\n"
               "// exceptions; results go through out-parameters.\n";
        for (const std::string& header : options_.headers) cpp << "#include \"" << header << "\"\n";
        cpp << "#include <cstdlib>\n#include <cstring>\n#include <exception>\n#include <new>\n#include <string>\n#include <string_view>\n\n"
               "namespace {\n"
               "char* edax_copy(const char* text, size_t n) {\n"
               "    char* out = static_cast<char*>(std::malloc(n + 1));\n"
               "    if (out) { std::memcpy(out, text, n); out[n] = 0; }\n"
               "    return out;\n"
               "}\n"
               "int edax_caught(char** error, size_t* error_len) {\n"
               "    try { throw; }\n"
               "    catch (const std::exception& e) { *error_len = std::strlen(e.what()); *error = edax_copy(e.what(), *error_len); }\n"
               "    catch (...) { static const char unknown[] = \"an exception that is not a std::exception\"; *error_len = sizeof unknown - 1; *error = edax_copy(unknown, *error_len); }\n"
               "    return 1;\n"
               "}\n"
               "}\n\n";
        for (const std::string& t : options_.templates) cpp << "template class " << t << ";\n";   // the listed instances
        cpp << "\n";
        if (!options_.ns.empty()) {
            h << "namespace " << options_.ns << " {\n";
            src << "namespace " << options_.ns << " {\n";
        }
        ext << ind << "private extern(std::build::language::c) {\n";
        ext << ind << "    void free(std::raw_ptr<mut u8> p);\n";
        for (const Class& c : classes_) {
            h << ind << std::format("// The C++ class {}, owned through a pointer (g.wrap_cpp).\n", c.qualified);
            h << ind << "class " << c.edax << " {\n" << ind << "public:\n";
            src << ind << "class " << c.edax << " {\n" << ind << "public:\n";
            for (const Method& f : c.methods) write_method(c, f, h, src, cpp, ext);
            if (c.destructible) {
                h << ind << "    ~" << c.edax << "();\n";
                src << ind << "    ~" << c.edax << "() {\n" << ind << "        unsafe { " << c_name(c, "drop") << "(this.handle); }\n" << ind << "    }\n";
                cpp << std::format("extern \"C\" void {}(void* self) {{ delete static_cast<{}*>(self); }}\n\n", c_name(c, "drop"), c.qualified);
                ext << ind << std::format("    void {}(std::raw_ptr<mut u8> self);\n", c_name(c, "drop"));
            } else {
                result_.report.emplace_back(c.qualified + "::~" + last_component(c.qualified), "not public: the Edax class never deletes the object");
            }
            h << ind << "}\n\n";
            src << "\n" << ind << "private:\n" << ind << "    std::raw_ptr<mut u8> handle;\n" << ind << "}\n\n";
        }
        ext << ind << "}\n";
        // Helpers: a malloc'd C string as a std::string (then freed), the caught exception.
        src << ind << "private std::string take_string(std::raw_ptr<mut u8> text, usize n) {\n"
            << ind << "    unsafe {\n"
            << ind << "        std::string s = std::string::from(std::raw::str_from(text, n));\n"
            << ind << "        free(text);\n"
            << ind << "        return s;\n"
            << ind << "    }\n"
            << ind << "}\n\n";
        src << ind << "private std::cpp_exception take_exception(std::raw_ptr<mut u8> text, usize n) {\n"
            << ind << "    return std::cpp_exception{ .message = take_string(text, n) };\n"
            << ind << "}\n\n";
        src << ext.str();
        if (!options_.ns.empty()) {
            h << "}\n";
            src << "}\n";
        }
        result_.header   = h.str();
        result_.source   = src.str();
        result_.wrappers = cpp.str();
    }

    void write_method(const Class& c, const Method& f, std::ostringstream& h, std::ostringstream& src, std::ostringstream& cpp,
                      std::ostringstream& ext) {
        std::string ind = options_.ns.empty() ? "" : "    ";
        std::string fn  = c_name(c, f.edax_name);
        bool        member = !f.is_constructor && !f.is_static;
        // The C wrapper.
        std::vector<std::string> cparams, args;
        if (member) cparams.push_back(std::format("{}void* self", f.is_const ? "const " : ""));
        c_params(f, cparams, args);
        Crossing made;   // a constructor makes an object of its class
        made.kind   = Crossing::Kind::object_out;
        made.edax   = c.edax;
        made.object = c.key;
        const Crossing& r = f.is_constructor ? made : f.result;
        switch (r.kind) {
        case Crossing::Kind::scalar: cparams.push_back(r.c + "* out"); break;
        case Crossing::Kind::string_out:
            cparams.push_back("char** out");
            cparams.push_back("size_t* out_len");
            break;
        case Crossing::Kind::object_out: cparams.push_back("void** out"); break;
        default: break;
        }
        cparams.push_back("char** error");
        cparams.push_back("size_t* error_len");
        std::string joined_params, joined_args;
        for (std::size_t i = 0; i < cparams.size(); ++i) joined_params += (i ? ", " : "") + cparams[i];
        for (std::size_t i = 0; i < args.size(); ++i) joined_args += (i ? ", " : "") + args[i];
        std::string call;
        if (f.is_constructor) {
            call = std::format("new {}({})", c.qualified, joined_args);
        } else if (f.is_static) {
            call = std::format("{}::{}({})", c.qualified, f.cpp_name, joined_args);
        } else {
            call = std::format("static_cast<{}{}*>(self)->{}({})", f.is_const ? "const " : "", c.qualified, f.cpp_name, joined_args);
        }
        std::string body;
        switch (r.kind) {
        case Crossing::Kind::none: body = call + ";"; break;
        case Crossing::Kind::scalar: body = std::format("*out = {};", call); break;
        case Crossing::Kind::string_out:
            body = std::format("std::string r = {}; *out_len = r.size(); *out = edax_copy(r.data(), r.size());", call);
            break;
        case Crossing::Kind::object_out:
            body = f.is_constructor ? std::format("*out = {};", call)
                                    : std::format("*out = new {}({});", classes_by_key(r.object).qualified, call);
            break;
        default: break;
        }
        cpp << std::format("extern \"C\" int {}({}) {{\n    try {{ {} return 0; }}\n    catch (...) {{ return edax_caught(error, error_len); }}\n}}\n\n",
                           fn, joined_params, body);

        // The Edax extern declaration.
        std::vector<std::string> rparams;
        if (member) rparams.push_back(std::format("std::raw_ptr<{}u8> self", f.is_const ? "" : "mut "));
        for (std::size_t i = 0; i < f.params.size(); ++i) {
            const Crossing& x = f.params[i];
            if (x.kind == Crossing::Kind::scalar) rparams.push_back(std::format("{} a{}", x.edax, i));
            if (x.kind == Crossing::Kind::string_in) {
                rparams.push_back(std::format("std::raw_ptr<u8> a{}", i));
                rparams.push_back(std::format("usize a{}_len", i));
            }
            if (x.kind == Crossing::Kind::object_in) rparams.push_back(std::format("std::raw_ptr<{}u8> a{}", x.is_mut ? "mut " : "", i));
        }
        switch (r.kind) {
        case Crossing::Kind::scalar: rparams.push_back(std::format("std::raw_ptr<mut {}> out", r.edax)); break;
        case Crossing::Kind::string_out:
            rparams.push_back("std::raw_ptr<mut std::raw_ptr<mut u8>> out");
            rparams.push_back("std::raw_ptr<mut usize> out_len");
            break;
        case Crossing::Kind::object_out: rparams.push_back("std::raw_ptr<mut std::raw_ptr<mut u8>> out"); break;
        default: break;
        }
        rparams.push_back("std::raw_ptr<mut std::raw_ptr<mut u8>> error");
        rparams.push_back("std::raw_ptr<mut usize> error_len");
        std::string decl;
        for (std::size_t i = 0; i < rparams.size(); ++i) decl += (i ? ", " : "") + rparams[i];
        ext << ind << std::format("    i32 {}({});\n", fn, decl);

        // The Edax method.
        std::string value = r.kind == Crossing::Kind::none ? "void" : r.edax;
        std::string result_type = f.no_throw ? value : std::format("std::result<{}, std::cpp_exception>", value);
        std::vector<std::string> sig;
        for (std::size_t i = 0; i < f.params.size(); ++i) {
            std::string pname = f.param_names[i].empty() || f.param_names[i].starts_with("_") ? std::format("a{}", i) : safe(f.param_names[i]);
            for (std::size_t j = 0; j < i; ++j) {
                if (f.param_names[j] == f.param_names[i]) pname = std::format("a{}", i);
            }
            sig.push_back(std::format("{} {}", f.params[i].edax, pname));
        }
        std::string sig_text;
        for (std::size_t i = 0; i < sig.size(); ++i) sig_text += (i ? ", " : "") + sig[i];
        bool        mut_self = member && !f.is_const;
        std::string head = std::format("{}{} {}({}){}", member ? "" : "static ", result_type, f.edax_name, sig_text, mut_self ? " mut" : "");
        h << ind << "    " << head << ";\n";
        src << ind << "    " << head << " {\n";
        std::string in = ind + "        ";
        src << in << "mut std::raw_ptr<mut u8> error = nullptr;\n" << in << "mut usize error_len = 0;\n";
        switch (r.kind) {
        case Crossing::Kind::scalar: src << in << std::format("mut {} out = default;\n", r.edax); break;
        case Crossing::Kind::string_out:
            src << in << "mut std::raw_ptr<mut u8> out = nullptr;\n" << in << "mut usize out_len = 0;\n";
            break;
        case Crossing::Kind::object_out: src << in << "mut std::raw_ptr<mut u8> out = nullptr;\n"; break;
        default: break;
        }
        std::vector<std::string> cargs;
        if (member) cargs.push_back("this.handle");
        for (std::size_t i = 0; i < f.params.size(); ++i) {
            std::string pname = sig[i].substr(sig[i].rfind(' ') + 1);
            const Crossing& x = f.params[i];
            if (x.kind == Crossing::Kind::scalar) cargs.push_back(pname);
            if (x.kind == Crossing::Kind::string_in) {
                cargs.push_back(pname + ".as_ptr()");
                cargs.push_back(pname + ".len()");
            }
            if (x.kind == Crossing::Kind::object_in) cargs.push_back(pname + ".handle");
        }
        switch (r.kind) {
        case Crossing::Kind::scalar: cargs.push_back("mut &out"); break;
        case Crossing::Kind::string_out:
            cargs.push_back("mut &out");
            cargs.push_back("mut &out_len");
            break;
        case Crossing::Kind::object_out: cargs.push_back("mut &out"); break;
        default: break;
        }
        cargs.push_back("mut &error");
        cargs.push_back("mut &error_len");
        std::string call_args;
        for (std::size_t i = 0; i < cargs.size(); ++i) call_args += (i ? ", " : "") + cargs[i];
        src << in << "mut i32 status = 0;\n";
        src << in << std::format("unsafe {{ status = {}({}); }}\n", fn, call_args);
        std::string produced;
        switch (r.kind) {
        case Crossing::Kind::none: produced = ""; break;
        case Crossing::Kind::scalar: produced = "out"; break;
        case Crossing::Kind::string_out: produced = "take_string(out, out_len)"; break;
        case Crossing::Kind::object_out: produced = std::format("{}{{ .handle = out }}", r.edax); break;
        default: break;
        }
        if (f.no_throw) {
            src << in << "if (status != 0) { std::panic(take_exception(error, error_len).message); }\n";
            if (!produced.empty()) src << in << "return " << produced << ";\n";
        } else {
            src << in << "if (status != 0) { return std::error(take_exception(error, error_len)); }\n";
            src << in << (produced.empty() ? "return std::ok();\n" : "return std::ok(" + produced + ");\n");
        }
        src << ind << "    }\n\n";
    }

    const Class& classes_by_key(const std::string& key) const {
        for (const Class& c : classes_) {
            if (c.key == key) return c;
        }
        return classes_.front();
    }
};

} // namespace

std::string cpp_wrap_probe(const CppWrapOptions& options) {
    std::string out;
    for (const std::string& h : options.headers) out += "#include \"" + h + "\"\n";
    for (const std::string& t : options.templates) out += "template class " + t + ";\n";
    return out;
}

std::string cpp_wrap_filter(const std::string& name) {
    std::string base = name.substr(0, name.find('<'));
    return base;
}

CppWrapResult cpp_wrap_generate(const CppWrapOptions& options, const std::vector<std::string>& dumps) {
    return Generator(options).run(dumps);
}

} // namespace edax
