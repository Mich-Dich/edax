#pragma once
// Semantic model produced by module loading and name resolution (M3).
//
//   Program   every module reached from the entry file through `import`
//   Module    one `.edh` header and/or `.ed` source with its own namespace (spec 09)
//   Symbol    anything a name can refer to: module items, members, enum alternatives,
//             template parameters, locals
//
// Symbols are owned by their Module (stable addresses in a deque) and referenced from the AST.

#include "edax/token.hpp"
#include "edax/ast.hpp"
#include "edax/diagnostics.hpp"

#include <cstdlib>
#include <deque>
#include <map>
#include <unordered_map>
#include <string_view>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace edax::types {
class TypeContext;
struct Type;
}

namespace edax::sema {

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


enum class LocalKind { variable, value_param, ref_param, binding, loop_variable, capture };

struct Symbol {
    SymbolKind  kind = SymbolKind::local;
    std::string name;
    Span        span;                 // where the name is declared
    Module*     module  = nullptr;    // owning module (null for locals)
    Symbol*     parent  = nullptr;    // enclosing type for members / alternatives
    std::string ns;                   // the namespace it is declared in, "a::b" ("" = global, spec 30)

    const ast::Decl* decl       = nullptr;   // declaration (the header one if there is a header)
    const ast::Decl* definition = nullptr;   // the `.ed` definition with the body, if separate
    const ast::Decl* source_block = nullptr; // a type's `.ed` block (method bodies, private members)
    const ast::TemplateParam* template_param = nullptr;   // a template parameter's declaration (constraints)

    bool        in_header  = false;   // declared in the module's .edh
    bool        is_private = false;   // module-local (`private`, or only in a .ed with a header)
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
    // A function template with a parameter pack, made ordinary for `pack_count` arguments
    // (templates, "Parameter packs"): the template it was made from.
    // `@std::simd::multiversion` (SIMD, "Choosing widths"): the copies of the function, one per
    // CPU level, the last one the build's own; a copy's level ("" for the build's own).
    std::vector<Symbol*>                        versions;
    std::string                                 cpu_level;
    // `@std::optimize{ .level = ... }` (spec 34): the function's own optimization ("" = the mode's).
    std::string                                 optimize_level;
    // `@std::float_mode{ ... }` (spec 34): fast-math flags for the function's float operations,
    // a set of `float_mode_*` bits; 0 = strict IEEE.
    unsigned                                    float_mode     = 0;
    const Symbol*                               version_of     = nullptr;
    // A field of a template instance whose `= default` doesn't work for the argument
    // (`A alloc = default;` with an arena allocator): the instance has no default then.
    bool                                        no_default     = false;
    const Symbol*                               pack_origin    = nullptr;
    std::size_t                                 pack_count     = 0;
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

    // The name with its namespace: "std::darray", "geometry::vec2", "main". Module items are
    // stored under it.
    [[nodiscard]] std::string key() const { return ns.empty() ? name : ns + "::" + name; }

    [[nodiscard]] bool is_type() const {
        return kind == SymbolKind::struct_ || kind == SymbolKind::class_ ||
               kind == SymbolKind::interface || kind == SymbolKind::enum_ ||
               kind == SymbolKind::type_param;
    }
    [[nodiscard]] const ast::FunctionDecl* function() const {
        return decl && decl->kind == ast::DeclKind::function ? &decl->as<ast::FunctionDecl>() : nullptr;
    }
    // For a symbol known to be a function or method: its declaration.
    [[nodiscard]] const ast::FunctionDecl& function_decl() const {
        const ast::FunctionDecl* f = function();
        if (!f) std::abort();   // a function symbol always keeps its declaration
        return *f;
    }
    [[nodiscard]] bool is_open() const {
        return decl && (decl->kind == ast::DeclKind::class_) && decl->as<ast::TypeDecl>().is_open;
    }

    // The base class (not an interface), or null (spec 07: at most one, written first).
    [[nodiscard]] const Symbol* base_class() const {
        for (const Symbol* b : bases) {
            if (b->kind == SymbolKind::class_) return b;
        }
        return nullptr;
    }

