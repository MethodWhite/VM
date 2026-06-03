#!/usr/bin/env python3
"""run_all_benches.py -- benchmark suite multi-lenguaje VestaVM.

Sprint bench-multilang (2026-06-02):
- Descubre benches en @c examples_codes_vex/benchmark/<bench_name>/ con
  multiples implementaciones (main.vex, main.c, main.cpp, main.py,
  Main.java).
- Detecta compiladores/runtimes disponibles (gcc, g++, javac+java,
  python, vesta).
- Compila + ejecuta cada implementacion midiendo wall time (mediana de
  N runs).
- Reporta tabla agrupada por bench con todos los lenguajes lado a lado.
- Genera grafica matplotlib comparativa + JSON con todos los datos.
- Tambien soporta el layout legacy @c benchmark/*.vex (un fichero
  Vex sin carpeta) ejecutando solo en Vex interp + JIT.

Modos de ejecucion del Vex:
- @b interp: VESTA_JIT_THRESHOLD=UINT32_MAX (default), 100% interp.
- @b jit: VESTA_JIT_THRESHOLD=1, fuerza JIT en el primer call.

Output:
- Tabla coloreada en terminal con spinner live + timer.
- @c bench_results.json con datos crudos.
- @c bench_results.png con grafica comparativa (matplotlib opcional).

Uso:
  python tools/bench/run_all_benches.py                  # auto-detect + prompt
  python tools/bench/run_all_benches.py /path/al/vm.exe  # binary fijo
  python tools/bench/run_all_benches.py --langs vex,c    # subset lenguajes
  python tools/bench/run_all_benches.py --filter fib     # subset benches
"""
from __future__ import annotations
import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional


# Benches legacy (single-file .vex) excluidos del modo multi-lenguaje.
LEGACY_SKIP_PATTERN = re.compile(
    r"^(bench_macro_|bench_shared_|bench_native_callback|"
    r"100_reflection|get_time|test_vptr_loop)"
)

RUNS_PER_MODE = 3
BENCH_TIMEOUT = 120.0  # algunos benches Python tardan minutos


# =============================================================================
# Colores ANSI
# =============================================================================

class C:
    _enabled = sys.stdout.isatty() and not os.environ.get("NO_COLOR")
    RESET = "\033[0m" if _enabled else ""
    BOLD = "\033[1m" if _enabled else ""
    DIM = "\033[2m" if _enabled else ""
    RED = "\033[31m" if _enabled else ""
    GREEN = "\033[32m" if _enabled else ""
    YELLOW = "\033[33m" if _enabled else ""
    BLUE = "\033[34m" if _enabled else ""
    MAGENTA = "\033[35m" if _enabled else ""
    CYAN = "\033[36m" if _enabled else ""
    GREY = "\033[90m" if _enabled else ""

    @classmethod
    def enable_windows_ansi(cls):
        if sys.platform == "win32":
            try:
                import ctypes
                kernel32 = ctypes.windll.kernel32
                kernel32.SetConsoleMode(kernel32.GetStdHandle(-11), 7)
            except Exception:
                pass


def info(msg: str) -> None:
    print(f"{C.CYAN}[info]{C.RESET} {msg}")


def warn(msg: str) -> None:
    print(f"{C.YELLOW}[warn]{C.RESET} {msg}")


def err(msg: str) -> None:
    print(f"{C.RED}[error]{C.RESET} {msg}", file=sys.stderr)


def ok(msg: str) -> None:
    print(f"{C.GREEN}[ok]{C.RESET} {msg}")


# =============================================================================
# Resolver project root + binario vm
# =============================================================================

def find_project_root(start: Path) -> Path:
    cur = start.resolve()
    while True:
        if (cur / "CMakeLists.txt").is_file() and (cur / "examples_codes_vex").is_dir():
            return cur
        if cur.parent == cur:
            err("no encuentro project root (CMakeLists.txt + examples_codes_vex/)")
            sys.exit(1)
        cur = cur.parent


def find_vm_candidates(project_root: Path) -> list[tuple[str, Path]]:
    candidates: list[tuple[str, Path]] = []
    seen: set[Path] = set()
    build_patterns = [
        "cmake-build-windows", "cmake-build-debug", "cmake-build-release",
        "cmake-build-windows-debug", "cmake-build-windows-release",
        "build", "build-debug", "build-release", "out",
    ]
    for sub in build_patterns:
        for name in ("vm.exe", "vm"):
            cand = project_root / sub / name
            if cand.is_file():
                resolved = cand.resolve()
                if resolved not in seen:
                    seen.add(resolved)
                    candidates.append((f"build local ({sub})", cand))
                break
    for tool_name in ("vm", "vesta"):
        path_bin = shutil.which(tool_name)
        if path_bin:
            resolved = Path(path_bin).resolve()
            if resolved not in seen:
                seen.add(resolved)
                candidates.append((f"PATH ({tool_name})", Path(path_bin)))
    return candidates


def prompt_choose_vm(candidates: list[tuple[str, Path]]) -> Path:
    if not candidates:
        err("no encontre ningun binario vm/vesta")
        sys.exit(1)
    if len(candidates) == 1:
        desc, path = candidates[0]
        info(f"unico binario: {C.BOLD}{path}{C.RESET} ({desc})")
        return path
    if not sys.stdin.isatty():
        desc, path = candidates[0]
        warn(f"stdin no es TTY -- default a [0]: {desc} {C.DIM}{path}{C.RESET}")
        return path
    print()
    info(f"{C.BOLD}Encontre {len(candidates)} binarios vesta:{C.RESET}")
    for i, (desc, path) in enumerate(candidates):
        print(f"  {C.GREEN}[{i}]{C.RESET} {desc:<28} {C.DIM}{path}{C.RESET}")
    while True:
        try:
            choice = input(f"{C.CYAN}Elige [0-{len(candidates)-1}] "
                           f"(default 0): {C.RESET}").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            sys.exit(1)
        if not choice:
            return candidates[0][1]
        if choice.isdigit() and 0 <= int(choice) < len(candidates):
            return candidates[int(choice)][1]
        warn(f"opcion invalida: {choice!r}")


# =============================================================================
# Detector de toolchain por lenguaje
# =============================================================================

@dataclass
class Toolchain:
    """Configuracion de toolchain por lenguaje."""
    name: str             # vex_interp, vex_jit, c, cpp, python, java
    label: str            # texto visible
    color: str            # ANSI color para tablas
    available: bool       # True si el compilador/runtime esta instalado
    why_unavailable: str = ""


def get_tool_version(cmd: list[str], timeout: float = 5.0) -> str:
    """Ejecuta cmd y retorna la primera linea no vacia del stdout/stderr.
    Util para `gcc --version`, `python --version`, etc."""
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout)
        out = (r.stdout + "\n" + r.stderr).strip()
        for line in out.splitlines():
            line = line.strip()
            if line:
                return line[:120]
    except Exception:
        pass
    return "(version no detectada)"


def capture_system_info(vm_bin: Path, tc: dict) -> dict:
    """Captura hardware + OS + versiones de todos los compiladores
    usados.  Critico para reproducibilidad cross-machine.

    Sin deps externas (no usa psutil).  Para info detallada de CPU
    en Windows usa @c wmic; en Linux @c /proc/cpuinfo; en macOS
    @c sysctl.  Si nada esta disponible, fallback a @c platform.
    """
    import datetime
    import platform

    info_d: dict = {}

    # ---- Hardware ----
    hw: dict = {
        "arch": platform.machine(),
        "cpu_logical": os.cpu_count() or "?",
    }
    # CPU model + freq + cores fisicos + RAM segun plataforma.
    if sys.platform == "win32":
        try:
            r = subprocess.run(
                ["wmic", "cpu", "get", "Name,MaxClockSpeed,NumberOfCores", "/value"],
                capture_output=True, text=True, timeout=5)
            for line in r.stdout.splitlines():
                line = line.strip()
                if not line or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                if k == "Name":
                    hw["cpu_model"] = v.strip()
                elif k == "MaxClockSpeed":
                    hw["cpu_freq_mhz"] = f"{v.strip()} MHz"
                elif k == "NumberOfCores":
                    hw["cpu_physical"] = v.strip()
        except Exception:
            pass
        try:
            r = subprocess.run(
                ["wmic", "ComputerSystem", "get", "TotalPhysicalMemory", "/value"],
                capture_output=True, text=True, timeout=5)
            for line in r.stdout.splitlines():
                if line.startswith("TotalPhysicalMemory="):
                    bytes_v = int(line.split("=", 1)[1].strip())
                    hw["ram_gb"] = f"{bytes_v / (1024**3):.1f} GB"
        except Exception:
            pass
    elif sys.platform.startswith("linux"):
        try:
            with open("/proc/cpuinfo") as f:
                cpu_data = f.read()
            for line in cpu_data.splitlines():
                if line.startswith("model name") and "cpu_model" not in hw:
                    hw["cpu_model"] = line.split(":", 1)[1].strip()
                elif line.startswith("cpu MHz") and "cpu_freq_mhz" not in hw:
                    hw["cpu_freq_mhz"] = f"{float(line.split(':', 1)[1].strip()):.0f} MHz"
            hw["cpu_physical"] = str(cpu_data.count("processor"))
        except Exception:
            pass
        try:
            with open("/proc/meminfo") as f:
                for line in f:
                    if line.startswith("MemTotal:"):
                        kb = int(line.split()[1])
                        hw["ram_gb"] = f"{kb / (1024**2):.1f} GB"
                        break
        except Exception:
            pass
    elif sys.platform == "darwin":
        try:
            for k, key, fmt in (
                ("machdep.cpu.brand_string", "cpu_model", lambda v: v),
                ("hw.physicalcpu", "cpu_physical", lambda v: v),
                ("hw.cpufrequency_max",  "cpu_freq_mhz",
                    lambda v: f"{int(v) // 1_000_000} MHz"),
                ("hw.memsize", "ram_gb",
                    lambda v: f"{int(v) / (1024**3):.1f} GB"),
            ):
                r = subprocess.run(["sysctl", "-n", k],
                                    capture_output=True, text=True, timeout=2)
                if r.returncode == 0:
                    hw[key] = fmt(r.stdout.strip())
        except Exception:
            pass

    # Fallback: platform.processor() puede traer string.
    if "cpu_model" not in hw:
        hw["cpu_model"] = platform.processor() or "(desconocido)"
    info_d["hardware"] = hw

    # ---- OS ----
    info_d["os"] = {
        "platform": f"{platform.system()} {platform.release()}",
        "version": platform.version(),
        "release": platform.release(),
    }

    # ---- Toolchains ----
    tooling: dict = {}
    # Vex
    if vm_bin.is_file():
        vex_ver = get_tool_version([str(vm_bin), "--version"])
        tooling["Vex VM"] = {
            "available": True,
            "version": vex_ver,
            "path": str(vm_bin),
        }
    else:
        tooling["Vex VM"] = {"available": False}
    # gcc
    if tc.get("c") and tc["c"].available:
        tooling["C (gcc)"] = {
            "available": True,
            "version": get_tool_version([tc["c"].path, "--version"]),
            "path": tc["c"].path,
        }
    else:
        tooling["C (gcc)"] = {"available": False}
    # g++
    if tc.get("cpp") and tc["cpp"].available:
        tooling["C++ (g++)"] = {
            "available": True,
            "version": get_tool_version([tc["cpp"].path, "--version"]),
            "path": tc["cpp"].path,
        }
    else:
        tooling["C++ (g++)"] = {"available": False}
    # python
    if tc.get("python") and tc["python"].available:
        tooling["Python"] = {
            "available": True,
            "version": get_tool_version([tc["python"].path, "--version"]),
            "path": tc["python"].path,
        }
    else:
        tooling["Python"] = {"available": False}
    # java + javac
    if tc.get("java") and tc["java"].available:
        tooling["Java (javac)"] = {
            "available": True,
            "version": get_tool_version([tc["java"].path_javac, "--version"]),
            "path": tc["java"].path_javac,
        }
        tooling["Java (java)"] = {
            "available": True,
            "version": get_tool_version([tc["java"].path_java, "--version"]),
            "path": tc["java"].path_java,
        }
    else:
        tooling["Java"] = {"available": False}
    info_d["tooling"] = tooling

    # ---- Fecha del reporte ----
    info_d["date"] = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    return info_d


