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
 * @file test_distrib_session_tls.cpp
 * @brief Test de VdpSession: handshake y mensajes sobre TCP con TLS.
 *
 * Genera un certificado RSA autofirmado en memoria (sin archivos en disco)
 * y lo pasa al contexto SSL del servidor.  El cliente acepta cualquier
 * certificado (sin verificacion de CA) para simplificar el test.
 *
 * Escenarios:
 *   1. Handshake TLS sin token: ambas sesiones deben llegar a ACTIVE.
 *   2. Handshake TLS con token CRAM correcto: ACTIVE.
 *   3. Handshake TLS con token CRAM incorrecto: SESSION_ERROR.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/bn.h>
#include <openssl/rand.h>

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
static void init_winsock()    { WSADATA w; WSAStartup(MAKEWORD(2,2),&w); }
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

// ---------------------------------------------------------------------------
// Contadores de resultado
// ---------------------------------------------------------------------------
static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char *desc) {
    if (cond) { printf("  [PASS] %s\n", desc); ++g_pass; }
    else       { printf("  [FAIL] %s\n", desc); ++g_fail; }
}
static void print_sep(const char *t) {
    printf("\n============================================================\n");
    printf("  %s\n", t);
    printf("============================================================\n");
}

// ---------------------------------------------------------------------------
// Estructura que almacena un certificado + clave generados en memoria
// ---------------------------------------------------------------------------
struct TestCert {
    EVP_PKEY *pkey = nullptr;  // clave privada RSA 2048
    X509     *cert = nullptr;  // certificado X.509 autofirmado

    // buffers PEM en memoria (para pasar a SSL_CTX sin disco)
    std::vector<uint8_t> cert_pem;
    std::vector<uint8_t> key_pem;

    ~TestCert() {
        if (cert) { X509_free(cert); cert = nullptr; }
        if (pkey) { EVP_PKEY_free(pkey); pkey = nullptr; }
    }
};

// ---------------------------------------------------------------------------
// Genera un certificado RSA 2048 autofirmado para usar en el test
// ---------------------------------------------------------------------------
static bool gen_test_cert(TestCert &tc) {
    // generar clave RSA 2048 via EVP_PKEY_CTX
    EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!kctx) return false;
    EVP_PKEY_keygen_init(kctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048);
    if (EVP_PKEY_keygen(kctx, &tc.pkey) <= 0) {
        EVP_PKEY_CTX_free(kctx);
        return false;
    }
    EVP_PKEY_CTX_free(kctx);

    // crear certificado X.509 v3
    tc.cert = X509_new();
    if (!tc.cert) return false;

    X509_set_version(tc.cert, 2); // X.509 v3

    // numero de serie aleatorio
    ASN1_INTEGER *serial = X509_get_serialNumber(tc.cert);
    uint8_t serial_bytes[8];
    RAND_bytes(serial_bytes, sizeof(serial_bytes));
    BIGNUM *bn = BN_bin2bn(serial_bytes, sizeof(serial_bytes), nullptr);
    BN_to_ASN1_INTEGER(bn, serial);
    BN_free(bn);

    // validez: ahora hasta 1 ano
    X509_gmtime_adj(X509_get_notBefore(tc.cert), 0);
    X509_gmtime_adj(X509_get_notAfter(tc.cert), 365L * 24 * 60 * 60);

    // clave publica
    X509_set_pubkey(tc.cert, tc.pkey);

    // sujeto e issuer (autofirmado -> son iguales)
    X509_NAME *name = X509_get_subject_name(tc.cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char *>("VestaVM-TestNode"), -1, -1, 0);
    X509_set_issuer_name(tc.cert, name);

    // firmar con la propia clave
    if (!X509_sign(tc.cert, tc.pkey, EVP_sha256())) return false;

    // serializar a PEM en memoria
    BIO *bio_cert = BIO_new(BIO_s_mem());
    BIO *bio_key  = BIO_new(BIO_s_mem());
    if (!bio_cert || !bio_key) {
        BIO_free(bio_cert); BIO_free(bio_key);
        return false;
    }

    PEM_write_bio_X509(bio_cert, tc.cert);
    PEM_write_bio_PrivateKey(bio_key, tc.pkey,
                             nullptr, nullptr, 0, nullptr, nullptr);

    char *ptr;
    long  len;

    len = BIO_get_mem_data(bio_cert, &ptr);
    tc.cert_pem.assign(reinterpret_cast<uint8_t *>(ptr),
                        reinterpret_cast<uint8_t *>(ptr) + len);

    len = BIO_get_mem_data(bio_key, &ptr);
    tc.key_pem.assign(reinterpret_cast<uint8_t *>(ptr),
                       reinterpret_cast<uint8_t *>(ptr) + len);

    BIO_free(bio_cert);
    BIO_free(bio_key);
    return true;
}

