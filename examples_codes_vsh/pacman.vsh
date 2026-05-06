// =============================================================================
// PACMAN TUI - Bloque C: Blinky (rojo) con AI clasica
// =============================================================================
// Pruebalo:
//   vesta --script pacman_blockC_test.vsh
//
// Controles:
//   Flechas / WASD   mover Pacman
//   P                 pausar
//   Q                 salir
//
// Lo nuevo en este bloque:
//   - Blinky (fantasma rojo) aparece en la ghost house y sale a perseguir
//   - AI clasica: en cada interseccion mira sus direcciones disponibles
//     (excepto retroceder), calcula distancia euclidea desde cada candidata
//     hasta su tile objetivo, y elige la que minimiza. Empates: U > L > D > R
//   - Tile objetivo de Blinky (modo Chase): la posicion actual de Pacman
//   - Si Blinky toca a Pacman: Pacman pierde una "vida" (de momento solo
//     reinicia posicion); en el bloque F sera game over real
//
// Lo que NO esta aun:
//   - Modo Scatter (alternancia con Chase)
//   - Modo Frightened (huida cuando comes power pellet)
//   - Los otros 3 fantasmas (vienen en bloque D)
//   - Vidas reales / game over (vienen en bloque F)
// =============================================================================


// =============================================================================
// Constantes
// =============================================================================

let ESC          = substr(ANSI["CLEAR_LINE"], 0, 1)
let ESC_HOME     = ESC + "[H"
let ESC_CUR_HIDE = ESC + "[?25l"
let ESC_CUR_SHOW = ESC + "[?25h"

let MAP_W = 28
let MAP_H = 31

let DIR_NONE  = 0
let DIR_UP    = 1
let DIR_DOWN  = 2
let DIR_LEFT  = 3
let DIR_RIGHT = 4

let TARGET_FPS = 30
let FRAME_MS = 33
let PACMAN_TICKS_PER_MOVE = 4         // Pacman: 7.5 mov/s
let GHOST_TICKS_NORMAL    = 5         // CHASE/SCATTER
let GHOST_TICKS_FRIGHT    = 10        // FRIGHTENED (lento)
let GHOST_TICKS_EATEN     = 2         // EATEN (rapidisimo, vuelve a casa)
let GHOST_TICKS_HOUSE     = 8         // dentro de la casa (espera lenta)

// Frightened (al comer power pellet)
let FRIGHT_FRAMES        = 180        // 6 segundos a 30 FPS
let FRIGHT_BLINK_FRAMES  = 60         // ultimos 2 segundos parpadea
let POPUP_FRAMES         = 30         // duracion del "+200" en pantalla

// Puntos
let PT_DOT     = 10
let PT_POWER   = 50
let PT_GHOST_1 = 200
let PT_GHOST_2 = 400
let PT_GHOST_3 = 800
let PT_GHOST_4 = 1600

// Fruta: aparece tras X dots, dura Y frames
let FRUIT_DOTS_1     = 70
let FRUIT_DOTS_2     = 170
let FRUIT_DURATION   = 300            // 10s a 30 FPS
let FRUIT_COL        = 13             // posicion fija debajo de la ghost house
let FRUIT_ROW        = 17

// Estados del juego
let STATE_SPLASH     = 0
let STATE_READY      = 1
let STATE_PLAYING    = 2
let STATE_DYING      = 3
let STATE_LEVEL_DONE = 4
let STATE_GAME_OVER  = 5

// Timers de los estados (en frames)
let READY_FRAMES      = 60            // 2s
let DYING_FRAMES      = 60            // 2s
let LEVEL_DONE_FRAMES = 90            // 3s
let GAME_OVER_FRAMES  = 180           // 6s

// High score persistente
let HIGH_SCORE_PATH = ".pacman_score.json"


// =============================================================================
// Helper: Fruta (puntos y representacion segun nivel)
// =============================================================================

fn fruit_points_for_level(level) {
    if level == 1 { return 100 }    // cherry
    if level == 2 { return 300 }    // strawberry
    if level == 3 { return 500 }    // orange
    if level == 4 { return 500 }    // orange
    if level == 5 { return 700 }    // apple
    if level == 6 { return 700 }    // apple
    if level == 7 { return 1000 }   // melon
    if level == 8 { return 1000 }   // melon
    if level == 9 { return 2000 }   // galaxian
    if level == 10 { return 2000 }
    if level == 11 { return 3000 }  // bell
    if level == 12 { return 3000 }
    return 5000                      // key (niveles 13+)
}

fn fruit_glyph(level, use_unicode) {
    // Devuelve el glifo de 2 chars del bonus segun nivel
    if not use_unicode {
        if level == 1 { return "ch" }
        if level == 2 { return "st" }
        if level <= 4 { return "or" }
        if level <= 6 { return "ap" }
        if level <= 8 { return "me" }
        if level <= 10 { return "gx" }
        if level <= 12 { return "be" }
        return "ky"
    }
    // Unicode aproximado: usamos un caracter "redondo" en color
    let dot = from_char(226) + from_char(151) + from_char(143)   // ●
    return dot + " "
}

fn fruit_color_for_level(level) {
    if level == 1 { return ansi_rgb(255,  60,  60) }    // cherry rojo
    if level == 2 { return ansi_rgb(255, 130, 200) }    // strawberry rosa
    if level <= 4 { return ansi_rgb(255, 165,   0) }    // orange naranja
    if level <= 6 { return ansi_rgb(220,  20,  60) }    // apple rojo oscuro
    if level <= 8 { return ansi_rgb(150, 220, 100) }    // melon verde
    if level <= 10 { return ansi_rgb(120, 200, 255) }   // galaxian azul
    if level <= 12 { return ansi_rgb(255, 215,   0) }   // bell amarillo
    return ansi_rgb(220, 220, 220)                       // key gris
}


// =============================================================================
// High score persistente
// =============================================================================

