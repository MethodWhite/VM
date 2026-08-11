#!/usr/bin/env python3
"""bench_runner.py -- runner comparativo de benchmarks para VestaVM.

Corre cada benchmark del corpus en los modos de ejecucion y genera una tabla
comparativa de rendimiento (wall-time) con ratios de speedup respecto al
intérprete, valida que el resultado (R0) sea consistente entre modos, y
detecta regresiones comparando contra un baseline guardado.

Modos:
  interp     : -m vm  (100% interprete, oraculo de resultado).
  jit-vreg   : -m jit (path por defecto, regalloc vreg).
  jit-slots  : VESTA_JIT_VREGS=0 -m jit (path selector).
  aot        : compila ELF nativo (--aot) y lo ejecuta.

Salida:
  Tabla en consola (bench | interp | jit-vreg | jit-slots | aot | speedup max)
  + comparacion vs --baseline si se guardo con --save-baseline.

Uso:
  python tools/bench_runner.py [vm.exe]
      [--bench DIR]            directorio de benchmarks (default examples_codes_vex/benchmark)
      [--filter X]             substring para filtrar nombres
      [--timeout S]            timeout por modo (default 60)
      [--no-aot]               no compilar/ejecutar AOT (mas rapido)
      [--save-baseline FILE]   guardar tiempos como baseline (primera corrida)
      [--baseline FILE]        comparar contra baseline y reportar regresiones
      [--threshold F]          umbral de regresion x (default 1.5)
      [--out FILE]             guardar JSON detallado (default bench_result.json)
"""
from __future__ import annotations
import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

R00_RE = re.compile(r"R00=0x([0-9a-fA-F]+)")

# Benches con R0 legitimo no-determinista (punteros host / tiempo real):
# no se valida R0 entre modos, solo se mide el tiempo.
R0_IGNORE = {"100_reflection_full"}

MODES = [
    ("interp",    []),
    ("jit-vreg",  ["-m", "jit"]),
    ("jit-slots", ["-m", "jit"]),  # VESTA_JIT_VREGS=0 se setea en run_mode
]


def bench_name(path: Path) -> str:
    return path.name


def compile_velb(vm: Path, vex: Path, out_prefix: Path, timeout: float) -> bool:
    cmd = [str(vm), "--vex", str(vex), "-o", str(out_prefix)]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout, check=False)
    except subprocess.TimeoutExpired:
        return False
    velb = Path(str(out_prefix) + ".velb")
    return r.returncode == 0 and velb.exists()


def compile_aot(vm: Path, vex: Path, out: Path, timeout: float) -> bool:
    cmd = [str(vm), "--aot", str(vex), "-o", str(out)]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout, check=False)
    except subprocess.TimeoutExpired:
        return False
    return r.returncode == 0 and out.exists()


def run_mode(vm: Path, velb: Path, mode: str, timeout: float, tmp: Path):
    """Corre el .velb.  Devuelve (status, r0, seconds)."""
    env = dict(os.environ)
    vm_abs = str(vm.resolve())
    velb_abs = str(velb.resolve())
    cmd = [vm_abs, "--run", velb_abs, "--stats", "--schedulers", "1"]
    if mode == "interp":
        cmd += ["-m", "vm"]
    elif mode == "jit-vreg":
        cmd += ["-m", "jit"]
    elif mode == "jit-slots":
        cmd += ["-m", "jit"]
        env["VESTA_JIT_VREGS"] = "0"
    t0 = time.perf_counter()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout, env=env, cwd=str(tmp), check=False)
    except subprocess.TimeoutExpired:
        return "timeout", None, timeout
    dt = time.perf_counter() - t0
    out = (r.stdout or "") + (r.stderr or "")
    m = R00_RE.search(out)
    r0 = m.group(1).lstrip("0") or "0" if m else None
    if r.returncode != 0:
        return "crash", r0, dt
    if r0 is None:
        return "norun", r0, dt
    return "ok", r0, dt


