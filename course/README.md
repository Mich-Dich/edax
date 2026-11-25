# Edax for C++ developers

Twelve lessons for people who already write C++. Each lesson explains a few features by how they differ from C++, and **why** Edax made a different choice. Then it gives you a program to finish, with an exact expected output and a checker that tells you when you're done.

Edax looks like C++ on purpose, so most of what you know carries over. The lessons concentrate on the places where a C++ habit is a compile error in Edax, and show the error you'll get, so that when you meet it in your own code you recognize it.

## Setting up

```
cmake -S . -B build -G Ninja && cmake --build build   # edaxc in build/compiler/edaxc
python3 course/check.py 1                             # check your lesson 1
```

## How a lesson works

Each lesson directory has:

| File | What it is |
|---|---|
| `README.md` | the lesson: explanations, examples, the compiler's real messages, hints, experiments |
| `examples/` | every example the lesson shows, as complete programs you can run and change |
| `exercise.ed` (or `exercise/`) | the program you finish: TODOs, or errors to fix |
| `expected.txt` | what the finished program prints |
| `solution.ed` (or `solution/`) | one way to do it; look after you have something working |

1. Read the lesson. Run its examples with `build/compiler/edaxc run course/01-basics/examples/casts.ed`, and change them: the fastest way to learn a rule is to break it and read the error.
2. Work in the exercise. Most exercises build as they are and print wrong values; some don't compile on purpose (the comment at the top says so), and fixing that is the exercise.
3. Check it:

   ```
   python3 course/check.py 1          # done, or a diff of what's still wrong
   python3 course/check.py 1 --show   # also print everything your program prints
   python3 course/check.py --status   # one line per lesson (both tracks; --track std or --track language for one)
   ```

   When your exercise doesn't build, the checker points at the entry of [`ERRORS.md`](ERRORS.md) for each error code it knows: what it means, which C++ habit causes it, and the fix.
4. Stuck? Each lesson has hints that open one at a time, from a nudge to almost the answer.
5. Done? Each lesson ends with "Try this afterwards": small edits that show where the language draws its lines. The answers are folded under them.

To start an exercise over, restore it with git: `git checkout -- course/01-basics/exercise.ed`. `edaxc run` builds in a temporary directory; `edaxc build file.ed` writes `build/<mode>/` next to the file, so prefer `run` (or `check.py`) in the course directories.

## Lessons

| # | Lesson | You build |
|---|---|---|
| 1 | [The C++ habits that don't compile](01-basics/README.md): `mut`, literals, casts, checked arithmetic, `mut` at the call, text return types, blocks as values | a temperature report |
| 2 | [Ownership: copy, move, borrow](02-ownership/README.md): copy vs owning types, `move`, `clone`, `std::take`, second-class references, views, places | fix a C++-style inventory |
| 3 | [Structs, classes, modules](03-types-and-modules/README.md): designated construction, factories, bare members, `.edh`/`.ed`, `private`, namespaces | a geometry module |
| 4 | [Enums, `match`, errors](04-enums-and-errors/README.md): payload enums, patterns, `is`, `option`/`result`, `?`, panics | an RPN calculator |
| 5 | [Inheritance and interfaces](05-interfaces/README.md): `open`, `virtual`/`override`, interfaces, `implement`, `cast::dynamic`, static vs dynamic dispatch | a tiny game world |
| 6 | [Templates](06-templates/README.md): constraints, value parameters, `if compile_time`, packs, type aliases, `distinct type` | a ring buffer, `largest`, `join` |
| 7 | [Lambdas and callables](07-lambdas/README.md): named captures, `mut` state, views, `std::function`, `fn_ptr`, algorithms | an event bus and a leaderboard |
| 8 | [Compile-time code and checks](08-compile-time/README.md): `compile_time`, types as values, `check`, entry checks, `compile_time` parameters | a CRC table, checked APIs |
| 9 | [Containers and text](09-containers-and-text/README.md): `darray`, maps, `str`/`string`, `view struct`, `embed_text`, format specs | a log report |
| 10 | [Threads without data races](10-concurrency/README.md): owned and scoped threads, atomics, `mutex<T>`, channels, pools | a prime counter with workers |
| 11 | [`build.ed`](11-build/README.md): targets, C libraries, settings, options, modes, tests | a project's build |
| 12 | [Performance and plugins](12-performance/README.md): `noalias`, `std::assume`, `@std::no_checks_left`, SIMD, hot reload | a check-free SIMD particle step |

The lessons build on each other; lessons 1 to 4 are the core. The full reference is [`docs/design/`](../docs/design/README.md).

## Next: the standard library

After the language, the [standard-library track](std/README.md) teaches what you build programs with, in 13 lessons of the same format: core utilities, sequences and maps in depth, text and formatting, smart pointers and allocators, math, time, files, logging, crash handling, reflection and annotations, synchronization, and channels, pools and parallel algorithms. Check them with `python3 course/check.py s1` and so on.

## Keeping the course honest

```
python3 course/check.py --solutions   # every reference solution prints its expected.txt
python3 course/check.py --examples    # every example does what its lesson says
```

Every example states in comments what it must do: `//> text` for the lines it prints, `//~ error[E5012]` on a line that must have that error (the format of `tests/feature`), `//! panic: text` for an expected panic, and `//! build error: E6004` for errors that only a build finds. Both commands should pass after every compiler or library change; when one fails, either the compiler regressed or the lesson needs updating.
