#pragma once
// A pre-order walk over every expression inside an expression: sub-expressions, the statements
// of blocks, f-string holes, match arms, loop bodies. Used where the compiler needs to know what
// a piece of code refers to (the holes of a lazy f-string, spec 31).

#include "edax/ast.hpp"

#include <functional>

namespace edax::ast {

// Calls `visit` for `e` and every expression nested in it. `visit` returns false to skip the
// children of that expression.
inline void walk_exprs(Expr& e, const std::function<bool(Expr&)>& visit) {
    if (!visit(e)) return;
    auto go = [&](Expr* x) {
        if (x) walk_exprs(*x, visit);
    };
    auto block = [&](BlockExpr* b) {
        if (!b) return;
        for (StmtPtr& s : b->statements) {
            switch (s->kind) {
            case StmtKind::variable: go(s->as<VariableStmt>().init.get()); break;
            case StmtKind::assign:
                go(s->as<AssignStmt>().target.get());
                go(s->as<AssignStmt>().value.get());
                break;
            case StmtKind::discard: go(s->as<DiscardStmt>().value.get()); break;
            case StmtKind::destructure: go(s->as<DestructureStmt>().init.get()); break;
            case StmtKind::expression: go(s->as<ExprStmt>().expr.get()); break;
            case StmtKind::check: {
                Check& c = s->as<CheckStmt>().check;
                go(c.condition.get());
                go(c.message.get());
                if (c.block) walk_exprs(*c.block, visit);
                break;
            }
            }
        }
        go(b->tail.get());
    };
    switch (e.kind) {
    case ExprKind::string_literal:
        for (FormatPart& p : e.as<LiteralExpr>().format_parts) go(p.expr.get());
        break;
    case ExprKind::unary: go(e.as<UnaryExpr>().operand.get()); break;
    case ExprKind::binary:
        go(e.as<BinaryExpr>().lhs.get());
        go(e.as<BinaryExpr>().rhs.get());
        break;
    case ExprKind::range:
        go(e.as<RangeExpr>().lo.get());
        go(e.as<RangeExpr>().hi.get());
        break;
    case ExprKind::call:
        go(e.as<CallExpr>().callee.get());
        for (ExprPtr& a : e.as<CallExpr>().args) go(a.get());
        block(e.as<CallExpr>().wrapped.get());
        break;
    case ExprKind::index:
        go(e.as<IndexExpr>().object.get());
        go(e.as<IndexExpr>().index.get());
        break;
    case ExprKind::member: go(e.as<MemberExpr>().object.get()); break;
    case ExprKind::try_: go(e.as<TryExpr>().operand.get()); break;
    case ExprKind::cast: go(e.as<CastExpr>().operand.get()); break;
    case ExprKind::bit: go(e.as<BitExpr>().operand.get()); break;
    case ExprKind::struct_literal:
        for (FieldInit& f : e.as<StructLiteralExpr>().fields) go(f.value.get());
        break;
    case ExprKind::lambda:
        for (Capture& c : e.as<LambdaExpr>().captures) go(c.init.get());
        break;   // the body sees only its captures
    case ExprKind::if_: {
        auto& i = e.as<IfExpr>();
        go(i.condition.get());
        block(i.then_block.get());
        go(i.else_branch.get());
        break;
    }
    case ExprKind::match: {
        auto& m = e.as<MatchExpr>();
        go(m.scrutinee.get());
        for (MatchArm& a : m.arms) {
            go(a.guard.get());
            go(a.body.get());
        }
        break;
    }
    case ExprKind::block: block(&e.as<BlockExpr>()); break;
    case ExprKind::while_:
        go(e.as<WhileExpr>().condition.get());
        block(e.as<WhileExpr>().body.get());
        break;
    case ExprKind::for_: {
        auto& f = e.as<ForExpr>();
        go(f.iterable.get());
        if (f.unrolled) {
            for (BlockPtr& c : f.copies) block(c.get());
        } else {
            block(f.body.get());
        }
        break;
    }
    case ExprKind::return_: go(e.as<ReturnExpr>().value.get()); break;
    case ExprKind::tuple:
        for (ExprPtr& x : e.as<TupleExpr>().elements) go(x.get());
        break;
    case ExprKind::list:
        for (ExprPtr& x : e.as<ListExpr>().elements) go(x.get());
        break;
    default: break;
    }
}

} // namespace edax::ast
