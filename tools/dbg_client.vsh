// =============================================================================
// tools/dbg_client.vsh - Cliente VestaShell del debugger TCP de VestaVM
// =============================================================================
//
// Uso:
//   ./vm.exe --script tools/dbg_client.vsh [host] [port]
//
//   host  : default "127.0.0.1"
//   port  : default 9229
//
// Para arrancar el server primero (en otra terminal):
//   ./vm.exe --run programa.velb --debug-port=9229
//
// Comandos del REPL:
//   ps                         lista procesos
//   attach <pid>               adjuntarse a un proceso
//   detach <pid>               desadjuntar
//   b <addr> [pid]             set breakpoint en addr (0 = todos los pid)
//   bd <id>                    borrar breakpoint por id
//   bl                         lista breakpoints
//   c <pid>                    continuar ejecucion
//   s <pid>                    step (una instruccion)
//   n <pid>                    next (step-over)
//   p <pid>                    pause
//   r <pid>   |  regs <pid>    volcado de registros
//   mem <addr> <len> <pid>     dump de memoria VM (max 4096 bytes)
//   stack <pid>                pila de llamadas
//   info <pid>  |  i <pid>     info del proceso
//   eval <expr> <pid>          eval r0..r15 / rip / pc
//   help  |  ?                 muestra esta ayuda
//   q     |  quit              salir
//
// Eventos asincronos (break/exit/exception/spawned/stepped) se imprimen
// con prefijo "[event]" cuando llegan, sin bloquear el prompt.

// ---- argumentos del script ----
let host = "127.0.0.1"
let port = 9229
if len(ARGV) >= 2 { host = ARGV[1] }
if len(ARGV) >= 3 { port = int(ARGV[2]) }

// =============================================================================
// Color helpers (ANSI 256).  Todos los colores quedan VACIOS si esta presente
// la variable de entorno VESTA_DBG_NOCOLOR (cualquier valor) -- util para
// pipes/logs que no entienden escape codes.  Auto-deteccion futura mejorada
// puede usar isatty(stdout); por ahora env var es suficiente.
// =============================================================================
let USE_COLOR = (getenv("VESTA_DBG_NOCOLOR") == null)
let CSI = "\x1b["
fn fg(code) {
    if not USE_COLOR { return "" }
    return CSI + str(code) + "m"
}
let RESET = fg(0)
let BOLD  = fg(1)
let DIM   = fg(2)
let ITAL  = fg(3)
let UNDR  = fg(4)
// Foreground basicos
let RED      = fg(31)
let GREEN    = fg(32)
let YELLOW   = fg(33)
let BLUE     = fg(34)
let MAGENTA  = fg(35)
let CYAN     = fg(36)
let WHITE    = fg(37)
// Foreground brillantes (mejor contraste en terminales oscuros)
let BRED     = fg(91)
let BGREEN   = fg(92)
let BYELLOW  = fg(93)
let BBLUE    = fg(94)
let BMAGENTA = fg(95)
let BCYAN    = fg(96)
let BWHITE   = fg(97)
let GRAY     = fg(90)

// Banner / utilidades de presentacion
fn banner_line(ch, n) {
    let s = ""
    let i = 0
    while i < n { s = s + ch; i = i + 1 }
    return s
}
fn header(title) {
    println(BOLD + CYAN + "== " + title + " ==" + RESET)
}
fn pad_left(s, n) {
    let cur = s
    while bytes_len(cur) < n { cur = " " + cur }
    return cur
}
fn pad_right(s, n) {
    let cur = s
    while bytes_len(cur) < n { cur = cur + " " }
    return cur
}
// Devuelve numero como hex con prefijo 0x y N digitos lpad-cero
fn to_hex(v, width) {
    let n = v
    if n < 0 { n = 0 }
    let digits = "0123456789abcdef"
    let s = ""
    if n == 0 { s = "0" } else {
        while n > 0 {
            let d = n % 16
            s = bytes_substr(digits, d, 1) + s
            n = (n - d) / 16
        }
    }
    while bytes_len(s) < width { s = "0" + s }
    return "0x" + s
}
fn human_bytes(n) {
    if n < 1024 { return str(n) + " B" }
    if n < 1048576 {
        let k = n / 1024
        return str(k) + " KB"
    }
    let m = n / 1048576
    return str(m) + " MB"
}
// Helper: imprime "key" coloreado seguido del valor.
fn kv(k, v) {
    println("  " + DIM + pad_right(k, 20) + RESET + " " + v)
}
fn kv_num(k, n) {
    kv(k, BWHITE + str(n) + RESET)
}
fn kv_hex(k, n, w) {
    kv(k, BMAGENTA + to_hex(n, w) + RESET)
}
fn kv_bytes(k, n) {
    kv(k, BWHITE + str(n) + RESET + DIM + "  (" + human_bytes(n) + ")" + RESET)
}

println(BOLD + BCYAN + "VestaVM Debugger Client (VSH)" + RESET)
println(DIM + "Conectando a " + host + ":" + str(port) + "..." + RESET)

let sock = tcp_connect(host, port)

// ---- Helpers de framing -------------------------------------------------
// Lee EXACTAMENTE n bytes del socket, agregando lo que recibe parcialmente.
fn recv_exact(s, n) {
    let buf = ""
    while bytes_len(buf) < n {
        let need = n - bytes_len(buf)
        let chunk = socket_recv(s, need)
        if bytes_len(chunk) == 0 {
            return null
        }
        buf = bytes_concat(buf, chunk)
    }
    return buf
}

// Envia un payload JSON con framing length-prefix u32 LE.
fn send_msg(s, json_str) {
    let hdr = u32_le_pack(bytes_len(json_str))
    socket_send(s, bytes_concat(hdr, json_str))
}

// Lee un mensaje (frame length-prefix).  Devuelve string JSON o null si EOF.
fn recv_msg(s) {
    let hdr = recv_exact(s, 4)
    if hdr == null { return null }
    let n = u32_le_unpack(hdr)
    if n == 0 or n > 4194304 { return null }
    return recv_exact(s, n)
}

// ---- Handshake (servidor envia 8 bytes: magic 'VBDG' + version u32 LE) -----
let hs = recv_exact(sock, 8)
if hs == null {
    println("Error: handshake fallo (server no envio bytes)")
    socket_close(sock)
    exit(1)
}
println(GREEN + "Conectado." + RESET + DIM + " Handshake (8 bytes): OK" + RESET)

// ---- Estado global del cliente ------------------------------------------
let next_seq = 1

// PID por defecto: actualizado por `attach <pid>` y por cmd_ps al primer
// proceso si solo hay uno.  Cualquier comando puede omitir el pid; si
// g_default_pid != null lo usamos.
let g_default_pid = null

// Lista de comandos a auto-imprimir tras cada break/step/watch/mon_block.
// Cada entrada: { id, cmd: "regs", args: [], desc: "regs" }.  El usuario los
// agrega con `display <cmd...>` y los borra con `undisplay <id>`.
let g_displays = []
let g_next_display_id = 1

// Flag puesto a true cuando rpc() observa un evento que indica que el proceso
// se detuvo (break/stepped/watch/mon_block).  El REPL lo lee tras cada
// comando, dispara render de displays, y lo limpia.
let g_pending_render = false

// Ultimo input no vacio (para que Enter repita el comando anterior).
let g_last_input = ""

