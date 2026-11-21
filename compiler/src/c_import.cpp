// Importing C headers (C interop, "Importing C headers"): the C compiler preprocesses the headers
// with the target's include directories and defines (`cc -E -dD`), and this file reads the
// declarations and writes a Rift header of `extern` declarations, structs with C's layout,
// enums, type aliases and constants. What it can't translate faithfully is listed in a report
// with the reason, never guessed.

#include "rift/c_import.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>

namespace rift {
namespace {

// --- tokens -------------------------------------------------------------------------------------

struct CToken {
    enum class Kind { identifier, number, string, character, punct, end };
    Kind        kind = Kind::end;
    std::string text;
    std::string file;   // the file it comes from (the preprocessor's line markers)
};

// The preprocessed text as tokens, and the object-like macros of `-dD`.
struct Lexed {
    std::vector<CToken>                                        tokens;
    std::vector<std::tuple<std::string, std::string, std::string>> defines;   // name, body, file
    std::vector<std::pair<std::string, std::string>>          function_macros;   // name, file
    std::vector<std::pair<std::string, std::string>>          includes;          // includer, included
};

Lexed lex(const std::string& text) {
    Lexed       out;
    std::string file;
    std::size_t i = 0;
    auto at_line_start = [&](std::size_t p) { return p == 0 || text[p - 1] == '\n'; };
    while (i < text.size()) {
        char c = text[i];
        if (c == '#' && at_line_start(i)) {   // a line marker or a directive kept by -dD
            std::size_t end = text.find('\n', i);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(i, end - i);
            i                = end;
            std::istringstream in(line.substr(1));
            std::string        word;
            in >> word;
            if (!word.empty() && std::isdigit(static_cast<unsigned char>(word[0]))) {
                std::size_t q = line.find('"');
                std::size_t r = q == std::string::npos ? q : line.find('"', q + 1);
                if (r != std::string::npos) {
                    std::string next = line.substr(q + 1, r - q - 1);
                    std::istringstream flags(line.substr(r + 1));
                    int                flag = 0;
                    while (flags >> flag) {
                        if (flag == 1) out.includes.emplace_back(file, next);   // `file` includes `next`
                    }
                    file = next;
                }
            } else if (word == "define") {
                std::string name;
                std::size_t at = line.find("define") + 6;
                while (at < line.size() && line[at] == ' ') ++at;
                std::size_t name_end = at;
                while (name_end < line.size() && (std::isalnum(static_cast<unsigned char>(line[name_end])) || line[name_end] == '_')) ++name_end;
                name = line.substr(at, name_end - at);
                if (name_end < line.size() && line[name_end] == '(') {
                    out.function_macros.emplace_back(name, file);
                } else {
                    std::string body = name_end < line.size() ? line.substr(name_end) : "";
                    std::size_t b    = body.find_first_not_of(" \t");
                    out.defines.emplace_back(name, b == std::string::npos ? "" : body.substr(b), file);
                }
            }
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        CToken t;
        t.file = file;
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t s = i;
            while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_')) ++i;
            t.kind = CToken::Kind::identifier;
            t.text = text.substr(s, i - s);
        } else if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1])))) {
            std::size_t s = i;
            while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '.' || text[i] == '_' ||
                                       ((text[i] == '+' || text[i] == '-') && (text[i - 1] == 'e' || text[i - 1] == 'E' || text[i - 1] == 'p')))) {
                ++i;
            }
            t.kind = CToken::Kind::number;
            t.text = text.substr(s, i - s);
        } else if (c == '"' || c == '\'') {
            std::size_t s = i++;
            while (i < text.size() && text[i] != c) {
                if (text[i] == '\\') ++i;
                ++i;
            }
            ++i;
            t.kind = c == '"' ? CToken::Kind::string : CToken::Kind::character;
            t.text = text.substr(s, i - s);
        } else {
            static const char* three[] = {"...", "<<=", ">>="};
            static const char* two[]   = {"->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "::"};
            t.kind = CToken::Kind::punct;
            bool found = false;
            for (const char* p : three) {
                if (text.compare(i, 3, p) == 0) {
                    t.text = p;
                    i += 3;
                    found = true;
                    break;
                }
            }
            if (!found) {
                for (const char* p : two) {
                    if (text.compare(i, 2, p) == 0) {
                        t.text = p;
                        i += 2;
                        found = true;
                        break;
                    }
                }
            }
            if (!found) t.text = std::string(1, text[i++]);
        }
        out.tokens.push_back(std::move(t));
    }
    CToken end;
    end.kind = CToken::Kind::end;
    out.tokens.push_back(end);
    return out;
}

// --- C types ------------------------------------------------------------------------------------

struct CType;
using CTypePtr = std::shared_ptr<CType>;

struct CType {
    enum class Kind { builtin, named, record, enumeration, pointer, array, function };
    Kind                  kind = Kind::builtin;
    std::string           name;          // builtin: "i32", "f64", "void", ...; named: the typedef; record/enum: the tag
    bool                  is_union = false;
    bool                  is_const = false;   // pointer: what it points to is const
    CTypePtr              inner;              // pointer, array, function: the pointee, element, result
    std::uint64_t         count = 0;          // array
    std::vector<CTypePtr> params;             // function
    std::vector<std::string> param_names;     // function: as the header names them ("" if not)
    bool                  variadic = false;
};

struct CField {
    std::string name;
    CTypePtr    type;
    bool        bitfield = false;
};

struct CRecord {
    std::string         name;
    bool                is_union = false;
    bool                defined  = false;
    std::vector<CField> fields;
    std::string         file;
};

struct CEnum {
    std::string                                       name;
    std::vector<std::pair<std::string, std::int64_t>> values;
    std::string                                       file;
};

// --- the parser ---------------------------------------------------------------------------------

class Parser {
public:
    Parser(const Lexed& lexed, std::function<bool(const std::string&)> wanted)
        : t_(lexed.tokens), wanted_(std::move(wanted)) {}

    std::map<std::string, CRecord>  records;
    std::vector<std::string>        record_order;
    std::map<std::string, CEnum>    enums;
    std::vector<std::string>        enum_order;
    std::map<std::string, CTypePtr> typedefs;
    std::vector<std::string>        typedef_order;
    std::map<std::string, std::string> typedef_file;
    std::vector<std::pair<std::string, CTypePtr>> functions;   // name, function type
    std::map<std::string, std::string> function_file;
    std::map<std::string, std::int64_t> enum_constants;          // every enumerator, for macros and arrays
    std::vector<std::pair<std::string, std::string>> skipped;     // name, reason

    void run() {
        while (!at_end()) {
            std::size_t before = p_;
            try {
                declaration();
            } catch (const Bail&) {
                skip_declaration(before);
            }
            if (p_ == before) ++p_;
        }
        name_anonymous();
    }