def print_system_banner(sys_info: dict) -> None:
    """Sprint bench-hwinfo (2026-06-02): banner expandido con hardware +
    OS + versiones de toolchains.  Permite comparar runs cross-maquina
    y dejar trazabilidad en el output de consola (no solo en el JSON).
    """
    hw = sys_info.get("hardware", {})
    os_ = sys_info.get("os", {})
    tools = sys_info.get("tooling", {})
    date = sys_info.get("date", "?")

    line_w = 78
    print()
    print(C.BOLD + "=" * line_w + C.RESET)
    print(f"{C.BOLD}  Sistema  {C.RESET}{C.DIM}{date}{C.RESET}")
    print(C.BOLD + "-" * line_w + C.RESET)

    def kv(k: str, v: str, color: str = C.BOLD) -> None:
        print(f"  {k:<14} {color}{v}{C.RESET}")

    kv("CPU",
       f"{hw.get('cpu_model', '?')} "
       f"({hw.get('cpu_physical', '?')} fisicos / "
       f"{hw.get('cpu_logical', '?')} logicos @ "
       f"{hw.get('cpu_freq_mhz', '?')})")
    kv("RAM",  hw.get("ram_gb", "?"))
    kv("Arch", hw.get("arch", "?"))
    kv("OS",   f"{os_.get('platform', '?')} ({os_.get('version', '?')})")

    if tools:
        print(C.BOLD + "-" * line_w + C.RESET)
        print(f"  {C.BOLD}Toolchains{C.RESET}")
        for name, t in tools.items():
            if t.get("available"):
                ver = t.get("version", "?")
                # Primera linea solamente (las versiones a veces son
                # multi-linea; e.g. gcc --version imprime banner+copyright).
                ver_short = (ver.splitlines() or ["?"])[0][:64]
                print(f"  {name:<14} {C.GREEN}{ver_short}{C.RESET}")
            else:
                print(f"  {name:<14} {C.RED}no disponible{C.RESET}")
    print(C.BOLD + "=" * line_w + C.RESET)


# =============================================================================
# Sprint bench-categories: agrupacion por dominio + geomean por categoria
# =============================================================================

# Inferencia automatica por el nombre del bench.  Cada bench cae en UNA
# categoria principal; cuando hay overlap (e.g. callvirt_hot mide tanto
# OOP como branch) prevalece la mas representativa del workload dominante.
BENCH_CATEGORIES: dict[str, str] = {
    # Floating-point heavy.
    "fp_jit":            "fp",
    # Integer arithmetic / counter loops.
    "tight_loop":        "int",
    "int_mixed":         "int",
    "cmp_fusion":        "int",
    "jit_method":        "int",
    "fib_recursive":     "int",
    "nested_loops":      "int",
    "intops_jit":        "int",
    "rotops_jit":        "bit",
    "bitops":            "bit",
    # Memory: allocation throughput.
    "alloc":             "alloc",
    "mem_class":         "alloc",
    "mem_malloc_free":   "alloc",
    "mem_struct":        "alloc",
    # Memory bandwidth.
    "memcpy_loop":       "mem",
    "array_sum":         "mem",
    # Branch-heavy / unpredictable control flow.
    "branch_unpredict":  "branch",
    "state_machine":     "branch",
    "quicksort":         "branch",
    # Hash / lookup tables.
    "hash_lookup":       "hash",
    # OOP: virtual dispatch + heap classes.
    "callvirt":          "oop",
    "callvirt_hot":      "oop",
    "polymorphic":       "oop",
    "pic_real":          "oop",
    "struct_field":      "oop",
    # String operations.
    "string_hot":        "string",
    "string_workout":    "string",
}


def bench_category(name: str) -> str:
    """Devuelve la categoria del bench, o 'misc' si no esta tagged."""
    return BENCH_CATEGORIES.get(name, "misc")


def print_category_summary(rows: list[dict], active_langs: list[str],
                            tc: dict[str, Toolchain],
                            baseline: str = "c") -> None:
    """Sprint bench-categories: geomean slowdown vs baseline POR categoria.

    Pemite identificar areas debiles del JIT (e.g. "FP: 7x, OOP: 3x,
    Branch: 12x").  Para cada categoria: geomean del ratio bench_lang /
    bench_baseline sobre todos los benches de esa categoria que tienen
    ambos valores positivos.
    """
    if not rows:
        return

    # Agrupar benches por categoria.
    cats: dict[str, list[dict]] = {}
    for row in rows:
        cats.setdefault(bench_category(row["bench"]), []).append(row)

    # Para cada (categoria, lang): geomean slowdown vs baseline.
    import math
    print()
    print(f"{C.BOLD}Geomean slowdown vs {tc[baseline].label} por categoria"
          f"{C.RESET}")
    print(f"{C.DIM}  (mas bajo = mejor; <1.0 = mas rapido que {baseline}){C.RESET}")
    print()

    header_cols = [f"{'CATEGORIA':<12}{'N':>4}"]
    for ln in active_langs:
        if ln == baseline:
            continue
        header_cols.append(f"{tc[ln].label:>16}")
    print(f"{C.BOLD}{' '.join(header_cols)}{C.RESET}")
    print("-" * (12 + 4 + 17 * (len(active_langs) - 1)))

    for cat in sorted(cats.keys()):
        rows_in_cat = cats[cat]
        cols = [f"{cat:<12}{len(rows_in_cat):>4}"]
        for ln in active_langs:
            if ln == baseline:
                continue
            ratios = []
            for r in rows_in_cat:
                v_ln = r.get(ln)
                v_base = r.get(baseline)
                if (v_ln is None or v_base is None
                        or v_ln <= 0 or v_base <= 0):
                    continue
                ratios.append(v_ln / v_base)
            if not ratios:
                cols.append(f"{C.GREY}{'N/A':>16}{C.RESET}")
                continue
            # Geomean.
            log_sum = sum(math.log(r) for r in ratios)
            geo = math.exp(log_sum / len(ratios))
            # Color: <1.5x = verde, <3x = amarillo, >=3x = rojo.
            if geo < 1.5:
                col = C.GREEN
            elif geo < 3.0:
                col = C.YELLOW
            else:
                col = C.RED
            cols.append(f"{col}{geo:>15.2f}x{C.RESET}")
        print(" ".join(cols))
    print("-" * (12 + 4 + 17 * (len(active_langs) - 1)))


def detect_toolchains(vm_bin: Path) -> dict[str, Toolchain]:
    tc: dict[str, Toolchain] = {}

    tc["vex_interp"] = Toolchain(
        "vex_interp", "Vex interp", C.YELLOW,
        available=vm_bin.is_file(),
        why_unavailable="" if vm_bin.is_file() else "vm binary missing"
    )
    tc["vex_jit"] = Toolchain(
        "vex_jit", "Vex JIT", C.GREEN,
        available=vm_bin.is_file(),
        why_unavailable="" if vm_bin.is_file() else "vm binary missing"
    )

    gcc = shutil.which("gcc")
    tc["c"] = Toolchain(
        "c", "C (gcc -O3)", C.BLUE,
        available=gcc is not None,
        why_unavailable="" if gcc else "gcc not in PATH"
    )
    tc["c"].path = gcc if gcc else ""

    gpp = shutil.which("g++")
    tc["cpp"] = Toolchain(
        "cpp", "C++ (g++ -O3)", C.MAGENTA,
        available=gpp is not None,
        why_unavailable="" if gpp else "g++ not in PATH"
    )
    tc["cpp"].path = gpp if gpp else ""

    py = shutil.which("python") or shutil.which("python3")
    tc["python"] = Toolchain(
        "python", "Python (CPython)", C.CYAN,
        available=py is not None,
        why_unavailable="" if py else "python not in PATH"
    )
    tc["python"].path = py if py else ""

    javac = shutil.which("javac")
    java = shutil.which("java")
    tc["java"] = Toolchain(
        "java", "Java (HotSpot)", C.RED,
        available=javac is not None and java is not None,
        why_unavailable="" if (javac and java) else "javac/java not in PATH"
    )
    tc["java"].path_javac = javac if javac else ""
    tc["java"].path_java = java if java else ""

    return tc


