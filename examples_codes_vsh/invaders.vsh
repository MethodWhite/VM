// ============================================================================
// VestaShell - Space Invaders de consola (tiempo real via FFI)
// VERSION COMENTADA LINEA A LINEA
// ============================================================================
//
// Este fichero es el codigo del juego con explicaciones detalladas de cada
// linea. La intencion es servir como material didactico para entender:
//   - Como se construye un game loop en VestaShell.
//   - Como funcionan las clases, herencia y polimorfismo en .vsh.
//   - Como se llama a funciones nativas via FFI.
//   - Como se renderiza en consola sin parpadeo usando secuencias ANSI.
//   - Como se manejan colisiones, fisicas y entrada no bloqueante.
//
// El juego ejecuta a 30 fps. Cada frame: leer teclas, avanzar logica,
// pintar el frame entero a un buffer en memoria, volcarlo a pantalla.
//
// ============================================================================


// ----------------------------------------------------------------------------
// SECCION 1: Secuencias ANSI literales
// ----------------------------------------------------------------------------
// El mapa builtin ANSI no expone todas las secuencias que necesitamos
// (HOME, CURSOR_HIDE/SHOW, etc.). Y el lexer de strings de VestaShell no
// interpreta el escape octal "\033", asi que escribir "\033[H" en el
// codigo da literalmente cuatro caracteres en lugar del byte ESC + texto.
//
// Truco: el mapa SI contiene secuencias bien formadas (ej. CLEAR_LINE = ESC+"[2K\r"),
// asi que cogemos el primer byte de una de ellas para obtener el ESC real
// y construimos las que faltan concatenando.

let ESC             = substr(ANSI["CLEAR_LINE"], 0, 1)  // Extrae el byte 0x1B (ESC) del mapa builtin.
                                                        // substr(s, inicio, longitud) -> primer caracter de "ESC[2K\r".
let ESC_HOME        = ESC + "[H"                        // Mueve el cursor a la fila 1, columna 1.
                                                        // Lo usamos cada frame en lugar de "limpiar pantalla"
                                                        // para evitar parpadeo: sobreescribimos encima.
let ESC_CLEAR       = ESC + "[2J" + ESC + "[H"          // Limpia toda la pantalla y mueve cursor al inicio.
                                                        // Solo lo usamos UNA VEZ al arrancar el juego.
let ESC_CURSOR_HIDE = ESC + "[?25l"                     // Oculta el cursor (queda mas limpio durante el juego).
let ESC_CURSOR_SHOW = ESC + "[?25h"                     // Muestra el cursor de nuevo al salir.
let ESC_CLR_EOL     = ESC + "[K"                        // "Clear to End Of Line": borra desde el cursor hasta
                                                        // el final de la linea. Lo ponemos al final de cada
                                                        // linea pintada para no dejar restos del frame previo.
let ESC_CLR_EOS     = ESC + "[J"                        // "Clear to End Of Screen": limpia todo lo que quede
                                                        // por debajo del cursor (por si el frame anterior era
                                                        // mas alto).


// ----------------------------------------------------------------------------
// SECCION 2: Capa de entrada no bloqueante via FFI
// ----------------------------------------------------------------------------
// Problema: input() en VestaShell bloquea hasta que el usuario pulse Enter.
// Para un juego en tiempo real necesitamos detectar pulsaciones SIN bloquear
// el game loop. Solucion: llamar a funciones nativas de la libreria estandar
// del sistema mediante FFI (Foreign Function Interface).
//
// En Windows: msvcrt.dll expone _kbhit (no bloqueante) y _getch (lee sin echo).
// En Linux/macOS: hay que poner el terminal en "modo raw" con stty y
// despues getchar de libc devuelve -1 si no hay datos.

class InputBackend {                                    // Encapsula toda la diferencia entre plataformas
                                                        // detras de un metodo poll_key() simple.
    "Backend de entrada no bloqueante usando FFI."      // Docstring; util para help() en el REPL.

