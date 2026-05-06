// =============================================================================
// SNAKE TUI
// =============================================================================
// Pruebalo:
//   vesta --script snake.vsh
//
// Controles:
//   Flechas / WASD   mover
//   P                pausar
//   Q / Esc          salir
//   Cualquier tecla  iniciar (en splash)
//
// Config: edita .snake.json en el cwd, o crea uno con:
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

let FIELD_W = 40
let FIELD_H = 20

// Direcciones (vector dcol, drow)
let DIR_NONE  = 0
let DIR_UP    = 1
let DIR_DOWN  = 2
let DIR_LEFT  = 3
let DIR_RIGHT = 4

// Velocidad: tiempo entre movimientos en ms.
// Empieza en SPEED_START y baja (mas rapido) cada APPLES_PER_LEVEL manzanas
// hasta SPEED_MIN.
let SPEED_START      = 150       // ms entre movimientos al inicio
let SPEED_MIN        = 50        // ms minimo (mas rapido)
let SPEED_DECREMENT  = 10        // cuanto baja por nivel
let APPLES_PER_LEVEL = 5         // manzanas comidas para subir nivel

// Frame rate del bucle principal (input). Se desacopla del movimiento.
let FRAME_MS = 16                // ~60 FPS del bucle, suficiente para input

// Puntos
let PT_APPLE_BASE = 10           // se multiplica por nivel

// Estados del juego
let STATE_SPLASH    = 0
let STATE_READY     = 1
let STATE_PLAYING   = 2
let STATE_GAME_OVER = 3

let READY_FRAMES_MS = 1500       // 1.5s
let GAME_OVER_MS    = 4000       // 4s antes de volver a splash

// Codigos de tecla
let KEY_UP    = -1000
let KEY_DOWN  = -1001
let KEY_LEFT  = -1002
let KEY_RIGHT = -1003
let KEY_ESC   = 27

// Persistencia high score
let HIGH_SCORE_PATH = ".snake_score.json"


// =============================================================================
// Config
// =============================================================================

