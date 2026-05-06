// =============================================================================
// modules/editor_core.vh - clase Editor (orquestador) + main
// =============================================================================
//
// Orquesta InputBackend + Screen + Buffer + ExtensionRegistry + FileIO.
// El metodo run() es el ciclo "leer tecla, procesar, redibujar".
//
// Layout TUI:
//   Fila 1            : cabecera (nombre del editor + ayuda + last_key)
//   Filas 2..rows-2   : viewport del buffer (rows-3 lineas visibles)
//   Fila rows-1       : barra de mensajes (status_msg)
//   Fila rows         : barra de estado inferior (modo + posicion + scroll)
//
// El cursor se posiciona segun (cursor_row - viewport_top) + 2 para
// mantenerlo en pantalla.  Cuando el cursor sale del viewport,
// adjust_viewport() hace scroll para devolverlo a la parte visible.
//
// El sistema de extensiones se invoca via reflexion: al registrar una
// extension por nombre de clase, el Editor mantiene esa cadena viva en
// el ExtensionRegistry, y dispatch_ext_hook() recorre los slots
// invocando `forName + findmethod + callm` -- ver dispatch_hook.
//
// El destructor RAII de Buffer libera el malloc al salir del scope de
// run() (cuando ed::~Editor encadena ~Buffer via A.32).

class Editor {
    public InputBackend       input;
    public Screen             screen;
    public Buffer             buffer;
    public ExtensionRegistry  exts;
    public FileIO             io;
    public SyntaxVex          syntax;          // extension de coloreado Vex
    public string             filename;        // path actual (vacio si scratch)
    public string             status_msg;      // mensaje temporal en barra inferior
    public i32                running;         // 1 = sigue, 0 = sale
    public i32                last_key;        // ultima tecla pulsada (debug)
    public i32                save_count;      // numero de guardados realizados
    public i32                viewport_top;    // primera fila visible (0-based)

    public Editor() {
        this.input        = new InputBackend();
        this.screen       = new Screen();
        this.buffer       = new Buffer();
        this.exts         = new ExtensionRegistry();
        this.io           = new FileIO();
        this.syntax       = new SyntaxVex();
        this.filename     = "vexed_out.txt";
        this.status_msg   = "F1 ayuda  F2 syntax-toggle (WIP - bug regalloc)";
        this.running      = 1;
        this.last_key     = 0;
        this.save_count   = 0;
        this.viewport_top = 0;
    }

    // Carga un fichero al buffer.  Util al arrancar el editor con un
    // argumento (extension futura: leer argv via FFI a getargv).
    public void load_file(string path) {
        i32 ok = this.buffer.load_from_file(this.io, path);
        if (ok == 1) {
            this.filename     = path;
            this.viewport_top = 0;
            this.status_msg   = "Cargado: ${path}";
        } else {
            this.status_msg   = "ERROR: no se pudo abrir ${path}";
        }
    }

    // Guarda el buffer al `filename` actual via FileIO.
    public void save_file() {
        i32 ok = this.buffer.save_to_file(this.io, this.filename);
        if (ok == 1) {
            this.save_count = this.save_count + 1;
            this.status_msg = "Guardado en ${this.filename} (${this.save_count}x)";
        } else {
            this.status_msg = "ERROR al guardar ${this.filename}";
        }
    }

    // Numero de filas disponibles para el viewport (header + msg + status
    // ocupan 3 filas; el resto es contenido).
    public i32 viewport_rows() {
        i32 r = this.screen.rows - 3;
        if (r < 1) { r = 1; }
        return r;
    }

    // Ajusta viewport_top para que el cursor siga visible.  Llamarse
    // SIEMPRE antes de render_buffer y place_cursor.
    public void adjust_viewport() {
        i32 cr = this.buffer.cursor_row();
        i32 vh = this.viewport_rows();
        if (cr < this.viewport_top) {
            this.viewport_top = cr;
        }
        if (cr >= this.viewport_top + vh) {
            this.viewport_top = cr - vh + 1;
        }
        if (this.viewport_top < 0) { this.viewport_top = 0; }
    }