    fn __init__(self) {                                 // Constructor; se llama al hacer InputBackend().
        self.os = platform()                            // platform() es un builtin que devuelve "windows",
                                                        // "linux" o "macos".
        self.lib = 0                                    // Handle de la libreria (0 = sin abrir).
        self.kbhit_sym = 0                              // Direccion del simbolo _kbhit (Windows).
        self.getch_sym = 0                              // Direccion del simbolo _getch (Windows).
        self.getchar_sym = 0                            // Direccion de getchar (POSIX).
        self._setup()                                   // Hace el dlopen + dlsym apropiado para el SO.
    }

    fn _setup(self) {                                   // Metodo privado por convencion (guion bajo).
        if self.os == "windows" {                       // En Windows usamos las funciones tipicas del CRT.
            self.lib = ffi_open("msvcrt.dll")           // Carga la DLL en memoria; equivale a LoadLibrary.
            self.kbhit_sym = ffi_sym(self.lib, "_kbhit")// Resuelve la direccion de _kbhit; sin parametros.
                                                        // _kbhit retorna 0 si no hay tecla, !=0 si la hay.
            self.getch_sym = ffi_sym(self.lib, "_getch")// _getch lee un caracter sin echo y sin buffer.
        } else {                                        // Linux o macOS.
            let libname = "libc.so.6"                   // Nombre de libc en glibc (Linux).
            if self.os == "macos" { libname = "libc.dylib" }  // En macOS la libc se llama distinto.
            self.lib = ffi_open(libname)                // Cargamos libc.
            self.getchar_sym = ffi_sym(self.lib, "getchar")  // getchar lee un byte de stdin.

            // stty pone el terminal en modo "raw":
            //   -icanon : sin buffer de linea (no espera Enter).
            //   -echo   : no muestra lo que el usuario teclea.
            //   min 0 time 0 : read no bloquea; si no hay datos retorna 0/-1.
            shell("stty -icanon -echo min 0 time 0")    // shell() es un builtin que ejecuta comandos.
        }
    }

    fn poll_key(self) {                                 // Devuelve un codigo de tecla, o -1 si no hay nada.
        if self.os == "windows" {
            let n = ffi_call(self.kbhit_sym)            // Llama a _kbhit() (sin args). Retorno = bool int.
            if n == 0 { return -1 }                     // Cola vacia: no hay tecla pendiente.
            let c = ffi_call(self.getch_sym)            // Hay tecla; la leemos. Retorna su codigo ASCII.

            // Las teclas especiales (flechas, F1..F12, Inicio, etc.) llegan en
            // dos pasos: un byte prefijo (0 o 224) seguido del codigo real.
            if c == 0 or c == 224 {
                let c2 = ffi_call(self.getch_sym)       // Segundo byte: el codigo real.
                if c2 == 75 { return -1001 }            // Codigo Windows para flecha izquierda.
                                                        // Devolvemos -1001 como "codigo sintetico" para
                                                        // distinguirlo de cualquier ASCII real.
                if c2 == 77 { return -1002 }            // Flecha derecha.
                return -2000                            // Otra tecla especial: la ignoramos.
            }
            return c                                    // Tecla normal: devolvemos su codigo ASCII.
        } else {
            let c = ffi_call(self.getchar_sym)          // En POSIX getchar() bloquea... salvo que stty
                                                        // este en modo raw con time 0, en cuyo caso
                                                        // retorna -1 (EOF) si no hay nada.
            if c == -1 { return -1 }                    // No hay tecla.

            // En POSIX las flechas llegan como secuencias ESC [ A/B/C/D.
            if c == 27 {                                // Si leimos ESC, miramos los siguientes dos bytes.
                let c1 = ffi_call(self.getchar_sym)
                if c1 != 91 { return 27 }               // No empezaba con '['; era ESC suelto.
                let c2 = ffi_call(self.getchar_sym)
                if c2 == 68 { return -1001 }            // 'D' = flecha izquierda.
                if c2 == 67 { return -1002 }            // 'C' = flecha derecha.
                return -2000
            }
            return c
        }
    }

    fn shutdown(self) {                                 // Limpia recursos al salir; importante para no
                                                        // dejar el terminal en modo raw.
        if self.os != "windows" {
            shell("stty icanon echo")                   // Restaura modo canonical y echo.
        }
        if self.lib != 0 { ffi_close(self.lib) }        // Libera la libreria cargada (dlclose / FreeLibrary).
    }
}


