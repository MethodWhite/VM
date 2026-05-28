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
 * @file dist_runtime.cpp
 * @brief Implementacion del coordinador del sistema distribuido de VestaVM.
 *
 * Notas de implementacion:
 *   - Los futures distribuidos se crean en el GcHeap del proceso solicitante.
 *   - El hilo lector de cada sesion puede escribir en FutureObject desde un
 *     hilo distinto al del scheduler; para v1 se acepta este riesgo (los campos
 *     estan alineados a 64 bits, lo que garantiza escritura atomica en x86/ARM).
 *     La solucion definitiva es una cola de resolucion procesada por el scheduler.
 *   - El reenvio at-least-once de MSGSEND usa un hilo dedicado (retry_thread_).
 *   - Para rspawn, se envia el bloque de bytecode desde fn_addr hasta
 *     fn_addr + RSPAWN_CODE_LIMIT (64 KB por defecto).  El usuario debe asegurarse
 *     de que la funcion remota sea autocontenida en ese rango.
 */

#include "distrib/dist_runtime.h"
#include "distrib/dist_debug.h"

// definicion del flag de depuracion del subsistema distribuido
bool distrib::g_dist_debug = false;
#include "distrib/mailbox.h"
#include "distrib/dirty_tracker.h"

#include "runtime/runtime.h"
#include "runtime/proceso_runtime.h"
#include "loader/oop_types.h"
#include "loader/loader.h"
#include "gc/gc_heap.h"
#include "net/tcp_server.h"

#include <cstring>
#include <cstdio>
#include <chrono>
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#endif
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace distrib {

// tamano maximo del bloque de bytecode que se envia en rspawn (64 KB)
static constexpr size_t RSPAWN_CODE_LIMIT = 64 * 1024;

// ---------------------------------------------------------------------------
// Servidor VDP interno: subclase de TCPServer que delega al DistRuntime
// ---------------------------------------------------------------------------

/**
 * @brief Conexion centinela que TCPServer puede destruir sin cerrar el socket.
 *
 * Se devuelve desde VdpServer::create_connection cuando el handshake VDP
 * tiene exito y VdpSession ya es propietaria del fd.  TCPServer llamara a
 * start() (no-op) y luego borrara este objeto; el destructor de Connection
 * llama a close_socket() sobre INVALID_SOCKET (-1), que es inofensivo.
 */
class VdpSentinelConnection : public Connection {
public:
    VdpSentinelConnection() : Connection(static_cast<socket_t>(-1)) {}
    void handle() override {} // VdpSession gestiona su propio hilo lector
};

/**
 * @brief Servidor VDP que acepta conexiones VDP entrantes.
 *
 * Cada conexion aceptada se pasa a DistRuntime para ejecutar el handshake
 * y crear una VdpSession correspondiente.
 */
class VdpServer : public TCPServer {
public:
    VdpServer(uint16_t port, TLSContext *tls, DistRuntime *dr)
        : TCPServer(port, tls), dr_(dr) {}

protected:
    /**
     * @brief Acepta una conexion y ejecuta el handshake VDP de forma asincrona.
     *
     * Sobreescribe create_connection() para crear una VdpSession directamente
     * con el fd y el SSL* (si hay TLS), ejecutar el handshake y registrarla.
     *
     * Devuelve VdpSentinelConnection (no nullptr) para que TCPServer no cierre
     * el fd: VdpSession es la propietaria real del socket y lo cerrara en su
     * destructor.  Si el handshake falla, la VdpSession destruida cierra el fd
     * y TCPServer recibe nullptr -> cierra el fd ya invalido (close de -1, no-op).
     */
    Connection *create_connection(socket_t client_fd, TLSContext *tls_ctx) override {
        // realizar handshake TLS si el servidor tiene TLS habilitado
        SSL *ssl = nullptr;
        if (tls_enabled && tls_ctx) {
            ssl = SSL_new(tls_ctx->get());
            SSL_set_fd(ssl, static_cast<int>(client_fd));
            if (SSL_accept(ssl) <= 0) {
                SSL_free(ssl);
                return nullptr; // TLS fallido; TCPServer cerrara el socket
            }
        }

        // crear sesion entrante con el socket (y SSL si aplica)
        auto *sess = new distrib::VdpSession(
            static_cast<vdp_sock_t>(client_fd),
            ssl,
            UINT32_MAX,
            dr_->local_node_id()
        );

        // ejecutar handshake VDP con la configuracion de autenticacion del servidor
        if (!sess->server_handshake(server_auth_)) {
            delete sess; // destructor: SSL_free + CLOSE_SOCK(client_fd)
            return nullptr; // TCPServer intentara close_socket, pero ya esta cerrado (no-op)
        }

        // registrar la sesion activa en el DistRuntime
        dr_->on_inbound_session_(sess);

        // devolver centinela en lugar de nullptr para que TCPServer NO cierre client_fd;
        // VdpSession es la propietaria del fd y lo cerrara en su destructor
        return new VdpSentinelConnection();
    }

public:
    NodeAuthConfig  server_auth_; // configuracion de autenticacion del servidor
    DistRuntime    *dr_;          // coordinador propietario
};

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

DistRuntime::DistRuntime(runtime::VM &vm, const DistRuntimeConfig &config)
    : vm_(vm)
    , config_(config)
    , registry_(config.local_node_id, config.vdp_listen_port)
{
    // NO generar node_id en el constructor.  El
    // RAND_bytes de OpenSSL inicializa su RNG la primera vez (~3ms en
    // Windows) y para programas sin networking (vdp_listen_port=0,
    // enable_discovery=false, sin add-node) ese coste es 100% wasted.
    // Si el usuario llama start() y aun no hay node_id, lo generamos
    // alli.  Para uso solo-local (msgsend/msgrecv intra-VM) ni siquiera
    // se necesita node_id.
    //
    // registrar callback para nuevos nodos descubiertos dinamicamente
    registry_.on_new_node([this](const NodeInfo &n) { on_new_node_(n); });
}

DistRuntime::~DistRuntime() {
    stop();
}

// ---------------------------------------------------------------------------
// Arranque y parada
// ---------------------------------------------------------------------------

bool DistRuntime::start() {
    // generar node_id aqui (no en el constructor) para
    // que los programas sin networking no paguen el coste de inicializar
    // OpenSSL RNG.  Si llegamos a start(), el usuario quiere networking y
    // necesita un node_id valido.
    if (config_.local_node_id == 0) {
        config_.local_node_id = generate_node_id_();
    }

#ifdef _WIN32
    // WSAStartup debe llamarse antes de cualquier funcion de sockets de Winsock;
    // si el TCPServer no arranca (puerto 0), nadie mas lo inicializa
    { WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa); }
#endif

    // crear y lanzar el servidor VDP
    TLSContext *tls = nullptr;
    if (config_.server_auth.use_tls &&
        config_.server_auth.cert_path[0] &&
        config_.server_auth.key_path[0]) {
        // TLSContext solo se puede crear si hay certificado y clave disponibles
        try {
            tls = new TLSContext(config_.server_auth.cert_path,
                                 config_.server_auth.key_path);
        } catch (...) {
            tls = nullptr; // fallback: operar sin TLS si no hay certificado valido
        }
    }

    // solo abrir servidor TCP si se especifico un puerto; 0 significa nodo solo-cliente
    if (config_.vdp_listen_port) {
        auto *srv = new VdpServer(config_.vdp_listen_port, tls, this);
        srv->server_auth_ = config_.server_auth;
        vdp_server_ = srv;

        if (!vdp_server_->start()) {
            delete vdp_server_;
            vdp_server_ = nullptr;
            return false;
        }
    }

    // iniciar hilo de reintentos at-least-once
    retry_running_.store(true);
    retry_thread_ = std::thread([this]() { retry_loop_(); });

    // iniciar descubrimiento dinamico si esta habilitado
    if (config_.enable_discovery) {
        uint16_t disc_port = config_.discover_port ? config_.discover_port : VDP_DISCOVER_PORT;
        registry_.start_discovery(disc_port);
    }

    return true;
}

