#pragma once
// Deep copies of resolved syntax trees (spec 26, `for compile_time`).
//
// A `for compile_time` loop type-checks its body once per element, and every copy needs its own
// nodes (the checker stores types in them). The copy keeps the resolved symbols, except that
// every local the copied code declares gets a fresh symbol, and the code inside the copy refers
// to those. Types and compile-time values are not copied: the checker fills them in again.

#include "edax/ast.hpp"

#include <functional>
#include <map>
#include <string>

namespace edax::ast {

class Cloner {
public:
    // Makes the fresh symbol for a local declared inside the copied code.
    using SymbolMaker = std::function<sema::Symbol*(const sema::Symbol& original)>;

    explicit Cloner(SymbolMaker make) : make_(std::move(make)) {}

    // Uses `to` wherever the copied code refers to `from` (e.g. the loop variable).
    void map(const sema::Symbol* from, sema::Symbol* to) { map_[from] = to; }

    // Expands a parameter pack while copying unresolved code (templates, "Parameter packs"):
    // `for compile_time (A& v : values) { ... }` becomes one block per element, with `A`
    // renamed to `A$i` and `v` bound to `values$i`; `values...` in a call becomes the
    // arguments `values$0, ..., values$n-1`; `values.length()` becomes `n`.
    void expand_pack(std::string type_name, std::string value_name, std::size_t count) {
        pack_type_  = std::move(type_name);
        pack_value_ = std::move(value_name);
        pack_count_ = count;
        pack_       = true;
    }

    // Renames the first segment of every path named `from` (a type parameter, while copying).
    void rename(const std::string& from, const std::string& to) { rename_[from] = to; }

    BlockPtr block(const BlockExpr& b);
    ExprPtr  expr(const Expr& e);
    TypePtr  type(const TypeExpr& t);

    // A copy of a resolved function (an instance of a method template, spec 14): it is no longer
    // a template, and its parameters and locals get fresh symbols.
    std::unique_ptr<FunctionDecl> function(const FunctionDecl& f);

private:
    SymbolRef ref(SymbolRef s) const;
    SymbolRef fresh(SymbolRef s);
    Path      path(const Path& p);
    StmtPtr   stmt(const Stmt& s);
    StmtPtr   stmt_inner(const Stmt& s);
    PatternPtr pattern(const Pattern& p);
    template <typename T> std::unique_ptr<T> opt(const std::unique_ptr<T>& p);

    bool is_pack_value(const Expr& e) const;

    SymbolMaker                                      make_;
    std::map<const sema::Symbol*, sema::Symbol*>     map_;
    std::map<std::string, std::string>               rename_;
    std::string                                      pack_type_;
    std::string                                      pack_value_;
    std::size_t                                      pack_count_ = 0;
    bool                                             pack_       = false;
};

} // namespace edax::ast