// ----------------------------------------------------------------------------
// SECCION 3: Constantes de teclas
// ----------------------------------------------------------------------------
// Centralizamos los codigos para no esparcir numeros magicos por el codigo.

let KEY_LEFT        = 97                                // 'a' en ASCII (97 = 0x61).
let KEY_RIGHT       = 100                               // 'd'.
let KEY_FIRE        = 32                                // Espacio.
let KEY_QUIT        = 113                               // 'q'.
let KEY_ESC         = 27                                // ESC suelto.
let KEY_ARROW_LEFT  = -1001                             // Codigo sintetico que devolvemos en poll_key.
let KEY_ARROW_RIGHT = -1002


// ----------------------------------------------------------------------------
// SECCION 4: Jerarquia de entidades del juego (POO con herencia)
// ----------------------------------------------------------------------------
// Aprovechamos las clases de VestaShell. Entity es la base; Player, Alien y
// Bullet heredan de ella. Esto permite tratar a todas como "entidades con
// posicion y glifo" y al mismo tiempo darles comportamiento especifico.

class Entity {                                          // Clase base abstracta (no se instancia directamente).
    "Entidad base."

    fn __init__(self, x, y, glyph) {                    // Constructor: posicion y caracter de dibujo.
        self.x = x; self.y = y                          // Coordenadas en celdas de la consola.
        self.glyph = glyph                              // String que se dibuja en (x, y).
        self.alive = true                               // Flag de vida; cuando es false, no se actualiza
                                                        // ni se renderiza.
    }
}

class Player : Entity {                                 // Player hereda de Entity (sintaxis "Hijo : Padre").
    "Nave del jugador."

    fn __init__(self, x, y) {
        self.x = x; self.y = y
        self.glyph = "/A\\"                             // 3 caracteres: '/', 'A', '\'. La barra invertida
                                                        // se escribe doble porque es escape.
        self.alive = true
        self.score = 0                                  // Puntuacion acumulada.
        self.lives = 3                                  // Vidas restantes; al llegar a 0 -> game over.
        self.cooldown = 0                               // Frames a esperar antes de poder volver a disparar.
                                                        // Sin cooldown podrias spamear espacio.
    }
}

class Alien : Entity {
    "Invasor enemigo."

    fn __init__(self, x, y, kind) {                     // 'kind' determina aspecto y puntos.
        self.x = x; self.y = y; self.alive = true
        self.kind = kind                                // 0 = fila superior (mas puntos), 2 = inferior.

        if kind == 0 {                                  // Aliens de la fila de arriba: dificiles de alcanzar.
            self.glyph = "><"; self.points = 30
        } elif kind == 1 {
            self.glyph = "oo"; self.points = 20
        } else {
            self.glyph = "WW"; self.points = 10         // Los mas faciles dan menos puntos.
        }
    }
}

class Bullet : Entity {
    "Proyectil."

    fn __init__(self, x, y, dy, from_player) {          // dy = direccion vertical (-1 sube, +1 baja).
        self.x = x; self.y = y; self.alive = true
        self.dy = dy
        self.from_player = from_player                  // True si la disparo el jugador, false si un alien.
                                                        // Determina contra quien colisiona.
        if from_player { self.glyph = "|" } else { self.glyph = "*" }
    }
}


// ----------------------------------------------------------------------------
// SECCION 5: Clase Game - estado y logica del juego
// ----------------------------------------------------------------------------
// Centraliza el estado mutable: jugador, lista de aliens, lista de balas,
// temporizadores y flags. Tambien implementa el ciclo step/render.

class Game {
    "Estado y logica del Space Invaders."

