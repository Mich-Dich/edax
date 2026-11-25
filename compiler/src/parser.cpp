#include "edax/parser.hpp"
#include "edax/lexer.hpp"

#include "edax/ast_printer.hpp"
#include "edax/ast_clone.hpp"

#include <algorithm>
#include <utility>
#include <deque>
#include <format>
#include <mutex>
#include <optional>

namespace edax {

namespace {

using namespace ast;

// Thrown after a syntax error has been reported (or silently while speculating). Caught at
// the nearest recovery point, which skips ahead to a synchronization token.
struct ParseError {};

enum class MemberContext { top_level, struct_, class_, interface };

struct Modifiers {
    Span                       start;
    std::string                doc;
    bool                       is_private      = false;
    bool                       is_static       = false;
    bool                       is_thread_local = false;
    bool                       is_compile_time = false;
    bool                       is_run_time     = false;
    bool                       is_unsafe       = false;
    bool                       is_extern       = false;
    bool                       is_open         = false;
    bool                       is_view         = false;
    bool                       is_virtual      = false;
    bool                       is_mut          = false;
    std::string                abi;
    bool                       is_template     = false;
    std::vector<TemplateParam> template_params;
    std::vector<AnnotationUse> annotations;          // `@name{...}` (spec 29)
    bool                       is_annotation   = false;   // `annotation(targets)` before `struct`
    std::vector<Path>          annotation_targets;
    Span                       annotation_span;
};

struct TypeOptions {
    bool allow_auto      = false;   // variable declarations
    bool allow_deduce    = false;   // function results
    bool allow_reference = true;    // false in `type T : a & b` constraints
    bool allow_function  = false;   // template arguments: `std::function<u32(u32)>`
};

// Operator precedence (spec 16), higher binds tighter. -1 = not a binary operator.
constexpr int prec_or = 1, prec_and = 2, prec_compare = 3, prec_range = 4, prec_bit_or = 5,
              prec_bit_xor = 6, prec_bit_and = 7, prec_shift = 8, prec_add = 9, prec_mul = 10;

int precedence(TokenKind k) {
    switch (k) {
    case TokenKind::pipe_pipe: return prec_or;
    case TokenKind::amp_amp: return prec_and;
    case TokenKind::eq_eq:
    case TokenKind::bang_eq:
    case TokenKind::less:
    case TokenKind::less_eq:
    case TokenKind::greater:
    case TokenKind::greater_eq:
    case TokenKind::spaceship: return prec_compare;
    case TokenKind::dot_dot:
    case TokenKind::dot_dot_eq: return prec_range;
    case TokenKind::pipe: return prec_bit_or;
    case TokenKind::caret: return prec_bit_xor;
    case TokenKind::amp: return prec_bit_and;
    case TokenKind::shl:
    case TokenKind::shr: return prec_shift;
    case TokenKind::plus:
    case TokenKind::minus:
    case TokenKind::plus_percent:
    case TokenKind::minus_percent: return prec_add;
    case TokenKind::star:
    case TokenKind::slash:
    case TokenKind::percent:
    case TokenKind::star_percent: return prec_mul;
    default: return -1;
    }
}

bool is_assignment_op(TokenKind k) {
    switch (k) {
    case TokenKind::eq:
    case TokenKind::plus_eq:
    case TokenKind::minus_eq:
    case TokenKind::star_eq:
    case TokenKind::slash_eq:
    case TokenKind::percent_eq:
    case TokenKind::amp_eq:
    case TokenKind::pipe_eq:
    case TokenKind::caret_eq:
    case TokenKind::shl_eq:
    case TokenKind::shr_eq:
    case TokenKind::plus_percent_eq:
    case TokenKind::minus_percent_eq:
    case TokenKind::star_percent_eq: return true;
    default: return false;
    }
}

bool is_literal(TokenKind k) {
    switch (k) {
    case TokenKind::int_literal:
    case TokenKind::float_literal:
    case TokenKind::char_literal:
    case TokenKind::string_literal:
    case TokenKind::raw_string_literal:
    case TokenKind::c_string_literal:
    case TokenKind::format_string_literal: return true;
    default: return false;
    }
}

bool starts_type(TokenKind k) {
    return k == TokenKind::identifier || is_type_keyword(k) || k == TokenKind::kw_type || k == TokenKind::colon_colon;
}

bool starts_expression(TokenKind k) {
    if (is_literal(k) || starts_type(k)) {
        return true;
    }
    switch (k) {
    case TokenKind::at:   // `@std::dispatch_hint<T> call`
    case TokenKind::kw_this:
    case TokenKind::kw_default:
    case TokenKind::kw_true:
    case TokenKind::kw_false:
    case TokenKind::kw_nullptr:
    case TokenKind::kw_if:
    case TokenKind::kw_match:
    case TokenKind::kw_cast:
    case TokenKind::kw_bit:
    case TokenKind::kw_move:
    case TokenKind::kw_mut:
    case TokenKind::kw_unsafe:
    case TokenKind::kw_return:
    case TokenKind::kw_break:
    case TokenKind::kw_continue:
    case TokenKind::l_paren:
    case TokenKind::l_bracket:
    case TokenKind::l_brace:
    case TokenKind::minus:
    case TokenKind::bang:
    case TokenKind::tilde:
    case TokenKind::star:
    case TokenKind::amp: return true;
    default: return false;
    }
}

bool is_block_like(const Expr& e) {
    switch (e.kind) {
    case ExprKind::if_:
    case ExprKind::match:
    case ExprKind::block:
    case ExprKind::while_:
    case ExprKind::for_: return true;
    default: return false;
    }
}

class Parser {
public:
    Parser(const SourceFile& file, FileId id, const std::vector<Token>& tokens, DiagnosticEngine& diags)
        : file_(file), id_(id), diags_(diags) {
        std::string pending_doc;
        for (const Token& t : tokens) {
            if (t.is(TokenKind::doc_comment)) {
                pending_doc += (pending_doc.empty() ? "" : "\n") + t.value;
                continue;
            }
            tokens_.push_back(t);
            docs_.push_back(std::move(pending_doc));
            pending_doc.clear();
        }
        if (tokens_.empty() || !tokens_.back().is(TokenKind::eof)) {
            Token eof;
            eof.span = Span{id, static_cast<std::uint32_t>(file.text().size()),
                            static_cast<std::uint32_t>(file.text().size())};
            tokens_.push_back(eof);
            docs_.emplace_back();
        }
    }

    File parse_file() {
        File f;
        f.file = id_;
        while (!at(TokenKind::eof)) {
            std::size_t before = pos_;
            try {
                parse_top_level(f.decls);
                stray_code_reported_ = false;
            } catch (const ParseError&) {
                synchronize(/*top_level=*/true);
            }
            if (pos_ == before) {
                advance();   // always make progress
            }
        }
        f.aliases     = std::move(aliases_);
        f.namespaces  = std::move(ns_blocks_);
        f.annotations = std::move(file_annotations_);
        return f;
    }

    ExprPtr parse_single_expression() {
        try {
            ExprPtr e = parse_expr();
            if (!at(TokenKind::eof)) {
                fail_unexpected("end of expression");
            }
            return e;
        } catch (const ParseError&) {
            return std::make_unique<SimpleExpr>(ExprKind::error, cur().span);
        }
    }

private:
    // --- token access ---------------------------------------------------------------------

    const Token& cur() const { return split_ ? split_token_ : tokens_[pos_]; }
    const Token& peek(std::size_t n) const {
        if (n == 0) {
            return cur();
        }
        return tokens_[std::min(pos_ + n, tokens_.size() - 1)];
    }
    bool at(TokenKind k) const { return cur().kind == k; }

    Token advance() {
        Token t   = cur();
        prev_span_ = t.span;
        if (split_) {
            split_ = false;
            ++pos_;
        } else if (!t.is(TokenKind::eof)) {
            ++pos_;
        }
        return t;
    }

    bool accept(TokenKind k) {
        if (at(k)) {
            advance();
            return true;
        }
        return false;
    }

    Span from(Span start) const {
        return Span{id_, start.begin, std::max(start.begin, prev_span_.end)};
    }

    std::string take_doc() const { return split_ ? std::string() : docs_[pos_]; }

    // --- diagnostics ----------------------------------------------------------------------

    std::string describe(const Token& t) const {
        switch (t.kind) {
        case TokenKind::eof: return "end of file";
        case TokenKind::identifier: return std::format("identifier '{}'", t.text);
        case TokenKind::int_literal:
        case TokenKind::float_literal:
        case TokenKind::char_literal:
        case TokenKind::string_literal:
        case TokenKind::raw_string_literal:
        case TokenKind::c_string_literal:
        case TokenKind::format_string_literal: return std::format("{} {}", token_kind_spelling(t.kind), t.text);
        default:
            if (is_type_keyword(t.kind)) {
                return std::format("type '{}'", t.text);
            }
            if (is_keyword(t.kind)) {
                return std::format("keyword '{}'", t.text);
            }
            return std::format("'{}'", t.text);
        }
    }

    // Reports without unwinding (for errors the parser can step over). While speculating
    // nothing is reported; the returned scratch diagnostic just absorbs notes.
    Diagnostic& report(DiagCode code, Span span, std::string message, std::string label = {}) {
        if (speculating_ > 0) {
            scratch_ = Diagnostic{code, Severity::error, {}, span, {}, {}};
            return scratch_;
        }
        return diags_.report(code, span, std::move(message), std::move(label));
    }

    [[noreturn]] void fail(DiagCode code, Span span, std::string message, std::string label = {},
                           std::vector<Note> notes = {}) {
        report(code, span, std::move(message), std::move(label)).notes = std::move(notes);
        throw ParseError{};
    }

    [[noreturn]] void fail_unexpected(std::string_view expected) {
        fail(DiagCode::E2001, cur().span, std::format("expected {}, found {}", expected, describe(cur())),
             "unexpected here");
    }

    std::uint32_t line_of(std::uint32_t offset) const { return file_.line_col(offset).line; }

    // Where to point when a separator is missing: right after the previous token if the
    // current token is on a later line, otherwise at the current token.
    Span missing_token_span() const {
        if (pos_ > 0 && line_of(prev_span_.end) < line_of(cur().span.begin)) {
            return Span{id_, prev_span_.end, prev_span_.end};
        }
        return cur().span;
    }

    Token expect(TokenKind k, std::string_view context = {}) {
        if (at(k)) {
            return advance();
        }
        std::string want = std::format("'{}'", token_kind_spelling(k));
        std::string msg  = std::format("expected {}{}{}, found {}", want, context.empty() ? "" : " ",
                                       context, describe(cur()));
        if (k == TokenKind::semicolon || k == TokenKind::comma) {
            Span where = missing_token_span();
            if (where.size() == 0 && k == TokenKind::semicolon) {
                // Missing ';' at the end of a line: report it and carry on as if it were there,
                // so the next line is parsed normally instead of being skipped.
                report(DiagCode::E2001, where, msg, std::format("expected {} here", want));
                Token virtual_semicolon;
                virtual_semicolon.kind = TokenKind::semicolon;
                virtual_semicolon.span = where;
                return virtual_semicolon;
            }
            fail(DiagCode::E2001, where, msg,
                 where.size() == 0 ? std::format("expected {} here", want) : "unexpected here");
        }
        fail(DiagCode::E2001, cur().span, msg, "unexpected here");
    }

    // A field or method name: an identifier or a keyword (names reached through `.` or `::`).
    std::string expect_member_name(std::string_view what) {
        if (!at(TokenKind::identifier) && !is_keyword(cur().kind)) {
            fail_unexpected(what);
        }
        return std::string(advance().text);
    }

    // After a keyword used as a member name: what may follow a field or method name.
    bool peek_is_name_follower() const {
        TokenKind k = peek(1).kind;
        return k == TokenKind::eq || k == TokenKind::semicolon || k == TokenKind::l_paren;
    }

    std::string expect_identifier(std::string_view what) {
        if (!at(TokenKind::identifier)) {
            fail_unexpected(what);
        }
        return std::string(advance().text);
    }

    // Consumes a closing delimiter, pointing back at the opener when it is missing.
    void expect_close(TokenKind close, Span open) {
        if (accept(close)) {
            return;
        }
        std::string_view open_text = file_.text().substr(open.begin, open.size());
        if (at(TokenKind::eof)) {
            fail(DiagCode::E2016, open, std::format("unclosed '{}'", open_text),
                 std::format("this '{}' is never closed", open_text));
        }
        fail(DiagCode::E2001, cur().span,
             std::format("expected '{}', found {}", token_kind_spelling(close), describe(cur())),
             "unexpected here", {Note{std::format("to match this '{}'", open_text), open}});
    }

    // Closes template arguments. Splits `>>` into two `>` tokens when needed.
    void expect_template_close() {
        if (accept(TokenKind::greater)) {
            return;
        }
        if (at(TokenKind::shr) && !split_) {
            Token t        = cur();
            split_token_   = t;
            split_token_.kind = TokenKind::greater;
            split_token_.span.begin = t.span.begin + 1;
            split_token_.text = t.text.substr(1);
            prev_span_     = Span{id_, t.span.begin, t.span.begin + 1};
            split_         = true;
            return;
        }
        fail_unexpected("'>' to close the template arguments");
    }

    // --- speculation and recovery ---------------------------------------------------------

    struct SavedState {
        std::size_t pos;
        bool        split;
        Token       split_token;
        Span        prev;
    };
    SavedState save() const { return SavedState{pos_, split_, split_token_, prev_span_}; }
    void       restore(const SavedState& s) {
        pos_ = s.pos, split_ = s.split, split_token_ = s.split_token, prev_span_ = s.prev;
    }

    // Runs `f` without reporting. On failure restores the position and returns false.
    template <typename F>
    bool speculate(F&& f) {
        SavedState saved = save();
        ++speculating_;
        try {
            f();
            --speculating_;
            return true;
        } catch (const ParseError&) {
            --speculating_;
            restore(saved);
            return false;
        }
    }

    // Like speculate, but always rewinds: pure lookahead.
    template <typename F>
    bool lookahead(F&& f) {
        SavedState saved = save();
        bool       ok    = speculate(std::forward<F>(f));
        restore(saved);
        return ok;
    }

    // Skips to a statement / declaration boundary after an error: past the next `;` or the
    // next `}` that closes a group opened while skipping, or up to a `}` that closes the
    // enclosing block. Only braces are balanced; parentheses may be broken by the error.
    void synchronize(bool top_level) {
        int depth = 0;
        while (!at(TokenKind::eof)) {
            TokenKind k = cur().kind;
            if (depth == 0 && k == TokenKind::semicolon) {
                advance();
                return;
            }
            if (k == TokenKind::l_brace) {
                ++depth;
            } else if (k == TokenKind::r_brace) {
                if (depth == 0) {
                    if (top_level) {
                        advance();   // stray '}'
                    }
                    return;
                }
                --depth;
                if (depth == 0) {
                    advance();
                    if (!at(TokenKind::kw_else) && !at(TokenKind::semicolon)) {
                        return;
                    }
                    continue;
                }
            }
            advance();
        }
    }

    // After an error inside `( ... )`: skip to the matching `)` and consume it.
    // Stops early at `{`, `}` or `;` (unless `through_semicolons`), where the `)` is missing.
    void skip_past_close_paren(bool through_semicolons = false) {
        int depth = 0;
        while (!at(TokenKind::eof)) {
            TokenKind k = cur().kind;
            if (k == TokenKind::l_paren) {
                ++depth;
            } else if (k == TokenKind::r_paren) {
                if (depth == 0) {
                    advance();
                    return;
                }
                --depth;
            } else if (k == TokenKind::l_brace || k == TokenKind::r_brace ||
                       (k == TokenKind::semicolon && !through_semicolons)) {
                return;   // the ')' is missing; leave the rest to the caller
            }
            advance();
        }
    }

    // Skips a balanced `{...}` or `(...)` group starting at the current token.
    void skip_group() {
        TokenKind open  = cur().kind;
        TokenKind close = open == TokenKind::l_brace ? TokenKind::r_brace : TokenKind::r_paren;
        int       depth = 0;
        while (!at(TokenKind::eof)) {
            if (at(open)) {
                ++depth;
            } else if (at(close) && --depth == 0) {
                advance();
                return;
            }
            advance();
        }
    }