    // Names anonymous structs, unions and enums: `typedef struct { ... } name;` after the
    // typedef, a nested `union { ... } data;` of `token` as `token_data`.
    void name_anonymous() {
        auto rename_record = [&](const CTypePtr& t, const std::string& to) {
            if (records.contains(to)) return false;
            auto node = records.extract(t->name);
            node.key()       = to;
            node.mapped().name = to;
            records.insert(std::move(node));
            std::replace(record_order.begin(), record_order.end(), t->name, to);
            t->name = to;
            return true;
        };
        for (const std::string& name : typedef_order) {
            const CTypePtr& t = typedefs.at(name);
            if (t->kind == CType::Kind::record && t->name.starts_with("anonymous") && records.contains(t->name)) {
                rename_record(t, name);
            } else if (t->kind == CType::Kind::enumeration && t->name.starts_with("anonymous") && enums.contains(t->name) && !enums.contains(name)) {
                auto node = enums.extract(t->name);
                node.key()         = name;
                node.mapped().name = name;
                enums.insert(std::move(node));
                std::replace(enum_order.begin(), enum_order.end(), t->name, name);
                t->name = name;
            }
        }
        std::function<void(const std::string&)> nested = [&](const std::string& outer) {
            std::vector<std::pair<CTypePtr, std::string>> todo;
            for (CField& f : records.at(outer).fields) {
                CTypePtr t = f.type;
                while (t->kind == CType::Kind::array) t = t->inner;
                if (t->kind == CType::Kind::record && t->name.starts_with("anonymous") && records.contains(t->name)) todo.emplace_back(t, outer + "_" + f.name);
            }
            for (auto& [t, to] : todo) {
                if (rename_record(t, to)) nested(to);
            }
        };
        std::vector<std::string> named;
        for (const std::string& n : record_order) {
            if (!n.starts_with("anonymous")) named.push_back(n);
        }
        for (const std::string& n : named) nested(n);
    }

private:
    struct Bail {};
    const std::vector<CToken>&               t_;
    std::size_t                              p_ = 0;
    std::function<bool(const std::string&)> wanted_;
    int                                      anonymous_ = 0;

    bool at_end() const { return t_[p_].kind == CToken::Kind::end; }
    const CToken& cur() const { return t_[p_]; }
    bool is(const char* text) const { return t_[p_].text == text && t_[p_].kind != CToken::Kind::string; }
    bool accept(const char* text) {
        if (!is(text)) return false;
        ++p_;
        return true;
    }
    void expect(const char* text) {
        if (!accept(text)) throw Bail{};
    }

    // Skips to the end of the declaration that starts at `from`: the next `;` at depth 0, or
    // the end of a `{ ... }` body (a function definition).
    void skip_declaration(std::size_t from) {
        p_        = from;
        int depth = 0;
        while (!at_end()) {
            const std::string& x = cur().text;
            if (x == "(" || x == "[" || x == "{") ++depth;
            if (x == ")" || x == "]" || x == "}") {
                --depth;
                if (depth == 0 && x == "}") {
                    ++p_;
                    if (is(";")) ++p_;
                    else return;
                    return;
                }
            }
            ++p_;
            if (depth <= 0 && x == ";") return;
        }
    }

    // `__attribute__((...))`, `__asm__(...)`, `__declspec(...)`, `__extension__`, ...
    void skip_extensions() {
        while (true) {
            const std::string& x = cur().text;
            if (x == "__extension__" || x == "__inline" || x == "__inline__" || x == "__restrict" || x == "__restrict__" ||
                x == "restrict" || x == "volatile" || x == "__volatile__" || x == "_Noreturn" || x == "__wur" || x == "_Nonnull" ||
                x == "_Nullable" || x == "__THROW") {
                ++p_;
                continue;
            }
            if (x == "__attribute__" || x == "__asm__" || x == "__asm" || x == "asm" || x == "__declspec" || x == "_Alignas" ||
                x == "__alignas" || x == "_Static_assert" || x == "__typeof__") {
                ++p_;
                if (is("(")) {
                    int depth = 0;
                    do {
                        if (is("(")) ++depth;
                        if (is(")")) --depth;
                        ++p_;
                    } while (depth > 0 && !at_end());
                }
                continue;
            }
            return;
        }
    }

    // `a op b` in 64-bit two's complement, without signed overflow.
    static std::int64_t wrap(std::int64_t a, std::int64_t b, char op) {
        std::uint64_t x = static_cast<std::uint64_t>(a), y = static_cast<std::uint64_t>(b);
        return static_cast<std::int64_t>(op == '+' ? x + y : op == '-' ? x - y : x * y);
    }

    static bool c_type_word(const std::string& w) {
        static const std::set<std::string> words = {"void", "char", "short", "int", "long", "float", "double", "signed", "unsigned",
                                                    "_Bool", "bool", "const", "volatile", "struct", "union", "enum"};
        return words.contains(w);
    }

    static CTypePtr builtin(const std::string& n) {
        auto t  = std::make_shared<CType>();
        t->name = n;
        return t;
    }

    // The declaration specifiers: storage class, qualifiers and the base type.
    struct Specifiers {
        bool     is_typedef = false, is_static = false, is_inline = false, is_extern = false, is_const = false;
        CTypePtr type;
    };

    Specifiers specifiers() {
        Specifiers s;
        int  longs = 0;
        bool is_unsigned = false, is_signed = false, saw_int = false, saw_char = false, saw_short = false;
        bool saw_float = false, saw_double = false, saw_bool = false, saw_void = false;
        while (true) {
            skip_extensions();
            const std::string& x = cur().text;
            if (cur().kind != CToken::Kind::identifier) break;
            if (x == "typedef") s.is_typedef = true;
            else if (x == "static") s.is_static = true;
            else if (x == "inline") s.is_inline = true;
            else if (x == "extern") s.is_extern = true;
            else if (x == "const" || x == "__const") s.is_const = true;
            else if (x == "register" || x == "auto" || x == "_Thread_local" || x == "__thread") {}
            else if (x == "unsigned") is_unsigned = true;
            else if (x == "signed" || x == "__signed__") is_signed = true;
            else if (x == "long") ++longs;
            else if (x == "short") saw_short = true;
            else if (x == "int") saw_int = true;
            else if (x == "char") saw_char = true;
            else if (x == "float") saw_float = true;
            else if (x == "double") saw_double = true;
            else if (x == "_Bool" || x == "bool") saw_bool = true;
            else if (x == "void") saw_void = true;
            else if (x == "struct" || x == "union") {
                ++p_;
                s.type = record(x == "union");
                continue;
            } else if (x == "enum") {
                ++p_;
                s.type = enumeration();
                continue;
            } else if (!s.type && !is_unsigned && !is_signed && !longs && !saw_int && !saw_char && !saw_short && !saw_float &&
                       !saw_double && !saw_bool && !saw_void) {
                auto t  = std::make_shared<CType>();
                t->kind = CType::Kind::named;
                t->name = x;
                s.type  = t;
            } else {
                break;
            }
            ++p_;
        }
        if (!s.type) {
            std::string n;
            if (saw_void) n = "void";
            else if (saw_bool) n = "bool";
            else if (saw_float) n = "f32";
            else if (saw_double) n = longs ? "long double" : "f64";
            else if (saw_char) n = is_unsigned ? "u8" : "i8";
            else if (saw_short) n = is_unsigned ? "u16" : "i16";
            else if (longs >= 1) n = is_unsigned ? "u64" : "i64";
            else if (saw_int || is_unsigned || is_signed) n = is_unsigned ? "u32" : "i32";
            else throw Bail{};
            s.type = builtin(n);
        }
        return s;
    }

