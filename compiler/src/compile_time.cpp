#include "edax/llvm_backend.hpp"
#include "edax/compile_time.hpp"

#include "edax/ast_printer.hpp"
#include "edax/source.hpp"

#include <bit>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <regex>
#include <unistd.h>
#include <algorithm>
#include <format>

#ifndef EDAX_VERSION
#define EDAX_VERSION "0.0.0-dev"
#endif

namespace edax::ct {

using namespace ast;
using sema::Symbol;
using sema::SymbolKind;
using types::Kind;
using types::Type;

namespace {

u128 mask(u128 v, int bits) { return bits >= 128 ? v : v & ((u128{1} << bits) - 1); }

i128 signed_min(int bits) { return bits >= 128 ? static_cast<i128>(u128{1} << 127) : -(static_cast<i128>(1) << (bits - 1)); }
i128 signed_max(int bits) { return bits >= 128 ? static_cast<i128>(~u128{0} >> 1) : (static_cast<i128>(1) << (bits - 1)) - 1; }
u128 unsigned_max(int bits) { return bits >= 128 ? ~u128{0} : (u128{1} << bits) - 1; }

double round_f32(double v) { return static_cast<double>(static_cast<float>(v)); }
// f16 and f32 values are kept as doubles, rounded to their width after each operation (spec 34).
double round_float(double v, int bits) {
    if (bits == 16) return static_cast<double>(static_cast<_Float16>(v));
    return bits == 32 ? round_f32(v) : v;
}

std::string signed_text(i128 v) {
    if (v >= 0) return edax::to_string(static_cast<u128>(v));
    return "-" + edax::to_string(static_cast<u128>(-(v + 1)) + 1);
}

// Thrown to abandon an evaluation: after an error was reported, or (unknown) silently.
struct Stop {
    bool unknown = false;
};

enum class Flow { normal, break_, continue_, return_ };

const FunctionDecl* body_decl(const Symbol* s) {
    const Decl* d = s->definition ? s->definition : s->decl;
    return d && d->kind == DeclKind::function ? &d->as<FunctionDecl>() : nullptr;
}

} // namespace

// --- values --------------------------------------------------------------------------------

Value Value::of_integer(u128 bits, const Type* t) {
    Value v;
    v.kind    = Kind::integer;
    v.type    = t;
    v.integer = mask(bits, t && t->is_integer() ? t->bits : 128);
    return v;
}

Value Value::of_bool(bool b, const Type* t) {
    Value v;
    v.kind    = Kind::boolean;
    v.type    = t;
    v.boolean = b;
    return v;
}

Value Value::of_type(const Type* denoted, const Type* type_type) {
    Value v;
    v.kind       = Kind::type;
    v.type       = type_type;
    v.type_value = denoted;
    return v;
}

i128 Value::as_signed() const {
    int bits = type && type->is_integer() ? type->bits : 128;
    if (!type || !type->is_signed || bits >= 128) return static_cast<i128>(integer);
    u128 sign = u128{1} << (bits - 1);
    return (integer & sign) ? static_cast<i128>(integer | ~unsigned_max(bits)) : static_cast<i128>(integer);
}

std::string to_string(const Value& v) {
    switch (v.kind) {
    case Value::Kind::void_: return "()";
    case Value::Kind::integer:
        return v.type && v.type->is_signed ? signed_text(v.as_signed()) : edax::to_string(v.integer);
    case Value::Kind::floating: return std::format("{}", v.floating);
    case Value::Kind::boolean: return v.boolean ? "true" : "false";
    case Value::Kind::character: {
        std::string s;
        encode_utf8(v.character, s);
        return "'" + s + "'";
    }
    case Value::Kind::string: return "\"" + v.string + "\"";
    case Value::Kind::type: return types::to_string(v.type_value);
    case Value::Kind::aggregate: {
        std::string out = types::to_string(v.type) + "{";
        std::size_t i   = 0;
        if (v.type && v.type->is(Kind::user)) {
            for (const Symbol* f : v.type->symbol->member_order) {
                if (f->kind != SymbolKind::field || f->is_static || i >= v.elements.size()) continue;
                out += std::format("{} .{} = {}", i ? "," : "", f->name, to_string(v.elements[i]));
                ++i;
            }
        }
        return out + " }";
    }
    case Value::Kind::enumeration: {
        std::string out = types::to_string(v.type) + "::" + (v.symbol ? v.symbol->name : "?");
        if (!v.elements.empty()) out += "(" + to_string(v.elements[0]) + ")";
        return out;
    }
    case Value::Kind::list: {
        std::string out = "[";
        for (std::size_t i = 0; i < v.elements.size(); ++i) out += (i ? ", " : "") + to_string(v.elements[i]);
        return out + "]";
    }
    case Value::Kind::field: return std::format("field '{}' of {}", v.symbol->name, types::to_string(v.type_value));
    case Value::Kind::alternative: return std::format("alternative '{}' of {}", v.symbol->name, types::to_string(v.type_value));
    case Value::Kind::function: return std::format("method '{}' of {}", v.symbol->name, types::to_string(v.type_value));
    case Value::Kind::parameter: return std::format("parameter {} of '{}'", v.index, v.symbol->name);
    case Value::Kind::file: return std::format("file '{}'", v.string);
    }
    return "?";
}

bool equal(const Value& a, const Value& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
    case Value::Kind::void_: return true;
    case Value::Kind::integer: return a.integer == b.integer;
    case Value::Kind::floating: return a.floating == b.floating;
    case Value::Kind::boolean: return a.boolean == b.boolean;
    case Value::Kind::character: return a.character == b.character;
    case Value::Kind::string: return a.string == b.string;
    case Value::Kind::type: return a.type_value == b.type_value;
    case Value::Kind::field:
    case Value::Kind::alternative:
    case Value::Kind::function: return a.symbol == b.symbol && a.type_value == b.type_value;
    case Value::Kind::parameter: return a.symbol == b.symbol && a.index == b.index;
    case Value::Kind::file: return a.string == b.string;
    case Value::Kind::enumeration:
        if (!a.symbol || !b.symbol || a.symbol->name != b.symbol->name) return false;
        [[fallthrough]];
    case Value::Kind::aggregate:
    case Value::Kind::list:
        if (a.elements.size() != b.elements.size()) return false;
        for (std::size_t i = 0; i < a.elements.size(); ++i) {
            if (!equal(a.elements[i], b.elements[i])) return false;
        }
        return true;
    }
    return false;
}

std::uint64_t interface_hash(const Symbol* iface) {
    std::string text = (iface->ns.empty() ? "" : iface->ns + "::") + iface->name;
    for (const Symbol* m : iface->member_order) {
        const FunctionDecl* f = m->function();
        if (!f) continue;
        text += "|" + m->name + "(";
        for (const ast::Param& p : f->params) text += ast::to_string(*p.type) + ",";
        text += ")" + (f->result ? ast::to_string(*f->result) : std::string("void")) + (f->is_mut ? " mut" : "");
    }
    std::uint64_t h = 1469598103934665603ull;   // FNV-1a
    for (char ch : text) h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ull;
    return h;
}

std::string builtin_name(const CallExpr& c) {
    const Expr& callee = *c.callee;
    if (callee.kind != ExprKind::path) return {};
    const auto& pe = callee.as<PathExpr>();
    if (pe.symbol) return {};
    const auto& segs = pe.path.segments;
    if (segs.size() == 2 && segs[0].name == "std") {
        const std::string& n = segs[1].name;
        if (n == "is_integer" || n == "is_float" || n == "is_same" || n == "implements" || n == "size_of" || n == "align_of" || n == "type_name" ||
            n == "compile_error" || n == "compile_warning" || n == "compile_note" || n == "is_compile_time" || n == "location_of" ||
            n == "literal_text" || n == "embed" || n == "embed_text") {
            return n;
        }
    }
    if (segs.size() == 3 && segs[0].name == "std" && segs[1].name == "meta") {
        const std::string& n = segs[2].name;
        if (n == "fields" || n == "alternatives" || n == "is_struct" || n == "is_class" || n == "is_enum" || n == "result_of" ||
            n == "is_view" || n == "methods" || n == "interfaces" || n == "template_args" || n == "annotation" || n == "has_annotation" ||
            n == "function_annotation" || n == "type_annotation" || n == "file_annotation" || n == "functions_with" ||
            n == "types_with" || n == "interface_hash" || n == "compiler_version" || n == "type_id") {
            return "meta::" + n;
        }
    }
    return {};
}

std::string raw_intrinsic(const CallExpr& c) {
    const Expr& callee = *c.callee;
    if (callee.kind != ExprKind::path) return {};
    const auto& pe = callee.as<PathExpr>();
    if (pe.symbol) return {};
    const auto& segs = pe.path.segments;
    if (segs.size() == 2 && segs[0].name == "std" && segs[1].name == "bit_cast") return "bit_cast";   // spec 34, safe
    if (segs.size() == 3 && segs[0].name == "std" && segs[1].name == "raw") {
        const std::string& n = segs[2].name;
        if (n == "assume" || n == "unreachable") return n;   // spec 34
        if (n == "allocate" || n == "reallocate" || n == "free" || n == "read" || n == "write" || n == "drop" || n == "copy" ||
            n == "str_from" || n == "static_str_from" || n.starts_with("atomic_") || n == "futex_wait" || n == "futex_wait_for" || n == "futex_wake" || n == "spin_hint") {
            return n;
        }
    }
    return {};
}

// --- the interpreter -----------------------------------------------------------------------

struct Evaluator::Impl {
    struct Frame {
        const Symbol*                  function = nullptr;
        Span                           call_site;
        std::map<const Symbol*, Value> locals;
        const Type*                    result   = nullptr;
        std::optional<Value>           self;
    };

    sema::Program&      program;
    DiagnosticEngine&   diags;
    Hooks               hooks;
    types::TypeContext& tc;
    std::vector<Frame>  frames;
    std::size_t         steps = 0;
    Value               returned;
    std::string         why;   // what the evaluation is for, for messages
    bool                quiet = false;               // report nothing (Evaluator::Context::quiet)
    std::optional<Span> site;                        // where compile_error/warning report
    std::function<std::shared_ptr<const Value>(const Symbol*)> parameter;   // entry checks: the call's arguments
    std::function<bool(const Symbol*)>                         parameter_known;
    std::function<std::optional<Span>(const Symbol*)>          parameter_site;
    std::function<std::optional<std::string>(const Symbol*)>   parameter_text;
    std::vector<Note>   pending_notes;               // std::compile_note, for the next error or warning

    static constexpr std::size_t max_steps = 2'000'000;
    static constexpr std::size_t max_depth = 256;

    Impl(sema::Program& p, DiagnosticEngine& d, Hooks h) : program(p), diags(d), hooks(std::move(h)), tc(*p.types) {}

    // --- errors ------------------------------------------------------------------------

    [[noreturn]] void unknown() { throw Stop{true}; }