    // `const` and `fn` are gone (spec 01). Reports them and returns true for `const` (which
    // the caller skips and treats as `compile_time` at file scope).
    bool check_removed_keyword() {
        if (!at(TokenKind::identifier)) {
            return false;
        }
        TokenKind next = peek(1).kind;
        if (cur().text == "const" && starts_type(next)) {
            report(DiagCode::E2019, cur().span, "'const' is not a keyword in Edax",
                   "remove 'const'")
                .notes.push_back(Note{"values are immutable by default; for a compile-time "
                                       "constant write 'compile_time'",
                                       std::nullopt});
            advance();
            return true;
        }
        if (cur().text == "fn" && next == TokenKind::identifier && peek(2).kind == TokenKind::l_paren) {
            fail(DiagCode::E2019, cur().span, "'fn' is not a keyword in Edax",
                 "functions start with their return type",
                 {Note{"write e.g. 'void name(...)' or 'u32 name(...)'", std::nullopt}});
        }
        return false;
    }

    // --- declarations ---------------------------------------------------------------------

    Modifiers parse_modifiers(MemberContext ctx) {
        Modifiers m;
        m.start = cur().span;
        m.doc   = take_doc();
        while (true) {
            switch (cur().kind) {
            case TokenKind::at: m.annotations.push_back(parse_annotation_use()); continue;
            case TokenKind::kw_template: parse_template_header(m); continue;
            case TokenKind::kw_private:
                if (ctx != MemberContext::top_level) {
                    return finish_modifiers(m);   // `private:` access label
                }
                m.is_private = true;
                break;
            case TokenKind::kw_static: m.is_static = true; break;
            case TokenKind::kw_thread_local: m.is_thread_local = true; break;
            case TokenKind::kw_compile_time: m.is_compile_time = true; break;
            case TokenKind::kw_run_time: m.is_run_time = true; break;
            case TokenKind::kw_unsafe: m.is_unsafe = true; break;
            case TokenKind::identifier:   // `open` is a keyword only before `class` (spec 01, 07)
                if (cur().text == "annotation" && peek(1).kind == TokenKind::l_paren && ctx == MemberContext::top_level) {
                    parse_annotation_targets(m);
                    continue;
                }
                if (cur().text == "open" && (peek(1).kind == TokenKind::kw_class || peek(1).kind == TokenKind::kw_struct ||
                                             peek(1).kind == TokenKind::kw_view)) {
                    m.is_open = true;
                    break;
                }
                return finish_modifiers(m);
            case TokenKind::kw_view: m.is_view = true; break;
            case TokenKind::kw_virtual: m.is_virtual = true; break;
            case TokenKind::kw_mut: m.is_mut = true; break;
            case TokenKind::kw_extern:
                parse_extern_language(m);
                continue;
            default: return finish_modifiers(m);
            }
            advance();
        }
    }

    // `extern(std::build::language::c)` (spec 13): the language is an alternative of
    // `std::build::language`, written out in full like every std name.
    void parse_extern_language(Modifiers& m) {
        Span kw     = advance().span;   // extern
        m.is_extern = true;
        m.abi       = "c";
        auto bad    = [&](Span where, std::string label) {
            report(DiagCode::E2033, where, "'extern' names the language: 'extern(std::build::language::c)'", std::move(label))
                .notes.push_back(Note{"the languages are std::build::language::c and std::build::language::cpp", std::nullopt});
        };
        if (at(TokenKind::string_literal)) {   // C++'s `extern "C"`
            Span lit = advance().span;
            bad(Span{kw.file, kw.begin, lit.end}, "write 'extern(std::build::language::c)'");
            return;
        }
        if (!at(TokenKind::l_paren)) {
            bad(kw, "the language is missing");
            return;
        }
        Span open = advance().span;
        if (at(TokenKind::r_paren)) {
            bad(cur().span, "the language is missing");
        } else {
            Span        first = cur().span;
            std::string path;
            while (!at(TokenKind::r_paren) && !at(TokenKind::eof) && !at(TokenKind::semicolon)) {
                path += std::string(advance().text);
            }
            Span where{first.file, first.begin, pos_ > 0 ? tokens_[pos_ - 1].span.end : first.end};
            if (path == "std::build::language::c" || path == "std::build::language::cpp") {
                m.abi = path.substr(path.rfind(':') + 1);
            } else if (path == "c" || path == "cpp") {
                bad(where, std::format("write 'std::build::language::{}': std names are written in full", path));
            } else {
                bad(where, "not a language");
            }
        }
        expect_close(TokenKind::r_paren, open);
    }

    // A path copied by its names (annotation paths have no template arguments).
    static Path copy_path(const Path& p) {
        Path out;
        out.span   = p.span;
        out.global = p.global;
        for (const PathSegment& seg : p.segments) {
            PathSegment c;
            c.name = seg.name;
            c.span = seg.span;
            out.segments.push_back(std::move(c));
        }
        return out;
    }

    // `@path` or `@path{ .field = value, ... }` (spec 29). The value is kept as the struct literal
    // `path{ ... }`.
    AnnotationUse parse_annotation_use() {
        AnnotationUse u;
        Span          start = advance().span;   // @
        if (!at(TokenKind::identifier) && !at(TokenKind::colon_colon)) fail_unexpected("an annotation name after '@'");
        u.path    = parse_type_path();
        auto type = std::make_unique<NamedType>(u.path.span, copy_path(u.path));
        if (at(TokenKind::l_brace)) {
            u.value = parse_struct_literal(u.path.span, std::move(type));
        } else {
            u.value = std::make_unique<StructLiteralExpr>(u.path.span, std::move(type));
        }
        u.span        = from(start);
        u.value->span = u.span;
        return u;
    }

    // `annotation(std::meta::target::field, ...)` before `struct` (spec 29).
    void parse_annotation_targets(Modifiers& m) {
        m.annotation_span = advance().span;   // annotation
        m.is_annotation   = true;
        Span open         = advance().span;   // (
        if (!at(TokenKind::r_paren)) {
            do {
                m.annotation_targets.push_back(parse_type_path());
            } while (accept(TokenKind::comma));
        }
        expect_close(TokenKind::r_paren, open);
        m.annotation_span = from(m.annotation_span);
        if (m.annotation_targets.empty()) {
            report(DiagCode::E2035, m.annotation_span, "an annotation names where it may be written",
                   "no targets")
                .notes.push_back(Note{"write e.g. 'annotation(std::meta::target::field) struct name { ... }'", std::nullopt});
        }
    }

    // `generate : interfaces { members }` inside an annotation (spec 29).
    void parse_generate(TypeDecl& decl) {
        auto g  = std::make_unique<GenerateBlock>();
        g->span = advance().span;   // generate
        if (accept(TokenKind::colon)) {
            do {
                g->bases.push_back(parse_type({}));
            } while (accept(TokenKind::comma));
        }
        if (!at(TokenKind::l_brace)) fail_unexpected("'{' to start the generate block");
        Span   open   = advance().span;
        Access access = Access::public_;
        while (!at(TokenKind::r_brace) && !at(TokenKind::eof)) {
            std::size_t before = pos_;
            try {
                if (!parse_access_label(MemberContext::class_, access)) {
                    DeclPtr d = parse_member(MemberContext::class_, access);
                    if (!d) continue;
                    if (d->kind != DeclKind::function) {
                        report(DiagCode::E2035, d->name_span, "a generate block adds methods only, not fields or checks",
                               "not a method")
                            .notes.push_back(Note{"generated fields would change a type's layout from far away (spec 29)", std::nullopt});
                    } else {
                        g->members.push_back(std::move(d));
                    }
                }
            } catch (const ParseError&) {
                synchronize(false);
            }
            if (pos_ == before) advance();
        }
        expect_close(TokenKind::r_brace, open);
        g->span = from(g->span);
        if (decl.generate) {
            report(DiagCode::E2035, g->span, "an annotation has at most one generate block", "second block");
        }
        decl.generate = std::move(g);
    }

    Modifiers finish_modifiers(Modifiers& m) {
        if (m.is_run_time && !m.is_compile_time) {
            report(DiagCode::E2018, m.start, "'run_time' without 'compile_time'",
                   "remove 'run_time'")
                .notes.push_back(Note{"functions run at run time by default; write "
                                       "'compile_time run_time' for both",
                                       std::nullopt});
        }
        return std::move(m);
    }

    void parse_template_header(Modifiers& m) {
        advance();   // template
        expect(TokenKind::less, "after 'template'");
        m.is_template = true;
        do {
            TemplateParam p;
            Span          start = cur().span;
            if (accept(TokenKind::kw_type)) {
                p.is_type = true;
                p.is_pack = accept(TokenKind::ellipsis);
                p.name    = expect_identifier("a template parameter name");
                if (accept(TokenKind::colon)) {
                    TypeOptions no_ref;
                    no_ref.allow_reference = false;
                    do {
                        p.constraints.push_back(parse_type(no_ref));
                    } while (accept(TokenKind::amp));
                }
                if (accept(TokenKind::eq)) p.default_type = parse_type({});
            } else {
                p.is_type    = false;
                p.value_type = parse_type({});
                p.is_pack    = accept(TokenKind::ellipsis);   // `usize... N` (spec 34): a value pack
                p.name       = expect_identifier("a template parameter name");
                if (accept(TokenKind::eq)) p.default_value = parse_binary(prec_add);
            }
            if (!p.default_type && !p.default_value && !m.template_params.empty() &&
                (m.template_params.back().default_type || m.template_params.back().default_value)) {
                report(DiagCode::E2001, from(start), "a parameter without a default after one with a default",
                       "give it a default or move it before the defaults");
            }
            p.span = from(start);
            if (!m.template_params.empty() && m.template_params.back().is_pack) {
                report(DiagCode::E2001, m.template_params.back().span, "a parameter pack is the last template parameter",
                       "move it to the end");
            }
            m.template_params.push_back(std::move(p));
        } while (accept(TokenKind::comma));
        expect_template_close();
    }

    // --- checks (spec 23) -----------------------------------------------------------------

    // `compile_time`, `run_time`, their mode lists, then `check`.
    bool at_check_start() {
        if (at(TokenKind::kw_check)) return true;
        if (!at(TokenKind::kw_compile_time) && !at(TokenKind::kw_run_time)) return false;
        std::size_t i = 0;   // `compile_time (modes)? run_time (modes)? check`, without speculating
        for (TokenKind k : {TokenKind::kw_compile_time, TokenKind::kw_run_time}) {
            if (peek(i).kind != k) continue;
            ++i;
            if (peek(i).kind == TokenKind::l_paren) {
                ++i;
                while (peek(i).kind != TokenKind::r_paren && peek(i).kind != TokenKind::eof) ++i;
                ++i;
            }
        }
        return peek(i).kind == TokenKind::kw_check;
    }

    std::vector<std::string> parse_mode_list(Span& span) {
        std::vector<std::string> modes;
        Span open = advance().span;   // (
        do {   // full paths, like every std name: `std::build::mode::debug` (spec 23)
            std::string name = expect_identifier("a build mode");
            while (accept(TokenKind::colon_colon)) name += "::" + expect_identifier("a name after '::'");
            modes.push_back(std::move(name));
        } while (accept(TokenKind::comma));
        expect_close(TokenKind::r_paren, open);
        span = from(open);
        return modes;
    }

    Check parse_check() {
        Check c;
        Span  start = cur().span;
        if (accept(TokenKind::kw_compile_time)) {
            c.is_compile_time = true;
            if (at(TokenKind::l_paren)) c.compile_modes = parse_mode_list(c.compile_modes_span);
        }
        if (accept(TokenKind::kw_run_time)) {
            c.is_run_time = true;
            if (at(TokenKind::l_paren)) c.run_modes = parse_mode_list(c.run_modes_span);
        }
        Span word = expect(TokenKind::kw_check, "").span;
        if (!c.is_compile_time && !c.is_run_time) {
            report(DiagCode::E2031, word, "a check says when it runs", "add 'compile_time' or 'run_time'")
                .notes.push_back(Note{"write 'run_time check (...) else \"...\";' for an assert, or 'compile_time check' "
                                      "for a build requirement",
                                      std::nullopt});
            c.is_run_time = true;
        }
        if (at(TokenKind::l_brace)) {
            if (c.is_run_time) {
                report(DiagCode::E2032, from(start), "a block-form check is 'compile_time' only", "block form")
                    .notes.push_back(Note{"put a complex run-time condition into a 'compile_time run_time bool' "
                                          "function and use 'run_time check (f(...)) else ...;'",
                                          std::nullopt});
            }
            c.block = parse_block();
            return c;
        }
        Span open = expect(TokenKind::l_paren, "after 'check'").span;
        c.condition = parse_expr();
        expect_close(TokenKind::r_paren, open);
        expect(TokenKind::kw_else, "after the check's condition (write 'else \"message\"')");
        c.message = parse_expr();
        expect(TokenKind::semicolon, "after the check's message");
        return c;
    }

    DeclPtr parse_check_decl() {
        Span start = cur().span;
        auto d     = std::make_unique<CheckDecl>(start);
        d->doc     = take_doc();
        d->name    = "check";
        d->check   = parse_check();
        d->span    = from(start);
        d->name_span = start;
        return d;
    }

    void parse_top_level(std::vector<DeclPtr>& out) {
        std::size_t first = out.size();
        parse_top_level_decl(out);
        for (DeclPtr& d : hoisted_) out.push_back(std::move(d));
        hoisted_.clear();
        std::string ns = current_namespace();
        for (std::size_t i = first; i < out.size(); ++i) {
            if (out[i]->ns.empty()) out[i]->ns = ns;
        }
    }

    std::string current_namespace() const {
        std::string out;
        for (const std::string& n : ns_stack_) out += (out.empty() ? "" : "::") + n;
        return out;
    }

    // `namespace a::b { ... }` or the alias `namespace vk = gfx::vulkan;` (spec 30).
    void parse_namespace(std::vector<DeclPtr>& out) {
        Span start = advance().span;   // namespace
        Path path;
        Span path_start = cur().span;
        do {
            PathSegment seg;
            seg.span = cur().span;
            seg.name = expect_identifier("a namespace name");
            path.segments.push_back(std::move(seg));
        } while (accept(TokenKind::colon_colon));
        path.span = from(path_start);
        if (accept(TokenKind::eq)) {
            NamespaceAlias alias;
            alias.span = path.span;
            alias.ns   = current_namespace();
            if (path.segments.size() != 1) {
                report(DiagCode::E2034, path.span, "an alias name is a single name", "not a single name");
            }
            alias.name = path.segments.front().name;
            Span target_start = cur().span;
            if (accept(TokenKind::colon_colon)) alias.target.global = true;
            do {
                PathSegment seg;
                seg.span = cur().span;
                seg.name = expect_identifier("a namespace name");
                alias.target.segments.push_back(std::move(seg));
            } while (accept(TokenKind::colon_colon));
            alias.target.span = from(target_start);
            expect(TokenKind::semicolon, "after the namespace alias");
            aliases_.push_back(std::move(alias));
            return;
        }
        if (at(TokenKind::semicolon)) {
            fail(DiagCode::E2034, cur().span, "a namespace is a block: 'namespace name { ... }'", "expected '{'");
        }
        if (!at(TokenKind::l_brace)) fail_unexpected("'{' after the namespace name");
        Span open = advance().span;   // {
        std::size_t depth = ns_stack_.size();
        for (const PathSegment& seg : path.segments) ns_stack_.push_back(seg.name);
        ns_blocks_.push_back(NamespaceBlock{current_namespace(), path.span});
        (void)start;
        while (!at(TokenKind::r_brace) && !at(TokenKind::eof)) {
            std::size_t before = pos_;
            try {
                if (at(TokenKind::kw_import)) {
                    report(DiagCode::E2034, cur().span, "imports are written at file scope, not inside a namespace",
                           "move this import to the top of the file");
                }
                parse_top_level(out);
                stray_code_reported_ = false;
            } catch (const ParseError&) {
                synchronize(/*top_level=*/false);
            }
            if (pos_ == before) advance();
        }
        ns_stack_.resize(depth);
        expect_close(TokenKind::r_brace, open);
        if (at(TokenKind::semicolon)) {
            report(DiagCode::E2009, cur().span, "unnecessary ';' after a namespace block", "remove this ';'");
            advance();
        }
    }

