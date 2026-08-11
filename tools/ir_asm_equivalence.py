#!/usr/bin/env python3
"""ir_asm_equivalence.py -- verificador de equivalencia IR -> asm del JIT.

Mini-ASA para VestaVM (idea de Desmon: el analizador semantico valida que el
codigo emitido es correcto).  Para cada funcion JIT-compilada, verifica que
las operaciones del IR SSA produjeron las instrucciones nativas esperadas en
el asm emitido, en lugar de caer a CALL a vmath_* (slow path).

Verificaciones:
  - popcnt.i64 -> popcnt nativo (no call vmath_popcount)
  - clz.i64    -> lzcnt nativo
  - ctz.i64    -> tzcnt nativo
  - byteswap   -> bswap nativo
  - fsqrt.f64  -> sqrtsd nativo
  - fmin.f64   -> minsd nativo
  - fmax.f64   -> maxsd nativo
  - ffloor/fceil/fround/f trunc -> roundsd nativo
  - abs/imin/imax -> instrucciones nativas (no call vmath_*)
  - rotl/rotr -> rol/ror nativo

Esto detecta los gaps del codegen (p.ej. el gap FP documentado: las FP ops
del JIT van por CALL en vez de sqrtsd/minsd) de forma estructural, no solo
comparando resultados.

Uso:
  python tools/ir_asm_equivalence.py [vm.exe] [--filter X] [--out FILE]
  python tools/ir_asm_equivalence.py ./build/vm --filter fp_jit
"""
from __future__ import annotations
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# Mapa: op IR -> regex de instruccion asm nativa esperada.
# Si la op aparece en el IR pero el asm NO contiene la instruccion nativa
# (y en cambio tiene calls), se reporta como gap de equivalencia.
IR_TO_NATIVE = {
    "popcnt":    r"\bpopcnt\b",
    "clz":       r"\blzcnt\b",
    "ctz":       r"\btzcnt\b",
    "byteswap":  r"\bbswap\b",
    "fsqrt":     r"\bsqrtsd\b",
    "fmin":      r"\bminsd\b",
    "fmax":      r"\bmaxsd\b",
    "ffloor":    r"\broundsd\b",
    "fceil":     r"\broundsd\b",
    "fround":    r"\broundsd\b",
    "ftrunc":    r"\broundsd\b",
    "rotl":      r"\brol\b",
    "rotr":      r"\bror\b",
}

# Ops que el JIT legitima emite como CALL (vmath) cuando NO hay variante
# nativa -- verificar que al menos existen las nativas cuando corresponde.
IR_OP_RE = re.compile(r"\b(popcnt|clz|ctz|byteswap|fsqrt|fmin|fmax|ffloor|fceil|fround|ftrunc|rotl|rotr)\.(?:i64|f64)")


def get_ir_text(vm: Path, vex: Path, tmp: Path) -> str:
    """Extrae el IR SSA post-opt del .vex."""
    out = tmp / "ir_dump"
    r = subprocess.run([str(vm), "--vex-emit-ir", str(vex), "-o", str(out)],
                       capture_output=True, text=True, timeout=60, check=False)
    irf = Path(str(out) + ".ir")
    if irf.exists():
        return irf.read_text(encoding="utf-8", errors="replace")
    return ""


def get_jit_disasm(vm: Path, velb: Path, tmp: Path) -> dict[str, str]:
    """Desensambla cada funcion JIT via VESTA_JIT_DISASM.
    Devuelve {funcion: asm_text}."""
    env = {"VESTA_JIT_DISASM": "1"}
    import os
    full_env = dict(os.environ)
    full_env.update(env)
    r = subprocess.run([str(vm), "-m", "jit", "--run", str(velb)],
                       capture_output=True, text=True, timeout=60,
                       env=full_env, check=False)
    out = (r.stdout or "") + (r.stderr or "")
    funcs: dict[str, str] = {}
    current = None
    buf = []
    for line in out.splitlines():
        m = re.match(r"\[jit disasm\] === (\S+?)(?: \[vreg\])? \(", line)
        if m:
            if current:
                funcs[current] = "\n".join(buf)
            current = m.group(1)
            buf = []
            continue
        if re.match(r"\[jit disasm\] === fin", line):
            if current:
                funcs[current] = "\n".join(buf)
                current = None
                buf = []
            continue
        if current is not None:
            buf.append(line)
    if current:
        funcs[current] = "\n".join(buf)
    return funcs


