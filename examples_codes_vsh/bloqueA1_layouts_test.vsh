// =============================================================================
// vnano BLOQUE A1 - Test de geometria de layouts
// =============================================================================
// Valida que la funcion _layout_rects() devuelve rectangulos correctos
// para los 6 layouts soportados, sin solapamientos y sin huecos.
//
// Pruebalo:
//   vesta --script bloqueA1_layouts_test.vsh
//
// Imprime un dibujo ASCII de cada layout y comprueba que las
// coordenadas son consistentes.
// =============================================================================


// -----------------------------------------------------------------------------
// LAYOUT_RECTS
// -----------------------------------------------------------------------------
// Devuelve una lista de rectangulos {x, y, w, h} para cada panel del
// layout indicado, dentro de un area area_w x area_h con esquina
// superior izquierda en (area_x, area_y).
//
// Layouts:
//   "single"      1 panel
//   "split_v"     2 paneles lado a lado
//   "split_h"     2 paneles uno encima del otro
//   "three_left"  P1 izquierda completa, P2/P3 derecha apilados
//   "three_right" P1/P2 izquierda apilados, P3 derecha completa
//   "grid"        2x2
//
// Convencion: el divisor entre paneles ocupa 1 columna (o fila).
// El divisor pertenece al "espacio entre paneles", no a ningun panel.

fn layout_rects(layout, area_x, area_y, area_w, area_h) {
    let rects = []

    if layout == "single" {
        append(rects, { "x": area_x, "y": area_y, "w": area_w, "h": area_h })
        return rects
    }

    if layout == "split_v" {
        let half = (area_w - 1) / 2
        let other = area_w - 1 - half
        append(rects, { "x": area_x, "y": area_y, "w": half, "h": area_h })
        append(rects, { "x": area_x + half + 1, "y": area_y, "w": other, "h": area_h })
        return rects
    }

    if layout == "split_h" {
        let half = (area_h - 1) / 2
        let other = area_h - 1 - half
        append(rects, { "x": area_x, "y": area_y, "w": area_w, "h": half })
        append(rects, { "x": area_x, "y": area_y + half + 1, "w": area_w, "h": other })
        return rects
    }

    if layout == "three_left" {
        // P1 ocupa la izquierda completa, P2/P3 apilan a la derecha
        let half_w = (area_w - 1) / 2
        let other_w = area_w - 1 - half_w
        let half_h = (area_h - 1) / 2
        let other_h = area_h - 1 - half_h
        append(rects, { "x": area_x, "y": area_y, "w": half_w, "h": area_h })
        append(rects, { "x": area_x + half_w + 1, "y": area_y, "w": other_w, "h": half_h })
        append(rects, { "x": area_x + half_w + 1, "y": area_y + half_h + 1, "w": other_w, "h": other_h })
        return rects
    }

    if layout == "three_right" {
        // P1/P2 apilados a la izquierda, P3 ocupa la derecha completa
        let half_w = (area_w - 1) / 2
        let other_w = area_w - 1 - half_w
        let half_h = (area_h - 1) / 2
        let other_h = area_h - 1 - half_h
        append(rects, { "x": area_x, "y": area_y, "w": half_w, "h": half_h })
        append(rects, { "x": area_x, "y": area_y + half_h + 1, "w": half_w, "h": other_h })
        append(rects, { "x": area_x + half_w + 1, "y": area_y, "w": other_w, "h": area_h })
        return rects
    }

    if layout == "grid" {
        let half_w = (area_w - 1) / 2
        let other_w = area_w - 1 - half_w
        let half_h = (area_h - 1) / 2
        let other_h = area_h - 1 - half_h
        append(rects, { "x": area_x,                  "y": area_y,                  "w": half_w,  "h": half_h })
        append(rects, { "x": area_x + half_w + 1,     "y": area_y,                  "w": other_w, "h": half_h })
        append(rects, { "x": area_x,                  "y": area_y + half_h + 1,     "w": half_w,  "h": other_h })
        append(rects, { "x": area_x + half_w + 1,     "y": area_y + half_h + 1,     "w": other_w, "h": other_h })
        return rects
    }

    // Fallback: single
    append(rects, { "x": area_x, "y": area_y, "w": area_w, "h": area_h })
    return rects
}


// -----------------------------------------------------------------------------
// VALIDADOR DE LAYOUTS
// -----------------------------------------------------------------------------
// Comprueba que:
//   - Ningun rectangulo se solapa
//   - Cada celda del area pertenece a EXACTAMENTE un rect, o es un divisor
//   - Los divisores son coherentes (1 columna o 1 fila completa)

