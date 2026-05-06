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
 * @file node_registry.cpp
 * @brief Implementacion del registro de nodos VDP con descubrimiento estatico y dinamico.
 *
 * El descubrimiento dinamico usa UDP:
 *   - Broadcast NODE_DISCOVER cada 30 segundos.
 *   - Escucha unicast NODE_ANNOUNCE en el mismo puerto.
 *   - Los nodos nuevos se agregan al registro y se notifica via callback.
 */

#include "distrib/node_registry.h"

#include <cstring>
#include <cstdio>
#include <chrono>

#ifdef _WIN32
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0600  // Vista+: inet_ntop, inet_pton disponibles
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET udp_sock_t;
#  define INVALID_UDP_SOCK INVALID_SOCKET
#  define close_udp(s)     closesocket(s)
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
typedef int udp_sock_t;
#  define INVALID_UDP_SOCK (-1)
#  define close_udp(s)     ::close(s)
#endif

namespace distrib {

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

NodeRegistry::NodeRegistry(uint64_t local_node_id, uint16_t local_port)
    : local_id_(local_node_id)
    , local_port_(local_port)
{}

NodeRegistry::~NodeRegistry() {
    stop_discovery();
}

// ---------------------------------------------------------------------------
// Agregar nodos
// ---------------------------------------------------------------------------

uint32_t NodeRegistry::add_static(const char *ip, uint16_t port,
                                   const NodeAuthConfig &auth,
                                   const char *name) {
    std::lock_guard<std::mutex> lk(mtx_);

    // verificar si ya existe un nodo con la misma IP:puerto
    for (auto &n : nodes_) {
        if (port == n.port && std::strcmp(ip, n.ip) == 0) {
            n.is_static = true; // promover a estatico si era dinamico
            n.auth      = auth;
            if (name && name[0]) {
                std::snprintf(n.name, sizeof(n.name), "%s", name);
            }
            return n.idx;
        }
    }

    if (nodes_.size() >= VDP_MAX_NODES) return VDP_MAX_NODES; // registro lleno

    NodeInfo ni{};
    ni.idx       = static_cast<uint32_t>(nodes_.size());
    ni.node_id   = 0;             // desconocido hasta el HELLO
    ni.port      = port;
    ni.is_static = true;
    ni.state     = NodeState::UNKNOWN;
    ni.auth      = auth;
    ni.session   = nullptr;
    ni.auth_failures = 0;

    std::snprintf(ni.ip,   sizeof(ni.ip),   "%s", ip);
    std::snprintf(ni.name, sizeof(ni.name), "%s", name ? name : ip);

    nodes_.push_back(ni);
    return ni.idx;
}

uint32_t NodeRegistry::add_dynamic(uint64_t node_id, const char *ip,
                                    uint16_t port, const char *name) {
    std::lock_guard<std::mutex> lk(mtx_);

    // no sobreescribir nodos estaticos existentes
    for (auto &n : nodes_) {
        if (n.node_id == node_id) return n.idx; // ya existe
        if (port == n.port && std::strcmp(ip, n.ip) == 0) return n.idx;
    }

    if (nodes_.size() >= VDP_MAX_NODES) return VDP_MAX_NODES;

    NodeInfo ni{};
    ni.idx       = static_cast<uint32_t>(nodes_.size());
    ni.node_id   = node_id;
    ni.port      = port;
    ni.is_static = false;
    ni.state     = NodeState::UNKNOWN;
    ni.session   = nullptr;
    ni.auth_failures = 0;

    std::memset(&ni.auth, 0, sizeof(ni.auth)); // sin config de auth por defecto

    std::snprintf(ni.ip,   sizeof(ni.ip),   "%s", ip);
    std::snprintf(ni.name, sizeof(ni.name), "%s", name ? name : ip);

    nodes_.push_back(ni);
    return ni.idx;
}

// ---------------------------------------------------------------------------
// Consultas
// ---------------------------------------------------------------------------

NodeInfo *NodeRegistry::get(uint32_t idx) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (idx >= nodes_.size()) return nullptr;
    return &nodes_[idx];
}

NodeInfo *NodeRegistry::find_by_id(uint64_t node_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto &n : nodes_) {
        if (n.node_id == node_id) return &n;
    }
    return nullptr;
}

size_t NodeRegistry::count() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return nodes_.size();
}

// ---------------------------------------------------------------------------
// Actualizaciones de estado
// ---------------------------------------------------------------------------

void NodeRegistry::set_state(uint32_t idx, NodeState state) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (idx < nodes_.size()) nodes_[idx].state = state;
}

void NodeRegistry::set_session(uint32_t idx, void *session) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (idx < nodes_.size()) nodes_[idx].session = session;
}

// ---------------------------------------------------------------------------
// Descubrimiento dinamico UDP
// ---------------------------------------------------------------------------

void NodeRegistry::start_discovery(uint16_t discover_port) {
    if (disc_running_.exchange(true)) return; // ya activo

    disc_thread_ = std::thread([this, discover_port]() {
        discovery_loop_(discover_port);
    });
}

void NodeRegistry::stop_discovery() {
    disc_running_.store(false);
    if (disc_thread_.joinable()) disc_thread_.join();
}

void NodeRegistry::broadcast_discover() {
    // crear socket UDP para el broadcast
    udp_sock_t sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_UDP_SOCK) return;

    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
               reinterpret_cast<const char *>(&bcast), sizeof(bcast));

    // construir el payload NODE_DISCOVER
    VdpPayloadNodeDiscover disc{};
    disc.magic[0] = 'V'; disc.magic[1] = 'D';
    disc.magic[2] = 'P'; disc.magic[3] = 'D';
    disc.node_id   = local_id_;
    disc.reply_port = local_port_;
    disc._pad       = 0;

    sockaddr_in dest{};
    dest.sin_family      = AF_INET;
    dest.sin_port        = htons(VDP_DISCOVER_PORT);
    dest.sin_addr.s_addr = INADDR_BROADCAST;

    sendto(sock, reinterpret_cast<const char *>(&disc), sizeof(disc), 0,
           reinterpret_cast<const sockaddr *>(&dest), sizeof(dest));

    close_udp(sock);
}

