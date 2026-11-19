#include "rift/types.hpp"

#include "rift/ast_printer.hpp"

namespace rift::types {

bool Type::is_user(sema::SymbolKind k) const { return kind == Kind::user && symbol && symbol->kind == k; }

std::string_view Type::external_base() const {
    if (kind != Kind::external) return {};
    std::string_view n = name;
    return n.substr(0, n.find('<'));
}

TypeContext::TypeContext() {
    error_         = make(Type{Kind::error, 0, false, false, nullptr, nullptr, false, "{unknown}", {}});
    untyped_int_   = make(Type{Kind::untyped_int, 0, false, false, nullptr, nullptr, false, "{integer}", {}});
    untyped_float_ = make(Type{Kind::untyped_float, 0, false, false, nullptr, nullptr, false, "{float}", {}});
    bool_t_        = make(Type{Kind::bool_, 0, false, false, nullptr, nullptr, false, "bool", {}});
    char_t_        = make(Type{Kind::char_, 0, false, false, nullptr, nullptr, false, "char", {}});
    void_t_        = make(Type{Kind::void_, 0, false, false, nullptr, nullptr, false, "void", {}});
    never_         = make(Type{Kind::never, 0, false, false, nullptr, nullptr, false, "never", {}});
    nullptr_t_     = make(Type{Kind::nullptr_, 0, false, false, nullptr, nullptr, false, "nullptr", {}});
    static_str_    = make(Type{Kind::static_str, 0, false, false, nullptr, nullptr, false, "std::static_str", {}});
    untyped_default_ = make(Type{Kind::untyped_default, 0, false, false, nullptr, nullptr, false, "{default}", {}});
    type_value_      = make(Type{Kind::type_value, 0, false, false, nullptr, nullptr, false, "type", {}});
    meta_field_      = make(Type{Kind::meta, 0, false, false, nullptr, nullptr, false, "std::meta::field", {}});
    meta_alternative_ = make(Type{Kind::meta, 0, false, false, nullptr, nullptr, false, "std::meta::alternative", {}});
    meta_function_    = make(Type{Kind::meta, 0, false, false, nullptr, nullptr, false, "std::meta::function", {}});
    meta_parameter_   = make(Type{Kind::meta, 0, false, false, nullptr, nullptr, false, "std::meta::parameter", {}});
    builtins_["type"] = type_value_;
    builtins_["bool"]  = bool_t_;
    builtins_["char"]  = char_t_;
    builtins_["void"]  = void_t_;
    builtins_["never"] = never_;
    struct IntSpec {
        const char* name;
        int         bits;
        bool        is_signed;
        bool        is_size;
    };
    // isize/usize are taken as 64-bit for range checks until targets exist.
    for (IntSpec s : {IntSpec{"i8", 8, true, false}, IntSpec{"i16", 16, true, false}, IntSpec{"i32", 32, true, false},
                      IntSpec{"i64", 64, true, false}, IntSpec{"i128", 128, true, false}, IntSpec{"isize", 64, true, true},
                      IntSpec{"u8", 8, false, false}, IntSpec{"u16", 16, false, false}, IntSpec{"u32", 32, false, false},
                      IntSpec{"u64", 64, false, false}, IntSpec{"u128", 128, false, false},
                      IntSpec{"usize", 64, false, true}}) {
        builtins_[s.name] = make(Type{Kind::int_, s.bits, s.is_signed, s.is_size, nullptr, nullptr, false, s.name, {}});
    }
    builtins_["f32"] = make(Type{Kind::float_, 32, true, false, nullptr, nullptr, false, "f32", {}});
    builtins_["f64"] = make(Type{Kind::float_, 64, true, false, nullptr, nullptr, false, "f64", {}});
}

const Type* TypeContext::make(Type t) {
    storage_.push_back(std::move(t));
    return &storage_.back();
}

const Type* TypeContext::builtin(std::string_view name) const {
    auto it = builtins_.find(std::string(name));
    return it == builtins_.end() ? nullptr : it->second;
}

const Type* TypeContext::reference(const Type* inner, bool is_mut) {
    if (inner->kind == Kind::reference) inner = inner->inner;   // no references to references
    auto key = std::make_tuple(static_cast<int>(Kind::reference), inner, is_mut);
    if (auto it = composites_.find(key); it != composites_.end()) return it->second;
    return composites_[key] = make(Type{Kind::reference, 0, false, false, nullptr, inner, is_mut, "", {}});
}

const Type* TypeContext::pointer(const Type* inner, bool is_mut) {
    auto key = std::make_tuple(static_cast<int>(Kind::pointer), inner, is_mut);
    if (auto it = composites_.find(key); it != composites_.end()) return it->second;
    return composites_[key] = make(Type{Kind::pointer, 0, false, false, nullptr, inner, is_mut, "", {}});
}

const Type* TypeContext::range(const Type* element) {
    auto key = std::make_tuple(static_cast<int>(Kind::range), element, false);
    if (auto it = composites_.find(key); it != composites_.end()) return it->second;
    return composites_[key] = make(Type{Kind::range, 0, false, false, nullptr, element, false, "", {}});
}

const Type* TypeContext::user(const sema::Symbol* symbol) {
    if (auto it = users_.find(symbol); it != users_.end()) return it->second;
    return users_[symbol] = make(Type{Kind::user, 0, false, false, symbol, nullptr, false, symbol->name, {}});
}

const Type* TypeContext::type_param(const sema::Symbol* symbol) {
    if (auto it = params_.find(symbol); it != params_.end()) return it->second;
    return params_[symbol] = make(Type{Kind::type_param, 0, false, false, symbol, nullptr, false, symbol->name, {}});
}

const Type* TypeContext::external(const std::string& spelling) {
    if (spelling == "std::static_str") return static_str_;
    if (auto it = externals_.find(spelling); it != externals_.end()) return it->second;
    return externals_[spelling] = make(Type{Kind::external, 0, false, false, nullptr, nullptr, false, spelling, {}});
}

const Type* TypeContext::lower(const ast::TypeExpr& t, const std::map<const sema::Symbol*, const Type*>* subst) {
    using namespace ast;
    switch (t.kind) {
    case TypeKind::auto_:
    case TypeKind::deduce:
    case TypeKind::function: return error_;
    case TypeKind::reference: {
        const auto& r = t.as<ReferenceType>();
        return reference(lower(*r.inner, subst), r.is_mut);
    }
    case TypeKind::named: break;
    }
    const auto& nt   = t.as<NamedType>();
    const auto& segs = nt.path.segments;
    // `std::function<R(A...)>` and `std::fn_ptr<R(A...)>` (spec 18): built-in callable types.
    if (!nt.symbol && segs.size() == 2 && segs[0].name == "std" && (segs[1].name == "function" || segs[1].name == "fn_ptr") &&
        segs[1].template_args.size() == 1 && segs[1].template_args[0].type &&
        segs[1].template_args[0].type->kind == TypeKind::function) {
        const auto& f = segs[1].template_args[0].type->as<FunctionType>();
        std::vector<const Type*> params;
        for (const TypePtr& p : f.params) params.push_back(lower(*p, subst));
        const Type* result = f.result ? lower(*f.result, subst) : void_t_;
        return callable(segs[1].name == "function" ? Kind::function : Kind::fn_ptr, std::move(params), result);
    }
    if (segs.size() == 1 && !nt.symbol) {
        if (const Type* b = builtin(segs[0].name)) return b;
    }
    if (nt.symbol) {
        const sema::Symbol* s = nt.symbol;
        if (s->kind == sema::SymbolKind::type_param) {
            if (subst) {
                if (auto it = subst->find(s); it != subst->end()) return it->second;
            }
            return type_param(s);
        }
        if (s->is_type_value) {   // a compile_time `type` value used as a type (spec 26)
            const Type* value = type_values_ ? type_values_(s) : nullptr;
            return value ? value : error_;
        }
        if (s->is_type() && s->decl && !s->decl->is_template) return user(s);
        if (s->is_type() && s->decl && s->decl->is_template) {
            // `box<u32>`: an instance of the template for these arguments. Arguments that are
            // still unknown (inside a template that is not instantiated) make it unknown too.
            std::vector<const Type*> args;
            const auto& seg_args = segs.back().template_args;
            // `std::span<mut T>` (spec 20, 27): the library's `span_mut<T>`, a view with mutable elements.
            if (s->name == "span" && s->module && s->module->name == "std" && seg_args.size() == 1 && seg_args[0].is_mut &&
                seg_args[0].type && seg_args[0].type->kind != ast::TypeKind::reference) {
                if (const sema::Symbol* sm = s->module->item("std::span_mut")) s = sm;
            }
            for (std::size_t i = 0; i < seg_args.size(); ++i) {
                const TemplateArg& a = seg_args[i];
                if (a.type) {
                    const Type* arg = lower(*a.type, subst);
                    // `std::option<mut T&>`: the `mut` belongs to the reference argument
                    if (a.is_mut && arg->is(Kind::reference)) arg = reference(arg->inner, true);
                    args.push_back(arg);
                    continue;
                }
                // A value argument (spec 14): evaluated by the checker once per written argument.
                if (a.evaluated) {
                    args.push_back(a.evaluated);
                    continue;
                }
                const Type* v = value_args_ && a.value ? value_args_(s, i, *a.value, subst) : nullptr;
                if (!v) return error_;
                if (!v->is_unknown()) a.evaluated = v;
                args.push_back(v);
            }
            std::optional<Span> saved = site_;
            site_                     = nt.span;
            const Type* inst          = instance(s, args);
            site_                     = saved;
            return inst;
        }
        return error_;
    }
    if (!segs.empty() && segs[0].name == "std") {
        // `std::raw::array<T, N>`: N values inline. N may depend on the template being instantiated.
        if (segs.size() == 3 && segs[1].name == "raw" && segs[2].name == "array" && segs[2].template_args.size() == 2 &&
            segs[2].template_args[0].type && segs[2].template_args[1].value) {
            const Type* element = lower(*segs[2].template_args[0].type, subst);
            const Type* count   = value_args_ ? value_args_(nullptr, 1, *segs[2].template_args[1].value, subst) : nullptr;
            if (!count || count->is_unknown() || element->is_unknown()) return error_;
            return array(element, count);
        }
        if (segs.size() == 2 && segs[1].name == "raw_ptr" && segs[1].template_args.size() == 1 &&
            segs[1].template_args[0].type) {
            const TemplateArg& arg = segs[1].template_args[0];
            return pointer(lower(*arg.type, subst), arg.is_mut);
        }
        std::string spelling = ast::to_string(nt.path);
        if (spelling == "std::meta::field") return meta_field_;
        if (spelling == "std::meta::alternative") return meta_alternative_;
        if (spelling == "std::meta::function") return meta_function_;
        if (spelling == "std::meta::parameter") return meta_parameter_;
        const Type*              ext = external(spelling);
        std::vector<const Type*> args;
        for (const TemplateArg& a : segs.back().template_args) {
            args.push_back(a.type ? lower(*a.type, subst) : error_);
        }
        external_args_[ext] = args;
        return ext;
    }
    return error_;
}

const Type* TypeContext::instance(const sema::Symbol* templ, const std::vector<const Type*>& args) {
    auto key = std::make_pair(templ, args);
    if (auto it = instances_.find(key); it != instances_.end()) return it->second;   // made before
    if (!instantiator_ || !templ->decl || args.size() != templ->decl->template_params.size()) return error_;
    for (const Type* a : args) {
        if (a->is_unknown()) return error_;
    }
    const Type* t = instantiator_(templ, args);
    instances_[key] = t;
    return t;
}

const Type* TypeContext::value_arg(u128 bits, const Type* of) {
    auto key = std::make_pair(bits, of);
    if (auto it = value_args_made_.find(key); it != value_args_made_.end()) return it->second;
    std::string text;
    if (of->kind == Kind::bool_) {
        text = bits ? "true" : "false";
    } else if (of->is_signed && of->bits < 128 && (bits >> (of->bits - 1)) & 1) {
        u128 magnitude = (~bits + 1) & ((u128{1} << of->bits) - 1);
        text           = "-" + rift::to_string(magnitude);
    } else {
        text = rift::to_string(bits);
    }
    const Type* t = make(Type{Kind::value_arg, 0, false, false, nullptr, of, false, text, {}});
    value_of_[t]  = bits;
    return value_args_made_[key] = t;
}

const Type* TypeContext::closure(const ast::LambdaExpr* l) {
    if (auto it = closures_.find(l); it != closures_.end()) return it->second;
    const Type* t = make(Type{Kind::closure, 0, false, false, nullptr, nullptr, false, "lambda", {}});
    lambdas_[t]   = l;
    return closures_[l] = t;
}

void TypeContext::set_closure_signature(const Type* closure, std::vector<const Type*> params, const Type* result) {
    params.push_back(result);
    const_cast<Type*>(closure)->args = std::move(params);   // the type is unique to its lambda
}

const Type* TypeContext::callable(Kind kind, std::vector<const Type*> params, const Type* result) {
    params.push_back(result);
    auto key = std::make_pair(kind, params);
    if (auto it = callables_.find(key); it != callables_.end()) return it->second;
    std::string sig = to_string(result) + "(";
    for (std::size_t i = 0; i + 1 < params.size(); ++i) sig += (i ? ", " : "") + to_string(params[i]);
    sig += ")";
    Type t{kind, 0, false, false, nullptr, nullptr, false,
           (kind == Kind::function ? "std::function<" : "std::fn_ptr<") + sig + ">", {}};
    t.args = std::move(params);
    return callables_[key] = make(std::move(t));
}

const Type* TypeContext::array(const Type* element, const Type* count) {
    auto key = std::make_pair(element, count);
    if (auto it = arrays_.find(key); it != arrays_.end()) return it->second;
    Type t{Kind::array, 0, false, false, nullptr, element, false,
           "std::raw::array<" + to_string(element) + ", " + rift::to_string(value_bits(count)) + ">", {}};
    t.args = {count};
    return arrays_[key] = make(std::move(t));
}

std::uint64_t TypeContext::array_count(const Type* array) const {
    return array && array->kind == Kind::array && !array->args.empty() ? static_cast<std::uint64_t>(value_bits(array->args[0])) : 0;
}

const ast::LambdaExpr* TypeContext::lambda_of(const Type* closure) const {
    auto it = lambdas_.find(closure);
    return it == lambdas_.end() ? nullptr : it->second;
}

const Type* TypeContext::value_arg_of(const std::string& text, const Type* of, std::shared_ptr<const void> payload) {
    auto key = std::make_pair(text, of);
    if (auto it = rich_values_made_.find(key); it != rich_values_made_.end()) return it->second;
    const Type* t     = make(Type{Kind::value_arg, 0, false, false, nullptr, of, false, text, {}});
    rich_value_of_[t] = std::move(payload);
    return rich_values_made_[key] = t;
}

std::shared_ptr<const void> TypeContext::value_payload(const Type* t) const {
    auto it = rich_value_of_.find(t);
    return it == rich_value_of_.end() ? nullptr : it->second;
}

u128 TypeContext::value_bits(const Type* t) const {
    auto it = value_of_.find(t);
    return it == value_of_.end() ? 0 : it->second;
}

const Type* TypeContext::pending(const sema::Symbol* templ, const std::vector<const Type*>& args) {
    auto key = std::make_pair(templ, args);
    if (auto it = pendings_.find(key); it != pendings_.end()) return it->second;
    Type t{Kind::pending, 0, false, false, templ, nullptr, false, templ->name, {}};
    t.args = args;
    return pendings_[key] = make(std::move(t));
}

const std::vector<const Type*>* TypeContext::external_args(const Type* t) const {
    auto it = external_args_.find(t);
    return it == external_args_.end() ? nullptr : &it->second;
}

std::string to_string(const Type* t) { return type_name(t, false); }

std::string qualified_name(const Type* t) { return type_name(t, true); }

std::string type_name(const Type* t, bool qualified) {
    auto to_string = [qualified](const Type* x) { return type_name(x, qualified); };
    if (!t) return "?";
    switch (t->kind) {
    case Kind::reference: return (t->is_mut ? "mut " : "") + to_string(t->inner) + "&";
    case Kind::pointer: return std::string("std::raw_ptr<") + (t->is_mut ? "mut " : "") + to_string(t->inner) + ">";
    case Kind::range: return "range<" + to_string(t->inner) + ">";
    case Kind::closure: {
        std::string out = "lambda(";
        for (std::size_t i = 0; i + 1 < t->args.size(); ++i) out += (i ? ", " : "") + to_string(t->args[i]);
        out += ")";
        if (!t->args.empty() && t->args.back() && !t->args.back()->is(Kind::void_)) out += " -> " + to_string(t->args.back());
        return out;
    }
    case Kind::user:
    case Kind::pending: {
        // Standard-library types are written `std::option<u32>`; instances show their arguments.
        const sema::Symbol* s = t->symbol;
        std::string name = s->name;
        if (s->ns == "std" && sema::is_std_nested_type(name)) {
            name = "thread_pool::" + name.substr(std::string_view("thread_pool_").size());   // written as a member type
        }
        // Names are written with their namespace (spec 30): `std::darray`, `geometry::vec2`.
        std::string out = (s->ns.empty() ? "" : s->ns + "::") + name;
        const sema::Symbol* templ = s->template_of ? s->template_of : s;
        if (templ->name == "span_mut" && templ->module && templ->module->name == "std" && s->template_args.size() == 1) {
            return "std::span<mut " + to_string(s->template_args[0]) + ">";   // how it is written (spec 20)
        }
        const std::vector<const Type*>& args = t->kind == Kind::pending ? t->args : s->template_args;
        if (!args.empty()) {
            out += '<';
            for (std::size_t i = 0; i < args.size(); ++i) out += (i ? ", " : "") + (args[i] ? to_string(args[i]) : "?");
            out += '>';
        }
        return out;
    }
    default: return t->name;
    }
}

bool is_std_instance(const Type* t, std::string_view name) {
    t = strip_ref(t);
    if (!t || t->kind != Kind::user || !t->symbol->template_of) return false;
    const sema::Symbol* templ = t->symbol->template_of;
    return templ->name == name && templ->module && templ->module->name == "std";
}

bool is_std_builtin_method(const Type* receiver, std::string_view method) {
    if (is_std_instance(receiver, "option")) {
        return method == "value_or" || method == "expect" || method == "or_error" || method == "is_some" || method == "is_none";
    }
    if (is_std_instance(receiver, "result")) {
        return method == "value_or" || method == "expect" || method == "or_error" || method == "map_error" || method == "is_ok" ||
               method == "is_error";
    }
    return false;
}

bool is_std_query_method(std::string_view method) {
    return method == "is_some" || method == "is_none" || method == "is_ok" || method == "is_error";
}

const Type* strip_ref(const Type* t) { return t && t->kind == Kind::reference ? t->inner : t; }

namespace {
std::uint64_t align_up(std::uint64_t v, std::uint64_t a) { return (v + a - 1) / a * a; }
} // namespace

Layout TypeContext::layout(const Type* t) {
    if (!t) return {};
    switch (t->kind) {
    case Kind::int_: {
        std::uint64_t bytes = static_cast<std::uint64_t>(t->bits) / 8;
        return {bytes, bytes};   // i128 is 16-byte aligned on x86-64 with LLVM 18
    }
    case Kind::float_: return {static_cast<std::uint64_t>(t->bits) / 8, static_cast<std::uint64_t>(t->bits) / 8};
    case Kind::bool_: return {1, 1};
    case Kind::char_: return {4, 4};
    case Kind::reference:
    case Kind::pointer:
    case Kind::nullptr_: return {8, 8};
    case Kind::static_str: return {16, 8};
    case Kind::array: {
        Layout e = layout(t->inner);
        return {e.size * array_count(t), e.align};
    }
    case Kind::external: return t->name == "std::str" ? Layout{16, 8} : Layout{};
    case Kind::user: break;
    default: return {};
    }
    const sema::Symbol* s = t->symbol;
    const std::map<const sema::Symbol*, const Type*>* subst = sema::substitution_of(s);
    if (s->kind == sema::SymbolKind::enum_) {
        const auto& e   = s->decl->as<ast::EnumDecl>();
        Layout      tag = layout(lower(*e.tag_type, subst));
        std::uint64_t size = 0, align = 1;
        for (const sema::Symbol* a : s->member_order) {
            if (!a->alternative || !a->alternative->payload) continue;
            Layout p = layout(lower(*a->alternative->payload, subst));
            size     = std::max(size, p.size);
            align    = std::max(align, p.align);
        }
        if (size == 0) return tag;
        std::uint64_t storage = align_up(size, align);
        std::uint64_t at      = align_up(tag.size, align);
        std::uint64_t whole   = std::max(tag.align, align);
        return {align_up(at + storage, whole), whole};
    }
    std::uint64_t offset = 0, align = 1;
    bool          any    = false;
    if (const sema::Symbol* base = s->base_class()) {   // the base object comes first (spec 07)
        Layout b = layout(user(base));
        offset = b.size;
        align  = b.align;
        any    = true;
    } else if (s->has_vtable_pointer()) {
        offset = align = 8;
        any    = true;
    }
    for (const sema::Symbol* f : s->member_order) {
        if (f->kind != sema::SymbolKind::field || f->is_static) continue;
        Layout l = layout(lower(*f->decl->as<ast::VariableDecl>().type, subst));
        offset   = align_up(offset, l.align) + l.size;
        align    = std::max(align, l.align);
        any      = true;
    }
    if (!any) return {1, 1};   // an empty struct holds one byte
    return {align_up(offset, align), align};
}

std::vector<std::uint64_t> TypeContext::field_offsets(const Type* t) {
    std::vector<std::uint64_t> out;
    if (!t || t->kind != Kind::user) return out;
    const sema::Symbol* s = t->symbol;
    const std::map<const sema::Symbol*, const Type*>* subst = sema::substitution_of(s);
    std::uint64_t offset = 0;
    if (const sema::Symbol* base = s->base_class()) offset = layout(user(base)).size;
    else if (s->has_vtable_pointer()) offset = 8;
    for (const sema::Symbol* f : s->member_order) {
        if (f->kind != sema::SymbolKind::field || f->is_static) continue;
        Layout l = layout(lower(*f->decl->as<ast::VariableDecl>().type, subst));
        offset   = align_up(offset, l.align);
        out.push_back(offset);
        offset += l.size;
    }
    return out;
}

bool literal_fits(u128 magnitude, bool negative, const Type* t) {
    if (!t || t->kind != Kind::int_) return true;
    int bits = t->bits;
    if (!t->is_signed) {
        if (negative) return magnitude == 0;
        return bits == 128 || magnitude <= ((u128{1} << bits) - 1);
    }
    u128 limit = u128{1} << (bits - 1);   // |min|
    return negative ? magnitude <= limit : magnitude <= limit - 1;
}

bool is_mutating_library_method(const Type* receiver, std::string_view method) {
    if (!receiver || receiver->kind != Kind::external) return false;
    std::string_view base = receiver->external_base();
    static const char* containers[] = {"std::darray", "std::string", "std::hash_map", "std::tree_map", "std::bitset"};
    static const char* mutating[]   = {"push", "pop", "insert", "remove", "clear", "retain", "sort", "sort_by_key",
                                       "resize", "reserve", "append", "extend", "truncate", "erase", "set"};
    bool is_container = false;
    for (const char* c : containers) is_container = is_container || base == c;
    if (!is_container) return false;
    for (const char* m : mutating) {
        if (method == m) return true;
    }
    return false;
}

std::string integer_range(const Type* t) {
    if (!t || t->kind != Kind::int_) return "";
    int bits = t->bits;
    if (!t->is_signed) {
        u128 max = bits == 128 ? ~u128{0} : (u128{1} << bits) - 1;
        return "0 to " + rift::to_string(max);
    }
    u128 limit = u128{1} << (bits - 1);
    return "-" + rift::to_string(limit) + " to " + rift::to_string(limit - 1);
}

} // namespace rift::types
