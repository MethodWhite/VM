/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 */

/**
 * @file test_distrib_session_plain.cpp
 * @brief Test de VdpSession: handshake y mensajes sobre TCP plano (sin TLS).
 *
 * Escenario:
 *   - Hilo servidor: abre un socket TCP en 127.0.0.1:17789, acepta una conexion,
 *     crea VdpSession(fd, nullptr, ...) y ejecuta server_handshake().
 *   - Hilo cliente (main): crea VdpSession(node_idx, local_id) y ejecuta connect_to().
 *   - Tras el handshake ambas sesiones deben estar en estado ACTIVE.
 *   - Se intercambian mensajes VDP via send_msg y start_reader.
 *
 * Escenario secundario (con token CRAM):
 *   - Mismo flujo pero con NodeAuthConfig.use_token = true.
 *   - Token correcto -> ACTIVE; token incorrecto -> SESSION_ERROR.
 */

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <string>

#include "distrib/vdp_session.h"
#include "distrib/vdp_protocol.h"
#include "distrib/node_registry.h"

// ---------------------------------------------------------------------------
// Compatibilidad de sockets (Windows / POSIX)
// ---------------------------------------------------------------------------
#ifdef _WIN32
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0600
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET test_sock_t;
#  define TEST_INVALID_SOCK INVALID_SOCKET
#  define test_close(s)     closesocket(s)
static void init_winsock() {
    WSADATA w;
    WSAStartup(MAKEWORD(2,2), &w);
}
static void cleanup_winsock() { WSACleanup(); }
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
typedef int test_sock_t;
#  define TEST_INVALID_SOCK (-1)
#  define test_close(s)     ::close(s)
static void init_winsock()    {}
static void cleanup_winsock() {}
#endif

#include <openssl/sha.h>    // SHA256
#include <openssl/ssl.h>    // SSL_library_init
#include "controller/tls_context.h" // TLSContext::initialize()

// ---------------------------------------------------------------------------
// Contadores globales de resultado
// ---------------------------------------------------------------------------
static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char *desc) {
    if (cond) {
        printf("  [PASS] %s\n", desc);
        ++g_pass;
    } else {
        printf("  [FAIL] %s\n", desc);
        ++g_fail;
    }
}
static void print_sep(const char *t) {
    printf("\n============================================================\n");
    printf("  %s\n", t);
    printf("============================================================\n");
}

// ---------------------------------------------------------------------------
// Crear un socket de escucha en el puerto indicado
// ---------------------------------------------------------------------------
static test_sock_t create_listen_sock(uint16_t port) {
    test_sock_t lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock == TEST_INVALID_SOCK) return TEST_INVALID_SOCK;

    // reusar el puerto rapidamente tras cerrar
    int reuse = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char *>(&reuse), sizeof(reuse));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(port);

    if (bind(lsock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        test_close(lsock);
        return TEST_INVALID_SOCK;
    }
    if (listen(lsock, 1) != 0) {
        test_close(lsock);
        return TEST_INVALID_SOCK;
    }
    return lsock;
}

// ---------------------------------------------------------------------------
// Estructura para pasar datos entre el hilo servidor y el hilo principal
// ---------------------------------------------------------------------------
struct ServerResult {
    std::atomic<bool>  ready{false};   // servidor listo para aceptar
    std::atomic<bool>  done{false};    // handshake completado
    bool               handshake_ok{false};
    distrib::SessionState state{distrib::SessionState::INIT};
    uint64_t           remote_node_id{0};
    uint64_t           session_id{0};
    // mensajes recibidos por el hilo lector del servidor
    std::vector<std::vector<uint8_t>> received_msgs;
    std::mutex         msgs_mtx;
};

