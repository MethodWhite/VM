// =============================================================================
// PHOENIX TUI - Bloque A: tablero + nave + disparos
// =============================================================================
// Pruebalo:
//   vesta --script phoenix_blockA_test.vsh
//
// Controles:
//   Flechas izq/der / A D    mover lateral
//   Espacio                  disparar
//   U                        toggle Unicode/ASCII en vivo
//   P                        pausar
//   Q / Esc                  salir
//   Cualquier tecla          arrancar (en splash)
//
// Lo que valida este bloque:
//   - Tablero 60x25 con marco
//   - Nave del jugador (4 chars de ancho) movible lateral
//   - Disparos saliendo de la nave con cooldown, suben y desaparecen al salir
//   - Toggle U para Unicode/ASCII funcional sin reiniciar
//   - State machine SPLASH -> READY -> PLAYING
//   - HUD basico
//
// Lo que NO esta:
//   - Enemigos
//   - Vidas (por ahora invulnerable)
//   - Game over
// =============================================================================


// =============================================================================
// Constantes
// =============================================================================

let ESC          = substr(ANSI["CLEAR_LINE"], 0, 1)
let ESC_HOME     = ESC + "[H"
let ESC_CUR_HIDE = ESC + "[?25l"
let ESC_CUR_SHOW = ESC + "[?25h"

let FIELD_W = 60
let FIELD_H = 25

let FRAME_MS = 33                    // ~30 FPS

// Velocidad: la nave se mueve cada N frames cuando se mantiene la tecla.
// Como el input es discreto (cada pulsacion = 1 movimiento), esto es solo
// referencia. La nave avanza tantas celdas como teclas pulses por segundo.
let PLAYER_SHOT_COOLDOWN_MS = 200    // 5 disparos/seg max
let SHOT_SPEED_MS = 50               // cada N ms el disparo sube 1 celda

// Estados
let STATE_SPLASH    = 0
let STATE_READY     = 1
let STATE_PLAYING   = 2

let READY_MS = 1500

// Codigos de tecla
let KEY_UP    = -1000
let KEY_DOWN  = -1001
let KEY_LEFT  = -1002
let KEY_RIGHT = -1003
let KEY_ESC   = 27

// Virtual-Key codes para GetAsyncKeyState (Windows)
let VK_LEFT  = 37
let VK_UP    = 38
let VK_RIGHT = 39
let VK_DOWN  = 40
let VK_SPACE = 32
let VK_A     = 65
let VK_D     = 68
let VK_W     = 87
let VK_S     = 83

// Persistencia
let HIGH_SCORE_PATH = ".phoenix_score.json"


// =============================================================================
// Config
// =============================================================================