class Config {
    fn __init__(self) {
        self.use_unicode = true
        self.path = ".snake.json"
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
// Helpers
// =============================================================================

fn dir_delta(d) {
    if d == DIR_UP    { return [ 0, -1] }
    if d == DIR_DOWN  { return [ 0,  1] }
    if d == DIR_LEFT  { return [-1,  0] }
    if d == DIR_RIGHT { return [ 1,  0] }
    return [0, 0]
}

fn dir_opposite(d) {
    if d == DIR_UP    { return DIR_DOWN  }
    if d == DIR_DOWN  { return DIR_UP    }
    if d == DIR_LEFT  { return DIR_RIGHT }
    if d == DIR_RIGHT { return DIR_LEFT  }
    return DIR_NONE
}

// Pseudo-random simple basado en una semilla mutable
class Rng {
    fn __init__(self, seed) {
        self.s = seed
        if self.s == 0 { self.s = 12345 }
    }
    fn next(self) {
        // LCG simple
        self.s = (self.s * 1103515245 + 12345) % 2147483648
        return self.s
    }
    fn range(self, lo, hi) {
        // Devuelve entero en [lo, hi)
        let span = hi - lo
        if span <= 0 { return lo }
        let r = self.next()
        if r < 0 { r = -r }
        return lo + (r % span)
    }
}


// =============================================================================
// Snake
// =============================================================================

class Snake {
    "Cuerpo de la serpiente: lista de [col, row] con head al final."

    fn __init__(self) {
        self.reset()
    }

    fn reset(self) {
        // Empezamos en el centro, mirando a la derecha, longitud 4
        let cx = FIELD_W / 2
        let cy = FIELD_H / 2
        self.body = [[cx - 3, cy], [cx - 2, cy], [cx - 1, cy], [cx, cy]]
        self.dir = DIR_RIGHT
        self.next_dir = DIR_RIGHT
        self.grow_pending = 0       // cuando come, crece N celdas
    }

    fn head(self) {
        return self.body[len(self.body) - 1]
    }

    fn length(self) {
        return len(self.body)
    }

    // ¿Esta (col, row) ocupada por el cuerpo? Si include_head=false ignora la cabeza.
    fn occupies(self, col, row, include_head) {
        let n = len(self.body)
        let limit = n
        if not include_head { limit = n - 1 }
        let i = 0
        while i < limit {
            let p = self.body[i]
            if p[0] == col and p[1] == row { return true }
            i = i + 1
        }
        return false
    }

    // Aplica next_dir si no es la opuesta a la actual (no se puede dar marcha atras)
    fn _apply_buffered_dir(self) {
        if self.next_dir == DIR_NONE { return }
        if self.next_dir == dir_opposite(self.dir) { return }
        self.dir = self.next_dir
    }

    // Devuelve la celda de la siguiente cabeza
    fn next_head(self) {
        let h = self.head()
        let d = dir_delta(self.dir)
        return [h[0] + d[0], h[1] + d[1]]
    }

    // Avanza un paso. Retorna:
    //   "wall"      colision con pared (game over)
    //   "self"      colision con su propio cuerpo (game over)
    //   "apple"     comio una manzana (apple es eaten en este paso)
    //   "ok"        movimiento normal
    // El llamador (Game) debe encargarse de comprobar si la nueva head coincide
    // con la manzana ANTES de llamar a step (o pasar apple_pos para que step la
    // compruebe). Hacemos lo segundo, mas simple.
    fn step(self, apple_col, apple_row) {
        self._apply_buffered_dir()
        let nh = self.next_head()
        let ncol = nh[0]
        let nrow = nh[1]

        // Colision con paredes
        if ncol < 0 or ncol >= FIELD_W { return "wall" }
        if nrow < 0 or nrow >= FIELD_H { return "wall" }

        // ¿Va a comer manzana?
        let ate = (ncol == apple_col and nrow == apple_row)

        // Colision con cuerpo: comprobamos contra TODO el cuerpo MENOS la cola
        // (porque la cola se mueve al avanzar, salvo si crecemos por comer).
        // Si no come, ignoramos la cola (que se libera).
        // Si come, no se libera la cola, asi que comprobamos contra todo.
        let n = len(self.body)
        let check_until = n - 1   // sin la cola
        if ate or self.grow_pending > 0 { check_until = n }
        let i = 0
        while i < check_until {
            let p = self.body[i]
            if p[0] == ncol and p[1] == nrow { return "self" }
            i = i + 1
        }

        // Aplicar movimiento: anadir nueva cabeza
        append(self.body, [ncol, nrow])
        // Quitar la cola excepto si crece (por grow_pending o porque acaba de comer)
        if ate or self.grow_pending > 0 {
            if self.grow_pending > 0 { self.grow_pending = self.grow_pending - 1 }
            // si fue por ate, no decrementamos grow_pending (no estaba pendiente)
        } else {
            // Quitar primer elemento (la cola)
            let new_body = []
            let j = 1
            while j < len(self.body) {
                append(new_body, self.body[j])
                j = j + 1
            }
            self.body = new_body
        }

        if ate { return "apple" }
        return "ok"
    }
}


// =============================================================================
// Apple
// =============================================================================
// Una sola manzana en el campo. Se reposiciona aleatoriamente cuando se come,
// evitando colocarse encima de la serpiente.

class Apple {
    fn __init__(self, rng) {
        self.col = 0
        self.row = 0
        self.rng = rng
        self.respawn_safe(null)
    }

    // Coloca la manzana en una celda aleatoria que NO este ocupada por snake.
    // Si snake es null, sin restriccion.
    fn respawn_safe(self, snake) {
        // Intentos limitados; si tras 200 no encuentra, escaneo lineal
        let attempts = 0
        while attempts < 200 {
            let c = self.rng.range(0, FIELD_W)
            let r = self.rng.range(0, FIELD_H)
            if snake == null or not snake.occupies(c, r, true) {
                self.col = c
                self.row = r
                return
            }
            attempts = attempts + 1
        }
        // Plan B: escaneo lineal
        let r = 0
        while r < FIELD_H {
            let c = 0
            while c < FIELD_W {
                if not snake.occupies(c, r, true) {
                    self.col = c
                    self.row = r
                    return
                }
                c = c + 1
            }
            r = r + 1
        }
    }
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
// Cada celda se dibuja como 2 chars de ancho. El campo ocupa FIELD_W*2 chars.
// Un marco rodea el campo.
// El render NO es completo cada frame: solo redibujamos las celdas que cambian
// (cabeza nueva, cola vieja, manzana al respawn). El marco solo se dibuja en
// full_redraw.

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
            // Marco
            self.frame_h    = from_char(226) + from_char(149) + from_char(144)  // ═
            self.frame_v    = from_char(226) + from_char(149) + from_char(145)  // ║
            self.frame_tl   = from_char(226) + from_char(149) + from_char(148)  // ╔
            self.frame_tr   = from_char(226) + from_char(149) + from_char(151)  // ╗
            self.frame_bl   = from_char(226) + from_char(149) + from_char(154)  // ╚
            self.frame_br   = from_char(226) + from_char(149) + from_char(157)  // ╝
            // Snake body: bloque pleno que se ve uniforme
            self.snake_body = from_char(226) + from_char(150) + from_char(136)  // █
            self.snake_head = from_char(226) + from_char(150) + from_char(136)
            // Manzana: hoja + cuerpo. La hoja es una "mini-cuna" (ˎ) o similar
            // y el cuerpo es un circulo lleno. Combinacion estilo:
            //   Hoja:    U+02CE  ˎ   (modifier letter low grave accent)
            //   Cuerpo:  U+2B24  ⬤   (black large circle, mas grande que ●)
            // En terminales que no soporten ⬤, queda fallback automatico al
            // glifo del terminal.
            self.apple_leaf = from_char(203) + from_char(142)                    // ˎ (rabito)
            self.apple_body = from_char(226) + from_char(172) + from_char(164)   // ⬤ (cuerpo gordo)
        } else {
            self.frame_h    = "="
            self.frame_v    = "|"
            self.frame_tl   = "+"
            self.frame_tr   = "+"
            self.frame_bl   = "+"
            self.frame_br   = "+"
            self.snake_body = "#"
            self.snake_head = "@"
            self.apple_leaf = "'"
            self.apple_body = "@"
        }
    }

