#!/usr/bin/env python3
"""flamegraph.py -- convierte un archivo folded (Brendan Gregg) en un
flamegraph SVG autocontenido.

El sampling profiler del VM escribe un archivo folded con una linea por
frame: "<funcion>[;<subfuncion>...] <count>".  Este script lo parsea,
construye el arbol de frames y renderiza un SVG con rectangulos cuyo
ancho es proporcional a las muestras (mismo modelo que flamegraph.pl de
Brendan Gregg), con tooltip (nombre, muestras, %).

Uso:
  python tools/flamegraph.py perfil.folded -o perfil.svg
  python tools/flamegraph.py perfil.folded --min-pct 1 --width 1600
  python tools/flamegraph.py perfil.folded -o perfil.svg --reverse

Salida:
  Un SVG de ancho fijo y altura segun la profundidad del arbol.
  Abrir en cualquier navegador; pasar el raton sobre un frame para
  ver nombre + muestras + %.
"""
from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

# Paleta de 10 colores (tonos calidos -> frios).  El color de un frame se
# deriva de un hash estable de su nombre, asi es reproducible entre runs.
PALETTE = [
    "#e8615a", "#e8945a", "#e8c35a", "#b8e85a", "#5ae861",
    "#5ae8b8", "#5ac3e8", "#5a7fe8", "#8a5ae8", "#e85ac3",
]


def frame_color(name: str) -> str:
    digest = hashlib.md5(name.encode("utf-8")).digest()
    return PALETTE[digest[0] % len(PALETTE)]


def build_tree(folded: list[tuple[list[str], int]]) -> dict:
    """Construye un arbol {name: {"name", "count", "children"}}.

    Cada nodo sabe su nombre (la raiz virtual usa name="").  children es
    un dict nombre -> nodo, preservando el orden de insercion.
    """
    root: dict = {"name": "", "count": 0, "children": {}}
    for frames, count in folded:
        node = root
        for f in frames:
            node["count"] += count
            child = node["children"].get(f)
            if child is None:
                child = {"name": f, "count": 0, "children": {}}
                node["children"][f] = child
            node = child
        node["count"] += count
    return root


def render(node: dict, x: float, width: float, y: int, row_h: int,
           depth: int, total: int, out: list[str], min_pct: float) -> int:
    """Render recursivo.  Devuelve la maxima profundidad alcanzada."""
    if width < 0.5 or node["count"] <= 0:
        return depth
    max_depth = depth
    if depth > 0:  # el nivel 0 es la raiz virtual (total), no se dibuja
        pct = 100.0 * node["count"] / total if total else 0.0
        if pct >= min_pct:
            color = frame_color(node["name"])
            out.append(
                f'<rect x="{x:.2f}" y="{y}" width="{width:.2f}" '
                f'height="{row_h}" fill="{color}" stroke="#00000022">'
                f'<title>{node["name"]} ({node["count"]} muestras, '
                f'{pct:.2f}%)</title></rect>'
            )
    x_child = x
    # Ordenar hijos por count descendente para dibujar los grandes primero.
    children = sorted(
        node["children"].values(), key=lambda c: c["count"], reverse=True)
    if children and depth < 200:
        child_row = y + row_h
        for child in children:
            w = width * child["count"] / node["count"] if node["count"] else 0.0
            max_depth = max(max_depth, render(child, x_child, w, child_row,
                                              row_h, depth + 1, total, out,
                                              min_pct))
            x_child += w
    return max_depth


def render_tree(tree: dict, total: int, width: int, row_h: int,
                min_pct: float) -> tuple[list[str], int]:
    """Convierte el arbol en lineas SVG.  Devuelve (lineas, max_depth)."""
    out: list[str] = []
    # La raiz virtual acumula 'total'; render() la recorre sin dibujarla.
    max_depth = render(tree, 0.0, float(width), 0, row_h, 0, total, out,
                       min_pct)
    return out, max_depth


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Convierte un archivo folded a un flamegraph SVG.")
    ap.add_argument("folded", help="Archivo folded del sampling profiler")
    ap.add_argument("-o", "--output", default="flamegraph.svg",
                    help="Ruta del SVG de salida (default: flamegraph.svg)")
    ap.add_argument("--width", type=int, default=1200,
                    help="Ancho del SVG en px (default: 1200)")
    ap.add_argument("--row-height", type=int, default=16,
                    help="Alto por fila/frame en px (default: 16)")
    ap.add_argument("--min-pct", type=float, default=0.0,
                    help="Ocultar frames con menos de este %% del total "
                         "(default: 0)")
    ap.add_argument("--sort", action="store_true",
                    help="Ordenar la salida folded por count antes de render")
    args = ap.parse_args()

    folded_path = Path(args.folded)
    if not folded_path.is_file():
        print(f"[error] no existe '{args.folded}'", file=sys.stderr)
        return 1

    entries: list[tuple[list[str], int]] = []
    total = 0
    with folded_path.open("r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            # Formato: "a;b;c <count>"  (el separador puede ser espacio o tab)
            parts = line.rsplit(" ", 1)
            if len(parts) != 2:
                parts = line.rsplit("\t", 1)
            if len(parts) != 2:
                print(f"[warn] linea sin count, se ignora: {line!r}",
                      file=sys.stderr)
                continue
            frames = parts[0].split(";")
            try:
                count = int(parts[1])
            except ValueError:
                print(f"[warn] count invalido, se ignora: {line!r}",
                      file=sys.stderr)
                continue
            entries.append((frames, count))
            total += count

    if not entries or total <= 0:
        print("[error] el archivo folded no tiene muestras", file=sys.stderr)
        return 1

    if args.sort:
        entries.sort(key=lambda e: e[1], reverse=True)

    tree = build_tree(entries)
    lines, max_depth = render_tree(tree, total, args.width, args.row_height,
                                   args.min_pct)

    height = (max_depth + 1) * args.row_height + 40
    header = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{args.width}" '
        f'height="{height}" viewBox="0 0 {args.width} {height}">\n'
        '<style>text { font-family: monospace; font-size: 11px; }</style>\n'
        f'<rect x="0" y="0" width="{args.width}" height="{height}" '
        'fill="#ffffff"/>\n'
    )
    footer = "</svg>\n"

    svg = (
        header
        + "".join(f"  {l}\n" for l in lines)
        + footer
    )

    out_path = Path(args.output)
    out_path.write_text(svg, encoding="utf-8")
    print(f"[ok] flamegraph escrito en '{out_path}' "
          f"({len(entries)} stacks, {total} muestras, "
          f"profundidad {max_depth})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
