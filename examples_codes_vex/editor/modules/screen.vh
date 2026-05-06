// =============================================================================
// modules/screen.vh - render TUI con escapes ANSI + tamano dinamico
// =============================================================================
//
// Toda la salida pasa por print()/println() del runtime con identificadores
// ANSI magicos (BOLD, RED, RESET, CLEAR_SCREEN, ...) que el frontend
// resuelve inline a su escape correspondiente.  Cero overhead vs strings
// literales.  El render es liberalmente flush()-eado por el Editor para
// asegurar visibilidad antes de bloquear en input.
//
// Tamano dinamico: en Windows consulta GetConsoleScreenBufferInfo via FFI
// a kernel32; el struct devuelto (CONSOLE_SCREEN_BUFFER_INFO, 22 bytes)
// contiene en el offset 12 el rect "srWindow" con (Left, Top, Right,
// Bottom) como i16 little-endian.  Ancho = Right-Left+1, alto = Bottom-Top+1.
// Si la API falla por cualquier motivo (no hay consola, redirected, etc.)
// se cae a 100x30 que es el default seguro para entornos sin TTY.

class Screen {
    public i64 lib_kernel32;
    public i64 sym_getstdhandle;
    public i64 sym_getbufinfo;
    public i64 hstdout;          // HANDLE stdout cacheado
    public i32 cols;
    public i32 rows;

    public Screen() {
        // FFI a kernel32 para consultar tamano de consola.  El handle
        // STD_OUTPUT_HANDLE = -11 (signed) -> 0xFFFFFFF5 cuando se castea.
        this.lib_kernel32     = ffi_open("kernel32.dll");
        this.sym_getstdhandle = ffi_sym(this.lib_kernel32, "GetStdHandle");
        this.sym_getbufinfo   = ffi_sym(this.lib_kernel32, "GetConsoleScreenBufferInfo");
        // STD_OUTPUT_HANDLE = -11.  ffi_call recibe i64; el ABI Win32 trunca
        // a DWORD (32 bits low) en la llamada, asi que pasamos el valor raw.
        this.hstdout          = ffi_call(this.sym_getstdhandle, -11);
        // Defaults seguros si la query falla.
        this.cols             = 100;
        this.rows             = 30;
        this.refresh_size();
    }

    // refresh_size: consulta GetConsoleScreenBufferInfo y actualiza cols/rows.
    // El struct CONSOLE_SCREEN_BUFFER_INFO ocupa 22 bytes:
    //   +0   COORD dwSize         (2 i16: cols, rows del buffer entero)
    //   +4   COORD dwCursorPos    (2 i16)
    //   +8   WORD wAttributes     (1 u16)
    //   +10  SMALL_RECT srWindow  (4 i16: Left, Top, Right, Bottom)
    //   +18  COORD dwMaxWindowSize(2 i16)
    // Usamos el rect srWindow porque es el VIEWPORT actual (lo que el
    // usuario ve), no el buffer de scrollback.
    public void refresh_size() {
        if (this.hstdout == 0) { return; }
        u8* info = malloc(22);
        // Cero por defecto para detectar si la API no escribio.
        i32 z = 0;
        while (z < 22) { info[z] = 0; z = z + 1; }
        i64 ok = ffi_call(this.sym_getbufinfo, this.hstdout, info);
        if (ok == 0) {
            free(info);
            return;
        }
        // Leer i16 LE en offset 10, 12, 14, 16.
        i32 left   = this.read_i16_le(info, 10);
        i32 top    = this.read_i16_le(info, 12);
        i32 right  = this.read_i16_le(info, 14);
        i32 bottom = this.read_i16_le(info, 16);
        i32 nc = right  - left + 1;
        i32 nr = bottom - top  + 1;
        // Sanity: tamanos absurdos -> mantener defaults.
        if (nc > 0 && nc < 1024) { this.cols = nc; }
        if (nr > 0 && nr < 1024) { this.rows = nr; }
        free(info);
    }

    // Helper: lee 2 bytes LE desde un buffer host como i16 con sign extend.
    public i32 read_i16_le(u8* buf, i32 off) {
        i32 lo = buf[off];
        i32 hi = buf[off + 1];
        i32 v  = (hi << 8) | lo;
        // sign extend de 16 a 32 bits (los SHORT de Win32 son signed).
        if ((v & 0x8000) != 0) { v = v | 0xFFFF0000; }
        return v;
    }

    public void clear() {
        print(CLEAR_SCREEN);
        print(CURSOR_HOME);
    }

    // Mueve el cursor a (row, col) 1-based segun convencion ANSI.
    public void move_cursor(i32 row, i32 col) {
        print("\x1b[${row};${col}H");
    }

    public void hide_cursor() { print("\x1b[?25l"); }
    public void show_cursor() { print("\x1b[?25h"); }
}
