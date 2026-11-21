// `riftc definition <file> <line>:<column>`: where the name at a place in a file is declared, for
// an editor's "go to definition". The program is loaded, resolved and type-checked as far as it
// goes; then every name in the file (paths, types, members, struct literal fields, patterns,
// imports) is matched against the place, the innermost first.

#include "rift/definition.hpp"

#include <functional>
#include <optional>

namespace rift {
namespace {

using namespace ast;

class Finder {
public:
    Finder(const sema::Program& program, std::uint32_t offset) : program_(program), offset_(offset) {}

    std::optional<Span> result() const { return best_target_; }

    void file(const File& f) {
        for (const DeclPtr& d : f.decls) decl(*d);
        for (const NamespaceAlias& a : f.aliases) path(a.target, nullptr);
    }

private:
    const sema::Program&  program_;
    std::uint32_t         offset_;
    std::optional<Span>   best_target_;
    std::uint32_t         best_size_ = UINT32_MAX;

    bool covers(Span s) const { return s.begin <= offset_ && offset_ <= s.end && s.end > s.begin; }

    void found(Span where, const sema::Symbol* s) {
        if (!s || !covers(where) || where.size() > best_size_) return;
        while (s->template_of && s->template_of->kind == s->kind) s = s->template_of;   // an instance: its template
        best_size_   = where.size();
        best_target_ = s->span;
    }

    void found_file(Span where, Span target) {
        if (!covers(where) || where.size() > best_size_) return;
        best_size_   = where.size();
        best_target_ = target;
    }

    // A path naming `s`: only the last segment is `s` (`wnd::monitor` names monitor, not wnd).
    void path(const Path& p, const sema::Symbol* s) {
        for (const PathSegment& seg : p.segments) {
            for (const TemplateArg& a : seg.template_args) {
                if (a.type) type(*a.type);
                if (a.value) expr(*a.value);
            }
        }
        if (p.segments.empty()) return;
        found(p.segments.back().span, s);
        // `wnd::system::start`, `shape::circle`: the segment before a member names its type.
        if (s && s->parent && s->parent->is_type() && p.segments.size() >= 2) found(p.segments[p.segments.size() - 2].span, s->parent);
    }

    void type(const TypeExpr& t) {
        switch (t.kind) {
        case TypeKind::named: path(t.as<NamedType>().path, t.as<NamedType>().symbol); break;
        case TypeKind::reference: type(*t.as<ReferenceType>().inner); break;
        case TypeKind::function: {
            const auto& f = t.as<FunctionType>();
            if (f.result) type(*f.result);
            for (const TypePtr& p : f.params) type(*p);
            break;
        }
        case TypeKind::tuple:
            for (const TypePtr& e : t.as<TupleType>().elements) type(*e);
            break;
        default: break;
        }
    }

    void annotations(const std::vector<AnnotationUse>& uses) {
        for (const AnnotationUse& a : uses) {
            if (a.value) expr(*a.value);
        }
    }

    void params(const std::vector<Param>& ps) {
        for (const Param& p : ps) {
            annotations(p.annotations);
            if (p.type) type(*p.type);
        }
    }

    void decl(const Decl& d) {
        annotations(d.annotations);
        for (const TemplateParam& tp : d.template_params) {
            for (const TypePtr& c : tp.constraints) type(*c);
            if (tp.value_type) type(*tp.value_type);
            if (tp.default_type) type(*tp.default_type);
            if (tp.default_value) expr(*tp.default_value);
        }
        switch (d.kind) {
        case DeclKind::import: {
            const auto& im = d.as<ImportDecl>();
            std::string name;
            for (const PathSegment& seg : im.path.segments) name += (name.empty() ? "" : "::") + seg.name;
            if (auto it = program_.modules.find(name); it != program_.modules.end()) {
                const sema::Module& m = *it->second;
                FileId file = m.header ? m.header->id : m.source ? m.source->id : 0;
                if (m.header || m.source) found_file(im.path.span, Span{file, 0, 0});
            }
            break;
        }
        case DeclKind::function: {
            const auto& f = d.as<FunctionDecl>();
            params(f.params);
            if (f.result) type(*f.result);
            if (f.body) expr(*f.body);
            break;
        }
        case DeclKind::variable:
        case DeclKind::field: {
            const auto& v = d.as<VariableDecl>();
            if (v.type) type(*v.type);
            if (v.init) expr(*v.init);
            break;
        }
        case DeclKind::struct_:
        case DeclKind::class_:
        case DeclKind::interface: {
            const auto& t = d.as<TypeDecl>();
            for (const TypePtr& b : t.bases) type(*b);
            for (const DeclPtr& m : t.members) decl(*m);
            break;
        }
        case DeclKind::enum_: {
            const auto& e = d.as<EnumDecl>();
            if (e.tag_type) type(*e.tag_type);
            for (const EnumAlternative& a : e.alternatives) {
                annotations(a.annotations);
                if (a.payload) type(*a.payload);
                if (a.value) expr(*a.value);
            }
            break;
        }
        case DeclKind::check: break;
        }
    }

    void pattern(const Pattern& p) {
        switch (p.kind) {
        case PatternKind::enum_alternative: {
            const auto& e = p.as<EnumPattern>();
            path(e.path, e.symbol);
            if (e.payload) pattern(*e.payload);
            break;
        }
        case PatternKind::struct_:
            for (const FieldPattern& f : p.as<StructPattern>().fields) {
                if (f.pattern) pattern(*f.pattern);
            }
            break;
        case PatternKind::tuple:
            for (const PatternPtr& e : p.as<TuplePattern>().elements) pattern(*e);
            break;
        case PatternKind::literal: expr(*p.as<LiteralPattern>().literal); break;
        default: break;
        }
    }

