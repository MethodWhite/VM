// =============================================================================
// vnano BLOQUE 1 - Config + FileTree (modulo de prueba)
// =============================================================================
// Pruebalo asi:
//   vesta --script bloque1_filetree_test.vsh
//
// =============================================================================

let ESC          = substr(ANSI["CLEAR_LINE"], 0, 1)
let ESC_HOME     = ESC + "[H"
let ESC_CUR_HIDE = ESC + "[?25l"
let ESC_CUR_SHOW = ESC + "[?25h"
let ESC_CLR_EOL  = ESC + "[K"
let ESC_CLR_EOS  = ESC + "[J"


// =============================================================================
// Config: persistente en .vnano.json del cwd, con defaults sensatos.
// =============================================================================
// Estructura del fichero:
//   {
//     "sidebar_width": 28,
//     "show_hidden": false,
//     "use_unicode": true,
//     "ignore_patterns": [".git", "node_modules", "build", "target", ...]
//   }
//
// El parser de JSON es minimo (solo lo que necesitamos). Si falla, usamos
// defaults sin avisar - el editor no debe morir por una config mala.

class Config {
    "Configuracion del editor con persistencia en .vnano.json."

    fn __init__(self) {
        // Defaults
        self.sidebar_width = 28
        self.show_hidden = false
        self.use_unicode = true
        self.ignore_patterns = [".git", "node_modules", "build", "target",
                                "__pycache__", ".vscode", ".idea", "dist",
                                ".cache", ".pytest_cache"]
        self.path = ".vnano.json"
        self._load()
    }

    fn _load(self) {
        if not exists(self.path) { return }
        try {
            let txt = read_file(self.path)
            self._parse(txt)
        } catch e {
            // Config mala: ignoramos y seguimos con defaults
        }
    }

    fn save(self) {
        let lst = "["
        let i = 0
        while i < len(self.ignore_patterns) {
            if i > 0 { lst = lst + ", " }
            lst = lst + "\"" + self.ignore_patterns[i] + "\""
            i = i + 1
        }
        lst = lst + "]"
        let txt = "{\n"
        txt = txt + "  \"sidebar_width\": " + str(self.sidebar_width) + ",\n"
        txt = txt + "  \"show_hidden\": " + str(self.show_hidden) + ",\n"
        txt = txt + "  \"use_unicode\": " + str(self.use_unicode) + ",\n"
        txt = txt + "  \"ignore_patterns\": " + lst + "\n"
        txt = txt + "}\n"
        try {
            write_file(self.path, txt)
            return true
        } catch e {
            return false
        }
    }

    // Parser JSON minimo: solo lee los 4 campos que nos importan.
    // No es un parser completo (no maneja escapes, anidamiento profundo,
    // etc.), pero basta para nuestros propios ficheros bien formados.
    fn _parse(self, txt) {
        // sidebar_width: numero
        let v = self._extract_num(txt, "sidebar_width")
        if v != null and v >= 10 and v <= 100 { self.sidebar_width = v }

        // show_hidden: bool
        let b = self._extract_bool(txt, "show_hidden")
        if b != null { self.show_hidden = b }

        // use_unicode: bool
        let b2 = self._extract_bool(txt, "use_unicode")
        if b2 != null { self.use_unicode = b2 }

        // ignore_patterns: lista de strings
        let lst = self._extract_string_list(txt, "ignore_patterns")
        if lst != null { self.ignore_patterns = lst }
    }

    fn _extract_num(self, txt, key) {
        let needle = "\"" + key + "\""
        let pos = find_str(txt, needle, 0)
        if pos == -1 { return null }
        let i = pos + len(needle)
        // Saltar espacios y ':'
        while i < len(txt) and (substr(txt, i, 1) == " " or substr(txt, i, 1) == ":") {
            i = i + 1
        }
        let start = i
        while i < len(txt) {
            let c = substr(txt, i, 1)
            let cc = char_code(c)
            if cc < 48 or cc > 57 { break }
            i = i + 1
        }
        if i == start { return null }
        let s = substr(txt, start, i - start)
        if not is_numeric(s) { return null }
        return int(s)
    }

