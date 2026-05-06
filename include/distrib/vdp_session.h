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
 * @file vdp_session.h
 * @brief Sesion VDP: conexion autenticada con un nodo remoto.
 *
 * VdpSession encapsula una conexion TCP (plana o TLS) y el estado
 * de autenticacion VDP sobre esa conexion.
 *
 * Ciclo de vida de una sesion saliente (cliente):
 *   1. Conectar al nodo remoto (connect_to).
 *   2. Enviar VDP_HELLO.
 *   3. Recibir VDP_HELLO_ACK con el nonce.
 *   4. Si use_token: enviar VDP_AUTH_TOKEN con la respuesta CRAM.
 *   5. Recibir VDP_AUTH_OK o VDP_AUTH_FAIL.
 *   6. Sesion activa: send_msg / recv_msg disponibles.
 *
 * Ciclo de vida de una sesion entrante (servidor):
 *   1. Aceptar conexion de TCPServer (socket ya disponible).
 *   2. Recibir VDP_HELLO.
 *   3. Enviar VDP_HELLO_ACK con nonce aleatorio.
 *   4. Si se requiere token: esperar VDP_AUTH_TOKEN y verificar.
 *   5. Enviar VDP_AUTH_OK.
 *   6. Iniciar hilo lector que despacha mensajes al DistRuntime.
 *
 * Si TLS esta habilitado, el handshake TLS ocurre en la capa de transporte
 * (Connection / TLSConnection) antes del handshake VDP.
 */

#ifndef VDP_SESSION_H
#define VDP_SESSION_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <memory>

#include <openssl/ssl.h>

#include "distrib/vdp_protocol.h"
#include "distrib/node_registry.h"

// Tipo de descriptor de socket compatible con Windows (SOCKET) y POSIX (int)
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET vdp_sock_t;
#else
typedef int vdp_sock_t;
#endif

namespace distrib {

// ---------------------------------------------------------------------------
// Estado interno de la sesion
// ---------------------------------------------------------------------------
enum class SessionState {
    INIT          = 0, // recien creada, sin conexion
    CONNECTING    = 1, // conexion TCP en progreso
    HELLO_SENT    = 2, // VDP_HELLO enviado, esperando HELLO_ACK
    AUTH_SENT     = 3, // VDP_AUTH_TOKEN enviado, esperando AUTH_OK
    ACTIVE        = 4, // sesion autenticada y lista para usar
    CLOSED        = 5, // sesion cerrada normalmente
    SESSION_ERROR = 6, // sesion cerrada por error (ERROR es macro en Windows)
};

// ---------------------------------------------------------------------------
// Callback para mensajes entrantes
// ---------------------------------------------------------------------------
/**
 * @brief Tipo de callback para mensajes VDP recibidos.
 *
 * El hilo lector de la sesion llama este callback por cada paquete completo
 * recibido.  El DistRuntime registra su manejador en cada sesion.
 *
 * @param hdr     Cabecera del paquete recibido.
 * @param payload Bytes del payload (puede estar vacio).
 * @param node_idx Indice del nodo emisor en NodeRegistry.
 */
using VdpMsgHandler = std::function<void(
    const VdpHeader &hdr,
    const std::vector<uint8_t> &payload,
    uint32_t node_idx
)>;

/**
 * @brief Callback invocado cuando la sesion se desconecta (el hilo lector finaliza).
 * @param node_idx Indice del nodo cuya sesion ha cerrado.
 */
using VdpDisconnectHandler = std::function<void(uint32_t node_idx)>;

// ---------------------------------------------------------------------------
// Clase VdpSession
// ---------------------------------------------------------------------------

/**
 * @brief Sesion VDP sobre una conexion TCP (plana o TLS).
 *
 * Gestiona el handshake de autenticacion y el envio/recepcion de paquetes VDP.
 * Para conexiones TLS, se espera que @p conn sea un TLSConnection ya con el
 * handshake TLS completado.
 *
 * El envio es thread-safe (protegido por send_mtx_).
 * El hilo lector corre de forma independiente e invoca el VdpMsgHandler.
 */
class VdpSession {
public:
    /**
     * @brief Construye una sesion con un socket ya aceptado (modo servidor).
     *
     * Se usa cuando TCPServer acepta una conexion y la pasa a DistRuntime.
     * Si la conexion usa TLS, @p ssl debe ser un SSL* ya tras SSL_accept().
     *
     * @param fd        Descriptor de socket ya aceptado.
     * @param ssl       Objeto SSL si la conexion es TLS; nullptr si es TCP plano.
     * @param node_idx  Indice del nodo en NodeRegistry (UINT32_MAX si desconocido).
     * @param local_id  ID de este nodo (para incluirlo en el HELLO_ACK).
     */
    VdpSession(vdp_sock_t fd, SSL *ssl, uint32_t node_idx, uint64_t local_id);