fn load_high_score() {
    if not exists(HIGH_SCORE_PATH) { return 0 }
    try {
        let txt = read_file(HIGH_SCORE_PATH)
        // Buscar el primer numero en el JSON
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

// Tipos de fantasma
let G_BLINKY = 0
let G_PINKY  = 1
let G_INKY   = 2
let G_CLYDE  = 3

// Modos de fantasma
let GHOST_CHASE      = 1
let GHOST_SCATTER    = 2
let GHOST_FRIGHTENED = 3
let GHOST_EATEN      = 4

let KEY_UP    = -1000
let KEY_DOWN  = -1001
let KEY_LEFT  = -1002
let KEY_RIGHT = -1003
let KEY_ESC   = 27


// =============================================================================
// Config
// =============================================================================

class Config {
    fn __init__(self) {
        self.use_unicode = true
        self.path = ".pacman.json"
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
        } catch e {
            // ignore
        }
    }
}


// =============================================================================
// Mapa
// =============================================================================

let MAP_RAW = [
    "############################",
    "#............##............#",
    "#.####.#####.##.#####.####.#",
    "#o####.#####.##.#####.####o#",
    "#.####.#####.##.#####.####.#",
    "#..........................#",
    "#.####.##.########.##.####.#",
    "#.####.##.########.##.####.#",
    "#......##....##....##......#",
    "######.##### ## #####.######",
    "######.##### ## #####.######",
    "######.##          ##.######",
    "######.## ###--### ##.######",
    "######.## #      # ##.######",
    "T     .   #      #   .     T",
    "######.## #      # ##.######",
    "######.## ######## ##.######",
    "######.##          ##.######",
    "######.## ######## ##.######",
    "######.## ######## ##.######",
    "#............##............#",
    "#.####.#####.##.#####.####.#",
    "#.####.#####.##.#####.####.#",
    "#o..##.......  .......##..o#",
    "###.##.##.########.##.##.###",
    "###.##.##.########.##.##.###",
    "#......##....##....##......#",
    "#.##########.##.##########.#",
    "#.##########.##.##########.#",
    "#..........................#",
    "############################"
]


class Maze {
    fn __init__(self) {
        self.w = MAP_W
        self.h = MAP_H
        self.grid = []
        self.dirty = []
        self.dots_total = 0
        self.dots_eaten = 0
        let r = 0
        while r < self.h {
            let row = []
            let drow = []
            let c = 0
            while c < self.w {
                let ch = substr(MAP_RAW[r], c, 1)
                append(row, ch)
                append(drow, true)
                if ch == "." or ch == "o" { self.dots_total = self.dots_total + 1 }
                c = c + 1
            }
            append(self.grid, row)
            append(self.dirty, drow)
            r = r + 1
        }
    }

    fn at(self, col, row) {
        if row < 0 or row >= self.h { return "#" }
        if col < 0 or col >= self.w { return "#" }
        return self.grid[row][col]
    }

    fn set(self, col, row, ch) {
        if row < 0 or row >= self.h { return }
        if col < 0 or col >= self.w { return }
        self.grid[row][col] = ch
        self.dirty[row][col] = true
    }

    fn mark_dirty(self, col, row) {
        if row < 0 or row >= self.h { return }
        if col < 0 or col >= self.w { return }
        self.dirty[row][col] = true
    }

    fn is_wall(self, col, row) {
        return self.at(col, row) == "#"
    }

    fn pacman_can_enter(self, col, row) {
        let c = self.at(col, row)
        if c == "#" { return false }
        if c == "-" { return false }
        return true
    }

    // Para fantasmas: permite la puerta '-' SOLO si pueden cruzarla,
    // que depende del modo (solo EATEN vuelve a casa). En modos normales
    // la puerta los bloquea, igual que a Pacman, asi que tienen que salir
    // por las salidas laterales del corredor superior.
    fn ghost_can_enter(self, col, row) {
        return self.at(col, row) != "#"
    }

    // Variante que respeta la puerta segun el modo del fantasma.
    // - Si el fantasma esta EATEN: puede cruzar la puerta para volver a casa
    // - Si el fantasma esta dentro de la casa intentando salir: puede cruzar
    // - En cualquier otro caso (modo normal, ya fuera): puerta bloquea
    fn ghost_can_enter_for(self, col, row, ghost_mode, ghost_in_house) {
        let c = self.at(col, row)
        if c == "#" { return false }
        if c == "-" {
            if ghost_mode == GHOST_EATEN { return true }
            if ghost_in_house { return true }
            return false
        }
        return true
    }

    fn eat(self, col, row) {
        let c = self.at(col, row)
        if c == "." {
            self.set(col, row, " ")
            self.dots_eaten = self.dots_eaten + 1
            return PT_DOT
        }
        if c == "o" {
            self.set(col, row, " ")
            self.dots_eaten = self.dots_eaten + 1
            return PT_POWER
        }
        return 0
    }

    fn all_dots_eaten(self) {
        return self.dots_eaten >= self.dots_total
    }

    // Marca todas las celdas como dirty (para forzar redraw completo del mapa)
    fn mark_all_dirty(self) {
        let r = 0
        while r < self.h {
            let c = 0
            while c < self.w {
                self.dirty[r][c] = true
                c = c + 1
            }
            r = r + 1
        }
    }

    // Resetea el laberinto para un nuevo nivel: vuelve a poner dots y power
    fn reset_for_new_level(self) {
        self.dots_eaten = 0
        self.dots_total = 0
        let r = 0
        while r < self.h {
            let c = 0
            while c < self.w {
                let ch = substr(MAP_RAW[r], c, 1)
                self.grid[r][c] = ch
                self.dirty[r][c] = true
                if ch == "." or ch == "o" { self.dots_total = self.dots_total + 1 }
                c = c + 1
            }
            r = r + 1
        }
    }
}


// =============================================================================
// Helpers de direccion
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

// Aplica wraparound del tunel: si cambia de fila se queda igual; si va por la
// fila del tunel y se sale, wraparound.
fn apply_tunnel(col, row) {
    if row == 14 {
        if col < 0 { col = MAP_W - 1 }
        if col >= MAP_W { col = 0 }
    }
    return [col, row]
}


// =============================================================================
// Pacman
// =============================================================================

class Pacman {
    fn __init__(self) {
        self.col = 13
        self.row = 23
        self.dir = DIR_LEFT
        self.next_dir = DIR_NONE
        self.move_counter = 0
        self.anim_frame = 0
        self.alive = true
    }

    fn _peek(self, d) {
        let dlt = dir_delta(d)
        let p = apply_tunnel(self.col + dlt[0], self.row + dlt[1])
        return p
    }

    fn can_move(self, maze, d) {
        if d == DIR_NONE { return false }
        let p = self._peek(d)
        return maze.pacman_can_enter(p[0], p[1])
    }

    fn step(self, maze) {
        self.move_counter = self.move_counter + 1
        if self.move_counter < PACMAN_TICKS_PER_MOVE { return 0 }
        self.move_counter = 0

        if self.next_dir != DIR_NONE and self.can_move(maze, self.next_dir) {
            self.dir = self.next_dir
            self.next_dir = DIR_NONE
        }

        if self.can_move(maze, self.dir) {
            maze.mark_dirty(self.col, self.row)
            let p = self._peek(self.dir)
            self.col = p[0]
            self.row = p[1]
            self.anim_frame = (self.anim_frame + 1) % 4
        }

        return maze.eat(self.col, self.row)
    }

    fn glyph(self, use_unicode) {
        let f = self.anim_frame
        if use_unicode {
            let half_left   = from_char(226) + from_char(151) + from_char(144)
            let half_right  = from_char(226) + from_char(151) + from_char(145)
            let half_bottom = from_char(226) + from_char(151) + from_char(146)
            let half_top    = from_char(226) + from_char(151) + from_char(147)
            let full_circle = from_char(226) + from_char(151) + from_char(143)
            if f == 0 or f == 1 { return full_circle + " " }
            if self.dir == DIR_RIGHT { return half_left + " " }
            if self.dir == DIR_LEFT  { return " " + half_right }
            if self.dir == DIR_UP    { return half_bottom + " " }
            if self.dir == DIR_DOWN  { return half_top + " " }
            return full_circle + " "
        }
        if f == 0 or f == 1 { return "@ " }
        if self.dir == DIR_RIGHT { return "< " }
        if self.dir == DIR_LEFT  { return " >" }
        if self.dir == DIR_UP    { return "^ " }
        if self.dir == DIR_DOWN  { return "v " }
        return "@ "
    }

    fn reset_to_start(self) {
        self.col = 13
        self.row = 23
        self.dir = DIR_LEFT
        self.next_dir = DIR_NONE
        self.move_counter = 0
        self.anim_frame = 0
    }
}


// =============================================================================
// Ghost (clase base con la AI clasica)
// =============================================================================



// =============================================================================
// Ghost: clase unica para los 4 fantasmas
// =============================================================================
// El comportamiento varia segun self.kind (G_BLINKY/PINKY/INKY/CLYDE).
// La AI consiste en:
//   1. Calcular el tile objetivo segun kind y modo
//   2. Elegir la direccion que minimiza distancia^2 al tile objetivo
//      (excluyendo la direccion opuesta y las paredes/puerta cerrada)
//   3. Empate -> prioridad U > L > D > R
//
// Estados:
//   GHOST_CHASE      -> persigue (cada uno con su algoritmo)
//   GHOST_SCATTER    -> va a su esquina de scatter
//   GHOST_FRIGHTENED -> direccion al azar, mas lento
//   GHOST_EATEN      -> vuelve a la entrada de la casa
//
// Salida de la casa:
//   in_house = true cuando esta esperando dentro
//   Cuando le toca salir, target = (13, 11) y se le permite cruzar la puerta
//   Una vez fuera, in_house = false e in_house ya no es accesible salvo EATEN

class Ghost {
    fn __init__(self, kind) {
        self.kind = kind
        self.exiting_corridor = false    // se activa al salir de la casa
        self._set_initial_position()
        self.dir = DIR_LEFT
        self.move_counter = 0
        self.mode = GHOST_SCATTER
        self.dot_counter_required = 0    // dots a comer para que salga
    }

    fn _set_initial_position(self) {
        // Posiciones iniciales canonicas del Pacman original
        if self.kind == G_BLINKY {
            self.col = 13
            self.row = 11             // FUERA, encima de la puerta
            self.in_house = false
            self.exiting_corridor = true  // tiene que subir al laberinto principal
            self.start_col = 13
            self.start_row = 11
            self.scatter_col = 25
            self.scatter_row = 0
        }
        if self.kind == G_PINKY {
            self.col = 13
            self.row = 14             // Dentro, columna central izq
            self.in_house = true
            self.exiting_corridor = false
            self.start_col = 13
            self.start_row = 14
            self.scatter_col = 2
            self.scatter_row = 0
        }
        if self.kind == G_INKY {
            self.col = 11
            self.row = 14             // Dentro, slot izquierdo
            self.in_house = true
            self.exiting_corridor = false
            self.start_col = 11
            self.start_row = 14
            self.scatter_col = 27
            self.scatter_row = 30
        }
        if self.kind == G_CLYDE {
            self.col = 16
            self.row = 14             // Dentro, slot derecho
            self.in_house = true
            self.exiting_corridor = false
            self.start_col = 16
            self.start_row = 14
            self.scatter_col = 0
            self.scatter_row = 30
        }
    }

    fn name(self) {
        if self.kind == G_BLINKY { return "Blinky" }
        if self.kind == G_PINKY  { return "Pinky"  }
        if self.kind == G_INKY   { return "Inky"   }
        if self.kind == G_CLYDE  { return "Clyde"  }
        return "?"
    }

    // Velocidad actual en ticks por movimiento (depende del modo)
    fn _ticks_per_move(self) {
        if self.in_house          { return GHOST_TICKS_HOUSE  }
        if self.mode == GHOST_FRIGHTENED { return GHOST_TICKS_FRIGHT }
        if self.mode == GHOST_EATEN      { return GHOST_TICKS_EATEN  }
        return GHOST_TICKS_NORMAL
    }

    // Calcula el tile objetivo segun kind y modo
    fn _target(self, pacman, blinky) {
        if self.mode == GHOST_EATEN {
            return [13, 11]   // entrada de la casa
        }
        if self.mode == GHOST_SCATTER {
            return [self.scatter_col, self.scatter_row]
        }
        if self.mode == GHOST_FRIGHTENED {
            // En frightened no se usa target (movimiento aleatorio).
            // Devolvemos algo cualquiera.
            return [self.col, self.row]
        }

        // CHASE: cada fantasma tiene su formula
        if self.kind == G_BLINKY {
            return [pacman.col, pacman.row]
        }
        if self.kind == G_PINKY {
            // 4 casillas por delante de Pacman.
            // Bug iconico: si Pacman mira UP, el target tambien se mueve 4
            // a la izquierda (overflow del 8086 en el original).
            let p = self._ahead_of(pacman, 4)
            return p
        }
        if self.kind == G_INKY {
            // 1. Punto P = 2 casillas delante de Pacman (con bug UP)
            // 2. Vector de Blinky a P
            // 3. Target = P + vector (es decir, doblar el vector)
            let p = self._ahead_of(pacman, 2)
            let bx = blinky.col
            let by = blinky.row
            let vx = p[0] - bx
            let vy = p[1] - by
            return [p[0] + vx, p[1] + vy]
        }
        if self.kind == G_CLYDE {
            // Si esta a mas de 8 casillas de Pacman -> target = Pacman
            // Si no -> target = su esquina de scatter
            let dx = self.col - pacman.col
            let dy = self.row - pacman.row
            let dist2 = dx * dx + dy * dy
            if dist2 > 64 { return [pacman.col, pacman.row] }
            return [self.scatter_col, self.scatter_row]
        }
        return [pacman.col, pacman.row]
    }

    // Punto "n casillas delante de Pacman" segun su direccion.
    // Replica el bug del original: cuando Pacman mira UP, el target
    // se desplaza tambien n casillas a la izquierda.
    fn _ahead_of(self, pacman, n) {
        let pc = pacman.col
        let pr = pacman.row
        if pacman.dir == DIR_UP    { return [pc - n, pr - n] }   // bug iconico
        if pacman.dir == DIR_DOWN  { return [pc, pr + n] }
        if pacman.dir == DIR_LEFT  { return [pc - n, pr] }
        if pacman.dir == DIR_RIGHT { return [pc + n, pr] }
        return [pc, pr]
    }

    // ¿Puede el fantasma cruzar la puerta '-' en este tick?
    // Si:
    //   - esta saliendo de la casa (in_house en proceso de salir), o
    //   - esta EATEN (volviendo a casa)
    // En cualquier otro caso (modo normal estando fuera): NO.
    fn _can_cross_gate(self) {
        if self.in_house { return true }
        if self.mode == GHOST_EATEN { return true }
        return false
    }

    // Elige direccion segun la AI clasica
    fn _choose_direction(self, maze, target) {
        let dirs_in_order = [DIR_UP, DIR_LEFT, DIR_DOWN, DIR_RIGHT]
        let opposite = dir_opposite(self.dir)
        let can_cross = self._can_cross_gate()

        let candidates = []
        for d in dirs_in_order {
            if d == opposite { continue }
            let dlt = dir_delta(d)
            let nc = self.col + dlt[0]
            let nr = self.row + dlt[1]
            let tp = apply_tunnel(nc, nr)
            nc = tp[0]; nr = tp[1]
            let cell = maze.at(nc, nr)
            if cell == "#" { continue }
            if cell == "-" and not can_cross { continue }
            let dx = nc - target[0]
            let dy = nr - target[1]
            let dist = dx * dx + dy * dy
            append(candidates, { "dir": d, "dist": dist })
        }

        if len(candidates) == 0 { return opposite }

        let best = candidates[0]
        let i = 1
        while i < len(candidates) {
            if candidates[i]["dist"] < best["dist"] {
                best = candidates[i]
            }
            i = i + 1
        }
        return best["dir"]
    }

    // Movimiento dentro de la casa: anima arriba/abajo en su slot
    fn _step_inside_house(self, maze) {
        // Simple: alterna entre row=13 y row=15 manteniendo la columna
        if self.dir == DIR_UP {
            if self.row > 13 { self.row = self.row - 1
            } else { self.dir = DIR_DOWN; self.row = self.row + 1 }
        } else {
            if self.row < 15 { self.row = self.row + 1
            } else { self.dir = DIR_UP; self.row = self.row - 1 }
        }
    }

    // Step principal del fantasma. Devuelve true si se movio de celda.
    fn step(self, maze, pacman, blinky, dots_eaten) {
        self.move_counter = self.move_counter + 1
        if self.move_counter < self._ticks_per_move() { return false }
        self.move_counter = 0

        // Si esta dentro y aun no le toca salir: anima arriba/abajo
        if self.in_house and dots_eaten < self.dot_counter_required {
            maze.mark_dirty(self.col, self.row)
            self._step_inside_house(maze)
            return true
        }

        // Si esta dentro y le toca salir: target = (13,11), permite cruzar puerta
        if self.in_house {
            // Primero alinear a la columna central (col 13 o 14)
            let target = [13, 11]
            let new_dir = self._choose_direction(maze, target)
            self.dir = new_dir
            let dlt = dir_delta(new_dir)
            let nc = self.col + dlt[0]
            let nr = self.row + dlt[1]
            let cell = maze.at(nc, nr)
            // Validacion: no entrar a paredes
            if cell != "#" {
                maze.mark_dirty(self.col, self.row)
                self.col = nc
                self.row = nr
                // ¿Ya salio? Esta fuera cuando llega a (13, 11) o por encima.
                if self.row <= 11 {
                    self.in_house = false
                    // Marcador: aun necesita subir mas para llegar al laberinto
                    // principal. Sin esto, la AI lo manda a dar vueltas en el
                    // corredor de la fila 11 buscando una mejora local que
                    // requiere alejarse temporalmente.
                    self.exiting_corridor = true
                }
            }
            return true
        }

        // Recien salido de la casa: forzar subida hasta la fila 9 (laberinto
        // principal). Solo entonces aplicamos la AI normal. Esto evita que se
        // queden dando vueltas en el corredor de fila 11 cuyas distancias
        // euclidianas locales no premian la subida.
        if self.exiting_corridor {
            // Subir si la celda de arriba es libre
            let nr = self.row - 1
            if not maze.is_wall(self.col, nr) {
                maze.mark_dirty(self.col, self.row)
                self.row = nr
                self.dir = DIR_UP
                if self.row <= 9 { self.exiting_corridor = false }
                return true
            }
            // Si no se puede subir (estamos en una columna sin salida hacia
            // arriba), buscar lateralmente una col que SI permita subir.
            // En el corredor 11, las cols 12 y 15 son las que conectan arriba.
            let target_col = 12
            if self.col > 13 { target_col = 15 }
            let dx = target_col - self.col
            let want_dir = DIR_RIGHT
            if dx < 0 { want_dir = DIR_LEFT }
            // Probar el lateral
            let dlt = dir_delta(want_dir)
            let nc = self.col + dlt[0]
            let nr2 = self.row + dlt[1]
            if not maze.is_wall(nc, nr2) {
                maze.mark_dirty(self.col, self.row)
                self.col = nc
                self.row = nr2
                self.dir = want_dir
                return true
            }
            // Caso degenerado: cancelar exiting_corridor y dejar AI normal
            self.exiting_corridor = false
        }

        // Modo EATEN: si llegamos a la entrada de la casa, "respawneamos"
        if self.mode == GHOST_EATEN {
            if self.col == 13 and self.row == 11 {
                // Volver al estado inicial. Si el fantasma originalmente
                // empieza fuera (Blinky), respawnea fuera con exiting_corridor
                // para que se reincorpore al laberinto. Si empieza dentro
                // (Pinky/Inky/Clyde), entra a la casa para salir como nuevo.
                self.mode = GHOST_SCATTER
                if self.kind == G_BLINKY {
                    self.in_house = false
                    self.exiting_corridor = true
                    self.col = 13
                    self.row = 11
                    self.dir = DIR_LEFT
                } else {
                    self.in_house = true
                    self.exiting_corridor = false
                    self.col = self.start_col
                    self.row = self.start_row
                    self.dir = DIR_UP
                }
                return true
            }
        }

        // Modo FRIGHTENED: direccion aleatoria entre validas
        if self.mode == GHOST_FRIGHTENED {
            let dirs_all = [DIR_UP, DIR_LEFT, DIR_DOWN, DIR_RIGHT]
            let opposite = dir_opposite(self.dir)
            let valid = []
            for d in dirs_all {
                if d == opposite { continue }
                let dlt = dir_delta(d)
                let nc = self.col + dlt[0]
                let nr = self.row + dlt[1]
                let tp = apply_tunnel(nc, nr)
                nc = tp[0]; nr = tp[1]
                let cell = maze.at(nc, nr)
                if cell == "#" { continue }
                if cell == "-" { continue }
                append(valid, d)
            }
            if len(valid) == 0 {
                self.dir = opposite
            } else {
                // Pseudo-random simple basado en posicion + counter
                let idx = (self.col * 7 + self.row * 13 + self.move_counter * 3) % len(valid)
                self.dir = valid[idx]
            }
        } else {
            // CHASE/SCATTER/EATEN normal: AI clasica
            let target = self._target(pacman, blinky)
            self.dir = self._choose_direction(maze, target)
        }

        // Aplicar movimiento
        let dlt = dir_delta(self.dir)
        let nc = self.col + dlt[0]
        let nr = self.row + dlt[1]
        let tp = apply_tunnel(nc, nr)
        nc = tp[0]; nr = tp[1]
        let cell = maze.at(nc, nr)
        let blocked = cell == "#"
        if cell == "-" and not self._can_cross_gate() { blocked = true }
        if not blocked {
            maze.mark_dirty(self.col, self.row)
            self.col = nc
            self.row = nr
        }
        return true
    }

    // Reset al inicio del nivel o tras vida perdida
    fn reset_to_start(self) {
        self._set_initial_position()
        self.dir = DIR_LEFT
        self.move_counter = 0
        self.mode = GHOST_SCATTER
    }

    fn glyph(self, use_unicode) {
        if use_unicode {
            let l = from_char(226) + from_char(150) + from_char(159)  // ▟
            let r = from_char(226) + from_char(150) + from_char(153)  // ▙
            return l + r
        }
        return "MM"
    }
}


// =============================================================================
// Input
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
            self.wall_h     = from_char(226) + from_char(149) + from_char(144)
            self.wall_v     = from_char(226) + from_char(149) + from_char(145)
            self.wall_tl    = from_char(226) + from_char(149) + from_char(148)
            self.wall_tr    = from_char(226) + from_char(149) + from_char(151)
            self.wall_bl    = from_char(226) + from_char(149) + from_char(154)
            self.wall_br    = from_char(226) + from_char(149) + from_char(157)
            self.wall_t     = from_char(226) + from_char(149) + from_char(166)
            self.wall_b     = from_char(226) + from_char(149) + from_char(169)
            self.wall_l     = from_char(226) + from_char(149) + from_char(160)
            self.wall_r     = from_char(226) + from_char(149) + from_char(163)
            self.wall_x     = from_char(226) + from_char(149) + from_char(172)
            self.wall_block = from_char(226) + from_char(150) + from_char(136)
            self.dot        = from_char(194) + from_char(183)
            self.power      = from_char(226) + from_char(151) + from_char(143)
            self.gate       = from_char(226) + from_char(148) + from_char(128)
        } else {
            self.wall_h     = "="
            self.wall_v     = "|"
            self.wall_tl    = "+"
            self.wall_tr    = "+"
            self.wall_bl    = "+"
            self.wall_br    = "+"
            self.wall_t     = "+"
            self.wall_b     = "+"
            self.wall_l     = "+"
            self.wall_r     = "+"
            self.wall_x     = "+"
            self.wall_block = "#"
            self.dot        = "."
            self.power      = "O"
            self.gate       = "-"
        }
    }

    fn _setup_colors(self) {
        self.col_wall   = ansi_rgb(33, 33, 222)
        self.col_dot    = ansi_rgb(255, 184, 151)
        self.col_power  = ansi_rgb(255, 184, 151)
        self.col_gate   = ansi_rgb(255, 184, 222)
        self.col_pacman = ansi_rgb(255, 255, 0)
        // Colores oficiales de los fantasmas
        self.col_blinky   = ansi_rgb(255,   0,   0)   // rojo
        self.col_pinky    = ansi_rgb(255, 184, 222)   // rosa
        self.col_inky     = ansi_rgb(  0, 255, 255)   // cyan
        self.col_clyde    = ansi_rgb(255, 184,  71)   // naranja
        self.col_frighten = ansi_rgb( 33,  33, 222)   // azul (asustado)
        self.col_text   = ansi_rgb(255, 255, 255)
        self.col_score  = ansi_rgb(255, 255, 0)
        self.col_dim    = ansi_rgb(120, 120, 120)
        self.R = ANSI["RESET"]
    }

    fn _wall_glyph(self, maze, col, row) {
        if not self.config.use_unicode { return self.wall_block + self.wall_block }

        let up    = maze.is_wall(col, row - 1)
        let down  = maze.is_wall(col, row + 1)
        let left  = maze.is_wall(col - 1, row)
        let right = maze.is_wall(col + 1, row)

        if up and down and not left and not right { return self.wall_v + self.wall_v }
        if left and right and not up and not down { return self.wall_h + self.wall_h }
        if up and not down and not left and not right { return self.wall_v + self.wall_v }
        if down and not up and not left and not right { return self.wall_v + self.wall_v }
        if left and not right and not up and not down { return self.wall_h + self.wall_h }
        if right and not left and not up and not down { return self.wall_h + self.wall_h }

        if right and down and not left and not up    { return self.wall_tl + self.wall_h }
        if left  and down and not right and not up   { return self.wall_h  + self.wall_tr }
        if right and up   and not left and not down  { return self.wall_bl + self.wall_h }
        if left  and up   and not right and not down { return self.wall_h  + self.wall_br }

        if up and down and right and not left  { return self.wall_l + self.wall_h }
        if up and down and left and not right  { return self.wall_h + self.wall_r }
        if left and right and down and not up  { return self.wall_h + self.wall_t }
        if left and right and up and not down  { return self.wall_h + self.wall_b }

        if up and down and left and right { return self.wall_x + self.wall_h }

        return self.wall_block + self.wall_block
    }

    fn _cell_glyph(self, maze, col, row) {
        let ch = maze.at(col, row)
        if ch == "#" { return self.col_wall + self._wall_glyph(maze, col, row) + self.R }
        if ch == "." { return self.col_dot + " " + self.dot + self.R }
        if ch == "o" { return self.col_power + " " + self.power + self.R }
        if ch == "-" { return self.col_gate + self.gate + self.gate + self.R }
        return "  "
    }

    fn draw_maze(self, maze, ox, oy) {
        let r = 0
        while r < maze.h {
            let c = 0
            while c < maze.w {
                if self.full_redraw or maze.dirty[r][c] {
                    self._w(ansi_cursor_pos(oy + r, ox + c * 2))
                    self._w(self._cell_glyph(maze, c, r))
                    maze.dirty[r][c] = false
                }
                c = c + 1
            }
            r = r + 1
        }
    }

    fn draw_pacman(self, pac, ox, oy) {
        self._w(ansi_cursor_pos(oy + pac.row, ox + pac.col * 2))
        self._w(self.col_pacman + ANSI["BOLD"] + pac.glyph(self.config.use_unicode) + self.R)
    }

    fn draw_ghost(self, ghost, ox, oy, frighten_timer) {
        self._w(ansi_cursor_pos(oy + ghost.row, ox + ghost.col * 2))

        // Sprite especial para EATEN: solo "ojos"
        if ghost.mode == GHOST_EATEN {
            let glyph = "''"
            if self.config.use_unicode {
                // U+00B0 (°) como aproximacion de ojos pequenos
                let dot = from_char(194) + from_char(176)   // °
                glyph = dot + dot
            }
            self._w(ansi_rgb(220, 220, 255) + ANSI["BOLD"] + glyph + self.R)
            return
        }

        // Color base segun kind
        let col = self.col_blinky
        if ghost.kind == G_PINKY { col = self.col_pinky }
        if ghost.kind == G_INKY  { col = self.col_inky  }
        if ghost.kind == G_CLYDE { col = self.col_clyde }

        // Modo FRIGHTENED: azul, con parpadeo a blanco en los ultimos 2s
        if ghost.mode == GHOST_FRIGHTENED {
            col = self.col_frighten
            // Parpadeo: en los ultimos FRIGHT_BLINK_FRAMES, alternar cada 8 frames
            if frighten_timer > 0 and frighten_timer < FRIGHT_BLINK_FRAMES {
                let phase = (frighten_timer / 8) % 2
                if phase == 0 { col = ansi_rgb(255, 255, 255) }
            }
        }

        self._w(col + ANSI["BOLD"] + ghost.glyph(self.config.use_unicode) + self.R)
    }

    fn draw_popup(self, popup, ox, oy) {
        // Posicionar; puede salirse del mapa, en cuyo caso ignoramos
        let py = oy + popup["row"]
        let px = ox + popup["col"] * 2
        self._w(ansi_cursor_pos(py, px))
        self._w(self.col_score + ANSI["BOLD"] + popup["text"] + self.R)
    }

    fn draw_hud(self, score, high, lives, level, ox, oy) {
        self._w(ansi_cursor_pos(oy, ox))
        let s = self.col_text + "1UP  " + self.col_score + pad_left(str(score), 6) + "   "
        s = s + self.col_text + "HIGH SCORE  " + self.col_score + pad_left(str(high), 6) + "   "
        s = s + self.col_text + "LIVES " + self.col_pacman + str(lives) + "   "
        s = s + self.col_text + "LEVEL " + str(level) + self.R
        self._w(s)
    }

    fn draw_status(self, msg, ox, oy) {
        self._w(ansi_cursor_pos(oy, ox))
        self._w(ANSI["CLEAR_LINE"])
        self._w(self.col_dim + msg + self.R)
    }
}