    fn _extract_bool(self, txt, key) {
        let needle = "\"" + key + "\""
        let pos = find_str(txt, needle, 0)
        if pos == -1 { return null }
        let rest = substr(txt, pos + len(needle), len(txt) - (pos + len(needle)))
        // Buscamos "true" o "false" tras los : y espacios
        let pt = find_str(rest, "true", 0)
        let pf = find_str(rest, "false", 0)
        // El primero que aparezca antes de un newline o coma o }
        let stop = self._first_stop(rest)
        let true_ok = pt != -1 and pt < stop
        let false_ok = pf != -1 and pf < stop
        if true_ok and (not false_ok or pt < pf) { return true }
        if false_ok { return false }
        return null
    }

    fn _first_stop(self, s) {
        let p1 = find_str(s, ",", 0)
        let p2 = find_str(s, "}", 0)
        let p3 = find_str(s, "\n", 0)
        let best = len(s)
        if p1 != -1 and p1 < best { best = p1 }
        if p2 != -1 and p2 < best { best = p2 }
        if p3 != -1 and p3 < best { best = p3 }
        return best
    }

    fn _extract_string_list(self, txt, key) {
        let needle = "\"" + key + "\""
        let pos = find_str(txt, needle, 0)
        if pos == -1 { return null }
        let lb = find_str(txt, "[", pos)
        if lb == -1 { return null }
        let rb = find_str(txt, "]", lb)
        if rb == -1 { return null }
        let inner = substr(txt, lb + 1, rb - lb - 1)
        // Extraer cada "..." dentro
        let result = []
        let i = 0
        while i < len(inner) {
            if substr(inner, i, 1) == "\"" {
                let j = i + 1
                while j < len(inner) and substr(inner, j, 1) != "\"" {
                    j = j + 1
                }
                if j < len(inner) {
                    append(result, substr(inner, i + 1, j - i - 1))
                    i = j + 1
                } else {
                    break
                }
            } else {
                i = i + 1
            }
        }
        return result
    }
}


// =============================================================================
// FileTree: arbol de ficheros con expansion lazy.
// =============================================================================
// Cada nodo es un map con:
//   "name":     basename del fichero/carpeta
//   "path":     ruta absoluta o relativa al cwd
//   "is_dir":   true/false
//   "expanded": true si la carpeta esta abierta (mostrando hijos)
//   "loaded":   true si ya se ha leido el contenido al menos una vez
//   "children": lista de nodos hijos (vacia hasta que loaded=true)
//   "depth":    profundidad para indentacion (0 = raiz)
//
// El arbol se "aplana" para renderizado: list_visible() recorre el arbol
// en preorden y devuelve la lista de nodos visibles (la raiz mas las
// carpetas expandidas y sus hijos directos).
//

class FileTree {
    "Arbol de ficheros lazy con render configurable Unicode/ASCII."

    fn __init__(self, root_path, config) {
        self.root_path = root_path
        self.config = config
        // El nodo raiz no se muestra (su nombre seria el del cwd); se
        // muestran sus hijos directos como nivel 0.
        self.root = self._make_node(root_path, basename_or(root_path, "/"), true, 0)
        self.expand(self.root)   // raiz siempre expandida
    }

    fn _make_node(self, path, name, is_dir, depth) {
        return {
            "name": name,
            "path": path,
            "is_dir": is_dir,
            "expanded": false,
            "loaded": false,
            "children": [],
            "depth": depth
        }
    }

    // Expande una carpeta (carga sus hijos si aun no estan cargados).
    fn expand(self, node) {
        if not node["is_dir"] { return }
        if not node["loaded"] {
            node["children"] = self._read_dir(node["path"], node["depth"] + 1)
            node["loaded"] = true
        }
        node["expanded"] = true
    }

    fn collapse(self, node) {
        if not node["is_dir"] { return }
        node["expanded"] = false
    }

    fn toggle(self, node) {
        if node["expanded"] { self.collapse(node) 
        } else { self.expand(node) }
    }