    fn _setup_colors(self) {
        self.col_frame      = ansi_rgb(120, 120, 180)
        self.col_snake_body = ansi_rgb( 50, 220,  80)    // verde
        self.col_snake_head = ansi_rgb(150, 255, 100)    // verde claro
        self.col_apple      = ansi_rgb(255,  60,  60)    // rojo
        self.col_apple_leaf = ansi_rgb( 90, 180,  60)    // verde hoja
        self.col_text       = ansi_rgb(255, 255, 255)
        self.col_score      = ansi_rgb(255, 255,   0)
        self.col_dim        = ansi_rgb(120, 120, 120)
        self.R = ANSI["RESET"]
    }

    // Dibuja el marco completo del campo
    fn draw_frame(self, ox, oy) {
        // El campo va de (ox, oy) a (ox+FIELD_W*2-1, oy+FIELD_H-1).
        // El marco va alrededor: en (ox-2, oy-1) a (ox+FIELD_W*2+1, oy+FIELD_H).
        // En 2 chars de ancho, la pared lateral son 2 chars tambien.
        let inner_w = FIELD_W * 2

        // Top
        self._w(ansi_cursor_pos(oy - 1, ox - 2))
        let top = self.col_frame + self.frame_tl + self.frame_tl
        let i = 0
        while i < inner_w {
            top = top + self.frame_h
            i = i + 1
        }
        top = top + self.frame_tr + self.frame_tr + self.R
        self._w(top)

        // Bottom
        self._w(ansi_cursor_pos(oy + FIELD_H, ox - 2))
        let bot = self.col_frame + self.frame_bl + self.frame_bl
        let j = 0
        while j < inner_w {
            bot = bot + self.frame_h
            j = j + 1
        }
        bot = bot + self.frame_br + self.frame_br + self.R
        self._w(bot)

        // Lados
        let r = 0
        while r < FIELD_H {
            self._w(ansi_cursor_pos(oy + r, ox - 2))
            self._w(self.col_frame + self.frame_v + self.frame_v + self.R)
            self._w(ansi_cursor_pos(oy + r, ox + inner_w))
            self._w(self.col_frame + self.frame_v + self.frame_v + self.R)
            r = r + 1
        }
    }