    void parse_top_level_decl(std::vector<DeclPtr>& out) {
        if (at(TokenKind::kw_namespace)) {
            parse_namespace(out);
            return;
        }
        if (at_check_start()) {
            out.push_back(parse_check_decl());
            return;
        }
        bool      was_const = check_removed_keyword();
        Modifiers m         = parse_modifiers(MemberContext::top_level);
        if (was_const) {
            m.is_compile_time = true;
        }
        if (m.is_extern && at(TokenKind::l_brace)) {
            parse_extern_block(m, out);
            return;
        }
        if (!m.annotations.empty() && at(TokenKind::semicolon)) {   // `@a{ ... };`: annotates the file (spec 31)
            Span semi = advance().span;
            bool late = !ns_stack_.empty() || std::any_of(out.begin(), out.end(), [](const DeclPtr& d) {
                            return d->kind != DeclKind::import;
                        });
            if (late) {
                report(DiagCode::E2038, Span{id_, m.annotations.front().span.begin, semi.end},
                       "a file annotation stands after the imports and before the first declaration", "too late")
                    .notes.push_back(Note{"move it to the top of the file, after the imports", std::nullopt});
            }
            for (AnnotationUse& u : m.annotations) file_annotations_.push_back(std::move(u));
            return;
        }
        out.push_back(parse_top_level_item(m));
    }

    // `extern(std::build::language::c) { i32 puts(std::raw_ptr<u8> text); ... }`: every function inside gets the
    // block's modifiers (spec 13).
    void parse_extern_block(Modifiers& outer, std::vector<DeclPtr>& out) {
        Span open = advance().span;   // {
        while (!at(TokenKind::r_brace) && !at(TokenKind::eof)) {
            std::size_t before = pos_;
            try {
                Modifiers m = parse_modifiers(MemberContext::top_level);
                if (m.is_extern) {
                    report(DiagCode::E2033, m.start, "already inside an 'extern' block", "remove this 'extern'");
                }
                m.is_extern  = true;
                m.abi        = outer.abi;
                m.is_private = m.is_private || outer.is_private;
                m.is_unsafe  = m.is_unsafe || outer.is_unsafe;
                if (m.doc.empty()) m.doc = outer.doc;
                if (!starts_declaration()) {
                    fail(DiagCode::E2033, cur().span, std::format("expected a function declaration, found {}", describe(cur())),
                         "an 'extern' block holds function declarations");
                }
                DeclPtr d = parse_function_or_variable(m, MemberContext::top_level, Access::none);
                if (d->kind != DeclKind::function) {
                    report(DiagCode::E2033, d->name_span, "an 'extern' block holds function declarations only",
                           "not a function");
                }
                out.push_back(std::move(d));
            } catch (const ParseError&) {
                synchronize(/*top_level=*/false);   // stops before the block's '}'
            }
            if (pos_ == before) advance();
        }
        expect_close(TokenKind::r_brace, open);
    }

    // `distinct type meters = f32;` (spec 34): a new type with the layout of `f32`.
    bool union_next_ = false;   // the next type declaration is a `union`

    DeclPtr parse_distinct(Modifiers& m) {
        auto decl = std::make_unique<TypeDecl>(DeclKind::struct_, m.start);
        apply_common(*decl, m);
        decl->is_distinct = true;
        advance();   // distinct
        advance();   // type
        decl->name_span = cur().span;
        decl->name      = expect_identifier("a type name");
        expect(TokenKind::eq, "after the name of a distinct type");
        auto field   = std::make_unique<VariableDecl>(DeclKind::field, decl->name_span);
        field->name  = "$value";
        field->name_span = decl->name_span;
        field->type  = parse_type({});
        expect(TokenKind::semicolon, "after a distinct type");
        decl->members.push_back(std::move(field));
        decl->span = from(m.start);
        return decl;
    }

    // `implement geometry::point for math::vec3 { ... }` (spec 34): methods that make an existing
    // type implement an interface.
    DeclPtr parse_implement(Modifiers& m) {
        auto decl = std::make_unique<TypeDecl>(DeclKind::struct_, m.start);
        apply_common(*decl, m);
        advance();   // implement
        decl->name_span = cur().span;
        decl->bases.push_back(parse_type({}));
        expect(TokenKind::kw_for, "after the interface in 'implement I for T'");
        decl->implement_target = parse_type({});
        decl->name             = "implement";
        Span open = expect(TokenKind::l_brace, "to start the implementation").span;
        Access access = Access::none;
        while (!at(TokenKind::r_brace) && !at(TokenKind::eof)) {
            std::size_t before = pos_;
            try {
                if (DeclPtr member = parse_member(MemberContext::struct_, access)) decl->members.push_back(std::move(member));
            } catch (const ParseError&) {
                synchronize(false);
            }
            if (pos_ == before) advance();
        }
        expect_close(TokenKind::r_brace, open);
        decl->span = from(m.start);
        return decl;
    }

    DeclPtr parse_top_level_item(Modifiers& m) {
        if (at(TokenKind::identifier) && cur().text == "distinct" && peek(1).kind == TokenKind::kw_type) return parse_distinct(m);
        if (at(TokenKind::identifier) && cur().text == "union" && peek(1).kind == TokenKind::identifier && peek(2).kind == TokenKind::l_brace) {
            union_next_ = true;   // `union u { ... }` (spec 34): a struct whose fields share memory
            return parse_type_decl(m, DeclKind::struct_);
        }
        if (at(TokenKind::identifier) && cur().text == "implement" && peek(1).kind != TokenKind::l_paren) return parse_implement(m);
        switch (cur().kind) {
        case TokenKind::kw_import: return parse_import(m);
        case TokenKind::kw_struct: return parse_type_decl(m, DeclKind::struct_);
        case TokenKind::kw_class: return parse_type_decl(m, DeclKind::class_);
        case TokenKind::kw_interface: return parse_type_decl(m, DeclKind::interface);
        case TokenKind::kw_enum: return parse_enum(m);
        case TokenKind::semicolon:
            report(DiagCode::E2009, cur().span, "unnecessary ';' at file scope", "remove this ';'");
            advance();
            throw ParseError{};
        default: break;
        }
        check_removed_keyword();
        if (!starts_declaration()) {
            // A run of statement-like code at file scope (typically after a '}' closed a
            // function too early) is reported once, not once per line.
            if (stray_code_reported_) {
                throw ParseError{};
            }
            stray_code_reported_ = true;
            fail(DiagCode::E2004, cur().span,
                 std::format("expected a declaration, found {}", describe(cur())),
                 "statements belong inside a function");
        }
        return parse_function_or_variable(m, MemberContext::top_level, Access::none);
    }

    DeclPtr parse_import(Modifiers& m) {
        auto decl = std::make_unique<ImportDecl>(m.start);
        advance();   // import
        Span start = cur().span;
        do {
            PathSegment seg;
            seg.span = cur().span;
            seg.name = expect_identifier("a module name");
            decl->path.segments.push_back(std::move(seg));
        } while (accept(TokenKind::colon_colon));
        decl->path.span = from(start);
        decl->name      = to_string(decl->path);
        expect(TokenKind::semicolon, "after the import");
        decl->span = from(m.start);
        return decl;
    }

    void apply_common(Decl& d, Modifiers& m) {
        d.doc             = m.doc;
        d.is_private      = m.is_private;
        d.is_template     = m.is_template;
        d.template_params = std::move(m.template_params);
        d.annotations     = std::move(m.annotations);
        if (m.is_annotation && d.kind != DeclKind::struct_) {
            report(DiagCode::E2035, m.annotation_span, "an annotation is a struct: 'annotation(...) struct name { ... }'",
                   "not before a struct");
        }
    }

    DeclPtr parse_type_decl(Modifiers& m, DeclKind kind) {
        auto decl = std::make_unique<TypeDecl>(kind, m.start);
        apply_common(*decl, m);
        decl->is_open = m.is_open;
        decl->is_view = m.is_view;
        if (kind == DeclKind::struct_) {
            decl->is_annotation      = m.is_annotation;
            decl->annotation_targets = std::move(m.annotation_targets);
        }
        decl->is_union = std::exchange(union_next_, false);
        std::string_view what = decl->is_union ? "union" : kind == DeclKind::struct_ ? "struct" : kind == DeclKind::class_ ? "class" : "interface";
        advance();
        decl->name_span = cur().span;
        decl->name      = expect_identifier(std::format("a {} name", what));
        type_stack_.push_back(decl->name);
        struct PopType {
            std::vector<std::string>& s;
            ~PopType() { s.pop_back(); }
        } pop_type{type_stack_};
        if (accept(TokenKind::colon)) {
            do {
                if (at(TokenKind::kw_unsafe)) {   // `unsafe std::sendable(cond)`: a promise (spec 27)
                    ThreadPromise p;
                    p.span = cur().span;
                    advance();
                    p.path      = parse_type_path();
                    p.shareable = !p.path.segments.empty() && p.path.segments.back().name == "shareable";
                    if (accept(TokenKind::l_paren)) {
                        p.condition = parse_expr();
                        expect(TokenKind::r_paren, "')' after the condition");
                    }
                    p.span = from(p.span);
                    decl->promises.push_back(std::move(p));
                    continue;
                }
                decl->bases.push_back(parse_type({}));
            } while (accept(TokenKind::comma));
        }
        if (!at(TokenKind::l_brace)) {
            fail_unexpected(std::format("'{{' to start the {} body", what));
        }
        Span open = advance().span;

        MemberContext ctx = kind == DeclKind::struct_ ? MemberContext::struct_
                          : kind == DeclKind::class_  ? MemberContext::class_
                                                      : MemberContext::interface;
        Access access = Access::none;
        while (!at(TokenKind::r_brace) && !at(TokenKind::eof)) {
            std::size_t before = pos_;
            try {
                if (parse_access_label(ctx, access)) {
                    continue;
                }
                if (decl->is_annotation && at(TokenKind::identifier) && cur().text == "generate" &&
                    (peek(1).kind == TokenKind::colon || peek(1).kind == TokenKind::l_brace)) {
                    parse_generate(*decl);
                    continue;
                }
                if (decl->is_annotation && at(TokenKind::identifier) && cur().text == "wrap" && peek(1).kind == TokenKind::l_brace) {
                    Span word = advance().span;   // wrap
                    if (decl->wrap) report(DiagCode::E2035, word, "an annotation has at most one wrap block", "second block");
                    decl->wrap = parse_block();
                    continue;
                }
                // `type for_type = void;` in an annotation: a field whose value is a type (spec 29),
                // not an associated type.
                if (decl->is_annotation && at(TokenKind::kw_type) && peek(1).kind == TokenKind::identifier &&
                    (peek(2).kind == TokenKind::eq || peek(2).kind == TokenKind::semicolon)) {
                    Modifiers fm = parse_modifiers(ctx);
                    auto field = std::make_unique<VariableDecl>(DeclKind::field, fm.start);
                    apply_common(*field, fm);
                    field->access = access;
                    Path p;
                    p.span = cur().span;
                    PathSegment seg;
                    seg.name = "type";
                    seg.span = cur().span;
                    p.segments.push_back(std::move(seg));
                    field->type      = std::make_unique<NamedType>(advance().span, std::move(p));
                    field->name_span = cur().span;
                    field->name      = expect_identifier("the name of the field");
                    if (accept(TokenKind::eq)) field->init = parse_expr();
                    expect(TokenKind::semicolon, "after a field");
                    field->span = from(fm.start);
                    decl->members.push_back(std::move(field));
                    continue;
                }
                if (DeclPtr member = parse_member(ctx, access)) decl->members.push_back(std::move(member));
            } catch (const ParseError&) {
                synchronize(false);
            }
            if (pos_ == before) {
                advance();
            }
        }
        expect_close(TokenKind::r_brace, open);
        reject_semicolon_after_brace(what);
        decl->span = from(m.start);
        return decl;
    }

    void reject_semicolon_after_brace(std::string_view what) {
        if (at(TokenKind::semicolon)) {
            report(DiagCode::E2009, cur().span, std::format("unnecessary ';' after the '}}' of a {}", what),
                   "remove this ';'");
            advance();
        }
    }

    bool parse_access_label(MemberContext ctx, Access& access) {
        TokenKind k = cur().kind;
        if ((k != TokenKind::kw_public && k != TokenKind::kw_protected && k != TokenKind::kw_private) ||
            peek(1).kind != TokenKind::colon) {
            return false;
        }
        Span label = cur().span;
        advance();
        advance();
        if (ctx == MemberContext::struct_) {
            report(DiagCode::E2021, from(label), "structs have no access sections",
                   "everything in a struct is public")
                .notes.push_back(Note{"use a 'class' for hidden state", std::nullopt});
        } else if (ctx == MemberContext::interface) {
            report(DiagCode::E2001, from(label), "interfaces have no access sections", "remove this");
        }
        access = k == TokenKind::kw_public ? Access::public_ : k == TokenKind::kw_protected ? Access::protected_ : Access::private_;
        return true;
    }

    DeclPtr parse_member(MemberContext ctx, Access access) {
        if (at_check_start()) {
            DeclPtr d = parse_check_decl();
            d->access = access;
            return d;
        }
        Modifiers m = parse_modifiers(ctx);
        if (ctx == MemberContext::struct_ && m.is_virtual) {
            report(DiagCode::E2021, m.start, "structs cannot have virtual methods",
                   "structs cannot be inherited from");
        }
        if (at(TokenKind::tilde)) {
            return parse_destructor(m, ctx, access);
        }
        if (at(TokenKind::kw_struct) || at(TokenKind::kw_class) || at(TokenKind::kw_interface) || at(TokenKind::kw_enum)) {
            return parse_nested_type(m, ctx, access);
        }
        if (at(TokenKind::kw_type) && peek(1).kind == TokenKind::identifier &&
            (peek(2).kind == TokenKind::eq || peek(2).kind == TokenKind::semicolon)) {
            return parse_associated_type(m, ctx, access);
        }
        if (!starts_declaration(/*member=*/true)) {
            fail_unexpected("a field or method declaration");
        }
        return parse_function_or_variable(m, ctx, access);
    }

    // `type element = u32;` in a type, `type element;` in an interface (spec 34): an associated
    // type, a member `compile_time type element = u32;` that `T::element` names.
    DeclPtr parse_associated_type(Modifiers& m, MemberContext ctx, Access access) {
        auto decl = std::make_unique<VariableDecl>(DeclKind::field, m.start);
        apply_common(*decl, m);
        decl->access  = access;
        decl->storage = Storage::compile_time;
        Path p;
        p.span = cur().span;
        PathSegment seg;
        seg.name = "type";
        seg.span = cur().span;
        p.segments.push_back(std::move(seg));
        decl->type = std::make_unique<NamedType>(advance().span, std::move(p));   // type
        decl->name_span = cur().span;
        decl->name      = expect_identifier("the name of the associated type");
        if (accept(TokenKind::eq)) {
            decl->init = parse_expr();
        } else if (ctx != MemberContext::interface) {
            report(DiagCode::E2001, decl->name_span, "an associated type outside an interface needs its type: 'type name = T;'",
                   "no type");
        }
        expect(TokenKind::semicolon, "after an associated type");
        decl->span = from(m.start);
        return decl;
    }

    // A struct, class, enum or interface inside a struct or class: parsed like a file-scope type,
    // then moved to file scope in the namespace `<outer namespace>::<outer type>`.
    DeclPtr parse_nested_type(Modifiers& m, MemberContext ctx, Access access) {
        if (ctx == MemberContext::interface) {
            fail(DiagCode::E2001, cur().span, "an interface has no nested types", "declare it outside the interface");
        }
        std::string outer = type_stack_.empty() ? std::string() : type_stack_.back();
        std::string ns    = current_namespace();
        for (const std::string& t : type_stack_) ns += (ns.empty() ? "" : "::") + t;
        DeclPtr d = parse_top_level_item(m);
        d->ns        = ns;
        d->nested_in = outer;
        d->access    = access;
        if (access == Access::private_ || access == Access::protected_) d->is_private = true;
        hoisted_.push_back(std::move(d));
        return nullptr;
    }

    DeclPtr parse_destructor(Modifiers& m, MemberContext ctx, Access access) {
        Span start = cur().span;
        advance();   // ~
        auto fn = std::make_unique<FunctionDecl>(m.start);
        apply_common(*fn, m);
        fn->access        = access;
        fn->is_destructor = true;
        fn->name_span     = cur().span;
        fn->name          = "~" + expect_identifier("the class name after '~'");
        if (ctx != MemberContext::class_) {
            report(DiagCode::E2022, from(start), "only classes can have a destructor",
                   "a type that owns a resource must be a 'class'");
        }
        Span open = expect(TokenKind::l_paren, "after the destructor name").span;
        expect_close(TokenKind::r_paren, open);
        parse_function_tail(*fn, ctx);
        fn->span = from(m.start);
        return fn;
    }

