# Edax for C++ developers

This course is twelve lessons for people who already write C++. Each lesson explains a few
features, mostly by how they differ from C++, then gives you a program to finish with an exact
expected output. Each lesson ends with "Try this afterwards" experiments: small edits that show
where the language draws its lines.

## How it works

```
cmake -S . -B build -G Ninja && cmake --build build   # edaxc in build/compiler/edaxc
python3 course/check.py 1                             # check your lesson 1
python3 course/check.py 1 --show                      # also print the program's output
python3 course/check.py --solutions                   # all reference solutions (they must pass)
```

- Each lesson directory has a `README.md` (the lesson), an `exercise.ed` (or a project in
  `exercise/`) with TODOs, an `expected.txt`, and a `solution`.
- Work in `exercise`. The checker runs it with `edaxc run` and compares stdout with
  `expected.txt`.
- Most exercises build as they are and print wrong values. Some don't compile on purpose: fixing
  that is the exercise, and the comment at the top says so.
- Solutions are next to the exercises. Look at them after you have something working: other
  solutions that produce the same output are just as valid.
- The full reference is `docs/design/`. Each lesson only covers what a C++ developer needs first.

## Lessons

| # | Lesson | You build |
|---|---|---|
| 1 | [The C++ habits that don't compile](01-basics/README.md): `mut`, literals, casts, checked arithmetic, `for`, blocks as values | a temperature report |
| 2 | [Ownership: copy, move, borrow](02-ownership/README.md): copy vs owning types, `move`, `clone`, second-class references, views | fix a C++-style inventory |
| 3 | [Structs, classes, modules](03-types-and-modules/README.md): designated construction, members and `this`, `.edh`/`.ed`, `private`, namespaces | a geometry module |
| 4 | [Enums, `match`, errors](04-enums-and-errors/README.md): payload enums, `is`, `option`/`result`, `?`, panics | an RPN calculator |
| 5 | [Inheritance and interfaces](05-interfaces/README.md): `open`, `virtual`/`override`, interfaces, `cast::dynamic`, static vs dynamic dispatch | a tiny game world |
| 6 | [Templates](06-templates/README.md): constraints, value parameters, `if compile_time`, packs | a ring buffer, `largest`, `join` |
| 7 | [Lambdas and callables](07-lambdas/README.md): named captures, `mut` state, `std::function`, `fn_ptr`, algorithms | an event bus and a leaderboard |
| 8 | [Compile-time code and checks](08-compile-time/README.md): `compile_time`, `check`, entry checks, `compile_time` parameters | a CRC table, checked APIs |
| 9 | [Containers and text](09-containers-and-text/README.md): `darray`, maps, `str`/`string`, views, `embed_text`, format specs | a log report |
| 10 | [Threads without data races](10-concurrency/README.md): scoped threads, `sendable`, atomics, `mutex<T>`, channels, pools | a prime counter with workers |
| 11 | [`build.ed`](11-build/README.md): targets, C libraries, settings, options, modes | a project's build |
| 12 | [Performance and plugins](12-performance/README.md): `noalias`, `std::assume`, `@std::no_checks_left`, SIMD, hot reload | a check-free SIMD particle step |
