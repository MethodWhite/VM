// =============================================================================
// modules/sidebar.vh - Panel lateral con arbol de archivos
// =============================================================================
//
// Equivalente a la clase FileTree de vnano.vsh.  Muestra una lista jerarquica
// de archivos del directorio actual (con subdirectorios expandibles) y permite
// abrir archivos seleccionados con Enter.
//
// Activable con Ctrl+B (toggle).  Foco intercambiable con el editor con F6
// (rota foco) o Ctrl+0 (foco al sidebar) / Ctrl+1 (foco al editor).
//
// Implementacion basada en FileExplorer (modules/file_explorer.vh) que ya hace
// FFI a kernel32.dll/FindFirstFileA.  Aqui anadimos:
//   - render columnar con scroll
//   - expand/collapse por subdirectorio
//   - navegacion con arrow keys
//   - flag de visibilidad togglable
//
// El layout es: [sidebar de N cols][editor area de cols-N-1].  Por defecto
// el sidebar ocupa 25 cols.  Cuando esta hidden (visible == 0), el editor
// usa cols completos.

class TreeNode {
    public string   name;     // basename del entry
    public string   path;     // path completo (relativo al root)
    public i32      is_dir;
    public i32      depth;    // nivel de indentacion (0 = root)
    public i32      expanded; // 1 = mostrar children (solo si is_dir)
    public TreeNode next;     // siguiente nodo en la lista plana

    public TreeNode() {
        this.name     = "";
        this.path     = "";
        this.is_dir   = 0;
        this.depth    = 0;
        this.expanded = 0;
        this.next     = null;
    }
}

class Sidebar {
    public FileExplorer fexp;
    public TreeNode     root_list;     // lista plana de nodos visibles
    public i32          n_visible;     // count de nodos en root_list
    public i32          selected_idx;  // 0-based dentro de la lista visible
    public i32          scroll_top;    // primer indice visible (scroll)
    public i32          width;         // cols del sidebar (default 25)
    public i32          visible;       // 0 = oculto, 1 = mostrado
    public i32          focused;       // 0 = editor tiene foco, 1 = sidebar
    public Screen       screen;        // ref para acceso a cols/rows + ANSI
    public string       root_path;     // path del proyecto (para construir paths)

    public Sidebar(Screen scr) {
        this.fexp         = new FileExplorer();
        this.root_list    = null;
        this.n_visible    = 0;
        this.selected_idx = 0;
        this.scroll_top   = 0;
        this.width        = 25;
        // Visible por defecto al arrancar (estilo vnano).  El usuario puede
        // cerrarlo con Ctrl+B si prefiere usar todo el ancho.
        this.visible      = 1;
        this.focused      = 0;
        this.screen       = scr;
        this.root_path    = ".";
    }

    // Reconstruye root_list desde el directorio actual.  Filtra "." y ".."
    // (ya filtrados por FileExplorer) y ordena de forma simple: dirs antes
    // que archivos, ambos en orden de FindFirstFile (que en NTFS suele ser
    // alfabetico).
    public void refresh() {
        DirEntry entries = this.fexp.list_current_dir();
        this.root_list = null;
        this.n_visible = 0;
        // Pase 1: directorios primero
        DirEntry p = entries;
        while (p != null) {
            if (p.is_dir == 1) {
                TreeNode n = new TreeNode();
                n.name     = p.name;
                n.path     = p.name;
                n.is_dir   = 1;
                n.depth    = 0;
                n.expanded = 0;
                n.next     = this.root_list;
                this.root_list = n;
                this.n_visible = this.n_visible + 1;
            }
            p = p.next;
        }
        // Pase 2: archivos despues (los anyadimos al final reverse-iterando)
        // Implementacion simple: append en orden de descubrimiento.
        TreeNode tail = this.root_list;
        if (tail != null) {
            while (tail.next != null) { tail = tail.next; }
        }
        p = entries;
        while (p != null) {
            if (p.is_dir == 0) {
                TreeNode n = new TreeNode();
                n.name     = p.name;
                n.path     = p.name;
                n.is_dir   = 0;
                n.depth    = 0;
                n.expanded = 0;
                n.next     = null;
                if (tail == null) { this.root_list = n; tail = n; }
                else              { tail.next = n; tail = n; }
                this.n_visible = this.n_visible + 1;
            }
            p = p.next;
        }
        if (this.selected_idx >= this.n_visible) {
            this.selected_idx = this.n_visible - 1;
        }
        if (this.selected_idx < 0) { this.selected_idx = 0; }
    }

    // Toggle visibilidad del sidebar.  Cuando se muestra por primera vez,
    // refresca la lista de archivos automaticamente.
    public void toggle_visible() {
        if (this.visible == 0) {
            this.visible = 1;
            this.focused = 1;
            if (this.root_list == null) { this.refresh(); }
        } else {
            this.visible = 0;
            this.focused = 0;
        }
    }

