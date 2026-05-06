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
 * @file node_registry.h
 * @brief Registro de nodos VestaVM conocidos (estaticos y dinamicamente descubiertos).
 *
 * NodeRegistry mantiene la tabla de todos los nodos VDP con los que puede
 * comunicarse esta instancia.  Soporta dos modos de descubrimiento:
 *
 *   - Estatico: lista de nodos en el archivo de configuracion (IP + puerto + token).
 *   - Dinamico: broadcast UDP en VDP_DISCOVER_PORT para descubrir pares en la LAN.
 *     Los nodos responden con NODE_ANNOUNCE y se agregan automaticamente.
 *
 * La clase es thread-safe: puede leerse y modificarse desde el hilo de red
 * y desde los hilos de proceso simultaneamente.
 */

#ifndef NODE_REGISTRY_H
#define NODE_REGISTRY_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>

#include "distrib/vdp_protocol.h"

namespace distrib {

// ---------------------------------------------------------------------------
// Configuracion de autenticacion por nodo
// ---------------------------------------------------------------------------

/**
 * @brief Configuracion de autenticacion para una conexion a un nodo.
 *
 * Controla si la conexion usa TLS, token, o ambos.
 * Para mTLS, los certificados deben estar en las rutas indicadas.
 * Para token, el campo @p token_hash contiene SHA-256(plaintext_token).
 */
struct NodeAuthConfig {
    bool    use_tls;            // true = usar TLS sobre la conexion TCP
    bool    require_client_cert;// true = mutual TLS (verificar el certificado del servidor)
    bool    use_token;          // true = realizar CRAM de token sobre la sesion VDP
    uint8_t token_hash[32];     // SHA-256(plaintext_token); solo valido si use_token == true
    char    cert_path[256];     // ruta al certificado local (PEM); vacio si no se usa TLS
    char    key_path[256];      // ruta a la clave privada (PEM); vacio si no se usa TLS
    char    ca_path[256];       // ruta al CA bundle para verificar el servidor; vacio = sin verificar
};

// ---------------------------------------------------------------------------
// Estado de conexion de un nodo
// ---------------------------------------------------------------------------
enum class NodeState {
    UNKNOWN      = 0, // nodo conocido pero sin sesion establecida
    CONNECTING   = 1, // handshake en progreso
    AUTHENTICATED= 2, // sesion activa y autenticada
    DISCONNECTED = 3, // sesion cerrada o perdida
    BANNED       = 4, // nodo rechazado por fallo de autenticacion repetido
};

// ---------------------------------------------------------------------------
// Informacion de un nodo VDP
// ---------------------------------------------------------------------------

/**
 * @brief Descriptor de un nodo VDP en el registro.
 *
 * Un nodo puede ser:
 *   - Estatico: configurado por el usuario en el archivo de config.
 *   - Dinamico: descubierto via broadcast UDP.
 *
 * El campo @p session es un puntero opaco a la sesion activa (VdpSession*).
 * Es nullptr si el nodo no tiene sesion establecida.
 */
struct NodeInfo {
    uint32_t       idx;          // indice de este nodo en el vector del registro (inmutable)
    uint64_t       node_id;      // identificador unico de 64 bits del nodo
    char           ip[64];       // direccion IP o nombre de host (IPv4 o IPv6)
    uint16_t       port;         // puerto TCP del servidor VDP del nodo
    char           name[32];     // nombre legible del nodo (para logs)
    bool           is_static;    // true si fue configurado estaticamente; false si es dinamico
    NodeState      state;        // estado actual de la conexion
    NodeAuthConfig auth;         // configuracion de autenticacion para este nodo
    void          *session;      // puntero a VdpSession activa (nullptr si desconectado)
    uint32_t       auth_failures;// intentos de autenticacion fallidos consecutivos
};

// ---------------------------------------------------------------------------
// Clase principal del registro
// ---------------------------------------------------------------------------

/**
 * @brief Registro de nodos VDP conocidos.
 *
 * Permite:
 *   - Agregar nodos estaticos via add_static().
 *   - Iniciar descubrimiento dinamico via start_discovery().
 *   - Obtener un nodo por indice via get().
 *   - Actualizar el estado de un nodo via set_state().
 *
 * El descubrimiento dinamico lanza un hilo UDP que:
 *   1. Emite broadcasts NODE_DISCOVER cada 30 s.
 *   2. Escucha respuestas NODE_ANNOUNCE y agrega los nodos nuevos.
 *
 * Callback on_new_node: se llama cuando se descubre un nodo nuevo para que
 *   DistRuntime pueda iniciar la conexion y autenticacion.
 */
class NodeRegistry {
public:
    using NewNodeCallback = std::function<void(const NodeInfo &)>;

