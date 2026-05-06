// =============================================================================
// VestaShell - vnano: Editor de codigo con sidebar, pestanas y multicursor
// =============================================================================
// VERSION 4 - Cambios sobre v3:
//   - Launcher TUI inicial para elegir fichero / proyecto / nuevo
//   - Sidebar lateral con explorador de ficheros (lazy, expansible)
//   - Pestanas multiples (Ctrl+N nueva, Ctrl+W cerrar, Ctrl+PgUp/PgDn cambiar)
//   - Foco editor / sidebar con F6 o Ctrl+0/Ctrl+1
//   - Configuracion persistente en .vnano.json (sidebar width, ignorados,
//     unicode/ascii, ocultos)
//   - Ayuda flotante con Ctrl+H
//   - Refactor: cada pestana tiene su propio EditorBuffer (cursores,
//     scroll, busqueda independientes)
// =============================================================================
//
// ATAJOS COMPLETOS
// ----------------
// Globales:
//   Ctrl+H            mostrar ayuda
//   Ctrl+B            toggle sidebar
//   Ctrl+0            foco al sidebar
//   Ctrl+1            foco al editor
//   F6                rotar foco editor <-> sidebar
//   Ctrl+N            nueva pestana
//   Ctrl+W            cerrar pestana
//   Ctrl+PgUp/PgDn    pestana anterior / siguiente
//   Ctrl+Q            salir
//
// Editor (con foco en editor):
//   Movimiento:       flechas, Ctrl+L/R (palabra), Home/End, PgUp/PgDn
//   Seleccion:        Shift + flechas / Home / End
//   Multicursor:      Ctrl+Up / Ctrl+Down anaden, Esc colapsa
//   Edicion:          Tab, Enter (auto-indent), Backspace, Del
//   Cortar/pegar:     Ctrl+K (cortar linea), Ctrl+C, Ctrl+V, Ctrl+U
//   Acciones:         Ctrl+S guardar, Ctrl+E ejecutar, Ctrl+F buscar,
//                     F3 siguiente, Ctrl+G ir a linea, Ctrl+Z/Y undo/redo,
//                     Ctrl+T comando shell, Ctrl+Space autocompletar
//
// Sidebar (con foco en sidebar):
//   Up/Down           navegar
//   Enter o Right     expandir carpeta / abrir fichero en pestana nueva
//   Left              colapsar carpeta
//   .                 toggle ficheros ocultos
//   u                 toggle Unicode/ASCII
// =============================================================================


// =============================================================================
// SECCION 0: Constantes ANSI y caracteres Unicode
// =============================================================================

let ESC          = substr(ANSI["CLEAR_LINE"], 0, 1)
let ESC_HOME     = ESC + "[H"
let ESC_CUR_HIDE = ESC + "[?25l"
let ESC_CUR_SHOW = ESC + "[?25h"
let ESC_CLR_EOL  = ESC + "[K"
let ESC_CLR_EOS  = ESC + "[J"

// Caracteres Unicode (UTF-8 byte a byte porque VSH no soporta \xNN)
//   ▾ U+25BE = bytes 226 150 190
//   ▸ U+25B8 = bytes 226 150 184
//   │ U+2502 = bytes 226 148 130 (separador vertical para sidebar)
let TRI_DOWN  = from_char(226) + from_char(150) + from_char(190)
let TRI_RIGHT = from_char(226) + from_char(150) + from_char(184)
let VBAR      = from_char(226) + from_char(148) + from_char(130)


// =============================================================================
// SECCION 1: Codigos de tecla
// =============================================================================

// Ctrl+letra (ASCII 1..26)
let KEY_CTRL_A    = 1
let KEY_CTRL_B    = 2
let KEY_CTRL_C    = 3
let KEY_CTRL_D    = 4
let KEY_CTRL_E    = 5
let KEY_CTRL_F    = 6
let KEY_CTRL_G    = 7
let KEY_BACKSPACE = 8
let KEY_TAB       = 9
let KEY_ENTER     = 13
let KEY_CTRL_J    = 10
let KEY_CTRL_K    = 11
let KEY_CTRL_L    = 12
let KEY_CTRL_N    = 14
let KEY_CTRL_O    = 15
let KEY_CTRL_Q    = 17
let KEY_CTRL_R    = 18
let KEY_CTRL_S    = 19
let KEY_CTRL_T    = 20
let KEY_CTRL_U    = 21
let KEY_CTRL_V    = 22
let KEY_CTRL_W    = 23
let KEY_CTRL_X    = 24
let KEY_CTRL_Y    = 25
let KEY_CTRL_Z    = 26
let KEY_CTRL_H    = 8       // Ctrl+H = ASCII 8 = mismo que Backspace en muchas terminales
                            // Tratamos KEY_BACKSPACE en el editor; Ctrl+H se invoca con
                            // un hack o con F1 si conflicta. Ver KEY_F1.
let KEY_ESC       = 27
let KEY_DELETE    = 127

// Codigos sinteticos negativos
let KEY_UP          = -1000
let KEY_DOWN        = -1001
let KEY_LEFT        = -1002
let KEY_RIGHT       = -1003
let KEY_HOME        = -1004
let KEY_END         = -1005
let KEY_PGUP        = -1006
let KEY_PGDN        = -1007
let KEY_DEL         = -1008
let KEY_F1          = -1010
let KEY_F2          = -1011
let KEY_F3          = -1012
let KEY_F4          = -1013
let KEY_F5          = -1014
let KEY_F6          = -1015
let KEY_F7          = -1016
let KEY_F8          = -1017
let KEY_F9          = -1018
let KEY_F10         = -1019

let KEY_SHIFT_UP    = -2000
let KEY_SHIFT_DOWN  = -2001
let KEY_SHIFT_LEFT  = -2002
let KEY_SHIFT_RIGHT = -2003
let KEY_SHIFT_HOME  = -2004
let KEY_SHIFT_END   = -2005

let KEY_CTRL_LEFT   = -3000
let KEY_CTRL_RIGHT  = -3001
let KEY_CTRL_UP     = -3002
let KEY_CTRL_DOWN   = -3003
let KEY_CTRL_PGUP   = -3004
let KEY_CTRL_PGDN   = -3005
let KEY_CTRL_HOME   = -3006
let KEY_CTRL_END    = -3007

let KEY_CTRL_SPACE  = 0       // Ctrl+Space = NUL en muchas terminales
let KEY_CTRL_0      = -4000   // Sintetico (cmd no envia esto directamente; lo usamos como F-tecla)
let KEY_CTRL_1      = -4001


// =============================================================================
// SECCION 2: InputBackend
// =============================================================================