    CTypePtr record(bool is_union) {
        skip_extensions();
        std::string tag;
        if (cur().kind == CToken::Kind::identifier) tag = t_[p_++].text;
        if (tag.empty()) tag = std::format("anonymous_{}", ++anonymous_);
        auto t      = std::make_shared<CType>();
        t->kind     = CType::Kind::record;
        t->name     = tag;
        t->is_union = is_union;
        CRecord& r = records[tag];
        if (r.name.empty()) {
            r.name     = tag;
            r.is_union = is_union;
            r.file     = cur().file;
            record_order.push_back(tag);
        }
        if (accept("{")) {
            r.defined = true;
            r.file    = cur().file;
            r.fields.clear();
            while (!accept("}")) {
                if (at_end()) throw Bail{};
                Specifiers fs = specifiers();
                do {
                    CField f;
                    f.type = declarator(fs.type, f.name, fs.is_const);
                    if (accept(":")) {
                        f.bitfield = true;
                        constant_expression();
                    }
                    if (f.name.empty() && f.type->kind == CType::Kind::record) f.name = std::format("anonymous_{}", r.fields.size());
                    r.fields.push_back(f);
                } while (accept(","));
                expect(";");
            }
        }
        skip_extensions();
        return t;
    }

    CTypePtr enumeration() {
        skip_extensions();
        std::string tag;
        if (cur().kind == CToken::Kind::identifier) tag = t_[p_++].text;
        if (tag.empty()) tag = std::format("anonymous_enum_{}", ++anonymous_);
        auto t  = std::make_shared<CType>();
        t->kind = CType::Kind::enumeration;
        t->name = tag;
        if (accept("{")) {
            CEnum& e = enums[tag];
            if (e.name.empty()) enum_order.push_back(tag);
            e.name = tag;
            e.file = cur().file;
            std::int64_t next = 0;
            while (!accept("}")) {
                if (cur().kind != CToken::Kind::identifier) throw Bail{};
                std::string name = t_[p_++].text;
                if (accept("=")) next = constant_expression();
                e.values.emplace_back(name, next);
                enum_constants[name] = next;
                next = wrap(next, 1, '+');
                if (!accept(",")) {
                    expect("}");
                    break;
                }
            }
        }
        return t;
    }

    // A declarator: pointers, a name (or a parenthesized declarator: a function pointer),
    // then arrays and parameter lists. Builds the type from the inside out.
    CTypePtr declarator(CTypePtr base, std::string& name, bool base_const) {
        skip_extensions();
        std::vector<bool> pointers;   // each `*`, with whether what it points to is const
        bool              next_const = base_const;
        while (is("*")) {
            ++p_;
            pointers.push_back(next_const);
            next_const = false;
            while (is("const") || is("__const") || is("volatile") || is("restrict") || is("__restrict") || is("__restrict__") ||
                   is("_Nonnull") || is("_Nullable")) {
                if (is("const") || is("__const")) next_const = true;
                ++p_;
            }
        }
        CTypePtr t = base;
        for (bool c : pointers) {
            auto ptr      = std::make_shared<CType>();
            ptr->kind     = CType::Kind::pointer;
            ptr->inner    = t;
            ptr->is_const = c;
            t             = ptr;
        }
        // A parenthesized declarator: `(*name)(params)`. Read its inside later, around the suffixes.
        std::size_t inner_start = 0, inner_end = 0;
        if (is("(") && (t_[p_ + 1].text == "*" || t_[p_ + 1].text == "^")) {
            inner_start = ++p_;
            int depth   = 1;
            while (depth > 0 && !at_end()) {
                if (is("(")) ++depth;
                if (is(")")) --depth;
                ++p_;
            }
            inner_end = p_ - 1;
        } else if (is("(") && t_[p_ + 1].kind == CToken::Kind::identifier && t_[p_ + 2].text == ")" && !typedefs.contains(t_[p_ + 1].text) &&
                   !c_type_word(t_[p_ + 1].text)) {
            name = t_[p_ + 1].text;   // `( name) (params)`: a parenthesized name (libpng's PNG_EXPORT)
            p_ += 3;
        } else if (cur().kind == CToken::Kind::identifier && !is("const")) {
            name = t_[p_++].text;
        }
        skip_extensions();
        t = suffixes(t);
        skip_extensions();
        if (inner_end) {
            std::size_t after = p_;
            p_                = inner_start;
            t                 = declarator(t, name, false);
            p_                = after;
        }
        return t;
    }

    CTypePtr suffixes(CTypePtr t) {
        if (accept("[")) {
            std::uint64_t n = 0;
            if (!is("]")) n = static_cast<std::uint64_t>(constant_expression());
            expect("]");
            CTypePtr element = suffixes(t);
            auto a           = std::make_shared<CType>();
            a->kind          = CType::Kind::array;
            a->inner         = element;
            a->count         = n;
            return a;
        }
        if (accept("(")) {
            auto f   = std::make_shared<CType>();
            f->kind  = CType::Kind::function;
            f->inner = t;
            if (is("void") && t_[p_ + 1].text == ")") ++p_;
            while (!accept(")")) {
                if (accept("...")) {
                    f->variadic = true;
                    continue;
                }
                Specifiers  ps = specifiers();
                std::string pname;
                CTypePtr    pt = declarator(ps.type, pname, ps.is_const);
                f->param_names.push_back(pname);
                if (pt->kind == CType::Kind::array) {   // an array parameter is a pointer
                    auto ptr   = std::make_shared<CType>();
                    ptr->kind  = CType::Kind::pointer;
                    ptr->inner = pt->inner;
                    pt         = ptr;
                }
                f->params.push_back(pt);
                if (!accept(",")) {
                    expect(")");
                    break;
                }
            }
            return f;
        }
        return t;
    }

