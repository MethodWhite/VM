#!/usr/bin/env python3
"""check_jit_regressions.py -- gate de CI para regresiones del JIT.

Corre tools/diff_harness.py (interp vs jit-vreg vs jit-slots sobre el corpus)
y FALLA si hay bugs del JIT (CRASH/DIVERGE) que no estaban en el baseline
anterior.  El backlog conocido se tolera: solo se bloquean los bugs NUEVOS.

Esto convierte el harness en un test de regresion automatizable:
  - CI pasa    : no aparecio ningun bug JIT nuevo.
  - CI falla   : un cambio introdujo un DIVERGE/CRASH nuevo (o arreglo del
                 codegen con efecto lateral).  Revisar y actualizar baseline.

Uso:
  python tools/check_jit_regressions.py [vm.exe]
      [--baseline FILE]   baseline anterior (default tmp/diff_harness/known_bugs.json)
      [--timeout S]
      [--filter X]
      [--no-benchmarks]
"""
from __future__ import annotations
import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load_known(path: Path) -> set:
    if not path.exists():
        return set()
    try:
        d = json.loads(path.read_text(encoding="utf-8"))
        known = set()
        for rec in d.get("detail", []):
            if rec.get("cat") in ("CRASH", "DIVERGE"):
                known.add(rec["name"])
        return known
    except Exception:
        return set()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("vm", nargs="?", default=str(ROOT / "build" / "vm"))
    ap.add_argument("--baseline", default=str(ROOT / "bench_results" / "known_jit_bugs.json"))
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--filter", default="")
    ap.add_argument("--no-benchmarks", action="store_true")
    args = ap.parse_args()

    harness = ROOT / "tools" / "diff_harness.py"
    out = ROOT / "tmp" / "diff_harness" / "current.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.unlink(missing_ok=True)

    vm_abs = str(Path(args.vm).resolve())
    cmd = [sys.executable, str(harness), vm_abs, "--out", str(out),
           "--timeout", str(args.timeout)]
    if args.filter:
        cmd += ["--filter", args.filter]
    if args.no_benchmarks:
        cmd += ["--no-benchmarks"]

    r = subprocess.run(cmd, capture_output=False, check=False)
    if not out.exists():
        print("[check-jit] diff_harness no produjo salida; abortando")
        return 2

    cur = json.loads(out.read_text(encoding="utf-8"))
    summary = cur.get("summary", {})
    known = load_known(Path(args.baseline))

    new_bugs = []
    for rec in cur.get("detail", []):
        if rec.get("cat") in ("CRASH", "DIVERGE") and rec["name"] not in known:
            new_bugs.append(rec)

    print(f"[check-jit] summary: {summary}")
    print(f"[check-jit] bugs conocidos (baseline): {len(known)}")
    if new_bugs:
        print(f"\n[check-jit] BUGS NUEVOS DEL JIT ({len(new_bugs)}):")
        for rec in new_bugs:
            print(f"  {rec['cat']:8s} {rec['name']}")
            for mode in ("interp", "jit-vreg", "jit-slots"):
                print(f"             {mode:9s} {rec.get(mode)}")
        print("\n[check-jit] FALLA: revisa el codegen; si el cambio es legitimo,")
        print("            regenera el baseline con tools/diff_harness.py")
        return 1

    print("\n[check-jit] OK: sin regresiones nuevas del JIT.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