// Comunicacion sync: envia un comando y devuelve el JSON parseado de la
// respuesta correlacionada.  Cualquier evento que llegue intercalado se
// imprime con prefijo "[event]" antes de devolver la respuesta.
fn rpc(cmd_obj) {
    let seq = next_seq
    next_seq = next_seq + 1
    cmd_obj["seq"] = seq
    let payload = json_stringify(cmd_obj)
    if getenv("VESTA_DBG_TRACE") != null {
        println("[trace SEND] " + payload)
    }
    send_msg(sock, payload)
    while true {
        let raw = recv_msg(sock)
        if raw == null {
            println("Error: conexion cerrada por el servidor")
            return null
        }
        let m = json_parse(raw)
        // Eventos asincronos: tienen "event", no "ok".  Se imprimen
        // coloreados segun severidad (break=cyan, exit=yellow,
        // exception=red, spawned=magenta, stepped=dim).
        if contains(m, "event") {
            let ev = m["event"]
            let col = BCYAN
            if ev == "exit"       { col = BYELLOW }
            if ev == "exception"  { col = BRED }
            if ev == "spawned"    { col = BMAGENTA }
            if ev == "stepped"    { col = GRAY }
            if ev == "watch"      { col = BYELLOW }
            if ev == "mon_block"  { col = BRED }
            if ev == "msg_trace"  { col = GRAY }
            print(BOLD + col + "[" + ev + "]" + RESET)
            if contains(m, "pid")    { print(DIM + " pid=" + RESET + str(m["pid"])) }
            if contains(m, "pc")     { print(DIM + " pc=" + RESET + BMAGENTA + to_hex(m["pc"], 8) + RESET) }
            if contains(m, "bp_id")  { print(DIM + " bp=" + RESET + BCYAN + str(m["bp_id"]) + RESET) }
            if contains(m, "exc")    { print(DIM + " exc=" + RESET + RED + str(m["exc"]) + RESET) }
            if contains(m, "class")  { print(DIM + " class=" + RESET + RED + str(m["class"]) + RESET) }
            if contains(m, "wp_id")  { print(DIM + " wp=" + RESET + BCYAN + str(m["wp_id"]) + RESET) }
            if contains(m, "handle") { print(DIM + " h=" + RESET + BCYAN + str(m["handle"]) + RESET) }
            if contains(m, "owner")  { print(DIM + " owner=" + RESET + str(m["owner"])) }
            if contains(m, "reason") { print(DIM + " reason=" + RESET + YELLOW + str(m["reason"]) + RESET) }
            if contains(m, "dir")    { print(DIM + " dir=" + RESET + str(m["dir"])) }
            if contains(m, "peer")   { print(DIM + " peer=" + RESET + str(m["peer"])) }
            if contains(m, "value")  { print(DIM + " val=" + RESET + str(m["value"])) }
            println("")
            // Eventos que indican que el proceso se detuvo: marcar para
            // re-render de displays tras esta llamada rpc().
            if ev == "break" or ev == "stepped" or ev == "watch" or ev == "mon_block" or ev == "exception" {
                g_pending_render = true
            }
            continue
        }
        // Respuesta correlacionada.
        if contains(m, "seq") and m["seq"] == seq {
            return m
        }
        // Respuesta con seq distinto: lo descartamos imprimiendolo (raro,
        // ocurre si el cliente envia varios comandos seguidos sin esperar).
        println("[warn] respuesta con seq inesperado: " + raw)
    }
    return null
}

// Helper: imprime el "data" del response o el "error".  Solo lo usamos
// como FALLBACK -- los comandos comunes tienen formatters dedicados que
// presentan la info en tablas/colores.  print_resp imprime el JSON crudo
// con indent (debug fallback).
fn print_resp(resp) {
    if resp == null { return }
    if contains(resp, "ok") and resp["ok"] == true {
        if contains(resp, "data") {
            println(DIM + json_stringify(resp["data"], 2) + RESET)
        } else {
            println(GREEN + "OK" + RESET)
        }
    } else {
        let msg = "(error desconocido)"
        if contains(resp, "error") { msg = resp["error"] }
        println(BRED + "ERROR" + RESET + ": " + msg)
    }
}

// Errores con color, sin imprimir el data crudo.  Usado por formatters
// dedicados que llaman a check_err(resp) y solo imprimen pretty si OK.
fn check_err(resp) {
    if resp == null { return false }
    if contains(resp, "ok") and resp["ok"] == true { return true }
    let msg = "(error desconocido)"
    if contains(resp, "error") { msg = resp["error"] }
    println(BRED + "ERROR" + RESET + ": " + msg)
    return false
}
// Devuelve resp["data"] o null si error.
fn data_of(resp) {
    if not check_err(resp) { return null }
    if not contains(resp, "data") { return null }
    return resp["data"]
}

// ---- Helpers de comandos ------------------------------------------------
// Mapeo state -> color (RUNNING=verde brillante, EXECUTE=cian, HALT=gris,
// otros estados=amarillo).
fn state_color(st) {
    if st == "RUNNING" or st == "EXECUTE" { return BGREEN }
    if st == "READY" or st == "DECODE"    { return BCYAN }
    if st == "HALT"  or st == "DEAD"      { return GRAY }
    if st == "WAIT_IO" or st == "BLOCKED" { return BYELLOW }
    return YELLOW
}

fn cmd_ps() {
    let d = data_of(rpc({"cmd":"list_procs"}))
    if d == null { return }
    header("Procesos")
    if len(d) == 0 {
        println(DIM + "  (sin procesos)" + RESET)
        return
    }
    let hdr = BOLD + "  " + pad_right("PID", 6) + pad_right("SCH", 4)
    hdr = hdr + pad_right("STATE", 10) + pad_right("PC", 14)
    hdr = hdr + pad_right("REDUC", 12) + RESET
    println(hdr)
    let i = 0
    while i < len(d) {
        let p = d[i]
        let pid_s = pad_right(str(p["pid"]), 6)
        let sch_s = pad_right(str(p["sched_id"]), 4)
        let st = p["state"]
        let st_col_len = bytes_len(state_color(st)) + bytes_len(RESET)
        let st_s = pad_right(state_color(st) + st + RESET, 10 + st_col_len)
        let pc_col_len = bytes_len(BMAGENTA) + bytes_len(RESET)
        let pc_s = pad_right(BMAGENTA + to_hex(p["pc"], 8) + RESET, 14 + pc_col_len)
        let red_s = str(p["reductions"])
        println("  " + pid_s + sch_s + st_s + pc_s + red_s)
        i = i + 1
    }
}
fn cmd_attach(pid) {
    let d = data_of(rpc({"cmd":"attach","pid":pid}))
    if d == null { return }
    println(GREEN + "[attach]" + RESET + " pid=" + str(d["pid"]))
}
fn cmd_detach(pid) {
    let d = data_of(rpc({"cmd":"detach","pid":pid}))
    if d == null { return }
    println(YELLOW + "[detach]" + RESET + " pid=" + str(pid))
}
fn cmd_break(addr, pid) {
    let m = {"cmd":"set_break","addr":addr}
    if pid != null { m["pid"] = pid }
    let d = data_of(rpc(m))
    if d == null { return }
    let s = GREEN + "[bp]" + RESET + " id=" + BCYAN + str(d["id"]) + RESET
    s = s + " addr=" + BMAGENTA + to_hex(d["addr"], 8) + RESET
    println(s)
}
fn cmd_break_del(id) {
    let d = data_of(rpc({"cmd":"del_break","id":id}))
    if d == null { return }
    println(YELLOW + "[bp deleted]" + RESET + " id=" + str(id))
}
fn cmd_break_list() {
    let d = data_of(rpc({"cmd":"list_breaks"}))
    if d == null { return }
    header("Breakpoints")
    if len(d) == 0 {
        println(DIM + "  (sin breakpoints)" + RESET)
        return
    }
    let hdr = BOLD + "  " + pad_right("ID", 4) + pad_right("ADDR", 14)
    hdr = hdr + pad_right("PID", 6) + pad_right("EN", 4) + "HITS" + RESET
    println(hdr)
    let i = 0
    while i < len(d) {
        let bp = d[i]
        let id_w = 4 + bytes_len(BCYAN) + bytes_len(RESET)
        let id_s = pad_right(BCYAN + str(bp["id"]) + RESET, id_w)
        let addr_w = 14 + bytes_len(BMAGENTA) + bytes_len(RESET)
        let addr_s = pad_right(BMAGENTA + to_hex(bp["addr"], 8) + RESET, addr_w)
        let pid_s = pad_right(str(bp["pid"]), 6)
        let en = ""
        if bp["enabled"] { en = GREEN + "on" + RESET } else { en = DIM + "off" + RESET }
        let en_s = pad_right(en, 4 + bytes_len(GREEN) + bytes_len(RESET))
        let hits = str(bp["hits"])
        println("  " + id_s + addr_s + pid_s + en_s + hits)
        i = i + 1
    }
}
fn cmd_continue(pid) {
    let d = data_of(rpc({"cmd":"continue","pid":pid}))
    if d == null { return }
    println(GREEN + "[continue]" + RESET + " pid=" + str(pid))
}
fn cmd_step(pid) {
    let d = data_of(rpc({"cmd":"step","pid":pid}))
    if d == null { return }
    println(BCYAN + "[step]" + RESET + " pid=" + str(pid))
}
fn cmd_next(pid) {
    let d = data_of(rpc({"cmd":"next","pid":pid}))
    if d == null { return }
    println(BCYAN + "[next]" + RESET + " pid=" + str(pid))
}
fn cmd_pause(pid) {
    let d = data_of(rpc({"cmd":"pause","pid":pid}))
    if d == null { return }
    println(YELLOW + "[pause]" + RESET + " pid=" + str(pid))
}