    DeclPtr parse_function_or_variable(Modifiers& m, MemberContext ctx, Access access) {
        TypeOptions opts;
        opts.allow_auto   = true;
        opts.allow_deduce = true;
        TypePtr type      = parse_type(opts);

        // Operators: `bool operator==(T& other)`.
        if (at(TokenKind::kw_operator)) {
            auto fn = std::make_unique<FunctionDecl>(m.start);
            apply_common(*fn, m);
            fn->access      = access;
            fn->is_operator = true;
            fn->result      = std::move(type);
            if (m.is_mut && fn->result && fn->result->kind == TypeKind::reference) {   // `mut T& operator[](usize i)`
                fn->result->as<ReferenceType>().is_mut = true;
                m.is_mut                               = false;
            }
            advance();
            fn->name_span = cur().span;
            if (accept(TokenKind::l_bracket)) {
                expect(TokenKind::r_bracket, "in 'operator[]'");
                fn->name = "[]";
            } else if (precedence(cur().kind) >= 0 || at(TokenKind::tilde) || at(TokenKind::bang) ||
                       (is_assignment_op(cur().kind) && !at(TokenKind::eq) && !at(TokenKind::plus_percent_eq) &&
                        !at(TokenKind::minus_percent_eq) && !at(TokenKind::star_percent_eq))) {
                fn->name = std::string(advance().text);
            } else {
                fail_unexpected("an operator after 'operator'");
            }
            return finish_function(std::move(fn), m, ctx);
        }

        Span name_span = cur().span;
        std::string name = ctx == MemberContext::top_level ? expect_identifier("a name") : expect_member_name("a name");
        if (at(TokenKind::colon_colon)) {
            Span start = name_span;
            while (accept(TokenKind::colon_colon)) {
                name = expect_identifier("a name");
            }
            report(DiagCode::E2013, from(start), "out-of-line definitions are not allowed",
                   "qualified name")
                .notes.push_back(Note{"define methods inside the 'struct' or 'class' block of the "
                                       "'.ed' file",
                                       std::nullopt});
        }

        if (at(TokenKind::l_paren)) {
            auto fn = std::make_unique<FunctionDecl>(m.start);
            apply_common(*fn, m);
            fn->access    = access;
            fn->name      = std::move(name);
            fn->name_span = name_span;
            fn->result    = std::move(type);
            // `mut T& at(...)`: the function returns a mutable reference.
            if (m.is_mut && fn->result && fn->result->kind == TypeKind::reference) {
                fn->result->as<ReferenceType>().is_mut = true;
                m.is_mut                               = false;
            }
            return finish_function(std::move(fn), m, ctx);
        }

        // A variable or field.
        bool is_field = ctx == MemberContext::struct_ || ctx == MemberContext::class_;
        auto var = std::make_unique<VariableDecl>(is_field ? DeclKind::field : DeclKind::variable, m.start);
        apply_common(*var, m);
        var->access    = access;
        var->name      = std::move(name);
        var->name_span = name_span;
        var->storage   = m.is_compile_time ? Storage::compile_time
                       : m.is_thread_local ? Storage::thread_local_
                       : m.is_static       ? Storage::static_
                                           : Storage::local;
        if (m.is_thread_local && (is_field || m.is_static || m.is_compile_time)) {
            report(DiagCode::E2001, m.start, "'thread_local' is a storage of its own: a file-scope variable, one copy per thread",
                   "not here");
        }
        var->type      = std::move(type);
        var->is_mut    = apply_mut(*var->type, m.is_mut);   // `mut T& field` in a view: a mutable reference
        if (!var->is_mut) m.is_mut = false;

        if (ctx == MemberContext::interface) {
            fail(DiagCode::E2001, name_span, "interfaces cannot have fields", "only method signatures are allowed");
        }
        if (is_field && m.is_mut && var->storage == Storage::local) {
            report(DiagCode::E2001, m.start, "fields have no 'mut' marker", "remove 'mut'")
                .notes.push_back(Note{"mutability belongs to the variable that holds the object",
                                       std::nullopt});
        }
        if (ctx == MemberContext::top_level && var->storage == Storage::local) {
            report(DiagCode::E2012, var->name_span,
                   std::format("file-scope variable '{}' needs 'static' or 'compile_time'", var->name),
                   "missing storage")
                .notes.push_back(Note{"'compile_time' for a constant, 'static' for storage that "
                                       "lives as long as the program",
                                       std::nullopt});
        }
        parse_initializer(var->init, var->name, var->name_span, /*optional=*/is_field);
        var->span = from(m.start);
        return var;
    }

    // `= expr ;` with the spec's errors for `T x;` and `T x{...};`.
    void parse_initializer(ExprPtr& init, const std::string& name, Span name_span, bool optional) {
        if (at(TokenKind::semicolon)) {
            if (!optional) {
                report(DiagCode::E2010, name_span,
                       std::format("variable '{}' has no initial value", name), "add '= value'")
                    .notes.push_back(Note{"Edax has no uninitialized variables; use '= default' "
                                           "for the type's default value",
                                           std::nullopt});
            }
            advance();
            return;
        }
        if (at(TokenKind::l_brace) || at(TokenKind::l_paren)) {
            Span start = cur().span;
            skip_group();
            report(DiagCode::E2011, from(start), std::format("initialize '{}' with '='", name),
                   "not an initializer")
                .notes.push_back(Note{"write 'T x = T{ ... };'", std::nullopt});
            expect(TokenKind::semicolon, "after the declaration");
            return;
        }
        expect(TokenKind::eq, std::format("after '{}'", name));
        init = parse_expr();
        expect(TokenKind::semicolon, "after the declaration");
    }

    DeclPtr finish_function(std::unique_ptr<FunctionDecl> fn, Modifiers& m, MemberContext ctx) {
        fn->is_static       = m.is_static;
        fn->is_virtual      = m.is_virtual;
        fn->is_compile_time = m.is_compile_time;
        fn->is_run_time     = m.is_run_time;
        fn->is_unsafe       = m.is_unsafe;
        fn->is_extern       = m.is_extern;
        fn->abi             = m.abi;
        fn->params          = parse_params();
        check_special_params(*fn);
        parse_function_tail(*fn, ctx);
        fn->span = from(m.start);
        return fn;
    }

    // `[mut] [override] [abstract] ( ; | = default ; | { body } )`
    void parse_function_tail(FunctionDecl& fn, [[maybe_unused]] MemberContext ctx) {
        fn.is_mut = accept(TokenKind::kw_mut);
        if (!fn.is_mut) fn.is_move = accept(TokenKind::kw_move);
        while (true) {
            if (at(TokenKind::kw_override)) {
                if (fn.is_virtual) {
                    report(DiagCode::E2023, cur().span, "'virtual' and 'override' cannot be combined",
                           "remove one")
                        .notes.push_back(Note{"'virtual' introduces a method, 'override' implements one",
                                               std::nullopt});
                }
                fn.is_override = true;
            } else if (at(TokenKind::kw_abstract)) {
                fn.is_abstract = true;
            } else {
                break;
            }
            advance();
        }
        if (at(TokenKind::eq) && peek(1).kind == TokenKind::kw_default) {
            advance();
            advance();
            fn.is_defaulted = true;
            expect(TokenKind::semicolon, "after '= default'");
            return;
        }
        if (accept(TokenKind::semicolon)) {
            return;
        }
        if (!at(TokenKind::l_brace)) {
            if (line_of(cur().span.begin) > line_of(prev_span_.end) && !at(TokenKind::eof)) {
                // Most likely a forgotten '{': report it and parse the body anyway, so its
                // statements are not misread as file-scope declarations.
                Span where = missing_token_span();
                report(DiagCode::E2001, where,
                       std::format("expected '{{' or ';' after the function signature, found {}", describe(cur())),
                       "expected '{' here");
                fn.body = parse_block_contents(where, where);
                return;
            }
            fail_unexpected("'{' or ';' after the function signature");
        }
        Span body_start = cur().span;
        fn.body         = parse_block();
        (void)body_start;   // interface methods may have a body: the default (spec 07)
        reject_semicolon_after_brace("function");
    }

    // `implicit` parameters come last; each `compile_time` parameter gets a hidden template value
    // parameter `$name`, so the function is instantiated per distinct compile-time argument
    // (spec 31).
    void check_special_params(FunctionDecl& fn) {
        bool implicit_seen = false;
        for (Param& p : fn.params) {
            if (p.is_implicit) {
                implicit_seen = true;
            } else if (implicit_seen) {
                report(DiagCode::E2037, p.span, "implicit parameters come after every ordinary parameter",
                       "move this parameter before the implicit ones");
            }
            if (!p.is_compile_time || !p.type) continue;
            TemplateParam tp;
            tp.is_type    = false;
            tp.name       = "$" + p.name;
            tp.span       = p.span;
            tp.value_type = ast::Cloner([](const sema::Symbol&) { return static_cast<sema::Symbol*>(nullptr); }).type(*p.type);
            fn.template_params.push_back(std::move(tp));
            fn.is_template = true;
        }
    }

    // Lambdas and function types have no `implicit` or `compile_time` parameters.
    void reject_special_params(const std::vector<Param>& params) {
        for (const Param& p : params) {
            if (p.is_implicit || p.is_compile_time) {
                report(DiagCode::E2036, p.span, "'implicit' and 'compile_time' parameters belong to function declarations",
                       "not allowed in a lambda");
            }
        }
    }

    std::vector<Param> parse_params() {
        std::vector<Param> params;
        Span               open = expect(TokenKind::l_paren).span;
        if (accept(TokenKind::r_paren)) {
            return params;
        }
        do {
            if (at(TokenKind::r_paren)) {
                break;   // trailing comma
            }
            params.push_back(parse_param());
        } while (accept(TokenKind::comma));
        expect_close(TokenKind::r_paren, open);
        return params;
    }

    Param parse_param() {
        Param p;
        Span  start = cur().span;
        while (at(TokenKind::at)) p.annotations.push_back(parse_annotation_use());
        // `implicit` and `compile_time` (spec 31), in either order.
        // `implicit` is a contextual keyword: a type named `implicit` is `implicit x)` / `implicit x,`.
        while ((at(TokenKind::identifier) && cur().text == "implicit" &&
                !(peek(1).kind == TokenKind::identifier &&
                  (peek(2).kind == TokenKind::comma || peek(2).kind == TokenKind::r_paren || peek(2).kind == TokenKind::eq))) ||
               at(TokenKind::kw_compile_time)) {
            if (at(TokenKind::kw_compile_time)) p.is_compile_time = true;
            else p.is_implicit = true;
            advance();
        }
        bool  mut   = accept(TokenKind::kw_mut);
        TypeOptions opts;
        opts.allow_auto = true;
        p.type          = parse_type(opts);
        if (p.type->kind == TypeKind::auto_) {
            report(DiagCode::E2025, p.type->span, "'auto' is not allowed in function signatures",
                   "write the type")
                .notes.push_back(Note{"for a generic parameter, use 'template<type T>'", std::nullopt});
        }
        p.is_mut  = apply_mut(*p.type, mut);
        p.is_pack = accept(TokenKind::ellipsis);
        p.name    = expect_identifier("a parameter name");
        if (at(TokenKind::eq)) {
            Span def = cur().span;
            advance();
            parse_expr();
            report(DiagCode::E2024, from(def), "default arguments are not allowed", "remove this")
                .notes.push_back(Note{"write an overload, or pass a struct", std::nullopt});
        }
        p.span = from(start);
        return p;
    }

    // `mut` before a reference type makes the reference mutable; otherwise it makes the binding
    // mutable. Returns whether the binding itself is mutable.
    static bool apply_mut(TypeExpr& type, bool mut) {
        if (mut && type.kind == TypeKind::reference) {
            type.as<ReferenceType>().is_mut = true;
            return false;
        }
        return mut;
    }

    DeclPtr parse_enum(Modifiers& m) {
        auto decl = std::make_unique<EnumDecl>(m.start);
        apply_common(*decl, m);
        advance();   // enum
        decl->name_span = cur().span;
        decl->name      = expect_identifier("an enum name");
        if (accept(TokenKind::colon)) {
            decl->tag_type = parse_type({});
        } else {
            report(DiagCode::E2027, decl->name_span,
                   std::format("enum '{}' needs an underlying tag type", decl->name), "add ': u8'")
                .notes.push_back(Note{"write 'enum name : u8 { ... }'", std::nullopt});
            decl->tag_type = std::make_unique<PlaceholderType>(TypeKind::auto_, decl->name_span);
        }
        if (!at(TokenKind::l_brace)) {
            fail_unexpected("'{' to start the enum body");
        }
        Span open = advance().span;
        while (!at(TokenKind::r_brace) && !at(TokenKind::eof)) {
            EnumAlternative alt;
            alt.span = cur().span;
            while (at(TokenKind::at)) alt.annotations.push_back(parse_annotation_use());
            if (at(TokenKind::kw_type)) {   // `std::meta::target::type` (spec 29): written after `::` like any name
                alt.name = std::string(advance().text);
            } else {
                alt.name = expect_identifier("an enum alternative");
            }
            if (at(TokenKind::l_paren)) {
                Span popen  = advance().span;
                alt.payload = parse_type({});
                expect_close(TokenKind::r_paren, popen);
            }
            if (accept(TokenKind::eq)) {
                alt.value = parse_expr();
            }
            alt.span = from(alt.span);
            decl->alternatives.push_back(std::move(alt));
            if (!accept(TokenKind::comma)) {
                break;
            }
        }
        expect_close(TokenKind::r_brace, open);
        reject_semicolon_after_brace("enum");
        decl->span = from(m.start);
        return decl;
    }

    // --- types ----------------------------------------------------------------------------

    TypePtr parse_type(const TypeOptions& opts) {
        Span start = cur().span;
        if (at(TokenKind::kw_auto) && opts.allow_auto) {
            advance();
            return std::make_unique<PlaceholderType>(TypeKind::auto_, start);
        }
        if (at(TokenKind::kw_deduce) && opts.allow_deduce) {
            advance();
            return std::make_unique<PlaceholderType>(TypeKind::deduce, start);
        }
        if (at(TokenKind::l_paren)) {   // `(A, B)`: a tuple type
            Span open  = advance().span;
            auto tuple = std::make_unique<TupleType>(open);
            do {
                if (at(TokenKind::r_paren)) break;
                if (at(TokenKind::kw_mut) && opts.allow_reference) {   // `(mut A&, B&)`: references into distinct fields (spec 34)
                    Span m      = advance().span;
                    TypePtr ref = parse_type({});
                    if (ref->kind != TypeKind::reference) {
                        report(DiagCode::E2003, from(m), "'mut' in a tuple type marks a reference: write 'mut T&'", "not a reference");
                    } else {
                        ref->as<ReferenceType>().is_mut = true;
                    }
                    tuple->elements.push_back(std::move(ref));
                    continue;
                }
                tuple->elements.push_back(parse_type({}));
            } while (accept(TokenKind::comma));
            expect_close(TokenKind::r_paren, open);
            tuple->span = from(start);
            if (tuple->elements.size() < 2) {
                report(DiagCode::E2003, tuple->span, "a tuple type has two or more elements",
                       tuple->elements.empty() ? "use 'void' for no value" : "a one-element tuple is just its element");
            }
            TypePtr type = std::move(tuple);
            if (opts.allow_reference && at(TokenKind::amp)) {
                advance();
                type = std::make_unique<ReferenceType>(from(start), std::move(type));
            }
            return type;
        }
        if (!starts_type(cur().kind)) {
            fail(DiagCode::E2003, cur().span, std::format("expected a type, found {}", describe(cur())),
                 "not a type");
        }
        Path    path = parse_type_path();
        TypePtr type = std::make_unique<NamedType>(from(start), std::move(path));

        if (opts.allow_function && at(TokenKind::l_paren)) {
            Span open = advance().span;
            auto fn   = std::make_unique<FunctionType>(start, std::move(type));
            if (!at(TokenKind::r_paren)) {
                do {
                    if (at(TokenKind::kw_mut)) {   // `void(mut painter&)`: a parameter that is a mutable reference
                        Span    m   = advance().span;
                        TypePtr ref = parse_type({});
                        if (ref->kind != TypeKind::reference) {
                            report(DiagCode::E2003, from(m), "'mut' in a function type marks a reference parameter: write 'mut T&'",
                                   "not a reference");
                        } else {
                            ref->as<ReferenceType>().is_mut = true;
                        }
                        fn->params.push_back(std::move(ref));
                        continue;
                    }
                    fn->params.push_back(parse_type({}));
                } while (accept(TokenKind::comma));
            }
            expect_close(TokenKind::r_paren, open);
            fn->span = from(start);
            type     = std::move(fn);
        }
        if (opts.allow_reference && at(TokenKind::amp)) {
            advance();
            type = std::make_unique<ReferenceType>(from(start), std::move(type));
        }
        return type;
    }

