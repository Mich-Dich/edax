#pragma once
// Semantic model produced by module loading and name resolution (M3).
//
//   Program   every module reached from the entry file through `import`
//   Module    one `.rifth` header and/or `.rifts` source with its own namespace (spec 09)
//   Symbol    anything a name can refer to: module items, members, enum alternatives,
//             template parameters, locals
//
// Symbols are owned by their Module (stable addresses in a deque) and referenced from the AST.

#include "rift/ast.hpp"
#include "rift/diagnostics.hpp"

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rift::types {
class TypeContext;
struct Type;
}

namespace rift::sema {

struct Module;

enum class SymbolKind {
    function,      // free function (all overloads are separate symbols)
    global,        // file-scope static / compile_time variable
    struct_,
    class_,
    interface,
    enum_,
    alternative,   // enum alternative
    field,
    method,        // includes destructors and operators
    type_param,    // `template<type T>`
    value_param,   // `template<usize N>`
    local,         // variable, parameter, loop variable, match binding, lambda capture
};

std::string_view to_string(SymbolKind kind);

/// The std types that are named as members of another std type (`std::thread_pool::options` is
/// the library's `thread_pool_options`, spec 28). Rift has no nested type declarations; these
/// names are the standard library's only ones.
inline bool is_std_nested_type(std::string_view name) {
    return name == "thread_pool_options" || name == "thread_pool_task" || name == "thread_pool_scoped_task";
}

enum class LocalKind { variable, value_param, ref_param, binding, loop_variable, capture };

struct Symbol {
    SymbolKind  kind = SymbolKind::local;
    std::string name;
    Span        span;                 // where the name is declared
    Module*     module  = nullptr;    // owning module (null for locals)
    Symbol*     parent  = nullptr;    // enclosing type for members / alternatives

    const ast::Decl* decl       = nullptr;   // declaration (the header one if there is a header)
    const ast::Decl* definition = nullptr;   // the `.rifts` definition with the body, if separate
    const ast::Decl* source_block = nullptr; // a type's `.rifts` block (method bodies, private members)
    const ast::TemplateParam* template_param = nullptr;   // a template parameter's declaration (constraints)

    bool        in_header  = false;   // declared in the module's .rifth
    bool        is_private = false;   // module-local (`private`, or only in a .rifts with a header)
    ast::Access access     = ast::Access::none;   // class member section (public/protected/private)
    bool        is_static  = false;
    bool        is_mut     = false;   // locals and globals: declared `mut`

    // Types: members by name (methods may be overloaded), bases in declaration order.
    std::map<std::string, std::vector<Symbol*>> members;
    std::vector<Symbol*>                        member_order;   // fields + methods in layout order
    std::vector<Symbol*>                        bases;          // resolved base class / interfaces
    bool                                        has_opaque_base = false;   // e.g. std::formattable

    // Instances of a template type (spec 14): the template, the copy's index among the template's
    // instances (for names), and the type each template parameter stands for.
    const Symbol*                               template_of    = nullptr;
    unsigned                                    instance_index = 0;
    std::map<const Symbol*, const types::Type*> template_subst;
    std::vector<const types::Type*>             template_args;   // the arguments in order

    // Alternatives.
    const ast::EnumAlternative* alternative = nullptr;

    // Locals.
    LocalKind local_kind   = LocalKind::variable;
    bool      is_reference = false;   // declared with a reference type (`T& x`): cannot be moved
    const types::Type* type = nullptr;  // locals: set by the type checker

    // Compile time (spec 15, 26).
    bool is_type_value = false;   // a compile_time variable or parameter of type `type`: usable as a type
    const ast::TypeExpr* value_type = nullptr;   // template value parameters: `usize` in `template<usize N>`
    std::shared_ptr<const ct::Value> ct_value;   // the value of a compile_time constant, once evaluated

    [[nodiscard]] bool is_type() const {
        return kind == SymbolKind::struct_ || kind == SymbolKind::class_ ||
               kind == SymbolKind::interface || kind == SymbolKind::enum_ ||
               kind == SymbolKind::type_param;
    }
    [[nodiscard]] const ast::FunctionDecl* function() const {
        return decl && decl->kind == ast::DeclKind::function ? &decl->as<ast::FunctionDecl>() : nullptr;
    }
    [[nodiscard]] bool is_open() const {
        return decl && (decl->kind == ast::DeclKind::class_) && decl->as<ast::TypeDecl>().is_open;
    }

