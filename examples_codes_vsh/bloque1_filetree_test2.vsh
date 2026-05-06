// =============================================================================
// vnano BLOQUE 1 v2 - Config + FileTree (modulo de prueba)
// =============================================================================
// Fixes sobre v1:
//   - Sustituir "\xe2\x96\xbe" por from_char(226)+from_char(150)+from_char(190)
//     porque el lexer de VSH no interpreta escapes hex
// =============================================================================


let ESC          = substr(ANSI["CLEAR_LINE"], 0, 1)
let ESC_HOME     = ESC + "[H"
let ESC_CUR_HIDE = ESC + "[?25l"
let ESC_CUR_SHOW = ESC + "[?25h"
let ESC_CLR_EOL  = ESC + "[K"
let ESC_CLR_EOS  = ESC + "[J"

// Caracteres Unicode para el arbol, construidos byte a byte porque
// el lexer de VSH no soporta \xNN.
//   ▾ U+25BE = bytes UTF-8: 0xE2 0x96 0xBE = 226 150 190
//   ▸ U+25B8 = bytes UTF-8: 0xE2 0x96 0xB8 = 226 150 184
let TRI_DOWN = from_char(226) + from_char(150) + from_char(190)
let TRI_RIGHT = from_char(226) + from_char(150) + from_char(184)


// =============================================================================
// Config: persistente en .vnano.json del cwd, con defaults sensatos.
// =============================================================================

class Config {
    "Configuracion del editor con persistencia en .vnano.json."