    Path parse_type_path() {
        Path path;
        Span start = cur().span;
        if (accept(TokenKind::colon_colon)) path.global = true;   // `::name` (spec 30)
        while (true) {
            PathSegment seg;
            seg.span = cur().span;
            seg.name = std::string(advance().text);
            if (at(TokenKind::less)) {
                seg.has_template_args = true;
                seg.template_args     = parse_template_args();
            }
            seg.span = from(seg.span);
            path.segments.push_back(std::move(seg));
            if (at(TokenKind::colon_colon) &&
                (peek(1).kind == TokenKind::identifier || is_keyword(peek(1).kind))) {
                advance();
                continue;
            }
            break;
        }
        path.span = from(start);
        return path;
    }

    std::vector<TemplateArg> parse_template_args() {
        std::vector<TemplateArg> args;
        advance();   // <
        if (at(TokenKind::greater) || at(TokenKind::shr)) {
            expect_template_close();
            return args;
        }
        do {
            args.push_back(parse_template_arg());
        } while (accept(TokenKind::comma));
        expect_template_close();
        return args;
    }

    TemplateArg parse_template_arg() {
        TemplateArg arg;
        Span        start = cur().span;
        TypeOptions opts;
        opts.allow_function = true;
        if (accept(TokenKind::kw_mut)) {
            arg.is_mut = true;
            arg.type   = parse_type(opts);
        } else if (starts_type(cur().kind) || at(TokenKind::l_paren)) {
            // A type, unless it continues like an expression (`N + 1`).
            TypePtr type;
            bool    is_type = speculate([&] {
                type = parse_type(opts);
                if (!at(TokenKind::comma) && !at(TokenKind::greater) && !at(TokenKind::shr)) {
                    throw ParseError{};
                }
            });
            if (is_type) {
                arg.type = std::move(type);
            } else {
                arg.value = parse_binary(prec_add);
            }
        } else {
            // Value arguments stop before `<`, `>` and `>>`: use parentheses for those.
            arg.value = parse_binary(prec_add);
        }
        if (arg.value && at(TokenKind::ellipsis)) {   // `product<N...>`: a value pack passed on (spec 34)
            Span all  = from(start);
            advance();
            arg.value = std::make_unique<UnaryExpr>(all, UnaryOp::spread, std::move(arg.value));
        }
        arg.span = from(start);
        return arg;
    }

    // --- statements -----------------------------------------------------------------------

    BlockPtr parse_block(bool is_unsafe = false) {
        Span start = cur().span;
        Span open  = expect(TokenKind::l_brace).span;
        return parse_block_contents(start, open, is_unsafe);
    }

    // Statements up to the closing `}`; the opening `{` (at `open`) is already consumed.
    BlockPtr parse_block_contents(Span start, Span open, bool is_unsafe = false) {
        auto block = std::make_unique<BlockExpr>(start);
        block->is_unsafe = is_unsafe;
        while (!at(TokenKind::r_brace) && !at(TokenKind::eof)) {
            std::size_t before = pos_;
            try {
                parse_statement(*block);
            } catch (const ParseError&) {
                synchronize(false);
            }
            if (pos_ == before) {
                advance();
            }
        }
        expect_close(TokenKind::r_brace, open);
        block->span = from(start);
        return block;
    }

    // The body of if/else/while/for: braces are mandatory (spec 01, 10).
    BlockPtr parse_body(std::string_view construct) {
        if (!at(TokenKind::l_brace)) {
            fail(DiagCode::E2005, cur().span,
                 std::format("expected '{{' after '{}': braces are required", construct),
                 "add '{ ... }' around the body");
        }
        return parse_block();
    }

    // A declaration is a type followed by a name or `operator`.
    bool starts_declaration(bool member = false) {
        if (at(TokenKind::kw_auto) || at(TokenKind::kw_deduce)) {
            return true;
        }
        if (!starts_type(cur().kind) && !at(TokenKind::l_paren)) {
            return false;
        }
        return lookahead([&] {
            parse_type({});
            if (!at(TokenKind::identifier) && !at(TokenKind::kw_operator) &&
                !(member && is_keyword(cur().kind) && peek_is_name_follower())) {
                throw ParseError{};
            }
        });
    }

    bool looks_like_declaration() {
        if (!at(TokenKind::identifier) && !at(TokenKind::colon_colon) && !at(TokenKind::l_paren)) {
            return false;
        }
        if (at(TokenKind::identifier)) {   // the common cases, without speculating
            switch (peek(1).kind) {
            case TokenKind::identifier: return true;   // `T name`
            case TokenKind::dot: case TokenKind::l_paren: case TokenKind::r_paren: case TokenKind::semicolon:
            case TokenKind::comma: case TokenKind::r_brace: case TokenKind::l_brace: case TokenKind::eq:
            case TokenKind::eq_eq: case TokenKind::bang_eq: case TokenKind::plus: case TokenKind::minus:
            case TokenKind::slash: case TokenKind::percent: case TokenKind::question: case TokenKind::dot_dot:
            case TokenKind::dot_dot_eq: case TokenKind::arrow: case TokenKind::fat_arrow: case TokenKind::pipe_pipe:
            case TokenKind::amp_amp: case TokenKind::pipe: case TokenKind::caret: case TokenKind::shl:
            case TokenKind::greater: case TokenKind::greater_eq: case TokenKind::less_eq: case TokenKind::colon:
                return false;   // an expression goes on (a type is followed by `::`, `<`, `&`, `[`, `*`, `...` or a name)
            default:
                if (is_assignment_op(peek(1).kind)) return false;
                break;
            }
        }
        return lookahead([&] {
            parse_type({});
            if (!at(TokenKind::identifier)) {
                throw ParseError{};
            }
        });
    }

    void parse_statement(BlockExpr& block) {
        Span start = cur().span;
        if (at(TokenKind::semicolon)) {
            bool after_brace = pos_ > 0 && tokens_[pos_ - 1].is(TokenKind::r_brace);
            report(DiagCode::E2009, cur().span,
                   after_brace ? "unnecessary ';' after '}'" : "empty statement", "remove this ';'");
            advance();
            return;
        }
        check_removed_keyword();

        if (at_check_start()) {
            auto stmt   = std::make_unique<CheckStmt>(start);
            stmt->check = parse_check();
            stmt->span  = from(start);
            block.statements.push_back(std::move(stmt));
            return;
        }
        bool hinted_call = at(TokenKind::at) && peek(1).kind == TokenKind::identifier && peek(1).text == "std" &&
                           peek(2).kind == TokenKind::colon_colon && peek(3).kind == TokenKind::identifier && peek(3).text == "dispatch_hint";
        if (at(TokenKind::at) && !hinted_call) {   // `@std::allow{ .id = x } stmt;`: only @std::allow on a statement
            std::vector<ExprPtr> allowed;
            while (at(TokenKind::at)) {
                AnnotationUse u = parse_annotation_use();
                if (to_string(u.path) != "std::allow") {
                    report(DiagCode::E2035, u.span, "only '@std::allow' may be written on a statement", "not allowed here")
                        .notes.push_back(Note{"annotations describe declarations; on a statement, '@std::allow' silences one warning", std::nullopt});
                    continue;
                }
                for (FieldInit& f : u.value->as<StructLiteralExpr>().fields) {
                    if (f.name == "id") allowed.push_back(std::move(f.value));
                }
            }
            std::size_t before = block.statements.size();
            parse_statement(block);
            if (block.statements.size() > before) {
                for (ExprPtr& a : allowed) block.statements.back()->allowed.push_back(std::move(a));
            } else if (block.tail) {
                auto wrapped = std::make_unique<ExprStmt>(block.tail->span, std::move(block.tail));
                for (ExprPtr& a : allowed) wrapped->allowed.push_back(std::move(a));
                block.statements.push_back(std::move(wrapped));
            }
            return;
        }
        switch (cur().kind) {
        case TokenKind::kw_compile_time:
            if (peek(1).kind != TokenKind::l_paren) {
                advance();
                block.statements.push_back(parse_variable_stmt(start, Storage::compile_time));
                return;
            }
            break;
        case TokenKind::kw_auto:
            if (peek(1).kind == TokenKind::l_paren) {
                block.statements.push_back(parse_destructure(start, /*is_auto=*/true));
                return;
            }
            block.statements.push_back(parse_variable_stmt(start, Storage::local));
            return;
        case TokenKind::kw_mut:
            block.statements.push_back(parse_variable_stmt(start, Storage::local));
            return;
        case TokenKind::l_paren:
            if (looks_like_destructure()) {
                block.statements.push_back(parse_destructure(start, /*is_auto=*/false));
                return;
            }
            if (looks_like_declaration()) {
                block.statements.push_back(parse_variable_stmt(start, Storage::local));
                return;
            }
            break;
        case TokenKind::underscore:
            if (peek(1).kind == TokenKind::eq) {
                advance();
                advance();
                ExprPtr value = parse_expr();
                expect(TokenKind::semicolon, "after the discarded expression");
                block.statements.push_back(std::make_unique<DiscardStmt>(from(start), std::move(value)));
                return;
            }
            break;
        case TokenKind::kw_while:
        case TokenKind::kw_for: {
            ExprPtr loop = at(TokenKind::kw_while) ? parse_while() : parse_for();
            block.statements.push_back(std::make_unique<ExprStmt>(loop->span, std::move(loop)));
            reject_semicolon_after_brace("loop");
            return;
        }
        default:
            if ((is_type_keyword(cur().kind) || at(TokenKind::kw_type)) && peek(1).kind != TokenKind::colon_colon &&
                peek(1).kind != TokenKind::r_brace && peek(1).kind != TokenKind::semicolon) {
                block.statements.push_back(parse_variable_stmt(start, Storage::local));
                return;
            }
            if (looks_like_declaration()) {
                block.statements.push_back(parse_variable_stmt(start, Storage::local));
                return;
            }
            break;
        }

        // Expression statement, assignment, or the block's value.
        ExprPtr expr;
        switch (cur().kind) {
        case TokenKind::kw_if: expr = parse_if(/*as_value=*/false); break;
        case TokenKind::kw_match: expr = parse_match(); break;
        case TokenKind::l_brace: expr = parse_block(); break;
        case TokenKind::kw_unsafe:
            if (peek(1).kind == TokenKind::l_brace) {
                advance();
                expr = parse_block(/*is_unsafe=*/true);
                expr->span = from(start);
                break;
            }
            [[fallthrough]];
        default: expr = parse_expr(); break;
        }

        if (is_assignment_op(cur().kind)) {
            TokenKind op = advance().kind;
            ExprPtr value = parse_expr();
            expect(TokenKind::semicolon, "after the assignment");
            block.statements.push_back(std::make_unique<AssignStmt>(from(start), op, std::move(expr), std::move(value)));
            return;
        }
        if (is_block_like(*expr)) {
            if (at(TokenKind::r_brace)) {
                block.tail = std::move(expr);
                return;
            }
            if (at(TokenKind::semicolon)) {
                report(DiagCode::E2009, cur().span, "unnecessary ';' after '}'", "remove this ';'");
                advance();
            }
            block.statements.push_back(std::make_unique<ExprStmt>(from(start), std::move(expr)));
            return;
        }
        if (at(TokenKind::kw_else) && expr->kind == ExprKind::call) {
            // `f(...) else { ... }` (spec 10): the block runs when the call returns false.
            advance();
            auto node            = std::make_unique<IfExpr>(start);
            node->from_call_else = true;
            Span call_span       = expr->span;
            node->condition      = std::make_unique<UnaryExpr>(call_span, UnaryOp::logical_not, std::move(expr));
            node->then_block     = parse_body("else");
            node->span           = from(start);
            block.statements.push_back(std::make_unique<ExprStmt>(from(start), std::move(node)));
            reject_semicolon_after_brace("'else' block");
            return;
        }
        if (at(TokenKind::kw_else)) {
            fail(DiagCode::E2039, cur().span, "'else' without 'if': only a call may be followed by 'else { ... }'",
                 "this 'else' follows an expression that isn't a call");
        }
        if (at(TokenKind::r_brace)) {
            block.tail = std::move(expr);
            return;
        }
        expect(TokenKind::semicolon, "after expression");
        block.statements.push_back(std::make_unique<ExprStmt>(from(start), std::move(expr)));
    }

    // `[...]` directly followed by `(` is a lambda's capture list; anything else is a list literal.
    bool bracket_starts_lambda() const {
        std::size_t i     = pos_;
        int         depth = 0;
        for (; i < tokens_.size(); ++i) {
            TokenKind k = tokens_[i].kind;
            if (k == TokenKind::l_bracket || k == TokenKind::l_paren || k == TokenKind::l_brace) ++depth;
            if (k == TokenKind::r_bracket || k == TokenKind::r_paren || k == TokenKind::r_brace) {
                if (--depth == 0) break;
            }
            if (k == TokenKind::eof) return false;
        }
        return i + 1 < tokens_.size() && tokens_[i].kind == TokenKind::r_bracket && tokens_[i + 1].kind == TokenKind::l_paren;
    }

    // `[a, b, c]` (or `[]`): a list literal.
    ExprPtr parse_list() {
        Span open = advance().span;   // [
        auto list = std::make_unique<ListExpr>(open);
        while (!at(TokenKind::r_bracket) && !at(TokenKind::eof)) {
            list->elements.push_back(parse_expr());
            if (!accept(TokenKind::comma)) break;
        }
        expect_close(TokenKind::r_bracket, open);
        list->span = from(open);
        return list;
    }

    // `(T1 a, T2 b) = ...` / `(mut u32 lo, _) = ...` ahead: a destructuring declaration.
    bool looks_like_destructure() {
        if (!at(TokenKind::l_paren)) return false;
        return lookahead([&] {
            advance();
            do {
                if (accept(TokenKind::underscore)) continue;
                accept(TokenKind::kw_mut);
                parse_type({});
                if (!at(TokenKind::identifier)) throw ParseError{};
                advance();
            } while (accept(TokenKind::comma));
            if (!accept(TokenKind::r_paren) || !at(TokenKind::eq)) throw ParseError{};
        });
    }

    // `(T1 a, T2 b) = expr;` or `auto (a, b) = expr;`.
    StmtPtr parse_destructure(Span start, bool is_auto) {
        auto d     = std::make_unique<DestructureStmt>(start);
        d->is_auto = is_auto;
        if (is_auto) advance();   // auto
        Span open = expect(TokenKind::l_paren, "to start the names").span;
        do {
            DestructureBinding b;
            b.span = cur().span;
            if (accept(TokenKind::underscore)) {
                b.wildcard = true;
                d->bindings.push_back(std::move(b));
                continue;
            }
            b.is_mut = accept(TokenKind::kw_mut);
            if (!is_auto) {
                b.type = parse_type({});
                b.is_mut = apply_mut(*b.type, b.is_mut);
            }
            b.span = cur().span;
            b.name = expect_identifier("a variable name");
            d->bindings.push_back(std::move(b));
        } while (accept(TokenKind::comma));
        expect_close(TokenKind::r_paren, open);
        if (d->bindings.size() < 2) {
            report(DiagCode::E2001, from(open), "a destructuring declaration names two or more elements", "one name");
        }
        expect(TokenKind::eq, "after the names (destructuring takes a value)");
        d->init = parse_expr();
        expect(TokenKind::semicolon, "after the destructuring declaration");
        d->span = from(start);
        return d;
    }

