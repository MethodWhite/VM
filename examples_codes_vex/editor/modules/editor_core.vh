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

// =============================================================================
// UndoState: snapshot del buffer para undo/redo (linked list).
// =============================================================================
// Cada nodo guarda una copia COMPLETA del buffer como `string` (StringObject
// GC-managed, eficiente para textos pequenos-medianos).  Mantenemos dos
// pilas (undo + redo) en el Editor; cada operacion modificadora hace push
// a la pila undo antes de aplicar el cambio, y limpia la pila redo.
//
// Coste: O(N) memoria por snapshot (N = bytes del buffer).  Limitamos a
// MAX_UNDO=64 entradas; al exceder, soltamos el snapshot mas antiguo
// (cola del linked list).  Para archivos < 100 KB el coste total es
// < 6.4 MB, aceptable en una sesion de edicion normal.

class UndoState {
    public string    snap;        // contenido completo del buffer (StringObject GC-managed)
    public i32       cursor;
    public i32       sel_anchor;
    public UndoState next;

    public UndoState() {
        this.snap       = "";
        this.cursor     = 0;
        this.sel_anchor = -1;
        this.next       = null;
    }
}

// =============================================================================
// Tab: estado por-buffer (multi-document).  El Editor mantiene un linked
// list de Tabs y promueve el estado del "tab activo" a sus campos
// `buffer/filename/save_count/viewport_top/sel_anchor/undo_*` durante la
// ejecucion.  Al conmutar de tab, hace save_to_tab() + load_from_tab() para
// trasvasar el estado.
// =============================================================================
class Tab {
    public Buffer    buffer;
    public string    filename;
    public i32       save_count;
    public i32       viewport_top;
    public i32       sel_anchor;
    public UndoState undo_top;
    public UndoState redo_top;
    public i32       undo_count;
    public Tab       next;       // siguiente tab en el linked list (orden de creacion)

    public Tab() {
        this.buffer       = new Buffer();
        this.filename     = "scratch.vex";
        this.save_count   = 0;
        this.viewport_top = 0;
        this.sel_anchor   = -1;
        this.undo_top     = null;
        this.redo_top     = null;
        this.undo_count   = 0;
        this.next         = null;
    }
}

class Editor {
    public InputBackend       input;
    public Screen             screen;
    public Buffer             buffer;
    public ExtensionRegistry  exts;
    public FileIO             io;
    public string             filename;        // path actual (vacio si scratch)
    public string             status_msg;      // mensaje temporal en barra inferior
    public i32                running;         // 1 = sigue, 0 = sale
    public i32                last_key;        // ultima tecla pulsada (debug)
    public i32                save_count;      // numero de guardados realizados
    public i32                viewport_top;    // primera fila visible (0-based)
    public i32                syntax_on;       // 1 = render via extension, 0 = render por defecto
    public string             clipboard;       // ultimo Ctrl+K / Ctrl+C / Ctrl+X
    public string             last_search;     // ultimo termino buscado (F3 lo reusa)
    public i32                sel_anchor;      // -1 = sin seleccion; >=0 = offset pivote
    public UndoState          undo_top;        // pila de undo (snapshots LIFO)
    public UndoState          redo_top;        // pila de redo (snapshots LIFO)
    public i32                undo_count;      // numero de entradas en undo (cap a 64)
    public Tab                tab_head;        // primer tab del linked list
    public Tab                tab_active;      // tab actualmente visible
    public i32                tab_count;       // numero de tabs activos

    public FileExplorer       fexp;            // listado de directorio para Ctrl+O
    public Config             config;          // configuracion persistida en .vexedrc
    public Sidebar            sidebar;         // panel lateral con arbol de archivos (Ctrl+B)
    public Autocomplete       autocomplete;    // popup de autocompletado (estilo vnano)
    public MultiCursor        mcursors;        // cursores adicionales (multi-edit vertical)
    // Multi-panel split (vertical, dos paneles del mismo buffer con
    // viewport independiente).  panel_split=0 -> single panel (default),
    // 1 -> split_v.  panel_focus=0 panel izquierdo, 1 derecho.  El
    // viewport_top "principal" (this.viewport_top) es el del panel
    // izquierdo en split mode; el derecho lleva su propio.
    public i32                panel_split;     // 0=single, 1=split_v
    public i32                panel_focus;     // 0=left, 1=right
    public i32                viewport_top_right; // viewport del panel derecho
    public i64                dlog_fp;         // file handle para debug log (0 = disabled)
    public i32                dlog_seq;        // contador de mensajes para timestamping

    public Editor() {
        this.input        = new InputBackend();
        this.screen       = new Screen();
        this.exts         = new ExtensionRegistry();
        this.io           = new FileIO();
        this.fexp         = new FileExplorer();
        this.config       = new Config();
        this.config.load();   // cargar .vexedrc del cwd si existe
        this.sidebar      = new Sidebar(this.screen);
        this.sidebar.width = this.config.sidebar_width;
        this.autocomplete = new Autocomplete();
        this.mcursors     = new MultiCursor();
        this.panel_split        = 0;
        this.panel_focus        = 0;
        this.viewport_top_right = 0;
        // Refresh diferido a render: ejecutarlo aqui en el ctor crasheaba
        // por una interaccion GC + FileExplorer.list_current_dir() (str_make
        // sobre host_ptr durante la cadena de allocaciones del setup
        // inicial).  El primer render llama a refresh() solo cuando
        // visible == 1 && root_list == null (1 vez por sesion).
        this.dlog_fp      = 0;     // dlog_open() lo activa cuando se desee
        this.dlog_seq     = 0;
        // Inicializar el tab activo (con su Buffer interno).
        this.tab_head     = new Tab();
        this.tab_active   = this.tab_head;
        this.tab_count    = 1;
        // Promover el estado del tab activo a campos planos del Editor.
        // Estos shadows se sincronizan en switch_to_tab() y antes/despues
        // de operaciones que tocan multiples tabs.
        this.buffer       = this.tab_active.buffer;
        this.filename     = this.tab_active.filename;
        this.save_count   = this.tab_active.save_count;
        this.viewport_top = this.tab_active.viewport_top;
        this.sel_anchor   = this.tab_active.sel_anchor;
        this.undo_top     = this.tab_active.undo_top;
        this.redo_top     = this.tab_active.redo_top;
        this.undo_count   = this.tab_active.undo_count;
        // Estado global no por-tab.
        this.status_msg   = "Ctrl+Q salir | Ctrl+S guardar | Ctrl+B sidebar | F1 ayuda";
        this.running      = 1;
        this.last_key     = 0;
        // Calcular el basename de la carpeta del proyecto para mostrarlo
        // en el header (estilo vnano: "Proy: <basename>").
        this.refresh_project_basename();
        // syntax_on viene de Config (.vexedrc): default 1, override-able via
        // `syntax_on=0` en el rc.  F2 lo toggle en runtime sin persistir.
        this.syntax_on    = this.config.syntax_on;
        this.clipboard    = "";
        this.last_search  = "";
    }

    // -------------------- DEBUG LOG --------------------
    // Sistema de logging activable.  Cuando dlog_fp != 0, los puntos
    // criticos del editor (run loop, render, handle_key, read_key)
    // emiten una linea al log.  El log se flush'ea tras cada escritura
    // para sobrevivir a un crash.  Activar via dlog_open(path) desde main.

    // Recorre la lista de tabs y devuelve 1 si alguno tiene buffer dirty.
    // Usado por Ctrl+Q para preguntar antes de salir cuando hay cambios
    // sin guardar (igual que vnano).  Asegurarse de sincronizar el
    // tab_active.buffer.dirty desde this.buffer.dirty si la implementacion
    // de switch_to_tab no lo hace -- aqui asumimos que cada Tab.buffer
    // tiene su flag dirty consistente.
    public i32 any_tab_modified() {
        Tab t = this.tab_head;
        while (t != null) {
            if (t == this.tab_active) {
                // El buffer activo puede tener dirty en t.buffer o this.buffer
                // (son la misma instancia por la promotion en switch_to_tab).
                if (this.buffer.dirty == 1) { return 1; }
            } else {
                if (t.buffer.dirty == 1) { return 1; }
            }
            t = t.next;
        }
        return 0;
    }

    // Calcula el basename de la cwd y lo guarda en config.project_basename.
    // Llamado al init + tras cada chdir (prompt_project_folder).  El
    // basename es la ultima componente del path; si no hay '/' o '\\',
    // devuelve el path entero.
    public void refresh_project_basename() {
        u8* buf = malloc(1024);
        i64 r   = this.io.getcwd(buf, 1024);
        if (r == 0) { free(buf); this.config.project_basename = "?"; return; }
        // Encontrar el ULTIMO separador (/ o \).
        i32 len = 0;
        while (len < 1024 && buf[len] != 0) { len = len + 1; }
        i32 last_sep = -1;
        i32 i = 0;
        while (i < len) {
            if (buf[i] == 47 || buf[i] == 92) { last_sep = i; }
            i = i + 1;
        }
        if (last_sep < 0 || last_sep == len - 1) {
            this.config.project_basename = str_make(buf, len);
        } else {
            i32 start = last_sep + 1;
            this.config.project_basename = str_make(buf + start, len - start);
        }
        free(buf);
    }

    public void dlog_open(string path) {
        if (this.dlog_fp != 0) { this.dlog_close(); }
        this.dlog_fp = this.io.open(path, "w");
    }

    public void dlog_close() {
        if (this.dlog_fp != 0) {
            this.io.close(this.dlog_fp);
            this.dlog_fp = 0;
        }
    }

    // Escribe `msg` + '\n' al log + flush.  No-op si log no esta abierto.
    public void dlog(string msg) {
        if (this.dlog_fp == 0) { return; }
        this.dlog_seq = this.dlog_seq + 1;
        i32 n = str_bytes(msg);
        u8* p = str_cstr(msg);
        // Prefijo de secuencia: "[NNN] "
        u8* pre = malloc(8);
        i32 seq = this.dlog_seq;
        pre[0] = 91;          // '['
        pre[1] = 48 + ((seq / 1000) % 10);
        pre[2] = 48 + ((seq /  100) % 10);
        pre[3] = 48 + ((seq /   10) % 10);
        pre[4] = 48 + ( seq         % 10);
        pre[5] = 93;          // ']'
        pre[6] = 32;          // ' '
        pre[7] = 0;
        this.io.write_bytes(this.dlog_fp, pre, 7);
        this.io.write_bytes(this.dlog_fp, p, n);
        // Newline
        u8* nl = malloc(1);
        nl[0] = 10;
        this.io.write_bytes(this.dlog_fp, nl, 1);
        this.io.flush_fp(this.dlog_fp);
        free(pre);
        free(nl);
    }