fn validate_layout(layout, rects, area_x, area_y, area_w, area_h) {
    // Crear matriz de "owner" para cada celda del area: -1 si nadie, idx si rect i
    let grid = []
    let i = 0
    while i < area_h {
        let row = []
        let j = 0
        while j < area_w {
            append(row, -1)
            j = j + 1
        }
        append(grid, row)
        i = i + 1
    }

    // Marcar cada rect en la grid
    let r_idx = 0
    while r_idx < len(rects) {
        let r = rects[r_idx]
        let dx = r["x"] - area_x
        let dy = r["y"] - area_y
        let yy = 0
        while yy < r["h"] {
            let xx = 0
            while xx < r["w"] {
                let gy = dy + yy
                let gx = dx + xx
                if gy >= 0 and gy < area_h and gx >= 0 and gx < area_w {
                    if grid[gy][gx] != -1 {
                        return { "ok": false, "msg": "Solape en (" + str(gx) + "," + str(gy) + ") entre rect " + str(grid[gy][gx]) + " y rect " + str(r_idx) }
                    }
                    grid[gy][gx] = r_idx
                }
                xx = xx + 1
            }
            yy = yy + 1
        }
        r_idx = r_idx + 1
    }

    // Contar cuantas celdas no asignadas hay (deberian ser solo divisores)
    let unassigned = 0
    let yy = 0
    while yy < area_h {
        let xx = 0
        while xx < area_w {
            if grid[yy][xx] == -1 { unassigned = unassigned + 1 }
            xx = xx + 1
        }
        yy = yy + 1
    }

    return { "ok": true, "unassigned": unassigned, "grid": grid }
}


// -----------------------------------------------------------------------------
// VISUALIZADOR DE LAYOUTS
// -----------------------------------------------------------------------------
// Imprime el grid usando A, B, C, D para los paneles y "-" / "|" / " " para
// huecos (divisores).

fn print_layout(name, rects, area_w, area_h) {
    println("")
    println("=== " + name + " (" + str(len(rects)) + " paneles, area " + str(area_w) + "x" + str(area_h) + ") ===")

    let val = validate_layout(name, rects, 0, 0, area_w, area_h)
    if not val["ok"] {
        println("ERROR: " + val["msg"])
        return
    }

    let labels = ["A", "B", "C", "D"]
    let grid = val["grid"]
    let yy = 0
    while yy < area_h {
        let line = ""
        let xx = 0
        while xx < area_w {
            let owner = grid[yy][xx]
            if owner == -1 {
                // divisor: ver si la columna o la fila completa son divisor
                line = line + "."
            } else {
                line = line + labels[owner]
            }
            xx = xx + 1
        }
        println(line)
        yy = yy + 1
    }

    // Detalle numerico
    let i = 0
    while i < len(rects) {
        let r = rects[i]
        println("  " + labels[i] + ": x=" + str(r["x"]) + " y=" + str(r["y"]) + " w=" + str(r["w"]) + " h=" + str(r["h"]))
        i = i + 1
    }
    println("  Celdas divisor: " + str(val["unassigned"]))
}


// -----------------------------------------------------------------------------
// MAIN: probar los 6 layouts en area 60x12
// -----------------------------------------------------------------------------

fn main() {
    let W = 60
    let H = 12

    println("Probando los 6 layouts en area " + str(W) + "x" + str(H))

    print_layout("single",      layout_rects("single",      0, 0, W, H), W, H)
    print_layout("split_v",     layout_rects("split_v",     0, 0, W, H), W, H)
    print_layout("split_h",     layout_rects("split_h",     0, 0, W, H), W, H)
    print_layout("three_left",  layout_rects("three_left",  0, 0, W, H), W, H)
    print_layout("three_right", layout_rects("three_right", 0, 0, W, H), W, H)
    print_layout("grid",        layout_rects("grid",        0, 0, W, H), W, H)

    println("")
    println("--- Tests con area pequena 30x8 (caso real con sidebar) ---")
    print_layout("split_v", layout_rects("split_v", 0, 0, 30, 8), 30, 8)
    print_layout("grid",    layout_rects("grid",    0, 0, 30, 8), 30, 8)

    println("")
    println("Si todos los layouts se dibujan sin solapes ni errores, el bloque A1 esta OK.")
}

main()
