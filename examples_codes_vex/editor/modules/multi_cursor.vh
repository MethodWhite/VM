// =============================================================================
// modules/multi_cursor.vh - Multi-cursor minimo (vertical, columna fija)
// =============================================================================
//
// Modelo: el editor mantiene una columna OBJETIVO + una lista de FILAS donde
// hay cursores adicionales (ademas del primario en this.buffer.cursor).
// Cuando el usuario inserta un caracter, lo replicamos en todas las filas
// extras a la misma columna.  Cuando borra (backspace), borra el char
// anterior en cada cursor.  Las operaciones se ordenan descending para
// que el array de bytes del buffer no se invalide entre ediciones.
//
// API:
//   add_row(row, col)        anyade una fila como cursor extra (col se
//                            usa solo la PRIMERA vez para fijar la columna
//                            objetivo; las siguientes ignoran col)
//   add_below(buffer)        anyade un cursor en la linea siguiente del
//                            primario, a la columna del primario
//   add_above(buffer)        idem pero arriba
//   clear()                  borra todos los extras
//   count() -> i32
//   target_col() -> i32      columna objetivo
//   row_at(i) -> i32         row del extra i-esimo
//
// El editor llama collect_offsets(buffer) para obtener un array (ordenado
// descending) con TODOS los cursores (primario + extras como byte offsets).
// Luego aplica la edicion en cada offset.  Tras la edicion, recalcula los
// extras: si insert/delete cambio las lineas, los rows extras pueden
// haber cambiado de byte-offset pero NO de row (estan ligados a (row, col)
// no a byte_offset).

class MultiCursor {
    public i32 col_target;      ///< columna objetivo (-1 = inactivo)
    public i32 count_extras;    ///< numero de extras (0..7)

    // Hasta 8 extras (paralelo de filas).
    public i32 r0; public i32 r1; public i32 r2; public i32 r3;
    public i32 r4; public i32 r5; public i32 r6; public i32 r7;

    public MultiCursor() {
        this.col_target   = -1;
        this.count_extras = 0;
        this.r0 = -1; this.r1 = -1; this.r2 = -1; this.r3 = -1;
        this.r4 = -1; this.r5 = -1; this.r6 = -1; this.r7 = -1;
    }

    public i32 count() { return this.count_extras; }

    public i32 row_at(i32 i) {
        if (i == 0) { return this.r0; }
        if (i == 1) { return this.r1; }
        if (i == 2) { return this.r2; }
        if (i == 3) { return this.r3; }
        if (i == 4) { return this.r4; }
        if (i == 5) { return this.r5; }
        if (i == 6) { return this.r6; }
        if (i == 7) { return this.r7; }
        return -1;
    }

    public void set_row(i32 i, i32 v) {
        if (i == 0) { this.r0 = v; return; }
        if (i == 1) { this.r1 = v; return; }
        if (i == 2) { this.r2 = v; return; }
        if (i == 3) { this.r3 = v; return; }
        if (i == 4) { this.r4 = v; return; }
        if (i == 5) { this.r5 = v; return; }
        if (i == 6) { this.r6 = v; return; }
        if (i == 7) { this.r7 = v; return; }
    }

    public i32 has_row(i32 row) {
        i32 i = 0;
        while (i < this.count_extras) {
            if (this.row_at(i) == row) { return 1; }
            i = i + 1;
        }
        return 0;
    }

    public void clear() {
        this.col_target   = -1;
        this.count_extras = 0;
    }

    // Activa multi-cursor con columna objetivo.  Si ya estaba activo, no
    // toca col_target.
    public void ensure_active(i32 col) {
        if (this.col_target < 0) { this.col_target = col; }
    }

    // Anyade un extra (idempotente: no duplica filas).
    public void add_row(i32 row, i32 col) {
        if (this.count_extras >= 8) { return; }
        this.ensure_active(col);
        if (this.has_row(row) == 1) { return; }
        this.set_row(this.count_extras, row);
        this.count_extras = this.count_extras + 1;
    }
}

// (las .vh no llevan main: forman parte del modulo del editor)