// =============================================================================
// Game
// =============================================================================

class Game {
    fn __init__(self, config) {
        self.config = config
        self.maze = Maze()
        self.pacman = Pacman()
        self.blinky = Ghost(G_BLINKY)
        self.pinky  = Ghost(G_PINKY)
        self.inky   = Ghost(G_INKY)
        self.clyde  = Ghost(G_CLYDE)
        self.pinky.dot_counter_required = 0
        self.inky.dot_counter_required  = 30
        self.clyde.dot_counter_required = 60
        self.ghosts = [self.blinky, self.pinky, self.inky, self.clyde]

        self.renderer = Renderer(config)
        self.score = 0
        self.high_score = load_high_score()
        self.lives = 3
        self.level = 1
        self.paused = false
        self.running = true

        // State machine
        self.state = STATE_SPLASH
        self.state_timer = 0

        // Ciclo Scatter/Chase del nivel 1
        self.sc_phases = [
            { "mode": GHOST_SCATTER, "frames": 210 },
            { "mode": GHOST_CHASE,   "frames": 600 },
            { "mode": GHOST_SCATTER, "frames": 210 },
            { "mode": GHOST_CHASE,   "frames": 600 },
            { "mode": GHOST_SCATTER, "frames": 150 },
            { "mode": GHOST_CHASE,   "frames": 600 },
            { "mode": GHOST_SCATTER, "frames": 150 },
            { "mode": GHOST_CHASE,   "frames": -1 }
        ]
        self.phase_idx = 0
        self.phase_timer = 0

        // Frightened
        self.frighten_timer = 0
        self.ghost_combo = 0
        self.popups = []

        // Fruta
        self.fruit_active = false
        self.fruit_timer = 0
        self.fruit_count = 0          // 0/1/2 frutas que han aparecido en este nivel

        // Animacion de muerte
        self.dying_anim_frame = 0

        // Animacion de fin de nivel: laberinto parpadea
        self.level_done_anim = 0

        let ts = TermSize()
        self.term_w = ts.cols
        self.term_h = ts.rows
        let maze_w_chars = MAP_W * 2
        self.ox = (self.term_w - maze_w_chars) / 2
        self.oy = (self.term_h - MAP_H - 4) / 2 + 2
        if self.ox < 1 { self.ox = 1 }
        if self.oy < 2 { self.oy = 2 }
    }