    /**
     * @brief Construye el registro con el ID de nodo local indicado.
     * @param local_node_id ID de este nodo (para incluirlo en los mensajes de descubrimiento).
     * @param local_port    Puerto TCP del servidor VDP local.
     */
    NodeRegistry(uint64_t local_node_id, uint16_t local_port);

    /**
     * @brief Destructor: detiene el hilo de descubrimiento si esta activo.
     */
    ~NodeRegistry();

    /**
     * @brief Agrega un nodo configurado estaticamente.
     *
     * Si ya existe un nodo con el mismo node_id, lo actualiza.
     *
     * @param ip    Direccion IP o nombre de host del nodo.
     * @param port  Puerto TCP del servidor VDP del nodo.
     * @param auth  Configuracion de autenticacion para este nodo.
     * @param name  Nombre legible del nodo (para logs).
     * @return Indice del nodo en el registro.
     */
    uint32_t add_static(const char *ip, uint16_t port,
                        const NodeAuthConfig &auth,
                        const char *name = "");

    /**
     * @brief Agrega un nodo descubierto dinamicamente.
     *
     * Llamado desde el hilo de descubrimiento cuando llega un NODE_ANNOUNCE.
     * No sobreescribe nodos estaticos existentes con el mismo node_id.
     *
     * @param node_id ID del nodo anunciante.
     * @param ip      Direccion IP del anunciante.
     * @param port    Puerto TCP del servidor VDP.
     * @param name    Nombre del nodo.
     * @return Indice del nodo en el registro, o VDP_MAX_NODES si el registro esta lleno.
     */
    uint32_t add_dynamic(uint64_t node_id, const char *ip, uint16_t port, const char *name);

    /**
     * @brief Obtiene un nodo por su indice.
     * @param idx Indice del nodo (0-based).
     * @return Puntero al NodeInfo, o nullptr si el indice es invalido.
     */
    NodeInfo *get(uint32_t idx);

    /**
     * @brief Busca un nodo por su node_id.
     * @param node_id Identificador del nodo.
     * @return Puntero al NodeInfo, o nullptr si no se encuentra.
     */
    NodeInfo *find_by_id(uint64_t node_id);

    /**
     * @brief Numero de nodos registrados (estaticos + dinamicos).
     */
    size_t count() const;

    /**
     * @brief Actualiza el estado de conexion de un nodo.
     * @param idx   Indice del nodo.
     * @param state Nuevo estado.
     */
    void set_state(uint32_t idx, NodeState state);

    /**
     * @brief Asocia una sesion VdpSession a un nodo.
     * @param idx     Indice del nodo.
     * @param session Puntero a la sesion activa (puede ser nullptr para desconectar).
     */
    void set_session(uint32_t idx, void *session);

    /**
     * @brief Registra el callback que se llama al descubrir un nodo nuevo.
     * @param cb Funcion a invocar con el NodeInfo del nodo descubierto.
     */
    void on_new_node(NewNodeCallback cb) { new_node_cb_ = std::move(cb); }

    /**
     * @brief Inicia el hilo de descubrimiento dinamico UDP.
     *
     * El hilo emite broadcasts NODE_DISCOVER periodicamente y escucha
     * respuestas NODE_ANNOUNCE para agregar nodos nuevos al registro.
     * No hace nada si el descubrimiento ya esta activo.
     *
     * @param discover_port Puerto UDP para descubrimiento (default VDP_DISCOVER_PORT).
     */
    void start_discovery(uint16_t discover_port = VDP_DISCOVER_PORT);

    /**
     * @brief Detiene el hilo de descubrimiento dinamico.
     */
    void stop_discovery();

    /**
     * @brief Emite un broadcast NODE_DISCOVER una sola vez (util para test).
     */
    void broadcast_discover();

private:
    mutable std::mutex     mtx_;        // protege nodes_
    std::vector<NodeInfo>  nodes_;      // tabla de nodos (indice = idx del nodo)
    uint64_t               local_id_;   // ID de este nodo (no se registra en la tabla)
    uint16_t               local_port_; // puerto TCP local del servidor VDP

    NewNodeCallback        new_node_cb_; // callback al descubrir un nodo nuevo

    std::atomic<bool>      disc_running_{false}; // true si el hilo de descubrimiento esta activo
    std::thread            disc_thread_;          // hilo de descubrimiento UDP

    // bucle del hilo de descubrimiento
    void discovery_loop_(uint16_t discover_port);
};

} // namespace distrib

#endif // NODE_REGISTRY_H