    fn _cell_pos(self, ox, oy, col, row) {
        return ansi_cursor_pos(oy + row, ox + col * 2)
    }

    fn clear_cell(self, ox, oy, col, row) {
        self._w(self._cell_pos(ox, oy, col, row))
        self._w("  ")
    }

    fn draw_apple(self, ox, oy, apple) {
        // Manzana = hoja verde (char izquierdo) + cuerpo rojo (char derecho)
        self._w(self._cell_pos(ox, oy, apple.col, apple.row))
        self._w(self.col_apple_leaf + self.apple_leaf)
        self._w(self.col_apple + ANSI["BOLD"] + self.apple_body + self.R)
    }

    fn draw_snake_full(self, ox, oy, snake) {
        // Dibuja todo el cuerpo (usado solo en full_redraw)
        let n = len(snake.body)
        let i = 0
        while i < n {
            let p = snake.body[i]
            self._w(self._cell_pos(ox, oy, p[0], p[1]))
            if i == n - 1 {
                self._w(self.col_snake_head + ANSI["BOLD"] + self.snake_head + self.snake_head + self.R)
            } else {
                self._w(self.col_snake_body + self.snake_body + self.snake_body + self.R)
            }
            i = i + 1
        }
    }

    // Dibuja solo los cambios incrementales:
    //   - Nueva cabeza
    //   - El segmento que era cabeza ahora es cuerpo
    //   - Si no creció, el viejo "tail" se borra
    fn draw_snake_incremental(self, ox, oy, snake, old_tail, prev_head) {
        // Borrar la celda que ocupaba la cola vieja (si la perdimos)
        if old_tail != null {
            self.clear_cell(ox, oy, old_tail[0], old_tail[1])
        }
        // Reescribir prev_head como cuerpo (ya no es head)
        if prev_head != null {
            self._w(self._cell_pos(ox, oy, prev_head[0], prev_head[1]))
            self._w(self.col_snake_body + self.snake_body + self.snake_body + self.R)
        }
        // Dibujar nueva cabeza
        let h = snake.head()
        self._w(self._cell_pos(ox, oy, h[0], h[1]))
        self._w(self.col_snake_head + ANSI["BOLD"] + self.snake_head + self.snake_head + self.R)
    }

    fn draw_hud(self, score, high, level, length, ox, oy) {
        self._w(ansi_cursor_pos(oy, ox))
        let s = self.col_text + "SCORE  " + self.col_score + pad_left(str(score), 6) + "   "
        s = s + self.col_text + "HIGH  " + self.col_score + pad_left(str(high), 6) + "   "
        s = s + self.col_text + "LEVEL  " + self.col_score + str(level) + "   "
        s = s + self.col_text + "LENGTH  " + self.col_score + str(length) + self.R
        self._w(s)
    }

    fn draw_status(self, msg, ox, oy) {
        self._w(ansi_cursor_pos(oy, ox))
        self._w(ANSI["CLEAR_LINE"])
        self._w(self.col_dim + msg + self.R)
    }

    fn draw_centered(self, text, color, ox, oy, in_row) {
        let inner_w = FIELD_W * 2
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
        self.snake = Snake()
        self.apple = Apple(self.rng)
        self.apple.respawn_safe(self.snake)
        self.renderer = Renderer(config)

        self.score = 0
        // high_score en RAM: se actualiza al comer manzanas para mostrar en HUD.
        // persistent_high_score: el valor guardado en disco. Se carga al inicio
        // y solo se sobreescribe al final de la partida si self.score lo supera.
        self.persistent_high_score = load_high_score()
        self.high_score = self.persistent_high_score
        self.is_new_high_score = false
        self.level = 1
        self.apples_eaten = 0
        self.paused = false
        self.running = true

        self.state = STATE_SPLASH
        self.state_timer_ms = 0

        // Tiempo desde el ultimo movimiento de la serpiente
        self.move_accum_ms = 0
        self.last_tick_ms = time_ms()

        // Para el render incremental
        self.prev_head = null
        self.last_tail = null

        let ts = TermSize()
        self.term_w = ts.cols
        self.term_h = ts.rows

        let inner_w = FIELD_W * 2
        self.ox = (self.term_w - inner_w) / 2
        self.oy = (self.term_h - FIELD_H) / 2 + 1
        if self.ox < 3 { self.ox = 3 }
        if self.oy < 3 { self.oy = 3 }
    }