    /**
     * @brief Construye una sesion para conexion saliente (modo cliente).
     *
     * El socket se crea en connect_to(); este constructor solo inicializa
     * los metadatos del nodo y deja fd_=-1, ssl_=nullptr.
     *
     * @param node_idx Indice del nodo destino en NodeRegistry.
     * @param local_id ID de este nodo (para incluirlo en el HELLO saliente).
     */
    VdpSession(uint32_t node_idx, uint64_t local_id);

    /**
     * @brief Destructor: detiene el hilo lector y cierra la conexion.
     */
    ~VdpSession();

    // Sin copia ni movimiento (gestion de recursos por puntero)
    VdpSession(const VdpSession &) = delete;
    VdpSession &operator=(const VdpSession &) = delete;

    // ----------------------------------------------------------------
    // Handshake de sesion saliente (cliente inicia la conexion)
    // ----------------------------------------------------------------

    /**
     * @brief Conecta a un nodo remoto e inicia el handshake VDP.
     *
     * Crea un socket TCP, conecta al host:port, realiza el handshake TLS
     * si auth.use_tls==true, y luego ejecuta el handshake VDP (HELLO -> AUTH_OK).
     *
     * Esta llamada bloquea hasta que la sesion quede en estado ACTIVE o ERROR.
     *
     * @param ip    Direccion IP o nombre de host del nodo remoto.
     * @param port  Puerto TCP del servidor VDP.
     * @param auth  Configuracion de autenticacion.
     * @param local_name Nombre de este nodo (para incluir en el HELLO).
     * @return true si la sesion quedo activa; false si hubo error.
     */
    bool connect_to(const char *ip, uint16_t port,
                    const NodeAuthConfig &auth,
                    const char *local_name = "");

    // ----------------------------------------------------------------
    // Handshake de sesion entrante (servidor acepta la conexion)
    // ----------------------------------------------------------------

    /**
     * @brief Ejecuta el lado servidor del handshake VDP.
     *
     * Espera VDP_HELLO, envia VDP_HELLO_ACK con nonce, valida el token si
     * es necesario, y envia VDP_AUTH_OK.  Bloquea hasta AUTH_OK o ERROR.
     *
     * @param auth_config Config del servidor (token esperado, flags de cert).
     * @return true si la autenticacion fue exitosa.
     */
    bool server_handshake(const NodeAuthConfig &auth_config);

    // ----------------------------------------------------------------
    // Envio de mensajes (thread-safe)
    // ----------------------------------------------------------------

    /**
     * @brief Envia un paquete VDP completo (cabecera + payload).
     *
     * Construye la cabecera, la serializa y envia por la conexion.
     * Thread-safe: usa send_mtx_ internamente.
     *
     * @param msg_type   Tipo de mensaje VDP.
     * @param payload    Bytes del payload (puede ser vacio).
     * @param seq_num    Numero de secuencia (0 si no se espera ACK).
     * @return true si se envio correctamente.
     */
    bool send_msg(VdpMsgType msg_type,
                  const std::vector<uint8_t> &payload,
                  uint32_t seq_num = VDP_SEQ_NONE);

