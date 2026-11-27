#!/usr/bin/env python3
"""Edax standard library benchmarks against C++ (STL, Abseil, Boost).

    python3 bench/run.py [--edaxc build/compiler/edaxc] [--runs 3] [filter...]

Each benchmark is bench/edax/<name>.ed (built with `edaxc build --mode=release`) and
bench/cpp/<name>[.<impl>].cpp (built with `c++ -O2`). Both must print the same result; the table
shows the best wall-clock time of the runs and Edax's time relative to the fastest C++ version.
"""
import argparse, os, pathlib, shutil, subprocess, sys, tempfile, time

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent


INSTALL_HINT = {"absl": "libabsl-dev", "boost": "libboost-dev", "entt": "libentt-dev (or set EDAX_BENCH_INCLUDE to a folder with entt/entt.hpp)",
                "glm": "libglm-dev (or set EDAX_BENCH_INCLUDE to a folder with glm/glm.hpp)"}


def cxx_flags(source: pathlib.Path) -> list[str] | None:
    """The compiler flags for one C++ benchmark; None when a library it needs isn't installed."""
    flags = ["-O2", "-std=c++20", "-DNDEBUG"]
    for folder in filter(None, os.environ.get("EDAX_BENCH_INCLUDE", "").split(os.pathsep)):   # e.g. where entt/entt.hpp is
        flags.append(f"-I{folder}")
    text = source.read_text()
    if "absl/" in text:
        mods = []
        if "flat_hash_map" in text: mods.append("absl_flat_hash_map")
        if "btree" in text: mods.append("absl_btree")
        try:
            out = subprocess.run(["pkg-config", "--cflags", "--libs", *mods], capture_output=True, text=True)
        except FileNotFoundError:
            return None
        if out.returncode != 0:
            return None
        flags += out.stdout.split()
    return flags


def timed(exe: str, runs: int) -> tuple[float, str]:
    best, output = float("inf"), ""
    for _ in range(runs):
        start = time.perf_counter()
        p = subprocess.run([exe], capture_output=True, text=True)
        elapsed = time.perf_counter() - start
        if p.returncode != 0:
            raise RuntimeError(f"{exe} failed ({p.returncode}): {p.stderr[-500:]}")
        best, output = min(best, elapsed), p.stdout.strip()
    return best, output


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--edaxc", default=str(ROOT / "build/compiler/edaxc"))
    ap.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--mode", default="release")
    ap.add_argument("filters", nargs="*")
    args = ap.parse_args()
    names = sorted(p.stem for p in (HERE / "edax").glob("*.ed"))
    if args.filters:
        names = [n for n in names if any(f in n for f in args.filters)]
    rows, ok, missing = [], True, set()
    with tempfile.TemporaryDirectory(prefix="edax-bench-") as tmp:
        for name in names:
            exe = f"{tmp}/{name}.edax"
            b = subprocess.run([args.edaxc, "build", f"--mode={args.mode}", str(HERE / "edax" / f"{name}.ed"), "-o", exe],
                               capture_output=True, text=True)
            if b.returncode != 0:
                print(f"{name}: edaxc failed\n{b.stderr[-2000:]}", file=sys.stderr)
                ok = False
                continue
            edax_time, edax_out = timed(exe, args.runs)
            others = []
            sources = [p for p in sorted((HERE / "cpp").glob(f"{name}*.cpp")) if p.stem == name or p.stem.startswith(name + ".")]
            for src in sources:
                impl = src.stem.split(".", 1)[1] if "." in src.stem else "std"
                cexe = f"{tmp}/{src.stem}.cpp.exe"
                flags = cxx_flags(src)
                if flags is None:
                    missing.add(impl)
                    continue
                c = subprocess.run([args.cxx, str(src), "-o", cexe, *flags], capture_output=True, text=True)
                if c.returncode != 0:
                    if impl != "std" and "No such file or directory" in c.stderr:   # the library isn't installed
                        missing.add(impl)
                        continue
                    print(f"{src.name}: c++ failed\n{c.stderr[-2000:]}", file=sys.stderr)
                    continue
                t, out = timed(cexe, args.runs)
                if out != edax_out:
                    print(f"{name}: output differs: edax {edax_out!r}, {impl} {out!r}", file=sys.stderr)
                    ok = False
                others.append((impl, t))
            rows.append((name, edax_time, others))
    print(f"{'benchmark':<14} {'edax':>8}   c++ (time per implementation)              edax / fastest c++")
    for name, rt, others in rows:
        cells = "  ".join(f"{impl} {t:6.3f}" for impl, t in others)
        best = min((t for _, t in others), default=None)
        ratio = f"{rt / best:5.2f}x" if best else "-"
        print(f"{name:<14} {rt:8.3f}   {cells:<42} {ratio}")
    for impl in sorted(missing):
        print(f"(skipped {impl}: not installed; sudo apt install {INSTALL_HINT.get(impl, impl)})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
