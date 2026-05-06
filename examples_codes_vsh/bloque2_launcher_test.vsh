// =============================================================================
// vnano BLOQUE 2 - Launcher TUI (modulo de prueba autocontenido)
// =============================================================================
// Pruebalo:
//   vesta --script bloque2_launcher_test.vsh
//
// Atajos del launcher:
//   Up/Down       mover seleccion
//   Enter         abrir fichero o ENTRAR en carpeta
//   Right         ENTRAR en carpeta (no abrir como proyecto)
//   Left          subir a carpeta padre (..)
//   p             abrir la carpeta ACTUAL como PROYECTO
//   n             nuevo fichero (pide nombre)
//   .             toggle ocultos
//   u             toggle Unicode/ASCII
//   q / Esc       cancelar
//   /             buscar (filtra entradas en vivo)
//
// Diferencia clave entre Enter sobre carpeta y 'p':
//   Enter         entra en la carpeta para navegar
//   p             abre la carpeta como proyecto (sale del launcher
//                 retornando un dict { "mode": "project", "path": ... })
//
// Resultado: el launcher devuelve un dict con uno de estos modos:
//   { "mode": "file",    "path": "ruta/al/fichero.vsh" }
//   { "mode": "new",     "path": "nombre_nuevo.vsh"    }
//   { "mode": "project", "path": "ruta/a/la/carpeta"   }
//   { "mode": "cancel"                                  }
// =============================================================================


// ---- Constantes ANSI ----

let ESC          = substr(ANSI["CLEAR_LINE"], 0, 1)
let ESC_HOME     = ESC + "[H"
let ESC_CUR_HIDE = ESC + "[?25l"
let ESC_CUR_SHOW = ESC + "[?25h"
let ESC_CLR_EOL  = ESC + "[K"
let ESC_CLR_EOS  = ESC + "[J"

let TRI_DOWN  = from_char(226) + from_char(150) + from_char(190)
let TRI_RIGHT = from_char(226) + from_char(150) + from_char(184)


// ---- Codigos de tecla (subset minimo) ----

let KEY_ENTER     = 13
let KEY_TAB       = 9
let KEY_BACKSPACE = 8
let KEY_DELETE    = 127
let KEY_ESC       = 27

let KEY_UP    = -1000
let KEY_DOWN  = -1001
let KEY_LEFT  = -1002
let KEY_RIGHT = -1003
let KEY_HOME  = -1004
let KEY_END   = -1005
let KEY_PGUP  = -1006
let KEY_PGDN  = -1007
let KEY_DEL   = -1008


// =============================================================================
// InputBackend (mismo que vnano.vsh, copiado para que el modulo funcione solo)
// =============================================================================

class InputBackend {
    fn __init__(self) {
        self.os = platform()
        self.lib = 0
        self.kbhit_sym = 0
        self.getch_sym = 0
        self.getchar_sym = 0
        self._setup()
    }

    fn _setup(self) {
        if self.os == "windows" {
            self.lib = ffi_open("msvcrt.dll")
            self.kbhit_sym = ffi_sym(self.lib, "_kbhit")
            self.getch_sym = ffi_sym(self.lib, "_getch")
        } else {
            let libname = "libc.so.6"
            if self.os == "macos" { libname = "libc.dylib" }
            self.lib = ffi_open(libname)
            self.getchar_sym = ffi_sym(self.lib, "getchar")
            shell("stty -icanon -echo -ixon -ixoff min 0 time 0")
        }
    }

    fn read_key_blocking(self) {
        while true {
            let k = self.poll_key()
            if k != -1 { return k }
            sleep(4)
        }
        return -1
    }

    fn poll_key(self) {
        if self.os == "windows" {
            let n = ffi_call(self.kbhit_sym)
            if n == 0 { return -1 }
            let c = ffi_call(self.getch_sym)
            if c == 0 or c == 224 {
                let c2 = ffi_call(self.getch_sym)
                return self._win_special(c2)
            }
            return c
        } else {
            let c = ffi_call(self.getchar_sym)
            if c == -1 { return -1 }
            if c == 27 {
                let c1 = self._poll_with_timeout(15)
                if c1 == -1 { return KEY_ESC }
                if c1 != 91 { return KEY_ESC }
                let c2 = self._poll_with_timeout(15)
                if c2 == -1 { return KEY_ESC }
                return self._posix_special(c2)
            }
            return c
        }
    }

