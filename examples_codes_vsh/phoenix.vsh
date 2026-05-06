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
let STATE_WAVE_DONE = 3
let STATE_DYING     = 4
let STATE_GAME_OVER = 5

let READY_MS     = 1500
let WAVE_DONE_MS = 1800
let DYING_MS     = 1500
let GAME_OVER_MS = 5000

// Enemigos: pajaros pequenos
let SMALL_BIRD_W = 2
let SMALL_BIRD_HP = 1
let PT_SMALL_BIRD = 50

// Pajaros grandes (oleadas 3-4): 4 chars, 2 HP
let BIG_BIRD_W = 4
let BIG_BIRD_HP = 2
let PT_BIG_BIRD = 150

// Huevos que sueltan los pajaros grandes
let EGG_FALL_MS = 120                 // velocidad de caida del huevo
let EGG_W = 2                         // 2 chars de ancho
let PT_EGG = 25                       // puntos por destruir un huevo
let EGG_HATCH_PROB = 50               // de 100 huevos que llegan al suelo, eclosionan

// Boss (oleada 5)
let BOSS_W = 16
let BOSS_HP = 12                      // disparos para abrir la barriga
let BOSS_ALIEN_HP = 5                 // disparos al alien una vez abierta
let PT_BOSS_PIECE = 100
let PT_BOSS = 5000
let BOSS_MOVE_MS = 250                // cada cuanto se mueve 1 celda
let BOSS_SHOT_PROB = 200              // de 10000 ticks, mas frecuente que pajaros

// === Tipos de enemigos extra ===

// Sniper: pequeno como un pajaro normal pero apunta al jugador
let SNIPER_HP = 1
let PT_SNIPER = 80
let SNIPER_SHOT_PROB = 30             // mas que pajaro normal pero menos que tank

// Tank: 4 chars de ancho, 4 HP, no suelta huevos
let TANK_W = 4
let TANK_HP = 4
let PT_TANK = 250
let TANK_SHOT_PROB = 15

// Mosquito: 1 char, 1 HP, mucho mas rapido en el patron
let MOSQUITO_W = 1
let MOSQUITO_HP = 1
let PT_MOSQUITO = 70
let MOSQUITO_SHOT_PROB = 8

// === Patrones de formacion ===
let PATTERN_OSCILLATE = 0     // oscilacion lateral (actual)
let PATTERN_WAVE      = 1     // filas desfasadas (onda viajando)
let PATTERN_DESCEND   = 2     // zigzag con descenso lento
let DESCENT_MS = 1500         // cada cuanto baja 1 fila en patron descend
let MAX_DESCENT_ROWS = 8      // hasta donde puede bajar la formacion

// === Picados (dives) ===
let DIVE_INTERVAL_MS = 7000   // cada cuanto sale un pajaro a picar
let DIVE_MS_PER_ROW = 80      // velocidad del picado (ms por fila)
let DIVE_RETURN_BONUS = 100   // bonus si el pajaro completa el picado y vuelve
let PT_DIVE_KILL = 200        // bonus extra por matarlo durante el picado

// === Armas del jugador ===
let WEAPON_SINGLE  = 0
let WEAPON_DOUBLE  = 1
let WEAPON_SPREAD  = 2
let WEAPON_LASER   = 3
let WEAPON_MISSILE = 4

let WEAPON_DURATION_MS = 10000        // duracion de armas adquiridas

let SINGLE_COOLDOWN_MS  = 200
let DOUBLE_COOLDOWN_MS  = 200
let SPREAD_COOLDOWN_MS  = 280
let LASER_COOLDOWN_MS   = 600
let MISSILE_COOLDOWN_MS = 400

let LASER_DURATION_MS   = 250         // tiempo que el rayo permanece visible
let MISSILE_SPEED_MS    = 50          // tiempo entre pasos del misil

// === Power-ups ===
// Existing: POWERUP_NONE, POWERUP_DOUBLE, POWERUP_SHIELD, POWERUP_LIFE
// (definidos mas abajo en el archivo, los actualizamos alli)

// Disparos enemigos
let ENEMY_SHOT_SPEED_MS = 80
let ENEMY_SHOT_PROB_PER_TICK = 5

// Bonus
let WAVE_CLEAR_BONUS = 100

// Movimiento de la formacion
let FORMATION_AMPLITUDE = 8
let FORMATION_PERIOD_MS = 4000

// Numero total de oleadas en una vuelta del juego (5 = 1 ciclo Phoenix)
let WAVES_PER_LOOP = 5

// Vidas
let LIVES_START = 3

// Escudo
let SHIELD_DURATION_MS = 1500         // dura 1.5s al activarse
let SHIELD_COOLDOWN_MS = 6000         // cooldown total de 6s
let SHIELD_INVULN_AFTER_HIT_MS = 1500  // tras perder vida, invulnerable este tiempo

// Power-ups
let POWERUP_FALL_MS = 200             // caen lento
let POWERUP_W = 2
let PT_POWERUP_PICKUP = 200
let POWERUP_DROP_PROB = 800           // de 10000 cuando muere un pajaro
let POWERUP_DOUBLE_DURATION_MS = 8000 // (no usado tras el cambio a WEAPON_DURATION_MS)

let POWERUP_NONE    = 0
let POWERUP_DOUBLE  = 1               // arma DOUBLE
let POWERUP_SHIELD  = 2               // recarga el escudo
let POWERUP_LIFE    = 3               // +1 vida
let POWERUP_SPREAD  = 4               // arma SPREAD
let POWERUP_LASER   = 5               // arma LASER
let POWERUP_MISSILE = 6               // arma MISSILE

// --- Enemigos nuevos ---
let KAMIKAZE_HP = 1
let KAMIKAZE_W  = 2
let PT_KAMIKAZE = 120
let KAMIKAZE_DIVE_SPEED_MS = 40       // muy rapido bajando

let SPLITTER_HP = 2
let SPLITTER_W  = 2
let PT_SPLITTER = 180
let SPLIT_MINI_HP = 1
let SPLIT_MINI_W  = 1
let PT_SPLIT_MINI = 40

let SHIELDED_HP = 2
let SHIELDED_W  = 2
let PT_SHIELDED = 200

let BOMBER_HP = 3
let BOMBER_W  = 6
let PT_BOMBER = 300
let BOMB_FALL_MS = 100
let BOMB_BLAST_RADIUS = 1             // 1 celda en cada direccion

// --- Patrones de formacion extra ---
let PATTERN_CIRCLE    = 3             // formacion rota en ovalo
let PATTERN_EXPAND    = 4             // expansion/contraccion
let PATTERN_ALTERNATE = 5             // filas alternas
let PATTERN_CHAOS     = 6             // enjambre caotico

// --- Mecanicas jugador ---
let BOMB_KEY = 98                     // 'b'
let BOMB_KEY_UPPER = 66               // 'B'
let VK_B = 66
let BOMB_FLASH_MS = 400               // flash visual de la bomba

let DASH_DISTANCE = 8                 // celdas del dash
let DASH_COOLDOWN_MS = 2000
let DASH_KEY_TIMEOUT_MS = 250         // ventana para doble-tap

// --- Estrellas de fondo ---
let STAR_COUNT = 20                   // cuantas estrellas en el campo
let STAR_SCROLL_MS = 300              // cada cuanto bajan 1 fila

// --- Explosiones ---
let EXPLOSION_FRAMES = 4
let EXPLOSION_FRAME_MS = 80           // duracion de cada frame

// --- Puntos flotantes ---
let FLOAT_TEXT_DURATION_MS = 800      // cuanto dura el "+80" visible
let FLOAT_TEXT_RISE = 2               // filas que sube

// --- Oleadas especiales ---
let WAVE_BONUS_DURATION_MS = 10000    // 10 segundos para recoger powerups
let WAVE_SWARM_COUNT = 30             // mosquitos en oleada enjambre

// --- Boss fase 2 ---
let BOSS_PHASE2_SPAWN = 8            // mini-pajaros al explotar el boss

// Combo
let COMBO_TIMEOUT_MS = 2500           // si pasa este tiempo sin matar, combo se rompe

// Persistencia
let HIGH_SCORE_PATH = ".phoenix_score.json"

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

// =============================================================================
// PRNG simple
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
// EnemyBird (pajaros pequeno y grande)
// =============================================================================

class EnemyBird {
    fn __init__(self, formation_col, formation_row, spawn_col, spawn_row, kind) {
        // kind: "small", "big", "sniper", "tank", "mosquito",
        //       "kamikaze", "splitter", "shielded", "bomber"
        self.kind = kind
        self.width = SMALL_BIRD_W
        self.hp_max = SMALL_BIRD_HP
        if kind == "big" {
            self.width = BIG_BIRD_W
            self.hp_max = BIG_BIRD_HP
        } elif kind == "sniper" {
            self.width = 2
            self.hp_max = SNIPER_HP
        } elif kind == "tank" {
            self.width = TANK_W
            self.hp_max = TANK_HP
        } elif kind == "mosquito" {
            self.width = MOSQUITO_W
            self.hp_max = MOSQUITO_HP
        } elif kind == "kamikaze" {
            self.width = KAMIKAZE_W
            self.hp_max = KAMIKAZE_HP
        } elif kind == "splitter" {
            self.width = SPLITTER_W
            self.hp_max = SPLITTER_HP
        } elif kind == "shielded" {
            self.width = SHIELDED_W
            self.hp_max = SHIELDED_HP
        } elif kind == "bomber" {
            self.width = BOMBER_W
            self.hp_max = BOMBER_HP
        }
        self.formation_col = formation_col
        self.formation_row = formation_row
        self.base_col = spawn_col
        self.base_row = spawn_row
        self.col = spawn_col
        self.row = spawn_row
        self.alive = true
        self.hp = self.hp_max
        self.egg_cooldown_ms = 3000

        // Para picados: si esta en picado, tiene una "trayectoria"
        self.diving = false
        self.dive_phase_ms = 0           // tiempo desde inicio del picado
        self.dive_origin_col = spawn_col
        self.dive_origin_row = spawn_row
        self.dive_target_col = 0         // columna objetivo (jugador al iniciar)
        self.dive_returning = false      // tras tocar fondo, vuelve a la formacion
    }

    fn update_pos(self, formation_offset, formation_descent) {
        // Si esta en picado, no aplica el offset de la formacion
        if self.diving { return }
        self.col = self.base_col + formation_offset
        self.row = self.base_row + formation_descent
    }

    fn cells(self) {
        let out = []
        let i = 0
        while i < self.width {
            append(out, [self.col + i, self.row])
            i = i + 1
        }
        return out
    }

    fn is_hit_by(self, shot_col, shot_row) {
        if not self.alive { return false }
        if shot_row != self.row { return false }
        if shot_col < self.col { return false }
        if shot_col > self.col + self.width - 1 { return false }
        return true
    }

    fn take_damage(self) {
        self.hp = self.hp - 1
        if self.hp <= 0 { self.alive = false }
    }

    fn point_value(self) {
        if self.kind == "big"      { return PT_BIG_BIRD }
        if self.kind == "sniper"   { return PT_SNIPER }
        if self.kind == "tank"     { return PT_TANK }
        if self.kind == "mosquito" { return PT_MOSQUITO }
        if self.kind == "kamikaze" { return PT_KAMIKAZE }
        if self.kind == "splitter" { return PT_SPLITTER }
        if self.kind == "shielded" { return PT_SHIELDED }
        if self.kind == "bomber"   { return PT_BOMBER }
        return PT_SMALL_BIRD
    }

    // Probabilidad de disparar este tick (de 10000)
    fn shot_prob(self) {
        if self.kind == "sniper"   { return SNIPER_SHOT_PROB }
        if self.kind == "tank"     { return TANK_SHOT_PROB }
        if self.kind == "mosquito" { return MOSQUITO_SHOT_PROB }
        return ENEMY_SHOT_PROB_PER_TICK
    }

    // Inicia un picado hacia el jugador
    fn start_dive(self, target_col) {
        self.diving = true
        self.dive_phase_ms = 0
        self.dive_origin_col = self.col
        self.dive_origin_row = self.row
        self.dive_target_col = target_col
        self.dive_returning = false
    }