void DistRuntime::stop() {
    // detener descubrimiento
    registry_.stop_discovery();

    // detener hilo de reintentos
    retry_running_.store(false);
    if (retry_thread_.joinable()) retry_thread_.join();

    // cerrar todas las sesiones activas
    {
        std::lock_guard<std::mutex> lk(sessions_mtx_);
        for (auto &kv : sessions_) {
            kv.second->close();
            delete kv.second;
        }
        sessions_.clear();
    }

    // detener el servidor VDP
    if (vdp_server_) {
        vdp_server_->stop();
        delete vdp_server_;
        vdp_server_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Gestion de nodos y sesiones
// ---------------------------------------------------------------------------

uint32_t DistRuntime::add_node(const char *ip, uint16_t port,
                                const NodeAuthConfig &auth,
                                const char *name) {
    uint32_t idx = registry_.add_static(ip, port, auth, name);
    if (idx < VDP_MAX_NODES) {
        // intentar conectar de forma sincrona
        connect_node(idx);
    }
    return idx;
}

bool DistRuntime::connect_node(uint32_t node_idx) {
    NodeInfo *info = registry_.get(node_idx);
    if (!info) return false;
    if (info->state == NodeState::AUTHENTICATED) return true; // ya conectado

    registry_.set_state(node_idx, NodeState::CONNECTING);

    auto *sess = new VdpSession(node_idx, config_.local_node_id);
    if (!sess->connect_to(info->ip, info->port, info->auth, config_.local_node_name)) {
        delete sess;
        registry_.set_state(node_idx, NodeState::DISCONNECTED);
        return false;
    }

    // actualizar el node_id del nodo con el ID real recibido en el HELLO
    {
        std::lock_guard<std::mutex> lk(sessions_mtx_);
        registry_.set_state(node_idx, NodeState::AUTHENTICATED);
        registry_.set_session(node_idx, sess);
        sessions_[node_idx] = sess;
    }

    // iniciar el hilo lector de la sesion con callback de desconexion
    sess->start_reader(
        [this](const VdpHeader &hdr,
               const std::vector<uint8_t> &payload,
               uint32_t nidx) {
            on_vdp_msg_(hdr, payload, nidx);
        },
        [this](uint32_t nidx) {
            // sesion caida: marcar como DESCONECTADO en el registro
            // no se elimina de sessions_ aqui porque stop() lo hace con delete seguro
            registry_.set_state(nidx, NodeState::DISCONNECTED);
            registry_.set_session(nidx, nullptr);
        }
    );

    return true;
}

// sesion entrante creada por VdpServer tras un handshake exitoso
void DistRuntime::on_inbound_session_(VdpSession *sess) {
    uint64_t remote_id = sess->remote_node_id(); // ID del nodo que se conecto a nosotros

    // buscar una entrada existente en el registro por node_id
    NodeInfo *ni = registry_.find_by_id(remote_id);
    uint32_t node_idx;
    if (ni) {
        // nodo ya conocido (configurado estaticamente o descubierto antes)
        node_idx = ni->idx;
    } else {
        // nodo desconocido: registrarlo de forma dinamica (descubrimiento inbound)
        node_idx = registry_.add_dynamic(remote_id, "", 0, "inbound");
        if (node_idx >= VDP_MAX_NODES) {
            delete sess; // registro lleno; rechazar la sesion
            return;
        }
    }

    // actualizar el indice en la sesion para que los callbacks usen el idx correcto
    sess->set_node_idx(node_idx);

    {
        std::lock_guard<std::mutex> lk(sessions_mtx_);
        sessions_[node_idx] = sess;
    }
    registry_.set_state(node_idx, NodeState::AUTHENTICATED);
    registry_.set_session(node_idx, sess);

    sess->start_reader(
        [this](const VdpHeader &hdr,
               const std::vector<uint8_t> &payload,
               uint32_t nidx) {
            on_vdp_msg_(hdr, payload, nidx);
        },
        [this](uint32_t nidx) {
            registry_.set_state(nidx, NodeState::DISCONNECTED);
            registry_.set_session(nidx, nullptr);
        }
    );
}

void DistRuntime::on_new_node_(const NodeInfo &node) {
    // conectar de forma asincrona en un hilo separado para no bloquear el discovery
    std::thread([this, idx = node.idx]() {
        connect_node(idx);
    }).detach();
}

// ---------------------------------------------------------------------------
// Instruccion rspawn
// ---------------------------------------------------------------------------

uint32_t DistRuntime::rspawn(runtime::ProcessVM *proc,
                              uint64_t fn_addr,
                              uint32_t node_idx) {
    // verificar que el nodo existe y esta autenticado
    VdpSession *sess = nullptr;
    {
        std::lock_guard<std::mutex> lk(sessions_mtx_);
        auto it = sessions_.find(node_idx);
        if (it == sessions_.end() || !it->second->is_active()) return 0xFFFFFFFF;
        sess = it->second;
    }

    // leer hasta RSPAWN_CODE_LIMIT bytes de bytecode desde fn_addr
    std::vector<uint8_t> code(RSPAWN_CODE_LIMIT, 0);
    size_t code_read = 0;
    for (size_t i = 0; i < RSPAWN_CODE_LIMIT; ++i) {
        uint8_t byte_val = proc->vm_mem[fn_addr + static_cast<uint64_t>(i)];
        if (proc->err_thread != runtime::THREAD_NO_ERROR) {
            // acceso fuera del espacio de direcciones: truncar aqui
            proc->err_thread = runtime::THREAD_NO_ERROR; // limpiar error de lectura
            break;
        }
        code[i] = byte_val;
        code_read = i + 1;
    }
    code.resize(code_read);

    // crear un FutureObject en el GC del proceso solicitante
    gc::GcHandle h = proc->gc_heap.alloc(sizeof(loader::FutureObject));
    if (h == gc::GC_NULL_HANDLE) return 0xFFFFFFFF;

    uint8_t *payload_ptr = proc->gc_heap.deref(h);
    auto *fut        = reinterpret_cast<loader::FutureObject *>(payload_ptr);
    fut->header.class_ptr  = nullptr; // sin clase OOP (future raw)
    fut->header.flags      = loader::OBJ_FLAG_GC_OWNED;
    fut->header.hash_code  = 0;
    // owner_pid / lock_depth ahora viven empacados en monitor_word; cero = unlocked.
    fut->header.monitor_word.store(0, std::memory_order_relaxed);
    fut->state      = loader::FutureState::PENDING;
    std::memset(fut->_pad, 0, sizeof(fut->_pad));
    fut->result     = 0;
    fut->waiter_pid = 0;

    // registrar el future pendiente para correlacionar con el ACK
    uint64_t future_id = next_future_id_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(futures_mtx_);
        PendingFuture pf{};
        pf.future_id   = future_id;
        pf.gc_handle   = static_cast<uint32_t>(h);
        pf.requester   = proc->pid;
        pending_futures_[future_id] = pf;
    }

    // construir y enviar el paquete VDP_RSPAWN
    VdpPayloadRspawn req{};
    req.future_id  = future_id;
    req.fn_offset  = 0; // entrypoint al inicio del bloque enviado
    req.code_size  = static_cast<uint64_t>(code_read);
    req.argc       = static_cast<uint32_t>(proc->registers.regs[15].qword());
    req.flags      = 0; // bytecode raw: el remoto usa load_raw_code
    // copiar argumentos r1..r12 segun argc
    uint32_t argc = (req.argc > 12) ? 12 : req.argc;
    for (uint32_t i = 0; i < argc; ++i) {
        req.args[i] = proc->registers.regs[1 + i].qword();
    }

    // serializar: header de rspawn + bytecode
    std::vector<uint8_t> pkt(sizeof(VdpPayloadRspawn) + code_read);
    std::memcpy(pkt.data(), &req, sizeof(VdpPayloadRspawn));
    std::memcpy(pkt.data() + sizeof(VdpPayloadRspawn), code.data(), code_read);

    if (!sess->send_msg(VdpMsgType::RSPAWN, pkt)) {
        // limpiar el future si el envio fallo
        std::lock_guard<std::mutex> lk(futures_mtx_);
        pending_futures_.erase(future_id);
        return 0xFFFFFFFF;
    }

    return static_cast<uint32_t>(h); // retornar el GcHandle del future
}

// ---------------------------------------------------------------------------
// rspawn_velb: enviar fichero .velb completo para ejecucion en remoto
// ---------------------------------------------------------------------------

uint32_t DistRuntime::rspawn_velb(runtime::ProcessVM *proc,
                                   const std::vector<uint8_t> &velb_bytes,
                                   uint32_t node_idx) {
    // verificar que el nodo existe y esta autenticado
    VdpSession *sess = nullptr;
    {
        std::lock_guard<std::mutex> lk(sessions_mtx_);
        auto it = sessions_.find(node_idx);
        if (it == sessions_.end() || !it->second->is_active()) return 0xFFFFFFFF;
        sess = it->second;
    }

    if (velb_bytes.empty() || velb_bytes.size() > VDP_MAX_PAYLOAD) return 0xFFFFFFFF;

    // crear un FutureObject en el GC del proceso solicitante
    gc::GcHandle h = proc->gc_heap.alloc(sizeof(loader::FutureObject));
    if (h == gc::GC_NULL_HANDLE) return 0xFFFFFFFF;

    uint8_t *payload_ptr = proc->gc_heap.deref(h);
    auto *fut            = reinterpret_cast<loader::FutureObject *>(payload_ptr);
    fut->header.class_ptr  = nullptr;
    fut->header.flags      = loader::OBJ_FLAG_GC_OWNED;
    fut->header.hash_code  = 0;
    // monitor_word empaqueta owner + lock_depth; cero = unlocked.
    fut->header.monitor_word.store(0, std::memory_order_relaxed);
    fut->state      = loader::FutureState::PENDING;
    std::memset(fut->_pad, 0, sizeof(fut->_pad));
    fut->result     = 0;
    fut->waiter_pid = 0;

    // registrar el future pendiente para correlacionar con FUTURE_FULFILL
    uint64_t future_id = next_future_id_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(futures_mtx_);
        PendingFuture pf{};
        pf.future_id = future_id;
        pf.gc_handle = static_cast<uint32_t>(h);
        pf.requester = proc->pid;
        pending_futures_[future_id] = pf;
    }

    // construir cabecera y enviar: RSPAWN_FLAG_VELB indica al remoto que use load_executable
    VdpPayloadRspawn req{};
    req.future_id = future_id;
    req.fn_offset = 0;
    req.code_size = static_cast<uint64_t>(velb_bytes.size());
    req.argc      = 0;
    req.flags     = RSPAWN_FLAG_VELB;
    std::memset(req.args, 0, sizeof(req.args));

    std::vector<uint8_t> pkt(sizeof(VdpPayloadRspawn) + velb_bytes.size());
    std::memcpy(pkt.data(), &req, sizeof(VdpPayloadRspawn));
    std::memcpy(pkt.data() + sizeof(VdpPayloadRspawn), velb_bytes.data(), velb_bytes.size());

    DIST_DBG("rspawn_velb: enviando %zu bytes .velb a nodo=%u future_id=%llu",
             velb_bytes.size(), node_idx, (unsigned long long)future_id);

    if (!sess->send_msg(VdpMsgType::RSPAWN, pkt)) {
        DIST_DBG("rspawn_velb: send_msg fallo para nodo=%u", node_idx);
        std::lock_guard<std::mutex> lk(futures_mtx_);
        pending_futures_.erase(future_id);
        return 0xFFFFFFFF;
    }

    return static_cast<uint32_t>(h);
}