    /// The base class (not an interface), or null (spec 07: at most one, written first).
    [[nodiscard]] const Symbol* base_class() const {
        for (const Symbol* b : bases) {
            if (b->kind == SymbolKind::class_) return b;
        }
        return nullptr;
    }

    /// Whether objects of this class start with a vtable pointer: an `open` class at the root of
    /// its hierarchy (its destructor is virtual, spec 07). Derived classes reach it through their
    /// base, which is their first member.
    [[nodiscard]] bool has_vtable_pointer() const { return is_open() && !base_class(); }

    /// The members an object starts with before its own fields: the base object or the vtable
    /// pointer (0 or 1).
    [[nodiscard]] unsigned layout_prefix() const { return base_class() || has_vtable_pointer() ? 1 : 0; }
};

/// The parameter substitution of the template instance that declares `s` (a member, payload or
/// alternative belongs to its enclosing type), or null outside instances.
inline const std::map<const Symbol*, const types::Type*>* substitution_of(const Symbol* s) {
    for (; s; s = s->parent) {
        if (!s->template_subst.empty()) return &s->template_subst;
    }
    return nullptr;
}

/// One parsed file of a module.
struct ModuleFile {
    FileId    id;
    ast::File ast;
};

struct Module {
    std::string               name;   // "counter", "shapes::circle"; the entry file's own name too
    std::optional<ModuleFile> header;
    std::optional<ModuleFile> source;

    std::vector<Module*> header_imports;   // imports visible to the header
    std::vector<Module*> source_imports;   // imports written in the source (header imports apply too)
    bool                 header_imports_std = false;
    bool                 source_imports_std = false;

    std::map<std::string, std::vector<Symbol*>> items;   // module scope (functions may overload)
    std::vector<Symbol*>                        item_order;

    std::deque<Symbol> storage;

    Symbol* make(SymbolKind kind, std::string symbol_name, Span span) {
        Symbol& s = storage.emplace_back();
        s.kind    = kind;
        s.name    = std::move(symbol_name);
        s.span    = span;
        s.module  = this;
        return &s;
    }
};

struct Program {
    std::map<std::string, std::unique_ptr<Module>> modules;   // by module name
    Module*                                        entry = nullptr;
    /// The standard library module (library/std, built into riftc), loaded on the first
    /// `import std`; null if nothing imports it. Also in `modules` as "std".
    Module*                                        std_module = nullptr;
    /// The build mode being compiled for (spec 24): decides which mode-gated checks exist (spec 23).
    std::string                                    build_mode = "debug";
    /// The class of that mode (`std::build::mode::release`, or a mode of build.rifts), once
    /// resolved; null without the standard library.
    const Symbol*                                  build_mode_class = nullptr;
    /// The values of the mode's settings by method name, as text: "optimization" → "speed",
    /// "allow_undefined_behavior" → "false" (spec 24). Empty without the standard library.
    std::map<std::string, std::string>             mode_settings;
    /// What build.rifts passed to the target being compiled: `std::build::setting<T>(name)`.
    std::map<std::string, std::shared_ptr<const ct::Value>> build_settings;
    /// The graph that build.rifts' `build(g)` fills in; null when compiling ordinary code.
    ct::BuildGraph*                                build_graph = nullptr;
    /// `--warn=id=level` (spec 23): the level of each named diagnostic that is not the default.
    std::map<std::string, std::string>             warn_levels;
    std::shared_ptr<types::TypeContext>            types;     // owns every Type the checker creates
    /// Instances of template types, in creation order. They are not in any module's items.
    std::vector<Symbol*>                           instances;
    /// The AST copy of each instance (same order), which the later phases walk.
    std::vector<ast::Decl*>                        instance_copies;
    /// The re-parsed copies of templates that instances are made of (see check.cpp).
    std::deque<ast::File>                          instance_files;
    /// Errors in methods of class template instances, kept until the method is used: like
    /// C++, an instance's method is only required to compile when it is called
    /// (`std::darray<std::unique_ptr<T>>` without `clone()`). By the method's declaration.
    std::map<const ast::Decl*, std::vector<Diagnostic>> deferred_errors;
    /// Copies of resolved declarations: the instances of method templates (spec 14).
    std::deque<std::unique_ptr<ast::Decl>>         cloned_decls;
};

} // namespace rift::sema