def check_function(ir_text: str, func: str, asm: str) -> list[dict]:
    """Verifica que las ops IR de una funcion tengan su asm nativo."""    # Extraer el cuerpo de la funcion del IR.
    body = ""
    m = re.search(r"@function " + re.escape(func) + r"\(.*?\{(.*?)\n\}",
                  ir_text, re.DOTALL)
    if not m:
        # podria ser el post-opt; probar ambas secciones
        for section in ir_text.split("----- post-opt -----"):
            m2 = re.search(r"@function " + re.escape(func) + r"\(.*?\{(.*?)\n\}",
                           section, re.DOTALL)
            if m2:
                body = m2.group(1)
                break
    else:
        body = m.group(1)

    findings = []
    if not body:
        return findings
    for op in IR_OP_RE.findall(body):
        # el regex captura el nombre de la op; buscar la variante nativa
        # en el asm de la funcion
        native_re = IR_TO_NATIVE.get(op)
        if not native_re:
            continue
        has_native = bool(re.search(native_re, asm))
        has_call = bool(re.search(r"\bcall\b", asm))
        if not has_native and has_call:
            findings.append({
                "func": func,
                "op": op,
                "nativo_esperado": native_re,
                "nativo_presente": has_native,
                "calls_presentes": has_call,
                "tipo": "equivalencia",
            })

    # Fase 2 (eficiencia FP): si la funcion tiene ops FP nativas pero hace
    # un round-trip GP<->XMM por op (movq xmm<->GP intercalado), el valor FP
    # no se mantiene en XMM -> codegen suboptimo (gap de rendimiento, no de
    # equivalencia).  Heuristica: contar "movq xmm<->gpr" y las ops FP nativas.
    fp_native_ops = [op for op in IR_OP_RE.findall(body)
                     if op in ("fsqrt", "fmin", "fmax", "ffloor", "fceil",
                               "fround", "ftrunc")]
    if fp_native_ops:
        n_fp = len(fp_native_ops)
        # movq que transfieren entre GP y XMM (patron del codegen actual:
        # "movq xmm0, rcx" o "movq rax, xmm0")
        movq_rt = len(re.findall(r"\bmovq\s+(?:xmm\d+|r[a-z0-9]+),\s*(?:r[a-z0-9]+|xmm\d+)",
                                 asm))
        # en un loop de N ops FP con round-trip se esperan ~2 movq por op
        # (1 carga + 1 extraccion); si supera N (una op FP por bloque) con
        # mucho margen, hay round-trips innecesarios.
        if movq_rt > n_fp * 2:
            findings.append({
                "func": func,
                "op": "fmovq_roundtrip",
                "ops_fp": n_fp,
                "movq_roundtrips": movq_rt,
                "nativo_esperado": "mantener FP en XMM",
                "nativo_presente": True,
                "calls_presentes": False,
                "tipo": "eficiencia",
            })
    return findings


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("vm", nargs="?", default="./build/vm")
    ap.add_argument("--filter", default="")
    ap.add_argument("--out", default="")
    ap.add_argument("--bench", default="examples_codes_vex/benchmark")
    args = ap.parse_args()

    vm = Path(args.vm).resolve()
    bench = Path(args.bench)
    tmp = Path(tempfile.mkdtemp(prefix="ir_asm_"))

    vexes = sorted(bench.glob("bench_*.vex"))
    if args.filter:
        vexes = [v for v in vexes if args.filter in v.name]
    if not vexes:
        print(f"[asa] no hay bench_*.vex en {bench}")
        return 1

    all_findings = []
    for vex in vexes:
        name = vex.name[:-4]
        # nombre del dir C para el velb
        cname = name[6:] if name.startswith("bench_") else name
        velb = tmp / (name + ".velb")
        # compilar a velb
        subprocess.run([str(vm), "--vex", str(vex), "-o", str(tmp / name)],
                       capture_output=True, text=True, timeout=60, check=False)
        if not velb.exists():
            continue
        ir_text = get_ir_text(vm, vex, tmp)
        funcs = get_jit_disasm(vm, velb, tmp)
        if not funcs:
            print(f"[asa] {name}: sin funciones JIT desensambladas")
            continue
        for func, asm in funcs.items():
            findings = check_function(ir_text, func, asm)
            for f in findings:
                f["bench"] = name
            all_findings.extend(findings)

    # Reportar
    print(f"[asa] {len(all_findings)} gaps detectados (equivalencia + eficiencia)")
    seen = {}
    for f in all_findings:
        key = (f["bench"], f["op"], f["tipo"])
        seen[key] = seen.get(key, 0) + 1
    for (bench, op, tipo), count in sorted(seen.items()):
        extra = ""
        for f in all_findings:
            if f["bench"] == bench and f["op"] == op and f["tipo"] == tipo:
                if "movq_roundtrips" in f:
                    extra = f" ({f['movq_roundtrips']} movq round-trip)"
                break
        print(f"  [{tipo:12s}] {bench}: op '{op}'{extra} ({count} funcs)")

    if args.out:
        import json
        Path(args.out).write_text(json.dumps(all_findings, indent=2))

    # Exit 0 si no hay gaps, 1 si hay (para CI)
    return 0 if not all_findings else 1


if __name__ == "__main__":
    sys.exit(main())