    fn _poll_with_timeout(self, ms) {
        let elapsed = 0
        while elapsed < ms {
            let c = ffi_call(self.getchar_sym)
            if c != -1 { return c }
            sleep(2)
            elapsed = elapsed + 2
        }
        return -1
    }

    fn _win_special(self, c2) {
        if c2 == 72 { return KEY_UP    }
        if c2 == 80 { return KEY_DOWN  }
        if c2 == 75 { return KEY_LEFT  }
        if c2 == 77 { return KEY_RIGHT }
        if c2 == 71 { return KEY_HOME  }
        if c2 == 79 { return KEY_END   }
        if c2 == 73 { return KEY_PGUP  }
        if c2 == 81 { return KEY_PGDN  }
        if c2 == 83 { return KEY_DEL   }
        return -2999
    }

    fn _posix_special(self, c2) {
        if c2 == 65 { return KEY_UP    }
        if c2 == 66 { return KEY_DOWN  }
        if c2 == 67 { return KEY_RIGHT }
        if c2 == 68 { return KEY_LEFT  }
        if c2 == 72 { return KEY_HOME  }
        if c2 == 70 { return KEY_END   }
        if c2 == 53 { self._poll_with_timeout(10); return KEY_PGUP }
        if c2 == 54 { self._poll_with_timeout(10); return KEY_PGDN }
        if c2 == 51 { self._poll_with_timeout(10); return KEY_DEL  }
        return -2999
    }

    fn shutdown(self) {
        if self.os != "windows" {
            shell("stty icanon echo ixon ixoff")
        }
        if self.lib != 0 { ffi_close(self.lib) }
    }
}


// =============================================================================
// TermSize (mismo que vnano.vsh)
// =============================================================================

class TermSize {
    fn __init__(self) {
        self.cols = 80
        self.rows = 24
        self._detect()
    }

    fn _detect(self) {
        let os = platform()
        if os == "windows" {
            let out = shell("mode con")
            let nums = self._all_numbers(out)
            if len(nums) >= 2 {
                let r = nums[0]; let c = nums[1]
                if r >= 5 and r <= 500 { self.rows = r }
                if c >= 20 and c <= 500 { self.cols = c }
            }
        } else {
            let out = trim(shell("stty size"))
            let parts = split(out, " ")
            if len(parts) >= 2 {
                if is_numeric(parts[0]) {
                    let r = int(parts[0])
                    if r >= 5 and r <= 500 { self.rows = r }
                }
                if is_numeric(parts[1]) {
                    let c = int(parts[1])
                    if c >= 20 and c <= 500 { self.cols = c }
                }
            }
        }
        if self.cols < 20 { self.cols = 80 }
        if self.rows < 5  { self.rows = 24 }
    }