    // Lee el contenido de una carpeta. Devuelve lista de nodos hijos
    // ordenados: carpetas primero (alfabetico), luego ficheros (alfabetico).
    // Filtra segun ignore_patterns y show_hidden.
    fn _read_dir(self, path, depth) {
        let entries = []
        try {
            entries = listdir(path)
        } catch e {
            return []
        }

        let dirs = []
        let files = []

        for name in entries {
            // Filtros
            if self._is_ignored(name) { continue }
            if not self.config.show_hidden and starts_with(name, ".") { continue }

            let full = self._join_path(path, name)
            let is_dir = false
            try {
                is_dir = is_dir_safe(full)
            } catch e {
                is_dir = false
            }

            let node = self._make_node(full, name, is_dir, depth)
            if is_dir {
                append(dirs, node)
            } else {
                append(files, node)
            }
        }

        // Ordenar alfabeticamente cada lista
        dirs = self._sort_by_name(dirs)
        files = self._sort_by_name(files)

        // Concatenar: dirs primero
        let result = []
        for d in dirs { append(result, d) }
        for f in files { append(result, f) }
        return result
    }

    fn _is_ignored(self, name) {
        for pat in self.config.ignore_patterns {
            if name == pat { return true }
        }
        return false
    }

    fn _join_path(self, dir, name) {
        // Manejo simple de separador: si el dir acaba en / o \, no anadir
        // otro; si no, anadir / (Windows acepta tambien /).
        if len(dir) == 0 { return name }
        let last = substr(dir, len(dir) - 1, 1)
        if last == "/" or last == "\\" {
            return dir + name
        }
        return dir + "/" + name
    }

    fn _sort_by_name(self, lst) {
        // Bubble sort. n suele ser <100 por carpeta.
        let n = len(lst)
        let i = 0
        while i < n {
            let j = 0
            while j < n - 1 - i {
                let a = lst[j]; let b = lst[j+1]
                // Comparacion case-insensitive: convertir a minusculas
                if lower(a["name"]) > lower(b["name"]) {
                    lst[j] = b; lst[j+1] = a
                }
                j = j + 1
            }
            i = i + 1
        }
        return lst
    }

    // Devuelve la lista plana de nodos visibles. Cada elemento es el
    // mismo map del nodo, con "depth" para indentar al renderizar.
    fn list_visible(self) {
        let out = []
        self._walk(self.root, out, true)
        return out
    }

    fn _walk(self, node, out, skip_self) {
        if not skip_self { append(out, node) }
        if node["is_dir"] and node["expanded"] and node["loaded"] {
            for child in node["children"] {
                self._walk(child, out, false)
            }
        }
    }

    // Render de UN nodo a una linea con colores. Devuelve string con ANSI.
    // Esta funcion no imprime nada; solo construye la cadena.
    fn render_line(self, node, width, selected, is_active_file) {
        let u = self.config.use_unicode

        // Caracteres del arbol
        let folder_open    = "v "       // "▾ "
        let folder_closed  = "> "       // "▸ "
        let file_marker    = "  "
        if u {
            folder_open   = "\xe2\x96\xbe "   // ▾  (UTF-8: E2 96 BE)
            folder_closed = "\xe2\x96\xb8 "   // ▸  (UTF-8: E2 96 B8)
        }

        // Indent: 2 espacios por nivel
        let indent = repeat(" ", node["depth"] * 2)

        let icon = file_marker
        if node["is_dir"] {
            if node["expanded"] { icon = folder_open 
            } else { icon = folder_closed }
        }

        let name = node["name"]
        if node["is_dir"] { name = name + "/" }

        // Truncar si pasa de width
        let raw = indent + icon + name
        if len(raw) > width {
            raw = substr(raw, 0, width - 1) + ">"
        } else {
            raw = raw + repeat(" ", width - len(raw))
        }

        // Colores
        let color = ""
        if node["is_dir"] {
            color = ansi_rgb(80, 150, 255)            // azul brillante
        } else {
            color = ansi_rgb(80, 250, 123)            // verde Dracula
        }

        let result = ""
        if selected and is_active_file {
            // Fichero activo Y bajo cursor: fondo doble distinto
            result = ANSI["REVERSE"] + ANSI["BOLD"] + color + raw + ANSI["RESET"]
        } elif selected {
            result = ansi_rgb_bg(68, 71, 90) + color + raw + ANSI["RESET"]
        } elif is_active_file {
            result = ANSI["BOLD"] + color + raw + ANSI["RESET"]
        } else {
            result = color + raw + ANSI["RESET"]
        }

        return result
    }
}