# =============================================================================
# Bench discovery
# =============================================================================

@dataclass
class BenchVariant:
    """Una implementacion de un bench en un lenguaje especifico."""
    lang: str             # vex, c, cpp, python, java
    src_path: Path
    bench_name: str       # nombre del bench (ej. tight_loop)


@dataclass
class Bench:
    """Un benchmark con todas sus variantes en distintos lenguajes."""
    name: str
    variants: dict[str, BenchVariant] = field(default_factory=dict)
    is_legacy: bool = False  # True si es un .vex single-file old style


def discover_benches(bench_dir: Path) -> list[Bench]:
    benches: list[Bench] = []
    # 1. Carpetas multi-lenguaje.
    multilang_names: set[str] = set()
    for d in sorted(bench_dir.iterdir()):
        if not d.is_dir() or d.name == "out":
            continue
        b = Bench(name=d.name)
        candidates = {
            "vex":    d / "main.vex",
            "c":      d / "main.c",
            "cpp":    d / "main.cpp",
            "python": d / "main.py",
            "java":   d / "Main.java",
        }
        for lang, p in candidates.items():
            if p.is_file():
                b.variants[lang] = BenchVariant(lang=lang, src_path=p,
                                                 bench_name=b.name)
        if b.variants:
            benches.append(b)
            multilang_names.add(d.name)
    # 2. Single-file legacy (.vex) -- solo Vex.
    # Sprint string-perf-6 (2026-06-02): skip de duplicados.  Si el .vex
    # legacy tiene contrapartida multi-lenguaje (e.g. bench_array_sum.vex
    # vs carpeta array_sum/), saltar el legacy para evitar filas dobles
    # en la tabla de resultados.  El nombre logico del legacy se deriva
    # quitando el prefijo "bench_" si existe.
    for f in sorted(bench_dir.glob("*.vex")):
        if LEGACY_SKIP_PATTERN.match(f.stem):
            continue
        logical = f.stem
        if logical.startswith("bench_"):
            logical = logical[len("bench_"):]
        # Tambien aceptar sufijos comunes que el multi-lang no usa.
        # bench_bitops_jit -> bitops, bench_intops_jit -> int_mixed (no
        # 1:1 pero el caso bitops_jit -> bitops es directo).  Eliminamos
        # el sufijo "_jit" para que esos legacy se colapsen con el dir.
        logical_no_jit = logical[:-len("_jit")] if logical.endswith("_jit") else logical
        if logical in multilang_names or logical_no_jit in multilang_names:
            continue
        b = Bench(name=f.stem, is_legacy=True)
        b.variants["vex"] = BenchVariant(lang="vex", src_path=f,
                                          bench_name=b.name)
        benches.append(b)
    return benches


# =============================================================================
# Spinner con timer
# =============================================================================

class Spinner:
    FRAMES = ["|", "/", "-", "\\"]

    def __init__(self, label: str, color: str = ""):
        self.label = label
        self.color = color
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._start = 0.0

    def _run(self):
        i = 0
        while not self._stop.is_set():
            elapsed = (time.perf_counter() - self._start) * 1000.0
            frame = self.FRAMES[i % len(self.FRAMES)]
            sys.stdout.write(
                f"\r{self.color}{frame}{C.RESET} {self.label} "
                f"{C.DIM}({elapsed:7.1f} ms){C.RESET}\033[K"
            )
            sys.stdout.flush()
            i += 1
            time.sleep(0.08)

    def __enter__(self):
        self._start = time.perf_counter()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return self

    def __exit__(self, *_):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=0.5)
        sys.stdout.write("\r\033[K")
        sys.stdout.flush()


# =============================================================================
# Ejecucion / compilacion por lenguaje
# =============================================================================

def run_timed(cmd: list[str], env: dict | None = None,
              timeout: float = 60.0, cwd: Optional[Path] = None) -> float:
    """Wall time external (incluye fork + startup del runtime).

    Para C/C++/Python/Java el overhead del fork+startup es bajo (<5ms en
    binarios optimizados; Python ~30ms; Java ~50-80ms por JVM init).  Para
    Vex la VM tiene ~28ms de overhead de boot que distorsiona benches
    cortos -- usar @c run_timed_vex en su lugar."""
    t0 = time.perf_counter()
    try:
        subprocess.run(
            cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            env=env, timeout=timeout, cwd=str(cwd) if cwd else None, check=False,
        )
    except subprocess.TimeoutExpired:
        return -1.0
    return (time.perf_counter() - t0) * 1000.0


# Sprint bench-walltime (2026-06-02): patron de parseo para extraer el
# Wall time interno reportado por @c --stats.  Forma del output:
#   "Wall time:     2895000 ns  (2895 us, 2 ms)"
_VEX_WALL_RE = re.compile(r"Wall time:\s+(\d+)\s+ns")


def run_timed_vex(cmd: list[str], env: dict | None,
                   timeout: float, cwd: Optional[Path]) -> float:
    """Wall time INTERNO del scheduler (excluye VM init overhead).

    Ejecuta el binario Vex con @c --stats, captura stdout, y extrae la
    linea "Wall time: N ns".  Si no se encuentra (Vex < esta version),
    fallback a wall externo via @c run_timed.

    Cualquier @c --stats / @c --jit-stats que el usuario ya tenga en
    @c cmd se preserva (no se duplica)."""
    cmd_with_stats = list(cmd)
    if "--stats" not in cmd_with_stats:
        cmd_with_stats.append("--stats")
    try:
        proc = subprocess.run(
            cmd_with_stats, capture_output=True, text=True,
            env=env, timeout=timeout, cwd=str(cwd) if cwd else None, check=False,
        )
    except subprocess.TimeoutExpired:
        return -1.0
    m = _VEX_WALL_RE.search(proc.stdout or "")
    if not m:
        m = _VEX_WALL_RE.search(proc.stderr or "")
    if not m:
        # Fallback a wall externo si el binario no soporta --stats.
        return run_timed(cmd, env=env, timeout=timeout, cwd=cwd)
    ns = int(m.group(1))
    return ns / 1_000_000.0  # ns -> ms


def median_runs(cmd: list[str], env: dict | None, runs: int,
                 timeout: float, cwd: Optional[Path] = None) -> float:
    times = []
    for _ in range(runs):
        ms = run_timed(cmd, env=env, timeout=timeout, cwd=cwd)
        if ms < 0:
            return -1.0
        times.append(ms)
    return statistics.median(times)


def all_runs(cmd: list[str], env: dict | None, runs: int,
             timeout: float, cwd: Optional[Path] = None,
             warmup: int = 0, use_vex_walltime: bool = False) -> list[float]:
    """Devuelve los runs MEDIDOS (excluye los primeros @c warmup runs).

    Para JIT/JVM el primer run paga compile-fresh y no es representativo
    del steady-state.  Estandar de la industria: descartar W warmups.

    Total runs ejecutados: warmup + runs.  Retorna solo los runs reales
    (longitud == @c runs).  Devuelve lista vacia si cualquier run falla.

    @param use_vex_walltime  Si true, ejecuta con @c --stats y extrae el
    Wall time interno (sin VM init overhead).  Solo para langs Vex.

    Resiliencia contra flakes ambientales:
    - @c MAX_ATTEMPTS=5 reintentos por run individual antes de fallar.
    - Backoff exponencial entre reintentos (250ms, 500ms, 1s, 2s).
    - Si un TIMEOUT ocurre tras el primer run exitoso, no abortar todo
      el bench: skipear ese run y continuar con los demas (devolver lo
      medido si al menos 1 run salio).  Solo abortar si NINGUN run sale.

    Causas tipicas de flake en Windows:
    - AV scan / Windows Defender bloqueando momentaneamente el .exe.
    - Carga del sistema durante el run (otros procesos, telemetria).
    - Handle/file lock contention al lanzar muchos subprocess rapido.
    """
    total = max(0, warmup) + runs
    times = []
    timer = run_timed_vex if use_vex_walltime else run_timed
    MAX_ATTEMPTS = 5
    BACKOFF_MS = [0, 250, 500, 1000, 2000]
    for i in range(total):
        ms = -1.0
        for attempt in range(MAX_ATTEMPTS):
            if attempt > 0:
                time.sleep(BACKOFF_MS[attempt] / 1000.0)
            ms = timer(cmd, env=env, timeout=timeout, cwd=cwd)
            if ms >= 0:
                break
        if ms < 0:
            # Run individual fallo tras 5 reintentos.  En lugar de
            # abortar todo el bench (lo que tira hasta 4 runs validos
            # por culpa de 1 flake), continuamos: aceptamos perder 1
            # run si los demas estan bien.  Solo si TODOS fallan
            # devolvemos lista vacia.
            continue
        if i >= warmup:
            times.append(ms)
    return times


# Compilers: cada uno devuelve (cmd_to_run, work_cwd, normalize_factor).
# normalize_factor: multiplicar wall time medido por este factor para
# obtener el equivalente "100% workload".  Util cuando Python reduce
# iteraciones para no tardar horas.