// ---------------------------------------------------------------------------
// notify_rspawn_halt: enviar FUTURE_FULFILL al nodo origen con el valor r0
// ---------------------------------------------------------------------------

void DistRuntime::notify_rspawn_halt(runtime::ProcessVM *proc) {
    if (proc->rspawn_future_id == 0 || proc->rspawn_origin_node == 0xFFFFFFFFu) return;

    uint64_t r0 = proc->registers.regs[0].qword();

    DIST_DBG("HALT PID=(sched=%u local=%llu) r0=%llu (0x%llX) err=%d tsc=%llu "
             "future_id=%llu -> FUTURE_FULFILL a nodo=%u",
             proc->pid.scheduler_id,
             (unsigned long long)proc->pid.local_pid,
             (unsigned long long)r0, (unsigned long long)r0,
             (int)proc->err_thread,
             (unsigned long long)proc->tsc,
             (unsigned long long)proc->rspawn_future_id,
             proc->rspawn_origin_node);

    VdpSession *sess = nullptr;
    {
        std::lock_guard<std::mutex> lk(sessions_mtx_);
        auto it = sessions_.find(proc->rspawn_origin_node);
        if (it == sessions_.end() || !it->second->is_active()) {
            DIST_DBG("notify_rspawn_halt: sesion nodo=%u no activa, FUTURE_FULFILL descartado",
                     proc->rspawn_origin_node);
            return;
        }
        sess = it->second;
    }

    VdpPayloadFutureFulfill msg{};
    msg.future_id = proc->rspawn_future_id;
    msg.result    = r0;

    sess->send_raw(VdpMsgType::FUTURE_FULFILL,
                   reinterpret_cast<const uint8_t *>(&msg), sizeof(msg));
}