    // -------------------------------------------------------------------------
    // Velocidad segun nivel
    // -------------------------------------------------------------------------

    fn _current_speed_ms(self) {
        let speed = SPEED_START - (self.level - 1) * SPEED_DECREMENT
        if speed < SPEED_MIN { speed = SPEED_MIN }
        return speed
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
        if self.state == STATE_GAME_OVER {
            return
        }

        if k == 112 or k == 80 {
            self.paused = not self.paused
            return
        }
        if self.paused { return }
        if self.state != STATE_PLAYING { return }

        if k == KEY_UP    or k ==  87 or k == 119 { self.snake.next_dir = DIR_UP    }
        if k == KEY_DOWN  or k ==  83 or k == 115 { self.snake.next_dir = DIR_DOWN  }
        if k == KEY_LEFT  or k ==  65 or k ==  97 { self.snake.next_dir = DIR_LEFT  }
        if k == KEY_RIGHT or k ==  68 or k == 100 { self.snake.next_dir = DIR_RIGHT }
    }

    // -------------------------------------------------------------------------
    // Transiciones
    // -------------------------------------------------------------------------

    fn _start_new_game(self) {
        self.score = 0
        // Resetear high_score en vivo al persistente (para que el HUD vuelva
        // al record real al empezar una nueva partida)
        self.high_score = self.persistent_high_score
        self.is_new_high_score = false
        self.level = 1
        self.apples_eaten = 0
        self.snake.reset()
        self.apple.respawn_safe(self.snake)
        self.state = STATE_READY
        self.state_timer_ms = READY_FRAMES_MS
        self.move_accum_ms = 0
        self.prev_head = null
        self.last_tail = null
        self.renderer.full_redraw = true
    }

