#!/usr/bin/env python3
"""jit_fuzzer.py -- fuzzer diferencial del JIT de VestaVM.

Genera programas Vex aleatorios pero SEMANTICAMENTE deterministas (main que
devuelve un i32/i64 calculado; sin input externo, sin aleatoriedad en runtime),
los compila a .velb, los ejecuta en interp / jit-vreg / jit-slots y compara R00.

Si el interprete produce un R0 limpio (exit 0) pero alguno de los modos JIT
devuelve un R0 distinto o crashea (exit != 0) -> BUG del JIT.  Cada hallazgo
se guarda en tools/fuzz_findings/ (el .vex + un .json con metadatos), y con
--minimize se reduce el programa al minimo que sigue reproduciendo el bug.

El PRNG de Python decide SOLO la estructura (que operaciones, constantes y
tamanos de loop).  El programa generado es deterministico por construccion:
todo se deriva de constantes y del contador de loops, asi que interp y JIT
deben producir exactamente el mismo R00 (cualquier diferencia es un bug).

Uso:
  python tools/jit_fuzzer.py [vm.exe]
      [--iters N]        cuantos programas generar (default 300)
      [--seed S]         semilla del PRNG (default 1)
      [--timeout S]      timeout por corrida, segundos (default 15)
      [--minimize]       minimizar el repro al encontrar un bug
      [--out DIR]        directorio de hallazgos (default tools/fuzz_findings)
      [--no-vreg]        no comparar contra jit-vreg
      [--no-slots]       no comparar contra jit-slots
      [--dump-dir DIR]   guardar TAMBIEN los .vex generados (debug)
"""
from __future__ import annotations

import argparse
import json
import os
import random
import re
import subprocess
import sys
import time
from pathlib import Path

try:
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))  # no core dumps (rapidez)
except (ImportError, OSError, ValueError):
    pass

sys.path.insert(0, str(Path(__file__).resolve().parent))
from diff_harness import find_vm, run_mode

ROOT = Path(__file__).resolve().parents[1]

COLOR = sys.stdout.isatty() and not os.environ.get("NO_COLOR")


def col(c: str, s: str) -> str:
    return f"\033[{c}m{s}\033[0m" if COLOR else s