// ---------------------------------------------------------------------------
// Instruccion msgsend
// ---------------------------------------------------------------------------

bool DistRuntime::msgsend(runtime::ProcessVM *proc,
                           uint64_t target_pid,
                           uint64_t vm_addr,
                           uint64_t len) {
    if (len == 0 || len > VDP_MAX_PAYLOAD) return false;

    // leer el mensaje desde la memoria VM del proceso emisor
    std::vector<uint8_t> data(static_cast<size_t>(len));
    for (uint64_t i = 0; i < len; ++i) {
        data[static_cast<size_t>(i)] = proc->vm_mem[vm_addr + i];
    }

    if (vdp_is_remote_pid(target_pid)) {
        // PID remoto: enviar VDP_MSGSEND al nodo correspondiente
        uint32_t node_idx  = vdp_pid_node_idx(target_pid);
        uint32_t local_pid = vdp_pid_local(target_pid);

        VdpSession *sess = nullptr;
        {
            std::lock_guard<std::mutex> lk(sessions_mtx_);
            auto it = sessions_.find(node_idx);
            if (it == sessions_.end() || !it->second->is_active()) return false;
            sess = it->second;
        }

        uint32_t seq = sess->next_seq();

        // construir payload: VdpPayloadMsgsend + datos del mensaje
        VdpPayloadMsgsend req{};
        req.target_pid = static_cast<uint64_t>(local_pid);
        req.data_size  = len;

        std::vector<uint8_t> pkt(sizeof(VdpPayloadMsgsend) + data.size());
        std::memcpy(pkt.data(), &req, sizeof(VdpPayloadMsgsend));
        std::memcpy(pkt.data() + sizeof(VdpPayloadMsgsend), data.data(), data.size());

        if (!sess->send_raw(VdpMsgType::MSGSEND, pkt.data(), pkt.size(), seq)) return false;

        // registrar en la tabla de pendientes para reintento at-least-once
        std::lock_guard<std::mutex> lk(msgs_mtx_);
        auto &pm       = pending_msgs_[seq];
        pm.seq_num     = seq;
        pm.node_idx    = node_idx;
        pm.payload     = std::move(pkt);
        pm.retries     = 0;
        auto now       = std::chrono::steady_clock::now().time_since_epoch();
        pm.sent_at_ms  = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now).count());

        return true;
    } else {
        // PID local: depositar directamente en el mailbox del proceso destino.
        // El PID encoded contiene scheduler_id en bits 63-32 y local_pid
        // en bits 31-0 (formato emitido por exec_instr_spawn y exec_instr_getpid).
        // Extraemos ambos para localizar el proceso en el scheduler correcto.
        const uint32_t sched_id  = static_cast<uint32_t>((target_pid >> 32) & 0xFFFFFFFFu);
        const uint64_t local_pid = target_pid & 0xFFFFFFFFu;
        GlobalPID dest_gpid{};
        dest_gpid.scheduler_id = sched_id;
        dest_gpid.local_pid    = local_pid;

        runtime::ProcessVM *dest = vm_.get_process(dest_gpid);
        // Fallback: si scheduler_id apunta a un scheduler inexistente o el
        // proceso no esta ahi, escanear todos los schedulers buscando por
        // local_pid (caso de PIDs construidos a mano sin scheduler_id).
        if (!dest) {
            for (size_t s = 0; s < vm_.schedulers.size(); ++s) {
                GlobalPID try_gpid{static_cast<uint32_t>(s), local_pid};
                runtime::ProcessVM *p = vm_.get_process(try_gpid);
                if (p) { dest = p; dest_gpid = try_gpid; break; }
            }
        }
        if (!dest) return false;

        Mailbox *mb = get_or_create_mailbox(dest);
        if (!mb->push(proc->pid.local_pid, data.data(), data.size())) return false;

        // SIEMPRE invocamos make_ready tras push, sin condicional sobre
        // @c dest->state.  El propio @c make_ready usa CAS y
        // wake_pending para resolver correctamente:
        //   - dest activo (EXECUTE/RUNNING/etc): no toca state pero deja
        //     wake_pending=true; el scheduler lo respeta al transicionar
        //     a WAIT_IO via double-check.
        //   - dest durmiente (WAIT_IO/BLOCKED/NEW/HALT/DEAD): CAS a READY
        //     y reencola.
        // Esto cierra la ventana race "lost wakeup" en multi-thread real.
        vm_.make_ready(dest_gpid);

        return true;
    }
}