def compile_c(variant: BenchVariant, work_dir: Path,
              tc: Toolchain) -> Optional[tuple[list[str], Path, float]]:
    out = work_dir / f"{variant.bench_name}_c.exe"
    r = subprocess.run(
        [tc.path, "-O3", "-std=c11", "-march=native",
         str(variant.src_path), "-o", str(out), "-lm"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60.0,
    )
    if r.returncode != 0 or not out.is_file():
        return None
    return ([str(out)], work_dir, 1.0)


def compile_cpp(variant: BenchVariant, work_dir: Path,
                tc: Toolchain) -> Optional[tuple[list[str], Path, float]]:
    out = work_dir / f"{variant.bench_name}_cpp.exe"
    r = subprocess.run(
        [tc.path, "-O3", "-std=c++17", "-march=native",
         str(variant.src_path), "-o", str(out)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60.0,
    )
    if r.returncode != 0 or not out.is_file():
        return None
    return ([str(out)], work_dir, 1.0)


def compile_python(variant: BenchVariant, work_dir: Path,
                   tc: Toolchain) -> Optional[tuple[list[str], Path, float]]:
    # Python no requiere compilacion.  Factor 10 para memcpy_loop (que
    # reduce a 10 iter para no tardar horas).
    factor = 10.0 if variant.bench_name == "memcpy_loop" else 1.0
    return ([tc.path, str(variant.src_path)], variant.src_path.parent, factor)


def compile_java(variant: BenchVariant, work_dir: Path,
                 tc: Toolchain) -> Optional[tuple[list[str], Path, float]]:
    # Compilar Main.java -> Main.class en un dir dedicado por bench.
    bench_work = work_dir / f"{variant.bench_name}_java"
    bench_work.mkdir(exist_ok=True)
    r = subprocess.run(
        [tc.path_javac, "-d", str(bench_work), str(variant.src_path)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60.0,
    )
    if r.returncode != 0:
        return None
    return ([tc.path_java, "-cp", str(bench_work), "Main"], bench_work, 1.0)


def compile_vex(variant: BenchVariant, work_dir: Path,
                vm_bin: Path) -> Optional[tuple[list[str], Path, float]]:
    velb_stem = work_dir / f"{variant.bench_name}_vex"
    r = subprocess.run(
        [str(vm_bin), "--vex", str(variant.src_path), "-o", str(velb_stem)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60.0,
    )
    velb = Path(str(velb_stem) + ".velb")
    if r.returncode != 0 or not velb.is_file():
        return None
    return ([str(vm_bin), "--run", str(velb)], work_dir, 1.0)


# =============================================================================
# Reporte: tabla + grafica matplotlib
# =============================================================================

def colored_time(ms: float, color: str) -> str:
    if ms < 0:
        return f"{C.RED}TIMEOUT{C.RESET}"
    return f"{color}{ms:>10.1f}{C.RESET}"


def _stats_summary(runs: list[float]) -> dict:
    """min/p50/p95/max/stddev en ms para una lista de runs."""
    if not runs:
        return {}
    sorted_r = sorted(runs)
    n = len(sorted_r)
    p50 = statistics.median(sorted_r)
    # p95 con interpolacion linear simple.
    idx = 0.95 * (n - 1)
    lo = int(idx)
    hi = min(lo + 1, n - 1)
    frac = idx - lo
    p95 = sorted_r[lo] + (sorted_r[hi] - sorted_r[lo]) * frac
    return {
        "min": sorted_r[0],
        "p50": p50,
        "p95": p95,
        "max": sorted_r[-1],
        "stddev": (statistics.stdev(sorted_r) if n >= 2 else 0.0),
        "n": n,
    }


def print_verbose_stats_table(rows: list[dict], active_langs: list[str],
                                tc: dict[str, Toolchain]) -> None:
    """Tabla con min/p50/p95/max/stddev por (bench, lang).

    Sprint bench-stats: la mediana sola oculta la variancia.  Esta tabla
    expone los percentiles para identificar benches inestables (stddev
    alto -> ruido del SO, GC pauses, JIT recompile, etc.).
    """
    print()
    print(f"{C.BOLD}{'Stats detalladas (ms)':<22}"
          f"{'min':>10}{'p50':>10}{'p95':>10}{'max':>10}{'sdev':>9}{C.RESET}")
    print("-" * 90)
    for row in rows:
        runs_map = row.get("_runs", {})
        for ln in active_langs:
            rs = runs_map.get(ln)
            if not rs:
                continue
            s = _stats_summary(rs)
            if not s:
                continue
            label = f"  {row['bench']}/{ln}"
            color = tc[ln].color
            sdev_pct = (100.0 * s["stddev"] / s["p50"]) if s["p50"] > 0 else 0.0
            print(f"{color}{label:<22}{C.RESET}"
                  f"{s['min']:>10.1f}{s['p50']:>10.1f}{s['p95']:>10.1f}"
                  f"{s['max']:>10.1f}{sdev_pct:>8.1f}%")
    print("-" * 90)


def print_baseline_comparison(rows: list[dict], active_langs: list[str],
                                tc: dict[str, Toolchain],
                                baseline_path: str,
                                threshold_pct: float) -> None:
    """Compara cada (bench, lang) vs el JSON baseline.

    Reporta delta porcentual:
      - >= +threshold% -> regresion (rojo).
      - <= -threshold% -> mejora    (verde).
      - dentro del threshold -> neutro (gris).
    """
    print()
    try:
        with open(baseline_path, encoding="utf-8") as f:
            base_data = json.load(f)
    except Exception as e:  # noqa: BLE001
        warn(f"baseline no se pudo leer ({baseline_path}): {e}")
        return

    base_results = {r["bench"]: r for r in base_data.get("results", [])}
    print(f"{C.BOLD}Comparacion vs baseline "
          f"({baseline_path}){C.RESET}")
    print(f"{C.DIM}umbral regresion/mejora: +/-{threshold_pct:.1f}%{C.RESET}")
    print()

    header_cols = [f"{'BENCHMARK':<22}"]
    for ln in active_langs:
        header_cols.append(f"{tc[ln].label:>14}")
    print(f"{C.BOLD}{' '.join(header_cols)}{C.RESET}")
    print("-" * (22 + 15 * len(active_langs)))

    n_regress = 0
    n_improve = 0
    n_neutral = 0
    for row in rows:
        cols = [f"{row['bench']:<22}"]
        base_row = base_results.get(row["bench"], {})
        for ln in active_langs:
            cur = row.get(ln)
            prev = base_row.get(ln)
            if (cur is None or cur < 0 or prev is None
                    or (isinstance(prev, (int, float)) and prev < 0)):
                cols.append(f"{C.GREY}{'-':>14}{C.RESET}")
                continue
            delta = (cur - prev) * 100.0 / prev if prev > 0 else 0.0
            if delta >= threshold_pct:
                col = C.RED
                n_regress += 1
            elif delta <= -threshold_pct:
                col = C.GREEN
                n_improve += 1
            else:
                col = C.GREY
                n_neutral += 1
            # El formato +13.1f ya emite signo (+/-); no duplicar.
            cols.append(f"{col}{delta:>+13.1f}%{C.RESET}")
        print(" ".join(cols))
    print("-" * (22 + 15 * len(active_langs)))
    print(f"  {C.RED}regresiones (>=+{threshold_pct:.0f}%): "
          f"{n_regress}{C.RESET}   "
          f"{C.GREEN}mejoras (<=-{threshold_pct:.0f}%): "
          f"{n_improve}{C.RESET}   "
          f"{C.GREY}neutros: {n_neutral}{C.RESET}")


def print_results_table(rows: list[dict], tc: dict[str, Toolchain],
                         active_langs: list[str]) -> None:
    """Tabla: filas=benches, columnas=lenguajes con wall time ms."""
    # Cabecera.
    header_cols = [f"{'BENCHMARK':<22}"]
    for ln in active_langs:
        label = tc[ln].label
        header_cols.append(f"{label:>14}")
    header = " ".join(header_cols)
    print()
    print(f"{C.BOLD}{header}{C.RESET}")
    print("-" * len(header))

    for row in rows:
        cols = [f"{row['bench']:<22}"]
        # Determinar el ganador (menor tiempo > 0) para resaltar.
        valid_times = {k: row[k] for k in active_langs
                       if k in row and row[k] is not None and row[k] >= 0}
        min_time = min(valid_times.values()) if valid_times else None
        for ln in active_langs:
            t = row.get(ln)
            if t is None:
                cols.append(f"{C.GREY}{'N/A':>14}{C.RESET}")
            elif t < 0:
                cols.append(f"{C.RED}{'TIMEOUT':>14}{C.RESET}")
            else:
                color = C.BOLD + C.GREEN if t == min_time else tc[ln].color
                cols.append(f"{color}{t:>14.1f}{C.RESET}")
        print(" ".join(cols))
    print("-" * len(header))


def ascii_bar(value: float, max_value: float, width: int = 30,
              color: str = "") -> str:
    """Barra ASCII proporcional."""
    if max_value <= 0:
        return ""
    fill = int(width * value / max_value)
    fill = max(0, min(width, fill))
    return f"{color}{'#' * fill}{C.DIM}{'.' * (width - fill)}{C.RESET}"


def print_ascii_charts(rows: list[dict], active_langs: list[str],
                        tc: dict[str, Toolchain]) -> None:
    """Imprime los charts ASCII en consola: 3 visualizaciones rapidas
    sin necesidad de abrir el PNG."""
    print()
    print(f"{C.BOLD}=== ASCII charts (vista rapida en consola) =={C.RESET}")

    # Chart 1: wall time por bench (lenguaje mas rapido como baseline).
    print()
    print(f"{C.BOLD}(1) Wall time relativo por bench (lang mas rapido = 100%){C.RESET}")
    for r in rows:
        valid = [(ln, r[ln]) for ln in active_langs
                 if r.get(ln) is not None and r.get(ln, -1) > 0]
        if len(valid) < 2:
            continue
        valid.sort(key=lambda x: x[1])
        fastest_t = valid[0][1]
        max_t = valid[-1][1]
        print(f"\n  {C.BOLD}{r['bench']}{C.RESET}")
        for ln, v in valid:
            ratio = v / fastest_t
            bar = ascii_bar(v, max_t, width=30,
                            color=tc[ln].color)
            mark = f"{C.GREEN}<-- ganador{C.RESET}" if ratio == 1.0 else ""
            label = LANG_LABELS_PLAIN.get(ln, ln)
            print(f"    {label:<16} {bar} {v:>9.1f} ms  ({ratio:>5.2f}x) {mark}")

    # Chart 2: speedup Vex JIT vs Vex interp por bench.
    print()
    print(f"{C.BOLD}(2) Speedup Vex JIT vs Vex interp{C.RESET}")
    vex_data = []
    for r in rows:
        ti = r.get("vex_interp")
        tj = r.get("vex_jit")
        if ti and ti > 0 and tj and tj > 0:
            vex_data.append((r["bench"], ti / tj))
    if vex_data:
        vex_data.sort(key=lambda x: x[1], reverse=True)
        max_sp = vex_data[0][1]
        for name, sp in vex_data:
            color = (C.GREEN if sp >= 10 else
                     C.CYAN if sp >= 2 else
                     C.YELLOW if sp >= 1 else C.RED)
            bar = ascii_bar(sp, max_sp, width=35, color=color)
            print(f"  {name:<18} {bar} {color}{sp:>7.2f}x{C.RESET}")

    # Chart 3: geomean slowdown vs C por lenguaje.
    if "c" in active_langs:
        print()
        print(f"{C.BOLD}(3) Geomean slowdown vs C por lenguaje (mas bajo = mejor){C.RESET}")
        geomeans = []
        for ln in active_langs:
            if ln == "c":
                continue
            ratios = []
            for r in rows:
                tc_v = r.get("c")
                tl = r.get(ln)
                if tc_v and tc_v > 0 and tl and tl > 0:
                    ratios.append(tl / tc_v)
            if ratios:
                import math
                gm = math.exp(sum(math.log(x) for x in ratios) / len(ratios))
                geomeans.append((ln, gm))
        geomeans.sort(key=lambda x: x[1])
        if geomeans:
            max_gm = geomeans[-1][1]
            for ln, gm in geomeans:
                color = (C.GREEN if gm < 2 else
                         C.YELLOW if gm < 5 else
                         C.CYAN if gm < 20 else C.RED)
                bar = ascii_bar(gm, max_gm, width=35, color=color)
                label = LANG_LABELS_PLAIN.get(ln, ln)
                print(f"  {label:<18} {bar} {color}{gm:>7.2f}x{C.RESET}")


# Labels sin codigos ANSI para padding consistente.
LANG_LABELS_PLAIN = {
    "vex_interp": "Vex interp",
    "vex_jit":    "Vex JIT",
    "c":          "C (gcc -O3)",
    "cpp":        "C++ (g++ -O3)",
    "python":     "Python",
    "java":       "Java",
}


def print_speedup_summary(rows: list[dict], active_langs: list[str],
                           baseline: str = "c") -> None:
    """Compara cada lenguaje contra C (baseline = mas rapido tipicamente)."""
    if baseline not in active_langs:
        return
    print()
    print(f"{C.BOLD}Speedup vs {baseline.upper()} (mas alto = peor){C.RESET}")
    print(f"{C.DIM}(C es el baseline porque tipicamente es el mas rapido){C.RESET}")
    print()
    header = f"{'BENCH':<22}"
    for ln in active_langs:
        if ln == baseline:
            continue
        header += f"{ln+' vs '+baseline:>20}"
    print(f"{C.BOLD}{header}{C.RESET}")
    for row in rows:
        cols = [f"{row['bench']:<22}"]
        baseline_t = row.get(baseline)
        if baseline_t is None or baseline_t < 0 or baseline_t == 0:
            for ln in active_langs:
                if ln == baseline:
                    continue
                cols.append(f"{'-':>20}")
        else:
            for ln in active_langs:
                if ln == baseline:
                    continue
                t = row.get(ln)
                if t is None or t < 0:
                    cols.append(f"{'-':>20}")
                else:
                    ratio = t / baseline_t
                    if ratio < 2:
                        clr = C.GREEN
                    elif ratio < 5:
                        clr = C.YELLOW
                    elif ratio < 20:
                        clr = C.CYAN
                    else:
                        clr = C.RED
                    text = f"{ratio:>14.2f}x"
                    pad = 20 - len(text)
                    cols.append(f"{' '*pad}{clr}{text}{C.RESET}")
        print(" ".join(cols))


def save_matplotlib(rows: list[dict], active_langs: list[str],
                     tc: dict[str, Toolchain], out_path: Path) -> bool:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        return False

    # Filtrar benches que tienen al menos 2 lenguajes con datos validos.
    valid_rows = []
    for row in rows:
        cnt = sum(1 for ln in active_langs
                  if row.get(ln) is not None and row.get(ln, -1) >= 0)
        if cnt >= 2:
            valid_rows.append(row)
    if not valid_rows:
        return False

    # Map lenguaje -> color matplotlib.
    lang_color = {
        "vex_interp": "#ff7f0e",
        "vex_jit":    "#2ca02c",
        "c":          "#1f77b4",
        "cpp":        "#9467bd",
        "python":     "#17becf",
        "java":       "#d62728",
    }

    n_benches = len(valid_rows)
    n_langs = len(active_langs)
    bar_h = 0.8 / n_langs
    y_pos = np.arange(n_benches)

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(18, max(6, n_benches * 0.7)))

    # Panel 1: tiempo absoluto (log scale).
    for i, ln in enumerate(active_langs):
        times = [row.get(ln) for row in valid_rows]
        # Sustituir None/-1 por NaN para que no se grafiquen.
        plot_times = [t if (t is not None and t >= 0) else float("nan") for t in times]
        offset = (i - n_langs / 2 + 0.5) * bar_h
        ax1.barh(y_pos + offset, plot_times, bar_h,
                 label=tc[ln].label, color=lang_color.get(ln, "#888"))
    ax1.set_yticks(y_pos)
    ax1.set_yticklabels([r["bench"] for r in valid_rows], fontsize=10)
    ax1.set_xscale("log")
    ax1.set_xlabel("Wall time (ms, log scale)")
    ax1.set_title("Wall time absoluto por lenguaje")
    ax1.legend(loc="lower right", fontsize=9)
    ax1.grid(axis="x", which="both", alpha=0.3)

    # Panel 2: ratio vs C (cuanto mas lento es cada lenguaje vs C).
    baseline = "c"
    if baseline in active_langs:
        for i, ln in enumerate(active_langs):
            if ln == baseline:
                continue
            ratios = []
            for row in valid_rows:
                tb = row.get(baseline)
                tl = row.get(ln)
                if tb is None or tb <= 0 or tl is None or tl < 0:
                    ratios.append(float("nan"))
                else:
                    ratios.append(tl / tb)
            offset = (i - n_langs / 2 + 0.5) * bar_h
            ax2.barh(y_pos + offset, ratios, bar_h,
                     label=tc[ln].label, color=lang_color.get(ln, "#888"))
        ax2.axvline(x=1.0, color="grey", linestyle="--", alpha=0.5,
                    label="1x (paridad con C)")
        ax2.axvline(x=10.0, color="orange", linestyle=":", alpha=0.5,
                    label="10x mas lento")
        ax2.set_yticks(y_pos)
        ax2.set_yticklabels([r["bench"] for r in valid_rows], fontsize=10)
        ax2.set_xscale("log")
        ax2.set_xlabel("Ratio vs C (mas alto = mas lento)")
        ax2.set_title("Comparativa vs C (baseline)")
        ax2.legend(loc="lower right", fontsize=9)
        ax2.grid(axis="x", which="both", alpha=0.3)

    plt.tight_layout()
    plt.savefig(out_path, dpi=110, bbox_inches="tight")
    plt.close(fig)
    return True


# =============================================================================
# Sprint bench-html (2026-06-02): reporte HTML autocontenido
# =============================================================================

def _html_escape(s: str) -> str:
    """Escape minimo para HTML."""
    return (s.replace("&", "&amp;")
             .replace("<", "&lt;").replace(">", "&gt;")
             .replace('"', "&quot;"))


def generate_html_report(rows: list[dict], active_langs: list[str],
                          tc: dict[str, "Toolchain"],
                          sys_info: dict, plot_dir: Path,
                          json_path: Path,
                          baseline_path: str = "",
                          threshold_pct: float = 10.0) -> Path:
    """Genera @c bench_plots/index.html con sistema info + tabla de
    resultados + tabla de categorias + plots embebidos.

    El HTML referencia los PNG por ruta relativa, asi que abrir
    @c bench_plots/index.html en cualquier navegador funciona stand-alone.

    Si @c baseline_path apunta a un JSON valido, se incluye una tabla
    extra con deltas vs baseline coloreados.
    """
    import datetime
    import math
    out_path = plot_dir / "index.html"
    hw = sys_info.get("hardware", {})
    os_info = sys_info.get("os", {})
    tooling = sys_info.get("tooling", {})
    timestamp = sys_info.get("date") or datetime.datetime.now().strftime(
        "%Y-%m-%d %H:%M:%S")

    # CSS minimalista.
    css = """
    body { font-family: -apple-system,Segoe UI,Roboto,sans-serif; max-width:1280px;
           margin:24px auto; padding:0 20px; color:#1f2328; }
    h1 { border-bottom:2px solid #d0d7de; padding-bottom:8px; }
    h2 { margin-top:32px; border-bottom:1px solid #d0d7de; padding-bottom:4px; }
    dl.sysinfo { display:grid; grid-template-columns:160px 1fr; gap:4px 12px; }
    dl.sysinfo dt { font-weight:600; color:#57606a; }
    table { border-collapse:collapse; width:100%; margin:8px 0 24px;
            font-variant-numeric:tabular-nums; font-size:13px; }
    th,td { padding:6px 10px; border-bottom:1px solid #eaecef; text-align:right; }
    th:first-child,td:first-child { text-align:left; font-weight:500; }
    th { background:#f6f8fa; font-weight:600; }
    tr:hover { background:#f8f9fb; }
    .winner { font-weight:700; color:#1a7f37; }
    .timeout { color:#cf222e; }
    .na { color:#8c959f; }
    .cat-good { color:#1a7f37; font-weight:600; }
    .cat-mid { color:#bf8700; font-weight:600; }
    .cat-bad { color:#cf222e; font-weight:600; }
    .delta-regress { color:#cf222e; font-weight:600; }
    .delta-improve { color:#1a7f37; font-weight:600; }
    .delta-neutral { color:#8c959f; }
    img.plot { max-width:100%; height:auto; border:1px solid #d0d7de;
               border-radius:6px; margin:12px 0; }
    .meta { color:#6e7681; font-size:12px; margin-top:24px;
             padding-top:12px; border-top:1px solid #eaecef; }
    .grid-plots { display:grid; grid-template-columns:repeat(auto-fit,
                  minmax(400px,1fr)); gap:16px; }
    """

    html: list[str] = []
    html.append("<!doctype html>")
    html.append('<html lang="es"><head>')
    html.append('<meta charset="utf-8">')
    html.append("<title>VestaVM Benchmark Report</title>")
    html.append(f"<style>{css}</style>")
    html.append("</head><body>")
    html.append(f"<h1>VestaVM Benchmark Report</h1>")
    html.append(f'<p class="meta">Generado {_html_escape(timestamp)} '
                f'&middot; JSON: <code>{_html_escape(str(json_path))}</code></p>')

    # ---- System info ----
    html.append("<h2>Sistema</h2>")
    html.append('<dl class="sysinfo">')
    cpu_line = (f"{hw.get('cpu_model','?')} "
                f"({hw.get('cpu_physical','?')} fisicos / "
                f"{hw.get('cpu_logical','?')} logicos @ "
                f"{hw.get('cpu_freq_mhz','?')})")
    html.append(f"<dt>CPU</dt><dd>{_html_escape(cpu_line)}</dd>")
    html.append(f"<dt>RAM</dt><dd>{_html_escape(str(hw.get('ram_gb','?')))}</dd>")
    html.append(f"<dt>Arch</dt><dd>{_html_escape(str(hw.get('arch','?')))}</dd>")
    html.append(f"<dt>OS</dt><dd>{_html_escape(os_info.get('platform','?'))} "
                f"({_html_escape(os_info.get('version','?'))})</dd>")
    html.append("</dl>")

    if tooling:
        html.append("<h2>Toolchains</h2>")
        html.append('<dl class="sysinfo">')
        for name, t in tooling.items():
            if t.get("available"):
                ver = (t.get("version") or "").splitlines()
                ver_short = (ver[0] if ver else "?")[:80]
                html.append(f"<dt>{_html_escape(name)}</dt>"
                            f"<dd>{_html_escape(ver_short)}</dd>")
            else:
                html.append(f"<dt>{_html_escape(name)}</dt>"
                            f"<dd class='na'>no disponible</dd>")
        html.append("</dl>")

    # ---- Tabla principal de resultados ----
    html.append("<h2>Resultados (ms, mediana)</h2>")
    html.append("<table>")
    head = ["<th>Benchmark</th>"]
    for ln in active_langs:
        head.append(f"<th>{_html_escape(tc[ln].label)}</th>")
    html.append("<thead><tr>" + "".join(head) + "</tr></thead>")
    html.append("<tbody>")
    for row in rows:
        valid_times = {k: row[k] for k in active_langs
                       if k in row and row[k] is not None and row[k] >= 0}
        min_time = min(valid_times.values()) if valid_times else None
        cols = [f"<td>{_html_escape(row['bench'])}</td>"]
        for ln in active_langs:
            t = row.get(ln)
            if t is None:
                cols.append('<td class="na">N/A</td>')
            elif t < 0:
                cols.append('<td class="timeout">TIMEOUT</td>')
            else:
                cls = ' class="winner"' if t == min_time else ""
                cols.append(f'<td{cls}>{t:.1f}</td>')
        html.append("<tr>" + "".join(cols) + "</tr>")
    html.append("</tbody></table>")

    # ---- Tabla de categorias ----
    cats: dict[str, list[dict]] = {}
    for row in rows:
        cats.setdefault(bench_category(row["bench"]), []).append(row)
    if cats and "c" in active_langs:
        html.append("<h2>Geomean slowdown vs C por categoria</h2>")
        html.append('<p class="meta">Verde &lt;1.5x &middot; Amarillo '
                    "&lt;3x &middot; Rojo &ge;3x</p>")
        html.append("<table>")
        head = ["<th>Categoria</th><th>N</th>"]
        for ln in active_langs:
            if ln == "c":
                continue
            head.append(f"<th>{_html_escape(tc[ln].label)}</th>")
        html.append("<thead><tr>" + "".join(head) + "</tr></thead>")
        html.append("<tbody>")
        for cat in sorted(cats.keys()):
            rs = cats[cat]
            cols = [f"<td>{_html_escape(cat)}</td><td>{len(rs)}</td>"]
            for ln in active_langs:
                if ln == "c":
                    continue
                ratios = []
                for r in rs:
                    v_ln = r.get(ln); v_c = r.get("c")
                    if (v_ln is None or v_c is None
                            or v_ln <= 0 or v_c <= 0):
                        continue
                    ratios.append(v_ln / v_c)
                if not ratios:
                    cols.append('<td class="na">N/A</td>')
                    continue
                log_sum = sum(math.log(x) for x in ratios)
                geo = math.exp(log_sum / len(ratios))
                cls = ("cat-good" if geo < 1.5
                       else "cat-mid" if geo < 3.0
                       else "cat-bad")
                cols.append(f'<td class="{cls}">{geo:.2f}x</td>')
            html.append("<tr>" + "".join(cols) + "</tr>")
        html.append("</tbody></table>")

    # ---- Comparacion vs baseline ----
    if baseline_path:
        try:
            with open(baseline_path, encoding="utf-8") as f:
                base_data = json.load(f)
            base_map = {r["bench"]: r for r in base_data.get("results", [])}
            html.append(f"<h2>Comparacion vs baseline</h2>")
            html.append(f'<p class="meta">Baseline: '
                        f'<code>{_html_escape(baseline_path)}</code> '
                        f'&middot; Umbral: &plusmn;{threshold_pct:.1f}%</p>')
            html.append("<table>")
            head = ["<th>Benchmark</th>"]
            for ln in active_langs:
                head.append(f"<th>{_html_escape(tc[ln].label)}</th>")
            html.append("<thead><tr>" + "".join(head) + "</tr></thead>")
            html.append("<tbody>")
            n_r = n_i = n_n = 0
            for row in rows:
                base_row = base_map.get(row["bench"], {})
                cols = [f"<td>{_html_escape(row['bench'])}</td>"]
                for ln in active_langs:
                    cur = row.get(ln); prev = base_row.get(ln)
                    if (cur is None or cur < 0 or prev is None
                            or (isinstance(prev, (int, float)) and prev < 0)):
                        cols.append('<td class="na">-</td>')
                        continue
                    delta = (cur - prev) * 100.0 / prev if prev > 0 else 0.0
                    if delta >= threshold_pct:
                        cls = "delta-regress"; n_r += 1
                    elif delta <= -threshold_pct:
                        cls = "delta-improve"; n_i += 1
                    else:
                        cls = "delta-neutral"; n_n += 1
                    cols.append(f'<td class="{cls}">{delta:+.1f}%</td>')
                html.append("<tr>" + "".join(cols) + "</tr>")
            html.append("</tbody></table>")
            html.append(f'<p class="meta">'
                        f'<span class="delta-regress">regresiones: {n_r}</span> '
                        f'&middot; <span class="delta-improve">mejoras: {n_i}</span> '
                        f'&middot; <span class="delta-neutral">neutros: {n_n}</span>'
                        f'</p>')
        except Exception as e:  # noqa: BLE001
            html.append(f"<p>baseline error: {_html_escape(str(e))}</p>")

    # ---- Plots embebidos ----
    html.append("<h2>Graficas</h2>")
    # Listar PNG ordenados (los numericos primero por prefijo "NN_").
    pngs = sorted(plot_dir.glob("*.png"))
    if pngs:
        html.append('<div class="grid-plots">')
        for png in pngs:
            rel = png.name  # ya estan en el mismo dir.
            stem = png.stem
            html.append(f'<div><h3>{_html_escape(stem)}</h3>'
                        f'<img class="plot" src="{_html_escape(rel)}" '
                        f'alt="{_html_escape(stem)}"></div>')
        html.append("</div>")
        # per_bench/ tambien.
        per_bench_dir = plot_dir / "per_bench"
        if per_bench_dir.is_dir():
            pb_pngs = sorted(per_bench_dir.glob("*.png"))
            if pb_pngs:
                html.append("<h2>Per-bench</h2>")
                html.append('<div class="grid-plots">')
                for png in pb_pngs:
                    rel = "per_bench/" + png.name
                    html.append(f'<div><h3>{_html_escape(png.stem)}</h3>'
                                f'<img class="plot" src="{_html_escape(rel)}" '
                                f'alt="{_html_escape(png.stem)}"></div>')
                html.append("</div>")
    else:
        html.append('<p class="na">No hay PNG en este directorio.</p>')

    html.append("</body></html>")
    out_path.write_text("\n".join(html), encoding="utf-8")
    return out_path


# =============================================================================
# Sprint bench-from-json: re-render desde JSON sin re-medir
# =============================================================================

def rerender_from_json(json_path: Path, project_root: Path,
                        no_plot: bool, no_html: bool,
                        verbose_stats: bool,
                        baseline_path: str, threshold_pct: float) -> int:
    """Carga un bench_results.json previo y re-genera tabla consola +
    plots + HTML.  Util para tweakear el reporte sin re-medir."""
    print()
    info(f"re-renderizando desde JSON: {C.BOLD}{json_path}{C.RESET}")
    try:
        with open(json_path, encoding="utf-8") as f:
            data = json.load(f)
    except Exception as e:  # noqa: BLE001
        err(f"no pude leer JSON: {e}")
        return 1

    active_langs = data.get("active_langs", [])
    sys_info = data.get("system_info", {})

    # Reconstruir rows con shape esperado.
    rows: list[dict] = []
    for r in data.get("results", []):
        row = {k: v for k, v in r.items() if k != "runs_individual"}
        # Para reusar las funciones que usan _runs:
        row["_runs"] = r.get("runs_individual", {})
        rows.append(row)

    # Toolchains: solo necesitamos los labels.  Construir un mock minimal.
    class _MockTc:
        def __init__(self, name, label, color):
            self.name = name; self.label = label; self.color = color
            self.available = True
    LABELS = {
        "vex_interp": ("Vex interp",   C.YELLOW),
        "vex_jit":    ("Vex JIT",      C.GREEN),
        "c":          ("C (gcc -O3)",  C.BLUE),
        "cpp":        ("C++ (g++ -O3)", C.MAGENTA),
        "python":     ("Python (CPython)", C.CYAN),
        "java":       ("Java (HotSpot)", C.RED),
    }
    tc = {ln: _MockTc(ln, *LABELS.get(ln, (ln, C.RESET))) for ln in active_langs}

    # Banner.
    if sys_info:
        print_system_banner(sys_info)

    # Tabla principal.
    print_results_table(rows, tc, active_langs)
    print_speedup_summary(rows, active_langs, baseline="c")
    if "c" in active_langs:
        print_category_summary(rows, active_langs, tc, baseline="c")
    if verbose_stats:
        print_verbose_stats_table(rows, active_langs, tc)
    if baseline_path:
        print_baseline_comparison(rows, active_langs, tc,
                                   baseline_path, threshold_pct)

    # Plots.
    plot_dir = project_root / "bench_plots"
    if not no_plot:
        try:
            from . import plots
        except (ImportError, ValueError):
            sys.path.insert(0, str(Path(__file__).parent))
            import plots  # type: ignore
        try:
            results = plots.generate_all(rows, active_langs, plot_dir,
                                          sys_info=sys_info)
            print()
            info(f"plots: {plot_dir}")
            for name, success in results.items():
                marker = (f"{C.GREEN}OK{C.RESET}" if success
                          else f"{C.RED}FAIL{C.RESET}")
                print(f"  {marker}  {name}")
        except Exception as e:  # noqa: BLE001
            warn(f"plots error: {e}")

    # HTML.
    if not no_html:
        try:
            html_path = generate_html_report(
                rows, active_langs, tc, sys_info, plot_dir,
                json_path, baseline_path, threshold_pct)
            ok(f"HTML: {C.BOLD}{html_path}{C.RESET}")
        except Exception as e:  # noqa: BLE001
            warn(f"HTML error: {e}")

    return 0


# =============================================================================
# Main
# =============================================================================

def main() -> int:
    C.enable_windows_ansi()

    parser = argparse.ArgumentParser(
        description="Benchmark suite multi-lenguaje VestaVM",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("vm_path", nargs="?")
    parser.add_argument("--langs", type=str,
                        default="vex_interp,vex_jit,c,cpp,python,java",
                        help="lista CSV de lenguajes a ejecutar")
    parser.add_argument("--filter", type=str, default="",
                        help="Regex para filtrar benches por nombre")
    parser.add_argument("--runs", type=int, default=RUNS_PER_MODE)
    parser.add_argument("--timeout", type=float, default=BENCH_TIMEOUT)
    parser.add_argument("--no-plot", action="store_true")
    parser.add_argument("--out-json", type=str, default="bench_results.json")
    parser.add_argument("--out-plot", type=str, default="bench_results.png")
    parser.add_argument("--skip-legacy", action="store_true",
                        help="No correr benches legacy single-file .vex")
    parser.add_argument("--compile-jobs", type=int, default=0,
                        help=("Paralelismo de compilacion (default: os.cpu_count()).  "
                              "Las EJECUCIONES siguen secuenciales para no contaminar "
                              "las mediciones de tiempo."))
    parser.add_argument("--warmup", type=int, default=1,
                        help=("Runs de warmup descartados antes de medir (default 1).  "
                              "Cubre JIT-compile fresco de Vex/Java/Python.  "
                              "Total runs ejecutados por bench: warmup + runs."))
    parser.add_argument("--baseline", type=str, default="",
                        help=("Path a un bench_results.json previo para comparar.  "
                              "Imprime delta porcentual por bench y marca "
                              "regresiones/mejoras segun --threshold."))
    parser.add_argument("--threshold", type=float, default=10.0,
                        help=("Umbral porcentual para marcar regresion/mejora vs "
                              "baseline (default 10).  Solo afecta la coloracion."))
    parser.add_argument("--verbose-stats", action="store_true",
                        help=("Imprime min/p50/p95/max/stddev por bench (no solo "
                              "mediana).  Default false; activable para diagnostico."))
    parser.add_argument("--from-json", type=str, default="",
                        help=("Path a un bench_results.json previo.  Skip compile "
                              "+ run; solo re-genera tabla/plots/HTML desde los "
                              "datos guardados.  Util para re-renderizar tras "
                              "cambiar el codigo de reporte sin re-medir."))
    parser.add_argument("--no-html", action="store_true",
                        help="No generar bench_plots/index.html")
    # Sprint bench-fair (2026-06-03): measurement methodology.
    parser.add_argument("--fair", action="store_true", default=True,
                        help=("FAIR MODE (default): mide wall externo para "
                              "TODOS los lenguajes (incluye fork + runtime "
                              "init).  Es la metrica honesta -- 'cuanto tarda "
                              "el usuario al invocar el programa'.  Vex paga "
                              "sus ~30ms de VM init igual que Python/Java pagan "
                              "su startup.  Benches cortos quedan dominados "
                              "por init y se ve realmente el coste."))
    parser.add_argument("--unfair", dest="fair", action="store_false",
                        help=("UNFAIR MODE (legacy): mide wall externo para "
                              "C/C++/Python/Java pero wall INTERNO (--stats) "
                              "para Vex.  Sobreestima Vex en benches cortos "
                              "porque excluye sus ~30ms de VM init.  Util "
                              "solo para diagnostico del scheduler interno."))
    args = parser.parse_args()

    project_root = find_project_root(Path(__file__).parent)
    info(f"project root: {C.BOLD}{project_root}{C.RESET}")
    if args.fair:
        info(f"measurement: {C.BOLD}{C.CYAN}FAIR{C.RESET} (wall externo "
             f"para TODOS los lenguajes; incluye fork + runtime init).  "
             f"Vex paga sus ~30ms de VM init igual que Python ~15ms / Java ~80ms.")
    else:
        info(f"measurement: {C.BOLD}{C.YELLOW}UNFAIR (legacy){C.RESET} "
             f"-- Vex usa Wall time interno --stats (sin VM init); otros "
             f"langs usan wall externo.  Sobreestima Vex en benches cortos.")

    # Sprint bench-from-json: short-circuit del flujo normal cuando el
    # usuario solo quiere re-renderizar reportes desde un JSON previo.
    if args.from_json:
        json_path = Path(args.from_json)
        if not json_path.is_absolute():
            json_path = project_root / json_path
        if not json_path.is_file():
            err(f"from-json: archivo no existe: {json_path}")
            return 1
        return rerender_from_json(
            json_path, project_root,
            no_plot=args.no_plot, no_html=args.no_html,
            verbose_stats=args.verbose_stats,
            baseline_path=args.baseline,
            threshold_pct=args.threshold)

    # Resolver el binario vm.
    if args.vm_path:
        vm = Path(args.vm_path)
        if not vm.is_file():
            err(f"binario no existe: {vm}")
            return 1
    else:
        candidates = find_vm_candidates(project_root)
        vm = prompt_choose_vm(candidates)
    ok(f"usando vm: {C.BOLD}{vm}{C.RESET}")

    # Detectar toolchains.
    tc = detect_toolchains(vm)
    print()
    info(f"{C.BOLD}Toolchains detectados:{C.RESET}")
    for name, t in tc.items():
        if t.available:
            print(f"  {C.GREEN}OK{C.RESET}  {t.color}{t.label}{C.RESET}")
        else:
            print(f"  {C.RED}--{C.RESET}  {C.DIM}{t.label}{C.RESET} "
                  f"({t.why_unavailable})")

    # Capturar info del sistema (CPU/RAM/OS + versiones).
    print()
    with Spinner("capturando info del sistema...", color=C.CYAN):
        sys_info = capture_system_info(vm, tc)
    # Sprint bench-hwinfo: banner expandido con hardware + toolchains.
    print_system_banner(sys_info)

    # Filtrar lenguajes activos.
    requested = [ln.strip() for ln in args.langs.split(",")]
    active_langs = [ln for ln in requested if ln in tc and tc[ln].available]
    if not active_langs:
        err("ningun lenguaje disponible/solicitado")
        return 1

    # Descubrir benches.
    bench_dir = project_root / "examples_codes_vex" / "benchmark"
    benches = discover_benches(bench_dir)
    if args.skip_legacy:
        benches = [b for b in benches if not b.is_legacy]
    if args.filter:
        pat = re.compile(args.filter)
        benches = [b for b in benches if pat.search(b.name)]
    if not benches:
        err("ningun bench encontrado")
        return 1
    multi = sum(1 for b in benches if not b.is_legacy)
    legacy = sum(1 for b in benches if b.is_legacy)
    info(f"benches: {C.BOLD}{len(benches)}{C.RESET} "
         f"({multi} multi-lenguaje, {legacy} legacy single-vex)")

    work_dir = Path(os.environ.get("TEMP", "/tmp")) / "vex_bench_multi"
    work_dir.mkdir(parents=True, exist_ok=True)

    # -------------------------------------------------------------------
    # Sprint bench-parallel (2026-06-02): pre-compile en paralelo.
    #
    # Antes el flujo era: por cada (bench, lang) hacer compile + run.  Los
    # compiles son IO-heavy (subprocess de gcc/g++/javac/vm.exe) y se
    # serializaban -> dominaban el tiempo total cuando se cubren todos
    # los lenguajes.  Ahora ejecutamos TODOS los compiles en paralelo via
    # ThreadPoolExecutor (workers = --compile-jobs, default cpu_count()).
    #
    # Las EJECUCIONES (mediciones reales) siguen secuenciales por diseno:
    # paralelizarlas contaminaria las mediciones (CPU compartida entre
    # benches -> tiempo wallclock no representativo).
    # -------------------------------------------------------------------
    n_compile_jobs = args.compile_jobs if args.compile_jobs > 0 else (os.cpu_count() or 4)

    # Build worklist de (bench_idx, bench, lang, variant).
    compile_tasks: list[tuple[int, Bench, str, BenchVariant]] = []
    for idx, b in enumerate(benches, 1):
        for ln in active_langs:
            variant: Optional[BenchVariant] = None
            if ln in ("vex_interp", "vex_jit"):
                variant = b.variants.get("vex")
            else:
                variant = b.variants.get(ln)
            if variant is None:
                continue
            compile_tasks.append((idx, b, ln, variant))

    def compile_one(task):
        idx, b, ln, variant = task
        try:
            if ln in ("vex_interp", "vex_jit"):
                return (idx, b.name, ln, compile_vex(variant, work_dir, vm), None)
            elif ln == "c":
                return (idx, b.name, ln, compile_c(variant, work_dir, tc["c"]), None)
            elif ln == "cpp":
                return (idx, b.name, ln, compile_cpp(variant, work_dir, tc["cpp"]), None)
            elif ln == "python":
                return (idx, b.name, ln, compile_python(variant, work_dir, tc["python"]), None)
            elif ln == "java":
                return (idx, b.name, ln, compile_java(variant, work_dir, tc["java"]), None)
            else:
                return (idx, b.name, ln, None, "unknown lang")
        except Exception as e:  # noqa: BLE001
            return (idx, b.name, ln, None, str(e))

    print()
    info(f"compilando en paralelo: {C.BOLD}{len(compile_tasks)}{C.RESET} targets"
         f" ({C.BOLD}{n_compile_jobs}{C.RESET} workers)")
    compiled_map: dict[tuple[str, str], Optional[tuple]] = {}
    compile_errors: dict[tuple[str, str], str] = {}

    import concurrent.futures as _futures
    compile_t0 = time.perf_counter()
    with Spinner("compilando todo", color=C.MAGENTA):
        with _futures.ThreadPoolExecutor(max_workers=n_compile_jobs) as pool:
            futures = [pool.submit(compile_one, t) for t in compile_tasks]
            for fut in _futures.as_completed(futures):
                idx, bname, ln, compiled, err = fut.result()
                compiled_map[(bname, ln)] = compiled
                if err is not None:
                    compile_errors[(bname, ln)] = err
    compile_elapsed = time.perf_counter() - compile_t0
    info(f"compile total: {C.BOLD}{compile_elapsed:.2f}s{C.RESET}")
    if compile_errors:
        for (bname, ln), err in compile_errors.items():
            warn(f"compile fail {bname}/{ln}: {err}")

    # -------------------------------------------------------------------
    # Fase 2: EJECUCIONES (secuencial; mediciones no contaminadas).
    # -------------------------------------------------------------------
    print()
    rows: list[dict] = []
    started_at = time.perf_counter()

    for idx, b in enumerate(benches, 1):
        row: dict = {"bench": b.name, "is_legacy": b.is_legacy}

        for ln in active_langs:
            variant: Optional[BenchVariant] = None
            if ln in ("vex_interp", "vex_jit"):
                variant = b.variants.get("vex")
            else:
                variant = b.variants.get(ln)
            if variant is None:
                row[ln] = None
                continue

            compiled = compiled_map.get((b.name, ln))
            if compiled is None:
                row[ln] = -1.0
                continue
            cmd, cwd, factor = compiled

            # Env: JIT solo para vex_jit.
            env = os.environ.copy()
            if ln == "vex_jit":
                env["VESTA_JIT_THRESHOLD"] = "1"

            # Run + capturar TODOS los runs individuales (no solo mediana).
            warm = args.warmup
            label_run = (f"[{idx}/{len(benches)}] {b.name} | "
                         f"{tc[ln].label} run ({args.runs}x"
                         + (f"+{warm}W" if warm > 0 else "")
                         + ")")
            # Sprint bench-fair (2026-06-03): por default mode FAIR usa
            # wall externo para TODOS los lenguajes (incluye fork + runtime
            # init).  Mode --unfair restaura el comportamiento legacy
            # (Vex usa --stats interno, otros wall externo).
            use_vex_wt = (not args.fair) and ln in ("vex_interp", "vex_jit")
            with Spinner(label_run, color=tc[ln].color):
                runs_ms = all_runs(cmd, env=env, runs=args.runs,
                                    timeout=args.timeout, cwd=cwd,
                                    warmup=warm,
                                    use_vex_walltime=use_vex_wt)
            if not runs_ms:
                row[ln] = -1.0
                row.setdefault("_runs", {})[ln] = []
            else:
                # Normalizar (Python memcpy_loop reduce iters).
                normalized = [t * factor for t in runs_ms]
                row[ln] = statistics.median(normalized)
                row.setdefault("_runs", {})[ln] = normalized

        rows.append(row)

        # Imprimir fila parcial.
        line = f"  [{idx:>3}/{len(benches)}] {C.BOLD}{b.name:<22}{C.RESET}"
        for ln in active_langs:
            v = row.get(ln)
            if v is None:
                line += f"  {C.GREY}N/A{C.RESET}"
            elif v < 0:
                line += f"  {C.RED}FAIL{C.RESET}"
            else:
                line += f"  {tc[ln].color}{ln}={v:.0f}ms{C.RESET}"
        print(line)

    elapsed = time.perf_counter() - started_at
    print()
    info(f"tiempo total bench suite: {C.BOLD}{elapsed:.1f} s{C.RESET}")

    # Tabla completa.
    print_results_table(rows, tc, active_langs)
    print_speedup_summary(rows, active_langs, baseline="c")

    # Sprint bench-categories: geomean slowdown vs C por categoria.
    if "c" in active_langs:
        print_category_summary(rows, active_langs, tc, baseline="c")

    # Sprint bench-stats (2026-06-02): tabla verbose con min/p50/p95/max/stddev.
    if args.verbose_stats:
        print_verbose_stats_table(rows, active_langs, tc)

    # Sprint bench-baseline (2026-06-02): comparacion vs JSON previo.
    if args.baseline:
        print_baseline_comparison(rows, active_langs, tc,
                                   args.baseline, args.threshold)

    # ASCII charts en consola para vista rapida.
    print_ascii_charts(rows, active_langs, tc)

    # JSON con runs individuales para post-analisis.
    json_path = (project_root / args.out_json
                 if not os.path.isabs(args.out_json) else Path(args.out_json))
    serializable_rows = []
    for r in rows:
        new_r = {k: v for k, v in r.items() if not k.startswith("_")}
        new_r["runs_individual"] = r.get("_runs", {})
        # Sprint bench-categories: tag de categoria por bench.
        new_r["category"] = bench_category(r["bench"])
        # Sprint bench-stats: incluir summary por lang en el JSON para
        # que herramientas externas no tengan que reprocesar los runs.
        stats_per_lang = {}
        for ln, rs in r.get("_runs", {}).items():
            if rs:
                stats_per_lang[ln] = _stats_summary(rs)
        new_r["stats"] = stats_per_lang
        serializable_rows.append(new_r)
    json_data = {
        "vm_binary": str(vm),
        "project_root": str(project_root),
        "runs_per_mode": args.runs,
        "warmup_runs": args.warmup,
        "active_langs": active_langs,
        "elapsed_total_s": elapsed,
        "system_info": sys_info,
        "results": serializable_rows,
    }
    try:
        json_path.write_text(json.dumps(json_data, indent=2))
        print()
        ok(f"JSON: {C.BOLD}{json_path}{C.RESET}")
    except Exception as e:
        warn(f"no pude escribir JSON: {e}")

    # Matplotlib: generar TODAS las graficas en un directorio.
    if not args.no_plot:
        plot_dir = project_root / "bench_plots"
        try:
            from . import plots  # cuando se invoca como modulo
        except (ImportError, ValueError):
            # Fallback: import directo cuando se corre como script.
            sys.path.insert(0, str(Path(__file__).parent))
            import plots  # type: ignore
        try:
            results = plots.generate_all(rows, active_langs, plot_dir,
                                          sys_info=sys_info)
            print()
            info(f"{C.BOLD}Graficas generadas en {plot_dir}/:{C.RESET}")
            for name, success in results.items():
                marker = (f"{C.GREEN}OK{C.RESET}" if success
                          else f"{C.RED}FAIL{C.RESET}")
                print(f"  {marker}  {name}")
        except Exception as e:
            warn(f"error generando plots: {e}")

    # Sprint bench-html: reporte HTML autocontenido con todo el contexto.
    if not args.no_html and not args.no_plot:
        plot_dir = project_root / "bench_plots"
        try:
            html_path = generate_html_report(
                rows, active_langs, tc, sys_info, plot_dir,
                json_path, args.baseline, args.threshold)
            print()
            ok(f"HTML: {C.BOLD}{html_path}{C.RESET}")
        except Exception as e:  # noqa: BLE001
            warn(f"HTML error: {e}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
