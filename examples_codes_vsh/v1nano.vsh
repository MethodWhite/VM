// =============================================================================
// VestaShell - vnano: Editor de texto tipo nano con highlighter
// =============================================================================
// VERSION 3 - Cambios sobre v2:
//   - Multicursores tipo VS Code (Ctrl+Up/Down anaden cursor; Esc colapsa)
//   - Optimizaciones de rendimiento (anti-trabamiento):
//       * Marcado dirty solo de la linea editada, no del resto
//       * _recompute_state solo si la edicion afecta caracteres especiales
//       * Pila undo/redo con tope logico en lugar de slicing
//       * Polling de teclado mas agresivo (sleep 4ms en vez de 8ms)
//   - Tema Dracula 2.0 con true color
//   - Highlighter con docstrings """, nombres de funcion, ruler en col 80
//   - Ctrl+Left/Right saltan palabra
//   - Ctrl+E guarda y ejecuta el script
//   - Save As con prompt si el fichero no tiene nombre
// =============================================================================
//
// Atajos completos:
//   Movimiento:
//     Flechas         mover cursor
//     Ctrl+Left/Right saltar palabra
//     Home/End        inicio/fin de linea
//     PgUp/PgDn       pagina arriba/abajo
//   Seleccion:
//     Shift+flechas   extender seleccion
//     Shift+Home/End  extender hasta inicio/fin de linea
//   Multicursor:
//     Ctrl+Up         anadir cursor en linea de arriba
//     Ctrl+Down       anadir cursor en linea de abajo
//     Esc             colapsar a un solo cursor
//   Edicion:
//     Tab             4 espacios (en TODOS los cursores)
//     Enter           nueva linea con auto-indent
//     Backspace/Del   borrar
//     Ctrl+K          cortar linea
//     Ctrl+U          pegar
//     Ctrl+C          copiar seleccion
//     Ctrl+V          pegar
//     Ctrl+Z / Ctrl+Y deshacer / rehacer
//   Acciones:
//     Ctrl+S          guardar (pide nombre si nuevo)
//     Ctrl+E          ejecutar script con vm
//     Ctrl+W          buscar
//     F3              siguiente coincidencia
//     Ctrl+G          ir a linea
//     Ctrl+L          toggle numeros de linea
//     Ctrl+Q          salir
// =============================================================================


// =============================================================================
// SECCION 0: Secuencias ANSI auxiliares
// =============================================================================
// El mapa builtin ANSI no expone HOME, CURSOR_HIDE/SHOW, CLR_EOL/EOS, asi que
// las construimos extrayendo el byte ESC del propio mapa (ANSI["CLEAR_LINE"]
// arranca con \033, asi que su primer caracter es 0x1B).
// Hacemos esto porque el lexer de strings de VestaShell no interpreta "\033"
// como escape octal; lo dejaria como cuatro caracteres literales.

let ESC          = substr(ANSI["CLEAR_LINE"], 0, 1)
let ESC_HOME     = ESC + "[H"        // Cursor a (1,1)
let ESC_CUR_HIDE = ESC + "[?25l"     // Ocultar cursor del terminal
let ESC_CUR_SHOW = ESC + "[?25h"     // Mostrar cursor del terminal
let ESC_CLR_EOL  = ESC + "[K"        // Borrar desde cursor hasta fin de linea
let ESC_CLR_EOS  = ESC + "[J"        // Borrar desde cursor hasta fin de pantalla


// =============================================================================
// SECCION 1: Codigos de tecla
// =============================================================================
// Constantes para no esparcir numeros magicos. Los positivos son ASCII directo;
// los negativos son codigos sinteticos que devuelve poll_key() para teclas
// especiales (flechas, F-keys, Shift+flechas, Ctrl+flechas).

// Teclas Ctrl+letra (ASCII 1..26)
let KEY_CTRL_A    = 1
let KEY_CTRL_B    = 2
let KEY_CTRL_C    = 3
let KEY_CTRL_D    = 4
let KEY_CTRL_E    = 5
let KEY_CTRL_F    = 6
let KEY_CTRL_G    = 7
let KEY_BACKSPACE = 8       // tambien Ctrl+H
let KEY_TAB       = 9       // tambien Ctrl+I
let KEY_ENTER     = 13      // tambien Ctrl+M
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
let KEY_ESC       = 27
let KEY_DELETE    = 127     // Backspace en muchas terminales POSIX

// Codigos sinteticos: por debajo de 0 para no chocar con ASCII real.
// Bloque -1000: flechas y movimiento basico
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
let KEY_F3          = -1012
// Bloque -2000: con Shift (extender seleccion)
let KEY_SHIFT_UP    = -2000
let KEY_SHIFT_DOWN  = -2001
let KEY_SHIFT_LEFT  = -2002
let KEY_SHIFT_RIGHT = -2003
let KEY_SHIFT_HOME  = -2004
let KEY_SHIFT_END   = -2005
// Bloque -3000: con Ctrl
let KEY_CTRL_LEFT   = -3000
let KEY_CTRL_RIGHT  = -3001
let KEY_CTRL_UP     = -3002      // anadir cursor arriba (multicursor)
let KEY_CTRL_DOWN   = -3003      // anadir cursor abajo (multicursor)
let KEY_CTRL_SPACE = 0   // Ctrl+Space en muchas terminales = NUL (codigo 0)

// =============================================================================
// SECCION 2: InputBackend - lectura no bloqueante via FFI
// =============================================================================
// Encapsula la diferencia entre Windows (msvcrt _kbhit/_getch) y POSIX
// (libc.getchar tras stty raw). Devuelve codigos sinteticos para teclas
// especiales para que el editor no tenga que conocer las secuencias ESC[
// ni los prefijos 0/224 de Windows.

class InputBackend {
    "Backend de entrada via FFI."

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
            // msvcrt expone _kbhit (no bloqueante) y _getch (lee sin echo)
            self.lib = ffi_open("msvcrt.dll")
            self.kbhit_sym = ffi_sym(self.lib, "_kbhit")
            self.getch_sym = ffi_sym(self.lib, "_getch")
        } else {
            // En POSIX cargamos libc y ponemos el terminal en modo raw.
            // 'min 0 time 0' hace que getchar() retorne -1 (EOF) si no
            // hay datos disponibles inmediatamente, lo que nos permite
            // hacer polling no bloqueante sin termios.
            let libname = "libc.so.6"
            if self.os == "macos" { libname = "libc.dylib" }
            self.lib = ffi_open(libname)
            self.getchar_sym = ffi_sym(self.lib, "getchar")
            // -ixon -ixoff: desactiva el control XOFF/XON para que Ctrl+S
            // y Ctrl+Q lleguen al programa en vez de pausar la salida.
            shell("stty -icanon -echo -ixon -ixoff min 0 time 0")
        }
    }

    // Bucle de lectura bloqueante: el editor llama a esto en su loop
    // principal. Sleeps cortos (4ms) para mantener responsividad.
    fn read_key_blocking(self) {
        while true {
            let k = self.poll_key()
            if k != -1 { return k }
            sleep(4)   // ~250 polls/seg, latencia max ~4ms
        }
        return -1
    }

    // Polling no bloqueante: devuelve codigo de tecla o -1 si no hay nada.
    fn poll_key(self) {
        if self.os == "windows" {
            let n = ffi_call(self.kbhit_sym)
            if n == 0 { return -1 }
            let c = ffi_call(self.getch_sym)
            // Teclas extendidas en Windows: prefijo 0 o 224 + segundo byte
            if c == 0 or c == 224 {
                let c2 = ffi_call(self.getch_sym)
                return self._win_special(c2)
            }
            return c
        } else {
            let c = ffi_call(self.getchar_sym)
            if c == -1 { return -1 }
            // En POSIX las teclas especiales llegan como secuencia ESC [...]
            if c == 27 {
                // Distinguir ESC suelto de ESC[
                let c1 = self._poll_with_timeout(15)
                if c1 == -1 { return KEY_ESC }
                if c1 != 91 { return KEY_ESC }   // 91 = '['
                let c2 = self._poll_with_timeout(15)
                if c2 == -1 { return KEY_ESC }
                return self._posix_special(c2)
            }
            return c
        }
    }

    // Polling con timeout corto, para distinguir ESC suelto de ESC[
    // (cuando llega un Esc real, los siguientes bytes vendran muy rapido;
    // si no llegan en X ms es Esc solo).
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

    // Mapeo de teclas especiales en Windows (segundo byte tras 0/224).
    // Codigos extraidos de la API _getch documentation.
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
        if c2 == 61 { return KEY_F3    }
        // Ctrl+flechas en Windows
        if c2 == 116 { return KEY_CTRL_RIGHT }
        if c2 == 115 { return KEY_CTRL_LEFT  }
        if c2 == 141 { return KEY_CTRL_UP    }
        if c2 == 145 { return KEY_CTRL_DOWN  }
        // Shift+flechas en Windows
        if c2 == 152 { return KEY_SHIFT_UP    }
        if c2 == 160 { return KEY_SHIFT_DOWN  }
        if c2 == 155 { return KEY_SHIFT_LEFT  }
        if c2 == 157 { return KEY_SHIFT_RIGHT }
        return -2999   // tecla especial desconocida
    }

    // Mapeo de teclas especiales en POSIX (tras ESC[).
    // Las flechas vienen como ESC[A/B/C/D; con modificadores como ESC[1;NX
    // donde N codifica los modificadores: 2=Shift, 5=Ctrl.
    fn _posix_special(self, c2) {
        if c2 == 65 { return KEY_UP    }
        if c2 == 66 { return KEY_DOWN  }
        if c2 == 67 { return KEY_RIGHT }
        if c2 == 68 { return KEY_LEFT  }
        if c2 == 72 { return KEY_HOME  }
        if c2 == 70 { return KEY_END   }
        // ESC[5~ = PgUp, ESC[6~ = PgDn, ESC[3~ = Del
        if c2 == 53 { self._poll_with_timeout(10); return KEY_PGUP }
        if c2 == 54 { self._poll_with_timeout(10); return KEY_PGDN }
        if c2 == 51 { self._poll_with_timeout(10); return KEY_DEL  }
        // ESC[1;NX = flechas con modificadores
        if c2 == 49 {
            let semi = self._poll_with_timeout(10)
            if semi == 59 {   // ';'
                let mod = self._poll_with_timeout(10)
                let dir = self._poll_with_timeout(10)
                // Shift = 2
                if mod == 50 {
                    if dir == 65 { return KEY_SHIFT_UP    }
                    if dir == 66 { return KEY_SHIFT_DOWN  }
                    if dir == 67 { return KEY_SHIFT_RIGHT }
                    if dir == 68 { return KEY_SHIFT_LEFT  }
                }
                // Ctrl = 5
                if mod == 53 {
                    if dir == 65 { return KEY_CTRL_UP    }
                    if dir == 66 { return KEY_CTRL_DOWN  }
                    if dir == 67 { return KEY_CTRL_RIGHT }
                    if dir == 68 { return KEY_CTRL_LEFT  }
                }
            }
            return KEY_HOME
        }
        return -2999
    }

    // Restaura el terminal antes de salir. CRITICO en POSIX porque si
    // dejamos el modo raw activo, la shell del usuario queda inservible.
    fn shutdown(self) {
        if self.os != "windows" {
            shell("stty icanon echo ixon ixoff")
        }
        if self.lib != 0 { ffi_close(self.lib) }
    }
}