// ---------------------------------------------------------------------------
// Instruccion msgrecv
// ---------------------------------------------------------------------------

uint64_t DistRuntime::msgrecv(runtime::ProcessVM *proc,
                               uint64_t buf_addr,
                               uint64_t max_len) {
    Mailbox *mb = get_or_create_mailbox(proc);

    if (mb->empty()) {
        // no hay mensajes: bloquear el proceso hasta que llegue uno
        // el emisor (msgsend local o el handler de MSGSEND remoto) llamara make_ready
        proc->registers.regs[0].qword(0); // r0 = 0 mientras espera
        return 0; // la instruccion indica al scheduler que bloquee via blocking=true
    }

    MailboxMsg msg = mb->try_pop();
    if (msg.data.empty()) {
        return 0; // buzon vacio de nuevo (race poco probable): bloquear
    }

    // copiar el mensaje al buffer VM del receptor (truncar si es necesario)
    uint64_t copy_len = msg.data.size();
    if (copy_len > max_len) copy_len = max_len;

    for (uint64_t i = 0; i < copy_len; ++i) {
        // escribir byte a byte en la memoria VM del proceso receptor
        proc->vm_mem.write_u8(buf_addr + i, msg.data[static_cast<size_t>(i)]);
    }

    return copy_len; // r0 = bytes copiados
}

// ---------------------------------------------------------------------------
// Instruccion memsync
// ---------------------------------------------------------------------------

void DistRuntime::memsync(runtime::ProcessVM *proc, uint64_t params_addr) {
    // leer la estructura MemsyncParams desde la memoria VM del proceso
    MemsyncParams params{};
    for (size_t i = 0; i < sizeof(MemsyncParams); ++i) {
        reinterpret_cast<uint8_t *>(&params)[i] = proc->vm_mem[params_addr + static_cast<uint64_t>(i)];
    }

    VdpSession *sess = nullptr;
    {
        std::lock_guard<std::mutex> lk(sessions_mtx_);
        auto it = sessions_.find(params.node_idx);
        if (it == sessions_.end() || !it->second->is_active()) return;
        sess = it->second;
    }

    // crear un DirtyTracker para la region indicada
    // (en produccion se reutilizaria un tracker persistente por region)
    DirtyTracker tracker(params.local_addr, params.len);

    // detectar paginas modificadas usando la funcion lambda de lectura de VM memory
    auto dirty = tracker.detect([&proc](uint64_t addr, uint8_t *buf, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            buf[i] = proc->vm_mem[addr + static_cast<uint64_t>(i)];
        }
    });

    if (dirty.empty()) return; // nada que sincronizar

    // construir la tabla de hashes para VDP_MEMSYNC_HASH
    VdpPayloadMemsyncHash hash_hdr{};
    hash_hdr.local_addr   = params.local_addr;
    hash_hdr.remote_addr  = params.remote_addr;
    hash_hdr.region_size  = params.len;
    hash_hdr.page_count   = static_cast<uint32_t>(dirty.size());

    std::vector<uint8_t> hash_pkt(sizeof(VdpPayloadMemsyncHash) +
                                   dirty.size() * sizeof(VdpPageHash));
    std::memcpy(hash_pkt.data(), &hash_hdr, sizeof(VdpPayloadMemsyncHash));

    uint8_t *hash_ptr = hash_pkt.data() + sizeof(VdpPayloadMemsyncHash);
    for (const DirtyPage &dp : dirty) {
        VdpPageHash ph{};
        ph.page_idx = dp.page_idx;
        ph.adler32  = DirtyTracker::adler32(
            nullptr, 0); // adler32 del tracker (calculado en detect, no disponible aqui)
        std::memcpy(ph.blake2s, dp.blake2s, DT_BLAKE2S_LEN);
        std::memcpy(hash_ptr, &ph, sizeof(VdpPageHash));
        hash_ptr += sizeof(VdpPageHash);
    }

    uint32_t seq = sess->next_seq();
    if (!sess->send_raw(VdpMsgType::MEMSYNC_HASH,
                         hash_pkt.data(), hash_pkt.size(), seq)) return;

    // la respuesta (MEMSYNC_DIFF + MEMSYNC_ACK) llega via el hilo lector
    // y se procesa en handle_memsync_data_.
    // Para el caso sincrono (future_handle == 0), el proceso queda bloqueado
    // hasta que el hilo lector resuelva el future o senalice la condicion.
    // En esta implementacion v1, el caso sincrono no bloquea el proceso;
    // se usa siempre el modo asincrono (future_handle se ignora si es 0).
    // TODO v2: implementar memsync sincrono con condicion de espera por proceso.

    tracker.update_baseline(dirty); // actualizar baseline para el proximo memsync
}

// ---------------------------------------------------------------------------
// Obtencion / creacion de mailbox
// ---------------------------------------------------------------------------

