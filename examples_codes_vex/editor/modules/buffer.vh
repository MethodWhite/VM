// =============================================================================
// modules/buffer.vh - documento + cursor (single buffer plano)
// =============================================================================
//
// Diseno: el buffer es UN solo bloque de bytes raw (`u8* data`) con
// saltos de linea LF (10) como separadores.  El cursor es un offset en
// bytes; las "lineas" se calculan recorriendo el buffer cada vez que
// se renderiza.  Esto evita:
//
//   * Cadenas largas de objetos GC anidados (Line dentro de Buffer
//     dentro de Editor) que estresan el regalloc + GC dance.
//   * Realocacion de slots Line al insertar / borrar lineas.
//
// Coste: la conversion (row, col) -> offset es O(N) en el contenido,
// asi que para buffers muy grandes (>100 KB) habra que cachear el
// indice de inicio de cada linea.  Para edicion humana es invisible.

class Buffer {
    public u8* data;        // contenido en bytes
    public i32 length;      // bytes usados
    public i32 capacity;    // bytes alocados
    public i32 cursor;      // offset del cursor (0..length)
    public i32 dirty;       // 1 si hubo modificacion desde load/save

    public Buffer() {
        this.capacity = 1024;
        this.data = malloc(1024);
        this.length = 0;
        this.cursor = 0;
        this.dirty = 0;
        this.data[0] = 0;
    }

    public ~Buffer() {
        if (this.data != null) {
            free(this.data);
        }
    }

    // Garantiza al menos `need` bytes (sin contar terminador NUL).
    public void ensure_capacity(i32 need) {
        if (need + 1 <= this.capacity) { return; }
        i32 new_cap = this.capacity;
        while (new_cap < need + 1) {
            new_cap = new_cap * 2;
        }
        u8* nbuf = malloc(new_cap);
        i32 i = 0;
        while (i < this.length) {
            nbuf[i] = this.data[i];
            i = i + 1;
        }
        nbuf[this.length] = 0;
        free(this.data);
        this.data = nbuf;
        this.capacity = new_cap;
    }

    // Inserta el byte `c` en la posicion del cursor.
    public void insert_char(i32 c) {
        this.ensure_capacity(this.length + 1);
        i32 i = this.length;
        while (i > this.cursor) {
            this.data[i] = this.data[i - 1];
            i = i - 1;
        }
        this.data[this.cursor] = c;
        this.length = this.length + 1;
        this.data[this.length] = 0;
        this.cursor = this.cursor + 1;
        this.dirty = 1;
    }

    // Inserta un salto de linea en la posicion del cursor.
    public void newline() {
        this.insert_char(10);
    }

    // Borra el byte ANTES del cursor (BS).
    public void backspace() {
        if (this.cursor == 0) { return; }
        i32 i = this.cursor - 1;
        while (i < this.length - 1) {
            this.data[i] = this.data[i + 1];
            i = i + 1;
        }
        this.length = this.length - 1;
        this.data[this.length] = 0;
        this.cursor = this.cursor - 1;
        this.dirty = 1;
    }

    // Movimientos del cursor sobre el buffer plano.
    public void move_left() {
        if (this.cursor > 0) { this.cursor = this.cursor - 1; }
    }
    public void move_right() {
        if (this.cursor < this.length) { this.cursor = this.cursor + 1; }
    }

    // Va al inicio de la linea actual (Home).
    public void move_home() {
        this.cursor = this.line_start();
    }

    // Va al final de la linea actual: posicion del LF o end-of-buffer.
    public void move_end() {
        i32 i = this.cursor;
        while (i < this.length && this.data[i] != 10) {
            i = i + 1;
        }
        this.cursor = i;
    }

    // Borra el byte EN el cursor (Del).
    public void del_char() {
        if (this.cursor >= this.length) { return; }
        i32 i = this.cursor;
        while (i < this.length - 1) {
            this.data[i] = this.data[i + 1];
            i = i + 1;
        }
        this.length = this.length - 1;
        this.data[this.length] = 0;
        this.dirty = 1;
    }

    // Calcula el offset del primer byte de la fila `row` (0-based).
    // Si row excede line_count() devuelve length (cursor al final).
    public i32 offset_of_row(i32 row) {
        if (row <= 0) { return 0; }
        i32 r = 0;
        i32 i = 0;
        while (i < this.length) {
            if (this.data[i] == 10) {
                r = r + 1;
                if (r == row) { return i + 1; }
            }
            i = i + 1;
        }
        return this.length;
    }