// =============================================================================
// SECCION 3: TermSize - detectar tamano del terminal
// =============================================================================
// En Windows usamos 'mode con' que imprime "Lineas: N", "Columnas: N".
// En POSIX usamos 'stty size' que imprime "rows cols".
// Si la deteccion falla, fallback a 80x24.

class TermSize {
    "Detecta el tamaño del terminal de forma robusta y multi-idioma."

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
                let r = nums[0]
                let c = nums[1]
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
                let part = substr(s, start, i - start)
                append(nums, int(part))
            } else {
                i = i + 1
            }
        }
        return nums
    }
}


// =============================================================================
// SECCION 4: Highlighter - tokenizador y coloreador de sintaxis VSH
// =============================================================================
// El highlighter trabaja en DOS pasos:
//   1) tokenize(line, state) -> lista de tokens + nuevo estado
//   2) render(tokens, overlays) -> string con escapes ANSI
// Esta separacion permite aplicar overlays (busqueda, palabra bajo cursor,
// seleccion, overflow >80) en una segunda pasada sin tener que descomponer
// secuencias ANSI ya emitidas.
//
// 'state' es un map { "in_cmt": bool, "in_docstr": bool } que indica si la
// linea arranca dentro de un comentario de bloque /* */ o dentro de un
// docstring """...""". Esto permite manejar correctamente bloques que
// abarcan varias lineas.
//
// Categorias de tokens:
//   "kw"      - palabras clave (let, fn, if, while...)
//   "type"    - tipos primitivos (int, string, bool...)
//   "builtin" - funciones builtin (print, len, range...)
//   "func"    - identificador seguido de '(' (definicion o llamada)
//   "str"     - literales de string "..."
//   "docstr"  - literales de docstring """..."""
//   "num"     - numeros (incluye 0xFF, 3.14)
//   "cmt"     - comentarios // y /* */
//   "op"      - operadores y puntuacion
//   "id"      - identificadores normales
//   "ws"      - espacios y otros caracteres no clasificables