    fn __init__(self, w, h) {                           // w = ancho del campo en celdas, h = alto.
        self.w = w; self.h = h
        self.player = Player(int(w / 2), h - 2)         // Jugador empieza centrado horizontalmente,
                                                        // a una fila del borde inferior.
        self.aliens = []                                // Lista vacia; la rellena _spawn_aliens().
        self.bullets = []                               // Todas las balas activas (player + alien) en una
                                                        // unica lista; las distinguimos por b.from_player.
        self.tick = 0                                   // Contador de frames; util para algunas decisiones.
        self.alien_dx = 1                               // Direccion horizontal del enjambre: +1 derecha, -1 izquierda.

        // Logica basada en TIEMPO REAL (ms) en lugar de en frames. Esto hace
        // que la velocidad sea independiente del frame rate y de hipos en el
        // sleep. Mucho mas predecible que "cada N ticks".
        self.alien_move_ms = 600                        // Cada 600ms los aliens dan un paso.
        self.last_alien_move = time_ms()                // Marca de tiempo del ultimo paso. time_ms() devuelve
                                                        // ms desde algun epoch (no importa cual).
        self.last_alien_shot = time_ms()                // Ultimo disparo enemigo.
        self.alien_shot_ms = 900                        // Frecuencia de disparo enemigo: cada 900ms.

        self.running = true                             // Flag principal del bucle de main().
        self.message = ""                               // Mensaje final (victoria/derrota) para imprimir al salir.
        self._spawn_aliens()                            // Pinta la rejilla inicial.
    }

    fn _spawn_aliens(self) {                            // Crea la rejilla 8x4 de aliens.
        let cols = 8                                    // 8 columnas.
        let rows = 4                                    // 4 filas.
        let x0 = 4                                      // Margen izquierdo de partida.
        let y0 = 2                                      // Empezamos cerca del techo.
        let dx = 5                                      // Separacion horizontal entre aliens (en celdas).
        let dy = 2                                      // Separacion vertical.

        for r in range(0, rows) {                       // r = 0..3 (fila).
            for c in range(0, cols) {                   // c = 0..7 (columna).
                let kind = 2                            // Por defecto, fila inferior.
                if r == 0 { kind = 0                    // La fila superior tiene aliens "><".
                } elif r == 1 { kind = 1 }              // La segunda, "oo".
                let a = Alien(x0 + c * dx, y0 + r * dy, kind)  // Posicion calculada con offsets.
                append(self.aliens, a)                  // append es el builtin para anadir a una lista.
            }
        }
    }

    fn alive_aliens(self) {                             // Cuenta aliens vivos (para el HUD y para
                                                        // detectar victoria).
        let n = 0
        for a in self.aliens { if a.alive { n = n + 1 } }
        return n
    }

    fn handle_key(self, k) {                            // Procesa la tecla pulsada en este frame.
        if k == KEY_QUIT or k == KEY_ESC {              // 'q' o ESC -> salir.
            self.running = false
            self.message = "Saliste del juego"
            return                                      // Salimos del metodo aqui mismo.
        }
        if k == KEY_LEFT or k == KEY_ARROW_LEFT {       // 'a' o flecha izquierda.
            if self.player.x > 1 { self.player.x = self.player.x - 2 }
                                                        // Solo movemos si no estamos pegados al borde.
                                                        // Movemos en pasos de 2 celdas (sensacion de
                                                        // velocidad sin recorrer el borde tan despacio).
        }
        if k == KEY_RIGHT or k == KEY_ARROW_RIGHT {
            if self.player.x < self.w - 4 { self.player.x = self.player.x + 2 }
                                                        // self.w - 4 deja sitio para los 3 caracteres
                                                        // del glifo "/A\" + 1 de borde.
        }
        if k == KEY_FIRE {                              // Espacio: disparar.
            if self.player.cooldown <= 0 {              // Solo si el cooldown ha expirado.
                let b = Bullet(self.player.x + 1, self.player.y - 1, -1, true)
                                                        // Bala nace en el centro del jugador (x+1) y
                                                        // una fila por encima (y-1). dy=-1 sube. true
                                                        // = es del jugador.
                append(self.bullets, b)
                self.player.cooldown = 8                // Cooldown de 8 frames (~250ms a 30fps).
            }
        }
    }

    fn _step_bullets(self) {                            // Actualiza posicion de TODAS las balas.
        for b in self.bullets {
            if not b.alive { continue }                 // Saltamos las balas muertas (ya colisionaron
                                                        // o salieron). No las eliminamos de la lista
                                                        // por simplicidad; en un juego mayor convendria
                                                        // limpiar ocasionalmente.
            b.y = b.y + b.dy                            // Avanza segun su direccion vertical.
            if b.y < 1 or b.y >= self.h - 1 {           // Salio por arriba o por abajo.
                b.alive = false                         // Marcar como muerta para no dibujarla ni colisionar.
            }
        }
    }