// =============================================================================
// Helpers globales (algunos podrian estar ya en VSH como builtins; los
// envuelvo defensivamente)
// =============================================================================

fn basename_or(path, fallback) {
    if path == "" { return fallback }
    // Buscar el ultimo / o \
    let i = len(path) - 1
    while i >= 0 {
        let c = substr(path, i, 1)
        if c == "/" or c == "\\" {
            if i == len(path) - 1 {
                // Trailing slash: quitar y volver a buscar
                path = substr(path, 0, len(path) - 1)
                if len(path) == 0 { return fallback }
                i = len(path) - 1
                continue
            }
            return substr(path, i + 1, len(path) - i - 1)
        }
        i = i - 1
    }
    return path
}

// Wrapper defensivo para is_dir(). Si no existe el builtin, intentamos
// otra cosa.
fn is_dir_safe(path) {
    try {
        return is_dir(path)
    } catch e {
        // Fallback: probar listar el path; si funciona, es directorio
        try {
            let _ = listdir(path)
            return true
        } catch e2 {
            return false
        }
    }
}


// =============================================================================
// MAIN de prueba: muestra el arbol en un loop interactivo
// =============================================================================
// Atajos en esta prueba:
//   Up/Down      mover seleccion
//   Enter o Right expandir/abrir
//   Left          colapsar
//   . (punto)     toggle ocultos
//   u             toggle unicode/ascii
//   s             save config
//   q             salir
//
// El objetivo es que veas el arbol funcionando antes de integrarlo.

fn main() {
    let cfg = Config()
    println("Config cargada:")
    println("  sidebar_width:   " + str(cfg.sidebar_width))
    println("  show_hidden:     " + str(cfg.show_hidden))
    println("  use_unicode:     " + str(cfg.use_unicode))
    println("  ignore_patterns: " + str(cfg.ignore_patterns))
    println("")

    let cwd = getcwd()
    println("Construyendo arbol de: " + cwd)
    let tree = FileTree(cwd, cfg)

    let visible = tree.list_visible()
    println("Visibles inicialmente: " + str(len(visible)))
    println("")

    // Mostrar primeros 30 nodos como prueba estatica
    let i = 0
    while i < len(visible) and i < 30 {
        let node = visible[i]
        let line = tree.render_line(node, 50, i == 0, false)
        println(line)
        i = i + 1
    }
    if len(visible) > 30 {
        println("... y " + str(len(visible) - 30) + " mas")
    }

    println("")
    println("Pruebas:")

    // Test 1: filtros
    println("[1] Filtros - 'node_modules' deberia estar en ignore_patterns:")
    println("    -> " + str(tree._is_ignored("node_modules")))

    // Test 2: filtros con un nombre normal
    println("[2] Filtros - 'src' NO deberia estar:")
    println("    -> " + str(tree._is_ignored("src")))

    // Test 3: ordenacion
    println("[3] Lista visible empieza por carpetas (la primera deberia ser dir):")
    if len(visible) > 0 {
        println("    -> " + visible[0]["name"] + " is_dir=" + str(visible[0]["is_dir"]))
    }

    // Test 4: render unicode vs ascii
    println("[4] Render del primer nodo en Unicode vs ASCII:")
    if len(visible) > 0 {
        cfg.use_unicode = true
        let l1 = tree.render_line(visible[0], 40, false, false)
        cfg.use_unicode = false
        let l2 = tree.render_line(visible[0], 40, false, false)
        println("    Unicode: " + l1)
        println("    ASCII:   " + l2)
        cfg.use_unicode = true
    }

    println("")
    println("Si todo se ve bien, el bloque 1 esta OK.")
}

main()
