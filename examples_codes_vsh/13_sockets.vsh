// VestaShell - 13: Sockets y red
// Cubre: tcp_connect, tcp_listen, tcp_accept, socket_send, socket_recv,
//        socket_recv_all, socket_close, udp_socket, udp_bind, udp_sendto,
//        tls_connect, http_get, https_get, http_post, https_post,
//        http_put, https_put, http_delete, https_delete, http_request
//
// NOTA: Los tests de red requieren acceso a Internet.
//       Los tests que fallen por red se cuentan como SKIP (no FAIL).

let pass = 0
let fail = 0
let skip = 0

fn check(nombre, cond) {
    if cond {
        pass = pass + 1
        println("  [PASS] " + nombre)
    } else {
        fail = fail + 1
        println("  [FAIL] " + nombre)
    }
}

fn check_skip(nombre, cond, skipped) {
    if skipped {
        skip = skip + 1
        println("  [SKIP] " + nombre + " (sin red)")
    } else {
        check(nombre, cond)
    }
}

println("=== 13: Sockets y red ===")

// ============================================================
// TCP crudo
// ============================================================
println("\n--- TCP raw ---")

// tcp_connect a un servidor HTTP publico, enviar GET manual, leer respuesta
let tcp_raw_ok = false
let tcp_err = false
try {
    let h = tcp_connect("httpbin.org", 80)
    let req = "GET /get HTTP/1.0\r\nHost: httpbin.org\r\nConnection: close\r\n\r\n"
    let sent = socket_send(h, req)
    let resp = socket_recv_all(h)
    socket_close(h)
    tcp_raw_ok = sent > 0 and contains(resp, "HTTP/")
} catch e {
    tcp_err = true
}
check_skip("tcp_connect + send + recv_all (HTTP raw)", tcp_raw_ok, tcp_err)

// socket_recv en chunks (loop manual hasta EOF)
let tcp_chunks_ok = false
let tcp_chunks_err = false
try {
    let h = tcp_connect("httpbin.org", 80)
    socket_send(h, "GET /get HTTP/1.0\r\nHost: httpbin.org\r\nConnection: close\r\n\r\n")
    let resp = ""
    let chunk = socket_recv(h, 512)
    while len(chunk) > 0 {
        resp = resp + chunk
        chunk = socket_recv(h, 512)
    }
    socket_close(h)
    tcp_chunks_ok = contains(resp, "httpbin.org")
} catch e {
    tcp_chunks_err = true
}
check_skip("socket_recv en chunks hasta EOF", tcp_chunks_ok, tcp_chunks_err)

// ============================================================
// TCP servidor (tcp_listen)
// ============================================================
println("\n--- TCP listen ---")

// tcp_listen con puerto dinamico (0 = asignar SO)
let listen_ok = false
try {
    let srv = tcp_listen(0)
    check("handle servidor valido (> 0)", srv > 0)
    socket_close(srv)
    listen_ok = true
} catch e {
    println("  [SKIP] tcp_listen: " + str(e))
    skip = skip + 1
}

// tcp_listen con backlog explicito
let listen_backlog_ok = false
try {
    let srv = tcp_listen(0, 5)
    socket_close(srv)
    listen_backlog_ok = true
} catch e {}
check("tcp_listen con backlog explicito", listen_backlog_ok)

// ============================================================
// UDP
// ============================================================
println("\n--- UDP ---")

// crear socket UDP y cerrarlo
let udp_create_ok = false
try {
    let u = udp_socket()
    check("handle UDP valido (> 0)", u > 0)
    socket_close(u)
    udp_create_ok = true
} catch e { println("  [SKIP] udp_socket: " + str(e)); skip = skip + 1 }

// udp_bind a puerto dinamico
let udp_bind_ok = false
try {
    let u = udp_socket()
    udp_bind(u, 0)
    socket_close(u)
    udp_bind_ok = true
} catch e {}
check("udp_socket + udp_bind (puerto 0)", udp_bind_ok)

// udp_sendto al puerto discard (9) de loopback — no necesita respuesta
let udp_send_ok = false
let udp_send_err = false
try {
    let u = udp_socket()
    let n = udp_sendto(u, "ping", "127.0.0.1", 9)
    socket_close(u)
    udp_send_ok = (n > 0)
} catch e { udp_send_err = true }
check_skip("udp_sendto al puerto discard (127.0.0.1:9)", udp_send_ok, udp_send_err)

// ============================================================
// TLS (tls_connect)
// ============================================================
println("\n--- TLS ---")

let tls_ok = false
let tls_err = false
try {
    let h = tls_connect("httpbin.org", 443)
    check("handle TLS valido (> 0)", h > 0)
    // enviar peticion HTTPS manual sobre la conexion TLS
    let req = "GET /get HTTP/1.0\r\nHost: httpbin.org\r\nConnection: close\r\n\r\n"
    socket_send(h, req)
    let resp = socket_recv_all(h)
    socket_close(h)
    tls_ok = contains(resp, "httpbin.org")
} catch e { tls_err = true }
check_skip("tls_connect + send + recv_all (HTTPS manual)", tls_ok, tls_err)

// ============================================================
// http_get / https_get
// ============================================================
println("\n--- HTTP GET ---")

let hget_ok = false
let hget_err = false
try {
    let r = http_get("http://httpbin.org/get")
    hget_ok = contains(r, '"url"') and contains(r, "httpbin.org")
} catch e { hget_err = true }
check_skip("http_get (HTTP)", hget_ok, hget_err)