    fn _step_aliens(self) {                             // Actualiza posicion del enjambre.
        // Mover por tiempo real (ms) en vez de por ticks: la velocidad real
        // sera la misma aunque el frame rate fluctue.
        let now = time_ms()
        if now - self.last_alien_move < self.alien_move_ms { return }
                                                        // No es tiempo aun; salimos sin tocar nada.
        self.last_alien_move = now                      // Marcamos que en este instante hicimos un paso.

        // Calculo de los limites del enjambre vivo: minimo X, maximo X+ancho,
        // maximo Y. Lo usamos para detectar colision con bordes y proximidad
        // al jugador.
        let min_x = 9999                                // Inicializamos a valores extremos.
        let max_x = -1
        let max_y = -1
        for a in self.aliens {
            if not a.alive { continue }
            if a.x < min_x { min_x = a.x }
            if a.x + 2 > max_x { max_x = a.x + 2 }      // +2 porque cada alien ocupa 2 caracteres.
            if a.y > max_y { max_y = a.y }
        }
        if max_x < 0 { return }                         // Si max_x sigue siendo -1, no hay aliens vivos.

        // Decidir si toca bajar fila: cuando el enjambre toque un borde y
        // este moviendose hacia ese borde.
        let drop = false
        if self.alien_dx > 0 and max_x >= self.w - 2 { drop = true }  // Yendo a la derecha y tocando muro derecho.
        if self.alien_dx < 0 and min_x <= 1          { drop = true }  // Yendo a la izquierda y tocando muro izquierdo.

        if drop {
            self.alien_dx = -self.alien_dx              // Invertir direccion horizontal.
            for a in self.aliens {
                if a.alive { a.y = a.y + 1 }            // Bajar una fila a TODOS los aliens vivos.
            }
            // Acelerar suavemente: -8% por bajada (multiplicar por 92/100).
            // Como VestaShell trabaja con ints en este caso, * 92 / 100 hace
            // la division entera correctamente sin floats.
            let new_ms = self.alien_move_ms * 92 / 100
            if new_ms < 200 { new_ms = 200 }            // Tope minimo: si pasamos de 200ms el juego se
                                                        // vuelve injugable. Esto da el "climax" sin
                                                        // imposibilidades.
            self.alien_move_ms = new_ms
        } else {
            for a in self.aliens {
                if a.alive { a.x = a.x + self.alien_dx }  // Movimiento lateral normal.
            }
        }

        // Si los aliens han llegado al jugador, fin del juego.
        if max_y >= self.player.y - 1 {
            self.running = false
            self.message = "Game over: los aliens te invadieron"
        }
    }

    fn _alien_bombs(self) {                             // Logica de disparos enemigos.
        let now = time_ms()
        if now - self.last_alien_shot < self.alien_shot_ms { return }
                                                        // Solo cada 'alien_shot_ms' milisegundos.
        self.last_alien_shot = now

        // Construimos lista de candidatos a disparar (aliens vivos).
        let candidates = []
        for a in self.aliens {
            if a.alive { append(candidates, a) }
        }
        if len(candidates) == 0 { return }              // No quedan aliens.

        // Pseudo-aleatorio: usamos el tiempo + tick como semilla mod
        // numero de candidatos. No es aleatorio fuerte, pero es suficiente
        // para que los disparos vengan de aliens distintos.
        let r = (now + self.tick) % len(candidates)
        let shooter = candidates[r]
        let b = Bullet(shooter.x + 1, shooter.y + 1, 1, false)
                                                        // Bala desde el centro del alien, hacia abajo
                                                        // (dy=+1), false = no es del jugador.
        append(self.bullets, b)
    }

