// =============================================================================
// modules/config.vh - Configuracion persistida en .vexedrc
// =============================================================================
//
// Equivalente a class Config de vnano.vsh (lineas 374-651 del original).
// Lee un fichero .vexedrc del directorio actual al arrancar el editor con
// formato simple key=value (una linea por opcion).
//
// Opciones soportadas:
//
//     tab_width=4              # ancho del tab en espacios (default 4)
//     syntax_on=1              # 1 = highlight Vex activo, 0 = off
//     sidebar_width=25         # cols del panel lateral (default 25)
//     show_line_numbers=1      # 1 = gutter ExtLineNo activa
//
// Lineas vacias y las que empiezan con # son ignoradas.  Si .vexedrc no
// existe, los valores quedan en sus defaults sin error.  save() permite
// que el editor escriba el fichero (en sesion futura podriamos exponer
// un comando para editar/guardar settings desde el editor).

class Config {
    public i32    tab_width;
    public i32    syntax_on;
    public i32    sidebar_width;
    public i32    show_line_numbers;
    public string project_basename;  ///< basename de la carpeta del proyecto (cwd)
    public FileIO io;

    public Config() {
        // Valores por defecto.
        this.tab_width         = 4;
        this.syntax_on         = 1;
        this.sidebar_width     = 25;
        this.show_line_numbers = 1;
        this.project_basename  = "?";
        this.io                = new FileIO();
    }

    // Carga .vexedrc del cwd.  No-op si no existe.
    public void load() {
        i64 fp = this.io.open(".vexedrc", "rb");
        if (fp == 0) { return; }
        // Leer hasta 16 KB.
        i64 sz = this.io.file_size(fp);
        if (sz <= 0 || sz > 16384) { this.io.close(fp); return; }
        u8* buf = malloc(sz + 1);
        i64 got = this.io.read_bytes(fp, buf, sz);
        this.io.close(fp);
        if (got <= 0) { free(buf); return; }
        // Parsear linea por linea.
        i32 lstart = 0;
        i32 i      = 0;
        while (i < got) {
            if (buf[i] == 10 || buf[i] == 13 || i == got - 1) {
                i32 lend = i;
                if (i == got - 1 && buf[i] != 10 && buf[i] != 13) {
                    lend = i + 1;
                }
                this.parse_line(buf, lstart, lend);
                // Avanzar mas alla del CR/LF.
                if (i < got - 1 && buf[i] == 13 && buf[i + 1] == 10) { i = i + 1; }
                lstart = i + 1;
            }
            i = i + 1;
        }
        free(buf);
    }

    // Parsea una linea [start, end) del buffer.  Skip leading whitespace,
    // comentarios (#), lineas vacias.  Formato: key=value.
    public void parse_line(u8* buf, i32 start, i32 end) {
        // Skip leading whitespace.
        while (start < end && (buf[start] == 32 || buf[start] == 9)) {
            start = start + 1;
        }
        if (start >= end) { return; }
        // Comentario.
        if (buf[start] == 35) { return; }  // '#'
        // Buscar '='.
        i32 eq = start;
        while (eq < end && buf[eq] != 61) { eq = eq + 1; }  // '='
        if (eq >= end) { return; }  // sin '=' = ignorar
        // Trim trailing whitespace en key.
        i32 key_end = eq;
        while (key_end > start && (buf[key_end - 1] == 32 || buf[key_end - 1] == 9)) {
            key_end = key_end - 1;
        }
        // Skip whitespace post-=.
        i32 val_start = eq + 1;
        while (val_start < end && (buf[val_start] == 32 || buf[val_start] == 9)) {
            val_start = val_start + 1;
        }
        // Trim trailing whitespace en value.
        i32 val_end = end;
        while (val_end > val_start && (buf[val_end - 1] == 32 || buf[val_end - 1] == 9
                                    || buf[val_end - 1] == 13 || buf[val_end - 1] == 10)) {
            val_end = val_end - 1;
        }
        if (key_end <= start) { return; }
        // Parse value como entero (todas las opciones son numericas en MVP).
        i32 v = 0;
        i32 j = val_start;
        while (j < val_end && buf[j] >= 48 && buf[j] <= 57) {
            v = v * 10 + (buf[j] - 48);
            j = j + 1;
        }
        // Match key (comparacion byte-a-byte).
        if (this.key_eq(buf, start, key_end, "tab_width") == 1) {
            if (v >= 1 && v <= 16) { this.tab_width = v; }
        } else if (this.key_eq(buf, start, key_end, "syntax_on") == 1) {
            if (v == 0 || v == 1) { this.syntax_on = v; }
        } else if (this.key_eq(buf, start, key_end, "sidebar_width") == 1) {
            if (v >= 10 && v <= 80) { this.sidebar_width = v; }
        } else if (this.key_eq(buf, start, key_end, "show_line_numbers") == 1) {
            if (v == 0 || v == 1) { this.show_line_numbers = v; }
        }
    }

    // True si los bytes [start, end) coinciden con `key` (case-sensitive).
    public i32 key_eq(u8* buf, i32 start, i32 end, string key) {
        i32 kl = str_bytes(key);
        if (end - start != kl) { return 0; }
        u8* kd = str_cstr(key);
        i32 i = 0;
        while (i < kl) {
            if (buf[start + i] != kd[i]) { return 0; }
            i = i + 1;
        }
        return 1;
    }

    // Persiste la config actual a .vexedrc.
    public void save() {
        i64 fp = this.io.open(".vexedrc", "wb");
        if (fp == 0) { return; }
        this.write_kv(fp, "# Config del editor VEXED (auto-generado)\n");
        this.write_kv_int(fp, "tab_width=", this.tab_width);
        this.write_kv_int(fp, "syntax_on=", this.syntax_on);
        this.write_kv_int(fp, "sidebar_width=", this.sidebar_width);
        this.write_kv_int(fp, "show_line_numbers=", this.show_line_numbers);
        this.io.flush_fp(fp);
        this.io.close(fp);
    }

    public void write_kv(i64 fp, string s) {
        i32 n = str_bytes(s);
        u8* p = str_cstr(s);
        this.io.write_bytes(fp, p, n);
    }

    public void write_kv_int(i64 fp, string key, i32 v) {
        this.write_kv(fp, key);
        // Convertir entero a string.
        u8* tmp = malloc(16);
        i32 tn  = 0;
        if (v == 0) {
            tmp[0] = 48;
            tn = 1;
        } else {
            i32 vv = v;
            i32 neg = 0;
            if (vv < 0) { neg = 1; vv = -vv; }
            while (vv > 0) {
                tmp[tn] = 48 + (vv % 10);
                tn = tn + 1;
                vv = vv / 10;
            }
            // reverse
            i32 a = 0;
            i32 b = tn - 1;
            while (a < b) {
                i32 t = tmp[a]; tmp[a] = tmp[b]; tmp[b] = t;
                a = a + 1; b = b - 1;
            }
            if (neg == 1) {
                i32 i = tn;
                while (i > 0) { tmp[i] = tmp[i - 1]; i = i - 1; }
                tmp[0] = 45;  // '-'
                tn = tn + 1;
            }
        }
        this.io.write_bytes(fp, tmp, tn);
        // Newline.
        u8* nl = malloc(1);
        nl[0] = 10;
        this.io.write_bytes(fp, nl, 1);
        free(tmp);
        free(nl);
    }
}