    StmtPtr parse_variable_stmt(Span start, Storage storage) {
        auto var     = std::make_unique<VariableStmt>(start);
        var->storage = storage;
        bool mut     = accept(TokenKind::kw_mut);
        TypeOptions opts;
        opts.allow_auto = true;
        var->type       = parse_type(opts);
        var->is_mut     = apply_mut(*var->type, mut);
        var->name_span  = cur().span;
        var->name       = expect_identifier("a variable name");
        parse_initializer(var->init, var->name, var->name_span, /*optional=*/false);
        var->span = from(start);
        return var;
    }

    ExprPtr parse_paren_condition(std::string_view construct) {
        Span open = expect(TokenKind::l_paren, std::format("after '{}'", construct)).span;
        try {
            ExprPtr cond = parse_expr();
            if (is_assignment_op(cur().kind)) {
                fail(DiagCode::E2006, cur().span, "assignment is a statement, not an expression",
                     "this assigns", {Note{"compare with '==', or assign before the condition", std::nullopt}});
            }
            expect_close(TokenKind::r_paren, open);
            return cond;
        } catch (const ParseError&) {
            skip_past_close_paren();
            return std::make_unique<SimpleExpr>(ExprKind::error, from(open));
        }
    }

    // `(value is pattern)` or `(value is pattern && guard)` as the condition of `if` and `while`
    // (spec 34): one `match` arm. The scrutinee, pattern and guard, or null when the condition is
    // an ordinary one (then `plain` holds it).
    struct IsCondition {
        ExprPtr    plain;
        ExprPtr    value;
        PatternPtr pattern;
        ExprPtr    guard;
    };

    IsCondition parse_condition_with_is(std::string_view construct) {
        IsCondition out;
        Span open = expect(TokenKind::l_paren, std::format("after '{}'", construct)).span;
        try {
            ExprPtr cond = parse_expr();
            if (at(TokenKind::identifier) && cur().text == "is") {   // a contextual keyword
                advance();
                out.value   = std::move(cond);
                out.pattern = parse_pattern();
                if (accept(TokenKind::amp_amp)) out.guard = parse_expr();
                expect_close(TokenKind::r_paren, open);
                return out;
            }
            if (is_assignment_op(cur().kind)) {
                fail(DiagCode::E2006, cur().span, "assignment is a statement, not an expression",
                     "this assigns", {Note{"compare with '==', or assign before the condition", std::nullopt}});
            }
            expect_close(TokenKind::r_paren, open);
            out.plain = std::move(cond);
        } catch (const ParseError&) {
            skip_past_close_paren();
            out.plain = std::make_unique<SimpleExpr>(ExprKind::error, from(open));
        }
        return out;
    }

    // `if (v is p) { a } else { b }` is `match (v) { p => { a }, _ => { b } }`.
    ExprPtr if_is(Span start, IsCondition c, BlockPtr then_block, ExprPtr else_branch) {
        auto m       = std::make_unique<MatchExpr>(start);
        m->scrutinee = std::move(c.value);
        MatchArm yes;
        yes.span    = c.pattern->span;
        yes.pattern = std::move(c.pattern);
        yes.guard   = std::move(c.guard);
        yes.body    = std::move(then_block);
        MatchArm no;
        no.span    = start;
        no.pattern = std::make_unique<SimplePattern>(PatternKind::wildcard, start);
        no.body    = else_branch ? std::move(else_branch) : ExprPtr(std::make_unique<BlockExpr>(start));
        m->arms.push_back(std::move(yes));
        m->arms.push_back(std::move(no));
        m->span = from(start);
        return m;
    }

    ExprPtr parse_while() {
        Span        start = advance().span;
        auto        loop  = std::make_unique<WhileExpr>(start);
        IsCondition c     = parse_condition_with_is("while");
        loop->body        = parse_body("while");
        if (c.pattern) {   // `while (v is p) { body }`: `while (true) { match (v) { p => { body }, _ => { break; } } }`
            auto stop = std::make_unique<BlockExpr>(start);
            stop->statements.push_back(std::make_unique<ExprStmt>(start, std::make_unique<SimpleExpr>(ExprKind::break_, start)));
            ExprPtr m  = if_is(start, std::move(c), std::move(loop->body), std::move(stop));
            auto    body = std::make_unique<BlockExpr>(m->span);
            body->statements.push_back(std::make_unique<ExprStmt>(m->span, std::move(m)));
            loop->body = std::move(body);
            auto yes   = std::make_unique<LiteralExpr>(ExprKind::bool_literal, start, TokenKind::kw_true);
            yes->bool_value = true;
            yes->text       = "true";
            loop->condition = std::move(yes);
        } else {
            loop->condition = std::move(c.plain);
        }
        loop->span = from(start);
        return loop;
    }

    ExprPtr parse_for() {
        Span start = advance().span;
        auto loop  = std::make_unique<ForExpr>(start);
        loop->is_compile_time = accept(TokenKind::kw_compile_time);
        Span open  = expect(TokenKind::l_paren, "after 'for'").span;
        bool mut   = accept(TokenKind::kw_mut);
        loop->type   = parse_type({});
        loop->is_mut = apply_mut(*loop->type, mut);
        loop->name_span = cur().span;
        loop->name      = expect_identifier("a loop variable name");
        if (at(TokenKind::eq) || at(TokenKind::semicolon)) {
            report(DiagCode::E2015, from(start), "C-style 'for' loops do not exist",
                   "use a range-based loop")
                .notes.push_back(Note{"write 'for (u32 i : 0..n) { ... }'", std::nullopt});
            skip_past_close_paren(/*through_semicolons=*/true);
            loop->iterable = std::make_unique<SimpleExpr>(ExprKind::error, from(start));
        } else {
            expect(TokenKind::colon, "after the loop variable");
            loop->iterable = parse_expr();
            expect_close(TokenKind::r_paren, open);
        }
        loop->body = parse_body("for");
        loop->span = from(start);
        return loop;
    }

    // --- expressions ----------------------------------------------------------------------

    ExprPtr parse_expr() { return parse_binary(prec_or); }

    bool adjacent(const Token& a, const Token& b) const { return a.span.end == b.span.begin; }

    ExprPtr parse_binary(int min_prec) {
        Span    start = cur().span;
        ExprPtr lhs   = parse_prefix();
        int     last_non_assoc = -1;
        while (true) {
            TokenKind op   = cur().kind;
            int       prec = precedence(op);
            if (prec < min_prec) {
                break;
            }
            check_increment(op);
            if ((prec == prec_compare || prec == prec_range) && last_non_assoc == prec) {
                fail(DiagCode::E2008, cur().span,
                     prec == prec_compare ? "comparison operators cannot be chained"
                                          : "range operators cannot be chained",
                     "second operator", {Note{prec == prec_compare ? "write 'a < b && b < c'"
                                                                   : "add parentheses",
                                              std::nullopt}});
            }
            advance();
            ExprPtr rhs = parse_binary(prec + 1);
            if (prec == prec_range) {
                lhs = std::make_unique<RangeExpr>(from(start), std::move(lhs), std::move(rhs),
                                                  op == TokenKind::dot_dot_eq);
            } else {
                lhs = std::make_unique<BinaryExpr>(from(start), op, std::move(lhs), std::move(rhs));
            }
            last_non_assoc = (prec == prec_compare || prec == prec_range) ? prec : -1;
        }
        return lhs;
    }

    // `x++`, `x--`, `++x`: two adjacent `+`/`-` tokens.
    void check_increment(TokenKind op) {
        if ((op == TokenKind::plus || op == TokenKind::minus) && peek(1).kind == op &&
            adjacent(cur(), peek(1)) && !starts_expression(peek(2).kind)) {
            bool inc = op == TokenKind::plus;
            fail(DiagCode::E2007, Span{id_, cur().span.begin, peek(1).span.end},
                 inc ? "Edax has no '++' operator" : "Edax has no '--' operator",
                 inc ? "write '+= 1' instead" : "write '-= 1' instead");
        }
    }

    // `@std::dispatch_hint<circle> shape.area()` (performance §8): the only annotation written
    // on an expression. It names the class a dynamic call is expected to reach.
    ExprPtr parse_hinted_call() {
        AnnotationUse u = parse_annotation_use();
        ExprPtr       operand = parse_prefix();
        bool hint = to_string(u.path) == "std::dispatch_hint" ||
                    (u.path.segments.size() == 2 && u.path.segments[0].name == "std" && u.path.segments[1].name == "dispatch_hint");
        PathSegment& last = u.path.segments.back();
        if (!hint) {
            report(DiagCode::E2035, u.span, "only '@std::dispatch_hint<T>' may be written on an expression", "not allowed here")
                .notes.push_back(Note{"annotations describe declarations", std::nullopt});
            return operand;
        }
        if (last.template_args.size() != 1 || !last.template_args[0].type) {
            report(DiagCode::E2035, u.span, "'@std::dispatch_hint' names one class: '@std::dispatch_hint<circle>'", "no class");
            return operand;
        }
        if (operand->kind != ExprKind::call) {
            report(DiagCode::E2035, u.span, "'@std::dispatch_hint' is written directly before a call", "not before a call");
            return operand;
        }
        auto& call         = operand->as<CallExpr>();
        call.dispatch_hint = std::move(last.template_args[0].type);
        return operand;
    }

    ExprPtr parse_prefix() {
        Span    start = cur().span;
        UnaryOp op;
        switch (cur().kind) {
        case TokenKind::minus:
            if (peek(1).kind == TokenKind::minus && adjacent(cur(), peek(1))) {
                fail(DiagCode::E2007, Span{id_, start.begin, peek(1).span.end},
                     "Edax has no '--' operator", "write 'x -= 1;' instead");
            }
            op = UnaryOp::negate;
            break;
        case TokenKind::plus:
            if (peek(1).kind == TokenKind::plus && adjacent(cur(), peek(1))) {
                fail(DiagCode::E2007, Span{id_, start.begin, peek(1).span.end},
                     "Edax has no '++' operator", "write 'x += 1;' instead");
            }
            fail(DiagCode::E2002, cur().span, "Edax has no unary '+'", "remove this '+'");
        case TokenKind::bang: op = UnaryOp::logical_not; break;
        case TokenKind::tilde: op = UnaryOp::bit_not; break;
        case TokenKind::star: op = UnaryOp::deref; break;
        case TokenKind::amp: op = UnaryOp::address_of; break;
        case TokenKind::kw_move: op = UnaryOp::move; break;
        case TokenKind::at: return parse_hinted_call();
        case TokenKind::kw_mut:
            if (peek(1).kind == TokenKind::amp) {
                advance();
                op = UnaryOp::address_of_mut;
            } else {
                op = UnaryOp::mut_marker;
            }
            break;
        default: return parse_postfix(parse_primary());
        }
        advance();
        ExprPtr operand = parse_prefix();
        return std::make_unique<UnaryExpr>(from(start), op, std::move(operand));
    }

    ExprPtr parse_postfix(ExprPtr expr) {
        Span start = expr->span;
        while (true) {
            switch (cur().kind) {
            case TokenKind::l_paren: {
                Span open = advance().span;
                auto call = std::make_unique<CallExpr>(start, std::move(expr));
                if (!at(TokenKind::r_paren)) {
                    do {
                        if (at(TokenKind::r_paren)) {
                            break;
                        }
                        call->args.push_back(parse_expr());
                        if (at(TokenKind::ellipsis)) {   // `values...`: a pack passed on whole
                            Span    dots = advance().span;
                            ExprPtr arg  = std::move(call->args.back());
                            Span    all{arg->span.file, arg->span.begin, dots.end};
                            call->args.back() = std::make_unique<UnaryExpr>(all, UnaryOp::spread, std::move(arg));
                        }
                    } while (accept(TokenKind::comma));
                }
                expect_close(TokenKind::r_paren, open);
                call->span = from(start);
                expr       = std::move(call);
                break;
            }
            case TokenKind::l_bracket: {
                Span    open  = advance().span;
                ExprPtr index = parse_expr();
                expect_close(TokenKind::r_bracket, open);
                expr = std::make_unique<IndexExpr>(from(start), std::move(expr), std::move(index));
                break;
            }
            case TokenKind::dot:
            case TokenKind::arrow: {
                bool arrow = advance().is(TokenKind::arrow);
                // `t.0`: a tuple element. `t.0.1` lexes as `t` `.` `0.1`: two element accesses.
                if (!arrow && (at(TokenKind::int_literal) || at(TokenKind::float_literal))) {
                    Token       tok  = advance();
                    std::string text(tok.text);
                    std::size_t dot  = text.find('.');
                    auto element = [&](std::string n, Span sp) {
                        Path p;
                        PathSegment seg;
                        seg.name = std::move(n);
                        seg.span = sp;
                        p.segments.push_back(std::move(seg));
                        p.span = sp;
                        return p;
                    };
                    if (text.find_first_not_of("0123456789.") != std::string::npos ||
                        (dot != std::string::npos && text.find('.', dot + 1) != std::string::npos)) {
                        fail(DiagCode::E2001, tok.span, "a tuple element is written by its position: 't.0'", "not a position");
                    }
                    Span first = tok.span;
                    if (dot != std::string::npos) first.end = first.begin + static_cast<std::uint32_t>(dot);
                    expr = std::make_unique<MemberExpr>(from(start), std::move(expr), element(text.substr(0, dot), first), false);
                    if (dot != std::string::npos) {
                        Span second  = tok.span;
                        second.begin = tok.span.begin + static_cast<std::uint32_t>(dot) + 1;
                        expr = std::make_unique<MemberExpr>(from(start), std::move(expr), element(text.substr(dot + 1), second), false);
                    }
                    break;
                }
                Path name  = parse_expr_path(/*member=*/true);
                expr = std::make_unique<MemberExpr>(from(start), std::move(expr), std::move(name), arrow);
                break;
            }
            case TokenKind::question:
                advance();
                expr = std::make_unique<TryExpr>(from(start), std::move(expr));
                break;
            default: return expr;
            }
        }
    }

    // A path in expression position. `<` starts template arguments only if they parse and are
    // followed by `(`, `::` or `{` - otherwise it is the less-than operator.
    Path parse_expr_path(bool member) {
        Path path;
        Span start = cur().span;
        if (!member && accept(TokenKind::colon_colon)) path.global = true;   // `::name` (spec 30)
        while (true) {
            PathSegment seg;
            seg.span = cur().span;
            // After `.` or `::` any keyword is accepted as a name (`std::file::open`).
            bool qualified = member || !path.segments.empty();
            if (!at(TokenKind::identifier) && !(is_keyword(cur().kind) && qualified) &&
                !(is_type_keyword(cur().kind) && !member)) {
                fail_unexpected(member ? "a member name" : "a name");
            }
            seg.name = std::string(advance().text);
            if (at(TokenKind::less)) {
                std::vector<TemplateArg> args;
                bool ok = speculate([&] {
                    args = parse_template_args();
                    // Template arguments before `(`, `::` or `{`, or ending a type value
                    // (`std::option<u32>;`, spec 26): a comparison cannot be followed by these.
                    bool ends_value = at(TokenKind::r_paren) || at(TokenKind::comma) || at(TokenKind::semicolon) ||
                                      at(TokenKind::r_brace) || at(TokenKind::r_bracket) || at(TokenKind::eq_eq) ||
                                      at(TokenKind::bang_eq);
                    if (!at(TokenKind::l_paren) && !at(TokenKind::colon_colon) && !at(TokenKind::l_brace) && !ends_value) {
                        throw ParseError{};
                    }
                });
                if (ok) {
                    seg.has_template_args = true;
                    seg.template_args     = std::move(args);
                }
            }
            seg.span = from(seg.span);
            path.segments.push_back(std::move(seg));
            if (at(TokenKind::colon_colon)) {
                advance();
                continue;
            }
            break;
        }
        path.span = from(start);
        return path;
    }

    ExprPtr make_literal(ExprKind kind) {
        Token t   = advance();
        auto  lit = std::make_unique<LiteralExpr>(kind, t.span, t.kind);
        lit->text       = std::string(t.text);
        lit->value      = t.value;
        lit->int_value  = t.int_value;
        lit->suffix     = std::string(t.suffix);
        lit->bool_value = t.is(TokenKind::kw_true);
        if (t.is(TokenKind::format_string_literal)) split_format_string(*lit);
        return lit;
    }

    // --- f-strings (spec 19) --------------------------------------------------------------