    // Avanza el picado. Devuelve true si sigue activo, false si volvio a base.
    fn step_dive(self, elapsed_ms, formation_descent) {
        if not self.diving { return false }
        self.dive_phase_ms = self.dive_phase_ms + elapsed_ms
        // Trayectoria: 1 fila por DIVE_MS_PER_ROW. Lateral: lerp lineal hacia
        // dive_target_col durante la primera mitad, luego mantiene direccion
        // hasta tocar fondo. Despues "vuelve" linealmente a base_col.
        if not self.dive_returning {
            let new_row = self.dive_origin_row + self.dive_phase_ms / DIVE_MS_PER_ROW
            // Trayectoria lateral hacia el target: lerp suave
            let progress = self.dive_phase_ms
            if progress > 1500 { progress = 1500 }
            let dx = self.dive_target_col - self.dive_origin_col
            let new_col = self.dive_origin_col + (dx * progress) / 1500
            self.row = new_row
            self.col = new_col
            // Si toca el fondo, empieza a volver
            if self.row >= FIELD_H - 2 {
                self.dive_returning = true
                self.dive_phase_ms = 0
                self.dive_origin_col = self.col
                self.dive_origin_row = self.row
            }
        } else {
            // Volver a la base. Subimos fila a fila.
            let progress = self.dive_phase_ms
            let total = (self.dive_origin_row - self.base_row) * DIVE_MS_PER_ROW
            if total < 1 { total = 1 }
            if progress >= total {
                self.diving = false
                self.dive_returning = false
                return false
            }
            // Lerp de origen actual a (base_col + offset, base_row+descent)
            let target_row = self.base_row + formation_descent
            let dy_back = target_row - self.dive_origin_row
            let new_row = self.dive_origin_row + (dy_back * progress) / total
            // Lateral: vuelve a base_col (sin offset, pq update_pos no actua mientras dive)
            let new_col = self.dive_origin_col + (self.base_col - self.dive_origin_col) * progress / total
            self.row = new_row
            self.col = new_col
        }
        return true
    }
}


// =============================================================================
// Egg (huevo que sueltan los pajaros grandes)
// =============================================================================

class Egg {
    fn __init__(self, col, row) {
        self.col = col
        self.row = row
        self.alive = true
        self.fall_accum_ms = 0
        self.hatched = false
    }

    fn step(self, elapsed_ms) {
        // Devuelve la fila anterior si avanzo (para borrado en pantalla)
        if not self.alive { return -1 }
        let prev_row = self.row
        self.fall_accum_ms = self.fall_accum_ms + elapsed_ms
        while self.fall_accum_ms >= EGG_FALL_MS {
            self.fall_accum_ms = self.fall_accum_ms - EGG_FALL_MS
            self.row = self.row + 1
        }
        // Si llega al suelo (fila inferior del campo), muere
        if self.row >= FIELD_H - 1 {
            self.alive = false
        }
        if self.row != prev_row { return prev_row }
        return -1
    }

    fn is_hit_by(self, shot_col, shot_row) {
        if not self.alive { return false }
        if shot_row != self.row { return false }
        if shot_col < self.col { return false }
        if shot_col > self.col + EGG_W - 1 { return false }
        return true
    }

    fn cells(self) {
        return [[self.col, self.row], [self.col + 1, self.row]]
    }
}


// =============================================================================
// Boss (oleada 5)
// =============================================================================
// El boss tiene un sprite de 16 chars y dos fases:
//  - Fase 1 (escudo): la barriga (8 chars centrales en su fila inferior)
//    bloquea los disparos. Hay que disparar a la barriga para "abrirla".
//    Cada disparo destruye un trozo (1 char). Cuando todos los trozos estan
//    rotos, fase 2.
//  - Fase 2 (alien expuesto): el alien interior (4 chars centrales en la
//    fila superior del boss) recibe danio. Cuando alien_hp llega a 0, boss
//    muere -> oleada limpia.

class Boss {
    fn __init__(self) {
        self.col = (FIELD_W - BOSS_W) / 2
        self.row = 2                  // ocupara filas 2,3 (2 alto)
        self.alive = true
        self.hp = BOSS_HP             // trozos de barriga
        self.alien_hp = BOSS_ALIEN_HP
        // Estado de las "celdas" de la barriga (8 chars, todas vivas al principio)
        self.belly_pieces = []
        let i = 0
        while i < 8 {
            append(self.belly_pieces, true)
            i = i + 1
        }
        self.shot_cooldown_ms = 1500
        self.move_accum_ms = 0
        self.move_dir = 1             // 1 = derecha, -1 = izquierda
    }

    // Filas: row=cuerpo superior (alien aqui), row+1=barriga
    fn body_row(self)  { return self.row }
    fn belly_row(self) { return self.row + 1 }

    // ¿La barriga sigue cerrada (algun trozo vivo)?
    fn belly_closed(self) {
        for p in self.belly_pieces {
            if p { return true }
        }
        return false
    }

    // Posicion de cada trozo de barriga: centradas en el sprite, 8 chars de los 16
    fn belly_piece_col(self, idx) {
        return self.col + 4 + idx
    }

    // Posicion del alien (4 chars centrales en body_row)
    fn alien_cells(self) {
        let out = []
        let i = 0
        while i < 4 {
            append(out, [self.col + 6 + i, self.row])
            i = i + 1
        }
        return out
    }

    fn step(self, elapsed_ms) {
        // Movimiento lateral lento y rebote
        self.move_accum_ms = self.move_accum_ms + elapsed_ms
        let moved = false
        while self.move_accum_ms >= BOSS_MOVE_MS {
            self.move_accum_ms = self.move_accum_ms - BOSS_MOVE_MS
            self.col = self.col + self.move_dir
            if self.col <= 1 {
                self.col = 1
                self.move_dir = 1
            }
            if self.col + BOSS_W >= FIELD_W - 1 {
                self.col = FIELD_W - 1 - BOSS_W
                self.move_dir = -1
            }
            moved = true
        }
        return moved
    }

    fn try_shoot(self, rng) {
        let r = rng.range(0, 10000)
        if r >= BOSS_SHOT_PROB { return [] }
        // Dispara desde 3 puntos en la barriga abierta o desde los costados
        let shots = []
        // Disparo central (barriga)
        append(shots, [self.col + 8, self.belly_row() + 1])
        // Disparos diagonales si quedan trozos cercanos
        if self.belly_pieces[0] { append(shots, [self.col + 4, self.belly_row() + 1]) }
        if self.belly_pieces[7] { append(shots, [self.col + 11, self.belly_row() + 1]) }
        return shots
    }

    fn apply_player_shot(self, shot_col, shot_row) {
        // Devuelve "miss" / "belly" / "alien" / "killed"
        if not self.alive { return "miss" }

        // Comprobar barriga (8 trozos en self.belly_row())
        if shot_row == self.belly_row() {
            let i = 0
            while i < 8 {
                if self.belly_pieces[i] {
                    let bc = self.belly_piece_col(i)
                    if shot_col == bc {
                        self.belly_pieces[i] = false
                        self.hp = self.hp - 1
                        return "belly"
                    }
                }
                i = i + 1
            }
            return "miss"
        }

        // Comprobar cuerpo superior (filas del alien). Solo recibe danio
        // si la barriga esta abierta.
        if shot_row == self.body_row() {
            // ¿Esta en la zona del alien? (4 chars centrales)
            if shot_col >= self.col + 6 and shot_col <= self.col + 9 {
                if not self.belly_closed() {
                    self.alien_hp = self.alien_hp - 1
                    if self.alien_hp <= 0 {
                        self.alive = false
                        return "killed"
                    }
                    return "alien"
                }
            }
        }
        return "miss"
    }
}


// =============================================================================
// PowerUp (cae cuando muere algun pajaro)
// =============================================================================

class PowerUp {
    fn __init__(self, col, row, kind) {
        self.col = col
        self.row = row
        self.kind = kind              // POWERUP_DOUBLE, POWERUP_SHIELD, POWERUP_LIFE
        self.alive = true
        self.fall_accum_ms = 0
    }

    fn step(self, elapsed_ms) {
        if not self.alive { return -1 }
        let prev_row = self.row
        self.fall_accum_ms = self.fall_accum_ms + elapsed_ms
        while self.fall_accum_ms >= POWERUP_FALL_MS {
            self.fall_accum_ms = self.fall_accum_ms - POWERUP_FALL_MS
            self.row = self.row + 1
        }
        if self.row != prev_row { return prev_row }
        return -1
    }

    fn cells(self) {
        return [[self.col, self.row], [self.col + 1, self.row]]
    }
}


// =============================================================================
// EnemyShotPool
// =============================================================================
// Disparos enemigos: caen hacia abajo a velocidad ENEMY_SHOT_SPEED_MS

class EnemyShotPool {
    fn __init__(self) {
        self.shots = []
    }

    fn add(self, col, row) {
        append(self.shots, { "col": col, "row": row, "accum_ms": 0 })
    }

