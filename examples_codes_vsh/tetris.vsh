// =============================================================================
// TETRIS TUI
// =============================================================================
// Pruebalo:
//   vesta --script tetris.vsh
//
// Controles:
//   Flechas Izq/Der    mover pieza
//   Flecha Abajo       soft drop (caer mas rapido)
//   Flecha Arriba / X  rotar horario
//   Z                  rotar antihorario
//   Espacio            hard drop (caida instantanea)
//   C                  hold (guardar pieza para despues)
//   P                  pausa
//   Q / Esc            salir
//
// Config: edita .tetris.json en el cwd, o crea uno con:
//   {
//     "use_unicode": true
//   }
// =============================================================================


// =============================================================================
// Constantes
// =============================================================================

let ESC          = substr(ANSI["CLEAR_LINE"], 0, 1)
let ESC_HOME     = ESC + "[H"
let ESC_CUR_HIDE = ESC + "[?25l"
let ESC_CUR_SHOW = ESC + "[?25h"

let BOARD_W = 10
let BOARD_H = 20

// Frame del bucle principal: 60 FPS para input responsivo
let FRAME_MS = 16

// Velocidades de caida (gravity) por nivel, en ms entre cada caida natural.
// Curva oficial estilo Tetris: en niveles altos baja muy rapido.
fn gravity_ms_for_level(level) {
    if level <= 1  { return 800 }
    if level == 2  { return 720 }
    if level == 3  { return 630 }
    if level == 4  { return 550 }
    if level == 5  { return 470 }
    if level == 6  { return 380 }
    if level == 7  { return 300 }
    if level == 8  { return 220 }
    if level == 9  { return 130 }
    if level == 10 { return 100 }
    if level <= 13 { return 80 }
    if level <= 16 { return 60 }
    if level <= 19 { return 40 }
    return 30   // nivel 20+: muy rapido
}

// Soft drop: cae N veces mas rapido que la gravedad normal
let SOFT_DROP_DIVISOR = 20

// Codigos de tecla
let KEY_UP    = -1000
let KEY_DOWN  = -1001
let KEY_LEFT  = -1002
let KEY_RIGHT = -1003
let KEY_ESC   = 27

// Tipos de pieza (tetromino)
let PIECE_I = 0
let PIECE_O = 1
let PIECE_T = 2
let PIECE_S = 3
let PIECE_Z = 4
let PIECE_L = 5
let PIECE_J = 6
let N_PIECES = 7

// Estados del juego
let STATE_SPLASH    = 0
let STATE_READY     = 1
let STATE_PLAYING   = 2
let STATE_LINE_CLEAR = 3
let STATE_GAME_OVER = 4

let READY_MS       = 1500
let LINE_CLEAR_MS  = 400      // animacion de lineas eliminadas
let GAME_OVER_MS   = 5000

// Lock delay: tras tocar suelo, esperamos este tiempo antes de bloquear
// (permite mover/rotar la pieza un poco mas)
let LOCK_DELAY_MS  = 500

// Scoring oficial
fn line_score(n_lines, level) {
    let base = 0
    if n_lines == 1 { base = 100  }
    if n_lines == 2 { base = 300  }
    if n_lines == 3 { base = 500  }
    if n_lines == 4 { base = 800  }   // Tetris
    return base * level
}

let LINES_PER_LEVEL = 10

// Persistencia
let HIGH_SCORE_PATH = ".tetris_score.json"


// =============================================================================
// Config
// =============================================================================

class Config {
    fn __init__(self) {
        self.use_unicode = true
        self.path = ".tetris.json"
        self._load()
    }
    fn _load(self) {
        if not exists(self.path) { return }
        try {
            let txt = read_file(self.path)
            let pos = find_str(txt, "\"use_unicode\"", 0)
            if pos == -1 { return }
            let rest = substr(txt, pos, len(txt) - pos)
            let pt = find_str(rest, "true", 0)
            let pf = find_str(rest, "false", 0)
            if pf != -1 and (pt == -1 or pf < pt) {
                self.use_unicode = false
            } elif pt != -1 {
                self.use_unicode = true
            }
        } catch e { }
    }
}


// =============================================================================
// High score persistente
// =============================================================================

fn load_high_score() {
    if not exists(HIGH_SCORE_PATH) { return 0 }
    try {
        let txt = read_file(HIGH_SCORE_PATH)
        let i = 0
        let n = len(txt)
        while i < n {
            let cc = char_code(substr(txt, i, 1))
            if cc >= 48 and cc <= 57 {
                let start = i
                while i < n {
                    let c2 = char_code(substr(txt, i, 1))
                    if c2 < 48 or c2 > 57 { break }
                    i = i + 1
                }
                return int(substr(txt, start, i - start))
            }
            i = i + 1
        }
    } catch e { }
    return 0
}

fn save_high_score(score) {
    let txt = "{\n  \"high_score\": " + str(score) + "\n}\n"
    try {
        write_file(HIGH_SCORE_PATH, txt)
        return true
    } catch e {
        return false
    }
}


// =============================================================================
// PRNG simple (LCG)
// =============================================================================

class Rng {
    fn __init__(self, seed) {
        self.s = seed
        if self.s == 0 { self.s = 12345 }
    }
    fn next(self) {
        self.s = (self.s * 1103515245 + 12345) % 2147483648
        return self.s
    }
    fn range(self, lo, hi) {
        let span = hi - lo
        if span <= 0 { return lo }
        let r = self.next()
        if r < 0 { r = -r }
        return lo + (r % span)
    }
}