// ---------------------------------------------------------------------------
// Crea SSL_CTX para el servidor con el certificado de test generado en memoria
// ---------------------------------------------------------------------------
static SSL_CTX *make_server_ssl_ctx(const TestCert &tc) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return nullptr;

    // cargar certificado desde buffer PEM en memoria
    BIO *bio_cert = BIO_new_mem_buf(tc.cert_pem.data(),
                                     static_cast<int>(tc.cert_pem.size()));
    X509 *x = PEM_read_bio_X509(bio_cert, nullptr, nullptr, nullptr);
    BIO_free(bio_cert);
    if (!x || SSL_CTX_use_certificate(ctx, x) <= 0) {
        X509_free(x); SSL_CTX_free(ctx);
        return nullptr;
    }
    X509_free(x);

    // cargar clave privada desde buffer PEM en memoria
    BIO *bio_key = BIO_new_mem_buf(tc.key_pem.data(),
                                    static_cast<int>(tc.key_pem.size()));
    EVP_PKEY *pk = PEM_read_bio_PrivateKey(bio_key, nullptr, nullptr, nullptr);
    BIO_free(bio_key);
    if (!pk || SSL_CTX_use_PrivateKey(ctx, pk) <= 0) {
        EVP_PKEY_free(pk); SSL_CTX_free(ctx);
        return nullptr;
    }
    EVP_PKEY_free(pk);

    SSL_CTX_check_private_key(ctx); // verificar que cert y clave coinciden
    return ctx;
}

// ---------------------------------------------------------------------------
// Socket de escucha
// ---------------------------------------------------------------------------
static test_sock_t create_listen_sock(uint16_t port) {
    test_sock_t lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock == TEST_INVALID_SOCK) return TEST_INVALID_SOCK;
    int reuse = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char *>(&reuse), sizeof(reuse));
    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(port);
    if (bind(lsock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        test_close(lsock); return TEST_INVALID_SOCK;
    }
    if (listen(lsock, 1) != 0) {
        test_close(lsock); return TEST_INVALID_SOCK;
    }
    return lsock;
}

// ---------------------------------------------------------------------------
// SHA256 del token para NodeAuthConfig
// ---------------------------------------------------------------------------
static void sha256_token(const char *token, uint8_t out[32]) {
    SHA256(reinterpret_cast<const uint8_t *>(token), strlen(token), out);
}

// ---------------------------------------------------------------------------
// Estructura de resultado del servidor
// ---------------------------------------------------------------------------
struct SrvResult {
    std::atomic<bool> ready{false};
    std::atomic<bool> done{false};
    bool handshake_ok{false};
    uint64_t remote_node_id{0};
    std::vector<std::vector<uint8_t>> received;
    std::mutex recv_mtx;
};