// regs: 16 GP regs en grid 4x4 con hex + dec.  Cada celda muestra:
// "rNN: 0xHHHHHHHHHHHHHHHH (dec)" alineada para tabular.
fn cmd_regs(pid) {
    let d = data_of(rpc({"cmd":"registers","pid":pid}))
    if d == null { return }
    header("Registers (pid=" + str(pid) + ")")
    let i = 0
    while i < 16 {
        let key = "r" + str(i)
        let val = d[key]
        let rn = pad_left(str(i), 2)
        let hx = to_hex(val, 16)
        let line = DIM + "r" + rn + RESET + " " + BMAGENTA + hx + RESET
        line = line + DIM + " " + str(val) + RESET
        // 2 columnas por fila (i par -> izq, impar -> derecha).
        if i % 2 == 0 { print("  " + pad_right(line, 60)) } else { println(pad_right(line, 60)) }
        i = i + 1
    }
    println("")
    kv("pc",    BMAGENTA + to_hex(d["pc"], 16) + RESET + DIM + " (" + str(d["pc"]) + ")" + RESET)
    kv("sp",    BMAGENTA + to_hex(d["sp"], 16) + RESET)
    kv("bp",    BMAGENTA + to_hex(d["bp"], 16) + RESET)
    kv("flags", BYELLOW  + to_hex(d["flags"], 4)  + RESET)
}

// Helper: badge de bandera (verde si set, gris si unset).
fn flag_badge(name, v) {
    if v { return BGREEN + " " + name + " " + RESET }
    return GRAY + " " + name + " " + RESET
}

// flags: badges de cada bandera coloreadas (verde=set, gris=unset).
fn cmd_flags(pid) {
    let d = data_of(rpc({"cmd":"flags","pid":pid}))
    if d == null { return }
    header("RFlags (pid=" + str(pid) + ")")
    let raw = d["raw"]
    println("  " + DIM + "raw" + RESET + " = " + BYELLOW + to_hex(raw, 4) + RESET)
    let line = "  "
    line = line + flag_badge("CF", d["cf"])
    line = line + flag_badge("OF", d["of"])
    line = line + flag_badge("SF", d["sf"])
    line = line + flag_badge("ZF", d["zf"])
    line = line + flag_badge("DM", d["dm"])
    println(line)
    let leg = "  " + DIM + "leyenda: " + RESET + BGREEN + " SET " + RESET
    leg = leg + DIM + " / " + RESET + GRAY + " UNSET " + RESET
    println(leg)
}

// fregs: 16 ZMM/f64 regs.  Solo mostramos los que NO son cero por
// defecto (ahorra ruido), pero mostramos todos si hay alguno !=0.
fn cmd_fregs(pid) {
    let d = data_of(rpc({"cmd":"fregs","pid":pid}))
    if d == null { return }
    header("Float regs (pid=" + str(pid) + ")")
    // Comprobar si todos son 0; en ese caso solo mostramos un header.
    let any_nonzero = false
    let i = 0
    while i < 16 {
        if d["f" + str(i) + "_bits"] != 0 { any_nonzero = true }
        i = i + 1
    }
    if not any_nonzero {
        println("  " + DIM + "(todos los registros f0..f15 = 0.0)" + RESET)
        return
    }
    i = 0
    while i < 16 {
        let bits = d["f" + str(i) + "_bits"]
        let f64s = d["f" + str(i) + "_f64"]
        let rn = pad_left(str(i), 2)
        let line = DIM + "f" + rn + RESET + " " + BMAGENTA
        line = line + to_hex(bits, 16) + RESET + DIM + " (f64=" + RESET
        line = line + BWHITE + f64s + RESET + DIM + ")" + RESET
        println("  " + line)
        i = i + 1
    }
}

// gc_stats: bloques agrupados con humanizacion de bytes
fn cmd_gc_stats(pid) {
    let d = data_of(rpc({"cmd":"gc_stats","pid":pid}))
    if d == null { return }
    header("GC stats (pid=" + str(pid) + ")")
    println(BOLD + "  Heap actual:" + RESET)
    kv_num("  live_handles",      d["live_handles"])
    kv_num("  handle_slots",      d["handle_slots"])
    kv_bytes("  nursery_used",    d["nursery_used"])
    kv_bytes("  nursery_total",   d["nursery_total"])
    kv_bytes("  old_reserved",    d["old_reserved_bytes"])
    kv_bytes("  old_freelist",    d["old_freelist_bytes"])
    println(BOLD + "  Acumulados desde inicio:" + RESET)
    kv_num("  alloc_count",       d["alloc_count"])
    kv_bytes("  alloc_bytes",     d["alloc_bytes"])
    kv_num("  freed_count",       d["freed_count"])
    kv_bytes("  freed_bytes",     d["freed_bytes"])
    kv_num("  promoted_count",    d["promoted_count"])
    kv_bytes("  promoted_bytes",  d["promoted_bytes"])
    kv_num("  minor_gc_count",    d["minor_gc_count"])
    kv_num("  major_gc_count",    d["major_gc_count"])
    kv_bytes("  peak_nursery",    d["peak_nursery"])
    kv_bytes("  peak_old",        d["peak_old"])
}

// gc_handles: tabla columnar con clase coloreada por generacion.
fn cmd_gc_handles(pid, lim) {
    let d = data_of(rpc({"cmd":"gc_handles","pid":pid,"limit":lim}))
    if d == null { return }
    header("GC handles vivos (pid=" + str(pid) + ")")
    if len(d) == 0 {
        println(DIM + "  (sin handles vivos)" + RESET)
        return
    }
    let hdr = BOLD + "  " + pad_right("H", 6) + pad_right("SIZE", 8)
    hdr = hdr + pad_right("GEN", 8) + pad_right("ADDR", 18) + "CLASS" + RESET
    println(hdr)
    let i = 0
    while i < len(d) {
        let h = d[i]
        let h_w = 6 + bytes_len(BCYAN) + bytes_len(RESET)
        let h_s = pad_right(BCYAN + str(h["h"]) + RESET, h_w)
        let sz_s = pad_right(str(h["size"]), 8)
        let gen = h["gen"]
        let gen_col = GREEN
        if gen == "old" { gen_col = BMAGENTA }
        let gen_w = 8 + bytes_len(gen_col) + bytes_len(RESET)
        let gen_s = pad_right(gen_col + gen + RESET, gen_w)
        let addr_w = 18 + bytes_len(DIM) + bytes_len(RESET)
        let addr_s = pad_right(DIM + to_hex(h["addr"], 12) + RESET, addr_w)
        let cls = h["class"]
        if cls == "?" { cls = DIM + "?" + RESET } else { cls = BWHITE + cls + RESET }
        println("  " + h_s + sz_s + gen_s + addr_s + cls)
        i = i + 1
    }
    println(DIM + "  Total mostrados: " + str(len(d)) + RESET)
}