    public void render_header() {
        this.screen.move_cursor(1, 1);
        print(BOLD); print(CYAN);
        print("VEXED  ");
        print(RESET); print(YELLOW);
        print("Ctrl+Q salir | Ctrl+S guardar | F1 ayuda");
        print(RESET);
        print("    last_key=${this.last_key}");
        print("\x1b[K");
    }

    // render_buffer: solo dibuja la ventana visible [viewport_top, viewport_top+vh).
    // Escanea el buffer por bytes pero salta las primeras viewport_top
    // lineas, e imprime caracteres de las siguientes vh hasta encontrar
    // un LF (que cierra la fila) o agotar viewport.  El truncado a
    // screen.cols evita que lineas largas reescriban el header en
    // terminales sin word-wrap.
    //
    // Para cada linea fisica visible se invoca primero el hook
    // on_render_line(row, line_text) de cada extension; si la extension
    // imprime ella misma la linea (por ejemplo con syntax highlighting)
    // devuelve 1 y saltamos el render por defecto.  Devolver 0 = render
    // estandar.  Esta es la API que usara la extension de syntax Vex.
    public void render_buffer() {
        i32 vh   = this.viewport_rows();
        i32 cols = this.screen.cols;
        // 1) Saltar viewport_top lineas en el buffer.
        i32 off = this.buffer.offset_of_row(this.viewport_top);
        // 2) Dibujar hasta vh filas visibles.
        i32 vrow = 0;        // fila visible (0..vh-1)
        i32 col  = 0;        // columna dentro de la linea fisica
        // Mover al inicio de la zona de edicion.
        this.screen.move_cursor(2, 1);
        // Buffer global de this.buffer cacheado en variables locales para
        // no encadenar this.buffer.X.Y en cada iteracion (mejor regalloc).
        i32  blen = this.buffer.length;
        u8*  bdat = this.buffer.data;
        while (vrow < vh && off < blen) {
            // Si una extension renderiza la linea completa, saltamos el
            // render por defecto pero igual avanzamos `off` hasta el LF.
            i32 handled = this.dispatch_render_line(vrow, off);
            if (handled == 0) {
                // Render estandar: caracter a caracter hasta LF/EOB/cols.
                while (off < blen && bdat[off] != 10) {
                    if (col < cols) {
                        print_char(bdat[off]);
                    }
                    off = off + 1;
                    col = col + 1;
                }
            } else {
                // Extension consumio la linea: saltar hasta el LF/EOB.
                while (off < blen && bdat[off] != 10) {
                    off = off + 1;
                }
            }
            print("\x1b[K");
            if (off < blen) { off = off + 1; }   // saltar el LF
            vrow = vrow + 1;
            col  = 0;
            if (vrow < vh) { this.screen.move_cursor(vrow + 2, 1); }
        }
        // 3) Rellenar las filas restantes con tildes (estilo vim/nano).
        while (vrow < vh) {
            this.screen.move_cursor(vrow + 2, 1);
            print(DIM); print("~"); print(RESET);
            print("\x1b[K");
            vrow = vrow + 1;
        }
    }

    // Despacha el hook on_render_line a la extension activa.  El modelo
    // "extension cargada via loadmodule + reflexion" requiere builtins
    // de Vex que hoy no existen (getMethod / invoke / newInstance), asi
    // que el syntax highlighter es una extension compile-time integrada
    // como modulo .vh.  Esta funcion retorna 1 si la extension imprimio
    // la linea (y el render por defecto debe omitirse), 0 en caso contrario.
    public i32 dispatch_render_line(i32 vrow, i32 line_start_off) {
        if (this.syntax.enabled == 0) { return 0; }
        // Calcular line_end = posicion del LF o fin de buffer.
        i32 blen  = this.buffer.length;
        u8* bdat  = this.buffer.data;
        i32 le    = line_start_off;
        while (le < blen && bdat[le] != 10) { le = le + 1; }
        // Render coloreado de la linea.
        this.syntax.render_line(bdat, line_start_off, le, this.screen.cols);
        return 1;
    }