    // Variante con un i32 anyadido al final (formato "msg=N").
    public void dlog_i(string msg, i32 v) {
        if (this.dlog_fp == 0) { return; }
        this.dlog(msg);
        // Convertir v a string decimal y escribirlo via vio_print_int
        // requeriria capturar stdout; en su lugar emitimos el int como
        // texto de digitos directamente al log.  Reusamos dlog para
        // sequencer + flush.
        u8* buf = malloc(32);
        i32 n = 0;
        i32 vv = v;
        i32 neg = 0;
        if (vv < 0) { neg = 1; vv = -vv; }
        if (vv == 0) {
            buf[n] = 48; n = n + 1;
        } else {
            i32 start = n;
            while (vv > 0) {
                buf[n] = 48 + (vv % 10);
                n = n + 1;
                vv = vv / 10;
            }
            // reverse
            i32 a = start;
            i32 b = n - 1;
            while (a < b) {
                i32 t = buf[a]; buf[a] = buf[b]; buf[b] = t;
                a = a + 1; b = b - 1;
            }
        }
        if (neg == 1) {
            i32 i = n;
            while (i > 0) { buf[i] = buf[i - 1]; i = i - 1; }
            buf[0] = 45; // '-'
            n = n + 1;
        }
        u8* nl = malloc(1);
        nl[0] = 10;
        this.io.write_bytes(this.dlog_fp, buf, n);
        this.io.write_bytes(this.dlog_fp, nl, 1);
        this.io.flush_fp(this.dlog_fp);
        free(buf);
        free(nl);
    }

    // -------------------- TABS --------------------
    // Persiste el estado transient del Editor a la entrada `tab_active`.
    public void save_to_active_tab() {
        if (this.tab_active == null) { return; }
        this.tab_active.buffer       = this.buffer;
        this.tab_active.filename     = this.filename;
        this.tab_active.save_count   = this.save_count;
        this.tab_active.viewport_top = this.viewport_top;
        this.tab_active.sel_anchor   = this.sel_anchor;
        this.tab_active.undo_top     = this.undo_top;
        this.tab_active.redo_top     = this.redo_top;
        this.tab_active.undo_count   = this.undo_count;
    }

    public void load_from_active_tab() {
        this.buffer       = this.tab_active.buffer;
        this.filename     = this.tab_active.filename;
        this.save_count   = this.tab_active.save_count;
        this.viewport_top = this.tab_active.viewport_top;
        this.sel_anchor   = this.tab_active.sel_anchor;
        this.undo_top     = this.tab_active.undo_top;
        this.redo_top     = this.tab_active.redo_top;
        this.undo_count   = this.tab_active.undo_count;
    }

    public void switch_to_tab(Tab t) {
        if (t == null || t == this.tab_active) { return; }
        this.save_to_active_tab();
        this.tab_active = t;
        this.load_from_active_tab();
        this.status_msg = "Tab: ${this.filename}";
    }

    // Ctrl+N: crear un nuevo tab vacio y conmutar a el.  El tab nuevo se
    // anexa al final del linked list para preservar el orden de creacion.
    public void cmd_new_tab() {
        this.save_to_active_tab();
        Tab t = new Tab();
        // Append al final.
        Tab p = this.tab_head;
        while (p.next != null) { p = p.next; }
        p.next = t;
        this.tab_count = this.tab_count + 1;
        this.tab_active = t;
        this.load_from_active_tab();
        this.status_msg = "Nuevo tab (${this.tab_count} abiertos)";
    }

    // Ctrl+W: cerrar el tab activo.  Si es el ultimo, abre uno nuevo
    // vacio para mantener al menos un tab vivo.  Si tiene cambios sin
    // guardar (dirty=1), pide confirmacion en status bar.
    public void cmd_close_tab() {
        if (this.tab_active == null) { return; }
        if (this.tab_active.buffer.dirty == 1) {
            this.status_msg = "Cambios sin guardar; Ctrl+S primero (o cerrar de nuevo)";
            this.tab_active.buffer.dirty = 0;     // segundo Ctrl+W sin Ctrl+S cierra
            return;
        }
        // Encontrar el predecesor en el linked list.
        Tab prev = null;
        Tab cur  = this.tab_head;
        while (cur != null && cur != this.tab_active) {
            prev = cur;
            cur  = cur.next;
        }
        if (cur == null) { return; }   // sanity
        Tab next_tab = cur.next;
        if (prev == null) {
            this.tab_head = next_tab;
        } else {
            prev.next = next_tab;
        }
        this.tab_count = this.tab_count - 1;
        if (this.tab_count == 0) {
            // Mantener invariante: siempre hay >= 1 tab.  Crear uno vacio.
            Tab fresh = new Tab();
            this.tab_head   = fresh;
            this.tab_active = fresh;
            this.tab_count  = 1;
            this.load_from_active_tab();
            this.status_msg = "Tab cerrado; nuevo scratch creado";
            return;
        }
        // Conmutar a `next_tab` si existe; si no (cerramos el ultimo del list),
        // ir al previo.
        if (next_tab != null) {
            this.tab_active = next_tab;
        } else {
            this.tab_active = prev;
        }
        this.load_from_active_tab();
        this.status_msg = "Tab cerrado (${this.tab_count} restantes)";
    }

    // F4 / F5: ciclar entre tabs.
    public void cmd_next_tab() {
        if (this.tab_count <= 1) { return; }
        Tab t = this.tab_active.next;
        if (t == null) { t = this.tab_head; }   // wrap-around
        this.switch_to_tab(t);
    }

    public void cmd_prev_tab() {
        if (this.tab_count <= 1) { return; }
        // Encontrar el tab anterior (si tab_active es head, ir al ultimo).
        Tab prev = null;
        Tab cur  = this.tab_head;
        while (cur != null && cur != this.tab_active) {
            prev = cur;
            cur  = cur.next;
        }
        if (prev == null) {
            // tab_active es head: ir al ultimo.
            prev = this.tab_head;
            while (prev.next != null) { prev = prev.next; }
        }
        this.switch_to_tab(prev);
    }

    // -------------------- UNDO / REDO --------------------
    // Snapshot del estado actual (buffer + cursor + sel_anchor).  Tras Bug A
    // fix, str_make detecta automaticamente el host_ptr de buffer.data y
    // copia los bytes via strmake_h, garantizando un StringObject correcto.
    public UndoState take_snapshot() {
        UndoState st = new UndoState();
        st.snap       = str_make(this.buffer.data, this.buffer.length);
        st.cursor     = this.buffer.cursor;
        st.sel_anchor = this.sel_anchor;
        return st;
    }

    // Restaura el buffer + cursor + sel_anchor desde una snapshot.
    public void apply_snapshot(UndoState st) {
        i32 n   = str_bytes(st.snap);
        u8* src = str_cstr(st.snap);
        if (this.buffer.data != null) { free(this.buffer.data); }
        i32 cap = n + 1;
        if (cap < 64) { cap = 64; }
        u8* nbuf = malloc(cap);
        i32 i = 0;
        while (i < n) {
            nbuf[i] = src[i];
            i = i + 1;
        }
        nbuf[n] = 0;
        this.buffer.data     = nbuf;
        this.buffer.length   = n;
        this.buffer.capacity = cap;
        if (this.buffer.cursor > n) { this.buffer.cursor = n; }
        else                         { this.buffer.cursor = st.cursor; }
        this.buffer.dirty    = 1;
        this.sel_anchor      = st.sel_anchor;
        if (this.sel_anchor > n) { this.sel_anchor = -1; }
    }

    // Push del estado ACTUAL a la pila undo (antes de modificar).  Limpia
    // la pila redo porque cualquier nueva edicion invalida los snapshots
    // de redo.  Cap a 64 entradas: al exceder, soltamos el mas antiguo.
    public void push_undo() {
        UndoState snap = this.take_snapshot();
        snap.next      = this.undo_top;
        this.undo_top  = snap;
        this.undo_count = this.undo_count + 1;
        // Drop del mas antiguo si excedemos el cap.  Recorremos hasta el
        // penultimo y partimos el .next del cola.
        if (this.undo_count > 64) {
            UndoState p = this.undo_top;
            i32 i = 0;
            while (i < 62 && p.next != null) {
                p = p.next;
                i = i + 1;
            }
            // p ahora esta en posicion 62; p.next es la 63 (la que dropearemos).
            p.next = null;
            this.undo_count = 64;
        }
        // Cualquier nueva edicion invalida la pila redo.
        this.redo_top = null;
    }

    public void cmd_undo() {
        if (this.undo_top == null) {
            this.status_msg = "nada que deshacer";
            return;
        }
        // Snapshot del estado ACTUAL (post-edicion) -> pila redo.
        UndoState cur = this.take_snapshot();
        cur.next = this.redo_top;
        this.redo_top = cur;
        // Pop del top de undo y aplicar.
        UndoState st = this.undo_top;
        this.undo_top = st.next;
        if (this.undo_count > 0) { this.undo_count = this.undo_count - 1; }
        this.apply_snapshot(st);
        this.status_msg = "undo";
    }

    public void cmd_redo() {
        if (this.redo_top == null) {
            this.status_msg = "nada que rehacer";
            return;
        }
        // Snapshot del estado ACTUAL -> pila undo (sin tocar redo aqui).
        UndoState cur = this.take_snapshot();
        cur.next = this.undo_top;
        this.undo_top = cur;
        this.undo_count = this.undo_count + 1;
        if (this.undo_count > 64) { this.undo_count = 64; }
        // Pop del top de redo y aplicar.
        UndoState st = this.redo_top;
        this.redo_top = st.next;
        this.apply_snapshot(st);
        this.status_msg = "redo";
    }

    // Inicia o extiende la seleccion: si todavia no hay anchor, lo fija en
    // el offset actual del cursor (antes de moverse).  El llamador debe
    // ejecutar el movimiento DESPUES de invocar este helper.
    public void sel_begin_or_extend() {
        if (this.sel_anchor < 0) {
            this.sel_anchor = this.buffer.cursor;
        }
    }

    public void sel_clear() {
        this.sel_anchor = -1;
    }

    // Devuelve [lo, hi) ordenados.  Si no hay seleccion, lo == hi.
    public i32 sel_lo() {
        if (this.sel_anchor < 0) { return this.buffer.cursor; }
        i32 a = this.sel_anchor;
        i32 b = this.buffer.cursor;
        if (a < b) { return a; }
        return b;
    }

    public i32 sel_hi() {
        if (this.sel_anchor < 0) { return this.buffer.cursor; }
        i32 a = this.sel_anchor;
        i32 b = this.buffer.cursor;
        if (a > b) { return a; }
        return b;
    }

    public i32 has_selection() {
        if (this.sel_anchor < 0) { return 0; }
        if (this.sel_anchor == this.buffer.cursor) { return 0; }
        return 1;
    }