    // Whether objects of this class start with a vtable pointer: an `open` class at the root of
    // its hierarchy (its destructor is virtual, spec 07). Derived classes reach it through their
    // base, which is their first member.
    [[nodiscard]] bool has_vtable_pointer() const { return is_open() && !base_class(); }

    // The members an object starts with before its own fields: the base object or the vtable
    // pointer (0 or 1).
    [[nodiscard]] unsigned layout_prefix() const { return base_class() || has_vtable_pointer() ? 1 : 0; }
};

// The parameter substitution of the template instance that declares `s` (a member, payload or
// alternative belongs to its enclosing type), or null outside instances.
inline const std::map<const Symbol*, const types::Type*>* substitution_of(const Symbol* s) {
    for (; s; s = s->parent) {
        if (!s->template_subst.empty()) return &s->template_subst;
    }
    return nullptr;
}

// One parsed file of a module.
struct ModuleFile {
    FileId    id;
    ast::File ast;
};

struct Module {
    std::string               name;   // "counter", "shapes::circle"; the entry file's own name too
    // The project it belongs to (spec 30): "" for the program's own files, "std" for the
    // standard library, else the root directory of the dependency it was loaded from.
    std::string               project;
    std::optional<ModuleFile> header;
    std::optional<ModuleFile> source;

    std::vector<Module*> header_imports;   // imports visible to the header
    std::vector<Module*> source_imports;   // imports written in the source (header imports apply too)
    bool                 header_imports_std = false;
    bool                 source_imports_std = false;

    // Module scope by namespace-qualified name (`Symbol::key`: "std::darray", "main"); functions
    // may overload. A module is not a namespace (spec 30): its items live in the namespaces their
    // `namespace` blocks say.
    std::map<std::string, std::vector<Symbol*>> items;
    std::vector<Symbol*>                        item_order;
    std::set<std::string>                       namespaces;   // every namespace it declares into, with its parents

    // The first item called `key` ("std::string"), or null.
    [[nodiscard]] Symbol* item(const std::string& key) const {
        const std::vector<Symbol*>* found = find_item(key);
        return !found || found->empty() ? nullptr : found->front();
    }

    std::deque<Symbol> storage;

    // The modules whose items this module's header ([1]) and source ([0]) see, computed once by
    // the resolver (imports don't change after loading): name lookup is the resolver's hot path.
    mutable std::vector<Module*> visible_cache[2];
    // Position of each module (by `index_id`) in `visible_cache`, -1 when not visible.
    mutable std::vector<int> visible_position[2];
    mutable int              index_id = -1;   // set by the program-wide item index

    // `items` by hash (rebuilt when items were added): the keys share long prefixes
    // (`std::graphics::opengl::...`), which makes the ordered map's comparisons the hot spot.
    const std::vector<Symbol*>* find_item(const std::string& key) const {
        if (item_index_size_ != items.size()) {
            item_index_.clear();
            item_index_.reserve(items.size());
            for (const auto& [k, v] : items) item_index_.emplace(std::string_view(k), &v);
            item_index_size_ = items.size();
        }
        auto it = item_index_.find(std::string_view(key));
        return it == item_index_.end() ? nullptr : it->second;
    }
    mutable std::unordered_map<std::string_view, const std::vector<Symbol*>*> item_index_;
    mutable std::size_t                                                       item_index_size_ = static_cast<std::size_t>(-1);