    // -------------------------------------------------------------------------
    // Manejo de input
    // -------------------------------------------------------------------------

    fn handle_key(self, k) {
        if k == 113 or k == 81 or k == KEY_ESC { self.running = false; return }

        if self.state == STATE_SPLASH {
            // Cualquier tecla empieza el juego
            self._start_new_game()
            return
        }
        if self.state == STATE_GAME_OVER {
            // En game over solo se sale con Q (ya manejado arriba)
            return
        }

        if k == 112 or k == 80 {
            self.paused = not self.paused
            return
        }
        if self.paused { return }

        // Direcciones solo durante PLAYING
        if self.state != STATE_PLAYING { return }
        if k == KEY_UP    or k ==  87 or k == 119 { self.pacman.next_dir = DIR_UP    }
        if k == KEY_DOWN  or k ==  83 or k == 115 { self.pacman.next_dir = DIR_DOWN  }
        if k == KEY_LEFT  or k ==  65 or k ==  97 { self.pacman.next_dir = DIR_LEFT  }
        if k == KEY_RIGHT or k ==  68 or k == 100 { self.pacman.next_dir = DIR_RIGHT }
    }

    // -------------------------------------------------------------------------
    // Transiciones de estado
    // -------------------------------------------------------------------------

    fn _start_new_game(self) {
        // Reset completo del estado de partida
        self.score = 0
        self.lives = 3
        self.level = 1
        self.maze.reset_for_new_level()
        self.pacman.reset_to_start()
        for g in self.ghosts { g.reset_to_start() }
        self.frighten_timer = 0
        self.ghost_combo = 0
        self.popups = []
        self.fruit_active = false
        self.fruit_timer = 0
        self.fruit_count = 0
        self.phase_idx = 0
        self.phase_timer = 0
        self.state = STATE_READY
        self.state_timer = READY_FRAMES
        self.renderer.full_redraw = true
    }