// =============================================================================
// Definicion de las 7 piezas (tetrominos)
// =============================================================================
// Cada pieza tiene 4 rotaciones (0, 90, 180, 270) representadas como
// matrices 4x4. Cada matriz es una lista de 16 ints (0=vacio, 1=lleno).
//
// Spawn position canonica: arriba del centro del tablero.

fn piece_shapes(kind) {
    // I-piece (cyan)
    if kind == PIECE_I {
        return [
            [0,0,0,0,
             1,1,1,1,
             0,0,0,0,
             0,0,0,0],
            [0,0,1,0,
             0,0,1,0,
             0,0,1,0,
             0,0,1,0],
            [0,0,0,0,
             0,0,0,0,
             1,1,1,1,
             0,0,0,0],
            [0,1,0,0,
             0,1,0,0,
             0,1,0,0,
             0,1,0,0]
        ]
    }
    // O-piece (yellow): no rota visualmente
    if kind == PIECE_O {
        let base = [0,1,1,0,
                    0,1,1,0,
                    0,0,0,0,
                    0,0,0,0]
        return [base, base, base, base]
    }
    // T-piece (purple)
    if kind == PIECE_T {
        return [
            [0,1,0,0,
             1,1,1,0,
             0,0,0,0,
             0,0,0,0],
            [0,1,0,0,
             0,1,1,0,
             0,1,0,0,
             0,0,0,0],
            [0,0,0,0,
             1,1,1,0,
             0,1,0,0,
             0,0,0,0],
            [0,1,0,0,
             1,1,0,0,
             0,1,0,0,
             0,0,0,0]
        ]
    }
    // S-piece (green)
    if kind == PIECE_S {
        return [
            [0,1,1,0,
             1,1,0,0,
             0,0,0,0,
             0,0,0,0],
            [0,1,0,0,
             0,1,1,0,
             0,0,1,0,
             0,0,0,0],
            [0,0,0,0,
             0,1,1,0,
             1,1,0,0,
             0,0,0,0],
            [1,0,0,0,
             1,1,0,0,
             0,1,0,0,
             0,0,0,0]
        ]
    }
    // Z-piece (red)
    if kind == PIECE_Z {
        return [
            [1,1,0,0,
             0,1,1,0,
             0,0,0,0,
             0,0,0,0],
            [0,0,1,0,
             0,1,1,0,
             0,1,0,0,
             0,0,0,0],
            [0,0,0,0,
             1,1,0,0,
             0,1,1,0,
             0,0,0,0],
            [0,1,0,0,
             1,1,0,0,
             1,0,0,0,
             0,0,0,0]
        ]
    }
    // L-piece (orange)
    if kind == PIECE_L {
        return [
            [0,0,1,0,
             1,1,1,0,
             0,0,0,0,
             0,0,0,0],
            [0,1,0,0,
             0,1,0,0,
             0,1,1,0,
             0,0,0,0],
            [0,0,0,0,
             1,1,1,0,
             1,0,0,0,
             0,0,0,0],
            [1,1,0,0,
             0,1,0,0,
             0,1,0,0,
             0,0,0,0]
        ]
    }
    // J-piece (blue)
    if kind == PIECE_J {
        return [
            [1,0,0,0,
             1,1,1,0,
             0,0,0,0,
             0,0,0,0],
            [0,1,1,0,
             0,1,0,0,
             0,1,0,0,
             0,0,0,0],
            [0,0,0,0,
             1,1,1,0,
             0,0,1,0,
             0,0,0,0],
            [0,1,0,0,
             0,1,0,0,
             1,1,0,0,
             0,0,0,0]
        ]
    }
    // Default
    return piece_shapes(PIECE_I)
}

fn piece_letter(kind) {
    if kind == PIECE_I { return "I" }
    if kind == PIECE_O { return "O" }
    if kind == PIECE_T { return "T" }
    if kind == PIECE_S { return "S" }
    if kind == PIECE_Z { return "Z" }
    if kind == PIECE_L { return "L" }
    if kind == PIECE_J { return "J" }
    return "?"
}


// =============================================================================
// 7-bag generator
// =============================================================================
// Genera piezas en bolsas de 7 (cada tipo aparece 1 vez por bolsa) en orden
// aleatorio. Garantiza distribucion uniforme y evita largas rachas.

class BagGenerator {
    fn __init__(self, rng) {
        self.rng = rng
        self.queue = []
        self._refill()
    }
    fn _refill(self) {
        let items = [PIECE_I, PIECE_O, PIECE_T, PIECE_S, PIECE_Z, PIECE_L, PIECE_J]
        // Fisher-Yates shuffle
        let i = N_PIECES - 1
        while i > 0 {
            let j = self.rng.range(0, i + 1)
            let tmp = items[i]
            items[i] = items[j]
            items[j] = tmp
            i = i - 1
        }
        for p in items { append(self.queue, p) }
    }
    fn next(self) {
        if len(self.queue) == 0 { self._refill() }
        let p = self.queue[0]
        let new_q = []
        let i = 1
        while i < len(self.queue) {
            append(new_q, self.queue[i])
            i = i + 1
        }
        self.queue = new_q
        return p
    }
    fn peek(self, n) {
        // Devuelve los proximos n tipos sin consumir
        while len(self.queue) < n { self._refill() }
        let out = []
        let i = 0
        while i < n {
            append(out, self.queue[i])
            i = i + 1
        }
        return out
    }
}


// =============================================================================
// Pieza activa
// =============================================================================

class Piece {
    fn __init__(self, kind) {
        self.kind = kind
        self.rot = 0
        // Posicion top-left de la matriz 4x4
        self.col = 3
        self.row = 0
        // I-piece spawn un poquito mas a la izquierda
        if kind == PIECE_I { self.col = 3 }
    }