// gc_inspect: header del objeto + hex dump tipo `xxd` (16 bytes/linea).
fn cmd_gc_inspect(pid, h) {
    let d = data_of(rpc({"cmd":"gc_inspect","pid":pid,"h":h}))
    if d == null { return }
    header("GC inspect h=" + str(h) + " (pid=" + str(pid) + ")")
    if contains(d, "class") {
        kv("class",      BWHITE + d["class"] + RESET)
        let sz_v = BWHITE + str(d["size"]) + RESET + DIM
        kv("size", sz_v + " (" + human_bytes(d["size"]) + ")" + RESET)
        kv("gen",        d["gen"])
        kv_hex("addr",   d["addr"], 12)
        kv("flags",      to_hex(d["flags"], 8))
        kv("hash_code",  to_hex(d["hash_code"], 8))
        kv_num("owner_pid", d["owner_pid"])
        kv_num("lock_depth", d["lock_depth"])
    }
    if contains(d, "bytes") {
        let bs = d["bytes"]
        let nb = len(bs)
        if nb > 0 {
            println(BOLD + "  Hex dump:" + RESET)
        }
        let i = 0
        while i < nb {
            let row_off = pad_left(to_hex(i, 4), 6)
            print(DIM + row_off + RESET + "  ")
            // 16 bytes hex
            let j = 0
            while j < 16 {
                if i + j < nb {
                    let bv = bs[i + j]
                    let hx = to_hex(bv, 2)
                    // Quitar el "0x" del to_hex, quedan 2 chars hex
                    hx = bytes_substr(hx, 2, 2)
                    print(hx + " ")
                } else {
                    print("   ")
                }
                if j == 7 { print(" ") }
                j = j + 1
            }
            // ASCII printable
            print(" " + DIM + "|" + RESET)
            j = 0
            while j < 16 {
                if i + j < nb {
                    let bv = bs[i + j]
                    // ASCII visible -> '.', no visible -> ' ' (sin to_char
                    // nativo en VSH para hacer mejor el dump tipo xxd).
                    if bv >= 32 and bv < 127 { print(".") } else { print(" ") }
                }
                j = j + 1
            }
            println(DIM + "|" + RESET)
            i = i + 16
        }
        if d["truncated"] {
            println(YELLOW + "  ...(truncado a 256 bytes)" + RESET)
        }
    }
}

// dump_stack: tabla con offset, addr, value (hex + dec).
fn cmd_dump_stack(pid, n) {
    let d = data_of(rpc({"cmd":"dump_stack","pid":pid,"n":n}))
    if d == null { return }
    header("Stack dump (pid=" + str(pid) + ")")
    kv_hex("rsp", d["rsp"], 8)
    kv_hex("rbp", d["rbp"], 8)
    let qw = d["qwords"]
    if len(qw) == 0 { return }
    let hdr = BOLD + "  " + pad_right("OFF", 6) + pad_right("ADDR", 14)
    hdr = hdr + pad_right("HEX", 22) + "DEC" + RESET
    println(hdr)
    let i = 0
    while i < len(qw) {
        let q = qw[i]
        let off_s = pad_right("+" + str(q["off"]), 6)
        let addr_w = 14 + bytes_len(DIM) + bytes_len(RESET)
        let addr_s = pad_right(DIM + to_hex(q["addr"], 8) + RESET, addr_w)
        let hex_w = 22 + bytes_len(BMAGENTA) + bytes_len(RESET)
        let hex_s = pad_right(BMAGENTA + to_hex(q["v"], 16) + RESET, hex_w)
        // Indicar si la posicion es rbp con marker
        let marker = ""
        if q["addr"] == d["rbp"] { marker = BCYAN + "<- rbp" + RESET }
        println("  " + off_s + addr_s + hex_s + str(q["v"]) + " " + marker)
        i = i + 1
    }
}

// frame_info: presenta saved_rbp, ret_addr, etc en bloques.
fn cmd_frame_info(pid) {
    let d = data_of(rpc({"cmd":"frame_info","pid":pid}))
    if d == null { return }
    header("Frame info (pid=" + str(pid) + ")")
    kv_hex("rsp",        d["rsp"], 8)
    kv_hex("rbp",        d["rbp"], 8)
    kv_hex("saved_rbp",  d["saved_rbp"], 8)
    kv_hex("ret_addr",   d["ret_addr"], 8)
    kv_num("frame_size", d["frame_size"])
}

// backtrace: stack trace con depth, method.name, file:line resolved.
fn cmd_backtrace(pid) {
    let d = data_of(rpc({"cmd":"backtrace","pid":pid}))
    if d == null { return }
    header("Backtrace (pid=" + str(pid) + ")")
    let frames = d["frames"]
    if len(frames) == 0 {
        println(DIM + "  (sin frames)" + RESET)
        return
    }
    let i = 0
    while i < len(frames) {
        let f = frames[i]
        let depth_s = "#" + pad_right(str(f["depth"]), 3)
        let pc = 0
        if contains(f, "pc") { pc = f["pc"] } elif contains(f, "return_pc") { pc = f["return_pc"] }
        let pc_s = BMAGENTA + to_hex(pc, 8) + RESET
        let mn = "?"
        if contains(f, "method") and f["method"] != null {
            mn = f["method"]
        }
        let mn_s = BCYAN + mn + RESET
        let src = ""
        if contains(f, "file") and f["file"] != null {
            // Mostrar solo el basename si el path es largo.
            let fp = f["file"]
            let last_sep = -1
            let fi = 0
            let fn_len = bytes_len(fp)
            while fi < fn_len {
                if fp[fi] == "/" or fp[fi] == "\\" { last_sep = fi }
                fi = fi + 1
            }
            let bn = fp
            if last_sep >= 0 {
                bn = bytes_substr(fp, last_sep + 1, fn_len - last_sep - 1)
            }
            src = " " + DIM + "at" + RESET + " " + GREEN + bn + RESET
            if contains(f, "line") and f["line"] > 0 {
                src = src + DIM + ":" + RESET + BYELLOW + str(f["line"]) + RESET
            }
        }
        println("  " + DIM + depth_s + RESET + " " + pc_s + " " + mn_s + src)
        i = i + 1
    }
}

// Source-aware: bp por (file, line).  Requiere que el .velb se haya
// compilado con --vex-debug para incluir la seccion DVBG.
fn cmd_break_src(file_path, line_no, pid) {
    let m = {"cmd":"set_break_src","file":file_path,"line":line_no}
    if pid != null { m["pid"] = pid }
    let d = data_of(rpc(m))
    if d == null { return }
    let s = GREEN + "[bp]" + RESET + " id=" + BCYAN + str(d["id"]) + RESET
    s = s + " addr=" + BMAGENTA + to_hex(d["addr"], 8) + RESET
    s = s + " " + GREEN + d["file"] + RESET
    s = s + DIM + ":" + RESET + BYELLOW + str(d["line"]) + RESET
    println(s)
}
fn cmd_info_source(pid) {
    let d = data_of(rpc({"cmd":"info_source","pid":pid}))
    if d == null { return }
    if contains(d, "file") and d["file"] != null {
        // Mostrar solo basename
        let fp = d["file"]
        let last_sep = -1
        let fi = 0
        let fn_len = bytes_len(fp)
        while fi < fn_len {
            if fp[fi] == "/" or fp[fi] == "\\" { last_sep = fi }
            fi = fi + 1
        }
        let bn = fp
        if last_sep >= 0 {
            bn = bytes_substr(fp, last_sep + 1, fn_len - last_sep - 1)
        }
        let s = "  pc=" + BMAGENTA + to_hex(d["pc"], 8) + RESET
        s = s + "  " + GREEN + bn + RESET
        s = s + DIM + ":" + RESET + BYELLOW + str(d["line"]) + RESET
        s = s + DIM + "  (" + fp + ")" + RESET
        println(s)
    } else {
        let s = DIM + "  pc=" + to_hex(d["pc"], 8)
        s = s + " (sin info de debug; recompila con --vex-debug)" + RESET
        println(s)
    }
}

// info_proc con colores
fn cmd_info(pid) {
    let d = data_of(rpc({"cmd":"info_proc","pid":pid}))
    if d == null { return }
    header("Process info (pid=" + str(pid) + ")")
    kv_num("pid",         d["pid"])
    kv_num("sched_id",    d["sched_id"])
    kv("state",        state_color(d["state"]) + d["state"] + RESET)
    kv_hex("pc",          d["pc"], 8)
    kv_num("reductions",  d["reductions"])
    let err = d["err_thread"]
    let err_s = str(err)
    if err != 0 { err_s = BRED + err_s + RESET } else { err_s = GREEN + err_s + RESET }
    kv("err_thread",   err_s)
    kv_num("tsc",         d["tsc"])
}