    fn _check_collisions(self) {                        // Detecta impactos: bala-jugador y bala-alien.
        for b in self.bullets {
            if not b.alive { continue }

            if b.from_player {                          // Bala del jugador: comparar contra cada alien.
                for a in self.aliens {
                    if not a.alive { continue }
                    // Colision si bala y alien estan en la misma fila Y la X de
                    // la bala cae dentro de los 2 caracteres del alien.
                    if b.y == a.y and b.x >= a.x and b.x <= a.x + 1 {
                        a.alive = false                 // Alien muere.
                        b.alive = false                 // Bala consumida.
                        self.player.score = self.player.score + a.points  // Sumar puntos.
                    }
                }
            } else {                                    // Bala enemiga: comparar contra el jugador.
                let p = self.player
                if b.y == p.y and b.x >= p.x and b.x <= p.x + 2 {
                                                        // +2 porque el glifo del jugador ocupa 3 chars.
                    b.alive = false
                    p.lives = p.lives - 1
                    if p.lives <= 0 {
                        self.running = false
                        self.message = "Game over: te quedaste sin vidas"
                    }
                }
            }
        }
    }

    fn step(self, key) {                                // Avanza un frame de simulacion.
        if key != -1 { self.handle_key(key) }           // Si hay tecla, la procesamos.
        if not self.running { return }                  // Si el juego termino en el handle_key
                                                        // (p.ej. ESC), no seguimos.

        if self.player.cooldown > 0 {                   // Decrementa el cooldown del disparo.
            self.player.cooldown = self.player.cooldown - 1
        }

        // Orden importante:
        //   1. Mover balas.
        //   2. Mover aliens (puede llegar al jugador y terminar el juego).
        //   3. Disparos enemigos (nuevas balas).
        //   4. Detectar colisiones (sobre el estado ya actualizado).
        self._step_bullets()
        self._step_aliens()
        self._alien_bombs()
        self._check_collisions()

        if self.alive_aliens() == 0 {                   // Sin aliens vivos -> victoria.
            self.running = false
            self.message = "Victoria! Has derrotado a los invasores"
        }
        self.tick = self.tick + 1                       // Solo informativo y para el pseudo-aleatorio.
    }

    fn render(self) {                                   // Pinta el frame entero.
        // Tecnica: doble buffer simulado. Construimos la pantalla en un array
        // 2D de strings y al final la volcamos a la consola en una pasada.
        // Esto evita parpadeo (en lugar de "limpiar y dibujar" multiples veces).
        let buf = []
        for y in range(0, self.h) {                     // Para cada fila...
            let row = []
            for x in range(0, self.w) {                 // ...y cada columna...
                if y == 0 or y == self.h - 1 { append(row, "-")
                                                        // Bordes superior/inferior con guiones.
                } elif x == 0 or x == self.w - 1 { append(row, "|")
                                                        // Bordes laterales con barras verticales.
                } else { append(row, " ") }             // Interior: vacio.
            }
            append(buf, row)
        }

        // Dibujar aliens en el buffer. Cada alien ocupa 2 caracteres horizontales.
        for a in self.aliens {
            if not a.alive { continue }
            self._put(buf, a.x,     a.y, a.glyph[0])    // Primer caracter del glifo.
            self._put(buf, a.x + 1, a.y, a.glyph[1])    // Segundo. Indexado de strings devuelve string-de-1.
        }

        // Dibujar balas (1 caracter cada una).
        for b in self.bullets {
            if not b.alive { continue }
            self._put(buf, b.x, b.y, b.glyph)
        }

        // Dibujar al jugador (3 caracteres).
        let p = self.player
        if p.lives > 0 {
            self._put(buf, p.x,     p.y, p.glyph[0])    // '/'
            self._put(buf, p.x + 1, p.y, p.glyph[1])    // 'A'
            self._put(buf, p.x + 2, p.y, p.glyph[2])    // '\'
        }

        // Volcar el buffer a pantalla.
        print(ESC_HOME)                                 // Cursor al inicio (sin clear -> no parpadeo).

        print(ANSI["BOLD"] + ANSI["CYAN"])              // Empezar a imprimir en cian negrita.
        println("VESTA SPACE INVADERS  |  Score: " + str(p.score) + \
                "  Lives: " + str(p.lives) + \
                "  Aliens: " + str(self.alive_aliens()) + "   ")
                                                        // HUD con stats. Espacios extra al final para
                                                        // tapar restos del frame anterior.
        print(ANSI["RESET"])                            // Restaurar formato normal.

        for y in range(0, self.h) {                     // Imprimir cada fila del buffer.
            let line = ""
            for x in range(0, self.w) { line = line + buf[y][x] }
                                                        // Concatenamos toda la fila en un string.
            println(line + ESC_CLR_EOL)                 // Imprimir + limpiar resto de linea.
                                                        // El CLR_EOL borra cualquier caracter que
                                                        // hubiera quedado del frame previo si la
                                                        // linea actual es mas corta.
        }
        print(ANSI["DIM"])                              // Texto atenuado para la ayuda.
        println("[a/d] mover  [espacio] disparar  [q] salir" + ESC_CLR_EOL)
        print(ANSI["RESET"] + ESC_CLR_EOS)              // Reset + limpiar todo lo de debajo (por si
                                                        // un frame anterior fue mas alto).
    }