    fn _on_pacman_died(self) {
        self.state = STATE_DYING
        self.state_timer = DYING_FRAMES
        self.dying_anim_frame = 0
        self.frighten_timer = 0
        self.ghost_combo = 0
        self.popups = []
    }

    fn _on_dying_done(self) {
        self.lives = self.lives - 1
        if self.lives <= 0 {
            // Game Over
            if self.score > self.high_score {
                self.high_score = self.score
                save_high_score(self.high_score)
            }
            self.state = STATE_GAME_OVER
            self.state_timer = GAME_OVER_FRAMES
            self.renderer.full_redraw = true
            return
        }
        // Quedan vidas: respawn
        self.pacman.reset_to_start()
        for g in self.ghosts { g.reset_to_start() }
        self.phase_idx = 0
        self.phase_timer = 0
        self.state = STATE_READY
        self.state_timer = READY_FRAMES
        self.renderer.full_redraw = true
    }

    fn _on_level_done(self) {
        // Animacion: laberinto parpadea unos frames
        self.state = STATE_LEVEL_DONE
        self.state_timer = LEVEL_DONE_FRAMES
        self.level_done_anim = 0
        self.frighten_timer = 0
        self.ghost_combo = 0
        self.popups = []
        self.fruit_active = false
    }

    fn _on_level_done_finished(self) {
        self.level = self.level + 1
        self.maze.reset_for_new_level()
        self.pacman.reset_to_start()
        for g in self.ghosts { g.reset_to_start() }
        self.fruit_active = false
        self.fruit_timer = 0
        self.fruit_count = 0
        self.phase_idx = 0
        self.phase_timer = 0
        self.state = STATE_READY
        self.state_timer = READY_FRAMES
        self.renderer.full_redraw = true
    }