class Gen:
    """Genera programas Vex deterministicos con rastreo de tipos.

    Produce una lista de 'unidades' (statements de una linea + cabeceras de
    bloque) que el minimizador puede recortar unidad a unidad, y el source
    completo.  Solo se emiten construcciones verificadas contra el compiler
    real (ver tools/jit_fuzzer.py + los probes en /tmp).
    """

    RECIPE_NAMES = ("arith", "loop_accum", "bitops", "math", "struct",
                    "class_method", "class_plain", "nested", "cond")

    def __init__(self, rng: random.Random):
        self.rng = rng
        self.prelude: list[str] = []        # declaraciones top-level (struct/class)
        self.lines: list[str] = []          # unidades (cada una una linea)
        self.removable: set[int] = set()    # indices de lineas recortables
        self.loop_bounds: list[tuple[int, int]] = []  # (line_idx, bound)

    # -- utilidades -------------------------------------------------------
    def emit(self, line: str, removable: bool = True) -> None:
        idx = len(self.lines)
        self.lines.append(line)
        if removable:
            self.removable.add(idx)

    def emit_prelude(self, line: str) -> None:
        self.prelude.append(line)

    def pick(self, seq):
        return self.rng.choice(seq)

    def rint(self, a: int, b: int) -> int:
        return self.rng.randint(a, b)

    def hexlit(self, bits: int) -> str:
        v = self.rng.getrandbits(bits)
        return hex(v)

    def mask(self, bits: int) -> str:
        return hex((1 << bits) - 1)

    # -- expresiones (tipadas) -------------------------------------------
    def expr(self, typ: str, depth: int = 1) -> str:
        """Expresion de tipo typ (i32/i64/u64). depth>=1 evita recursiones
        sin fin; los leaves son literales tipados."""
        r = self.rng.random()
        if depth <= 0 or r < 0.45:
            return self.lit(typ)
        if r < 0.75:
            a = self.expr(typ, depth - 1)
            b = self.expr(typ, depth - 1)
            op = self.pick(["+", "-", "*", "^", "&", "|"])
            return f"({a} {op} {b})"
        # cast + shift (mezcla de anchos: SEXT/TRUNC, exercise del JIT)
        if r < 0.85:
            return self.cast(typ, depth)
        return self.shift(typ, depth)

    def lit(self, typ: str) -> str:
        if typ == "i32":
            return str(self.rint(-(1 << 20), (1 << 20)))
        if typ == "i64":
            return str(self.rint(-(1 << 40), (1 << 40)))
        return self.hexlit(64)

    def cast(self, typ: str, depth: int) -> str:
        src = self.pick([t for t in ("i32", "i64", "u64") if t != typ])
        return f"({typ})({self.expr(src, depth - 1)})"

    def shift(self, typ: str, depth: int) -> str:
        a = self.expr(typ, depth - 1)
        op = self.pick(["<<", ">>"])
        return f"({a} {op} {self.rint(1, 31)})"

    def bitop(self, inner: str) -> str:
        name = self.pick(["popcount", "clz", "ctz", "bswap", "rotl", "rotr"])
        arg = f"(u64)({inner})"
        if name in ("clz", "ctz"):
            arg = f"({arg} | 1)"          # evita clz(0)/ctz(0) UB
        if name in ("rotl", "rotr"):
            return f"{name}({arg}, {self.rint(1, 63)})"
        return f"{name}({arg})"

    def imath(self, inner: str) -> str:
        name = self.pick(["abs", "imin", "imax"])
        a = f"(i64)({inner})"
        if name == "abs":
            return f"abs({a})"
        return f"{name}({a}, (i64)({self.lit('i64')}))"

    def cond(self) -> str:
        t = self.pick(["i32", "i64", "u64"])
        a = self.expr(t)
        op = self.pick(["<", ">", "<=", ">=", "==", "!="])
        b = self.expr(t)
        return f"{a} {op} {b}"

    # -- recipes ----------------------------------------------------------
    def recipe_arith(self) -> None:
        self.emit("i64 a64 = 0;")
        for _ in range(self.rint(3, 8)):
            op = self.pick(["+", "-", "^", "&"])
            self.emit(f"a64 = (a64 {op} {self.expr('i64')}) {self.pick(['+', '^'])} 0x1F;")
        self.emit(f"i32 r = (i32)(a64 & {self.mask(32)});")

    def recipe_loop_accum(self) -> None:
        self.emit("i64 a64 = 0;")
        self.emit("i32 i = 0;", removable=False)
        n = self.rint(2, 40)
        idx = len(self.lines)
        self.emit(f"while (i < {n}) {{", removable=False)
        self.loop_bounds.append((idx, n))
        for _ in range(self.rint(2, 5)):
            op = self.pick(["+", "-", "^", "&", "|"])
            self.emit(f"a64 = a64 {op} (i64)(i * {self.rint(1, 9)}) {self.pick(['+', '^'])} {self.lit('i64')};")
        self.emit("i = i + 1;", removable=False)
        self.emit("}")
        self.emit("i32 r = (i32)(a64 & " + self.mask(32) + ");")

    def recipe_bitops(self) -> None:
        self.emit("u64 u = " + self.hexlit(64) + ";")
        self.emit("i32 i = 0;", removable=False)
        n = self.rint(2, 30)
        idx = len(self.lines)
        self.emit(f"while (i < {n}) {{", removable=False)
        self.loop_bounds.append((idx, n))
        for _ in range(self.rint(2, 4)):
            self.emit(f"u = u {self.pick(['^', '+', '&'])} {self.bitop('u')};")
        self.emit("i = i + 1;", removable=False)
        self.emit("}")
        self.emit("i32 r = (i32)(u & " + self.mask(32) + ");")

    def recipe_math(self) -> None:
        self.emit("i64 a64 = " + self.lit("i64") + ";")
        self.emit("i32 i = 0;", removable=False)
        n = self.rint(2, 30)
        idx = len(self.lines)
        self.emit(f"while (i < {n}) {{", removable=False)
        self.loop_bounds.append((idx, n))
        for _ in range(self.rint(2, 4)):
            self.emit(f"a64 = a64 + {self.imath('a64 - (i64)i')};")
        self.emit("i = i + 1;", removable=False)
        self.emit("}")
        self.emit("i32 r = (i32)(a64 & " + self.mask(32) + ");")

    def recipe_struct(self) -> None:
        """Struct value-type con campos de anchos mezclados + casts en loop
        (patron del bug conocido de struct_field con casts)."""
        fields = [("a", "i32"), ("b", "i64"), ("c", "u32"), ("d", "i64")]
        decl = " ".join(f"{t} {n};" for n, t in fields)
        self.emit_prelude(f"struct S {{ {decl} }}")
        self.emit("S s;")
        for n, t in fields:
            if t == "u32":
                self.emit(f"s.{n} = {self.hexlit(32)};")
            else:
                self.emit(f"s.{n} = {self.lit(t)};")
        self.emit("i64 a64 = 0;")
        self.emit("i32 i = 0;", removable=False)
        n = self.rint(2, 25)
        idx = len(self.lines)
        self.emit(f"while (i < {n}) {{", removable=False)
        self.loop_bounds.append((idx, n))
        ops = [
            "s.a = s.a + (i32)(s.b & 0xF);",
            "s.b = s.b ^ (i64)(s.a * 3) + (i64)s.c;",
            "s.c = s.c + (u32)((s.b >> 4) & 0xFF);",
            "s.d = s.d + (i64)s.a - (i64)s.c;",
            "a64 = a64 + s.b + (i64)s.a + (i64)s.c + s.d;",
        ]
        for _ in range(self.rint(2, 4)):
            self.emit(self.pick(ops))
        self.emit("i = i + 1;", removable=False)
        self.emit("}")
        self.emit("i32 r = (i32)(a64 & " + self.mask(32) + ");")

    def recipe_class_method(self) -> None:
        """Metodo de clase que lee this.<i32 field>, lo SEXT a i64 y lo
        combina con abs/imin/imax/popcount (patron del crash en jit-slots)."""
        self.emit_prelude("class Calc {")
        self.emit_prelude(" public i64 total;")
        self.emit_prelude(" public i32 k;")
        self.emit_prelude(" public Calc(i32 init) { this.total = (i64)init; this.k = 3; }")
        self.emit_prelude(" public i64 step(i32 x) {")
        self.emit_prelude("   i64 v = (i64)x * (i64)this.k;")
        for _ in range(self.rint(1, 2)):
            self.emit_prelude("   this.total = this.total + "
                      + self.pick([
                          "abs((i64)this.k - v) ^ (i64)this.k",
                          "imin((i64)this.k, v)",
                          "imax(v, (i64)this.k) + (i64)this.k",
                          "(i64)popcount((u64)v) + (i64)this.k",
                          "v ^ (i64)this.k ^ (i64)(x << 2)",
                      ]) + ";")
        self.emit_prelude("   return this.total;")
        self.emit_prelude(" }")
        self.emit_prelude("}")
        self.emit("Calc c = new Calc(5);")
        self.emit("i64 a64 = 0;")
        self.emit("i32 i = 0;", removable=False)
        n = self.rint(2, 20)
        idx = len(self.lines)
        self.emit(f"while (i < {n}) {{", removable=False)
        self.loop_bounds.append((idx, n))
        self.emit("a64 = a64 ^ c.step(i);")
        self.emit("i = i + 1;", removable=False)
        self.emit("}")
        self.emit("i32 r = (i32)(a64 & " + self.mask(32) + ");")

    def recipe_class_plain(self) -> None:
        """Metodo de clase simple (arith+field read) para stress del callvirt."""
        self.emit_prelude("class Acc {")
        self.emit_prelude(" public i64 total;")
        self.emit_prelude(" public i32 k;")
        self.emit_prelude(" public Acc(i32 init) { this.total = (i64)init; this.k = 7; }")
        self.emit_prelude(" public i64 bump(i32 x) {")
        self.emit_prelude("   this.total = this.total + (i64)(x + this.k);")
        self.emit_prelude("   return this.total ^ (i64)this.k;")
        self.emit_prelude(" }")
        self.emit_prelude("}")
        self.emit("Acc c = new Acc(1);")
        self.emit("i64 a64 = 0;")
        self.emit("i32 i = 0;", removable=False)
        n = self.rint(2, 20)
        idx = len(self.lines)
        self.emit(f"while (i < {n}) {{", removable=False)
        self.loop_bounds.append((idx, n))
        self.emit("a64 = a64 + c.bump(i);")
        self.emit("i = i + 1;", removable=False)
        self.emit("}")
        self.emit("i32 r = (i32)(a64 & " + self.mask(32) + ");")

    def recipe_nested(self) -> None:
        self.emit("i64 a64 = 0;")
        self.emit("i32 i = 0;", removable=False)
        n1 = self.rint(2, 10)
        idx1 = len(self.lines)
        self.emit(f"while (i < {n1}) {{", removable=False)
        self.loop_bounds.append((idx1, n1))
        self.emit("i32 j = 0;")
        n2 = self.rint(2, 10)
        idx2 = len(self.lines)
        self.emit(f"while (j < {n2}) {{", removable=False)
        self.loop_bounds.append((idx2, n2))
        self.emit(f"a64 = a64 + (i64)(i + j) * {self.rint(1, 7)};")
        self.emit("j = j + 1;", removable=False)
        self.emit("}")
        self.emit("i = i + 1;", removable=False)
        self.emit("}")
        self.emit("i32 r = (i32)(a64 & " + self.mask(32) + ");")

    def recipe_cond(self) -> None:
        self.emit("i64 a64 = 0;")
        self.emit("i32 i = 0;", removable=False)
        n = self.rint(2, 30)
        idx = len(self.lines)
        self.emit(f"while (i < {n}) {{", removable=False)
        self.loop_bounds.append((idx, n))
        for _ in range(self.rint(1, 3)):
            c = self.cond()
            t = self.lit("i64")
            f = self.lit("i64")
            self.emit(f"if ({c}) {{ a64 = a64 + {t}; }} else {{ a64 = a64 ^ {f}; }}")
        self.emit("i = i + 1;", removable=False)
        self.emit("}")
        self.emit("i32 r = (i32)(a64 & " + self.mask(32) + ");")

    # -- ensamblado -------------------------------------------------------
    def build(self) -> tuple[str, list[int], list[tuple[int, int]]]:
        """Devuelve (source, indices_removibles, loop_bounds)."""
        body = list(self.lines)
        pre = "\n".join(self.prelude)
        pre = pre + "\n\n" if pre else ""
        src = pre + "i32 main() {\n" + "\n".join(body) + "\n}"
        return src, list(self.removable), list(self.loop_bounds)

    def generate(self) -> tuple[str, list[int], list[tuple[int, int]], str]:
        self.prelude.clear()
        self.lines.clear()
        self.removable.clear()
        self.loop_bounds.clear()
        name = self.pick(self.RECIPE_NAMES)
        getattr(self, "recipe_" + name)()
        src, rem, bounds = self.build()
        return src, rem, bounds, name


