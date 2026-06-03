#!/usr/bin/env python3
"""Sprint lombok (2026-06-03): runner de tests de anotaciones tipo Lombok.

Compila cada .vex, lo ejecuta en interp y JIT, parsea R00 hex y verifica
que coincida con 0x2A (42) para PASS.  Reporta PASS/FAIL/DIFF/COMPILE_FAIL.
"""
from __future__ import annotations
import os, re, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
VM = ROOT / "cmake-build-windows" / "vm.exe"
TMP = Path(os.environ.get("TEMP", "/tmp"))

R_RE = re.compile(r"R00=0x([0-9a-fA-F]+)")


def run_vex(test: Path, mode: str) -> tuple[str, int | None]:
    velb_path = TMP / f"lombok_{test.stem}_{mode}.velb"
    base      = str(velb_path.with_suffix("")).replace("/", "\\")
    velb_abs  = str(velb_path).replace("/", "\\")
    vm        = str(VM).replace("/", "\\")
    test_abs  = str(test).replace("/", "\\")
    p = subprocess.run([vm, "--vex", test_abs, "-o", base],
                       capture_output=True, text=True, timeout=30)
    if p.returncode != 0 or not velb_path.exists():
        return "compile_fail", None
    args = [vm, "--run", velb_abs, "--stats"]
    if mode == "jit":
        # `-m jit` debe ir ANTES de `--run` para no confundir el parser.
        args.insert(1, "-m"); args.insert(2, "jit")
    p = subprocess.run(args, capture_output=True, text=True, timeout=30)
    if p.returncode != 0:
        if os.environ.get("LOMBOK_DEBUG"):
            sys.stderr.write(f"\n  [debug rc={p.returncode}] args={args}\n  stdout: {p.stdout[:200]}\n  stderr: {p.stderr[:200]}\n")
        return "crash", None
    m = R_RE.search(p.stdout)
    if not m:
        if os.environ.get("LOMBOK_DEBUG"):
            sys.stderr.write(f"\n  [debug no-R00] args={args}\n  out len={len(p.stdout)}\n  stdout[:200]: {p.stdout[:200]}\n")
        return "crash", None
    val = int(m.group(1), 16) & 0xFFFFFFFFFFFFFFFF
    return "ok", val


def main() -> int:
    tests = sorted(HERE.glob("*.vex"))
    if not tests:
        print("[lombok] sin tests en", HERE)
        return 0
    pass_n = fail_n = diff_n = cf_n = crash_n = 0
    fails: list[str] = []
    EXPECTED = 0x2A
    print(f"{'test':<40} {'interp':>10} {'jit':>10}  status")
    print("-" * 76)
    for t in tests:
        st_i, r_i = run_vex(t, "interp")
        st_j, r_j = run_vex(t, "jit")
        if st_i == "compile_fail" or st_j == "compile_fail":
            cf_n += 1
            print(f"{t.stem:<40} {'-':>10} {'-':>10}  COMPILE_FAIL")
            fails.append(f"  - {t.stem}: compile_fail")
            continue
        if st_i == "crash" or st_j == "crash":
            crash_n += 1
            print(f"{t.stem:<40} {'crash':>10} {'crash':>10}  CRASH")
            fails.append(f"  - {t.stem}: crash")
            continue
        ri = f"0x{r_i:x}" if r_i is not None else "-"
        rj = f"0x{r_j:x}" if r_j is not None else "-"
        if r_i != r_j:
            diff_n += 1
            print(f"{t.stem:<40} {ri:>10} {rj:>10}  DIFF")
            fails.append(f"  - {t.stem}: interp={ri} vs jit={rj}")
            continue
        if r_i == EXPECTED:
            pass_n += 1
            print(f"{t.stem:<40} {ri:>10} {rj:>10}  PASS")
        else:
            fail_n += 1
            print(f"{t.stem:<40} {ri:>10} {rj:>10}  FAIL")
            fails.append(f"  - {t.stem}: R00={ri} (esperado 0x2a)")
    n = len(tests)
    print()
    print("Resumen:")
    print(f"  PASS:           {pass_n}/{n}")
    print(f"  FAIL:           {fail_n}/{n}")
    print(f"  DIFF (i/j):     {diff_n}/{n}")
    print(f"  COMPILE_FAIL:   {cf_n}/{n}")
    print(f"  CRASH:          {crash_n}/{n}")
    if fails:
        print()
        print("Failures:")
        for f in fails: print(f)
    return 0 if (fail_n == 0 and diff_n == 0 and cf_n == 0 and crash_n == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