    fn _on_game_over_done(self) {
        self.state = STATE_SPLASH
        self.state_timer = 0
        self.renderer.full_redraw = true
    }

    // -------------------------------------------------------------------------
    // Subsistemas (solo activos durante PLAYING)
    // -------------------------------------------------------------------------

    fn _update_phase(self) {
        let phase = self.sc_phases[self.phase_idx]
        let frames = phase["frames"]
        if frames == -1 { return }
        self.phase_timer = self.phase_timer + 1
        if self.phase_timer >= frames {
            self.phase_idx = self.phase_idx + 1
            if self.phase_idx >= len(self.sc_phases) {
                self.phase_idx = len(self.sc_phases) - 1
            }
            self.phase_timer = 0
            let new_mode = self.sc_phases[self.phase_idx]["mode"]
            for g in self.ghosts {
                if g.mode == GHOST_FRIGHTENED { continue }
                if g.mode == GHOST_EATEN { continue }
                g.mode = new_mode
                g.dir = dir_opposite(g.dir)
            }
        }
    }

    fn _current_phase_mode(self) {
        return self.sc_phases[self.phase_idx]["mode"]
    }

    fn _on_power_pellet(self) {
        self.frighten_timer = FRIGHT_FRAMES
        self.ghost_combo = 0
        for g in self.ghosts {
            if g.in_house { continue }
            if g.mode == GHOST_EATEN { continue }
            g.mode = GHOST_FRIGHTENED
            g.dir = dir_opposite(g.dir)
            g.move_counter = 0
        }
    }