    fn _on_game_over(self) {
        // Comparamos contra el high score PERSISTIDO. self.high_score se ha
        // ido actualizando en vivo y por tanto siempre es >= self.score, no
        // sirve para decidir si guardar.
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
    // Tick
    // -------------------------------------------------------------------------

    fn tick(self) {
        let now = time_ms()
        let elapsed = now - self.last_tick_ms
        self.last_tick_ms = now

        if self.paused { return }

        if self.state == STATE_SPLASH {
            return
        }
        if self.state == STATE_READY {
            self.state_timer_ms = self.state_timer_ms - elapsed
            if self.state_timer_ms <= 0 {
                self.state = STATE_PLAYING
                self.move_accum_ms = 0
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

        // STATE_PLAYING: acumulamos tiempo y movemos cuando toca.
        // Si en un solo frame nos toca ejecutar mas de un movimiento (porque
        // el frame tardo demasiado), forzamos full_redraw despues, ya que
        // el render incremental solo conoce los cambios del ULTIMO step.
        self.move_accum_ms = self.move_accum_ms + elapsed
        let speed = self._current_speed_ms()
        let steps_done = 0
        while self.move_accum_ms >= speed {
            self.move_accum_ms = self.move_accum_ms - speed
            self._step_snake()
            steps_done = steps_done + 1
            if self.state != STATE_PLAYING { return }
            // Limite de seguridad: si nos atrasamos muchisimo, no acumular
            if steps_done >= 4 {
                self.move_accum_ms = 0
                break
            }
        }
        if steps_done > 1 {
            self.renderer.full_redraw = true
        }
    }

    fn _step_snake(self) {
        // Copiamos la cola actual COMO VALORES (no como referencia al elemento
        // del array). Si dejaramos `let old_tail = self.snake.body[0]`, podriamos
        // estar guardando una referencia que tras el reasignado de body queda
        // apuntando a basura o a otro elemento.
        let tail_ref = self.snake.body[0]
        let old_tail = [tail_ref[0], tail_ref[1]]
        // Copia tambien de la cabeza (mismo motivo)
        let head_ref = self.snake.head()
        let prev_head = [head_ref[0], head_ref[1]]

        let result = self.snake.step(self.apple.col, self.apple.row)

        if result == "wall" or result == "self" {
            self._on_game_over()
            return
        }

        self.prev_head = prev_head
        if result == "apple" {
            self.last_tail = null
            self.score = self.score + PT_APPLE_BASE * self.level
            if self.score > self.high_score { self.high_score = self.score }
            self.apples_eaten = self.apples_eaten + 1
            if self.apples_eaten % APPLES_PER_LEVEL == 0 {
                self.level = self.level + 1
            }
            self.apple.respawn_safe(self.snake)
        } else {
            self.last_tail = old_tail
        }
    }

    // -------------------------------------------------------------------------
    // Render
    // -------------------------------------------------------------------------

    fn render(self) {
        let r = self.renderer
        let hud_y = self.oy - 3
        let status_y = self.oy + FIELD_H + 1

        if r.full_redraw {
            r._w(ANSI["CLEAR"])
            r._w(ESC_HOME)
            r._w(ESC_CUR_HIDE)
        }

        // SPLASH
        if self.state == STATE_SPLASH {
            self._render_splash()
            r.full_redraw = false
            r._flush()
            return
        }

        // En cualquier otro estado dibujamos HUD + marco
        r.draw_hud(self.score, self.high_score, self.level, self.snake.length(), self.ox, hud_y)

        // Render simple: si full_redraw, dibujamos el marco. En cualquier caso,
        // limpiamos el interior del campo y volvemos a pintar serpiente + manzana
        // enteras. Es menos eficiente que el render incremental pero mucho mas
        // robusto: nunca quedan celdas con datos viejos.
        if r.full_redraw {
            r.draw_frame(self.ox, self.oy)
        }
        // Limpiar interior
        let rr = 0
        while rr < FIELD_H {
            r._w(ansi_cursor_pos(self.oy + rr, self.ox))
            let cc = 0
            while cc < FIELD_W {
                r._w("  ")
                cc = cc + 1
            }
            rr = rr + 1
        }
        // Pintar serpiente y manzana (solo en estados con tablero)
        if self.state == STATE_PLAYING or self.state == STATE_READY {
            r.draw_snake_full(self.ox, self.oy, self.snake)
            r.draw_apple(self.ox, self.oy, self.apple)
        }

        // Overlays
        if self.state == STATE_READY {
            r.draw_centered("READY!", ansi_rgb(255, 255, 0), self.ox, self.oy, FIELD_H / 2 - 1)
            r.draw_centered("Get going with arrows or WASD", ansi_rgb(180, 180, 180), self.ox, self.oy, FIELD_H / 2 + 1)
        }
        if self.state == STATE_GAME_OVER {
            r.draw_centered("GAME  OVER", ansi_rgb(255, 80, 80), self.ox, self.oy, FIELD_H / 2 - 2)
            let final_msg = "Final score: " + str(self.score)
            r.draw_centered(final_msg, ansi_rgb(255, 255, 0), self.ox, self.oy, FIELD_H / 2)
            if self.is_new_high_score {
                r.draw_centered("** NEW HIGH SCORE **", ansi_rgb(120, 255, 120), self.ox, self.oy, FIELD_H / 2 + 2)
            }
        }

        // Status
        let msg = "Arrows/WASD move  P pause  Q quit  | Speed: " + str(self._current_speed_ms()) + "ms"
        if self.paused { msg = "*** PAUSED ***  P to resume" }
        r.draw_status(msg, self.ox, status_y)

        r.full_redraw = false
        r._flush()
    }

    fn _render_splash(self) {
        let r = self.renderer
        r._w(ANSI["CLEAR"])
        r._w(ESC_HOME)
        let cy = self.term_h / 2 - 4
        let cx = self.term_w / 2

        let title = "S N A K E"
        let sub   = "TUI EDITION"
        let press = "PRESS ANY KEY TO START"
        let hi    = "HIGH SCORE: " + str(self.high_score)
        let quit  = "Q to quit"

        r._w(ansi_cursor_pos(cy,     cx - len(title) / 2))
        r._w(ansi_rgb(80, 240, 100) + ANSI["BOLD"] + title + ANSI["RESET"])

        r._w(ansi_cursor_pos(cy + 2, cx - len(sub) / 2))
        r._w(ansi_rgb(120, 200, 255) + sub + ANSI["RESET"])

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
    println("High score: " + str(game.high_score))
}

main()