    // Mover seleccion arriba/abajo, ajustar scroll automaticamente.
    public void move_up() {
        if (this.selected_idx > 0) {
            this.selected_idx = this.selected_idx - 1;
            if (this.selected_idx < this.scroll_top) {
                this.scroll_top = this.selected_idx;
            }
        }
    }

    public void move_down() {
        i32 max_visible = this.viewport_height();
        if (this.selected_idx < this.n_visible - 1) {
            this.selected_idx = this.selected_idx + 1;
            if (this.selected_idx >= this.scroll_top + max_visible) {
                this.scroll_top = this.selected_idx - max_visible + 1;
            }
        }
    }

    // Numero de filas disponibles para listing (excluye header + status bar).
    public i32 viewport_height() {
        // header (fila 1) + footer (ultimo) + 1 fila titulo del sidebar = 3
        i32 h = this.screen.rows - 3;
        if (h < 1) { h = 1; }
        return h;
    }

    // Devuelve el TreeNode en posicion idx (0-based) de la lista plana.
    public TreeNode node_at(i32 idx) {
        if (idx < 0 || idx >= this.n_visible) { return null; }
        TreeNode p = this.root_list;
        i32 i = 0;
        while (p != null && i < idx) {
            p = p.next;
            i = i + 1;
        }
        return p;
    }

    // Devuelve el path del entry seleccionado.  Vacio si no hay seleccion.
    public string selected_path() {
        TreeNode n = this.node_at(this.selected_idx);
        if (n == null) { return ""; }
        return n.path;
    }

    // Devuelve 1 si la seleccion actual es directorio.
    public i32 selected_is_dir() {
        TreeNode n = this.node_at(this.selected_idx);
        if (n == null) { return 0; }
        return n.is_dir;
    }

    // Render del sidebar en las primeras `this.width` columnas.  El
    // editor area empieza en columna `this.width + 1`.  No emite el
    // separador vertical (lo hace render_separator).
    public void render() {
        if (this.visible == 0) { return; }
        i32 w = this.width;
        // Titulo
        this.screen.move_cursor(2, 1);
        if (this.focused == 1) { print(REVERSE); print(BOLD); print(CYAN); }
        else                   { print(REVERSE); print(DIM); }
        print(" Files ");
        // Padding hasta width
        i32 ti = 7;
        while (ti < w) { print(" "); ti = ti + 1; }
        print(RESET);

        // Listar entries
        i32 vh = this.viewport_height();
        i32 y  = 0;
        while (y < vh) {
            i32 idx = this.scroll_top + y;
            this.screen.move_cursor(3 + y, 1);
            if (idx < this.n_visible) {
                TreeNode n = this.node_at(idx);
                this.render_entry(n, idx, w);
            } else {
                // Linea vacia (rellenar para no ver basura del editor)
                i32 c = 0;
                while (c < w) { print(" "); c = c + 1; }
            }
            y = y + 1;
        }
    }

    // Render de una entrada con su prefijo (icono dir/file) y resaltado
    // si es la seleccionada.
    public void render_entry(TreeNode n, i32 idx, i32 w) {
        i32 selected = 0;
        if (idx == this.selected_idx && this.focused == 1) { selected = 1; }
        if (selected == 1) { print(REVERSE); }
        // Icono: > para dir, espacio para archivo
        if (n.is_dir == 1) {
            print(YELLOW);
            print("> ");
            print(RESET);
            if (selected == 1) { print(REVERSE); }
            print(BOLD);
            print(n.name);
            print(RESET);
            if (selected == 1) { print(REVERSE); }
        } else {
            print("  ");
            print(n.name);
        }
        // Padding hasta ancho del sidebar (para no dejar caracteres sueltos
        // del frame anterior).
        i32 used = 2 + str_bytes(n.name);
        i32 pad  = w - used;
        while (pad > 0) { print(" "); pad = pad - 1; }
        if (selected == 1) { print(RESET); }
    }

    // Linea vertical separadora entre sidebar y editor area.  Llamarse
    // despues de render_buffer del editor.
    public void render_separator() {
        if (this.visible == 0) { return; }
        i32 vh = this.viewport_height() + 1;
        i32 y  = 0;
        while (y <= vh) {
            this.screen.move_cursor(2 + y, this.width + 1);
            print(DIM);
            print("|");
            print(RESET);
            y = y + 1;
        }
    }

    // Indica si el sidebar consumio la tecla `k`.  El llamante solo invoca
    // este metodo cuando this.focused == 1.  Devuelve 1 si la consumio.
    // Up/Down navegan, Enter abre archivo (o expande directorio en futuro),
    // Esc/Ctrl+1 devuelve foco al editor.
    public i32 handle_key_focused(i32 k) {
        if (k == KEY_UP)   { this.move_up();   return 1; }
        if (k == KEY_DOWN) { this.move_down(); return 1; }
        // Enter no se procesa aqui -- el editor lo intercepta para abrir el
        // archivo y mover foco a la nueva tab.  Devolvemos 0 para que el
        // editor lo vea.
        return 0;
    }
}