    // "while running 'f' at compile time" for the innermost calls, then the outermost one.
    void call_notes(Diagnostic& d) {
        std::vector<const Frame*> calls;
        for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
            if (it->function) calls.push_back(&*it);
        }
        constexpr std::size_t shown = 4;
        for (std::size_t i = 0; i < calls.size(); ++i) {
            if (calls.size() > shown + 1 && i == shown) {
                d.notes.push_back(Note{std::format("... {} more calls ...", calls.size() - shown - 1), std::nullopt});
                i = calls.size() - 1;
            }
            d.notes.push_back(Note{std::format("while running '{}' at compile time", calls[i]->function->name), calls[i]->call_site});
        }
    }

    [[noreturn]] void not_constant(Span span, std::string message, std::string note = {}) {
        if (quiet) throw Stop{};
        Diagnostic& d = diags.report(DiagCode::E4200, span, std::move(message), "not known at compile time");
        if (!note.empty()) d.notes.push_back(Note{std::move(note), std::nullopt});
        if (!why.empty()) d.notes.push_back(Note{why, std::nullopt});
        call_notes(d);
        throw Stop{};
    }

    [[noreturn]] void failed(Span span, std::string message, std::string label) {
        if (quiet) throw Stop{};
        Diagnostic& d = diags.report(DiagCode::E4201, span, std::move(message), std::move(label));
        if (!why.empty()) d.notes.push_back(Note{why, std::nullopt});
        call_notes(d);
        throw Stop{};
    }

    void step(Span span) {
        if (++steps > max_steps) {
            failed(span, std::format("compile-time evaluation took more than {} steps", max_steps), "still running here");
        }
    }

    // --- types -------------------------------------------------------------------------

    const Type* lower(const TypeExpr& t) {
        bool        top = frames.empty() || !frames.back().function;
        const Type* r   = top ? hooks.lower(t) : tc.lower(t, sema::substitution_of(frames.back().function));
        if (!r || r->is_unknown()) unknown();
        return r;
    }

    const Type* type_of(const Expr& e) {
        const Type* t = e.type ? types::strip_ref(e.type) : nullptr;
        if (!t || t->is_unknown() || t->is_untyped()) unknown();
        return t;
    }

    const Type* usize() { return tc.builtin("usize"); }

    Value string_value(std::string s) {
        Value v;
        v.kind   = Value::Kind::string;
        v.type   = tc.static_str();
        v.string = std::move(s);
        return v;
    }

    Value integer(u128 bits, const Type* t) { return Value::of_integer(bits, t); }

    // The type a type symbol names in a path (`point`, `T`, `box<u32>`).
    const Type* type_of_symbol(const Symbol* s, const Path& path) {
        if (s->kind == SymbolKind::type_param) {
            if (frames.empty() || !frames.back().function) {
                const Type* t = hooks.substitute(s);
                if (!t || t->is_unknown()) unknown();
                return t;
            }
            if (const auto* subst = sema::substitution_of(frames.back().function)) {
                if (auto it = subst->find(s); it != subst->end()) return it->second;
            }
            unknown();
        }
        if (s->decl && s->decl->is_template && !s->template_of) {
            const PathSegment& seg = path.segments.back();
            std::vector<const Type*> args;
            for (const TemplateArg& a : seg.template_args) {
                if (!a.type) unknown();
                args.push_back(lower(*a.type));
            }
            const Type* t = tc.instance(s, args);
            if (t->is_unknown()) unknown();
            return t;
        }
        return tc.user(s);
    }

    // --- expressions ---------------------------------------------------------------------

    Value eval(const Expr& e) {
        step(e.span);
        if (e.constant) return *e.constant;
        switch (e.kind) {
        case ExprKind::int_literal: {
            const Type* t = type_of(e);
            if (!t->is_integer()) unknown();
            return integer(e.as<LiteralExpr>().int_value, t);
        }
        case ExprKind::float_literal: {
            const Type* t   = type_of(e);
            std::string txt = e.as<LiteralExpr>().text;
            std::string digits;
            for (char ch : txt) {
                if (ch == '_') continue;
                if (ch == 'f' && !digits.empty()) break;   // the suffix
                digits += ch;
            }
            Value v;
            v.kind     = Value::Kind::floating;
            v.type     = t;
            v.floating = std::stod(digits);
            if (t->bits <= 32) v.floating = round_float(v.floating, t->bits);
            return v;
        }
        case ExprKind::bool_literal: return Value::of_bool(e.as<LiteralExpr>().bool_value, tc.bool_());
        case ExprKind::char_literal: {
            const std::string& text = e.as<LiteralExpr>().value;
            char32_t cp = 0;
            if (!text.empty() && decode_utf8(text, 0, cp) == 0) cp = static_cast<unsigned char>(text[0]);
            Value v;
            v.kind      = Value::Kind::character;
            v.type      = tc.char_();
            v.character = cp;
            return v;
        }
        case ExprKind::string_literal: {
            const auto& lit = e.as<LiteralExpr>();
            if (lit.token_kind == TokenKind::c_string_literal) not_constant(e.span, "a C string is a pointer, not known at compile time");
            if (lit.token_kind == TokenKind::format_string_literal) {
                std::string out;
                for (const FormatPart& part : lit.format_parts) {
                    out += part.expr ? format(eval(*part.expr), part.spec, part.expr->span) : part.text;
                }
                return string_value(std::move(out));
            }
            return string_value(lit.value);
        }
        case ExprKind::path: return path_value(e.as<PathExpr>());
        case ExprKind::this_:
            if (frames.empty() || !frames.back().self) not_constant(e.span, "'this' is not known at compile time");
            return *frames.back().self;
        case ExprKind::unary: return unary(e.as<UnaryExpr>());
        case ExprKind::binary: return binary(e.as<BinaryExpr>());
        case ExprKind::call: return call(e.as<CallExpr>());
        case ExprKind::member: return member(e.as<MemberExpr>());
        case ExprKind::index: {
            const auto& ix = e.as<IndexExpr>();
            Value       whole = eval(*ix.object);
            Value       i  = eval(*ix.index);
            const Value& o = elements_of(whole);   // a std::array: its items
            if (o.kind != Value::Kind::list) not_constant(e.span, "only lists can be indexed at compile time");
            if (i.integer >= o.elements.size()) {
                failed(ix.index->span, std::format("index {} is out of bounds for a list of {} elements", edax::to_string(i.integer),
                                                   o.elements.size()),
                       "out of bounds");
            }
            return o.elements[static_cast<std::size_t>(i.integer)];
        }
        case ExprKind::cast: return cast(e.as<CastExpr>());
        case ExprKind::bit: {
            const Type* t = type_of(e);
            Value       n = eval(*e.as<BitExpr>().operand);
            if (!t->is_integer() || n.integer >= static_cast<u128>(t->bits)) {
                failed(e.span, std::format("bit({}) is out of range for {}", edax::to_string(n.integer), types::to_string(t)),
                       "out of range");
            }
            return integer(u128{1} << static_cast<int>(n.integer), t);
        }
        case ExprKind::struct_literal: return struct_literal(e.as<StructLiteralExpr>());
        case ExprKind::tuple: {   // an aggregate with the elements in order
            Value v;
            v.kind = Value::Kind::aggregate;
            v.type = type_of(e);
            for (const ExprPtr& x : e.as<TupleExpr>().elements) v.elements.push_back(eval(*x));
            return v;
        }
        case ExprKind::list: {   // a list; as a std::array, the aggregate holding it
            Value list;
            list.kind = Value::Kind::list;
            const Type* t = type_of(e);
            list.type = t;
            for (const ExprPtr& x : e.as<ListExpr>().elements) list.elements.push_back(eval(*x));
            if (t->is(Kind::user) && t->symbol->template_of && t->symbol->template_of->name == "array" &&
                t->symbol->ns == "std") {
                Value agg;
                agg.kind = Value::Kind::aggregate;
                agg.type = t;
                list.type = t->symbol->template_args.size() == 2 ? tc.array(t->symbol->template_args[0], t->symbol->template_args[1]) : t;
                agg.elements.push_back(std::move(list));
                return agg;
            }
            return list;
        }
        case ExprKind::default_: return default_of(type_of(e), e.span);
        case ExprKind::block: return block(e.as<BlockExpr>());
        case ExprKind::if_: return if_value(e.as<IfExpr>());
        case ExprKind::match: return match_value(e.as<MatchExpr>());
        case ExprKind::while_: {
            const auto& w = e.as<WhileExpr>();
            while (truth(eval(*w.condition))) {
                Flow f = run_block(*w.body);
                if (f == Flow::break_) break;
                if (f == Flow::return_) throw f;
            }
            return Value{};
        }
        case ExprKind::for_: return for_loop(e.as<ForExpr>());
        case ExprKind::return_: {
            const auto& r = e.as<ReturnExpr>();
            returned      = r.value ? eval(*r.value) : Value{};
            throw Flow::return_;
        }
        case ExprKind::break_: throw Flow::break_;
        case ExprKind::continue_: throw Flow::continue_;
        case ExprKind::try_: {
            const auto& t = e.as<TryExpr>();
            Value       v = eval(*t.operand);
            if (v.kind != Value::Kind::enumeration) not_constant(e.span, "'?' needs a std::option or std::result here");
            if (v.symbol->name == "ok" || v.symbol->name == "some") return v.elements.at(0);
            if (frames.empty() || !frames.back().result || !frames.back().result->is(Kind::user)) {
                not_constant(e.span, "'?' can only return from a compile_time function");
            }
            const Type* r = frames.back().result;
            Value       out;
            out.kind   = Value::Kind::enumeration;
            out.type   = r;
            out.symbol = r->symbol->members.at(v.symbol->name).front();
            out.elements = v.elements;
            returned     = out;
            throw Flow::return_;
        }
        case ExprKind::range: not_constant(e.span, "a range is only a value in a 'for' loop");
        case ExprKind::nullptr_literal: not_constant(e.span, "pointers do not exist at compile time");
        case ExprKind::lambda: not_constant(e.span, "lambdas cannot run at compile time yet");
        case ExprKind::error: unknown();
        }
        unknown();
    }

    bool truth(const Value& v) {
        if (v.kind == Value::Kind::boolean) return v.boolean;
        if (v.kind == Value::Kind::integer) return v.integer != 0;
        unknown();
    }

    Value path_value(const PathExpr& pe) {
        const Symbol* s = pe.symbol;
        if (!s) {
            if (pe.path.segments.size() == 1) {
                if (const Type* b = tc.builtin(pe.path.segments[0].name)) return Value::of_type(b, tc.type_value());
            }
            // `std::fn_ptr<R(A...)>`, `std::function<R(A...)>`, `std::c_library::function<R(A...)>`:
            // built-in callable types (`compile_time type visitor = std::fn_ptr<...>;`).
            const auto& segs = pe.path.segments;
            if (!segs.empty() && segs[0].name == "std" && segs.back().template_args.size() == 1 && segs.back().template_args[0].type &&
                segs.back().template_args[0].type->kind == ast::TypeKind::function &&
                ((segs.size() == 2 && (segs[1].name == "fn_ptr" || segs[1].name == "function")) ||
                 (segs.size() == 3 && segs[1].name == "c_library" && segs[2].name == "function"))) {
                const auto& f = segs.back().template_args[0].type->as<ast::FunctionType>();
                if (segs.size() == 3) return Value::of_type(lower(f), tc.type_value());
                std::vector<const Type*> params;
                for (const auto& p : f.params) params.push_back(lower(*p));
                const Type* result = f.result ? lower(*f.result) : tc.void_();
                return Value::of_type(tc.callable(segs[1].name == "function" ? Kind::function : Kind::fn_ptr, std::move(params), result),
                                      tc.type_value());
            }
            not_constant(pe.span, std::format("'{}' is not known at compile time", ast::to_string(pe.path)));
        }
        switch (s->kind) {
        case SymbolKind::local: {
            for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
                if (auto found = it->locals.find(s); found != it->locals.end()) return found->second;
                break;   // locals of other frames are not visible
            }
            if (parameter && frames.size() == 1 && s->local_kind != sema::LocalKind::variable &&
                s->local_kind != sema::LocalKind::binding && s->local_kind != sema::LocalKind::loop_variable) {
                if (auto v = parameter(s)) {
                    frames.back().locals[s] = *v;
                    return *v;
                }
                throw Stop{};   // reported at the argument (or quiet)
            }
            if (s->ct_value) return *s->ct_value;
            if (auto v = hooks.constant_value(s)) return *v;
            if (hooks.depends_on_template && hooks.depends_on_template(s)) unknown();
            not_constant(pe.span, std::format("'{}' is not a compile-time value", s->name),
                         "only literals, compile_time constants and the results of compile_time functions are known "
                         "at compile time; declare it 'compile_time ...'");
        }
        case SymbolKind::global: {
            if (s->ct_value) return *s->ct_value;
            if (auto v = hooks.constant_value(s)) return *v;
            not_constant(pe.span, std::format("'{}' is not a compile-time value", s->name),
                         "a 'static' variable is initialized at run time; use 'compile_time' for a constant");
        }
        case SymbolKind::alternative: {
            Value v;
            v.kind   = Value::Kind::enumeration;
            v.type   = type_of(pe);
            v.symbol = s;
            if (!s->parent || !s->parent->decl) unknown();
            return v;
        }
        case SymbolKind::struct_:
        case SymbolKind::class_:
        case SymbolKind::enum_:
        case SymbolKind::interface:
        case SymbolKind::type_param: return Value::of_type(type_of_symbol(s, pe.path), tc.type_value());
        case SymbolKind::value_param: {   // `N` of `template<usize N>`
            const Type* arg = nullptr;
            if (!frames.empty() && frames.back().function) {
                if (const auto* subst = sema::substitution_of(frames.back().function)) {
                    if (auto it = subst->find(s); it != subst->end()) arg = it->second;
                }
            } else {
                arg = hooks.substitute(s);
            }
            if (!arg || !arg->is(Kind::value_arg)) unknown();
            if (std::shared_ptr<const void> rich = tc.value_payload(arg)) return *std::static_pointer_cast<const Value>(rich);
            if (arg->inner->is(Kind::bool_)) return Value::of_bool(tc.value_bits(arg) != 0, arg->inner);
            return integer(tc.value_bits(arg), arg->inner);
        }
        default:
            not_constant(pe.span, std::format("'{}' is not a compile-time value", ast::to_string(pe.path)));
        }
    }

    Value unary(const UnaryExpr& u) {
        if (u.distinct_result) {   // `-d` on a distinct type (spec 34)
            Value v = eval(*u.operand);
            if (v.kind == Value::Kind::floating) v.floating = -v.floating;
            else if (v.kind == Value::Kind::integer) v = integer(u128{0} - v.integer, v.type);
            else unknown();
            Value out;
            out.kind = Value::Kind::aggregate;
            out.type = u.distinct_result;
            out.elements.push_back(std::move(v));
            return out;
        }
        switch (u.op) {
        case UnaryOp::move:
        case UnaryOp::mut_marker: return eval(*u.operand);
        case UnaryOp::logical_not: return Value::of_bool(!truth(eval(*u.operand)), tc.bool_());
        case UnaryOp::bit_not: {
            Value v = eval(*u.operand);
            return integer(~v.integer, v.type);
        }
        case UnaryOp::negate: {
            const Type* t = type_of(u);
            if (t->is_float()) {
                Value v = eval(*u.operand);
                v.floating = -v.floating;
                return v;
            }
            if (!t->is_integer()) unknown();
            if (u.operand->kind == ExprKind::int_literal) {   // `-128i8`: the literal is checked with its sign
                return integer(u128{0} - u.operand->as<LiteralExpr>().int_value, t);
            }
            Value v = eval(*u.operand);
            if (!t->is_signed) failed(u.span, "cannot negate an unsigned value", "unsigned");
            if (v.as_signed() == signed_min(t->bits)) {
                failed(u.span, std::format("-({}) overflows {}", to_string(v), types::to_string(t)), "overflow");
            }
            return integer(static_cast<u128>(-v.as_signed()), t);
        }
        default: not_constant(u.span, "pointers and addresses do not exist at compile time");
        }
    }

    Value binary(const BinaryExpr& b) {
        TokenKind op = b.op;
        if (op == TokenKind::amp_amp || op == TokenKind::pipe_pipe) {
            bool l = truth(eval(*b.lhs));
            if (op == TokenKind::amp_amp ? !l : l) return Value::of_bool(l, tc.bool_());
            return Value::of_bool(truth(eval(*b.rhs)), tc.bool_());
        }
        if (b.op_symbol) return user_operator(b);
        Value l = eval(*b.lhs);
        Value r = eval(*b.rhs);
        Value v = apply(op, l, r, b.span, b.rhs->span);
        if (b.distinct_result) {   // distinct-type arithmetic (spec 34): wrap the underlying result
            Value out;
            out.kind = Value::Kind::aggregate;
            out.type = b.distinct_result;
            out.elements.push_back(std::move(v));
            return out;
        }
        return v;
    }

    // `a < b`, `a == b`, `a + b` with a user-defined operator (spec 16): its body runs, like
    // any compile_time function. `<`, `<=`, `>`, `>=` compare the result of `operator<=>` with
    // `equal`; `!=` negates `operator==`; a defaulted `==` compares field by field.
    Value user_operator(const BinaryExpr& b) {
        const Symbol*       s = b.op_symbol;
        const FunctionDecl* f = body_decl(s);
        Value               l = eval(*b.lhs);
        Value               r = eval(*b.rhs);
        Value               result;
        if (f && f->is_defaulted) {
            result = Value::of_bool(equal(l, r), tc.bool_());
        } else {
            if (!f || !f->is_compile_time) {
                std::string owner = s->parent ? s->parent->name : std::string("its type");
                not_constant(b.span, std::format("'operator{}' of '{}' is not a compile_time function", s->name, owner),
                             "mark it 'compile_time run_time' to use it in compile-time code (checks, compile_time constants)");
            }
            bool method = s->kind == SymbolKind::method && !f->is_static;
            result = method ? run_values(s, l, {r}, b.span) : run_values(s, std::nullopt, {l, r}, b.span);
        }
        switch (b.op) {
        case TokenKind::bang_eq: return Value::of_bool(!truth(result), tc.bool_());
        case TokenKind::less: return Value::of_bool(ordering_sign(result, b.span) < 0, tc.bool_());
        case TokenKind::less_eq: return Value::of_bool(ordering_sign(result, b.span) <= 0, tc.bool_());
        case TokenKind::greater: return Value::of_bool(ordering_sign(result, b.span) > 0, tc.bool_());
        case TokenKind::greater_eq: return Value::of_bool(ordering_sign(result, b.span) >= 0, tc.bool_());
        default: return result;
        }
    }

    // -1, 0 or 1 for a std::ordering value.
    int ordering_sign(const Value& v, Span span) {
        if (v.kind == Value::Kind::enumeration && v.symbol) {
            if (v.symbol->name == "less") return -1;
            if (v.symbol->name == "equal") return 0;
            if (v.symbol->name == "greater") return 1;
        }
        if (v.kind == Value::Kind::integer) return v.as_signed() < 0 ? -1 : v.as_signed() > 0 ? 1 : 0;
        not_constant(span, "'operator<=>' must return a std::ordering");
    }

    // The std::ordering alternative for `sign`.
    Value ordering_of(int sign, Span span) {
        const Symbol* ordering = program.std_module ? program.std_module->item("std::ordering") : nullptr;
        if (!ordering) not_constant(span, "std::ordering is not available");
        std::string_view name = sign < 0 ? "less" : sign > 0 ? "greater" : "equal";
        auto it = ordering->members.find(std::string(name));
        if (it == ordering->members.end() || it->second.empty()) not_constant(span, "std::ordering is not available");
        Value v;
        v.kind   = Value::Kind::enumeration;
        v.type   = tc.user(ordering);
        v.symbol = it->second.front();
        return v;
    }

    Value apply(TokenKind op, const Value& l, const Value& r, Span span, Span rhs_span) {
        if (op == TokenKind::eq_eq) return Value::of_bool(equal(l, r), tc.bool_());
        if (op == TokenKind::bang_eq) return Value::of_bool(!equal(l, r), tc.bool_());
        if (op == TokenKind::spaceship) {   // numbers, chars and text (byte order)
            int sign = 0;
            if (l.kind == Value::Kind::floating) sign = l.floating < r.floating ? -1 : l.floating > r.floating ? 1 : 0;
            else if (l.kind == Value::Kind::character) sign = l.character < r.character ? -1 : l.character > r.character ? 1 : 0;
            else if (l.kind == Value::Kind::string) sign = l.string < r.string ? -1 : l.string > r.string ? 1 : 0;
            else if (l.kind == Value::Kind::integer && l.type && l.type->is_signed) sign = l.as_signed() < r.as_signed() ? -1 : l.as_signed() > r.as_signed() ? 1 : 0;
            else if (l.kind == Value::Kind::integer) sign = l.integer < r.integer ? -1 : l.integer > r.integer ? 1 : 0;
            else unknown();
            return ordering_of(sign, span);
        }
        if (l.kind == Value::Kind::floating) {
            double a = l.floating, c = r.floating, x = 0;
            switch (op) {
            case TokenKind::less: return Value::of_bool(a < c, tc.bool_());
            case TokenKind::less_eq: return Value::of_bool(a <= c, tc.bool_());
            case TokenKind::greater: return Value::of_bool(a > c, tc.bool_());
            case TokenKind::greater_eq: return Value::of_bool(a >= c, tc.bool_());
            case TokenKind::plus: x = a + c; break;
            case TokenKind::minus: x = a - c; break;
            case TokenKind::star: x = a * c; break;
            case TokenKind::slash: x = a / c; break;
            case TokenKind::percent: x = std::fmod(a, c); break;
            default: unknown();
            }
            Value v = l;
            v.floating = l.type && l.type->bits <= 32 ? round_float(x, l.type->bits) : x;
            return v;
        }
        if (l.kind == Value::Kind::character) {
            switch (op) {
            case TokenKind::less: return Value::of_bool(l.character < r.character, tc.bool_());
            case TokenKind::less_eq: return Value::of_bool(l.character <= r.character, tc.bool_());
            case TokenKind::greater: return Value::of_bool(l.character > r.character, tc.bool_());
            case TokenKind::greater_eq: return Value::of_bool(l.character >= r.character, tc.bool_());
            default: unknown();
            }
        }
        if (l.kind == Value::Kind::boolean) {
            switch (op) {
            case TokenKind::amp: return Value::of_bool(l.boolean && r.boolean, tc.bool_());
            case TokenKind::pipe: return Value::of_bool(l.boolean || r.boolean, tc.bool_());
            case TokenKind::caret: return Value::of_bool(l.boolean != r.boolean, tc.bool_());
            default: unknown();
            }
        }
        if (l.kind != Value::Kind::integer) unknown();
        const Type* t    = l.type;
        int         bits = t->bits;
        bool        sgn  = t->is_signed;
        auto overflow = [&](const char* what) -> Value {
            failed(span, std::format("{} {} {} overflows {}", to_string(l), what, to_string(r), types::to_string(t)), "overflow");
        };
        switch (op) {
        case TokenKind::less:
            return Value::of_bool(sgn ? l.as_signed() < r.as_signed() : l.integer < r.integer, tc.bool_());
        case TokenKind::less_eq:
            return Value::of_bool(sgn ? l.as_signed() <= r.as_signed() : l.integer <= r.integer, tc.bool_());
        case TokenKind::greater:
            return Value::of_bool(sgn ? l.as_signed() > r.as_signed() : l.integer > r.integer, tc.bool_());
        case TokenKind::greater_eq:
            return Value::of_bool(sgn ? l.as_signed() >= r.as_signed() : l.integer >= r.integer, tc.bool_());
        case TokenKind::amp: return integer(l.integer & r.integer, t);
        case TokenKind::pipe: return integer(l.integer | r.integer, t);
        case TokenKind::caret: return integer(l.integer ^ r.integer, t);
        case TokenKind::plus_percent: return integer(l.integer + r.integer, t);
        case TokenKind::minus_percent: return integer(l.integer - r.integer, t);
        case TokenKind::star_percent: return integer(l.integer * r.integer, t);
        case TokenKind::shl:
        case TokenKind::shr: {
            u128 n = r.integer;
            if ((r.type && r.type->is_signed && r.as_signed() < 0) || n >= static_cast<u128>(bits)) {
                failed(rhs_span, std::format("shift by {} is out of range for {}", to_string(r), types::to_string(t)),
                       "shift count too large");
            }
            int c = static_cast<int>(n);
            if (op == TokenKind::shl) return integer(l.integer << c, t);
            return sgn ? integer(static_cast<u128>(l.as_signed() >> c), t) : integer(l.integer >> c, t);
        }
        case TokenKind::plus:
        case TokenKind::minus:
        case TokenKind::star: {
            const char* what = op == TokenKind::plus ? "+" : op == TokenKind::minus ? "-" : "*";
            if (sgn) {
                i128 a = l.as_signed(), c = r.as_signed(), x = 0;
                bool o = op == TokenKind::plus    ? __builtin_add_overflow(a, c, &x)
                       : op == TokenKind::minus ? __builtin_sub_overflow(a, c, &x)
                                                : __builtin_mul_overflow(a, c, &x);
                if (o || x < signed_min(bits) || x > signed_max(bits)) return overflow(what);
                return integer(static_cast<u128>(x), t);
            }
            u128 a = l.integer, c = r.integer, x = 0;
            bool o = op == TokenKind::plus    ? __builtin_add_overflow(a, c, &x)
                   : op == TokenKind::minus ? __builtin_sub_overflow(a, c, &x)
                                            : __builtin_mul_overflow(a, c, &x);
            if (o || x > unsigned_max(bits)) return overflow(what);
            return integer(x, t);
        }
        case TokenKind::slash:
        case TokenKind::percent: {
            if (r.integer == 0) failed(rhs_span, "division by zero at compile time", "this is zero");
            if (sgn) {
                i128 a = l.as_signed(), c = r.as_signed();
                if (a == signed_min(bits) && c == -1) return overflow(op == TokenKind::slash ? "/" : "%");
                return integer(static_cast<u128>(op == TokenKind::slash ? a / c : a % c), t);
            }
            return integer(op == TokenKind::slash ? l.integer / r.integer : l.integer % r.integer, t);
        }
        default: unknown();
        }
    }

    Value cast(const CastExpr& c) {
        const Type* to = type_of(c);
        Value       v  = eval(*c.operand);
        auto distinct = [](const Type* t) {
            return t && t->is(Kind::user) && t->symbol && t->symbol->decl && t->symbol->kind == SymbolKind::struct_ &&
                   t->symbol->decl->as<ast::TypeDecl>().is_distinct;
        };
        if (c.variant.empty() && distinct(to)) {   // spec 34: the value in its distinct type
            Value out;
            out.kind = Value::Kind::aggregate;
            out.type = to;
            out.elements.push_back(std::move(v));
            return out;
        }
        if (c.variant.empty() && v.kind == Value::Kind::aggregate && distinct(v.type) && !v.elements.empty()) return v.elements[0];
        if (c.variant == "reinterpret" || c.variant == "dynamic") not_constant(c.span, std::format("'cast::{}' cannot run at compile time", c.variant));
        if (c.variant == "checked") {
            const Type* inner = to->symbol->template_args.at(0);
            std::optional<Value> converted = convert(v, inner, /*exact=*/true);
            Value out;
            out.kind   = Value::Kind::enumeration;
            out.type   = to;
            out.symbol = to->symbol->members.at(converted ? "some" : "none").front();
            if (converted) out.elements.push_back(*converted);
            return out;
        }
        if (c.variant == "saturate" && to->is_integer()) {
            if (v.kind == Value::Kind::floating) {
                double f = v.floating;
                if (std::isnan(f)) return integer(0, to);
                double lo = to->is_signed ? std::ldexp(-1.0, to->bits - 1) : 0.0;
                double hi = std::ldexp(1.0, to->is_signed ? to->bits - 1 : to->bits);
                if (f <= lo) return integer(to->is_signed ? static_cast<u128>(signed_min(to->bits)) : 0, to);
                if (f >= hi) return integer(to->is_signed ? static_cast<u128>(signed_max(to->bits)) : unsigned_max(to->bits), to);
            } else if (v.kind == Value::Kind::integer) {
                i128 s = v.as_signed();
                bool neg = v.type->is_signed && s < 0;
                if (to->is_signed) {
                    if (neg && s < signed_min(to->bits)) return integer(static_cast<u128>(signed_min(to->bits)), to);
                    if (!neg && v.integer > static_cast<u128>(signed_max(to->bits))) return integer(static_cast<u128>(signed_max(to->bits)), to);
                } else {
                    if (neg) return integer(0, to);
                    if (v.integer > unsigned_max(to->bits)) return integer(unsigned_max(to->bits), to);
                }
            }
        }
        std::optional<Value> r = convert(v, to, /*exact=*/false);
        if (!r) unknown();
        return *r;
    }

    // Numeric conversion; with `exact`, nullopt when the value changes.
    std::optional<Value> convert(const Value& v, const Type* to, bool exact) {
        if (v.kind == Value::Kind::integer && to->is_integer()) {
            bool neg = v.type->is_signed && v.as_signed() < 0;
            Value r  = integer(neg ? static_cast<u128>(v.as_signed()) : v.integer, to);
            if (exact) {
                bool r_neg = to->is_signed && r.as_signed() < 0;
                if (neg != r_neg || (neg ? r.as_signed() != v.as_signed() : r.integer != v.integer)) return std::nullopt;
            }
            return r;
        }
        if (v.kind == Value::Kind::integer && to->is_float()) {
            Value r;
            r.kind     = Value::Kind::floating;
            r.type     = to;
            r.floating = v.type->is_signed ? static_cast<double>(v.as_signed()) : static_cast<double>(v.integer);
            if (to->bits <= 32) r.floating = round_float(r.floating, to->bits);
            if (exact) {
                std::optional<Value> back = convert(r, v.type, true);
                if (!back || !equal(*back, v)) return std::nullopt;
            }
            return r;
        }
        if (v.kind == Value::Kind::floating && to->is_integer()) {
            double f = std::trunc(v.floating);
            if (std::isnan(v.floating)) return std::nullopt;
            double lo = to->is_signed ? std::ldexp(-1.0, to->bits - 1) : 0.0;
            double hi = std::ldexp(1.0, to->is_signed ? to->bits - 1 : to->bits);
            if (f < lo || f >= hi || (exact && f != v.floating)) return std::nullopt;
            u128 bits = f < 0 ? static_cast<u128>(static_cast<i128>(f)) : static_cast<u128>(f);
            return integer(bits, to);
        }
        if (v.kind == Value::Kind::floating && to->is_float()) {
            Value r = v;
            r.type  = to;
            if (to->bits <= 32) r.floating = round_float(v.floating, to->bits);
            if (exact && !std::isnan(v.floating) && r.floating != v.floating) return std::nullopt;
            return r;
        }
        if (v.kind == Value::Kind::character && to->is_integer()) return integer(v.character, to);
        if (v.kind == Value::Kind::integer && to->is(Kind::char_)) {
            Value r;
            r.kind      = Value::Kind::character;
            r.type      = to;
            r.character = static_cast<char32_t>(v.integer);
            return r;
        }
        if (v.kind == Value::Kind::enumeration && to->is_integer() && v.symbol && v.symbol->alternative) {
            return integer(tag_of(v.symbol), to);
        }
        return std::nullopt;
    }

    u128 tag_of(const Symbol* alt) {
        u128 next = 0;
        for (const Symbol* a : alt->parent->member_order) {
            if (!a->alternative) continue;
            if (const Expr* v = a->alternative->value.get()) {
                bool negative = false;
                if (v->kind == ExprKind::unary && v->as<UnaryExpr>().op == UnaryOp::negate) {
                    negative = true;
                    v        = v->as<UnaryExpr>().operand.get();
                }
                if (v->kind == ExprKind::int_literal) {
                    u128 m = v->as<LiteralExpr>().int_value;
                    next   = negative ? u128{0} - m : m;
                }
            }
            if (a == alt || a->name == alt->name) return next;
            ++next;
        }
        return next;
    }

    std::vector<const Symbol*> fields_of(const Type* t) {
        std::vector<const Symbol*> out;
        for (const Symbol* f : t->symbol->member_order) {
            if (f->kind == SymbolKind::field && !f->is_static) out.push_back(f);
        }
        return out;
    }

    // The elements a compile-time value holds: a list's own, or a std::array's (its one field).
    static Value& elements_of(Value& v) {
        if (v.kind == Value::Kind::aggregate && v.elements.size() == 1 && v.elements[0].kind == Value::Kind::list && v.type &&
            types::is_std_instance(v.type, "array")) {
            return v.elements[0];
        }
        return v;
    }
    static const Value& elements_of(const Value& v) { return elements_of(const_cast<Value&>(v)); }

    Value default_of(const Type* t, Span span) {
        if (t->is_integer()) return integer(0, t);
        if (t->is_float()) {
            Value v;
            v.kind = Value::Kind::floating;
            v.type = t;
            return v;
        }
        if (t->is(Kind::bool_)) return Value::of_bool(false, t);
        if (t->is(Kind::char_)) {
            Value v;
            v.kind = Value::Kind::character;
            v.type = t;
            return v;
        }
        if (t->is(Kind::static_str)) return string_value("");
        if (types::is_std_instance(t, "option")) {
            Value v;
            v.kind   = Value::Kind::enumeration;
            v.type   = t;
            v.symbol = t->symbol->members.at("none").front();
            return v;
        }
        if (t->is(Kind::user) && (t->symbol->kind == SymbolKind::struct_ || t->symbol->kind == SymbolKind::class_)) {
            return build_aggregate(t, {}, span);
        }
        if (t->is(Kind::array) && !t->args.empty() && t->args[0]->is(Kind::value_arg)) {   // std::raw::array<T, N>: N defaults
            Value v;
            v.kind = Value::Kind::list;
            v.type = t;
            u128 n = tc.value_bits(t->args[0]);
            if (n > 4096) not_constant(span, std::format("the default of {} is too large to compute at compile time", types::to_string(t)));
            for (u128 i = 0; i < n; ++i) v.elements.push_back(default_of(t->inner, span));
            return v;
        }
        not_constant(span, std::format("the default of {} is not known at compile time", types::to_string(t)));
    }

    Value build_aggregate(const Type* t, const std::map<std::string, Value>& given, Span span) {
        Value v;
        v.kind = Value::Kind::aggregate;
        v.type = t;
        for (const Symbol* f : fields_of(t)) {
            if (auto it = given.find(f->name); it != given.end()) {
                v.elements.push_back(it->second);
                continue;
            }
            const auto& d = f->decl->as<VariableDecl>();
            if (d.init) {
                if (!d.init->type && hooks.check_field) hooks.check_field(f);   // declared later, or in another module
                if (!d.init->type) not_constant(span, std::format("the default of '{}::{}' is not checked yet", t->symbol->name, f->name),
                                                "declare the type before the compile-time code that uses it");
                v.elements.push_back(eval(*d.init));
            } else {
                v.elements.push_back(default_of(tc.lower(*d.type, sema::substitution_of(f)), span));
            }
        }
        return v;
    }

    Value struct_literal(const StructLiteralExpr& sl) {
        const Type* t = type_of(sl);
        if (!t->is(Kind::user)) unknown();
        if (types::is_union(t->symbol)) {   // spec 34: its bytes depend on the target's layout
            not_constant(sl.span, std::format("a union ({}) is not a compile-time value", types::to_string(t)),
                         "build it at run time");
        }
        std::map<std::string, Value> given;
        for (const FieldInit& f : sl.fields) given[f.name] = eval(*f.value);
        return build_aggregate(t, given, sl.span);
    }

    // --- members and calls -------------------------------------------------------------

    std::size_t field_index(const Type* t, const std::string& name, Span span) {
        std::vector<const Symbol*> fs = fields_of(t);
        for (std::size_t i = 0; i < fs.size(); ++i) {
            if (fs[i]->name == name) return i;
        }
        not_constant(span, std::format("'{}' is not a field that is known at compile time", name));
    }

    Value member(const MemberExpr& me) {
        Value              o    = eval(*me.object);
        const std::string& name = me.name.segments.back().name;
        if (o.kind == Value::Kind::aggregate && o.type && types::strip_ref(o.type)->is(Kind::tuple)) {   // `t.0`
            std::size_t i = static_cast<std::size_t>(std::stoull(name));
            if (i >= o.elements.size()) unknown();
            return o.elements[i];
        }
        if (o.kind == Value::Kind::field) {
            const Symbol* f = o.symbol;
            if (name == "name") return string_value(f->name);
            if (name == "type") return Value::of_type(tc.lower(*f->decl->as<VariableDecl>().type, sema::substitution_of(f)), tc.type_value());
            std::vector<const Symbol*> all = fields_of(o.type_value);
            std::size_t index = 0;
            while (index < all.size() && all[index] != f) ++index;
            if (name == "index") return integer(index, usize());
            if (name == "offset") return integer(tc.field_offsets(o.type_value).at(index), usize());
        }
        if (o.kind == Value::Kind::alternative) {
            const Symbol* a = o.symbol;
            if (name == "name") return string_value(a->name);
            if (name == "has_payload") return Value::of_bool(a->alternative->payload != nullptr, tc.bool_());
            if (name == "payload") {
                return Value::of_type(a->alternative->payload ? tc.lower(*a->alternative->payload, sema::substitution_of(a)) : tc.void_(),
                                      tc.type_value());
            }
            if (name == "index") {
                std::size_t index = 0;
                for (const Symbol* x : a->parent->member_order) {
                    if (x == a) break;
                    if (x->alternative) ++index;
                }
                return integer(index, usize());
            }
        }
        if (o.kind == Value::Kind::file) {   // std::meta::file: the file a file annotation is in
            if (name == "name") return string_value(o.string);
            if (name == "module") return o.elements.empty() ? string_value("") : o.elements[0];
        }
        if (o.kind == Value::Kind::function) {
            const Symbol*       m = o.symbol;
            const FunctionDecl* f = body_decl(m);
            if (name == "name") return string_value(m->name);
            if (name == "result") return Value::of_type(f->result ? tc.lower(*f->result, sema::substitution_of(m)) : tc.void_(), tc.type_value());
            if (name == "is_mut") return Value::of_bool(f->is_mut, tc.bool_());
            if (name == "is_static") return Value::of_bool(m->is_static, tc.bool_());
            if (name == "namespace_name") return string_value(m->ns);
            if (name == "parameters") {
                Value list;
                list.kind = Value::Kind::list;
                list.type = type_of(me);
                for (std::size_t i = 0; i < f->params.size(); ++i) {
                    Value p;
                    p.kind       = Value::Kind::parameter;
                    p.type       = tc.meta_parameter();
                    p.symbol     = m;
                    p.index      = i;
                    p.type_value = o.type_value;
                    list.elements.push_back(p);
                }
                return list;
            }
        }
        if (o.kind == Value::Kind::parameter) {
            const ast::Param& p = function_decl(o.symbol)->params.at(o.index);
            if (name == "name") return string_value(p.name);
            if (name == "index") return integer(o.index, usize());
            const Type* pt = tc.lower(*p.type, sema::substitution_of(o.symbol));
            if (name == "type") return Value::of_type(types::strip_ref(pt), tc.type_value());
            if (name == "passing") {
                const Type* passing = type_of(me);
                if (!passing->is(Kind::user)) unknown();
                Value v;
                v.kind   = Value::Kind::enumeration;
                v.type   = passing;
                v.symbol = passing->symbol->members.at(!pt->is(Kind::reference) ? "by_value" : pt->is_mut ? "by_mut_reference" : "by_reference").front();
                return v;
            }
        }
        if (o.kind == Value::Kind::aggregate) return o.elements.at(field_index(o.type, name, me.span));
        not_constant(me.span, std::format("'.{}' is not known at compile time", name));
    }

    // The declaration of a function whose parameters are reflected (the one with annotations).
    static const FunctionDecl* function_decl(const Symbol* f) { return &f->decl->as<FunctionDecl>(); }

    // `annotation<A>()` / `has<A>()` on the annotations `uses` (spec 29).
    Value annotation_of(const std::vector<ast::AnnotationUse>* uses, const Type* a, bool has, const CallExpr& c) {
        if (!a->is(Kind::user)) unknown();
        const ast::AnnotationUse* found = nullptr;
        if (uses) {
            for (const ast::AnnotationUse& u : *uses) {
                if (u.symbol == a->symbol) found = &u;
            }
        }
        if (has) return Value::of_bool(found != nullptr, tc.bool_());
        const Type* option = type_of(c);
        if (!option->is(Kind::user)) unknown();
        Value out;
        out.kind   = Value::Kind::enumeration;
        out.type   = option;
        out.symbol = option->symbol->members.at(found ? "some" : "none").front();
        if (found) {
            std::shared_ptr<const Value> v = found->value_symbol ? hooks.constant_value(found->value_symbol) : nullptr;
            if (!v) not_constant(c.span, std::format("the value of '@{}' is not known", ast::to_string(found->path)));
            out.elements.push_back(*v);
        }
        return out;
    }

    Value call(const CallExpr& c) {
        if (std::string b = builtin_name(c); !b.empty()) return builtin(c, b);
        if (std::string r = raw_intrinsic(c); r == "bit_cast") return bit_cast(c);
        const Expr& callee = *c.callee;
        if (callee.kind == ExprKind::path) {
            const auto& pe = callee.as<PathExpr>();
            const Symbol* s = pe.symbol;
            if (!s) not_constant(c.span, std::format("'{}' cannot run at compile time", ast::to_string(pe.path)),
                                 "I/O and other library functions only exist at run time");
            if (s->kind == SymbolKind::alternative) {
                Value v;
                v.kind   = Value::Kind::enumeration;
                v.type   = type_of(c);
                v.symbol = s;
                if (c.args.size() == 1) v.elements.push_back(eval(*c.args[0]));
                return v;
            }
            if (s->kind == SymbolKind::function || s->kind == SymbolKind::method) return run(s, std::nullopt, c);
        }
        if (callee.kind == ExprKind::member) {
            const auto&        me   = callee.as<MemberExpr>();
            const std::string& name = me.name.segments.back().name;
            if (me.symbol && me.symbol->kind == SymbolKind::method) {
                Value self = eval(*me.object);
                return run(dispatch(me.symbol, self), std::move(self), c);
            }
            Value o = eval(*me.object);
            return method(o, name, c);
        }
        not_constant(c.span, "this call cannot run at compile time");
    }

    // `std::bit_cast<T>(x)` at compile time (spec 34): between numbers of the same width.
    Value bit_cast(const CallExpr& c) {
        Value       x  = eval(*c.args.at(0));
        const Type* to = type_of(c);
        if (!to || !(to->is_integer() || to->is_float()) || !(x.kind == Value::Kind::integer || x.kind == Value::Kind::floating)) {
            not_constant(c.span, "std::bit_cast runs at compile time between numbers only", "not a number");
        }
        std::uint64_t bits = 0;
        if (x.kind == Value::Kind::floating) {
            if (x.type->bits == 16) bits = std::bit_cast<std::uint16_t>(static_cast<_Float16>(x.floating));
            else if (x.type->bits == 32) bits = std::bit_cast<std::uint32_t>(static_cast<float>(x.floating));
            else bits = std::bit_cast<std::uint64_t>(x.floating);
        } else {
            bits = static_cast<std::uint64_t>(x.integer);
        }
        if (to->is_integer()) return integer(static_cast<u128>(bits), to);
        Value v;
        v.kind     = Value::Kind::floating;
        v.type     = to;
        v.floating = to->bits == 16   ? static_cast<double>(std::bit_cast<_Float16>(static_cast<std::uint16_t>(bits)))
                   : to->bits == 32 ? static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(bits)))
                                    : std::bit_cast<double>(bits);
        return v;
    }

    // The methods of numbers (spec 34), at compile time.
    Value number_method(const Value& o, const std::string& name, const CallExpr& c) {
        std::vector<Value> a;
        for (const ExprPtr& e : c.args) a.push_back(eval(*e));
        if (o.kind == Value::Kind::integer) {
            bool sgn = o.type && o.type->is_signed;
            auto less = [&](const Value& x, const Value& y) { return sgn ? x.as_signed() < y.as_signed() : x.integer < y.integer; };
            if (name == "abs") {
                if (!sgn || o.as_signed() >= 0) return o;
                if (o.integer == (u128(1) << (o.type->bits - 1))) failed(c.span, std::format("integer overflow in 'abs' ({})", o.type->name), "overflow");
                return integer(static_cast<u128>(-o.as_signed()), o.type);
            }
            if (name == "min") return less(a.at(0), o) ? a[0] : o;
            if (name == "max") return less(o, a.at(0)) ? a[0] : o;
            if (name == "clamp") {
                if (less(a.at(1), a.at(0))) failed(c.span, "clamp with lo > hi", "lo > hi");
                return less(o, a[0]) ? a[0] : less(a[1], o) ? a[1] : o;
            }
            unknown();
        }
        if (o.kind != Value::Kind::floating) unknown();
        bool   f32 = o.type && o.type->bits <= 32;
        double x   = o.floating;
        auto   num = [&](double r) {
            Value v    = o;
            v.floating = f32 ? round_float(r, o.type->bits) : r;
            return v;
        };
        auto arg = [&](std::size_t i) { return a.at(i).floating; };
        if (name == "sqrt") return num(std::sqrt(x));
        if (name == "abs") return num(std::fabs(x));
        if (name == "floor") return num(std::floor(x));
        if (name == "ceil") return num(std::ceil(x));
        if (name == "round") return num(std::round(x));
        if (name == "trunc") return num(std::trunc(x));
        if (name == "sin") return num(std::sin(x));
        if (name == "cos") return num(std::cos(x));
        if (name == "tan") return num(std::tan(x));
        if (name == "asin") return num(std::asin(x));
        if (name == "acos") return num(std::acos(x));
        if (name == "atan") return num(std::atan(x));
        if (name == "atan2") return num(std::atan2(x, arg(0)));
        if (name == "exp") return num(std::exp(x));
        if (name == "exp2") return num(std::exp2(x));
        if (name == "log") return num(std::log(x));
        if (name == "log2") return num(std::log2(x));
        if (name == "log10") return num(std::log10(x));
        if (name == "pow") return num(std::pow(x, arg(0)));
        if (name == "copysign") return num(std::copysign(x, arg(0)));
        if (name == "min") return num(std::fmin(x, arg(0)));
        if (name == "max") return num(std::fmax(x, arg(0)));
        if (name == "mul_add") return num(f32 ? static_cast<double>(std::fma(static_cast<float>(x), static_cast<float>(arg(0)), static_cast<float>(arg(1))))
                                              : std::fma(x, arg(0), arg(1)));
        if (name == "clamp") {
            if (!(arg(0) <= arg(1))) failed(c.span, "clamp with lo > hi (or a NaN bound)", "lo > hi");
            return num(std::fmin(std::fmax(x, arg(0)), arg(1)));
        }
        if (name == "is_nan") return Value::of_bool(std::isnan(x), tc.bool_());
        if (name == "is_infinite") return Value::of_bool(std::isinf(x), tc.bool_());
        if (name == "is_finite") return Value::of_bool(std::isfinite(x), tc.bool_());
        unknown();
    }

    Value method(const Value& o, const std::string& name, const CallExpr& c) {
        if ((o.kind == Value::Kind::integer || o.kind == Value::Kind::floating) && types::is_number_method(o.type, name)) {
            return number_method(o, name, c);
        }
        if ((o.kind == Value::Kind::list || o.kind == Value::Kind::string) && c.args.empty()) {
            std::size_t n = o.kind == Value::Kind::list ? o.elements.size() : o.string.size();
            if (name == "len") return integer(n, usize());
            if (name == "is_empty") return Value::of_bool(n == 0, tc.bool_());
        }
        if (o.kind == Value::Kind::string && c.args.size() == 1 &&
            (name == "starts_with" || name == "ends_with" || name == "contains")) {   // std::str methods (spec 19)
            Value a = eval(*c.args[0]);
            std::string needle = a.kind == Value::Kind::string ? a.string
                               : a.kind == Value::Kind::character ? std::string(1, static_cast<char>(a.character))
                                                                  : (unknown(), std::string());
            bool r = name == "starts_with" ? o.string.starts_with(needle)
                   : name == "ends_with"   ? o.string.ends_with(needle)
                                           : o.string.find(needle) != std::string::npos;
            return Value::of_bool(r, tc.bool_());
        }
        if (o.kind == Value::Kind::string && name == "slice" && c.args.size() == 2) {   // byte range (spec 19, 31)
            Value from = eval(*c.args[0]);
            Value to   = eval(*c.args[1]);
            if (from.integer > to.integer || to.integer > o.string.size()) {
                failed(c.span, std::format("slice {}..{} of a text of {} bytes", edax::to_string(from.integer), edax::to_string(to.integer),
                                           o.string.size()),
                       "out of bounds");
            }
            return string_value(o.string.substr(static_cast<std::size_t>(from.integer), static_cast<std::size_t>(to.integer - from.integer)));
        }
        if (o.kind == Value::Kind::string && name == "find" && c.args.size() == 1) {   // std::option<usize>
            Value       a      = eval(*c.args[0]);
            std::string needle = a.kind == Value::Kind::string ? a.string
                               : a.kind == Value::Kind::character ? std::string(1, static_cast<char>(a.character))
                                                                  : (unknown(), std::string());
            const Type* option = type_of(c);
            if (!option || !option->is(Kind::user)) unknown();
            std::size_t at = o.string.find(needle);
            Value out;
            out.kind   = Value::Kind::enumeration;
            out.type   = option;
            out.symbol = option->symbol->members.at(at == std::string::npos ? "none" : "some").front();
            if (at != std::string::npos) out.elements.push_back(integer(at, usize()));
            return out;
        }
        if ((name == "has" || name == "annotation") &&
            (o.kind == Value::Kind::field || o.kind == Value::Kind::alternative || o.kind == Value::Kind::function ||
             o.kind == Value::Kind::parameter)) {
            const auto& seg = c.callee->as<MemberExpr>().name.segments.back();
            if (seg.template_args.empty() || !seg.template_args[0].type) unknown();
            const Type* a = lower(*seg.template_args[0].type);
            const std::vector<ast::AnnotationUse>* uses = nullptr;
            switch (o.kind) {
            case Value::Kind::field:
            case Value::Kind::function: uses = o.symbol->decl ? &o.symbol->decl->annotations : nullptr; break;
            case Value::Kind::alternative: uses = o.symbol->alternative ? &o.symbol->alternative->annotations : nullptr; break;
            case Value::Kind::parameter: uses = &function_decl(o.symbol)->params.at(o.index).annotations; break;
            default: break;
            }
            return annotation_of(uses, a, name == "has", c);
        }
        if (o.kind == Value::Kind::field && name == "get" && c.args.size() == 1) {
            Value v = eval(*c.args[0]);
            if (v.kind != Value::Kind::aggregate) unknown();
            return v.elements.at(field_index(v.type, o.symbol->name, c.span));
        }
        if (o.kind == Value::Kind::alternative) {
            if (name == "is" && c.args.size() == 1) {
                Value v = eval(*c.args[0]);
                return Value::of_bool(v.kind == Value::Kind::enumeration && v.symbol && v.symbol->name == o.symbol->name, tc.bool_());
            }
            if (name == "make") {
                Value v;
                v.kind   = Value::Kind::enumeration;
                v.type   = o.type_value;
                v.symbol = o.symbol;
                if (c.args.size() == 1) v.elements.push_back(eval(*c.args[0]));
                return v;
            }
        }
        if (o.kind == Value::Kind::enumeration && (types::is_std_instance(o.type, "option") || types::is_std_instance(o.type, "result"))) {
            bool good = o.symbol->name == "some" || o.symbol->name == "ok";
            if (c.args.empty() && (name == "is_some" || name == "is_ok")) return Value::of_bool(good, tc.bool_());
            if (c.args.empty() && (name == "is_none" || name == "is_error")) return Value::of_bool(!good, tc.bool_());
            if (name == "value_or" && c.args.size() == 1) return good ? o.elements.at(0) : eval(*c.args[0]);
            if (name == "expect" && c.args.size() == 1) {
                if (good) return o.elements.at(0);
                Value m = eval(*c.args[0]);
                failed(c.span, std::format("compile-time evaluation panicked: {}", m.string), "'expect' failed");
            }
        }
        not_constant(c.span, std::format("the method '{}' cannot run at compile time", name));
    }

    // `{value:spec}` of an f-string, formatted by the compiler (spec 19, 23).
    std::string format(const Value& v, const FormatSpec& spec, Span span) {
        std::string text;
        switch (v.kind) {
        case Value::Kind::integer: {
            bool       neg = v.type && v.type->is_signed && v.as_signed() < 0 && !spec.radix;
            u128       mag = neg ? static_cast<u128>(-(v.as_signed() + 1)) + 1 : v.integer;
            int        base = spec.radix == 'x' || spec.radix == 'X' ? 16 : spec.radix == 'b' ? 2 : spec.radix == 'o' ? 8 : 10;
            const char* digits = spec.radix == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            do {
                text.insert(text.begin(), digits[static_cast<int>(mag % static_cast<u128>(base))]);
                mag /= static_cast<u128>(base);
            } while (mag != 0);
            if (neg) text.insert(0, "-");
            else if (spec.plus) text.insert(0, "+");
            break;
        }
        case Value::Kind::floating: {
            double f = v.floating;
            if (std::isnan(f)) text = "NaN";
            else if (std::isinf(f)) text = f < 0 ? "-inf" : "inf";
            else if (spec.precision >= 0) text = std::format("{:.{}f}", f, spec.precision);
            else {
                text = v.type && v.type->bits == 32 ? std::format("{}", static_cast<float>(f)) : std::format("{}", f);
                if (text.find_first_of(".en") == std::string::npos) text += ".0";
            }
            if (spec.plus && f >= 0 && !std::isnan(f)) text.insert(0, "+");
            break;
        }
        case Value::Kind::boolean: text = v.boolean ? "true" : "false"; break;
        case Value::Kind::character: encode_utf8(v.character, text); break;
        case Value::Kind::string: text = v.string; break;
        case Value::Kind::type: text = types::to_string(v.type_value); break;
        case Value::Kind::enumeration: text = v.symbol ? v.symbol->name : "?"; break;
        default: not_constant(span, std::format("{} cannot be formatted at compile time", to_string(v)));
        }
        if (spec.width > 0) {
            std::size_t len = count_code_points(text);
            std::size_t w   = static_cast<std::size_t>(spec.width);
            if (len < w) {
                std::size_t pad   = w - len;
                bool        right = spec.align == '>' || (spec.align == 0 && (v.kind == Value::Kind::integer || v.kind == Value::Kind::floating));
                if (spec.align == '^') text = std::string(pad / 2, ' ') + text + std::string(pad - pad / 2, ' ');
                else if (right) text = std::string(pad, ' ') + text;
                else text += std::string(pad, ' ');
            }
        }
        return text;
    }

    // std::compile_error / compile_warning / compile_note (spec 23).
    Value diagnostic(const CallExpr& c, const std::string& name) {
        auto text = [&](std::size_t i) { return eval(*c.args.at(i)).string; };
        Span at   = site ? *site : c.span;
        if (name == "compile_note") {
            pending_notes.push_back(Note{text(0), std::nullopt});
            return Value{};
        }
        std::size_t first = 0;   // an optional std::source_location first: where to report
        if (!c.args.empty() && is_location(*c.args[0])) {
            Value loc = eval(*c.args[0]);
            if (loc.location) at = *loc.location;
            first = 1;
        }
        bool        with_id = c.args.size() - first == 2;
        std::string id      = with_id ? diagnostic_id(*c.args[first]) : std::string();
        std::string message = text(with_id ? first + 1 : first);
        if (quiet) throw Stop{};
        bool error = name == "compile_error";
        if (!error && !id.empty() &&   // `@std::allow{ .id = ... }` on the function being checked (spec 29)
            std::find(program.allowed_warnings.begin(), program.allowed_warnings.end(), id) != program.allowed_warnings.end()) {
            pending_notes.clear();
            return Value{};
        }
        if (!error && !id.empty()) {   // --warn=id=off|error (spec 23)
            auto level = program.warn_levels.find(id);
            if (level != program.warn_levels.end() && level->second == "off") {
                pending_notes.clear();
                return Value{};
            }
            if (level != program.warn_levels.end() && level->second == "error") error = true;
        }
        Diagnostic& d = diags.report(error ? DiagCode::E4210 : DiagCode::W4210, at, message,
                                     name == "compile_error" ? "check failed" : error ? "warning made an error" : "");
        for (Note& n : pending_notes) d.notes.push_back(std::move(n));
        pending_notes.clear();
        if (!id.empty()) d.notes.push_back(Note{std::format("{} id: {}", name == "compile_error" ? "error" : "warning", id), std::nullopt});
        // The check's own location, unless it is the standard library's (its source is not the user's)
        const Symbol* checker = frames.empty() ? nullptr : frames.front().function;
        bool in_std = checker && checker->module && (checker->module->name == "std" || checker->module->name.starts_with("std::"));
        if (site && !in_std) d.notes.push_back(Note{"reported by this check", c.span});
        if (name == "compile_error") throw Stop{};
        return Value{};
    }

    static bool is_location(const Expr& e) {
        const Type* t = types::strip_ref(e.type);
        return t && t->is(Kind::user) && t->symbol->name == "source_location" && t->symbol->module && t->symbol->module->name == "std";
    }

    // The full name of the `std::diagnostic` that `e` names: `gfx::deprecated`.
    static std::string diagnostic_id(const Expr& e) {
        const sema::Symbol* s = e.kind == ExprKind::path ? e.as<PathExpr>().symbol : nullptr;
        if (!s) return ast::to_string(e.as<PathExpr>().path);
        return s->key();
    }

    // A check met while running compile-time code (spec 23): an entry check of a compile_time
    // function, or a run-time check in a `compile_time run_time` function. Failing is an error.
    void run_check(const Check& k, Span span) {
        if (k.block) {
            if (k.is_compile_time && k.compile_active) block(*k.block);
            return;
        }
        if (!(k.is_compile_time && k.compile_active) && !(k.is_run_time && k.run_active)) return;
        if (truth(eval(*k.condition))) return;
        std::string message = eval(*k.message).string;
        Span        at      = span;
        for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
            if (it->function) {
                at = it->call_site;   // the call that broke the function's precondition
                break;
            }
        }
        if (quiet) throw Stop{};
        Diagnostic& d = diags.report(DiagCode::E4210, at, message, "check failed");
        if (at.begin != k.condition->span.begin || at.file != k.condition->span.file) {
            d.notes.push_back(Note{"the check is here", k.condition->span});
        }
        call_notes(d);
        throw Stop{};
    }

    Value builtin(const CallExpr& c, const std::string& name) {
        if (name.starts_with("compile_")) return diagnostic(c, name);
        if (name == "literal_text") {   // the text of a literal argument, or "" (spec 31)
            const Symbol* p = c.args.size() == 1 && c.args[0]->kind == ExprKind::path ? c.args[0]->as<PathExpr>().symbol : nullptr;
            std::optional<std::string> text = p && frames.size() == 1 && parameter_text ? parameter_text(p) : std::nullopt;
            return string_value(text ? *text : std::string());
        }
        if (name == "is_compile_time" || name == "location_of") {   // about a parameter's argument (spec 23)
            const Symbol* p = c.args.size() == 1 && c.args[0]->kind == ExprKind::path ? c.args[0]->as<PathExpr>().symbol : nullptr;
            if (name == "is_compile_time") {
                bool known = p && frames.size() == 1 && parameter_known && parameter_known(p);
                return Value::of_bool(known, tc.bool_());
            }
            std::optional<Span> at = p && frames.size() == 1 && parameter_site ? parameter_site(p) : std::nullopt;
            Value v;
            v.kind     = Value::Kind::aggregate;
            v.type     = c.type;
            v.location = at ? at : std::optional<Span>(c.args.empty() ? c.span : c.args[0]->span);
            return v;
        }
        if (name == "meta::function_annotation" || name == "meta::type_annotation" || name == "meta::file_annotation") {
            // `annotation<A>` around a call site (spec 31): on the function it is written in, on
            // that function's type, or on its file.
            const auto& s = c.callee->as<PathExpr>().path.segments.back();
            if (s.template_args.empty() || !s.template_args[0].type || c.args.size() != 1) unknown();
            const Type* a     = lower(*s.template_args[0].type);
            Value       where = eval(*c.args[0]);
            const std::vector<ast::AnnotationUse>* uses = nullptr;
            const Symbol* f = where.symbol;
            while (f && f->template_of) f = f->template_of;
            if (name == "meta::function_annotation" && f && f->decl) uses = &f->decl->annotations;
            if (name == "meta::type_annotation" && f && f->parent) {
                const Symbol* t = f->parent;
                while (t->template_of) t = t->template_of;
                if (t->decl) uses = &t->decl->annotations;
            }
            if (name == "meta::file_annotation" && where.location) {
                for (const auto& [mname, m] : program.modules) {
                    if (m->source && m->source->id == where.location->file) uses = &m->source->ast.annotations;
                }
            }
            return annotation_of(uses, a, false, c);
        }
        if (name == "meta::compiler_version") return string_value(EDAX_VERSION);   // what a plugin was built with
        const auto& seg = c.callee->as<PathExpr>().path.segments.back();
        auto arg = [&](std::size_t i) -> const Type* {
            if (i >= seg.template_args.size() || !seg.template_args[i].type) unknown();
            return lower(*seg.template_args[i].type);
        };
        const Type* t = arg(0);
        if (name == "is_integer") return Value::of_bool(t->is_integer(), tc.bool_());
        if (name == "is_float") return Value::of_bool(t->is_float(), tc.bool_());
        if (name == "is_same") return Value::of_bool(t == arg(1), tc.bool_());
        if (name == "implements") {
            const Type* i = arg(1);
            if (t->is_unknown() || t->is(Kind::type_param) || i->is_unknown()) unknown();
            return Value::of_bool(hooks.implements && hooks.implements(t, i), tc.bool_());
        }
        if (name == "type_name") return string_value(types::to_string(t));
        if (name == "meta::type_id") {   // spec 34: a hash of the fully qualified type, the same in every module
            std::string   text = types::qualified_name(t);
            std::uint64_t h    = 1469598103934665603ull;   // FNV-1a
            for (char ch : text) h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ull;
            return integer(h, tc.builtin("u64"));
        }
        if (name == "meta::interface_hash") {   // a plugin's interface (dynamic libraries): its name and methods
            if (!t->is(Kind::user) || t->symbol->kind != SymbolKind::interface) {
                failed(c.span, std::format("std::meta::interface_hash needs an interface, not {}", types::to_string(t)), "not an interface");
            }
            return integer(interface_hash(t->symbol), tc.builtin("u64"));
        }
        if (name == "size_of" || name == "align_of") {
            types::Layout l = tc.layout(t);
            if (l.size == 0 && !l.empty && !t->is(Kind::void_)) not_constant(c.span, std::format("the size of {} is not known", types::to_string(t)));
            return integer(name == "size_of" ? l.size : l.align, usize());
        }
        if (name == "meta::result_of") {   // what calling a callable gives (spec 26): the result type of a lambda
            if (t->is(Kind::closure) || t->is(Kind::function) || t->is(Kind::fn_ptr)) return Value::of_type(t->args.back(), c.type);
            if (t->is_unknown() || t->is(Kind::type_param)) unknown();
            failed(c.span, std::format("std::meta::result_of needs a lambda or callable type, not {}", types::to_string(t)), "not callable");
        }
        if (name == "meta::is_view") {
            if (t->is_unknown() || t->is(Kind::type_param)) unknown();
            return Value::of_bool(hooks.is_view && hooks.is_view(t), tc.bool_());
        }
        bool is_user = t->is(Kind::user);
        if (name == "meta::is_struct") return Value::of_bool(is_user && t->symbol->kind == SymbolKind::struct_, tc.bool_());
        if (name == "meta::is_class") return Value::of_bool(is_user && t->symbol->kind == SymbolKind::class_, tc.bool_());
        if (name == "meta::is_enum") return Value::of_bool(is_user && t->symbol->kind == SymbolKind::enum_, tc.bool_());
        Value list;
        list.kind = Value::Kind::list;
        list.type = c.type;
        if (name == "meta::fields") {
            if (!is_user || (t->symbol->kind != SymbolKind::struct_ && t->symbol->kind != SymbolKind::class_)) {
                failed(c.span, std::format("std::meta::fields needs a struct or class, not {}", types::to_string(t)), "not a struct or class");
            }
            for (const Symbol* f : hooks.visible_fields(t)) {
                Value v;
                v.kind       = Value::Kind::field;
                v.type       = tc.meta_field();
                v.symbol     = f;
                v.type_value = t;
                list.elements.push_back(v);
            }
            return list;
        }
        if (name == "meta::functions_with" || name == "meta::types_with") {   // registries (metaprogramming)
            bool types_wanted = name == "meta::types_with";
            if (!is_user) failed(c.span, std::format("{} is not an annotation", types::to_string(t)), "not an annotation");
            std::vector<const Symbol*> found;
            for (const auto& [mname, mod] : program.modules) {
                for (const Symbol* item : mod->item_order) {
                    bool is_fn   = item->kind == SymbolKind::function;
                    bool is_type = item->kind == SymbolKind::struct_ || item->kind == SymbolKind::class_ ||
                                   item->kind == SymbolKind::enum_ || item->kind == SymbolKind::interface;
                    if ((types_wanted ? !is_type : !is_fn) || !item->decl || item->decl->is_template) continue;
                    for (const ast::AnnotationUse& u : item->decl->annotations) {
                        if (u.symbol == t->symbol) {
                            found.push_back(item);
                            break;
                        }
                    }
                }
            }
            std::sort(found.begin(), found.end(), [](const Symbol* a, const Symbol* b) {
                return a->ns != b->ns ? a->ns < b->ns : a->name < b->name;
            });
            for (const Symbol* item : found) {
                if (types_wanted) {
                    list.elements.push_back(Value::of_type(tc.user(item), tc.type_value()));
                    continue;
                }
                Value v;
                v.kind   = Value::Kind::function;
                v.type   = tc.meta_function();
                v.symbol = item;
                list.elements.push_back(v);
            }
            return list;
        }
        if (name == "meta::methods") {
            if (!is_user || t->symbol->kind == SymbolKind::enum_) {
                failed(c.span, std::format("std::meta::methods needs a struct, class or interface, not {}", types::to_string(t)),
                       "no methods");
            }
            for (const Symbol* m : hooks.visible_methods ? hooks.visible_methods(t) : std::vector<const Symbol*>{}) {
                Value v;
                v.kind       = Value::Kind::function;
                v.type       = tc.meta_function();
                v.symbol     = m;
                v.type_value = t;
                list.elements.push_back(v);
            }
            return list;
        }
        if (name == "meta::interfaces") {
            if (!is_user) return list;
            std::vector<const Symbol*> found;
            std::function<void(const Symbol*)> walk = [&](const Symbol* s) {
                for (const Symbol* b : s->bases) {
                    if (b->kind == SymbolKind::interface && std::find(found.begin(), found.end(), b) == found.end()) found.push_back(b);
                    walk(b);
                }
            };
            walk(t->symbol);
            for (const Symbol* i : found) list.elements.push_back(Value::of_type(tc.user(i), tc.type_value()));
            return list;
        }
        if (name == "meta::template_args") {
            if (!is_user) return list;
            for (const Type* a : t->symbol->template_args) {
                if (a && !a->is(Kind::value_arg)) list.elements.push_back(Value::of_type(a, tc.type_value()));
            }
            return list;
        }
        if (name == "meta::annotation" || name == "meta::has_annotation") {   // (A, T): annotation A on type T (spec 29)
            const Type* a      = t;
            const Type* target = arg(1);
            if (a->is_unknown() || target->is_unknown() || target->is(Kind::type_param)) unknown();
            const std::vector<ast::AnnotationUse>* uses =
                target->is(Kind::user) && target->symbol->decl ? &target->symbol->decl->annotations : nullptr;
            return annotation_of(uses, a, name == "meta::has_annotation", c);
        }
        if (name == "meta::alternatives") {
            if (!is_user || t->symbol->kind != SymbolKind::enum_) {
                failed(c.span, std::format("std::meta::alternatives needs an enum, not {}", types::to_string(t)), "not an enum");
            }
            for (const Symbol* a : t->symbol->member_order) {
                if (!a->alternative) continue;
                Value v;
                v.kind       = Value::Kind::alternative;
                v.type       = tc.meta_alternative();
                v.symbol     = a;
                v.type_value = t;
                list.elements.push_back(v);
            }
            return list;
        }
        unknown();
    }

    // Runs a compile_time function.
    // The method a call runs: an interface or virtual method is the object's own override
    // (the value knows its class at compile time).
    const Symbol* dispatch(const Symbol* m, const Value& self) {
        if (self.kind != Value::Kind::aggregate || !self.type || !self.type->is(Kind::user)) return m;
        for (const Symbol* cls = self.type->symbol; cls; cls = cls->base_class()) {
            auto it = cls->members.find(m->name);
            if (it == cls->members.end()) continue;
            for (const Symbol* candidate : it->second) {
                if (candidate->kind != SymbolKind::method) continue;
                const FunctionDecl* f = body_decl(candidate);
                if (const FunctionDecl* mf = body_decl(m); f && f->body && mf && f->params.size() == mf->params.size()) return candidate;
            }
        }
        return m;
    }

    // --- std::build (spec 24, 25) --------------------------------------------------------

    static bool in_build_module(const Symbol* s) {
        const Symbol* owner = s->parent && s->parent->kind != SymbolKind::function ? s->parent : nullptr;
        const sema::Module* m = s->module;
        if (!m) return false;
        if (m->name != "std::build") return false;
        return !owner || owner->name == "graph" || owner->name == "path" || owner->name == "setting" || owner->name == "package";
    }

    // `**/*.cpp` as a regular expression over paths with `/`.
    static std::string glob_regex(const std::string& pattern) {
        std::string out;
        for (std::size_t i = 0; i < pattern.size(); ++i) {
            char ch = pattern[i];
            if (ch == '*' && i + 1 < pattern.size() && pattern[i + 1] == '*') {
                bool slash = i + 2 < pattern.size() && pattern[i + 2] == '/';
                out += slash ? "(.*/)?" : ".*";
                i += slash ? 2 : 1;
            } else if (ch == '*') {
                out += "[^/]*";
            } else if (ch == '?') {
                out += "[^/]";
            } else if (std::string_view(".+()[]{}^$|\\").find(ch) != std::string_view::npos) {
                out += '\\';
                out += ch;
            } else {
                out += ch;
            }
        }
        return out;
    }

    // The `path` struct value with `text`.
    Value path_value(const std::string& text, const Type* path_type) {
        Value v;
        v.kind = Value::Kind::aggregate;
        v.type = path_type;
        v.elements.push_back(string_value(text));
        return v;
    }

    // Whether class `cls` is `base` or derives from it.
    static bool derives(const Symbol* cls, const Symbol* base) {
        for (const Symbol* c = cls; c; c = c->base_class()) {
            if (c == base) return true;
        }
        return false;
    }

    // A setting from build.ed' program as a value of this program's type `want` (spec 25).
    Value setting_value(const Value& v, const Type* want, const std::string& name, Span at) {
        std::string had = v.type ? types::to_string(types::strip_ref(v.type)) : "?";
        if (want && had == types::to_string(want)) {
            if (v.kind == Value::Kind::boolean) return Value::of_bool(v.boolean, want);
            if (v.kind == Value::Kind::integer) return integer(v.integer, want);
            if (v.kind == Value::Kind::string) return string_value(v.string);
        }
        if (want && want->is(Kind::static_str) && v.kind == Value::Kind::string) return string_value(v.string);
        failed(at, std::format("the setting '{}' is a '{}', not a '{}'", name, had, want ? types::to_string(want) : "?"), "wrong type");
    }

    static std::string static_text(const Value& v) { return v.kind == Value::Kind::string ? v.string : std::string(); }

    // A value of `t` from the text of `-Dname=value`.
    Value option_value(const std::string& text, const Type* t, Span span, const std::string& name) {
        if (t->is(Kind::bool_)) {
            if (text != "true" && text != "false") failed(span, std::format("-D{}={}: expected true or false", name, text), "bool option");
            return Value::of_bool(text == "true", t);
        }
        if (t->is_integer()) {
            try {
                std::size_t used = 0;
                long long   n    = std::stoll(text, &used);
                if (used != text.size()) throw std::invalid_argument("rest");
                return integer(static_cast<u128>(static_cast<i128>(n)), t);
            } catch (...) {
                failed(span, std::format("-D{}={}: expected an integer", name, text), "integer option");
            }
        }
        if (t->is(Kind::static_str)) return string_value(text);
        failed(span, std::format("an option of type '{}' is not supported", types::to_string(t)), "option type");
    }


    // A `std::build::setting<T>` named by `e` (a declared compile_time constant): its full name
    // (the key of its values) and its short name (the `-D` option). Empty when `e` is text.
    static std::pair<std::string, std::string> setting_name(const Expr& e) {
        const Expr* x = &e;
        while (x->kind == ExprKind::unary && (x->as<UnaryExpr>().op == UnaryOp::mut_marker || x->as<UnaryExpr>().op == UnaryOp::move)) {
            x = x->as<UnaryExpr>().operand.get();
        }
        const types::Type* t = types::strip_ref(x->type);
        bool is_setting = t && t->is(Kind::user) && t->symbol->template_of && t->symbol->template_of->name == "setting" &&
                          t->symbol->template_of->module && t->symbol->template_of->module->name == "std::build";
        if (!is_setting || x->kind != ExprKind::path || !x->as<PathExpr>().symbol) return {};
        const Symbol* sym = x->as<PathExpr>().symbol;
        return {sym->key(), sym->name};
    }

    // The value build.ed gave a setting for the file of `where` (empty: the target's), or null.
    std::shared_ptr<const Value> setting_for(const std::string& name, const std::string& file) {
        namespace fs = std::filesystem;
        std::error_code ec;
        std::string     canonical = file.empty() ? std::string() : fs::weakly_canonical(fs::path(file), ec).generic_string();
        std::shared_ptr<const Value> found;
        for (const sema::FileSetting& fset : program.file_settings) {
            if (fset.name == name && std::find(fset.files.begin(), fset.files.end(), canonical) != fset.files.end()) found = fset.value;
        }
        if (!found) {
            auto it = program.build_settings.find(name);
            if (it != program.build_settings.end()) found = it->second;
        }
        return found;
    }

    // An alternative of an enum of std::build without a payload, by name.
    Value build_enum(const char* type_name, const std::string& alternative) {
        Value v;
        v.kind = Value::Kind::enumeration;
        auto bm = program.modules.find("std::build");
        const Symbol* e = bm != program.modules.end() ? bm->second->item(std::string("std::build::") + type_name) : nullptr;
        if (!e) unknown();
        v.type = tc.user(e);
        auto it = e->members.find(alternative);
        if (it == e->members.end()) unknown();
        v.symbol = it->second.front();
        return v;
    }

    // A std::build::platform value (build system, "Target platforms").
    Value platform_value(const TargetPlatform& p, const Type* t) {
        Value v;
        v.kind = Value::Kind::aggregate;
        v.type = t;
        v.elements.push_back(string_value(p.triple));
        v.elements.push_back(build_enum("os", p.os));
        v.elements.push_back(build_enum("cpu", p.cpu.empty() ? "baseline" : p.cpu));
        Value caps;
        caps.kind = Value::Kind::list;
        const Symbol* ps = t && t->is(Kind::user) ? t->symbol : nullptr;
        if (ps) {
            for (const Symbol* m : ps->member_order) {
                if (m->kind == SymbolKind::field && m->name == "capabilities") caps.type = m->type;
            }
        }
        for (const std::string& c : p.capabilities) caps.elements.push_back(build_enum("capability", c));
        v.elements.push_back(std::move(caps));
        return v;
    }

    Value build_intrinsic(const Symbol* s, const std::optional<Value>& self, const CallExpr& c) {
        const std::string& n = s->name;
        if (s->parent && s->parent->name == "setting" && c.callee->kind == ExprKind::member) {   // setting.value(), .value_at(where)
            auto [key, short_name] = setting_name(*c.callee->as<MemberExpr>().object);
            if (key.empty()) failed(c.callee->span, "a setting is read through its declared name: 'build::name.value()'", "not a declared setting");
            if (n == "value") {
                auto it = program.build_settings.find(key);
                if (it == program.build_settings.end() || !it->second) {
                    failed(c.span, std::format("build.ed does not set '{}' for this target", key), "no such setting");
                }
                return setting_value(*it->second, type_of(c), key, c.span);
            }
            if (n == "value_at") {
                Value       where = eval(*c.args.at(0));
                std::string file;
                if (where.type && where.type->is(Kind::user)) {
                    std::size_t i = 0;
                    for (const Symbol* m : where.type->symbol->member_order) {
                        if (m->kind != SymbolKind::field || m->is_static) continue;
                        if (m->name == "file" && i < where.elements.size()) file = where.elements[i].string;
                        ++i;
                    }
                }
                std::shared_ptr<const Value> found  = setting_for(key, file);
                const Type*                  option = type_of(c);
                if (!option || !option->is(Kind::user) || option->symbol->template_args.empty()) unknown();
                Value out;
                out.kind   = Value::Kind::enumeration;
                out.type   = option;
                out.symbol = option->symbol->members.at(found ? "some" : "none").front();
                if (found) out.elements.push_back(setting_value(*found, option->symbol->template_args[0], key, c.span));
                return out;
            }
        }
        if (n == "target_platform") return platform_value(target_platform(), type_of(c));
        if (n == "global_allocator_type") {   // allocators: the type of g.set_global_allocator, or void
            auto it = program.build_settings.find("std::global_allocator");
            if (it == program.build_settings.end() || !it->second || it->second->kind != Value::Kind::string) {
                return Value::of_type(tc.void_(), tc.type_value());
            }
            const std::string& name = it->second->string;
            for (const auto& [module_name, m] : program.modules) {
                if (const Symbol* chosen = m->item(name); chosen && chosen->is_type()) return Value::of_type(tc.user(chosen), tc.type_value());
            }
            failed(c.span, std::format("build.ed chose the global allocator '{}', which this program doesn't have", name), "unknown allocator");
        }
        if (n == "target_has") {
            Value cap = eval(*c.args.at(0));
            return Value::of_bool(cap.symbol && target_platform().has(cap.symbol->name), tc.bool_());
        }
        if (n == "target_has_cpu_feature") {
            Value f = eval(*c.args.at(0));
            return Value::of_bool(f.symbol && target_platform().has_feature(f.symbol->name), tc.bool_());
        }
        if (n == "current_mode") {
            if (!program.build_mode_class) failed(c.span, "the build mode is not known here", "no mode");
            Value v;
            v.kind = Value::Kind::aggregate;
            v.type = tc.user(program.build_mode_class);
            return v;
        }
        if (n == "mode_is") {
            Value m = eval(*c.args.at(0));
            const Type* t = m.kind == Value::Kind::type ? m.type_value : nullptr;
            if (!t || !t->is(Kind::user)) failed(c.args[0]->span, "'std::build::mode_is' takes a mode class", "not a mode");
            return Value::of_bool(program.build_mode_class && derives(program.build_mode_class, t->symbol), tc.bool_());
        }
        if (n == "setting_at") {   // the setting for the file of a call site, or the target's, or none (spec 31)
            namespace fs = std::filesystem;
            Value       where = eval(*c.args.at(0));
            std::string name  = static_text(eval(*c.args.at(1)));
            std::string file;
            if (where.type && where.type->is(Kind::user)) {
                std::size_t i = 0;
                for (const Symbol* m : where.type->symbol->member_order) {
                    if (m->kind != SymbolKind::field || m->is_static) continue;
                    if (m->name == "file" && i < where.elements.size()) file = where.elements[i].string;
                    ++i;
                }
            }
            std::error_code ec;
            std::string     canonical = file.empty() ? std::string() : fs::weakly_canonical(fs::path(file), ec).generic_string();
            std::shared_ptr<const Value> found;
            for (const sema::FileSetting& fset : program.file_settings) {
                if (fset.name == name && std::find(fset.files.begin(), fset.files.end(), canonical) != fset.files.end()) found = fset.value;
            }
            if (!found) {
                auto it = program.build_settings.find(name);
                if (it != program.build_settings.end()) found = it->second;
            }
            const Type* option = type_of(c);
            if (!option || !option->is(Kind::user) || option->symbol->template_args.empty()) unknown();
            Value out;
            out.kind   = Value::Kind::enumeration;
            out.type   = option;
            out.symbol = option->symbol->members.at(found ? "some" : "none").front();
            if (found) out.elements.push_back(setting_value(*found, option->symbol->template_args[0], name, c.span));
            return out;
        }
        if (n == "setting") {
            std::string name = static_text(eval(*c.args.at(0)));
            auto it = program.build_settings.find(name);
            if (it == program.build_settings.end() || !it->second) {
                failed(c.span, std::format("build.ed does not set '{}' for this target", name), "no such setting");
            }
            return setting_value(*it->second, type_of(c), name, c.span);
        }
        if (s->parent && s->parent->name == "path") {   // the methods of std::build::path
            std::string text = self && self->kind == Value::Kind::aggregate && !self->elements.empty() ? self->elements[0].string : "";
            std::string file = text.substr(text.find_last_of('/') == std::string::npos ? 0 : text.find_last_of('/') + 1);
            std::size_t dot  = file.find_last_of('.');
            if (n == "stem") return string_value(dot == std::string::npos || dot == 0 ? file : file.substr(0, dot));
            if (n == "extension") return string_value(dot == std::string::npos || dot == 0 ? "" : file.substr(dot));
            if (n == "with_extension") {
                std::string ext  = static_text(eval(*c.args.at(0)));
                std::string base = dot == std::string::npos || dot == 0 ? text : text.substr(0, text.size() - (file.size() - dot));
                if (!ext.empty() && ext.front() != '.') ext = "." + ext;
                return path_value(base + ext, self->type);
            }
        }
        ct::BuildGraph* g = program.build_graph;
        if (!g) failed(c.span, "'std::build::graph' only exists while build.ed runs", "not in build.ed");
        std::vector<Value> args;
        for (const ExprPtr& a : c.args) args.push_back(eval(*a));
        auto target_of = [&](std::size_t i) -> BuildTarget& {
            u128 id = args.at(i).kind == Value::Kind::aggregate && !args[i].elements.empty() ? args[i].elements[0].integer : 0;
            if (id == 0 || id > g->targets.size()) failed(c.args[i]->span, "not a target of this build", "unknown target");
            return g->targets[static_cast<std::size_t>(id - 1)];
        };
        auto field = [&](const Value& v, const char* name) -> const Value& {
            return v.elements.at(field_index(v.type, name, c.span));
        };
        auto handle = [&](std::size_t id) {
            Value v;
            v.kind = Value::Kind::aggregate;
            v.type = type_of(c);
            v.elements.push_back(integer(id, tc.builtin("u32")));
            return v;
        };
        if (n == "add_executable" || n == "add_library" || n == "add_test" || n == "add_c_library" || n == "add_shared_library" ||
            n == "add_plugin") {
            BuildTarget t;
            t.kind = n == "add_executable" ? BuildTarget::Kind::executable
                   : n == "add_library"    ? BuildTarget::Kind::library
                   : n == "add_test"       ? BuildTarget::Kind::test
                   : n == "add_plugin"     ? BuildTarget::Kind::plugin
                                           : BuildTarget::Kind::c_library;
            t.shared = n == "add_shared_library";
            const Value& spec = args.at(0);
            if (n == "add_shared_library" && spec.type && spec.type->is(Kind::user) && spec.type->symbol->name == "edax_target") {
                t.kind = BuildTarget::Kind::library;   // a shared Edax library
            }
            t.name = static_text(field(spec, "name"));
            if (t.name.empty()) failed(c.args[0]->span, "a target needs a '.name'", "no name");
            for (const BuildTarget& other : g->targets) {
                if (other.name == t.name) failed(c.args[0]->span, std::format("there is already a target '{}'", t.name), "duplicate");
            }
            if (t.kind == BuildTarget::Kind::c_library) {
                const Value& lang = field(spec, "language");
                const Value& std_ = field(spec, "standard");
                t.language = lang.symbol ? lang.symbol->name : "c";
                t.standard = std_.symbol ? std_.symbol->name : "";
            } else {
                t.root = static_text(field(spec, "root"));
                if (t.root.empty()) failed(c.args[0]->span, "a Edax target needs a '.root' (its main .ed file)", "no root");
            }
            t.span = c.span;
            g->targets.push_back(std::move(t));
            return handle(g->targets.size());
        }
        if (n == "import_c_headers") {
            BuildTarget& t = target_of(0);
            if (t.kind != BuildTarget::Kind::c_library) failed(c.args[0]->span, "C headers are imported from a C target", "not a C target");
            const Value&       spec = args.at(1);
            BuildTarget::CImport imp;
            const Value& hs = field(spec, "headers");
            for (const Value& h : hs.elements) imp.headers.push_back(h.kind == Value::Kind::string ? h.string : !h.elements.empty() ? h.elements[0].string : std::string());
            imp.module = static_text(field(spec, "module"));
            imp.ns     = static_text(field(spec, "namespace"));
            imp.span   = c.span;
            if (imp.headers.empty()) failed(c.args[1]->span, "'.headers' lists no header", "no headers");
            if (imp.module.empty()) failed(c.args[1]->span, "a C import needs a '.module' (the generated module's name)", "no module");
            t.c_imports.push_back(std::move(imp));
            return Value{};
        }
        if (n == "wrap_cpp") {
            BuildTarget& t = target_of(0);
            if (t.kind != BuildTarget::Kind::c_library || t.language != "cpp") {
                failed(c.args[0]->span, "C++ classes are wrapped from a C++ target (language = std::build::language::cpp)", "not a C++ target");
            }
            const Value& spec = args.at(1);
            auto texts = [&](const char* name) {
                std::vector<std::string> out;
                for (const Value& h : field(spec, name).elements) {
                    out.push_back(h.kind == Value::Kind::string ? h.string : !h.elements.empty() ? h.elements[0].string : std::string());
                }
                return out;
            };
            BuildTarget::CppWrap w;
            w.headers   = texts("headers");
            w.classes   = texts("classes");
            w.templates = texts("templates");
            w.module    = static_text(field(spec, "module"));
            w.ns        = static_text(field(spec, "namespace"));
            w.span      = c.span;
            if (w.headers.empty()) failed(c.args[1]->span, "'.headers' lists no header", "no headers");
            if (w.classes.empty() && w.templates.empty()) failed(c.args[1]->span, "'.classes' and '.templates' list nothing to wrap", "nothing to wrap");
            if (w.module.empty()) failed(c.args[1]->span, "a C++ wrap needs a '.module' (the generated module's name)", "no module");
            t.cpp_wraps.push_back(std::move(w));
            return Value{};
        }
        if (n == "link") {
            target_of(0).links.push_back(static_cast<std::size_t>(args[1].elements.at(0).integer - 1));
            target_of(1);
            return Value{};
        }
        if (n == "add_sources") {   // a file, or a list of paths (g.glob)
            BuildTarget& t = target_of(0);
            if (t.kind != BuildTarget::Kind::c_library) failed(c.span, "sources are added to C targets", "not a C target");
            if (args[1].kind == Value::Kind::list) {
                for (const Value& f : args[1].elements) t.sources.push_back(f.elements.at(0).string);
            } else {
                t.sources.push_back(static_text(args[1]));
            }
            return Value{};
        }
        if (n == "add_include_dir") {
            target_of(0).include_dirs.emplace_back(static_text(args[1]), args[2].symbol && args[2].symbol->name == "shared");
            return Value{};
        }
        if (n == "define") {
            target_of(0).defines.emplace_back(static_text(args[1]), static_text(args[2]));
            return Value{};
        }
        if (n == "add_mode" || n == "set_mode") {
            const Value& m = args.at(n == "add_mode" ? 0 : 1);
            const Symbol* settings = nullptr;
            if (auto mm = program.modules.find("std::build::mode"); mm != program.modules.end()) {
                settings = mm->second->item("std::build::mode::settings");
            }
            const Type* t = m.kind == Value::Kind::type ? m.type_value : nullptr;
            bool ok = t && t->is(Kind::user) && t->symbol->kind == SymbolKind::class_;
            bool implements = false;
            for (const Symbol* cls = ok ? t->symbol : nullptr; cls && !implements; cls = cls->base_class()) {
                for (const Symbol* b : cls->bases) implements = implements || b == settings;
            }
            if (!ok || !implements) {
                failed(c.args.back()->span, "a mode is a class that derives from a std::build::mode class", "not a mode");
            }
            if (n == "set_mode" && ok && implements) {
                target_of(0).mode              = t->symbol;
                // That mode's optimization(): its method, as Evaluator::mode_setting runs it.
                const Symbol* method = nullptr;
                for (const Symbol* cls = t->symbol; cls && !method; cls = cls->base_class()) {
                    auto it = cls->members.find("optimization");
                    const FunctionDecl* f = it != cls->members.end() && it->second.front()->kind == SymbolKind::method
                                                ? body_decl(it->second.front()) : nullptr;
                    if (f && f->body) {
                        method = it->second.front();
                    }
                }
                if (method) {
                    Value mode_self;
                    mode_self.kind = Value::Kind::aggregate;
                    mode_self.type = tc.user(t->symbol);
                    target_of(0).mode_optimization = std::make_shared<const Value>(run_values(method, mode_self, {}, method->span));
                }
            }
            else g->modes.push_back(t->symbol);
            return Value{};
        }
        auto [setting_key, setting_short] = c.args.empty() ? std::pair<std::string, std::string>{}
                                                          : setting_name(*c.args[n == "option" ? 0 : n == "set" ? 1 : n == "set_for_files" ? 2 : 0]);
        if (n == "option" && !setting_key.empty()) {   // `g.option(setting, default, help)`: -D<short name>
            const Type* t = type_of(c);
            Value       v = args.at(1);
            if (auto it = g->given.find(setting_short); it != g->given.end()) v = option_value(it->second, t, c.span, setting_short);
            g->options.push_back(BuildOption{setting_short, static_text(args.at(2)), types::to_string(t), to_string(v)});
            return v;
        }
        if (n == "output" || n == "details") {   // how the build reports itself (build-output.md)
            auto member = [&](const Value& v, std::string_view wanted) -> const Value* {
                if (!v.type || !v.type->is(Kind::user)) return nullptr;
                std::size_t i = 0;
                for (const Symbol* m : v.type->symbol->member_order) {
                    if (m->kind != SymbolKind::field || m->is_static) continue;
                    if (m->name == wanted) return i < v.elements.size() ? &v.elements[i] : nullptr;
                    ++i;
                }
                return nullptr;
            };
            auto names = [](const Value* list) {
                std::vector<std::string> out;
                if (!list) return out;
                for (const Value& e : list->elements) {
                    if (e.symbol) out.push_back(e.symbol->name);
                }
                return out;
            };
            if (n == "details") {
                BuildTarget& t = target_of(0);
                t.details      = names(&args.at(1));
                t.has_details  = true;
                return Value{};
            }
            const Value& o   = args.at(0);
            BuildOutput& out = g->output;
            out.set          = true;
            if (const Value* p = member(o, "progress"); p && p->symbol) out.progress = p->symbol->name;
            out.details = names(member(o, "details"));
            if (const Value* l = member(o, "log_file"); l && l->kind == Value::Kind::boolean) out.log_file = l->boolean;
            if (const Value* k = member(o, "keep_logs"); k && k->kind == Value::Kind::integer) out.keep_logs = static_cast<unsigned>(k->integer);
            if (const Value* x = member(o, "target_line")) out.target_line = static_text(*x);
            if (const Value* x = member(o, "done_line")) out.done_line = static_text(*x);
            if (const Value* x = member(o, "source_row")) out.source_row = static_text(*x);
            if (const Value* x = member(o, "last_line")) out.last_line = static_text(*x);
            if (const Value* x = member(o, "failed_line")) out.failed_line = static_text(*x);
            return Value{};
        }
        if (n == "target_cpu") {   // g.target_cpu(t, level): the driver compiles t for the level (spec 34)
            const Value& level = args.at(1);
            if (!level.symbol) failed(c.args[1]->span, "'target_cpu' takes a 'std::build::cpu' level", "not a CPU level");
            target_of(0).settings["std::build::cpu"] = std::make_shared<const Value>(level);
            return Value{};
        }
        if (n == "set_global_allocator") {
            const Value& a = args.at(1);
            if (a.kind == Value::Kind::string) {   // a type of the program, by name
                target_of(0).settings["std::global_allocator"] = std::make_shared<const Value>(string_value(a.string));
                return Value{};
            }
            const Type* t = a.kind == Value::Kind::type ? a.type_value : nullptr;
            if (!t || !t->is(Kind::user)) failed(c.args[1]->span, "'set_global_allocator' takes an allocator type", "not a type");
            target_of(0).settings["std::global_allocator"] = std::make_shared<const Value>(string_value(types::qualified_name(t)));
            return Value{};
        }
        if (n == "set" && !setting_key.empty()) {
            target_of(0).settings[setting_key] = std::make_shared<const Value>(args.at(2));
            return Value{};
        }
        if (n == "option") {
            std::string name = static_text(args.at(0));
            const Type* t    = type_of(c);
            Value       v    = args.at(1);
            if (auto it = g->given.find(name); it != g->given.end()) v = option_value(it->second, t, c.span, name);
            g->options.push_back(BuildOption{name, static_text(args.at(2)), types::to_string(t), to_string(v)});
            return v;
        }
        if (n == "set") {
            BuildTarget& t = target_of(0);
            t.settings[static_text(args.at(1))] = std::make_shared<const Value>(args.at(2));
            return Value{};
        }
        if (n == "set_for_files" || n == "optimize_files") {   // a setting for the files of `t` matching a pattern (spec 31, 34)
            namespace fs = std::filesystem;
            BuildTarget&    t       = target_of(0);
            std::string     pattern = static_text(args.at(1));
            std::regex      re(glob_regex(pattern));
            std::error_code ec;
            fs::path base = fs::path(t.base.empty() ? g->root : t.base) / t.root;
            if (!fs::is_directory(base, ec)) base = base.parent_path();
            // optimize_files reaches every module the target compiles, so it is relative to the
            // project (the target's package root), not to the target's own directory.
            if (n == "optimize_files") base = fs::path(t.base.empty() ? g->root : t.base);
            sema::FileSetting fsetting;
            if (n == "optimize_files") {   // g.optimize_files(t, pattern, level): the backend reads it
                fsetting.name  = "std::build::optimization";
                fsetting.value = std::make_shared<const Value>(args.at(2));
                if (pattern.starts_with("<std>/")) {   // the standard library's files, wherever it is installed (spec 34)
                    fsetting.std_regex = glob_regex(pattern.substr(6));
                    t.file_settings.push_back(std::move(fsetting));
                    return Value{};
                }
            } else {
                fsetting.name  = setting_key.empty() ? static_text(args.at(2)) : setting_key;
                fsetting.value = std::make_shared<const Value>(args.at(3));
            }
            for (auto it = fs::recursive_directory_iterator(base, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (it->is_directory(ec)) {
                    if (it->path().filename().string().starts_with(".") || it->path() == fs::path(g->out_dir)) it.disable_recursion_pending();
                    continue;
                }
                std::string rel = it->path().lexically_relative(base).generic_string();
                if (std::regex_match(rel, re)) fsetting.files.push_back(fs::weakly_canonical(it->path(), ec).generic_string());
            }
            if (fsetting.files.empty()) {
                failed(c.args[1]->span, std::format("\"{}\" matches no file of target '{}' (relative to '{}')", pattern, t.name,
                                                    base.lexically_relative(g->root).generic_string()),
                       "no such file");
            }
            g->globs[pattern] = fsetting.files;
            t.file_settings.push_back(std::move(fsetting));
            return Value{};
        }
        auto path_text = [&](std::size_t i) -> std::string {
            const Value& v = args.at(i);
            if (v.kind == Value::Kind::string) return v.string;
            return v.kind == Value::Kind::aggregate && !v.elements.empty() ? v.elements[0].string : std::string();
        };
        auto step_of = [&](std::size_t i) -> BuildStep& {
            u128 id = args.at(i).kind == Value::Kind::aggregate && !args[i].elements.empty() ? args[i].elements[0].integer : 0;
            if (id == 0 || id > g->steps.size()) failed(c.args[i]->span, "not a step of this build", "unknown step");
            return g->steps[static_cast<std::size_t>(id - 1)];
        };
        bool package_method = s->parent && s->parent->name == "package" && self;   // `pkg.target(...)`, `pkg.glob(...)`
        if (package_method) {
            args.insert(args.begin(), *self);
        }
        if (package_method && n == "target" && [&] {   // a described package: its exports (below)
                u128 id = args.at(0).elements.empty() ? 0 : args[0].elements[0].integer;
                return id == 0 || id > g->dependencies.size() || g->dependencies[static_cast<std::size_t>(id - 1)].exports.empty();
            }()) {
            u128 id = args.at(0).elements.empty() ? 0 : args[0].elements[0].integer;
            if (id == 0 || id > g->dependencies.size()) failed(c.span, "not a package of this build", "unknown package");
            BuildTarget t;
            t.kind       = BuildTarget::Kind::foreign;
            t.name       = static_text(args.at(1));
            t.dependency = static_cast<std::size_t>(id);
            t.span       = c.span;
            g->targets.push_back(std::move(t));
            return handle(g->targets.size());
        }
        auto package_of = [&]() -> BuildDependency& {
            u128 id = args.at(0).elements.empty() ? 0 : args[0].elements[0].integer;
            if (id == 0 || id > g->dependencies.size()) failed(c.span, "not a package of this build", "unknown package");
            return g->dependencies[static_cast<std::size_t>(id - 1)];
        };
        if (package_method && (n == "files" || n == "at")) {   // `p.files([...])`, `p.at("include")`
            namespace fs = std::filesystem;
            BuildDependency& d = package_of();
            std::error_code ec;
            auto inside = [&](const std::string& rel, Span where) {
                fs::path full = (fs::path(d.dir) / rel).lexically_normal();
                if (fs::path(rel).is_absolute() || rel.starts_with("..")) {
                    failed(where, std::format("'{}' is not a path inside the package '{}'", rel, d.name), "outside the package");
                }
                return full;
            };
            if (n == "at") return string_value(inside(static_text(args.at(1)), c.args.at(0)->span).generic_string());
            Value list;
            list.kind = Value::Kind::list;
            list.type = type_of(c);
            const Type* path_type = nullptr;
            if (auto bm = program.modules.find("std::build"); bm != program.modules.end()) {
                if (const Symbol* p = bm->second->item("std::build::path")) path_type = tc.user(p);
            }
            for (const Value& f : args.at(1).elements) {
                fs::path full = inside(static_text(f), c.args.at(0)->span);
                if (!fs::is_regular_file(full, ec)) {
                    failed(c.args.at(0)->span, std::format("the package '{}' has no file '{}'", d.name, static_text(f)), "no such file");
                }
                list.elements.push_back(path_value(full.generic_string(), path_type));
            }
            return list;
        }
        if (package_method && n == "export_target") {   // in a package description (packages, section 3)
            BuildDependency& d = package_of();
            BuildTarget&     t = target_of(1);
            std::string name = args.size() > 2 ? static_text(args[2]) : std::string();
            if (name.empty()) name = t.name;
            if (d.exports.contains(name)) failed(c.span, std::format("the package '{}' already exports a target '{}'", d.name, name), "exported twice");
            if (!t.name.starts_with(d.name + ":")) t.name = d.name + ":" + t.name;
            d.exports[name] = static_cast<std::size_t>(args[1].elements[0].integer - 1);
            return Value{};
        }
        if (package_method && n == "target" && !package_of().exports.empty()) {   // a described package's export
            BuildDependency& d = package_of();
            std::string name = static_text(args.at(1));
            auto it = d.exports.find(name);
            if (it == d.exports.end()) {
                std::string list;
                for (const auto& [k, v] : d.exports) list += (list.empty() ? "" : ", ") + k;
                failed(c.args.at(0)->span, std::format("the package '{}' has no target '{}' (its targets: {})", d.name, name, list),
                       "unknown target");
            }
            return handle(it->second + 1);
        }
        if (n == "package" && !package_method) {   // g.package(spec) (packages)
            BuildDependency d;
            d.is_package       = true;
            d.span             = c.span;
            const Value& spec  = args.at(0);
            const Value& src   = field(spec, "source");
            std::string  kind  = src.type && src.type->is(Kind::user) ? src.type->symbol->name : "";
            if (kind != "git" && kind != "local" && kind != "archive") failed(c.args[0]->span, "a package's '.source' is a std::build::git, archive or local", "no source");
            d.kind = kind;
            if (kind == "git") {
                d.source = static_text(field(src, "url"));
                d.commit = static_text(field(src, "commit"));
                d.tag    = static_text(field(src, "tag"));
                if (d.commit.empty() == d.tag.empty()) failed(c.args[0]->span, "a git package names a '.commit' or a '.tag' (never a branch)", "no version");
                if (!d.commit.empty() && (d.commit.size() < 7 || d.commit.find_first_not_of("0123456789abcdef") != std::string::npos)) {
                    failed(c.args[0]->span, std::format("'{}' is not a commit hash", d.commit), "not a commit");
                }
            } else if (kind == "archive") {
                d.source = static_text(field(src, "url"));
                d.sha256 = static_text(field(src, "sha256"));
                if (d.sha256.size() != 64) failed(c.args[0]->span, "an archive package needs its '.sha256' (64 hex digits)", "no checksum");
            } else {
                d.source = static_text(field(src, "path"));
            }
            d.name = static_text(field(spec, "name"));
            if (d.name.empty()) {
                std::string last = d.source;
                while (!last.empty() && last.back() == '/') last.pop_back();
                last = last.substr(last.find_last_of('/') + 1);
                for (const char* ext : {".git", ".tar.gz", ".tar.xz", ".tgz", ".zip"}) {
                    if (last.ends_with(ext)) last = last.substr(0, last.size() - std::string(ext).size());
                }
                d.name = last;
            }
            for (const BuildDependency& other : g->dependencies) {
                if (other.is_package && other.name == d.name) failed(c.args[0]->span, std::format("there is already a package '{}'", d.name), "duplicate");
            }
            d.path = static_text(field(spec, "path"));
            if (d.path.empty() && kind != "local") d.path = "build/packages/" + d.name;
            const Value& how = field(spec, "build");
            d.build          = how.symbol ? how.symbol->name : "automatic";
            if (d.build == "cmake_external" && !how.elements.empty()) {
                const Value& outputs = how.elements[0];
                for (const Value& o : field(outputs, "outputs").elements) d.external_outputs.push_back(static_text(o));
                for (const Value& o : field(outputs, "include_dirs").elements) d.external_include_dirs.push_back(static_text(o));
                if (d.external_outputs.empty() && d.external_include_dirs.empty()) {
                    failed(c.args[0]->span, "cmake_external names the files it uses: '.outputs' and '.include_dirs'", "no outputs");
                }
            } else if (!how.elements.empty()) {
                d.build_arg = static_text(how.elements[0]);
            }
            for (const Value& o : field(spec, "options").elements) {
                if (o.elements.size() == 2) d.options.emplace_back(static_text(o.elements[0]), static_text(o.elements[1]));
            }
            for (const Value& u : field(spec, "uses").elements) {
                if (u.elements.size() != 2 || u.elements[1].elements.empty()) continue;
                d.uses.emplace_back(static_text(u.elements[0]), static_cast<std::size_t>(u.elements[1].elements[0].integer - 1));
            }
            for (const Value& p : field(spec, "patches").elements) d.patches.push_back(p.kind == Value::Kind::string ? p.string : static_text(p));
            if (!g->fetch) failed(c.span, "packages cannot be fetched here", "no fetch");
            std::string error;
            d.dir = g->fetch(d, error);
            if (d.dir.empty()) failed(c.span, std::format("cannot fetch the package '{}': {}", d.name, error), "fetch failed");
            // A description in the project (packages, section 3): `packages/<name>.ed`, unless the
            // package has a build.ed of its own.
            std::error_code ec;
            if (d.build == "automatic" && !std::filesystem::is_regular_file(std::filesystem::path(d.dir) / "build.ed", ec) &&
                std::filesystem::is_regular_file(std::filesystem::path(g->root) / "packages" / (d.name + ".ed"), ec)) {
                d.build     = "script";
                d.build_arg = "packages/" + d.name + ".ed";
            }
            g->dependencies.push_back(d);
            Value v;
            v.kind = Value::Kind::aggregate;
            v.type = type_of(c);
            v.elements.push_back(integer(g->dependencies.size(), tc.builtin("u32")));
            v.elements.push_back(string_value(kind == "local" ? d.source : d.path));
            if (d.build == "script") {
                std::string file = std::filesystem::path(d.build_arg).lexically_normal().generic_string();
                if (!file.starts_with("packages/") || !file.ends_with(".ed")) {
                    failed(c.args[0]->span, std::format("a package description is a .ed file under packages/ ('{}')", d.build_arg), "not a description");
                }
                std::string module = file.substr(0, file.size() - 3);
                for (std::size_t at; (at = module.find('/')) != std::string::npos;) module.replace(at, 1, "::");
                auto mod = program.modules.find(module);
                const Symbol* describe = nullptr;
                if (mod != program.modules.end()) {
                    for (const Symbol* item : mod->second->item_order) {
                        if (item->name == "describe" && item->kind == SymbolKind::function) describe = item;
                    }
                }
                if (!describe) {
                    failed(c.args[0]->span, std::format("{} has no 'compile_time void describe(mut std::build::graph& g, std::build::package& p)'", file),
                           "no description");
                }
                Value graph;
                graph.kind = Value::Kind::aggregate;
                if (auto bm = program.modules.find("std::build"); bm != program.modules.end()) {
                    if (const Symbol* gs = bm->second->item("std::build::graph")) graph.type = tc.user(gs);
                }
                (void)run_values(describe, std::nullopt, {graph, v}, c.span);
                if (g->dependencies.back().exports.empty()) {
                    failed(c.span, std::format("{} exports no target (p.export_target(t))", file), "nothing exported");
                }
            }
            return v;
        }
        if (n == "glob" || n == "glob_in") {
            namespace fs = std::filesystem;
            bool        in_dep  = n == "glob_in" || package_method;   // in a dependency: absolute paths
            std::string pattern = static_text(args.at(in_dep ? 1 : 0));
            std::regex  re(glob_regex(pattern));
            std::vector<std::string> found;
            std::error_code ec;
            fs::path root(g->root);
            if (in_dep) {
                u128 id = args.at(0).elements.empty() ? 0 : args[0].elements[0].integer;
                if (id == 0 || id > g->dependencies.size()) failed(c.args[0]->span, "not a dependency of this build", "unknown project");
                root = g->dependencies[static_cast<std::size_t>(id - 1)].dir;
            }
            for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                fs::path p = it->path();
                if (it->is_directory(ec)) {
                    std::string name = p.filename().string();
                    if (name.starts_with(".") || p == fs::path(g->out_dir)) it.disable_recursion_pending();
                    continue;
                }
                std::string rel = p.lexically_relative(root).generic_string();
                if (std::regex_match(rel, re)) found.push_back(in_dep ? p.generic_string() : rel);
            }
            std::sort(found.begin(), found.end());
            g->globs[pattern] = found;
            Value list;
            list.kind = Value::Kind::list;
            list.type = type_of(c);
            const Type* path_type = nullptr;
            if (auto bm = program.modules.find("std::build"); bm != program.modules.end()) {
                if (const Symbol* p = bm->second->item("std::build::path")) path_type = tc.user(p);
            }
            for (const std::string& f : found) list.elements.push_back(path_value(f, path_type));
            return list;
        }
        if (n == "find_program") {
            std::string name = static_text(args.at(0));
            std::string found;
            if (name.find('/') != std::string::npos) {
                found = name;
            } else if (const char* path = std::getenv("PATH")) {
                std::string_view dirs(path);
                while (!dirs.empty() && found.empty()) {
                    std::size_t colon = dirs.find(':');
                    std::string dir(dirs.substr(0, colon));
                    std::filesystem::path candidate = std::filesystem::path(dir.empty() ? "." : dir) / name;
                    std::error_code ec;
                    if (std::filesystem::is_regular_file(candidate, ec) && access(candidate.c_str(), X_OK) == 0) found = candidate.string();
                    dirs = colon == std::string_view::npos ? std::string_view{} : dirs.substr(colon + 1);
                }
            }
            if (found.empty()) failed(c.span, std::format("the program '{}' is not on the PATH", name), "not found");
            g->programs[name] = found;
            Value v;
            v.kind = Value::Kind::aggregate;
            v.type = type_of(c);
            v.elements.push_back(string_value(found));
            return v;
        }
        if (n == "output_path") return path_value((std::filesystem::path(g->out_dir) / "gen" / path_text(0)).string(), type_of(c));
        if (n == "add_step") {
            BuildStep st;
            st.name    = static_text(field(args.at(0), "name"));
            const Value& prog_value = field(args.at(0), "program");
            st.program = prog_value.elements.empty() ? std::string() : prog_value.elements[0].string;
            if (st.program.empty()) failed(c.args[0]->span, "a step needs a '.program' (g.find_program(...))", "no program");
            st.span = c.span;
            g->steps.push_back(std::move(st));
            return handle(g->steps.size());
        }
        if (n == "add_argument") {
            step_of(0).arguments.push_back(path_text(1));
            return Value{};
        }
        auto list_texts = [&](std::size_t i) {   // a list of texts or paths: `[a, "-o", b.text]`, g.glob(...)
            std::vector<std::string> out;
            const Value& v = args.at(i);
            if (v.kind != Value::Kind::list) {
                out.push_back(path_text(i));
                return out;
            }
            for (const Value& e : v.elements) {
                out.push_back(e.kind == Value::Kind::string ? e.string : !e.elements.empty() ? e.elements[0].string : std::string());
            }
            return out;
        };
        if (n == "add_arguments" || n == "add_inputs" || n == "add_outputs") {
            BuildStep& st = step_of(0);
            auto&      to = n == "add_arguments" ? st.arguments : n == "add_inputs" ? st.inputs : st.outputs;
            for (std::string& t : list_texts(1)) to.push_back(std::move(t));
            return Value{};
        }
        if (n == "add_include_dirs") {
            BuildTarget& t   = target_of(0);
            std::string  vis = args.at(2).symbol ? args[2].symbol->name : "own";
            for (std::string& d : list_texts(1)) {
                if (vis == "users") t.user_include_dirs.push_back(std::move(d));
                else t.include_dirs.emplace_back(std::move(d), vis == "shared");
            }
            return Value{};
        }
        if (n == "add_input" || n == "add_output") {
            (n == "add_input" ? step_of(0).inputs : step_of(0).outputs).push_back(path_text(1));
            return Value{};
        }
        if (n == "depends_on") {
            BuildTarget& t = target_of(0);
            step_of(1);
            t.steps.push_back(static_cast<std::size_t>(args[1].elements[0].integer - 1));
            return Value{};
        }
        if (n == "add_system_library") {
            BuildTarget t;
            t.kind         = BuildTarget::Kind::system_library;
            t.name         = static_text(field(args.at(0), "name"));
            t.library_root = static_text(field(args.at(0), "root"));
            t.span         = c.span;
            if (t.name.empty()) failed(c.args[0]->span, "a system library needs a '.name'", "no name");
            g->targets.push_back(std::move(t));
            return handle(g->targets.size());
        }
        if (n == "dependency") {
            BuildDependency d;
            const Value& spec = args.at(0);
            bool is_git = spec.type && spec.type->is(Kind::user) && spec.type->symbol->name == "git";
            d.kind = is_git ? "git" : "local";
            if (is_git) {
                d.source = static_text(field(spec, "url"));
                d.commit = static_text(field(spec, "commit"));
                bool hex = d.commit.size() >= 7 && d.commit.find_first_not_of("0123456789abcdef") == std::string::npos;
                if (!hex) failed(c.args[0]->span, std::format("'{}' is not a commit: dependencies are pinned to a commit hash, never a branch", d.commit),
                                 "not a commit");
            } else {
                d.source = static_text(field(spec, "path"));
            }
            if (!g->fetch) failed(c.span, "dependencies cannot be fetched here", "no fetch");
            std::string error;
            d.dir = g->fetch(d, error);
            if (d.dir.empty()) failed(c.span, std::format("cannot fetch the dependency '{}': {}", d.source, error), "fetch failed");
            g->dependencies.push_back(std::move(d));
            return handle(g->dependencies.size());
        }
        if (n == "target_of") {
            u128 id = args.at(0).elements.empty() ? 0 : args[0].elements[0].integer;
            if (id == 0 || id > g->dependencies.size()) failed(c.args[0]->span, "not a dependency of this build", "unknown project");
            BuildTarget t;
            t.kind       = BuildTarget::Kind::foreign;
            t.name       = static_text(args.at(1));
            t.dependency = static_cast<std::size_t>(id);
            t.span       = c.span;
            g->targets.push_back(std::move(t));
            return handle(g->targets.size());
        }
        if (n == "host") return platform_value(host_platform(), type_of(c));
        if (n == "target_platform") return platform_value(target_platform(), type_of(c));
        if (n == "env") {
            std::string name = static_text(args.at(0));
            const char* value = std::getenv(name.c_str());
            g->env_read[name] = value ? value : "";
            return string_value(value ? value : "");
        }
        failed(c.span, std::format("'{}' is not supported by edaxc's build interpreter yet", n), "unsupported");
    }

    Value run(const Symbol* s, std::optional<Value> self, const CallExpr& c) {
        if (in_build_module(s) && (s->kind == SymbolKind::function || s->kind == SymbolKind::method)) return build_intrinsic(s, self, c);
        const FunctionDecl* f = body_decl(s);
        if (!f || !f->body) not_constant(c.span, std::format("'{}' has no body to run at compile time", s->name));
        if (!f->is_compile_time && !is_mode_class(s->parent)) {
            not_constant(c.span, std::format("'{}' is not a compile_time function", s->name),
                         std::format("mark it 'compile_time' (or 'compile_time run_time') to call it at compile time"));
        }
        std::vector<Value> args;
        for (const ExprPtr& a : c.args) args.push_back(eval(*a));
        // `mut T&` parameters and `mut` methods change the caller's place: the values are
        // copied in, and copied back out when the call returns (spec 15).
        std::vector<Value>   finals;
        std::optional<Value> final_self;
        Value result = run_values(s, std::move(self), std::move(args), c.span, &finals, &final_self);
        for (std::size_t i = 0; i < f->params.size() && i < c.args.size() && i < finals.size(); ++i) {
            const TypeExpr& pt = *f->params[i].type;
            if (pt.kind != TypeKind::reference || !pt.as<ReferenceType>().is_mut) continue;
            const Expr* arg = c.args[i].get();
            if (arg->kind == ExprKind::unary && arg->as<UnaryExpr>().op == UnaryOp::mut_marker) arg = arg->as<UnaryExpr>().operand.get();
            if (is_place_expr(*arg)) place(*arg) = finals[i];
        }
        if (f->is_mut && final_self && c.callee->kind == ExprKind::member) {
            const Expr& object = *c.callee->as<MemberExpr>().object;
            if (is_place_expr(object)) place(object) = *final_self;
        }
        return result;
    }

    bool is_place_expr(const Expr& e) {
        if (e.kind == ExprKind::this_) return !frames.empty() && frames.back().self.has_value();
        if (e.kind == ExprKind::path) {
            const Symbol* s = e.as<PathExpr>().symbol;
            return !frames.empty() && frames.back().locals.count(s);
        }
        if (e.kind == ExprKind::member) return is_place_expr(*e.as<MemberExpr>().object);
        if (e.kind == ExprKind::index) return is_place_expr(*e.as<IndexExpr>().object);
        return false;
    }

    // A build mode's methods run at compile time without being marked (spec 24): the
    // interpreter is what reads a mode's settings.
    static bool is_mode_class(const Symbol* cls) {
        for (const Symbol* c = cls; c && (c->kind == SymbolKind::class_); c = c->base_class()) {
            if (c->module && c->module->name == "std::build::mode") return true;
        }
        return false;
    }

    Value run_values(const Symbol* s, std::optional<Value> self, std::vector<Value> args, Span call_site,
                     std::vector<Value>* finals = nullptr, std::optional<Value>* final_self = nullptr) {
        const FunctionDecl* f = body_decl(s);
        if (!f || !f->body) not_constant(call_site, std::format("'{}' has no body to run at compile time", s->name));
        if (!hooks.check_function(s)) unknown();
        if (frames.size() >= max_depth) {
            failed(call_site, std::format("compile-time calls are nested more than {} deep", max_depth), "too deep");
        }
        Frame frame;
        frame.function  = s;
        frame.call_site = call_site;
        frame.self      = std::move(self);
        // Type-valued parameters are read through their symbol when a type is lowered.
        std::vector<std::pair<const Symbol*, std::shared_ptr<const Value>>> saved;
        for (std::size_t i = 0; i < f->params.size() && i < args.size(); ++i) {
            const Symbol* p = f->params[i].symbol;
            if (!p) continue;
            frame.locals[p] = args[i];
            if (p->is_type_value) {
                saved.emplace_back(p, p->ct_value);
                const_cast<Symbol*>(p)->ct_value = std::make_shared<const Value>(args[i]);
            }
        }
        frames.push_back(std::move(frame));
        auto restore = [&] {
            frames.pop_back();
            for (auto& [p, v] : saved) const_cast<Symbol*>(p)->ct_value = v;
        };
        Value result;
        try {
            frames.back().result = f->result ? lower(*f->result) : tc.void_();
            Flow flow = run_block(*f->body);
            if (flow == Flow::normal && f->body->tail) result = returned;
            if (flow == Flow::return_) result = returned;
        } catch (...) {
            restore();
            throw;
        }
        if (finals) {
            for (std::size_t i = 0; i < f->params.size(); ++i) {
                auto it = f->params[i].symbol ? frames.back().locals.find(f->params[i].symbol) : frames.back().locals.end();
                finals->push_back(it != frames.back().locals.end() ? it->second : Value{});
            }
        }
        if (final_self) *final_self = frames.back().self;
        restore();
        return result;
    }

    // --- blocks and control flow -------------------------------------------------------

    // Runs a block as a statement. A tail value is left in `returned` only for function bodies.
    Flow run_block(const BlockExpr& b) {
        try {
            for (const StmtPtr& s : b.statements) statement(*s);
            if (b.tail) returned = eval(*b.tail);
        } catch (Flow f) {
            return f;
        }
        return Flow::normal;
    }

    Value block(const BlockExpr& b) {
        for (const StmtPtr& s : b.statements) statement(*s);
        return b.tail ? eval(*b.tail) : Value{};
    }

    void bind(const Symbol* s, Value v) {
        if (frames.empty()) {
            frames.emplace_back();   // top-level code: a frame without a function
        }
        frames.back().locals[s] = std::move(v);
    }

    void statement(const Stmt& s) {
        step(s.span);
        switch (s.kind) {
        case StmtKind::destructure: {
            const auto& d = s.as<DestructureStmt>();
            Value       v = eval(*d.init);
            if (v.kind != Value::Kind::aggregate || v.elements.size() != d.bindings.size()) unknown();
            for (std::size_t i = 0; i < d.bindings.size(); ++i) {
                if (d.bindings[i].symbol) bind(d.bindings[i].symbol, v.elements[i]);
            }
            return;
        }
        case StmtKind::variable: {
            const auto& v = s.as<VariableStmt>();
            if (!v.symbol) unknown();
            if (v.storage == Storage::compile_time && v.symbol->ct_value) {
                bind(v.symbol, *v.symbol->ct_value);
                return;
            }
            bind(v.symbol, v.init ? eval(*v.init) : default_of(lower(*v.type), v.span));
            return;
        }
        case StmtKind::assign: {
            const auto& a      = s.as<AssignStmt>();
            Value       value  = eval(*a.value);
            Value&      target = place(*a.target);
            if (a.op == TokenKind::eq) {
                target = std::move(value);
            } else {
                // compound_base is in check.cpp; map the few compound operators here.
                TokenKind op = TokenKind::eq;
                switch (a.op) {
                case TokenKind::plus_eq: op = TokenKind::plus; break;
                case TokenKind::minus_eq: op = TokenKind::minus; break;
                case TokenKind::star_eq: op = TokenKind::star; break;
                case TokenKind::slash_eq: op = TokenKind::slash; break;
                case TokenKind::percent_eq: op = TokenKind::percent; break;
                case TokenKind::amp_eq: op = TokenKind::amp; break;
                case TokenKind::pipe_eq: op = TokenKind::pipe; break;
                case TokenKind::caret_eq: op = TokenKind::caret; break;
                case TokenKind::shl_eq: op = TokenKind::shl; break;
                case TokenKind::shr_eq: op = TokenKind::shr; break;
                case TokenKind::plus_percent_eq: op = TokenKind::plus_percent; break;
                case TokenKind::minus_percent_eq: op = TokenKind::minus_percent; break;
                case TokenKind::star_percent_eq: op = TokenKind::star_percent; break;
                default: unknown();
                }
                if (a.distinct_underlying && target.kind == Value::Kind::aggregate && !target.elements.empty()) {   // spec 34
                    target.elements[0] = apply(op, target.elements[0], value, a.span, a.value->span);
                } else {
                    target = apply(op, target, value, a.span, a.value->span);
                }
            }
            return;
        }
        case StmtKind::discard: eval(*s.as<DiscardStmt>().value); return;
        case StmtKind::expression: eval(*s.as<ExprStmt>().expr); return;
        case StmtKind::check: run_check(s.as<CheckStmt>().check, s.span); return;
        }
    }

    // The storage a compile-time assignment target denotes.
    Value& place(const Expr& e) {
        if (e.kind == ExprKind::this_ && !frames.empty() && frames.back().self) return *frames.back().self;   // a `mut` method
        if (e.kind == ExprKind::path) {
            const Symbol* s = e.as<PathExpr>().symbol;
            if (!frames.empty()) {
                if (auto it = frames.back().locals.find(s); it != frames.back().locals.end()) return it->second;
            }
            not_constant(e.span, "only local variables of compile-time code can be assigned at compile time");
        }
        if (e.kind == ExprKind::member) {
            const auto& me = e.as<MemberExpr>();
            Value&      o  = place(*me.object);
            if (o.kind != Value::Kind::aggregate) unknown();
            return o.elements.at(field_index(o.type, me.name.segments.back().name, me.span));
        }
        if (e.kind == ExprKind::index) {
            const auto& ix = e.as<IndexExpr>();
            Value&      o  = elements_of(place(*ix.object));
            Value       i  = eval(*ix.index);
            if (o.kind != Value::Kind::list || i.integer >= o.elements.size()) {
                failed(ix.index->span, "index out of bounds at compile time", "out of bounds");
            }
            return o.elements[static_cast<std::size_t>(i.integer)];
        }
        not_constant(e.span, "this cannot be assigned at compile time");
    }

    Value if_value(const IfExpr& i) {
        bool take_then;
        if (i.is_compile_time && i.taken >= 0) {
            take_then = i.taken == 0;
            if (i.taken == 2) return Value{};
        } else {
            take_then = truth(eval(*i.condition));
        }
        if (take_then) return eval(*i.then_block);
        return i.else_branch ? eval(*i.else_branch) : Value{};
    }

    Value for_loop(const ForExpr& f) {
        if (f.is_compile_time) {
            for (const BlockPtr& copy : f.copies) {
                Flow flow = run_block(*copy);
                if (flow == Flow::break_) break;
                if (flow == Flow::return_) throw flow;
            }
            return Value{};
        }
        std::optional<std::vector<Value>> items = items_of(*f.iterable);
        for (Value& v : *items) {
            bind(f.symbol, v);
            Flow flow = run_block(*f.body);
            if (flow == Flow::break_) break;
            if (flow == Flow::return_) throw flow;
        }
        return Value{};
    }

    std::optional<std::vector<Value>> items_of(const Expr& iterable) {
        std::vector<Value> out;
        if (iterable.kind == ExprKind::range) {
            const auto& r  = iterable.as<RangeExpr>();
            Value       lo = eval(*r.lo);
            Value       hi = eval(*r.hi);
            if (lo.kind != Value::Kind::integer) unknown();
            bool sgn = lo.type->is_signed;
            for (Value v = lo;; v = integer(v.integer + 1, v.type)) {
                bool below = sgn ? v.as_signed() < hi.as_signed() : v.integer < hi.integer;
                bool equal_hi = v.integer == hi.integer;
                if (!(below || (r.inclusive && equal_hi))) break;
                out.push_back(v);
                step(iterable.span);
                if (equal_hi) break;
            }
            return out;
        }
        Value v = eval(iterable);
        if (v.kind != Value::Kind::list) not_constant(iterable.span, "a compile-time loop needs a list or a range");
        return v.elements;
    }

    Value match_value(const MatchExpr& m) {
        Value s = eval(*m.scrutinee);
        for (const MatchArm& arm : m.arms) {
            if (!matches(*arm.pattern, s)) continue;
            if (arm.guard && !truth(eval(*arm.guard))) continue;
            return eval(*arm.body);
        }
        failed(m.span, std::format("no match arm matches {} at compile time", to_string(s)), "no arm matches");
    }

    bool matches(const Pattern& p, const Value& v) {
        switch (p.kind) {
        case PatternKind::wildcard: return true;
        case PatternKind::tuple: {
            const auto& tp = p.as<TuplePattern>();
            if (v.elements.size() != tp.elements.size()) return false;
            for (std::size_t i = 0; i < tp.elements.size(); ++i) {
                if (!matches(*tp.elements[i], v.elements[i])) return false;
            }
            return true;
        }
        case PatternKind::binding:
            if (p.as<BindingPattern>().symbol) bind(p.as<BindingPattern>().symbol, v);
            return true;
        case PatternKind::literal: return equal(eval(*p.as<LiteralPattern>().literal), v);
        case PatternKind::range: {
            const auto& r = p.as<RangePattern>();
            bool        ok = true;
            if (r.lo) ok = ok && truth(apply(TokenKind::greater_eq, v, eval(*r.lo), p.span, p.span));
            if (r.hi) ok = ok && truth(apply(r.inclusive ? TokenKind::less_eq : TokenKind::less, v, eval(*r.hi), p.span, p.span));
            return ok;
        }
        case PatternKind::enum_alternative: {
            const auto& ep = p.as<EnumPattern>();
            if (v.kind != Value::Kind::enumeration || !ep.symbol || !v.symbol || ep.symbol->name != v.symbol->name) return false;
            return !ep.payload || v.elements.empty() || matches(*ep.payload, v.elements[0]);
        }
        case PatternKind::struct_: {
            const auto& sp = p.as<StructPattern>();
            if (v.kind != Value::Kind::aggregate) unknown();
            for (const FieldPattern& fp : sp.fields) {
                if (fp.pattern && !matches(*fp.pattern, v.elements.at(field_index(v.type, fp.name, fp.span)))) return false;
            }
            return true;
        }
        case PatternKind::error: unknown();
        }
        return false;
    }
};