    fn _all_numbers(self, s) {
        let nums = []
        let i = 0
        let n = len(s)
        while i < n {
            let cc = char_code(substr(s, i, 1))
            if cc >= 48 and cc <= 57 {
                let start = i
                while i < n {
                    let c2 = char_code(substr(s, i, 1))
                    if c2 < 48 or c2 > 57 { break }
                    i = i + 1
                }
                append(nums, int(substr(s, start, i - start)))
            } else {
                i = i + 1
            }
        }
        return nums
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

fn dirname_of(path) {
    if path == "" { return "" }
    let i = len(path) - 1
    // saltar trailing slash
    while i >= 0 {
        let c = substr(path, i, 1)
        if c != "/" and c != "\\" { break }
        i = i - 1
    }
    while i >= 0 {
        let c = substr(path, i, 1)
        if c == "/" or c == "\\" {
            if i == 0 { return substr(path, 0, 1) }
            return substr(path, 0, i)
        }
        i = i - 1
    }
    return ""
}

fn join_path(dir, name) {
    if len(dir) == 0 { return name }
    let last = substr(dir, len(dir) - 1, 1)
    if last == "/" or last == "\\" { return dir + name }
    return dir + "/" + name
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

// Calcula el ancho visible en columnas, descontando bytes UTF-8 multibyte.
fn visual_len(s) {
    let n = len(s)
    let i = 0
    let visible = 0
    while i < n {
        let cc = char_code(substr(s, i, 1))
        if cc < 128 {
            visible = visible + 1; i = i + 1
        } elif cc >= 192 and cc < 224 {
            visible = visible + 1; i = i + 2
        } elif cc >= 224 and cc < 240 {
            visible = visible + 1; i = i + 3
        } elif cc >= 240 {
            visible = visible + 1; i = i + 4
        } else {
            i = i + 1
        }
    }
    return visible
}

// Trunca o rellena s para que ocupe exactamente target columnas visuales.
fn pad_visual(s, target) {
    let v = visual_len(s)
    if v == target { return s }
    if v < target { return s + repeat(" ", target - v) }
    // Truncar: cortamos por bytes hasta que el ancho visual sea target-1, +">"
    let i = 0
    let visible = 0
    while i < len(s) and visible < target - 1 {
        let cc = char_code(substr(s, i, 1))
        let step = 1
        if cc < 128 {
            step = 1
        } elif cc >= 192 and cc < 224 {
            step = 2
        } elif cc >= 224 and cc < 240 {
            step = 3
        } elif cc >= 240 {
            step = 4
        }
        i = i + step
        visible = visible + 1
    }
    return substr(s, 0, i) + ">"
}


// =============================================================================
// Launcher: TUI para elegir fichero / proyecto / nuevo
// =============================================================================
// Lista las entradas del directorio actual con la misma logica que FileTree
// (carpetas primero, alfabetico, filtros). Permite navegar dentro/fuera con
// Left/Right como un file manager basico.

class Launcher {
    "Selector inicial: fichero, proyecto, o nuevo."

    fn __init__(self, start_path, ignore_patterns, show_hidden, use_unicode) {
        self.cwd = start_path
        self.ignore_patterns = ignore_patterns
        self.show_hidden = show_hidden
        self.use_unicode = use_unicode

        self.entries = []           // entradas visibles del cwd
        self.filtered = []          // tras aplicar filtro de busqueda
        self.sel = 0                // indice seleccionado en filtered
        self.scroll = 0             // primera fila visible
        self.search_query = ""      // filtro tras pulsar /
        self.search_active = false

        let ts = TermSize()
        self.term_w = ts.cols
        self.term_h = ts.rows

        self._reload()
    }

    fn _reload(self) {
        self.entries = []
        let raw = []
        try {
            raw = listdir(self.cwd)
        } catch e {
            raw = []
        }

        let dirs = []
        let files = []
        for name in raw {
            if self._is_ignored(name) { continue }
            if not self.show_hidden and starts_with(name, ".") { continue }
            let full = join_path(self.cwd, name)
            let isd = false
            try { isd = is_dir_safe(full) } catch e { isd = false }
            let entry = { "name": name, "path": full, "is_dir": isd }
            if isd {
                append(dirs, entry)
            } else {
                append(files, entry)
            }
        }
        dirs = self._sort_by_name(dirs)
        files = self._sort_by_name(files)

        // Anadir ".." al principio si no estamos en raiz
        let parent = dirname_of(self.cwd)
        if parent != "" and parent != self.cwd {
            append(self.entries, { "name": "..", "path": parent, "is_dir": true })
        }
        for d in dirs { append(self.entries, d) }
        for f in files { append(self.entries, f) }

        self._apply_filter()
    }

    fn _apply_filter(self) {
        if self.search_query == "" {
            self.filtered = self.entries
        } else {
            let q = lower(self.search_query)
            self.filtered = []
            for e in self.entries {
                if contains(lower(e["name"]), q) {
                    append(self.filtered, e)
                }
            }
        }
        if self.sel >= len(self.filtered) { self.sel = len(self.filtered) - 1 }
        if self.sel < 0 { self.sel = 0 }
    }

    fn _is_ignored(self, name) {
        for pat in self.ignore_patterns {
            if name == pat { return true }
        }
        return false
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

    // ---- Render ----

    fn render(self) {
        print(ANSI["CLEAR"])
        print(ESC_HOME)
        print(ESC_CUR_HIDE)

        // Header
        print(ANSI["REVERSE"] + ANSI["BOLD"])
        let title = " vnano - Selector "
        let header_total = pad_visual(title, self.term_w)
        print(header_total)
        print(ANSI["RESET"])

        // Path actual
        print("\n")
        print(ansi_rgb(98, 114, 164))
        print("Carpeta: " + self.cwd)
        print(ANSI["RESET"])
        print("\n\n")

        // Lista
        let list_height = self.term_h - 8   // header(1) + cwd(2) + buscador(1) + ayuda(2) + status(1) = ~7
        if list_height < 5 { list_height = 5 }

        // Ajustar scroll para que sel sea visible
        if self.sel < self.scroll { self.scroll = self.sel }
        if self.sel >= self.scroll + list_height {
            self.scroll = self.sel - list_height + 1
        }
        if self.scroll < 0 { self.scroll = 0 }

        let i = 0
        while i < list_height {
            let idx = self.scroll + i
            if idx < len(self.filtered) {
                let e = self.filtered[idx]
                self._draw_entry(e, idx == self.sel)
            }
            print("\n")
            i = i + 1
        }

        // Buscador
        if self.search_active or self.search_query != "" {
            print(ansi_rgb(241, 250, 140))
            print("Buscar: " + self.search_query + "_")
            print(ANSI["RESET"])
            print("\n")
        } else {
            print("\n")
        }

        // Ayuda
        print(ansi_rgb(98, 114, 164))
        print("Up/Dn navega  Enter abre/entra  Right entra carpeta  Left subir  p Proyecto  n Nuevo  / buscar  . ocultos  u Unicode  q salir")
        print(ANSI["RESET"])
        print("\n")
    }

    fn _draw_entry(self, e, selected) {
        let icon = "  "
        if e["is_dir"] {
            if self.use_unicode {
                icon = TRI_RIGHT + " "
            } else {
                icon = "> "
            }
        }

        let name = e["name"]
        if e["is_dir"] and name != ".." { name = name + "/" }

        let line = "  " + icon + name
        let target = self.term_w - 2
        line = pad_visual(line, target)

        let color = ansi_rgb(80, 250, 123)
        if e["is_dir"] { color = ansi_rgb(80, 150, 255) }

        if selected {
            print(ansi_rgb_bg(68, 71, 90) + ANSI["BOLD"] + color + line + ANSI["RESET"])
        } else {
            print(color + line + ANSI["RESET"])
        }
    }

    // ---- Loop principal ----

    fn run(self, inp) {
        self.render()
        while true {
            let k = inp.read_key_blocking()

            // Si la busqueda esta activa, los caracteres normales van al filtro
            if self.search_active {
                if k == KEY_ESC {
                    self.search_active = false
                    self.search_query = ""
                    self._apply_filter()
                    self.render()
                    continue
                }
                if k == KEY_ENTER {
                    self.search_active = false
                    self.render()
                    continue
                }
                if k == KEY_BACKSPACE or k == KEY_DELETE {
                    if len(self.search_query) > 0 {
                        self.search_query = substr(self.search_query, 0, len(self.search_query) - 1)
                        self._apply_filter()
                    }
                    self.render()
                    continue
                }
                if k >= 32 and k < 127 {
                    self.search_query = self.search_query + from_char(k)
                    self._apply_filter()
                    self.render()
                    continue
                }
                // Las flechas siguen navegando aunque la busqueda este activa
            }

            if k == KEY_ESC or k == 113 {
                // q o Esc: cancelar
                print(ANSI["CLEAR"] + ESC_HOME + ESC_CUR_SHOW)
                return { "mode": "cancel" }
            }

            if k == KEY_UP {
                if self.sel > 0 { self.sel = self.sel - 1 }
                self.render()
                continue
            }
            if k == KEY_DOWN {
                if self.sel < len(self.filtered) - 1 { self.sel = self.sel + 1 }
                self.render()
                continue
            }
            if k == KEY_PGUP {
                self.sel = self.sel - 10
                if self.sel < 0 { self.sel = 0 }
                self.render()
                continue
            }
            if k == KEY_PGDN {
                self.sel = self.sel + 10
                if self.sel >= len(self.filtered) { self.sel = len(self.filtered) - 1 }
                self.render()
                continue
            }
            if k == KEY_HOME {
                self.sel = 0
                self.render()
                continue
            }
            if k == KEY_END {
                self.sel = len(self.filtered) - 1
                self.render()
                continue
            }

            if k == KEY_LEFT {
                // Subir a carpeta padre
                let parent = dirname_of(self.cwd)
                if parent != "" and parent != self.cwd {
                    self.cwd = parent
                    self.sel = 0
                    self.scroll = 0
                    self.search_query = ""
                    self._reload()
                    self.render()
                }
                continue
            }
            if k == KEY_RIGHT {
                // Entrar en carpeta seleccionada (sin abrir como proyecto)
                if len(self.filtered) > 0 {
                    let e = self.filtered[self.sel]
                    if e["is_dir"] {
                        self.cwd = e["path"]
                        self.sel = 0
                        self.scroll = 0
                        self.search_query = ""
                        self._reload()
                        self.render()
                    }
                }
                continue
            }
            if k == KEY_ENTER {
                if len(self.filtered) == 0 { continue }
                let e = self.filtered[self.sel]
                if e["is_dir"] {
                    // Entrar en la carpeta para navegar
                    self.cwd = e["path"]
                    self.sel = 0
                    self.scroll = 0
                    self.search_query = ""
                    self._reload()
                    self.render()
                } else {
                    // Abrir fichero
                    print(ANSI["CLEAR"] + ESC_HOME + ESC_CUR_SHOW)
                    return { "mode": "file", "path": e["path"] }
                }
                continue
            }

            // p: abrir cwd como proyecto
            if k == 112 {
                print(ANSI["CLEAR"] + ESC_HOME + ESC_CUR_SHOW)
                return { "mode": "project", "path": self.cwd }
            }

            // n: nuevo fichero
            if k == 110 {
                let name = self._prompt_name(inp, "Nombre del nuevo fichero: ")
                if name != null and name != "" {
                    let full = join_path(self.cwd, name)
                    print(ANSI["CLEAR"] + ESC_HOME + ESC_CUR_SHOW)
                    return { "mode": "new", "path": full }
                }
                self.render()
                continue
            }

            // .: toggle ocultos
            if k == 46 {
                self.show_hidden = not self.show_hidden
                self._reload()
                self.render()
                continue
            }

            // u: toggle Unicode
            if k == 117 {
                self.use_unicode = not self.use_unicode
                self.render()
                continue
            }

            // /: activar busqueda
            if k == 47 {
                self.search_active = true
                self.render()
                continue
            }
        }
    }

    // Prompt de nombre en una linea inferior. Devuelve string o null si Esc.
    fn _prompt_name(self, inp, label) {
        let ans = ""
        while true {
            // Pintar prompt en la penultima linea
            print(ansi_cursor_pos(self.term_h - 1, 1))
            print(ESC_CLR_EOL)
            print(ANSI["REVERSE"])
            let line = " " + label + ans + " "
            line = pad_visual(line, self.term_w)
            print(line)
            print(ANSI["RESET"])
            print(ansi_cursor_pos(self.term_h - 1, 2 + len(label) + len(ans)))
            print(ESC_CUR_SHOW)

            let k = inp.read_key_blocking()
            if k == KEY_ESC { return null }
            if k == KEY_ENTER { return trim(ans) }
            if k == KEY_BACKSPACE or k == KEY_DELETE {
                if len(ans) > 0 { ans = substr(ans, 0, len(ans) - 1) }
                continue
            }
            if k >= 32 and k < 127 {
                ans = ans + from_char(k)
            }
        }
        return null
    }
}


// =============================================================================
// MAIN de prueba
// =============================================================================

fn main() {
    let inp = InputBackend()
    let cwd = getcwd()
    let ignore = [".git", "node_modules", "build", "target",
                  "__pycache__", ".vscode", ".idea", "dist"]

    try {
        let launcher = Launcher(cwd, ignore, false, true)
        let result = launcher.run(inp)
        inp.shutdown()
        print(ANSI["CLEAR"] + ESC_HOME + ESC_CUR_SHOW + ANSI["RESET"])
        println("Resultado del launcher:")
        println("  mode: " + result["mode"])
        if contains(result, "path") {
            println("  path: " + result["path"])
        }
    } catch e {
        inp.shutdown()
        print(ANSI["CLEAR"] + ESC_HOME + ESC_CUR_SHOW + ANSI["RESET"])
        println("Error: " + str(e))
    }
}

main()
