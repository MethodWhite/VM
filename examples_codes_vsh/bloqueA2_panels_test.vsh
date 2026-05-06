// =============================================================================
// vnano BLOQUE A2 - Panels interactivos con layouts (modulo de prueba)
// =============================================================================
// Pruebalo:
//   vesta --script bloqueA2_panels_test.vsh
//
// Atajos:
//   F7        rotar foco entre paneles
//   Ctrl+\    split vertical el panel activo
//   F8        split horizontal el panel activo
//   Alt+L     ciclar layout (rotacion entre disposiciones equivalentes)
//   Ctrl+W    cerrar panel activo
//   q         salir
//
// Cada panel muestra "Panel N" centrado y la cabecera indica si tiene foco.
// El objetivo es comprobar que la geometria, foco y splits funcionan bien
// antes de integrarlos con el editor real.
// =============================================================================


let ESC          = substr(ANSI["CLEAR_LINE"], 0, 1)
let ESC_HOME     = ESC + "[H"
let ESC_CUR_HIDE = ESC + "[?25l"
let ESC_CUR_SHOW = ESC + "[?25h"
let ESC_CLR_EOL  = ESC + "[K"

let VBAR = from_char(226) + from_char(148) + from_char(130)   // │
let HBAR = from_char(226) + from_char(148) + from_char(128)   // ─
let CROSS = from_char(226) + from_char(148) + from_char(188)  // ┼

let KEY_ENTER     = 13
let KEY_BACKSPACE = 8
let KEY_ESC       = 27
let KEY_CTRL_W    = 23
let KEY_UP    = -1000
let KEY_DOWN  = -1001
let KEY_LEFT  = -1002
let KEY_RIGHT = -1003
let KEY_F4    = -1013
let KEY_F5    = -1014
let KEY_F6    = -1015
let KEY_F7    = -1016
let KEY_F8    = -1017
let KEY_F9    = -1018


// -----------------------------------------------------------------------------
// InputBackend (subset)
// -----------------------------------------------------------------------------

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

    fn read_key(self) {
        if self.os == "windows" {
            while true {
                let n = ffi_call(self.kbhit_sym)
                if n != 0 { break }
                sleep(4)
            }
            let c = ffi_call(self.getch_sym)
            if c == 0 or c == 224 {
                let c2 = ffi_call(self.getch_sym)
                if c2 == 62 { return KEY_F4 }
                if c2 == 63 { return KEY_F5 }
                if c2 == 64 { return KEY_F6 }
                if c2 == 65 { return KEY_F7 }
                if c2 == 66 { return KEY_F8 }
                if c2 == 67 { return KEY_F9 }
                if c2 == 72 { return KEY_UP }
                if c2 == 80 { return KEY_DOWN }
                if c2 == 75 { return KEY_LEFT }
                if c2 == 77 { return KEY_RIGHT }
                return -2999
            }
            return c
        } else {
            while true {
                let c = ffi_call(self.getchar_sym)
                if c != -1 {
                    if c == 27 {
                        // posible Alt+letra: leer siguiente con timeout
                        let c2 = self._poll(15)
                        if c2 == -1 { return KEY_ESC }
                        if c2 == 91 {
                            // CSI: leer dos mas
                            let c3 = self._poll(15)
                            if c3 == 49 {
                                // F-keys en POSIX: ESC[15~ ESC[17~ ESC[18~ ESC[19~ ESC[20~
                                let c4 = self._poll(15)
                                let c5 = self._poll(15)
                                if c4 == 53 { return KEY_F5 }
                                if c4 == 55 { return KEY_F6 }
                                if c4 == 56 { return KEY_F7 }
                                if c4 == 57 { return KEY_F8 }
                            }
                            if c3 == 50 {
                                let c4 = self._poll(15)
                                let c5 = self._poll(15)
                                if c4 == 48 { return KEY_F9 }
                            }
                            if c3 == 65 { return KEY_UP }
                            if c3 == 66 { return KEY_DOWN }
                            if c3 == 67 { return KEY_RIGHT }
                            if c3 == 68 { return KEY_LEFT }
                            // F4 en POSIX a veces viene como ESC[OS o similar
                        }
                        return KEY_ESC
                    }
                    return c
                }
                sleep(4)
            }
            return -1
        }
    }

    fn _poll(self, ms) {
        let elapsed = 0
        while elapsed < ms {
            let c = ffi_call(self.getchar_sym)
            if c != -1 { return c }
            sleep(2)
            elapsed = elapsed + 2
        }
        return -1
    }

    fn shutdown(self) {
        if self.os != "windows" {
            shell("stty icanon echo ixon ixoff")
        }
        if self.lib != 0 { ffi_close(self.lib) }
    }
}