// --- public interface ----------------------------------------------------------------------

Evaluator::Evaluator(sema::Program& program, DiagnosticEngine& diags, Hooks hooks)
    : impl_(std::make_unique<Impl>(program, diags, std::move(hooks))) {}

Evaluator::~Evaluator() = default;

std::shared_ptr<const Value> Evaluator::evaluate(const Expr& e, std::string_view why) {
    return evaluate_in(e, why, Context{});
}

std::shared_ptr<const Value> Evaluator::call(const Symbol* fn, std::vector<Value> args, Span call_site) {
    std::vector<Impl::Frame> saved = std::move(impl_->frames);
    impl_->frames.clear();
    impl_->steps = 0;
    std::shared_ptr<const Value> result;
    try {
        result = std::make_shared<const Value>(impl_->run_values(fn, std::nullopt, std::move(args), call_site));
    } catch (Stop) {
    } catch (Flow) {
    }
    impl_->frames = std::move(saved);
    return result;
}

std::shared_ptr<const Value> Evaluator::mode_setting(const Symbol* mode_class, const std::string& name) {
    const Symbol* method = nullptr;
    for (const Symbol* c = mode_class; c && !method; c = c->base_class()) {
        auto it = c->members.find(name);
        if (it != c->members.end() && it->second.front()->kind == SymbolKind::method && body_decl(it->second.front()) &&
            body_decl(it->second.front())->body) {
            method = it->second.front();
        }
    }
    if (!method) return nullptr;
    Value self;
    self.kind = Value::Kind::aggregate;
    self.type = impl_->tc.user(mode_class);
    std::vector<Impl::Frame> saved = std::move(impl_->frames);
    impl_->frames.clear();
    std::shared_ptr<const Value> result;
    try {
        result = std::make_shared<const Value>(impl_->run_values(method, self, {}, method->span));
    } catch (Stop) {
    } catch (Flow) {
    }
    impl_->frames = std::move(saved);
    return result;
}