// Listas de palabras conocidas. Se buscan por igualdad exacta tras tokenizar.
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
        // Paleta Dracula 2.0 con true color (24 bits)
        // Referencia: https://draculatheme.com
        self.col_kw         = ansi_rgb(255, 121, 198)              // pink
        self.col_type       = ansi_rgb(139, 233, 253)              // cyan
        self.col_builtin    = ansi_rgb(80, 250, 123)               // green
        self.col_func       = ansi_rgb(80, 250, 123)               // green
        self.col_str        = ansi_rgb(241, 250, 140)              // yellow
        self.col_docstr     = ansi_rgb(241, 250, 140) + ANSI["ITALIC"]
        self.col_cmt        = ansi_rgb(98, 114, 164) + ANSI["ITALIC"]
        self.col_num        = ansi_rgb(189, 147, 249)              // purple
        self.col_op         = ansi_rgb(255, 121, 198)              // pink
        self.col_id         = ansi_rgb(248, 248, 242)              // foreground
        self.col_match_word = ANSI["REVERSE"]
        self.col_search     = ANSI["BG_YELLOW"] + ANSI["BLACK"]
        // Texto que pasa de la columna 80
        self.col_overflow   = ansi_rgb_bg(80, 30, 30) + ansi_rgb(255, 100, 100)
        // Numeros de linea + ruler vertical en col 80
        self.col_ruler      = ansi_rgb(68, 71, 90)
        // Cursor secundario (multicursor)
        self.col_cursor_sec = ansi_rgb_bg(98, 114, 164) + ansi_rgb(248, 248, 242)
        self.col_reset      = ANSI["RESET"]
    }

    // ---- predicados de caracter ----

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

    // ---- tokenizacion ----
    // Devuelve { "tokens": [...], "state": {...} }
    // 'state' codifica si seguimos dentro de un /* o de un """ al final.

    fn tokenize(self, line, state) {
        let tokens = []
        let i = 0
        let n = len(line)
        let in_cmt = state["in_cmt"]
        let in_docstr = state["in_docstr"]

        while i < n {
            // -- continuacion de docstring """..."""
            if in_docstr {
                let j = i
                let found = false
                while j <= n - 3 {
                    if substr(line, j, 3) == "\"\"\"" {
                        append(tokens, { "kind": "docstr",
                                         "text": substr(line, i, j+3-i),
                                         "start": i })
                        i = j + 3
                        in_docstr = false
                        found = true
                        break
                    }
                    j = j + 1
                }
                if not found {
                    append(tokens, { "kind": "docstr",
                                     "text": substr(line, i, n-i),
                                     "start": i })
                    i = n
                }
                continue
            }

            // -- continuacion de comentario de bloque /* ... */
            if in_cmt {
                let j = i
                let found = false
                while j < n - 1 {
                    if substr(line, j, 1) == "*" and substr(line, j+1, 1) == "/" {
                        append(tokens, { "kind": "cmt",
                                         "text": substr(line, i, j+2-i),
                                         "start": i })
                        i = j + 2
                        in_cmt = false
                        found = true
                        break
                    }
                    j = j + 1
                }
                if not found {
                    append(tokens, { "kind": "cmt",
                                     "text": substr(line, i, n-i),
                                     "start": i })
                    i = n
                }
                continue
            }

            let c = substr(line, i, 1)

            // -- triple comilla """ (apertura, abre docstring)
            if c == "\"" and i+2 < n and substr(line, i, 3) == "\"\"\"" {
                let start = i
                let j = i + 3
                let closed = false
                while j <= n - 3 {
                    if substr(line, j, 3) == "\"\"\"" {
                        append(tokens, { "kind": "docstr",
                                         "text": substr(line, start, j+3-start),
                                         "start": start })
                        i = j + 3
                        closed = true
                        break
                    }
                    j = j + 1
                }
                if not closed {
                    append(tokens, { "kind": "docstr",
                                     "text": substr(line, start, n-start),
                                     "start": start })
                    i = n
                    in_docstr = true
                }
                continue
            }

            // -- comentario de linea //
            if c == "/" and i+1 < n and substr(line, i+1, 1) == "/" {
                append(tokens, { "kind": "cmt",
                                 "text": substr(line, i, n-i),
                                 "start": i })
                i = n
                continue
            }

            // -- comentario de bloque /* (puede cerrar en la misma linea o no)
            if c == "/" and i+1 < n and substr(line, i+1, 1) == "*" {
                let start = i
                let j = i + 2
                let closed = false
                while j < n - 1 {
                    if substr(line, j, 1) == "*" and substr(line, j+1, 1) == "/" {
                        append(tokens, { "kind": "cmt",
                                         "text": substr(line, start, j+2-start),
                                         "start": start })
                        i = j + 2
                        closed = true
                        break
                    }
                    j = j + 1
                }
                if not closed {
                    append(tokens, { "kind": "cmt",
                                     "text": substr(line, start, n-start),
                                     "start": start })
                    i = n
                    in_cmt = true
                }
                continue
            }

            // -- string normal "..." (con escapes)
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
                append(tokens, { "kind": "str",
                                 "text": substr(line, start, j-start),
                                 "start": start })
                i = j
                continue
            }

            // -- numero (decimal o hex 0x...)
            if self._is_digit(c) {
                let start = i
                if c == "0" and i+1 < n and substr(line, i+1, 1) == "x" {
                    i = i + 2
                    while i < n {
                        let cc = substr(line, i, 1)
                        let b = char_code(cc)
                        let hex_ok = (b >= 48 and b <= 57) or \
                                     (b >= 65 and b <= 70) or \
                                     (b >= 97 and b <= 102)
                        if not hex_ok { break }
                        i = i + 1
                    }
                } else {
                    while i < n and (self._is_digit(substr(line, i, 1)) or substr(line, i, 1) == ".") {
                        i = i + 1
                    }
                }
                append(tokens, { "kind": "num",
                                 "text": substr(line, start, i-start),
                                 "start": start })
                continue
            }

            // -- identificador o palabra reservada
            if self._is_alpha(c) {
                let start = i
                while i < n and self._is_alnum(substr(line, i, 1)) {
                    i = i + 1
                }
                let word = substr(line, start, i-start)
                let kind = "id"
                if      self._in_list(word, VSH_KEYWORDS) { kind = "kw"
                } elif    self._in_list(word, VSH_TYPES)    { kind = "type"
                } elif    self._in_list(word, VSH_BUILTINS) { kind = "builtin" }
                // Si era id "puro" y le sigue '(' -> nombre de funcion
                if kind == "id" {
                    let k = i
                    while k < n and substr(line, k, 1) == " " { k = k + 1 }
                    if k < n and substr(line, k, 1) == "(" {
                        kind = "func"
                    }
                }
                append(tokens, { "kind": kind, "text": word, "start": start })
                continue
            }

            // -- operadores y puntuacion
            let is_op = false
            if c == "+" or c == "-" or c == "*" or c == "/" or c == "=" \
               or c == "<" or c == ">" or c == "!" or c == "&" or c == "|" \
               or c == "{" or c == "}" or c == "(" or c == ")" \
               or c == "[" or c == "]" or c == "," or c == ";" \
               or c == ":" or c == "." {
                is_op = true
            }
            if is_op {
                append(tokens, { "kind": "op", "text": c, "start": i })
                i = i + 1
                continue
            }

            // -- whitespace u otros: kind "ws"
            append(tokens, { "kind": "ws", "text": c, "start": i })
            i = i + 1
        }
        return { "tokens": tokens,
                 "state":  { "in_cmt": in_cmt, "in_docstr": in_docstr } }
    }

    // ---- render: tokens -> string con escapes ANSI ----
    // Aplica el color base por kind. Si un token cae sobre un overlay
    // (busqueda, palabra bajo cursor, seleccion, overflow), el overlay
    // pisa el color base.

    fn render(self, tokens, highlights) {
        let out = ""
        for tok in tokens {
            let color = self._color_for(tok["kind"])
            let txt = tok["text"]
            let start = tok["start"]
            let end = start + len(txt)

            let overlay = self._find_overlay(highlights, start, end)
            if overlay == null {
                if color == "" { out = out + txt
                } else { out = out + color + txt + self.col_reset }
            } else {
                out = out + overlay["color"] + txt + self.col_reset
            }
        }
        return out
    }

    fn _find_overlay(self, highlights, start, end) {
        if highlights == null { return null }
        for h in highlights {
            // Solapamiento: [start, end) intersecta [hs, he)
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

    // Busca apariciones de la palabra (mismo identificador) en los tokens
    fn find_word_matches(self, tokens, word) {
        let matches = []
        if word == "" { return matches }
        for tok in tokens {
            let k = tok["kind"]
            if k == "id" or k == "kw" or k == "builtin" or k == "type" or k == "func" {
                if tok["text"] == word {
                    let s = tok["start"]
                    append(matches, { "start": s,
                                      "end":   s + len(word),
                                      "color": self.col_match_word })
                }
            }
        }
        return matches
    }

    // Busca todas las apariciones de una subcadena en una linea
    fn find_search_matches(self, line, needle) {
        let matches = []
        if needle == "" { return matches }
        let pos = 0
        while true {
            let p = find_str(line, needle, pos)
            if p == -1 { break }
            append(matches, { "start": p,
                              "end":   p + len(needle),
                              "color": self.col_search })
            pos = p + len(needle)
        }
        return matches
    }
}


// =============================================================================
// SECCION 5: TextBuffer - modelo del documento + Undo/Redo optimizado
// =============================================================================
// Representamos el documento como una lista de strings (una por linea, sin \n).
// En paralelo mantenemos:
//   - line_dirty[i]: bool, si la linea i necesita redibujarse
//   - in_state_at[i]: estado del highlighter al INICIO de la linea i
//                     (para manejar /* */ y """ multilinea)
//
// OPTIMIZACION: las pilas undo/redo usan "tope logico" en lugar de slicing
// para evitar copiar la lista entera en cada pop. Mantenemos:
//   - undo_stack: lista que crece hasta max_undo
//   - undo_top: indice del proximo slot libre (size logico)
// Un pop es undo_top-- (no toca la lista). Un push reusa el slot si cabe.
//
// OPTIMIZACION: mark_dirty solo marca la linea editada, no de ahi al final.
// Solo si la edicion afecta al estado de comentarios/docstrings (insertando
// '/', '*', '"') se llama a mark_dirty_from para propagar.

class TextBuffer {
    "Modelo del documento. Maneja edicion y undo/redo."

    fn __init__(self) {
        self.lines = [""]
        self.line_dirty = [true]
        self.in_state_at = [{ "in_cmt": false, "in_docstr": false }]
        // Pila undo: lista preallocada conceptualmente; usamos tope logico
        self.undo_stack = []
        self.undo_top = 0           // numero de elementos validos
        self.redo_stack = []
        self.redo_top = 0
        self.max_undo = 200
        self.modified = false

        self.last_action_ms = 0
        self.last_action_kind = ""
        self.coalesce_window_ms = 600   // 600ms para fusionar acciones similares
    }

    fn load_text(self, text) {
        self.lines = []
        let raw = split(text, "\n")
        for ln in raw {
            // Quitar \r residual de ficheros CRLF de Windows
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
    fn nlines(self)  { return len(self.lines) }

    fn line(self, i) {
        if i < 0 or i >= len(self.lines) { return "" }
        return self.lines[i]
    }

    fn mark_dirty(self, i) {
        if i >= 0 and i < len(self.line_dirty) { self.line_dirty[i] = true }
    }

    // Solo marcar dirty desde i hacia adelante CUANDO el cambio
    // realmente puede haber afectado al estado de comentarios/docstrings.
    // Para cambios "neutros" (caracteres alfanumericos en medio de un
    // identificador, por ejemplo), basta con mark_dirty(i).
    fn mark_dirty_from(self, i) {
        let k = i
        while k < len(self.line_dirty) {
            self.line_dirty[k] = true
            k = k + 1
        }
    }

    // ---- gestion de pila undo con tope logico (evita copiar la lista) ----

    fn _push_undo(self, action) {
        let now = time_ms()
        let op = action["op"]

        // Coalescing: si la accion anterior es del mismo tipo y se hizo
        // hace menos de coalesce_window_ms, no creamos una nueva entrada
        // de undo. Por ejemplo, escribir "hola" -> Ctrl+Z borra "hola"
        // entera, no solo "a".
        let can_coalesce = false
        if self.undo_top > 0 {
            let prev = self.undo_stack[self.undo_top - 1]
            // Solo agrupamos delete_char (= insert_char fue hecho) consecutivos.
            // op == "delete_char" significa: el usuario inserto un caracter,
            // y la accion para deshacerlo es borrarlo.
            if op == "delete_char" and prev["op"] == "delete_char" {
                // Adyacencia: la columna del nuevo es prev.col + 1
                if prev["row"] == action["row"] and prev["col"] + 1 == action["col"] {
                    if now - self.last_action_ms < self.coalesce_window_ms {
                        can_coalesce = true
                    }
                }
            }
            // Coalescing de borrados con backspace consecutivo
            if op == "insert_char" and prev["op"] == "insert_char" {
                if prev["row"] == action["row"] and prev["col"] - 1 == action["col"] {
                    if now - self.last_action_ms < self.coalesce_window_ms {
                        can_coalesce = true
                    }
                }
            }
        }

        if can_coalesce {
            // Modificamos el ultimo undo para que abarque el rango ampliado.
            // Para delete_char: extendemos el "ch" (concatenando) y mantenemos col original.
            let prev = self.undo_stack[self.undo_top - 1]
            if op == "delete_char" {
                // Encadenamos chars en orden de insercion. La col se queda en la primera.
                prev["ch"] = prev["ch"] + action["ch"]
            } elif op == "insert_char" {
                // Backspaces consecutivos: chars en orden inverso
                prev["ch"] = action["ch"] + prev["ch"]
                prev["col"] = action["col"]
            }
        } else {
            // Push normal
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

    // ---- operaciones primitivas ----
    // Cada una empuja al undo la operacion INVERSA. Al hacer undo, la
    // inversa se aplica (sin pasar por aqui de nuevo) y a su vez se empuja
    // al redo la inversa de la inversa, que es la accion original.

    // Heuristica: "char afecta sintaxis" si es '/', '*', '"' (puede abrir
    // o cerrar comentarios o docstrings). Para el resto basta con marcar
    // la linea actual como dirty, sin propagar.
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
        let left  = substr(ln, 0, col)
        let right = substr(ln, col, len(ln)-col)
        self.lines[row] = left
        // Reconstruir lines, line_dirty, in_state_at insertando en row+1
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
        // Estructural: marcar todo desde row hacia adelante
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

    // ---- undo / redo ----

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
            // Push directo sin invalidar redo
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

    // Aplica la operacion inversa indicada por 'action' SIN apilar un nuevo
    // undo, y devuelve la inversa de la inversa para apilarla en el otro
    // stack (redo si venimos de undo, undo si venimos de redo).
    fn _apply_inverse(self, action) {
        let op = action["op"]
        //if op == "insert_char" {
        //    let r = action["row"]; let c = action["col"]; let ch = action["ch"]
        //    let ln = self.lines[r]
        //    self.lines[r] = substr(ln, 0, c) + ch + substr(ln, c, len(ln)-c)
        //    self.mark_dirty(r)
        //    if self._affects_syntax(ch) { self.mark_dirty_from(r) }
        //    return { "op": "delete_char", "row": r, "col": c, "ch": ch }
        //}
        if op == "insert_char" {
            // Aplica varias inserciones (1+) en sucesion
            let r = action["row"]; let c = action["col"]; let chs = action["ch"]
            let ln = self.lines[r]
            self.lines[r] = substr(ln, 0, c) + chs + substr(ln, c, len(ln)-c)
            self.mark_dirty(r)
            // Si alguno de los chars afecta sintaxis, propagar
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
            let want = action["ch"]   // puede ser 1+ caracteres
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

        //if op == "delete_char" {
        //    let r = action["row"]; let c = action["col"]
        //    let ln = self.lines[r]
        //    if c >= len(ln) { return null }
        //    let ch = substr(ln, c, 1)
        //    self.lines[r] = substr(ln, 0, c) + substr(ln, c+1, len(ln)-c-1)
        //    self.mark_dirty(r)
        //    if self._affects_syntax(ch) { self.mark_dirty_from(r) }
        //    return { "op": "insert_char", "row": r, "col": c, "ch": ch }
        //}
        if op == "split_line" {
            let r = action["row"]; let c = action["col"]
            let ln = self.lines[r]
            let left  = substr(ln, 0, c)
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
            self.lines = new_lines
            self.line_dirty = new_dirty
            self.in_state_at = new_state
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
            self.lines = new_lines; self.line_dirty = new_dirty
            self.in_state_at = new_state
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
            self.lines = new_lines; self.line_dirty = new_dirty
            self.in_state_at = new_state
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
            self.lines = new_lines; self.line_dirty = new_dirty
            self.in_state_at = new_state
            self.mark_dirty_from(r)
            return { "op": "insert_line", "row": r, "text": t }
        }
        return null
    }
}


// =============================================================================
// SECCION 6: Editor - vista, cursores multiples, comandos
// =============================================================================
// MULTICURSOR DESIGN
// ------------------
// self.cursors es una lista. Cada cursor es:
//   { "row": r, "col": c, "anchor_row": ar, "anchor_col": ac, "primary": bool }
// - El cursor primario es self.cursors[0]; el scroll y la palabra-bajo-cursor
//   se calculan respecto a el.
// - 'anchor' = posicion de inicio de seleccion (igual a row/col si no hay
//   seleccion activa para ese cursor).
// - has_selection: row != anchor_row or col != anchor_col
//
// REGLAS DE OPERACION:
//   1) Para evitar invalidaciones, cualquier mutacion del buffer que afecte
//      a varios cursores se hace iterando los cursores en orden INVERSO
//      (de mayor a menor por (row, col)). Asi al insertar/borrar caracteres
//      en un cursor inferior, las posiciones de los superiores no cambian.
//   2) Tras cualquier comando, llamar a _coalesce_cursors() para fusionar
//      cursores que hayan caido en la misma posicion.

class Editor {
    "Editor de texto multicursor con highlighter de VSH."

    fn __init__(self, filename) {
        self.filename = filename
        self.buf = TextBuffer()
        self.hl  = Highlighter()
        let ts = TermSize()
        self.term_w = ts.cols
        self.term_h = ts.rows

        self.input_backend = null   // se asigna en run()

        // Lista de cursores. Empieza con uno solo.
        self.cursors = [self._make_cursor(0, 0, true)]

        // Scroll
        self.row_off = 0
        self.col_off = 0
        self.full_redraw = true

        // Kill buffer (cortar/pegar). Para multicursor guardamos una lista
        // de strings paralela a la lista de cursores cuando hay seleccion;
        // si se pega con un solo cursor, se concatenan con \n.
        self.kill_buffer = ""
        self.kill_per_cursor = []   // si != [], paste en multicursor pega per-cursor

        // Busqueda
        self.last_search = ""

        // Mensaje de status efimero
        self.status_msg = ""
        self.status_until = 0

        // Mostrar numeros de linea
        self.show_lineno = true

        // Cargar fichero
        if self.filename != "" and exists(self.filename) {
            let txt = read_file(self.filename)
            self.buf.load_text(txt)
            self.set_status("Leido: " + self.filename + " (" + str(self.buf.nlines()) + " lineas)")
        } else {
            self.set_status("Nuevo fichero")
        }
        // Tras cargar, recomputar todos los estados
        self._recompute_state(0)
    }


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

        // Limpiar pantalla DESPUES, no solo antes
        print(ANSI["CLEAR"])
        print(ansi_cursor_pos(1, 1))

        self._clamp_all_cursors()
        self.full_redraw = true

        let i = 0
        while i < self.buf.nlines() {
            self.buf.line_dirty[i] = true
            i = i + 1
        }
    }

    // Recoge identificadores unicos del buffer entero. Cacheamos para no
    // re-escanear en cada Ctrl+Space; invalidamos en cualquier edicion.
    fn _build_word_corpus(self, prefix) {
        let words = []
        let seen = {}
        // Anadir builtins, keywords y types primero
        for w in VSH_KEYWORDS {
            if starts_with(w, prefix) and len(w) > len(prefix) {
                if not contains(seen, w) {
                    append(words, { "text": w, "kind": "kw" })
                    seen[w] = true
                }
            }
        }
        for w in VSH_TYPES {
            if starts_with(w, prefix) and len(w) > len(prefix) {
                if not contains(seen, w) {
                    append(words, { "text": w, "kind": "type" })
                    seen[w] = true
                }
            }
        }
        for w in VSH_BUILTINS {
            if starts_with(w, prefix) and len(w) > len(prefix) {
                if not contains(seen, w) {
                    append(words, { "text": w, "kind": "builtin" })
                    seen[w] = true
                }
            }
        }
        // Identificadores del documento: tokenizamos cada linea
        let r = 0
        while r < self.buf.nlines() {
            let res = self.hl.tokenize(self.buf.lines[r], self.buf.in_state_at[r])
            for tok in res["tokens"] {
                let k = tok["kind"]
                if k == "id" or k == "func" {
                    let w = tok["text"]
                    if starts_with(w, prefix) and len(w) > len(prefix) {
                        if not contains(seen, w) {
                            append(words, { "text": w, "kind": k })
                            seen[w] = true
                        }
                    }
                }
            }
            r = r + 1
        }
        return words
    }

    
    // Devuelve { "prefix": str, "start": int, "end": int }
    // Donde start..end es el rango del prefijo en la linea del cursor.
    fn _word_prefix_at_cursor(self) {
        let p = self._primary()
        let ln = self.buf.line(p["row"])
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

    // Comando principal: muestra dropdown, navega, inserta seleccion
    fn cmd_autocomplete(self, inp) {
        let info = self._word_prefix_at_cursor()
        let prefix = info["prefix"]
        if len(prefix) < 1 {
            self.set_status("Escribe al menos 1 caracter antes de Ctrl+Space")
            return
        }
        let candidates = self._build_word_corpus(prefix)
        if len(candidates) == 0 {
            self.set_status("Sin sugerencias para '" + prefix + "'")
            return
        }
        if len(candidates) == 1 {
            // Una sola opcion: insertar directamente
            self._apply_completion(candidates[0]["text"], info)
            self.set_status("Completado")
            return
        }

        // Mostrar dropdown bajo el cursor del primario
        let sel = 0
        let max_show = 8
        if max_show > len(candidates) { max_show = len(candidates) }
        let p = self._primary()
        let dropdown_row_screen = p["row"] - self.row_off + 2 + 1   // 1 fila bajo cursor
        let dropdown_col_screen = p["col"] - self.col_off + self._gutter_width() + 1

        // Si no cabe debajo, mostrar arriba
        if dropdown_row_screen + max_show > self.term_h {
            dropdown_row_screen = p["row"] - self.row_off + 2 - max_show
            if dropdown_row_screen < 2 { dropdown_row_screen = 2 }
        }

        // Calcular ancho del dropdown
        let max_w = 0
        let i = 0
        while i < len(candidates) {
            let l = len(candidates[i]["text"]) + 2 + 8   // texto + padding + tipo
            if l > max_w { max_w = l }
            i = i + 1
        }
        if max_w > 40 { max_w = 40 }

        let cancelled = false
        while true {
            // Pintar dropdown
            let i2 = 0
            while i2 < max_show {
                print(ansi_cursor_pos(dropdown_row_screen + i2, dropdown_col_screen))
                let cand = candidates[i2]
                let txt = cand["text"]
                let kind_label = "  " + cand["kind"]
                let line = " " + txt + repeat(" ", max_w - len(txt) - len(kind_label) - 1) + kind_label + " "
                if i2 == sel {
                    print(ANSI["BG_BLUE"] + ANSI["WHITE"] + line + ANSI["RESET"])
                } else {
                    print(self.hl.col_cursor_sec + line + ANSI["RESET"])
                }
                i2 = i2 + 1
            }

            let k = inp.read_key_blocking()
            if k == KEY_ESC { cancelled = true; break }
            if k == KEY_ENTER or k == KEY_TAB { break }
            if k == KEY_UP {
                sel = sel - 1
                if sel < 0 { sel = max_show - 1 }
            } elif k == KEY_DOWN {
                sel = sel + 1
                if sel >= max_show { sel = 0 }
            }
            // Cualquier otra tecla: cancelar
            if k != KEY_UP and k != KEY_DOWN { cancelled = true; break }
        }

        self.full_redraw = true
        if not cancelled {
            self._apply_completion(candidates[sel]["text"], info)
        }
    }

    // Reemplaza el prefijo bajo cursor por el texto completo
    fn _apply_completion(self, full_word, info) {
        let p = self._primary()
        // Borrar caracteres del prefijo y luego insertar la palabra completa
        // Para evitar tener que tocar buffer directo, hacemos backspace
        // por cada char del prefijo (sin coalescing) y luego insert.
        let n_to_del = info["end"] - info["start"]
        let i = 0
        while i < n_to_del {
            self.buf.delete_char(p["row"], info["start"])
            i = i + 1
        }
        // Ahora insertar la palabra completa
        p["col"] = info["start"]
        let j = 0
        while j < len(full_word) {
            self.buf.insert_char(p["row"], p["col"], substr(full_word, j, 1))
            p["col"] = p["col"] + 1
            j = j + 1
        }
        p["anchor_row"] = p["row"]; p["anchor_col"] = p["col"]
        self._recompute_state(p["row"])
        self._scroll()
    }


    // ---- helpers de cursor ----

    fn _make_cursor(self, row, col, primary) {
        return { "row": row, "col": col,
                 "anchor_row": row, "anchor_col": col,
                 "primary": primary }
    }

    fn _primary(self) { return self.cursors[0] }
    fn _has_multi(self) { return len(self.cursors) > 1 }

    // True si algun cursor tiene seleccion activa
    fn _any_selection(self) {
        for cur in self.cursors {
            if cur["row"] != cur["anchor_row"] or cur["col"] != cur["anchor_col"] {
                return true
            }
        }
        return false
    }

    // Para un cursor concreto: tiene seleccion?
    fn _cursor_has_sel(self, cur) {
        return cur["row"] != cur["anchor_row"] or cur["col"] != cur["anchor_col"]
    }

    // Devuelve el rango normalizado de un cursor: { r1, c1, r2, c2 } con r1<=r2
    fn _cursor_sel_range(self, cur) {
        let r1 = cur["anchor_row"]; let c1 = cur["anchor_col"]
        let r2 = cur["row"];        let c2 = cur["col"]
        if r1 > r2 or (r1 == r2 and c1 > c2) {
            let tr = r1; let tc = c1; r1 = r2; c1 = c2; r2 = tr; c2 = tc
        }
        return { "r1": r1, "c1": c1, "r2": r2, "c2": c2 }
    }

    // Ordena cursores DESCENDIENTE por (row, col) para iteraciones de mutacion
    fn _cursors_desc(self) {
        // Bubble sort simple. n suele ser <=10, asi que esta bien.
        let lst = []
        for c in self.cursors { append(lst, c) }
        let n = len(lst)
        let i = 0
        while i < n {
            let j = 0
            while j < n - 1 - i {
                let a = lst[j]; let b = lst[j+1]
                let cmp = false   // true si a debe ir DESPUES de b (a < b)
                if a["row"] < b["row"] { cmp = true 
                } elif a["row"] == b["row"] and a["col"] < b["col"] { cmp = true }
                if cmp {
                    lst[j] = b; lst[j+1] = a
                }
                j = j + 1
            }
            i = i + 1
        }
        return lst
    }

    // Tras una operacion, fusiona cursores que esten en la misma posicion
    fn _coalesce_cursors(self) {
        if len(self.cursors) <= 1 { return }
        let kept = []
        let i = 0
        while i < len(self.cursors) {
            let c = self.cursors[i]
            let dup = false
            for k in kept {
                if k["row"] == c["row"] and k["col"] == c["col"] {
                    dup = true
                    break
                }
            }
            if not dup { append(kept, c) }
            i = i + 1
        }
        // Asegurar que solo el primero es primary
        let j = 0
        while j < len(kept) {
            kept[j]["primary"] = (j == 0)
            j = j + 1
        }
        self.cursors = kept
        self.full_redraw = true
    }

    fn _clamp_all_cursors(self) {
        let n = self.buf.nlines()
        if n == 0 { return }
        for cur in self.cursors {
            if cur["row"] < 0 { cur["row"] = 0 }
            if cur["row"] >= n { cur["row"] = n - 1 }
            let ln_len = len(self.buf.line(cur["row"]))
            if cur["col"] < 0 { cur["col"] = 0 }
            if cur["col"] > ln_len { cur["col"] = ln_len }
            if cur["anchor_row"] < 0 { cur["anchor_row"] = 0 }
            if cur["anchor_row"] >= n { cur["anchor_row"] = n - 1 }
            let aln_len = len(self.buf.line(cur["anchor_row"]))
            if cur["anchor_col"] < 0 { cur["anchor_col"] = 0 }
            if cur["anchor_col"] > aln_len { cur["anchor_col"] = aln_len }
        }
    }

    // Colapsa todos los cursores secundarios al primario (Esc)
    fn cmd_collapse_cursors(self) {
        if not self._has_multi() { return }
        let p = self._primary()
        self.cursors = [self._make_cursor(p["row"], p["col"], true)]
        self.full_redraw = true
    }

    // Anade un cursor en la linea de arriba/abajo, misma columna que el
    // ultimo cursor anadido
    fn cmd_add_cursor_above(self) {
        let last = self.cursors[len(self.cursors)-1]
        let new_row = last["row"] - 1
        if new_row < 0 { self.set_status("Tope superior alcanzado"); return }
        let ln_len = len(self.buf.line(new_row))
        let new_col = last["col"]
        if new_col > ln_len { new_col = ln_len }
        append(self.cursors, self._make_cursor(new_row, new_col, false))
        self._coalesce_cursors()
        self.full_redraw = true
    }

    fn cmd_add_cursor_below(self) {
        let last = self.cursors[len(self.cursors)-1]
        let new_row = last["row"] + 1
        if new_row >= self.buf.nlines() { self.set_status("Tope inferior alcanzado"); return }
        let ln_len = len(self.buf.line(new_row))
        let new_col = last["col"]
        if new_col > ln_len { new_col = ln_len }
        append(self.cursors, self._make_cursor(new_row, new_col, false))
        self._coalesce_cursors()
        self.full_redraw = true
    }

    // ---- estado de comentario en cascada (anti-bug "una si, una no") ----
    // En lugar de leer un cache potencialmente obsoleto, recomputamos el
    // estado al final de la linea i-1 sobre la marcha tokenizando.
    fn _recompute_state(self, from_row) {
        let n = self.buf.nlines()
        if from_row < 0 { from_row = 0 }
        if from_row >= n { return }
        let i = from_row
        // Marcar la linea de entrada como dirty para refrescarla siempre
        self.buf.mark_dirty(i)
        while i < n {
            let entry_state = { "in_cmt": false, "in_docstr": false }
            if i > 0 {
                let prev = self.buf.in_state_at[i-1]
                let res = self.hl.tokenize(self.buf.lines[i-1], prev)
                entry_state = res["state"]
            }
            let cur = self.buf.in_state_at[i]
            if cur["in_cmt"] != entry_state["in_cmt"] or cur["in_docstr"] != entry_state["in_docstr"] {
                self.buf.in_state_at[i] = entry_state
                self.buf.mark_dirty(i)
            }
            i = i + 1
        }
    }

    fn set_status(self, msg) {
        self.status_msg = msg
        self.status_until = time_ms() + 4000
    }

    // ---- coordenadas de pantalla ----

    fn _line_area_height(self) { return self.term_h - 2 }

    fn _gutter_width(self) {
        if not self.show_lineno { return 0 }
        let n = self.buf.nlines()
        let w = 1
        let m = n
        while m >= 10 { w = w + 1; m = m / 10 }
        return w + 1   // +1 separador
    }

    fn _text_width(self) { return self.term_w - self._gutter_width() }

    // ---- scroll automatico (sigue al primary) ----

    fn _scroll(self) {
        self._clamp_all_cursors()      
        let h = self._line_area_height()
        let w = self._text_width()
        let p = self._primary()
        if p["row"] < self.row_off { self.row_off = p["row"]; self.full_redraw = true }
        if p["row"] >= self.row_off + h { self.row_off = p["row"] - h + 1; self.full_redraw = true }
        if p["col"] < self.col_off { self.col_off = p["col"]; self.full_redraw = true }
        if p["col"] >= self.col_off + w { self.col_off = p["col"] - w + 1; self.full_redraw = true }
    }

    // ---- palabra bajo cursor (del primary) ----

    fn _word_at_cursor(self) {
        let p = self._primary()
        let ln = self.buf.line(p["row"])
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

    // ---- render principal ----

    fn render(self) {
        let h = self._line_area_height()
        self._draw_header()
        let cur_word = self._word_at_cursor()

        let i = 0
        while i < h {
            let row_idx = self.row_off + i
            let must_draw = self.full_redraw
            if not must_draw and row_idx < self.buf.nlines() {
                must_draw = self.buf.line_dirty[row_idx]
            }
            if must_draw {
                self._draw_line(row_idx, i, cur_word)
                if row_idx < self.buf.nlines() {
                    self.buf.line_dirty[row_idx] = false
                }
            }
            i = i + 1
        }

        self._draw_status_bar()

        // Posicionar el cursor del terminal en el cursor primario
        let p = self._primary()
        let screen_row = p["row"] - self.row_off + 2
        let screen_col = p["col"] - self.col_off + self._gutter_width() + 1
        print(ansi_cursor_pos(screen_row, screen_col))
        print(ESC_CUR_SHOW)

        self.full_redraw = false
    }

    fn _draw_header(self) {
        print(ansi_cursor_pos(1, 1))
        print(ANSI["REVERSE"] + ANSI["BOLD"])
        let title = " vnano 0.3 "
        let fname = self.filename
        if fname == "" { fname = "[Sin nombre]" }
        let mod_marker = ""
        if self.buf.modified { mod_marker = " * " }
        let nc = ""
        if self._has_multi() { nc = "  [" + str(len(self.cursors)) + " cursores]" }
        let middle = "  File: " + fname + mod_marker + nc
        let total = title + middle
        if len(total) < self.term_w {
            total = total + repeat(" ", self.term_w - len(total))
        } else {
            total = substr(total, 0, self.term_w)
        }
        print(total)
        print(ANSI["RESET"])
    }

    // Dibuja una linea del editor con todos los overlays aplicados
    fn _draw_line(self, row_idx, screen_y, cur_word) {
        print(ansi_cursor_pos(screen_y + 2, 1))
        print(ESC_CLR_EOL)

        if row_idx >= self.buf.nlines() {
            print(self.hl.col_ruler + "~" + ANSI["RESET"])
            return
        }

        // Gutter (numeros de linea)
        let gw = 0
        if self.show_lineno {
            gw = self._gutter_width() - 1
            let lnum = pad_left(str(row_idx + 1), gw)
            print(self.hl.col_ruler + lnum + ANSI["RESET"] + " ")
            gw = gw + 1
        }

        let line = self.buf.line(row_idx)
        let in_state = self.buf.in_state_at[row_idx]
        let result = self.hl.tokenize(line, in_state)
        let tokens = result["tokens"]

        // Construir overlays. Orden de prioridad (primero gana en _find_overlay):
        //   1) seleccion de cualquier cursor
        //   2) cursores secundarios (caracter-marcador)
        //   3) coincidencias de busqueda
        //   4) palabra bajo cursor (otras apariciones)
        //   5) overflow >80 columnas
        let overlays = []

        // Selecciones de TODOS los cursores
        for cur in self.cursors {
            if self._cursor_has_sel(cur) {
                let sel_range = self._cursor_sel_range_in_line(cur, row_idx)
                if sel_range != null {
                    append(overlays, { "start": sel_range["start"],
                                       "end":   sel_range["end"],
                                       "color": ANSI["BG_BLUE"] + ANSI["WHITE"] })
                }
            }
        }

        // Cursores secundarios en esta linea (no el primario)
        // Marcamos un caracter de ancho 1 con BG diferenciado.
        let i_cur = 1
        while i_cur < len(self.cursors) {
            let cur = self.cursors[i_cur]
            if cur["row"] == row_idx and not self._cursor_has_sel(cur) {
                let pos = cur["col"]
                if pos < len(line) {
                    append(overlays, { "start": pos, "end": pos+1,
                                       "color": self.hl.col_cursor_sec })
                }
            }
            i_cur = i_cur + 1
        }

        if self.last_search != "" {
            let m = self.hl.find_search_matches(line, self.last_search)
            for x in m { append(overlays, x) }
        }
        if cur_word != "" and len(cur_word) >= 2 {
            let m = self.hl.find_word_matches(tokens, cur_word)
            let p = self._primary()
            for x in m {
                let is_cursor_word = (row_idx == p["row"] and p["col"] >= x["start"] and p["col"] <= x["end"])
                if not is_cursor_word { append(overlays, x) }
            }
        }
        if len(line) > 80 {
            append(overlays, { "start": 80, "end": len(line),
                               "color": self.hl.col_overflow })
        }

        // Render con scroll horizontal
        let painted = ""
        if self.col_off > 0 and self.col_off < len(line) {
            let visible = substr(line, self.col_off, len(line) - self.col_off)
            let res2 = self.hl.tokenize(visible, in_state)
            let overlays2 = []
            for ov in overlays {
                let s = ov["start"] - self.col_off
                let e = ov["end"] - self.col_off
                if e > 0 {
                    if s < 0 { s = 0 }
                    append(overlays2, { "start": s, "end": e, "color": ov["color"] })
                }
            }
            painted = self.hl.render(res2["tokens"], overlays2)
        } else {
            painted = self.hl.render(tokens, overlays)
        }

        print(painted)

        // Ruler vertical en columna 80 (si la linea es mas corta)
        if len(line) < 80 {
            let ruler_col = gw + 80 - self.col_off
            if ruler_col > gw and ruler_col <= self.term_w {
                print(ansi_cursor_pos(screen_y + 2, ruler_col + 1))
                print(self.hl.col_ruler + "|" + ANSI["RESET"])
            }
        }

        // Limpieza al final para evitar texto fantasma
        print(ESC_CLR_EOL)
    }

    fn _cursor_sel_range_in_line(self, cur, row_idx) {
        let r = self._cursor_sel_range(cur)
        if row_idx < r["r1"] or row_idx > r["r2"] { return null }
        let ln = self.buf.line(row_idx)
        let s = 0; let e = len(ln)
        if row_idx == r["r1"] { s = r["c1"] }
        if row_idx == r["r2"] { e = r["c2"] }
        return { "start": s, "end": e }
    }

    fn _draw_status_bar(self) {
        let y = self.term_h
        print(ansi_cursor_pos(y, 1))
        print(ANSI["REVERSE"])
        let p = self._primary()
        let pos = "Lin " + str(p["row"]+1) + ", Col " + str(p["col"]+1) + \
                  " / " + str(self.buf.nlines())
        let middle = ""
        let now = time_ms()
        if self.status_msg != "" and now < self.status_until {
            middle = "  " + self.status_msg + "  "
        } else {
            middle = "  ^S Save  ^E Run  ^T Cmd  ^Space Auto  ^Q Quit  ^W Find  ^G Goto  ^Z/^Y  ^Up/Dn Multi  "
        }
        let left  = " " + pos + " "
        let total = left + middle
        if len(total) < self.term_w {
            total = total + repeat(" ", self.term_w - len(total))
        } else {
            total = substr(total, 0, self.term_w)
        }
        print(total)
        print(ANSI["RESET"] + ESC_CLR_EOL)
    }

    // =========================================================================
    // COMANDOS DE EDICION (multicursor-aware)
    // =========================================================================
    // Patron: si hay seleccion en algun cursor, primero borrarla. Luego
    // iterar cursores en orden DESCENDIENTE para no invalidar posiciones.

    fn cmd_insert_char(self, ch) {
        // Si algun cursor tiene seleccion, borrarla primero
        if self._any_selection() { self._delete_all_selections() }
        let order = self._cursors_desc()
        let affects = self.buf._affects_syntax(ch)
        for cur in order {
            self.buf.insert_char(cur["row"], cur["col"], ch)
            cur["col"] = cur["col"] + 1
            cur["anchor_row"] = cur["row"]
            cur["anchor_col"] = cur["col"]
        }
        if affects { self._recompute_state(self._primary()["row"]) }
        self._coalesce_cursors()
    }

    fn cmd_backspace(self) {
        if self._any_selection() { self._delete_all_selections(); return }
        let order = self._cursors_desc()
        let any_struct = false
        for cur in order {
            if cur["col"] > 0 {
                let ch = substr(self.buf.line(cur["row"]), cur["col"]-1, 1)
                self.buf.delete_char(cur["row"], cur["col"]-1)
                cur["col"] = cur["col"] - 1
                cur["anchor_row"] = cur["row"]; cur["anchor_col"] = cur["col"]
                if self.buf._affects_syntax(ch) { any_struct = true }
            } else {
                if cur["row"] > 0 {
                    let prev_len = len(self.buf.line(cur["row"]-1))
                    self.buf.join_line(cur["row"]-1)
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
        if self._any_selection() { self._delete_all_selections(); return }
        let order = self._cursors_desc()
        let any_struct = false
        for cur in order {
            let ln = self.buf.line(cur["row"])
            if cur["col"] < len(ln) {
                let ch = substr(ln, cur["col"], 1)
                self.buf.delete_char(cur["row"], cur["col"])
                if self.buf._affects_syntax(ch) { any_struct = true }
            } else {
                if cur["row"]+1 < self.buf.nlines() {
                    self.buf.join_line(cur["row"])
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
        for cur in order {
            // Auto-indent: copiar espacios/tabs del inicio de la linea actual
            let ln = self.buf.line(cur["row"])
            let indent = ""
            let i = 0
            while i < len(ln) {
                let c = substr(ln, i, 1)
                if c == " " or c == "\t" { indent = indent + c; i = i + 1
                } else { break }
            }
            if cur["col"] <= len(indent) { indent = "" }

            self.buf.split_line(cur["row"], cur["col"])
            cur["row"] = cur["row"] + 1
            cur["col"] = 0
            // Insertar la indent
            let j = 0
            while j < len(indent) {
                self.buf.insert_char(cur["row"], cur["col"], substr(indent, j, 1))
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
        while i < 4 { self.cmd_insert_char(" "); i = i + 1 }
    }

    // Borra la seleccion de TODOS los cursores que tengan una.
    // Tambien guarda el texto borrado en kill_buffer (uniendo).
    fn _delete_all_selections(self) {
        // Procesar en orden DESCENDIENTE
        let order = self._cursors_desc()
        let killed_parts = []
        for cur in order {
            if not self._cursor_has_sel(cur) { continue }
            let r = self._cursor_sel_range(cur)
            let txt = self._extract_range(r["r1"], r["c1"], r["r2"], r["c2"])
            append(killed_parts, txt)
            // Borrar el rango (caracter a caracter por simplicidad)
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
        if r1 == r2 {
            let ln = self.buf.line(r1)
            // Sustituir directamente para evitar muchos undo de char
            let new_ln = substr(ln, 0, c1) + substr(ln, c2, len(ln)-c2)
            self.buf.lines[r1] = new_ln
            self.buf._push_undo({ "op": "replace_line", "row": r1, "old": ln, "new": new_ln })
            self.buf.mark_dirty(r1)
            return
        }
        // Multilinea: borrar lineas r1+1..r2 y juntar cabeza+cola
        let head = substr(self.buf.line(r1), 0, c1)
        let tail_line = self.buf.line(r2)
        let tail = substr(tail_line, c2, len(tail_line) - c2)
        let k = r2
        while k > r1 {
            self.buf.delete_line(k)
            k = k - 1
        }
        let old = self.buf.lines[r1]
        self.buf.lines[r1] = head + tail
        self.buf._push_undo({ "op": "replace_line", "row": r1,
                               "old": old, "new": head + tail })
        self.buf.mark_dirty(r1)
    }

    fn _extract_range(self, r1, c1, r2, c2) {
        if r1 == r2 {
            let ln = self.buf.line(r1)
            return substr(ln, c1, c2 - c1)
        }
        let parts = []
        let first = self.buf.line(r1)
        append(parts, substr(first, c1, len(first) - c1))
        let i = r1 + 1
        while i < r2 { append(parts, self.buf.line(i)); i = i + 1 }
        let last = self.buf.line(r2)
        append(parts, substr(last, 0, c2))
        return join(parts, "\n")
    }

    // ---- movimiento (multicursor-aware) ----

    fn cmd_move(self, drow, dcol, with_sel) {
        for cur in self.cursors {
            let new_row = cur["row"] + drow
            let new_col = cur["col"] + dcol
            if new_row < 0 { new_row = 0 }
            if new_row >= self.buf.nlines() { new_row = self.buf.nlines() - 1 }
            let ln_len = len(self.buf.line(new_row))
            if new_col < 0 {
                if new_row > 0 {
                    new_row = new_row - 1
                    new_col = len(self.buf.line(new_row))
                } else { new_col = 0 }
            }
            if new_col > ln_len {
                if drow == 0 and new_row+1 < self.buf.nlines() {
                    new_row = new_row + 1; new_col = 0
                } else { new_col = ln_len }
            }
            cur["row"] = new_row
            cur["col"] = new_col

            // CRITICO: si no extendemos sel, anchor sigue al cursor.
            // Asi _cursor_has_sel() devuelve false y nada se borra al escribir.
            if not with_sel {
                cur["anchor_row"] = new_row
                cur["anchor_col"] = new_col
            }
        }
        if with_sel { self.full_redraw = true }
        self._scroll()
    }

    fn cmd_home(self, with_sel) {
        for cur in self.cursors {
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
        for cur in self.cursors {
            let ln_len = len(self.buf.line(cur["row"]))
            cur["col"] = ln_len
            if not with_sel {
                cur["anchor_row"] = cur["row"]
                cur["anchor_col"] = ln_len
            }
        }
        if with_sel { self.full_redraw = true }
        self._scroll()
    }


    fn cmd_pgup(self) { self.cmd_move(-self._line_area_height(), 0, false) }
    fn cmd_pgdn(self) { self.cmd_move( self._line_area_height(), 0, false) }

    fn cmd_word_left(self) {
        for cur in self.cursors {
            cur["anchor_row"] = cur["row"]; cur["anchor_col"] = cur["col"]
            let ln = self.buf.line(cur["row"])
            let c = cur["col"]
            while c > 0 and not self.hl._is_alnum(substr(ln, c-1, 1)) { c = c - 1 }
            while c > 0 and self.hl._is_alnum(substr(ln, c-1, 1)) { c = c - 1 }
            if c == cur["col"] and cur["row"] > 0 {
                cur["row"] = cur["row"] - 1
                cur["col"] = len(self.buf.line(cur["row"]))
            } else { cur["col"] = c }
            cur["anchor_row"] = cur["row"]; cur["anchor_col"] = cur["col"]
        }
        self._scroll()
    }

    fn cmd_word_right(self) {
        for cur in self.cursors {
            cur["anchor_row"] = cur["row"]; cur["anchor_col"] = cur["col"]
            let ln = self.buf.line(cur["row"])
            let n = len(ln)
            let c = cur["col"]
            while c < n and self.hl._is_alnum(substr(ln, c, 1)) { c = c + 1 }
            while c < n and not self.hl._is_alnum(substr(ln, c, 1)) { c = c + 1 }
            if c == cur["col"] and cur["row"]+1 < self.buf.nlines() {
                cur["row"] = cur["row"] + 1; cur["col"] = 0
            } else { cur["col"] = c }
            cur["anchor_row"] = cur["row"]; cur["anchor_col"] = cur["col"]
        }
        self._scroll()
    }

    // ---- copy / paste / kill ----

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
            self.set_status("Buffer vacio"); return
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
        let parts = split(text, "\n")
        let i = 0
        while i < len(parts) {
            let p = parts[i]
            let j = 0
            while j < len(p) {
                self.buf.insert_char(cur["row"], cur["col"], substr(p, j, 1))
                cur["col"] = cur["col"] + 1
                j = j + 1
            }
            if i+1 < len(parts) {
                self.buf.split_line(cur["row"], cur["col"])
                cur["row"] = cur["row"] + 1
                cur["col"] = 0
            }
            i = i + 1
        }
        cur["anchor_row"] = cur["row"]; cur["anchor_col"] = cur["col"]
    }

    fn cmd_kill_line(self) {
        // Solo aplica al cursor primario; con multi avisamos
        if self._has_multi() {
            self.set_status("Ctrl+K solo con un cursor")
            return
        }
        let p = self._primary()
        let ln = self.buf.line(p["row"])
        if p["col"] == len(ln) {
            self.kill_buffer = ln + "\n"
            self.buf.delete_line(p["row"])
            if p["row"] >= self.buf.nlines() { p["row"] = self.buf.nlines() - 1 }
            p["col"] = 0
        } else {
            self.kill_buffer = substr(ln, p["col"], len(ln) - p["col"])
            let cnt = len(ln) - p["col"]
            let i = 0
            while i < cnt { self.buf.delete_char(p["row"], p["col"]); i = i + 1 }
        }
        p["anchor_row"] = p["row"]; p["anchor_col"] = p["col"]
        self._recompute_state(0)
        self.full_redraw = true
    }

    // ---- buscar / ir a linea ----

    fn cmd_find(self, inp) {
        let needle = self._prompt(inp, "Buscar: ", self.last_search)
        if needle == null { return }
        if needle == "" { self.last_search = ""; self.full_redraw = true; return }
        self.last_search = needle
        self.cmd_find_next()
    }

    fn cmd_find_next(self) {
        if self.last_search == "" { self.set_status("Nada que buscar"); return }
        let p = self._primary()
        let r = p["row"]; let c = p["col"] + 1
        let n = self.buf.nlines()
        let i = 0
        while i < n {
            let ln = self.buf.line(r)
            let pos = find_str(ln, self.last_search, c)
            if pos != -1 {
                p["row"] = r; p["col"] = pos
                p["anchor_row"] = r; p["anchor_col"] = pos
                self._scroll(); self.full_redraw = true
                self.set_status("Encontrado en " + str(r+1) + ":" + str(pos+1))
                return
            }
            r = r + 1
            if r >= n { r = 0 }
            c = 0; i = i + 1
        }
        self.set_status("No encontrado: " + self.last_search)
    }

    fn cmd_goto(self, inp) {
        let s = self._prompt(inp, "Ir a linea: ", "")
        if s == null or s == "" { return }
        if not is_numeric(s) { self.set_status("Numero invalido"); return }
        let n = int(s) - 1
        if n < 0 { n = 0 }
        if n >= self.buf.nlines() { n = self.buf.nlines() - 1 }
        let p = self._primary()
        p["row"] = n; p["col"] = 0
        p["anchor_row"] = n; p["anchor_col"] = 0
        self._scroll(); self.full_redraw = true
    }

    // ---- guardar ----

    fn cmd_save(self)    { return self._do_save(false) }
    fn cmd_save_as(self) { return self._do_save(true) }

    fn _do_save(self, force_prompt) {
        if self.filename == "" or force_prompt {
            if self.input_backend == null {
                self.set_status("Sin input backend")
                return false
            }
            let new_name = self._prompt(self.input_backend, "Guardar como: ", self.filename)
            if new_name == null { self.set_status("Cancelado"); return false }
            if new_name == ""   { self.set_status("Nombre vacio"); return false }
            self.filename = new_name
        }
        try {
            write_file(self.filename, self.buf.to_text())
            self.buf.modified = false
            self.set_status("Guardado: " + self.filename)
            return true
        } catch e {
            self.set_status("Error: " + str(e))
            return false
        }
    }

    // ---- ejecutar script ----

    fn cmd_run_script(self) {
        if not self._do_save(false) {
            self.set_status("Guarda antes de ejecutar")
            return
        }
        print(ANSI["CLEAR"] + ESC_CUR_SHOW)
        print("Ejecutando: vm --script " + self.filename + "\n")
        print(self.hl.col_ruler + repeat("-", 60) + ANSI["RESET"] + "\n")
        let r = shell_ex("vm --script " + self.filename)
        print(r["output"])
        print(self.hl.col_ruler + repeat("-", 60) + ANSI["RESET"] + "\n")
        print("Salida: " + str(r["code"]) + ".  Pulsa cualquier tecla para volver...")
        self.input_backend.read_key_blocking()
        self.full_redraw = true
    }

    // ---- prompt en linea de status ----

    fn _prompt(self, inp, prompt, def) {
        let ans = def
        let cancelled = false
        while true {
            print(ansi_cursor_pos(self.term_h, 1))
            print(ANSI["REVERSE"])
            let line = " " + prompt + ans + " "
            if len(line) < self.term_w {
                line = line + repeat(" ", self.term_w - len(line))
            } else { line = substr(line, 0, self.term_w) }
            print(line); print(ANSI["RESET"])
            print(ansi_cursor_pos(self.term_h, 1 + 1 + len(prompt) + len(ans)))
            print(ESC_CUR_SHOW)

            let k = inp.read_key_blocking()
            if k == KEY_ESC      { cancelled = true; break }
            if k == KEY_ENTER    { break }
            if k == KEY_BACKSPACE or k == KEY_DELETE {
                if len(ans) > 0 { ans = substr(ans, 0, len(ans)-1) }
                continue
            }
            if k >= 32 and k < 127 { ans = ans + from_char(k); continue }
        }
        self.full_redraw = true
        if cancelled { return null }
        return ans
    }

    // ---- bucle principal ----

    fn run(self, inp) {
        self.input_backend = inp
        print(ANSI["CLEAR"])
        self.full_redraw = true
        self.render()

        while true {
            let k = inp.read_key_blocking()

            // Salir
            if k == KEY_CTRL_Q {
                if self.buf.modified {
                    let ans = self._prompt(inp, "Cambios sin guardar. Salir? (y/N): ", "")
                    if ans == null { continue }
                    if lower(ans) != "y" { continue }
                }
                break
            }

            // Esc colapsa multi-cursor; si no hay multi, lo trato como nada
            if k == KEY_ESC {
                if self._has_multi() { self.cmd_collapse_cursors() 
                } elif self._any_selection() {
                    for cur in self.cursors {
                        cur["anchor_row"] = cur["row"]
                        cur["anchor_col"] = cur["col"]
                    }
                    self.full_redraw = true
                }
                self.render(); continue
            }

            // Acciones
            if k == KEY_CTRL_S { self.cmd_save()
            } elif k == KEY_CTRL_T { self.cmd_shell(inp)
            } elif k == KEY_CTRL_SPACE { self.cmd_autocomplete(inp)
            } elif k == KEY_CTRL_E { self.cmd_run_script()
            } elif k == KEY_CTRL_W { self.cmd_find(inp)
            } elif k == KEY_F3      { self.cmd_find_next()
            } elif k == KEY_CTRL_G  { self.cmd_goto(inp)
            } elif k == KEY_CTRL_Z  {
                if self.buf.undo() == null { self.set_status("Nada que deshacer") 
                } else { self.full_redraw = true; self._recompute_state(0) }
            } elif k == KEY_CTRL_Y or k == KEY_CTRL_R {
                if self.buf.redo() == null { self.set_status("Nada que rehacer") 
                } else { self.full_redraw = true; self._recompute_state(0) }
            } elif k == KEY_CTRL_K  { self.cmd_kill_line()
            } elif k == KEY_CTRL_U  { self.cmd_paste()
            } elif k == KEY_CTRL_C  { self.cmd_copy_selection()
            } elif k == KEY_CTRL_V  { self.cmd_paste()
            } elif k == KEY_CTRL_L  {
                self.show_lineno = not self.show_lineno
                self.full_redraw = true

            // Multicursor
            } elif k == KEY_CTRL_UP    { self.cmd_add_cursor_above()
            } elif k == KEY_CTRL_DOWN  { self.cmd_add_cursor_below()

            // Movimiento
            } elif k == KEY_UP    { self.cmd_move(-1, 0, false)
            } elif k == KEY_DOWN  { self.cmd_move( 1, 0, false)
            } elif k == KEY_LEFT  { self.cmd_move( 0,-1, false)
            } elif k == KEY_RIGHT { self.cmd_move( 0, 1, false)
            } elif k == KEY_HOME  { self.cmd_home(false)
            } elif k == KEY_END   { self.cmd_end(false)
            } elif k == KEY_PGUP  { self.cmd_pgup()
            } elif k == KEY_PGDN  { self.cmd_pgdn()
            } elif k == KEY_CTRL_LEFT  { self.cmd_word_left()
            } elif k == KEY_CTRL_RIGHT { self.cmd_word_right()

            // Movimiento + seleccion
            } elif k == KEY_SHIFT_UP    { self.cmd_move(-1, 0, true)
            } elif k == KEY_SHIFT_DOWN  { self.cmd_move( 1, 0, true)
            } elif k == KEY_SHIFT_LEFT  { self.cmd_move( 0,-1, true)
            } elif k == KEY_SHIFT_RIGHT { self.cmd_move( 0, 1, true)
            } elif k == KEY_SHIFT_HOME  { self.cmd_home(true)
            } elif k == KEY_SHIFT_END   { self.cmd_end(true)

            // Edicion
            } elif k == KEY_ENTER  { self.cmd_enter()
            } elif k == KEY_TAB    { self.cmd_tab()
            } elif k == KEY_BACKSPACE or k == KEY_DELETE { self.cmd_backspace()
            } elif k == KEY_DEL    { self.cmd_delete_forward()

            // Caracter imprimible
            } elif k >= 32 and k < 127 { self.cmd_insert_char(from_char(k)) }
            // Resto: ignorar

            self._scroll()
            self.render()
        }

        print(ANSI["CLEAR"] + ESC_CUR_SHOW + ANSI["RESET"])
    }
}


// =============================================================================
// SECCION 7: Main
// =============================================================================
fn main() {
    print("Fichero a editar (Enter para nuevo): ")
    let fname = trim(input(""))

    let inp = InputBackend()
    let editor = Editor(fname)

    editor.run(inp)
    inp.shutdown()
}
/*fn main() {
    print("Fichero a editar (Enter para nuevo): ")
    let fname = trim(input(""))

    let inp = InputBackend()
    let editor = Editor(fname)

    try {
        editor.run(inp)
    } catch e {
        inp.shutdown()
        print(ANSI["CLEAR"] + ESC_CUR_SHOW + ANSI["RESET"])
        println("Error: " + str(e))
        return
    }
    inp.shutdown()
}*/

main()
