#!/usr/bin/env python3
"""Bateria de edge case tests para Vex JIT + INTERP.

Compila cada .vex del directorio actual y lo ejecuta en ambos modos.
Reporta:
  - Compile fails.
  - Crashes (interp/jit).
  - Mismatches interp vs jit.
  - PASS si R00 == 0x2A (= 42).
  - FAIL si R00 != 0x2A (incluye 99 que indica "el catch no funciono").

Uso:
    python run_all.py [--filter REGEX] [--verbose]
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
VM = ROOT / "cmake-build-windows" / "vm.exe"
TMP = Path(os.environ.get("TEMP", "/tmp")) / "vex_edge"
TMP.mkdir(parents=True, exist_ok=True)

R00_RE = re.compile(r"R00=0x([0-9a-fA-F]+)")
WALL_RE = re.compile(r"Wall time:\s+(\d+)\s+ns")

# Colores ANSI (degraded si no TTY).
ENABLE_COLOR = sys.stdout.isatty() and not os.environ.get("NO_COLOR")
def c(code, s):
    if not ENABLE_COLOR: return s
    return f"\033[{code}m{s}\033[0m"
RED = lambda s: c("31", s)
GRN = lambda s: c("32", s)
YEL = lambda s: c("33", s)
CYN = lambda s: c("36", s)
DIM = lambda s: c("2",  s)
BLD = lambda s: c("1",  s)


def compile_vex(src: Path) -> Path | None:
    """Compila src.vex -> tmp/<stem>.velb.  None si falla."""
    stem = src.stem
    out_stem = TMP / stem
    velb = TMP / f"{stem}.velb"
    try:
        proc = subprocess.run(
            [str(VM), "--vex", str(src), "-o", str(out_stem)],
            capture_output=True, text=True, timeout=30.0,
        )
    except subprocess.TimeoutExpired:
        return None
    if proc.returncode != 0 or not velb.is_file():
        return None
    return velb


def run_vex(velb: Path, jit: bool) -> dict:
    """Ejecuta velb en interp o jit.  Devuelve dict con r00 + wall_ns +
    exit + stdout + stderr."""
    cmd = [str(VM), "--run", str(velb), "--stats"]
    if jit:
        cmd.append("-m"); cmd.append("jit")
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=30.0)
    except subprocess.TimeoutExpired:
        return {"r00": None, "wall_ns": -1, "exit": -1,
                "stdout": "", "stderr": "TIMEOUT"}
    r00 = None
    m = R00_RE.search(proc.stdout)
    if m:
        r00 = int(m.group(1), 16)
    wall_ns = -1
    mw = WALL_RE.search(proc.stdout)
    if mw:
        wall_ns = int(mw.group(1))
    return {
        "r00": r00, "wall_ns": wall_ns, "exit": proc.returncode,
        "stdout": proc.stdout, "stderr": proc.stderr,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--filter", type=str, default="")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    if not VM.is_file():
        print(RED(f"vm.exe no existe: {VM}"), file=sys.stderr)
        return 1

    tests = sorted(HERE.glob("*.vex"))
    if args.filter:
        flt = re.compile(args.filter)
        tests = [t for t in tests if flt.search(t.name)]
    if not tests:
        print("No hay tests que correr.")
        return 0

    print(f"{BLD('Edge case tests')} ({len(tests)} ficheros) vm={VM.name}")
    print()
    hdr = f"{'TEST':<30}{'INTERP':>14}{'JIT':>14}  {'STATUS':<14}"
    print(BLD(hdr))
    print(DIM("-" * len(hdr)))

    n_pass = 0
    n_fail = 0
    n_diff = 0
    n_compile_fail = 0
    n_interp_crash = 0
    n_jit_crash = 0
    failures: list[str] = []

    for src in tests:
        name = src.stem
        velb = compile_vex(src)
        if velb is None:
            n_compile_fail += 1
            print(f"{name:<30}{'-':>14}{'-':>14}  {RED('COMPILE_FAIL'):<14}")
            failures.append(f"{name}: compile_fail")
            continue

        ri = run_vex(velb, jit=False)
        rj = run_vex(velb, jit=True)

        def fmt(r):
            if r["r00"] is None:
                return RED("NULL")
            return f"0x{r['r00']:x}"

        # Veredicto.  Tests esperan 0x2A si pasan.
        i_ok = ri["r00"] == 0x2A
        j_ok = rj["r00"] == 0x2A
        diff = (ri["r00"] != rj["r00"])

        if ri["r00"] is None:
            n_interp_crash += 1
            status = RED("INTERP_CRASH")
        elif rj["r00"] is None:
            n_jit_crash += 1
            status = RED("JIT_CRASH")
        elif diff:
            n_diff += 1
            status = YEL("DIFF")
        elif i_ok and j_ok:
            n_pass += 1
            status = GRN("PASS")
        else:
            n_fail += 1
            status = RED("FAIL")
            failures.append(f"{name}: R00={ri['r00']:#x} (esperado 0x2a)")

        i_str = fmt(ri)
        j_str = fmt(rj)
        # Padding manual porque los codigos ANSI corrompen %-align.
        i_pad = 14 - len(f"0x{ri['r00']:x}" if ri['r00'] is not None else "NULL")
        j_pad = 14 - len(f"0x{rj['r00']:x}" if rj['r00'] is not None else "NULL")
        print(f"{name:<30}"
              f"{' ' * i_pad}{i_str}"
              f"{' ' * j_pad}{j_str}  "
              f"{status}")

        if args.verbose and (not i_ok or not j_ok):
            if ri["stderr"].strip():
                print(DIM(f"  interp stderr: {ri['stderr'].strip()[:200]}"))
            if rj["stderr"].strip():
                print(DIM(f"  jit    stderr: {rj['stderr'].strip()[:200]}"))

    total = len(tests)
    print()
    print(BLD("Resumen:"))
    print(f"  {GRN(f'PASS:           {n_pass}')}/{total}")
    print(f"  {RED(f'FAIL:           {n_fail}')}/{total}")
    print(f"  {YEL(f'DIFF (i/j):     {n_diff}')}/{total}")
    print(f"  {RED(f'INTERP_CRASH:   {n_interp_crash}')}/{total}")
    print(f"  {RED(f'JIT_CRASH:      {n_jit_crash}')}/{total}")
    print(f"  {RED(f'COMPILE_FAIL:   {n_compile_fail}')}/{total}")

    if failures:
        print()
        print(BLD("Failures:"))
        for f in failures:
            print(f"  {RED('-')} {f}")

    return 0 if (n_fail + n_diff + n_interp_crash + n_jit_crash + n_compile_fail) == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