    // Constant expressions: integers, enumerators, sizeof is not supported (bail).
    // An expression is unsigned when one of its literals is (`18446744073709551615UL / 2`): C's
    // usual arithmetic conversions make the whole of it unsigned.
public:
    bool unsigned_ = false;

private:
    int depth_ = 0;
    std::int64_t constant_expression() {
        if (depth_++ == 0) unsigned_ = false;
        std::int64_t v = conditional();
        --depth_;
        return v;
    }
    std::int64_t conditional() {
        std::int64_t c = binary(0);
        if (accept("?")) {
            std::int64_t a = conditional();
            expect(":");
            std::int64_t b = conditional();
            return c ? a : b;
        }
        return c;
    }
    static int precedence(const std::string& op) {
        static const std::map<std::string, int> p{{"||", 1}, {"&&", 2}, {"|", 3}, {"^", 4}, {"&", 5}, {"==", 6}, {"!=", 6},
                                                  {"<", 7},  {">", 7},  {"<=", 7}, {">=", 7}, {"<<", 8}, {">>", 8},
                                                  {"+", 9},  {"-", 9},  {"*", 10}, {"/", 10}, {"%", 10}};
        auto it = p.find(op);
        return it == p.end() ? -1 : it->second;
    }
    std::int64_t binary(int min) {
        std::int64_t lhs = unary();
        while (cur().kind == CToken::Kind::punct && precedence(cur().text) > min) {
            std::string op = t_[p_++].text;
            std::int64_t rhs = binary(precedence(op));
            if (op == "+") lhs = wrap(lhs, rhs, '+');   // C's unsigned arithmetic wraps; no signed overflow here
            else if (op == "-") lhs = wrap(lhs, rhs, '-');
            else if (op == "*") lhs = wrap(lhs, rhs, '*');
            else if (op == "/" && unsigned_) lhs = rhs ? static_cast<std::int64_t>(static_cast<std::uint64_t>(lhs) / static_cast<std::uint64_t>(rhs)) : 0;
            else if (op == "%" && unsigned_) lhs = rhs ? static_cast<std::int64_t>(static_cast<std::uint64_t>(lhs) % static_cast<std::uint64_t>(rhs)) : 0;
            else if (op == "/") lhs = rhs ? lhs / rhs : 0;
            else if (op == "%") lhs = rhs ? lhs % rhs : 0;
            else if (op == "<<") lhs = static_cast<std::int64_t>(static_cast<std::uint64_t>(lhs) << (rhs & 63));
            else if (op == ">>" && unsigned_) lhs = static_cast<std::int64_t>(static_cast<std::uint64_t>(lhs) >> (rhs & 63));
            else if (op == ">>") lhs >>= (rhs & 63);
            else if (op == "&") lhs &= rhs;
            else if (op == "|") lhs |= rhs;
            else if (op == "^") lhs ^= rhs;
            else if (op == "&&") lhs = lhs && rhs;
            else if (op == "||") lhs = lhs || rhs;
            else if (op == "==") lhs = lhs == rhs;
            else if (op == "!=") lhs = lhs != rhs;
            else if (op == "<") lhs = lhs < rhs;
            else if (op == ">") lhs = lhs > rhs;
            else if (op == "<=") lhs = lhs <= rhs;
            else if (op == ">=") lhs = lhs >= rhs;
        }
        return lhs;
    }
    // `sizeof(T)` for numbers and pointers (LP64), as array lengths in system headers use it.
    static std::int64_t size_of(const CTypePtr& t, const std::map<std::string, CTypePtr>& typedefs) {
        switch (t->kind) {
        case CType::Kind::builtin: {
            const std::string& n = t->name;
            if (n == "void") throw Bail{};
            return n == "bool" || n == "i8" || n == "u8" ? 1 : n == "i16" || n == "u16" ? 2 : n == "i32" || n == "u32" || n == "f32" ? 4 : n == "long double" ? 16 : 8;
        }
        case CType::Kind::pointer: return 8;
        case CType::Kind::enumeration: return 4;
        case CType::Kind::array: return static_cast<std::int64_t>(t->count) * size_of(t->inner, typedefs);
        case CType::Kind::named:
            if (auto it = typedefs.find(t->name); it != typedefs.end()) return size_of(it->second, typedefs);
            throw Bail{};
        default: throw Bail{};
        }
    }

    std::int64_t unary() {
        if (accept("sizeof")) {
            expect("(");
            Specifiers  st = specifiers();
            std::string unused;
            CTypePtr    t  = declarator(st.type, unused, st.is_const);
            expect(")");
            return size_of(t, typedefs);
        }
        if (accept("-")) return wrap(0, unary(), '-');
        if (accept("+")) return unary();
        if (accept("~")) return ~unary();
        if (accept("!")) return !unary();
        if (accept("(")) {
            // A cast `(type)expr` or a parenthesized expression.
            if (cur().kind == CToken::Kind::identifier && !enum_constants.contains(cur().text)) {
                int depth = 1;
                while (depth > 0 && !at_end()) {
                    if (is("(")) ++depth;
                    if (is(")")) --depth;
                    ++p_;
                }
                return unary();
            }
            std::int64_t v = constant_expression();
            expect(")");
            return v;
        }
        if (cur().kind == CToken::Kind::number) {
            const std::string& text = t_[p_++].text;
            std::int64_t       v    = integer_literal(text);
            if (text.find_first_of("uU") != std::string::npos || v < 0) unsigned_ = true;   // a suffix, or too big for a signed long
            return v;
        }
        if (cur().kind == CToken::Kind::character) {
            std::string c = t_[p_++].text;
            return c.size() >= 3 ? static_cast<unsigned char>(c[1]) : 0;
        }
        if (cur().kind == CToken::Kind::identifier) {
            auto it = enum_constants.find(cur().text);
            if (it == enum_constants.end()) throw Bail{};
            ++p_;
            return it->second;
        }
        throw Bail{};
    }

    void declaration() {
        skip_extensions();
        if (accept(";")) return;
        std::string file = cur().file;
        Specifiers  s    = specifiers();
        if (is(";")) {   // `struct x { ... };`, `enum e { ... };`
            ++p_;
            return;
        }
        while (true) {
            std::string name;
            CTypePtr    t = declarator(s.type, name, s.is_const);
            skip_extensions();
            if (is("{")) {   // a function definition (`static inline`): not translated
                if (!name.empty() && wanted_(file)) skipped.emplace_back(name, "an inline function (it has no symbol to call)");
                skip_declaration(p_);
                return;
            }
            if (accept("=")) {   // an initialized variable: skip its value
                int depth = 0;
                while (!at_end() && !(depth == 0 && (is(",") || is(";")))) {
                    if (is("(") || is("{") || is("[")) ++depth;
                    if (is(")") || is("}") || is("]")) --depth;
                    ++p_;
                }
            }
            if (!name.empty()) {
                if (s.is_typedef) {
                    if (!typedefs.contains(name)) typedef_order.push_back(name);
                    typedefs[name]     = t;
                    typedef_file[name] = file;
                } else if (t->kind == CType::Kind::function) {
                    if (s.is_static || s.is_inline) {
                        if (wanted_(file)) skipped.emplace_back(name, "a static or inline function (it has no symbol to call)");
                    } else if (!function_file.contains(name)) {
                        functions.emplace_back(name, t);
                        function_file[name] = file;
                    }
                } else if (wanted_(file)) {
                    skipped.emplace_back(name, "a global variable (only functions, types and constants are translated)");
                }
            }
            if (!accept(",")) break;
        }
        expect(";");
    }

public:
    static std::int64_t integer_literal(std::string text) {
        while (!text.empty() && (text.back() == 'u' || text.back() == 'U' || text.back() == 'l' || text.back() == 'L')) text.pop_back();
        try {
            if (text.starts_with("0x") || text.starts_with("0X")) return static_cast<std::int64_t>(std::stoull(text.substr(2), nullptr, 16));
            if (text.starts_with("0b") || text.starts_with("0B")) return static_cast<std::int64_t>(std::stoull(text.substr(2), nullptr, 2));
            if (text.size() > 1 && text[0] == '0') return static_cast<std::int64_t>(std::stoull(text, nullptr, 8));
            return static_cast<std::int64_t>(std::stoull(text, nullptr, 10));
        } catch (...) {
            throw Bail{};
        }
    }
};

// --- writing Rift ---------------------------------------------------------------------------------

const std::set<std::string>& rift_keywords() {
    static const std::set<std::string> k = {
        "import", "struct", "class", "interface", "enum", "union", "namespace", "public", "private", "static", "mut", "move",
        "return", "if", "else", "while", "for", "match", "break", "continue", "true", "false", "nullptr", "this", "type",
        "template", "unsafe", "extern", "check", "compile_time", "run_time", "default", "view", "override", "open", "abstract",
        "operator", "cast", "bit", "auto", "deduce", "thread_local", "annotation", "u8", "u16", "u32", "u64", "i8", "i16",
        "i32", "i64", "f32", "f64", "bool", "char", "void", "never", "usize", "isize", "u128", "i128"};
    return k;
}