    // Devuelve la matriz 4x4 de la rotacion actual
    fn shape(self) {
        let shapes = piece_shapes(self.kind)
        return shapes[self.rot]
    }

    // Lista de [col, row] de las celdas ocupadas en el tablero (absolutas)
    fn cells(self) {
        let m = self.shape()
        let out = []
        let i = 0
        while i < 16 {
            if m[i] == 1 {
                let dx = i % 4
                let dy = i / 4
                append(out, [self.col + dx, self.row + dy])
            }
            i = i + 1
        }
        return out
    }

    fn clone(self) {
        let p = Piece(self.kind)
        p.rot = self.rot
        p.col = self.col
        p.row = self.row
        return p
    }
}


// =============================================================================
// Tablero
// =============================================================================
// grid: matriz BOARD_H x BOARD_W de ints. 0 = vacio, 1..7 = ocupado por
// pieza (kind+1, asi 0 sigue siendo vacio).

class Board {
    fn __init__(self) {
        self.w = BOARD_W
        self.h = BOARD_H
        self._reset_grid()
    }
    fn _reset_grid(self) {
        self.grid = []
        let r = 0
        while r < self.h {
            let row = []
            let c = 0
            while c < self.w {
                append(row, 0)
                c = c + 1
            }
            append(self.grid, row)
            r = r + 1
        }
    }
    fn reset(self) {
        self._reset_grid()
    }

    fn at(self, col, row) {
        if row < 0 or row >= self.h { return -1 }
        if col < 0 or col >= self.w { return -1 }
        return self.grid[row][col]
    }
    fn set(self, col, row, v) {
        if row < 0 or row >= self.h { return }
        if col < 0 or col >= self.w { return }
        self.grid[row][col] = v
    }

    // ¿Una celda esta libre y dentro del tablero?
    // Las filas con row<0 son OK (la pieza puede empezar parcialmente arriba)
    fn is_free(self, col, row) {
        if col < 0 or col >= self.w { return false }
        if row >= self.h { return false }
        if row < 0 { return true }     // arriba del tablero: ok
        return self.grid[row][col] == 0
    }

    // ¿Es valida la posicion de una pieza? (todas sus celdas libres)
    fn fits(self, piece) {
        let cells = piece.cells()
        for c in cells {
            if not self.is_free(c[0], c[1]) { return false }
        }
        return true
    }

    // Bloquea una pieza en el tablero (cuando aterriza)
    fn lock_piece(self, piece) {
        let cells = piece.cells()
        let val = piece.kind + 1
        for c in cells {
            if c[1] >= 0 { self.set(c[0], c[1], val) }
        }
    }

    // Detecta filas completas y devuelve la lista de indices
    fn full_rows(self) {
        let rows = []
        let r = 0
        while r < self.h {
            let full = true
            let c = 0
            while c < self.w {
                if self.grid[r][c] == 0 { full = false; break }
                c = c + 1
            }
            if full { append(rows, r) }
            r = r + 1
        }
        return rows
    }

    // Elimina filas (se desplazan las superiores). rows debe estar ordenada asc.
    fn clear_rows(self, rows) {
        if len(rows) == 0 { return }
        // Construir nuevo grid: copiamos de abajo hacia arriba saltando las
        // filas eliminadas, y rellenamos arriba con filas vacias.
        let new_grid = []
        // Primero, las filas vacias arriba (tantas como rows eliminadas)
        let n_clear = len(rows)
        let i = 0
        while i < n_clear {
            let empty_row = []
            let c = 0
            while c < self.w {
                append(empty_row, 0)
                c = c + 1
            }
            append(new_grid, empty_row)
            i = i + 1
        }
        // Luego, las filas que se conservan
        let r = 0
        while r < self.h {
            let is_cleared = false
            for cr in rows {
                if cr == r { is_cleared = true; break }
            }
            if not is_cleared {
                append(new_grid, self.grid[r])
            }
            r = r + 1
        }
        self.grid = new_grid
    }
}


// =============================================================================
// Wall kicks (rotacion con desplazamiento si choca)
// =============================================================================
// SRS simplificado: probamos la rotacion en posicion neutra y luego
// con offsets (-1,0), (+1,0), (0,-1), (-2,0), (+2,0). Si alguno cabe,
// aplicamos. Si no, no rotamos.

fn try_rotate(piece, board, dir) {
    // dir: +1 = horario, -1 = antihorario
    let new_rot = piece.rot + dir
    if new_rot < 0 { new_rot = 3 }
    if new_rot > 3 { new_rot = 0 }

    let kicks = [
        [ 0,  0],
        [-1,  0],
        [ 1,  0],
        [ 0, -1],
        [-2,  0],
        [ 2,  0]
    ]

    let test = piece.clone()
    test.rot = new_rot
    for k in kicks {
        test.col = piece.col + k[0]
        test.row = piece.row + k[1]
        if board.fits(test) {
            piece.col = test.col
            piece.row = test.row
            piece.rot = new_rot
            return true
        }
    }
    return false
}


// =============================================================================
// Input non-blocking
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
    fn poll(self) {
        if self.os == "windows" {
            let n = ffi_call(self.kbhit_sym)
            if n == 0 { return -1 }
            let c = ffi_call(self.getch_sym)
            if c == 0 or c == 224 {
                let c2 = ffi_call(self.getch_sym)
                if c2 == 72 { return KEY_UP    }
                if c2 == 80 { return KEY_DOWN  }
                if c2 == 75 { return KEY_LEFT  }
                if c2 == 77 { return KEY_RIGHT }
                return -2
            }
            return c
        } else {
            let c = ffi_call(self.getchar_sym)
            if c == -1 { return -1 }
            if c == 27 {
                let c1 = ffi_call(self.getchar_sym)
                if c1 == -1 { return KEY_ESC }
                if c1 != 91 { return KEY_ESC }
                let c2 = ffi_call(self.getchar_sym)
                if c2 == 65 { return KEY_UP }
                if c2 == 66 { return KEY_DOWN }
                if c2 == 67 { return KEY_RIGHT }
                if c2 == 68 { return KEY_LEFT }
                return -2
            }
            return c
        }
    }
    fn shutdown(self) {
        if self.os != "windows" {
            shell("stty icanon echo ixon ixoff")
        }
        if self.lib != 0 { ffi_close(self.lib) }
    }
}