    // Splits `f"a {x:>4} b"` into text and holes. Hole expressions are lexed and parsed from
    // the original source range, so their spans and diagnostics point into the f-string.
    void split_format_string(LiteralExpr& lit) {
        std::string_view src   = file_.text();
        std::size_t      begin = lit.span.begin + 2;   // after `f"`
        std::size_t      end   = lit.span.end - 1;     // before the closing `"`
        std::string      text;
        std::size_t      text_begin = begin;
        auto flush_text = [&](std::size_t at) {
            if (!text.empty()) {
                FormatPart part;
                part.text = std::move(text);
                part.span = Span{id_, static_cast<std::uint32_t>(text_begin), static_cast<std::uint32_t>(at)};
                lit.format_parts.push_back(std::move(part));
                text.clear();
            }
        };
        auto sp = [&](std::size_t a, std::size_t b) {
            return Span{id_, static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b)};
        };
        std::size_t i = begin;
        while (i < end) {
            char c = src[i];
            if (c == '\\') {
                i = decode_escape(src, i, end, text);
                continue;
            }
            if (c == '}') {
                if (i + 1 < end && src[i + 1] == '}') {
                    text += '}';
                    i += 2;
                    continue;
                }
                report(DiagCode::E2030, sp(i, i + 1), "unmatched '}' in format string", "write '}}' for a literal '}'");
                ++i;
                continue;
            }
            if (c != '{') {
                text += c;
                ++i;
                continue;
            }
            if (i + 1 < end && src[i + 1] == '{') {
                text += '{';
                i += 2;
                continue;
            }
            // A hole: find the matching '}' and a top-level ':' that is not part of '::'.
            flush_text(i);
            std::size_t open = i, colon = 0, depth = 0, j = i + 1;
            for (; j < end; ++j) {
                char d = src[j];
                if (d == '"' || d == '\'') {   // a string or char literal in the hole
                    for (++j; j < end && src[j] != d; ++j) {
                        if (src[j] == '\\') ++j;
                    }
                    continue;
                }
                if (d == '(' || d == '[' || d == '{') ++depth;
                else if ((d == ')' || d == ']' || d == '}') && depth > 0) --depth;
                else if (d == '}') break;
                else if (d == ':' && depth == 0 && colon == 0) {
                    bool double_colon = (j + 1 < end && src[j + 1] == ':') || src[j - 1] == ':';
                    if (!double_colon) colon = j;
                }
            }
            if (j >= end) {
                report(DiagCode::E2030, sp(open, end), "'{' in format string is never closed",
                       "missing '}'").notes.push_back(Note{"write '{{' for a literal '{'", std::nullopt});
                text_begin = end;
                i = end;
                break;
            }
            std::size_t expr_end = colon ? colon : j;
            FormatPart  part;
            part.span = sp(open, j + 1);
            if (src.substr(open + 1, expr_end - open - 1).find_first_not_of(" \t") == std::string_view::npos) {
                report(DiagCode::E2030, part.span, "empty '{}' in format string", "no expression")
                    .notes.push_back(Note{"write '{{}}' for literal braces", std::nullopt});
                part.expr = std::make_unique<SimpleExpr>(ExprKind::error, part.span);
            } else {
                part.expr = parse_hole(open + 1, expr_end);
            }
            if (colon) part.spec = parse_format_spec(src, colon + 1, j);
            lit.format_parts.push_back(std::move(part));
            i          = j + 1;
            text_begin = i;
        }
        flush_text(end);
    }

    // Decodes the escape at `src[i]` (a backslash) into `out`; returns the index after it.
    // The lexer already validated every escape in the literal.
    static std::size_t decode_escape(std::string_view src, std::size_t i, std::size_t end, std::string& out) {
        if (i + 1 >= end) return end;
        char e = src[i + 1];
        switch (e) {
        case 'n': out += '\n'; return i + 2;
        case 't': out += '\t'; return i + 2;
        case 'r': out += '\r'; return i + 2;
        case '0': out += '\0'; return i + 2;
        case '\\': out += '\\'; return i + 2;
        case '"': out += '"'; return i + 2;
        case 'u': {
            std::size_t close = src.find('}', i);
            if (close == std::string_view::npos || close >= end) return end;
            std::uint32_t cp = 0;
            for (std::size_t k = i + 3; k < close; ++k) {
                char h = src[k];
                cp     = cp * 16 + static_cast<std::uint32_t>(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
            }
            append_utf8(out, cp);
            return close + 1;
        }
        default: out += e; return i + 2;
        }
    }

    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    // Parses the expression in a hole. Its diagnostics go through report(), so speculative
    // parsing of the enclosing code does not duplicate them.
    ExprPtr parse_hole(std::size_t begin, std::size_t end) {
        DiagnosticEngine   local;
        std::vector<Token> tokens = tokenize_range(file_, id_, local, begin, end);
        ExprPtr            e      = Parser(file_, id_, tokens, local).parse_single_expression();
        for (const Diagnostic& d : local.all()) {
            std::string message = d.message;   // the hole ends at its '}', not at the end of the file
            if (std::size_t at = message.find("end of file"); at != std::string::npos) message.replace(at, 11, "'}'");
            Diagnostic& r = report(d.code, d.span, message, d.label);
            r.notes       = d.notes;
        }
        return e;
    }

    // `[<>^][+][width][.precision][x|X|b|o]`
    FormatSpec parse_format_spec(std::string_view src, std::size_t begin, std::size_t end) {
        FormatSpec  spec;
        spec.present = true;
        spec.span    = Span{id_, static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end)};
        std::size_t i = begin;
        auto number = [&]() {
            int n = 0;
            while (i < end && src[i] >= '0' && src[i] <= '9' && n < 10000) n = n * 10 + (src[i++] - '0');
            return n;
        };
        if (i < end && (src[i] == '<' || src[i] == '>' || src[i] == '^')) spec.align = src[i++];
        if (i < end && src[i] == '+') {
            spec.plus = true;
            ++i;
        }
        if (i < end && src[i] >= '0' && src[i] <= '9') spec.width = number();
        if (i < end && src[i] == '.') {
            ++i;
            if (i >= end || src[i] < '0' || src[i] > '9') {
                report(DiagCode::E2030, spec.span, "'.' in a format spec needs a precision", "e.g. '.2'");
                return spec;
            }
            spec.precision = number();
        }
        if (i < end && (src[i] == 'x' || src[i] == 'X' || src[i] == 'b' || src[i] == 'o')) spec.radix = src[i++];
        if (i != end) {
            report(DiagCode::E2030, spec.span, std::format("invalid format spec '{}'", src.substr(begin, end - begin)),
                   "not a format spec")
                .notes.push_back(Note{"a spec is [<>^][+][width][.precision][x|X|b|o], e.g. '>8.2'", std::nullopt});
        }
        return spec;
    }

    ExprPtr parse_primary() {
        Span start = cur().span;
        switch (cur().kind) {
        case TokenKind::int_literal: return make_literal(ExprKind::int_literal);
        case TokenKind::float_literal: return make_literal(ExprKind::float_literal);
        case TokenKind::char_literal: return make_literal(ExprKind::char_literal);
        case TokenKind::string_literal:
        case TokenKind::raw_string_literal:
        case TokenKind::c_string_literal:
        case TokenKind::format_string_literal: return make_literal(ExprKind::string_literal);
        case TokenKind::kw_true:
        case TokenKind::kw_false: return make_literal(ExprKind::bool_literal);
        case TokenKind::kw_nullptr: return make_literal(ExprKind::nullptr_literal);
        case TokenKind::kw_this: advance(); return std::make_unique<SimpleExpr>(ExprKind::this_, start);
        case TokenKind::kw_default: advance(); return std::make_unique<SimpleExpr>(ExprKind::default_, start);
        case TokenKind::kw_break: advance(); return std::make_unique<SimpleExpr>(ExprKind::break_, start);
        case TokenKind::kw_continue: advance(); return std::make_unique<SimpleExpr>(ExprKind::continue_, start);
        case TokenKind::kw_return: {
            advance();
            ExprPtr value;
            if (starts_expression(cur().kind)) {
                value = parse_expr();
            }
            return std::make_unique<ReturnExpr>(from(start), std::move(value));
        }
        case TokenKind::l_paren: {
            Span    open  = advance().span;
            ExprPtr inner = parse_expr();
            if (is_assignment_op(cur().kind)) {
                fail(DiagCode::E2006, cur().span, "assignment is a statement, not an expression",
                     "this assigns");
            }
            if (at(TokenKind::comma)) {   // `(a, b)`: a tuple
                auto tuple = std::make_unique<TupleExpr>(open);
                tuple->elements.push_back(std::move(inner));
                while (accept(TokenKind::comma)) {
                    if (at(TokenKind::r_paren)) break;
                    tuple->elements.push_back(parse_expr());
                }
                expect_close(TokenKind::r_paren, open);
                tuple->span = from(open);
                return tuple;
            }
            expect_close(TokenKind::r_paren, open);
            return inner;
        }
        case TokenKind::l_bracket:
            if (bracket_starts_lambda()) return parse_lambda();
            return parse_list();
        case TokenKind::l_brace: return parse_block();
        case TokenKind::kw_unsafe: {
            advance();
            if (!at(TokenKind::l_brace)) {
                fail_unexpected("'{' after 'unsafe'");
            }
            BlockPtr block = parse_block(/*is_unsafe=*/true);
            block->span    = from(start);
            return block;
        }
        case TokenKind::kw_if: return parse_if(/*as_value=*/true);
        case TokenKind::kw_match: return parse_match();
        case TokenKind::kw_cast: return parse_cast();
        case TokenKind::kw_bit: return parse_bit();
        case TokenKind::underscore:
            fail(DiagCode::E2029, cur().span, "'_' is not a value",
                 "'_' only works in patterns and in '_ = expr;'");
        case TokenKind::colon_colon:
        case TokenKind::identifier: {
            Path path = parse_expr_path(false);
            if (at(TokenKind::l_brace)) {
                auto type = std::make_unique<NamedType>(path.span, std::move(path));
                return parse_struct_literal(start, std::move(type));
            }
            return std::make_unique<PathExpr>(from(start), std::move(path));
        }
        default:
            if (is_type_keyword(cur().kind)) {   // `u32::max`, or the type value `u32` (spec 26)
                Path path = parse_expr_path(false);
                return std::make_unique<PathExpr>(from(start), std::move(path));
            }
            fail(DiagCode::E2002, cur().span, std::format("expected an expression, found {}", describe(cur())),
                 "expected an expression");
        }
    }

    ExprPtr parse_struct_literal(Span start, TypePtr type) {
        auto lit  = std::make_unique<StructLiteralExpr>(start, std::move(type));
        Span open = advance().span;
        while (!at(TokenKind::r_brace) && !at(TokenKind::eof)) {
            if (!at(TokenKind::dot)) {
                Span bad = cur().span;
                // Skip the positional contents up to the closing brace.
                int depth = 0;
                while (!at(TokenKind::eof) && !(depth == 0 && at(TokenKind::r_brace))) {
                    if (at(TokenKind::l_brace)) ++depth;
                    if (at(TokenKind::r_brace)) --depth;
                    advance();
                }
                report(DiagCode::E2017, from(bad), "struct construction must name its fields",
                       "positional value")
                    .notes.push_back(Note{"write 'T{ .field = value, ... }'", std::nullopt});
                break;
            }
            advance();
            FieldInit field;
            field.span = cur().span;
            field.name = expect_member_name("a field name after '.'");
            expect(TokenKind::eq, std::format("after '.{}'", field.name));
            field.value = parse_expr();
            field.span  = from(field.span);
            lit->fields.push_back(std::move(field));
            if (!accept(TokenKind::comma)) {
                break;
            }
        }
        expect_close(TokenKind::r_brace, open);
        lit->span = from(start);
        return lit;
    }

    ExprPtr parse_if(bool as_value) {
        Span start = advance().span;
        auto node  = std::make_unique<IfExpr>(start);
        node->is_compile_time = accept(TokenKind::kw_compile_time);
        IsCondition cond      = parse_condition_with_is("if");
        node->then_block      = parse_body("if");
        if (cond.pattern) {   // `if (v is p)` (spec 34)
            if (node->is_compile_time) report(DiagCode::E2001, start, "'if compile_time' takes a condition, not 'is' with a pattern", "'is'");
            ExprPtr else_branch;
            if (accept(TokenKind::kw_else)) {
                else_branch = at(TokenKind::kw_if) ? parse_if(as_value) : ExprPtr(parse_body("else"));
            } else if (as_value) {
                report(DiagCode::E2020, start, "'if' used as a value needs an 'else' branch", "this 'if' has no 'else'");
            }
            return if_is(start, std::move(cond), std::move(node->then_block), std::move(else_branch));
        }
        node->condition = std::move(cond.plain);
        if (accept(TokenKind::kw_else)) {
            if (at(TokenKind::kw_if)) {
                node->else_branch = parse_if(as_value);
            } else {
                node->else_branch = parse_body("else");
            }
        } else if (as_value) {
            report(DiagCode::E2020, start, "'if' used as a value needs an 'else' branch",
                   "this 'if' has no 'else'");
        }
        node->span = from(start);
        return node;
    }

    ExprPtr parse_match() {
        Span start = advance().span;
        auto node  = std::make_unique<MatchExpr>(start);
        Span popen = expect(TokenKind::l_paren, "after 'match'").span;
        node->scrutinee = parse_expr();
        expect_close(TokenKind::r_paren, popen);
        if (!at(TokenKind::l_brace)) {
            fail_unexpected("'{' to start the match arms");
        }
        Span open = advance().span;
        while (!at(TokenKind::r_brace) && !at(TokenKind::eof)) {
            std::size_t before = pos_;
            try {
                MatchArm arm;
                arm.span    = cur().span;
                arm.pattern = parse_pattern();
                if (accept(TokenKind::kw_if)) {
                    arm.guard = parse_expr();
                }
                expect(TokenKind::fat_arrow, "after the pattern");
                arm.body = at(TokenKind::l_brace) ? ExprPtr(parse_block()) : parse_expr();
                arm.span = from(arm.span);
                node->arms.push_back(std::move(arm));
                if (!accept(TokenKind::comma)) {
                    Span where = Span{id_, prev_span_.end, prev_span_.end};
                    report(DiagCode::E2014, where, "match arm must end with ','", "add ','");
                    if (!at(TokenKind::r_brace) && is_block_like(*node->arms.back().body)) {
                        continue;   // next arm follows directly
                    }
                    if (!at(TokenKind::r_brace)) {
                        throw ParseError{};
                    }
                }
            } catch (const ParseError&) {
                skip_to_next_arm();
            }
            if (pos_ == before) {
                advance();
            }
        }
        expect_close(TokenKind::r_brace, open);
        node->span = from(start);
        return node;
    }

    void skip_to_next_arm() {
        int depth = 0;
        while (!at(TokenKind::eof)) {
            if (depth == 0 && at(TokenKind::comma)) {
                advance();
                return;
            }
            if (depth == 0 && at(TokenKind::r_brace)) {
                return;
            }
            if (at(TokenKind::l_brace) || at(TokenKind::l_paren)) ++depth;
            if ((at(TokenKind::r_brace) || at(TokenKind::r_paren)) && depth > 0) --depth;
            advance();
        }
    }

    ExprPtr parse_cast() {
        Span start = advance().span;
        auto node  = std::make_unique<CastExpr>(start);
        if (accept(TokenKind::colon_colon)) {
            node->variant = expect_identifier("a cast kind after 'cast::'");
        }
        expect(TokenKind::less, "after 'cast'");
        node->target = parse_type({});
        expect_template_close();
        Span open = expect(TokenKind::l_paren, "after the cast type").span;
        node->operand = parse_expr();
        expect_close(TokenKind::r_paren, open);
        node->span = from(start);
        return node;
    }

    ExprPtr parse_bit() {
        Span start = advance().span;
        auto node  = std::make_unique<BitExpr>(start);
        if (accept(TokenKind::less)) {
            node->type = parse_type({});
            expect_template_close();
        }
        Span open = expect(TokenKind::l_paren, "after 'bit'").span;
        node->operand = parse_expr();
        expect_close(TokenKind::r_paren, open);
        node->span = from(start);
        return node;
    }