std::string safe_name(const std::string& n) { return rift_keywords().contains(n) ? n + "_" : n; }

struct Writer {
    const Parser&                                     parser;
    std::function<bool(const std::string&)>           wanted;
    std::vector<std::pair<std::string, std::string>>& report;
    std::set<std::string>                             needed;   // records of other headers the translation uses

    // The C typedefs of the system headers that have a Rift spelling of their own.
    static std::optional<std::string> known_typedef(const std::string& n) {
        static const std::map<std::string, std::string> k = {
            {"size_t", "usize"},    {"ssize_t", "isize"},   {"ptrdiff_t", "isize"},  {"intptr_t", "isize"},  {"uintptr_t", "usize"},
            {"int8_t", "i8"},       {"int16_t", "i16"},     {"int32_t", "i32"},      {"int64_t", "i64"},     {"uint8_t", "u8"},
            {"uint16_t", "u16"},    {"uint32_t", "u32"},    {"uint64_t", "u64"},     {"bool", "bool"},       {"_Bool", "bool"},
            {"wchar_t", "i32"},     {"char16_t", "u16"},    {"char32_t", "u32"},     {"off_t", "i64"},       {"time_t", "i64"}};
        auto it = k.find(n);
        if (it == k.end()) return std::nullopt;
        return it->second;
    }

    // The Rift spelling of `t`; empty when it can't be written (`why` says why).
    std::string type(const CTypePtr& t, std::string& why, bool in_pointer = false) {
        switch (t->kind) {
        case CType::Kind::builtin:
            if (t->name == "long double") {
                why = "long double has no Rift type";
                return "";
            }
            if (t->name == "void" && !in_pointer) return "void";
            return t->name == "void" ? "u8" : t->name;
        case CType::Kind::named: {
            if (auto k = known_typedef(t->name)) return *k;
            if (auto it = parser.typedefs.find(t->name); it != parser.typedefs.end()) {
                // A typedef of another header (a system header's `uLong`) is spelled out.
                if (!wanted(parser.typedef_file.at(t->name))) return it->second->kind == CType::Kind::function ? function_pointer(it->second, why) : type(it->second, why, in_pointer);
                return t->name;
            }
            why = std::format("the type '{}' is not known", t->name);
            return "";
        }
        case CType::Kind::record:
            if (t->name.starts_with("anonymous")) {
                why = "an anonymous struct or union";
                return "";
            }
            if (auto it = parser.records.find(t->name); it == parser.records.end() || !wanted(it->second.file) || !it->second.defined) needed.insert(t->name);
            return t->name;
        case CType::Kind::enumeration:
            if (t->name.starts_with("anonymous")) return "i32";
            if (auto it = parser.enums.find(t->name); it == parser.enums.end() || !wanted(it->second.file)) return "i32";
            return t->name;
        case CType::Kind::pointer: {
            if (t->inner->kind == CType::Kind::function) return function_pointer(t->inner, why);
            if (t->inner->kind == CType::Kind::named) {   // a pointer to a function typedef is the typedef itself
                auto it = parser.typedefs.find(t->inner->name);
                if (it != parser.typedefs.end() && it->second->kind == CType::Kind::function) return function_pointer(it->second, why);
            }
            std::string inner = type(t->inner, why, true);
            if (inner.empty()) return "";
            return std::format("std::raw_ptr<{}{}>", t->is_const ? "" : "mut ", inner);
        }
        case CType::Kind::array: {
            std::string inner = type(t->inner, why);
            if (inner.empty()) return "";
            if (t->count == 0) {
                why = "an array without a length";
                return "";
            }
            return std::format("std::array<{}, {}>", inner, t->count);
        }
        case CType::Kind::function: return function_pointer(t, why);
        }
        return "";
    }

    // A field's initial value: `default`, or for an enum (which has no default) its
    // enumerator 0, else its first one.
    std::string initial(const CTypePtr& t) {
        CTypePtr e = t;
        while (e->kind == CType::Kind::named && parser.typedefs.contains(e->name)) e = parser.typedefs.at(e->name);
        if (e->kind != CType::Kind::enumeration || e->name.starts_with("anonymous")) return "default";
        auto it = parser.enums.find(e->name);
        if (it == parser.enums.end() || it->second.values.empty() || !wanted(it->second.file)) return "default";
        std::string chosen = it->second.values.front().first;
        for (const auto& [n, v] : it->second.values) {
            if (v == 0) {
                chosen = n;
                break;
            }
        }
        return safe_name(e->name) + "::" + safe_name(chosen);
    }

    std::string function_pointer(const CTypePtr& f, std::string& why) {
        if (f->variadic) {
            why = "a variadic function pointer";
            return "";
        }
        // Rift calls a fn_ptr with its own convention, which matches C's for numbers and pointers
        // but not for structs passed by value.
        auto by_value = [&](CTypePtr t) {
            while (t->kind == CType::Kind::named && parser.typedefs.contains(t->name)) t = parser.typedefs.at(t->name);
            return t->kind == CType::Kind::record;
        };
        bool structs = by_value(f->inner);
        for (const CTypePtr& p : f->params) structs = structs || by_value(p);
        if (structs) {
            why = "a function pointer that passes structs by value";
            return "";
        }
        std::string r = type(f->inner, why);
        if (r.empty()) return "";
        std::string out = "std::fn_ptr<" + r + "(";
        for (std::size_t i = 0; i < f->params.size(); ++i) {
            std::string p = type(f->params[i], why);
            if (p.empty()) return "";
            out += (i ? ", " : "") + p;
        }
        return out + ")>";
    }

    // C's size and alignment of `t` on 64-bit targets (for unions).
    std::pair<std::uint64_t, std::uint64_t> layout(const CTypePtr& t) {
        switch (t->kind) {
        case CType::Kind::builtin: {
            const std::string& n = t->name;
            std::uint64_t s = n == "bool" || n == "i8" || n == "u8" ? 1 : n == "i16" || n == "u16" ? 2 : n == "i32" || n == "u32" || n == "f32" ? 4 : 8;
            return {s, s};
        }
        case CType::Kind::named: {
            if (auto k = known_typedef(t->name)) {
                auto b  = std::make_shared<CType>();
                b->name = *k == "usize" ? "u64" : *k == "isize" ? "i64" : *k;
                return layout(b);
            }
            auto it = parser.typedefs.find(t->name);
            return it == parser.typedefs.end() ? std::pair<std::uint64_t, std::uint64_t>{8, 8} : layout(it->second);
        }
        case CType::Kind::enumeration: return {4, 4};
        case CType::Kind::pointer:
        case CType::Kind::function: return {8, 8};
        case CType::Kind::array: {
            auto [s, a] = layout(t->inner);
            return {s * t->count, a};
        }
        case CType::Kind::record: {
            auto it = parser.records.find(t->name);
            if (it == parser.records.end()) return {0, 1};
            std::uint64_t size = 0, align = 1;
            for (const CField& f : it->second.fields) {
                auto [s, a] = layout(f.type);
                align       = std::max(align, a);
                if (it->second.is_union) size = std::max(size, s);
                else size = (size + a - 1) / a * a + s;
            }
            return {(size + align - 1) / align * align, align};
        }
        }
        return {8, 8};
    }
};