# ---------------------------------------------------------------------------
# Harness de compilacion + ejecucion
# ---------------------------------------------------------------------------

def compile_prog(vm: Path, src: str, base: Path, timeout: float) -> bool:
    """Compila el .vex a base.velb.  Devuelve True si .velb existe."""
    vex = base.with_suffix(".vex")
    vex.write_text(src, encoding="utf-8")
    try:
        r = subprocess.run([str(vm), "--vex", str(vex), "-o", str(base)],
                           capture_output=True, text=True, errors="replace",
                           timeout=max(timeout, 20), check=False)
    except subprocess.TimeoutExpired:
        return False
    return r.returncode == 0 and base.with_suffix(".velb").is_file()


def classify(vm: Path, src: str, base: Path, modes: list[str],
             timeout: float) -> tuple[str, dict]:
    """Compila y corre.  Devuelve (cat, detail) con cat en
    NOCOMPILA | INTERP_BAD | OK | BUG.  INTERP_BAD = el interprete no dio un
    R0 limpio (programa no valido como oraculo; se descarta, no es bug)."""
    if not compile_prog(vm, src, base, timeout):
        return "NOCOMPILA", {}
    velb = base.with_suffix(".velb")
    res = {}
    for m in modes:
        st, r0 = run_mode(vm, velb, m, timeout, base.parent)
        res[m] = (st, r0)

    iv = res.get("interp", (None, None))
    if iv[0] != "ok" or iv[1] is None:
        return "INTERP_BAD", res

    detail = {"interp": iv, "modes": {}}
    buggy = []
    for m in modes:
        if m == "interp":
            continue
        st, r0 = res.get(m, (None, None))
        detail["modes"][m] = (st, r0)
        if st == "ok" and r0 == iv[1]:
            continue
        if st == "timeout":
            buggy.append((m, "TIMEOUT"))
        elif st == "crash":
            buggy.append((m, "CRASH"))
        else:
            buggy.append((m, "DIVERGE"))
    detail["buggy"] = buggy
    return ("BUG" if buggy else "OK", detail)