Mailbox *DistRuntime::get_or_create_mailbox(runtime::ProcessVM *proc) {
    if (!proc->mailbox) {
        proc->mailbox = new Mailbox();
    }
    return proc->mailbox;
}

// ---------------------------------------------------------------------------
// Manejador central de mensajes VDP entrantes
// ---------------------------------------------------------------------------

void DistRuntime::on_vdp_msg_(const VdpHeader &hdr,
                               const std::vector<uint8_t> &payload,
                               uint32_t node_idx) {
    VdpMsgType type = static_cast<VdpMsgType>(hdr.msg_type);

    switch (type) {
    case VdpMsgType::RSPAWN_ACK:
        if (payload.size() >= sizeof(VdpPayloadRspawnAck)) {
            handle_rspawn_ack_(
                *reinterpret_cast<const VdpPayloadRspawnAck *>(payload.data()));
        }
        break;

    case VdpMsgType::RSPAWN:
        if (payload.size() >= sizeof(VdpPayloadRspawn)) {
            const VdpPayloadRspawn *req =
                reinterpret_cast<const VdpPayloadRspawn *>(payload.data());
            const uint8_t *code =
                payload.data() + sizeof(VdpPayloadRspawn);
            size_t code_size =
                (payload.size() > sizeof(VdpPayloadRspawn))
                    ? payload.size() - sizeof(VdpPayloadRspawn)
                    : 0;
            handle_rspawn_(node_idx, *req, code, code_size);
        }
        break;

    case VdpMsgType::MSGSEND:
        if (payload.size() >= sizeof(VdpPayloadMsgsend)) {
            const VdpPayloadMsgsend *req =
                reinterpret_cast<const VdpPayloadMsgsend *>(payload.data());
            const uint8_t *data =
                payload.data() + sizeof(VdpPayloadMsgsend);
            size_t data_size =
                (payload.size() > sizeof(VdpPayloadMsgsend))
                    ? payload.size() - sizeof(VdpPayloadMsgsend)
                    : 0;
            handle_msgsend_(node_idx, *req, data, data_size, hdr.seq_num);
        }
        break;

    case VdpMsgType::MSGSEND_ACK:
        if (payload.size() >= sizeof(VdpPayloadMsgsendAck)) {
            handle_msgsend_ack_(
                *reinterpret_cast<const VdpPayloadMsgsendAck *>(payload.data()));
        }
        break;

    case VdpMsgType::MEMSYNC_DATA:
        if (payload.size() >= sizeof(VdpPayloadMemsyncData)) {
            const VdpPayloadMemsyncData *req =
                reinterpret_cast<const VdpPayloadMemsyncData *>(payload.data());
            const uint8_t *data =
                payload.data() + sizeof(VdpPayloadMemsyncData);
            size_t data_size =
                (payload.size() > sizeof(VdpPayloadMemsyncData))
                    ? payload.size() - sizeof(VdpPayloadMemsyncData)
                    : 0;
            handle_memsync_data_(node_idx, *req, data, data_size, hdr.seq_num);
        }
        break;

    case VdpMsgType::MEMSYNC_HASH:
        if (payload.size() >= sizeof(VdpPayloadMemsyncHash)) {
            const VdpPayloadMemsyncHash *req =
                reinterpret_cast<const VdpPayloadMemsyncHash *>(payload.data());
            const VdpPageHash *hashes =
                reinterpret_cast<const VdpPageHash *>(
                    payload.data() + sizeof(VdpPayloadMemsyncHash));
            size_t hash_count =
                (payload.size() - sizeof(VdpPayloadMemsyncHash)) / sizeof(VdpPageHash);
            handle_memsync_hash_(node_idx, *req, hashes, hash_count, hdr.seq_num);
        }
        break;

    case VdpMsgType::FUTURE_FULFILL:
        if (payload.size() >= sizeof(VdpPayloadFutureFulfill)) {
            handle_future_fulfill_(node_idx,
                *reinterpret_cast<const VdpPayloadFutureFulfill *>(payload.data()));
        }
        break;

    case VdpMsgType::FUTURE_REJECT:
        if (payload.size() >= sizeof(VdpPayloadFutureReject)) {
            handle_future_reject_(node_idx,
                *reinterpret_cast<const VdpPayloadFutureReject *>(payload.data()));
        }
        break;

    case VdpMsgType::PING: {
        // responder al PING con un PONG
        std::lock_guard<std::mutex> lk(sessions_mtx_);
        auto it = sessions_.find(node_idx);
        if (it != sessions_.end()) {
            it->second->send_msg(VdpMsgType::PONG, {});
        }
        break;
    }

    default:
        break; // mensajes desconocidos: ignorar silenciosamente
    }
}

// ---------------------------------------------------------------------------
// Manejadores especificos por tipo de mensaje
// ---------------------------------------------------------------------------

void DistRuntime::handle_rspawn_ack_(const VdpPayloadRspawnAck &ack) {
    if (ack.error_code != 0) {
        DIST_DBG("RSPAWN_ACK error: future_id=%llu error=%u",
                 (unsigned long long)ack.future_id, ack.error_code);
        // rspawn remoto fallo: rechazar el future local con el codigo de error
        resolve_future_(ack.future_id, ack.error_code, true);
    } else {
        DIST_DBG("RSPAWN_ACK OK: future_id=%llu remote_pid=%llu (proceso en marcha, "
                 "esperando FUTURE_FULFILL con r0)",
                 (unsigned long long)ack.future_id, (unsigned long long)ack.remote_pid);
    }
    // el future NO se resuelve aqui; se espera VDP_FUTURE_FULFILL cuando
    // el proceso remoto termine, que llevara el valor real de r0.
}