// `"a" "b"` as `"ab"` (adjacent string literals, as a macro's expansion often has).
std::string join_strings(const std::string& text) {
    Lexed l = lex(text);
    if (l.tokens.size() < 2) return text;
    std::string joined;
    for (std::size_t i = 0; i + 1 < l.tokens.size(); ++i) {
        if (l.tokens[i].kind != CToken::Kind::string) return text;
        joined += l.tokens[i].text.substr(1, l.tokens[i].text.size() - 2);
    }
    return "\"" + joined + "\"";
}

// The value of an object-like macro that is a constant: a string literal, a float literal or
// an integer expression of literals, enumerators and earlier constants, with an optional cast
// `((png_uint_32)0x7fffffffL)`. `cast_type` gives the Rift number type of a cast's C type.
std::string literal_constant(const std::string& body, const std::map<std::string, std::int64_t>& known, std::string& type,
                             const std::function<std::string(const std::string&)>& cast_type) {
    auto strip = [](std::string b) {
        while (b.size() >= 2 && b.front() == '(' && b.back() == ')') {
            int depth = 0;   // only when the parentheses enclose everything: not `(a) + (b)`
            bool whole = true;
            for (std::size_t i = 0; i < b.size(); ++i) {
                if (b[i] == '(') ++depth;
                if (b[i] == ')' && --depth == 0 && i + 1 != b.size()) whole = false;
            }
            if (!whole) break;
            b = b.substr(1, b.size() - 2);
            while (!b.empty() && b.front() == ' ') b.erase(0, 1);
            while (!b.empty() && b.back() == ' ') b.pop_back();
        }
        return b;
    };
    std::string b = strip(body);
    if (b.empty()) return "";
    if (b.front() == '"' && b.back() == '"' && b.find('"', 1) == b.size() - 1) {
        type = "std::static_str";
        return b;
    }
    std::string cast;   // `(T)rest`
    if (b.front() == '(') {
        std::size_t close = b.find(')');
        std::string inner = b.substr(1, close == std::string::npos ? 0 : close - 1);
        if (close != std::string::npos && close + 1 < b.size() && !inner.empty() && !std::isdigit(static_cast<unsigned char>(inner[0])) &&
            inner.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_ ") == std::string::npos) {
            cast = cast_type(inner);
            if (cast.empty()) return "";
            b = strip(b.substr(close + 1));
        }
    }
    bool is_float = !b.empty() && (std::isdigit(static_cast<unsigned char>(b[0])) || b[0] == '-' || b[0] == '.') && !b.starts_with("0x") &&
                    !b.starts_with("0X") && (b.find('.') != std::string::npos || b.find('e') != std::string::npos || b.find('E') != std::string::npos);
    if (is_float && (cast.empty() || cast == "f32" || cast == "f64")) {
        std::string digits = b;
        bool        f32    = false;
        if (digits.back() == 'f' || digits.back() == 'F') {
            digits.pop_back();
            f32 = true;
        }
        if (digits.find_first_not_of("0123456789.eE+-") != std::string::npos) return "";
        if (digits.find('.') == std::string::npos) {
            std::size_t e = digits.find_first_of("eE");
            digits.insert(e == std::string::npos ? digits.size() : e, ".0");
        }
        if (digits.back() == '.') digits += "0";
        if (digits.find(".e") != std::string::npos || digits.find(".E") != std::string::npos) digits.insert(digits.find('.') + 1, "0");
        type = !cast.empty() ? cast : f32 ? "f32" : "f64";
        return digits;
    }
    // An integer expression: evaluate it with the enumerators and earlier constants.
    Lexed lexed = lex(b);
    bool  is_long = false;
    for (const CToken& t : lexed.tokens) {
        if (t.kind == CToken::Kind::identifier && !known.contains(t.text) && cast_type(t.text).empty()) return "";   // a cast's type is fine
        if (t.kind == CToken::Kind::string) return "";
        if (t.kind == CToken::Kind::number && t.text.find_first_of("lL") != std::string::npos) is_long = true;
    }
    try {
        // Reuse the parser's expression reader through an enum value.
        Lexed wrapped = lex("enum __rift_constant { __rift_value = " + b + " };");
        Parser q(wrapped, [](const std::string&) { return false; });
        q.enum_constants = known;
        q.run();
        auto it = q.enum_constants.find("__rift_value");
        if (it == q.enum_constants.end()) return "";
        std::int64_t v           = it->second;
        bool         is_unsigned = q.unsigned_;
        if (!cast.empty()) {
            if (cast == "f32" || cast == "f64") {
                type = cast;
                return std::to_string(v) + ".0";
            }
            if (cast == "bool") {
                type = cast;
                return v ? "true" : "false";
            }
            int  bits      = cast == "u8" || cast == "i8" ? 8 : cast == "u16" || cast == "i16" ? 16 : cast == "u32" || cast == "i32" ? 32 : 64;
            bool unsigned_ = cast.starts_with("u");
            type           = cast;
            if (unsigned_) {
                std::uint64_t u = static_cast<std::uint64_t>(v);
                if (bits < 64) u &= (std::uint64_t{1} << bits) - 1;
                return std::to_string(u);
            }
            return std::to_string(v);
        }
        if (is_unsigned) {
            type = is_long || static_cast<std::uint64_t>(v) > 0xFFFFFFFFull ? "u64" : "u32";
            return std::to_string(static_cast<std::uint64_t>(v));
        }
        type = v >= INT32_MIN && v <= INT32_MAX ? "i32" : "i64";
        return std::to_string(v);
    } catch (...) {
        return "";
    }
}

// Which files' declarations are translated: the listed headers, the directories of a
// library's own headers (those of a listed header: `vulkan/vulkan.h` includes
// `vulkan/vulkan_core.h`; or the directory named after it: `lzma.h` includes `lzma/base.h`)
// and the C target's include directories. Not the system's other headers.
std::function<bool(const std::string&)> wanted_files(const Lexed& lexed, const CImportOptions& options) {
    std::set<std::string> files;
    for (const CToken& t : lexed.tokens) files.insert(t.file);
    for (const auto& d : lexed.defines) files.insert(std::get<2>(d));
    std::vector<std::string> library_dirs;
    for (const std::string& h : options.headers) {
        for (const std::string& f : files) {
            if (!f.ends_with("/" + h)) continue;
            std::string base  = f.substr(0, f.size() - h.size());   // the include directory, with its '/'
            std::size_t slash = h.rfind('/');
            if (slash != std::string::npos) library_dirs.push_back(base + h.substr(0, slash + 1));
            library_dirs.push_back(base + h.substr(0, h.rfind('.')) + "/");
        }
    }
    // The headers a listed header includes itself (zlib.h's zconf.h), except the C library's.
    static const std::set<std::string> c_library = {
        "assert.h", "complex.h", "ctype.h", "errno.h", "fenv.h", "float.h", "inttypes.h", "iso646.h", "limits.h", "locale.h",
        "math.h", "setjmp.h", "signal.h", "stdalign.h", "stdarg.h", "stdatomic.h", "stdbool.h", "stddef.h", "stdint.h", "stdio.h",
        "stdlib.h", "stdnoreturn.h", "string.h", "strings.h", "tgmath.h", "threads.h", "time.h", "uchar.h", "wchar.h", "wctype.h",
        "unistd.h", "fcntl.h", "pthread.h", "dlfcn.h", "malloc.h", "alloca.h", "features.h", "windows.h"};
    std::set<std::string> companions;
    for (const auto& [from, to] : lexed.includes) {
        bool listed = false;
        for (const std::string& h : options.headers) listed = listed || from == h || from.ends_with("/" + h);
        std::string name = to.substr(to.rfind('/') + 1);
        if (listed && !c_library.contains(name) && to.find("/sys/") == std::string::npos && to.find("/bits/") == std::string::npos &&
            !to.starts_with("<")) {
            companions.insert(to);
        }
    }
    return [options, library_dirs, companions](const std::string& file) {
        if (companions.contains(file)) return true;
        if (file.empty() || file.starts_with("<")) return false;
        for (const std::string& h : options.headers) {
            if (file == h || file.ends_with("/" + h)) return true;
        }
        for (const std::string& d : library_dirs) {
            if (file.starts_with(d)) return true;
        }
        for (const std::string& d : options.include_dirs) {
            if (!d.empty() && file.starts_with(d)) return true;
        }
        return false;
    };
}

} // namespace

