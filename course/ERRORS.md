# The errors you'll meet in this course

Each entry says what the error means, which C++ habit usually causes it, and how to fix it, with a link to the lesson that explains the rule. `python3 course/check.py N` points here when your exercise fails with one of these codes. `edaxc explain E5012` prints the one-line summary of any code.

The compiler reports errors in **stages**: syntax, then names, then types, then ownership, then code generation. Fixing all the errors of one stage can reveal errors of the next one. That's progress, not new mistakes.

---

## Syntax and declarations

## E2010 variable has no initial value
`u32 c;`. Edax has no uninitialized variables. Write `u32 c = 0;`, or `= default` for the type's default value. [Lesson 1](01-basics/README.md#1-variables--only-immutable-by-default)

## E2011 initialize with =
`u32 d{5};` or `T x(...)`. The only form is `T x = value;`. [Lesson 1](01-basics/README.md#1-variables--only-immutable-by-default)

## E2017 struct construction must name its fields
`span2{ 1, 4 }`. Write `span2{ .from = 1, .to = 4 }`. [Lesson 3](03-types-and-modules/README.md#building-values-designated-fields-no-constructors)

## E2026 captures are not allowed; name every capture
`[=]`, `[&]` or `[this]`. Name each capture: `[x]`, `[&x]`, `[mut &x]`, `[move x]`. [Lesson 7](07-lambdas/README.md#1-syntax-and-captures)

## E2001 expected something else
A plain syntax error. Two that come from C++ habits: `!(x is ...)` (`is` can't be negated, use a `match`, [lesson 9](09-containers-and-text/README.md#4-negative-patterns)), and a keyword used as a name (`bit`, `view` before `struct`, ...).

## E2030 invalid format spec
A spec the formatter doesn't know: a fill character (`{x:-<31}`) or a C/Python habit. The form is `[<>^][+][width][.precision][x|X|b|o]`. [Std lesson 4](std/04-text/README.md#3-format-specs)

---

## Names and modules

## E3010 defined more than once
Often a field and a method with the same name (`owner` and `owner()`): they share one scope. [Lesson 3](03-types-and-modules/README.md#members-are-named-bare-and-nothing-can-hide-them)

## E3013 declared but not defined
Either a header declaration has no body in the `.ed`, or the `.ed` **repeats** the declaration (`usize len();`) instead of giving it a body. The `.edh` declares, the `.ed` defines. [Lesson 3](03-types-and-modules/README.md#2-modules-edh-and-ed)

## E3015 not declared in the header
A public method or field that only the `.ed` has. Declare it in the `.edh`, or move it under `private:`. [Lesson 3](03-types-and-modules/README.md#2-modules-edh-and-ed)

## E3016 struct cannot add field in its .ed block
A struct's fields are all in the header. The `.ed`'s `struct` block only holds method bodies. [Lesson 3](03-types-and-modules/README.md#2-modules-edh-and-ed)

## E3017 function is not declared in the header
A helper function in a module's `.ed`. Mark it `private` (module-local), or declare it in the `.edh`. [Lesson 3](03-types-and-modules/README.md#2-modules-edh-and-ed)

## E3020 cannot find a name
Misspelled, not imported, or in another namespace. The notes list where the compiler looked and suggest close names.

## E3022 is a struct, not a function
`span2(1, 4)`: there are no constructors. Build with `span2{ ... }`, or call a `static` factory. [Lesson 3](03-types-and-modules/README.md#building-values-designated-fields-no-constructors)

## E3024 private to its module
Calling another module's `private` function. [Lesson 3](03-types-and-modules/README.md#privacy)

## E3026 a bare name is both a member and an outer item
A method and a free function with the same name, called bare inside a method. Write `this.name()` or qualify the free one. [Lesson 3](03-types-and-modules/README.md#members-are-named-bare-and-nothing-can-hide-them)

## E3028 already declared
No shadowing: a second variable with the same name in one function, or a local or parameter named like a member of the class. Choose another name. [Lesson 1](01-basics/README.md#1-variables--only-immutable-by-default), [lesson 3](03-types-and-modules/README.md#members-are-named-bare-and-nothing-can-hide-them)

## E3032 only local variables and by-value parameters can be moved
`move obj.field`. Use `std::take(mut obj.field)` (leaves the default) or `std::replace(mut obj.field, v)`. [Lesson 2](02-ownership/README.md#fields-stdtake-and-stdreplace)

## E3035 missing field
A struct literal leaves out a field that has no default. Give the field, or give it a default in the type. [Lesson 3](03-types-and-modules/README.md#building-values-designated-fields-no-constructors)

## E3040 class is not open and cannot be inherited from
Classes are final by default. Declare the base `open class`. [Lesson 5](05-interfaces/README.md#1-classes-open-virtual-override)

## E3043 marked override but overrides nothing
A typo in the name or the parameters, or the base method isn't `virtual`. [Lesson 5](05-interfaces/README.md#1-classes-open-virtual-override)

## E3044 has the same name as an inherited method
A method that replaces a virtual one needs `override`. [Lesson 5](05-interfaces/README.md#1-classes-open-virtual-override)

## E3065 declared by two modules
Two imported modules declare the same name in the same namespace. Rename one, or move it to another namespace. [Lesson 3](03-types-and-modules/README.md#try-this-afterwards)

---

## Types

## E4001 literal has no type
`auto n = 5;` or a literal in an f-string hole. Give it a suffix (`5u32`, `3.14f64`) or a typed declaration. [Lesson 1](01-basics/README.md#a-literal-takes-its-type-from-where-it-is-used)

## E4002 does not fit
`u8 x = 300;`. The literal is out of the type's range. [Lesson 1](01-basics/README.md#a-literal-takes-its-type-from-where-it-is-used)

## E4003 integer literal used as a float
`f64 x = 0;`. Write `0.0`. [Lesson 1](01-basics/README.md#a-literal-takes-its-type-from-where-it-is-used)

## E4010 expected one type, found another
No implicit conversions. The note says which cast to use (`cast<T>` when lossless, `cast::truncate` / `saturate` / `round` / `checked` otherwise), or that a container needs `.as_span()`, or that copying a derived object into a base would slice it. [Lesson 1](01-basics/README.md#casts-say-what-they-may-lose)

## E4011 operands have different types
`a + b` with `a` a `u32` and `b` a `usize`. Convert one side. [Lesson 1](01-basics/README.md#a-literal-takes-its-type-from-where-it-is-used)

## E4013 operator does not apply / cannot iterate
An operator the type doesn't have (`"a" + "b"`, `<` on a struct without `<=>`), or a `for` over something that isn't a container or stream (a bare list literal: give it a type first).

## E4020 cannot assign: it is not mutable
Declare the variable `mut`, or loop with `for (mut T& x : c)`. [Lesson 1](01-basics/README.md#1-variables--only-immutable-by-default)

## E4022 mut method on an immutable variable
Calling a method that changes the object (`push`, a `mut` method of yours) needs a `mut` variable. [Lesson 2](02-ownership/README.md#mutability-is-checked-at-every-use)

## E4023 a mutable reference argument is marked with mut
The caller writes `mut`: `bump(mut clicks)`. [Lesson 1](01-basics/README.md#3-functions)

## E4031 call is ambiguous
A literal argument that fits two overloads. Give it a suffix. [Lesson 1](01-basics/README.md#3-functions)

## E4060 match does not cover
A `match` must be exhaustive. Add the missing arms (better than `_`, which hides future alternatives). [Lesson 4](04-enums-and-errors/README.md#2-match)

## E4070 has no method
Often a C++ name: the note says the Edax one (`len`, `push`, `is_empty`, `get`). [Lesson 9](09-containers-and-text/README.md#1-the-containers)

## E4071 private to module
Reading a class's private field from another module. [Lesson 3](03-types-and-modules/README.md#privacy)

## E4073 is a function; call it
A function name used as a value. Wrap it in a lambda: `[](u32 v) { shout(v); }`. [Lesson 7](07-lambdas/README.md#a-functions-name-is-not-a-value)

## E4090 cast may lose data
`cast<f64>(some_i64)`: say how, with `cast::round`, `cast::truncate`, `cast::saturate` or `cast::checked`. [Lesson 1](01-basics/README.md#casts-say-what-they-may-lose)

## E4150 result is ignored
A returned `std::result` must be used. `_ = f();` drops it on purpose. [Lesson 4](04-enums-and-errors/README.md#a-result-cant-be-ignored)

## E4160 cannot deduce template parameter from untyped literals
`larger(3, 4)`. Write `larger(3u32, 4)` or `larger<u32>(3, 4)`. [Lesson 6](06-templates/README.md#a-literal-never-decides-what-t-is)

## E4161 does not satisfy a constraint
The type lacks what the template's constraint needs (here, `operator<=>`). [Lesson 6](06-templates/README.md#2-constraints-are-checked-twice-at-the-right-places)

## E4171 cannot be printed in an f-string
The value isn't `std::formattable`: give the type a `format` method or `@std::derive::formattable`, or print its parts. [Std lesson 4](std/04-text/README.md#4-printing-your-own-types)

## E4214 does not compile for this instance
A library method needs something your type doesn't have (`sort` needs `<`); the note names it. [Std lesson 1](std/01-core/README.md#4-ordering-and-hashing)

## E4191 ? would return the wrong error type
`?` needs identical error types. Convert with `.map_error(...)` or `.or_error(...)` first. [Lesson 4](04-enums-and-errors/README.md#-pass-the-error-up)

## E4200 not a compile-time value
A run-time value (such as an immutable local) where a compile-time one is needed. Make it `compile_time`. [Lesson 8](08-compile-time/README.md#1-compile-time-functions-and-values)

## E4201 build.ed does not set a setting
A setting read with `.value()` that `build.ed` didn't `g.set` for this target. [Lesson 11](11-build/README.md#3-settings-instead-of-define)

## E4210 a check failed
A `compile_time check` failed: the message is the check's own. For an entry check it's reported at the call. [Lesson 8](08-compile-time/README.md#3-check-static_assert-and-assert-in-one-statement)

## E4215 a mode turns off checks without allowing undefined behaviour
A build mode that weakens safety must say so with `allow_undefined_behavior()`. [Lesson 11](11-build/README.md#4-build-modes-are-classes)

## E4231 a lambda runs on several threads but can't be shared
A `for_chunks` / `reduce_chunks` / `std::parallel` body that captures `mut &`. Results come out through the chunks or a reduction. [Std lesson 13](std/13-parallel/README.md#2-thread-pools)

---

## Ownership and borrowing

## E5001 not a copy type
An owning value would be copied. Decide: `move x` (give it away), `x.clone()` (two copies), `std::take(mut obj.f)` (take a field), or borrow with a reference. [Lesson 2](02-ownership/README.md#choosing-a-small-decision-table)

## E5002 used after it was moved
The value went somewhere else. Use it before the move, move a clone, or assign a new value. [Lesson 2](02-ownership/README.md#2-owning-types-move-clone-or-take)

## E5010 field is a reference
References can't be stored in fields. Store a value or a smart pointer, or make the type a `view`. [Lesson 2](02-ownership/README.md#4-references-are-second-class)

## E5011 field has a view type, but the type is not a view
A `std::str` (or other view) field: declare the type `view struct`, or store a `std::string`. [Lesson 9](09-containers-and-text/README.md#3-views-in-containers)

## E5012 a returned reference needs exactly one source
A function returns a `std::str`, `std::span` or reference, but has no reference parameter (return a `std::static_str` or `std::string`), or several (return an owned value). [Lesson 1](01-basics/README.md#returning-text), [lesson 2](02-ownership/README.md#4-references-are-second-class)

## E5013 returns a reference that doesn't outlive the function
A returned reference or view into a local, a by-value parameter or a temporary (`std::string::from(x).as_str()`). Return an owned value. [Lesson 2](02-ownership/README.md#4-references-are-second-class)

## E5015 a std::function cannot hold a lambda with reference captures
A stored callback must own what it uses: capture by value or `move`. [Lesson 7](07-lambdas/README.md#2-a-lambda-with-a-reference-capture-is-a-view)

## E5020 passed as a mutable reference and also as another argument
Two references to one place, one of them `mut`. [Lesson 2](02-ownership/README.md#5-one-writer-or-many-readers)

## E5021 cannot call a mut method while the loop borrows it
Changing a container while looping over it (C++'s iterator invalidation). [Lesson 2](02-ownership/README.md#5-one-writer-or-many-readers)

## E5022 cannot use it while something borrows it
A view (a span, a darray of map entries, a container in an arena) still borrows it. Put the borrower in a block of its own. [Std lesson 3](std/03-maps/README.md#4-values-that-are-containers-and-walking-a-map), [std lesson 5](std/05-memory/README.md#4-allocators-and-arenas)

## E5023 already borrowed mutably
A second `mut &` borrow while the first lives, typically two threads capturing the same variable. Use an `std::atomic` or `std::mutex<T>`. [Lesson 10](10-concurrency/README.md#2-why-races-dont-compile)

---

## Code generation

## E6001 not supported yet
A feature the code generator doesn't implement yet. Often it follows from an earlier problem, such as a misspelled `std::` name the checker let through (it shows up as `{unknown}`).

## E6004 run-time checks are left in a no_checks_left function
State what the optimizer can't see with `std::assume`. [Lesson 12](12-performance/README.md#2-making-checks-disappear)

---

## Warnings

## W4002 implicit allocation of a std::string from a string literal
`std::string s = "text";`. Write `std::string::from("text")`. [Std lesson 4](std/04-text/README.md#2-building-text)

## W4210 a library check's warning
Warnings that library code raises from its own checks, each with an id: `std::relative_path` for a literal relative path ([std lesson 8](std/08-files/README.md#3-paths-and-where-files-are)), `std::log::trailing_newline`, `std::log::verbose_override` ([std lesson 9](std/09-logging/README.md)).
