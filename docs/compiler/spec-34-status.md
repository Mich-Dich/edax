# Spec 34 (engine performance and language extensions): implementation status

The design is in the language pages (see the index in `docs/design/README.md`); this file tracks
what `edaxc` does today. Legend: **done** (implemented and tested), **partial**, **designed**
(in the design pages, not implemented yet).

## Done
| Item | Design page | Where | Tests |
|---|---|---|---|
| `noalias` on `mut T&` / `mut this`; `noalias readonly` on `T&` of frozen types (classes too, unless `@std::interior_mutable`: atomic, mutex, rw_mutex, condition) | performance §1 | `llvm_backend.cpp` `add_alias_attributes`, `is_frozen` | whole suite; release run of every `tests/action/run` program |
| `release` stays at `-O2`: `-O3` was measured 3x slower on `string_build` (it stops inlining `std::string::push`) | build-modes | `build_executable` | benchmarks |
| `std::build::mode::development` (debug + `optimization::basic`, `-O1`) | build-modes | `library/std/build/mode.ed*`, driver, checker | `action/build/run_development` |
| `@std::optimize{ .level }` and `g.optimize_files(t, pattern, level)`; `optnone` honored (standard instrumentations) | performance §6, build-system | backend `edax-optimize` attribute, `compile_time.cpp` | `action/build/run_debug_optimized_files` |
| `@std::float_mode{ ... }` fast-math flags per function; `float_determinism()` mode setting | performance §7 | `gen_function`, `read_codegen_annotations` | `action/build/float_mode_*` |
| Number methods (`sqrt`, `clamp`, `mul_add`, `sin`, ... ; integer `abs/min/max/clamp`), also at compile time; unknown methods on numbers are errors | types | checker, backend `number_method`, evaluator | `feature/34-performance/number_methods`, `action/run/number_methods` |
| `std::bit_cast<T>(x)` (safe, plain bits) | layout §4, casts | checker `infer_bit_cast`, backend, evaluator | `feature/34-performance/bit_cast` |
| `std::meta::type_id<T>()` | metaprogramming | evaluator | `action/run/assume_unreachable` |
| `std::assume`, `std::unreachable`, `std::raw::assume`, `std::raw::unreachable` | performance §5 | `std.ed`, raw intrinsics | `action/run/assume_unreachable` |
| Calls of functions returning `never` end the block (was a crash) | - | `gen_expr` | same |
| `@std::layout{ .packed = true }`: no padding, accesses aligned to 1, fields can't be borrowed (arguments, receivers, references) | layout §3 | `types.cpp is_packed`, `unalign_packed_accesses`, `reject_packed_reference` | `feature/34-performance/packed`, `action/run/packed` |
| `template<std::static_str S>` text arguments (`name<"player">`) | templates | already worked through struct value parameters | `action/run/static_str_args` |
| `@std::layout{ .align, .no_implicit_padding }`, `std::padding<N>` | layout §2 | `types.cpp requested_alignment`, `user_type`, `check_layout` | `feature/34-performance/layout`, `action/run/layout` |
| `if (v is p)`, `while (v is p)` with `&& guard` | control-flow | parser (desugared to `match`) | `feature/34-performance/is_patterns*`, `action/run/is_patterns` |
| `mut` lambda captures (`[mut count = 0u32]`); `retain` and `std::thread::spawn` take `mut F` | lambdas | parser, resolver, `infer_closure_call` | `feature/18-lambdas/mut_captures`, `action/run/mut_captures` |
| `std::embed` / `std::embed_text` (files recorded as build inputs) | metaprogramming | `infer_embed`, `materialize`, driver stamp | `feature/34-performance/embed`, `action/run/embed` |
| `distinct type meters = f32;` with `cast` both ways (also at compile time); `@std::derive::arithmetic` (`d + d`, `d * k`, `k * d`, `d / k`, `d / d`, `-d`, comparisons, compound assignment) | templates | parser `parse_distinct` (a struct with a hidden `$value` field), `infer_cast`, `infer_distinct_binary` (rewrites operands to casts), backend/evaluator wrap the result | `feature/34-performance/distinct_*`, `action/run/distinct_types` |
| `f16`: arithmetic, casts, literals (`0.5f16`), compile time, formatting, number methods, `bit_cast` | types | keyword, `types.cpp`, backend `half`, evaluator `round_float` | `feature/34-performance/f16`, `action/run/f16` |
| `implement I for T { ... }` in the module of `I` or `T`: static and dynamic use, missing methods, orphan rule | templates | parser `parse_implement`, resolver `apply_implements` (methods resolved where the block is written) | `action/run/implement`, `action/check/implement_errors` |
| `sanitize()` mode setting: `address`, `thread` (Edax functions instrumented, C/C++ targets compiled and the program linked with `-fsanitize=`) | build-modes | `build_executable` (sanitizer passes, also at -O0), driver | `action/build/run_asan`, `action/build/run_tsan` |
| Niches: `std::option` of references, `std::unique_ptr`, `std::shared_ptr`, `std::nonzero<T>`, `bool`, `char`, enums (an unused tag) and structs holding one has no tag; `@std::never_zero` fields | layout §6 | `types.cpp niche`/`niche_enum` (also `size_of`), backend `load_tag`/`store_tag`/`payload_address` (every tag access) | `feature/34-performance/niches`, `action/run/niches*` |
| `std::inline_function<R(A...), N, I...>` (captures inline, size checked) and `std::function<R(A...), I...>` with `std::sendable` / `std::shareable` bounds | lambdas | `types.cpp callable`/`closure_layout`, checker `check_callable_fits`, backend `callable_from_lambda`/`inline_function_drop` | `feature/34-performance/inline_function`, `action/run/inline_function` |
| Value packs `template<usize... N>` in function and type templates: `for compile_time`, `N.len()`, `f<N...>` | templates | parser, `expand_pack`/`expand_type_pack`, `Cloner` (template-argument spread), arity in resolver/types | `action/run/value_packs` |
| Associated types: `type element;` in interfaces, `type element = T;` in structs/classes (templates too), `C::element` in templates, `owner<args>::element`; template instances implement their template's interfaces | templates | parser `parse_associated_type`, resolver (`member_scope_`, conformance), checker `check_member_type`, `types.cpp` dependent lookup | `feature/34-performance/associated_types*`, `action/run/associated_types` |
| Operators as interfaces: `std::addable<R>`, `std::subtractable<R>`, `std::multipliable<R>`, `std::dividable<R>`, `std::negatable` | templates | checker `satisfies` (`constraint_args_` for `V : std::addable<V>`) | `feature/34-performance/operator_interfaces`, `action/run/operator_interfaces` |
| `union`: largest member, strictest alignment; one-member literals; safe writes, `unsafe` reads; plain-data members; byte equality | layout §5 | parser (`union` before a name), `types.cpp is_union`, backend `field_address`, checker `union_store_`, ownership | `feature/34-performance/unions*`, `action/run/unions` |
| `g.target_cpu(t, level)`: one target checked and compiled for a CPU level | build-system | evaluator, driver (`build_edax_target`) | `action/build/run_target_cpu*` |
| Tuples of references into distinct fields: `(mut A&, B&)` return types, `return (mut this.a, this.b);`, destructuring into reference bindings | ownership "Returning several references" | parser (tuple types, `mut` in returned tuples), ownership `check_returned_reference`, backend tuple literals | `feature/12-ownership-references/tuple_references`, `action/run/tuple_references` |
| Disjoint field borrows in calls (`f(mut w.a, w.b)`), through reference parameters and local references, and in loops (`for (f32& v : w.b) { w.a.push(v); }`) | ownership "Places" | `ownership.cpp check_aliasing`, `LoopContext::path`, `reaches_loop` | `feature/12-ownership-references/disjoint_fields`, `.../loop_fields` |
| Alias scopes for owned memory: accesses through a `noalias` parameter (and methods returning a reference into `this`, functions marked `edax-borrows-this`) don't alias the other parameters' | performance §1 | `add_alias_scopes`, `borrows_from_this` | `bench/particles` |
| Conditional stores to an address already stored in the iteration become selects (pass at the vectorizer's start) | performance §2b | `ConditionalStoreSelect`, `register_edax_passes` | `action/build/vectorize_owned_aliasing`, `bench/particles` |
| Small `T&` in registers: LLVM argument promotion at O2 (up to 4 scalars) | performance §2 | `register_edax_passes` | whole suite |
| `bench/particles` (SoA kernel vs C++) | performance §9 | `bench/` | 0.55x of g++ -O2 (was 1.37x: g++ keeps reloading, Edax now vectorizes); 0.35x at x86_64_v3 |
| `edaxc explain-checks <file>`: every check of the file's functions, kept or removed at -O2 (functions on their own, for any arguments) | performance §5 | `explain_checks`, driver `run_explain_checks` | `action/cli/explain_checks` |
| Whole-program devirtualization: one implementation of a virtual method / one implementing type of an interface → direct call (off with plugins) | performance §8 | `single_implementation` | `action/run/devirtualize`, `action/insights/devirtualize` |
| `sort()` of integers, `sort_by_key` with integer keys: LSD radix sort from 256 elements (buffer of n) | stdlib | `std.ed radix_sort` | `action/run/radix_sort`, `bench/sort*` (0.19x, 0.16x) |
| Compile speed: the default build of edaxc is optimized (`RelWithDebInfo` without `CMAKE_BUILD_TYPE`: 4-5x faster than the unoptimized default); module visibility is computed once, module items are found by hash. `opengl_versions.ed` checks in 0.6 s (was 3.8 s unoptimized, 0.85 s optimized) | - (compiler internal) | `CMakeLists.txt`, `Module::find_item`, `visible_modules` | whole suite |
| Doc comments: `//` lines directly above a declaration (the 4 old doc-comment test failures) | lexical | `Lexer::skip_trivia` | `unit`, `action/lex/*`, `action/parse/*` |
| `@std::no_checks_left` (E6004 names each check left) | performance §5 | `check_no_checks_left` | `action/build/no_checks_left`, `bench/particles` |
| Entry checks take the caller's location as a hidden argument (no thread-local stores around `operator[]`) | performance §5 | `site_variant_of` | whole suite |

Already present before spec 34 (the roadmap listed them as missing): the whole program is one LLVM
module (cross-module inlining needs no interface bodies), `bench/` compares against C++/Abseil/Boost,
`std::meta::types_with` exists.

## Designed, not implemented
| Item | Design page | Notes |
|---|---|---|
| Mid-level IR (EIR) | - (compiler internal) | Prerequisite for check removal, generators, devirtualization, field-precise borrows beyond calls |
| Byte-copy moves as a guarantee; in-place construction guarantee | performance §3–4 | Mostly true today; needs EIR to guarantee |
| Check removal beyond LLVM (range analysis, hoisting) | performance §5 | LLVM already removes the checks of `xs[i]` for `i in 0..xs.len()`, and others after `std::assume` |
| `@std::dispatch_hint` (speculative devirtualization) | performance §8 | needs annotations on expressions |
| `profile_guided()` | build-modes | |
| `std::any_of<I...>`; bounds other than sendable/shareable; `std::function` with an allocator | lambdas | |
| Generators, resumable functions, `std::raw::stack_switch` | generators | needs EIR |
| `@std::diagnostic_frame` | templates | |
| Field-generating templates, `@std::reflect` run-time info, compile-time values as static data | metaprogramming | |
| Fast compile-time evaluator (bytecode), cached std/generated modules; template instances without re-parsing | - (compiler internal) | instantiating the graphics templates is now most of a 0.6 s check: re-parsing each template (23%) and resolving the copy (25%) |
| C++ interop extensions (virtual interfaces, instantiations, `@cpp::value`, exceptions) | c-interop | |
| Plugin hot reload, `std::filesystem::watch` | dynamic-libraries | |
| Windows (COFF, PDB, MSVC ABI), macOS, custom platform classes; generated debugger pretty-printers; `edaxc lsp` | - (tooling) | |

## Order of the remaining work
1. EIR, then check removal beyond LLVM.
2. `type_id`-based run-time info, static data, `embed`, field-generating templates.
3. Generators and resumables; fibers.
4. Template features for large libraries; lambda/`function` features.
5. Windows + PDB, C++ interop extensions, hot reload, language server.