    fn _update_frighten(self) {
        if self.frighten_timer <= 0 { return }
        self.frighten_timer = self.frighten_timer - 1
        if self.frighten_timer <= 0 {
            let phase_mode = self._current_phase_mode()
            for g in self.ghosts {
                if g.mode == GHOST_FRIGHTENED {
                    g.mode = phase_mode
                    g.move_counter = 0
                }
            }
            self.ghost_combo = 0
        }
    }

    fn _add_popup(self, col, row, points) {
        append(self.popups, { "col": col, "row": row, "text": "+" + str(points), "frames": POPUP_FRAMES })
    }

    fn _update_popups(self) {
        let alive = []
        for p in self.popups {
            p["frames"] = p["frames"] - 1
            if p["frames"] > 0 {
                append(alive, p)
            } else {
                self.maze.mark_dirty(p["col"], p["row"])
            }
        }
        self.popups = alive
    }

    fn _update_fruit(self) {
        // Aparicion: a 70 dots o 170 dots
        if not self.fruit_active {
            if self.fruit_count == 0 and self.maze.dots_eaten >= FRUIT_DOTS_1 {
                self.fruit_active = true
                self.fruit_timer = FRUIT_DURATION
                self.fruit_count = 1
                self.maze.mark_dirty(FRUIT_COL, FRUIT_ROW)
            } elif self.fruit_count == 1 and self.maze.dots_eaten >= FRUIT_DOTS_2 {
                self.fruit_active = true
                self.fruit_timer = FRUIT_DURATION
                self.fruit_count = 2
                self.maze.mark_dirty(FRUIT_COL, FRUIT_ROW)
            }
            return
        }
        // Activa: decrementar timer y ver si Pacman la come
        self.fruit_timer = self.fruit_timer - 1
        if self.pacman.col == FRUIT_COL and self.pacman.row == FRUIT_ROW {
            let pts = fruit_points_for_level(self.level)
            self.score = self.score + pts
            if self.score > self.high_score { self.high_score = self.score }
            self._add_popup(FRUIT_COL, FRUIT_ROW, pts)
            self.fruit_active = false
            self.maze.mark_dirty(FRUIT_COL, FRUIT_ROW)
        }
        if self.fruit_timer <= 0 {
            self.fruit_active = false
            self.maze.mark_dirty(FRUIT_COL, FRUIT_ROW)
        }
    }