    ExprPtr parse_lambda() {
        Span start = cur().span;
        auto node  = std::make_unique<LambdaExpr>(start);
        Span open  = advance().span;
        while (!at(TokenKind::r_bracket) && !at(TokenKind::eof)) {
            Capture cap;
            cap.span = cur().span;
            if (at(TokenKind::eq) ||
                (at(TokenKind::amp) && (peek(1).kind == TokenKind::r_bracket || peek(1).kind == TokenKind::comma))) {
                report(DiagCode::E2026, cur().span,
                       std::format("'[{}]' captures are not allowed; name every capture", cur().text),
                       "unnamed capture")
                    .notes.push_back(Note{"write e.g. '[x]', '[&x]', '[mut &x]' or '[move x]'", std::nullopt});
                advance();
                if (!accept(TokenKind::comma)) {
                    break;
                }
                continue;
            }
            if (at(TokenKind::kw_mut) && peek(1).kind != TokenKind::amp) {   // `[mut x = 0]`: a value the lambda changes
                advance();
                cap.is_mut = true;
            }
            if (accept(TokenKind::kw_move)) {
                cap.kind = CaptureKind::move;
            } else if (at(TokenKind::kw_mut) && peek(1).kind == TokenKind::amp) {
                advance();
                advance();
                cap.kind = CaptureKind::mut_ref;
            } else if (accept(TokenKind::amp)) {
                cap.kind = CaptureKind::ref;
            } else {
                cap.kind = CaptureKind::copy;
            }
            cap.name = expect_identifier("a capture name");
            if (cap.kind == CaptureKind::copy && accept(TokenKind::eq)) {
                cap.kind = CaptureKind::init;
                cap.init = parse_expr();
            }
            cap.span = from(cap.span);
            node->captures.push_back(std::move(cap));
            if (!accept(TokenKind::comma)) {
                break;
            }
        }
        expect_close(TokenKind::r_bracket, open);
        node->params = parse_params();
        reject_special_params(node->params);
        if (accept(TokenKind::arrow)) {
            node->result = parse_type({});
        }
        if (!at(TokenKind::l_brace)) {
            fail_unexpected("'{' to start the lambda body");
        }
        node->body = parse_block();
        node->span = from(start);
        return node;
    }

    // --- patterns -------------------------------------------------------------------------

    ExprPtr parse_pattern_literal() {
        Span start = cur().span;
        if (at(TokenKind::minus)) {
            advance();
            if (!at(TokenKind::int_literal) && !at(TokenKind::float_literal)) {
                fail_unexpected("a number after '-' in a pattern");
            }
            ExprPtr lit = make_literal(at(TokenKind::int_literal) ? ExprKind::int_literal : ExprKind::float_literal);
            return std::make_unique<UnaryExpr>(from(start), UnaryOp::negate, std::move(lit));
        }
        switch (cur().kind) {
        case TokenKind::int_literal: return make_literal(ExprKind::int_literal);
        case TokenKind::float_literal: return make_literal(ExprKind::float_literal);
        case TokenKind::char_literal: return make_literal(ExprKind::char_literal);
        case TokenKind::string_literal:
        case TokenKind::raw_string_literal: return make_literal(ExprKind::string_literal);
        case TokenKind::kw_true:
        case TokenKind::kw_false: return make_literal(ExprKind::bool_literal);
        default: fail_unexpected("a pattern");
        }
    }

    PatternPtr parse_pattern() {
        Span start = cur().span;
        if (accept(TokenKind::underscore)) {
            return std::make_unique<SimplePattern>(PatternKind::wildcard, start);
        }
        if (at(TokenKind::l_paren)) {   // `(p1, p2)`: a tuple pattern
            Span open  = advance().span;
            auto tuple = std::make_unique<TuplePattern>(open);
            do {
                if (at(TokenKind::r_paren)) break;
                tuple->elements.push_back(parse_pattern());
            } while (accept(TokenKind::comma));
            expect_close(TokenKind::r_paren, open);
            tuple->span = from(start);
            if (tuple->elements.size() < 2) {
                report(DiagCode::E2001, tuple->span, "a tuple pattern has two or more elements", "one element");
            }
            return tuple;
        }
        if (at(TokenKind::identifier)) {
            Path path;
            do {
                PathSegment seg;
                seg.span = cur().span;
                seg.name = expect_identifier("a name");
                path.segments.push_back(std::move(seg));
            } while (accept(TokenKind::colon_colon));
            path.span = from(start);
            if (at(TokenKind::l_paren)) {
                Span open = advance().span;
                auto alt  = std::make_unique<EnumPattern>(start, std::move(path));
                alt->payload = parse_pattern();
                expect_close(TokenKind::r_paren, open);
                alt->span = from(start);
                return alt;
            }
            if (at(TokenKind::l_brace)) {
                return parse_struct_pattern(start, std::move(path));
            }
            if (path.segments.size() == 1) {
                return std::make_unique<BindingPattern>(start, path.segments[0].name);
            }
            return std::make_unique<EnumPattern>(from(start), std::move(path));
        }
        ExprPtr lo = parse_pattern_literal();
        if (at(TokenKind::dot_dot) || at(TokenKind::dot_dot_eq)) {
            auto range       = std::make_unique<RangePattern>(start);
            range->inclusive = advance().is(TokenKind::dot_dot_eq);
            range->lo        = std::move(lo);
            range->hi        = parse_pattern_literal();
            range->span      = from(start);
            return range;
        }
        return std::make_unique<LiteralPattern>(from(start), std::move(lo));
    }

    PatternPtr parse_struct_pattern(Span start, Path path) {
        auto node = std::make_unique<StructPattern>(start, std::move(path));
        Span open = advance().span;
        while (!at(TokenKind::r_brace) && !at(TokenKind::eof)) {
            if (accept(TokenKind::dot_dot)) {
                node->has_rest = true;
                accept(TokenKind::comma);
                break;
            }
            expect(TokenKind::dot, "before the field name in a struct pattern");
            FieldPattern field;
            field.span = cur().span;
            field.name = expect_member_name("a field name");
            expect(TokenKind::eq, std::format("after '.{}'", field.name));
            field.pattern = parse_pattern();
            field.span    = from(field.span);
            node->fields.push_back(std::move(field));
            if (!accept(TokenKind::comma)) {
                break;
            }
        }
        expect_close(TokenKind::r_brace, open);
        node->span = from(start);
        return node;
    }

    const SourceFile&        file_;
    FileId                   id_;
    DiagnosticEngine&        diags_;
    std::vector<Token>       tokens_;
    std::vector<std::string> docs_;
    std::size_t              pos_         = 0;
    bool                     split_       = false;
    Token                    split_token_;
    Span                     prev_span_;
    int                      speculating_ = 0;
    bool                     stray_code_reported_ = false;
    std::vector<std::string>    ns_stack_;    // the open `namespace` blocks (spec 30)
    std::vector<std::string>    type_stack_;  // the struct/class bodies being parsed (nested types)
    std::vector<DeclPtr>        hoisted_;     // nested types, moved to file scope after their outer type
    std::vector<NamespaceAlias> aliases_;
    std::vector<AnnotationUse>  file_annotations_;   // `@a{ ... };` at the top of the file (spec 31)
    std::vector<NamespaceBlock> ns_blocks_;
    Diagnostic               scratch_{DiagCode::E2001, Severity::error, {}, {}, {}, {}};
};

} // namespace

namespace {

// ---- unbalanced delimiters ---------------------------------------------------------------------
//
// A missing or extra `}`, `)` or `]` makes a parser report errors far away from the mistake, one
// after another, until the end of the file. Before parsing, the tokens are matched: when they
// don't balance, indentation says where the mistake is (a closer that lines up with an outer
// opener closes that one; a block ends where the code dedents to the opener's line), the
// mistake is reported there, once, and the tokens are repaired so the parser sees a balanced
// file and reports only what is really wrong elsewhere.

bool is_opener(TokenKind k) { return k == TokenKind::l_brace || k == TokenKind::l_paren || k == TokenKind::l_bracket; }
bool is_closer(TokenKind k) { return k == TokenKind::r_brace || k == TokenKind::r_paren || k == TokenKind::r_bracket; }
TokenKind closer_of(TokenKind k) {
    return k == TokenKind::l_brace ? TokenKind::r_brace : k == TokenKind::l_paren ? TokenKind::r_paren : TokenKind::r_bracket;
}
TokenKind opener_of(TokenKind k) {
    return k == TokenKind::r_brace ? TokenKind::l_brace : k == TokenKind::r_paren ? TokenKind::l_paren : TokenKind::l_bracket;
}

bool balanced(const std::vector<Token>& tokens) {
    std::vector<TokenKind> stack;
    for (const Token& t : tokens) {
        if (is_opener(t.kind)) stack.push_back(t.kind);
        if (is_closer(t.kind)) {
            if (stack.empty() || closer_of(stack.back()) != t.kind) return false;
            stack.pop_back();
        }
    }
    return stack.empty();
}

std::vector<Token> repair_delimiters(const SourceFile& file, FileId id, const std::vector<Token>& tokens, DiagnosticEngine& diags) {
    if (balanced(tokens)) return tokens;
    auto line_of = [&](const Token& t) { return file.line_col(t.span.begin).line; };
    auto indent_of = [&](std::uint32_t line) {
        std::uint32_t n = 0;
        for (char c : file.line_text(line)) {
            if (c == ' ') ++n;
            else if (c == '\t') n += 4;
            else break;
        }
        return n;
    };
    auto first_on_line = [&](std::size_t i) { return i == 0 || line_of(tokens[i - 1]) < line_of(tokens[i]); };
    auto text_of = [&](const Token& t) { return std::string(file.text().substr(t.span.begin, t.span.size())); };
    auto synthetic = [&](TokenKind k, std::uint32_t at) {
        Token t;
        t.kind = k;
        t.span = Span{id, at, at};
        t.text = token_kind_spelling(k);
        return t;
    };
    struct Open {
        std::size_t   index;
        std::uint32_t line;
        std::uint32_t indent;
    };
    constexpr int max_reports = 3;
    int reported = 0;
    auto unclosed = [&](const Open& o, std::string note, std::optional<Span> where) {
        if (reported++ >= max_reports) return;
        std::string open = text_of(tokens[o.index]);
        Diagnostic& d = diags.report(DiagCode::E2016, tokens[o.index].span, std::format("unclosed '{}'", open),
                                     std::format("this '{}' is never closed", open));
        d.notes.push_back(Note{std::move(note), where});
    };

    std::vector<Token> out;
    out.reserve(tokens.size() + 4);
    std::vector<Open> stack;
    // Inserts a closer into `out` before the token that starts at `at`.
    auto insert_before = [&](TokenKind k, std::uint32_t at) {
        auto pos = std::find_if(out.begin(), out.end(), [&](const Token& x) { return x.span.begin >= at && x.span.size() > 0; });
        out.insert(pos, synthetic(k, at));
    };
    // A '(' or '[' left open ends with its statement or where a block starts: the first ';' or
    // '{' after it on its line (`take(1;`, `if (x > 3 {`). Reported and closed there; false when
    // its line has neither.
    auto close_at_semicolon = [&](const Open& o) {
        if (tokens[o.index].kind == TokenKind::l_brace) return false;
        for (std::size_t j = o.index + 1; j < tokens.size() && line_of(tokens[j]) == o.line; ++j) {
            if (tokens[j].kind != TokenKind::semicolon && tokens[j].kind != TokenKind::l_brace) continue;
            std::string close(token_kind_spelling(closer_of(tokens[o.index].kind)));
            unclosed(o, tokens[j].kind == TokenKind::semicolon
                            ? std::format("the statement ends here, before its '{}'", close)
                            : std::format("a block starts here, so the '{}' is probably missing before it", close),
                     tokens[j].span);
            insert_before(closer_of(tokens[o.index].kind), tokens[j].span.begin);
            return true;
        }
        return false;
    };
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const Token& t = tokens[i];
        if (t.kind == TokenKind::eof) {
            // Openers left at the end: each block ends where the code dedents to its opener's line.
            while (!stack.empty()) {
                Open o = stack.back();
                stack.pop_back();
                if (close_at_semicolon(o)) continue;
                std::size_t end = tokens.size() - 1;
                for (std::size_t j = o.index + 1; j < tokens.size() - 1; ++j) {
                    if (first_on_line(j) && line_of(tokens[j]) > o.line && indent_of(line_of(tokens[j])) <= o.indent) {
                        end = j;
                        break;
                    }
                }
                std::string close(token_kind_spelling(closer_of(tokens[o.index].kind)));
                if (end < tokens.size() - 1) {
                    unclosed(o, std::format("line {} is no longer indented inside it: the '{}' is probably missing before it", line_of(tokens[end]), close),
                             tokens[end].span);
                } else {
                    unclosed(o, std::format("the file ends before its '{}'", close), std::nullopt);
                }
                insert_before(closer_of(tokens[o.index].kind), tokens[end].span.begin);   // where the block ends
            }
            out.push_back(t);
            continue;
        }
        if (is_opener(t.kind)) {
            stack.push_back(Open{i, line_of(t), indent_of(line_of(t))});
            out.push_back(t);
            continue;
        }
        if (!is_closer(t.kind)) {
            out.push_back(t);
            continue;
        }
        TokenKind want = opener_of(t.kind);
        bool lines_up_elsewhere = false;
        std::size_t match = stack.size();
        if (first_on_line(i)) {   // a closer at the start of a line lines up with its opener's line
            std::uint32_t col = indent_of(line_of(t));
            for (std::size_t k = stack.size(); k-- > 0;) {
                if (tokens[stack[k].index].kind == want && stack[k].indent == col) {
                    match = k;
                    break;
                }
            }
            lines_up_elsewhere = match < stack.size() && match + 1 < stack.size() &&
                                 !(tokens[stack.back().index].kind == want && stack.back().indent == col);
        }
        if (!lines_up_elsewhere) {
            match = stack.size();
            if (!stack.empty() && tokens[stack.back().index].kind == want) match = stack.size() - 1;
            if (match == stack.size()) {   // the innermost opener of this kind
                for (std::size_t k = stack.size(); k-- > 0;) {
                    if (tokens[stack[k].index].kind == want) {
                        match = k;
                        break;
                    }
                }
            }
        }
        if (match == stack.size()) {   // nothing to close: an extra closer, or a forgotten opener
            out.push_back(t);          // the parser explains it (it knows whether a '{' is missing)
            continue;
        }
        // Everything opened after `match` is unclosed: closed here, or, for a '(' or '[', where its
        // statement ends (the ';' on its line).
        while (stack.size() > match + 1) {
            Open o = stack.back();
            stack.pop_back();
            if (close_at_semicolon(o)) continue;
            unclosed(o, std::format("this '{}' on line {} lines up with the '{}' on line {}, so it closes that one",
                                    text_of(t), line_of(t), text_of(tokens[stack[match].index]), stack[match].line),
                     t.span);
            out.push_back(synthetic(closer_of(tokens[o.index].kind), t.span.begin));
        }
        stack.pop_back();
        out.push_back(t);
    }
    return out;
}

} // namespace

ast::File parse(const SourceFile& file, FileId id, const std::vector<Token>& tokens,
                DiagnosticEngine& diags) {
    std::size_t errors_before = diags.error_count();
    std::vector<Token> repaired = repair_delimiters(file, id, tokens, diags);
    if (diags.error_count() == errors_before) return Parser(file, id, tokens, diags).parse_file();
    // The file had unbalanced delimiters, reported where they are. The repaired tokens are
    // parsed for what else is wrong; errors at the repairs themselves are the same mistake.
    // The syntax tree may point into its tokens' text, so the repaired tokens must live as long
    // as the program does. Only files with this error get here.
    static std::mutex                          keep_lock;
    static std::deque<std::vector<Token>>      kept;
    const std::vector<Token>*                  stable = nullptr;
    {
        std::lock_guard<std::mutex> guard(keep_lock);
        kept.push_back(std::move(repaired));
        stable = &kept.back();
    }
    std::size_t mark = diags.size();
    ast::File parsed = Parser(file, id, *stable, diags).parse_file();
    std::vector<Diagnostic> later = diags.take_since(mark);
    for (Diagnostic& d : later) {
        bool at_repair = false;
        for (const Token& t : *stable) {
            if (t.span.size() == 0 && t.span.begin == d.span.begin) at_repair = true;
        }
        if (!at_repair) diags.add(std::move(d));
    }
    return parsed;
}

ast::ExprPtr parse_expression(const SourceFile& file, FileId id, const std::vector<Token>& tokens,
                              DiagnosticEngine& diags) {
    return Parser(file, id, tokens, diags).parse_single_expression();
}

} // namespace edax