// -----------------------------------------------------------------------------
// TermSize
// -----------------------------------------------------------------------------

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


// -----------------------------------------------------------------------------
// LayoutManager: gestiona el layout y devuelve los rectangulos de cada panel
// -----------------------------------------------------------------------------

class LayoutManager {
    "Calcula rectangulos de paneles segun el layout activo."

    fn __init__(self) {
        self.layout = "single"
    }

    // Devuelve numero de paneles que requiere el layout actual
    fn n_panels_required(self) {
        if self.layout == "single" { return 1 }
        if self.layout == "split_v" or self.layout == "split_h" { return 2 }
        if self.layout == "three_left" or self.layout == "three_right" { return 3 }
        if self.layout == "grid" { return 4 }
        return 1
    }

    // Devuelve [{x,y,w,h}, ...] dentro del area dada
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
            append(rects, { "x": area_x, "y": area_y, "w": half_w, "h": half_h })
            append(rects, { "x": area_x + half_w + 1, "y": area_y, "w": other_w, "h": half_h })
            append(rects, { "x": area_x, "y": area_y + half_h + 1, "w": half_w, "h": other_h })
            append(rects, { "x": area_x + half_w + 1, "y": area_y + half_h + 1, "w": other_w, "h": other_h })
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
            // Anadir tercer panel: pasamos a three_right (P3 a la derecha entera)
            // o three_left si el split fue desde el izquierdo - simplificacion: three_right
            self.layout = "three_right"
            return true
        }
        if l == "split_h" {
            // Promover a 3 horizontal: ya son 2 horizontales -> mejor ir a 3
            // Pasamos a three_left con orientacion espejo. Simplificacion:
            self.layout = "three_left"
            return true
        }
        if l == "three_left" or l == "three_right" {
            self.layout = "grid"
            return true
        }
        // grid: ya esta lleno
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

    // Cycle entre layouts equivalentes para la misma cantidad de paneles
    fn cycle(self) {
        if self.layout == "split_v" { self.layout = "split_h"; return }
        if self.layout == "split_h" { self.layout = "split_v"; return }
        if self.layout == "three_left" { self.layout = "three_right"; return }
        if self.layout == "three_right" { self.layout = "three_left"; return }
        // single o grid: no hay alternativa
    }
}


// -----------------------------------------------------------------------------
// MockPanel: panel simplificado solo para testing
// -----------------------------------------------------------------------------
// Este Panel es una version de juguete: solo dibuja "Panel N" en un rect dado,
// con borde y header indicando si tiene foco.

class MockPanel {
    fn __init__(self, num) {
        self.num = num
        self.lines = []
        let i = 0
        while i < 50 {
            append(self.lines, "Linea " + str(i+1) + " del panel " + str(num))
            i = i + 1
        }
        self.scroll = 0
    }
}


// -----------------------------------------------------------------------------
// EditorMock: testbed con paneles
// -----------------------------------------------------------------------------

class EditorMock {
    fn __init__(self) {
        let ts = TermSize()
        self.term_w = ts.cols
        self.term_h = ts.rows
        self.layout_mgr = LayoutManager()
        self.panels = [MockPanel(1)]
        self.active_panel = 0
        self.status_msg = ""
    }

    fn _area(self) {
        // header(1) + statusbar(1) + ayuda(1)  = 3 filas reservadas
        return { "x": 0, "y": 1, "w": self.term_w, "h": self.term_h - 3 }
    }