// eval con resaltado del valor leido
fn cmd_eval(expr, pid) {
    let d = data_of(rpc({"cmd":"eval","expr":expr,"pid":pid}))
    if d == null { return }
    let s = "  " + DIM + d["name"] + RESET + " = "
    s = s + BMAGENTA + to_hex(d["value"], 16) + RESET
    s = s + DIM + " (" + str(d["value"]) + ")" + RESET
    println(s)
}

// memory: hex dump tipo xxd (similar a gc_inspect pero sin header).
fn cmd_mem(addr, len_b, pid) {
    let d = data_of(rpc({"cmd":"memory","addr":addr,"len":len_b,"pid":pid}))
    if d == null { return }
    header("Memoria @" + to_hex(addr, 8) + " +" + str(len_b) + " bytes")
    let bs = d
    let nb = len(bs)
    let i = 0
    while i < nb {
        let row_addr = addr + i
        print(DIM + to_hex(row_addr, 8) + RESET + "  ")
        let j = 0
        while j < 16 {
            if i + j < nb {
                let bv = bs[i + j]
                let hx = bytes_substr(to_hex(bv, 2), 2, 2)
                print(hx + " ")
            } else {
                print("   ")
            }
            if j == 7 { print(" ") }
            j = j + 1
        }
        println("")
        i = i + 16
    }
}

// stack: trace estilo simple (sin resolved methods, fallback)
fn cmd_stack(pid) {
    let d = data_of(rpc({"cmd":"stack","pid":pid}))
    if d == null { return }
    header("Stack frames (pid=" + str(pid) + ")")
    let frames = d["frames"]
    let i = 0
    while i < len(frames) {
        let f = frames[i]
        let pc = 0
        if contains(f, "pc") { pc = f["pc"] } elif contains(f, "return_pc") { pc = f["return_pc"] }
        let prefix = "  "
        if contains(f, "is_top") and f["is_top"] {
            prefix = BCYAN + "->" + RESET
        }
        println(prefix + " #" + str(i) + " pc=" + BMAGENTA + to_hex(pc, 8) + RESET)
        i = i + 1
    }
}

// Strip de comillas leading/trailing si las hubiere.
fn unquote_path(p) {
    let n = bytes_len(p)
    if n >= 2 {
        let c0 = p[0]
        let cn = p[n - 1]
        if (c0 == "\"" and cn == "\"") or (c0 == "'" and cn == "'") {
            return bytes_substr(p, 1, n - 2)
        }
    }
    return p
}

// list <file>:<line> [N] -- muestra N lineas alrededor de file:line.
// Lee el archivo desde el filesystem local del cliente VSH (el server
// no transmite el codigo fuente; el cliente lo abre directamente).
fn cmd_list_source(file_path, line_no, n) {
    let radius = 5
    if n != null { radius = n }
    let real_path = unquote_path(file_path)
    let content = ""
    try {
        content = read_file(real_path)
    } catch e {
        println("ERROR: no se puede abrir " + real_path)
        return
    }
    // Splitear por \n manualmente (VSH no tiene split nativo pero tiene
    // bytes_substr).  Dejamos cada linea sin el \n final.
    let lines = []
    let cur = ""
    let i = 0
    let total_bytes = bytes_len(content)
    while i < total_bytes {
        let ch = content[i]
        if ch == "\n" {
            lines = lines + [cur]
            cur = ""
        } else {
            if ch != "\r" { cur = cur + ch }
        }
        i = i + 1
    }
    if bytes_len(cur) > 0 { lines = lines + [cur] }
    // Mostrar [line_no - radius .. line_no + radius] (1-based).
    let first = line_no - radius
    if first < 1 { first = 1 }
    let last = line_no + radius
    let n_lines = len(lines)
    if last > n_lines { last = n_lines }
    let banner_t = BOLD + CYAN + "-- " + real_path + ":" + str(line_no)
    banner_t = banner_t + " (+/-" + str(radius) + ") --" + RESET
    println(banner_t)
    let j = first
    while j <= last {
        let is_target = (j == line_no)
        let marker = "  "
        if is_target { marker = BCYAN + BOLD + "->" + RESET }
        let txt = "(EOF)"
        if j <= n_lines { txt = lines[j - 1] }
        // Padding del numero de linea a 4 chars.
        let ln_str = str(j)
        while bytes_len(ln_str) < 4 { ln_str = " " + ln_str }
        let ln_col = DIM + ln_str + RESET
        if is_target {
            ln_col = BYELLOW + ln_str + RESET
            txt    = BWHITE + txt + RESET
        } else {
            txt    = DIM + txt + RESET
        }
        println(marker + " " + ln_col + " " + DIM + "|" + RESET + " " + txt)
        j = j + 1
    }
}
// ---- Comandos nuevos ----------------------------------------------------

fn cmd_disasm(pid, addr_or_null, count) {
    if not ensure_pid(pid, "disasm") { return }
    let cmd = {}
    cmd["cmd"]   = "disasm"
    cmd["pid"]   = pid
    cmd["count"] = count
    if addr_or_null != null { cmd["addr"] = addr_or_null }
    let resp = rpc(cmd)
    let d = data_of(resp)
    if d == null { return }
    let items = d["items"]
    let i = 0
    while i < len(items) {
        let it = items[i]
        let line = ""
        line = line + DIM + to_hex(it["addr"], 8) + RESET + "  "
        line = line + GRAY + pad_right(it["bytes"], 22) + RESET + "  "
        let mark = "  "
        if it["ext"] == true { mark = BMAGENTA + "ex" + RESET }
        line = line + mark + "  "
        line = line + BCYAN + BOLD + pad_right(it["name"], 18) + RESET
        line = line + DIM + " sz=" + str(it["size"]) + RESET
        println(line)
        i = i + 1
    }
}

fn cmd_locals(pid) {
    if not ensure_pid(pid, "locals") { return }
    let cmd = {}
    cmd["cmd"] = "locals"
    cmd["pid"] = pid
    let resp = rpc(cmd)
    let d = data_of(resp)
    if d == null { return }
    let vars = d["vars"]
    if len(vars) == 0 {
        println(DIM + "  (sin info de locales en este PC; recompila con --vex-debug y nivel VARS)" + RESET)
        return
    }
    println(BOLD + "  NAME            OFF      ADDR        VALUE" + RESET)
    let i = 0
    while i < len(vars) {
        let v = vars[i]
        let line = "  "
        line = line + GREEN + pad_right(v["name"], 14) + RESET + "  "
        line = line + DIM + pad_left(str(v["offset"]), 5) + RESET + "  "
        line = line + DIM + to_hex(v["addr"], 8) + RESET + "  "
        line = line + BMAGENTA + to_hex(v["value"], 16) + RESET
        line = line + DIM + " (" + str(v["value"]) + ")" + RESET
        println(line)
        i = i + 1
    }
}

fn cmd_gc_run(pid) {
    if not ensure_pid(pid, "gc_run") { return }
    let cmd = {}
    cmd["cmd"] = "gc_run"
    cmd["pid"] = pid
    let resp = rpc(cmd)
    let d = data_of(resp)
    if d == null { return }
    let s = GREEN + "[gc_run]" + RESET
    s = s + " before=" + BCYAN + str(d["before"]) + RESET
    s = s + " after="  + BCYAN + str(d["after"]) + RESET
    s = s + " freed="  + BYELLOW + str(d["freed"]) + RESET
    println(s)
}

fn cmd_finish(pid) {
    if not ensure_pid(pid, "finish") { return }
    let cmd = {}
    cmd["cmd"] = "step_out"
    cmd["pid"] = pid
    let resp = rpc(cmd)
    let d = data_of(resp)
    if d == null { return }
    let s = GREEN + "[finish]" + RESET
    s = s + " target=" + BMAGENTA + to_hex(d["target_pc"], 8) + RESET
    s = s + " bp=" + BCYAN + str(d["bp_id"]) + RESET
    println(s)
}

fn cmd_until(pid, file_or_addr, line_or_null) {
    if not ensure_pid(pid, "until") { return }
    let cmd = {}
    cmd["cmd"] = "step_until"
    cmd["pid"] = pid
    if line_or_null == null {
        cmd["addr"] = file_or_addr
    } else {
        cmd["file"] = file_or_addr
        cmd["line"] = line_or_null
    }
    let resp = rpc(cmd)
    let d = data_of(resp)
    if d == null { return }
    let s = GREEN + "[until]" + RESET
    s = s + " target=" + BMAGENTA + to_hex(d["target_pc"], 8) + RESET
    s = s + " bp=" + BCYAN + str(d["bp_id"]) + RESET
    println(s)
}