    fn step(self, elapsed_ms) {
        let cleared = []
        let alive = []
        for s in self.shots {
            let prev_col = s["col"]
            let prev_row = s["row"]
            s["accum_ms"] = s["accum_ms"] + elapsed_ms
            while s["accum_ms"] >= ENEMY_SHOT_SPEED_MS {
                s["accum_ms"] = s["accum_ms"] - ENEMY_SHOT_SPEED_MS
                s["row"] = s["row"] + 1
            }
            if s["row"] >= FIELD_H {
                append(cleared, [prev_col, prev_row])
                continue
            }
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
// Formation (oleada de pajaros)
// =============================================================================

// =============================================================================
// Missile (proyectil teleguiado)
// =============================================================================

class Missile {
    fn __init__(self, col, row) {
        self.col = col
        self.row = row
        self.alive = true
        self.move_accum_ms = 0
        // Lateral lerp hacia el target (recalculado cada paso)
        self.lateral_accum = 0
    }

    // Avanza el misil. target_col es la columna del enemigo mas cercano (o -1).
    // Devuelve la fila/col viejas si avanzo (para limpiar render).
    fn step(self, elapsed_ms, target_col) {
        if not self.alive { return null }
        let prev_col = self.col
        let prev_row = self.row
        self.move_accum_ms = self.move_accum_ms + elapsed_ms
        while self.move_accum_ms >= MISSILE_SPEED_MS {
            self.move_accum_ms = self.move_accum_ms - MISSILE_SPEED_MS
            // Subir 1 fila siempre
            self.row = self.row - 1
            // Si tiene target, ajustar lateralmente 1 cada 2 filas
            if target_col >= 0 {
                if self.col < target_col {
                    self.col = self.col + 1
                } elif self.col > target_col {
                    self.col = self.col - 1
                }
            }
        }
        if self.row < 0 {
            self.alive = false
            return [prev_col, prev_row]
        }
        if self.col != prev_col or self.row != prev_row {
            return [prev_col, prev_row]
        }
        return null
    }
}


// =============================================================================
// Laser (rayo vertical instantaneo desde la nave hacia arriba)
// =============================================================================
// El laser ocupa toda una columna (desde la fila 0 hasta la fila justo sobre
// el jugador) y permanece visible LASER_DURATION_MS. Atraviesa enemigos.

class Laser {
    fn __init__(self, col, top_row, bottom_row) {
        self.col = col
        self.top_row = top_row           // 0 normalmente
        self.bottom_row = bottom_row     // fila justo encima del jugador
        self.duration_ms = LASER_DURATION_MS
        self.alive = true
    }

    fn step(self, elapsed_ms) {
        self.duration_ms = self.duration_ms - elapsed_ms
        if self.duration_ms <= 0 { self.alive = false }
    }

    // ¿Toca el laser a un enemigo en (ec, er)?
    fn hits(self, ec, er) {
        if not self.alive { return false }
        if ec != self.col { return false }
        if er < self.top_row { return false }
        if er > self.bottom_row { return false }
        return true
    }
}


class Formation {
    fn __init__(self, wave_num, rng, loop_num) {
        self.wave_num = wave_num
        self.rng = rng
        self.loop_num = loop_num
        self.birds = []
        self.eggs = []
        self.offset = 0
        self.descent = 0                  // filas que ha bajado la formacion
        self.descent_accum_ms = 0
        self.elapsed_ms = 0
        self.pattern = PATTERN_OSCILLATE  // por defecto
        self.dive_timer_ms = DIVE_INTERVAL_MS
        self._build()
    }

    fn _build(self) {
        // Decidir patron y composicion segun oleada
        if self.wave_num == 1 {
            self.pattern = PATTERN_OSCILLATE
            self._build_grid(5, 3, "small", 4, 2, 3)
        } elif self.wave_num == 2 {
            self.pattern = PATTERN_WAVE
            self._build_grid(6, 3, "small", 4, 2, 3)
            self._convert_to(0, "sniper")
            self._convert_to(5, "sniper")
            self._convert_to(12, "sniper")
            self._convert_to(17, "sniper")
            // Añadir 2 kamikazes en la fila superior
            self._add_row_of("kamikaze", 2, 10, 1)
        } elif self.wave_num == 3 {
            self.pattern = PATTERN_ALTERNATE
            self._build_grid(4, 2, "big", 6, 2, 3)
            // Splitters arriba
            self._add_row_of("splitter", 3, 8, 1)
        } elif self.wave_num == 4 {
            self.pattern = PATTERN_CHAOS
            self._build_grid(4, 2, "big", 6, 2, 3)
            self._add_row_of("tank", 3, 7, 7)
            self._add_row_of("mosquito", 6, 6, 1)
            self._add_row_of("kamikaze", 3, 8, 9)
            // Shielded flanking
            self._add_row_of("shielded", 2, 14, 5)
        }
        // Wave 5 = boss (no entra aqui)

        // En loop 2+, se anaden bombers y mas enemigos
        if self.loop_num > 1 {
            if self.wave_num == 1 {
                self._add_row_of("splitter", 3, 8, 9)
            } elif self.wave_num == 2 {
                self._add_row_of("shielded", 2, 14, 9)
                self._add_row_of("bomber", 1, 0, 1)
            } elif self.wave_num == 3 {
                self._add_row_of("bomber", 2, 12, 1)
                self._add_row_of("kamikaze", 4, 6, 9)
            } elif self.wave_num == 4 {
                self._add_row_of("bomber", 2, 10, 1)
                self._add_row_of("splitter", 4, 6, 9)
                self._add_row_of("shielded", 3, 8, 11)
            }
        }
    }

    // Crea una matriz de cols x rows del kind dado, con spacing y vertical_step
    fn _build_grid(self, cols, rows, kind, spacing, vertical_step, start_row) {
        let bird_w = SMALL_BIRD_W
        if kind == "big"       { bird_w = BIG_BIRD_W }
        if kind == "tank"      { bird_w = TANK_W }
        if kind == "mosquito"  { bird_w = MOSQUITO_W }
        if kind == "sniper"    { bird_w = SNIPER_W }
        if kind == "kamikaze"  { bird_w = KAMIKAZE_W }
        if kind == "splitter"  { bird_w = SPLITTER_W }
        if kind == "shielded"  { bird_w = SHIELDED_W }
        if kind == "bomber"    { bird_w = BOMBER_W }
        let formation_w = (cols - 1) * spacing + bird_w
        let start_col = (FIELD_W - formation_w) / 2
        if start_col < 1 { start_col = 1 }
        let r = 0
        while r < rows {
            let c = 0
            while c < cols {
                let x = start_col + c * spacing
                let y = start_row + r * vertical_step
                let bird = EnemyBird(c, r, x, y, kind)
                append(self.birds, bird)
                c = c + 1
            }
            r = r + 1
        }
    }

    // Convierte el pajaro en posicion idx a otro kind (mantiene posicion base)
    fn _convert_to(self, idx, new_kind) {
        if idx < 0 or idx >= len(self.birds) { return }
        let old = self.birds[idx]
        let nb = EnemyBird(old.formation_col, old.formation_row, old.base_col, old.base_row, new_kind)
        self.birds[idx] = nb
    }

    // Anade una fila adicional de pajaros del tipo dado a una fila concreta
    fn _add_row_of(self, kind, cols, spacing, row_offset_from_top) {
        let bird_w = SMALL_BIRD_W
        if kind == "big"      { bird_w = BIG_BIRD_W }
        if kind == "tank"     { bird_w = TANK_W }
        if kind == "mosquito" { bird_w = MOSQUITO_W }
        let formation_w = (cols - 1) * spacing + bird_w
        let start_col = (FIELD_W - formation_w) / 2
        if start_col < 1 { start_col = 1 }
        let y = 1 + row_offset_from_top
        let c = 0
        while c < cols {
            let x = start_col + c * spacing
            let bird = EnemyBird(c, 99, x, y, kind)
            append(self.birds, bird)
            c = c + 1
        }
    }

    // Calcula el offset segun el patron actual y la fila del pajaro (para WAVE)
    fn _offset_for_row(self, formation_row) {
        let period = FORMATION_PERIOD_MS
        let A = FORMATION_AMPLITUDE
        let row_phase = 0
        if self.pattern == PATTERN_WAVE { row_phase = formation_row * (period / 6) }
        // ALTERNATE: filas impares van en sentido contrario
        let sign = 1
        if self.pattern == PATTERN_ALTERNATE {
            if formation_row % 2 == 1 { sign = -1 }
        }
        let phase_int = (self.elapsed_ms + row_phase) % period
        let q = phase_int * 4
        let new_offset = 0
        if q < period {
            new_offset = q * A / period
        } elif q < 2 * period {
            new_offset = A - (q - period) * A / period
        } elif q < 3 * period {
            new_offset = 0 - (q - 2 * period) * A / period
        } else {
            new_offset = 0 - A + (q - 3 * period) * A / period
        }
        return new_offset * sign
    }

    // Offset vertical para patron CIRCLE (desfasado 90 grados del horizontal)
    fn _vert_offset_for_row(self, formation_row) {
        let period = FORMATION_PERIOD_MS
        let A = 3                       // amplitud vertical menor
        // Desfase de 1/4 de periodo para hacer circularidad
        let phase_int = (self.elapsed_ms + period / 4) % period
        let q = phase_int * 4
        let v = 0
        if q < period {
            v = q * A / period
        } elif q < 2 * period {
            v = A - (q - period) * A / period
        } elif q < 3 * period {
            v = 0 - (q - 2 * period) * A / period
        } else {
            v = 0 - A + (q - 3 * period) * A / period
        }
        return v
    }

    // Offset de expansion/contraccion: cada pajaro se aleja del centro
    fn _expand_offset(self, formation_col, formation_row, cols_in_row) {
        let period = FORMATION_PERIOD_MS / 2   // ciclo mas rapido
        let phase_int = self.elapsed_ms % period
        let A = 4
        let q = phase_int * 4
        let pulse = 0
        if q < period {
            pulse = q * A / period
        } elif q < 2 * period {
            pulse = A - (q - period) * A / period
        } elif q < 3 * period {
            pulse = 0 - (q - 2 * period) * A / period
        } else {
            pulse = 0 - A + (q - 3 * period) * A / period
        }
        // La direccion de expansion depende de si la columna esta a izq o der del centro
        let center = cols_in_row / 2
        if formation_col < center { return 0 - pulse }
        if formation_col > center { return pulse }
        return 0
    }

    fn update(self, elapsed_ms) {
        self.elapsed_ms = self.elapsed_ms + elapsed_ms
        // Calcular offset general
        self.offset = self._offset_for_row(0)

        // Descenso si patron DESCEND
        if self.pattern == PATTERN_DESCEND {
            self.descent_accum_ms = self.descent_accum_ms + elapsed_ms
            while self.descent_accum_ms >= DESCENT_MS {
                self.descent_accum_ms = self.descent_accum_ms - DESCENT_MS
                if self.descent < MAX_DESCENT_ROWS {
                    self.descent = self.descent + 1
                }
            }
        }

        // Aplicar a cada pajaro
        for b in self.birds {
            if not b.alive { continue }
            if b.diving {
                let still = b.step_dive(elapsed_ms, self.descent)
                continue
            }
            let off = self.offset
            let vert_off = 0
            if self.pattern == PATTERN_WAVE {
                off = self._offset_for_row(b.formation_row)
            } elif self.pattern == PATTERN_ALTERNATE {
                off = self._offset_for_row(b.formation_row)
            } elif self.pattern == PATTERN_CIRCLE {
                off = self._offset_for_row(b.formation_row)
                vert_off = self._vert_offset_for_row(b.formation_row)
            } elif self.pattern == PATTERN_EXPAND {
                off = self._expand_offset(b.formation_col, b.formation_row, 6)
            } elif self.pattern == PATTERN_CHAOS {
                // Caos: cada pajaro tiene un offset que depende de su indice
                // de forma "aleatoria" usando su formation_col/row como seed
                let bird_phase = (b.formation_col * 700 + b.formation_row * 1300) % FORMATION_PERIOD_MS
                let phase_int = (self.elapsed_ms + bird_phase) % FORMATION_PERIOD_MS
                let A = FORMATION_AMPLITUDE + 2
                let q = phase_int * 4
                if q < FORMATION_PERIOD_MS {
                    off = q * A / FORMATION_PERIOD_MS
                } elif q < 2 * FORMATION_PERIOD_MS {
                    off = A - (q - FORMATION_PERIOD_MS) * A / FORMATION_PERIOD_MS
                } elif q < 3 * FORMATION_PERIOD_MS {
                    off = 0 - (q - 2 * FORMATION_PERIOD_MS) * A / FORMATION_PERIOD_MS
                } else {
                    off = 0 - A + (q - 3 * FORMATION_PERIOD_MS) * A / FORMATION_PERIOD_MS
                }
                // Tambien un poquito de vertical
                let vphase = (self.elapsed_ms + bird_phase + FORMATION_PERIOD_MS / 4) % FORMATION_PERIOD_MS
                let vq = vphase * 4
                let vA = 2
                if vq < FORMATION_PERIOD_MS {
                    vert_off = vq * vA / FORMATION_PERIOD_MS
                } elif vq < 2 * FORMATION_PERIOD_MS {
                    vert_off = vA - (vq - FORMATION_PERIOD_MS) * vA / FORMATION_PERIOD_MS
                } else {
                    vert_off = 0
                }
            }
            // Mosquitos se mueven con doble amplitud
            if b.kind == "mosquito" {
                off = off * 2
            }
            b.col = b.base_col + off
            b.row = b.base_row + self.descent + vert_off
        }

        // Avance de huevos
        for e in self.eggs {
            if e.alive { e.step(elapsed_ms) }
        }

        // Picados: cada DIVE_INTERVAL_MS, un pajaro al azar se sale
        self.dive_timer_ms = self.dive_timer_ms - elapsed_ms
        if self.dive_timer_ms <= 0 {
            self.dive_timer_ms = DIVE_INTERVAL_MS
        }
    }

    // Llamado desde Game cuando toca lanzar un picado. Devuelve el bird o null.
    fn try_start_dive(self, target_col) {
        // Lista de candidatos (vivos, no diving, no en fila inferior porque
        // sino vendrian directo del centro y rompe la idea)
        let candidates = []
        for b in self.birds {
            if b.alive and not b.diving { append(candidates, b) }
        }
        if len(candidates) == 0 { return null }
        let idx = self.rng.range(0, len(candidates))
        let bird = candidates[idx]
        bird.start_dive(target_col)
        return bird
    }

    fn alive_count(self) {
        let n = 0
        for b in self.birds {
            if b.alive { n = n + 1 }
        }
        return n
    }

    fn is_clear(self) {
        if self.alive_count() > 0 { return false }
        for e in self.eggs {
            if e.alive { return false }
        }
        return true
    }

    fn try_enemy_shoot(self, player_col) {
        let shots = []
        for b in self.birds {
            if not b.alive { continue }
            let r = self.rng.range(0, 10000)
            if r < b.shot_prob() {
                let shot_col = b.col + b.width / 2
                let shot_row = b.row + 1
                // Snipers apuntan al jugador: el "disparo" se inicializa con
                // un pequeno desplazamiento lateral hacia el jugador.
                // Como nuestros disparos enemigos solo caen recto, hacemos el
                // truco de spawnar el disparo directamente en la columna del
                // jugador con una row inicial apropiada (un poco mas baja).
                if b.kind == "sniper" {
                    // Apuntamos al jugador: spawn col = direccion hacia el jugador
                    let dx = player_col - shot_col
                    if dx > 2  { shot_col = shot_col + 1 }
                    if dx < -2 { shot_col = shot_col - 1 }
                }
                append(shots, [shot_col, shot_row])
            }
        }
        return shots
    }

    fn try_drop_eggs(self, elapsed_ms) {
        let new_eggs = []
        for b in self.birds {
            if not b.alive { continue }
            if b.kind != "big" { continue }
            b.egg_cooldown_ms = b.egg_cooldown_ms - elapsed_ms
            if b.egg_cooldown_ms <= 0 {
                let egg = Egg(b.col + 1, b.row + 1)
                append(new_eggs, egg)
                append(self.eggs, egg)
                b.egg_cooldown_ms = 4000 + self.rng.range(0, 4000)
            }
        }
        return new_eggs
    }

    fn apply_player_shot(self, shot_col, shot_row) {
        for b in self.birds {
            if b.is_hit_by(shot_col, shot_row) {
                b.take_damage()
                return [true, b]
            }
        }
        for e in self.eggs {
            if e.is_hit_by(shot_col, shot_row) {
                e.alive = false
                return [true, "egg"]
            }
        }
        return [false, null]
    }
}


// =============================================================================
// StarField (estrellas de fondo scrolleando)
// =============================================================================

class StarField {
    fn __init__(self, rng) {
        self.rng = rng
        self.stars = []
        self.scroll_accum_ms = 0
        let i = 0
        while i < STAR_COUNT {
            append(self.stars, [rng.range(0, FIELD_W), rng.range(0, FIELD_H)])
            i = i + 1
        }
    }
    fn step(self, elapsed_ms) {
        self.scroll_accum_ms = self.scroll_accum_ms + elapsed_ms
        while self.scroll_accum_ms >= STAR_SCROLL_MS {
            self.scroll_accum_ms = self.scroll_accum_ms - STAR_SCROLL_MS
            let new_stars = []
            for s in self.stars {
                let ny = s[1] + 1
                if ny >= FIELD_H {
                    ny = 0
                    s[0] = self.rng.range(0, FIELD_W)
                }
                append(new_stars, [s[0], ny])
            }
            self.stars = new_stars
        }
    }
}


// =============================================================================
// Explosion (efecto visual al morir un enemigo)
// =============================================================================

class Explosion {
    fn __init__(self, col, row, width) {
        self.col = col
        self.row = row
        self.width = width
        self.frame = 0
        self.accum_ms = 0
        self.alive = true
    }
    fn step(self, elapsed_ms) {
        if not self.alive { return }
        self.accum_ms = self.accum_ms + elapsed_ms
        while self.accum_ms >= EXPLOSION_FRAME_MS {
            self.accum_ms = self.accum_ms - EXPLOSION_FRAME_MS
            self.frame = self.frame + 1
        }
        if self.frame >= EXPLOSION_FRAMES { self.alive = false }
    }
    fn glyph(self) {
        if self.frame == 0 { return "*" }
        if self.frame == 1 { return "+" }
        if self.frame == 2 { return "x" }
        return "."
    }
}


// =============================================================================
// FloatText ("+80" que sube brevemente)
// =============================================================================

class FloatText {
    fn __init__(self, col, row, text) {
        self.col = col
        self.row = row
        self.text = text
        self.alive = true
        self.elapsed_ms = 0
        self.risen = 0
    }
    fn step(self, elapsed_ms) {
        if not self.alive { return }
        self.elapsed_ms = self.elapsed_ms + elapsed_ms
        // Subir 1 fila cada FLOAT_TEXT_DURATION_MS/FLOAT_TEXT_RISE ms
        let step_ms = FLOAT_TEXT_DURATION_MS / FLOAT_TEXT_RISE
        if step_ms < 1 { step_ms = 1 }
        let target_rise = self.elapsed_ms / step_ms
        if target_rise > FLOAT_TEXT_RISE { target_rise = FLOAT_TEXT_RISE }
        self.risen = target_rise
        if self.elapsed_ms >= FLOAT_TEXT_DURATION_MS { self.alive = false }
    }
    fn display_row(self) { return self.row - self.risen }
}


// =============================================================================
// EnemyBomb (bomba que suelta el Bombardero, explota al tocar suelo)
// =============================================================================

class EnemyBomb {
    fn __init__(self, col, row) {
        self.col = col
        self.row = row
        self.alive = true
        self.fall_accum_ms = 0
        self.exploded = false
    }
    fn step(self, elapsed_ms) {
        if not self.alive { return -1 }
        let prev_row = self.row
        self.fall_accum_ms = self.fall_accum_ms + elapsed_ms
        while self.fall_accum_ms >= BOMB_FALL_MS {
            self.fall_accum_ms = self.fall_accum_ms - BOMB_FALL_MS
            self.row = self.row + 1
        }
        if self.row >= FIELD_H - 1 {
            self.alive = false
            self.exploded = true
        }
        if self.row != prev_row { return prev_row }
        return -1
    }
    // Celdas afectadas por la explosion (cruz 3x3)
    fn blast_cells(self) {
        let cells = []
        let r = self.row - BOMB_BLAST_RADIUS
        while r <= self.row + BOMB_BLAST_RADIUS {
            let c = self.col - BOMB_BLAST_RADIUS
            while c <= self.col + BOMB_BLAST_RADIUS {
                if c >= 0 and c < FIELD_W and r >= 0 and r < FIELD_H {
                    append(cells, [c, r])
                }
                c = c + 1
            }
            r = r + 1
        }
        return cells
    }
}


// =============================================================================
// SplitMini (mini-pajaro que sale del Splitter al morir)
// =============================================================================

class SplitMini {
    fn __init__(self, col, row, dx) {
        self.col = col
        self.row = row
        self.dx = dx                 // -1 o +1 (direccion diagonal)
        self.alive = true
        self.hp = SPLIT_MINI_HP
        self.move_accum_ms = 0
    }
    fn step(self, elapsed_ms) {
        if not self.alive { return }
        self.move_accum_ms = self.move_accum_ms + elapsed_ms
        while self.move_accum_ms >= 100 {
            self.move_accum_ms = self.move_accum_ms - 100
            self.col = self.col + self.dx
            self.row = self.row + 1
        }
        if self.row >= FIELD_H or self.col < 0 or self.col >= FIELD_W {
            self.alive = false
        }
    }
    fn is_hit_by(self, shot_col, shot_row) {
        if not self.alive { return false }
        return shot_col == self.col and shot_row == self.row
    }
}


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
            self.frame_h  = from_char(226) + from_char(149) + from_char(144)  // ═
            self.frame_v  = from_char(226) + from_char(149) + from_char(145)  // ║
            self.frame_tl = from_char(226) + from_char(149) + from_char(148)  // ╔
            self.frame_tr = from_char(226) + from_char(149) + from_char(151)  // ╗
            self.frame_bl = from_char(226) + from_char(149) + from_char(154)  // ╚
            self.frame_br = from_char(226) + from_char(149) + from_char(157)  // ╝
            self.shot_glyph = from_char(226) + from_char(148) + from_char(130)  // │
            self.bird_small    = "vv"
            self.bird_big      = "<vv>"
            self.bird_sniper   = "VV"
            self.bird_tank     = "[##]"
            self.bird_mosquito = "x"
            self.egg_glyph  = "OO"
            self.enemy_shot_glyph = "*"
            self.boss_body  = "MMMMMM<<>>MMMMMM"
            self.boss_belly_piece = from_char(226) + from_char(150) + from_char(136)  // █
            self.boss_alien_glyph = "@@@@"
            self.shield_glyph = from_char(226) + from_char(151) + from_char(139)
            self.powerup_glyph = "[]"
            // Misil: flecha hacia arriba
            self.missile_glyph = from_char(226) + from_char(150) + from_char(178)  // ▲
            // Laser: bloque vertical
            self.laser_glyph = from_char(226) + from_char(149) + from_char(145)  // ║
        } else {
            self.frame_h  = "="
            self.frame_v  = "|"
            self.frame_tl = "+"
            self.frame_tr = "+"
            self.frame_bl = "+"
            self.frame_br = "+"
            self.shot_glyph = "|"
            self.bird_small    = "vv"
            self.bird_big      = "<vv>"
            self.bird_sniper   = "VV"
            self.bird_tank     = "[##]"
            self.bird_mosquito = "x"
            self.egg_glyph  = "OO"
            self.enemy_shot_glyph = "*"
            self.boss_body  = "MMMMMM<<>>MMMMMM"
            self.boss_belly_piece = "#"
            self.boss_alien_glyph = "@@@@"
            self.shield_glyph = "o"
            self.powerup_glyph = "[]"
            self.missile_glyph = "^"
            self.laser_glyph = "|"
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
        self.col_bird_w1 = ansi_rgb(220, 100, 220)    // magenta
        self.col_bird_w2 = ansi_rgb( 80, 220, 220)    // cyan
        self.col_bird_w3 = ansi_rgb(255, 150,  60)    // naranja
        self.col_bird_w4 = ansi_rgb(180, 100, 255)    // violeta
        // Colores de tipos especiales
        self.col_sniper   = ansi_rgb(180,  80, 220)   // morado profundo
        self.col_tank     = ansi_rgb(180,  60,  60)   // rojo oscuro
        self.col_mosquito = ansi_rgb(240, 220,  80)   // dorado
        self.col_egg     = ansi_rgb(240, 240, 200)
        self.col_enemy_shot = ansi_rgb(255,  90,  90)
        self.col_boss      = ansi_rgb(220, 100, 100)
        self.col_boss_belly = ansi_rgb(160, 160, 200)
        self.col_alien     = ansi_rgb(180, 255, 100)
        self.col_shield    = ansi_rgb(120, 220, 255)
        self.col_powerup   = ansi_rgb(255, 220,  80)
        self.col_combo     = ansi_rgb(255, 180,  80)
        self.col_missile   = ansi_rgb(255, 130,  60)
        self.col_laser     = ansi_rgb(120, 255, 200)
        self.R = ANSI["RESET"]
    }

    fn bird_color_for_wave(self, wave) {
        if wave == 1 { return self.col_bird_w1 }
        if wave == 2 { return self.col_bird_w2 }
        if wave == 3 { return self.col_bird_w3 }
        if wave == 4 { return self.col_bird_w4 }
        return self.col_bird_w1
    }

    // Devuelve el color para un pajaro segun su tipo (anula el color de oleada
    // para tipos especiales)
    fn bird_color(self, bird, wave) {
        if bird.kind == "sniper"   { return self.col_sniper }
        if bird.kind == "tank"     { return self.col_tank }
        if bird.kind == "mosquito" { return self.col_mosquito }
        if bird.kind == "kamikaze" { return ansi_rgb(255, 80, 40) }
        if bird.kind == "splitter" { return ansi_rgb(200, 220, 80) }
        if bird.kind == "shielded" { return ansi_rgb(100, 180, 255) }
        if bird.kind == "bomber"   { return ansi_rgb(200, 80, 80) }
        return self.bird_color_for_wave(wave)
    }

    // Devuelve el sprite de texto de un pajaro segun su tipo
    fn bird_sprite(self, bird) {
        if bird.kind == "big"      { return self.bird_big }
        if bird.kind == "sniper"   { return self.bird_sniper }
        if bird.kind == "tank"     { return self.bird_tank }
        if bird.kind == "mosquito" { return self.bird_mosquito }
        if bird.kind == "kamikaze" { return "!!" }
        if bird.kind == "splitter" { return "()" }
        if bird.kind == "shielded" { return "[]" }
        if bird.kind == "bomber"   { return "=BB=BB" }
        return self.bird_small
    }

    fn powerup_color(self, kind) {
        if kind == POWERUP_DOUBLE  { return ansi_rgb(255, 200,  80) }
        if kind == POWERUP_SHIELD  { return ansi_rgb(120, 220, 255) }
        if kind == POWERUP_LIFE    { return ansi_rgb(120, 255, 120) }
        if kind == POWERUP_SPREAD  { return ansi_rgb(255, 130, 200) }
        if kind == POWERUP_LASER   { return ansi_rgb(120, 255, 200) }
        if kind == POWERUP_MISSILE { return ansi_rgb(255, 130,  60) }
        return self.col_powerup
    }

    fn powerup_label(self, kind) {
        if kind == POWERUP_DOUBLE  { return "2x" }
        if kind == POWERUP_SHIELD  { return "SH" }
        if kind == POWERUP_LIFE    { return "1U" }
        if kind == POWERUP_SPREAD  { return "SP" }
        if kind == POWERUP_LASER   { return "LS" }
        if kind == POWERUP_MISSILE { return "MS" }
        return "??"
    }

    fn weapon_label(self, weapon) {
        if weapon == WEAPON_SINGLE  { return "SINGLE" }
        if weapon == WEAPON_DOUBLE  { return "DOUBLE" }
        if weapon == WEAPON_SPREAD  { return "SPREAD" }
        if weapon == WEAPON_LASER   { return "LASER " }
        if weapon == WEAPON_MISSILE { return "MISSILE" }
        return "??"
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

    fn draw_bird(self, ox, oy, bird, wave) {
        // Color y sprite segun el tipo del pajaro
        let color = self.bird_color(bird, wave)
        let sprite = self.bird_sprite(bird)
        self._w(self.cell_pos(ox, oy, bird.col, bird.row))
        self._w(color + ANSI["BOLD"] + sprite + self.R)
    }

    fn draw_egg(self, ox, oy, egg) {
        self._w(self.cell_pos(ox, oy, egg.col, egg.row))
        self._w(self.col_egg + ANSI["BOLD"] + self.egg_glyph + self.R)
    }

    fn draw_missile(self, ox, oy, m) {
        self._w(self.cell_pos(ox, oy, m.col, m.row))
        self._w(self.col_missile + ANSI["BOLD"] + self.missile_glyph + self.R)
    }

    fn draw_laser(self, ox, oy, laser) {
        // Dibujar columna entera desde top_row hasta bottom_row
        let r = laser.top_row
        while r <= laser.bottom_row {
            self._w(self.cell_pos(ox, oy, laser.col, r))
            self._w(self.col_laser + ANSI["BOLD"] + self.laser_glyph + self.R)
            r = r + 1
        }
    }

    fn draw_boss(self, ox, oy, boss) {
        // Cuerpo superior (16 chars)
        self._w(self.cell_pos(ox, oy, boss.col, boss.body_row()))
        self._w(self.col_boss + ANSI["BOLD"] + self.boss_body + self.R)
        // Si la barriga esta abierta, mostramos el alien
        if not boss.belly_closed() {
            // Tapamos los 4 chars centrales del cuerpo con el alien
            self._w(self.cell_pos(ox, oy, boss.col + 6, boss.body_row()))
            self._w(self.col_alien + ANSI["BOLD"] + self.boss_alien_glyph + self.R)
        }
        // Barriga: 8 chars en belly_row
        let i = 0
        while i < 8 {
            self._w(self.cell_pos(ox, oy, boss.belly_piece_col(i), boss.belly_row()))
            if boss.belly_pieces[i] {
                self._w(self.col_boss_belly + ANSI["BOLD"] + self.boss_belly_piece + self.R)
            } else {
                self._w(" ")
            }
            i = i + 1
        }
    }

    fn draw_powerup(self, ox, oy, pu) {
        let color = self.powerup_color(pu.kind)
        let label = self.powerup_label(pu.kind)
        self._w(self.cell_pos(ox, oy, pu.col, pu.row))
        self._w(color + ANSI["BOLD"] + label + self.R)
    }

    fn draw_shield(self, ox, oy, player_col, player_row) {
        // Dibujo del escudo: chars laterales y arriba/abajo de la nave
        let pulse_color = ansi_rgb(120, 220, 255)
        // Lados
        self._w(self.cell_pos(ox, oy, player_col - 1, player_row))
        self._w(pulse_color + ANSI["BOLD"] + "(" + self.R)
        self._w(self.cell_pos(ox, oy, player_col + 4, player_row))
        self._w(pulse_color + ANSI["BOLD"] + ")" + self.R)
    }

    fn clear_shield(self, ox, oy, player_col, player_row) {
        self._w(self.cell_pos(ox, oy, player_col - 1, player_row))
        self._w(" ")
        self._w(self.cell_pos(ox, oy, player_col + 4, player_row))
        self._w(" ")
    }

    fn clear_bird_area(self, ox, oy, col, row) {
        self._w(self.cell_pos(ox, oy, col, row))
        self._w("  ")
    }

    fn draw_enemy_shot(self, ox, oy, col, row) {
        self._w(self.cell_pos(ox, oy, col, row))
        self._w(self.col_enemy_shot + ANSI["BOLD"] + self.enemy_shot_glyph + self.R)
    }

    fn draw_star(self, ox, oy, col, row) {
        self._w(self.cell_pos(ox, oy, col, row))
        self._w(ansi_rgb(80, 80, 120) + "." + self.R)
    }

    fn draw_explosion(self, ox, oy, ex) {
        let g = ex.glyph()
        let colors = [ansi_rgb(255, 200, 60), ansi_rgb(255, 130, 40), ansi_rgb(200, 80, 30), ansi_rgb(120, 60, 30)]
        let ci = ex.frame
        if ci >= len(colors) { ci = len(colors) - 1 }
        let color = colors[ci]
        let i = 0
        while i < ex.width {
            self._w(self.cell_pos(ox, oy, ex.col + i, ex.row))
            self._w(color + ANSI["BOLD"] + g + self.R)
            i = i + 1
        }
    }

    fn draw_float_text(self, ox, oy, ft) {
        let dr = ft.display_row()
        if dr < 0 or dr >= FIELD_H { return }
        self._w(self.cell_pos(ox, oy, ft.col, dr))
        self._w(ansi_rgb(255, 255, 180) + ft.text + self.R)
    }

    fn draw_split_mini(self, ox, oy, sm) {
        self._w(self.cell_pos(ox, oy, sm.col, sm.row))
        self._w(ansi_rgb(200, 220, 80) + ANSI["BOLD"] + "o" + self.R)
    }

    fn draw_enemy_bomb(self, ox, oy, eb) {
        self._w(self.cell_pos(ox, oy, eb.col, eb.row))
        self._w(ansi_rgb(255, 100, 50) + ANSI["BOLD"] + "@" + self.R)
    }

    fn draw_bomb_flash(self, ox, oy) {
        // Flash de la bomba inteligente: linea horizontal en el centro
        let mid = FIELD_H / 2
        self._w(self.cell_pos(ox, oy, 0, mid))
        let i = 0
        let line = ""
        while i < FIELD_W {
            line = line + "-"
            i = i + 1
        }
        self._w(ansi_rgb(255, 255, 255) + ANSI["BOLD"] + line + self.R)
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
        self.inp = inp
        self.player = Player()
        self.shots = ShotPool()
        self.enemy_shots = EnemyShotPool()
        self.missiles = []                // misiles activos
        self.laser = null                 // laser activo (uno a la vez)
        self.rng = Rng(time_ms() % 2147483647)
        self.formation = null
        self.boss = null
        self.powerups = []
        self.renderer = Renderer(config)

        self.score = 0
        self.persistent_high_score = load_high_score()
        self.high_score = self.persistent_high_score
        self.is_new_high_score = false
        self.lives = LIVES_START
        self.wave = 1
        self.loop_num = 1
        self.paused = false
        self.running = true

        self.state = STATE_SPLASH
        self.state_timer_ms = 0

        self.last_tick_ms = time_ms()
        self.last_player_col = self.player.col

        self.held_left_ms  = 0
        self.held_right_ms = 0
        self.held_fire_ms  = 0
        self.held_shield_ms = 0
        self.held_timeout_ms = 80

        self.move_accum_ms = 0
        self.move_step_ms = 30

        // Sistema de armas
        self.weapon = WEAPON_SINGLE
        self.weapon_ms = 0                 // tiempo restante del arma activa (0 = single permanente)

        // Escudo
        self.shield_active_ms = 0
        self.shield_cooldown_ms = 0
        self.invuln_ms = 0

        // Combo
        self.combo = 0
        self.combo_timer_ms = 0

        // Animacion de muerte
        self.dying_anim_frame = 0

        // Picados
        self.dive_timer_ms = DIVE_INTERVAL_MS

        // Buffer
        self._pending_clear_cells = []
        self._cleared_shot_cells = []

        // Efectos visuales
        self.starfield = StarField(self.rng)
        self.explosions = []
        self.float_texts = []

        // Entidades nuevas
        self.split_minis = []
        self.enemy_bombs = []

        // Bomba inteligente
        self.bombs_remaining = 1           // 1 por vida
        self.bomb_flash_ms = 0

        // Dash
        self.dash_cooldown_ms = 0
        self.last_left_ms = 0              // para detectar doble-tap
        self.last_right_ms = 0

        // Boss fase 2
        self.boss_phase2_started = false

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

        if k == 117 or k == 85 {
            self.config.use_unicode = not self.config.use_unicode
            self.config.save()
            self.renderer.refresh_chars()
            return
        }

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

        // S = activar escudo
        if k == 115 or k == 83 {
            self._try_activate_shield()
            return
        }

        // B = bomba inteligente
        if k == BOMB_KEY or k == BOMB_KEY_UPPER {
            self._try_smart_bomb()
            return
        }

        // Doble-tap izquierda/derecha = dash
        // Requiere gap > 80ms (filtra auto-repeat del SO que es ~30ms)
        // y gap < DASH_KEY_TIMEOUT_MS (filtra pulsaciones lentas)
        let now = time_ms()
        if k == KEY_LEFT or k == 65 or k == 97 {
            let gap = now - self.last_left_ms
            if gap > 80 and gap < DASH_KEY_TIMEOUT_MS and self.dash_cooldown_ms <= 0 {
                self._do_dash(-1)
                self.last_left_ms = 0
            } else {
                self.last_left_ms = now
            }
        }
        if k == KEY_RIGHT or k == 68 or k == 100 {
            let gap = now - self.last_right_ms
            if gap > 80 and gap < DASH_KEY_TIMEOUT_MS and self.dash_cooldown_ms <= 0 {
                self._do_dash(1)
                self.last_right_ms = 0
            } else {
                self.last_right_ms = now
            }
        }
    }

    fn _move_player(self, dc) {
        if dc < 0 { self.player.move_left()  }
        if dc > 0 { self.player.move_right() }
    }

    fn _try_shoot(self) {
        if self.player.shot_cooldown_ms > 0 { return }
        let scol = self.player.shoot_col()
        let srow = self.player.shoot_row()
        let cooldown = SINGLE_COOLDOWN_MS

        if self.weapon == WEAPON_SINGLE {
            self.shots.add(scol, srow)
            cooldown = SINGLE_COOLDOWN_MS
        } elif self.weapon == WEAPON_DOUBLE {
            self.shots.add(self.player.col + 1, srow)
            self.shots.add(self.player.col + 3, srow)
            cooldown = DOUBLE_COOLDOWN_MS
        } elif self.weapon == WEAPON_SPREAD {
            // 3 disparos en abanico: el central va recto, los laterales
            // empiezan desplazados +/- 1 col por fila.
            self.shots.add(scol, srow)
            // Para el efecto abanico, anadimos disparos con offset lateral
            // inicial diferente. El ShotPool no soporta dx, asi que hacemos
            // que aparezcan en las cols laterales y suban rectos. Simple.
            self.shots.add(self.player.col, srow)
            self.shots.add(self.player.col + 3, srow)
            cooldown = SPREAD_COOLDOWN_MS
        } elif self.weapon == WEAPON_LASER {
            // Crear un laser que va desde row 0 hasta srow
            self.laser = Laser(scol, 0, srow)
            cooldown = LASER_COOLDOWN_MS
        } elif self.weapon == WEAPON_MISSILE {
            // Lanzar un misil teleguiado
            append(self.missiles, Missile(scol, srow))
            cooldown = MISSILE_COOLDOWN_MS
        }

        self.player.shot_cooldown_ms = cooldown
    }

    fn _try_activate_shield(self) {
        if self.shield_active_ms > 0 { return }
        if self.shield_cooldown_ms > 0 { return }
        self.shield_active_ms = SHIELD_DURATION_MS
        self.shield_cooldown_ms = SHIELD_COOLDOWN_MS
    }

    fn _try_smart_bomb(self) {
        if self.bombs_remaining <= 0 { return }
        self.bombs_remaining = self.bombs_remaining - 1
        self.bomb_flash_ms = BOMB_FLASH_MS
        // Matar todo en pantalla
        if self.formation != null {
            for b in self.formation.birds {
                if b.alive {
                    b.alive = false
                    self._add_combo_kill(b.point_value())
                    self._spawn_explosion(b.col, b.row, b.width)
                }
            }
            for e in self.formation.eggs {
                if e.alive { e.alive = false }
            }
        }
        // Matar todos los split_minis
        for sm in self.split_minis {
            if sm.alive { sm.alive = false }
        }
        // Limpiar disparos enemigos
        self.enemy_shots.shots = []
        self.enemy_bombs = []
        self.renderer.full_redraw = true
    }

    fn _do_dash(self, direction) {
        // Desplazar la nave N celdas instantaneamente
        let i = 0
        while i < DASH_DISTANCE {
            self._move_player(direction)
            i = i + 1
        }
        self.dash_cooldown_ms = DASH_COOLDOWN_MS
    }

    fn _spawn_explosion(self, col, row, width) {
        append(self.explosions, Explosion(col, row, width))
    }

    fn _spawn_float_text(self, col, row, text) {
        append(self.float_texts, FloatText(col, row, text))
    }

    fn _spawn_splitter_minis(self, col, row) {
        append(self.split_minis, SplitMini(col, row, -1))
        append(self.split_minis, SplitMini(col + 1, row, 1))
    }

    // Centraliza efectos al matar un pajaro: combo + explosion + texto + powerup + splitter
    fn _on_bird_killed(self, bird) {
        let pts = self._add_combo_kill(bird.point_value())
        self._spawn_explosion(bird.col, bird.row, bird.width)
        self._spawn_float_text(bird.col, bird.row, "+" + str(pts))
        self._maybe_drop_powerup(bird.col, bird.row)
        if bird.kind == "splitter" {
            self._spawn_splitter_minis(bird.col, bird.row)
        }
        for cc in bird.cells() { append(self._pending_clear_cells, cc) }
    }

    // Encuentra la columna del enemigo mas cercano por encima de from_row.
    // Devuelve -1 si no hay ninguno.
    fn _find_target_col(self, from_col, from_row) {
        let best_col = -1
        let best_dist = 999999
        if self.formation != null {
            for b in self.formation.birds {
                if not b.alive { continue }
                if b.row >= from_row { continue }   // solo arriba
                let dx = b.col + b.width / 2 - from_col
                if dx < 0 { dx = 0 - dx }
                let dy = from_row - b.row
                let d = dx + dy
                if d < best_dist {
                    best_dist = d
                    best_col = b.col + b.width / 2
                }
            }
        }
        if self.boss != null and self.boss.alive {
            let dx = self.boss.col + BOSS_W / 2 - from_col
            if dx < 0 { dx = 0 - dx }
            let dy = from_row - self.boss.row
            if dy < 0 { dy = 0 }
            let d = dx + dy
            if d < best_dist {
                best_col = self.boss.col + BOSS_W / 2
            }
        }
        return best_col
    }

    // -------------------------------------------------------------------------
    // Transiciones
    // -------------------------------------------------------------------------

    fn _start_new_game(self) {
        self.score = 0
        self.high_score = self.persistent_high_score
        self.is_new_high_score = false
        self.lives = LIVES_START
        self.wave = 1
        self.loop_num = 1
        self.player = Player()
        self.shots = ShotPool()
        self.enemy_shots = EnemyShotPool()
        self.missiles = []
        self.laser = null
        self.last_player_col = self.player.col
        self.combo = 0
        self.combo_timer_ms = 0
        self.weapon = WEAPON_SINGLE
        self.weapon_ms = 0
        self.shield_active_ms = 0
        self.shield_cooldown_ms = 0
        self.invuln_ms = 0
        self.powerups = []
        self.dive_timer_ms = DIVE_INTERVAL_MS
        self.explosions = []
        self.float_texts = []
        self.split_minis = []
        self.enemy_bombs = []
        self.bombs_remaining = 1
        self.bomb_flash_ms = 0
        self.dash_cooldown_ms = 0
        self.last_left_ms = 0
        self.last_right_ms = 0
        self.boss_phase2_started = false
        self._setup_wave()
        self.state = STATE_READY
        self.state_timer_ms = READY_MS
        self.renderer.full_redraw = true
    }

    fn _setup_wave(self) {
        // Wave 5 = boss; otras = formaciones
        if self.wave == 5 {
            self.formation = null
            self.boss = Boss()
        } else {
            self.formation = Formation(self.wave, self.rng, self.loop_num)
            self.boss = null
        }
        self.shots = ShotPool()
        self.enemy_shots = EnemyShotPool()
        self.powerups = []
        self.split_minis = []
        self.enemy_bombs = []
        self.explosions = []
        self.missiles = []
        self.laser = null
        self.boss_phase2_started = false
    }

    fn _start_wave(self, wave_num) {
        self.wave = wave_num
        self._setup_wave()
        self.state = STATE_READY
        self.state_timer_ms = READY_MS
        self.renderer.full_redraw = true
    }

    fn _on_wave_done(self) {
        // Bonus: oleadas 1-4 = base, oleada 5 (boss) = mucho mas
        let bonus = WAVE_CLEAR_BONUS
        if self.wave == 5 { bonus = WAVE_CLEAR_BONUS * 5 }
        self.score = self.score + bonus
        if self.score > self.high_score { self.high_score = self.score }
        self.state = STATE_WAVE_DONE
        self.state_timer_ms = WAVE_DONE_MS
    }

    fn _on_wave_done_finished(self) {
        // Pasar a la siguiente oleada. Tras la 5 -> reiniciar al loop+1 con
        // mas dificultad (loop infinito).
        if self.wave >= WAVES_PER_LOOP {
            self.loop_num = self.loop_num + 1
            self._start_wave(1)
        } else {
            self._start_wave(self.wave + 1)
        }
    }

    fn _on_player_hit(self) {
        // Llamado cuando el jugador es alcanzado y no tiene escudo
        self.lives = self.lives - 1
        self.combo = 0
        self.combo_timer_ms = 0
        self.weapon = WEAPON_SINGLE
        self.weapon_ms = 0
        self.missiles = []
        self.laser = null
        if self.lives <= 0 {
            self._on_dying()
        } else {
            self.invuln_ms = SHIELD_INVULN_AFTER_HIT_MS
            self.bombs_remaining = 1        // 1 bomba nueva cada vida
        }
    }

    fn _on_dying(self) {
        self.state = STATE_DYING
        self.state_timer_ms = DYING_MS
        self.dying_anim_frame = 0
    }

    fn _on_dying_finished(self) {
        // Todas las vidas perdidas -> game over
        self.is_new_high_score = false
        if self.score > self.persistent_high_score {
            self.is_new_high_score = true
            self.persistent_high_score = self.score
            save_high_score(self.persistent_high_score)
        }
        self.state = STATE_GAME_OVER
        self.state_timer_ms = GAME_OVER_MS
    }

    fn _on_game_over_done(self) {
        self.state = STATE_SPLASH
        self.renderer.full_redraw = true
    }

    // -------------------------------------------------------------------------
    // Logica de combo
    // -------------------------------------------------------------------------

    fn _add_combo_kill(self, base_pts) {
        self.combo = self.combo + 1
        self.combo_timer_ms = COMBO_TIMEOUT_MS
        // Multiplicador: combo 1=x1, combo 2-4=x2, combo 5+=x3
        let mult = 1
        if self.combo >= 2 { mult = 2 }
        if self.combo >= 5 { mult = 3 }
        let pts = base_pts * mult
        self.score = self.score + pts
        if self.score > self.high_score { self.high_score = self.score }
        return pts
    }

    // -------------------------------------------------------------------------
    // Power-ups
    // -------------------------------------------------------------------------

    fn _maybe_drop_powerup(self, col, row) {
        let r = self.rng.range(0, 10000)
        if r >= POWERUP_DROP_PROB { return }
        // Distribucion: 30% Double, 20% Spread, 15% Laser, 15% Missile,
        // 10% Shield, 10% 1UP
        let kind = POWERUP_DOUBLE
        let r2 = self.rng.range(0, 100)
        if r2 < 30 {
            kind = POWERUP_DOUBLE
        } elif r2 < 50 {
            kind = POWERUP_SPREAD
        } elif r2 < 65 {
            kind = POWERUP_LASER
        } elif r2 < 80 {
            kind = POWERUP_MISSILE
        } elif r2 < 90 {
            kind = POWERUP_SHIELD
        } else {
            kind = POWERUP_LIFE
        }
        append(self.powerups, PowerUp(col, row, kind))
    }

    fn _apply_powerup(self, kind) {
        if kind == POWERUP_DOUBLE {
            self.weapon = WEAPON_DOUBLE
            self.weapon_ms = WEAPON_DURATION_MS
        } elif kind == POWERUP_SPREAD {
            self.weapon = WEAPON_SPREAD
            self.weapon_ms = WEAPON_DURATION_MS
        } elif kind == POWERUP_LASER {
            self.weapon = WEAPON_LASER
            self.weapon_ms = WEAPON_DURATION_MS
        } elif kind == POWERUP_MISSILE {
            self.weapon = WEAPON_MISSILE
            self.weapon_ms = WEAPON_DURATION_MS
        } elif kind == POWERUP_SHIELD {
            self.shield_cooldown_ms = 0
            self.shield_active_ms = 0
        } elif kind == POWERUP_LIFE {
            self.lives = self.lives + 1
        }
        self.score = self.score + PT_POWERUP_PICKUP
        if self.score > self.high_score { self.high_score = self.score }
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
        if self.state == STATE_WAVE_DONE {
            self.state_timer_ms = self.state_timer_ms - elapsed
            if self.state_timer_ms <= 0 {
                self._on_wave_done_finished()
            }
            return
        }
        if self.state == STATE_DYING {
            self.dying_anim_frame = self.dying_anim_frame + 1
            self.state_timer_ms = self.state_timer_ms - elapsed
            if self.state_timer_ms <= 0 {
                self._on_dying_finished()
            }
            return
        }
        if self.state == STATE_GAME_OVER {
            self.state_timer_ms = self.state_timer_ms - elapsed
            if self.state_timer_ms <= 0 {
                self._on_game_over_done()
            }
            return
        }

        // STATE_PLAYING

        // Detectar teclas mantenidas
        let want_left  = false
        let want_right = false
        let want_fire  = false
        let want_shield = false

        if self.inp.has_gaks {
            if self.inp.is_key_down(VK_LEFT)  or self.inp.is_key_down(VK_A) { want_left  = true }
            if self.inp.is_key_down(VK_RIGHT) or self.inp.is_key_down(VK_D) { want_right = true }
            if self.inp.is_key_down(VK_SPACE) { want_fire = true }
            if self.inp.is_key_down(VK_S) { want_shield = true }
        } else {
            if self.held_left_ms  > 0 { self.held_left_ms  = self.held_left_ms  - elapsed }
            if self.held_right_ms > 0 { self.held_right_ms = self.held_right_ms - elapsed }
            if self.held_fire_ms  > 0 { self.held_fire_ms  = self.held_fire_ms  - elapsed }
            if self.held_left_ms  > 0 { want_left  = true }
            if self.held_right_ms > 0 { want_right = true }
            if self.held_fire_ms  > 0 { want_fire  = true }
        }

        // Activar escudo si se pulsa S
        if want_shield {
            self._try_activate_shield()
        }

        // Movimiento del jugador
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
        if move_start_col != self.player.col {
            self.last_player_col = move_start_col
        }

        // Cooldown disparo
        if self.player.shot_cooldown_ms > 0 {
            self.player.shot_cooldown_ms = self.player.shot_cooldown_ms - elapsed
            if self.player.shot_cooldown_ms < 0 { self.player.shot_cooldown_ms = 0 }
        }
        if want_fire { self._try_shoot() }

        // Decrementar timers de power-ups, escudo, invuln, combo
        if self.weapon_ms > 0 { self.weapon_ms = self.weapon_ms - elapsed }
        if self.weapon_ms < 0 { self.weapon_ms = 0 }
        // Si caduca el arma, vuelve al single
        if self.weapon != WEAPON_SINGLE and self.weapon_ms <= 0 {
            self.weapon = WEAPON_SINGLE
        }
        if self.shield_active_ms > 0 { self.shield_active_ms = self.shield_active_ms - elapsed }
        if self.shield_active_ms < 0 { self.shield_active_ms = 0 }
        if self.shield_cooldown_ms > 0 { self.shield_cooldown_ms = self.shield_cooldown_ms - elapsed }
        if self.shield_cooldown_ms < 0 { self.shield_cooldown_ms = 0 }
        if self.invuln_ms > 0 { self.invuln_ms = self.invuln_ms - elapsed }
        if self.invuln_ms < 0 { self.invuln_ms = 0 }
        if self.combo_timer_ms > 0 {
            self.combo_timer_ms = self.combo_timer_ms - elapsed
            if self.combo_timer_ms <= 0 { self.combo = 0 }
        }

        // Pre-positions de pajaros (para limpiar)
        let pre_bird_cells = []
        if self.formation != null {
            for b in self.formation.birds {
                if b.alive {
                    for cc in b.cells() { append(pre_bird_cells, cc) }
                }
            }
            // Posiciones de huevos
            for e in self.formation.eggs {
                if e.alive {
                    for cc in e.cells() { append(pre_bird_cells, cc) }
                }
            }
        }
        // Pre-positions del boss (puede moverse)
        let pre_boss_cells = []
        if self.boss != null and self.boss.alive {
            // Body: 16 chars en body_row + 8 chars en belly_row
            let i = 0
            while i < BOSS_W {
                append(pre_boss_cells, [self.boss.col + i, self.boss.body_row()])
                i = i + 1
            }
            i = 0
            while i < 8 {
                append(pre_boss_cells, [self.boss.col + 4 + i, self.boss.belly_row()])
                i = i + 1
            }
        }

        // Actualizar formacion / boss
        if self.formation != null {
            self.formation.update(elapsed)
            self.formation.try_drop_eggs(elapsed)
        }
        if self.boss != null and self.boss.alive {
            self.boss.step(elapsed)
            // Boss dispara
            let new_shots = self.boss.try_shoot(self.rng)
            for sh in new_shots { self.enemy_shots.add(sh[0], sh[1]) }
        }

        // Picados: cuando el timer llega a 0, lanzar uno y resetear
        if self.formation != null {
            self.dive_timer_ms = self.dive_timer_ms - elapsed
            if self.dive_timer_ms <= 0 {
                self.dive_timer_ms = DIVE_INTERVAL_MS
                let target = self.player.col + 2
                self.formation.try_start_dive(target)
            }
        }

        // Avanzar misiles y aplicar colision con enemigos
        let surviving_missiles = []
        for m in self.missiles {
            if not m.alive {
                append(self._cleared_shot_cells, [m.col, m.row])
                continue
            }
            let target_col = self._find_target_col(m.col, m.row)
            let prev = m.step(elapsed, target_col)
            if prev != null {
                append(self._cleared_shot_cells, [prev[0], prev[1]])
            }
            if not m.alive {
                continue
            }
            // Colision con enemigos
            let hit = false
            if self.boss != null and self.boss.alive {
                let res = self.boss.apply_player_shot(m.col, m.row)
                if res != "miss" {
                    hit = true
                    append(self._cleared_shot_cells, [m.col, m.row])
                    if res == "killed" {
                        self._add_combo_kill(PT_BOSS)
                    } else {
                        self._add_combo_kill(PT_BOSS_PIECE * 2)
                    }
                    m.alive = false
                }
            }
            if not hit and self.formation != null {
                let res = self.formation.apply_player_shot(m.col, m.row)
                if res[0] {
                    hit = true
                    append(self._cleared_shot_cells, [m.col, m.row])
                    let target = res[1]
                    if target == "egg" {
                        self._add_combo_kill(PT_EGG)
                    } else {
                        if not target.alive {
                            self._on_bird_killed(target)
                        }
                    }
                    m.alive = false
                }
            }
            if m.alive { append(surviving_missiles, m) }
        }
        self.missiles = surviving_missiles

        // Avanzar laser y aplicar colision (atraviesa enemigos)
        if self.laser != null {
            self.laser.step(elapsed)
            if not self.laser.alive {
                // Limpiar la columna entera del laser
                let lr = self.laser.top_row
                while lr <= self.laser.bottom_row {
                    append(self._cleared_shot_cells, [self.laser.col, lr])
                    lr = lr + 1
                }
                self.laser = null
            } else {
                // El laser atraviesa: comprueba colision con cada enemigo en su col
                if self.formation != null {
                    for b in self.formation.birds {
                        if not b.alive { continue }
                        // ¿La col del laser cae dentro del rango del pajaro?
                        if self.laser.col >= b.col and self.laser.col <= b.col + b.width - 1 {
                            // ¿La fila del pajaro esta dentro del laser?
                            if b.row >= self.laser.top_row and b.row <= self.laser.bottom_row {
                                b.take_damage()
                                if not b.alive {
                                    self._on_bird_killed(b)
                                }
                            }
                        }
                    }
                    // Tambien huevos
                    for e in self.formation.eggs {
                        if not e.alive { continue }
                        if self.laser.col >= e.col and self.laser.col <= e.col + EGG_W - 1 {
                            if e.row >= self.laser.top_row and e.row <= self.laser.bottom_row {
                                e.alive = false
                                self._add_combo_kill(PT_EGG)
                            }
                        }
                    }
                }
                if self.boss != null and self.boss.alive {
                    // El laser tambien afecta al boss
                    let res = self.boss.apply_player_shot(self.laser.col, self.boss.body_row())
                    if res == "killed" { self._add_combo_kill(PT_BOSS) }
                    let res2 = self.boss.apply_player_shot(self.laser.col, self.boss.belly_row())
                    if res2 != "miss" and res2 != res { self._add_combo_kill(PT_BOSS_PIECE) }
                }
            }
        }

        // Avanzar disparos del jugador
        let cleared_player_shots = self.shots.step(elapsed)
        for cc in cleared_player_shots { append(self._cleared_shot_cells, cc) }

        // Colision disparos jugador vs enemigos
        let surviving_shots = []
        for s in self.shots.shots {
            let hit = false
            let sc = s["col"]; let sr = s["row"]
            // Boss
            if self.boss != null and self.boss.alive {
                let res = self.boss.apply_player_shot(sc, sr)
                if res != "miss" {
                    hit = true
                    append(self._cleared_shot_cells, [sc, sr])
                    if res == "belly" {
                        self._add_combo_kill(PT_BOSS_PIECE)
                    } elif res == "alien" {
                        self._add_combo_kill(PT_BOSS_PIECE * 2)
                    } elif res == "killed" {
                        self._add_combo_kill(PT_BOSS)
                    }
                }
            }
            // Formacion
            if not hit and self.formation != null {
                let res = self.formation.apply_player_shot(sc, sr)
                if res[0] {
                    hit = true
                    append(self._cleared_shot_cells, [sc, sr])
                    let target = res[1]
                    if target == "egg" {
                        self._add_combo_kill(PT_EGG)
                    } else {
                        // target es un EnemyBird
                        if not target.alive {
                            self._on_bird_killed(target)
                        }
                    }
                }
            }
            if not hit { append(surviving_shots, s) }
        }
        self.shots.shots = surviving_shots

        // Marcar celdas viejas de pajaros y boss para limpiar
        for cc in pre_bird_cells { append(self._cleared_shot_cells, cc) }
        for cc in pre_boss_cells { append(self._cleared_shot_cells, cc) }

        // Disparos enemigos: pajaros formacion
        if self.formation != null {
            let new_enemy_shots = self.formation.try_enemy_shoot(self.player.col + 2)
            for sh in new_enemy_shots { self.enemy_shots.add(sh[0], sh[1]) }
        }

        // Avanzar disparos enemigos
        let cleared_enemy_shots = self.enemy_shots.step(elapsed)
        for cc in cleared_enemy_shots { append(self._cleared_shot_cells, cc) }

        // Colision disparos enemigos vs jugador
        let surviving_enemy_shots = []
        for s in self.enemy_shots.shots {
            let sc = s["col"]; let sr = s["row"]
            let pc = self.player.col
            if sr == self.player.row and sc >= pc and sc <= pc + 3 {
                // Ha tocado al jugador
                if self.shield_active_ms > 0 or self.invuln_ms > 0 {
                    // Bloqueado
                    append(self._cleared_shot_cells, [sc, sr])
                } else {
                    self._on_player_hit()
                    append(self._cleared_shot_cells, [sc, sr])
                    if self.state != STATE_PLAYING { return }
                }
            } else {
                append(surviving_enemy_shots, s)
            }
        }
        self.enemy_shots.shots = surviving_enemy_shots

        // Avanzar power-ups que caen
        let surviving_pu = []
        for pu in self.powerups {
            let prev = pu.step(elapsed)
            if prev != -1 {
                append(self._cleared_shot_cells, [pu.col, prev])
                append(self._cleared_shot_cells, [pu.col + 1, prev])
            }
            if pu.row >= FIELD_H {
                pu.alive = false
                continue
            }
            // ¿Recoge el jugador?
            if pu.row == self.player.row {
                let pc = self.player.col
                if pu.col + 1 >= pc and pu.col <= pc + 3 {
                    self._apply_powerup(pu.kind)
                    append(self._cleared_shot_cells, [pu.col, pu.row])
                    append(self._cleared_shot_cells, [pu.col + 1, pu.row])
                    pu.alive = false
                    continue
                }
            }
            append(surviving_pu, pu)
        }
        self.powerups = surviving_pu

        // Colision huevos vs jugador, y purga de huevos muertos
        if self.formation != null {
            let surviving_eggs = []
            for e in self.formation.eggs {
                if not e.alive { continue }    // ya muerto, no lo conservamos
                // ¿Toca al jugador?
                let pc = self.player.col
                if e.row == self.player.row and e.col + 1 >= pc and e.col <= pc + 3 {
                    if self.shield_active_ms > 0 or self.invuln_ms > 0 {
                        // bloqueado, simplemente desaparece
                        e.alive = false
                        continue
                    }
                    self._on_player_hit()
                    e.alive = false
                    if self.state != STATE_PLAYING { return }
                    continue
                }
                append(surviving_eggs, e)
            }
            self.formation.eggs = surviving_eggs
        }

        // ---------------------------------------------------------------
        // Efectos visuales
        // ---------------------------------------------------------------
        self.starfield.step(elapsed)

        // Explosiones
        let alive_expl = []
        for ex in self.explosions {
            ex.step(elapsed)
            if ex.alive { append(alive_expl, ex) }
        }
        self.explosions = alive_expl

        // Float texts
        let alive_ft = []
        for ft in self.float_texts {
            ft.step(elapsed)
            if ft.alive { append(alive_ft, ft) }
        }
        self.float_texts = alive_ft

        // Bomb flash
        if self.bomb_flash_ms > 0 {
            self.bomb_flash_ms = self.bomb_flash_ms - elapsed
            if self.bomb_flash_ms < 0 { self.bomb_flash_ms = 0 }
        }

        // Dash cooldown
        if self.dash_cooldown_ms > 0 {
            self.dash_cooldown_ms = self.dash_cooldown_ms - elapsed
            if self.dash_cooldown_ms < 0 { self.dash_cooldown_ms = 0 }
        }

        // ---------------------------------------------------------------
        // SplitMinis (mini-pajaros de splitters)
        // ---------------------------------------------------------------
        let alive_sm = []
        for sm in self.split_minis {
            sm.step(elapsed)
            if not sm.alive { continue }
            // Colision con jugador
            let pc = self.player.col
            if sm.row == self.player.row and sm.col >= pc and sm.col <= pc + 3 {
                if self.shield_active_ms <= 0 and self.invuln_ms <= 0 {
                    self._on_player_hit()
                    if self.state != STATE_PLAYING { return }
                }
                sm.alive = false
                continue
            }
            append(alive_sm, sm)
        }
        self.split_minis = alive_sm

        // Colision disparos del jugador con split_minis
        let surv_shots2 = []
        for s in self.shots.shots {
            let hit = false
            for sm in self.split_minis {
                if sm.is_hit_by(s["col"], s["row"]) {
                    sm.alive = false
                    sm.hp = 0
                    self._add_combo_kill(PT_SPLIT_MINI)
                    self._spawn_explosion(sm.col, sm.row, 1)
                    self._spawn_float_text(sm.col, sm.row, "+" + str(PT_SPLIT_MINI))
                    hit = true
                    break
                }
            }
            if not hit { append(surv_shots2, s) }
        }
        self.shots.shots = surv_shots2

        // ---------------------------------------------------------------
        // Enemy bombs (de los bombers)
        // ---------------------------------------------------------------
        if self.formation != null {
            for b in self.formation.birds {
                if not b.alive { continue }
                if b.kind != "bomber" { continue }
                b.egg_cooldown_ms = b.egg_cooldown_ms - elapsed
                if b.egg_cooldown_ms <= 0 {
                    append(self.enemy_bombs, EnemyBomb(b.col + b.width / 2, b.row + 1))
                    b.egg_cooldown_ms = 3000 + self.rng.range(0, 3000)
                }
            }
        }
        let alive_eb = []
        for eb in self.enemy_bombs {
            eb.step(elapsed)
            if not eb.alive {
                if eb.exploded {
                    // Explosion en area: danio al jugador si esta cerca
                    let blast = eb.blast_cells()
                    let pc = self.player.col
                    for bc in blast {
                        if bc[1] == self.player.row and bc[0] >= pc and bc[0] <= pc + 3 {
                            if self.shield_active_ms <= 0 and self.invuln_ms <= 0 {
                                self._on_player_hit()
                                if self.state != STATE_PLAYING { return }
                            }
                            break
                        }
                    }
                    self._spawn_explosion(eb.col - 1, eb.row - 1, 3)
                }
                continue
            }
            // Colision directa con jugador
            let pc = self.player.col
            if eb.row == self.player.row and eb.col >= pc and eb.col <= pc + 3 {
                if self.shield_active_ms <= 0 and self.invuln_ms <= 0 {
                    self._on_player_hit()
                    if self.state != STATE_PLAYING { return }
                }
                eb.alive = false
                continue
            }
            append(alive_eb, eb)
        }
        self.enemy_bombs = alive_eb

        // ¿Oleada limpia?
        if self.formation != null and self.formation.is_clear() {
            // Tambien comprobamos que no queden split_minis vivos
            let minis_left = 0
            for sm in self.split_minis {
                if sm.alive { minis_left = minis_left + 1 }
            }
            if minis_left == 0 {
                self._on_wave_done()
            }
        }
        if self.boss != null and not self.boss.alive {
            // Boss fase 2: al morir, spawn mini-pajaros como explosion final
            if not self.boss_phase2_started {
                self.boss_phase2_started = true
                let i = 0
                while i < BOSS_PHASE2_SPAWN {
                    let dx = -1
                    if i % 2 == 0 { dx = 1 }
                    let sc = self.boss.col + 4 + i
                    append(self.split_minis, SplitMini(sc, self.boss.body_row(), dx))
                    i = i + 1
                }
                self._spawn_explosion(self.boss.col, self.boss.body_row(), BOSS_W)
            }
            // Esperar a que todos los split_minis mueran o salgan del campo
            let minis_left = 0
            for sm in self.split_minis {
                if sm.alive { minis_left = minis_left + 1 }
            }
            if self.boss_phase2_started and minis_left == 0 {
                self._on_wave_done()
            }
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

        if self.state == STATE_SPLASH {
            self._render_splash()
            r.full_redraw = false
            r._flush()
            return
        }

        // Marco
        if r.full_redraw {
            r.draw_frame(self.ox, self.oy)
        }
        r.draw_hud(self.score, self.high_score, self.lives, self.wave, self.ox, hud_y)

        // Limpiar interior del campo COMPLETO cada frame. Mas lento pero
        // mucho mas robusto que el sistema dirty: nunca quedan rastros y
        // los enemigos siempre se pintan en su posicion correcta.
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

        // Resetear listas de dirty cells (ya no necesitamos limpiarlas
        // individualmente; el campo se ha limpiado entero)
        self._cleared_shot_cells = []
        self._pending_clear_cells = []
        self.last_player_col = self.player.col

        // Estrellas de fondo (detras de todo)
        for s in self.starfield.stars {
            r.draw_star(self.ox, self.oy, s[0], s[1])
        }

        // Pintar pajaros vivos
        if self.formation != null {
            for b in self.formation.birds {
                if b.alive {
                    r.draw_bird(self.ox, self.oy, b, self.wave)
                }
            }
            // Huevos
            for e in self.formation.eggs {
                if e.alive {
                    r.draw_egg(self.ox, self.oy, e)
                }
            }
        }

        // Boss
        if self.boss != null and self.boss.alive {
            r.draw_boss(self.ox, self.oy, self.boss)
        }

        // Nave del jugador (parpadea durante invuln)
        let draw_player_now = true
        if self.invuln_ms > 0 {
            // Parpadeo cada 80ms
            let phase = self.invuln_ms / 80
            if phase % 2 == 0 { draw_player_now = false }
        }
        if self.state == STATE_DYING {
            self._draw_dying_player()
        } elif self.state == STATE_PLAYING or self.state == STATE_READY or self.state == STATE_WAVE_DONE {
            if draw_player_now {
                r.draw_player(self.ox, self.oy, self.player)
            } else {
                // borrar la zona del jugador para crear el efecto parpadeo
                r._w(r.cell_pos(self.ox, self.oy, self.player.col, self.player.row))
                r._w("    ")
            }
            // Escudo
            if self.shield_active_ms > 0 {
                r.draw_shield(self.ox, self.oy, self.player.col, self.player.row)
            }
        }

        // Disparos
        if self.state == STATE_PLAYING or self.state == STATE_DYING {
            for s in self.shots.shots {
                r.draw_shot(self.ox, self.oy, s["col"], s["row"])
            }
            for s in self.enemy_shots.shots {
                r.draw_enemy_shot(self.ox, self.oy, s["col"], s["row"])
            }
            // Misiles y laser del jugador
            for m in self.missiles {
                if m.alive { r.draw_missile(self.ox, self.oy, m) }
            }
            if self.laser != null and self.laser.alive {
                r.draw_laser(self.ox, self.oy, self.laser)
            }
        }

        // Power-ups cayendo
        for pu in self.powerups {
            if pu.alive {
                r.draw_powerup(self.ox, self.oy, pu)
            }
        }

        // Explosiones
        for ex in self.explosions {
            if ex.alive {
                r.draw_explosion(self.ox, self.oy, ex)
            }
        }

        // SplitMinis
        for sm in self.split_minis {
            if sm.alive {
                r.draw_split_mini(self.ox, self.oy, sm)
            }
        }

        // Enemy bombs
        for eb in self.enemy_bombs {
            if eb.alive {
                r.draw_enemy_bomb(self.ox, self.oy, eb)
            }
        }

        // Float texts (+80, +250, etc)
        for ft in self.float_texts {
            if ft.alive {
                r.draw_float_text(self.ox, self.oy, ft)
            }
        }

        // Bomb flash (bomba inteligente)
        if self.bomb_flash_ms > 0 {
            r.draw_bomb_flash(self.ox, self.oy)
        }

        // Indicador de combo
        if self.combo >= 2 {
            let combo_msg = "COMBO x" + str(self.combo)
            if self.combo >= 5 { combo_msg = "MEGA COMBO x" + str(self.combo) }
            r._w(ansi_cursor_pos(self.oy + FIELD_H - 1, self.ox + 1))
            r._w(r.col_combo + ANSI["BOLD"] + combo_msg + r.R)
        }

        // Indicador de arma activa
        if self.weapon != WEAPON_SINGLE and self.weapon_ms > 0 {
            r._w(ansi_cursor_pos(self.oy + FIELD_H - 2, self.ox + 1))
            let t = self.weapon_ms / 1000 + 1
            r._w(r.col_powerup + ANSI["BOLD"] + r.weapon_label(self.weapon) + " " + str(t) + "s" + r.R)
        }

        // Indicador de escudo
        if self.shield_active_ms > 0 {
            r._w(ansi_cursor_pos(self.oy + FIELD_H - 2, self.ox + FIELD_W - 16))
            r._w(r.col_shield + ANSI["BOLD"] + "SHIELD ACTIVE" + r.R)
        } elif self.shield_cooldown_ms > 0 {
            r._w(ansi_cursor_pos(self.oy + FIELD_H - 2, self.ox + FIELD_W - 16))
            let t = self.shield_cooldown_ms / 1000 + 1
            r._w(r.col_dim + "shield: " + str(t) + "s   " + r.R)
        } else {
            r._w(ansi_cursor_pos(self.oy + FIELD_H - 2, self.ox + FIELD_W - 16))
            r._w(r.col_shield + "SHIELD READY (S)" + r.R)
        }

        // Indicador de bomba inteligente
        r._w(ansi_cursor_pos(self.oy + FIELD_H - 3, self.ox + 1))
        if self.bombs_remaining > 0 {
            r._w(ansi_rgb(255, 200, 80) + ANSI["BOLD"] + "BOMB(B):" + str(self.bombs_remaining) + r.R)
        } else {
            r._w(r.col_dim + "BOMB: --  " + r.R)
        }

        // Indicador de dash
        r._w(ansi_cursor_pos(self.oy + FIELD_H - 3, self.ox + FIELD_W - 16))
        if self.dash_cooldown_ms > 0 {
            let dt = self.dash_cooldown_ms / 1000 + 1
            r._w(r.col_dim + "DASH: " + str(dt) + "s   " + r.R)
        } else {
            r._w(ansi_rgb(180, 255, 180) + "DASH READY (2x)" + r.R)
        }

        // Overlays
        if self.state == STATE_READY {
            let title = "WAVE " + str(self.wave)
            if self.loop_num > 1 { title = "LOOP " + str(self.loop_num) + " - " + title }
            r.draw_centered("READY!", ansi_rgb(255, 255, 0), self.ox, self.oy, FIELD_H / 2 + 4)
            r.draw_centered(title, ansi_rgb(180, 180, 255), self.ox, self.oy, FIELD_H / 2 + 6)
        }
        if self.state == STATE_WAVE_DONE {
            let msg = "WAVE " + str(self.wave) + " CLEAR!"
            if self.wave == 5 { msg = "BOSS DESTROYED!" }
            r.draw_centered(msg, ansi_rgb(255, 255, 0), self.ox, self.oy, FIELD_H / 2 - 1)
            let bonus = WAVE_CLEAR_BONUS
            if self.wave == 5 { bonus = WAVE_CLEAR_BONUS * 5 }
            r.draw_centered("+" + str(bonus) + " bonus", ansi_rgb(180, 255, 180), self.ox, self.oy, FIELD_H / 2 + 1)
        }
        if self.state == STATE_GAME_OVER {
            r.draw_centered("GAME OVER", ansi_rgb(255, 80, 80), self.ox, self.oy, FIELD_H / 2 - 1)
            r.draw_centered("Score: " + str(self.score), ansi_rgb(255, 255, 0), self.ox, self.oy, FIELD_H / 2 + 1)
            if self.is_new_high_score {
                r.draw_centered("** NEW HIGH SCORE **", ansi_rgb(120, 255, 120), self.ox, self.oy, FIELD_H / 2 + 3)
            }
        }
        if self.paused {
            r.draw_centered("PAUSED", ansi_rgb(180, 180, 255), self.ox, self.oy, FIELD_H / 2)
        }

        // Status bar
        let mode_str = "Unicode"
        if not self.config.use_unicode { mode_str = "ASCII" }
        let msg = "Move:<-/->  SPC:shoot  S:shield  B:bomb  2x<-:dash  U:" + mode_str + "  P Q"
        if self.paused { msg = "*** PAUSED ***  P to resume" }
        r.draw_status(msg, self.ox, status_y)

        r.full_redraw = false
        r._flush()
    }

    fn _draw_dying_player(self) {
        // Animacion de explosion: cambio de glifos cada N frames
        let r = self.renderer
        let frames = ["<AA>", " XX ", "*  *", " .. ", "    "]
        let idx = self.dying_anim_frame / 8
        if idx >= len(frames) { idx = len(frames) - 1 }
        let g = frames[idx]
        r._w(r.cell_pos(self.ox, self.oy, self.player.col, self.player.row))
        r._w(ansi_rgb(255, 100, 100) + ANSI["BOLD"] + g + r.R)
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
        let hi    = "HIGH SCORE: " + str(self.persistent_high_score)
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

        // Pequena leyenda con controles
        let ctrls = "<-/-> move  SPC shoot  S shield  B bomb  2x<- dash"
        r._w(ansi_cursor_pos(cy + 14, cx - len(ctrls) / 2))
        r._w(ansi_rgb(150, 150, 200) + ctrls + ANSI["RESET"])
    }
}


// =============================================================================
// MAIN
// =============================================================================

fn main() {
    let cfg = Config()
    let inp = InputBackend()
    let game = Game(cfg, inp)

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