    fn render(self) {
        print(ANSI["CLEAR"])
        print(ESC_HOME)
        print(ESC_CUR_HIDE)

        // Header
        print(ansi_cursor_pos(1, 1))
        print(ANSI["REVERSE"] + ANSI["BOLD"])
        let header = " Test Panels  Layout: " + self.layout_mgr.layout +"  Paneles: " + str(len(self.panels)) +"  Activo: " + str(self.active_panel + 1)
        print(self._pad(header, self.term_w))
        print(ANSI["RESET"])

        // Paneles
        let area = self._area()
        let rects = self.layout_mgr.rects(area["x"], area["y"], area["w"], area["h"])
        let n = self.layout_mgr.n_panels_required()
        let i = 0
        while i < n {
            if i < len(self.panels) {
                self._draw_panel(self.panels[i], rects[i], i == self.active_panel, i + 1)
            }
            i = i + 1
        }

        // Divisores
        self._draw_dividers(rects, area)

        // Ayuda
        let y = self.term_h - 1
        print(ansi_cursor_pos(y, 1))
        print(ESC_CLR_EOL)
        print(ansi_rgb(98, 114, 164))
        print("F4 split-V | F8 split-H | F7 rotar foco | F9 cycle layout | Ctrl+W cerrar | q salir")
        print(ANSI["RESET"])

        // Status bar
        print(ansi_cursor_pos(self.term_h, 1))
        print(ESC_CLR_EOL)
        print(ANSI["REVERSE"])
        print(self._pad(" " + self.status_msg, self.term_w))
        print(ANSI["RESET"])
    }

    fn _draw_panel(self, p, rect, focused, label) {
        let x = rect["x"]
        let y = rect["y"]
        let w = rect["w"]
        let h = rect["h"]

        // Header del panel
        let head = " Panel " + str(label)
        if focused {
            head = head + " [FOCO]"
        }
        head = self._pad(head, w)

        let head_color = ansi_rgb_bg(40, 42, 54) + ansi_rgb(248, 248, 242)
        if focused {
            head_color = ansi_rgb_bg(98, 114, 164) + ANSI["BOLD"] + ansi_rgb(255, 255, 255)
        }
        print(ansi_cursor_pos(y + 1, x + 1))
        print(head_color + head + ANSI["RESET"])

        // Contenido: lineas del panel hasta llenar h-1
        let content_h = h - 1
        let i = 0
        while i < content_h {
            print(ansi_cursor_pos(y + 2 + i, x + 1))
            let row_idx = p.scroll + i
            let line = ""
            if row_idx < len(p.lines) {
                line = p.lines[row_idx]
            } else {
                line = ""
            }
            let painted = self._pad(line, w)
            if focused {
                print(ansi_rgb(189, 147, 249) + painted + ANSI["RESET"])
            } else {
                print(ansi_rgb(98, 114, 164) + painted + ANSI["RESET"])
            }
            i = i + 1
        }
    }

    fn _draw_dividers(self, rects, area) {
        // Para cada celda del area, comprobamos si pertenece a algun rect.
        // Si no (es decir, es divisor), pintamos la barra correspondiente.
        let dim = ansi_rgb(68, 71, 90)
        let yy = 0
        while yy < area["h"] {
            let xx = 0
            while xx < area["w"] {
                let owner = self._owner_of(rects, area["x"] + xx, area["y"] + yy)
                if owner == -1 {
                    // Determinar tipo: si la fila completa (excluyendo otros divisores)
                    // es divisor, es un HBAR; si la columna lo es, VBAR.
                    let is_h = self._is_horizontal_divider(rects, area, area["y"] + yy)
                    let is_v = self._is_vertical_divider(rects, area, area["x"] + xx)
                    let ch = " "
                    if is_h and is_v {
                        ch = CROSS
                    } elif is_h {
                        ch = HBAR
                    } elif is_v {
                        ch = VBAR
                    }
                    print(ansi_cursor_pos(area["y"] + yy + 1, area["x"] + xx + 1))
                    print(dim + ch + ANSI["RESET"])
                }
                xx = xx + 1
            }
            yy = yy + 1
        }
    }

    fn _owner_of(self, rects, gx, gy) {
        let i = 0
        while i < len(rects) {
            let r = rects[i]
            if gx >= r["x"] and gx < r["x"] + r["w"] and gy >= r["y"] and gy < r["y"] + r["h"] {
                return i
            }
            i = i + 1
        }
        return -1
    }

