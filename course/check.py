#!/usr/bin/env python3
"""Checks the course's exercises.

    python3 course/check.py 3          # lesson 3: run your exercise, compare with expected.txt
    python3 course/check.py 3 --show   # also print the whole output
    python3 course/check.py --solutions   # every lesson's solution (keeps the course honest)

A lesson directory holds `exercise.ed` (or a directory `exercise/`), `expected.txt` and the same
as `solution`. A directory with a build.ed is run as a project (`edaxc run --root=<dir> app`),
otherwise its `main.ed` is run.
"""
import argparse
import difflib
import pathlib
import subprocess
import sys
import tempfile

COURSE = pathlib.Path(__file__).resolve().parent
REPO = COURSE.parent


def lesson_dir(number):
    for d in sorted(COURSE.iterdir()):
        if d.is_dir() and d.name[:2].isdigit() and int(d.name[:2]) == number:
            return d
    sys.exit(f"no lesson {number} in {COURSE}")


def command(edaxc, what, out_dir):
    if what.is_file():
        return [edaxc, "run", str(what)]
    if (what / "build.ed").is_file():
        return [edaxc, "run", f"--root={what}", f"--out-dir={out_dir}", "app"]
    return [edaxc, "run", str(what / "main.ed")]


def check(edaxc, lesson, name, show):
    what = lesson / f"{name}.ed"
    if not what.exists():
        what = lesson / name
    expected = (lesson / "expected.txt").read_text()
    with tempfile.TemporaryDirectory() as out:
        proc = subprocess.run(command(edaxc, what, out), capture_output=True, text=True, cwd=REPO)
    if show:
        print(proc.stdout, end="")
    if proc.returncode != 0 and not proc.stdout:
        print(f"{lesson.name}/{what.name}: doesn't build or exits with {proc.returncode}:")
        print(proc.stderr, end="")
        return False
    if proc.stdout == expected:
        print(f"{lesson.name}/{what.name}: done - the output is what the goal asks for")
        return True
    print(f"{lesson.name}/{what.name}: not yet - the output differs from expected.txt:")
    for line in difflib.unified_diff(expected.splitlines(), proc.stdout.splitlines(), "expected", "yours", lineterm=""):
        print("  " + line)
    if proc.returncode != 0:
        print(proc.stderr, end="")
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("lesson", nargs="?", type=int, help="the lesson's number")
    parser.add_argument("--solutions", action="store_true", help="check the solutions instead")
    parser.add_argument("--show", action="store_true", help="print the program's output")
    parser.add_argument("--edaxc", default=str(REPO / "build" / "compiler" / "edaxc"))
    args = parser.parse_args()
    if args.lesson is None and not args.solutions:
        parser.error("name a lesson (or --solutions)")
    lessons = [lesson_dir(args.lesson)] if args.lesson is not None else \
        [d for d in sorted(COURSE.iterdir()) if d.is_dir() and d.name[:2].isdigit()]
    ok = all([check(args.edaxc, d, "solution" if args.solutions else "exercise", args.show) for d in lessons])
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