std::shared_ptr<const Value> Evaluator::evaluate_in(const Expr& e, std::string_view why, Context context) {
    unknown_ = false;
    std::vector<Impl::Frame> saved_frames = std::move(impl_->frames);
    std::string saved_why       = std::move(impl_->why);
    bool        saved_quiet     = impl_->quiet;
    auto        saved_site      = impl_->site;
    auto        saved_parameter = std::move(impl_->parameter);
    auto        saved_known     = std::move(impl_->parameter_known);
    auto        saved_psite     = std::move(impl_->parameter_site);
    auto        saved_ptext     = std::move(impl_->parameter_text);
    impl_->frames.clear();
    if (context.function) {
        Impl::Frame top;
        top.function = context.function;
        top.call_site = context.site ? *context.site : e.span;
        impl_->frames.push_back(std::move(top));
    }
    impl_->why       = std::string(why);
    impl_->quiet     = context.quiet;
    impl_->site      = context.site;
    impl_->parameter = std::move(context.parameter);
    impl_->parameter_known = std::move(context.parameter_known);
    impl_->parameter_site  = std::move(context.parameter_site);
    impl_->parameter_text  = std::move(context.parameter_text);
    impl_->steps     = 0;
    impl_->pending_notes.clear();
    std::shared_ptr<const Value> result;
    try {
        result = std::make_shared<const Value>(impl_->eval(e));
    } catch (Stop s) {
        unknown_ = s.unknown;
    } catch (Flow) {
        unknown_ = true;
    }
    impl_->frames    = std::move(saved_frames);
    impl_->why       = std::move(saved_why);
    impl_->quiet     = saved_quiet;
    impl_->site      = saved_site;
    impl_->parameter = std::move(saved_parameter);
    impl_->parameter_known = std::move(saved_known);
    impl_->parameter_site  = std::move(saved_psite);
    impl_->parameter_text  = std::move(saved_ptext);
    return result;
}

std::optional<std::vector<Value>> Evaluator::elements(const Expr& iterable) {
    unknown_ = false;
    std::vector<Impl::Frame> saved_frames = std::move(impl_->frames);
    impl_->frames.clear();
    impl_->steps = 0;
    std::optional<std::vector<Value>> result;
    try {
        result = impl_->items_of(iterable);
    } catch (Stop s) {
        unknown_ = s.unknown;
    } catch (Flow) {
        unknown_ = true;
    }
    impl_->frames = std::move(saved_frames);
    return result;
}

} // namespace edax::ct
