"""plots.py -- generador de visualizaciones para el benchmark suite.

Cada `plot_*` function genera UNA grafica enfocada en una pregunta
especifica.  Todas dependen de matplotlib + numpy.  Si el caller no
tiene matplotlib, las funciones retornan @c False y el caller cae al
output de texto.

Filosofia: una grafica = una pregunta.  Mejor 8 imagenes claras que 1
imagen sobrecargada.
"""
from __future__ import annotations
import math
from pathlib import Path
from typing import Optional

# Colores fijos por lenguaje para consistencia visual cross-plot.
LANG_COLORS = {
    "vex_interp": "#ff7f0e",   # naranja
    "vex_jit":    "#2ca02c",   # verde
    "c":          "#1f77b4",   # azul
    "cpp":        "#9467bd",   # violeta
    "python":     "#17becf",   # cyan
    "java":       "#d62728",   # rojo
}

# Labels formateados.
LANG_LABELS = {
    "vex_interp": "Vex interp",
    "vex_jit":    "Vex JIT",
    "c":          "C (gcc -O3)",
    "cpp":        "C++ (g++ -O3)",
    "python":     "Python",
    "java":       "Java",
}


def _try_import():
    """Intenta importar matplotlib + numpy.  Returns (mpl, plt, np) o None."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
        return matplotlib, plt, np
    except ImportError:
        return None


def _filter_valid(rows: list[dict], lang: str) -> tuple[list[str], list[float]]:
    """Extrae (nombres, valores) validos (>0) para un lenguaje."""
    names, vals = [], []
    for r in rows:
        v = r.get(lang)
        if v is not None and v > 0:
            names.append(r["bench"])
            vals.append(v)
    return names, vals


# =============================================================================
# Plot 1: dashboard general (1 PNG con 4 paneles resumiendo todo)
# =============================================================================

def plot_dashboard(rows: list[dict], langs: list[str], out_path: Path) -> bool:
    deps = _try_import()
    if not deps:
        return False
    _, plt, np = deps

    fig = plt.figure(figsize=(18, 12))
    gs = fig.add_gridspec(2, 2, hspace=0.32, wspace=0.25)

    # 1.1 Wall time absoluto por bench (log scale).
    ax1 = fig.add_subplot(gs[0, 0])
    _plot_wall_time(ax1, rows, langs)

    # 1.2 Ratio vs C.
    ax2 = fig.add_subplot(gs[0, 1])
    _plot_ratio_vs_c(ax2, rows, langs)

    # 1.3 Ranking (orden ordinal por bench).
    ax3 = fig.add_subplot(gs[1, 0])
    _plot_ranking_heatmap(ax3, rows, langs)

    # 1.4 Speedup Vex JIT vs interp.
    ax4 = fig.add_subplot(gs[1, 1])
    _plot_vex_speedup(ax4, rows)

    fig.suptitle("VestaVM benchmark suite -- dashboard",
                 fontsize=15, fontweight="bold", y=0.995)
    plt.savefig(out_path, dpi=110, bbox_inches="tight")
    plt.close(fig)
    return True


def _plot_wall_time(ax, rows, langs):
    n_benches = len(rows)
    n_langs = len(langs)
    bar_h = 0.8 / max(1, n_langs)
    y_pos = list(range(n_benches))
    for i, ln in enumerate(langs):
        times = []
        for r in rows:
            v = r.get(ln)
            times.append(v if (v is not None and v > 0) else float("nan"))
        offset = (i - n_langs / 2 + 0.5) * bar_h
        ax.barh([y + offset for y in y_pos], times, bar_h,
                label=LANG_LABELS.get(ln, ln),
                color=LANG_COLORS.get(ln, "#888"))
    ax.set_yticks(y_pos)
    ax.set_yticklabels([r["bench"] for r in rows], fontsize=9)
    ax.set_xscale("log")
    ax.set_xlabel("Wall time (ms, log scale)")
    ax.set_title("(a) Wall time absoluto por lenguaje")
    ax.legend(loc="lower right", fontsize=8)
    ax.grid(axis="x", which="both", alpha=0.3)


def _plot_ratio_vs_c(ax, rows, langs):
    """Barras: ratio de cada lenguaje vs C (mas alto = mas lento)."""
    if "c" not in langs:
        ax.text(0.5, 0.5, "C no disponible como baseline",
                ha="center", va="center", transform=ax.transAxes)
        return
    n_benches = len(rows)
    other_langs = [ln for ln in langs if ln != "c"]
    bar_h = 0.8 / max(1, len(other_langs))
    y_pos = list(range(n_benches))
    for i, ln in enumerate(other_langs):
        ratios = []
        for r in rows:
            t_c = r.get("c")
            t_l = r.get(ln)
            if t_c is None or t_c <= 0 or t_l is None or t_l <= 0:
                ratios.append(float("nan"))
            else:
                ratios.append(t_l / t_c)
        offset = (i - len(other_langs) / 2 + 0.5) * bar_h
        ax.barh([y + offset for y in y_pos], ratios, bar_h,
                label=LANG_LABELS.get(ln, ln),
                color=LANG_COLORS.get(ln, "#888"))
    ax.axvline(x=1.0, color="grey", linestyle="--", alpha=0.5,
               label="1x (paridad con C)")
    ax.axvline(x=10.0, color="orange", linestyle=":", alpha=0.5,
               label="10x mas lento")
    ax.set_yticks(y_pos)
    ax.set_yticklabels([r["bench"] for r in rows], fontsize=9)
    ax.set_xscale("log")
    ax.set_xlabel("Ratio vs C (mas alto = mas lento)")
    ax.set_title("(b) Comparativa vs C nativo")
    ax.legend(loc="lower right", fontsize=8)
    ax.grid(axis="x", which="both", alpha=0.3)


def _plot_ranking_heatmap(ax, rows, langs):
    """Heatmap de ranking ordinal (1=ganador, N=perdedor) por bench."""
    deps = _try_import()
    if not deps:
        return
    _, plt, np = deps
    n_benches = len(rows)
    n_langs = len(langs)
    matrix = np.full((n_benches, n_langs), np.nan)
    for i, r in enumerate(rows):
        vals_with_idx = []
        for j, ln in enumerate(langs):
            v = r.get(ln)
            if v is not None and v > 0:
                vals_with_idx.append((v, j))
        vals_with_idx.sort()
        for rank, (_, j) in enumerate(vals_with_idx, 1):
            matrix[i, j] = rank
    im = ax.imshow(matrix, cmap="RdYlGn_r", aspect="auto", vmin=1, vmax=n_langs)
    ax.set_xticks(range(n_langs))
    ax.set_xticklabels([LANG_LABELS.get(ln, ln) for ln in langs],
                       rotation=30, ha="right", fontsize=9)
    ax.set_yticks(range(n_benches))
    ax.set_yticklabels([r["bench"] for r in rows], fontsize=9)
    ax.set_title("(c) Ranking por bench (1 = mas rapido)")
    # Anotaciones con el rank.
    for i in range(n_benches):
        for j in range(n_langs):
            if not math.isnan(matrix[i, j]):
                txt = f"{int(matrix[i, j])}"
                ax.text(j, i, txt, ha="center", va="center",
                        fontsize=10, fontweight="bold",
                        color="white" if matrix[i, j] >= n_langs - 1 else "black")
    plt.colorbar(im, ax=ax, label="Ranking (1=mejor)", fraction=0.04)


def _plot_vex_speedup(ax, rows):
    """Barras del speedup Vex JIT vs Vex interp para cada bench."""
    names, speedups = [], []
    for r in rows:
        ti = r.get("vex_interp")
        tj = r.get("vex_jit")
        if ti and ti > 0 and tj and tj > 0:
            names.append(r["bench"])
            speedups.append(ti / tj)
    y_pos = list(range(len(names)))
    colors = ["#2ca02c" if s >= 10 else
              "#1f77b4" if s >= 2 else
              "#ff7f0e" if s >= 1 else "#d62728" for s in speedups]
    ax.barh(y_pos, speedups, color=colors)
    ax.set_yticks(y_pos)
    ax.set_yticklabels(names, fontsize=9)
    ax.set_xlabel("Speedup (interp_ms / JIT_ms)")
    ax.set_title("(d) Vex JIT vs Vex interp")
    ax.axvline(x=1.0, color="grey", linestyle="--", alpha=0.5)
    ax.axvline(x=10.0, color="green", linestyle=":", alpha=0.5)
    for i, sp in enumerate(speedups):
        ax.text(sp, i, f" {sp:.1f}x", va="center", fontsize=8)
    ax.grid(axis="x", alpha=0.3)


# =============================================================================
# Plot 2: heatmap puro (matriz bench x lang con log wall time)
# =============================================================================

def plot_heatmap(rows: list[dict], langs: list[str], out_path: Path) -> bool:
    deps = _try_import()
    if not deps:
        return False
    _, plt, np = deps
    n_benches = len(rows)
    n_langs = len(langs)
    # Matriz log10(ms).
    matrix = np.full((n_benches, n_langs), np.nan)
    for i, r in enumerate(rows):
        for j, ln in enumerate(langs):
            v = r.get(ln)
            if v is not None and v > 0:
                matrix[i, j] = math.log10(v)

    fig, ax = plt.subplots(figsize=(11, max(5, n_benches * 0.5)))
    im = ax.imshow(matrix, cmap="RdYlGn_r", aspect="auto")
    ax.set_xticks(range(n_langs))
    ax.set_xticklabels([LANG_LABELS.get(ln, ln) for ln in langs],
                       rotation=30, ha="right", fontsize=10)
    ax.set_yticks(range(n_benches))
    ax.set_yticklabels([r["bench"] for r in rows], fontsize=10)
    ax.set_title("Heatmap: wall time (ms, log10).  Verde = rapido, rojo = lento")
    # Annotations.
    for i in range(n_benches):
        for j in range(n_langs):
            if not math.isnan(matrix[i, j]):
                ms = 10 ** matrix[i, j]
                txt = f"{ms:.0f}" if ms >= 10 else f"{ms:.1f}"
                ax.text(j, i, f"{txt}ms", ha="center", va="center",
                        fontsize=9, color="black")
    cb = plt.colorbar(im, ax=ax, label="log10(wall time ms)")
    plt.tight_layout()
    plt.savefig(out_path, dpi=110, bbox_inches="tight")
    plt.close(fig)
    return True


# =============================================================================
# Plot 3: radar / spider chart por lenguaje
# =============================================================================

def plot_radar(rows: list[dict], langs: list[str], out_path: Path) -> bool:
    """Radar chart: cada lenguaje tiene un poligono que muestra su 'perfil'
    a traves de los benches.  Eje = bench, distancia al centro = log de
    wall time invertido (mas grande el area = mejor).
    """
    deps = _try_import()
    if not deps:
        return False
    _, plt, np = deps
    n_benches = len(rows)
    if n_benches < 3:
        return False
    # Cerrar el poligono con el primer punto al final.
    angles = np.linspace(0, 2 * np.pi, n_benches, endpoint=False).tolist()
    angles += [angles[0]]
    bench_names = [r["bench"] for r in rows]

    fig, ax = plt.subplots(figsize=(12, 11),
                            subplot_kw={"projection": "polar"})
    # Para cada lenguaje: 1/wall_time normalizado a [0, 1].
    max_inv = 0.0
    inv_data = {}
    for ln in langs:
        invs = []
        for r in rows:
            v = r.get(ln)
            invs.append(1.0 / v if (v is not None and v > 0) else 0.0)
        inv_data[ln] = invs
        max_inv = max(max_inv, max(invs) if invs else 0.0)
    if max_inv <= 0:
        return False
    for ln in langs:
        invs = inv_data[ln]
        norm = [x / max_inv for x in invs]
        norm += [norm[0]]
        ax.plot(angles, norm, "o-", linewidth=2,
                label=LANG_LABELS.get(ln, ln),
                color=LANG_COLORS.get(ln, "#888"))
        ax.fill(angles, norm, alpha=0.10,
                color=LANG_COLORS.get(ln, "#888"))
    ax.set_xticks(angles[:-1])
    ax.set_xticklabels(bench_names, fontsize=10)
    ax.set_yticks([0.2, 0.4, 0.6, 0.8, 1.0])
    ax.set_yticklabels(["20%", "40%", "60%", "80%", "100%"], fontsize=8)
    ax.set_ylim(0, 1.05)
    ax.set_title("Perfil de rendimiento por lenguaje\n"
                 "(area mayor = mejor; normalizado al mas rapido)",
                 fontsize=12, pad=20)
    ax.legend(loc="upper right", bbox_to_anchor=(1.3, 1.1), fontsize=9)
    plt.tight_layout()
    plt.savefig(out_path, dpi=110, bbox_inches="tight")
    plt.close(fig)
    return True


# =============================================================================
# Plot 4: box plot de runs individuales por bench
# =============================================================================

def plot_boxplot_variability(rows: list[dict], langs: list[str],
                              out_path: Path) -> bool:
    """Para cada bench, muestra los runs individuales como box plot.
    Permite ver variabilidad / outliers entre corridas."""
    deps = _try_import()
    if not deps:
        return False
    _, plt, np = deps

    # Filtrar benches con runs disponibles.
    valid_benches = [r for r in rows if r.get("_runs")]
    if not valid_benches:
        return False
    n = len(valid_benches)
    cols = min(3, n)
    rows_n = (n + cols - 1) // cols

    fig, axes = plt.subplots(rows_n, cols, figsize=(6 * cols, 4.5 * rows_n),
                              squeeze=False)
    for k, r in enumerate(valid_benches):
        ax = axes[k // cols][k % cols]
        data = []
        labels = []
        colors = []
        for ln in langs:
            runs = r.get("_runs", {}).get(ln, [])
            if runs:
                data.append(runs)
                labels.append(LANG_LABELS.get(ln, ln))
                colors.append(LANG_COLORS.get(ln, "#888"))
        if not data:
            ax.set_visible(False)
            continue
        bp = ax.boxplot(data, labels=labels, patch_artist=True,
                         widths=0.6, showmeans=True)
        for patch, c in zip(bp["boxes"], colors):
            patch.set_facecolor(c)
            patch.set_alpha(0.6)
        ax.set_yscale("log")
        ax.set_title(r["bench"], fontweight="bold")
        ax.set_ylabel("Wall time (ms, log)")
        ax.tick_params(axis="x", rotation=30, labelsize=8)
        ax.grid(axis="y", which="both", alpha=0.3)
    # Ocultar subplots sobrantes.
    for k in range(n, rows_n * cols):
        axes[k // cols][k % cols].set_visible(False)

    fig.suptitle("Variabilidad de runs individuales por bench\n"
                 "(box = IQR, linea = mediana, triangulo = media, "
                 "puntos = outliers)",
                 fontsize=13, y=1.005)
    plt.tight_layout()
    plt.savefig(out_path, dpi=110, bbox_inches="tight")
    plt.close(fig)
    return True


# =============================================================================
# Plot 5: per-bench (un PNG por bench con detalle individual)
# =============================================================================

def plot_per_bench(rows: list[dict], langs: list[str],
                    out_dir: Path) -> int:
    """Genera 1 PNG por bench con barras horizontales + valores.
    Retorna numero de plots generados."""
    deps = _try_import()
    if not deps:
        return 0
    _, plt, np = deps
    out_dir.mkdir(parents=True, exist_ok=True)
    count = 0
    for r in rows:
        valid = [(ln, r.get(ln)) for ln in langs
                 if r.get(ln) is not None and r.get(ln, -1) > 0]
        if len(valid) < 2:
            continue
        valid.sort(key=lambda x: x[1])
        names = [LANG_LABELS.get(ln, ln) for ln, _ in valid]
        vals = [v for _, v in valid]
        colors = [LANG_COLORS.get(ln, "#888") for ln, _ in valid]
        fastest = vals[0]
        ratios = [v / fastest for v in vals]

        fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, max(3, len(valid) * 0.55)))
        # Panel 1: tiempo absoluto.
        bars = ax1.barh(names, vals, color=colors, alpha=0.85)
        ax1.set_xscale("log")
        ax1.set_xlabel("Wall time (ms, log)")
        ax1.set_title(f"{r['bench']} -- tiempo absoluto")
        for bar, v in zip(bars, vals):
            ax1.text(v, bar.get_y() + bar.get_height() / 2,
                     f" {v:.1f} ms", va="center", fontsize=9)
        ax1.grid(axis="x", which="both", alpha=0.3)

        # Panel 2: ratio vs el mas rapido (slowdown).
        bars2 = ax2.barh(names, ratios, color=colors, alpha=0.85)
        ax2.set_xlabel(f"Slowdown vs {names[0]} (1x = mas rapido)")
        ax2.set_title(f"{r['bench']} -- relativo")
        ax2.axvline(x=1.0, color="grey", linestyle="--", alpha=0.5)
        for bar, ratio in zip(bars2, ratios):
            ax2.text(ratio, bar.get_y() + bar.get_height() / 2,
                     f" {ratio:.2f}x", va="center", fontsize=9)
        ax2.grid(axis="x", alpha=0.3)

        plt.tight_layout()
        plt.savefig(out_dir / f"{r['bench']}.png", dpi=100,
                    bbox_inches="tight")
        plt.close(fig)
        count += 1
    return count


# =============================================================================
# Plot 6: scatter ratio vs C en eje X, langs en Y (dispersion)
# =============================================================================

def plot_scatter_ratio(rows: list[dict], langs: list[str],
                        out_path: Path) -> bool:
    """Scatter: cada punto es (ratio_vs_C, lang) -- ver dispersion entre
    benches por lenguaje."""
    deps = _try_import()
    if not deps:
        return False
    _, plt, np = deps
    if "c" not in langs:
        return False
    other_langs = [ln for ln in langs if ln != "c"]
    fig, ax = plt.subplots(figsize=(14, 7))
    for i, ln in enumerate(other_langs):
        ratios = []
        names_ok = []
        for r in rows:
            tc = r.get("c")
            tl = r.get(ln)
            if tc and tc > 0 and tl and tl > 0:
                ratios.append(tl / tc)
                names_ok.append(r["bench"])
        if not ratios:
            continue
        y = [i] * len(ratios)
        ax.scatter(ratios, y, s=120,
                   color=LANG_COLORS.get(ln, "#888"),
                   alpha=0.75, edgecolors="black", linewidth=0.5,
                   label=LANG_LABELS.get(ln, ln))
        # Anotaciones por punto.
        for r_val, name in zip(ratios, names_ok):
            ax.annotate(name, (r_val, i),
                         xytext=(0, 8), textcoords="offset points",
                         fontsize=7, ha="center", alpha=0.8)
    ax.axvline(x=1.0, color="grey", linestyle="--", alpha=0.5,
                label="1x (paridad C)")
    ax.axvline(x=10.0, color="orange", linestyle=":", alpha=0.5,
                label="10x")
    ax.set_xscale("log")
    ax.set_yticks(range(len(other_langs)))
    ax.set_yticklabels([LANG_LABELS.get(ln, ln) for ln in other_langs])
    ax.set_xlabel("Ratio vs C (log scale)")
    ax.set_title("Dispersion de slowdown vs C por bench y lenguaje\n"
                 "Cada punto es un bench.  Cuanto mas a la izq = mejor.")
    ax.legend(loc="lower right", fontsize=9)
    ax.grid(axis="x", which="both", alpha=0.3)
    plt.tight_layout()
    plt.savefig(out_path, dpi=110, bbox_inches="tight")
    plt.close(fig)
    return True


# =============================================================================
# Plot 7: line chart de ranking (mostrar el orden de cada lang por bench)
# =============================================================================

def plot_ranking_lines(rows: list[dict], langs: list[str],
                        out_path: Path) -> bool:
    """Line chart con rank ordinal de cada lang en cada bench.  Permite
    ver consistencia de cada lang (lineas mas planas = mas consistente)."""
    deps = _try_import()
    if not deps:
        return False
    _, plt, np = deps
    n_benches = len(rows)
    if n_benches < 2:
        return False
    fig, ax = plt.subplots(figsize=(14, 7))
    # Para cada bench, calcular ranks.
    rank_data = {ln: [] for ln in langs}
    for r in rows:
        items = [(ln, r.get(ln)) for ln in langs
                 if r.get(ln) is not None and r.get(ln, -1) > 0]
        items.sort(key=lambda x: x[1])
        ranks = {ln: i + 1 for i, (ln, _) in enumerate(items)}
        for ln in langs:
            rank_data[ln].append(ranks.get(ln))
    x = list(range(n_benches))
    for ln in langs:
        ys = rank_data[ln]
        # NaN para faltantes para que el plot tenga gaps.
        ys_plot = [y if y is not None else float("nan") for y in ys]
        ax.plot(x, ys_plot, "o-", linewidth=2.5, markersize=10,
                label=LANG_LABELS.get(ln, ln),
                color=LANG_COLORS.get(ln, "#888"))
    ax.set_xticks(x)
    ax.set_xticklabels([r["bench"] for r in rows],
                       rotation=30, ha="right", fontsize=9)
    ax.set_yticks(range(1, len(langs) + 1))
    ax.set_ylabel("Ranking (1 = mas rapido)")
    ax.set_title("Ranking por bench: que lenguaje gana donde\n"
                 "(lineas mas planas = mas consistente)")
    ax.invert_yaxis()  # 1 arriba
    ax.legend(loc="lower right", fontsize=9, ncol=2)
    ax.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(out_path, dpi=110, bbox_inches="tight")
    plt.close(fig)
    return True


# =============================================================================
# Plot 8: barras agrupadas (ratio vs C, una grafica por bench)
# =============================================================================

def plot_grouped_ratio(rows: list[dict], langs: list[str],
                        out_path: Path) -> bool:
    """Bar chart agrupado: x=bench, grupos de barras = lenguajes,
    altura = ratio vs C."""
    deps = _try_import()
    if not deps:
        return False
    _, plt, np = deps
    if "c" not in langs:
        return False
    other_langs = [ln for ln in langs if ln != "c"]
    n_benches = len(rows)
    n_other = len(other_langs)
    bar_w = 0.8 / max(1, n_other)
    x = np.arange(n_benches)
    fig, ax = plt.subplots(figsize=(max(12, n_benches * 1.5), 7))
    for i, ln in enumerate(other_langs):
        ratios = []
        for r in rows:
            tc = r.get("c")
            tl = r.get(ln)
            if tc and tc > 0 and tl and tl > 0:
                ratios.append(tl / tc)
            else:
                ratios.append(float("nan"))
        offset = (i - n_other / 2 + 0.5) * bar_w
        bars = ax.bar(x + offset, ratios, bar_w,
                       label=LANG_LABELS.get(ln, ln),
                       color=LANG_COLORS.get(ln, "#888"),
                       alpha=0.85)
        # Anotaciones.
        for bar, ratio in zip(bars, ratios):
            if not math.isnan(ratio):
                ax.text(bar.get_x() + bar.get_width() / 2,
                        ratio * 1.05, f"{ratio:.1f}",
                        ha="center", fontsize=7, rotation=0)
    ax.axhline(y=1.0, color="grey", linestyle="--", alpha=0.5,
                label="1x = paridad C")
    ax.set_yscale("log")
    ax.set_xticks(x)
    ax.set_xticklabels([r["bench"] for r in rows],
                       rotation=30, ha="right")
    ax.set_ylabel("Ratio vs C (log)")
    ax.set_title("Slowdown vs C por bench (cada grupo = un bench)")
    ax.legend(loc="upper left", fontsize=9, ncol=2)
    ax.grid(axis="y", which="both", alpha=0.3)
    plt.tight_layout()
    plt.savefig(out_path, dpi=110, bbox_inches="tight")
    plt.close(fig)
    return True


# =============================================================================
# Plot 9: cumulative geometric mean -- "geomean slowdown" per lang
# =============================================================================

def plot_geomean_summary(rows: list[dict], langs: list[str],
                          out_path: Path) -> bool:
    """Bar chart simple con la media geometrica del ratio vs C de cada
    lenguaje a traves de todos los benches.  La media geometrica es
    la metrica correcta para ratios (no la aritmetica).
    """
    deps = _try_import()
    if not deps:
        return False
    _, plt, np = deps
    if "c" not in langs:
        return False
    other_langs = [ln for ln in langs if ln != "c"]
    geomeans = []
    for ln in other_langs:
        ratios = []
        for r in rows:
            tc = r.get("c")
            tl = r.get(ln)
            if tc and tc > 0 and tl and tl > 0:
                ratios.append(tl / tc)
        if not ratios:
            geomeans.append(float("nan"))
        else:
            # Media geometrica = exp(mean(log(x)))
            geomeans.append(math.exp(sum(math.log(r) for r in ratios)
                                       / len(ratios)))
    fig, ax = plt.subplots(figsize=(12, 6))
    # Ordenar por geomean (mejor primero).
    pairs = sorted(zip(other_langs, geomeans),
                    key=lambda p: p[1] if not math.isnan(p[1]) else float("inf"))
    labels = [LANG_LABELS.get(p[0], p[0]) for p in pairs]
    vals = [p[1] for p in pairs]
    colors = [LANG_COLORS.get(p[0], "#888") for p in pairs]
    bars = ax.bar(labels, vals, color=colors, alpha=0.85,
                   edgecolor="black")
    ax.axhline(y=1.0, color="grey", linestyle="--", alpha=0.5,
                label="1x = paridad C")
    ax.set_yscale("log")
    ax.set_ylabel("Geomean slowdown vs C (log)")
    ax.set_title("Resumen global: media geometrica de slowdown vs C\n"
                 "Mas bajo = lenguaje mas rapido en promedio")
    for bar, v in zip(bars, vals):
        if not math.isnan(v):
            ax.text(bar.get_x() + bar.get_width() / 2, v,
                    f"{v:.2f}x", ha="center", va="bottom",
                    fontsize=12, fontweight="bold")
    ax.legend(fontsize=10)
    ax.grid(axis="y", which="both", alpha=0.3)
    plt.tight_layout()
    plt.savefig(out_path, dpi=120, bbox_inches="tight")
    plt.close(fig)
    return True


# =============================================================================
# Plot 10: system info (hardware + versiones de compiladores)
# =============================================================================

def plot_system_info(sys_info: dict, out_path: Path) -> bool:
    """Imagen con info del sistema en el que se corrio el bench.

    Critico para reproducibilidad: incluye CPU, RAM, OS, y version
    EXACTA de cada compilador/runtime usado.

    @param sys_info dict con keys: 'hardware', 'os', 'tooling', 'date'.
    """
    deps = _try_import()
    if not deps:
        return False
    _, plt, _ = deps

    fig, ax = plt.subplots(figsize=(14, 9))
    ax.axis("off")

    # Titulo.
    ax.text(0.5, 0.97, "VestaVM Benchmark Suite -- System Info",
            fontsize=18, fontweight="bold", ha="center",
            transform=ax.transAxes)
    ax.text(0.5, 0.935,
            f"Reporte generado: {sys_info.get('date', '?')}",
            fontsize=10, ha="center", style="italic",
            transform=ax.transAxes, color="#666")

    # Linea separadora.
    ax.axhline(y=0.91, color="black", linewidth=0.5,
                xmin=0.05, xmax=0.95)

    # Seccion 1: Hardware.
    y = 0.86
    ax.text(0.05, y, "[1] HARDWARE", fontsize=13,
            fontweight="bold", color="#1f77b4",
            transform=ax.transAxes)
    y -= 0.04
    hw = sys_info.get("hardware", {})
    hw_items = [
        ("CPU",         hw.get("cpu_model", "?")),
        ("Arquitectura", hw.get("arch", "?")),
        ("Cores (logical/physical)",
            f"{hw.get('cpu_logical', '?')} / {hw.get('cpu_physical', '?')}"),
        ("Frecuencia CPU", hw.get("cpu_freq_mhz", "?")),
        ("RAM total",   hw.get("ram_gb", "?")),
    ]
    for key, val in hw_items:
        ax.text(0.07, y, f"{key}:", fontsize=10, fontweight="bold",
                transform=ax.transAxes)
        ax.text(0.35, y, str(val), fontsize=10,
                transform=ax.transAxes, family="monospace")
        y -= 0.032

    # Seccion 2: OS.
    y -= 0.015
    ax.text(0.05, y, "[2] SISTEMA OPERATIVO", fontsize=13,
            fontweight="bold", color="#2ca02c",
            transform=ax.transAxes)
    y -= 0.04
    os_info = sys_info.get("os", {})
    os_items = [
        ("Plataforma", os_info.get("platform", "?")),
        ("Version", os_info.get("version", "?")),
        ("Kernel/Release", os_info.get("release", "?")),
    ]
    for key, val in os_items:
        ax.text(0.07, y, f"{key}:", fontsize=10, fontweight="bold",
                transform=ax.transAxes)
        ax.text(0.35, y, str(val), fontsize=10,
                transform=ax.transAxes, family="monospace")
        y -= 0.032

    # Seccion 3: Toolchain / runtime versions.
    y -= 0.015
    ax.text(0.05, y, "[3] COMPILADORES / RUNTIMES",
            fontsize=13, fontweight="bold", color="#d62728",
            transform=ax.transAxes)
    y -= 0.04
    tooling = sys_info.get("tooling", {})
    for lang_name, info in tooling.items():
        if not info.get("available"):
            ax.text(0.07, y, f"{lang_name}:",
                    fontsize=10, fontweight="bold",
                    transform=ax.transAxes, color="#888")
            ax.text(0.35, y, "NO DISPONIBLE", fontsize=10,
                    transform=ax.transAxes, family="monospace",
                    color="#888")
        else:
            ax.text(0.07, y, f"{lang_name}:",
                    fontsize=10, fontweight="bold",
                    transform=ax.transAxes)
            version = info.get("version", "?")
            if len(version) > 80:
                version = version[:77] + "..."
            ax.text(0.35, y, version, fontsize=9,
                    transform=ax.transAxes, family="monospace",
                    color="#333")
            # Path en una sub-linea.
            path = info.get("path", "")
            if path:
                y -= 0.024
                if len(path) > 90:
                    path = "..." + path[-87:]
                ax.text(0.35, y, path, fontsize=8,
                        transform=ax.transAxes, family="monospace",
                        color="#888")
        y -= 0.032

    # Footer.
    ax.text(0.5, 0.02,
            "Estos parametros condicionan los tiempos.  Para comparar "
            "vs otro PC verifica que coincidan.",
            fontsize=9, ha="center", style="italic",
            transform=ax.transAxes, color="#666")

    plt.savefig(out_path, dpi=110, bbox_inches="tight")
    plt.close(fig)
    return True


# =============================================================================
# Generador maestro: produce TODAS las graficas en un directorio
# =============================================================================

def generate_all(rows: list[dict], langs: list[str], plot_dir: Path,
                 sys_info: Optional[dict] = None) -> dict:
    """Genera todas las graficas en @c plot_dir .  Retorna dict con
    {nombre_plot: bool_ok} para que el caller reporte que se genero.
    """
    plot_dir.mkdir(parents=True, exist_ok=True)
    results = {}
    # System info PRIMERO (00 prefix).  Es el contexto de todo lo demas.
    if sys_info is not None:
        results["00_system_info"] = plot_system_info(
            sys_info, plot_dir / "00_system_info.png")
    results["01_dashboard"] = plot_dashboard(
        rows, langs, plot_dir / "01_dashboard.png")
    results["02_heatmap"] = plot_heatmap(
        rows, langs, plot_dir / "02_heatmap.png")
    results["03_radar"] = plot_radar(
        rows, langs, plot_dir / "03_radar_profile.png")
    results["04_boxplot_variability"] = plot_boxplot_variability(
        rows, langs, plot_dir / "04_boxplot_variability.png")
    results["05_scatter_ratio"] = plot_scatter_ratio(
        rows, langs, plot_dir / "05_scatter_ratio_vs_c.png")
    results["06_ranking_lines"] = plot_ranking_lines(
        rows, langs, plot_dir / "06_ranking_lines.png")
    results["07_grouped_ratio"] = plot_grouped_ratio(
        rows, langs, plot_dir / "07_grouped_ratio.png")
    results["08_geomean"] = plot_geomean_summary(
        rows, langs, plot_dir / "08_geomean_summary.png")
    # Per-bench: subdir.
    per_bench_dir = plot_dir / "per_bench"
    count = plot_per_bench(rows, langs, per_bench_dir)
    results[f"per_bench ({count} archivos)"] = count > 0
    return results
