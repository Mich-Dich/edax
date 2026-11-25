#!/usr/bin/env python3
"""Checks the course's exercises, solutions and examples.

    python3 course/check.py 3              # lesson 3: run your exercise, compare with expected.txt
    python3 course/check.py s3             # lesson 3 of the standard-library track (course/std/)
    python3 course/check.py 3 --show       # also print the whole output
    python3 course/check.py --status       # one line per lesson: done or not yet
    python3 course/check.py --solutions    # every lesson's solution (keeps the course honest)
    python3 course/check.py --examples     # every lesson's examples/ (keeps the lessons honest)
    python3 course/check.py 3 --examples   # only lesson 3's examples

A lesson directory holds `exercise.ed` (or a directory `exercise/`), `expected.txt` and the same
as `solution`. A directory with a build.ed is run as a project (`edaxc run --root=<dir> app`),
otherwise its `main.ed` is run.

A lesson's `examples/` holds the programs its README shows: files, and directories whose
`main.ed` imports modules next to it. Each program says in its comments what it must do:
- `//~ error[E5012]` at the end of a line: that line must have this error. The annotations
  use the format of tests/feature (`//~^` = one line up). Every error of the file must be
  annotated, and the file is only checked, not run.
- `//> text`: a line the program prints. The lines are collected in order and compared with
  the program's whole stdout.
- `//! panic: text`: the program must stop with a panic whose message contains `text`.
- `//! build error: E6004`: building (not just checking) must fail with this error. For errors
  that only code generation finds, such as the checks left in a `@std::no_checks_left` function.
- An example with none of these must build and exit with 0.
"""
import argparse
import difflib
import pathlib
import re
import subprocess
import sys
import tempfile

COURSE = pathlib.Path(__file__).resolve().parent
REPO = COURSE.parent

SHORT_DIAG = re.compile(r"^(?P<path>.+?):(?P<line>\d+):(?P<col>\d+): (?P<sev>error|warning)\[(?P<code>[EW]\d{4})\]: (?P<msg>.*)$")
ANNOTATION = re.compile(r"//~(?P<dir>\^+|v+)?\s*(?P<sev>error|warning)\[(?P<code>[EW]\d{4})\]")
ERROR_CODE = re.compile(r"\b(?:error|warning)\[(?P<code>[EW]\d{4})\]")


# The tracks: the language course in course/, the standard library in course/std/.
TRACKS = {"language": COURSE, "std": COURSE / "std"}


def lesson_dirs(track=None):
    out = []
    for name, root in TRACKS.items():
        if track in (None, name) and root.is_dir():
            out += [d for d in sorted(root.iterdir()) if d.is_dir() and d.name[:2].isdigit()]
    return out


def lesson_dir(name):
    """`3` is lesson 3 of the language course, `s3` lesson 3 of the standard-library track."""
    m = re.fullmatch(r"([sS]?)(\d+)", name)
    if not m:
        sys.exit(f"'{name}' is not a lesson: write 3, or s3 for the standard-library track")
    track = "std" if m.group(1) else "language"
    for d in lesson_dirs(track):
        if int(d.name[:2]) == int(m.group(2)):
            return d
    sys.exit(f"no lesson {name} in {TRACKS[track]}")


def lesson_name(lesson):
    return str(lesson.relative_to(COURSE))


def error_index():
    """The codes ERRORS.md explains: code -> its heading's anchor."""
    index = {}
    errors = COURSE / "ERRORS.md"
    if errors.is_file():
        for line in errors.read_text().splitlines():
            m = re.match(r"^## (E\d{4}|W\d{4})\b(.*)$", line)
            if m:
                anchor = re.sub(r"[^a-z0-9 -]", "", line[3:].lower()).strip().replace(" ", "-")
                index[m.group(1)] = anchor
    return index


def explain_codes(stderr):
    """Points at ERRORS.md for every code in the compiler's output that it explains."""
    index = error_index()
    seen = []
    for m in ERROR_CODE.finditer(stderr):
        code = m.group("code")
        if code in index and code not in seen:
            seen.append(code)
    for code in seen:
        print(f"  {code} is explained in course/ERRORS.md#{index[code]}")


def command(edaxc, what, out_dir):
    if what.is_file():
        return [edaxc, "run", str(what)]
    if (what / "build.ed").is_file():
        return [edaxc, "run", f"--root={what}", f"--out-dir={out_dir}", "app"]
    return [edaxc, "run", str(what / "main.ed")]


def check(edaxc, lesson, name, show, quiet=False):
    what = lesson / f"{name}.ed"
    if not what.exists():
        what = lesson / name
    expected = (lesson / "expected.txt").read_text()
    with tempfile.TemporaryDirectory() as out:
        proc = subprocess.run(command(edaxc, what, out), capture_output=True, text=True, cwd=REPO)
    if show:
        print(proc.stdout, end="")
    if proc.returncode != 0 and not proc.stdout:
        print(f"{lesson_name(lesson)}/{what.name}: doesn't build or exits with {proc.returncode}")
        if not quiet:
            print(proc.stderr, end="")
            explain_codes(proc.stderr)
        return False
    if proc.stdout == expected:
        print(f"{lesson_name(lesson)}/{what.name}: done - the output is what the goal asks for")
        return True
    print(f"{lesson_name(lesson)}/{what.name}: not yet - the output differs from expected.txt")
    if not quiet:
        for line in difflib.unified_diff(expected.splitlines(), proc.stdout.splitlines(), "expected", "yours", lineterm=""):
            print("  " + line)
        if proc.returncode != 0:
            print(proc.stderr, end="")
    return False