// ---------------------------------------------------------------------------
// Test TLS: handshake sin token
// ---------------------------------------------------------------------------
static void test_session_tls_no_token(uint16_t port, const TestCert &tc) {
    print_sep("VdpSession TLS (sin token)");

    SrvResult srv;
    distrib::NodeAuthConfig server_auth{};
    server_auth.use_tls   = true;
    server_auth.use_token = false;

    SSL_CTX *srv_ctx = make_server_ssl_ctx(tc);
    if (!srv_ctx) {
        printf("  [ERROR] no se pudo crear SSL_CTX para el servidor. Omitiendo test.\n");
        return;
    }
    printf("  Certificado de test generado: RSA 2048 autofirmado, CN=VestaVM-TestNode\n");

    std::thread srv_thread([&]() {
        test_sock_t lsock = create_listen_sock(port);
        if (lsock == TEST_INVALID_SOCK) { srv.ready.store(true); srv.done.store(true); return; }
        printf("  [SERVIDOR TLS] escuchando en 127.0.0.1:%u\n", port);
        srv.ready.store(true);

        struct sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);
        test_sock_t cfd = accept(lsock, reinterpret_cast<struct sockaddr *>(&caddr), &clen);
        test_close(lsock);

        if (cfd == TEST_INVALID_SOCK) { srv.done.store(true); return; }

        // realizar handshake TLS con el SSL_CTX del servidor
        SSL *ssl = SSL_new(srv_ctx);
        SSL_set_fd(ssl, static_cast<int>(cfd));

        printf("  [SERVIDOR TLS] handshake TLS en progreso...\n");
        int tls_ret = SSL_accept(ssl);
        if (tls_ret <= 0) {
            printf("  [SERVIDOR TLS] SSL_accept fallo (ret=%d)\n", tls_ret);
            ERR_print_errors_fp(stderr);
            SSL_free(ssl);
            test_close(cfd);
            srv.done.store(true);
            return;
        }
        printf("  [SERVIDOR TLS] TLS establecido. Cipher: %s\n",
               SSL_get_cipher(ssl));

        // crear sesion VDP sobre el canal TLS
        distrib::VdpSession session(static_cast<vdp_sock_t>(cfd), ssl, 0, 5001);
        bool ok = session.server_handshake(server_auth);
        printf("  [SERVIDOR TLS] server_handshake() = %s\n", ok ? "ACTIVE" : "ERROR");

        srv.handshake_ok   = ok;
        srv.remote_node_id = session.remote_node_id();

        if (ok) {
            printf("  [SERVIDOR TLS] sesion activa: remote_id=%llu session_id=0x%llX\n",
                   (unsigned long long)session.remote_node_id(),
                   (unsigned long long)session.session_id());

            // escuchar PING y responder PONG
            session.start_reader([&](const distrib::VdpHeader &hdr,
                                       const std::vector<uint8_t> &payload,
                                       uint32_t) {
                if (static_cast<distrib::VdpMsgType>(hdr.msg_type) ==
                    distrib::VdpMsgType::PING) {
                    {
                        std::lock_guard<std::mutex> lk(srv.recv_mtx);
                        srv.received.push_back(payload);
                    }
                    printf("  [SERVIDOR TLS] PING recibido (%u bytes), enviando PONG\n",
                           hdr.payload_len);
                    session.send_msg(distrib::VdpMsgType::PONG, payload);
                }
            });

            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            session.close();
        }
        srv.done.store(true);
    });

    // esperar a que el servidor este listo
    while (!srv.ready.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // ----- cliente TLS -----
    printf("  [CLIENTE TLS]  conectando a 127.0.0.1:%u...\n", port);

    distrib::NodeAuthConfig client_auth{};
    client_auth.use_tls            = true;
    client_auth.use_token          = false;
    client_auth.require_client_cert= false;
    // sin cert de CA -> el cliente no verifica el servidor (solo para el test)
    client_auth.ca_path[0]         = '\0';

    distrib::VdpSession client(0, 6001);
    bool client_ok = client.connect_to("127.0.0.1", port, client_auth, "tls-test-client");
    printf("  [CLIENTE TLS]  connect_to() = %s\n", client_ok ? "ACTIVE" : "ERROR");

    std::atomic<bool> pong_rx{false};
    if (client_ok) {
        client.start_reader([&](const distrib::VdpHeader &hdr,
                                  const std::vector<uint8_t> &,
                                  uint32_t) {
            if (static_cast<distrib::VdpMsgType>(hdr.msg_type) ==
                distrib::VdpMsgType::PONG) {
                pong_rx.store(true);
                printf("  [CLIENTE TLS]  PONG recibido\n");
            }
        });

        const std::vector<uint8_t> ping = { 'T','L','S','-','O','K' };
        bool sent = client.send_msg(distrib::VdpMsgType::PING, ping);
        printf("  [CLIENTE TLS]  PING enviado: %s\n", sent ? "OK" : "ERROR");
        check(sent, "cliente TLS envia PING");

        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
        while (!pong_rx.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        printf("  [CLIENTE TLS]  PONG recibido a tiempo: %s\n",
               pong_rx.load() ? "SI" : "NO (timeout)");
        check(pong_rx.load(), "cliente TLS recibe PONG del servidor");

        client.close();
    }

    srv_thread.join();
    SSL_CTX_free(srv_ctx);

    check(client_ok,         "cliente TLS alcanza ACTIVE");
    check(srv.handshake_ok,  "servidor TLS alcanza ACTIVE");
    check(client.remote_node_id() == 5001,
          "cliente conoce node_id del servidor TLS (5001)");
    check(srv.remote_node_id == 6001,
          "servidor conoce node_id del cliente TLS (6001)");
    if (!srv.received.empty())
        check(srv.received[0] == std::vector<uint8_t>({'T','L','S','-','O','K'}),
              "payload PING llegó integro a traves de TLS");
}

// ---------------------------------------------------------------------------
// Test TLS con token CRAM
// ---------------------------------------------------------------------------
static void test_session_tls_with_token(uint16_t port, const TestCert &tc,
                                         bool correct_token) {
    char titulo[128];
    std::snprintf(titulo, sizeof(titulo),
                  "VdpSession TLS + token CRAM (%s)",
                  correct_token ? "token correcto" : "token incorrecto");
    print_sep(titulo);

    SrvResult srv;
    const char *server_token = "token-tls-vesta";
    distrib::NodeAuthConfig server_auth{};
    server_auth.use_tls   = true;
    server_auth.use_token = true;
    sha256_token(server_token, server_auth.token_hash);

    SSL_CTX *srv_ctx = make_server_ssl_ctx(tc);
    if (!srv_ctx) {
        printf("  [ERROR] no se pudo crear SSL_CTX. Omitiendo test.\n");
        return;
    }

    std::thread srv_thread([&]() {
        test_sock_t lsock = create_listen_sock(port);
        if (lsock == TEST_INVALID_SOCK) { srv.ready.store(true); srv.done.store(true); return; }
        printf("  [SERVIDOR TLS+TOKEN] escuchando en 127.0.0.1:%u\n", port);
        srv.ready.store(true);

        struct sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);
        test_sock_t cfd = accept(lsock, reinterpret_cast<struct sockaddr *>(&caddr), &clen);
        test_close(lsock);

        SSL *ssl = SSL_new(srv_ctx);
        SSL_set_fd(ssl, static_cast<int>(cfd));
        if (SSL_accept(ssl) <= 0) {
            SSL_free(ssl); test_close(cfd);
            srv.done.store(true); return;
        }

        distrib::VdpSession session(static_cast<vdp_sock_t>(cfd), ssl, 0, 7001);
        bool ok = session.server_handshake(server_auth);
        printf("  [SERVIDOR TLS+TOKEN] handshake = %s\n", ok ? "ACTIVE" : "ERROR/RECHAZADO");
        srv.handshake_ok = ok;
        if (ok) session.close();
        srv.done.store(true);
    });

    while (!srv.ready.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));

    const char *client_token = correct_token ? "token-tls-vesta" : "token-incorrecto";
    distrib::NodeAuthConfig client_auth{};
    client_auth.use_tls   = true;
    client_auth.use_token = true;
    sha256_token(client_token, client_auth.token_hash);

    printf("  [CLIENTE TLS+TOKEN] token='%s'\n", client_token);

    distrib::VdpSession client(0, 8001);
    bool client_ok = client.connect_to("127.0.0.1", port, client_auth, "tls-token-client");
    printf("  [CLIENTE TLS+TOKEN] connect_to() = %s\n",
           client_ok ? "ACTIVE" : "ERROR/RECHAZADO");
    if (client_ok) client.close();

    srv_thread.join();
    SSL_CTX_free(srv_ctx);

    if (correct_token) {
        check(client_ok,        "TLS+token correcto: cliente ACTIVE");
        check(srv.handshake_ok, "TLS+token correcto: servidor ACTIVE");
    } else {
        check(!client_ok,        "TLS+token incorrecto: cliente rechazado");
        check(!srv.handshake_ok, "TLS+token incorrecto: servidor rechaza");
    }
}