def run_aot(exe: Path, timeout: float):
    """Ejecuta el binario AOT.  El AOT propaga el return del programa al
    exit code (exit==(i32)(return)), asi que un return != 0 es OK, no crash.
    Devuelve (status, r0, seconds) con r0 = hex(exit & 0xffffffff)."""
    t0 = time.perf_counter()
    try:
        r = subprocess.run([str(exe.resolve())], capture_output=True, text=True,
                           timeout=timeout, check=False)
    except subprocess.TimeoutExpired:
        return "timeout", None, timeout
    dt = time.perf_counter() - t0
    if r.returncode is not None and (r.returncode & 0xFF) not in (0,):
        # return del programa como resultado (exit code)
        r0 = format(r.returncode & 0xFFFFFFFF, "x")
        return "ok", r0, dt
    if r.returncode != 0:
        return "crash", None, dt
    # exit 0 sin stdout -> R0=0
    return "ok", "0", dt


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("vm", nargs="?", default="./build/vm", help="vm.exe")
    ap.add_argument("--bench", default="examples_codes_vex/benchmark")
    ap.add_argument("--filter", default="")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--no-aot", action="store_true")
    ap.add_argument("--save-baseline", default="")
    ap.add_argument("--baseline", default="")
    ap.add_argument("--threshold", type=float, default=1.5)
    ap.add_argument("--out", default="bench_results/bench_runner.json",
                    help="JSON detallado (default bench_results/bench_runner.json)")
    ap.add_argument("--reps", type=int, default=1,
                    help="repeticiones por modo (usa el mejor tiempo)")
    args = ap.parse_args()

    vm = Path(args.vm).resolve()
    bench_dir = Path(args.bench)
    tmp = Path("build/bench_tmp")
    tmp.mkdir(parents=True, exist_ok=True)

    vex_files = sorted(bench_dir.glob("bench_*.vex"))
    if args.filter:
        vex_files = [f for f in vex_files if args.filter in f.name]
    if not vex_files:
        print(f"[bench] No hay bench_*.vex en {bench_dir}")
        return 1

    print(f"[bench] {len(vex_files)} benchmarks, modos: interp/jit-vreg/jit-slots"
          + ("" if args.no_aot else "/aot")
          + f", timeout {args.timeout:.0f}s\n")

    rows = []
    for vex in vex_files:
        name = bench_name(vex)
        # -o X genera X.velb.  El prefijo debe ir SIN ".vex" (el nombre del
        # bench ya incluye la extension) para que el .velb final sea limpio.
        prefix = name[:-4] if name.endswith(".vex") else name
        velb = tmp / (prefix + ".velb")
        if not compile_velb(vm, vex, tmp / prefix, 120):
            rows.append({"name": name, "error": "no-compila"})
            continue

        results = {}
        for mode, extra in MODES:
            best = None
            last = None
            for _ in range(args.reps):
                st, r0, dt = run_mode(vm, velb, mode, args.timeout, tmp)
                last = (st, r0, dt)
                if st == "ok" and (best is None or dt < best[2]):
                    best = (st, r0, dt)
            if best is None:
                # conservar el primer status no-OK real (timeout/crash/norun)
                results[mode] = {"status": last[0] if last else "crash",
                                 "r0": last[1] if last else None,
                                 "s": last[2] if last else None}
            else:
                results[mode] = {"status": best[0], "r0": best[1], "s": best[2]}

        aot_row = None
        if not args.no_aot:
            exe = tmp / (prefix + ".aot")
            if compile_aot(vm, vex, exe, 120):
                st, r0, dt = run_aot(exe, args.timeout)
                results["aot"] = {"status": st, "r0": r0, "s": dt}
            else:
                results["aot"] = {"status": "no-compila"}

        rows.append({"name": name, "results": results})

    # ---- tabla ----
    print(f"{'bench':<34} {'interp':>9} {'jit-vreg':>9} {'jit-slots':>9}"
          + (f"{'aot':>9}" if not args.no_aot else "")
          + "   R0  status")
    print("-" * (80 if args.no_aot else 90))

    for row in rows:
        if "error" in row:
            print(f"{row['name']:<34} {'-':>9} {'-':>9} {'-':>9}   -- {row['error']}")
            continue
        r = row["results"]
        cells = []
        r0s = set()
        all_ok = True
        for mode in (["interp"] + [m for m, _ in MODES if m != "interp"]
                     + (["aot"] if not args.no_aot else [])):
            if mode not in r:
                cells.append("--")
                continue
            d = r[mode]
            if d["status"] == "ok":
                cells.append(f"{d['s']*1000:7.1f}ms")
                if d["r0"]:
                    r0s.add(d["r0"])
            elif d["status"] == "crash":
                cells.append("CRASH")
                all_ok = False
            elif d["status"] == "timeout":
                cells.append("TIME")
                all_ok = False
            else:
                cells.append("N/A")
                all_ok = False
        # consistencia R0 (excepto NODET).  El AOT solo propaga los 8 bits
        # bajos del return (exit code), asi que truncamos los r0 de los modos
        # VM a 8 bits antes de comparar.
        def low8(x: str) -> str:
            return format(int(x, 16) & 0xFF, "x")

        r0s_norm = {low8(v) for v in r0s}
        r0_ok = "OK" if (len(r0s_norm) <= 1 or name in R0_IGNORE) else f"DIVERGE({r0s_norm})"
        if not all_ok:
            r0_ok = "!!"
        r0_show = next(iter(r0s)) if r0s else "-"
        print(f"{row['name']:<34} {cells[0]:>9} {cells[1]:>9} {cells[2]:>9}"
              + (f" {cells[3]:>9}" if not args.no_aot else "")
              + f"   {r0_show[:4]:<4} {r0_ok}")

    # ---- baseline ----
    if args.save_baseline:
        bl = {}
        for row in rows:
            if "results" in row:
                bl[row["name"]] = {m: d.get("s")
                                   for m, d in row["results"].items() if "s" in d}
        with open(args.save_baseline, "w") as f:
            json.dump(bl, f, indent=2)
        print(f"\n[bench] baseline guardado en {args.save_baseline}")

    if args.baseline:
        try:
            with open(args.baseline) as f:
                bl = json.load(f)
        except Exception as e:
            print(f"[bench] no se pudo leer baseline: {e}")
            return 1
        print(f"\n=== regresiones vs {args.baseline} (threshold x{args.threshold:.2f}) ===")
        n_reg = 0
        for row in rows:
            if "results" not in row:
                continue
            name = row["name"]
            for mode, base in bl.get(name, {}).items():
                d = row["results"].get(mode)
                if not d or not base or "s" not in d:
                    continue
                if d["s"] > base * args.threshold:
                    n_reg += 1
                    print(f"  REGRESION {name} [{mode}]: "
                          f"{base*1000:.1f}ms -> {d['s']*1000:.1f}ms "
                          f"(x{d['s']/base:.2f})")
        if n_reg == 0:
            print("  sin regresiones")
        else:
            print(f"  {n_reg} regresion(es) detectada(s)")

    # ---- out JSON ----
    with open(args.out, "w") as f:
        json.dump(rows, f, indent=2)
    print(f"\n[bench] detalle en {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