def expectations(source):
    """(expected errors as {(line, code)}, expected stdout or None, expected panic text or None,
    expected build errors as [code])."""
    errors = set()
    output = []
    panic = None
    build_errors = []
    for number, line in enumerate(source.splitlines(), start=1):
        for m in ANNOTATION.finditer(line):
            moves = m.group("dir") or ""
            target = number - len(moves) if moves.startswith("^") else number + len(moves)
            errors.add((target, m.group("code")))
        stripped = line.strip()
        if stripped.startswith("//>"):
            output.append(stripped[4:] if stripped.startswith("//> ") else stripped[3:])
        if stripped.startswith("//! panic:"):
            panic = stripped[len("//! panic:"):].strip()
        if stripped.startswith("//! build error:"):
            build_errors.append(stripped[len("//! build error:"):].strip())
    return errors, ("\n".join(output) + "\n" if output else None), panic, build_errors


def check_example(edaxc, path):
    """Checks one example; returns a list of problems (empty: it does what it says)."""
    errors, output, panic, build_errors = expectations(path.read_text())
    if build_errors:
        proc = subprocess.run([edaxc, "run", path.name], capture_output=True, text=True, cwd=path.parent)
        found = {m.group("code") for m in ERROR_CODE.finditer(proc.stderr)}
        problems = [f"expected {c} when building, which wasn't reported" for c in build_errors if c not in found]
        if proc.returncode == 0:
            problems.append("it builds, but the comments say it must not")
        return problems
    if errors:
        proc = subprocess.run([edaxc, "check", "--diagnostic-format=short", path.name], capture_output=True,
                              text=True, cwd=path.parent)
        found = set()
        for line in (proc.stdout + proc.stderr).splitlines():
            m = SHORT_DIAG.match(line)
            if m and pathlib.Path(m.group("path")).name == path.name:
                found.add((int(m.group("line")), m.group("code")))
        problems = [f"line {l}: expected {c}, which wasn't reported" for l, c in sorted(errors - found)]
        problems += [f"line {l}: unexpected {c}" for l, c in sorted(found - errors) if c.startswith("E")]
        if proc.returncode == 0:
            problems.append("it builds, but the annotations say it must not")
        return problems
    proc = subprocess.run([edaxc, "run", path.name], capture_output=True, text=True, cwd=path.parent)
    problems = []
    if panic is not None:
        if proc.returncode == 0 or panic not in proc.stderr:
            problems.append(f"expected a panic with '{panic}', got exit {proc.returncode}: {proc.stderr.strip()[:300]}")
    elif proc.returncode != 0:
        problems.append(f"exit {proc.returncode}: {proc.stderr.strip()[:600]}")
    if output is not None and proc.stdout != output:
        diff = difflib.unified_diff(output.splitlines(), proc.stdout.splitlines(), "expected", "printed", lineterm="")
        problems.append("the output differs:\n" + "\n".join("    " + d for d in diff))
    return problems


def check_examples(edaxc, lessons):
    ok = True
    for lesson in lessons:
        examples = lesson / "examples"
        if not examples.is_dir():
            continue
        # A file, or a directory with a main.ed (a program with its own modules).
        paths = sorted(list(examples.glob("*.ed")) + list(examples.glob("*/main.ed")))
        for path in paths:
            problems = check_example(edaxc, path)
            name = f"{lesson_name(lesson)}/{path.relative_to(lesson)}"
            if problems:
                ok = False
                print(f"{name}: FAILED")
                for p in problems:
                    print("  " + p)
            else:
                print(f"{name}: ok")
    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("lesson", nargs="?", help="the lesson's number: 3, or s3 for the standard-library track")
    parser.add_argument("--track", choices=sorted(TRACKS), help="with --status/--solutions/--examples: only this track")
    parser.add_argument("--solutions", action="store_true", help="check the solutions instead")
    parser.add_argument("--examples", action="store_true", help="check the lessons' examples/ instead")
    parser.add_argument("--status", action="store_true", help="one line per lesson for your exercises")
    parser.add_argument("--show", action="store_true", help="print the program's output")
    parser.add_argument("--edaxc", default=str(REPO / "build" / "compiler" / "edaxc"))
    args = parser.parse_args()
    if not pathlib.Path(args.edaxc).is_file():
        sys.exit(f"edaxc isn't built yet ({args.edaxc}): run `cmake -S . -B build -G Ninja && cmake --build build`")
    if args.lesson is None and not (args.solutions or args.examples or args.status):
        parser.error("name a lesson (or --status, --solutions, --examples)")
    lessons = [lesson_dir(args.lesson)] if args.lesson is not None else lesson_dirs(args.track)
    if args.examples:
        sys.exit(0 if check_examples(args.edaxc, lessons) else 1)
    name = "solution" if args.solutions else "exercise"
    quiet = args.status
    ok = all([check(args.edaxc, d, name, args.show, quiet) for d in lessons])
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