    public void render_message_bar() {
        // Fila rows-1: mensaje temporal.
        this.screen.move_cursor(this.screen.rows - 1, 1);
        print("\x1b[K");
        print(this.status_msg);
    }

    public void render_status() {
        // Fila rows: barra inferior con info estable.
        this.screen.move_cursor(this.screen.rows, 1);
        print(REVERSE);
        print(" ${this.filename}  ");
        i32 r  = this.buffer.cursor_row();
        i32 c  = this.buffer.cursor_col();
        i32 lc = this.buffer.line_count();
        print("L${r + 1}:C${c + 1}");
        print("  lineas=${lc}");
        if (this.buffer.dirty == 1) { print("  [modificado]"); }
        print("  exts=${this.exts.ext_count}");
        print("  view=${this.viewport_top + 1}");
        print("  ${this.screen.cols}x${this.screen.rows}");
        print("\x1b[K");
        print(RESET);
    }

    public void place_cursor() {
        i32 r = this.buffer.cursor_row();
        i32 c = this.buffer.cursor_col();
        i32 vr = r - this.viewport_top;
        i32 vc = c;
        if (vc >= this.screen.cols) { vc = this.screen.cols - 1; }
        this.screen.move_cursor(vr + 2, vc + 1);
    }

    public void render() {
        this.screen.refresh_size();
        this.adjust_viewport();
        this.screen.hide_cursor();
        this.render_header();
        this.render_buffer();
        this.render_message_bar();
        this.render_status();
        this.place_cursor();
        this.screen.show_cursor();
        flush();
    }

    public void handle_key(i32 k) {
        this.last_key = k;
        if (k == KEY_CTRL_Q) { this.running = 0;          return; }
        if (k == KEY_CTRL_S) { this.save_file();          return; }
        if (k == KEY_LEFT)   { this.buffer.move_left();   return; }
        if (k == KEY_RIGHT)  { this.buffer.move_right();  return; }
        if (k == KEY_UP)     { this.buffer.move_up();     return; }
        if (k == KEY_DOWN)   { this.buffer.move_down();   return; }
        if (k == KEY_HOME)   { this.buffer.move_home();   return; }
        if (k == KEY_END)    { this.buffer.move_end();    return; }
        if (k == KEY_DEL)    { this.buffer.del_char();    return; }
        if (k == KEY_PGUP)   { this.buffer.page_up(this.viewport_rows());   return; }
        if (k == KEY_PGDN)   { this.buffer.page_down(this.viewport_rows()); return; }
        if (k == KEY_ENTER)  { this.buffer.newline();     return; }
        if (k == KEY_BS)     { this.buffer.backspace();   return; }
        if (k == KEY_F1)     { this.show_help();          return; }
        if (k == KEY_F2)     {
            // Toggle syntax highlighting.
            if (this.syntax.enabled == 1) {
                this.syntax.enabled = 0;
                this.status_msg = "Syntax OFF";
            } else {
                this.syntax.enabled = 1;
                this.status_msg = "Syntax ON";
            }
            return;
        }
        if (k == KEY_TAB)    {
            // Insertar 4 espacios (indent suave).
            this.buffer.insert_char(32);
            this.buffer.insert_char(32);
            this.buffer.insert_char(32);
            this.buffer.insert_char(32);
            return;
        }
        if (k >= 32 && k <= 126) {
            this.buffer.insert_char(k);
            return;
        }
    }

    public void show_help() {
        this.status_msg = "Ctrl+Q=salir Ctrl+S=guardar  Flechas/Home/End/PgUp/PgDn  Tab=4sp  F1=ayuda";
    }

    public void run() {
        this.screen.clear();
        this.render();
        while (this.running == 1) {
            i32 k = this.input.read_key_blocking();
            this.handle_key(k);
            this.render();
        }
        this.screen.clear();
        this.screen.show_cursor();
        print("Hasta luego desde VEXED.\n");
        flush();
    }
}