// ---------------------------------------------------------------------------
// Test 1: handshake TCP plano sin token
// ---------------------------------------------------------------------------
static void test_session_plain_no_token(uint16_t port) {
    print_sep("VdpSession TCP plano (sin token)");

    ServerResult srv;
    distrib::NodeAuthConfig server_auth{};
    server_auth.use_tls   = false;
    server_auth.use_token = false;

    // hilo servidor
    std::thread srv_thread([&]() {
        test_sock_t lsock = create_listen_sock(port);
        if (lsock == TEST_INVALID_SOCK) {
            printf("  [SERVIDOR] error al crear socket de escucha en puerto %u\n", port);
            srv.ready.store(true);
            srv.done.store(true);
            return;
        }
        printf("  [SERVIDOR] escuchando en 127.0.0.1:%u\n", port);
        srv.ready.store(true); // notificar al cliente que ya puede conectar

        struct sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);
        test_sock_t cfd = accept(lsock, reinterpret_cast<struct sockaddr *>(&caddr), &clen);
        test_close(lsock);

        if (cfd == TEST_INVALID_SOCK) {
            printf("  [SERVIDOR] accept() fallo\n");
            srv.done.store(true);
            return;
        }
        printf("  [SERVIDOR] conexion aceptada, iniciando handshake VDP...\n");

        // crear sesion entrante con nodo local_id=1001
        distrib::VdpSession session(static_cast<vdp_sock_t>(cfd), nullptr, 0, 1001);
        bool ok = session.server_handshake(server_auth);

        printf("  [SERVIDOR] server_handshake() = %s\n", ok ? "ACTIVE" : "ERROR");
        srv.handshake_ok    = ok;
        srv.state           = session.state();
        srv.session_id      = session.session_id();
        srv.remote_node_id  = session.remote_node_id();

        if (ok) {
            printf("  [SERVIDOR] sesion activa: session_id=0x%llX remote_node_id=%llu\n",
                   (unsigned long long)session.session_id(),
                   (unsigned long long)session.remote_node_id());

            // iniciar hilo lector para recibir mensajes del cliente
            session.start_reader([&](const distrib::VdpHeader &hdr,
                                      const std::vector<uint8_t> &payload,
                                      uint32_t node_idx) {
                (void)node_idx;
                if (static_cast<distrib::VdpMsgType>(hdr.msg_type) ==
                    distrib::VdpMsgType::PING) {
                    std::lock_guard<std::mutex> lk(srv.msgs_mtx);
                    srv.received_msgs.push_back(payload);
                    printf("  [SERVIDOR] PING recibido con payload de %u bytes\n",
                           hdr.payload_len);
                    // responder PONG
                    session.send_msg(distrib::VdpMsgType::PONG, payload);
                }
            });

            // esperar brevemente a que el cliente envie y cierre
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            session.close();
        }
        srv.done.store(true);
    });

    // esperar a que el servidor este listo
    while (!srv.ready.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // hilo principal actua como cliente
    printf("  [CLIENTE]  conectando a 127.0.0.1:%u...\n", port);

    distrib::NodeAuthConfig client_auth{};
    client_auth.use_tls   = false;
    client_auth.use_token = false;

    distrib::VdpSession client(0, 2002); // node_idx=0, local_id=2002
    bool client_ok = client.connect_to("127.0.0.1", port, client_auth, "test-cliente");

    printf("  [CLIENTE]  connect_to() = %s\n", client_ok ? "ACTIVE" : "ERROR");
    if (client_ok) {
        printf("  [CLIENTE]  session_id=0x%llX remote_node_id=%llu\n",
               (unsigned long long)client.session_id(),
               (unsigned long long)client.remote_node_id());
    }

    // intercambiar mensaje PING si la conexion fue exitosa
    std::atomic<bool> pong_received{false};
    if (client_ok) {
        // iniciar hilo lector del cliente para recibir el PONG
        client.start_reader([&](const distrib::VdpHeader &hdr,
                                  const std::vector<uint8_t> &payload,
                                  uint32_t /*node_idx*/) {
            if (static_cast<distrib::VdpMsgType>(hdr.msg_type) ==
                distrib::VdpMsgType::PONG) {
                pong_received.store(true);
                printf("  [CLIENTE]  PONG recibido con payload de %u bytes\n",
                       hdr.payload_len);
            }
        });

        // enviar PING con payload "VDP-TEST"
        const std::vector<uint8_t> ping_payload = { 'V','D','P','-','T','E','S','T' };
        bool sent = client.send_msg(distrib::VdpMsgType::PING, ping_payload);
        printf("  [CLIENTE]  PING enviado: %s\n", sent ? "OK" : "ERROR");
        check(sent, "cliente envia PING correctamente");

        // esperar PONG (maximo 500 ms)
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (!pong_received.load() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        printf("  [CLIENTE]  PONG recibido dentro del timeout: %s\n",
               pong_received.load() ? "SI" : "NO (timeout)");
        check(pong_received.load(), "cliente recibe PONG del servidor");

        client.close();
    }

    srv_thread.join();

    // verificaciones finales
    check(client_ok,         "cliente alcanza estado ACTIVE");
    check(srv.handshake_ok,  "servidor alcanza estado ACTIVE");
    check(client.remote_node_id() == 1001,
          "cliente conoce el node_id del servidor (1001)");
    check(srv.remote_node_id == 2002,
          "servidor conoce el node_id del cliente (2002)");
    if (!srv.received_msgs.empty()) {
        const auto &msg = srv.received_msgs[0];
        check(msg.size() == 8 &&
              std::memcmp(msg.data(), "VDP-TEST", 8) == 0,
              "servidor recibio payload PING correcto ('VDP-TEST')");
    }
}

// ---------------------------------------------------------------------------
// Test 2: handshake TCP plano con token CRAM
// ---------------------------------------------------------------------------

// Calcula SHA-256 del token para rellenar NodeAuthConfig.token_hash
static void sha256_token(const char *token, uint8_t out[32]) {
    SHA256(reinterpret_cast<const uint8_t *>(token),
           strlen(token), out);
}

static void test_session_plain_with_token(uint16_t port, bool correct_token) {
    char titulo[128];
    std::snprintf(titulo, sizeof(titulo),
                  "VdpSession TCP plano CON token CRAM (%s)",
                  correct_token ? "token correcto" : "token incorrecto");
    print_sep(titulo);

    ServerResult srv;

    // servidor exige token = "secreto-vesta-2026"
    const char *server_token = "secreto-vesta-2026";
    distrib::NodeAuthConfig server_auth{};
    server_auth.use_tls   = false;
    server_auth.use_token = true;
    sha256_token(server_token, server_auth.token_hash);

    std::thread srv_thread([&]() {
        test_sock_t lsock = create_listen_sock(port);
        if (lsock == TEST_INVALID_SOCK) { srv.ready.store(true); srv.done.store(true); return; }

        printf("  [SERVIDOR] escuchando en 127.0.0.1:%u (con token)\n", port);
        srv.ready.store(true);

        struct sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);
        test_sock_t cfd = accept(lsock, reinterpret_cast<struct sockaddr *>(&caddr), &clen);
        test_close(lsock);

        distrib::VdpSession session(static_cast<vdp_sock_t>(cfd), nullptr, 0, 3001);
        bool ok = session.server_handshake(server_auth);
        printf("  [SERVIDOR] server_handshake() = %s\n", ok ? "ACTIVE" : "ERROR/RECHAZADO");
        srv.handshake_ok   = ok;
        srv.state          = session.state();
        if (ok) session.close();
        srv.done.store(true);
    });

    while (!srv.ready.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // cliente con token correcto o incorrecto segun el parametro
    const char *client_token = correct_token ? "secreto-vesta-2026" : "token-equivocado";
    distrib::NodeAuthConfig client_auth{};
    client_auth.use_tls   = false;
    client_auth.use_token = true;
    sha256_token(client_token, client_auth.token_hash);

    printf("  [CLIENTE]  token usado: '%s'\n", client_token);

    distrib::VdpSession client(0, 4001);
    bool client_ok = client.connect_to("127.0.0.1", port, client_auth, "test-token-client");
    printf("  [CLIENTE]  connect_to() = %s\n", client_ok ? "ACTIVE" : "ERROR/RECHAZADO");
    if (client_ok) client.close();

    srv_thread.join();

    if (correct_token) {
        check(client_ok,        "cliente con token correcto: ACTIVE");
        check(srv.handshake_ok, "servidor acepta token correcto: ACTIVE");
    } else {
        check(!client_ok,        "cliente con token incorrecto: rechazado (SESSION_ERROR)");
        check(!srv.handshake_ok, "servidor rechaza token incorrecto");
    }
}

// ---------------------------------------------------------------------------
// Punto de entrada
// ---------------------------------------------------------------------------

int main() {
    init_winsock();
    TLSContext::initialize(); // inicializar OpenSSL (para SHA256 interno)

    printf("====================================================\n");
    printf("  VestaVM - Tests VdpSession sobre TCP plano\n");
    printf("====================================================\n");

    // usar puertos altos para evitar colision con otros servicios
    test_session_plain_no_token(17789);
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // dejar que el SO libere el puerto
    test_session_plain_with_token(17790, true);   // token correcto
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    test_session_plain_with_token(17791, false);  // token incorrecto

    printf("\n============================================================\n");
    printf("  RESULTADO: %d PASS, %d FAIL\n", g_pass, g_fail);
    printf("============================================================\n");

    cleanup_winsock();
    return (g_fail == 0) ? 0 : 1;
}