fn cmd_break_cond(addr, pid, cond_str) {
    let cmd = {}
    cmd["cmd"]  = "set_break"
    cmd["addr"] = addr
    if pid != null { cmd["pid"] = pid }
    cmd["cond"] = cond_str
    let resp = rpc(cmd)
    let d = data_of(resp)
    if d == null { return }
    let s = GREEN + "[bp]" + RESET
    s = s + " id="   + BCYAN + str(d["id"]) + RESET
    s = s + " addr=" + BMAGENTA + to_hex(d["addr"], 8) + RESET
    s = s + " cond=" + YELLOW + "\"" + cond_str + "\"" + RESET
    println(s)
}

fn cmd_watch(pid, handle) {
    let cmd = {}
    cmd["cmd"] = "set_watch"
    cmd["handle"] = handle
    if pid != null { cmd["pid"] = pid }
    let resp = rpc(cmd)
    let d = data_of(resp)
    if d == null { return }
    let s = GREEN + "[watch]" + RESET
    s = s + " id=" + BCYAN + str(d["id"]) + RESET
    s = s + " h="  + BCYAN + str(d["handle"]) + RESET
    let alive_s = GRAY + "no" + RESET
    if d["alive"] == true { alive_s = GREEN + "yes" + RESET }
    s = s + " alive=" + alive_s
    s = s + " addr=" + DIM + to_hex(d["addr"], 16) + RESET
    println(s)
}

fn cmd_watch_del(id) {
    let cmd = {}
    cmd["cmd"] = "del_watch"
    cmd["id"]  = id
    let resp = rpc(cmd)
    if check_err(resp) { println(GREEN + "[ok]" + RESET + " watchpoint " + str(id) + " borrado") }
}

fn cmd_watch_list() {
    let cmd = {}
    cmd["cmd"] = "list_watches"
    let resp = rpc(cmd)
    if not check_err(resp) { return }
    let arr = resp["data"]
    if len(arr) == 0 {
        println(DIM + "  (no hay watchpoints activos)" + RESET)
        return
    }
    println(BOLD + "  ID  PID    HANDLE  ALIVE  ADDR" + RESET)
    let i = 0
    while i < len(arr) {
        let w = arr[i]
        let line = "  "
        line = line + BCYAN + pad_left(str(w["id"]), 2) + RESET + "  "
        line = line + DIM + pad_left(str(w["pid"]), 4) + RESET + "  "
        line = line + BCYAN + pad_left(str(w["handle"]), 6) + RESET + "  "
        if w["alive"] == true { line = line + GREEN + "yes" + RESET + "    " } else { line = line + RED + "no" + RESET + "     " }
        line = line + DIM + to_hex(w["addr"], 16) + RESET
        println(line)
        i = i + 1
    }
}

fn cmd_trace_msgs(pid, on) {
    if not ensure_pid(pid, "trace_msgs") { return }
    let cmd = {}
    cmd["cmd"] = "trace_msgs"
    cmd["pid"] = pid
    cmd["enabled"] = on
    let resp = rpc(cmd)
    if check_err(resp) {
        let s = "OFF"
        if on { s = "ON" }
        println(GREEN + "[trace_msgs]" + RESET + " pid=" + str(pid) + " " + s)
    }
}

fn cmd_break_mon(on) {
    let cmd = {}
    cmd["cmd"] = "break_mon"
    cmd["enabled"] = on
    let resp = rpc(cmd)
    if check_err(resp) {
        let s = "OFF"
        if on { s = "ON" }
        println(GREEN + "[break_mon]" + RESET + " " + s)
    }
}

fn cmd_display_add(name) {
    let d = {}
    d["id"]  = g_next_display_id
    d["cmd"] = name
    g_next_display_id = g_next_display_id + 1
    g_displays = g_displays + [d]
    let s = GREEN + "[display]" + RESET + " id=" + BCYAN + str(d["id"]) + RESET
    s = s + " cmd=" + YELLOW + name + RESET
    println(s)
}

fn cmd_display_del(id) {
    let new_list = []
    let i = 0
    while i < len(g_displays) {
        if g_displays[i]["id"] != id { new_list = new_list + [g_displays[i]] }
        i = i + 1
    }
    g_displays = new_list
    println(GREEN + "[ok]" + RESET + " display " + str(id) + " removido")
}

fn cmd_display_list() {
    if len(g_displays) == 0 {
        println(DIM + "  (sin displays)" + RESET)
        return
    }
    println(BOLD + "  ID  CMD" + RESET)
    let i = 0
    while i < len(g_displays) {
        let d = g_displays[i]
        let s = "  " + BCYAN + pad_left(str(d["id"]), 2) + RESET + "  "
        s = s + YELLOW + d["cmd"] + RESET
        println(s)
        i = i + 1
    }
}

fn help_row(cmd, desc) {
    println("  " + BCYAN + pad_right(cmd, 30) + RESET + DIM + desc + RESET)
}
fn help_section(name) {
    println("")
    println(BOLD + YELLOW + "  " + name + RESET)
}
fn print_help() {
    header("Comandos disponibles")
    help_section("Procesos")
    help_row("ps",                       "lista procesos")
    help_row("attach <pid>",             "adjuntarse a un proceso")
    help_row("detach <pid>",             "desadjuntar")
    help_row("info <pid> | i <pid>",     "info del proceso")
    help_section("Control de ejecucion")
    help_row("c <pid>",                  "continuar")
    help_row("s <pid>",                  "step (una instruccion)")
    help_row("n <pid>",                  "next (step-over)")
    help_row("p <pid>",                  "pausar proceso")
    help_row("q | quit",                 "salir")
    help_section("Breakpoints")
    help_row("b <addr> [pid]",           "set breakpoint por addr")
    help_row("b <file>:<line> [pid]",    "set bp por linea Vex (--vex-debug)")
    help_row("bd <id>",                  "borrar breakpoint")
    help_row("bl",                       "lista breakpoints")
    help_section("Inspeccion de registros")
    help_row("r <pid> | regs <pid>",     "R0..R15 + PC + SP + BP")
    help_row("flags <pid> | fl <pid>",   "RFlags (CF/OF/SF/ZF/DM)")
    help_row("fregs <pid> | fr <pid>",   "f0..f15 (bits + f64)")
    help_row("eval <expr> <pid>",        "r0..r15, rip, pc")
    help_section("Memoria + Stack")
    help_row("mem <addr> <len> <pid>",   "hex dump de memoria")
    help_row("xs <pid> [N]",             "dump_stack: N qwords desde rsp")
    help_row("frame <pid> | fi <pid>",   "saved_rbp / ret_addr / frame_size")
    help_row("stack <pid>",              "pila simple")
    help_row("bt <pid> | backtrace",     "stack trace con method + file:line")
    help_section("GC")
    help_row("gc <pid>",                 "estadisticas del GC")
    help_row("gch <pid> [N]",            "handles vivos (clase + size + gen)")
    help_row("gci <pid> <h>",            "dump de un handle (header + bytes)")
    help_section("Source")
    help_row("where [pid] | src [pid]",  "file:line del PC actual")
    help_row("list [<file>:<line>] [N]", "muestra N lineas alrededor (sin args = PC actual)")
    help_row("disasm [pid] [addr] [N]",  "desensambla N instrucciones (default = PC)")
    help_row("locals [pid]",             "variables del scope actual (--vex-debug VARS)")
    help_section("Step avanzado")
    help_row("finish [pid]",             "correr hasta el RET del frame actual")
    help_row("until <line> [pid]",       "correr hasta linea Vex (file inferido)")
    help_row("until <file>:<line> [pid]","correr hasta linea Vex especifica")
    help_row("until @<addr> [pid]",      "correr hasta direccion VM")
    help_section("Breakpoints condicionales")
    help_row("bc <addr|file:line> <cond> [pid]", "bp con condicion (ej. bc 0x100 \"r0 == 99\")")
    help_section("GC avanzado")
    help_row("gc_run [pid]",             "forzar major_gc (devuelve handles before/after/freed)")
    help_section("Watchpoints (sobre GcHandle)")
    help_row("watch <handle> [pid]",     "pausa al evacuar/morir el handle")
    help_row("wd <id>",                  "borrar watchpoint")
    help_row("wl",                       "lista watchpoints")
    help_section("Tracing y break-on-event")
    help_row("trace_msgs <pid> on|off",  "logging de msgsend/msgrecv del proceso")
    help_row("break_mon on|off",         "pausar al hit de monitor contention")
    help_section("Display (auto-print tras pausa)")
    help_row("display <cmd>",            "anade comando al auto-print (regs|flags|fregs|where|bt|stack|frame|locals|disasm)")
    help_row("undisplay <id>",           "borra un display")
    help_row("displays",                 "lista displays activos")
    help_section("Otros")
    help_row("help | ?",                 "esta ayuda")
    help_row("(Enter)",                  "repite el ultimo comando")
}

