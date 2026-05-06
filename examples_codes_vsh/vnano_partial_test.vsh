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
let KEY_F6          = -1015
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
        if c2 == 64 { return KEY_F6    }
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
                if self._in_list(word, VSH_KEYWORDS) {
                    kind = "kw"
                } elif self._in_list(word, VSH_TYPES) {
                    kind = "type"
                } elif self._in_list(word, VSH_BUILTINS) {
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
// SECCION 12: Editor (workspace con pestanas y sidebar)
// =============================================================================

class Editor {
    "Editor de texto multicursor con sidebar y pestanas."

    fn __init__(self, config) {
        self.config = config
        self.hl = Highlighter()

        // Lista de buffers (pestanas) y activo
        self.buffers = []           // list of EditorBuffer
        self.active_idx = 0

        // Sidebar
        self.sidebar_visible = false
        self.sidebar_root = ""      // path del proyecto
        self.tree = null            // FileTree (null si no hay proyecto)
        self.sidebar_sel = 0        // indice seleccionado en list_visible
        self.sidebar_scroll = 0

        // Foco: "editor" o "sidebar"
        self.focus = "editor"

        // Clipboard global
        self.kill_buffer = ""

        // Backend de input (asignado en run)
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
    }

    // -------------------------------------------------------------------------
    // ACCESO AL BUFFER ACTIVO
    // -------------------------------------------------------------------------

    fn _ab(self) { return self.buffers[self.active_idx] }

    fn _has_active(self) { return len(self.buffers) > 0 }

    // -------------------------------------------------------------------------
    // GESTION DE PESTANAS
    // -------------------------------------------------------------------------

    fn open_file(self, path) {
        // Si ya esta abierto, conmutar a esa pestana
        let i = 0
        while i < len(self.buffers) {
            if self.buffers[i].filename == path {
                self.active_idx = i
                self.full_redraw = true
                self.set_status("Cambiado a pestana: " + basename_or(path, "?"))
                return
            }
            i = i + 1
        }
        // Nueva pestana
        let buf = EditorBuffer(path)
        append(self.buffers, buf)
        self.active_idx = len(self.buffers) - 1
        self.full_redraw = true
        self._recompute_state(0)
        self.set_status("Abierto: " + basename_or(path, "?"))
    }

    fn new_tab(self) {
        let buf = EditorBuffer("")
        append(self.buffers, buf)
        self.active_idx = len(self.buffers) - 1
        self.full_redraw = true
        self.set_status("Nueva pestana")
    }

    fn close_tab(self, inp) {
        if not self._has_active() { return false }
        let ab = self._ab()
        if ab.buf.modified {
            let ans = self._prompt(inp, "Cambios sin guardar en '" + ab.display_name() + "'. Cerrar? (y/N): ", "")
            if ans == null { return false }
            if lower(ans) != "y" { return false }
        }
        // Quitar la pestana
        let new_buffers = []
        let i = 0
        while i < len(self.buffers) {
            if i != self.active_idx { append(new_buffers, self.buffers[i]) }
            i = i + 1
        }
        self.buffers = new_buffers
        if self.active_idx >= len(self.buffers) { self.active_idx = len(self.buffers) - 1 }
        if self.active_idx < 0 { self.active_idx = 0 }
        self.full_redraw = true
        if len(self.buffers) == 0 {
            // Si cerramos la ultima, abrir una vacia para no quedar sin nada
            self.new_tab()
        }
        return true
    }

    fn next_tab(self) {
        if len(self.buffers) <= 1 { return }
        self.active_idx = self.active_idx + 1
        if self.active_idx >= len(self.buffers) { self.active_idx = 0 }
        self.full_redraw = true
    }

    fn prev_tab(self) {
        if len(self.buffers) <= 1 { return }
        self.active_idx = self.active_idx - 1
        if self.active_idx < 0 { self.active_idx = len(self.buffers) - 1 }
        self.full_redraw = true
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
            // Si no hay proyecto, abrir el cwd como proyecto
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
    // HELPERS DE CURSOR (operan sobre el buffer activo)
    // -------------------------------------------------------------------------

    fn _make_cursor(self, row, col, primary) {
        return { "row": row, "col": col, "anchor_row": row, "anchor_col": col, "primary": primary }
    }

    fn _primary(self) { return self._ab().cursors[0] }
    fn _has_multi(self) { return len(self._ab().cursors) > 1 }

    fn _any_selection(self) {
        for cur in self._ab().cursors {
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
        let lst = []
        for c in self._ab().cursors { append(lst, c) }
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
                if cmp {
                    lst[j] = b; lst[j+1] = a
                }
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

    // -------------------------------------------------------------------------
    // GEOMETRIA DE LA UI
    // -------------------------------------------------------------------------

    fn _has_tabbar(self) { return len(self.buffers) > 1 }

    // Filas: header(1) + tabbar(0|1) + texto + statusbar(1)
    fn _editor_top_row(self) {
        let r = 2  // tras header
        if self._has_tabbar() { r = r + 1 }
        return r
    }

    fn _editor_height(self) {
        let used = 2  // header + statusbar
        if self._has_tabbar() { used = used + 1 }
        return self.term_h - used
    }

    fn _sidebar_w(self) {
        if not self.sidebar_visible or self.tree == null { return 0 }
        return self.config.sidebar_width
    }

    // Ancho del area de texto (sin sidebar ni gutter)
    fn _text_area_left(self) { return self._sidebar_w() }   // columna 0-based desde donde empieza el editor
    fn _text_area_width(self) { return self.term_w - self._sidebar_w() }

    fn _gutter_width(self) {
        if not self.show_lineno { return 0 }
        if not self._has_active() { return 0 }
        let n = self._ab().buf.nlines()
        let w = 1
        let m = n
        while m >= 10 { w = w + 1; m = m / 10 }
        return w + 1
    }

    fn _text_width(self) { return self._text_area_width() - self._gutter_width() }

    // -------------------------------------------------------------------------
    // SCROLL
    // -------------------------------------------------------------------------

    fn _scroll(self) {
        self._clamp_all_cursors()
        let h = self._editor_height()
        let w = self._text_width()
        let ab = self._ab()
        let p = self._primary()
        if p["row"] < ab.row_off {
            ab.row_off = p["row"]; self.full_redraw = true
        }
        if p["row"] >= ab.row_off + h {
            ab.row_off = p["row"] - h + 1; self.full_redraw = true
        }
        if p["col"] < ab.col_off {
            ab.col_off = p["col"]; self.full_redraw = true
        }
        if p["col"] >= ab.col_off + w {
            ab.col_off = p["col"] - w + 1; self.full_redraw = true
        }
    }

    fn _word_at_cursor(self) {
        let ab = self._ab()
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
}

// =============================================================================
// MAIN DE PRUEBA INTERMEDIO
// =============================================================================
// Solo comprueba que las clases construidas hasta aqui se cargan sin error.
// El editor real no funcionara aun (faltan render, comandos, etc).

fn main() {
    println("=== Test de carga de clases (vnano v4 partial) ===")
    println("")

    let cfg = Config()
    println("[OK] Config")

    let ts = TermSize()
    println("[OK] TermSize: " + str(ts.cols) + " x " + str(ts.rows))

    let hl = Highlighter()
    println("[OK] Highlighter")

    let tb = TextBuffer()
    println("[OK] TextBuffer")

    let eb = EditorBuffer("")
    println("[OK] EditorBuffer (display name: '" + eb.display_name() + "')")

    let tree = FileTree(getcwd(), cfg)
    let visible = tree.list_visible()
    println("[OK] FileTree (" + str(len(visible)) + " entradas visibles en cwd)")

    let editor = Editor(cfg)
    println("[OK] Editor (workspace)")
    editor.new_tab()
    println("[OK] Editor.new_tab() - buffers: " + str(len(editor.buffers)))
    println("    display_name del activo: '" + editor._ab().display_name() + "'")
    println("    text_area_width: " + str(editor._text_area_width()))
    println("    sidebar_visible: " + str(editor.sidebar_visible))

    println("")
    println("Todas las clases base se construyen sin errores.")
    println("El proximo turno completara render, comandos y main real.")
}

main()
