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
 * @file vdp_session.cpp
 * @brief Implementacion de la sesion VDP con handshake de autenticacion.
 *
 * Soporta dos modos de autenticacion:
 *   - Token CRAM: challenge-response via SHA-256 sobre TCP plano o TLS.
 *   - mTLS: autenticacion por certificado a nivel de transporte.
 *     En este caso el handshake VDP se limita a HELLO/HELLO_ACK/AUTH_OK.
 *
 * Para conexiones salientes (cliente) se crea un socket TCP directamente.
 * Para conexiones entrantes (servidor) se recibe el socket ya aceptado.
 */

#include "distrib/vdp_session.h"

#include <cstring>
#include <cstdio>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET raw_sock_t;
#  define CLOSE_SOCK(s) closesocket(s)
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
typedef int raw_sock_t;
#  define CLOSE_SOCK(s) ::close(s)
#endif

namespace distrib {

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

VdpSession::VdpSession(vdp_sock_t fd, SSL *ssl, uint32_t node_idx, uint64_t local_id)
    : fd_(fd)
    , ssl_(ssl)
    , node_idx_(node_idx)
    , local_id_(local_id)
    , remote_node_id_(0)
    , session_id_(0)
{}

VdpSession::VdpSession(uint32_t node_idx, uint64_t local_id)
    : fd_(static_cast<vdp_sock_t>(-1))
    , ssl_(nullptr)
    , node_idx_(node_idx)
    , local_id_(local_id)
    , remote_node_id_(0)
    , session_id_(0)
{}

VdpSession::~VdpSession() {
    close();
    // liberar recursos TLS y socket
    if (ssl_) {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (fd_ != static_cast<vdp_sock_t>(-1)) {
        CLOSE_SOCK(fd_);
        fd_ = static_cast<vdp_sock_t>(-1);
    }
}

// ---------------------------------------------------------------------------
// Helpers de I/O de bajo nivel sobre socket raw o SSL
// ---------------------------------------------------------------------------

bool VdpSession::write_exact_(const uint8_t *data, size_t len) {
    while (len > 0) {
        int n;
        if (ssl_) {
            n = SSL_write(ssl_, data, static_cast<int>(len));
        } else {
            n = send(static_cast<raw_sock_t>(fd_),
                     reinterpret_cast<const char *>(data),
                     static_cast<int>(len), 0);
        }
        if (n <= 0) return false; // error o conexion cerrada
        data += static_cast<size_t>(n);
        len  -= static_cast<size_t>(n);
    }
    return true;
}

bool VdpSession::read_exact_(uint8_t *data, size_t len) {
    while (len > 0) {
        int n;
        if (ssl_) {
            n = SSL_read(ssl_, data, static_cast<int>(len));
        } else {
            n = recv(static_cast<raw_sock_t>(fd_),
                     reinterpret_cast<char *>(data),
                     static_cast<int>(len), 0);
        }
        if (n <= 0) return false; // error o conexion cerrada
        data += static_cast<size_t>(n);
        len  -= static_cast<size_t>(n);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Envio de mensajes
// ---------------------------------------------------------------------------

bool VdpSession::send_raw(VdpMsgType msg_type,
                           const uint8_t *data, size_t len,
                           uint32_t seq_num) {
    std::lock_guard<std::mutex> lk(send_mtx_);

    if (!send_header_(msg_type, static_cast<uint32_t>(len), seq_num)) return false;
    if (len == 0) return true;

    return write_exact_(data, len); // enviar el payload en crudo
}

bool VdpSession::send_msg(VdpMsgType msg_type,
                           const std::vector<uint8_t> &payload,
                           uint32_t seq_num) {
    return send_raw(msg_type,
                    payload.empty() ? nullptr : payload.data(),
                    payload.size(),
                    seq_num);
}

bool VdpSession::send_header_(VdpMsgType msg_type, uint32_t payload_len, uint32_t seq_num) {
    VdpHeader hdr{};
    hdr.magic[0] = 'V'; hdr.magic[1] = 'D';
    hdr.magic[2] = 'P'; hdr.magic[3] = '\0';
    hdr.version     = VDP_VERSION;
    hdr.flags       = 0; // el emisor no conoce los flags del receptor; se usan 0 en el envio
    hdr.msg_type    = static_cast<uint16_t>(msg_type);
    hdr.session_id  = session_id_;
    hdr.seq_num     = seq_num;
    hdr.payload_len = payload_len;

    return write_exact_(reinterpret_cast<const uint8_t *>(&hdr), sizeof(VdpHeader));
}

// ---------------------------------------------------------------------------
// Lectura de paquetes
// ---------------------------------------------------------------------------

bool VdpSession::read_packet_(VdpHeader &hdr, std::vector<uint8_t> &payload) {
    // leer la cabecera fija de 24 bytes
    if (!read_exact_(reinterpret_cast<uint8_t *>(&hdr), sizeof(VdpHeader))) return false;

    // validar magic "VDP\0"
    if (hdr.magic[0] != 'V' || hdr.magic[1] != 'D' ||
        hdr.magic[2] != 'P' || hdr.magic[3] != '\0') return false;

    // leer el payload si hay alguno
    if (hdr.payload_len > 0) {
        if (hdr.payload_len > VDP_MAX_PAYLOAD) return false; // proteccion contra payloads gigantes
        payload.resize(hdr.payload_len);
        if (!read_exact_(payload.data(), hdr.payload_len)) return false;
    } else {
        payload.clear();
    }

    return true;
}

// ---------------------------------------------------------------------------
// Handshake saliente (cliente)
// ---------------------------------------------------------------------------

bool VdpSession::connect_to(const char *ip, uint16_t port,
                              const NodeAuthConfig &auth,
                              const char *local_name) {
    state_.store(SessionState::CONNECTING);

    // resolver el host y conectar el socket TCP
    raw_sock_t fd;
    {
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        char port_str[8];
        std::snprintf(port_str, sizeof(port_str), "%u", port);

        if (getaddrinfo(ip, port_str, &hints, &res) != 0) {
            state_.store(SessionState::SESSION_ERROR);
            return false;
        }

        fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd == static_cast<raw_sock_t>(-1)) {
            freeaddrinfo(res);
            state_.store(SessionState::SESSION_ERROR);
            return false;
        }

        if (connect(fd, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
            CLOSE_SOCK(fd);
            freeaddrinfo(res);
            state_.store(SessionState::SESSION_ERROR);
            return false;
        }
        freeaddrinfo(res);
    }

    // configurar el socket (TLS o TCP plano segun configuracion)
    fd_  = static_cast<vdp_sock_t>(fd);
    ssl_ = nullptr;

    if (auth.use_tls) {
        // crear SSL_CTX de cliente directamente con OpenSSL
        SSL_CTX *ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (!ssl_ctx) {
            CLOSE_SOCK(fd);
            state_.store(SessionState::SESSION_ERROR);
            return false;
        }

        // cargar certificado y clave si se requiere mTLS del cliente
        if (auth.cert_path[0]) {
            SSL_CTX_use_certificate_file(ssl_ctx, auth.cert_path, SSL_FILETYPE_PEM);
            SSL_CTX_use_PrivateKey_file(ssl_ctx,  auth.key_path,  SSL_FILETYPE_PEM);
        }
        // cargar CA para verificar el servidor
        if (auth.ca_path[0])
            SSL_CTX_load_verify_locations(ssl_ctx, auth.ca_path, nullptr);

        ssl_ = SSL_new(ssl_ctx);
        SSL_CTX_free(ssl_ctx); // ssl_ mantiene una referencia interna; liberamos el contexto
        SSL_set_fd(ssl_, static_cast<int>(fd));
        SSL_set_tlsext_host_name(ssl_, ip); // SNI para el servidor

        if (SSL_connect(ssl_) <= 0) {
            SSL_free(ssl_);
            ssl_ = nullptr;
            CLOSE_SOCK(fd);
            fd_ = static_cast<vdp_sock_t>(-1);
            state_.store(SessionState::SESSION_ERROR);
            return false;
        }
    }

    // enviar VDP_HELLO
    VdpPayloadHello hello{};
    hello.node_id     = local_id_;
    hello.vdp_version = VDP_VERSION;
    hello.listen_port = 0; // se rellena si el cliente tambien sirve
    hello.auth_flags  = auth.use_token ? VDP_AUTH_TOKEN : VDP_AUTH_NONE;
    if (auth.use_tls && auth.require_client_cert) hello.auth_flags |= VDP_AUTH_CERT;
    std::snprintf(hello.node_name, sizeof(hello.node_name), "%s",
                  local_name ? local_name : "vesta-node");

    state_.store(SessionState::HELLO_SENT);
    if (!send_raw(VdpMsgType::HELLO,
                  reinterpret_cast<const uint8_t *>(&hello), sizeof(hello))) {
        state_.store(SessionState::SESSION_ERROR);
        return false;
    }

    // esperar VDP_HELLO_ACK
    VdpHeader hdr{};
    std::vector<uint8_t> payload;
    if (!read_packet_(hdr, payload)) { state_.store(SessionState::SESSION_ERROR); return false; }
    if (static_cast<VdpMsgType>(hdr.msg_type) != VdpMsgType::HELLO_ACK) {
        state_.store(SessionState::SESSION_ERROR);
        return false;
    }
    if (payload.size() < sizeof(VdpPayloadHelloAck)) { state_.store(SessionState::SESSION_ERROR); return false; }

    const VdpPayloadHelloAck *ack =
        reinterpret_cast<const VdpPayloadHelloAck *>(payload.data());
    session_id_      = ack->session_id;
    remote_node_id_  = ack->server_node_id;

    // si se requiere autenticacion por token, enviar respuesta CRAM
    if (auth.use_token) {
        VdpPayloadAuthToken tok{};
        if (!compute_cram_(auth.token_hash, ack->nonce, tok.response)) {
            state_.store(SessionState::SESSION_ERROR);
            return false;
        }

        state_.store(SessionState::AUTH_SENT);
        if (!send_raw(VdpMsgType::AUTH_TOKEN,
                      reinterpret_cast<const uint8_t *>(&tok), sizeof(tok))) {
            state_.store(SessionState::SESSION_ERROR);
            return false;
        }

        // esperar AUTH_OK o AUTH_FAIL
        if (!read_packet_(hdr, payload)) { state_.store(SessionState::SESSION_ERROR); return false; }
        if (static_cast<VdpMsgType>(hdr.msg_type) == VdpMsgType::AUTH_FAIL) {
            state_.store(SessionState::SESSION_ERROR);
            return false;
        }
        if (static_cast<VdpMsgType>(hdr.msg_type) != VdpMsgType::AUTH_OK) {
            state_.store(SessionState::SESSION_ERROR);
            return false;
        }
    } else {
        // sin token: esperar AUTH_OK del servidor (el servidor decide si acepta sin token)
        if (!read_packet_(hdr, payload)) { state_.store(SessionState::SESSION_ERROR); return false; }
        if (static_cast<VdpMsgType>(hdr.msg_type) != VdpMsgType::AUTH_OK) {
            state_.store(SessionState::SESSION_ERROR);
            return false;
        }
    }

    state_.store(SessionState::ACTIVE);
    return true;
}

// ---------------------------------------------------------------------------
// Handshake entrante (servidor)
// ---------------------------------------------------------------------------

bool VdpSession::server_handshake(const NodeAuthConfig &auth_config) {
    // esperar VDP_HELLO del cliente
    VdpHeader hdr{};
    std::vector<uint8_t> payload;
    if (!read_packet_(hdr, payload)) { state_.store(SessionState::SESSION_ERROR); return false; }
    if (static_cast<VdpMsgType>(hdr.msg_type) != VdpMsgType::HELLO) {
        state_.store(SessionState::SESSION_ERROR);
        return false;
    }
    if (payload.size() < sizeof(VdpPayloadHello)) { state_.store(SessionState::SESSION_ERROR); return false; }

    const VdpPayloadHello *client_hello =
        reinterpret_cast<const VdpPayloadHello *>(payload.data());
    remote_node_id_ = client_hello->node_id;

    // generar nonce y session_id aleatorios
    uint8_t nonce[32] = {};
    if (!gen_random_(nonce, 32)) { state_.store(SessionState::SESSION_ERROR); return false; }
    if (!gen_random_(reinterpret_cast<uint8_t *>(&session_id_), 8)) {
        state_.store(SessionState::SESSION_ERROR);
        return false;
    }

    // enviar VDP_HELLO_ACK con el nonce
    VdpPayloadHelloAck hello_ack{};
    hello_ack.session_id     = session_id_;
    hello_ack.server_node_id = local_id_;
    std::memcpy(hello_ack.nonce, nonce, 32);

    if (!send_raw(VdpMsgType::HELLO_ACK,
                  reinterpret_cast<const uint8_t *>(&hello_ack), sizeof(hello_ack))) {
        state_.store(SessionState::SESSION_ERROR);
        return false;
    }

    // si se requiere token: verificar respuesta CRAM del cliente
    bool token_required = auth_config.use_token;
    bool token_ok       = !token_required; // si no se requiere, es ok por defecto

    if (token_required) {
        if (!read_packet_(hdr, payload)) { state_.store(SessionState::SESSION_ERROR); return false; }
        if (static_cast<VdpMsgType>(hdr.msg_type) != VdpMsgType::AUTH_TOKEN) {
            // cliente no envio el token requerido
            send_raw(VdpMsgType::AUTH_FAIL, nullptr, 0);
            state_.store(SessionState::SESSION_ERROR);
            return false;
        }
        if (payload.size() < sizeof(VdpPayloadAuthToken)) {
            send_raw(VdpMsgType::AUTH_FAIL, nullptr, 0);
            state_.store(SessionState::SESSION_ERROR);
            return false;
        }

        const VdpPayloadAuthToken *tok =
            reinterpret_cast<const VdpPayloadAuthToken *>(payload.data());

        // calcular la respuesta esperada con nuestro token_hash
        uint8_t expected[32] = {};
        if (!compute_cram_(auth_config.token_hash, nonce, expected)) {
            state_.store(SessionState::SESSION_ERROR);
            return false;
        }

        // comparacion en tiempo constante para evitar timing attacks
        int diff = 0;
        for (int i = 0; i < 32; ++i) {
            diff |= (tok->response[i] ^ expected[i]);
        }
        token_ok = (diff == 0);
    }

    if (!token_ok) {
        VdpPayloadAuthFail fail{};
        fail.error_code = static_cast<uint32_t>(VdpError::AUTH_FAILED);
        std::snprintf(fail.message, sizeof(fail.message), "token incorrecto");
        send_raw(VdpMsgType::AUTH_FAIL,
                 reinterpret_cast<const uint8_t *>(&fail), sizeof(fail));
        state_.store(SessionState::SESSION_ERROR);
        return false;
    }

    // enviar AUTH_OK
    VdpPayloadAuthOk ok_payload{};
    ok_payload.session_id  = session_id_;
    ok_payload.server_caps = 0;
    if (!send_raw(VdpMsgType::AUTH_OK,
                  reinterpret_cast<const uint8_t *>(&ok_payload), sizeof(ok_payload))) {
        state_.store(SessionState::SESSION_ERROR);
        return false;
    }

    state_.store(SessionState::ACTIVE);
    return true;
}

// ---------------------------------------------------------------------------
// Hilo lector
// ---------------------------------------------------------------------------

void VdpSession::start_reader(VdpMsgHandler handler, VdpDisconnectHandler on_disconnect) {
    reader_running_.store(true);
    reader_thread_ = std::thread([this,
                                  handler       = std::move(handler),
                                  on_disconnect = std::move(on_disconnect)]() {
        reader_loop_(handler, on_disconnect);
    });
}

void VdpSession::reader_loop_(VdpMsgHandler handler, VdpDisconnectHandler on_disconnect) {
    while (reader_running_.load() && state_.load() == SessionState::ACTIVE) {
        VdpHeader             hdr{};
        std::vector<uint8_t>  payload;

        if (!read_packet_(hdr, payload)) break; // conexion cerrada o error

        handler(hdr, payload, node_idx_); // despachar al DistRuntime
    }
    state_.store(SessionState::CLOSED);
    reader_running_.store(false);

    // notificar al DistRuntime para que actualice el registro y elimine la sesion
    if (on_disconnect) on_disconnect(node_idx_);
}

void VdpSession::close() {
    state_.store(SessionState::CLOSED);
    reader_running_.store(false);
    // cerrar el socket para que el hilo lector salga del recv/SSL_read bloqueante
    if (fd_ != static_cast<vdp_sock_t>(-1)) {
#ifdef _WIN32
        shutdown(fd_, SD_BOTH);
#else
        shutdown(fd_, SHUT_RDWR);
#endif
    }
    if (reader_thread_.joinable()) reader_thread_.join();
}

// ---------------------------------------------------------------------------
// Utilidades criptograficas
// ---------------------------------------------------------------------------

bool VdpSession::compute_cram_(const uint8_t token_hash[32],
                                const uint8_t nonce[32],
                                uint8_t out[32]) {
    // CRAM: SHA256(token_hash_hex || ":" || nonce_hex)
    // Representar token_hash y nonce como cadenas hexadecimales de 64 chars cada una
    char token_hex[65] = {};
    char nonce_hex[65] = {};
    for (int i = 0; i < 32; ++i) {
        std::snprintf(token_hex + i * 2, 3, "%02x", token_hash[i]);
        std::snprintf(nonce_hex  + i * 2, 3, "%02x", nonce[i]);
    }

    // mensaje = token_hex + ":" + nonce_hex (131 bytes total: 64 + 1 + 64 + nul)
    char msg[131] = {};
    std::snprintf(msg, sizeof(msg), "%s:%s", token_hex, nonce_hex);

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return false;

    unsigned int out_len = 0;
    bool ok = true;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) ok = false;
    if (ok && EVP_DigestUpdate(ctx, msg, std::strlen(msg)) != 1) ok = false;
    if (ok && EVP_DigestFinal_ex(ctx, out, &out_len) != 1) ok = false;

    EVP_MD_CTX_free(ctx);
    return ok && (out_len == 32);
}

bool VdpSession::gen_random_(uint8_t *buf, size_t len) {
    return RAND_bytes(buf, static_cast<int>(len)) == 1;
}

} // namespace distrib