// =============================================================================
// TermSize
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
    }
    fn _all_numbers(self, s) {
        let nums = []
        let i = 0
        while i < len(s) {
            let cc = char_code(substr(s, i, 1))
            if cc >= 48 and cc <= 57 {
                let start = i
                while i < len(s) {
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
// Renderer
// =============================================================================

class Renderer {
    fn __init__(self, config) {
        self.config = config
        self._out = ""
        self.full_redraw = true
        self._setup_chars()
        self._setup_colors()
    }
    fn _w(self, s) { self._out = self._out + s }
    fn _flush(self) {
        if self._out != "" {
            print(self._out)
            self._out = ""
        }
        try { flush_output() } catch e { }
    }

    fn _setup_chars(self) {
        let u = self.config.use_unicode
        if u {
            // Bloque para celdas ocupadas
            self.block       = from_char(226) + from_char(150) + from_char(136)  // █
            // Marco
            self.frame_h     = from_char(226) + from_char(149) + from_char(144)  // ═
            self.frame_v     = from_char(226) + from_char(149) + from_char(145)  // ║
            self.frame_tl    = from_char(226) + from_char(149) + from_char(148)  // ╔
            self.frame_tr    = from_char(226) + from_char(149) + from_char(151)  // ╗
            self.frame_bl    = from_char(226) + from_char(149) + from_char(154)  // ╚
            self.frame_br    = from_char(226) + from_char(149) + from_char(157)  // ╝
            // Ghost piece (donde aterrizara): bloque hueco
            self.ghost_block = from_char(226) + from_char(150) + from_char(145)  // ▒
        } else {
            self.block       = "#"
            self.frame_h     = "="
            self.frame_v     = "|"
            self.frame_tl    = "+"
            self.frame_tr    = "+"
            self.frame_bl    = "+"
            self.frame_br    = "+"
            self.ghost_block = "."
        }
    }

    fn _setup_colors(self) {
        // Colores oficiales de cada pieza (Tetris guideline)
        self.col_I = ansi_rgb( 80, 220, 230)    // cyan
        self.col_O = ansi_rgb(240, 220,  60)    // yellow
        self.col_T = ansi_rgb(170,  80, 200)    // purple
        self.col_S = ansi_rgb( 80, 220,  80)    // green
        self.col_Z = ansi_rgb(230,  70,  70)    // red
        self.col_L = ansi_rgb(240, 150,  60)    // orange
        self.col_J = ansi_rgb( 70, 100, 230)    // blue

        self.col_frame  = ansi_rgb(150, 150, 200)
        self.col_text   = ansi_rgb(255, 255, 255)
        self.col_score  = ansi_rgb(255, 255,   0)
        self.col_dim    = ansi_rgb(120, 120, 120)
        self.col_ghost  = ansi_rgb( 90,  90,  90)
        self.R = ANSI["RESET"]
    }

    fn piece_color(self, kind) {
        if kind == PIECE_I { return self.col_I }
        if kind == PIECE_O { return self.col_O }
        if kind == PIECE_T { return self.col_T }
        if kind == PIECE_S { return self.col_S }
        if kind == PIECE_Z { return self.col_Z }
        if kind == PIECE_L { return self.col_L }
        if kind == PIECE_J { return self.col_J }
        return self.col_text
    }

    fn cell_pos(self, ox, oy, col, row) {
        return ansi_cursor_pos(oy + row, ox + col * 2)
    }

    fn draw_block(self, ox, oy, col, row, color) {
        self._w(self.cell_pos(ox, oy, col, row))
        self._w(color + self.block + self.block + self.R)
    }
    fn draw_ghost(self, ox, oy, col, row) {
        self._w(self.cell_pos(ox, oy, col, row))
        self._w(self.col_ghost + self.ghost_block + self.ghost_block + self.R)
    }
    fn clear_cell(self, ox, oy, col, row) {
        self._w(self.cell_pos(ox, oy, col, row))
        self._w("  ")
    }

    // Marco alrededor del tablero
    fn draw_board_frame(self, ox, oy) {
        let inner_w = BOARD_W * 2
        // Top
        self._w(ansi_cursor_pos(oy - 1, ox - 2))
        let top = self.col_frame + self.frame_tl + self.frame_tl
        let i = 0
        while i < inner_w { top = top + self.frame_h; i = i + 1 }
        top = top + self.frame_tr + self.frame_tr + self.R
        self._w(top)
        // Bottom
        self._w(ansi_cursor_pos(oy + BOARD_H, ox - 2))
        let bot = self.col_frame + self.frame_bl + self.frame_bl
        let j = 0
        while j < inner_w { bot = bot + self.frame_h; j = j + 1 }
        bot = bot + self.frame_br + self.frame_br + self.R
        self._w(bot)
        // Lados
        let r = 0
        while r < BOARD_H {
            self._w(ansi_cursor_pos(oy + r, ox - 2))
            self._w(self.col_frame + self.frame_v + self.frame_v + self.R)
            self._w(ansi_cursor_pos(oy + r, ox + inner_w))
            self._w(self.col_frame + self.frame_v + self.frame_v + self.R)
            r = r + 1
        }
    }

    fn draw_panel_frame(self, ox, oy, w_cells, h_cells, title) {
        let inner_w = w_cells * 2
        self._w(ansi_cursor_pos(oy - 1, ox - 2))
        let top = self.col_frame + self.frame_tl + self.frame_tl
        let i = 0
        while i < inner_w { top = top + self.frame_h; i = i + 1 }
        top = top + self.frame_tr + self.frame_tr + self.R
        self._w(top)

        self._w(ansi_cursor_pos(oy + h_cells, ox - 2))
        let bot = self.col_frame + self.frame_bl + self.frame_bl
        let j = 0
        while j < inner_w { bot = bot + self.frame_h; j = j + 1 }
        bot = bot + self.frame_br + self.frame_br + self.R
        self._w(bot)

        let r = 0
        while r < h_cells {
            self._w(ansi_cursor_pos(oy + r, ox - 2))
            self._w(self.col_frame + self.frame_v + self.frame_v + self.R)
            self._w(ansi_cursor_pos(oy + r, ox + inner_w))
            self._w(self.col_frame + self.frame_v + self.frame_v + self.R)
            r = r + 1
        }

        // Titulo en la parte superior del marco
        if title != "" {
            self._w(ansi_cursor_pos(oy - 1, ox + 1))
            self._w(self.col_text + " " + title + " " + self.R)
        }
    }

    // Dibuja una pieza en un panel (Hold/Next), centrandola en w_cells x 4
    fn draw_piece_in_panel(self, kind, ox, oy, w_cells) {
        if kind < 0 { return }
        let m = piece_shapes(kind)[0]
        let color = self.piece_color(kind)
        let i = 0
        while i < 16 {
            if m[i] == 1 {
                let dx = i % 4
                let dy = i / 4
                // Centrado: en w_cells x 4, la pieza ocupa 4x4
                let cx = (w_cells - 4) / 2
                self.draw_block(ox, oy, cx + dx, dy, color)
            }
            i = i + 1
        }
    }

    fn draw_text_at(self, x, y, text, color) {
        self._w(ansi_cursor_pos(y, x))
        self._w(color + text + self.R)
    }

    fn draw_centered(self, text, color, ox, oy, in_row) {
        let inner_w = BOARD_W * 2
        let x = ox + (inner_w - len(text)) / 2
        self._w(ansi_cursor_pos(oy + in_row, x))
        self._w(color + ANSI["BOLD"] + text + self.R)
    }
}


// =============================================================================
// Game
// =============================================================================

class Game {
    fn __init__(self, config) {
        self.config = config
        self.rng = Rng(time_ms() % 2147483647)
        self.bag = BagGenerator(self.rng)
        self.board = Board()
        self.renderer = Renderer(config)

        self.score = 0
        self.persistent_high_score = load_high_score()
        self.high_score = self.persistent_high_score
        self.is_new_high_score = false
        self.level = 1
        self.lines_cleared_total = 0
        self.paused = false
        self.running = true

        // Pieza activa y hold
        self.active = null
        self.hold_kind = -1            // -1 = no hay
        self.hold_used_this_drop = false

        // Timing
        self.gravity_accum_ms = 0
        self.lock_timer_ms    = 0       // se activa cuando la pieza toca suelo
        self.last_tick_ms = time_ms()
        self.soft_drop = false

        // Animacion line clear
        self.lines_to_clear = []
        self.line_clear_timer_ms = 0

        // Estado de juego
        self.state = STATE_SPLASH
        self.state_timer_ms = 0

        let ts = TermSize()
        self.term_w = ts.cols
        self.term_h = ts.rows
        // Layout: tablero centrado, paneles a los lados
        // Tablero: 10*2 = 20 chars de ancho + 4 chars de marco = 24
        // Panel (Hold/Next): 6 chars de cells = 12 chars + 4 marco = 16
        // Total horizontal: 16 (hold) + gap + 24 (board) + gap + 16 (next) = ~60
        let board_w = BOARD_W * 2
        let panel_w = 6 * 2
        let total_w = panel_w + 4 + board_w + 4 + panel_w + 4
        let left = (self.term_w - total_w) / 2
        if left < 4 { left = 4 }

        self.hold_ox = left + 2
        self.board_ox = self.hold_ox + panel_w + 4 + 2
        self.next_ox  = self.board_ox + board_w + 4 + 2

        self.board_oy = (self.term_h - BOARD_H) / 2
        if self.board_oy < 3 { self.board_oy = 3 }
        self.hold_oy = self.board_oy + 1
        self.next_oy = self.board_oy + 1
    }

    // -------------------------------------------------------------------------
    // Input
    // -------------------------------------------------------------------------

    fn handle_key(self, k) {
        if k == 113 or k == 81 or k == KEY_ESC { self.running = false; return }

        if self.state == STATE_SPLASH {
            self._start_new_game()
            return
        }
        if self.state == STATE_GAME_OVER { return }

        if k == 112 or k == 80 {
            self.paused = not self.paused
            return
        }
        if self.paused { return }
        if self.state != STATE_PLAYING { return }
        if self.active == null { return }

        // Movimiento horizontal
        if k == KEY_LEFT or k == 65 or k == 97 {
            self._try_move(-1, 0)
            return
        }
        if k == KEY_RIGHT or k == 68 or k == 100 {
            self._try_move(1, 0)
            return
        }
        // Soft drop (mantener flecha abajo acelera). Como nuestro input es
        // discreto (no detectamos "tecla suelta"), interpretamos cada pulsacion
        // como un paso de drop adicional.
        if k == KEY_DOWN or k == 83 or k == 115 {
            self._try_move(0, 1)
            // pequeno bonus de score por soft drop manual
            self.score = self.score + 1
            return
        }
        // Rotacion horaria
        if k == KEY_UP or k == 87 or k == 119 or k == 120 or k == 88 {
            // x/X tambien rota horario
            try_rotate(self.active, self.board, 1)
            self.lock_timer_ms = 0   // resetear lock delay tras rotar
            return
        }
        // Rotacion antihoraria
        if k == 122 or k == 90 {   // z/Z
            try_rotate(self.active, self.board, -1)
            self.lock_timer_ms = 0
            return
        }
        // Hard drop
        if k == 32 {   // espacio
            self._hard_drop()
            return
        }
        // Hold
        if k == 99 or k == 67 {    // c/C
            self._do_hold()
            return
        }
    }

    fn _try_move(self, dc, dr) {
        if self.active == null { return false }
        let p = self.active.clone()
        p.col = p.col + dc
        p.row = p.row + dr
        if self.board.fits(p) {
            self.active.col = p.col
            self.active.row = p.row
            // Movimiento horizontal exitoso reinicia lock timer
            if dc != 0 { self.lock_timer_ms = 0 }
            return true
        }
        return false
    }

    fn _hard_drop(self) {
        if self.active == null { return }
        // Bajar hasta que no quepa
        let drop_count = 0
        while self._try_move(0, 1) {
            drop_count = drop_count + 1
        }
        // Bonus por hard drop: 2 puntos por celda
        self.score = self.score + drop_count * 2
        // Bloquear inmediatamente
        self._lock_active()
    }

    fn _do_hold(self) {
        if self.hold_used_this_drop { return }
        if self.active == null { return }
        let cur_kind = self.active.kind
        if self.hold_kind < 0 {
            // No habia hold: tomamos del bag para reemplazar
            self.hold_kind = cur_kind
            self.active = self._spawn_piece(self.bag.next())
        } else {
            // Intercambiar
            let prev = self.hold_kind
            self.hold_kind = cur_kind
            self.active = self._spawn_piece(prev)
        }
        self.hold_used_this_drop = true
        self.lock_timer_ms = 0
    }

    // -------------------------------------------------------------------------
    // Transiciones de estado
    // -------------------------------------------------------------------------

    fn _start_new_game(self) {
        self.score = 0
        self.high_score = self.persistent_high_score
        self.is_new_high_score = false
        self.level = 1
        self.lines_cleared_total = 0
        self.board.reset()
        self.bag = BagGenerator(self.rng)
        self.hold_kind = -1
        self.hold_used_this_drop = false
        self.active = self._spawn_piece(self.bag.next())
        self.gravity_accum_ms = 0
        self.lock_timer_ms = 0
        self.lines_to_clear = []
        self.line_clear_timer_ms = 0
        self.soft_drop = false
        self.state = STATE_READY
        self.state_timer_ms = READY_MS
        self.renderer.full_redraw = true
    }

    fn _on_game_over(self) {
        self.is_new_high_score = false
        if self.score > self.persistent_high_score {
            self.is_new_high_score = true
            self.persistent_high_score = self.score
            save_high_score(self.persistent_high_score)
        }
        self.state = STATE_GAME_OVER
        self.state_timer_ms = GAME_OVER_MS
    }

    // -------------------------------------------------------------------------
    // Logica de juego
    // -------------------------------------------------------------------------

    fn _spawn_piece(self, kind) {
        let p = Piece(kind)
        // Si en el spawn no cabe -> game over
        if not self.board.fits(p) {
            // No marcar game over aqui; lo marca _spawn_or_game_over
        }
        return p
    }

    fn _spawn_next(self) {
        let kind = self.bag.next()
        self.active = self._spawn_piece(kind)
        self.hold_used_this_drop = false
        if not self.board.fits(self.active) {
            // Game over
            self._on_game_over()
        }
    }

    // Bloquea la pieza actual, comprueba lineas, y spawnea la siguiente
    fn _lock_active(self) {
        if self.active == null { return }
        self.board.lock_piece(self.active)
        self.active = null
        self.lock_timer_ms = 0
        self.gravity_accum_ms = 0

        // Lineas completas?
        let rows = self.board.full_rows()
        if len(rows) > 0 {
            self.lines_to_clear = rows
            self.line_clear_timer_ms = LINE_CLEAR_MS
            self.state = STATE_LINE_CLEAR
            // El score se calcula al terminar la animacion
            return
        }
        // Si no hay lineas, spawn directo
        self._spawn_next()
    }

    fn _finish_line_clear(self) {
        let n = len(self.lines_to_clear)
        let pts = line_score(n, self.level)
        self.score = self.score + pts
        if self.score > self.high_score { self.high_score = self.score }
        self.lines_cleared_total = self.lines_cleared_total + n
        // Subir nivel cada LINES_PER_LEVEL lineas
        let new_level = (self.lines_cleared_total / LINES_PER_LEVEL) + 1
        if new_level > self.level { self.level = new_level }
        self.board.clear_rows(self.lines_to_clear)
        self.lines_to_clear = []
        self.state = STATE_PLAYING
        self.renderer.full_redraw = true
        self._spawn_next()
    }

    // ¿Esta la pieza apoyada (no puede bajar)?
    fn _is_resting(self) {
        if self.active == null { return false }
        let p = self.active.clone()
        p.row = p.row + 1
        return not self.board.fits(p)
    }

    // -------------------------------------------------------------------------
    // Tick
    // -------------------------------------------------------------------------

    fn tick(self) {
        let now = time_ms()
        let elapsed = now - self.last_tick_ms
        self.last_tick_ms = now
        if self.paused { return }

        if self.state == STATE_SPLASH { return }

        if self.state == STATE_READY {
            self.state_timer_ms = self.state_timer_ms - elapsed
            if self.state_timer_ms <= 0 {
                self.state = STATE_PLAYING
                self.renderer.full_redraw = true
            }
            return
        }
        if self.state == STATE_GAME_OVER {
            self.state_timer_ms = self.state_timer_ms - elapsed
            if self.state_timer_ms <= 0 {
                self.state = STATE_SPLASH
                self.renderer.full_redraw = true
            }
            return
        }
        if self.state == STATE_LINE_CLEAR {
            self.line_clear_timer_ms = self.line_clear_timer_ms - elapsed
            if self.line_clear_timer_ms <= 0 {
                self._finish_line_clear()
            }
            return
        }

        // STATE_PLAYING
        if self.active == null { return }

        // Gravedad
        let gravity = gravity_ms_for_level(self.level)
        self.gravity_accum_ms = self.gravity_accum_ms + elapsed
        while self.gravity_accum_ms >= gravity {
            self.gravity_accum_ms = self.gravity_accum_ms - gravity
            // Intentar bajar 1
            let moved = self._try_move(0, 1)
            if not moved {
                // Tocando suelo: empezar lock delay
                break
            }
        }

        // Lock delay
        if self._is_resting() {
            self.lock_timer_ms = self.lock_timer_ms + elapsed
            if self.lock_timer_ms >= LOCK_DELAY_MS {
                self._lock_active()
            }
        } else {
            self.lock_timer_ms = 0
        }
    }

    // -------------------------------------------------------------------------
    // Render
    // -------------------------------------------------------------------------

    // Calcula la fila en la que aterrizaria la pieza activa (para ghost)
    fn _ghost_row(self) {
        if self.active == null { return -1 }
        let p = self.active.clone()
        while true {
            let next = p.clone()
            next.row = next.row + 1
            if not self.board.fits(next) { break }
            p.row = p.row + 1
        }
        return p.row
    }

    fn render(self) {
        let r = self.renderer

        if r.full_redraw {
            r._w(ANSI["CLEAR"])
            r._w(ESC_HOME)
            r._w(ESC_CUR_HIDE)
        }

        if self.state == STATE_SPLASH {
            self._render_splash()
            r.full_redraw = false
            r._flush()
            return
        }

        // Marcos
        if r.full_redraw {
            r.draw_board_frame(self.board_ox, self.board_oy)
            r.draw_panel_frame(self.hold_ox, self.hold_oy, 6, 4, "HOLD")
            r.draw_panel_frame(self.next_ox, self.next_oy, 6, 14, "NEXT")
        }

        // Limpiar interior del tablero (rapido, robusto)
        let rr = 0
        while rr < BOARD_H {
            r._w(ansi_cursor_pos(self.board_oy + rr, self.board_ox))
            let cc = 0
            while cc < BOARD_W {
                r._w("  ")
                cc = cc + 1
            }
            rr = rr + 1
        }

        // Pintar bloques bloqueados del tablero
        let row = 0
        while row < BOARD_H {
            let col = 0
            while col < BOARD_W {
                let v = self.board.at(col, row)
                if v > 0 {
                    let kind = v - 1
                    r.draw_block(self.board_ox, self.board_oy, col, row, r.piece_color(kind))
                }
                col = col + 1
            }
            row = row + 1
        }

        // Animacion de line clear: parpadeo en blanco
        if self.state == STATE_LINE_CLEAR {
            let phase = (self.line_clear_timer_ms / 100) % 2
            let col = ansi_rgb(255, 255, 255)
            if phase == 0 { col = ansi_rgb(180, 180, 180) }
            for lr in self.lines_to_clear {
                let cc = 0
                while cc < BOARD_W {
                    r.draw_block(self.board_ox, self.board_oy, cc, lr, col)
                    cc = cc + 1
                }
            }
        }

        // Ghost piece (donde aterrizaria) - solo durante PLAYING
        if self.state == STATE_PLAYING and self.active != null {
            let ghost_row = self._ghost_row()
            // Solo dibujamos ghost si es DISTINTA fila a la actual (si no, taparia)
            if ghost_row != self.active.row {
                let cells = self.active.cells()
                for c in cells {
                    let ghost_r = c[1] + (ghost_row - self.active.row)
                    if ghost_r >= 0 and ghost_r < BOARD_H {
                        r.draw_ghost(self.board_ox, self.board_oy, c[0], ghost_r)
                    }
                }
            }
        }

        // Pieza activa
        if self.active != null and (self.state == STATE_PLAYING or self.state == STATE_READY) {
            let color = r.piece_color(self.active.kind)
            let cells = self.active.cells()
            for c in cells {
                if c[1] >= 0 and c[1] < BOARD_H {
                    r.draw_block(self.board_ox, self.board_oy, c[0], c[1], color)
                }
            }
        }

        // HOLD panel
        let rr2 = 0
        while rr2 < 4 {
            r._w(ansi_cursor_pos(self.hold_oy + rr2, self.hold_ox))
            let cc2 = 0
            while cc2 < 6 {
                r._w("  ")
                cc2 = cc2 + 1
            }
            rr2 = rr2 + 1
        }
        if self.hold_kind >= 0 {
            r.draw_piece_in_panel(self.hold_kind, self.hold_ox, self.hold_oy, 6)
        }

        // NEXT panel: muestra las proximas 3 piezas
        let rr3 = 0
        while rr3 < 14 {
            r._w(ansi_cursor_pos(self.next_oy + rr3, self.next_ox))
            let cc3 = 0
            while cc3 < 6 {
                r._w("  ")
                cc3 = cc3 + 1
            }
            rr3 = rr3 + 1
        }
        let nexts = self.bag.peek(3)
        let i = 0
        while i < len(nexts) {
            r.draw_piece_in_panel(nexts[i], self.next_ox, self.next_oy + i * 4, 6)
            i = i + 1
        }

        // HUD: a la izquierda del HOLD o debajo
        let info_x = self.hold_ox - 2
        let info_y = self.hold_oy + 6
        r.draw_text_at(info_x, info_y    , "SCORE", r.col_text)
        r.draw_text_at(info_x, info_y + 1, pad_left(str(self.score), 8), r.col_score + ANSI["BOLD"])
        r.draw_text_at(info_x, info_y + 3, "HIGH", r.col_text)
        r.draw_text_at(info_x, info_y + 4, pad_left(str(self.high_score), 8), r.col_score)
        r.draw_text_at(info_x, info_y + 6, "LEVEL", r.col_text)
        r.draw_text_at(info_x, info_y + 7, pad_left(str(self.level), 8), r.col_score + ANSI["BOLD"])
        r.draw_text_at(info_x, info_y + 9, "LINES", r.col_text)
        r.draw_text_at(info_x, info_y + 10, pad_left(str(self.lines_cleared_total), 8), r.col_score)

        // Controles a la derecha del NEXT
        let ctrl_x = self.next_ox - 2
        let ctrl_y = self.next_oy + 14
        r.draw_text_at(ctrl_x, ctrl_y    , "<- ->  move", r.col_dim)
        r.draw_text_at(ctrl_x, ctrl_y + 1, "v      drop ", r.col_dim)
        r.draw_text_at(ctrl_x, ctrl_y + 2, "^ X    rot.r", r.col_dim)
        r.draw_text_at(ctrl_x, ctrl_y + 3, "Z      rot.l", r.col_dim)
        r.draw_text_at(ctrl_x, ctrl_y + 4, "SPC    drop!", r.col_dim)
        r.draw_text_at(ctrl_x, ctrl_y + 5, "C      hold ", r.col_dim)
        r.draw_text_at(ctrl_x, ctrl_y + 6, "P      pause", r.col_dim)
        r.draw_text_at(ctrl_x, ctrl_y + 7, "Q      quit ", r.col_dim)

        // Overlays
        if self.state == STATE_READY {
            r.draw_centered("READY!", ansi_rgb(255, 255, 0), self.board_ox, self.board_oy, BOARD_H / 2 - 1)
        }
        if self.state == STATE_GAME_OVER {
            r.draw_centered("GAME OVER", ansi_rgb(255, 80, 80), self.board_ox, self.board_oy, BOARD_H / 2 - 1)
            let msg = "Score: " + str(self.score)
            r.draw_centered(msg, ansi_rgb(255, 255, 0), self.board_ox, self.board_oy, BOARD_H / 2 + 1)
            if self.is_new_high_score {
                r.draw_centered("** NEW HIGH **", ansi_rgb(120, 255, 120), self.board_ox, self.board_oy, BOARD_H / 2 + 3)
            }
        }
        if self.paused {
            r.draw_centered("PAUSED", ansi_rgb(180, 180, 255), self.board_ox, self.board_oy, BOARD_H / 2)
        }

        r.full_redraw = false
        r._flush()
    }

    fn _render_splash(self) {
        let r = self.renderer
        r._w(ANSI["CLEAR"])
        r._w(ESC_HOME)
        let cy = self.term_h / 2 - 5
        let cx = self.term_w / 2

        let title = "T E T R I S"
        let sub   = "TUI EDITION"
        let press = "PRESS ANY KEY TO START"
        let hi    = "HIGH SCORE: " + str(self.persistent_high_score)
        let quit  = "Q to quit"

        r._w(ansi_cursor_pos(cy,     cx - len(title) / 2))
        r._w(ansi_rgb( 80, 220, 230) + ANSI["BOLD"] + title + ANSI["RESET"])

        r._w(ansi_cursor_pos(cy + 2, cx - len(sub) / 2))
        r._w(ansi_rgb(170,  80, 200) + sub + ANSI["RESET"])

        r._w(ansi_cursor_pos(cy + 5, cx - len(hi) / 2))
        r._w(ansi_rgb(255, 100, 100) + hi + ANSI["RESET"])

        r._w(ansi_cursor_pos(cy + 8, cx - len(press) / 2))
        r._w(ansi_rgb(255, 255, 255) + ANSI["BOLD"] + press + ANSI["RESET"])

        r._w(ansi_cursor_pos(cy + 10, cx - len(quit) / 2))
        r._w(ansi_rgb(120, 120, 120) + quit + ANSI["RESET"])
    }
}


// =============================================================================
// MAIN
// =============================================================================

fn main() {
    let cfg = Config()
    let inp = InputBackend()
    let game = Game(cfg)

    print(ANSI["CLEAR"])
    print(ESC_HOME)
    print(ESC_CUR_HIDE)
    try { flush_output() } catch e { }

    while game.running {
        let frame_start = time_ms()
        let safety = 0
        while safety < 8 {
            let k = inp.poll()
            if k == -1 { break }
            game.handle_key(k)
            safety = safety + 1
        }
        game.tick()
        game.render()
        let elapsed = time_ms() - frame_start
        let sleep_ms = FRAME_MS - elapsed
        if sleep_ms > 0 { sleep(sleep_ms) }
    }

    inp.shutdown()
    print(ANSI["CLEAR"])
    print(ESC_HOME)
    print(ESC_CUR_SHOW)
    print(ANSI["RESET"])
    println("Score final: " + str(game.score))
    println("High score: " + str(game.persistent_high_score))
}

main()