    // Posiciona el cursor al inicio de `row` con la columna `col`,
    // saturando al final de la linea si col excede su longitud.
    public void goto_row_col(i32 row, i32 col) {
        i32 off = this.offset_of_row(row);
        // Avanzar hasta col bytes pero detenerse en LF o end.
        i32 c = 0;
        while (c < col && off < this.length && this.data[off] != 10) {
            off = off + 1;
            c   = c + 1;
        }
        this.cursor = off;
    }

    // Page up: sube `rows` filas manteniendo columna aproximada.
    public void page_up(i32 rows) {
        i32 r = this.cursor_row();
        i32 c = this.cursor_col();
        i32 nr = r - rows;
        if (nr < 0) { nr = 0; }
        this.goto_row_col(nr, c);
    }

    // Page down: baja `rows` filas.
    public void page_down(i32 rows) {
        i32 r = this.cursor_row();
        i32 c = this.cursor_col();
        i32 nr = r + rows;
        i32 lc = this.line_count();
        if (nr >= lc) { nr = lc - 1; }
        if (nr < 0) { nr = 0; }
        this.goto_row_col(nr, c);
    }

    // Sube una linea: busca el LF anterior al inicio de la linea actual,
    // y retrocede al mismo offset relativo (o al final de la linea
    // anterior si esta es mas corta).
    public void move_up() {
        i32 line_start = this.line_start();
        if (line_start == 0) { return; }
        i32 col = this.cursor - line_start;
        // Posicion del LF que precede a line_start es line_start - 1.
        // Buscar el LF antes de ese para localizar el inicio de la
        // linea anterior.
        i32 prev_end = line_start - 1;     // posicion del LF
        i32 prev_start = 0;
        i32 i = prev_end - 1;
        while (i >= 0) {
            if (this.data[i] == 10) {
                prev_start = i + 1;
                i = -1;
            } else {
                i = i - 1;
            }
        }
        i32 prev_len = prev_end - prev_start;
        i32 ncol = col;
        if (ncol > prev_len) { ncol = prev_len; }
        this.cursor = prev_start + ncol;
    }

    // Baja una linea.
    public void move_down() {
        i32 next_lf = this.cursor;
        while (next_lf < this.length && this.data[next_lf] != 10) {
            next_lf = next_lf + 1;
        }
        if (next_lf >= this.length) { return; }
        i32 line_start = this.line_start();
        i32 col = this.cursor - line_start;
        i32 next_start = next_lf + 1;
        i32 next_end = next_start;
        while (next_end < this.length && this.data[next_end] != 10) {
            next_end = next_end + 1;
        }
        i32 next_len = next_end - next_start;
        i32 ncol = col;
        if (ncol > next_len) { ncol = next_len; }
        this.cursor = next_start + ncol;
    }

    // Devuelve el offset del primer byte de la linea actual.
    public i32 line_start() {
        i32 i = this.cursor;
        while (i > 0 && this.data[i - 1] != 10) {
            i = i - 1;
        }
        return i;
    }

    // Cuenta el numero de lineas (LFs + 1 si no termina en LF).
    public i32 line_count() {
        i32 c = 1;
        i32 i = 0;
        while (i < this.length) {
            if (this.data[i] == 10) { c = c + 1; }
            i = i + 1;
        }
        return c;
    }

    // Calcula (row, col) actuales del cursor recorriendo el buffer.
    public i32 cursor_row() {
        i32 r = 0;
        i32 i = 0;
        while (i < this.cursor && i < this.length) {
            if (this.data[i] == 10) { r = r + 1; }
            i = i + 1;
        }
        return r;
    }
    public i32 cursor_col() {
        return this.cursor - this.line_start();
    }

    // Guarda el buffer al fichero `path`.  Devuelve 1 OK / 0 fail.
    public i32 save_to_file(FileIO io, string path) {
        i64 fp = io.open(path, "wb");
        if (fp == 0) { return 0; }
        if (this.length > 0) {
            io.write_bytes(fp, this.data, this.length);
        }
        io.close(fp);
        this.dirty = 0;
        return 1;
    }

    // Carga el fichero `path` al buffer.  Devuelve 1 OK / 0 fail.
    public i32 load_from_file(FileIO io, string path) {
        i64 fp = io.open(path, "rb");
        if (fp == 0) { return 0; }
        i64 sz = io.file_size(fp);
        if (sz < 0) {
            io.close(fp);
            return 0;
        }
        i32 isz = sz;
        this.ensure_capacity(isz);
        if (isz > 0) {
            io.read_bytes(fp, this.data, isz);
        }
        io.close(fp);
        this.length = isz;
        this.data[this.length] = 0;
        this.cursor = 0;
        this.dirty = 0;
        return 1;
    }
}