    void block(const BlockExpr* b) {
        if (!b) return;
        for (const StmtPtr& s : b->statements) {
            switch (s->kind) {
            case StmtKind::variable: {
                const auto& v = s->as<VariableStmt>();
                if (v.type) type(*v.type);
                if (v.init) expr(*v.init);
                break;
            }
            case StmtKind::assign:
                expr(*s->as<AssignStmt>().target);
                expr(*s->as<AssignStmt>().value);
                break;
            case StmtKind::discard: expr(*s->as<DiscardStmt>().value); break;
            case StmtKind::expression: expr(*s->as<ExprStmt>().expr); break;
            case StmtKind::destructure: {
                const auto& ds = s->as<DestructureStmt>();
                for (const DestructureBinding& b2 : ds.bindings) {
                    if (b2.type) type(*b2.type);
                }
                expr(*ds.init);
                break;
            }
            case StmtKind::check: {
                const Check& c = s->as<CheckStmt>().check;
                if (c.condition) expr(*c.condition);
                if (c.message) expr(*c.message);
                if (c.block) expr(*c.block);
                break;
            }
            }
        }
        if (b->tail) expr(*b->tail);
    }

    // The type a struct literal builds, for its `.field = value` names.
    const sema::Symbol* literal_type(const StructLiteralExpr& l) const {
        if (!l.type || l.type->kind != TypeKind::named) return nullptr;
        return l.type->as<NamedType>().symbol;
    }

    void expr(const Expr& e) {
        switch (e.kind) {
        case ExprKind::path: path(e.as<PathExpr>().path, e.as<PathExpr>().symbol); return;
        case ExprKind::member: {
            const auto& m = e.as<MemberExpr>();
            expr(*m.object);
            path(m.name, m.symbol);
            return;
        }
        case ExprKind::struct_literal: {
            const auto& l = e.as<StructLiteralExpr>();
            if (l.type) type(*l.type);
            const sema::Symbol* t = literal_type(l);
            for (const FieldInit& f : l.fields) {
                if (t) {
                    if (auto it = t->members.find(f.name); it != t->members.end() && !it->second.empty()) found(f.span, it->second.front());
                }
                if (f.value) expr(*f.value);
            }
            return;
        }
        case ExprKind::cast: {
            const auto& c = e.as<CastExpr>();
            if (c.target) type(*c.target);
            expr(*c.operand);
            return;
        }
        case ExprKind::bit: {
            const auto& b = e.as<BitExpr>();
            if (b.type) type(*b.type);
            expr(*b.operand);
            return;
        }
        case ExprKind::lambda: {
            const auto& l = e.as<LambdaExpr>();
            for (const Capture& c : l.captures) {
                if (c.init) expr(*c.init);
            }
            params(l.params);
            if (l.result) type(*l.result);
            block(l.body.get());
            return;
        }
        case ExprKind::match: {
            const auto& m = e.as<MatchExpr>();
            expr(*m.scrutinee);
            for (const MatchArm& a : m.arms) {
                if (a.pattern) pattern(*a.pattern);
                if (a.guard) expr(*a.guard);
                if (a.body) expr(*a.body);
            }
            return;
        }
        case ExprKind::for_: {
            const auto& f = e.as<ForExpr>();
            if (f.type) type(*f.type);
            expr(*f.iterable);
            block(f.body.get());
            return;
        }
        case ExprKind::block: block(&e.as<BlockExpr>()); return;
        case ExprKind::if_: {
            const auto& i = e.as<IfExpr>();
            expr(*i.condition);
            block(i.then_block.get());
            if (i.else_branch) expr(*i.else_branch);
            return;
        }
        case ExprKind::while_:
            expr(*e.as<WhileExpr>().condition);
            block(e.as<WhileExpr>().body.get());
            return;
        case ExprKind::string_literal:
            for (const FormatPart& p : e.as<LiteralExpr>().format_parts) {
                if (p.expr) expr(*p.expr);
            }
            return;
        case ExprKind::unary: expr(*e.as<UnaryExpr>().operand); return;
        case ExprKind::binary:
            expr(*e.as<BinaryExpr>().lhs);
            expr(*e.as<BinaryExpr>().rhs);
            return;
        case ExprKind::range:
            if (e.as<RangeExpr>().lo) expr(*e.as<RangeExpr>().lo);
            if (e.as<RangeExpr>().hi) expr(*e.as<RangeExpr>().hi);
            return;
        case ExprKind::call: {
            const auto& c = e.as<CallExpr>();
            expr(*c.callee);
            for (const ExprPtr& a : c.args) expr(*a);
            return;
        }
        case ExprKind::index:
            expr(*e.as<IndexExpr>().object);
            expr(*e.as<IndexExpr>().index);
            return;
        case ExprKind::try_: expr(*e.as<TryExpr>().operand); return;
        case ExprKind::return_:
            if (e.as<ReturnExpr>().value) expr(*e.as<ReturnExpr>().value);
            return;
        case ExprKind::tuple:
            for (const ExprPtr& x : e.as<TupleExpr>().elements) expr(*x);
            return;
        case ExprKind::list:
            for (const ExprPtr& x : e.as<ListExpr>().elements) expr(*x);
            return;
        default: return;
        }
    }
};

} // namespace

std::optional<Span> find_definition(const sema::Program& program, FileId file, std::uint32_t offset) {
    Finder f(program, offset);
    for (const auto& [name, m] : program.modules) {
        if (m->header && m->header->id == file) f.file(m->header->ast);
        if (m->source && m->source->id == file) f.file(m->source->ast);
    }
    return f.result();
}

} // namespace rift