void DistRuntime::handle_rspawn_(uint32_t node_idx,
                                  const VdpPayloadRspawn &req,
                                  const uint8_t *code, size_t code_size) {
    runtime::ProcessVM *new_proc = nullptr;
    GlobalPID           new_pid  = {};

    auto send_ack_fail = [&](uint32_t err) {
        VdpPayloadRspawnAck ack{};
        ack.future_id  = req.future_id;
        ack.remote_pid = 0;
        ack.error_code = err;
        std::lock_guard<std::mutex> lk(sessions_mtx_);
        auto it = sessions_.find(node_idx);
        if (it != sessions_.end())
            it->second->send_raw(VdpMsgType::RSPAWN_ACK,
                                  reinterpret_cast<const uint8_t *>(&ack), sizeof(ack));
    };

    std::vector<uint8_t> code_vec(code, code + code_size);

    DIST_DBG("RSPAWN recibido: nodo=%u future_id=%llu size=%zu flags=0x%X argc=%u",
             node_idx, (unsigned long long)req.future_id,
             code_size, req.flags, req.argc);

    if (req.flags & RSPAWN_FLAG_VELB) {
        // el payload es un fichero .velb completo: usar load_executable para
        // que las importaciones (calln) se resuelvan con las bibliotecas de este nodo
        try {
            new_proc = vm_.loader_public.load_executable(vm_, code_vec);
        } catch (const std::exception &e) {
            DIST_DBG("RSPAWN: load_executable excepcion: %s", e.what());
            new_proc = nullptr;
        } catch (...) {
            DIST_DBG("RSPAWN: load_executable excepcion desconocida");
            new_proc = nullptr;
        }
        if (!new_proc) {
            DIST_DBG("RSPAWN: load_executable devolvio nullptr -> SPAWN_FAILED");
            send_ack_fail(static_cast<uint32_t>(VdpError::SPAWN_FAILED));
            return;
        }
        new_pid = new_proc->pid;
        DIST_DBG("RSPAWN: proceso creado PID=(sched=%u local=%llu) PC=0x%llX",
                 new_pid.scheduler_id, (unsigned long long)new_pid.local_pid,
                 (unsigned long long)new_proc->registers.rip.raw());

        // copiar argumentos a los registros
        uint32_t argc = (req.argc > 12) ? 12 : req.argc;
        for (uint32_t i = 0; i < argc; ++i)
            new_proc->registers.regs[1 + i].qword(req.args[i]);
        new_proc->registers.regs[15].qword(req.argc);

        // establecer los campos de tracking ANTES de make_ready para evitar la carrera:
        // el scheduler puede ejecutar el proceso hasta HALT antes de que retornemos
        new_proc->rspawn_future_id   = req.future_id;
        new_proc->rspawn_origin_node = node_idx;

        // load_executable deja el proceso en NEW; pasarlo a READY para que el scheduler lo ejecute
        vm_.make_ready(new_pid);
        DIST_DBG("RSPAWN: make_ready() -> proceso listo para ejecucion");
    } else {
        // bytecode raw (sin resolucion de importaciones): carga directa
        uint64_t code_addr = 0x100000000ULL;
        new_pid  = vm_.spawn_process();
        new_proc = vm_.get_process(new_pid);
        if (!new_proc) {
            DIST_DBG("RSPAWN (raw): spawn_process() fallo -> SPAWN_FAILED");
            send_ack_fail(static_cast<uint32_t>(VdpError::SPAWN_FAILED));
            return;
        }

        new_proc->load_raw_code(code_addr, code_vec);

        uint32_t argc = (req.argc > 12) ? 12 : req.argc;
        for (uint32_t i = 0; i < argc; ++i)
            new_proc->registers.regs[1 + i].qword(req.args[i]);
        new_proc->registers.regs[15].qword(req.argc);
        new_proc->registers.rip.qword(code_addr + req.fn_offset);

        // establecer campos de tracking antes de make_ready (misma razon de carrera)
        new_proc->rspawn_future_id   = req.future_id;
        new_proc->rspawn_origin_node = node_idx;

        DIST_DBG("RSPAWN (raw): proceso creado PID=(sched=%u local=%llu) PC=0x%llX",
                 new_pid.scheduler_id, (unsigned long long)new_pid.local_pid,
                 (unsigned long long)new_proc->registers.rip.raw());
        vm_.make_ready(new_pid);
        DIST_DBG("RSPAWN (raw): make_ready() -> proceso listo");
    }

    // enviar ACK con el PID local del proceso creado
    VdpPayloadRspawnAck ack{};
    ack.future_id  = req.future_id;
    ack.remote_pid = new_proc->pid.local_pid;
    ack.error_code = 0;

    {
        std::lock_guard<std::mutex> lk(sessions_mtx_);
        auto it = sessions_.find(node_idx);
        if (it != sessions_.end())
            it->second->send_raw(VdpMsgType::RSPAWN_ACK,
                                  reinterpret_cast<const uint8_t *>(&ack), sizeof(ack));
    }
}

void DistRuntime::handle_msgsend_(uint32_t node_idx,
                                   const VdpPayloadMsgsend &req,
                                   const uint8_t *data, size_t data_size,
                                   uint32_t seq_num) {
    // buscar el proceso destino por su local_pid
    GlobalPID dest{};
    dest.scheduler_id = 0;
    dest.local_pid    = req.target_pid;

    runtime::ProcessVM *proc = vm_.get_process(dest);

    bool delivered = false;
    if (proc) {
        Mailbox *mb = get_or_create_mailbox(proc);
        delivered = mb->push(node_idx, data, data_size); // node_idx como "PID remoto" del emisor

        // SIEMPRE make_ready tras push exitoso para cerrar
        // race "lost wakeup" en multi-thread real.  Ver dist_runtime::msgsend
        // local para detalles.
        if (delivered) {
            vm_.make_ready(dest);
        }
    }

    // enviar ACK (at-least-once: el emisor reintenta si no recibe este ACK)
    VdpPayloadMsgsendAck ack{};
    ack.seq_num    = seq_num;
    ack.error_code = delivered ? 0 : static_cast<uint32_t>(VdpError::PROCESS_NOT_FOUND);

    std::lock_guard<std::mutex> lk(sessions_mtx_);
    auto it = sessions_.find(node_idx);
    if (it != sessions_.end()) {
        it->second->send_raw(VdpMsgType::MSGSEND_ACK,
                              reinterpret_cast<const uint8_t *>(&ack), sizeof(ack));
    }
}

void DistRuntime::handle_msgsend_ack_(const VdpPayloadMsgsendAck &ack) {
    // mensaje entregado: eliminar de la tabla de pendientes
    std::lock_guard<std::mutex> lk(msgs_mtx_);
    pending_msgs_.erase(ack.seq_num);
}