def minimizar(vm: Path, src: str, base: Path,
              modes: list[str], timeout: float, orig_buggy: set) -> str:
    """Reduce el programa: recorta statements y achica los loop-bounds.
    Acepta el recorte si el bug se mantiene (al menos un modo buggy original
    sigue siendo buggy).  Acotado: max N recortes por programa."""
    lines = src.split("\n")
    max_trims = 24
    trims = 0

    def scan_bounds(cur: list[str]) -> list[tuple[int, int]]:
        out = []
        for i, ln in enumerate(cur):
            m = re.search(r"while \(i < (\d+)\)", ln)
            if m:
                out.append((i, int(m.group(1))))
        return out

    # hasta 2 rondas: (1) recorte lineal de statements, (2) achicar bounds,
    # y repetir una vez mas porque los bounds cambian los indices.
    for _round in range(2):
        # recorte lineal: un pase, re-probando el mismo indice tras recortar.
        i = 1
        n = len(lines)
        while i < n - 1 and trims < max_trims:
            line = lines[i].strip()
            if not line or line in ("}", "{") or "while (" in line or "if (" in line:
                i += 1
                continue
            cand = lines[:i] + lines[i + 1:]
            cat, det = classify(vm, "\n".join(cand), base, modes, timeout)
            if cat == "BUG" and _sigue_el_bug(det, orig_buggy):
                lines = cand
                n = len(lines)
                trims += 1
            else:
                i += 1
        # achicar loop bounds.
        for li, b in scan_bounds(lines):
            while b > 1 and trims < max_trims:
                nb = max(1, b // 2)
                cand = lines[:]
                cand[li] = re.sub(r"while \(i < \d+\)", f"while (i < {nb})", cand[li])
                cat, det = classify(vm, "\n".join(cand), base, modes, timeout)
                if cat == "BUG" and _sigue_el_bug(det, orig_buggy):
                    lines, b = cand, nb
                    trims += 1
                else:
                    break
    return "\n".join(lines)


def _sigue_el_bug(det: dict, orig_buggy: set) -> bool:
    buggy = {f"{m}:{k}" for m, k in det.get("buggy", [])}
    return bool(buggy & orig_buggy)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("vm_path", nargs="?", help="ruta a build/vm (default: buscar)")
    ap.add_argument("--iters", type=int, default=300)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--timeout", type=float, default=15.0)
    ap.add_argument("--minimize", action="store_true")
    ap.add_argument("--out", default="tools/fuzz_findings")
    ap.add_argument("--no-vreg", action="store_true")
    ap.add_argument("--no-slots", action="store_true")
    ap.add_argument("--dump-dir", default="")
    args = ap.parse_args()

    vm = find_vm(args.vm_path, ROOT)
    if not vm:
        print(col("31", "[error] no se encontro vm (pasa la ruta a build/vm)"))
        return 1
    vm = vm.resolve()

    modes = ["interp"]
    if not args.no_vreg:
        modes.append("jit-vreg")
    if not args.no_slots:
        modes.append("jit-slots")

    outdir = ROOT / args.out
    outdir.mkdir(parents=True, exist_ok=True)
    dumpdir = Path(args.dump_dir) if args.dump_dir else None
    if dumpdir:
        dumpdir.mkdir(parents=True, exist_ok=True)

    tmp = ROOT / "tmp" / "jit_fuzzer"
    tmp.mkdir(parents=True, exist_ok=True)

    rng = random.Random(args.seed)
    gen = Gen(rng)

    stats = {"generados": 0, "compilados": 0, "corridos": 0,
             "NOCOMPILA": 0, "INTERP_BAD": 0, "OK": 0, "BUGS": 0}
    findings = []
    seen = set()
    t0 = time.time()

    print(col("36", f"[info] vm: {vm}"))
    print(col("36", f"[info] iters={args.iters} seed={args.seed} "
                    f"modes={modes[1:]} minimize={args.minimize}\n"))

    for it in range(1, args.iters + 1):
        src, _rem, bounds, recipe = gen.generate()
        stats["generados"] += 1
        base = tmp / f"fuzz_{args.seed}_{it}"

        cat, det = classify(vm, src, base, modes, args.timeout)
        if cat == "NOCOMPILA":
            stats["NOCOMPILA"] += 1
            continue
        stats["compilados"] += 1
        if cat == "INTERP_BAD":
            stats["INTERP_BAD"] += 1
            continue
        stats["corridos"] += 1
        if dumpdir:
            (dumpdir / f"{it}_{recipe}.vex").write_text(src, encoding="utf-8")

        if cat == "OK":
            stats["OK"] += 1
            continue

        # --- BUG ---
        stats["BUGS"] += 1
        h = hash(src)
        if h in seen:
            continue
        seen.add(h)

        buggy = {f"{m}:{k}" for m, k in det["buggy"]}
        final_src = src
        if args.minimize:
            # durante la minimizacion solo hace falta interp (oraculo) + los
            # modos que ya dieron bug -> mucho mas rapido por intento.
            min_modes = ["interp"] + [m for m, _ in det["buggy"]]
            final_src = minimizar(vm, src, base, min_modes,
                                  args.timeout, buggy)
            # re-clasificar para el reporte (con todos los modos)
            _, det = classify(vm, final_src, base, modes, args.timeout)

        rid = f"bug_s{args.seed}_{it}_{recipe}"
        (outdir / f"{rid}.vex").write_text(final_src, encoding="utf-8")
        rec = {"id": rid, "recipe": recipe, "seed": args.seed, "iter": it,
               "interp": det.get("interp"), "buggy": det.get("buggy"),
               "minimized": args.minimize}
        findings.append(rec)
        (outdir / f"{rid}.json").write_text(
            json.dumps(rec, indent=2), encoding="utf-8")

        print(col("31", f"  [BUG] {recipe:14s} iter={it:4d} "
                        f"buggy={det.get('buggy')} -> {outdir / (rid + '.vex')}"))

    elapsed = time.time() - t0
    rate = stats["generados"] / elapsed if elapsed > 0 else 0

    print(f"\n{col('36', '=== resumen jit_fuzzer ===')}")
    print(f"  generados : {stats['generados']}")
    print(f"  compilan  : {stats['compilados']}  ({100 * stats['compilados'] / max(1, stats['generados']):.0f}%)")
    print(f"  corren    : {stats['corridos']}")
    print(f"  NOCOMPILA : {stats['NOCOMPILA']}")
    print(f"  INTERP_BAD: {stats['INTERP_BAD']}")
    print(f"  OK        : {stats['OK']}")
    print(f"  BUGS      : {col('31', str(stats['BUGS']))}")
    print(f"  tiempo    : {elapsed:.1f}s")
    print(f"  throughput: {rate:.1f} programas/s  ({1000 / rate:.1f} ms/prog)")
    if findings:
        print(f"\n{col('31', 'hallazgos guardados en ' + str(outdir))}:")
        for rec in findings:
            print(f"  {rec['id']:32s} buggy={rec['buggy']}")

    out = outdir / "findings_summary.json"
    out.write_text(json.dumps(
        {"seed": args.seed, "iters": args.iters, "elapsed_s": elapsed,
         "throughput": round(rate, 2), "stats": stats, "findings": findings},
        indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