// ---- Parser muy simple de tokens del prompt -----------------------------
// Splitea por espacios.  No soporta comillas (no las necesitamos).
fn tokenize(line) {
    let toks = []
    let cur = ""
    let i = 0
    let n = len(line)
    while i < n {
        let c = line[i]
        if c == " " or c == "\t" {
            if len(cur) > 0 {
                toks = toks + [cur]
                cur = ""
            }
        } else {
            cur = cur + c
        }
        i = i + 1
    }
    if len(cur) > 0 { toks = toks + [cur] }
    return toks
}

// Convierte token a int.  Acepta decimal y "0x..." (hex).
// El builtin `int()` con un solo arg auto-detecta el prefijo "0x"/"0b"/"0o".
fn parse_addr(s) {
    return int(s)
}

// Devuelve toks[idx] parseado como int, o g_default_pid si no esta presente.
// Si tampoco hay default, devuelve null (el wrapper imprime un error claro).
fn pid_arg(toks, idx) {
    if len(toks) > idx { return parse_addr(toks[idx]) }
    return g_default_pid
}

// Comprueba que tengamos un pid valido; si null, imprime ayuda y devuelve false.
fn ensure_pid(pid, cmd_name) {
    if pid != null { return true }
    let s = BRED + "ERROR" + RESET + ": " + cmd_name
    s = s + " necesita un PID; usa 'attach <pid>' para fijarlo por defecto, "
    s = s + "o pasalo como argumento (ej. '" + cmd_name + " 0')."
    println(s)
    return false
}

// Tras un comando que pueda hacer que el proceso se detenga (c/s/n/finish/
// until), esperamos brevemente eventos del server.  Si llega un break/step/
// watch/mon_block, el rpc() interno setea g_pending_render.  Luego, si la
// flag esta activa, ejecutamos los displays guardados.  Coste: 1-2 selects
// de bajo timeout en el caso comun.
fn wait_break_render(pid, max_ms) {
    let waited = 0
    let chunk = 50
    while waited < max_ms {
        let r = socket_poll(sock, chunk)
        if r == 1 {
            // Hay datos: drenar via un rpc no-op (pedimos info_proc al pid
            // por defecto si lo hay; si no, procesos).  Esto leera los
            // eventos buffereados antes de la respuesta y setea
            // g_pending_render dentro del handler.
            let cmd = {}
            cmd["cmd"] = "list_procs"
            rpc(cmd)
            break
        }
        waited = waited + chunk
    }
    if g_pending_render {
        render_displays(pid)
        g_pending_render = false
    }
}

// Renderiza cada display registrado.  Cada display.cmd se traduce a un
// comando estandar.  Si pid != null, lo usa como argumento.
fn render_displays(pid) {
    if len(g_displays) == 0 { return }
    println(DIM + "  -- displays --" + RESET)
    let i = 0
    while i < len(g_displays) {
        let d = g_displays[i]
        let cmd_name = d["cmd"]
        print(DIM + "  [" + str(d["id"]) + "] " + cmd_name + " -> " + RESET)
        try {
            if cmd_name == "regs"     { cmd_regs(pid)         } elif cmd_name == "flags"  { cmd_flags(pid)        } elif cmd_name == "fregs"  { cmd_fregs(pid)        } elif cmd_name == "where"  { cmd_info_source(pid)  } elif cmd_name == "bt"     { cmd_backtrace(pid)    } elif cmd_name == "stack"  { cmd_stack(pid)        } elif cmd_name == "frame"  { cmd_frame_info(pid)   } elif cmd_name == "locals" { cmd_locals(pid)       } elif cmd_name == "disasm" { cmd_disasm(pid, null, 8) } else { println(YELLOW + "(display desconocido: " + cmd_name + ")" + RESET) }
        } catch e {
            println(BRED + "(display error: " + str(e) + ")" + RESET)
        }
        i = i + 1
    }
}

// ---- REPL ---------------------------------------------------------------
print_help()
println("")

