#!/usr/bin/env python3
"""Proves the C++ engine and the Java engine are the same engine.

Node counts are the check. Two independent implementations of the same search
will only visit exactly the same number of nodes, return the same move, and
score it the same if they agree on every rule, every hash, and every move
ordering decision along the way. That makes this a far stronger test than any
set of unit tests anyone would realistically write.

The comparison runs bottom-up -- zobrist, then codec, then movegen, then step,
then eval, and only then the full search. A whole-search divergence is close to
impossible to localise on its own; with the layers checked in order, the first
one that fails tells you where to look.

Usage:
    python scripts/check-parity.py            # build both, compare everything
    python scripts/check-parity.py --depths 4,6,8
"""
import argparse
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
JAVA_OUT = ROOT / "target" / "parity-classes"
CPP_SRC = ROOT / "engine" / "src"
CPP_BIN = ROOT / "engine" / "build" / ("fanorona_bench.exe" if sys.platform == "win32" else "fanorona_bench")
POSITIONS = ROOT / "bench" / "positions.txt"
GOLDEN = ROOT / "bench" / "golden.txt"

LAYERS = ["zobrist", "codec", "movegen", "step", "eval"]

GREEN, RED, DIM, RESET = "\033[32m", "\033[31m", "\033[2m", "\033[0m"


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", **kw)


def build_java():
    JAVA_OUT.mkdir(parents=True, exist_ok=True)
    srcs = sorted(str(p) for p in (ROOT / "src/main/java/org/willy").glob("*.java"))
    r = run(["javac", "-d", str(JAVA_OUT), *srcs])
    if r.returncode != 0:
        sys.exit(f"javac failed:\n{r.stdout}\n{r.stderr}")


# The engine library plus the benchmark driver. server.cpp and trashtalk.cpp
# belong to the playable binary and carry their own main(), so globbing the
# directory would try to link two entry points together.
CPP_SOURCES = ["board.cpp", "zobrist.cpp", "eval.cpp", "search.cpp", "codec.cpp", "bench.cpp"]


def build_cpp():
    CPP_BIN.parent.mkdir(parents=True, exist_ok=True)
    cxx = shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        sys.exit("no g++ or clang++ on PATH")
    srcs = [str(CPP_SRC / name) for name in CPP_SOURCES]
    r = run([cxx, "-O3", "-std=c++20", "-pthread", "-o", str(CPP_BIN), *srcs])
    if r.returncode != 0:
        sys.exit(f"{cxx} failed:\n{r.stdout}\n{r.stderr}")


def java(*args):
    return run(["java", "-cp", str(JAVA_OUT), "org.willy.FanoronaServer", *args])


def cpp(*args):
    return run([str(CPP_BIN), f"--positions={POSITIONS}", *args])


def answers(bench_output):
    """Strips node/ttHit counts, keeping only what the search concluded."""
    out = []
    for line in bench_output.splitlines():
        if line.startswith("#"):
            continue
        fields = [f for f in line.split() if f.startswith(("score=", "bestMove="))]
        out.append(f"{line.split()[0]} {' '.join(fields)}")
    return "\n".join(out)


def compare(name, a, b):
    if a == b:
        print(f"  {GREEN}OK{RESET}   {name} {DIM}({len(a.splitlines())} lines){RESET}")
        return True
    print(f"  {RED}FAIL{RESET} {name}")
    ja, jb = a.splitlines(), b.splitlines()
    shown = 0
    for i in range(max(len(ja), len(jb))):
        x = ja[i] if i < len(ja) else "<missing>"
        y = jb[i] if i < len(jb) else "<missing>"
        if x != y:
            print(f"       line {i + 1}:\n         java: {x}\n         c++ : {y}")
            shown += 1
            if shown == 5:
                print("       ...")
                break
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--depths", default="4,6,8",
                    help="depths compared node-for-node (default 4,6,8)")
    ap.add_argument("--score-depths", default="10",
                    help="depths compared on score/best move only (default 10)")
    ap.add_argument("--skip-build", action="store_true")
    args = ap.parse_args()

    if not args.skip_build:
        print("building...")
        build_java()
        build_cpp()

    ok = True

    print("\nlayers:")
    for layer in LAYERS:
        ok &= compare(layer, java(f"--bench-dump={layer}").stdout, cpp(f"--bench-dump={layer}").stdout)

    print("\nperft (pure rules; must hold through every optimisation):")
    ok &= compare("perft depth<=5", java("--bench-perft=5").stdout, cpp("--bench-perft=5").stdout)

    print("\nsearch (node counts must match exactly):")
    for d in [x.strip() for x in args.depths.split(",") if x.strip()]:
        jr, cr = java("--bench", f"--depth={d}"), cpp(f"--depth={d}")
        ok &= compare(f"depth={d}", jr.stdout, cr.stdout)
        print(f"       {DIM}java {jr.stderr.strip().lstrip('# ')} | "
              f"c++ {cr.stderr.strip().lstrip('# ')}{RESET}")

    # Past the point where the fixed-size table starts evicting, the two engines
    # legitimately visit different numbers of nodes: Java's map grows without
    # bound and never loses an entry, while the C++ table is capped and replaces.
    # What must still agree is the answer. Alpha-beta returns the same minimax
    # value whatever the table remembers, so a score or best-move mismatch here
    # would be a real bug, while a small node-count difference is just eviction.
    if args.score_depths.strip():
        print("\ndeep search (table evicts, so compare the answer, not the effort):")
        for d in [x.strip() for x in args.score_depths.split(",") if x.strip()]:
            jr, cr = java("--bench", f"--depth={d}"), cpp(f"--depth={d}")
            ok &= compare(f"depth={d} score/bestMove", answers(jr.stdout), answers(cr.stdout))
            print(f"       {DIM}java {jr.stderr.strip().lstrip('# ')} | "
                  f"c++ {cr.stderr.strip().lstrip('# ')}{RESET}")

    print("\ngolden file:")
    ok &= compare("bench/golden.txt vs c++", GOLDEN.read_text(encoding="utf-8"), cpp().stdout)

    print()
    if ok:
        print(f"{GREEN}PARITY HOLDS{RESET} — the C++ port matches Java exactly.")
    else:
        print(f"{RED}PARITY BROKEN{RESET} — fix the topmost failing layer first.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