    // Borra la region seleccionada.  Tras esto la seleccion queda vacia
    // y el cursor esta en el offset bajo.
    public void sel_delete() {
        if (this.has_selection() == 0) { return; }
        i32 lo = this.sel_lo();
        i32 hi = this.sel_hi();
        this.buffer.cursor = hi;
        i32 n = hi - lo;
        i32 i = 0;
        while (i < n) {
            this.buffer.backspace();
            i = i + 1;
        }
        this.sel_anchor = -1;
    }

    // Copia la region seleccionada al clipboard.  Tras Bug A fix, str_make
    // detecta automaticamente que buffer.data es host_ptr y emite strmake_h.
    public void sel_copy() {
        if (this.has_selection() == 0) {
            this.status_msg = "sin seleccion para copiar";
            return;
        }
        i32 lo = this.sel_lo();
        i32 hi = this.sel_hi();
        i32 n = hi - lo;
        this.clipboard = str_make(this.buffer.data + lo, n);
        this.status_msg = "Copiado: ${n} chars";
    }

    // Cut = copy + delete.
    public void sel_cut() {
        if (this.has_selection() == 0) {
            this.status_msg = "sin seleccion para cortar";
            return;
        }
        this.sel_copy();
        this.sel_delete();
    }

    // ----- Prompt en status bar: lee una linea del usuario -----
    // Muestra `label` en la fila inferior y va echando los chars escritos.
    // Termina con Enter (devuelve string), Esc (devuelve "" + status="cancel").
    // Backspace borra el ultimo char.  Buffer max 255 chars (cabe una linea).
    public string read_prompt(string label) {
        u8* buf = malloc(256);
        i32 len = 0;
        i32 done = 0;
        i32 ok = 0;
        while (done == 0) {
            this.screen.move_cursor(this.screen.rows - 1, 1);
            print("\x1b[K");
            print(label);
            i32 i = 0;
            while (i < len) {
                print_char(buf[i]);
                i = i + 1;
            }
            this.screen.show_cursor();
            flush();
            i32 k = this.input.read_key_blocking();
            if (k == KEY_ESC) {
                done = 1; ok = 0;
            } else {
                if (k == KEY_ENTER) {
                    done = 1; ok = 1;
                } else {
                    if (k == KEY_BS) {
                        if (len > 0) { len = len - 1; }
                    } else {
                        if (k >= 32 && k <= 126 && len < 255) {
                            buf[len] = k;
                            len = len + 1;
                        }
                    }
                }
            }
        }
        string result = "";
        if (ok == 1) {
            result = str_make(buf, len);
        }
        free(buf);
        return result;
    }

    // Parser entero positivo decimal.  Devuelve -1 si invalido.
    public i32 parse_positive_int(string s) {
        i32 n = str_bytes(s);
        if (n == 0) { return -1; }
        u8* p = str_cstr(s);
        i32 r = 0;
        i32 i = 0;
        while (i < n) {
            i32 c = p[i];
            if (c < 48 || c > 57) { return -1; }
            r = r * 10 + (c - 48);
            i = i + 1;
        }
        return r;
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
        // Determinar cual viewport sigue al cursor: en split mode, el del
        // panel enfocado.  En single, siempre viewport_top.
        if (this.panel_split == 1 && this.panel_focus == 1) {
            // El cursor sigue al viewport derecho.
            if (cr < this.viewport_top_right) {
                this.viewport_top_right = cr;
            }
            if (cr >= this.viewport_top_right + vh) {
                this.viewport_top_right = cr - vh + 1;
            }
            if (this.viewport_top_right < 0) { this.viewport_top_right = 0; }
        } else {
            if (cr < this.viewport_top) {
                this.viewport_top = cr;
            }
            if (cr >= this.viewport_top + vh) {
                this.viewport_top = cr - vh + 1;
            }
            if (this.viewport_top < 0) { this.viewport_top = 0; }
        }
    }