    Symbol* make(SymbolKind kind, std::string symbol_name, Span span) {
        Symbol& s = storage.emplace_back();
        s.kind    = kind;
        s.name    = std::move(symbol_name);
        s.span    = span;
        s.module  = this;
        return &s;
    }
};

// `g.set_for_files(target, pattern, name, value)` in build.ed (spec 31): a setting for the
// files matching a pattern. `files` are absolute, normalized paths.
struct FileSetting {
    std::vector<std::string>         files;
    std::string                      std_regex;   // `<std>/graphics/**`: the standard library's files it names (spec 34)
    std::string                      name;
    std::shared_ptr<const ct::Value> value;
};

// Where an annotation can be written (spec 29: `std::meta::target`).
enum class AnnotationTarget { type, field, function, parameter, alternative, other, file };

// One use of an annotation whose struct has checks (spec 29): a copy of the checks, resolved
// with `annotation` and `annotated` bound to this use. The checker runs it.
struct AnnotationCheck {
    AnnotationTarget          target = AnnotationTarget::type;
    ast::CheckDecl*           check  = nullptr;   // the copy (owned by Program::cloned_decls)
    const ast::AnnotationUse* use    = nullptr;
    Module*                   origin = nullptr;   // the annotation's module: the check is its code
    Symbol*                   handle = nullptr;   // `annotated` for fields, functions, parameters, alternatives
    const Symbol*             owner  = nullptr;   // the annotated type, or the type/function the member belongs to
    const Symbol*             member = nullptr;   // the field, method, function or alternative
    std::size_t               param_index = 0;
    bool                      done = false;
};

// `@std::float_mode` bits (spec 34).
inline constexpr unsigned float_mode_contract        = 1u << 0;   // a*b+c may become one fused instruction
inline constexpr unsigned float_mode_reassociate     = 1u << 1;   // (a+b)+c may become a+(b+c): vectorized sums
inline constexpr unsigned float_mode_finite_only     = 1u << 2;   // no NaN or infinity reaches the operations
inline constexpr unsigned float_mode_no_signed_zeros = 1u << 3;   // -0.0 and 0.0 may be treated alike
inline constexpr unsigned float_mode_reciprocal      = 1u << 4;   // a/b may become a*(1/b)

// The use of the annotation `key` ("std::deprecated") in `uses`, or null.
inline const ast::AnnotationUse* find_annotation(const std::vector<ast::AnnotationUse>& uses, std::string_view key) {
    for (const ast::AnnotationUse& u : uses) {
        if (u.symbol && u.symbol->key() == key) return &u;
    }
    return nullptr;
}

struct Program {
    std::map<std::string, std::unique_ptr<Module>> modules;   // by module name