class Config {
    fn __init__(self) {
        self.use_unicode = true
        self.path = ".phoenix.json"
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
    fn save(self) {
        let v = "false"
        if self.use_unicode { v = "true" }
        let txt = "{\n  \"use_unicode\": " + v + "\n}\n"
        try { write_file(self.path, txt); return true } catch e { return false }
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


// =============================================================================
// Player (nave)
// =============================================================================
// Nave de 4 chars de ancho. La posicion .col es la del char MAS A LA IZQUIERDA.
// El centro logico (para disparar) es .col + 1 (entre los chars 1 y 2 del sprite).

class Player {
    fn __init__(self) {
        self.col = (FIELD_W - 4) / 2
        self.row = FIELD_H - 2
        self.shot_cooldown_ms = 0
    }

    fn move_left(self) {
        if self.col > 0 { self.col = self.col - 1 }
    }
    fn move_right(self) {
        if self.col + 4 < FIELD_W { self.col = self.col + 1 }
    }

    // Centro de disparo (col del cañon)
    fn shoot_col(self) {
        return self.col + 2
    }
    fn shoot_row(self) {
        return self.row - 1
    }
}


// =============================================================================
// Shots (disparos)
// =============================================================================
// Lista de disparos activos. Cada uno: {col, row, accum_ms}. Suben cada
// SHOT_SPEED_MS milisegundos.

class ShotPool {
    fn __init__(self) {
        self.shots = []
    }

    fn add(self, col, row) {
        append(self.shots, { "col": col, "row": row, "accum_ms": 0 })
    }

    // Avanza todos los disparos. Devuelve la lista de positions que ANTES
    // ocupaban los disparos (para borrarlos en pantalla).
    fn step(self, elapsed_ms) {
        let cleared = []
        let alive = []
        for s in self.shots {
            let prev_col = s["col"]
            let prev_row = s["row"]
            s["accum_ms"] = s["accum_ms"] + elapsed_ms
            // Pueden subir varias celdas en un mismo tick si elapsed_ms es alto
            while s["accum_ms"] >= SHOT_SPEED_MS {
                s["accum_ms"] = s["accum_ms"] - SHOT_SPEED_MS
                s["row"] = s["row"] - 1
            }
            // ¿Sigue dentro del campo?
            if s["row"] < 0 {
                append(cleared, [prev_col, prev_row])
                continue
            }
            // ¿Cambio de fila? Marca la fila anterior como cleared
            if s["row"] != prev_row {
                append(cleared, [prev_col, prev_row])
            }
            append(alive, s)
        }
        self.shots = alive
        return cleared
    }

    fn count(self) { return len(self.shots) }
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
        // Para deteccion real de teclas mantenidas (Windows con GetAsyncKeyState)
        self.user32 = 0
        self.gaks_sym = 0
        self.has_gaks = false
        self._setup()
    }
    fn _setup(self) {
        if self.os == "windows" {
            self.lib = ffi_open("msvcrt.dll")
            self.kbhit_sym = ffi_sym(self.lib, "_kbhit")
            self.getch_sym = ffi_sym(self.lib, "_getch")
            // GetAsyncKeyState para detectar teclas fisicamente pulsadas
            try {
                self.user32 = ffi_open("user32.dll")
                self.gaks_sym = ffi_sym(self.user32, "GetAsyncKeyState")
                self.has_gaks = true
            } catch e {
                self.has_gaks = false
            }
        } else {
            let libname = "libc.so.6"
            if self.os == "macos" { libname = "libc.dylib" }
            self.lib = ffi_open(libname)
            self.getchar_sym = ffi_sym(self.lib, "getchar")
            shell("stty -icanon -echo -ixon -ixoff min 0 time 0")
        }
    }

    // Devuelve true si la tecla con virtual-key vk esta fisicamente pulsada
    // ahora mismo (Windows). En otros SO, devuelve false (fallback al sistema
    // de eventos).
    //
    // Virtual-Key codes utiles (Windows):
    //   VK_LEFT  = 0x25 = 37
    //   VK_UP    = 0x26 = 38
    //   VK_RIGHT = 0x27 = 39
    //   VK_DOWN  = 0x28 = 40
    //   VK_SPACE = 0x20 = 32
    //   Letras: codigo ASCII en mayuscula (A=0x41=65, etc.)
    fn is_key_down(self, vk) {
        if not self.has_gaks { return false }
        let r = ffi_call(self.gaks_sym, vk)
        // El bit 0x8000 indica que la tecla esta pulsada actualmente
        // (en Windows el resultado es un short, pero ffi_call devuelve int)
        if r == 0 { return false }
        // Comprobar el bit alto. Como puede venir negativo si interpretado
        // como short con signo, comprobamos magnitud absoluta:
        if r < 0 { return true }              // bit alto = signo negativo en short
        if r >= 32768 { return true }         // bit 0x8000 puesto
        return false
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
        if self.user32 != 0 { ffi_close(self.user32) }
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
// Cada celda del campo es 1 char de ancho. Eso permite movimiento lateral
// suave. El precio: los sprites son mas estrechos.

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

    // Re-cargar caracteres cuando cambia el modo Unicode/ASCII en vivo
    fn refresh_chars(self) {
        self._setup_chars()
        self.full_redraw = true
    }

    fn _setup_chars(self) {
        let u = self.config.use_unicode
        if u {
            // Marco doble
            self.frame_h  = from_char(226) + from_char(149) + from_char(144)  // ═
            self.frame_v  = from_char(226) + from_char(149) + from_char(145)  // ║
            self.frame_tl = from_char(226) + from_char(149) + from_char(148)  // ╔
            self.frame_tr = from_char(226) + from_char(149) + from_char(151)  // ╗
            self.frame_bl = from_char(226) + from_char(149) + from_char(154)  // ╚
            self.frame_br = from_char(226) + from_char(149) + from_char(157)  // ╝
            // Disparo del jugador: linea vertical brillante
            self.shot_glyph = from_char(226) + from_char(148) + from_char(130)  // │
        } else {
            self.frame_h  = "="
            self.frame_v  = "|"
            self.frame_tl = "+"
            self.frame_tr = "+"
            self.frame_bl = "+"
            self.frame_br = "+"
            self.shot_glyph = "|"
        }
    }

    fn _setup_colors(self) {
        self.col_frame   = ansi_rgb(120, 120, 200)
        self.col_player  = ansi_rgb( 80, 220, 255)
        self.col_engine  = ansi_rgb(255, 180,  60)
        self.col_shot    = ansi_rgb(255, 255, 100)
        self.col_text    = ansi_rgb(255, 255, 255)
        self.col_score   = ansi_rgb(255, 255,   0)
        self.col_dim     = ansi_rgb(120, 120, 120)
        self.col_warn    = ansi_rgb(255,  80,  80)
        self.R = ANSI["RESET"]
    }

    // Sprite de la nave del jugador (4 chars). Recibe use_unicode actual.
    fn player_sprite(self, use_unicode) {
        // Devuelve string de 4 chars con escapes ANSI inline.
        // Sprite legible: alas '<' y '>' a los lados, fuselaje 'AA' en medio.
        // En ASCII y Unicode mantenemos el mismo glifo (ya es legible en ambos).
        let cyan = self.col_player + ANSI["BOLD"]
        let R = self.R
        return cyan + "<" + "A" + "A" + ">" + R
    }

    fn cell_pos(self, ox, oy, col, row) {
        return ansi_cursor_pos(oy + row, ox + col)
    }

    fn clear_cell(self, ox, oy, col, row) {
        self._w(self.cell_pos(ox, oy, col, row))
        self._w(" ")
    }

    fn draw_player(self, ox, oy, player) {
        self._w(self.cell_pos(ox, oy, player.col, player.row))
        self._w(self.player_sprite(self.config.use_unicode))
    }

    fn clear_player_area(self, ox, oy, player) {
        self._w(self.cell_pos(ox, oy, player.col, player.row))
        self._w("    ")
    }

    fn draw_shot(self, ox, oy, col, row) {
        self._w(self.cell_pos(ox, oy, col, row))
        self._w(self.col_shot + ANSI["BOLD"] + self.shot_glyph + self.R)
    }

    fn draw_frame(self, ox, oy) {
        // El campo ocupa (ox, oy) a (ox+FIELD_W-1, oy+FIELD_H-1).
        // El marco va alrededor.
        // Top
        self._w(ansi_cursor_pos(oy - 1, ox - 1))
        let top = self.col_frame + self.frame_tl
        let i = 0
        while i < FIELD_W { top = top + self.frame_h; i = i + 1 }
        top = top + self.frame_tr + self.R
        self._w(top)
        // Bottom
        self._w(ansi_cursor_pos(oy + FIELD_H, ox - 1))
        let bot = self.col_frame + self.frame_bl
        let j = 0
        while j < FIELD_W { bot = bot + self.frame_h; j = j + 1 }
        bot = bot + self.frame_br + self.R
        self._w(bot)
        // Lados
        let r = 0
        while r < FIELD_H {
            self._w(ansi_cursor_pos(oy + r, ox - 1))
            self._w(self.col_frame + self.frame_v + self.R)
            self._w(ansi_cursor_pos(oy + r, ox + FIELD_W))
            self._w(self.col_frame + self.frame_v + self.R)
            r = r + 1
        }
    }

    fn draw_hud(self, score, high, lives, level, ox, oy) {
        self._w(ansi_cursor_pos(oy, ox))
        let s = self.col_text + "SCORE  " + self.col_score + pad_left(str(score), 6) + "   "
        s = s + self.col_text + "HIGH  " + self.col_score + pad_left(str(high), 6) + "   "
        s = s + self.col_text + "LIVES  " + self.col_player + str(lives) + "   "
        s = s + self.col_text + "WAVE  " + self.col_score + str(level) + self.R
        self._w(s)
    }

    fn draw_status(self, msg, ox, oy) {
        self._w(ansi_cursor_pos(oy, ox))
        self._w(ANSI["CLEAR_LINE"])
        self._w(self.col_dim + msg + self.R)
    }

    fn draw_centered(self, text, color, ox, oy, in_row) {
        let x = ox + (FIELD_W - len(text)) / 2
        self._w(ansi_cursor_pos(oy + in_row, x))
        self._w(color + ANSI["BOLD"] + text + self.R)
    }
}


// =============================================================================
// Game
// =============================================================================

class Game {
    fn __init__(self, config, inp) {
        self.config = config
        self.inp = inp                  // referencia al InputBackend para is_key_down
        self.player = Player()
        self.shots = ShotPool()
        self.renderer = Renderer(config)

        self.score = 0
        self.high_score = load_high_score()
        self.lives = 3
        self.wave = 1
        self.paused = false
        self.running = true

        self.state = STATE_SPLASH
        self.state_timer_ms = 0

        self.last_tick_ms = time_ms()

        // Para borrado eficiente de la nave cuando se mueve
        self.last_player_col = self.player.col

        // Estado de teclas "mantenidas" para fallback (Linux/macOS o si
        // GetAsyncKeyState no esta disponible). En Windows usamos is_key_down
        // directamente, mucho mas robusto.
        self.held_left_ms  = 0
        self.held_right_ms = 0
        self.held_fire_ms  = 0
        self.held_timeout_ms = 80

        // Acumulador para mover la nave a velocidad constante
        self.move_accum_ms = 0
        self.move_step_ms = 30   // 1 celda cada 30ms = ~33 celdas/s

        let ts = TermSize()
        self.term_w = ts.cols
        self.term_h = ts.rows
        self.ox = (self.term_w - FIELD_W) / 2
        self.oy = (self.term_h - FIELD_H) / 2 + 1
        if self.ox < 2 { self.ox = 2 }
        if self.oy < 2 { self.oy = 2 }
    }

    // -------------------------------------------------------------------------
    // Input
    // -------------------------------------------------------------------------

    fn handle_key(self, k) {
        if k == 113 or k == 81 or k == KEY_ESC { self.running = false; return }

        // Toggle Unicode/ASCII en cualquier estado
        if k == 117 or k == 85 {           // u/U
            self.config.use_unicode = not self.config.use_unicode
            self.config.save()
            self.renderer.refresh_chars()
            return
        }

        if self.state == STATE_SPLASH {
            self._start_new_game()
            return
        }

        if k == 112 or k == 80 {           // p/P
            self.paused = not self.paused
            return
        }
        if self.paused { return }
        if self.state != STATE_PLAYING { return }

        // Activar flags de "tecla mantenida". El movimiento y el disparo se
        // aplicaran en tick() segun los flags y los acumuladores de tiempo.
        // Asi, mantener una flecha mueve continuamente, y mantener espacio
        // dispara con el cooldown adecuado, todo en paralelo.
        if k == KEY_LEFT  or k == 65 or k == 97  { self.held_left_ms  = self.held_timeout_ms; return }
        if k == KEY_RIGHT or k == 68 or k == 100 { self.held_right_ms = self.held_timeout_ms; return }
        if k == 32                                { self.held_fire_ms  = self.held_timeout_ms; return }
    }

    fn _move_player(self, dc) {
        // No tocamos last_player_col aqui; se gestiona desde tick() para
        // que recoja la posicion antes del bucle de movimientos completo.
        if dc < 0 { self.player.move_left()  }
        if dc > 0 { self.player.move_right() }
    }

    fn _try_shoot(self) {
        if self.player.shot_cooldown_ms > 0 { return }
        self.shots.add(self.player.shoot_col(), self.player.shoot_row())
        self.player.shot_cooldown_ms = PLAYER_SHOT_COOLDOWN_MS
    }

    // -------------------------------------------------------------------------
    // Transiciones
    // -------------------------------------------------------------------------

    fn _start_new_game(self) {
        self.score = 0
        self.lives = 3
        self.wave = 1
        self.player = Player()
        self.shots = ShotPool()
        self.last_player_col = self.player.col
        self.state = STATE_READY
        self.state_timer_ms = READY_MS
        self.renderer.full_redraw = true
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

        // STATE_PLAYING

        // Detectar teclas mantenidas. Si tenemos GetAsyncKeyState (Windows),
        // lo usamos directamente porque permite detectar varias teclas a la
        // vez. Si no, fallback a los flags con timeout (Linux/macOS).
        let want_left  = false
        let want_right = false
        let want_fire  = false

        if self.inp.has_gaks {
            // Windows: estado real de las teclas
            if self.inp.is_key_down(VK_LEFT)  or self.inp.is_key_down(VK_A) { want_left  = true }
            if self.inp.is_key_down(VK_RIGHT) or self.inp.is_key_down(VK_D) { want_right = true }
            if self.inp.is_key_down(VK_SPACE) { want_fire = true }
        } else {
            // Fallback: timeouts decrementados con el tiempo
            if self.held_left_ms  > 0 { self.held_left_ms  = self.held_left_ms  - elapsed }
            if self.held_right_ms > 0 { self.held_right_ms = self.held_right_ms - elapsed }
            if self.held_fire_ms  > 0 { self.held_fire_ms  = self.held_fire_ms  - elapsed }
            if self.held_left_ms  > 0 { want_left  = true }
            if self.held_right_ms > 0 { want_right = true }
            if self.held_fire_ms  > 0 { want_fire  = true }
        }

        // Movimiento continuo: 1 celda cada move_step_ms.
        // CRITICO: guardamos la posicion ANTES del bucle. Si la nave se
        // mueve varias celdas en un mismo frame, el render solo borra
        // desde esta posicion inicial, no desde cada paso intermedio.
        let move_start_col = self.player.col
        self.move_accum_ms = self.move_accum_ms + elapsed
        while self.move_accum_ms >= self.move_step_ms {
            self.move_accum_ms = self.move_accum_ms - self.move_step_ms
            if want_left and not want_right {
                self._move_player(-1)
            } elif want_right and not want_left {
                self._move_player(1)
            }
        }
        // Si se ha movido en este frame, registramos la posicion inicial
        // como "vieja" para que el render la limpie.
        if move_start_col != self.player.col {
            self.last_player_col = move_start_col
        }

        // Cooldown del disparo
        if self.player.shot_cooldown_ms > 0 {
            self.player.shot_cooldown_ms = self.player.shot_cooldown_ms - elapsed
            if self.player.shot_cooldown_ms < 0 { self.player.shot_cooldown_ms = 0 }
        }

        // Disparo continuo
        if want_fire {
            self._try_shoot()
        }

        // Actualizar disparos
        let cleared = self.shots.step(elapsed)
        self._cleared_shot_cells = cleared
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

        if self.state == STATE_SPLASH {
            self._render_splash()
            r.full_redraw = false
            r._flush()
            return
        }

        // Marco + HUD + status (el marco solo en full_redraw)
        if r.full_redraw {
            r.draw_frame(self.ox, self.oy)
        }
        r.draw_hud(self.score, self.high_score, self.lives, self.wave, self.ox, hud_y)

        // En full_redraw, limpiar interior del campo
        if r.full_redraw {
            let rr = 0
            while rr < FIELD_H {
                r._w(ansi_cursor_pos(self.oy + rr, self.ox))
                let cc = 0
                while cc < FIELD_W {
                    r._w(" ")
                    cc = cc + 1
                }
                rr = rr + 1
            }
        }

        // Borrar celdas de disparos que ya no estan o cambiaron de fila
        if self.state == STATE_PLAYING {
            for cell in self._cleared_shot_cells {
                r.clear_cell(self.ox, self.oy, cell[0], cell[1])
            }
        }

        // Borrar la posicion vieja de la nave si se ha movido. Si se ha
        // movido varias celdas en este frame, hay que borrar TODO el rango
        // [last_player_col, last_player_col+3] (la nave ocupa 4 chars).
        // El nuevo dibujo de la nave repinta sobre la posicion actual, asi
        // que solo necesitamos borrar las celdas que estaban ocupadas antes
        // y NO van a estar tras el repintado.
        if self.last_player_col != self.player.col {
            // Calcular rango ocupado antes (4 chars desde last_player_col)
            let old_lo = self.last_player_col
            let old_hi = self.last_player_col + 3
            // Y rango ocupado ahora (que se repintara)
            let new_lo = self.player.col
            let new_hi = self.player.col + 3
            // Borrar las celdas viejas que NO estan dentro del rango nuevo
            let c = old_lo
            while c <= old_hi {
                if c < new_lo or c > new_hi {
                    r._w(r.cell_pos(self.ox, self.oy, c, self.player.row))
                    r._w(" ")
                }
                c = c + 1
            }
            self.last_player_col = self.player.col
        }

        // Pintar la nave del jugador (excepto durante splash)
        if self.state == STATE_PLAYING or self.state == STATE_READY {
            r.draw_player(self.ox, self.oy, self.player)
        }

        // Pintar disparos activos
        if self.state == STATE_PLAYING {
            for s in self.shots.shots {
                r.draw_shot(self.ox, self.oy, s["col"], s["row"])
            }
        }

        // Overlays
        if self.state == STATE_READY {
            r.draw_centered("READY!", ansi_rgb(255, 255, 0), self.ox, self.oy, FIELD_H / 2 - 1)
            r.draw_centered("Get ready, pilot!", ansi_rgb(180, 180, 180), self.ox, self.oy, FIELD_H / 2 + 1)
        }
        if self.paused {
            r.draw_centered("PAUSED", ansi_rgb(180, 180, 255), self.ox, self.oy, FIELD_H / 2)
        }

        // Status bar
        let mode_str = "Unicode"
        if not self.config.use_unicode { mode_str = "ASCII" }
        let msg = "<- ->/AD move  SPC shoot  U toggle " + mode_str + "  P pause  Q quit"
        if self.paused { msg = "*** PAUSED ***  P to resume" }
        r.draw_status(msg, self.ox, status_y)

        r.full_redraw = false
        r._flush()
    }

    fn _render_splash(self) {
        let r = self.renderer
        r._w(ANSI["CLEAR"])
        r._w(ESC_HOME)
        let cy = self.term_h / 2 - 5
        let cx = self.term_w / 2

        let title = "P H O E N I X"
        let sub   = "TUI EDITION"
        let press = "PRESS ANY KEY TO START"
        let hi    = "HIGH SCORE: " + str(self.high_score)
        let mode  = "Mode: Unicode"
        if not self.config.use_unicode { mode = "Mode: ASCII" }
        let toggle = "Press U to toggle"
        let quit  = "Q to quit"

        r._w(ansi_cursor_pos(cy,     cx - len(title) / 2))
        r._w(ansi_rgb(255, 100, 60) + ANSI["BOLD"] + title + ANSI["RESET"])

        r._w(ansi_cursor_pos(cy + 2, cx - len(sub) / 2))
        r._w(ansi_rgb(120, 200, 255) + sub + ANSI["RESET"])

        r._w(ansi_cursor_pos(cy + 5, cx - len(hi) / 2))
        r._w(ansi_rgb(255, 100, 100) + hi + ANSI["RESET"])

        r._w(ansi_cursor_pos(cy + 7, cx - len(mode) / 2))
        r._w(ansi_rgb(150, 150, 150) + mode + ANSI["RESET"])
        r._w(ansi_cursor_pos(cy + 8, cx - len(toggle) / 2))
        r._w(ansi_rgb(120, 120, 120) + toggle + ANSI["RESET"])

        r._w(ansi_cursor_pos(cy + 10, cx - len(press) / 2))
        r._w(ansi_rgb(255, 255, 255) + ANSI["BOLD"] + press + ANSI["RESET"])

        r._w(ansi_cursor_pos(cy + 12, cx - len(quit) / 2))
        r._w(ansi_rgb(120, 120, 120) + quit + ANSI["RESET"])
    }
}


// =============================================================================
// MAIN
// =============================================================================

fn main() {
    let cfg = Config()
    let inp = InputBackend()
    let game = Game(cfg, inp)
    // Para que tick() no falle al leer self._cleared_shot_cells la primera vez
    game._cleared_shot_cells = []

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
    println("Bloque A OK si has visto la nave moverse y disparar correctamente.")
}

main()
