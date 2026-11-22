#include "edax/ast_clone.hpp"

namespace edax::ast {

SymbolRef Cloner::ref(SymbolRef s) const {
    if (!s) return nullptr;
    auto it = map_.find(s);
    return it == map_.end() ? s : it->second;
}

SymbolRef Cloner::fresh(SymbolRef s) {
    if (!s) return nullptr;
    sema::Symbol* copy = make_(*s);
    map_[s]            = copy;
    return copy;
}

template <typename T> std::unique_ptr<T> Cloner::opt(const std::unique_ptr<T>& p) {
    if (!p) return nullptr;
    if constexpr (std::is_same_v<T, Expr>) return expr(*p);
    else if constexpr (std::is_same_v<T, TypeExpr>) return type(*p);
    else if constexpr (std::is_same_v<T, Pattern>) return pattern(*p);
    else if constexpr (std::is_same_v<T, BlockExpr>) return block(*p);
}

bool Cloner::is_pack_value(const Expr& e) const {
    if (e.kind != ExprKind::path) return false;
    const Path& p = e.as<PathExpr>().path;
    return !p.global && p.segments.size() == 1 && p.segments[0].name == pack_value_;
}

Path Cloner::path(const Path& p) {
    Path out;
    out.span   = p.span;
    out.global = p.global;
    for (const PathSegment& s : p.segments) {
        PathSegment seg;
        seg.name              = s.name;
        if (out.segments.empty() && !p.global && !rename_.empty()) {
            if (auto it = rename_.find(s.name); it != rename_.end()) seg.name = it->second;
        }
        seg.span              = s.span;
        seg.has_template_args = s.has_template_args;
        for (const TemplateArg& a : s.template_args) {
            if (pack_ && a.value && a.value->kind == ExprKind::unary && a.value->as<UnaryExpr>().op == UnaryOp::spread &&
                is_pack_value(*a.value->as<UnaryExpr>().operand)) {   // `f<N...>`: one argument per element (spec 34)
                for (std::size_t i = 0; i < pack_count_; ++i) {
                    Path element;
                    element.span = a.span;
                    element.segments.push_back(PathSegment{pack_value_ + "$" + std::to_string(i), a.span, false, {}});
                    TemplateArg arg;
                    arg.value = std::make_unique<PathExpr>(a.span, std::move(element));
                    arg.span  = a.span;
                    seg.template_args.push_back(std::move(arg));
                }
                continue;
            }
            TemplateArg arg;
            arg.type   = opt(a.type);
            arg.value  = opt(a.value);
            arg.is_mut = a.is_mut;
            arg.span   = a.span;
            seg.template_args.push_back(std::move(arg));
        }
        out.segments.push_back(std::move(seg));
    }
    return out;
}

TypePtr Cloner::type(const TypeExpr& t) {
    switch (t.kind) {
    case TypeKind::named: {
        const auto& n   = t.as<NamedType>();
        auto        out = std::make_unique<NamedType>(n.span, path(n.path));
        out->symbol     = ref(n.symbol);
        return out;
    }
    case TypeKind::reference: {
        const auto& r   = t.as<ReferenceType>();
        auto        out = std::make_unique<ReferenceType>(r.span, type(*r.inner));
        out->is_mut     = r.is_mut;
        return out;
    }
    case TypeKind::function: {
        const auto& f   = t.as<FunctionType>();
        auto        out = std::make_unique<FunctionType>(f.span, opt(f.result));
        for (const TypePtr& p : f.params) out->params.push_back(type(*p));
        return out;
    }
    case TypeKind::auto_:
    case TypeKind::deduce: return std::make_unique<PlaceholderType>(t.kind, t.span);
    case TypeKind::tuple: {
        auto out = std::make_unique<TupleType>(t.span);
        for (const TypePtr& e : t.as<TupleType>().elements) out->elements.push_back(type(*e));
        return out;
    }
    }
    return nullptr;
}

BlockPtr Cloner::block(const BlockExpr& b) {
    auto out       = std::make_unique<BlockExpr>(b.span);
    out->is_unsafe = b.is_unsafe;
    for (const StmtPtr& s : b.statements) out->statements.push_back(stmt(*s));
    out->tail = opt(b.tail);
    return out;
}

StmtPtr Cloner::stmt(const Stmt& s) {
    StmtPtr out = stmt_inner(s);
    if (out) {
        for (const ExprPtr& a : s.allowed) out->allowed.push_back(expr(*a));
    }
    return out;
}

StmtPtr Cloner::stmt_inner(const Stmt& s) {
    switch (s.kind) {
    case StmtKind::variable: {
        const auto& v   = s.as<VariableStmt>();
        auto        out = std::make_unique<VariableStmt>(v.span);
        out->storage    = v.storage;
        out->is_mut     = v.is_mut;
        out->type       = opt(v.type);
        out->name       = v.name;
        out->name_span  = v.name_span;
        out->init       = opt(v.init);   // the initializer cannot see the variable itself
        out->symbol     = fresh(v.symbol);
        return out;
    }
    case StmtKind::assign: {
        const auto& a = s.as<AssignStmt>();
        return std::make_unique<AssignStmt>(a.span, a.op, expr(*a.target), expr(*a.value));
    }
    case StmtKind::discard: {
        const auto& d = s.as<DiscardStmt>();
        return std::make_unique<DiscardStmt>(d.span, expr(*d.value));
    }
    case StmtKind::destructure: {
        const auto& d   = s.as<DestructureStmt>();
        auto        out = std::make_unique<DestructureStmt>(d.span);
        out->is_auto    = d.is_auto;
        out->init       = opt(d.init);
        for (const DestructureBinding& b : d.bindings) {
            DestructureBinding c;
            c.is_mut   = b.is_mut;
            c.wildcard = b.wildcard;
            c.type     = opt(b.type);
            c.name     = b.name;
            c.span     = b.span;
            c.symbol   = fresh(b.symbol);
            out->bindings.push_back(std::move(c));
        }
        return out;
    }
    case StmtKind::expression: {
        const auto& e = s.as<ExprStmt>();
        return std::make_unique<ExprStmt>(e.span, expr(*e.expr));
    }
    case StmtKind::check: {
        const Check& c   = s.as<CheckStmt>().check;
        auto         out = std::make_unique<CheckStmt>(s.span);
        Check&       k   = out->check;
        k.is_compile_time    = c.is_compile_time;
        k.is_run_time        = c.is_run_time;
        k.compile_modes      = c.compile_modes;
        k.run_modes          = c.run_modes;
        k.compile_modes_span = c.compile_modes_span;
        k.run_modes_span     = c.run_modes_span;
        k.condition          = opt(c.condition);
        k.message            = opt(c.message);
        k.block              = opt(c.block);
        return out;
    }
    }
    return nullptr;
}

PatternPtr Cloner::pattern(const Pattern& p) {
    switch (p.kind) {
    case PatternKind::wildcard:
    case PatternKind::error: return std::make_unique<SimplePattern>(p.kind, p.span);
    case PatternKind::literal: return std::make_unique<LiteralPattern>(p.span, expr(*p.as<LiteralPattern>().literal));
    case PatternKind::range: {
        const auto& r   = p.as<RangePattern>();
        auto        out = std::make_unique<RangePattern>(r.span);
        out->lo         = opt(r.lo);
        out->hi         = opt(r.hi);
        out->inclusive  = r.inclusive;
        return out;
    }
    case PatternKind::binding: {
        const auto& b   = p.as<BindingPattern>();
        auto        out = std::make_unique<BindingPattern>(b.span, b.name);
        out->symbol     = fresh(b.symbol);
        return out;
    }
    case PatternKind::enum_alternative: {
        const auto& e   = p.as<EnumPattern>();
        auto        out = std::make_unique<EnumPattern>(e.span, path(e.path));
        out->payload    = opt(e.payload);
        out->symbol     = ref(e.symbol);
        return out;
    }
    case PatternKind::tuple: {
        auto out = std::make_unique<TuplePattern>(p.span);
        for (const PatternPtr& e : p.as<TuplePattern>().elements) out->elements.push_back(pattern(*e));
        return out;
    }
    case PatternKind::struct_: {
        const auto& sp  = p.as<StructPattern>();
        auto        out = std::make_unique<StructPattern>(sp.span, path(sp.path));
        out->has_rest   = sp.has_rest;
        for (const FieldPattern& f : sp.fields) out->fields.push_back(FieldPattern{f.name, f.span, opt(f.pattern)});
        return out;
    }
    }
    return nullptr;
}

ExprPtr Cloner::expr(const Expr& e) {
    switch (e.kind) {
    case ExprKind::int_literal:
    case ExprKind::float_literal:
    case ExprKind::char_literal:
    case ExprKind::string_literal:
    case ExprKind::bool_literal:
    case ExprKind::nullptr_literal: {
        const auto& l   = e.as<LiteralExpr>();
        auto        out = std::make_unique<LiteralExpr>(l.kind, l.span, l.token_kind);
        out->text       = l.text;
        out->value      = l.value;
        out->int_value  = l.int_value;
        out->suffix     = l.suffix;
        out->bool_value = l.bool_value;
        for (const FormatPart& part : l.format_parts) {
            FormatPart p;
            p.text = part.text;
            p.expr = opt(part.expr);
            p.spec = part.spec;
            p.span = part.span;
            out->format_parts.push_back(std::move(p));
        }
        return out;
    }
    case ExprKind::this_:
    case ExprKind::default_:
    case ExprKind::break_:
    case ExprKind::continue_:
    case ExprKind::error: return std::make_unique<SimpleExpr>(e.kind, e.span);
    case ExprKind::path: {
        const auto& p   = e.as<PathExpr>();
        auto        out = std::make_unique<PathExpr>(p.span, path(p.path));
        out->symbol     = ref(p.symbol);
        return out;
    }
    case ExprKind::unary: {
        const auto& u = e.as<UnaryExpr>();
        return std::make_unique<UnaryExpr>(u.span, u.op, expr(*u.operand));
    }
    case ExprKind::binary: {
        const auto& b = e.as<BinaryExpr>();
        return std::make_unique<BinaryExpr>(b.span, b.op, expr(*b.lhs), expr(*b.rhs));
    }
    case ExprKind::range: {
        const auto& r = e.as<RangeExpr>();
        return std::make_unique<RangeExpr>(r.span, expr(*r.lo), expr(*r.hi), r.inclusive);
    }
    case ExprKind::call: {
        const auto& c = e.as<CallExpr>();
        if (pack_ && c.args.empty() && c.callee->kind == ExprKind::member) {   // `values.len()`
            const auto& m = c.callee->as<MemberExpr>();
            if (is_pack_value(*m.object) && m.name.segments.size() == 1 && m.name.segments[0].name == "len") {
                auto n       = std::make_unique<LiteralExpr>(ExprKind::int_literal, c.span, TokenKind::int_literal);
                n->int_value = pack_count_;
                n->text      = std::to_string(pack_count_) + "usize";
                n->suffix    = "usize";
                return n;
            }
        }
        auto out = std::make_unique<CallExpr>(c.span, expr(*c.callee));
        for (const ExprPtr& a : c.args) {
            if (pack_ && a->kind == ExprKind::unary && a->as<UnaryExpr>().op == UnaryOp::spread && is_pack_value(*a->as<UnaryExpr>().operand)) {
                for (std::size_t i = 0; i < pack_count_; ++i) {   // `values...`
                    Path element;
                    element.span = a->span;
                    element.segments.push_back(PathSegment{pack_value_ + "$" + std::to_string(i), a->span, false, {}});
                    out->args.push_back(std::make_unique<PathExpr>(a->span, std::move(element)));
                }
                continue;
            }
            out->args.push_back(expr(*a));
        }
        if (c.wrapped) out->wrapped = block(*c.wrapped);
        return out;
    }
    case ExprKind::index: {
        const auto& i = e.as<IndexExpr>();
        return std::make_unique<IndexExpr>(i.span, expr(*i.object), expr(*i.index));
    }
    case ExprKind::member: {
        const auto& m   = e.as<MemberExpr>();
        auto        out = std::make_unique<MemberExpr>(m.span, expr(*m.object), path(m.name), m.arrow);
        out->symbol     = ref(m.symbol);
        return out;
    }
    case ExprKind::try_: return std::make_unique<TryExpr>(e.span, expr(*e.as<TryExpr>().operand));
    case ExprKind::cast: {
        const auto& c   = e.as<CastExpr>();
        auto        out = std::make_unique<CastExpr>(c.span);
        out->variant    = c.variant;
        out->target     = opt(c.target);
        out->operand    = opt(c.operand);
        return out;
    }
    case ExprKind::bit: {
        const auto& b   = e.as<BitExpr>();
        auto        out = std::make_unique<BitExpr>(b.span);
        out->type       = opt(b.type);
        out->operand    = opt(b.operand);
        return out;
    }
    case ExprKind::tuple: {
        auto out = std::make_unique<TupleExpr>(e.span);
        for (const ExprPtr& x : e.as<TupleExpr>().elements) out->elements.push_back(expr(*x));
        return out;
    }
    case ExprKind::list: {
        auto out = std::make_unique<ListExpr>(e.span);
        for (const ExprPtr& x : e.as<ListExpr>().elements) out->elements.push_back(expr(*x));
        return out;
    }
    case ExprKind::struct_literal: {
        const auto& s   = e.as<StructLiteralExpr>();
        auto        out = std::make_unique<StructLiteralExpr>(s.span, opt(s.type));
        for (const FieldInit& f : s.fields) out->fields.push_back(FieldInit{f.name, f.span, opt(f.value)});
        return out;
    }
    case ExprKind::lambda: {
        const auto& l   = e.as<LambdaExpr>();
        auto        out = std::make_unique<LambdaExpr>(l.span);
        for (const Capture& c : l.captures) {
            Capture copy{c.kind, c.name, c.span, opt(c.init), c.is_mut, nullptr, ref(c.captured)};
            copy.symbol = fresh(c.symbol);
            out->captures.push_back(std::move(copy));
        }
        for (const Param& p : l.params) {
            Param copy;
            copy.is_mut = p.is_mut;
            copy.type   = opt(p.type);
            copy.name   = p.name;
            copy.span   = p.span;
            copy.symbol = fresh(p.symbol);
            out->params.push_back(std::move(copy));
        }
        out->result = opt(l.result);
        out->body   = opt(l.body);
        return out;
    }
    case ExprKind::if_: {
        const auto& i         = e.as<IfExpr>();
        auto        out       = std::make_unique<IfExpr>(i.span);
        out->is_compile_time  = i.is_compile_time;
        out->from_call_else   = i.from_call_else;
        out->condition        = opt(i.condition);
        out->then_block       = opt(i.then_block);
        out->else_branch      = opt(i.else_branch);
        return out;
    }
    case ExprKind::match: {
        const auto& m   = e.as<MatchExpr>();
        auto        out = std::make_unique<MatchExpr>(m.span);
        out->scrutinee  = opt(m.scrutinee);
        for (const MatchArm& a : m.arms) {
            MatchArm arm;
            arm.pattern = opt(a.pattern);
            arm.guard   = opt(a.guard);
            arm.body    = opt(a.body);
            arm.span    = a.span;
            out->arms.push_back(std::move(arm));
        }
        return out;
    }
    case ExprKind::block: return block(e.as<BlockExpr>());
    case ExprKind::while_: {
        const auto& w   = e.as<WhileExpr>();
        auto        out = std::make_unique<WhileExpr>(w.span);
        out->condition  = opt(w.condition);
        out->body       = opt(w.body);
        return out;
    }
    case ExprKind::for_: {
        const auto& f = e.as<ForExpr>();
        if (pack_ && f.is_compile_time && f.iterable && is_pack_value(*f.iterable)) {
            // One block per element: `{ A$i& v = values$i; body }`.
            auto all = std::make_unique<BlockExpr>(f.span);
            for (std::size_t i = 0; i < pack_count_; ++i) {
                std::string element = pack_type_ + "$" + std::to_string(i);
                auto        saved   = rename_;
                if (!pack_type_.empty()) rename_[pack_type_] = element;   // a value pack renames no type
                auto one            = std::make_unique<BlockExpr>(f.span);
                auto var            = std::make_unique<VariableStmt>(f.span);
                var->is_mut         = f.is_mut;
                var->type           = opt(f.type);
                var->name           = f.name;
                var->name_span      = f.name_span;
                Path value;
                value.span = f.iterable->span;
                value.segments.push_back(PathSegment{pack_value_ + "$" + std::to_string(i), f.iterable->span, false, {}});
                var->init = std::make_unique<PathExpr>(f.iterable->span, std::move(value));
                one->statements.push_back(std::move(var));
                if (f.body) one->statements.push_back(std::make_unique<ExprStmt>(f.body->span, block(*f.body)));
                rename_ = std::move(saved);
                all->statements.push_back(std::make_unique<ExprStmt>(f.span, std::move(one)));
            }
            return all;
        }
        auto        out      = std::make_unique<ForExpr>(f.span);
        out->is_compile_time = f.is_compile_time;
        out->is_mut          = f.is_mut;
        out->type            = opt(f.type);
        out->name            = f.name;
        out->name_span       = f.name_span;
        out->iterable        = opt(f.iterable);
        out->symbol          = fresh(f.symbol);
        out->body            = opt(f.body);
        return out;
    }
    case ExprKind::return_: return std::make_unique<ReturnExpr>(e.span, opt(e.as<ReturnExpr>().value));
    }
    return nullptr;
}

std::unique_ptr<FunctionDecl> Cloner::function(const FunctionDecl& f) {
    auto out             = std::make_unique<FunctionDecl>(f.span);
    out->name            = f.name;
    out->name_span       = f.name_span;
    out->doc             = f.doc;
    out->ns              = f.ns;
    out->is_private      = f.is_private;
    out->access          = f.access;
    out->result          = opt(f.result);
    out->is_static       = f.is_static;
    out->is_virtual      = f.is_virtual;
    out->is_override     = f.is_override;
    out->is_abstract     = f.is_abstract;
    out->is_mut          = f.is_mut;
    out->is_move         = f.is_move;
    out->is_compile_time = f.is_compile_time;
    out->is_run_time     = f.is_run_time;
    out->is_unsafe       = f.is_unsafe;
    out->is_extern       = f.is_extern;
    out->abi             = f.abi;
    out->is_destructor   = f.is_destructor;
    out->is_operator     = f.is_operator;
    out->is_defaulted    = f.is_defaulted;
    for (const Param& p : f.params) {
        Param copy;
        copy.is_mut          = p.is_mut;
        copy.is_implicit     = p.is_implicit;
        copy.is_compile_time = p.is_compile_time;
        copy.type            = opt(p.type);
        copy.name            = p.name;
        copy.span            = p.span;
        copy.symbol          = fresh(p.symbol);
        out->params.push_back(std::move(copy));
    }
    out->body = opt(f.body);
    return out;
}

} // namespace edax::ast