    fn __init__(self) {
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

    fn _parse(self, txt) {
        let v = self._extract_num(txt, "sidebar_width")
        if v != null and v >= 10 and v <= 100 { self.sidebar_width = v }

        let b = self._extract_bool(txt, "show_hidden")
        if b != null { self.show_hidden = b }

        let b2 = self._extract_bool(txt, "use_unicode")
        if b2 != null { self.use_unicode = b2 }

        let lst = self._extract_string_list(txt, "ignore_patterns")
        if lst != null { self.ignore_patterns = lst }
    }

    fn _extract_num(self, txt, key) {
        let needle = "\"" + key + "\""
        let pos = find_str(txt, needle, 0)
        if pos == -1 { return null }
        let i = pos + len(needle)
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
        let pt = find_str(rest, "true", 0)
        let pf = find_str(rest, "false", 0)
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
// FileTree
// =============================================================================

class FileTree {
    "Arbol de ficheros lazy con render configurable Unicode/ASCII."

    fn __init__(self, root_path, config) {
        self.root_path = root_path
        self.config = config
        self.root = self._make_node(root_path, basename_or(root_path, "/"), true, 0)
        self.expand(self.root)
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
        if node["expanded"] {
            self.collapse(node)
        } else {
            self.expand(node)
        }
    }

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

        dirs = self._sort_by_name(dirs)
        files = self._sort_by_name(files)

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
        if len(dir) == 0 { return name }
        let last = substr(dir, len(dir) - 1, 1)
        if last == "/" or last == "\\" {
            return dir + name
        }
        return dir + "/" + name
    }

    fn _sort_by_name(self, lst) {
        let n = len(lst)
        let i = 0
        while i < n {
            let j = 0
            while j < n - 1 - i {
                let a = lst[j]; let b = lst[j+1]
                if lower(a["name"]) > lower(b["name"]) {
                    lst[j] = b; lst[j+1] = a
                }
                j = j + 1
            }
            i = i + 1
        }
        return lst
    }

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

    fn render_line(self, node, width, selected, is_active_file) {
        let u = self.config.use_unicode

        let folder_open = "v "
        let folder_closed = "> "
        let file_marker = "  "
        if u {
            folder_open = TRI_DOWN + " "
            folder_closed = TRI_RIGHT + " "
        }

        let indent = repeat(" ", node["depth"] * 2)

        let icon = file_marker
        if node["is_dir"] {
            if node["expanded"] {
                icon = folder_open
            } else {
                icon = folder_closed
            }
        }

        let name = node["name"]
        if node["is_dir"] { name = name + "/" }

        let raw = indent + icon + name
        let visual_width = self._visual_len(raw, u)

        if visual_width > width {
            // Truncado con cuidado: cortamos por bytes, ajustamos visual
            raw = substr(raw, 0, width - 1) + ">"
            visual_width = self._visual_len(raw, u)
        }
        if visual_width < width {
            raw = raw + repeat(" ", width - visual_width)
        }

        let color = ""
        if node["is_dir"] {
            color = ansi_rgb(80, 150, 255)
        } else {
            color = ansi_rgb(80, 250, 123)
        }

        let result = ""
        if selected and is_active_file {
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

    // Calcula el "ancho visible" en columnas. Las secuencias UTF-8 multibyte
    // como ▾ y ▸ ocupan 3 bytes pero 1 columna.
    fn _visual_len(self, s, has_unicode) {
        if not has_unicode { return len(s) }
        let n = len(s)
        let i = 0
        let visible = 0
        while i < n {
            let cc = char_code(substr(s, i, 1))
            if cc < 128 {
                visible = visible + 1
                i = i + 1
            } elif cc >= 192 and cc < 224 {
                visible = visible + 1
                i = i + 2
            } elif cc >= 224 and cc < 240 {
                visible = visible + 1
                i = i + 3
            } elif cc >= 240 {
                visible = visible + 1
                i = i + 4
            } else {
                i = i + 1
            }
        }
        return visible
    }
}


// =============================================================================
// Helpers
// =============================================================================

fn basename_or(path, fallback) {
    if path == "" { return fallback }
    let i = len(path) - 1
    while i >= 0 {
        let c = substr(path, i, 1)
        if c == "/" or c == "\\" {
            if i == len(path) - 1 {
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

fn is_dir_safe(path) {
    try {
        return is_dir(path)
    } catch e {
        try {
            let _ = listdir(path)
            return true
        } catch e2 {
            return false
        }
    }
}


// =============================================================================
// MAIN de prueba
// =============================================================================

fn main() {
    let cfg = Config()
    println("Config cargada:")
    println("  sidebar_width:   " + str(cfg.sidebar_width))
    println("  show_hidden:     " + str(cfg.show_hidden))
    println("  use_unicode:     " + str(cfg.use_unicode))
    println("  ignore_patterns: " + str(len(cfg.ignore_patterns)) + " patrones")
    println("")

    println("Caracteres Unicode (deberian verse como triangulos):")
    println("  TRI_DOWN  bytes: " + str(char_code(substr(TRI_DOWN, 0, 1))) + " " + str(char_code(substr(TRI_DOWN, 1, 1))) + " " + str(char_code(substr(TRI_DOWN, 2, 1))))
    println("  TRI_DOWN  visual: '" + TRI_DOWN + "'")
    println("  TRI_RIGHT visual: '" + TRI_RIGHT + "'")
    println("")

    let cwd = getcwd()
    println("Construyendo arbol de: " + cwd)
    let tree = FileTree(cwd, cfg)

    let visible = tree.list_visible()
    println("Visibles inicialmente: " + str(len(visible)))
    println("")

    println("Render Unicode (primeros 10):")
    let i = 0
    while i < len(visible) and i < 10 {
        let node = visible[i]
        let line = tree.render_line(node, 50, i == 0, false)
        println(line)
        i = i + 1
    }

    println("")
    println("Render ASCII (primeros 10):")
    cfg.use_unicode = false
    let j = 0
    while j < len(visible) and j < 10 {
        let node = visible[j]
        let line = tree.render_line(node, 50, j == 0, false)
        println(line)
        j = j + 1
    }
    cfg.use_unicode = true

    println("")
    println("Tests:")
    println("[1] Filtros - 'node_modules' deberia estar en ignore_patterns:")
    println("    -> " + str(tree._is_ignored("node_modules")))

    println("[2] Lista visible empieza por carpetas:")
    if len(visible) > 0 {
        println("    -> " + visible[0]["name"] + " is_dir=" + str(visible[0]["is_dir"]))
    }

    println("[3] _visual_len de 'hola':         " + str(tree._visual_len("hola", false)))
    println("[4] _visual_len de TRI_DOWN+'x':   " + str(tree._visual_len(TRI_DOWN + "x", true)))
    println("    (deberia ser 2: triangulo + x)")

    println("")
    println("Si los triangulos se ven correctamente arriba, el bloque 1 esta OK.")
}

main()