    fn _check_collision(self) {
        for g in self.ghosts {
            if self.pacman.col == g.col and self.pacman.row == g.row {
                if g.mode == GHOST_FRIGHTENED {
                    self.ghost_combo = self.ghost_combo + 1
                    let pts = PT_GHOST_1
                    if self.ghost_combo == 2 { pts = PT_GHOST_2 }
                    if self.ghost_combo == 3 { pts = PT_GHOST_3 }
                    if self.ghost_combo == 4 { pts = PT_GHOST_4 }
                    self.score = self.score + pts
                    if self.score > self.high_score { self.high_score = self.score }
                    self._add_popup(g.col, g.row, pts)
                    g.mode = GHOST_EATEN
                    g.move_counter = 0
                } elif g.mode == GHOST_EATEN {
                    // Es solo ojos, no pasa nada
                } else {
                    self._on_pacman_died()
                    return
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // Tick principal
    // -------------------------------------------------------------------------

    fn tick(self) {
        if self.paused { return }

        if self.state == STATE_SPLASH {
            // Animacion de splash si la hay; por ahora estatico
            return
        }

        if self.state == STATE_READY {
            self.state_timer = self.state_timer - 1
            if self.state_timer <= 0 {
                self.state = STATE_PLAYING
                self.renderer.full_redraw = true
            }
            return
        }

        if self.state == STATE_DYING {
            self.dying_anim_frame = self.dying_anim_frame + 1
            self.state_timer = self.state_timer - 1
            if self.state_timer <= 0 {
                self._on_dying_done()
            }
            return
        }

        if self.state == STATE_LEVEL_DONE {
            self.level_done_anim = self.level_done_anim + 1
            self.state_timer = self.state_timer - 1
            if self.state_timer <= 0 {
                self._on_level_done_finished()
            }
            return
        }

        if self.state == STATE_GAME_OVER {
            self.state_timer = self.state_timer - 1
            if self.state_timer <= 0 {
                self._on_game_over_done()
            }
            return
        }

        // STATE_PLAYING
        self._update_phase()
        self._update_frighten()
        self._update_popups()
        self._update_fruit()

        let pts = self.pacman.step(self.maze)
        if pts > 0 {
            self.score = self.score + pts
            if self.score > self.high_score { self.high_score = self.score }
            if pts == PT_POWER {
                self._on_power_pellet()
            }
        }
        self._check_collision()
        if self.state != STATE_PLAYING { return }

        for g in self.ghosts {
            g.step(self.maze, self.pacman, self.blinky, self.maze.dots_eaten)
        }
        self._check_collision()
        if self.state != STATE_PLAYING { return }

        // ¿Nivel completado?
        if self.maze.all_dots_eaten() {
            self._on_level_done()
        }
    }

    // -------------------------------------------------------------------------
    // Render
    // -------------------------------------------------------------------------

    fn _draw_lives_indicator(self, ox, oy) {
        // Pequenos pacmans al pie del laberinto
        self.renderer._w(ansi_cursor_pos(oy, ox))
        let i = 0
        let s = ""
        while i < self.lives - 1 {
            // Cada vida: glifo de pacman amarillo (sin contar la actual)
            let glyph = "@ "
            if self.config.use_unicode {
                let half = from_char(226) + from_char(151) + from_char(144)   // ◐
                glyph = half + " "
            }
            s = s + ansi_rgb(255, 255, 0) + ANSI["BOLD"] + glyph + ANSI["RESET"]
            i = i + 1
        }
        self.renderer._w(s)
    }

    fn _draw_fruit_indicator(self, ox, oy) {
        // Fruta del nivel actual al pie derecho
        let glyph = fruit_glyph(self.level, self.config.use_unicode)
        let col = fruit_color_for_level(self.level)
        self.renderer._w(ansi_cursor_pos(oy, ox))
        self.renderer._w(col + ANSI["BOLD"] + glyph + ANSI["RESET"])
    }

    fn _draw_centered_text(self, text, color, ox, oy, maze_w, row_in_maze) {
        // Dibuja text centrado horizontalmente sobre el laberinto en row_in_maze
        let maze_w_chars = maze_w * 2
        let text_w = len(text)
        let x = ox + (maze_w_chars - text_w) / 2
        let y = oy + row_in_maze
        self.renderer._w(ansi_cursor_pos(y, x))
        self.renderer._w(color + ANSI["BOLD"] + text + ANSI["RESET"])
    }

    fn _draw_overlay_text(self, ox, maze_y) {
        // Mensajes flotantes segun estado
        if self.state == STATE_READY {
            self._draw_centered_text("READY!", ansi_rgb(255, 255, 0), ox, maze_y, MAP_W, 17)
        }
        if self.state == STATE_GAME_OVER {
            self._draw_centered_text("GAME  OVER", ansi_rgb(255, 0, 0), ox, maze_y, MAP_W, 17)
        }
    }

    fn _draw_pacman_dying(self, ox, maze_y) {
        // Animacion: alternamos colores y "encogemos" reduciendo el sprite a "."
        let frame = self.dying_anim_frame
        let glyphs = []
        if self.config.use_unicode {
            let full = from_char(226) + from_char(151) + from_char(143)
            let half_left = from_char(226) + from_char(151) + from_char(144)
            let half_right = from_char(226) + from_char(151) + from_char(145)
            let dot = from_char(194) + from_char(183)
            glyphs = [full + " ", half_left + " ", " " + half_right, full + " ", dot + " ", "  "]
        } else {
            glyphs = ["@ ", "C ", " >", "@ ", ". ", "  "]
        }
        let idx = (frame / 10) % len(glyphs)
        let g = glyphs[idx]
        self.renderer._w(ansi_cursor_pos(maze_y + self.pacman.row, ox + self.pacman.col * 2))
        self.renderer._w(ansi_rgb(255, 255, 0) + ANSI["BOLD"] + g + ANSI["RESET"])
    }

    fn render(self) {
        let r = self.renderer
        let hud_y = self.oy - 2
        let maze_y = self.oy
        let bottom_y = self.oy + MAP_H + 1

        if r.full_redraw {
            r._w(ANSI["CLEAR"])
            r._w(ESC_HOME)
            r._w(ESC_CUR_HIDE)
        }

        // SPLASH: pantalla aparte
        if self.state == STATE_SPLASH {
            self._render_splash(maze_y)
            r.full_redraw = false
            r._flush()
            return
        }

        // HUD siempre visible (excepto en splash)
        r.draw_hud(self.score, self.high_score, self.lives, self.level, self.ox, hud_y)

        // LEVEL_DONE: laberinto parpadea
        if self.state == STATE_LEVEL_DONE {
            // Cada 8 frames cambia color del laberinto
            let phase = (self.level_done_anim / 8) % 2
            if phase == 0 {
                r.col_wall = ansi_rgb(255, 255, 255)
            } else {
                r.col_wall = ansi_rgb(33, 33, 222)
            }
            self.maze.mark_all_dirty()
            r.draw_maze(self.maze, self.ox, maze_y)
            self._draw_centered_text("LEVEL " + str(self.level) + " CLEARED!", ansi_rgb(255, 255, 0), self.ox, maze_y, MAP_W, 17)
            r.full_redraw = false
            r._flush()
            return
        }

        // Restaurar color de pared normal por si veniamos de level_done
        r.col_wall = ansi_rgb(33, 33, 222)

        r.draw_maze(self.maze, self.ox, maze_y)

        // Fruta
        if self.fruit_active {
            r._w(ansi_cursor_pos(maze_y + FRUIT_ROW, self.ox + FRUIT_COL * 2))
            r._w(fruit_color_for_level(self.level) + ANSI["BOLD"] + fruit_glyph(self.level, self.config.use_unicode) + ANSI["RESET"])
        }

        // Pacman: distinto segun estado
        if self.state == STATE_DYING {
            self._draw_pacman_dying(self.ox, maze_y)
        } else {
            r.draw_pacman(self.pacman, self.ox, maze_y)
        }

        // Fantasmas (no se dibujan en DYING ni en game over)
        if self.state == STATE_PLAYING or self.state == STATE_READY {
            for g in self.ghosts {
                r.draw_ghost(g, self.ox, maze_y, self.frighten_timer)
            }
        }

        // Popups
        for p in self.popups {
            r.draw_popup(p, self.ox, maze_y)
        }

        // Overlay text (READY / GAME OVER)
        self._draw_overlay_text(self.ox, maze_y)

        // Footer: indicador de vidas + fruta del nivel
        r._w(ansi_cursor_pos(bottom_y, self.ox))
        r._w(ANSI["CLEAR_LINE"])
        self._draw_lives_indicator(self.ox, bottom_y)
        self._draw_fruit_indicator(self.ox + MAP_W * 2 - 4, bottom_y)

        // Status (linea por debajo del footer)
        let mode_str = "?"
        let m = self._current_phase_mode()
        if m == GHOST_CHASE   { mode_str = "CHASE"   }
        if m == GHOST_SCATTER { mode_str = "SCATTER" }
        if self.frighten_timer > 0 {
            mode_str = "FRIGHT(" + str(self.frighten_timer / 30 + 1) + "s)"
        }

        let msg = "Flechas/WASD  P pausa  Q salir  | " + mode_str + "  | Dots: " + str(self.maze.dots_eaten) + "/" + str(self.maze.dots_total)
        if self.paused { msg = "*** PAUSA ***  P para reanudar" }
        r.draw_status(msg, self.ox, bottom_y + 1)

        r.full_redraw = false
        r._flush()
    }

    fn _render_splash(self, maze_y) {
        let r = self.renderer
        r._w(ANSI["CLEAR"])
        r._w(ESC_HOME)

        // Logo PACMAN (texto grande con caracteres de bloque)
        let title = "P A C M A N"
        let sub   = "TUI EDITION"
        let press = "PRESS ANY KEY TO START"
        let hi    = "HIGH SCORE: " + str(self.high_score)
        let quit  = "Q to quit"

        let center_x = self.ox + MAP_W
        let base_y = maze_y + 5

        r._w(ansi_cursor_pos(base_y, center_x - len(title) / 2))
        r._w(ansi_rgb(255, 255, 0) + ANSI["BOLD"] + title + ANSI["RESET"])

        r._w(ansi_cursor_pos(base_y + 2, center_x - len(sub) / 2))
        r._w(ansi_rgb(120, 200, 255) + sub + ANSI["RESET"])

        r._w(ansi_cursor_pos(base_y + 5, center_x - len(hi) / 2))
        r._w(ansi_rgb(255, 100, 100) + hi + ANSI["RESET"])

        r._w(ansi_cursor_pos(base_y + 8, center_x - len(press) / 2))
        r._w(ansi_rgb(255, 255, 255) + ANSI["BOLD"] + press + ANSI["RESET"])

        r._w(ansi_cursor_pos(base_y + 10, center_x - len(quit) / 2))
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
    println("Dots comidos: " + str(game.maze.dots_eaten) + " / " + str(game.maze.dots_total))
    println("Muertes: " + str(game.deaths))
}

main()