    fn _put(self, buf, x, y, ch) {                      // Helper: escribe un caracter en el buffer
                                                        // con check de limites.
        if y < 0 or y >= self.h { return }              // Fuera de pantalla por arriba/abajo: ignorar.
        if x < 0 or x >= self.w { return }              // Fuera por izquierda/derecha: ignorar.
        buf[y][x] = ch                                  // Acceso 2D: buf[fila][columna] = caracter.
    }
}


// ----------------------------------------------------------------------------
// SECCION 6: Main - punto de entrada y bucle principal
// ----------------------------------------------------------------------------

fn main() {
    print(ESC_CLEAR + ESC_CURSOR_HIDE)                  // UNA limpieza inicial + ocultar cursor.
                                                        // A partir de aqui solo sobreescribimos.

    let input = InputBackend()                          // Inicializa FFI / modo raw.
    let game = Game(50, 22)                             // Tablero de 50x22 celdas. Tamano elegido
                                                        // para caber comodo en una consola estandar.

    let frame_ms = 33                                   // 33ms por frame ~ 30 fps. Suficiente para
                                                        // que la accion se vea fluida sin saturar la
                                                        // CPU del intérprete.

    try {                                               // Try/catch para garantizar shutdown del
                                                        // backend incluso si hay error.
        while game.running {
            let t0 = time_ms()                          // Inicio del frame: lo medimos para hacer
                                                        // sleep variable al final.

            // Drenar TODAS las teclas pendientes en el buffer (hasta 8).
            // Si el usuario aporrea el teclado, no perdemos pulsaciones
            // y el ultimo input que llego es el que cuenta.
            let last_key = -1
            let drained = 0
            while drained < 8 {
                let k = input.poll_key()
                if k == -1 { break }                    // Cola vacia.
                last_key = k                            // Sobrescribimos para quedarnos con la ultima.
                drained = drained + 1
            }

            game.step(last_key)                         // Avanzar logica con la ultima tecla.
            game.render()                               // Pintar el frame.

            let elapsed = time_ms() - t0                // Tiempo gastado en step+render.
            if elapsed < frame_ms {
                sleep(frame_ms - elapsed)               // Dormir lo que reste del frame para
                                                        // mantener 30 fps constantes.
                                                        // Si elapsed >= frame_ms (frame lento),
                                                        // no dormimos: vamos lo mas rapido posible.
            }
        }
    } catch e {                                         // Capturamos cualquier excepcion para hacer
                                                        // limpieza ANTES de propagar.
        input.shutdown()                                // CRITICO: si saltamos sin esto en POSIX,
                                                        // el terminal se queda en modo raw y la
                                                        // shell no responde a Enter.
        print(ESC_CURSOR_SHOW + ANSI["RESET"])
        println("Error: " + str(e))
        return
    }

    // Camino normal de salida (game.running = false porque el usuario salio,
    // perdio o gano).
    input.shutdown()                                    // Restaurar terminal.
    print(ESC_CURSOR_SHOW + ANSI["RESET"])              // Mostrar cursor y resetear formato.
    println("")
    println(game.message)                               // "Victoria!", "Game over: ...", etc.
    println("Puntuacion final: " + str(game.player.score))
}

main()                                                  // Arranque. Como en Python, el script se
                                                        // ejecuta de arriba abajo y main() es solo
                                                        // una convencion para organizar el codigo.