void DistRuntime::handle_memsync_hash_(uint32_t node_idx,
                                        const VdpPayloadMemsyncHash &req,
                                        const VdpPageHash *hashes, size_t hash_count,
                                        uint32_t seq_num) {
    // el nodo receptor compara los hashes recibidos con sus propias paginas
    // y responde con MEMSYNC_DIFF indicando cuales difieren.
    // En v1: se indica que TODAS las paginas difieren (respuesta conservadora).
    // En v2: se lee la memoria VM local en req.remote_addr y se comparan hashes.

    VdpPayloadMemsyncDiff diff_hdr{};
    diff_hdr.diff_count = static_cast<uint32_t>(hash_count);

    std::vector<uint8_t> diff_pkt(sizeof(VdpPayloadMemsyncDiff) +
                                   hash_count * sizeof(uint32_t));
    std::memcpy(diff_pkt.data(), &diff_hdr, sizeof(VdpPayloadMemsyncDiff));

    uint32_t *idx_ptr = reinterpret_cast<uint32_t *>(
        diff_pkt.data() + sizeof(VdpPayloadMemsyncDiff));
    for (size_t i = 0; i < hash_count; ++i) {
        idx_ptr[i] = hashes[i].page_idx;
    }

    std::lock_guard<std::mutex> lk(sessions_mtx_);
    auto it = sessions_.find(node_idx);
    if (it != sessions_.end()) {
        it->second->send_raw(VdpMsgType::MEMSYNC_DIFF,
                              diff_pkt.data(), diff_pkt.size(), seq_num);
    }
}

void DistRuntime::handle_memsync_data_(uint32_t /*node_idx*/,
                                        const VdpPayloadMemsyncData &req,
                                        const uint8_t *data, size_t data_size,
                                        uint32_t /*seq_num*/) {
    // escribir los datos de paginas recibidos en la VM memory del proceso receptor
    // En v1: no hay contexto de proceso asociado a la sesion, asi que se
    // necesitaria un mecanismo de correlacion (futuro improvement).
    // Por ahora se registra en el log.
    std::fprintf(stderr,
        "[dist_runtime] memsync_data: %u paginas hacia 0x%llx (%zu bytes)\n",
        req.page_count,
        static_cast<unsigned long long>(req.remote_addr),
        data_size);
    (void)data; // evitar warning de unused en v1
}

void DistRuntime::handle_future_fulfill_(uint32_t /*node_idx*/,
                                          const VdpPayloadFutureFulfill &msg) {
    DIST_DBG("FUTURE_FULFILL recibido: future_id=%llu result=%llu (0x%llX)",
             (unsigned long long)msg.future_id,
             (unsigned long long)msg.result,
             (unsigned long long)msg.result);
    resolve_future_(msg.future_id, msg.result, false);
}

void DistRuntime::handle_future_reject_(uint32_t /*node_idx*/,
                                         const VdpPayloadFutureReject &msg) {
    resolve_future_(msg.future_id, msg.error_code, true);
}

// ---------------------------------------------------------------------------
// Resolucion de futuros distribuidos
// ---------------------------------------------------------------------------

void DistRuntime::resolve_future_(uint64_t future_id, uint64_t result, bool rejected) {
    PendingFuture pf{};
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(futures_mtx_);
        auto it = pending_futures_.find(future_id);
        if (it != pending_futures_.end()) {
            pf    = it->second;
            found = true;
            pending_futures_.erase(it);
        }
    }
    if (!found) return;

    // encontrar el proceso solicitante
    runtime::ProcessVM *proc = vm_.get_process(pf.requester);
    if (!proc) return;

    // resolver el FutureObject en el GC del proceso solicitante
    uint8_t *payload_ptr = proc->gc_heap.deref(pf.gc_handle);
    if (!payload_ptr) return;

    auto *fut = reinterpret_cast<loader::FutureObject *>(payload_ptr);
    fut->state  = rejected ? loader::FutureState::REJECTED : loader::FutureState::RESOLVED;
    fut->result = result;

    if (fut->waiter_pid != 0) {
        GlobalPID target{};
        target.scheduler_id = static_cast<uint32_t>(fut->waiter_pid >> 32);
        target.local_pid    = fut->waiter_pid & 0xFFFFFFFFULL;
        vm_.make_ready(target);
        fut->waiter_pid = 0;
    }
}

// ---------------------------------------------------------------------------
// Hilo de reintentos at-least-once
// ---------------------------------------------------------------------------

void DistRuntime::retry_loop_() {
    while (retry_running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        auto now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        std::lock_guard<std::mutex> lk(msgs_mtx_);
        for (auto it = pending_msgs_.begin(); it != pending_msgs_.end(); ) {
            PendingMsg &pm = it->second;

            if (now_ms - pm.sent_at_ms < PendingMsg::RETRY_MS) {
                ++it;
                continue;
            }

            if (pm.retries >= PendingMsg::MAX_RETRIES) {
                // demasiados reintentos: descartar el mensaje
                it = pending_msgs_.erase(it);
                continue;
            }

            // reenviar el mensaje
            VdpSession *sess = nullptr;
            {
                std::lock_guard<std::mutex> lk2(sessions_mtx_);
                auto sit = sessions_.find(pm.node_idx);
                if (sit != sessions_.end() && sit->second->is_active()) {
                    sess = sit->second;
                }
            }

            if (sess) {
                sess->send_raw(VdpMsgType::MSGSEND,
                               pm.payload.data(), pm.payload.size(), pm.seq_num);
                pm.sent_at_ms = now_ms;
                ++pm.retries;
                ++it;
            } else {
                // nodo desconectado: descartar
                it = pending_msgs_.erase(it);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Generacion de node_id
// ---------------------------------------------------------------------------

uint64_t DistRuntime::generate_node_id_() {
    // combinar timestamp de alta resolucion con bytes aleatorios
    uint64_t ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    uint64_t rnd = 0;
    RAND_bytes(reinterpret_cast<uint8_t *>(&rnd), 8);

    return ts ^ rnd; // XOR para mezclar las dos fuentes de entropia
}

} // namespace distrib