    fn _is_horizontal_divider(self, rects, area, gy) {
        // Comprueba si la fila gy tiene al menos una celda dentro del area que
        // sea divisor (no perteneciente a ningun rect)
        // Aqui simplificamos: una fila es horizontal divider si existe algun
        // rect cuyo borde inferior sea gy-1 y otro rect debajo.
        let i = 0
        while i < len(rects) {
            let r = rects[i]
            if r["y"] + r["h"] == gy {
                // hay un rect arriba; comprobar que hay otro debajo o que es divisor cross
                let j = 0
                while j < len(rects) {
                    let r2 = rects[j]
                    if r2["y"] == gy + 1 {
                        // overlap horizontal entre r y r2
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

    fn _is_vertical_divider(self, rects, area, gx) {
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

    fn _pad(self, s, w) {
        let n = len(s)
        if n >= w { return substr(s, 0, w) }
        return s + repeat(" ", w - n)
    }

    fn split_active(self, direction) {
        let n_before = self.layout_mgr.n_panels_required()
        let ok = self.layout_mgr.split(direction)
        if not ok {
            self.status_msg = "Maximo de paneles alcanzado"
            return
        }
        let n_after = self.layout_mgr.n_panels_required()
        // Anadir paneles necesarios
        while len(self.panels) < n_after {
            append(self.panels, MockPanel(len(self.panels) + 1))
        }
        self.active_panel = n_after - 1
        self.status_msg = "Split " + direction + ": layout=" + self.layout_mgr.layout
    }

    fn close_active(self) {
        if len(self.panels) == 1 {
            self.status_msg = "No se puede cerrar el ultimo panel"
            return
        }
        // Quitar el panel activo
        let new_panels = []
        let i = 0
        while i < len(self.panels) {
            if i != self.active_panel { append(new_panels, self.panels[i]) }
            i = i + 1
        }
        self.panels = new_panels
        self.layout_mgr.shrink()
        if self.active_panel >= len(self.panels) {
            self.active_panel = len(self.panels) - 1
        }
        self.status_msg = "Cerrado. layout=" + self.layout_mgr.layout
    }

    fn rotate_focus(self) {
        self.active_panel = self.active_panel + 1
        if self.active_panel >= len(self.panels) { self.active_panel = 0 }
        self.status_msg = "Foco -> Panel " + str(self.active_panel + 1)
    }

    fn cycle_layout(self) {
        self.layout_mgr.cycle()
        self.status_msg = "Layout: " + self.layout_mgr.layout
    }
}


// -----------------------------------------------------------------------------
// MAIN
// -----------------------------------------------------------------------------

fn main() {
    let inp = InputBackend()
    let ed = EditorMock()
    ed.status_msg = "F4 split-V, F8 split-H, F7 rotar, F9 cycle, Ctrl+W cerrar, q salir"

    while true {
        ed.render()
        let k = inp.read_key()
        if k == 113 {   // q
            inp.shutdown()
            print(ANSI["CLEAR"] + ESC_CUR_SHOW + ANSI["RESET"])
            print(ansi_cursor_pos(1, 1))
            println("Salida limpia.")
            return
        }
        if k == KEY_F4 {
            ed.split_active("v")
            continue
        }
        if k == KEY_F8 {
            ed.split_active("h")
            continue
        }
        if k == KEY_F7 {
            ed.rotate_focus()
            continue
        }
        if k == KEY_F9 {
            ed.cycle_layout()
            continue
        }
        if k == KEY_CTRL_W {
            ed.close_active()
            continue
        }
        if k == KEY_UP {
            let p = ed.panels[ed.active_panel]
            if p.scroll > 0 { p.scroll = p.scroll - 1 }
            continue
        }
        if k == KEY_DOWN {
            let p = ed.panels[ed.active_panel]
            p.scroll = p.scroll + 1
            continue
        }
        // Otras teclas: mostrar codigo en status bar
        ed.status_msg = "Tecla codigo: " + str(k)
    }
    inp.shutdown()
}

main()