std::string c_macro_probe(const std::string& preprocessed, const CImportOptions& options) {
    Lexed lexed  = lex(preprocessed);
    auto  wanted = wanted_files(lexed, options);
    std::string out;
    for (const auto& [name, body, file] : lexed.defines) {
        if (wanted(file) && !name.starts_with("__") && !body.empty()) out += std::format("\n__rift_macro \"{}\"\n{}\n", name, name);
    }
    return out;
}

std::map<std::string, std::string> c_macro_values(const std::string& probe_output) {
    std::map<std::string, std::string> values;
    std::istringstream in(probe_output);
    std::string        line, pending;
    while (std::getline(in, line)) {
        if (line.starts_with("#")) continue;
        if (line.starts_with("__rift_macro \"")) {   // the name is a string: not expanded
            pending = line.substr(14);
            pending = pending.substr(0, pending.find('"'));
            continue;
        }
        if (!pending.empty()) {   // the expansion, maybe split by line markers
            std::size_t b = line.find_first_not_of(" \t");
            if (b == std::string::npos) continue;
            std::string& v = values[pending];
            v += (v.empty() ? "" : " ") + line.substr(b);
        }
    }
    return values;
}

CImportResult import_c_headers(const std::string& preprocessed, const CImportOptions& options) {
    CImportResult result;
    Lexed         lexed  = lex(preprocessed);
    auto          wanted = wanted_files(lexed, options);
    Parser parser(lexed, wanted);
    parser.run();
    std::vector<std::pair<std::string, std::string>>& report = result.skipped;
    report.insert(report.end(), parser.skipped.begin(), parser.skipped.end());
    Writer w{parser, wanted, report, {}};
    std::ostringstream out;
    out << "// Generated by riftc (g.import_c_headers) from:";
    for (const std::string& h : options.headers) out << " " << h;
    out << "\n// Do not edit: it is written again when the headers change.\nimport std;\n\n";
    if (!options.ns.empty()) out << "namespace " << options.ns << " {\n";
    std::string indent = options.ns.empty() ? "" : "    ";
    std::ostringstream source;   // the .rifts: the bodies of the union accessors

    // Enums: simple enums with C's underlying type (int).
    for (const std::string& name : parser.enum_order) {
        const CEnum& e = parser.enums.at(name);
        if (!wanted(e.file)) continue;
        if (name.starts_with("anonymous")) {   // `enum { A = 1 };`: constants
            for (const auto& [n, v] : e.values) out << indent << std::format("compile_time i32 {} = {};\n", safe_name(n), v);
            continue;
        }
        out << indent << "enum " << safe_name(name) << " : i32 {\n";
        for (const auto& [n, v] : e.values) out << indent << "    " << safe_name(n) << " = " << v << ",\n";
        out << indent << "}\n\n";
    }
    // Structs and unions. An opaque one (declared, never defined) is an empty struct: only its
    // pointers are used.
    std::set<std::string> written;
    auto emit_record = [&](const std::string& name) {
        written.insert(name);
        auto found = parser.records.find(name);
        if (found == parser.records.end() || !found->second.defined) {   // declared, never defined: only its pointers are used
            out << indent << "struct " << safe_name(name) << " { }\n\n";
            return;
        }
        const CRecord& r = found->second;
        bool bad = false;
        std::string why;
        std::ostringstream body;
        for (const CField& f : r.fields) {
            if (f.bitfield) {
                why = std::format("the bit field '{}'", f.name);
                bad = true;
                break;
            }
        }
        if (bad) {
            report.emplace_back(name, why + ": bit fields are not translated");
            out << indent << "// Not translated (" << why << "): an opaque struct.\n" << indent << "struct " << safe_name(name) << " { }\n\n";
            return;
        }
        if (r.is_union) {
            auto rt      = std::make_shared<CType>();
            rt->kind     = CType::Kind::record;
            rt->name     = name;
            rt->is_union = true;
            auto [size, align] = w.layout(rt);
            std::string cell = align >= 8 ? "u64" : align == 4 ? "u32" : align == 2 ? "u16" : "u8";
            std::uint64_t n  = size / std::max<std::uint64_t>(align, 1);
            body << indent << "// A C union: its members are read and written through `unsafe` methods.\n";
            source << indent << "struct " << safe_name(name) << " {\n";
            body << indent << "struct " << safe_name(name) << " {\n";
            body << indent << std::format("    std::raw::array<{}, {}> storage = default;\n", cell, std::max<std::uint64_t>(n, 1));
            for (const CField& f : r.fields) {
                std::string ft = w.type(f.type, why);
                if (ft.empty()) {
                    report.emplace_back(name + "." + f.name, why);
                    continue;
                }
                body << indent << std::format("    unsafe {} {}();\n", ft, safe_name(f.name));
                body << indent << std::format("    unsafe void set_{}({} value) mut;\n", f.name, ft);
                source << indent << std::format("    unsafe {} {}() {{ return std::raw::read(cast::reinterpret<std::raw_ptr<{}>>(&this.storage)); }}\n",
                                                ft, safe_name(f.name), ft);
                source << indent << std::format("    unsafe void set_{}({} value) mut {{ std::raw::write(cast::reinterpret<std::raw_ptr<mut {}>>(mut &this.storage), value); }}\n",
                                                f.name, ft, ft);
            }
            body << indent << "}\n\n";
            source << indent << "}\n\n";
            out << body.str();
            return;
        }
        body << indent << "struct " << safe_name(name) << " {\n";
        for (const CField& f : r.fields) {
            std::string ft = w.type(f.type, why);
            if (ft.empty()) {
                bad = true;
                why = std::format("field '{}': {}", f.name, why);
                break;
            }
            body << indent << std::format("    {} {} = {};\n", ft, safe_name(f.name), w.initial(f.type));
        }
        body << indent << "}\n\n";
        if (bad) {
            report.emplace_back(name, why);
            out << indent << "// Not translated (" << why << "): an opaque struct.\n" << indent << "struct " << safe_name(name) << " { }\n\n";
        } else {
            out << body.str();
        }
    };
    for (const std::string& name : parser.record_order) {
        const CRecord& r = parser.records.at(name);
        if (!name.starts_with("anonymous") && wanted(r.file)) emit_record(name);
    }
    // Typedefs: aliases. `typedef struct x x;` names the struct itself.
    for (const std::string& name : parser.typedef_order) {
        if (!wanted(parser.typedef_file.at(name)) || Writer::known_typedef(name)) continue;
        const CTypePtr& t = parser.typedefs.at(name);
        if ((t->kind == CType::Kind::record || t->kind == CType::Kind::enumeration) && t->name == name) continue;
        std::string why;
        std::string target;
        if (t->kind == CType::Kind::record && t->name.starts_with("anonymous")) {   // `typedef struct { ... } name;`
            const CRecord& r = parser.records.at(t->name);
            std::ostringstream body;
            bool bad = false;
            body << indent << "struct " << safe_name(name) << " {\n";
            for (const CField& f : r.fields) {
                std::string ft = f.bitfield ? "" : w.type(f.type, why);
                if (ft.empty()) {
                    bad = true;
                    why = f.bitfield ? "a bit field" : why;
                    break;
                }
                body << indent << std::format("    {} {} = {};\n", ft, safe_name(f.name), w.initial(f.type));
            }
            body << indent << "}\n\n";
            if (bad || r.is_union) {
                report.emplace_back(name, bad ? why : "an anonymous union");
                out << indent << "struct " << safe_name(name) << " { }\n\n";
            } else {
                out << body.str();
            }
            continue;
        }
        if (t->kind == CType::Kind::enumeration && t->name.starts_with("anonymous")) {
            out << indent << std::format("compile_time type {} = i32;\n", safe_name(name));
            continue;
        }
        target = t->kind == CType::Kind::function ? w.function_pointer(t, why) : w.type(t, why);
        if (target.empty()) {
            report.emplace_back(name, why);
            continue;
        }
        out << indent << std::format("compile_time type {} = {};\n", safe_name(name), target);
    }
    // #defines of numbers and strings: constants.
    std::map<std::string, std::int64_t> known = parser.enum_constants;
    auto cast_type = [&](const std::string& text) -> std::string {
        Lexed words = lex(text);
        std::vector<std::string> ws;
        for (const CToken& t : words.tokens) {
            if (t.kind == CToken::Kind::identifier) ws.push_back(t.text);
        }
        CTypePtr t;
        if (ws.size() == 1 && (parser.typedefs.contains(ws[0]) || Writer::known_typedef(ws[0]))) {
            t       = std::make_shared<CType>();
            t->kind = CType::Kind::named;
            t->name = ws[0];
            // Through the typedefs to the number type: `(XML_Bool)1` is a u8.
            while (t->kind == CType::Kind::named && !Writer::known_typedef(t->name) && parser.typedefs.contains(t->name)) t = parser.typedefs.at(t->name);
        } else {   // `unsigned long`, `int`, `double`, ...
            int  longs = 0;
            bool u = false, ch = false, sh = false, fl = false, db = false;
            for (const std::string& x : ws) {
                if (x == "unsigned") u = true;
                else if (x == "long") ++longs;
                else if (x == "char") ch = true;
                else if (x == "short") sh = true;
                else if (x == "float") fl = true;
                else if (x == "double") db = true;
                else if (x != "signed" && x != "int" && x != "const") return "";
            }
            if (fl) return "f32";
            if (db) return longs ? "" : "f64";
            return std::string(u ? "u" : "i") + (ch ? "8" : sh ? "16" : longs ? "64" : "32");
        }
        std::string why;
        std::string r = w.type(t, why);
        static const std::set<std::string> numbers = {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "isize", "usize", "f32", "f64", "bool"};
        return numbers.contains(r) ? r : "";
    };
    for (const auto& [name, body, file] : lexed.defines) {
        if (!wanted(file) || name.starts_with("__")) continue;
        std::string type;
        auto        expanded = options.macro_values.find(name);   // the body with its macros expanded (c_macro_values)
        std::string value    = expanded != options.macro_values.end() ? literal_constant(join_strings(expanded->second), known, type, cast_type) : "";
        if (value.empty()) value = literal_constant(body, known, type, cast_type);   // e.g. #undef'd by the end
        if (value.empty()) {
            if (!body.empty()) report.emplace_back(name, "a #define that is not a number or string literal");
            continue;
        }
        if (type.starts_with("i") || type.starts_with("u") || type == "isize" || type == "usize") {
            known[name] = type.starts_with("u") ? static_cast<std::int64_t>(std::stoull(value)) : std::stoll(value);
        }
        out << indent << std::format("compile_time {} {} = {};\n", type, safe_name(name), value);
    }
    for (const auto& [name, file] : lexed.function_macros) {
        if (wanted(file) && !name.starts_with("__")) report.emplace_back(name, "a function-like macro");
    }
    // Functions: extern declarations.
    std::ostringstream fns;
    for (const auto& [name, f] : parser.functions) {
        if (!wanted(parser.function_file.at(name))) continue;
        std::string why;
        if (f->variadic) {
            report.emplace_back(name, "a variadic function");
            continue;
        }
        if (rift_keywords().contains(name)) {   // C's symbol name can't be changed
            report.emplace_back(name, "its name is a Rift keyword");
            continue;
        }
        std::string r = w.type(f->inner, why);
        if (r.empty()) {
            report.emplace_back(name, why);
            continue;
        }
        std::string line = std::format("{} {}(", r, name);
        bool        bad  = false;
        for (std::size_t i = 0; i < f->params.size(); ++i) {
            std::string p = w.type(f->params[i], why);
            if (p.empty()) {
                bad = true;
                break;
            }
            std::string pname = i < f->param_names.size() ? f->param_names[i] : "";
            bool        clash = pname.empty() || pname.starts_with("__") || parser.typedefs.contains(pname) ||
                         parser.records.contains(pname) || parser.enums.contains(pname) || parser.enum_constants.contains(pname);
            for (std::size_t j = 0; j < i && !clash; ++j) clash = f->param_names[j] == pname;
            line += std::format("{}{} {}", i ? ", " : "", p, clash ? std::format("p{}", i) : safe_name(pname));
        }
        if (bad) {
            report.emplace_back(name, why);
            continue;
        }
        fns << indent << "    " << line << ");\n";
        ++result.functions;
    }
    // The structs of other headers (system headers) that the declarations use.
    while (true) {
        std::vector<std::string> missing;
        for (const std::string& n : w.needed) {
            if (!written.contains(n)) missing.push_back(n);
        }
        if (missing.empty()) break;
        out << "\n";
        for (const std::string& n : missing) emit_record(n);
    }
    if (!fns.str().empty()) out << "\n" << indent << "extern(std::build::language::c) {\n" << fns.str() << indent << "}\n";
    if (!options.ns.empty()) out << "}\n";
    result.header = out.str();
    result.source = "// Generated by riftc (g.import_c_headers): the accessors of the C unions.\n";
    if (!source.str().empty()) {
        result.source += options.ns.empty() ? source.str() : "namespace " + options.ns + " {\n" + source.str() + "}\n";
    }
    return result;
}

} // namespace rift