// ---------------------------------------------------------------------------
// Punto de entrada
// ---------------------------------------------------------------------------
int main() {
    init_winsock();
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    printf("====================================================\n");
    printf("  VestaVM - Tests VdpSession sobre TLS\n");
    printf("====================================================\n");

    // generar certificado RSA 2048 autofirmado una sola vez para todos los tests
    printf("\n[SETUP] Generando certificado RSA 2048 de test...\n");
    TestCert tc;
    if (!gen_test_cert(tc)) {
        printf("[ERROR] No se pudo generar el certificado de test. "
               "Verificar que OpenSSL 3.x este disponible.\n");
        ERR_print_errors_fp(stderr);
        cleanup_winsock();
        return 1;
    }
    printf("[SETUP] Certificado generado: %zu bytes PEM cert, %zu bytes PEM key\n",
           tc.cert_pem.size(), tc.key_pem.size());

    test_session_tls_no_token(17892, tc);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    test_session_tls_with_token(17893, tc, true);   // token correcto
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    test_session_tls_with_token(17894, tc, false);  // token incorrecto

    printf("\n============================================================\n");
    printf("  RESULTADO: %d PASS, %d FAIL\n", g_pass, g_fail);
    printf("============================================================\n");

    EVP_cleanup();
    ERR_free_strings();
    cleanup_winsock();
    return (g_fail == 0) ? 0 : 1;
}