let running = true
while running {
    let prompt_pid = ""
    if g_default_pid != null { prompt_pid = DIM + "[" + str(g_default_pid) + "] " + RESET }
    print(BOLD + BCYAN + "(dbg) " + RESET + prompt_pid)
    let line = input("")
    if line == null {
        // EOF en stdin
        running = false
        break
    }
    // Trim whitespace para detectar input vacio.
    let trimmed = ""
    let _i = 0
    let _n = len(line)
    while _i < _n {
        let _c = line[_i]
        if _c != " " and _c != "\t" { trimmed = trimmed + _c }
        _i = _i + 1
    }
    // Enter sin contenido => repite el ultimo comando.
    if len(trimmed) == 0 {
        if len(g_last_input) == 0 { continue }
        line = g_last_input
    } else {
        g_last_input = line
    }
    let toks = tokenize(line)
    if len(toks) == 0 { continue }
    let c0 = toks[0]
    try {
        if c0 == "q" or c0 == "quit" or c0 == "exit" {
            running = false
        } elif c0 == "help" or c0 == "?" {
            print_help()
        } elif c0 == "ps" {
            cmd_ps()
        } elif c0 == "attach" {
            let p = parse_addr(toks[1])
            cmd_attach(p)
            g_default_pid = p
        } elif c0 == "detach" {
            let pid = pid_arg(toks, 1)
            if pid == null { pid = 0 }
            cmd_detach(pid)
        } elif c0 == "b" or c0 == "break" {
            let arg = toks[1]
            let colon_idx = -1
            let i_scan = 0
            let n_scan = len(arg)
            while i_scan < n_scan {
                if arg[i_scan] == ":" { colon_idx = i_scan }
                i_scan = i_scan + 1
            }
            let pid = pid_arg(toks, 2)
            if colon_idx > 0 {
                let file_path = bytes_substr(arg, 0, colon_idx)
                let after_idx = colon_idx + 1
                let after_len = n_scan - colon_idx - 1
                let line_str  = bytes_substr(arg, after_idx, after_len)
                let line_no   = int(line_str)
                cmd_break_src(file_path, line_no, pid)
            } else {
                let addr = parse_addr(arg)
                cmd_break(addr, pid)
            }
        } elif c0 == "bc" {
            // bc <addr|file:line> <cond> [pid]
            // condicion entre comillas.  Reconstruimos desde toks[2:N-pid?]
            let arg = toks[1]
            let pid = null
            // detectar si el ultimo token es un numero que parsea como pid
            let last_n = len(toks) - 1
            let cond_end = last_n
            try {
                let maybe_pid = parse_addr(toks[last_n])
                pid = maybe_pid
                cond_end = last_n - 1
            } catch e { pid = null }
            // armar la condicion uniendo toks[2..cond_end]
            let cond_str = ""
            let ki = 2
            while ki <= cond_end {
                if ki > 2 { cond_str = cond_str + " " }
                cond_str = cond_str + toks[ki]
                ki = ki + 1
            }
            // quitar comillas envolventes si las tiene
            if len(cond_str) >= 2 and cond_str[0] == "\"" and cond_str[len(cond_str)-1] == "\"" {
                cond_str = bytes_substr(cond_str, 1, len(cond_str) - 2)
            }
            cmd_break_cond(parse_addr(arg), pid, cond_str)
        } elif c0 == "where" or c0 == "src" {
            cmd_info_source(pid_arg(toks, 1))
        } elif c0 == "gc" or c0 == "gcstats" {
            cmd_gc_stats(pid_arg(toks, 1))
        } elif c0 == "gch" or c0 == "gchandles" {
            let lim = 64
            if len(toks) >= 3 { lim = parse_addr(toks[2]) }
            cmd_gc_handles(pid_arg(toks, 1), lim)
        } elif c0 == "gci" or c0 == "gcinspect" {
            cmd_gc_inspect(pid_arg(toks, 1), parse_addr(toks[2]))
        } elif c0 == "gc_run" {
            cmd_gc_run(pid_arg(toks, 1))
        } elif c0 == "flags" or c0 == "fl" {
            cmd_flags(pid_arg(toks, 1))
        } elif c0 == "fregs" or c0 == "fr" {
            cmd_fregs(pid_arg(toks, 1))
        } elif c0 == "xs" or c0 == "dump_stack" {
            let n = 16
            if len(toks) >= 3 { n = parse_addr(toks[2]) }
            cmd_dump_stack(pid_arg(toks, 1), n)
        } elif c0 == "frame" or c0 == "fi" {
            cmd_frame_info(pid_arg(toks, 1))
        } elif c0 == "bt" or c0 == "backtrace" {
            cmd_backtrace(pid_arg(toks, 1))
        } elif c0 == "list" or c0 == "l" {
            // Sin args -> usa where + lista alrededor del PC.  Con args
            // file:line [radius] -> lista esa zona.  Con solo radius -> usa where.
            let radius = null
            let file_path = null
            let line_no = null
            if len(toks) >= 2 {
                let arg = toks[1]
                let colon = -1
                let ki = 0
                let kn = len(arg)
                while ki < kn {
                    if arg[ki] == ":" { colon = ki }
                    ki = ki + 1
                }
                if colon < 0 {
                    // un solo numero -> radius con file actual via where
                    radius = parse_addr(arg)
                } else {
                    file_path = bytes_substr(arg, 0, colon)
                    let after_idx = colon + 1
                    let after_len = kn - colon - 1
                    line_no = int(bytes_substr(arg, after_idx, after_len))
                    if len(toks) >= 3 { radius = parse_addr(toks[2]) }
                }
            }
            if file_path == null {
                // resolver via info_source del pid por defecto
                if g_default_pid == null {
                    println(BRED + "ERROR" + RESET + ": list sin args necesita 'attach <pid>' previo, o usa 'list file:line'.")
                } else {
                    let cmd = {}
                    cmd["cmd"] = "info_source"
                    cmd["pid"] = g_default_pid
                    let resp = rpc(cmd)
                    let d = data_of(resp)
                    if d != null and d["file"] != null and d["line"] > 0 {
                        cmd_list_source(d["file"], d["line"], radius)
                    } else {
                        println(YELLOW + "(no hay info de debug; recompila con --vex-debug)" + RESET)
                    }
                }
            } else {
                cmd_list_source(file_path, line_no, radius)
            }
        } elif c0 == "bd" {
            cmd_break_del(parse_addr(toks[1]))
        } elif c0 == "bl" {
            cmd_break_list()
        } elif c0 == "c" or c0 == "cont" or c0 == "continue" {
            let pid = pid_arg(toks, 1)
            if ensure_pid(pid, "c") {
                cmd_continue(pid)
                wait_break_render(pid, 200)
            }
        } elif c0 == "s" or c0 == "step" {
            let pid = pid_arg(toks, 1)
            if ensure_pid(pid, "s") {
                cmd_step(pid)
                wait_break_render(pid, 200)
            }
        } elif c0 == "n" or c0 == "next" {
            let pid = pid_arg(toks, 1)
            if ensure_pid(pid, "n") {
                cmd_next(pid)
                wait_break_render(pid, 200)
            }
        } elif c0 == "p" or c0 == "pause" {
            cmd_pause(pid_arg(toks, 1))
        } elif c0 == "r" or c0 == "regs" {
            cmd_regs(pid_arg(toks, 1))
        } elif c0 == "mem" {
            let pid = g_default_pid
            if len(toks) >= 4 { pid = parse_addr(toks[3]) }
            if ensure_pid(pid, "mem") {
                cmd_mem(parse_addr(toks[1]), parse_addr(toks[2]), pid)
            }
        } elif c0 == "stack" {
            cmd_stack(pid_arg(toks, 1))
        } elif c0 == "info" or c0 == "i" {
            cmd_info(pid_arg(toks, 1))
        } elif c0 == "eval" {
            let pid = g_default_pid
            if len(toks) >= 3 { pid = parse_addr(toks[2]) }
            cmd_eval(toks[1], pid)
        } elif c0 == "disasm" or c0 == "u" {
            let pid = pid_arg(toks, 1)
            let addr = null
            let count = 16
            if len(toks) >= 3 { addr = parse_addr(toks[2]) }
            if len(toks) >= 4 { count = parse_addr(toks[3]) }
            cmd_disasm(pid, addr, count)
        } elif c0 == "locals" {
            cmd_locals(pid_arg(toks, 1))
        } elif c0 == "finish" or c0 == "fin" {
            let pid = pid_arg(toks, 1)
            if ensure_pid(pid, "finish") {
                cmd_finish(pid)
                wait_break_render(pid, 500)
            }
        } elif c0 == "until" {
            // until <line>            -> usa file actual (via where)
            // until <file>:<line>     -> usa esa file:line
            // until @<addr>           -> direccion VM
            // ultimo arg opcional: pid
            let arg = toks[1]
            let pid = pid_arg(toks, 2)
            if not ensure_pid(pid, "until") {
                // err already printed
            } elif len(arg) >= 1 and arg[0] == "@" {
                let a = bytes_substr(arg, 1, len(arg) - 1)
                cmd_until(pid, parse_addr(a), null)
                wait_break_render(pid, 500)
            } else {
                let colon = -1
                let ki = 0
                let kn = len(arg)
                while ki < kn {
                    if arg[ki] == ":" { colon = ki }
                    ki = ki + 1
                }
                if colon > 0 {
                    let fp = bytes_substr(arg, 0, colon)
                    let lns = bytes_substr(arg, colon+1, kn - colon - 1)
                    cmd_until(pid, fp, int(lns))
                    wait_break_render(pid, 500)
                } else {
                    // solo line: pedir el file actual via info_source
                    let cmd = {}
                    cmd["cmd"] = "info_source"
                    cmd["pid"] = pid
                    let resp = rpc(cmd)
                    let d = data_of(resp)
                    if d != null and d["file"] != null {
                        cmd_until(pid, d["file"], int(arg))
                        wait_break_render(pid, 500)
                    } else {
                        println(YELLOW + "(no se pudo inferir el file; usa 'until file.vex:N')" + RESET)
                    }
                }
            }
        } elif c0 == "watch" or c0 == "w" {
            let pid = pid_arg(toks, 2)
            cmd_watch(pid, parse_addr(toks[1]))
        } elif c0 == "wd" {
            cmd_watch_del(parse_addr(toks[1]))
        } elif c0 == "wl" {
            cmd_watch_list()
        } elif c0 == "trace_msgs" or c0 == "tm" {
            let pid = pid_arg(toks, 1)
            let on = true
            if len(toks) >= 3 {
                if toks[2] == "off" or toks[2] == "0" or toks[2] == "false" { on = false }
            }
            cmd_trace_msgs(pid, on)
        } elif c0 == "break_mon" or c0 == "bm" {
            let on = true
            if len(toks) >= 2 {
                if toks[1] == "off" or toks[1] == "0" or toks[1] == "false" { on = false }
            }
            cmd_break_mon(on)
        } elif c0 == "display" or c0 == "disp" {
            if len(toks) < 2 { cmd_display_list() } else { cmd_display_add(toks[1]) }
        } elif c0 == "undisplay" or c0 == "und" {
            cmd_display_del(parse_addr(toks[1]))
        } elif c0 == "displays" {
            cmd_display_list()
        } else {
            println("comando desconocido: " + c0 + "  (escribe 'help' para ayuda)")
        }
    } catch e {
        println("Error: " + str(e))
    }
}

socket_close(sock)
println("Adios")