    // Every module's items and namespaces by key, one hash per lookup instead of one per visible
    // module (name lookup is the resolver's hot path). Rebuilt when items were added.
    struct IndexedItems {
        Module*                     module;
        const std::vector<Symbol*>* symbols;
    };
    mutable std::unordered_map<std::string_view, std::vector<IndexedItems>> item_index;
    mutable std::unordered_map<std::string_view, std::vector<Module*>>      namespace_index;
    mutable std::size_t                                                     indexed_count = static_cast<std::size_t>(-1);
    mutable std::size_t                                                     module_ids    = 0;   // index_id < module_ids
    std::size_t                                                             items_generation = 0;   // bumped when a module's items or namespaces change
    void refresh_index() const {
        std::size_t count = modules.size() + (items_generation << 16);
        if (count == indexed_count) return;
        indexed_count = count;
        item_index.clear();
        namespace_index.clear();
        int id = 0;
        for (const auto& [n, m] : modules) {
            if (m->index_id < 0) m->index_id = id;
            id = std::max(id, m->index_id) + 1;
        }
        module_ids = static_cast<std::size_t>(id);
        for (const auto& [n, m] : modules) {
            for (const auto& [k, v] : m->items) item_index[std::string_view(k)].push_back(IndexedItems{m.get(), &v});
            for (const std::string& ns : m->namespaces) namespace_index[std::string_view(ns)].push_back(m.get());
        }
    }
    Module*                                        entry = nullptr;
    // The standard library module (library/std, built into edaxc), loaded on the first
    // `import std`; null if nothing imports it. Also in `modules` as "std".
    Module*                                        std_module = nullptr;
    // The build mode being compiled for (spec 24): decides which mode-gated checks exist (spec 23).
    std::string                                    build_mode = "debug";
    // The class of that mode (`std::build::mode::release`, or a mode of build.ed), once
    // resolved; null without the standard library.
    const Symbol*                                  build_mode_class = nullptr;
    // The values of the mode's settings by method name, as text: "optimization" → "speed",
    // "allow_undefined_behavior" → "false" (spec 24). Empty without the standard library.
    std::map<std::string, std::string>             mode_settings;
    // The build target being compiled (`{target}` of a `std::call_site`, spec 31): the target's
    // name from build.ed, or the main file's name without `.ed`.
    std::string                                    build_target;
    // What build.ed passed to the target being compiled: `std::build::setting<T>(name)`.
    std::map<std::string, std::shared_ptr<const ct::Value>> build_settings;
    // What build.ed passed to some files of that target: `std::build::setting_at<T>(where, name)`.
    std::vector<FileSetting>                       file_settings;
    // The graph that build.ed' `build(g)` fills in; null when compiling ordinary code.
    ct::BuildGraph*                                build_graph = nullptr;
    // `--warn=id=level` (spec 23): the level of each named diagnostic that is not the default.
    std::map<std::string, std::string>             warn_levels;
    std::shared_ptr<types::TypeContext>            types;     // owns every Type the checker creates
    // Instances of template types, in creation order. They are not in any module's items.
    std::vector<Symbol*>                           instances;
    // The AST copy of each instance (same order), which the later phases walk.
    std::vector<ast::Decl*>                        instance_copies;
    // The re-parsed copies of templates that instances are made of (see check.cpp).
    std::deque<ast::File>                          instance_files;
    // A plugin is being built (`g.add_plugin`): its @std::plugin_entry class, the interface the
    // program uses it through, its make() and the hash of the interface (dynamic libraries).
    bool                                           is_plugin = false;
    // Shared Edax libraries (g.add_shared_library(std::build::edax_target{...})). Building one:
    // the code of `library_modules` is exported (every non-generic function, global, vtable and
    // type id). Building a program that links some: `shared_modules` are compiled in them, so the
    // program only declares what it uses of them (generic instances are still made here).
    bool                                           is_shared_library = false;
    std::set<const Module*>                        library_modules;
    std::set<const Module*>                        shared_modules;
    const Symbol*                                  plugin_class = nullptr;       // the @std::plugin_entry class
    const Symbol*                                  plugin_interface = nullptr;   // the interface it is used through
    const Symbol*                                  plugin_make = nullptr;        // its static make()
    std::uint64_t                                  plugin_interface_hash = 0;
    // The plain loops of `@std::simd::must_vectorize` functions, by their number in the loop
    // metadata (the backend fills it; SIMD, "Requiring vectorized code").
    std::vector<std::pair<Span, std::string>>      must_vectorize_loops;   // the loop and its LLVM function
    std::vector<std::string>                       embedded_files;         // `std::embed` (spec 34): inputs of the build
    std::vector<std::pair<Span, std::string>>      no_checks_left;         // `@std::no_checks_left` functions (spec 34) and their LLVM names
    // The tokens of each loaded file, kept for re-parsing templates (see check.cpp).
    std::map<FileId, std::vector<Token>>           file_tokens;
    // Errors in methods of class template instances, kept until the method is used: like
    // C++, an instance's method is only required to compile when it is called
    // (`std::darray<std::unique_ptr<T>>` without `clone()`). By the method's declaration.
    std::map<const ast::Decl*, std::vector<Diagnostic>> deferred_errors;
    // The methods of template instances that the program uses (calls, operators): the
    // ownership pass checks only those, like the type checker reports only their errors.
    std::set<const ast::Decl*> used_instance_methods;
    // Copies of resolved declarations: the instances of method templates (spec 14), the
    // constants that hold annotation values and the copies of annotation checks (spec 29).
    std::deque<std::unique_ptr<ast::Decl>>         cloned_decls;
    // The checks of annotations, one entry per use (spec 29), in the order they were made.
    std::deque<AnnotationCheck>                    annotation_checks;
    // `annotated` inside the `wrap` blocks applied to functions (annotations, section 8): the
    // handle global and the function it describes. The checker gives each its value.
    std::vector<std::pair<Symbol*, const Symbol*>> wrap_handles;
    // The warning ids that `@std::allow` silences in the function being checked (spec 29).
    std::vector<std::string>                       allowed_warnings;
    // Uses that were warned about already (`@std::deprecated`): file, offset, symbol.
    std::set<std::tuple<FileId, std::uint32_t, const Symbol*>> reported_uses;
};

} // namespace edax::sema