    /**
     * @brief Envia un paquete VDP con payload en bruto.
     *
     * Sobrecarga conveniente para evitar copiar en un vector.
     *
     * @param msg_type  Tipo de mensaje.
     * @param data      Puntero al payload.
     * @param len       Longitud del payload en bytes.
     * @param seq_num   Numero de secuencia.
     * @return true si se envio correctamente.
     */
    bool send_raw(VdpMsgType msg_type,
                  const uint8_t *data, size_t len,
                  uint32_t seq_num = VDP_SEQ_NONE);

    // ----------------------------------------------------------------
    // Hilo lector
    // ----------------------------------------------------------------

    /**
     * @brief Inicia el hilo lector que procesa paquetes entrantes.
     *
     * El hilo llama al handler @p handler por cada paquete completo recibido.
     * Se detiene cuando la conexion se cierra o se llama a close().
     *
     * @param handler      Callback invocado por cada paquete recibido.
     * @param on_disconnect Callback opcional invocado al cerrar la sesion (puede ser nulo).
     */
    void start_reader(VdpMsgHandler handler, VdpDisconnectHandler on_disconnect = {});

    /**
     * @brief Cierra la sesion y detiene el hilo lector.
     */
    void close();

    // ----------------------------------------------------------------
    // Consultas de estado
    // ----------------------------------------------------------------

    /** @brief Retorna true si la sesion esta autenticada y lista. */
    bool is_active() const { return state_.load() == SessionState::ACTIVE; }

    /** @brief Estado actual de la sesion. */
    SessionState state() const { return state_.load(); }

    /** @brief ID de sesion asignado por el servidor (0 si no hay sesion). */
    uint64_t session_id() const { return session_id_; }

    /** @brief Indice del nodo remoto en NodeRegistry. */
    uint32_t node_idx() const { return node_idx_; }

    /** @brief Actualiza el indice del nodo (necesario para sesiones entrantes). */
    void set_node_idx(uint32_t idx) { node_idx_ = idx; }

    /** @brief ID del nodo remoto (obtenido en el HELLO). */
    uint64_t remote_node_id() const { return remote_node_id_; }

    /** @brief Incrementa el contador de secuencia y retorna el valor nuevo. */
    uint32_t next_seq() { return ++seq_counter_; }

private:
    vdp_sock_t                     fd_;             // descriptor de socket TCP subyacente
    SSL                           *ssl_;             // objeto SSL (nullptr = sin TLS)
    uint32_t                       node_idx_;        // indice en NodeRegistry
    uint64_t                       local_id_;        // ID de este nodo
    uint64_t                       remote_node_id_;  // ID del nodo remoto (del HELLO)
    uint64_t                       session_id_;      // ID de sesion activo
    std::atomic<SessionState>      state_{SessionState::INIT};
    std::atomic<uint32_t>          seq_counter_{0};  // contador de secuencia monotono
    std::mutex                     send_mtx_;        // protege el envio (thread-safe)
    std::thread                    reader_thread_;   // hilo lector de paquetes
    std::atomic<bool>              reader_running_{false};

    // escribe exactamente len bytes en el socket (bloqueante)
    bool write_exact_(const uint8_t *data, size_t len);

    // lee exactamente len bytes del socket (bloqueante)
    bool read_exact_(uint8_t *data, size_t len);

    // lee un paquete completo (bloquea hasta leer la cabecera + payload)
    bool read_packet_(VdpHeader &hdr, std::vector<uint8_t> &payload);

    // construye y envia la cabecera VDP
    bool send_header_(VdpMsgType msg_type, uint32_t payload_len, uint32_t seq_num);

    // bucle del hilo lector
    void reader_loop_(VdpMsgHandler handler, VdpDisconnectHandler on_disconnect);

    // calcula el CRAM SHA-256: SHA256(token_hash_hex || ":" || nonce_hex)
    static bool compute_cram_(const uint8_t token_hash[32],
                               const uint8_t nonce[32],
                               uint8_t out[32]);

    // genera bytes aleatorios seguros para el nonce
    static bool gen_random_(uint8_t *buf, size_t len);
};

} // namespace distrib

#endif // VDP_SESSION_H