void NodeRegistry::discovery_loop_(uint16_t discover_port) {
    // crear socket UDP de escucha (enlazado al puerto de descubrimiento)
    udp_sock_t sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_UDP_SOCK) {
        disc_running_.store(false);
        return;
    }

    // reutilizacion de puerto para que varios nodos en el mismo host puedan escuchar
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char *>(&reuse), sizeof(reuse));

    // habilitar broadcast en el socket de escucha
    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
               reinterpret_cast<const char *>(&bcast), sizeof(bcast));

    sockaddr_in bind_addr{};
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(discover_port);
    bind_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, reinterpret_cast<const sockaddr *>(&bind_addr), sizeof(bind_addr)) != 0) {
        close_udp(sock);
        disc_running_.store(false);
        return;
    }

    // timeout de recepcion para que el bucle pueda verificar disc_running_ periodicamente
#ifdef _WIN32
    DWORD timeout_ms = 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));
#else
    struct timeval tv{1, 0}; // 1 segundo
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char *>(&tv), sizeof(tv));
#endif

    auto last_broadcast = std::chrono::steady_clock::now();
    constexpr auto BROADCAST_INTERVAL = std::chrono::seconds(30);

    while (disc_running_.load()) {
        // emitir broadcast periodicamente
        auto now = std::chrono::steady_clock::now();
        if (now - last_broadcast >= BROADCAST_INTERVAL) {
            broadcast_discover();
            last_broadcast = now;
        }

        // escuchar respuestas NODE_ANNOUNCE
        uint8_t buf[sizeof(VdpPayloadNodeAnnounce) + 16]; // margen de seguridad
        sockaddr_in from{};
#ifdef _WIN32
        int from_len = sizeof(from);
#else
        socklen_t from_len = sizeof(from);
#endif
        int n = recvfrom(sock, reinterpret_cast<char *>(buf), sizeof(buf), 0,
                         reinterpret_cast<sockaddr *>(&from), &from_len);

        if (n < static_cast<int>(sizeof(VdpPayloadNodeDiscover))) continue;

        // obtener la IP del remitente del paquete UDP
        char ip_str[64] = {};
        const uint8_t *ab = reinterpret_cast<const uint8_t *>(&from.sin_addr);
        std::snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
                      static_cast<unsigned>(ab[0]), static_cast<unsigned>(ab[1]),
                      static_cast<unsigned>(ab[2]), static_cast<unsigned>(ab[3]));

        // comprobar si es NODE_DISCOVER ("VDPD") para responder con NODE_ANNOUNCE
        const uint8_t *magic = buf;
        if (magic[0] == 'V' && magic[1] == 'D' && magic[2] == 'P' && magic[3] == 'D') {
            const VdpPayloadNodeDiscover *disc =
                reinterpret_cast<const VdpPayloadNodeDiscover *>(buf);

            // ignorar descubrimientos propios
            if (disc->node_id == local_id_) continue;

            // responder con NODE_ANNOUNCE al mismo puerto de descubrimiento del remitente
            VdpPayloadNodeAnnounce ann{};
            ann.magic[0] = 'V'; ann.magic[1] = 'D';
            ann.magic[2] = 'P'; ann.magic[3] = 'A';
            ann.node_id   = local_id_;
            ann.vdp_port  = local_port_; // nuestro puerto TCP VDP
            ann.auth_flags = 0;
            ann._pad       = 0;
            std::snprintf(ann.node_name, sizeof(ann.node_name), "node");

            // enviar unicast al remitente en el mismo puerto de descubrimiento
            sockaddr_in reply_addr = from;
            reply_addr.sin_port = htons(discover_port);
            sendto(sock, reinterpret_cast<const char *>(&ann), sizeof(ann), 0,
                   reinterpret_cast<const sockaddr *>(&reply_addr), sizeof(reply_addr));

            // registrar el nodo descubierto (su puerto TCP esta en disc->reply_port)
            uint32_t idx = add_dynamic(disc->node_id, ip_str, disc->reply_port, "discovered");
            if (idx < VDP_MAX_NODES && new_node_cb_) {
                NodeInfo *info = get(idx);
                if (info && info->state == NodeState::UNKNOWN) {
                    new_node_cb_(*info); // notificar al DistRuntime para conectar
                }
            }
            continue;
        }

        // comprobar si es NODE_ANNOUNCE ("VDPA")
        if (n < static_cast<int>(sizeof(VdpPayloadNodeAnnounce))) continue;
        const VdpPayloadNodeAnnounce *ann =
            reinterpret_cast<const VdpPayloadNodeAnnounce *>(buf);

        if (ann->magic[0] != 'V' || ann->magic[1] != 'D' ||
            ann->magic[2] != 'P' || ann->magic[3] != 'A') continue;

        // ignorar anuncios propios
        if (ann->node_id == local_id_) continue;

        uint32_t idx = add_dynamic(ann->node_id, ip_str, ann->vdp_port, ann->node_name);
        if (idx < VDP_MAX_NODES && new_node_cb_) {
            NodeInfo *info = get(idx);
            if (info && info->state == NodeState::UNKNOWN) {
                new_node_cb_(*info); // notificar al DistRuntime para conectar
            }
        }
    }

    close_udp(sock);
}

} // namespace distrib
