#include "edax/types.hpp"
#include "edax/ast_clone.hpp"

#include "edax/ast_printer.hpp"

namespace edax::types {

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
    untyped_list_    = make(Type{Kind::untyped_list, 0, false, false, nullptr, nullptr, false, "{list}", {}});
    type_value_      = make(Type{Kind::type_value, 0, false, false, nullptr, nullptr, false, "type", {}});
    meta_field_      = make(Type{Kind::meta, 0, false, false, nullptr, nullptr, false, "std::meta::field", {}});
    meta_alternative_ = make(Type{Kind::meta, 0, false, false, nullptr, nullptr, false, "std::meta::alternative", {}});
    meta_function_    = make(Type{Kind::meta, 0, false, false, nullptr, nullptr, false, "std::meta::function", {}});
    meta_parameter_   = make(Type{Kind::meta, 0, false, false, nullptr, nullptr, false, "std::meta::parameter", {}});
    meta_file_        = make(Type{Kind::meta, 0, false, false, nullptr, nullptr, false, "std::meta::file", {}});
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
    builtins_["f16"] = make(Type{Kind::float_, 16, true, false, nullptr, nullptr, false, "f16", {}});   // spec 34
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
    case TypeKind::deduce: return error_;
    case TypeKind::function: {   // `R(A...)` as a type: a C function signature (std::c_library)
        const auto& f = t.as<FunctionType>();
        std::vector<const Type*> params;
        for (const TypePtr& p : f.params) params.push_back(lower(*p, subst));
        return c_function(std::move(params), f.result ? lower(*f.result, subst) : void_t_);
    }
    case TypeKind::reference: {
        const auto& r = t.as<ReferenceType>();
        return reference(lower(*r.inner, subst), r.is_mut);
    }
    case TypeKind::tuple: {
        std::vector<const Type*> elements;
        for (const TypePtr& e : t.as<TupleType>().elements) {
            const Type* et = lower(*e, subst);
            if (et->is(Kind::error)) return error_;
            elements.push_back(et);
        }
        return tuple(std::move(elements));
    }
    case TypeKind::named: break;
    }
    const auto& nt   = t.as<NamedType>();
    const auto& segs = nt.path.segments;
    // `std::function<R(A...)>` and `std::fn_ptr<R(A...)>` (spec 18): built-in callable types.
    if (!nt.symbol && segs.size() == 3 && segs[0].name == "std" && segs[1].name == "c_library" && segs[2].name == "function" &&
        segs[2].template_args.size() == 1 && segs[2].template_args[0].type && segs[2].template_args[0].type->kind == TypeKind::function) {
        return lower(*segs[2].template_args[0].type, subst);   // `std::c_library::function<R(A...)>`
    }
    if (!nt.symbol && segs.size() == 2 && segs[0].name == "std" &&
        (segs[1].name == "function" || segs[1].name == "fn_ptr" || segs[1].name == "inline_function") &&
        !segs[1].template_args.empty() && segs[1].template_args[0].type &&
        segs[1].template_args[0].type->kind == TypeKind::function) {
        const auto& targs = segs[1].template_args;
        const auto& f     = targs[0].type->as<FunctionType>();
        std::vector<const Type*> params;
        for (const TypePtr& p : f.params) params.push_back(lower(*p, subst));
        const Type* result = f.result ? lower(*f.result, subst) : void_t_;
        if (segs[1].name == "fn_ptr") return targs.size() == 1 ? callable(Kind::fn_ptr, std::move(params), result) : error_;
        // `std::inline_function<R(A...), N, I...>`: N bytes of captures inline; then the bounds (spec 34).
        std::size_t next  = 1;
        unsigned    bytes = 0;
        if (segs[1].name == "inline_function") {
            if (targs.size() < 2 || !targs[1].value) return error_;
            const Type* n = value_args_ ? value_args_(nullptr, 1, *targs[1].value, subst) : nullptr;
            if (!n || n->is_unknown() || value_bits(n) <= 0 || value_bits(n) > 4096) return error_;
            bytes = static_cast<unsigned>(value_bits(n));
            next  = 2;
        }
        bool sendable = false, shareable = false;
        for (std::size_t i = next; i < targs.size(); ++i) {
            // `std::sendable`, `std::shareable`: names the compiler provides (threads, spec 27)
            const auto* bound = targs[i].type && targs[i].type->kind == TypeKind::named ? &targs[i].type->as<NamedType>() : nullptr;
            const auto* bs    = bound ? &bound->path.segments : nullptr;
            if (!bs || bs->size() != 2 || (*bs)[0].name != "std" || ((*bs)[1].name != "sendable" && (*bs)[1].name != "shareable")) return error_;
            ((*bs)[1].name == "sendable" ? sendable : shareable) = true;
        }
        return callable(Kind::function, std::move(params), result, bytes, sendable, shareable);
    }
    if (segs.size() == 1 && !nt.symbol) {
        if (const Type* b = builtin_or_vector(segs[0].name)) return b;
    }
    // `C::element` with `C` a template parameter (spec 34): the associated type of the argument.
    if (!nt.symbol && segs.size() == 2 && segs[0].template_args.empty() && subst) {
        for (const auto& [param, arg] : *subst) {
            if (!param || param->kind != sema::SymbolKind::type_param || param->name != segs[0].name || !arg) continue;
            const Type* owner = arg->is(Kind::reference) ? arg->inner : arg;
            if (!owner->is(Kind::user) || !owner->symbol) return error_;
            for (const sema::Symbol* o = owner->symbol; o; o = o->base_class()) {
                auto it = o->members.find(segs[1].name);
                if (it == o->members.end() || it->second.empty() || !it->second.front()->is_type_value) continue;
                const Type* value = type_values_ ? type_values_(it->second.front()) : nullptr;
                return value ? value : error_;
            }
            return error_;
        }
    }
    if (nt.symbol) {
        const sema::Symbol* s = nt.symbol;
        if (s->kind == sema::SymbolKind::type_param) {
            if (subst) {
                if (auto it = subst->find(s); it != subst->end()) return it->second;
                // The same parameter from another resolution of its template (a default argument).
                for (const auto& [k, v] : *subst) {
                    if (k->template_param && k->template_param == s->template_param) return v;
                }
            }
            return type_param(s);
        }
        // `repeat<f64>::element` (spec 34): the associated type of the instance, not of the template.
        if (s->is_type_value && s->kind == sema::SymbolKind::field && s->parent && s->parent->decl && s->parent->decl->is_template &&
            !s->parent->template_of && segs.size() >= 2 && segs[segs.size() - 2].has_template_args) {
            ast::Cloner cloner([](const sema::Symbol&) -> sema::Symbol* { return nullptr; });
            TypePtr     owner = cloner.type(t);
            auto&       on    = owner->as<NamedType>();
            on.path.segments.pop_back();
            on.symbol = const_cast<sema::Symbol*>(s->parent);
            const Type* inst = lower(*owner, subst);
            if (!inst->is(Kind::user)) return inst->is_unknown() ? inst : error_;
            auto it = inst->symbol->members.find(s->name);
            if (it == inst->symbol->members.end() || it->second.empty()) return error_;
            const Type* value = type_values_ ? type_values_(it->second.front()) : nullptr;
            return value ? value : error_;
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
                if (!v->is_unknown() && (!subst || subst->empty())) a.evaluated = v;   // a value that depends on no parameter
                args.push_back(v);
            }
            // Default template arguments: `std::darray<u32>` is `std::darray<u32, std::global_allocator>`.
            // A default may name earlier parameters.
            const auto& params = s->decl->template_params;
            if (args.size() < params.size() && args.size() == seg_args.size()) {
                std::map<const sema::Symbol*, const Type*> inner;
                const auto& syms = s->decl->template_param_symbols;
                for (std::size_t i = args.size(); i < params.size(); ++i) {
                    for (std::size_t j = 0; j < i && j < syms.size(); ++j) inner[syms[j]] = args[j];
                    const TemplateParam& p = params[i];
                    if (p.default_type) {
                        args.push_back(lower(*p.default_type, &inner));
                    } else if (p.default_value && value_args_) {
                        const Type* v = value_args_(s, i, *p.default_value, &inner);
                        if (!v) return error_;
                        args.push_back(v);
                    } else {
                        break;
                    }
                }
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
        // `std::simd::vec<T, N>` and `std::simd::mask<N>` (SIMD).
        if (segs.size() == 3 && segs[1].name == "simd" && (segs[2].name == "vec" || segs[2].name == "mask")) {
            const auto& targs = segs[2].template_args;
            bool        is_vec = segs[2].name == "vec";
            if (targs.size() != (is_vec ? 2u : 1u)) return error_;
            const TemplateArg& count = targs.back();
            const Type*        n     = count.value && value_args_ ? value_args_(nullptr, targs.size() - 1, *count.value, subst) : nullptr;
            if (!n && count.type && count.type->kind == ast::TypeKind::named && value_args_) {
                // a bare name (`w`, `N`) parsed as a type: evaluated as a value
                auto pe = std::make_unique<ast::PathExpr>(count.type->span, ast::Path{});
                for (const auto& sg : count.type->as<NamedType>().path.segments) {
                    ast::PathSegment c;
                    c.name = sg.name;
                    c.span = sg.span;
                    pe->path.segments.push_back(std::move(c));
                }
                pe->symbol = count.type->as<NamedType>().symbol;
                n          = value_args_(nullptr, targs.size() - 1, *pe, subst);
            }
            if (!n || !n->is(Kind::value_arg)) return error_;
            u128 lanes = value_bits(n);
            if (lanes < 2 || lanes > 64 || (lanes & (lanes - 1)) != 0) return error_;
            if (!is_vec) return mask(static_cast<int>(lanes));
            const Type* lane = targs[0].type ? lower(*targs[0].type, subst) : error_;
            if (!lane->is_numeric() || lane->bits > 64 || lane->is_size) return error_;
            return vector(lane, static_cast<int>(lanes));
        }
        std::string spelling = ast::to_string(nt.path);
        if (spelling == "std::meta::field") return meta_field_;
        if (spelling == "std::meta::alternative") return meta_alternative_;
        if (spelling == "std::meta::function") return meta_function_;
        if (spelling == "std::meta::parameter") return meta_parameter_;
        if (spelling == "std::meta::file") return meta_file_;
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
    if (!instantiator_ || !templ->decl) return error_;
    const auto& params = templ->decl->template_params;
    bool        pack   = !params.empty() && params.back().is_pack;   // `usize... N`: any number for the last (spec 34)
    if (pack ? args.size() + 1 < params.size() : args.size() != params.size()) return error_;
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
        text           = "-" + edax::to_string(magnitude);
    } else {
        text = edax::to_string(bits);
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

const Type* TypeContext::callable(Kind kind, std::vector<const Type*> params, const Type* result, unsigned inline_bytes,
                                  bool sendable, bool shareable) {
    params.push_back(result);
    if (kind != Kind::function) inline_bytes = 0, sendable = shareable = false;
    inline_bytes = (inline_bytes + 7) / 8 * 8;   // whole words
    auto key = std::make_tuple(kind, inline_bytes, sendable, shareable, params);
    if (auto it = callables_.find(key); it != callables_.end()) return it->second;
    std::string sig = to_string(result) + "(";
    for (std::size_t i = 0; i + 1 < params.size(); ++i) sig += (i ? ", " : "") + to_string(params[i]);
    sig += ")";
    if (inline_bytes) sig += ", " + std::to_string(inline_bytes);
    if (sendable) sig += ", std::sendable";
    if (shareable) sig += ", std::shareable";
    std::string spelling = kind == Kind::fn_ptr ? "std::fn_ptr<" : inline_bytes ? "std::inline_function<" : "std::function<";
    Type t{kind, static_cast<int>(inline_bytes), sendable, false, nullptr, nullptr, shareable, spelling + sig + ">", {}};
    t.args = std::move(params);
    return callables_[key] = make(std::move(t));
}


const Type* TypeContext::c_function(std::vector<const Type*> params, const Type* result) {
    params.push_back(result);
    auto key = std::make_tuple(Kind::pointer, 0u, false, false, params);   // a key of its own, apart from std::fn_ptr's
    if (auto it = callables_.find(key); it != callables_.end()) return it->second;
    std::string sig = to_string(result) + "(";
    for (std::size_t i = 0; i + 1 < params.size(); ++i) sig += (i ? ", " : "") + to_string(params[i]);
    sig += ")";
    Type t{Kind::fn_ptr, 0, false, true, nullptr, nullptr, false, "std::c_library::function<" + sig + ">", {}};
    t.args = std::move(params);
    return callables_[key] = make(std::move(t));
}

const Type* TypeContext::array(const Type* element, const Type* count) {
    auto key = std::make_pair(element, count);
    if (auto it = arrays_.find(key); it != arrays_.end()) return it->second;
    Type t{Kind::array, 0, false, false, nullptr, element, false,
           "std::raw::array<" + to_string(element) + ", " + edax::to_string(value_bits(count)) + ">", {}};
    t.args = {count};
    return arrays_[key] = make(std::move(t));
}

const Type* TypeContext::tuple(std::vector<const Type*> elements) {
    if (auto it = tuples_.find(elements); it != tuples_.end()) return it->second;
    std::string name = "(";
    for (std::size_t i = 0; i < elements.size(); ++i) name += (i ? ", " : "") + to_string(elements[i]);
    name += ")";
    Type t{Kind::tuple, 0, false, false, nullptr, nullptr, false, name, {}};
    t.args = elements;
    return tuples_[elements] = make(std::move(t));
}

const Type* TypeContext::vector(const Type* lane, int lanes) {
    auto key = std::make_pair(lane, lanes);
    if (auto it = vectors_.find(key); it != vectors_.end()) return it->second;
    Type t{Kind::vector, lanes, false, false, nullptr, lane, false, to_string(lane) + "x" + std::to_string(lanes), {}};
    return vectors_[key] = make(std::move(t));
}

const Type* TypeContext::mask(int lanes) {
    if (auto it = masks_.find(lanes); it != masks_.end()) return it->second;
    Type t{Kind::mask, lanes, false, false, nullptr, nullptr, false, "std::simd::mask<" + std::to_string(lanes) + ">", {}};
    return masks_[lanes] = make(std::move(t));
}

const Type* TypeContext::builtin_or_vector(std::string_view name) {
    if (const Type* b = builtin(name)) return b;
    std::size_t x = name.rfind('x');   // `f32x8`: a number type, 'x', a power of two from 2 to 64
    if (x == std::string_view::npos || x == 0 || x + 1 >= name.size()) return nullptr;
    const Type* lane = builtin(name.substr(0, x));
    if (!lane || !lane->is_numeric() || lane->bits > 64 || lane->is_size) return nullptr;
    std::string_view digits = name.substr(x + 1);
    if (digits.find_first_not_of("0123456789") != std::string_view::npos || digits.size() > 2 || digits[0] == '0') return nullptr;
    int n = std::stoi(std::string(digits));
    if (n < 2 || n > 64 || (n & (n - 1)) != 0) return nullptr;
    return vector(lane, n);
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
    case Kind::tuple: {
        std::string out = "(";
        for (std::size_t i = 0; i < t->args.size(); ++i) out += (i ? ", " : "") + to_string(t->args[i]);
        return out + ")";
    }
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
        // Names are written with their namespace (spec 30): `std::darray`, `geometry::vec2`.
        std::string out = (s->ns.empty() ? "" : s->ns + "::") + name;
        const sema::Symbol* templ = s->template_of ? s->template_of : s;
        if (templ->name == "span_mut" && templ->module && templ->module->name == "std" && s->template_args.size() == 1) {
            return "std::span<mut " + to_string(s->template_args[0]) + ">";   // how it is written (spec 20)
        }
        const std::vector<const Type*>& all = t->kind == Kind::pending ? t->args : s->template_args;
        // The default allocator is left out, as it is written: `std::darray<u32>` (allocators).
        std::size_t shown = all.size();
        while (shown > 1 && all[shown - 1] && all[shown - 1]->kind == Kind::user && all[shown - 1]->symbol->name == "global_allocator" &&
               all[shown - 1]->symbol->ns == "std") {
            --shown;
        }
        const std::vector<const Type*> args(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(shown));
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

// The methods of numbers (spec 34). Floats: rounding, roots, trigonometry, exponentials,
// min/max/clamp, a fused multiply-add and classification; integers: abs, min, max, clamp.
bool is_number_method(const Type* receiver, std::string_view m) {
    if (!receiver) return false;
    if (receiver->is_float()) {
        return m == "sqrt" || m == "abs" || m == "floor" || m == "ceil" || m == "round" || m == "trunc" || m == "min" || m == "max" ||
               m == "clamp" || m == "mul_add" || m == "sin" || m == "cos" || m == "tan" || m == "asin" || m == "acos" ||
               m == "atan" || m == "atan2" || m == "exp" || m == "exp2" || m == "log" || m == "log2" || m == "log10" ||
               m == "pow" || m == "copysign" || m == "is_nan" || m == "is_infinite" || m == "is_finite";
    }
    if (receiver->is_integer()) return m == "abs" || m == "min" || m == "max" || m == "clamp";
    return false;
}

std::size_t number_method_arity(std::string_view m) {
    if (m == "clamp" || m == "mul_add") return 2;
    if (m == "min" || m == "max" || m == "atan2" || m == "pow" || m == "copysign") return 1;
    return 0;
}

bool is_std_query_method(std::string_view method) {
    return method == "is_some" || method == "is_none" || method == "is_ok" || method == "is_error";
}

const Type* strip_ref(const Type* t) { return t && t->kind == Kind::reference ? t->inner : t; }

namespace {
std::uint64_t align_up(std::uint64_t v, std::uint64_t a) { return (v + a - 1) / a * a; }
} // namespace

namespace {
// The literal value of field `name` of `@std::layout{...}` on the type, or null.
const ast::Expr* layout_field(const sema::Symbol* s, std::string_view name) {
    const sema::Symbol* d = s;
    while (d && !d->decl && d->template_of) d = d->template_of;
    if (!d || !d->decl) return nullptr;
    const ast::AnnotationUse* u = sema::find_annotation(d->decl->annotations, "std::layout");
    if (!u && s->template_of && s->template_of->decl) u = sema::find_annotation(s->template_of->decl->annotations, "std::layout");
    if (!u) return nullptr;
    const ast::Expr* value = u->value.get();
    if (!value && u->value_symbol && u->value_symbol->decl && u->value_symbol->decl->kind != ast::DeclKind::function) {
        value = u->value_symbol->decl->as<ast::VariableDecl>().init.get();
    }
    if (!value || value->kind != ast::ExprKind::struct_literal) return nullptr;
    for (const ast::FieldInit& f : value->as<ast::StructLiteralExpr>().fields) {
        if (f.name == name) return f.value.get();
    }
    return nullptr;
}
} // namespace

// `@std::layout{ .packed = true }` (spec 34): no padding at all.
bool is_union(const sema::Symbol* s) {
    for (const sema::Symbol* d = s; d; d = d->template_of) {
        if (d->decl) return d->decl->kind == ast::DeclKind::struct_ && d->decl->as<ast::TypeDecl>().is_union;
    }
    return false;
}

bool is_packed(const sema::Symbol* s) {
    const ast::Expr* v = layout_field(s, "packed");
    return v && v->kind == ast::ExprKind::bool_literal && v->as<ast::LiteralExpr>().bool_value;
}

// `@std::layout{ .align = N }` on the type (spec 34), read from the literal; 0 without one.
std::uint64_t requested_alignment(const sema::Symbol* s) {
    const sema::Symbol* d = s;
    while (d && !d->decl && d->template_of) d = d->template_of;
    if (!d || !d->decl) return 0;
    const ast::AnnotationUse* u = sema::find_annotation(d->decl->annotations, "std::layout");
    if (!u && s->template_of && s->template_of->decl) u = sema::find_annotation(s->template_of->decl->annotations, "std::layout");
    if (!u) return 0;
    // The value is moved into a compile-time constant during name resolution.
    const ast::Expr* value = u->value.get();
    if (!value && u->value_symbol && u->value_symbol->decl && u->value_symbol->decl->kind != ast::DeclKind::function) {
        value = u->value_symbol->decl->as<ast::VariableDecl>().init.get();
    }
    if (!value || value->kind != ast::ExprKind::struct_literal) return 0;
    for (const ast::FieldInit& f : value->as<ast::StructLiteralExpr>().fields) {
        if (f.name == "align" && f.value && f.value->kind == ast::ExprKind::int_literal) {
            return static_cast<std::uint64_t>(f.value->as<ast::LiteralExpr>().int_value);
        }
    }
    return 0;
}

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
        // A pointer or reference to an interface is the object and its interface's table.
        if (t->inner && t->inner->is(Kind::user) && t->inner->symbol && t->inner->symbol->kind == sema::SymbolKind::interface) return {16, 8};
        return {8, 8};
    case Kind::fn_ptr:
    case Kind::nullptr_: return {8, 8};
    case Kind::function: return {t->bits > 0 ? static_cast<std::uint64_t>(t->bits) + 16 : 24, 8};   // { captures or their address, code, drop }
    case Kind::static_str: return {16, 8};
    case Kind::array: {
        Layout e = layout(t->inner);
        return {e.size * array_count(t), e.align};
    }
    case Kind::vector: {
        std::uint64_t lane = static_cast<std::uint64_t>(t->inner->bits) / 8;
        std::uint64_t size = lane * static_cast<std::uint64_t>(t->bits);
        return {size, std::min<std::uint64_t>(size, 64)};
    }
    case Kind::tuple: {
        std::uint64_t size = 0, align = 1;
        for (const Type* e : t->args) {
            Layout l = layout(e);
            size     = align_up(size, l.align) + l.size;
            align    = std::max(align, l.align);
        }
        return {align_up(size, align), align};
    }
    case Kind::external: return t->name == "std::str" ? Layout{16, 8} : Layout{};
    case Kind::user: break;
    default: return {};
    }
    const sema::Symbol* s = t->symbol;
    const std::map<const sema::Symbol*, const Type*>* subst = sema::substitution_of(s);
    if (s->kind == sema::SymbolKind::enum_) {
        if (std::optional<NicheEnum> n = niche_enum(s)) return layout(lower(*n->with->alternative->payload, sema::substitution_of(n->with)));
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
    if (is_union(s)) {   // spec 34: the largest member, the strictest alignment
        std::uint64_t size = 0, most = 1;
        for (const sema::Symbol* f : s->member_order) {
            if (f->kind != sema::SymbolKind::field || f->is_static) continue;
            Layout l = layout(lower(*f->decl->as<ast::VariableDecl>().type, subst));
            if (l.size == 0 && !l.empty) return {};
            size = std::max(size, l.size);
            most = std::max(most, l.align);
        }
        return {align_up(size, most), most};
    }
    bool packed = is_packed(s);
    for (const sema::Symbol* f : s->member_order) {
        if (f->kind != sema::SymbolKind::field || f->is_static) continue;
        Layout l = layout(lower(*f->decl->as<ast::VariableDecl>().type, subst));
        if (l.size == 0 && !l.empty) return {};   // not known
        offset   = (packed ? offset : align_up(offset, l.align)) + l.size;
        align    = packed ? 1 : std::max(align, l.align);
        any      = true;
    }
    if (!any || offset == 0) return {0, 1, true};   // an empty struct takes no space
    if (std::uint64_t want = requested_alignment(s); want > align) align = want;   // @std::layout{ .align } (spec 34)
    return {align_up(offset, align), align};
}

// The tag values of the enum `e` as `bits`-bit patterns, or none when one isn't a literal.
static std::optional<std::set<std::uint64_t>> tag_patterns(const sema::Symbol* e, unsigned bits) {
    std::set<std::uint64_t> out;
    std::uint64_t           mask = bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
    std::uint64_t           next = 0;
    for (const sema::Symbol* a : e->member_order) {
        if (!a->alternative) continue;
        if (const ast::Expr* v = a->alternative->value.get()) {
            bool negative = false;
            if (v->kind == ast::ExprKind::unary && v->as<ast::UnaryExpr>().op == ast::UnaryOp::negate) {
                negative = true;
                v        = v->as<ast::UnaryExpr>().operand.get();
            }
            if (v->kind != ast::ExprKind::int_literal) return std::nullopt;
            std::uint64_t m = static_cast<std::uint64_t>(v->as<ast::LiteralExpr>().int_value);
            next            = negative ? std::uint64_t{0} - m : m;
        }
        out.insert(next & mask);
        next = next + 1;
    }
    return out;
}

std::optional<Niche> TypeContext::niche(const Type* t) {
    if (!t) return std::nullopt;
    if (t->is(Kind::reference)) return Niche{0, 8, 0};   // never null (an interface reference: its object)
    if (t->is(Kind::bool_)) return Niche{0, 1, 2};        // only 0 and 1
    if (t->is(Kind::char_)) return Niche{0, 4, 0x110000};  // a Unicode scalar value is at most 0x10FFFF
    if (!t->is(Kind::user) || !t->symbol || !t->symbol->decl) return std::nullopt;
    const sema::Symbol* s = t->symbol;
    if (s->kind == sema::SymbolKind::enum_) {   // an unused tag value (the tag comes first)
        if (niche_enum(s)) return std::nullopt;  // stored without a tag: its niche is taken
        const auto& e   = s->decl->as<ast::EnumDecl>();
        const Type* tag = lower(*e.tag_type, sema::substitution_of(s));
        Layout      l   = layout(tag);
        if (l.size != 1 && l.size != 2 && l.size != 4 && l.size != 8) return std::nullopt;
        unsigned bits = static_cast<unsigned>(l.size * 8);
        std::optional<std::set<std::uint64_t>> used = tag_patterns(s, bits);
        if (!used || used->empty()) return std::nullopt;
        std::uint64_t mask = bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
        for (std::uint64_t v = (*used->rbegin() + 1) & mask, tries = 0; tries < 64; v = (v + 1) & mask, ++tries) {
            if (!used->contains(v)) return Niche{0, l.size, v};
        }
        return std::nullopt;
    }
    if (is_packed(s) || is_union(s)) return std::nullopt;
    const std::map<const sema::Symbol*, const Type*>* subst = sema::substitution_of(s);
    std::vector<std::uint64_t> offsets = field_offsets(t);
    std::size_t                i       = 0;
    for (const sema::Symbol* f : s->member_order) {
        if (f->kind != sema::SymbolKind::field || f->is_static) continue;
        std::uint64_t at = i < offsets.size() ? offsets[i] : 0;
        ++i;
        const Type* ft = lower(*f->decl->as<ast::VariableDecl>().type, sema::substitution_of(f) ? sema::substitution_of(f) : subst);
        if (sema::find_annotation(f->decl->annotations, "std::never_zero") && (ft->is_integer() || ft->is(Kind::pointer))) {
            Layout l = layout(ft);
            if (l.size == 1 || l.size == 2 || l.size == 4 || l.size == 8) return Niche{at, l.size, 0};
            if (ft->is(Kind::pointer) && l.size == 16) return Niche{at, 8, 0};   // a pointer to an interface: its object
        }
        if (std::optional<Niche> inner = niche(ft)) return Niche{at + inner->offset, inner->size, inner->value};
    }
    return std::nullopt;
}

std::optional<NicheEnum> TypeContext::niche_enum(const sema::Symbol* e) {
    if (!e || e->kind != sema::SymbolKind::enum_ || !e->decl) return std::nullopt;
    if (auto it = niche_cache_.find(e); it != niche_cache_.end()) return it->second;
    niche_cache_[e] = std::nullopt;   // a recursive enum has no niche
    std::vector<const sema::Symbol*> alts;
    for (const sema::Symbol* a : e->member_order) {
        if (a->alternative) alts.push_back(a);
    }
    std::optional<NicheEnum> out;
    if (alts.size() == 2 && !alts[0]->alternative->value && !alts[1]->alternative->value) {
        auto payload = [&](const sema::Symbol* a) -> const Type* {
            if (!a->alternative->payload) return nullptr;
            const Type* p = lower(*a->alternative->payload, sema::substitution_of(a));
            return p && !p->is(Kind::void_) ? p : nullptr;
        };
        const Type* p0 = payload(alts[0]);
        const Type* p1 = payload(alts[1]);
        if ((p0 == nullptr) != (p1 == nullptr)) {
            const sema::Symbol* with    = p0 ? alts[0] : alts[1];
            const sema::Symbol* without = p0 ? alts[1] : alts[0];
            if (!without->alternative->payload) {   // `none`, not `none(void)`
                if (std::optional<Niche> z = niche(p0 ? p0 : p1)) out = NicheEnum{with, without, *z};
            }
        }
    }
    niche_cache_[e] = out;
    return out;
}

Layout TypeContext::closure_layout(const Type* closure) {
    const ast::LambdaExpr* l = closure ? lambda_of(closure) : nullptr;
    if (!l) return {};
    std::uint64_t offset = 0, align = 1;
    for (const ast::Capture& c : l->captures) {
        if (!c.symbol) continue;
        Layout f = c.kind == ast::CaptureKind::ref || c.kind == ast::CaptureKind::mut_ref ? Layout{8, 8} : layout(c.symbol->type);
        offset   = align_up(offset, f.align) + f.size;
        align    = std::max(align, f.align);
    }
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
        if (is_union(s)) {   // every member at the start
            out.push_back(0);
            continue;
        }
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
        return "0 to " + edax::to_string(max);
    }
    u128 limit = u128{1} << (bits - 1);
    return "-" + edax::to_string(limit) + " to " + edax::to_string(limit - 1);
}

} // namespace edax::types