class InputBackend {
    "Backend de entrada via FFI: msvcrt en Windows, libc en POSIX."

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
        // Garantizar que cualquier output pendiente en el buffer del
        // interprete se vea ANTES de bloquearnos esperando tecla. Sin
        // esto, el contenido recien pintado se queda invisible hasta
        // el siguiente flush por umbral.
        flush_output()
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
        if c2 == 59 { return KEY_F1    }
        if c2 == 60 { return KEY_F2    }
        if c2 == 61 { return KEY_F3    }
        if c2 == 62 { return KEY_F4    }
        if c2 == 63 { return KEY_F5    }
        if c2 == 64 { return KEY_F6    }
        if c2 == 65 { return KEY_F7    }
        if c2 == 66 { return KEY_F8    }
        if c2 == 67 { return KEY_F9    }
        if c2 == 68 { return KEY_F10   }
        // Ctrl+flechas en Windows
        if c2 == 116 { return KEY_CTRL_RIGHT }
        if c2 == 115 { return KEY_CTRL_LEFT  }
        if c2 == 141 { return KEY_CTRL_UP    }
        if c2 == 145 { return KEY_CTRL_DOWN  }
        if c2 == 134 { return KEY_CTRL_PGUP  }
        if c2 == 118 { return KEY_CTRL_PGDN  }
        if c2 == 119 { return KEY_CTRL_HOME  }
        if c2 == 117 { return KEY_CTRL_END   }
        // Shift+flechas
        if c2 == 152 { return KEY_SHIFT_UP    }
        if c2 == 160 { return KEY_SHIFT_DOWN  }
        if c2 == 155 { return KEY_SHIFT_LEFT  }
        if c2 == 157 { return KEY_SHIFT_RIGHT }
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
        if c2 == 49 {
            let semi = self._poll_with_timeout(10)
            if semi == 59 {
                let mod = self._poll_with_timeout(10)
                let dir = self._poll_with_timeout(10)
                if mod == 50 {
                    if dir == 65 { return KEY_SHIFT_UP    }
                    if dir == 66 { return KEY_SHIFT_DOWN  }
                    if dir == 67 { return KEY_SHIFT_RIGHT }
                    if dir == 68 { return KEY_SHIFT_LEFT  }
                }
                if mod == 53 {
                    if dir == 65 { return KEY_CTRL_UP    }
                    if dir == 66 { return KEY_CTRL_DOWN  }
                    if dir == 67 { return KEY_CTRL_RIGHT }
                    if dir == 68 { return KEY_CTRL_LEFT  }
                }
                if dir == 55 { return KEY_F6 }
            }
            return KEY_HOME
        }
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
// SECCION 3: TermSize
// =============================================================================

class TermSize {
    "Detecta el tamano del terminal de forma robusta y multi-idioma."

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
// SECCION 4: Config
// =============================================================================

class Config {
    "Configuracion persistente en .vnano.json del cwd."

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
            // Config invalida: ignoramos
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
            let cc = char_code(substr(txt, i, 1))
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
// SECCION 5: Helpers globales
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

// Ancho visible en columnas, tratando bytes UTF-8 multibyte como 1 columna.
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

// Trunca o rellena para que ocupe exactamente 'target' columnas visuales.
fn pad_visual(s, target) {
    let v = visual_len(s)
    if v == target { return s }
    if v < target { return s + repeat(" ", target - v) }
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
// SECCION 6: Highlighter (sintaxis VSH)
// =============================================================================

let VSH_KEYWORDS = ["let", "fn", "class", "if", "elif", "else", "while", "for",
                    "in", "return", "try", "catch", "throw", "continue", "break",
                    "import", "as", "true", "false", "null", "self", "super",
                    "not", "and", "or", "is"]

let VSH_TYPES = ["int", "float", "string", "bool", "list", "map", "function",
                 "any", "void"]

let VSH_BUILTINS = ["print", "println", "echo", "len", "range", "append",
                    "read_file", "write_file", "ffi_open", "ffi_close", "ffi_sym",
                    "ffi_call", "ffi_call_f", "ffi_str", "time_ms", "sleep",
                    "shell", "shell_ex", "platform", "str", "int", "float",
                    "bool", "substr", "split", "join", "trim", "upper", "lower",
                    "exists", "is_dir", "is_file", "basename", "dirname",
                    "starts_with", "ends_with", "replace", "find_str",
                    "count_str", "repeat", "pad_left", "pad_right", "contains",
                    "char_code", "from_char", "is_numeric", "hex", "bin_str",
                    "ansi_cursor_pos", "ansi_clear", "ansi_clear_line",
                    "ansi_cursor_up", "ansi_cursor_down", "ansi_cursor_left",
                    "ansi_cursor_right", "strip_ansi", "colorize", "ansi_rgb",
                    "ansi_rgb_bg", "ansi_code", "ansi_enable",
                    "lstrip", "rstrip", "any_of", "all_of", "unique", "index_of",
                    "clamp", "input", "exit", "listdir", "getcwd", "chdir",
                    "file_size", "isinstance", "classname", "help", "doc"]


class Highlighter {
    "Tokenizador y coloreador de sintaxis para VestaShell."

    fn __init__(self) {
        // Paleta Dracula 2.0 con true color
        self.col_kw         = ansi_rgb(255, 121, 198)
        self.col_type       = ansi_rgb(139, 233, 253)
        self.col_builtin    = ansi_rgb(80, 250, 123)
        self.col_func       = ansi_rgb(80, 250, 123)
        self.col_str        = ansi_rgb(241, 250, 140)
        self.col_docstr     = ansi_rgb(241, 250, 140) + ANSI["ITALIC"]
        self.col_cmt        = ansi_rgb(98, 114, 164) + ANSI["ITALIC"]
        self.col_num        = ansi_rgb(189, 147, 249)
        self.col_op         = ansi_rgb(255, 121, 198)
        self.col_id         = ansi_rgb(248, 248, 242)
        self.col_match_word = ANSI["REVERSE"]
        self.col_search     = ANSI["BG_YELLOW"] + ANSI["BLACK"]
        self.col_overflow   = ansi_rgb_bg(80, 30, 30) + ansi_rgb(255, 100, 100)
        self.col_ruler      = ansi_rgb(68, 71, 90)
        self.col_cursor_sec = ansi_rgb_bg(98, 114, 164) + ansi_rgb(248, 248, 242)
        self.col_reset      = ANSI["RESET"]

        // OPTIMIZACION: precomputar maps para lookup O(1) en vez de O(n)
        // Asi tokenize() consulta keyword/type/builtin en tiempo constante.
        self.kw_map = {}
        for w in VSH_KEYWORDS { self.kw_map[w] = true }
        self.type_map = {}
        for w in VSH_TYPES { self.type_map[w] = true }
        self.builtin_map = {}
        for w in VSH_BUILTINS { self.builtin_map[w] = true }
    }

    fn _is_alpha(self, c) {
        let b = char_code(c)
        if b >= 65 and b <= 90  { return true }
        if b >= 97 and b <= 122 { return true }
        if c == "_" { return true }
        return false
    }

    fn _is_digit(self, c) {
        let b = char_code(c)
        return b >= 48 and b <= 57
    }

    fn _is_alnum(self, c) {
        return self._is_alpha(c) or self._is_digit(c)
    }

    fn _in_list(self, word, lst) {
        for w in lst { if w == word { return true } }
        return false
    }

    fn tokenize(self, line, state) {
        let tokens = []
        let i = 0
        let n = len(line)
        let in_cmt = state["in_cmt"]
        let in_docstr = state["in_docstr"]

        while i < n {
            if in_docstr {
                let j = i
                let found = false
                while j <= n - 3 {
                    if substr(line, j, 3) == "\"\"\"" {
                        append(tokens, { "kind": "docstr", "text": substr(line, i, j+3-i), "start": i })
                        i = j + 3
                        in_docstr = false
                        found = true
                        break
                    }
                    j = j + 1
                }
                if not found {
                    append(tokens, { "kind": "docstr", "text": substr(line, i, n-i), "start": i })
                    i = n
                }
                continue
            }

            if in_cmt {
                let j = i
                let found = false
                while j < n - 1 {
                    if substr(line, j, 1) == "*" and substr(line, j+1, 1) == "/" {
                        append(tokens, { "kind": "cmt", "text": substr(line, i, j+2-i), "start": i })
                        i = j + 2
                        in_cmt = false
                        found = true
                        break
                    }
                    j = j + 1
                }
                if not found {
                    append(tokens, { "kind": "cmt", "text": substr(line, i, n-i), "start": i })
                    i = n
                }
                continue
            }

            let c = substr(line, i, 1)

            if c == "\"" and i+2 < n and substr(line, i, 3) == "\"\"\"" {
                let start = i
                let j = i + 3
                let closed = false
                while j <= n - 3 {
                    if substr(line, j, 3) == "\"\"\"" {
                        append(tokens, { "kind": "docstr", "text": substr(line, start, j+3-start), "start": start })
                        i = j + 3
                        closed = true
                        break
                    }
                    j = j + 1
                }
                if not closed {
                    append(tokens, { "kind": "docstr", "text": substr(line, start, n-start), "start": start })
                    i = n
                    in_docstr = true
                }
                continue
            }

            if c == "/" and i+1 < n and substr(line, i+1, 1) == "/" {
                append(tokens, { "kind": "cmt", "text": substr(line, i, n-i), "start": i })
                i = n
                continue
            }

            if c == "/" and i+1 < n and substr(line, i+1, 1) == "*" {
                let start = i
                let j = i + 2
                let closed = false
                while j < n - 1 {
                    if substr(line, j, 1) == "*" and substr(line, j+1, 1) == "/" {
                        append(tokens, { "kind": "cmt", "text": substr(line, start, j+2-start), "start": start })
                        i = j + 2
                        closed = true
                        break
                    }
                    j = j + 1
                }
                if not closed {
                    append(tokens, { "kind": "cmt", "text": substr(line, start, n-start), "start": start })
                    i = n
                    in_cmt = true
                }
                continue
            }

            if c == "\"" {
                let start = i
                let j = i + 1
                while j < n {
                    if substr(line, j, 1) == "\\" and j+1 < n {
                        j = j + 2
                        continue
                    }
                    if substr(line, j, 1) == "\"" { j = j + 1; break }
                    j = j + 1
                }
                append(tokens, { "kind": "str", "text": substr(line, start, j-start), "start": start })
                i = j
                continue
            }

            if self._is_digit(c) {
                let start = i
                if c == "0" and i+1 < n and substr(line, i+1, 1) == "x" {
                    i = i + 2
                    while i < n {
                        let cc = substr(line, i, 1)
                        let b = char_code(cc)
                        let hex_ok = (b >= 48 and b <= 57) or (b >= 65 and b <= 70) or (b >= 97 and b <= 102)
                        if not hex_ok { break }
                        i = i + 1
                    }
                } else {
                    while i < n and (self._is_digit(substr(line, i, 1)) or substr(line, i, 1) == ".") {
                        i = i + 1
                    }
                }
                append(tokens, { "kind": "num", "text": substr(line, start, i-start), "start": start })
                continue
            }

            if self._is_alpha(c) {
                let start = i
                while i < n and self._is_alnum(substr(line, i, 1)) {
                    i = i + 1
                }
                let word = substr(line, start, i-start)
                let kind = "id"
                if contains(self.kw_map, word) {
                    kind = "kw"
                } elif contains(self.type_map, word) {
                    kind = "type"
                } elif contains(self.builtin_map, word) {
                    kind = "builtin"
                }
                if kind == "id" {
                    let k = i
                    while k < n and substr(line, k, 1) == " " { k = k + 1 }
                    if k < n and substr(line, k, 1) == "(" { kind = "func" }
                }
                append(tokens, { "kind": kind, "text": word, "start": start })
                continue
            }

            let is_op = false
            if c == "+" or c == "-" or c == "*" or c == "/" or c == "=" or c == "<" or c == ">" or c == "!" or c == "&" or c == "|" or c == "{" or c == "}" or c == "(" or c == ")" or c == "[" or c == "]" or c == "," or c == ";" or c == ":" or c == "." {
                is_op = true
            }
            if is_op {
                append(tokens, { "kind": "op", "text": c, "start": i })
                i = i + 1
                continue
            }

            append(tokens, { "kind": "ws", "text": c, "start": i })
            i = i + 1
        }
        return { "tokens": tokens, "state": { "in_cmt": in_cmt, "in_docstr": in_docstr } }
    }

    fn render(self, tokens, highlights) {
        let out = ""
        for tok in tokens {
            let color = self._color_for(tok["kind"])
            let txt = tok["text"]
            let start = tok["start"]
            let end = start + len(txt)
            let overlay = self._find_overlay(highlights, start, end)
            if overlay == null {
                if color == "" {
                    out = out + txt
                } else {
                    out = out + color + txt + self.col_reset
                }
            } else {
                out = out + overlay["color"] + txt + self.col_reset
            }
        }
        return out
    }

    fn _find_overlay(self, highlights, start, end) {
        if highlights == null { return null }
        for h in highlights {
            if start < h["end"] and end > h["start"] { return h }
        }
        return null
    }

    fn _color_for(self, kind) {
        if kind == "kw"      { return self.col_kw      }
        if kind == "type"    { return self.col_type    }
        if kind == "builtin" { return self.col_builtin }
        if kind == "func"    { return self.col_func    }
        if kind == "str"     { return self.col_str     }
        if kind == "docstr"  { return self.col_docstr  }
        if kind == "num"     { return self.col_num     }
        if kind == "cmt"     { return self.col_cmt     }
        if kind == "op"      { return self.col_op      }
        return self.col_id
    }

    fn find_word_matches(self, tokens, word) {
        let matches = []
        if word == "" { return matches }
        for tok in tokens {
            let k = tok["kind"]
            if k == "id" or k == "kw" or k == "builtin" or k == "type" or k == "func" {
                if tok["text"] == word {
                    let s = tok["start"]
                    append(matches, { "start": s, "end": s + len(word), "color": self.col_match_word })
                }
            }
        }
        return matches
    }

    fn find_search_matches(self, line, needle) {
        let matches = []
        if needle == "" { return matches }
        let pos = 0
        while true {
            let p = find_str(line, needle, pos)
            if p == -1 { break }
            append(matches, { "start": p, "end": p + len(needle), "color": self.col_search })
            pos = p + len(needle)
        }
        return matches
    }
}
// =============================================================================
// SECCION 7: TextBuffer (modelo del documento + undo/redo)
// =============================================================================

class TextBuffer {
    "Modelo de un documento. Maneja edicion y undo/redo."

    fn __init__(self) {
        self.lines = [""]
        self.line_dirty = [true]
        self.in_state_at = [{ "in_cmt": false, "in_docstr": false }]
        self.undo_stack = []
        self.undo_top = 0
        self.redo_stack = []
        self.redo_top = 0
        self.max_undo = 200
        self.modified = false
        self.last_action_ms = 0
        self.last_action_kind = ""
        self.coalesce_window_ms = 600
    }

    fn load_text(self, text) {
        self.lines = []
        let raw = split(text, "\n")
        for ln in raw {
            if len(ln) > 0 and substr(ln, len(ln)-1, 1) == "\r" {
                ln = substr(ln, 0, len(ln)-1)
            }
            append(self.lines, ln)
        }
        if len(self.lines) == 0 { append(self.lines, "") }
        self.line_dirty = []
        self.in_state_at = []
        let i = 0
        while i < len(self.lines) {
            append(self.line_dirty, true)
            append(self.in_state_at, { "in_cmt": false, "in_docstr": false })
            i = i + 1
        }
        self.modified = false
        self.undo_stack = []; self.undo_top = 0
        self.redo_stack = []; self.redo_top = 0
    }

    fn to_text(self) { return join(self.lines, "\n") }
    fn nlines(self) { return len(self.lines) }

    fn line(self, i) {
        if i < 0 or i >= len(self.lines) { return "" }
        return self.lines[i]
    }

    fn mark_dirty(self, i) {
        if i >= 0 and i < len(self.line_dirty) { self.line_dirty[i] = true }
    }

    fn mark_dirty_from(self, i) {
        let k = i
        while k < len(self.line_dirty) {
            self.line_dirty[k] = true
            k = k + 1
        }
    }

    fn _push_undo(self, action) {
        let now = time_ms()
        let op = action["op"]
        let can_coalesce = false
        if self.undo_top > 0 {
            let prev = self.undo_stack[self.undo_top - 1]
            if op == "delete_char" and prev["op"] == "delete_char" {
                if prev["row"] == action["row"] and prev["col"] + 1 == action["col"] {
                    if now - self.last_action_ms < self.coalesce_window_ms {
                        can_coalesce = true
                    }
                }
            }
            if op == "insert_char" and prev["op"] == "insert_char" {
                if prev["row"] == action["row"] and prev["col"] - 1 == action["col"] {
                    if now - self.last_action_ms < self.coalesce_window_ms {
                        can_coalesce = true
                    }
                }
            }
        }
        if can_coalesce {
            let prev = self.undo_stack[self.undo_top - 1]
            if op == "delete_char" {
                prev["ch"] = prev["ch"] + action["ch"]
            } elif op == "insert_char" {
                prev["ch"] = action["ch"] + prev["ch"]
                prev["col"] = action["col"]
            }
        } else {
            if self.undo_top < len(self.undo_stack) {
                self.undo_stack[self.undo_top] = action
            } else {
                append(self.undo_stack, action)
            }
            self.undo_top = self.undo_top + 1
            if self.undo_top > self.max_undo {
                let new_stack = []
                let start = self.undo_top - self.max_undo
                let i = start
                while i < self.undo_top {
                    append(new_stack, self.undo_stack[i])
                    i = i + 1
                }
                self.undo_stack = new_stack
                self.undo_top = len(new_stack)
            }
        }
        self.last_action_ms = now
        self.last_action_kind = op
        self.redo_top = 0
        self.modified = true
    }

    fn _pop_undo(self) {
        if self.undo_top == 0 { return null }
        self.undo_top = self.undo_top - 1
        return self.undo_stack[self.undo_top]
    }

    fn _push_redo(self, action) {
        if self.redo_top < len(self.redo_stack) {
            self.redo_stack[self.redo_top] = action
        } else {
            append(self.redo_stack, action)
        }
        self.redo_top = self.redo_top + 1
    }

    fn _pop_redo(self) {
        if self.redo_top == 0 { return null }
        self.redo_top = self.redo_top - 1
        return self.redo_stack[self.redo_top]
    }

    fn _affects_syntax(self, ch) {
        return ch == "/" or ch == "*" or ch == "\""
    }

    fn insert_char(self, row, col, ch) {
        let ln = self.lines[row]
        if col > len(ln) { col = len(ln) }
        self.lines[row] = substr(ln, 0, col) + ch + substr(ln, col, len(ln)-col)
        self._push_undo({ "op": "delete_char", "row": row, "col": col, "ch": ch })
        self.mark_dirty(row)
        if self._affects_syntax(ch) { self.mark_dirty_from(row) }
    }

    fn delete_char(self, row, col) {
        let ln = self.lines[row]
        if col < 0 or col >= len(ln) { return false }
        let ch = substr(ln, col, 1)
        self.lines[row] = substr(ln, 0, col) + substr(ln, col+1, len(ln)-col-1)
        self._push_undo({ "op": "insert_char", "row": row, "col": col, "ch": ch })
        self.mark_dirty(row)
        if self._affects_syntax(ch) { self.mark_dirty_from(row) }
        return true
    }

    fn split_line(self, row, col) {
        let ln = self.lines[row]
        if col > len(ln) { col = len(ln) }
        let left = substr(ln, 0, col)
        let right = substr(ln, col, len(ln)-col)
        self.lines[row] = left
        let new_lines = []; let new_dirty = []; let new_state = []
        let i = 0
        while i < len(self.lines) {
            append(new_lines, self.lines[i])
            append(new_dirty, self.line_dirty[i])
            append(new_state, self.in_state_at[i])
            if i == row {
                append(new_lines, right)
                append(new_dirty, true)
                append(new_state, { "in_cmt": false, "in_docstr": false })
            }
            i = i + 1
        }
        self.lines = new_lines
        self.line_dirty = new_dirty
        self.in_state_at = new_state
        self._push_undo({ "op": "join_line", "row": row, "tail": right })
        self.mark_dirty_from(row)
    }

    fn join_line(self, row) {
        if row+1 >= len(self.lines) { return false }
        let tail = self.lines[row+1]
        let col = len(self.lines[row])
        self.lines[row] = self.lines[row] + tail
        let new_lines = []; let new_dirty = []; let new_state = []
        let i = 0
        while i < len(self.lines) {
            if i != row+1 {
                append(new_lines, self.lines[i])
                append(new_dirty, self.line_dirty[i])
                append(new_state, self.in_state_at[i])
            }
            i = i + 1
        }
        self.lines = new_lines
        self.line_dirty = new_dirty
        self.in_state_at = new_state
        self._push_undo({ "op": "split_line", "row": row, "col": col })
        self.mark_dirty_from(row)
        return true
    }

    fn delete_line(self, row) {
        if row < 0 or row >= len(self.lines) { return "" }
        let text = self.lines[row]
        let new_lines = []; let new_dirty = []; let new_state = []
        let i = 0
        while i < len(self.lines) {
            if i != row {
                append(new_lines, self.lines[i])
                append(new_dirty, self.line_dirty[i])
                append(new_state, self.in_state_at[i])
            }
            i = i + 1
        }
        if len(new_lines) == 0 {
            append(new_lines, "")
            append(new_dirty, true)
            append(new_state, { "in_cmt": false, "in_docstr": false })
        }
        self.lines = new_lines
        self.line_dirty = new_dirty
        self.in_state_at = new_state
        self._push_undo({ "op": "insert_line", "row": row, "text": text })
        self.mark_dirty_from(row)
        return text
    }

    fn insert_line(self, row, text) {
        if row > len(self.lines) { row = len(self.lines) }
        let new_lines = []; let new_dirty = []; let new_state = []
        let i = 0
        while i <= len(self.lines) {
            if i == row {
                append(new_lines, text)
                append(new_dirty, true)
                append(new_state, { "in_cmt": false, "in_docstr": false })
            }
            if i < len(self.lines) {
                append(new_lines, self.lines[i])
                append(new_dirty, self.line_dirty[i])
                append(new_state, self.in_state_at[i])
            }
            i = i + 1
        }
        self.lines = new_lines
        self.line_dirty = new_dirty
        self.in_state_at = new_state
        self._push_undo({ "op": "delete_line", "row": row, "text": text })
        self.mark_dirty_from(row)
    }

    fn undo(self) {
        let action = self._pop_undo()
        if action == null { return null }
        let redo_action = self._apply_inverse(action)
        if redo_action != null { self._push_redo(redo_action) }
        return action
    }

    fn redo(self) {
        let action = self._pop_redo()
        if action == null { return null }
        let undo_action = self._apply_inverse(action)
        if undo_action != null {
            if self.undo_top < len(self.undo_stack) {
                self.undo_stack[self.undo_top] = undo_action
            } else {
                append(self.undo_stack, undo_action)
            }
            self.undo_top = self.undo_top + 1
            self.modified = true
        }
        return action
    }

    fn _apply_inverse(self, action) {
        let op = action["op"]
        if op == "insert_char" {
            let r = action["row"]; let c = action["col"]; let chs = action["ch"]
            let ln = self.lines[r]
            self.lines[r] = substr(ln, 0, c) + chs + substr(ln, c, len(ln)-c)
            self.mark_dirty(r)
            let i = 0
            while i < len(chs) {
                if self._affects_syntax(substr(chs, i, 1)) {
                    self.mark_dirty_from(r); break
                }
                i = i + 1
            }
            return { "op": "delete_char", "row": r, "col": c, "ch": chs }
        }
        if op == "delete_char" {
            let r = action["row"]; let c = action["col"]
            let ln = self.lines[r]
            let want = action["ch"]
            let n_to_delete = len(want)
            if c + n_to_delete > len(ln) { return null }
            let chs = substr(ln, c, n_to_delete)
            self.lines[r] = substr(ln, 0, c) + substr(ln, c+n_to_delete, len(ln)-c-n_to_delete)
            self.mark_dirty(r)
            let i = 0
            while i < len(chs) {
                if self._affects_syntax(substr(chs, i, 1)) {
                    self.mark_dirty_from(r); break
                }
                i = i + 1
            }
            return { "op": "insert_char", "row": r, "col": c, "ch": chs }
        }
        if op == "split_line" {
            let r = action["row"]; let c = action["col"]
            let ln = self.lines[r]
            let left = substr(ln, 0, c)
            let right = substr(ln, c, len(ln)-c)
            self.lines[r] = left
            let new_lines = []; let new_dirty = []; let new_state = []
            let i = 0
            while i < len(self.lines) {
                append(new_lines, self.lines[i])
                append(new_dirty, self.line_dirty[i])
                append(new_state, self.in_state_at[i])
                if i == r {
                    append(new_lines, right)
                    append(new_dirty, true)
                    append(new_state, { "in_cmt": false, "in_docstr": false })
                }
                i = i + 1
            }
            self.lines = new_lines; self.line_dirty = new_dirty; self.in_state_at = new_state
            self.mark_dirty_from(r)
            return { "op": "join_line", "row": r, "tail": right }
        }
        if op == "join_line" {
            let r = action["row"]
            if r+1 >= len(self.lines) { return null }
            let col = len(self.lines[r])
            self.lines[r] = self.lines[r] + self.lines[r+1]
            let new_lines = []; let new_dirty = []; let new_state = []
            let i = 0
            while i < len(self.lines) {
                if i != r+1 {
                    append(new_lines, self.lines[i])
                    append(new_dirty, self.line_dirty[i])
                    append(new_state, self.in_state_at[i])
                }
                i = i + 1
            }
            self.lines = new_lines; self.line_dirty = new_dirty; self.in_state_at = new_state
            self.mark_dirty_from(r)
            return { "op": "split_line", "row": r, "col": col }
        }
        if op == "insert_line" {
            let r = action["row"]; let t = action["text"]
            let new_lines = []; let new_dirty = []; let new_state = []
            let i = 0
            while i <= len(self.lines) {
                if i == r {
                    append(new_lines, t)
                    append(new_dirty, true)
                    append(new_state, { "in_cmt": false, "in_docstr": false })
                }
                if i < len(self.lines) {
                    append(new_lines, self.lines[i])
                    append(new_dirty, self.line_dirty[i])
                    append(new_state, self.in_state_at[i])
                }
                i = i + 1
            }
            self.lines = new_lines; self.line_dirty = new_dirty; self.in_state_at = new_state
            self.mark_dirty_from(r)
            return { "op": "delete_line", "row": r, "text": t }
        }
        if op == "delete_line" {
            let r = action["row"]
            if r >= len(self.lines) { return null }
            let t = self.lines[r]
            let new_lines = []; let new_dirty = []; let new_state = []
            let i = 0
            while i < len(self.lines) {
                if i != r {
                    append(new_lines, self.lines[i])
                    append(new_dirty, self.line_dirty[i])
                    append(new_state, self.in_state_at[i])
                }
                i = i + 1
            }
            if len(new_lines) == 0 {
                append(new_lines, "")
                append(new_dirty, true)
                append(new_state, { "in_cmt": false, "in_docstr": false })
            }
            self.lines = new_lines; self.line_dirty = new_dirty; self.in_state_at = new_state
            self.mark_dirty_from(r)
            return { "op": "insert_line", "row": r, "text": t }
        }
        return null
    }
}


// =============================================================================
// SECCION 8: EditorBuffer (estado por pestana)
// =============================================================================
// Encapsula todo lo "por fichero": el TextBuffer, los cursores, el scroll
// y la busqueda. El Editor (workspace) tiene una lista de estos.

class EditorBuffer {
    "Estado de una pestana: documento, cursores, scroll y busqueda."

    fn __init__(self, filename) {
        self.filename = filename
        self.buf = TextBuffer()
        self.cursors = [self._make_cursor(0, 0, true)]
        self.row_off = 0
        self.col_off = 0
        self.last_search = ""
        // Ultima palabra-bajo-cursor que se renderizo, para minimizar
        // las lineas que hay que redibujar cuando cur_word cambia
        self.last_cur_word = ""
        // Cargar fichero si existe
        if filename != "" and exists(filename) {
            try {
                let txt = read_file(filename)
                self.buf.load_text(txt)
            } catch e {
                // Fichero no legible: queda vacio
            }
        }
    }

    fn _make_cursor(self, row, col, primary) {
        return { "row": row, "col": col, "anchor_row": row, "anchor_col": col, "primary": primary }
    }

    fn primary(self) { return self.cursors[0] }
    fn has_multi(self) { return len(self.cursors) > 1 }

    fn display_name(self) {
        if self.filename == "" { return "[Sin nombre]" }
        return basename_or(self.filename, self.filename)
    }

    // Crear una "vista" nueva: comparte el mismo TextBuffer pero tiene
    // cursores y scroll independientes. Util para split donde dos paneles
    // editan el mismo fichero pero ven el cursor en sitios distintos.
    fn clone_view(self) {
        // Construye con "" (no lee fichero), luego sustituye campos
        let v = EditorBuffer("")
        v.filename = self.filename
        v.buf = self.buf            // referencia compartida al MISMO TextBuffer
        // cursors, row_off, col_off, last_search ya inicializados por __init__
        return v
    }
}


// =============================================================================
// SECCION 9: FileTree (arbol de ficheros lazy)
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
            "name": name, "path": path, "is_dir": is_dir,
            "expanded": false, "loaded": false, "children": [], "depth": depth
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

    fn refresh_root(self) {
        self.root["loaded"] = false
        self.root["children"] = []
        self.expand(self.root)
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
            let full = join_path(path, name)
            let isd = false
            try { isd = is_dir_safe(full) } catch e { isd = false }
            let node = self._make_node(full, name, isd, depth)
            if isd {
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

    fn render_line(self, node, width, selected, focused, is_active_file) {
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
        let raw = " " + indent + icon + name
        raw = pad_visual(raw, width)

        let color = ansi_rgb(80, 250, 123)
        if node["is_dir"] { color = ansi_rgb(80, 150, 255) }

        if selected and focused {
            return ansi_rgb_bg(98, 114, 164) + ANSI["BOLD"] + color + raw + ANSI["RESET"]
        }
        if selected {
            return ansi_rgb_bg(68, 71, 90) + color + raw + ANSI["RESET"]
        }
        if is_active_file {
            return ANSI["BOLD"] + color + raw + ANSI["RESET"]
        }
        return color + raw + ANSI["RESET"]
    }
}
// =============================================================================
// SECCION 10: Launcher (selector inicial)
// =============================================================================

class Launcher {
    "Selector inicial: fichero, proyecto o nuevo."

    fn __init__(self, start_path, config) {
        self.cwd = start_path
        self.config = config
        self.entries = []
        self.filtered = []
        self.sel = 0
        self.scroll = 0
        self.search_query = ""
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
            if not self.config.show_hidden and starts_with(name, ".") { continue }
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
        for pat in self.config.ignore_patterns {
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

    fn render(self) {
        print(ANSI["CLEAR"])
        print(ESC_HOME)
        print(ESC_CUR_HIDE)

        print(ANSI["REVERSE"] + ANSI["BOLD"])
        let title = " vnano - Selector "
        print(pad_visual(title, self.term_w))
        print(ANSI["RESET"])

        print("\n")
        print(ansi_rgb(98, 114, 164))
        print("Carpeta: " + self.cwd)
        print(ANSI["RESET"])
        print("\n\n")

        let list_height = self.term_h - 8
        if list_height < 5 { list_height = 5 }
        if self.sel < self.scroll { self.scroll = self.sel }
        if self.sel >= self.scroll + list_height {
            self.scroll = self.sel - list_height + 1
        }
        if self.scroll < 0 { self.scroll = 0 }

        let i = 0
        while i < list_height {
            let idx = self.scroll + i
            if idx < len(self.filtered) {
                self._draw_entry(self.filtered[idx], idx == self.sel)
            }
            print("\n")
            i = i + 1
        }

        if self.search_active or self.search_query != "" {
            print(ansi_rgb(241, 250, 140))
            print("Buscar: " + self.search_query + "_")
            print(ANSI["RESET"])
            print("\n")
        } else {
            print("\n")
        }

        print(ansi_rgb(98, 114, 164))
        print("Up/Dn  Enter abre  Right entra  Left subir  p Proyecto  n Nuevo  / buscar  . ocultos  u Unicode  q salir")
        print(ANSI["RESET"])
        print("\n")
    }

    fn _draw_entry(self, e, selected) {
        let icon = "  "
        if e["is_dir"] {
            if self.config.use_unicode {
                icon = TRI_RIGHT + " "
            } else {
                icon = "> "
            }
        }
        let name = e["name"]
        if e["is_dir"] and name != ".." { name = name + "/" }
        let line = "  " + icon + name
        line = pad_visual(line, self.term_w - 2)
        let color = ansi_rgb(80, 250, 123)
        if e["is_dir"] { color = ansi_rgb(80, 150, 255) }
        if selected {
            print(ansi_rgb_bg(68, 71, 90) + ANSI["BOLD"] + color + line + ANSI["RESET"])
        } else {
            print(color + line + ANSI["RESET"])
        }
    }

    fn run(self, inp) {
        self.render()
        while true {
            let k = inp.read_key_blocking()

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
            }

            if k == KEY_ESC or k == 113 {
                print(ANSI["CLEAR"] + ESC_HOME + ESC_CUR_SHOW)
                return { "mode": "cancel" }
            }
            if k == KEY_UP {
                if self.sel > 0 { self.sel = self.sel - 1 }
                self.render(); continue
            }
            if k == KEY_DOWN {
                if self.sel < len(self.filtered) - 1 { self.sel = self.sel + 1 }
                self.render(); continue
            }
            if k == KEY_PGUP {
                self.sel = self.sel - 10
                if self.sel < 0 { self.sel = 0 }
                self.render(); continue
            }
            if k == KEY_PGDN {
                self.sel = self.sel + 10
                if self.sel >= len(self.filtered) { self.sel = len(self.filtered) - 1 }
                self.render(); continue
            }
            if k == KEY_HOME { self.sel = 0; self.render(); continue }
            if k == KEY_END {
                self.sel = len(self.filtered) - 1
                self.render(); continue
            }
            if k == KEY_LEFT {
                let parent = dirname_of(self.cwd)
                if parent != "" and parent != self.cwd {
                    self.cwd = parent
                    self.sel = 0; self.scroll = 0; self.search_query = ""
                    self._reload()
                    self.render()
                }
                continue
            }
            if k == KEY_RIGHT {
                if len(self.filtered) > 0 {
                    let e = self.filtered[self.sel]
                    if e["is_dir"] {
                        self.cwd = e["path"]
                        self.sel = 0; self.scroll = 0; self.search_query = ""
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
                    self.cwd = e["path"]
                    self.sel = 0; self.scroll = 0; self.search_query = ""
                    self._reload()
                    self.render()
                } else {
                    print(ANSI["CLEAR"] + ESC_HOME + ESC_CUR_SHOW)
                    return { "mode": "file", "path": e["path"] }
                }
                continue
            }
            if k == 112 {
                print(ANSI["CLEAR"] + ESC_HOME + ESC_CUR_SHOW)
                return { "mode": "project", "path": self.cwd }
            }
            if k == 110 {
                let name = self._prompt_name(inp, "Nombre del nuevo fichero: ")
                if name != null and name != "" {
                    let full = join_path(self.cwd, name)
                    print(ANSI["CLEAR"] + ESC_HOME + ESC_CUR_SHOW)
                    return { "mode": "new", "path": full }
                }
                self.render(); continue
            }
            if k == 46 {
                self.config.show_hidden = not self.config.show_hidden
                self._reload()
                self.render(); continue
            }
            if k == 117 {
                self.config.use_unicode = not self.config.use_unicode
                self.render(); continue
            }
            if k == 47 {
                self.search_active = true
                self.render(); continue
            }
        }
        return { "mode": "cancel" }
    }

    fn _prompt_name(self, inp, label) {
        let ans = ""
        while true {
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
// SECCION 11: HelpOverlay
// =============================================================================
// Pantalla flotante con todos los atajos. Se invoca con F1 (asignamos F1
// como "ayuda" porque Ctrl+H choca con KEY_BACKSPACE en muchas terminales).

class HelpOverlay {
    "Pantalla de ayuda con todos los atajos."

    fn show(self, inp, term_w, term_h) {
        print(ANSI["CLEAR"])
        print(ESC_HOME)
        print(ESC_CUR_HIDE)

        let title_color = ansi_rgb(255, 121, 198) + ANSI["BOLD"]
        let sect_color  = ansi_rgb(139, 233, 253) + ANSI["BOLD"]
        let key_color   = ansi_rgb(80, 250, 123)
        let dim_color   = ansi_rgb(98, 114, 164)
        let R = ANSI["RESET"]

        print(title_color + " vnano - Atajos" + R + "\n\n")

        print(sect_color + "GLOBALES" + R + "\n")
        self._row(key_color, "F1", "Esta ayuda")
        self._row(key_color, "Ctrl+B", "Toggle sidebar")
        self._row(key_color, "Ctrl+0", "Foco al sidebar")
        self._row(key_color, "Ctrl+1", "Foco al editor")
        self._row(key_color, "F6", "Rotar foco editor <-> sidebar")
        self._row(key_color, "Ctrl+N", "Nueva pestana")
        self._row(key_color, "Ctrl+W", "Cerrar pestana")
        self._row(key_color, "Ctrl+PgUp/PgDn", "Pestana anterior / siguiente")
        self._row(key_color, "Ctrl+Q", "Salir del editor")
        print("\n")

        print(sect_color + "EDITOR - Movimiento" + R + "\n")
        self._row(key_color, "Flechas", "Mover cursor")
        self._row(key_color, "Ctrl+Left/Right", "Saltar palabra")
        self._row(key_color, "Home / End", "Inicio / fin de linea")
        self._row(key_color, "PgUp / PgDn", "Pagina arriba / abajo")
        print("\n")

        print(sect_color + "EDITOR - Seleccion" + R + "\n")
        self._row(key_color, "Shift+Flechas", "Extender seleccion")
        self._row(key_color, "Shift+Home/End", "Extender hasta inicio / fin")
        print("\n")

        print(sect_color + "EDITOR - Multicursor" + R + "\n")
        self._row(key_color, "Ctrl+Up", "Anadir cursor arriba")
        self._row(key_color, "Ctrl+Down", "Anadir cursor abajo")
        self._row(key_color, "Esc", "Colapsar a un solo cursor")
        print("\n")

        print(sect_color + "EDITOR - Edicion" + R + "\n")
        self._row(key_color, "Tab", "4 espacios (en todos los cursores)")
        self._row(key_color, "Enter", "Nueva linea con auto-indent")
        self._row(key_color, "Backspace / Del", "Borrar")
        self._row(key_color, "Ctrl+K", "Cortar linea")
        self._row(key_color, "Ctrl+C / Ctrl+V", "Copiar / Pegar")
        self._row(key_color, "Ctrl+U", "Pegar (alias)")
        self._row(key_color, "Ctrl+Z / Ctrl+Y", "Deshacer / Rehacer")
        print("\n")

        print(sect_color + "EDITOR - Acciones" + R + "\n")
        self._row(key_color, "Ctrl+S", "Guardar")
        self._row(key_color, "Ctrl+E", "Ejecutar script con vm")
        self._row(key_color, "Ctrl+F", "Buscar")
        self._row(key_color, "F3", "Siguiente coincidencia")
        self._row(key_color, "Ctrl+G", "Ir a linea")
        self._row(key_color, "Ctrl+T", "Ejecutar comando shell")
        self._row(key_color, "Ctrl+Space", "Autocompletado")
        self._row(key_color, "Ctrl+L", "Toggle numeros de linea")
        print("\n")

        print(sect_color + "SIDEBAR (con foco)" + R + "\n")
        self._row(key_color, "Up / Down", "Navegar")
        self._row(key_color, "Enter / Right", "Expandir carpeta o abrir fichero")
        self._row(key_color, "Left", "Colapsar carpeta")
        self._row(key_color, ".", "Toggle ficheros ocultos")
        self._row(key_color, "u", "Toggle Unicode/ASCII")
        print("\n")

        print(dim_color + "Pulsa cualquier tecla para volver..." + R + "\n")
        inp.read_key_blocking()
    }

    fn _row(self, key_color, key, desc) {
        let R = ANSI["RESET"]
        let dim = ansi_rgb(98, 114, 164)
        print("  " + key_color + pad_visual(key, 18) + R + dim + " - " + R + desc + "\n")
    }
}
// =============================================================================
// =============================================================================
// SECCION 11.5: Panel y LayoutManager
// =============================================================================
// Un Panel es un area de edicion. Tiene su propia lista de pestanas
// (EditorBuffer) y su propio buffer activo. Varios paneles componen el
// workspace del Editor segun el layout activo.
//
// LayoutManager gestiona los 6 layouts predefinidos:
//   single, split_v, split_h, three_left, three_right, grid

class Panel {
    "Panel de edicion: contiene una lista de buffers y uno activo."

    fn __init__(self, buffer) {
        // Si recibe un EditorBuffer, lo usa como inicial; si null, queda vacio
        self.buffers = []
        if buffer != null {
            append(self.buffers, buffer)
        }
        self.active_idx = 0
        // Cuando es true, el panel se redibuja entero la proxima vez
        self.full_redraw_local = true
    }

    fn ab(self) {
        // Active EditorBuffer; null si no hay buffers
        if len(self.buffers) == 0 { return null }
        if self.active_idx < 0 or self.active_idx >= len(self.buffers) {
            self.active_idx = 0
        }
        return self.buffers[self.active_idx]
    }

    fn tab_count(self) { return len(self.buffers) }

    fn has_tabbar(self) { return len(self.buffers) > 1 }

    // Conmutar a la pestana N (idx); si esta fuera de rango, no hace nada
    fn switch_to(self, idx) {
        if idx >= 0 and idx < len(self.buffers) {
            self.active_idx = idx
            self.full_redraw_local = true
        }
    }

    // Anadir una pestana y conmutar a ella
    fn add_buffer(self, buffer) {
        append(self.buffers, buffer)
        self.active_idx = len(self.buffers) - 1
        self.full_redraw_local = true
    }

    // Cerrar la pestana en idx; devuelve true si quedan pestanas, false si no
    fn close_tab(self, idx) {
        if idx < 0 or idx >= len(self.buffers) { return true }
        let new_buffers = []
        let i = 0
        while i < len(self.buffers) {
            if i != idx { append(new_buffers, self.buffers[i]) }
            i = i + 1
        }
        self.buffers = new_buffers
        if self.active_idx >= len(self.buffers) {
            self.active_idx = len(self.buffers) - 1
        }
        if self.active_idx < 0 { self.active_idx = 0 }
        self.full_redraw_local = true
        return len(self.buffers) > 0
    }

    // Buscar si ya hay una pestana con ese filename; retorna idx o -1
    fn find_buffer(self, filename) {
        let i = 0
        while i < len(self.buffers) {
            if self.buffers[i].filename == filename { return i }
            i = i + 1
        }
        return -1
    }

    fn is_modified(self) {
        for b in self.buffers {
            if b.buf.modified { return true }
        }
        return false
    }
}


class LayoutManager {
    "Gestor del layout de paneles. Calcula rectangulos y transiciones."

    fn __init__(self) {
        self.layout = "single"
    }

    fn n_panels_required(self) {
        if self.layout == "single" { return 1 }
        if self.layout == "split_v" or self.layout == "split_h" { return 2 }
        if self.layout == "three_left" or self.layout == "three_right" { return 3 }
        if self.layout == "grid" { return 4 }
        return 1
    }

    // Devuelve [{x,y,w,h}, ...] para los paneles dentro del area dada
    fn rects(self, area_x, area_y, area_w, area_h) {
        let rects = []
        let l = self.layout

        if l == "single" {
            append(rects, { "x": area_x, "y": area_y, "w": area_w, "h": area_h })
            return rects
        }
        if l == "split_v" {
            let half = (area_w - 1) / 2
            let other = area_w - 1 - half
            append(rects, { "x": area_x, "y": area_y, "w": half, "h": area_h })
            append(rects, { "x": area_x + half + 1, "y": area_y, "w": other, "h": area_h })
            return rects
        }
        if l == "split_h" {
            let half = (area_h - 1) / 2
            let other = area_h - 1 - half
            append(rects, { "x": area_x, "y": area_y, "w": area_w, "h": half })
            append(rects, { "x": area_x, "y": area_y + half + 1, "w": area_w, "h": other })
            return rects
        }
        if l == "three_left" {
            let half_w = (area_w - 1) / 2
            let other_w = area_w - 1 - half_w
            let half_h = (area_h - 1) / 2
            let other_h = area_h - 1 - half_h
            append(rects, { "x": area_x, "y": area_y, "w": half_w, "h": area_h })
            append(rects, { "x": area_x + half_w + 1, "y": area_y, "w": other_w, "h": half_h })
            append(rects, { "x": area_x + half_w + 1, "y": area_y + half_h + 1, "w": other_w, "h": other_h })
            return rects
        }
        if l == "three_right" {
            let half_w = (area_w - 1) / 2
            let other_w = area_w - 1 - half_w
            let half_h = (area_h - 1) / 2
            let other_h = area_h - 1 - half_h
            append(rects, { "x": area_x, "y": area_y, "w": half_w, "h": half_h })
            append(rects, { "x": area_x, "y": area_y + half_h + 1, "w": half_w, "h": other_h })
            append(rects, { "x": area_x + half_w + 1, "y": area_y, "w": other_w, "h": area_h })
            return rects
        }
        if l == "grid" {
            let half_w = (area_w - 1) / 2
            let other_w = area_w - 1 - half_w
            let half_h = (area_h - 1) / 2
            let other_h = area_h - 1 - half_h
            append(rects, { "x": area_x,                  "y": area_y,                  "w": half_w,  "h": half_h })
            append(rects, { "x": area_x + half_w + 1,     "y": area_y,                  "w": other_w, "h": half_h })
            append(rects, { "x": area_x,                  "y": area_y + half_h + 1,     "w": half_w,  "h": other_h })
            append(rects, { "x": area_x + half_w + 1,     "y": area_y + half_h + 1,     "w": other_w, "h": other_h })
            return rects
        }
        append(rects, { "x": area_x, "y": area_y, "w": area_w, "h": area_h })
        return rects
    }

    // Transicion al hacer split. direction = "v" o "h".
    // Devuelve true si se pudo, false si ya no caben mas paneles.
    fn split(self, direction) {
        let l = self.layout
        if l == "single" {
            if direction == "v" { self.layout = "split_v" }
            if direction == "h" { self.layout = "split_h" }
            return true
        }
        if l == "split_v" {
            self.layout = "three_right"
            return true
        }
        if l == "split_h" {
            self.layout = "three_left"
            return true
        }
        if l == "three_left" or l == "three_right" {
            self.layout = "grid"
            return true
        }
        return false
    }

    // Reducir layout cuando se cierra un panel
    fn shrink(self) {
        let l = self.layout
        if l == "grid" {
            self.layout = "three_right"
        } elif l == "three_left" or l == "three_right" {
            self.layout = "split_v"
        } elif l == "split_v" or l == "split_h" {
            self.layout = "single"
        }
    }

    fn cycle(self) {
        if self.layout == "split_v" { self.layout = "split_h"; return }
        if self.layout == "split_h" { self.layout = "split_v"; return }
        if self.layout == "three_left" { self.layout = "three_right"; return }
        if self.layout == "three_right" { self.layout = "three_left"; return }
    }
}


// =============================================================================
// SECCION 11.6: AutocompletePopup (estilo VS Code)
// =============================================================================
// Popup flotante con bordes Unicode/ASCII y candidatos resaltados por
// prefijo. Auto-trigger tras 2 caracteres alfanumericos.

class AutocompletePopup {
    "Popup de autocompletado. Muestra candidatos con icono y resaltado."

    fn __init__(self, candidates, x, y, prefix, use_unicode) {
        self.candidates = candidates    // list of {text, kind}
        self.sel = 0
        self.x = x                       // 1-based ANSI
        self.y = y                       // 1-based ANSI
        self.prefix = prefix
        self.use_unicode = use_unicode
        self.max_show = 8
        self.scroll = 0
        self.width = self._compute_width()
    }

    fn _compute_width(self) {
        let mw = 0
        for c in self.candidates {
            // " [K] text  kind " = 2 + 4 + len(text) + 2 + len(kind) + 1
            let w = 2 + 4 + len(c["text"]) + 2 + len(c["kind"]) + 1
            if w > mw { mw = w }
        }
        if mw < 18 { mw = 18 }
        if mw > 50 { mw = 50 }
        return mw
    }

    fn n_visible(self) {
        let n = len(self.candidates)
        if n > self.max_show { return self.max_show }
        return n
    }

    fn move_up(self) {
        if len(self.candidates) == 0 { return }
        if self.sel > 0 {
            self.sel = self.sel - 1
        } else {
            self.sel = len(self.candidates) - 1
        }
        self._adjust_scroll()
    }

    fn move_down(self) {
        if len(self.candidates) == 0 { return }
        self.sel = self.sel + 1
        if self.sel >= len(self.candidates) { self.sel = 0 }
        self._adjust_scroll()
    }

    fn _adjust_scroll(self) {
        if self.sel < self.scroll { self.scroll = self.sel }
        if self.sel >= self.scroll + self.max_show {
            self.scroll = self.sel - self.max_show + 1
        }
    }

    fn sel_text(self) {
        if self.sel < 0 or self.sel >= len(self.candidates) { return null }
        return self.candidates[self.sel]["text"]
    }

    // Pintar el popup. Devuelve string ANSI para que el editor lo añada
    // a su buffer de salida en lugar de printear directamente.
    fn render(self) {
        let out = ""
        let n = self.n_visible()
        if n == 0 { return out }

        let u = self.use_unicode
        // Caracteres de borde
        let TL = "+"; let TR = "+"; let BL = "+"; let BR = "+"
        let H = "-";  let V = "|"
        if u {
            TL = from_char(226) + from_char(149) + from_char(173)  // ╭
            TR = from_char(226) + from_char(149) + from_char(174)  // ╮
            BL = from_char(226) + from_char(149) + from_char(176)  // ╰
            BR = from_char(226) + from_char(149) + from_char(175)  // ╯
            H  = from_char(226) + from_char(148) + from_char(128)  // ─
            V  = from_char(226) + from_char(148) + from_char(130)  // │
        }

        let bg = ansi_rgb_bg(40, 42, 54)
        let bg_sel = ansi_rgb_bg(98, 114, 164)
        let fg = ansi_rgb(248, 248, 242)
        let dim = ansi_rgb(98, 114, 164)
        let prefix_color = ansi_rgb(255, 121, 198) + ANSI["BOLD"]
        let R = ANSI["RESET"]

        let inner_w = self.width

        // Top border
        out = out + ansi_cursor_pos(self.y, self.x)
        out = out + dim + TL + repeat(H, inner_w) + TR + R

        // Filas
        let i = 0
        while i < n {
            let idx = self.scroll + i
            out = out + ansi_cursor_pos(self.y + 1 + i, self.x)
            out = out + dim + V + R

            if idx < len(self.candidates) {
                let cand = self.candidates[idx]
                let kind = cand["kind"]
                let text = cand["text"]

                let icon_letter = "?"
                let icon_color = fg
                if kind == "kw"      { icon_letter = "K"; icon_color = ansi_rgb(255, 121, 198) }
                if kind == "type"    { icon_letter = "T"; icon_color = ansi_rgb(139, 233, 253) }
                if kind == "builtin" { icon_letter = "B"; icon_color = ansi_rgb(80, 250, 123) }
                if kind == "func"    { icon_letter = "F"; icon_color = ansi_rgb(80, 250, 123) }
                if kind == "id"      { icon_letter = "V"; icon_color = ansi_rgb(248, 248, 242) }

                let row_bg = bg
                if idx == self.sel { row_bg = bg_sel }

                let plen = len(self.prefix)
                let prefix_part = ""
                let rest_part = text
                if plen <= len(text) and lower(substr(text, 0, plen)) == lower(self.prefix) {
                    prefix_part = substr(text, 0, plen)
                    rest_part = substr(text, plen, len(text) - plen)
                }

                let consumed = 2 + 3 + 1 + len(text) + 2 + len(kind) + 1
                let pad = inner_w - consumed
                if pad < 0 { pad = 0 }

                out = out + row_bg + " "
                out = out + "[" + icon_color + icon_letter + R + row_bg + "]"
                out = out + " "
                if prefix_part != "" {
                    out = out + prefix_color + row_bg + prefix_part + R + row_bg
                }
                out = out + fg + row_bg + rest_part + R + row_bg
                out = out + repeat(" ", pad)
                out = out + dim + row_bg + " " + kind + " " + R
            }
            out = out + dim + V + R
            i = i + 1
        }

        // Bottom border
        out = out + ansi_cursor_pos(self.y + 1 + n, self.x)
        out = out + dim + BL + repeat(H, inner_w) + BR + R

        return out
    }

    // Devuelve el rectangulo {x, y, w, h} ocupado por el popup en pantalla,
    // 1-based (para que el editor pueda re-pintar esas zonas al cerrar).
    fn rect(self) {
        let n = self.n_visible()
        return { "x": self.x, "y": self.y, "w": self.width + 2, "h": n + 2 }
    }
}
// =============================================================================
// SECCION 12: Editor (workspace con paneles, sidebar y autocompletado)
// =============================================================================

class Editor {
    "Editor multicursor con paneles, sidebar, pestanas y autocompletado."

    fn __init__(self, config) {
        self.config = config
        self.hl = Highlighter()

        // Panels
        self.panels = []                 // list[Panel]
        self.active_panel_idx = 0
        self.layout_mgr = LayoutManager()

        // Sidebar
        self.sidebar_visible = false
        self.sidebar_root = ""
        self.tree = null
        self.sidebar_sel = 0
        self.sidebar_scroll = 0

        // Foco: "editor" o "sidebar"
        self.focus = "editor"

        // Clipboard global
        self.kill_buffer = ""

        // Backend de input
        self.input_backend = null

        // Tamano del terminal
        let ts = TermSize()
        self.term_w = ts.cols
        self.term_h = ts.rows

        // UI state
        self.full_redraw = true
        self.status_msg = ""
        self.status_until = 0
        self.show_lineno = true

        // Autocomplete popup
        self.popup = null            // null si no hay popup activo

        // Output buffer para render bufferizado: acumulamos toda la salida
        // del frame y hacemos un solo print al final. Reduce drasticamente
        // los syscalls a WriteConsole en Windows.
        self._out = ""
    }

    // Helper: escribe en el buffer de salida del frame
    fn _w(self, s) {
        self._out = self._out + s
    }

    // Vuelca el buffer a stdout y lo resetea.
    // CRITICO: tras el cambio del interprete a print bufferizado interno,
    // necesitamos llamar flush_output() para forzar el volcado al terminal.
    // Si no, el output se queda en el buffer C++ del interprete y nada
    // aparece en pantalla hasta que se acumulan 64KB o cierras vnano.
    fn _flush(self) {
        if self._out != "" {
            print(self._out)
            self._out = ""
        }
        flush_output()
    }

    // -------------------------------------------------------------------------
    // ACCESO AL PANEL/BUFFER ACTIVO
    // -------------------------------------------------------------------------

    fn _ap(self) {
        // Active panel; null si no hay paneles
        if len(self.panels) == 0 { return null }
        if self.active_panel_idx < 0 or self.active_panel_idx >= len(self.panels) {
            self.active_panel_idx = 0
        }
        return self.panels[self.active_panel_idx]
    }

    fn _ab(self) {
        // Active EditorBuffer of active panel; null si no hay
        let p = self._ap()
        if p == null { return null }
        return p.ab()
    }

    fn _has_active(self) {
        let b = self._ab()
        return b != null
    }

    // -------------------------------------------------------------------------
    // GESTION DE PESTANAS (en el panel activo)
    // -------------------------------------------------------------------------

    fn open_file(self, path) {
        // Si ya hay una pestana con ese fichero EN EL PANEL ACTIVO,
        // conmutar; si no, anadir nueva pestana al panel activo.
        let p = self._ap()
        if p == null {
            // No hay paneles: crear el primero
            self._ensure_initial_panel()
            p = self._ap()
        }
        let idx = p.find_buffer(path)
        if idx != -1 {
            p.switch_to(idx)
            self.full_redraw = true
            self.set_status("Cambiado a pestana: " + basename_or(path, "?"))
            return
        }
        let buf = EditorBuffer(path)
        p.add_buffer(buf)
        self.full_redraw = true
        self._recompute_state(0)
        self.set_status("Abierto: " + basename_or(path, "?"))
    }

    fn new_tab(self) {
        let p = self._ap()
        if p == null {
            self._ensure_initial_panel()
            p = self._ap()
        }
        let buf = EditorBuffer("")
        p.add_buffer(buf)
        self.full_redraw = true
        self.set_status("Nueva pestana")
    }

    fn _ensure_initial_panel(self) {
        if len(self.panels) == 0 {
            append(self.panels, Panel(null))
            self.active_panel_idx = 0
        }
    }

    fn close_tab(self, inp) {
        let p = self._ap()
        if p == null { return false }
        if p.tab_count() == 0 { return false }
        let ab = p.ab()
        if ab.buf.modified {
            let ans = self._prompt(inp, "Cambios sin guardar en '" + ab.display_name() + "'. Cerrar? (y/N): ", "")
            if ans == null { return false }
            if lower(ans) != "y" { return false }
        }
        let still_has = p.close_tab(p.active_idx)
        self.full_redraw = true
        if not still_has {
            // Panel vacio: si hay mas paneles, cerrar este. Si es el unico, abrir
            // pestana vacia.
            if len(self.panels) > 1 {
                self._close_active_panel()
            } else {
                self.new_tab()
            }
        }
        return true
    }

    fn _close_active_panel(self) {
        let new_panels = []
        let i = 0
        while i < len(self.panels) {
            if i != self.active_panel_idx { append(new_panels, self.panels[i]) }
            i = i + 1
        }
        self.panels = new_panels
        self.layout_mgr.shrink()
        if self.active_panel_idx >= len(self.panels) {
            self.active_panel_idx = len(self.panels) - 1
        }
        if self.active_panel_idx < 0 { self.active_panel_idx = 0 }
        self.full_redraw = true
        self.set_status("Panel cerrado. Layout: " + self.layout_mgr.layout)
    }

    fn next_tab(self) {
        let p = self._ap()
        if p == null or p.tab_count() <= 1 { return }
        p.active_idx = p.active_idx + 1
        if p.active_idx >= p.tab_count() { p.active_idx = 0 }
        p.full_redraw_local = true
        self.full_redraw = true
    }

    fn prev_tab(self) {
        let p = self._ap()
        if p == null or p.tab_count() <= 1 { return }
        p.active_idx = p.active_idx - 1
        if p.active_idx < 0 { p.active_idx = p.tab_count() - 1 }
        p.full_redraw_local = true
        self.full_redraw = true
    }

    // -------------------------------------------------------------------------
    // GESTION DE PANELES
    // -------------------------------------------------------------------------

    fn split_active_panel(self, direction) {
        let p = self._ap()
        if p == null { return }
        let n_before = self.layout_mgr.n_panels_required()
        let ok = self.layout_mgr.split(direction)
        if not ok {
            self.set_status("Maximo de paneles alcanzado")
            return
        }
        let n_after = self.layout_mgr.n_panels_required()
        // Crear paneles nuevos hasta llegar a n_after, copiando el buffer activo
        // del panel actual como vista clonada
        while len(self.panels) < n_after {
            let active = p.ab()
            let new_buf = null
            if active != null {
                new_buf = active.clone_view()
            }
            append(self.panels, Panel(new_buf))
        }
        // Foco al panel nuevo
        self.active_panel_idx = len(self.panels) - 1
        self.full_redraw = true
        self.set_status("Split " + direction + ". Layout: " + self.layout_mgr.layout)
    }

    fn rotate_panel_focus(self) {
        if len(self.panels) <= 1 {
            self.set_status("Solo hay un panel")
            return
        }
        self.active_panel_idx = self.active_panel_idx + 1
        if self.active_panel_idx >= len(self.panels) { self.active_panel_idx = 0 }
        self.full_redraw = true
        self.set_status("Foco -> Panel " + str(self.active_panel_idx + 1))
    }

    fn cycle_layout(self) {
        if self.layout_mgr.n_panels_required() == 1 {
            self.set_status("Layout sin alternativa para 1 panel")
            return
        }
        self.layout_mgr.cycle()
        self.full_redraw = true
        self.set_status("Layout: " + self.layout_mgr.layout)
    }

    // -------------------------------------------------------------------------
    // PROYECTO Y SIDEBAR
    // -------------------------------------------------------------------------

    fn set_project(self, path) {
        self.sidebar_root = path
        self.tree = FileTree(path, self.config)
        self.sidebar_visible = true
        self.full_redraw = true
    }

    fn toggle_sidebar(self) {
        if self.tree == null {
            self.set_project(getcwd())
            self.set_status("Sidebar abierto en cwd")
            return
        }
        self.sidebar_visible = not self.sidebar_visible
        if not self.sidebar_visible and self.focus == "sidebar" {
            self.focus = "editor"
        }
        self.full_redraw = true
    }

    fn focus_sidebar(self) {
        if not self.sidebar_visible or self.tree == null { return }
        self.focus = "sidebar"
        self.full_redraw = true
    }

    fn focus_editor(self) {
        self.focus = "editor"
        self.full_redraw = true
    }

    fn rotate_focus(self) {
        if self.focus == "editor" {
            self.focus_sidebar()
        } else {
            self.focus_editor()
        }
    }

    // -------------------------------------------------------------------------
    // HELPERS DE CURSOR
    // -------------------------------------------------------------------------

    fn _make_cursor(self, row, col, primary) {
        return { "row": row, "col": col, "anchor_row": row, "anchor_col": col, "primary": primary }
    }

    fn _primary(self) {
        let ab = self._ab()
        if ab == null { return null }
        return ab.cursors[0]
    }

    fn _has_multi(self) {
        let ab = self._ab()
        if ab == null { return false }
        return len(ab.cursors) > 1
    }

    fn _any_selection(self) {
        let ab = self._ab()
        if ab == null { return false }
        for cur in ab.cursors {
            if cur["row"] != cur["anchor_row"] or cur["col"] != cur["anchor_col"] {
                return true
            }
        }
        return false
    }

    fn _cursor_has_sel(self, cur) {
        return cur["row"] != cur["anchor_row"] or cur["col"] != cur["anchor_col"]
    }

    fn _cursor_sel_range(self, cur) {
        let r1 = cur["anchor_row"]; let c1 = cur["anchor_col"]
        let r2 = cur["row"]; let c2 = cur["col"]
        if r1 > r2 or (r1 == r2 and c1 > c2) {
            let tr = r1; let tc = c1; r1 = r2; c1 = c2; r2 = tr; c2 = tc
        }
        return { "r1": r1, "c1": c1, "r2": r2, "c2": c2 }
    }

    fn _cursors_desc(self) {
        let ab = self._ab()
        let lst = []
        for c in ab.cursors { append(lst, c) }
        let n = len(lst)
        let i = 0
        while i < n {
            let j = 0
            while j < n - 1 - i {
                let a = lst[j]; let b = lst[j+1]
                let cmp = false
                if a["row"] < b["row"] {
                    cmp = true
                } elif a["row"] == b["row"] and a["col"] < b["col"] {
                    cmp = true
                }
                if cmp { lst[j] = b; lst[j+1] = a }
                j = j + 1
            }
            i = i + 1
        }
        return lst
    }

    fn _coalesce_cursors(self) {
        let ab = self._ab()
        if len(ab.cursors) <= 1 { return }
        let kept = []
        let i = 0
        while i < len(ab.cursors) {
            let c = ab.cursors[i]
            let dup = false
            for k in kept {
                if k["row"] == c["row"] and k["col"] == c["col"] {
                    dup = true; break
                }
            }
            if not dup { append(kept, c) }
            i = i + 1
        }
        let j = 0
        while j < len(kept) {
            kept[j]["primary"] = (j == 0)
            j = j + 1
        }
        ab.cursors = kept
        self.full_redraw = true
    }

    fn _clamp_all_cursors(self) {
        let ab = self._ab()
        if ab == null { return }
        let n = ab.buf.nlines()
        if n == 0 { return }
        for cur in ab.cursors {
            if cur["row"] < 0 { cur["row"] = 0 }
            if cur["row"] >= n { cur["row"] = n - 1 }
            let ln_len = len(ab.buf.line(cur["row"]))
            if cur["col"] < 0 { cur["col"] = 0 }
            if cur["col"] > ln_len { cur["col"] = ln_len }
            if cur["anchor_row"] < 0 { cur["anchor_row"] = 0 }
            if cur["anchor_row"] >= n { cur["anchor_row"] = n - 1 }
            let aln = len(ab.buf.line(cur["anchor_row"]))
            if cur["anchor_col"] < 0 { cur["anchor_col"] = 0 }
            if cur["anchor_col"] > aln { cur["anchor_col"] = aln }
        }
    }

    fn _recompute_state(self, from_row) {
        let ab = self._ab()
        if ab == null { return }
        let n = ab.buf.nlines()
        if from_row < 0 { from_row = 0 }
        if from_row >= n { return }
        let i = from_row
        ab.buf.mark_dirty(i)
        while i < n {
            let entry_state = { "in_cmt": false, "in_docstr": false }
            if i > 0 {
                let prev = ab.buf.in_state_at[i-1]
                let res = self.hl.tokenize(ab.buf.lines[i-1], prev)
                entry_state = res["state"]
            }
            let cur_st = ab.buf.in_state_at[i]
            if cur_st["in_cmt"] != entry_state["in_cmt"] or cur_st["in_docstr"] != entry_state["in_docstr"] {
                ab.buf.in_state_at[i] = entry_state
                ab.buf.mark_dirty(i)
            }
            i = i + 1
        }
    }

    fn set_status(self, msg) {
        self.status_msg = msg
        self.status_until = time_ms() + 4000
    }

    fn _any_modified(self) {
        for p in self.panels {
            if p.is_modified() { return true }
        }
        return false
    }

    fn _word_at_cursor(self) {
        let ab = self._ab()
        if ab == null { return "" }
        let p = self._primary()
        let ln = ab.buf.line(p["row"])
        if p["col"] > len(ln) { return "" }
        let start = p["col"]
        while start > 0 {
            if not self.hl._is_alnum(substr(ln, start-1, 1)) { break }
            start = start - 1
        }
        let end = p["col"]
        while end < len(ln) {
            if not self.hl._is_alnum(substr(ln, end, 1)) { break }
            end = end + 1
        }
        if end <= start { return "" }
        return substr(ln, start, end - start)
    }
    // -------------------------------------------------------------------------
    // GEOMETRIA
    // -------------------------------------------------------------------------

    fn _sidebar_w(self) {
        if not self.sidebar_visible or self.tree == null { return 0 }
        return self.config.sidebar_width
    }

    // El area total que ocupan TODOS los paneles juntos:
    // x = sidebar_w (1-based: +1 al pintar)
    // y = 2 (debajo del header)
    // w = term_w - sidebar_w
    // h = term_h - 2 (header + statusbar)
    fn _panels_area(self) {
        let x = self._sidebar_w()
        let y = 1                        // 0-based: fila 1 (debajo de header en fila 0)
        let w = self.term_w - self._sidebar_w()
        let h = self.term_h - 2
        return { "x": x, "y": y, "w": w, "h": h }
    }

    // Devuelve el rect del panel idx (0-based en pantalla) ya en
    // coordenadas absolutas dentro del workspace
    fn _panel_rects(self) {
        let area = self._panels_area()
        return self.layout_mgr.rects(area["x"], area["y"], area["w"], area["h"])
    }

    // Geometria interna de un panel: cabecera (1), tabbar (0|1), contenido
    fn _panel_header_h(self) { return 1 }

    fn _panel_content_h(self, panel, rect) {
        let used = self._panel_header_h()
        if panel.has_tabbar() { used = used + 1 }
        return rect["h"] - used
    }

    fn _panel_content_y(self, panel, rect) {
        let y = rect["y"] + self._panel_header_h()
        if panel.has_tabbar() { y = y + 1 }
        return y
    }

    fn _gutter_width_for(self, panel) {
        if not self.show_lineno { return 0 }
        let ab = panel.ab()
        if ab == null { return 0 }
        let n = ab.buf.nlines()
        let w = 1
        let m = n
        while m >= 10 { w = w + 1; m = m / 10 }
        return w + 1
    }

    fn _panel_text_width(self, panel, rect) {
        return rect["w"] - self._gutter_width_for(panel)
    }

    // -------------------------------------------------------------------------
    // SCROLL (para el panel/buffer activo)
    // -------------------------------------------------------------------------

    fn _scroll(self) {
        self._clamp_all_cursors()
        let p = self._ap()
        if p == null { return }
        let ab = p.ab()
        if ab == null { return }
        let rects = self._panel_rects()
        if self.active_panel_idx >= len(rects) { return }
        let rect = rects[self.active_panel_idx]
        let h = self._panel_content_h(p, rect)
        let w = self._panel_text_width(p, rect)
        let cur = ab.cursors[0]
        if cur["row"] < ab.row_off {
            ab.row_off = cur["row"]; self.full_redraw = true
        }
        if cur["row"] >= ab.row_off + h {
            ab.row_off = cur["row"] - h + 1; self.full_redraw = true
        }
        if cur["col"] < ab.col_off {
            ab.col_off = cur["col"]; self.full_redraw = true
        }
        if cur["col"] >= ab.col_off + w {
            ab.col_off = cur["col"] - w + 1; self.full_redraw = true
        }
    }

    // -------------------------------------------------------------------------
    // RENDER PRINCIPAL
    // -------------------------------------------------------------------------

    fn render(self) {
        if not self._has_active() { return }

        if self.full_redraw { self._w(ANSI["CLEAR"]) }

        // Chrome (header, sidebar, divisores) solo se redibuja cuando hay
        // un cambio estructural (full_redraw=true). En el resto de casos,
        // solo redibujamos el contenido del panel activo (que respeta line_dirty)
        // y la status bar (que muestra Lin/Col actuales).
        if self.full_redraw {
            self._draw_header()
            if self.sidebar_visible and self.tree != null { self._draw_sidebar() }
            self._draw_dividers()
        }

        self._draw_panels()
        self._draw_status_bar()

        // Popup encima de todo
        if self.popup != null {
            self._w(self.popup.render())
        }

        // Posicionar cursor del terminal
        if self.focus == "editor" and self.popup == null {
            self._position_cursor_in_active_panel()
            self._w(ESC_CUR_SHOW)
        } else {
            self._w(ESC_CUR_HIDE)
        }

        self.full_redraw = false
        self._flush()
    }

    fn _position_cursor_in_active_panel(self) {
        let p = self._ap()
        if p == null { return }
        let ab = p.ab()
        if ab == null { return }
        let rects = self._panel_rects()
        if self.active_panel_idx >= len(rects) { return }
        let rect = rects[self.active_panel_idx]
        let cy = self._panel_content_y(p, rect)
        let gw = self._gutter_width_for(p)
        let cur = ab.cursors[0]
        let screen_row = cur["row"] - ab.row_off + cy + 1            // +1 para 1-based ANSI
        let screen_col = cur["col"] - ab.col_off + rect["x"] + gw + 1
        self._w(ansi_cursor_pos(screen_row, screen_col))
    }

    fn _draw_header(self) {
        self._w(ansi_cursor_pos(1, 1))
        self._w(ANSI["REVERSE"] + ANSI["BOLD"])
        let title = " vnano 0.5 "
        let ab = self._ab()
        let fname = "[Sin buffer]"
        let mod_marker = ""
        let multi = ""
        if ab != null {
            fname = ab.display_name()
            if ab.buf.modified { mod_marker = " * " }
            if self._has_multi() { multi = "  [" + str(len(ab.cursors)) + " cursores]" }
        }
        let foco = ""
        if self.focus == "sidebar" { foco = "  [sidebar]" }
        let project = ""
        if self.tree != null { project = "  Proy: " + basename_or(self.sidebar_root, "?") }
        let panels_info = ""
        if len(self.panels) > 1 {
            panels_info = "  [" + str(self.active_panel_idx + 1) + "/" + str(len(self.panels)) + " " + self.layout_mgr.layout + "]"
        }
        let middle = "  " + fname + mod_marker + multi + foco + panels_info + project
        let total = pad_visual(title + middle, self.term_w)
        self._w(total)
        self._w(ANSI["RESET"])
    }

    fn _draw_sidebar(self) {
        let w = self._sidebar_w()
        let h = self.term_h - 2          // header + statusbar
        let visible = self.tree.list_visible()

        if self.sidebar_sel < self.sidebar_scroll { self.sidebar_scroll = self.sidebar_sel }
        if self.sidebar_sel >= self.sidebar_scroll + h - 1 {
            self.sidebar_scroll = self.sidebar_sel - h + 2
        }
        if self.sidebar_scroll < 0 { self.sidebar_scroll = 0 }

        self._w(ansi_cursor_pos(2, 1))
        let dim = ansi_rgb(98, 114, 164)
        let header_txt = pad_visual(" EXPLORADOR", w - 1) + VBAR
        self._w(dim + ANSI["BOLD"] + header_txt + ANSI["RESET"])

        let active_path = ""
        let ab = self._ab()
        if ab != null { active_path = ab.filename }

        let i = 1
        while i < h {
            self._w(ansi_cursor_pos(2 + i, 1))
            let idx = self.sidebar_scroll + i - 1
            if idx < len(visible) {
                let node = visible[idx]
                let line = self.tree.render_line(node, w - 1, idx == self.sidebar_sel, self.focus == "sidebar", node["path"] == active_path)
                self._w(line)
                self._w(dim + VBAR + ANSI["RESET"])
            } else {
                self._w(pad_visual("", w - 1))
                self._w(dim + VBAR + ANSI["RESET"])
            }
            i = i + 1
        }
    }

    fn _draw_panels(self) {
        let rects = self._panel_rects()
        let i = 0
        while i < len(self.panels) and i < len(rects) {
            let p = self.panels[i]
            let rect = rects[i]
            self._draw_panel(p, rect, i == self.active_panel_idx)
            i = i + 1
        }
    }

    fn _draw_panel(self, panel, rect, focused) {
        let ab = panel.ab()

        // El header (1 linea) y la tabbar (0-1 lineas) son baratos: los
        // repintamos siempre. Solo el contenido del panel (muchas lineas)
        // se optimiza con line_dirty.

        // Header del panel: nombre del fichero activo, marcador de foco
        let head_text = " "
        if ab != null {
            head_text = head_text + ab.display_name()
            if ab.buf.modified { head_text = head_text + " *" }
        }
        if focused { head_text = head_text + "  [FOCO]" }
        head_text = pad_visual(head_text, rect["w"])

        let bg = ansi_rgb_bg(40, 42, 54)
        let active_bg = ansi_rgb_bg(98, 114, 164)
        let fg = ansi_rgb(248, 248, 242)
        let pink = ansi_rgb(255, 121, 198)
        let R = ANSI["RESET"]

        self._w(ansi_cursor_pos(rect["y"] + 1, rect["x"] + 1))
        if focused {
            self._w(active_bg + ANSI["BOLD"] + fg + head_text + R)
        } else {
            self._w(bg + fg + head_text + R)
        }

        // Tabbar del panel (si tiene >1 buffer)
        if panel.has_tabbar() {
            self._w(ansi_cursor_pos(rect["y"] + 2, rect["x"] + 1))
            let s = ""
            let i = 0
            while i < panel.tab_count() {
                let b = panel.buffers[i]
                let label = " " + b.display_name()
                if b.buf.modified { label = label + "*" }
                label = label + " "
                if i == panel.active_idx {
                    s = s + active_bg + pink + ANSI["BOLD"] + label + R
                } else {
                    s = s + bg + fg + label + R
                }
                s = s + ansi_rgb(68, 71, 90) + VBAR + R
                i = i + 1
            }
            let visual = visual_len(strip_ansi(s))
            if visual < rect["w"] {
                s = s + bg + repeat(" ", rect["w"] - visual) + R
            }
            self._w(s)
        }

        // header_used: 1 solo header, 2 header+tabbar
        let header_used = 1
        if panel.has_tabbar() { header_used = 2 }

        // Contenido del buffer activo (siempre se evalua, respeta line_dirty)
        if ab != null {
            self._draw_panel_content(panel, ab, rect, header_used, focused)
        }
    }

    // Marca como dirty las lineas visibles del buffer ab que contienen la
    // palabra completa word. Usado para invalidar solo las lineas afectadas
    // cuando cambia cur_word.
    fn _mark_lines_with_word_dirty(self, ab, word, content_h) {
        let i = 0
        while i < content_h {
            let row_idx = ab.row_off + i
            if row_idx >= ab.buf.nlines() {
                i = i + 1
                continue
            }
            let line = ab.buf.lines[row_idx]
            // Optimizacion: si la palabra no esta como subcadena, ni siquiera
            // tokenizamos. find_str es mucho mas barato que tokenize.
            if find_str(line, word, 0) != -1 {
                ab.buf.line_dirty[row_idx] = true
            }
            i = i + 1
        }
    }

    fn _draw_panel_content(self, panel, ab, rect, header_used, focused) {
        let content_y = rect["y"] + header_used   // 0-based; 1-based = +1 al pintar
        let content_h = rect["h"] - header_used
        let content_x = rect["x"]
        let cw = rect["w"]
        let gw = self._gutter_width_for(panel)
        let tw = cw - gw

        let cur_word = ""
        if focused {
            let p = self._primary()
            if p != null {
                cur_word = self._word_at_cursor()
            }
        }

        // Optimizacion clave: si cur_word cambio respecto al render anterior,
        // solo marcamos como dirty las lineas que CONTIENEN la palabra vieja
        // o la palabra nueva. Asi al mover el cursor solo redibujamos las
        // lineas con la palabra resaltada, no todo el panel.
        if cur_word != ab.last_cur_word {
            // Marcar dirty lineas con la palabra vieja (para borrar highlight)
            if ab.last_cur_word != "" and len(ab.last_cur_word) >= 2 {
                self._mark_lines_with_word_dirty(ab, ab.last_cur_word, content_h)
            }
            // Marcar dirty lineas con la palabra nueva (para pintar highlight)
            if cur_word != "" and len(cur_word) >= 2 {
                self._mark_lines_with_word_dirty(ab, cur_word, content_h)
            }
            ab.last_cur_word = cur_word
        }

        let force_all = self.full_redraw or panel.full_redraw_local

        let i = 0
        while i < content_h {
            let row_idx = ab.row_off + i
            let must_draw = force_all
            if not must_draw and row_idx < ab.buf.nlines() {
                must_draw = ab.buf.line_dirty[row_idx]
            }
            if must_draw {
                let screen_y = content_y + i + 1   // 1-based ANSI
                self._w(ansi_cursor_pos(screen_y, content_x + 1))
                self._w(ESC_CLR_EOL)

                if row_idx >= ab.buf.nlines() {
                    self._w(self.hl.col_ruler + "~" + ANSI["RESET"])
                } else {
                    self._paint_line_in_panel(panel, ab, row_idx, content_x, gw, tw, cur_word, focused)
                    ab.buf.line_dirty[row_idx] = false
                }
            }
            i = i + 1
        }
        panel.full_redraw_local = false
    }

    fn _paint_line_in_panel(self, panel, ab, row_idx, content_x, gw, tw, cur_word, focused) {
        // Pintar gutter
        if self.show_lineno {
            let lnum = pad_left(str(row_idx + 1), gw - 1)
            self._w(self.hl.col_ruler + lnum + ANSI["RESET"] + " ")
        }

        let line = ab.buf.line(row_idx)
        let in_state = ab.buf.in_state_at[row_idx]
        let result = self.hl.tokenize(line, in_state)
        let tokens = result["tokens"]

        // Overlays (selecciones, multicursor, busqueda, word match, overflow)
        let overlays = []
        for cur in ab.cursors {
            if self._cursor_has_sel(cur) {
                let sel_range = self._cursor_sel_range_in_line(cur, row_idx)
                if sel_range != null {
                    append(overlays, { "start": sel_range["start"], "end": sel_range["end"], "color": ANSI["BG_BLUE"] + ANSI["WHITE"] })
                }
            }
        }
        let i_cur = 1
        while i_cur < len(ab.cursors) {
            let cur = ab.cursors[i_cur]
            if cur["row"] == row_idx and not self._cursor_has_sel(cur) {
                let pos = cur["col"]
                if pos < len(line) {
                    append(overlays, { "start": pos, "end": pos+1, "color": self.hl.col_cursor_sec })
                }
            }
            i_cur = i_cur + 1
        }
        if ab.last_search != "" {
            let m = self.hl.find_search_matches(line, ab.last_search)
            for x in m { append(overlays, x) }
        }
        if focused and cur_word != "" and len(cur_word) >= 2 {
            let m = self.hl.find_word_matches(tokens, cur_word)
            let p = self._primary()
            for x in m {
                let is_cw = (row_idx == p["row"] and p["col"] >= x["start"] and p["col"] <= x["end"])
                if not is_cw { append(overlays, x) }
            }
        }
        if len(line) > 80 {
            append(overlays, { "start": 80, "end": len(line), "color": self.hl.col_overflow })
        }

        // Render con scroll horizontal y truncado al ancho del panel (tw)
        let painted = ""
        if ab.col_off > 0 and ab.col_off < len(line) {
            let visible = substr(line, ab.col_off, len(line) - ab.col_off)
            let res2 = self.hl.tokenize(visible, in_state)
            let overlays2 = []
            for ov in overlays {
                let s = ov["start"] - ab.col_off
                let e = ov["end"] - ab.col_off
                if e > 0 {
                    if s < 0 { s = 0 }
                    append(overlays2, { "start": s, "end": e, "color": ov["color"] })
                }
            }
            painted = self.hl.render(res2["tokens"], overlays2)
        } else {
            painted = self.hl.render(tokens, overlays)
        }

        // Truncar pintado a tw columnas visibles aproximadas
        // (quick&dirty: confiamos en que no hay caracteres ANSI excesivos)
        self._w(painted)
        self._w(ESC_CLR_EOL)
    }

    fn _cursor_sel_range_in_line(self, cur, row_idx) {
        let r = self._cursor_sel_range(cur)
        if row_idx < r["r1"] or row_idx > r["r2"] { return null }
        let ab = self._ab()
        let ln = ab.buf.line(row_idx)
        let s = 0; let e = len(ln)
        if row_idx == r["r1"] { s = r["c1"] }
        if row_idx == r["r2"] { e = r["c2"] }
        return { "start": s, "end": e }
    }

    // Pinta los divisores entre paneles
    fn _draw_dividers(self) {
        if self.layout_mgr.n_panels_required() == 1 { return }
        let area = self._panels_area()
        let rects = self._panel_rects()
        let dim = ansi_rgb(68, 71, 90)
        let yy = 0
        while yy < area["h"] {
            let xx = 0
            while xx < area["w"] {
                let gx = area["x"] + xx
                let gy = area["y"] + yy
                if not self._is_inside_any_rect(rects, gx, gy) {
                    let is_h = self._is_horizontal_divider(rects, gy)
                    let is_v = self._is_vertical_divider(rects, gx)
                    let ch = " "
                    if is_h and is_v {
                        ch = from_char(226) + from_char(148) + from_char(188)   // ┼
                    } elif is_h {
                        ch = from_char(226) + from_char(148) + from_char(128)   // ─
                    } elif is_v {
                        ch = VBAR                                               // │
                    }
                    self._w(ansi_cursor_pos(gy + 1, gx + 1))
                    self._w(dim + ch + ANSI["RESET"])
                }
                xx = xx + 1
            }
            yy = yy + 1
        }
    }

    fn _is_inside_any_rect(self, rects, gx, gy) {
        for r in rects {
            if gx >= r["x"] and gx < r["x"] + r["w"] and gy >= r["y"] and gy < r["y"] + r["h"] {
                return true
            }
        }
        return false
    }

    fn _is_horizontal_divider(self, rects, gy) {
        let i = 0
        while i < len(rects) {
            let r = rects[i]
            if r["y"] + r["h"] == gy {
                let j = 0
                while j < len(rects) {
                    let r2 = rects[j]
                    if r2["y"] == gy + 1 {
                        let lo = r["x"]
                        if r2["x"] > lo { lo = r2["x"] }
                        let hi = r["x"] + r["w"]
                        let hi2 = r2["x"] + r2["w"]
                        if hi2 < hi { hi = hi2 }
                        if hi > lo { return true }
                    }
                    j = j + 1
                }
            }
            i = i + 1
        }
        return false
    }

    fn _is_vertical_divider(self, rects, gx) {
        let i = 0
        while i < len(rects) {
            let r = rects[i]
            if r["x"] + r["w"] == gx {
                let j = 0
                while j < len(rects) {
                    let r2 = rects[j]
                    if r2["x"] == gx + 1 {
                        let lo = r["y"]
                        if r2["y"] > lo { lo = r2["y"] }
                        let hi = r["y"] + r["h"]
                        let hi2 = r2["y"] + r2["h"]
                        if hi2 < hi { hi = hi2 }
                        if hi > lo { return true }
                    }
                    j = j + 1
                }
            }
            i = i + 1
        }
        return false
    }

    fn _draw_status_bar(self) {
        let y = self.term_h
        self._w(ansi_cursor_pos(y, 1))
        self._w(ANSI["REVERSE"])
        let pos = ""
        let p = self._primary()
        let ab = self._ab()
        if p != null and ab != null {
            pos = "Lin " + str(p["row"]+1) + ", Col " + str(p["col"]+1) + " / " + str(ab.buf.nlines())
        }
        let middle = ""
        let now = time_ms()
        if self.status_msg != "" and now < self.status_until {
            middle = "  " + self.status_msg + "  "
        } else {
            middle = "  F1 Ayuda  ^O Save  ^J Auto  ^F Find  F4/F8 Split  F7 Foco  F9 Layout  ^B Sidebar  ^N Tab  ^W Cerrar  ^Q Salir  "
        }
        let left = " " + pos + " "
        let total = pad_visual(left + middle, self.term_w)
        self._w(total)
        self._w(ANSI["RESET"] + ESC_CLR_EOL)
    }
// COMANDOS DE EDICION (multicursor-aware)
    // -------------------------------------------------------------------------

    fn cmd_insert_char(self, ch) {
        if self._any_selection() { self._delete_all_selections() }
        let order = self._cursors_desc()
        let ab = self._ab()
        let affects = ab.buf._affects_syntax(ch)
        for cur in order {
            ab.buf.insert_char(cur["row"], cur["col"], ch)
            cur["col"] = cur["col"] + 1
            cur["anchor_row"] = cur["row"]
            cur["anchor_col"] = cur["col"]
        }
        if affects { self._recompute_state(self._primary()["row"]) }
        self._coalesce_cursors()
    }

    fn cmd_backspace(self) {
        if self._any_selection() {
            self._delete_all_selections()
            return
        }
        let order = self._cursors_desc()
        let ab = self._ab()
        let any_struct = false
        for cur in order {
            if cur["col"] > 0 {
                let ch = substr(ab.buf.line(cur["row"]), cur["col"]-1, 1)
                ab.buf.delete_char(cur["row"], cur["col"]-1)
                cur["col"] = cur["col"] - 1
                cur["anchor_row"] = cur["row"]; cur["anchor_col"] = cur["col"]
                if ab.buf._affects_syntax(ch) { any_struct = true }
            } else {
                if cur["row"] > 0 {
                    let prev_len = len(ab.buf.line(cur["row"]-1))
                    ab.buf.join_line(cur["row"]-1)
                    cur["row"] = cur["row"] - 1
                    cur["col"] = prev_len
                    cur["anchor_row"] = cur["row"]; cur["anchor_col"] = cur["col"]
                    any_struct = true
                }
            }
        }
        if any_struct { self._recompute_state(0) }
        self._coalesce_cursors()
    }

    fn cmd_delete_forward(self) {
        if self._any_selection() {
            self._delete_all_selections()
            return
        }
        let order = self._cursors_desc()
        let ab = self._ab()
        let any_struct = false
        for cur in order {
            let ln = ab.buf.line(cur["row"])
            if cur["col"] < len(ln) {
                let ch = substr(ln, cur["col"], 1)
                ab.buf.delete_char(cur["row"], cur["col"])
                if ab.buf._affects_syntax(ch) { any_struct = true }
            } else {
                if cur["row"]+1 < ab.buf.nlines() {
                    ab.buf.join_line(cur["row"])
                    any_struct = true
                }
            }
        }
        if any_struct { self._recompute_state(0) }
        self._coalesce_cursors()
    }

    fn cmd_enter(self) {
        if self._any_selection() { self._delete_all_selections() }
        let order = self._cursors_desc()
        let ab = self._ab()
        for cur in order {
            let ln = ab.buf.line(cur["row"])
            let indent = ""
            let i = 0
            while i < len(ln) {
                let c = substr(ln, i, 1)
                if c == " " or c == "\t" {
                    indent = indent + c
                    i = i + 1
                } else {
                    break
                }
            }
            if cur["col"] <= len(indent) { indent = "" }

            ab.buf.split_line(cur["row"], cur["col"])
            cur["row"] = cur["row"] + 1
            cur["col"] = 0
            let j = 0
            while j < len(indent) {
                ab.buf.insert_char(cur["row"], cur["col"], substr(indent, j, 1))
                cur["col"] = cur["col"] + 1
                j = j + 1
            }
            cur["anchor_row"] = cur["row"]; cur["anchor_col"] = cur["col"]
        }
        self._recompute_state(0)
        self._coalesce_cursors()
        self.full_redraw = true
    }

    fn cmd_tab(self) {
        let i = 0
        while i < 4 {
            self.cmd_insert_char(" ")
            i = i + 1
        }
    }

    fn _delete_all_selections(self) {
        let order = self._cursors_desc()
        let killed_parts = []
        for cur in order {
            if not self._cursor_has_sel(cur) { continue }
            let r = self._cursor_sel_range(cur)
            let txt = self._extract_range(r["r1"], r["c1"], r["r2"], r["c2"])
            append(killed_parts, txt)
            self._delete_range(r["r1"], r["c1"], r["r2"], r["c2"])
            cur["row"] = r["r1"]; cur["col"] = r["c1"]
            cur["anchor_row"] = cur["row"]; cur["anchor_col"] = cur["col"]
        }
        if len(killed_parts) > 0 {
            self.kill_buffer = join(killed_parts, "\n")
        }
        self._recompute_state(0)
        self._coalesce_cursors()
        self.full_redraw = true
    }

    fn _delete_range(self, r1, c1, r2, c2) {
        let ab = self._ab()
        if r1 == r2 {
            let ln = ab.buf.line(r1)
            let new_ln = substr(ln, 0, c1) + substr(ln, c2, len(ln)-c2)
            ab.buf.lines[r1] = new_ln
            ab.buf._push_undo({ "op": "replace_line", "row": r1, "old": ln, "new": new_ln })
            ab.buf.mark_dirty(r1)
            return
        }
        let head = substr(ab.buf.line(r1), 0, c1)
        let tail_line = ab.buf.line(r2)
        let tail = substr(tail_line, c2, len(tail_line) - c2)
        let k = r2
        while k > r1 {
            ab.buf.delete_line(k)
            k = k - 1
        }
        let old = ab.buf.lines[r1]
        ab.buf.lines[r1] = head + tail
        ab.buf._push_undo({ "op": "replace_line", "row": r1, "old": old, "new": head + tail })
        ab.buf.mark_dirty(r1)
    }

    fn _extract_range(self, r1, c1, r2, c2) {
        let ab = self._ab()
        if r1 == r2 {
            let ln = ab.buf.line(r1)
            return substr(ln, c1, c2 - c1)
        }
        let parts = []
        let first = ab.buf.line(r1)
        append(parts, substr(first, c1, len(first) - c1))
        let i = r1 + 1
        while i < r2 {
            append(parts, ab.buf.line(i))
            i = i + 1
        }
        let last = ab.buf.line(r2)
        append(parts, substr(last, 0, c2))
        return join(parts, "\n")
    }

    // -------------------------------------------------------------------------
    // MOVIMIENTO
    // -------------------------------------------------------------------------

    fn cmd_move(self, drow, dcol, with_sel) {
        let ab = self._ab()
        for cur in ab.cursors {
            let new_row = cur["row"] + drow
            let new_col = cur["col"] + dcol
            if new_row < 0 { new_row = 0 }
            if new_row >= ab.buf.nlines() { new_row = ab.buf.nlines() - 1 }
            let ln_len = len(ab.buf.line(new_row))
            if new_col < 0 {
                if new_row > 0 {
                    new_row = new_row - 1
                    new_col = len(ab.buf.line(new_row))
                } else {
                    new_col = 0
                }
            }
            if new_col > ln_len {
                if drow == 0 and new_row+1 < ab.buf.nlines() {
                    new_row = new_row + 1
                    new_col = 0
                } else {
                    new_col = ln_len
                }
            }
            cur["row"] = new_row
            cur["col"] = new_col
            if not with_sel {
                cur["anchor_row"] = new_row
                cur["anchor_col"] = new_col
            }
        }
        if with_sel { self.full_redraw = true }
        self._scroll()
    }

    fn cmd_home(self, with_sel) {
        let ab = self._ab()
        for cur in ab.cursors {
            cur["col"] = 0
            if not with_sel {
                cur["anchor_row"] = cur["row"]
                cur["anchor_col"] = 0
            }
        }
        if with_sel { self.full_redraw = true }
        self._scroll()
    }

    fn cmd_end(self, with_sel) {
        let ab = self._ab()
        for cur in ab.cursors {
            let ln_len = len(ab.buf.line(cur["row"]))
            cur["col"] = ln_len
            if not with_sel {
                cur["anchor_row"] = cur["row"]
                cur["anchor_col"] = ln_len
            }
        }
        if with_sel { self.full_redraw = true }
        self._scroll()
    }

    fn cmd_pgup(self) {
        let h = self._active_panel_content_h()
        self.cmd_move(-h, 0, false)
    }
    fn cmd_pgdn(self) {
        let h = self._active_panel_content_h()
        self.cmd_move(h, 0, false)
    }

    fn cmd_word_left(self) {
        let ab = self._ab()
        for cur in ab.cursors {
            let ln = ab.buf.line(cur["row"])
            let c = cur["col"]
            while c > 0 and not self.hl._is_alnum(substr(ln, c-1, 1)) { c = c - 1 }
            while c > 0 and self.hl._is_alnum(substr(ln, c-1, 1)) { c = c - 1 }
            if c == cur["col"] and cur["row"] > 0 {
                cur["row"] = cur["row"] - 1
                cur["col"] = len(ab.buf.line(cur["row"]))
            } else {
                cur["col"] = c
            }
            cur["anchor_row"] = cur["row"]; cur["anchor_col"] = cur["col"]
        }
        self._scroll()
    }

    fn cmd_word_right(self) {
        let ab = self._ab()
        for cur in ab.cursors {
            let ln = ab.buf.line(cur["row"])
            let n = len(ln)
            let c = cur["col"]
            while c < n and self.hl._is_alnum(substr(ln, c, 1)) { c = c + 1 }
            while c < n and not self.hl._is_alnum(substr(ln, c, 1)) { c = c + 1 }
            if c == cur["col"] and cur["row"]+1 < ab.buf.nlines() {
                cur["row"] = cur["row"] + 1
                cur["col"] = 0
            } else {
                cur["col"] = c
            }
            cur["anchor_row"] = cur["row"]; cur["anchor_col"] = cur["col"]
        }
        self._scroll()
    }

    // -------------------------------------------------------------------------
    // MULTICURSOR
    // -------------------------------------------------------------------------

    fn cmd_collapse_cursors(self) {
        let ab = self._ab()
        if not self._has_multi() { return }
        let p = self._primary()
        ab.cursors = [self._make_cursor(p["row"], p["col"], true)]
        self.full_redraw = true
    }

    fn cmd_add_cursor_above(self) {
        let ab = self._ab()
        let last = ab.cursors[len(ab.cursors)-1]
        let new_row = last["row"] - 1
        if new_row < 0 {
            self.set_status("Tope superior alcanzado")
            return
        }
        let ln_len = len(ab.buf.line(new_row))
        let new_col = last["col"]
        if new_col > ln_len { new_col = ln_len }
        append(ab.cursors, self._make_cursor(new_row, new_col, false))
        self._coalesce_cursors()
        self.full_redraw = true
    }

    fn cmd_add_cursor_below(self) {
        let ab = self._ab()
        let last = ab.cursors[len(ab.cursors)-1]
        let new_row = last["row"] + 1
        if new_row >= ab.buf.nlines() {
            self.set_status("Tope inferior alcanzado")
            return
        }
        let ln_len = len(ab.buf.line(new_row))
        let new_col = last["col"]
        if new_col > ln_len { new_col = ln_len }
        append(ab.cursors, self._make_cursor(new_row, new_col, false))
        self._coalesce_cursors()
        self.full_redraw = true
    }

    // -------------------------------------------------------------------------
    // COPY / PASTE / KILL
    // -------------------------------------------------------------------------

    fn cmd_copy_selection(self) {
        if not self._any_selection() {
            self.set_status("No hay seleccion")
            return
        }
        let order = self._cursors_desc()
        let parts = []
        for cur in order {
            if self._cursor_has_sel(cur) {
                let r = self._cursor_sel_range(cur)
                append(parts, self._extract_range(r["r1"], r["c1"], r["r2"], r["c2"]))
            }
        }
        self.kill_buffer = join(parts, "\n")
        self.set_status("Copiado " + str(len(self.kill_buffer)) + " chars")
    }

    fn cmd_paste(self) {
        if self.kill_buffer == "" {
            self.set_status("Buffer vacio")
            return
        }
        if self._any_selection() { self._delete_all_selections() }
        let order = self._cursors_desc()
        for cur in order {
            self._insert_text_at_cursor(cur, self.kill_buffer)
        }
        self.full_redraw = true
        self._recompute_state(0)
        self._coalesce_cursors()
        self._scroll()
    }

    fn _insert_text_at_cursor(self, cur, text) {
        let ab = self._ab()
        let parts = split(text, "\n")
        let i = 0
        while i < len(parts) {
            let p = parts[i]
            let j = 0
            while j < len(p) {
                ab.buf.insert_char(cur["row"], cur["col"], substr(p, j, 1))
                cur["col"] = cur["col"] + 1
                j = j + 1
            }
            if i+1 < len(parts) {
                ab.buf.split_line(cur["row"], cur["col"])
                cur["row"] = cur["row"] + 1
                cur["col"] = 0
            }
            i = i + 1
        }
        cur["anchor_row"] = cur["row"]; cur["anchor_col"] = cur["col"]
    }

    fn cmd_kill_line(self) {
        if self._has_multi() {
            self.set_status("Ctrl+K solo con un cursor")
            return
        }
        let p = self._primary()
        let ab = self._ab()
        let ln = ab.buf.line(p["row"])
        if p["col"] == len(ln) {
            self.kill_buffer = ln + "\n"
            ab.buf.delete_line(p["row"])
            if p["row"] >= ab.buf.nlines() { p["row"] = ab.buf.nlines() - 1 }
            p["col"] = 0
        } else {
            self.kill_buffer = substr(ln, p["col"], len(ln) - p["col"])
            let cnt = len(ln) - p["col"]
            let i = 0
            while i < cnt {
                ab.buf.delete_char(p["row"], p["col"])
                i = i + 1
            }
        }
        p["anchor_row"] = p["row"]; p["anchor_col"] = p["col"]
        self._recompute_state(0)
        self.full_redraw = true
    }

    // -------------------------------------------------------------------------
    // BUSCAR / IR A LINEA
    // -------------------------------------------------------------------------

    fn cmd_find(self, inp) {
        let ab = self._ab()
        let needle = self._prompt(inp, "Buscar: ", ab.last_search)
        if needle == null { return }
        if needle == "" {
            ab.last_search = ""
            self.full_redraw = true
            return
        }
        ab.last_search = needle
        self.cmd_find_next()
    }

    fn cmd_find_next(self) {
        let ab = self._ab()
        if ab.last_search == "" {
            self.set_status("Nada que buscar")
            return
        }
        let p = self._primary()
        let r = p["row"]; let c = p["col"] + 1
        let n = ab.buf.nlines()
        let i = 0
        while i < n {
            let ln = ab.buf.line(r)
            let pos = find_str(ln, ab.last_search, c)
            if pos != -1 {
                p["row"] = r; p["col"] = pos
                p["anchor_row"] = r; p["anchor_col"] = pos
                self._scroll()
                self.full_redraw = true
                self.set_status("Encontrado en " + str(r+1) + ":" + str(pos+1))
                return
            }
            r = r + 1
            if r >= n { r = 0 }
            c = 0
            i = i + 1
        }
        self.set_status("No encontrado: " + ab.last_search)
    }

    fn cmd_goto(self, inp) {
        let s = self._prompt(inp, "Ir a linea: ", "")
        if s == null or s == "" { return }
        if not is_numeric(s) {
            self.set_status("Numero invalido")
            return
        }
        let ab = self._ab()
        let n = int(s) - 1
        if n < 0 { n = 0 }
        if n >= ab.buf.nlines() { n = ab.buf.nlines() - 1 }
        let p = self._primary()
        p["row"] = n; p["col"] = 0
        p["anchor_row"] = n; p["anchor_col"] = 0
        self._scroll()
        self.full_redraw = true
    }

    // -------------------------------------------------------------------------
    // GUARDAR
    // -------------------------------------------------------------------------

    fn cmd_save(self) { return self._do_save(false) }
    fn cmd_save_as(self) { return self._do_save(true) }

    fn _do_save(self, force_prompt) {
        let ab = self._ab()
        if ab.filename == "" or force_prompt {
            if self.input_backend == null {
                self.set_status("Sin input backend")
                return false
            }
            let new_name = self._prompt(self.input_backend, "Guardar como: ", ab.filename)
            if new_name == null { self.set_status("Cancelado"); return false }
            if new_name == "" { self.set_status("Nombre vacio"); return false }
            ab.filename = new_name
        }
        try {
            write_file(ab.filename, ab.buf.to_text())
            ab.buf.modified = false
            self.set_status("Guardado: " + ab.filename)
            // Refrescar sidebar por si el fichero es nuevo
            if self.tree != null { self.tree.refresh_root() }
            return true
        } catch e {
            self.set_status("Error: " + str(e))
            return false
        }
    }

    // -------------------------------------------------------------------------
    // EJECUTAR SCRIPT
    // -------------------------------------------------------------------------

    fn cmd_run_script(self, inp) {
        if not self._do_save(false) {
            self.set_status("Guarda antes de ejecutar")
            return
        }
        let ab = self._ab()
        print(ANSI["CLEAR"])
        print(ansi_cursor_pos(1, 1))
        print(ESC_CUR_SHOW)
        print("Ejecutando: vesta --script " + ab.filename + "\n")
        print(self.hl.col_ruler + repeat("-", 60) + ANSI["RESET"] + "\n")
        let r = shell_ex("vesta --script " + ab.filename)
        print(r["output"])
        if not ends_with(r["output"], "\n") { print("\n") }
        print(self.hl.col_ruler + repeat("-", 60) + ANSI["RESET"] + "\n")
        let rc_color = ANSI["GREEN"]
        if r["code"] != 0 { rc_color = ANSI["RED"] }
        print(rc_color + "Codigo: " + str(r["code"]) + ANSI["RESET"])
        print("  Pulsa cualquier tecla para volver...")
        inp.read_key_blocking()
        print(ANSI["CLEAR"])
        print(ansi_cursor_pos(1, 1))
        self._clamp_all_cursors()
        self.full_redraw = true
        let i = 0
        while i < self._ab().buf.nlines() {
            self._ab().buf.line_dirty[i] = true
            i = i + 1
        }
    }

    // -------------------------------------------------------------------------
    // SHELL
    // -------------------------------------------------------------------------

    fn cmd_shell(self, inp) {
        let cmd = self._prompt(inp, "Comando: ", "")
        if cmd == null or cmd == "" {
            self.full_redraw = true
            return
        }
        print(ANSI["CLEAR"])
        print(ansi_cursor_pos(1, 1))
        print(ESC_CUR_SHOW)
        print(self.hl.col_ruler + "$ " + ANSI["RESET"] + cmd + "\n")
        print(self.hl.col_ruler + repeat("-", 60) + ANSI["RESET"] + "\n")
        let r = shell_ex(cmd)
        print(r["output"])
        if not ends_with(r["output"], "\n") { print("\n") }
        print(self.hl.col_ruler + repeat("-", 60) + ANSI["RESET"] + "\n")
        let rc_color = ANSI["GREEN"]
        if r["code"] != 0 { rc_color = ANSI["RED"] }
        print(rc_color + "Codigo: " + str(r["code"]) + ANSI["RESET"])
        print("  Pulsa cualquier tecla para volver...")
        inp.read_key_blocking()
        print(ANSI["CLEAR"])
        print(ansi_cursor_pos(1, 1))
        self._clamp_all_cursors()
        self.full_redraw = true
        let i = 0
        while i < self._ab().buf.nlines() {
            self._ab().buf.line_dirty[i] = true
            i = i + 1
        }
    }

    // -------------------------------------------------------------------------
    // PROMPT GENERICO en la status bar
    // -------------------------------------------------------------------------

    fn _prompt(self, inp, prompt, def) {
        let ans = def
        let cancelled = false
        while true {
            print(ansi_cursor_pos(self.term_h, 1))
            print(ANSI["REVERSE"])
            let line = " " + prompt + ans + " "
            line = pad_visual(line, self.term_w)
            print(line)
            print(ANSI["RESET"])
            print(ansi_cursor_pos(self.term_h, 2 + len(prompt) + len(ans)))
            print(ESC_CUR_SHOW)

            let k = inp.read_key_blocking()
            if k == KEY_ESC {
                cancelled = true
                break
            }
            if k == KEY_ENTER { break }
            if k == KEY_BACKSPACE or k == KEY_DELETE {
                if len(ans) > 0 { ans = substr(ans, 0, len(ans) - 1) }
                continue
            }
            if k >= 32 and k < 127 {
                ans = ans + from_char(k)
                continue
            }
        }
        self.full_redraw = true
        if cancelled { return null }
        return ans
    }
    // -------------------------------------------------------------------------
    // AUTOCOMPLETADO (estilo VS Code, popup flotante)
    // -------------------------------------------------------------------------

    fn _word_prefix_at_cursor(self) {
        let p = self._primary()
        let ab = self._ab()
        if p == null or ab == null {
            return { "prefix": "", "start": 0, "end": 0 }
        }
        let ln = ab.buf.line(p["row"])
        let end = p["col"]
        let start = end
        while start > 0 {
            let c = substr(ln, start-1, 1)
            if not self.hl._is_alnum(c) { break }
            start = start - 1
        }
        let prefix = substr(ln, start, end - start)
        return { "prefix": prefix, "start": start, "end": end }
    }

    fn _build_word_corpus(self, prefix) {
        let words = []
        let seen = {}
        let plower = lower(prefix)
        for w in VSH_KEYWORDS {
            if starts_with(lower(w), plower) and len(w) > len(prefix) {
                if not contains(seen, w) {
                    append(words, { "text": w, "kind": "kw" })
                    seen[w] = true
                }
            }
        }
        for w in VSH_TYPES {
            if starts_with(lower(w), plower) and len(w) > len(prefix) {
                if not contains(seen, w) {
                    append(words, { "text": w, "kind": "type" })
                    seen[w] = true
                }
            }
        }
        for w in VSH_BUILTINS {
            if starts_with(lower(w), plower) and len(w) > len(prefix) {
                if not contains(seen, w) {
                    append(words, { "text": w, "kind": "builtin" })
                    seen[w] = true
                }
            }
        }
        let ab = self._ab()
        if ab != null {
            let r = 0
            while r < ab.buf.nlines() {
                let res = self.hl.tokenize(ab.buf.lines[r], ab.buf.in_state_at[r])
                for tok in res["tokens"] {
                    let k = tok["kind"]
                    if k == "id" or k == "func" {
                        let w = tok["text"]
                        if starts_with(lower(w), plower) and len(w) > len(prefix) {
                            if not contains(seen, w) {
                                append(words, { "text": w, "kind": k })
                                seen[w] = true
                            }
                        }
                    }
                }
                r = r + 1
            }
        }
        return words
    }

    // Calcula posicion 1-based del popup, justo debajo del prefijo
    fn _popup_position(self, prefix_start_col) {
        let p = self._primary()
        let ab = self._ab()
        let rects = self._panel_rects()
        if self.active_panel_idx >= len(rects) {
            return { "x": 1, "y": 1 }
        }
        let rect = rects[self.active_panel_idx]
        let ap = self._ap()
        let cy = self._panel_content_y(ap, rect)
        let gw = self._gutter_width_for(ap)
        let x = prefix_start_col - ab.col_off + rect["x"] + gw + 1
        let y = p["row"] - ab.row_off + cy + 2   // +2: 1 por 1-based, 1 para ir debajo
        return { "x": x, "y": y }
    }

    // Abre o actualiza el popup. Si el prefijo es vacio, lo cierra.
    fn _popup_refresh(self) {
        let info = self._word_prefix_at_cursor()
        let prefix = info["prefix"]
        if len(prefix) == 0 {
            self.popup = null
            return
        }
        let candidates = self._build_word_corpus(prefix)
        if len(candidates) == 0 {
            self.popup = null
            return
        }
        let pos = self._popup_position(info["start"])
        self.popup = AutocompletePopup(candidates, pos["x"], pos["y"], prefix, self.config.use_unicode)
    }

    // Trigger automatico tras escribir un caracter alfanumerico
    fn _maybe_trigger_autocomplete(self) {
        let info = self._word_prefix_at_cursor()
        let prefix = info["prefix"]
        if len(prefix) >= 2 {
            self._popup_refresh()
        } else {
            self.popup = null
        }
    }

    // Apertura manual con Ctrl+J
    fn cmd_autocomplete_manual(self, inp) {
        let info = self._word_prefix_at_cursor()
        if len(info["prefix"]) == 0 {
            self.set_status("Escribe al menos 1 caracter antes de Ctrl+J")
            return
        }
        self._popup_refresh()
        if self.popup == null {
            self.set_status("Sin sugerencias para '" + info["prefix"] + "'")
        }
    }

    // Aplicar el candidato seleccionado del popup
    fn _popup_accept(self) {
        if self.popup == null { return }
        let txt = self.popup.sel_text()
        if txt == null { return }
        let info = self._word_prefix_at_cursor()
        let p = self._primary()
        let ab = self._ab()
        let n_to_del = info["end"] - info["start"]
        let i = 0
        while i < n_to_del {
            ab.buf.delete_char(p["row"], info["start"])
            i = i + 1
        }
        p["col"] = info["start"]
        let j = 0
        while j < len(txt) {
            ab.buf.insert_char(p["row"], p["col"], substr(txt, j, 1))
            p["col"] = p["col"] + 1
            j = j + 1
        }
        p["anchor_row"] = p["row"]; p["anchor_col"] = p["col"]
        self._recompute_state(p["row"])
        self._scroll()
        self.popup = null
        self.full_redraw = true
    }

    fn _popup_cancel(self) {
        self.popup = null
        self.full_redraw = true
    }

    // Helper para que cmd_pgup/pgdn sepan el alto del panel
    fn _active_panel_content_h(self) {
        let rects = self._panel_rects()
        if self.active_panel_idx >= len(rects) { return self.term_h - 4 }
        let rect = rects[self.active_panel_idx]
        let p = self._ap()
        return self._panel_content_h(p, rect)
    }

    // -------------------------------------------------------------------------
    // MANEJO DEL SIDEBAR (cuando tiene foco)
    // -------------------------------------------------------------------------

    fn _sidebar_node_at_sel(self) {
        if self.tree == null { return null }
        let visible = self.tree.list_visible()
        if self.sidebar_sel < 0 or self.sidebar_sel >= len(visible) { return null }
        return visible[self.sidebar_sel]
    }

    fn _handle_sidebar_key(self, k, inp) {
        if self.tree == null { return false }
        let visible = self.tree.list_visible()

        if k == KEY_UP {
            if self.sidebar_sel > 0 { self.sidebar_sel = self.sidebar_sel - 1 }
            self.full_redraw = true
            return true
        }
        if k == KEY_DOWN {
            if self.sidebar_sel < len(visible) - 1 { self.sidebar_sel = self.sidebar_sel + 1 }
            self.full_redraw = true
            return true
        }
        if k == KEY_PGUP {
            self.sidebar_sel = self.sidebar_sel - 10
            if self.sidebar_sel < 0 { self.sidebar_sel = 0 }
            self.full_redraw = true
            return true
        }
        if k == KEY_PGDN {
            self.sidebar_sel = self.sidebar_sel + 10
            if self.sidebar_sel >= len(visible) { self.sidebar_sel = len(visible) - 1 }
            self.full_redraw = true
            return true
        }
        if k == KEY_HOME {
            self.sidebar_sel = 0
            self.full_redraw = true
            return true
        }
        if k == KEY_END {
            self.sidebar_sel = len(visible) - 1
            self.full_redraw = true
            return true
        }
        if k == KEY_RIGHT or k == KEY_ENTER {
            let node = self._sidebar_node_at_sel()
            if node == null { return true }
            if node["is_dir"] {
                if not node["expanded"] {
                    self.tree.expand(node)
                } else {
                    if k == KEY_ENTER { self.tree.collapse(node) }
                }
            } else {
                // Abrir fichero en pestana nueva (o conmutar si existe)
                self.open_file(node["path"])
                self.focus_editor()
            }
            self.full_redraw = true
            return true
        }
        if k == KEY_LEFT {
            let node = self._sidebar_node_at_sel()
            if node == null { return true }
            if node["is_dir"] and node["expanded"] {
                self.tree.collapse(node)
                self.full_redraw = true
            }
            return true
        }
        // . toggle ocultos
        if k == 46 {
            self.config.show_hidden = not self.config.show_hidden
            self.tree.refresh_root()
            self.sidebar_sel = 0
            self.full_redraw = true
            return true
        }
        // u toggle Unicode
        if k == 117 {
            self.config.use_unicode = not self.config.use_unicode
            self.full_redraw = true
            return true
        }
        return false
    }

    // -------------------------------------------------------------------------
    // BUCLE PRINCIPAL
    // -------------------------------------------------------------------------

    fn run(self, inp) {
        self.input_backend = inp
        print(ANSI["CLEAR"])
        self.full_redraw = true
        self.render()

        while true {
            let k = inp.read_key_blocking()

            // ---- Si hay POPUP abierto, primero el popup ----
            if self.popup != null {
                if k == KEY_ESC {
                    self._popup_cancel()
                    self.render(); continue
                }
                if k == KEY_ENTER or k == KEY_TAB {
                    self._popup_accept()
                    self.render(); continue
                }
                if k == KEY_UP { self.popup.move_up(); self.render(); continue }
                if k == KEY_DOWN { self.popup.move_down(); self.render(); continue }
                if k == KEY_BACKSPACE or k == KEY_DELETE {
                    self.cmd_backspace()
                    // Recompute popup
                    self._maybe_trigger_autocomplete()
                    self._scroll()
                    self.render()
                    continue
                }
                if k >= 32 and k < 127 {
                    let ch = from_char(k)
                    if self.hl._is_alnum(ch) {
                        self.cmd_insert_char(ch)
                        self._popup_refresh()
                        self._scroll()
                        self.render()
                        continue
                    } else {
                        // Caracter no alfanumerico: cerrar popup y procesar la tecla
                        self._popup_cancel()
                    }
                } else {
                    // Tecla especial no manejada por popup: cerrar y dejar caer
                    self._popup_cancel()
                }
                // Si llegamos aqui, popup se cerro y queremos procesar la tecla
                // como entrada normal. Continua el flujo normal abajo.
            }

            // ---- Globales ----

            if k == KEY_CTRL_Q {
                if self._any_modified() {
                    let ans = self._prompt(inp, "Hay pestanas sin guardar. Salir? (y/N): ", "")
                    if ans == null { continue }
                    if lower(ans) != "y" { continue }
                }
                break
            }

            if k == KEY_F1 {
                let h = HelpOverlay()
                h.show(inp, self.term_w, self.term_h)
                self.full_redraw = true
                self.render()
                continue
            }

            if k == KEY_CTRL_B {
                self.toggle_sidebar()
                self.render(); continue
            }

            if k == KEY_F6 {
                self.rotate_focus()
                self.render(); continue
            }

            if k == KEY_F2 {
                self.focus_sidebar()
                self.render(); continue
            }

            // Splits y paneles
            if k == KEY_F4 {
                self.split_active_panel("v")
                self.render(); continue
            }
            if k == KEY_F8 {
                self.split_active_panel("h")
                self.render(); continue
            }
            if k == KEY_F7 {
                self.rotate_panel_focus()
                self.render(); continue
            }
            if k == KEY_F9 {
                self.cycle_layout()
                self.render(); continue
            }

            if k == KEY_CTRL_N {
                self.new_tab()
                self.focus = "editor"
                self.render(); continue
            }
            if k == KEY_CTRL_PGUP {
                self.prev_tab()
                self.render(); continue
            }
            if k == KEY_CTRL_PGDN {
                self.next_tab()
                self.render(); continue
            }

            if k == KEY_CTRL_W {
                self.close_tab(inp)
                self.render(); continue
            }

            // ---- Foco en sidebar ----
            if self.focus == "sidebar" {
                if k == KEY_ESC {
                    self.focus_editor()
                    self.render(); continue
                }
                let handled = self._handle_sidebar_key(k, inp)
                if handled {
                    self.render(); continue
                }
                continue
            }

            // ---- Foco en EDITOR ----

            if k == KEY_ESC {
                if self._has_multi() {
                    self.cmd_collapse_cursors()
                } elif self._any_selection() {
                    let ab = self._ab()
                    for cur in ab.cursors {
                        cur["anchor_row"] = cur["row"]
                        cur["anchor_col"] = cur["col"]
                    }
                    self.full_redraw = true
                }
                self.render(); continue
            }

            // Ctrl+S y Ctrl+O: ambos guardan
            if k == KEY_CTRL_S { self.cmd_save()
            } elif k == KEY_CTRL_O { self.cmd_save()
            } elif k == KEY_CTRL_T { self.cmd_shell(inp)
            } elif k == KEY_CTRL_J { self.cmd_autocomplete_manual(inp)
            } elif k == KEY_CTRL_E { self.cmd_run_script(inp)
            } elif k == KEY_CTRL_F { self.cmd_find(inp)
            } elif k == KEY_F3 { self.cmd_find_next()
            } elif k == KEY_CTRL_G { self.cmd_goto(inp)
            } elif k == KEY_CTRL_Z {
                let ab = self._ab()
                if ab.buf.undo() == null {
                    self.set_status("Nada que deshacer")
                } else {
                    self.full_redraw = true
                    self._recompute_state(0)
                }
            } elif k == KEY_CTRL_Y or k == KEY_CTRL_R {
                let ab = self._ab()
                if ab.buf.redo() == null {
                    self.set_status("Nada que rehacer")
                } else {
                    self.full_redraw = true
                    self._recompute_state(0)
                }
            } elif k == KEY_CTRL_K { self.cmd_kill_line()
            } elif k == KEY_CTRL_U { self.cmd_paste()
            } elif k == KEY_CTRL_C { self.cmd_copy_selection()
            } elif k == KEY_CTRL_V { self.cmd_paste()
            } elif k == KEY_CTRL_L {
                self.show_lineno = not self.show_lineno
                self.full_redraw = true
            } elif k == KEY_CTRL_UP { self.cmd_add_cursor_above()
            } elif k == KEY_CTRL_DOWN { self.cmd_add_cursor_below()
            } elif k == KEY_UP { self.cmd_move(-1, 0, false)
            } elif k == KEY_DOWN { self.cmd_move(1, 0, false)
            } elif k == KEY_LEFT { self.cmd_move(0, -1, false)
            } elif k == KEY_RIGHT { self.cmd_move(0, 1, false)
            } elif k == KEY_HOME { self.cmd_home(false)
            } elif k == KEY_END { self.cmd_end(false)
            } elif k == KEY_PGUP { self.cmd_pgup()
            } elif k == KEY_PGDN { self.cmd_pgdn()
            } elif k == KEY_CTRL_LEFT { self.cmd_word_left()
            } elif k == KEY_CTRL_RIGHT { self.cmd_word_right()
            } elif k == KEY_SHIFT_UP { self.cmd_move(-1, 0, true)
            } elif k == KEY_SHIFT_DOWN { self.cmd_move(1, 0, true)
            } elif k == KEY_SHIFT_LEFT { self.cmd_move(0, -1, true)
            } elif k == KEY_SHIFT_RIGHT { self.cmd_move(0, 1, true)
            } elif k == KEY_SHIFT_HOME { self.cmd_home(true)
            } elif k == KEY_SHIFT_END { self.cmd_end(true)
            } elif k == KEY_ENTER { self.cmd_enter()
            } elif k == KEY_TAB { self.cmd_tab()
            } elif k == KEY_BACKSPACE or k == KEY_DELETE { self.cmd_backspace()
            } elif k == KEY_DEL { self.cmd_delete_forward()
            } elif k >= 32 and k < 127 {
                let ch = from_char(k)
                self.cmd_insert_char(ch)
                // Auto-trigger autocompletado si tras este char tenemos prefijo >= 2
                if self.hl._is_alnum(ch) {
                    self._maybe_trigger_autocomplete()
                }
            }

            self._scroll()
            self.render()
        }

        print(ANSI["CLEAR"] + ESC_CUR_SHOW + ANSI["RESET"])
    }

}

// =============================================================================
// SECCION 13: main()
// =============================================================================

fn main() {
    let inp = InputBackend()
    let cfg = Config()

    try {
        let launcher = Launcher(getcwd(), cfg)
        let result = launcher.run(inp)

        if result["mode"] == "cancel" {
            inp.shutdown()
            print(ANSI["CLEAR"] + ESC_CUR_SHOW + ANSI["RESET"])
            return
        }

        let editor = Editor(cfg)

        if result["mode"] == "project" {
            editor.set_project(result["path"])
            editor.new_tab()
        } elif result["mode"] == "file" {
            editor.open_file(result["path"])
        } elif result["mode"] == "new" {
            editor._ensure_initial_panel()
            let buf = EditorBuffer(result["path"])
            editor._ap().add_buffer(buf)
            editor.full_redraw = true
        }

        editor.run(inp)
    } catch e {
        inp.shutdown()
        print(ANSI["CLEAR"] + ESC_CUR_SHOW + ANSI["RESET"])
        println("=== ERROR DETALLADO ===")
        println(str(e))
        println("=======================")
        return
    }

    inp.shutdown()
}

main()