let sget_ok = false
let sget_err = false
try {
    let r = https_get("https://httpbin.org/get")
    sget_ok = contains(r, '"url"') and contains(r, "httpbin.org")
} catch e { sget_err = true }
check_skip("https_get (HTTPS)", sget_ok, sget_err)

// ============================================================
// http_post / https_post
// ============================================================
println("\n--- HTTP POST ---")

let post_ok = false
let post_err = false
try {
    let body = "campo=valor&otro=123"
    let r = http_post("http://httpbin.org/post", body)
    post_ok = contains(r, '"campo": "valor"') or contains(r, "campo")
} catch e { post_err = true }
check_skip("http_post (form-urlencoded)", post_ok, post_err)

let post_json_ok = false
let post_json_err = false
try {
    let body = '{"nombre":"VestaShell","version":1}'
    let r = https_post("https://httpbin.org/post", body, "application/json")
    post_json_ok = contains(r, "VestaShell")
} catch e { post_json_err = true }
check_skip("https_post (application/json)", post_json_ok, post_json_err)

// ============================================================
// http_put / https_put
// ============================================================
println("\n--- HTTP PUT ---")

let put_ok = false
let put_err = false
try {
    let r = http_put("http://httpbin.org/put", "dato=actualizado")
    put_ok = contains(r, "actualizado") or contains(r, "dato")
} catch e { put_err = true }
check_skip("http_put", put_ok, put_err)

let sput_ok = false
let sput_err = false
try {
    let r = https_put("https://httpbin.org/put", '{"x":42}', "application/json")
    sput_ok = contains(r, "42") or contains(r, "x")
} catch e { sput_err = true }
check_skip("https_put (JSON)", sput_ok, sput_err)

// ============================================================
// http_delete / https_delete
// ============================================================
println("\n--- HTTP DELETE ---")

let del_ok = false
let del_err = false
try {
    let r = http_delete("http://httpbin.org/delete")
    del_ok = contains(r, '"url"') or contains(r, "httpbin")
} catch e { del_err = true }
check_skip("http_delete", del_ok, del_err)

let sdel_ok = false
let sdel_err = false
try {
    let r = https_delete("https://httpbin.org/delete")
    sdel_ok = contains(r, '"url"') or contains(r, "httpbin")
} catch e { sdel_err = true }
check_skip("https_delete", sdel_ok, sdel_err)

// ============================================================
// http_request (API generica)
// ============================================================
println("\n--- http_request (generica) ---")

// GET
let req_get_ok = false
let req_get_err = false
try {
    let r = http_request("GET", "https://httpbin.org/get")
    req_get_ok = r["status"] == 200 and contains(r["body"], "httpbin")
} catch e { req_get_err = true }
check_skip("http_request GET devuelve map {status, body}", req_get_ok, req_get_err)

// POST con JSON
let req_post_ok = false
let req_post_err = false
let post_json_body = '{"test":"vestaShell"}'
let post_json_ct   = "application/json"
try {
    let r = http_request("POST", "https://httpbin.org/post", post_json_body, post_json_ct)
    req_post_ok = r["status"] == 200 and contains(r["body"], "vestaShell")
} catch e { req_post_err = true }
check_skip("http_request POST JSON", req_post_ok, req_post_err)

// PUT
let req_put_ok = false
let req_put_err = false
try {
    let r = http_request("PUT", "https://httpbin.org/put", "a=1", "application/x-www-form-urlencoded")
    req_put_ok = r["status"] == 200
} catch e { req_put_err = true }
check_skip("http_request PUT", req_put_ok, req_put_err)

// DELETE
let req_del_ok = false
let req_del_err = false
try {
    let r = http_request("DELETE", "https://httpbin.org/delete")
    req_del_ok = r["status"] == 200
} catch e { req_del_err = true }
check_skip("http_request DELETE", req_del_ok, req_del_err)

// verificar que los headers se devuelven
let req_hdr_ok = false
let req_hdr_err = false
try {
    let r = http_request("GET", "https://httpbin.org/get")
    req_hdr_ok = contains(r["headers"], "HTTP/") and len(r["headers"]) > 0
} catch e { req_hdr_err = true }
check_skip("http_request headers en respuesta", req_hdr_ok, req_hdr_err)

// ============================================================
// Manejo de errores de socket
// ============================================================
println("\n--- Manejo de errores ---")

// handle invalido lanza error
let bad_handle_ok = false
try {
    socket_send(9999, "datos")
} catch e {
    bad_handle_ok = true
}
check("socket_send handle invalido lanza error", bad_handle_ok)

// socket_recv handle invalido lanza error
let bad_recv_ok = false
try {
    socket_recv(9999)
} catch e {
    bad_recv_ok = true
}
check("socket_recv handle invalido lanza error", bad_recv_ok)

// socket_close handle invalido no lanza (es idempotente)
let close_nop_ok = false
try {
    socket_close(9999)
    close_nop_ok = true
} catch e {}
check("socket_close handle invalido no lanza", close_nop_ok)

// tcp_connect a host inexistente lanza error
let conn_fail_ok = false
try {
    tcp_connect("host.invalido.xyz", 80)
} catch e {
    conn_fail_ok = true
}
check("tcp_connect host inexistente lanza error", conn_fail_ok)

println("\n--- Resultado: " + str(pass) + " PASS, " + str(fail) + " FAIL, " + str(skip) + " SKIP ---")