    public void render_header() {
        // Header estilo vnano: barra reverse a toda la anchura con
        //   " vexed 0.5   <filename>[ *]  [N cursores]?  [sidebar]?  Proy: <basename>?  | tabs..."
        // Las decoraciones opcionales (sidebar, proyecto) solo aparecen
        // cuando aplican; el resto se padea para que el reverse llene
        // toda la fila.
        this.screen.move_cursor(1, 1);
        print(REVERSE); print(BOLD);
        // Titulo principal con prefijo de espacio (estetica vnano).
        print(" vexed 0.5  ");
        // Nombre del archivo activo + marcador "modificado".
        i32 dirty = 0;
        if (this.tab_active.buffer.dirty == 1) { dirty = 1; }
        print(this.tab_active.filename);
        if (dirty == 1) { print(" * "); } else { print("   "); }
        // Indicador [sidebar] cuando el foco esta alli.
        if (this.sidebar.visible == 1 && this.sidebar.focused == 1) {
            print("[sidebar]  ");
        }
        // Indicador Proy: cuando se conoce la carpeta del proyecto
        // (siempre se conoce; la status la setea prompt_project_folder).
        // Mostramos el basename para no agotar el ancho.
        print("Proy: ");
        print(this.config.project_basename);
        print("  ");
        // Lista compacta de tabs: activo en BOLD+REVERSE-inverso, otros en DIM.
        Tab t = this.tab_head;
        i32 idx = 1;
        while (t != null) {
            if (t == this.tab_active) {
                print(BOLD);
                print("[${idx}:${t.filename}]");
                print(RESET); print(REVERSE); print(BOLD);
            } else {
                print(DIM);
                print(" ${idx}:${t.filename} ");
                print(RESET); print(REVERSE); print(BOLD);
            }
            t = t.next;
            idx = idx + 1;
        }
        // Limpiar hasta fin de linea para que el REVERSE llene la fila.
        print("\x1b[K");
        print(RESET);
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
    // Dibuja la barra vertical entre dos paneles split_v en la columna col.
    public void render_split_separator(i32 col) {
        i32 r  = 2;
        i32 vh = this.viewport_rows();
        while (r < 2 + vh) {
            this.screen.move_cursor(r, col);
            print(DIM); print("|"); print(RESET);
            r = r + 1;
        }
    }

    // Render del buffer en el modo single-panel: usa la columna y viewport
    // por defecto (this.editor_col_offset(), this.viewport_top).  En split
    // mode el render() llama directamente a render_buffer_at(col, cols, vp)
    // dos veces (una por panel) para dibujar las dos vistas.
    public void render_buffer() {
        i32 col_off = this.editor_col_offset();
        i32 cols    = this.screen.cols - col_off + 1;
        if (cols < 1) { cols = 1; }
        this.render_buffer_at(col_off, cols, this.viewport_top);
    }

    // Render parametrizado: dibuja el buffer en el rectangulo definido por
    // (col_off, cols) horizontalmente y filas [vptop, vptop+vh) verticalmente.
    // Usado por render_buffer() (single) y por render() en split_v.
    public void render_buffer_at(i32 col_off, i32 cols, i32 vptop) {
        i32 vh   = this.viewport_rows();
        if (cols < 1) { cols = 1; }
        // 1) Saltar vptop lineas en el buffer.
        i32 off = this.buffer.offset_of_row(vptop);
        // 2) Dibujar hasta vh filas visibles.
        i32 vrow = 0;        // fila visible (0..vh-1)
        i32 col  = 0;        // columna dentro de la linea fisica
        // Mover al inicio de la zona de edicion.
        this.screen.move_cursor(2, col_off);
        // Buffer global de this.buffer cacheado en variables locales para
        // no encadenar this.buffer.X.Y en cada iteracion (mejor regalloc).
        i32  blen = this.buffer.length;
        u8*  bdat = this.buffer.data;
        // Rango de seleccion para resaltado con ANSI REVERSE.
        i32  s_lo = this.sel_lo();
        i32  s_hi = this.sel_hi();
        i32  has_sel = this.has_selection();
        i32  in_sel = 0;     // 1 = estamos dentro del rango selecionado
        while (vrow < vh && off < blen) {
            // Si una extension renderiza la linea completa, saltamos el
            // render por defecto pero igual avanzamos `off` hasta el LF.
            i32 handled = this.dispatch_render_line(vrow, off);
            if (handled == 0) {
                // Render estandar: caracter a caracter hasta LF/EOB/cols.
                while (off < blen && bdat[off] != 10) {
                    if (has_sel == 1) {
                        if (in_sel == 0 && off >= s_lo && off < s_hi) {
                            print(REVERSE);
                            in_sel = 1;
                        } else if (in_sel == 1 && off >= s_hi) {
                            print(RESET);
                            in_sel = 0;
                        }
                    }
                    if (col < cols) {
                        print_char(bdat[off]);
                    }
                    off = off + 1;
                    col = col + 1;
                }
                if (in_sel == 1) { print(RESET); in_sel = 0; }
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
            if (vrow < vh) { this.screen.move_cursor(vrow + 2, col_off); }
        }
        // 3) Rellenar las filas restantes con tildes (estilo vim/nano).
        while (vrow < vh) {
            this.screen.move_cursor(vrow + 2, col_off);
            print(DIM); print("~"); print(RESET);
            print("\x1b[K");
            vrow = vrow + 1;
        }
    }

    // Despacha el hook on_render_line a las extensiones registradas en
    // runtime (cargadas via loadmodule + register_extension).  Itera los
    // slots del ExtensionRegistry e invoca via reflexion (callm).  La
    // primera extension que devuelva 1 corta el dispatch (linea consumida).
    // Si syntax_on==0 o ninguna extension consume la linea, retorna 0
    // (render por defecto).
    public i32 dispatch_render_line(i32 vrow, i32 line_start_off) {
        if (this.syntax_on == 0) { return 0; }
        i32 blen  = this.buffer.length;
        u8* bdat  = this.buffer.data;
        i32 le    = line_start_off;
        while (le < blen && bdat[le] != 10) { le = le + 1; }
        // line_no = numero absoluto de linea (1-based) que se va a renderizar.
        // Permite que extensiones tipo "line gutter" muestren el numero
        // correcto sin estado interno.
        i32 line_no = this.viewport_top + vrow + 1;
        // cols disponibles para la extension: screen.cols menos el offset
        // del editor area (sidebar.width + 2 si visible).  Sin esto, la
        // extension podria escribir mas alla del ancho real y cortar al
        // borde de la pantalla, escribiendo sobre el area del sidebar
        // contiguamente.
        i32 col_off = this.editor_col_offset();
        i32 avail   = this.screen.cols - col_off + 1;
        if (avail < 1) { avail = 1; }
        return this.exts.dispatch_render_line(line_no, bdat, line_start_off, le, avail);
    }

    public void render_message_bar() {
        // No-op tras refactor al estilo vnano: el status_msg ahora se
        // integra en render_status() (fila inferior).  Solo limpiamos
        // la fila rows-1 que antes usabamos como mensaje temporal para
        // que no quede texto basura tras un resize/redraw.
        this.screen.move_cursor(this.screen.rows - 1, 1);
        print("\x1b[K");
    }

    public void render_status() {
        // Status bar estilo vnano:
        //   " Lin R, Col C / total_lines    <default shortcuts o status_msg>"
        // Toda la barra en REVERSE, llenada con espacios hasta el ancho.
        this.screen.move_cursor(this.screen.rows, 1);
        print(REVERSE);
        i32 r  = this.buffer.cursor_row();
        i32 c  = this.buffer.cursor_col();
        i32 lc = this.buffer.line_count();
        print(" Lin ${r + 1}, Col ${c + 1} / ${lc} ");
        // Middle: status_msg si existe, default shortcuts en su defecto.
        i32 msg_len = str_bytes(this.status_msg);
        if (msg_len > 0) {
            print("  ");
            print(this.status_msg);
            print("  ");
        } else {
            print("  F1 Ayuda  ^O Open  ^S Save  ^F Find  ^P Quick  ^B Sidebar  ^N Tab  ^W Cerrar  ^Q Salir  ");
        }
        print("\x1b[K");
        print(RESET);
    }

    // Offset de columna donde empieza el editor area.  1 = pegado al borde
    // izquierdo (sidebar oculto).  N+2 = tras la barra del sidebar visible.
    public i32 editor_col_offset() {
        if (this.sidebar.visible == 1) {
            return this.sidebar.width + 2;
        }
        return 1;
    }

    public void place_cursor() {
        i32 r = this.buffer.cursor_row();
        i32 c = this.buffer.cursor_col();
        i32 vr = r - this.viewport_top;
        i32 vc = c;
        i32 off = this.editor_col_offset();
        i32 max_col = this.screen.cols - off;
        if (vc >= max_col) { vc = max_col - 1; }
        if (vc < 0) { vc = 0; }
        // Si el sidebar tiene foco, mover el cursor al sidebar para que
        // el blink visible sea ahi (mejor UX que dejarlo sobre el editor).
        if (this.sidebar.visible == 1 && this.sidebar.focused == 1) {
            i32 sy = this.sidebar.selected_idx - this.sidebar.scroll_top;
            this.screen.move_cursor(3 + sy, 1);
            return;
        }
        this.screen.move_cursor(vr + 2, vc + off);
    }

    public void render() {
        this.dlog("render: enter");
        this.screen.refresh_size();
        this.dlog("render: post refresh_size");
        this.adjust_viewport();
        this.dlog("render: post adjust_viewport");
        this.screen.hide_cursor();
        this.dlog("render: post hide_cursor");
        this.render_header();
        this.dlog("render: post render_header");
        if (this.panel_split == 1) {
            // Modo split_v: dos paneles lado a lado.  El area editor
            // se divide en mitades aproximadas; el separador queda en
            // la columna media (lo dibuja render_split_separator).
            i32 area_start = this.editor_col_offset();
            i32 area_end   = this.screen.cols;
            i32 total      = area_end - area_start + 1;
            i32 left_w     = total / 2;
            i32 right_w    = total - left_w - 1;   // -1 para el separador
            if (left_w  < 4) { left_w  = 4; }
            if (right_w < 4) { right_w = 4; }
            // Render izquierdo
            this.render_buffer_at(area_start, left_w, this.viewport_top);
            // Render derecho (col_off justo tras el separador)
            i32 right_col = area_start + left_w + 1;
            this.render_buffer_at(right_col, right_w, this.viewport_top_right);
            // Separador vertical entre paneles
            this.render_split_separator(area_start + left_w);
        } else {
            this.render_buffer();
        }
        this.dlog("render: post render_buffer");
        this.render_message_bar();
        this.dlog("render: post render_message_bar");
        this.render_status();
        this.dlog("render: post render_status");
        // Sidebar (overlay sobre editor area).  Despues del editor + barras
        // para que sus columnas queden sobre cualquier garbage que el
        // editor haya escrito (cuando sidebar esta visible, el editor
        // SI dibuja en columnas 1..width pero las re-escribimos aqui).
        if (this.sidebar.visible == 1) {
            // Refresh perezoso: si root_list aun no se ha llenado, la
            // primera llamada a render lo dispara automaticamente.
            // El bug de phi-destination liveness que rompia este patron
            // fue arreglado en compute_liveness (extension del def del
            // PHI hacia el predecesor mas temprano).
            if (this.sidebar.root_list == null) {
                this.sidebar.refresh();
            }
            this.sidebar.render();
            this.sidebar.render_separator();
        }
        this.dlog("render: post sidebar");
        this.place_cursor();
        this.dlog("render: post place_cursor");
        // Popup de autocompletado: dibujar TRAS place_cursor para que la
        // caja quede por encima del editor.  Si no esta activo, no hace
        // nada (early-return en render()).  Posicion calculada en
        // maybe_trigger usando coordenadas ANSI 1-based.
        if (this.autocomplete.is_active() == 1) {
            this.autocomplete.render(this.screen);
        }
        this.dlog("render: post autocomplete");
        this.screen.show_cursor();
        this.dlog("render: post show_cursor");
        flush();
        this.dlog("render: exit");
    }

    // ----- Ctrl+G: ir a una linea concreta -----
    public void cmd_goto_line() {
        string s = this.read_prompt("Ir a linea: ");
        if (str_bytes(s) == 0) {
            this.status_msg = "goto cancelado";
            return;
        }
        i32 ln = this.parse_positive_int(s);
        if (ln < 1) {
            this.status_msg = "Numero invalido: ${s}";
            return;
        }
        i32 lc = this.buffer.line_count();
        if (ln > lc) { ln = lc; }
        this.buffer.goto_row_col(ln - 1, 0);
        this.status_msg = "Saltado a linea ${ln}";
    }

    public i32 find_substring(u8* bdat, i32 start_off, i32 end_off, u8* needle, i32 nlen) {
        if (nlen == 0) { return -1; }
        i32 i = start_off;
        i32 limit = end_off - nlen;
        while (i <= limit) {
            i32 j = 0;
            i32 hit = 1;
            while (j < nlen && hit == 1) {
                if (bdat[i + j] != needle[j]) { hit = 0; }
                j = j + 1;
            }
            if (hit == 1) { return i; }
            i = i + 1;
        }
        return -1;
    }

    // Posiciona el cursor en el offset absoluto `off` del buffer.
    public void cursor_to_offset(i32 off) {
        i32 blen = this.buffer.length;
        if (off < 0)    { off = 0; }
        if (off > blen) { off = blen; }
        this.buffer.cursor = off;
    }

    // ----- Ctrl+F: prompt + buscar primera ocurrencia desde cursor -----
    public void cmd_search() {
        string q = this.read_prompt("Buscar: ");
        if (str_bytes(q) == 0) {
            this.status_msg = "busqueda cancelada";
            return;
        }
        this.last_search = q;
        this.search_from(this.buffer.cursor);
    }

    // ----- F3: siguiente ocurrencia del ultimo termino -----
    public void cmd_search_next() {
        if (str_bytes(this.last_search) == 0) {
            this.status_msg = "Sin termino previo (Ctrl+F primero)";
            return;
        }
        // Avanzar 1 desde cursor para no atascarse en el match actual.
        this.search_from(this.buffer.cursor + 1);
    }

    public void search_from(i32 from_off) {
        i32 blen = this.buffer.length;
        u8* bdat = this.buffer.data;
        u8* needle = str_cstr(this.last_search);
        i32 nlen = str_bytes(this.last_search);
        i32 found = this.find_substring(bdat, from_off, blen, needle, nlen);
        if (found < 0 && from_off > 0) {
            // Intentar desde el inicio (wrap-around).
            found = this.find_substring(bdat, 0, blen, needle, nlen);
            if (found >= 0) { this.status_msg = "wrap: '${this.last_search}' encontrado"; }
        } else if (found >= 0) {
            this.status_msg = "encontrado '${this.last_search}'";
        }
        if (found < 0) {
            this.status_msg = "No encontrado: '${this.last_search}'";
            return;
        }
        this.cursor_to_offset(found);
    }

    // ----- Ctrl+K: cortar linea actual al clipboard -----
    public void cmd_cut_line() {
        i32 ls = this.buffer.line_start();
        i32 blen = this.buffer.length;
        u8* bdat = this.buffer.data;
        i32 le = ls;
        while (le < blen && bdat[le] != 10) { le = le + 1; }
        // Incluir el \n al cortar (si no es el final del buffer).
        i32 cut_end = le;
        if (cut_end < blen && bdat[cut_end] == 10) { cut_end = cut_end + 1; }
        i32 cut_len = cut_end - ls;
        if (cut_len <= 0) {
            this.status_msg = "linea vacia, nada que cortar";
            return;
        }
        // Guardar al clipboard como string (str_make auto-detecta host_ptr).
        this.clipboard = str_make(bdat + ls, cut_len);
        // Borrar [ls, cut_end) del buffer: shift left + ajustar length.
        i32 i = ls;
        while (i + cut_len < blen) {
            bdat[i] = bdat[i + cut_len];
            i = i + 1;
        }
        this.buffer.length = blen - cut_len;
        this.buffer.dirty  = 1;
        this.cursor_to_offset(ls);
        this.status_msg = "Cortado ${cut_len} bytes";
    }

    // ----- Ctrl+V: pegar clipboard en el cursor -----
    public void cmd_paste() {
        i32 clen = str_bytes(this.clipboard);
        if (clen == 0) {
            this.status_msg = "Clipboard vacio";
            return;
        }
        this.buffer.ensure_capacity(this.buffer.length + clen);
        u8* bdat = this.buffer.data;
        i32 cur = this.buffer.cursor;
        i32 blen = this.buffer.length;
        // Shift right desde cur..blen para hacer espacio.
        i32 i = blen;
        while (i > cur) {
            bdat[i + clen - 1] = bdat[i - 1];
            i = i - 1;
        }
        // Copiar clipboard al hueco.
        u8* src = str_cstr(this.clipboard);
        i32 j = 0;
        while (j < clen) {
            bdat[cur + j] = src[j];
            j = j + 1;
        }
        this.buffer.length = blen + clen;
        this.buffer.dirty  = 1;
        this.cursor_to_offset(cur + clen);
        this.status_msg = "Pegado ${clen} bytes";
    }

    // ----- Ctrl+L / Ctrl+R: movimiento por palabras -----
    public i32 is_word_char(i32 c) {
        if (c >= 65 && c <= 90)  { return 1; }
        if (c >= 97 && c <= 122) { return 1; }
        if (c >= 48 && c <= 57)  { return 1; }
        if (c == 95)             { return 1; }
        return 0;
    }

    public void cmd_word_left() {
        i32 cur = this.buffer.cursor;
        u8* bdat = this.buffer.data;
        // Saltar separadores hacia atras.
        while (cur > 0 && this.is_word_char(bdat[cur - 1]) == 0) { cur = cur - 1; }
        // Saltar la palabra hacia atras.
        while (cur > 0 && this.is_word_char(bdat[cur - 1]) == 1) { cur = cur - 1; }
        this.cursor_to_offset(cur);
    }

    public void cmd_word_right() {
        i32 cur = this.buffer.cursor;
        i32 blen = this.buffer.length;
        u8* bdat = this.buffer.data;
        // Saltar la palabra actual hacia adelante.
        while (cur < blen && this.is_word_char(bdat[cur]) == 1) { cur = cur + 1; }
        // Saltar separadores.
        while (cur < blen && this.is_word_char(bdat[cur]) == 0) { cur = cur + 1; }
        this.cursor_to_offset(cur);
    }

    // -------------------- FILE PICKER (Ctrl+O) --------------------
    // Cuenta los entries del linked list devuelto por list_current_dir.
    public i32 dir_count(DirEntry head) {
        i32 n = 0;
        DirEntry p = head;
        while (p != null) { n = n + 1; p = p.next; }
        return n;
    }

    // Devuelve el i-esimo entry o null si fuera de rango.
    public DirEntry dir_at(DirEntry head, i32 idx) {
        i32 i = 0;
        DirEntry p = head;
        while (p != null) {
            if (i == idx) { return p; }
            i = i + 1;
            p = p.next;
        }
        return null;
    }

    // Ctrl+O: lista el directorio actual en una caja centrada y permite
    // navegar Up/Down + Enter para abrir, Esc para cancelar.
    public void cmd_open_file() {
        DirEntry head = this.fexp.list_current_dir();
        if (head == null) {
            this.status_msg = "No hay archivos en el directorio actual";
            return;
        }
        i32 total = this.dir_count(head);
        i32 cols  = this.screen.cols;
        i32 rows  = this.screen.rows;
        i32 box_w = 50;
        i32 box_h = 20;
        if (box_w > cols - 4) { box_w = cols - 4; }
        if (box_h > rows - 4) { box_h = rows - 4; }
        i32 col0 = (cols - box_w) / 2 + 1;
        i32 row0 = (rows - box_h) / 2 + 1;
        i32 visible = box_h - 4;
        if (visible < 4) { visible = 4; }
        i32 sel  = 0;
        i32 view = 0;
        i32 done = 0;
        string opened = "";
        while (done == 0) {
            i32 r = 0;
            while (r < box_h) {
                this.screen.move_cursor(row0 + r, col0);
                print(REVERSE);
                i32 c = 0;
                while (c < box_w) { print(" "); c = c + 1; }
                print(RESET);
                r = r + 1;
            }
            this.screen.move_cursor(row0, col0 + 2);
            print(REVERSE); print(BOLD); print(CYAN);
            print(" Abrir archivo (${total}) ");
            print(RESET);
            i32 li = 0;
            while (li < visible && view + li < total) {
                DirEntry e = this.dir_at(head, view + li);
                this.screen.move_cursor(row0 + 2 + li, col0 + 2);
                if (view + li == sel) { print(REVERSE); print(BOLD); print(YELLOW); }
                else                  { print(REVERSE); }
                if (e.is_dir == 1) {
                    print("[");
                    print(e.name);
                    print("]");
                } else {
                    print(" ");
                    print(e.name);
                }
                print(RESET);
                li = li + 1;
            }
            this.screen.move_cursor(row0 + box_h - 2, col0 + 2);
            print(REVERSE); print(DIM);
            print(" Up/Down navegar  Enter abrir  Esc cancelar ");
            print(RESET);
            flush();
            i32 k = this.input.read_key_blocking();
            if (k == KEY_ESC) {
                done = 1;
            } else if (k == KEY_UP) {
                if (sel > 0) { sel = sel - 1; }
                if (sel < view) { view = sel; }
            } else if (k == KEY_DOWN) {
                if (sel < total - 1) { sel = sel + 1; }
                if (sel >= view + visible) { view = sel - visible + 1; }
            } else if (k == KEY_ENTER) {
                DirEntry e = this.dir_at(head, sel);
                if (e != null && e.is_dir == 0 && str_bytes(e.name) > 0) {
                    opened = e.name;
                    done = 1;
                } else {
                    this.status_msg = "(no se navega a directorios; usar archivo)";
                }
            }
        }
        if (str_bytes(opened) > 0) {
            if (this.buffer.dirty == 1) { this.cmd_new_tab(); }
            this.load_file(opened);
        }
    }

    // -------------------- PROJECT FOLDER PROMPT --------------------
    // Equivalente al inicial Launcher de vnano.vsh: al arrancar el editor
    // muestra una caja preguntando "Project folder" con el cwd actual
    // como default.  Si el usuario escribe un path no vacio, el editor
    // hace chdir() y refresca el sidebar para mostrar esa carpeta.
    //
    // Skip via Esc o Enter sin escribir nada (mantiene cwd actual).

    public void prompt_project_folder() {
        // Mostrar cwd actual como informacion.
        u8* cwdbuf = malloc(1024);
        i64 r = this.io.getcwd(cwdbuf, 1024);
        string cwd = "(?)";
        if (r != 0) {
            i32 cwd_len = 0;
            while (cwd_len < 1024 && cwdbuf[cwd_len] != 0) { cwd_len = cwd_len + 1; }
            cwd = str_make(cwdbuf, cwd_len);
        }
        free(cwdbuf);

        // Caja centrada con prompt.
        i32 cols  = this.screen.cols;
        i32 rows  = this.screen.rows;
        i32 box_w = 70;
        i32 box_h = 9;
        if (box_w > cols - 4) { box_w = cols - 4; }
        if (box_h > rows - 4) { box_h = rows - 4; }
        i32 col0 = (cols - box_w) / 2 + 1;
        i32 row0 = (rows - box_h) / 2 + 1;
        // Bg.
        i32 r2 = 0;
        while (r2 < box_h) {
            this.screen.move_cursor(row0 + r2, col0);
            print(REVERSE);
            i32 c = 0;
            while (c < box_w) { print(" "); c = c + 1; }
            print(RESET);
            r2 = r2 + 1;
        }
        // Title.
        this.screen.move_cursor(row0, col0 + 2);
        print(REVERSE); print(BOLD); print(CYAN);
        print(" VEXED - Project folder ");
        print(RESET);
        // Cwd actual.
        this.screen.move_cursor(row0 + 2, col0 + 2);
        print(REVERSE); print(DIM);
        print("Carpeta actual: ${cwd}");
        print(RESET);
        // Prompt label.
        this.screen.move_cursor(row0 + 4, col0 + 2);
        print(REVERSE); print(YELLOW);
        print("Nuevo path (Enter = mantener actual, Esc = saltar):");
        print(RESET);
        // Input area.
        this.screen.move_cursor(row0 + 5, col0 + 2);
        print(REVERSE); print(BOLD);
        print("> ");
        print(RESET);
        // Footer.
        this.screen.move_cursor(row0 + box_h - 1, col0 + 2);
        print(REVERSE); print(DIM);
        print(" Tip: '..' sube un nivel  '/path/abs' absoluto ");
        print(RESET);
        flush();

        // Leer input simple (sin redibujar la caja completa por cada keystroke
        // -- tras Backspace borramos solo desde el cursor del input).
        u8* buf  = malloc(512);
        i32 len  = 0;
        i32 done = 0;
        i32 ok   = 0;
        while (done == 0) {
            // Re-pintar la linea de input.
            this.screen.move_cursor(row0 + 5, col0 + 2);
            print(REVERSE); print(BOLD); print("> "); print(RESET);
            print(REVERSE);
            i32 i = 0;
            while (i < len) {
                print_char(buf[i]);
                i = i + 1;
            }
            // Pad a fin de caja para que el backspace borre visualmente.
            i32 used = 2 + len;
            i32 pad  = box_w - 4 - used;
            while (pad > 0) { print(" "); pad = pad - 1; }
            print(RESET);
            this.screen.show_cursor();
            this.screen.move_cursor(row0 + 5, col0 + 4 + len);
            flush();
            i32 k = this.input.read_key_blocking();
            if (k == KEY_ESC) {
                done = 1;
            } else if (k == KEY_ENTER) {
                done = 1;
                ok = 1;
            } else if (k == KEY_BS) {
                if (len > 0) { len = len - 1; }
            } else if (k >= 32 && k <= 126 && len < 511) {
                buf[len] = k;
                len = len + 1;
            }
        }
        // Aplicar cambio si hay path no vacio.  En cualquier caso (cambio
        // o no), refrescamos el sidebar aqui -- es el momento mas seguro
        // para hacerlo: post inicializacion completa + post-input I/O
        // (que sirve de barrera para el regalloc).
        if (ok == 1 && len > 0) {
            string path = str_make(buf, len);
            i32 cr = this.io.chdir(path);
            if (cr == 0) {
                this.status_msg = "Carpeta cambiada a: ${path}";
                // Actualizar el basename del proyecto que se muestra en el header.
                this.refresh_project_basename();
            } else {
                this.status_msg = "Error: no existe la carpeta '${path}' (mantiene actual)";
            }
        }
        this.sidebar.refresh();
        free(buf);
    }

    // -------------------- QUICK OPEN (Ctrl+P) --------------------
    // Launcher tipo VS Code / Sublime: caja con prompt incremental que
    // filtra los archivos del directorio en tiempo real.  Equivalente a
    // class Launcher de vnano.vsh.
    //
    // Helpers privados de matching: substring case-insensitive sobre el
    // basename.  No usamos fuzzy real (bonus complejo) -- solo "needle
    // aparece en haystack como subcadena consecutiva, ignorando case".
    // Suficiente para `s.v` -> `sidebar.vh`, `e_c` -> `editor_core.vh`.

    // True si el byte 'b' es ASCII upper.
    public i32 is_upper_byte(i32 b) {
        if (b >= 65 && b <= 90) { return 1; }
        return 0;
    }

    // Lower de un byte ASCII (no toca >127).
    public i32 to_lower_byte(i32 b) {
        if (this.is_upper_byte(b) == 1) { return b + 32; }
        return b;
    }

    // True si needle aparece como subcadena en haystack (case-insensitive).
    // Empty needle siempre matchea (mostrar todos).
    public i32 str_icontains(string haystack, string needle) {
        i32 nl = str_bytes(needle);
        if (nl == 0) { return 1; }
        i32 hl = str_bytes(haystack);
        if (hl < nl) { return 0; }
        u8* hd = str_cstr(haystack);
        u8* nd = str_cstr(needle);
        i32 i = 0;
        while (i <= hl - nl) {
            i32 j = 0;
            i32 ok = 1;
            while (j < nl) {
                i32 hb = this.to_lower_byte(hd[i + j]);
                i32 nb = this.to_lower_byte(nd[j]);
                if (hb != nb) { ok = 0; j = nl; }
                else { j = j + 1; }
            }
            if (ok == 1) { return 1; }
            i = i + 1;
        }
        return 0;
    }

    // Cuenta los entries del linked list cuyo nombre matchea el filtro.
    public i32 count_matches(DirEntry head, string filter) {
        i32 n = 0;
        DirEntry p = head;
        while (p != null) {
            if (p.is_dir == 0 && this.str_icontains(p.name, filter) == 1) {
                n = n + 1;
            }
            p = p.next;
        }
        return n;
    }

    // Devuelve el Nth entry que matchea (0-based).  null si fuera de rango.
    public DirEntry match_at(DirEntry head, i32 idx, string filter) {
        i32 n = 0;
        DirEntry p = head;
        while (p != null) {
            if (p.is_dir == 0 && this.str_icontains(p.name, filter) == 1) {
                if (n == idx) { return p; }
                n = n + 1;
            }
            p = p.next;
        }
        return null;
    }

    // Quick Open principal: caja flotante con prompt + lista filtrada.
    public void cmd_quick_open() {
        DirEntry head = this.fexp.list_current_dir();
        if (head == null) {
            this.status_msg = "No hay archivos en el directorio";
            return;
        }
        i32 cols = this.screen.cols;
        i32 rows = this.screen.rows;
        i32 box_w = 60;
        i32 box_h = 20;
        if (box_w > cols - 4) { box_w = cols - 4; }
        if (box_h > rows - 4) { box_h = rows - 4; }
        i32 col0 = (cols - box_w) / 2 + 1;
        i32 row0 = (rows - box_h) / 2 + 1;
        i32 list_h = box_h - 5;  // header + prompt + footer + 2 border
        if (list_h < 4) { list_h = 4; }

        string filter = "";
        i32 sel       = 0;
        i32 view      = 0;
        i32 done      = 0;
        string opened = "";

        while (done == 0) {
            i32 nmatches = this.count_matches(head, filter);
            if (sel >= nmatches) { sel = nmatches - 1; }
            if (sel < 0)         { sel = 0; }
            if (sel < view)      { view = sel; }
            if (sel >= view + list_h) { view = sel - list_h + 1; }
            if (view < 0) { view = 0; }

            // Box bg.
            i32 r = 0;
            while (r < box_h) {
                this.screen.move_cursor(row0 + r, col0);
                print(REVERSE);
                i32 c = 0;
                while (c < box_w) { print(" "); c = c + 1; }
                print(RESET);
                r = r + 1;
            }
            // Title.
            this.screen.move_cursor(row0, col0 + 2);
            print(REVERSE); print(BOLD); print(CYAN);
            print(" Quick Open (${nmatches} match) ");
            print(RESET);
            // Prompt line.
            this.screen.move_cursor(row0 + 2, col0 + 2);
            print(REVERSE); print(YELLOW);
            print("> ");
            print(RESET); print(REVERSE); print(BOLD);
            print(filter);
            print(RESET);
            // Filtered list.
            i32 li = 0;
            while (li < list_h && view + li < nmatches) {
                DirEntry e = this.match_at(head, view + li, filter);
                this.screen.move_cursor(row0 + 4 + li, col0 + 2);
                if (view + li == sel) { print(REVERSE); print(BOLD); print(YELLOW); }
                else                  { print(REVERSE); }
                if (e != null) {
                    print(" ");
                    print(e.name);
                }
                print(RESET);
                li = li + 1;
            }
            // Footer hint.
            this.screen.move_cursor(row0 + box_h - 1, col0 + 2);
            print(REVERSE); print(DIM);
            print(" type=filtrar  Up/Down=navegar  Enter=abrir  Esc=cancelar ");
            print(RESET);
            flush();

            i32 k = this.input.read_key_blocking();
            if (k == KEY_ESC) {
                done = 1;
            } else if (k == KEY_UP) {
                if (sel > 0) { sel = sel - 1; }
            } else if (k == KEY_DOWN) {
                if (sel < nmatches - 1) { sel = sel + 1; }
            } else if (k == KEY_ENTER) {
                DirEntry e = this.match_at(head, sel, filter);
                if (e != null) {
                    opened = e.name;
                    done = 1;
                }
            } else if (k == KEY_BACKSPACE) {
                i32 fl = str_bytes(filter);
                if (fl > 0) {
                    // Quitar ultimo byte (sirve para ASCII; UTF-8 multi-byte
                    // borrara byte a byte, aceptable para nombres de archivo).
                    u8* fd = str_cstr(filter);
                    string nf = str_make(fd, fl - 1);
                    filter = nf;
                    sel = 0;
                    view = 0;
                }
            } else if (k >= 32 && k <= 126) {
                // Caracter imprimible: anadir al filter.
                u8* tmp = malloc(2);
                tmp[0] = k;
                tmp[1] = 0;
                string ch = str_make(tmp, 1);
                free(tmp);
                if (str_bytes(filter) == 0) {
                    filter = ch;
                } else {
                    filter = filter + ch;
                }
                sel = 0;
                view = 0;
            }
        }

        if (str_bytes(opened) > 0) {
            if (this.buffer.dirty == 1) { this.cmd_new_tab(); }
            this.load_file(opened);
        }
    }

    // ----- F1: dialogo de ayuda overlay -----
    public void show_help_dialog() {
        // Dibujar caja centrada con la lista de keys.  Tras Esc/Enter,
        // el ciclo principal redibuja sobre ella.
        i32 cols = this.screen.cols;
        i32 rows = this.screen.rows;
        i32 box_w = 64;
        i32 box_h = 30;
        if (box_w > cols - 4) { box_w = cols - 4; }
        if (box_h > rows - 4) { box_h = rows - 4; }
        i32 col0 = (cols - box_w) / 2 + 1;
        i32 row0 = (rows - box_h) / 2 + 1;
        // Fondo de la caja: filas con espacios para no ver el codigo de fondo.
        i32 r = 0;
        while (r < box_h) {
            this.screen.move_cursor(row0 + r, col0);
            print(REVERSE);
            i32 c = 0;
            while (c < box_w) { print(" "); c = c + 1; }
            print(RESET);
            r = r + 1;
        }
        // Titulo.
        this.screen.move_cursor(row0, col0 + 2);
        print(REVERSE); print(BOLD); print(CYAN);
        print(" VEXED - Ayuda rapida ");
        print(RESET);
        // Lista de atajos.
        i32 line = 2;
        this.print_help_at(row0 + line, col0 + 2, "Ctrl+Q          salir");          line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "Ctrl+S          guardar");        line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "Ctrl+Z / Ctrl+Y deshacer / rehacer"); line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "Ctrl+F          buscar");         line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "F3              siguiente match");line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "Ctrl+G          ir a linea");     line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "Ctrl+K          cortar linea");   line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "Ctrl+V          pegar");          line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "Ctrl+C          copiar seleccion"); line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "Ctrl+X          cortar seleccion (o linea)"); line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "Shift+flechas   extender seleccion"); line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "Shift+Home/End  seleccionar a inicio/fin"); line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "Ctrl+L / Ctrl+R mover por palabra"); line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "Flechas/Home/End/PgUp/PgDn");     line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "Tab             4 espacios");      line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "Ctrl+N          nuevo tab");        line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "Ctrl+W          cerrar tab");       line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "Ctrl+O          abrir archivo (picker)"); line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "F4 / F5         siguiente / anterior tab"); line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "Ctrl+P          quick open (filter incremental)");   line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "Ctrl+R          cambiar carpeta proyecto");          line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "Ctrl+B          toggle sidebar (panel de archivos)"); line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "F6              foco editor <-> sidebar");           line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "F1              esta ayuda");      line = line + 1;
        this.print_help_at(row0 + line, col0 + 2, "F2              toggle syntax");   line = line + 1;
        this.print_help_at(row0 + line - 1 + 2, col0 + 2, "[Pulsa cualquier tecla para cerrar]");
        flush();
        this.input.read_key_blocking();
    }

    public void print_help_at(i32 row, i32 col, string msg) {
        this.screen.move_cursor(row, col);
        print(REVERSE);
        print(msg);
        print(RESET);
    }

    public void handle_key(i32 k) {
        this.dlog_i("handle_key: enter k=", k);
        this.last_key = k;
        // Autocomplete popup activo: consume las teclas de navegacion +
        // acepta/cancela.  Si el popup esta abierto y el user pulsa una
        // tecla no-relacionada (movimiento, ctrl, esc), cancelamos popup
        // y dejamos que la tecla pase al flujo normal.
        if (this.autocomplete.is_active() == 1) {
            if (k == VK_ESCAPE) {
                this.autocomplete.cancel();
                this.status_msg = "autocomplete: cancelado";
                return;
            }
            if (k == KEY_UP) {
                this.autocomplete.move_up();
                return;
            }
            if (k == KEY_DOWN) {
                this.autocomplete.move_down();
                return;
            }
            if (k == KEY_ENTER || k == KEY_TAB) {
                string suffix = this.autocomplete.accept_suffix();
                i32 sl = str_bytes(suffix);
                if (sl > 0) {
                    this.push_undo();
                    u8* sp = str_cstr(suffix);
                    i32 si = 0;
                    while (si < sl) {
                        this.buffer.insert_char(sp[si]);
                        si = si + 1;
                    }
                    this.status_msg = "autocomplete: inserto ${suffix}";
                }
                return;
            }
            // Backspace dentro de popup: borrar char + refrescar candidatos.
            if (k == KEY_BS) {
                this.push_undo();
                this.buffer.backspace();
                this.refresh_autocomplete();
                return;
            }
            // Caracter alfanumerico: insertar + refrescar.
            if (k >= 32 && k <= 126
             && (this.autocomplete.is_word_byte(k) == 1)) {
                this.push_undo();
                this.buffer.insert_char(k);
                this.refresh_autocomplete();
                return;
            }
            // Cualquier otra cosa: cerrar popup y dejar caer al flujo normal.
            this.autocomplete.cancel();
        }
        // Sidebar focus mode: si el sidebar esta visible y enfocado, deja
        // que el sidebar consuma navegacion.  Enter abre el archivo
        // seleccionado.  Esc/Ctrl+1 devuelve foco al editor.
        if (this.sidebar.visible == 1 && this.sidebar.focused == 1) {
            if (k == KEY_CTRL_B) {
                this.sidebar.toggle_visible();
                return;
            }
            if (k == VK_ESCAPE || k == KEY_CTRL_Q || k == KEY_F6) {
                if (k == KEY_CTRL_Q) { this.running = 0; return; }
                this.sidebar.focused = 0;
                this.status_msg = "foco al editor";
                return;
            }
            if (k == KEY_ENTER) {
                // Si es directorio: en esta version simple, no expandimos
                // (TODO).  Si es archivo: abrir en nuevo tab.
                if (this.sidebar.selected_is_dir() == 0) {
                    string path = this.sidebar.selected_path();
                    if (str_bytes(path) > 0) {
                        this.cmd_new_tab();
                        this.load_file(path);
                        this.sidebar.focused = 0;
                        this.status_msg = "abierto: ";
                    }
                } else {
                    this.status_msg = "[directorio - expandir TODO]";
                }
                return;
            }
            if (this.sidebar.handle_key_focused(k) == 1) { return; }
            // Cualquier otra tecla: ignorar (no editar el buffer mientras
            // el foco esta en el sidebar).
            return;
        }
        // Editor focus mode: keys generales.
        if (k == KEY_CTRL_B) {
            this.sidebar.toggle_visible();
            if (this.sidebar.visible == 1) {
                this.status_msg = "sidebar abierto (Esc cierra foco, Ctrl+B cierra sidebar)";
            } else {
                this.status_msg = "sidebar cerrado";
            }
            return;
        }
        if (k == KEY_F6) {
            // Rotar foco entre editor y sidebar.  Si sidebar oculto, abrirlo.
            if (this.sidebar.visible == 0) { this.sidebar.toggle_visible(); }
            else { this.sidebar.focused = 1; }
            return;
        }
        // Primero intentamos delegar la tecla a las extensiones runtime.
        // Si alguna devuelve 1, la consumio y no procesamos por defecto.
        i32 consumed = this.exts.dispatch_keypress(k);
        this.dlog_i("handle_key: post ext dispatch consumed=", consumed);
        if (consumed == 1) { return; }
        if (k == KEY_CTRL_Q) {
            // Si hay buffers con cambios sin guardar, confirmar antes de
            // salir (estilo vnano: prompt "(y/N)" inline).
            if (this.any_tab_modified() == 1) {
                string ans = this.read_prompt("Hay pestanas sin guardar. Salir? (y/N): ");
                i32 al = str_bytes(ans);
                if (al == 0) { return; }
                u8* ab = str_cstr(ans);
                i32 first = ab[0];
                if (first != 121 && first != 89) { return; }  // 'y'/'Y'
            }
            this.running = 0;
            return;
        }
        if (k == KEY_CTRL_S) { this.save_file();          return; }
        if (k == KEY_CTRL_Z) { this.cmd_undo();           return; }
        if (k == KEY_CTRL_Y) { this.cmd_redo();           return; }
        if (k == KEY_CTRL_N) { this.cmd_new_tab();        return; }
        if (k == KEY_CTRL_W) { this.cmd_close_tab();      return; }
        if (k == KEY_CTRL_O) { this.cmd_open_file();      return; }
        if (k == KEY_CTRL_P) { this.cmd_quick_open();     return; }
        if (k == KEY_CTRL_R) { this.prompt_project_folder(); return; }
        if (k == KEY_F4) {
            // Toggle split vertical (estilo vnano: dos paneles del mismo
            // buffer con viewport independiente).  Permite ver dos zonas
            // del archivo simultaneamente.
            if (this.panel_split == 0) {
                this.panel_split        = 1;
                this.viewport_top_right = this.viewport_top;
                this.panel_focus        = 0;
                this.status_msg = "split vertical (F7 cambia foco, F4 cierra)";
            } else {
                this.panel_split = 0;
                this.panel_focus = 0;
                this.status_msg  = "split cerrado";
            }
            return;
        }
        if (k == KEY_F7) {
            // Cambiar foco entre paneles izquierdo y derecho cuando el
            // editor esta en modo split_v.
            if (this.panel_split == 1) {
                if (this.panel_focus == 0) {
                    this.panel_focus = 1;
                    this.status_msg  = "foco panel derecho";
                } else {
                    this.panel_focus = 0;
                    this.status_msg  = "foco panel izquierdo";
                }
            }
            return;
        }
        // Tabs: F11/F12 (F4/F5 ahora son split + cerrar split).
        if (k == KEY_F9) {
            // Anyadir cursor extra una linea por debajo del primario.
            this.mc_add_below();
            return;
        }
        if (k == KEY_F10) {
            // Anyadir cursor extra una linea por arriba del primario.
            this.mc_add_above();
            return;
        }
        // Esc: si hay multi-cursor activo, lo limpiamos (en vez de
        // pasar el VK_ESCAPE al flujo).
        if (k == VK_ESCAPE && this.mc_active() == 1) {
            this.mc_clear();
            return;
        }
        if (k == KEY_F11)    { this.cmd_next_tab();       return; }
        if (k == KEY_F12)    { this.cmd_prev_tab();       return; }
        // Compat: aceptar F5 como atajo legacy hacia tab anterior.
        if (k == KEY_F5)     { this.cmd_prev_tab();       return; }
        if (k == KEY_CTRL_F) { this.cmd_search();         return; }
        if (k == KEY_F3)     { this.cmd_search_next();    return; }
        if (k == KEY_CTRL_G) { this.cmd_goto_line();      return; }
        // Cut / Copy / Paste con seleccion: si hay seleccion activa, Ctrl+K
        // y Ctrl+C operan sobre ella; en caso contrario fallback al modo
        // linea (legacy de etapas anteriores).
        if (k == KEY_CTRL_C) {
            if (this.has_selection() == 1) { this.sel_copy(); }
            else { this.status_msg = "sin seleccion (usa Shift+flechas para seleccionar)"; }
            return;
        }
        if (k == KEY_CTRL_X) {
            if (this.has_selection() == 1) { this.push_undo(); this.sel_cut(); return; }
            this.push_undo();
            this.cmd_cut_line();
            return;
        }
        if (k == KEY_CTRL_K) { this.push_undo(); this.cmd_cut_line();       return; }
        if (k == KEY_CTRL_V) {
            this.push_undo();
            if (this.has_selection() == 1) { this.sel_delete(); }
            this.cmd_paste();
            return;
        }
        // Movimiento normal: limpia la seleccion antes de moverse.
        if (k == KEY_LEFT)   { this.sel_clear(); this.buffer.move_left();   return; }
        if (k == KEY_RIGHT)  { this.sel_clear(); this.buffer.move_right();  return; }
        if (k == KEY_UP)     { this.sel_clear(); this.buffer.move_up();     return; }
        if (k == KEY_DOWN)   { this.sel_clear(); this.buffer.move_down();   return; }
        if (k == KEY_HOME)   { this.sel_clear(); this.buffer.move_home();   return; }
        if (k == KEY_END)    { this.sel_clear(); this.buffer.move_end();    return; }
        if (k == KEY_DEL)    {
            this.push_undo();
            if (this.has_selection() == 1) { this.sel_delete(); return; }
            this.buffer.del_char();
            return;
        }
        if (k == KEY_PGUP)   { this.sel_clear(); this.buffer.page_up(this.viewport_rows());   return; }
        if (k == KEY_PGDN)   { this.sel_clear(); this.buffer.page_down(this.viewport_rows()); return; }
        // Movimiento con shift: extiende seleccion (anchor + cursor).
        if (k == KEY_SHIFT_LEFT)  { this.sel_begin_or_extend(); this.buffer.move_left();   return; }
        if (k == KEY_SHIFT_RIGHT) { this.sel_begin_or_extend(); this.buffer.move_right();  return; }
        if (k == KEY_SHIFT_UP)    { this.sel_begin_or_extend(); this.buffer.move_up();     return; }
        if (k == KEY_SHIFT_DOWN)  { this.sel_begin_or_extend(); this.buffer.move_down();   return; }
        if (k == KEY_SHIFT_HOME)  { this.sel_begin_or_extend(); this.buffer.move_home();   return; }
        if (k == KEY_SHIFT_END)   { this.sel_begin_or_extend(); this.buffer.move_end();    return; }
        if (k == KEY_CTRL_L) { this.sel_clear(); this.cmd_word_left();      return; }
        if (k == KEY_CTRL_R) { this.sel_clear(); this.cmd_word_right();     return; }
        if (k == KEY_ENTER)  {
            this.push_undo();
            // Si hay seleccion activa, la sustituimos por el newline
            // (replace-on-type estandar de editores GUI).
            if (this.has_selection() == 1) { this.sel_delete(); }
            // Auto-indent: insertar \n + replicar el leading whitespace de
            // la linea actual.  Capturamos los chars de indent a un buffer
            // temporal ANTES de newline() porque ensure_capacity puede
            // realocar `buffer.data` invalidando el host_ptr.
            i32 ls = this.buffer.line_start();
            i32 cur = this.buffer.cursor;
            u8* indent_save = malloc(64);
            i32 indent_len = 0;
            i32 ie = ls;
            while (ie < cur && indent_len < 64) {
                i32 c = this.buffer.data[ie];
                if (c == 32 || c == 9) {
                    indent_save[indent_len] = c;
                    indent_len = indent_len + 1;
                    ie = ie + 1;
                } else {
                    ie = cur;   // break
                }
            }
            this.buffer.newline();
            i32 i = 0;
            while (i < indent_len) {
                this.buffer.insert_char(indent_save[i]);
                i = i + 1;
            }
            free(indent_save);
            return;
        }
        if (k == KEY_BS)     {
            this.push_undo();
            if (this.has_selection() == 1) { this.sel_delete(); return; }
            if (this.mc_active() == 1) {
                this.mc_apply_backspace();
            } else {
                this.buffer.backspace();
            }
            return;
        }
        if (k == KEY_F1)     { this.show_help_dialog();   return; }
        if (k == KEY_F2)     {
            // Toggle del syntax-highlighting global (afecta a todas las
            // extensiones registradas con on_render_line).
            if (this.syntax_on == 1) {
                this.syntax_on  = 0;
                this.status_msg = "Syntax OFF";
            } else {
                this.syntax_on  = 1;
                this.status_msg = "Syntax ON";
            }
            return;
        }
        if (k == KEY_TAB)    {
            this.push_undo();
            if (this.has_selection() == 1) { this.sel_delete(); }
            // Insertar tab_width espacios (configurable via .vexedrc).
            i32 tw = this.config.tab_width;
            i32 ti = 0;
            while (ti < tw) {
                this.buffer.insert_char(32);
                ti = ti + 1;
            }
            return;
        }
        if (k >= 32 && k <= 126) {
            this.push_undo();
            if (this.has_selection() == 1) { this.sel_delete(); }
            if (this.mc_active() == 1) {
                this.mc_apply_insert(k);
                // Avanzar col_target porque acabamos de insertar 1 char.
                this.mcursors.col_target = this.mcursors.col_target + 1;
            } else {
                this.buffer.insert_char(k);
            }
            // Si el char es alfanumerico, posiblemente disparar autocomplete.
            if (this.autocomplete.is_word_byte(k) == 1
             && this.mc_active() == 0) {
                this.refresh_autocomplete();
            }
            return;
        }
    }

    // -----------------------------------------------------------------
    // Multi-cursor: aplicar una operacion (insert_char o backspace) a
    // todos los extras + el primario.  Procesamos las filas en orden
    // DESCENDENTE para que las inserciones/borrados no invaliden los
    // byte offsets de las filas anteriores.
    //
    // Para insert: posicionamos el cursor en (row, col_target) y
    // llamamos this.buffer.insert_char.
    // Para backspace: posicionamos el cursor en (row, col_target) y
    // llamamos this.buffer.backspace.
    //
    // En ambos casos restauramos finalmente this.buffer.cursor al
    // primario (su nueva posicion tras el edit en su propia fila).
    // -----------------------------------------------------------------

    // True si el multi-cursor esta activo (col_target >= 0 y al menos 1 extra).
    public i32 mc_active() {
        if (this.mcursors.col_target < 0)  { return 0; }
        if (this.mcursors.count_extras <= 0) { return 0; }
        return 1;
    }

    // Recorre extras + primario, ordenando descendente por row.  Como solo
    // hay <= 9 cursores, hacemos un selection sort sobre un array local.
    // El resultado es un array ordenado de FILAS (incluyendo la del primario)
    // que el caller usa para iterar.
    //
    // Limitacion: si dos cursores apuntan a la misma fila se ignora el
    // duplicado (caso edge: primario y un extra colisionan).
    public void mc_apply_insert(i32 ch) {
        i32 col   = this.mcursors.col_target;
        i32 prow  = this.buffer.cursor_row();
        // Construir array local de filas (max 9 = 1 primario + 8 extras).
        i32 rows0 = prow;
        i32 rows1 = -1; i32 rows2 = -1; i32 rows3 = -1; i32 rows4 = -1;
        i32 rows5 = -1; i32 rows6 = -1; i32 rows7 = -1; i32 rows8 = -1;
        i32 n     = 1;
        i32 i     = 0;
        while (i < this.mcursors.count_extras) {
            i32 r = this.mcursors.row_at(i);
            if (r != prow) {
                if      (n == 1) { rows1 = r; }
                else if (n == 2) { rows2 = r; }
                else if (n == 3) { rows3 = r; }
                else if (n == 4) { rows4 = r; }
                else if (n == 5) { rows5 = r; }
                else if (n == 6) { rows6 = r; }
                else if (n == 7) { rows7 = r; }
                else if (n == 8) { rows8 = r; }
                n = n + 1;
            }
            i = i + 1;
        }
        // Selection sort DESCENDENTE in-place sobre el array de 9 slots.
        // Para simplificarlo, dump a buffer fijo de tamano 9 y reordenar.
        // Vamos a usar un truco: aplicar cada fila iterando max-min.
        // Helper local: encontrar el siguiente max no procesado.
        i32 used0 = 0; i32 used1 = 0; i32 used2 = 0; i32 used3 = 0;
        i32 used4 = 0; i32 used5 = 0; i32 used6 = 0; i32 used7 = 0;
        i32 used8 = 0;
        i32 done = 0;
        while (done < n) {
            // Buscar la fila maxima no usada.
            i32 maxv = -1;
            i32 maxi = -1;
            if (used0 == 0 && rows0 > maxv) { maxv = rows0; maxi = 0; }
            if (n >= 2 && used1 == 0 && rows1 > maxv) { maxv = rows1; maxi = 1; }
            if (n >= 3 && used2 == 0 && rows2 > maxv) { maxv = rows2; maxi = 2; }
            if (n >= 4 && used3 == 0 && rows3 > maxv) { maxv = rows3; maxi = 3; }
            if (n >= 5 && used4 == 0 && rows4 > maxv) { maxv = rows4; maxi = 4; }
            if (n >= 6 && used5 == 0 && rows5 > maxv) { maxv = rows5; maxi = 5; }
            if (n >= 7 && used6 == 0 && rows6 > maxv) { maxv = rows6; maxi = 6; }
            if (n >= 8 && used7 == 0 && rows7 > maxv) { maxv = rows7; maxi = 7; }
            if (n >= 9 && used8 == 0 && rows8 > maxv) { maxv = rows8; maxi = 8; }
            if (maxi < 0) { return; }
            // Marcar usada.
            if      (maxi == 0) { used0 = 1; }
            else if (maxi == 1) { used1 = 1; }
            else if (maxi == 2) { used2 = 1; }
            else if (maxi == 3) { used3 = 1; }
            else if (maxi == 4) { used4 = 1; }
            else if (maxi == 5) { used5 = 1; }
            else if (maxi == 6) { used6 = 1; }
            else if (maxi == 7) { used7 = 1; }
            else if (maxi == 8) { used8 = 1; }
            // Aplicar insert en (maxv, col): posicionar y insertar.
            this.buffer.goto_row_col(maxv, col);
            this.buffer.insert_char(ch);
            done = done + 1;
        }
        // Tras los inserts, el cursor primario quedo en la ultima fila
        // procesada (la mas pequena = prow tipicamente).  Lo restauramos
        // a (prow, col+1) explicitamente para que la columna avance como
        // en un insert normal.
        this.buffer.goto_row_col(prow, col + 1);
    }

    public void mc_apply_backspace() {
        i32 col   = this.mcursors.col_target;
        if (col <= 0) { return; }  // nada que borrar a la izquierda
        i32 prow  = this.buffer.cursor_row();
        i32 i     = 0;
        // Aplicamos en filas en orden descendente (max -> min) para no
        // invalidar offsets de filas inferiores.  Reusamos el bucle del
        // insert pero llamamos a backspace.
        i32 rows0 = prow;
        i32 rows1 = -1; i32 rows2 = -1; i32 rows3 = -1; i32 rows4 = -1;
        i32 rows5 = -1; i32 rows6 = -1; i32 rows7 = -1; i32 rows8 = -1;
        i32 n = 1;
        while (i < this.mcursors.count_extras) {
            i32 r = this.mcursors.row_at(i);
            if (r != prow) {
                if      (n == 1) { rows1 = r; }
                else if (n == 2) { rows2 = r; }
                else if (n == 3) { rows3 = r; }
                else if (n == 4) { rows4 = r; }
                else if (n == 5) { rows5 = r; }
                else if (n == 6) { rows6 = r; }
                else if (n == 7) { rows7 = r; }
                else if (n == 8) { rows8 = r; }
                n = n + 1;
            }
            i = i + 1;
        }
        i32 used0 = 0; i32 used1 = 0; i32 used2 = 0; i32 used3 = 0;
        i32 used4 = 0; i32 used5 = 0; i32 used6 = 0; i32 used7 = 0;
        i32 used8 = 0;
        i32 done = 0;
        while (done < n) {
            i32 maxv = -1; i32 maxi = -1;
            if (used0 == 0 && rows0 > maxv) { maxv = rows0; maxi = 0; }
            if (n >= 2 && used1 == 0 && rows1 > maxv) { maxv = rows1; maxi = 1; }
            if (n >= 3 && used2 == 0 && rows2 > maxv) { maxv = rows2; maxi = 2; }
            if (n >= 4 && used3 == 0 && rows3 > maxv) { maxv = rows3; maxi = 3; }
            if (n >= 5 && used4 == 0 && rows4 > maxv) { maxv = rows4; maxi = 4; }
            if (n >= 6 && used5 == 0 && rows5 > maxv) { maxv = rows5; maxi = 5; }
            if (n >= 7 && used6 == 0 && rows6 > maxv) { maxv = rows6; maxi = 6; }
            if (n >= 8 && used7 == 0 && rows7 > maxv) { maxv = rows7; maxi = 7; }
            if (n >= 9 && used8 == 0 && rows8 > maxv) { maxv = rows8; maxi = 8; }
            if (maxi < 0) { return; }
            if      (maxi == 0) { used0 = 1; }
            else if (maxi == 1) { used1 = 1; }
            else if (maxi == 2) { used2 = 1; }
            else if (maxi == 3) { used3 = 1; }
            else if (maxi == 4) { used4 = 1; }
            else if (maxi == 5) { used5 = 1; }
            else if (maxi == 6) { used6 = 1; }
            else if (maxi == 7) { used7 = 1; }
            else if (maxi == 8) { used8 = 1; }
            this.buffer.goto_row_col(maxv, col);
            this.buffer.backspace();
            done = done + 1;
        }
        // Reposicionar el primario tras el borrado.
        i32 new_col = col - 1;
        if (new_col < 0) { new_col = 0; }
        this.buffer.goto_row_col(prow, new_col);
        // Actualizar columna objetivo del multi-cursor (avanzo a la izquierda).
        this.mcursors.col_target = new_col;
    }

    // Anyade un cursor extra una fila arriba o abajo del primario.
    public void mc_add_below() {
        i32 prow = this.buffer.cursor_row();
        i32 pcol = this.buffer.cursor_col();
        i32 lc   = this.buffer.line_count();
        if (prow + 1 >= lc) { return; }
        this.mcursors.add_row(prow + 1, pcol);
        this.status_msg = "multi-cursor: ${this.mcursors.count_extras} extras";
    }
    public void mc_add_above() {
        i32 prow = this.buffer.cursor_row();
        i32 pcol = this.buffer.cursor_col();
        if (prow <= 0) { return; }
        this.mcursors.add_row(prow - 1, pcol);
        this.status_msg = "multi-cursor: ${this.mcursors.count_extras} extras";
    }
    public void mc_clear() {
        this.mcursors.clear();
        this.status_msg = "multi-cursor: clear";
    }

    // Recalcula prefijo + candidatos y reposiciona el popup en la pantalla.
    // Llamado tras insertar/borrar caracter alfanumerico.
    public void refresh_autocomplete() {
        i32 cr = this.buffer.cursor_row();
        i32 cc = this.buffer.cursor_col();
        // Posicion ANSI: row = cr - viewport_top + 2 (header ocupa fila 1);
        // col = cc + editor_col_offset() + 1.  +1 para colocar el popup
        // JUSTO DEBAJO del cursor (no encima).
        i32 prow = cr - this.viewport_top + 2 + 1;
        i32 pcol = cc + this.editor_col_offset();
        // Si la fila se sale hacia abajo, colocar ENCIMA.
        if (prow + 4 > this.screen.rows) {
            prow = cr - this.viewport_top + 2 - 1 - 8;
            if (prow < 2) { prow = 2; }
        }
        this.autocomplete.maybe_trigger(
            this.buffer.data, this.buffer.length, this.buffer.cursor,
            prow, pcol);
    }

    public void show_help() {
        this.status_msg = "Ctrl+Q=salir Ctrl+S=guardar  Flechas/Home/End/PgUp/PgDn  Tab=4sp  F1=ayuda";
    }

    public void run() {
        this.dlog("run: entry");
        this.screen.clear();
        this.dlog("run: post screen.clear");
        this.render();
        this.dlog("run: post first render, entering loop");
        while (this.running == 1) {
            this.dlog("run: top of loop");
            i32 k = this.input.read_key_blocking();
            this.dlog_i("run: got key=", k);
            this.handle_key(k);
            this.dlog_i("run: post handle_key, running=", this.running);
            this.render();
            this.dlog("run: post render");
        }
        this.dlog("run: exit loop");
        // Pantalla limpia + cursor visible al salir.  Usamos los builtins
        // term_* (mas legibles que las secuencias ANSI raw del Screen).
        term_clear();
        term_show_cursor();
        term_reset();
        flush();
        println("Hasta luego desde VEXED.");
        flush();
    }
}
