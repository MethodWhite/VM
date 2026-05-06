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
 * @file debugger.cpp
 * @brief Implementacion del servidor de depuracion TCP de VestaVM.
 *
 * Protocolo de framing:
 *   [uint32_t len LE (4 bytes)][payload JSON UTF-8 (len bytes)]
 *
 * El servidor acepta multiples clientes simultaneos.  Cada cliente se
 * atiende en un hilo independiente.  Las operaciones sobre breakpoints
 * y estados de proceso estan protegidas por mutex.
 *
 * El metodo on_before_exec() es el punto de intercepcion critico: se
 * llama desde el hilo del scheduler antes de cada instruccion.  Su
 * implementacion usa un atomic<bool> (any_bp_) como fast path para el
 * caso sin breakpoints, evitando contention de mutex en produccion.
 *
 * Plataformas soportadas:
 *   - POSIX (Linux, macOS): sockets BSD via sys/socket.h
 *   - Windows: Winsock2 via winsock2.h
 *     La abstraccion se hace con las macros SOCK_T y CLOSE_SOCK.
 */

#include "debug/debugger.h"
#include "runtime/proceso_runtime.h"
#include "runtime/runtime.h"

#include <cstring>
#include <cstdio>
#include <sstream>
#include <algorithm>

/* =========================================================================
 * Abstraccion de sockets multiplataforma
 * ========================================================================= */

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   typedef SOCKET SOCK_T;
#  define CLOSE_SOCK(s) closesocket(s)
#  define INVALID_SOCK  INVALID_SOCKET
#  define SOCK_ERR      SOCKET_ERROR
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <unistd.h>
#  include <fcntl.h>
   typedef int SOCK_T;
#  define CLOSE_SOCK(s) ::close(s)
#  define INVALID_SOCK  (-1)
#  define SOCK_ERR      (-1)
#endif

namespace debug {

    /* =====================================================================
     * Inicializacion de Winsock (no-op en POSIX)
     * ===================================================================== */

#ifdef _WIN32
    /**
     * @brief RAII para inicializar y finalizar Winsock en Windows.
     */
    struct WinsockInit {
        WinsockInit() {
            WSADATA wd;
            WSAStartup(MAKEWORD(2, 2), &wd);
        }
        ~WinsockInit() { WSACleanup(); }
    };
    static WinsockInit g_winsock_init;
#endif

    /* =====================================================================
     * Serializacion JSON minima (sin dependencias externas)
     *
     * Para el protocolo de depuracion usamos una serializacion JSON manual
     * simple: suficiente para los mensajes del protocolo sin vincular una
     * libreria externa.
     * ===================================================================== */

    /**
     * @brief Escapa una cadena para incluirla en JSON.
     * @param s Cadena de entrada.
     * @return Cadena con backslashes y comillas escapados.
     */
    static std::string json_escape(const std::string &s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:   out += c;      break;
            }
        }
        return out;
    }

    /**
     * @brief Extrae el valor de un campo JSON simple: {"key": "value"}.
     *
     * Implementacion basica para el protocolo: busca la clave y extrae
     * el valor entre comillas o numerico que le sigue.
     *
     * @param json  Cadena JSON.
     * @param key   Nombre del campo (sin comillas).
     * @return Valor extraido como cadena, o "" si no se encuentra.
     */
    static std::string json_get(const std::string &json, const std::string &key) {
        // buscar "key":
        std::string search = "\"" + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return "";
        pos += search.size();
        // saltar espacios y ':'
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) pos++;
        if (pos >= json.size()) return "";
        if (json[pos] == '"') {
            // valor de cadena
            pos++;
            size_t end = json.find('"', pos);
            if (end == std::string::npos) return json.substr(pos);
            return json.substr(pos, end - pos);
        }
        // valor numerico o booleano: hasta ',', '}' o whitespace
        size_t end = json.find_first_of(",} \t\n", pos);
        return json.substr(pos, end == std::string::npos ? std::string::npos
                                                         : end - pos);
    }

    /* =====================================================================
     * debug_cmd_parse
     * ===================================================================== */

    /**
     * @brief Convierte el nombre de un comando a DebugCmd.
     */
    DebugCmd debug_cmd_parse(const std::string &name) {
        static const struct { const char *n; DebugCmd c; } table[] = {
            {"attach",      DebugCmd::ATTACH},
            {"detach",      DebugCmd::DETACH},
            {"list_procs",  DebugCmd::LIST_PROCS},
            {"set_break",   DebugCmd::SET_BREAK},
            {"del_break",   DebugCmd::DEL_BREAK},
            {"list_breaks", DebugCmd::LIST_BREAKS},
            {"continue",    DebugCmd::CONTINUE},
            {"step",        DebugCmd::STEP},
            {"next",        DebugCmd::NEXT},
            {"registers",   DebugCmd::REGISTERS},
            {"memory",      DebugCmd::MEMORY},
            {"stack",       DebugCmd::STACK},
            {"info_proc",   DebugCmd::INFO_PROC},
            {"eval",        DebugCmd::EVAL},
            {"pause",       DebugCmd::PAUSE},
        };
        for (const auto &e : table) {
            if (name == e.n) return e.c;
        }
        return DebugCmd::UNKNOWN;
    }

    /* =====================================================================
     * Debugger: constructor / destructor
     * ===================================================================== */

    Debugger::Debugger(runtime::VM &vm)
        : vm_(vm), port_(DBG_DEFAULT_PORT), server_fd_(INVALID_SOCK) {}

    Debugger::~Debugger() {
        stop();
    }

    /* =====================================================================
     * start / stop
     * ===================================================================== */

    /**
     * @brief Inicia el servidor TCP de depuracion.
     */
    bool Debugger::start(uint16_t port) {
        if (running_.load()) return true; // ya activo

        port_ = port;

        // crear socket de escucha
        server_fd_ = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
        if (server_fd_ == static_cast<int>(INVALID_SOCK)) return false;

        // SO_REUSEADDR para reiniciar rapido
        int opt = 1;
        ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char *>(&opt), sizeof(opt));

        // desactivar Nagle para baja latencia en mensajes pequenos
        ::setsockopt(server_fd_, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char *>(&opt), sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port_);

        if (::bind(server_fd_, reinterpret_cast<struct sockaddr *>(&addr),
                   sizeof(addr)) == SOCK_ERR) {
            CLOSE_SOCK(server_fd_);
            server_fd_ = static_cast<int>(INVALID_SOCK);
            return false;
        }
        if (::listen(server_fd_, 4) == SOCK_ERR) {
            CLOSE_SOCK(server_fd_);
            server_fd_ = static_cast<int>(INVALID_SOCK);
            return false;
        }

        running_.store(true);
        accept_thread_ = std::thread(&Debugger::accept_loop, this);
        return true;
    }

    /**
     * @brief Detiene el servidor y cierra todas las conexiones.
     */
    void Debugger::stop() {
        if (!running_.exchange(false)) return; // ya parado

        // cerrar socket de escucha para desbloquear accept()
        if (server_fd_ != static_cast<int>(INVALID_SOCK)) {
            CLOSE_SOCK(server_fd_);
            server_fd_ = static_cast<int>(INVALID_SOCK);
        }

        if (accept_thread_.joinable()) accept_thread_.join();

        // cerrar sockets de clientes
        {
            std::lock_guard<std::mutex> lk(client_mutex_);
            for (int fd : client_fds_) CLOSE_SOCK(fd);
            client_fds_.clear();
        }

        // reanudar todos los procesos pausados
        {
            std::lock_guard<std::mutex> lk(proc_mutex_);
            for (auto &kv : proc_ctx_) {
                kv.second.state    = DbgProcState::DETACHED;
                kv.second.step_mode = false;
                kv.second.pause_cv.notify_all();
            }
        }
    }

    /* =====================================================================
     * accept_loop
     * ===================================================================== */

    /**
     * @brief Bucle de aceptacion de conexiones TCP.
     */
    void Debugger::accept_loop() {
        while (running_.load()) {
            struct sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = static_cast<int>(
                ::accept(server_fd_,
                         reinterpret_cast<struct sockaddr *>(&client_addr),
                         &client_len));
            if (client_fd == static_cast<int>(INVALID_SOCK)) {
                if (!running_.load()) break; // servidor detenido
                continue;
            }
            // registrar cliente
            {
                std::lock_guard<std::mutex> lk(client_mutex_);
                client_fds_.push_back(client_fd);
            }
            // lanzar hilo por cliente
            std::thread t(&Debugger::client_loop, this, client_fd);
            t.detach(); // se limpia solo al cerrar
        }
    }

    /* =====================================================================
     * Funciones de envio/recepcion con framing de longitud
     * ===================================================================== */

    /**
     * @brief Lee exactamente n bytes del socket.
     * @return true si se leyeron todos los bytes.
     */
    static bool recv_exact(int fd, void *buf, size_t n) {
        size_t total = 0;
        char *p = static_cast<char *>(buf);
        while (total < n) {
            int r = static_cast<int>(::recv(fd, p + total,
                                            static_cast<int>(n - total), 0));
            if (r <= 0) return false;
            total += static_cast<size_t>(r);
        }
        return true;
    }

    /**
     * @brief Envia exactamente n bytes por el socket.
     * @return true si se enviaron todos.
     */
    static bool send_exact(int fd, const void *buf, size_t n) {
        size_t total = 0;
        const char *p = static_cast<const char *>(buf);
        while (total < n) {
            int s = static_cast<int>(::send(fd, p + total,
                                            static_cast<int>(n - total), 0));
            if (s <= 0) return false;
            total += static_cast<size_t>(s);
        }
        return true;
    }

    /**
     * @brief Lee un mensaje con framing de longitud del socket.
     * @param fd  Socket.
     * @param out Cadena de salida con el payload JSON.
     * @return true si se leyo el mensaje completo.
     */
    static bool recv_msg(int fd, std::string &out) {
        uint32_t len_le = 0;
        if (!recv_exact(fd, &len_le, 4)) return false;
        // convertir de LE a host
        uint32_t len = ((len_le      ) & 0xFF)       |
                       ((len_le >>  8) & 0xFF) <<  8 |
                       ((len_le >> 16) & 0xFF) << 16 |
                       ((len_le >> 24) & 0xFF) << 24;
        if (len == 0 || len > DBG_MAX_MSG_SIZE) return false;
        out.resize(len);
        return recv_exact(fd, &out[0], len);
    }

    /**
     * @brief Envia un payload JSON con framing de longitud.
     */
    void Debugger::send_msg(int client_fd, const std::string &json) {
        uint32_t len = static_cast<uint32_t>(json.size());
        // serializar longitud en LE
        uint8_t hdr[4] = {
            static_cast<uint8_t>(len        & 0xFF),
            static_cast<uint8_t>((len >>  8) & 0xFF),
            static_cast<uint8_t>((len >> 16) & 0xFF),
            static_cast<uint8_t>((len >> 24) & 0xFF)
        };
        send_exact(client_fd, hdr, 4);
        send_exact(client_fd, json.c_str(), len);
    }

    /**
     * @brief Emite un evento JSON a todos los clientes conectados.
     */
    void Debugger::broadcast_event(const std::string &json) {
        std::lock_guard<std::mutex> lk(client_mutex_);
        for (int fd : client_fds_) {
            send_msg(fd, json);
        }
    }

    /* =====================================================================
     * client_loop
     * ===================================================================== */

    /**
     * @brief Bucle de atencion a un cliente conectado.
     */
    void Debugger::client_loop(int client_fd) {
        // handshake: enviar magic de identificacion
        char handshake[8];
        uint32_t magic = DBG_HANDSHAKE_MAGIC;
        std::memcpy(handshake, &magic, 4);
        // version del protocolo en los 4 bytes siguientes
        uint32_t proto_ver = 1;
        std::memcpy(handshake + 4, &proto_ver, 4);
        send_exact(client_fd, handshake, 8);

        // bucle de comandos
        std::string msg;
        while (running_.load()) {
            if (!recv_msg(client_fd, msg)) break; // cliente cerro la conexion
            handle_command(msg, client_fd);
        }

        // limpiar
        {
            std::lock_guard<std::mutex> lk(client_mutex_);
            client_fds_.erase(
                std::remove(client_fds_.begin(), client_fds_.end(), client_fd),
                client_fds_.end());
        }
        CLOSE_SOCK(client_fd);
    }

    /* =====================================================================
     * get_or_create_ctx
     * ===================================================================== */

    /**
     * @brief Obtiene o crea el contexto de depuracion de un proceso.
     */
    DbgProcCtx &Debugger::get_or_create_ctx(uint64_t pid) {
        auto it = proc_ctx_.find(pid);
        if (it != proc_ctx_.end()) return it->second;
        // emplace construye en-sitio: mutex y condition_variable no son movibles
        auto res = proc_ctx_.emplace(std::piecewise_construct,
                                     std::forward_as_tuple(pid),
                                     std::forward_as_tuple());
        DbgProcCtx &ctx = res.first->second;
        ctx.pid        = pid;
        ctx.state      = DbgProcState::DETACHED;
        ctx.step_mode  = false;
        ctx.next_mode  = false;
        ctx.call_depth = 0;
        return ctx;
    }

    /* =====================================================================
     * find_breakpoint
     * ===================================================================== */

    Breakpoint *Debugger::find_breakpoint(uint64_t pc, uint64_t pid) {
        for (auto &bp : breakpoints_) {
            if (!bp.enabled) continue;
            if (bp.addr != pc) continue;
            if (bp.pid != 0 && bp.pid != pid) continue;
            return &bp;
        }
        return nullptr;
    }

    /* =====================================================================
     * on_before_exec: punto de intercepcion critico
     * ===================================================================== */

    /**
     * @brief Llamado antes de ejecutar cada instruccion.
     *
     * Fast path: si any_bp_ es false y el proceso no esta en step_mode,
     * retorna inmediatamente sin adquirir ningun mutex.
     */
    void Debugger::on_before_exec(runtime::ProcessVM *proc) {
        if (!proc) return;

        uint64_t pid = proc->pid.local_pid; // PID local del proceso
        uint64_t pc  = proc->registers.rip.raw(); // Program Counter actual

        // fast path: sin breakpoints y sin step mode
        if (!any_bp_.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lk(proc_mutex_);
            auto it = proc_ctx_.find(pid);
            if (it == proc_ctx_.end() ||
                (!it->second.step_mode && it->second.state != DbgProcState::PAUSED)) {
                return;
            }
        }

        // slow path: verificar breakpoints y step mode
        bool should_pause = false;
        std::string pause_reason;

        {
            std::lock_guard<std::mutex> lk(bp_mutex_);
            Breakpoint *bp = find_breakpoint(pc, pid);
            if (bp) {
                bp->hit_count++;
                should_pause = true;
                pause_reason = "break";
                // emitir evento de breakpoint
                std::ostringstream ev;
                ev << "{\"event\":\"break\","
                   << "\"pid\":" << pid << ","
                   << "\"pc\":" << pc << ","
                   << "\"bp_id\":" << bp->id << "}";
                broadcast_event(ev.str());
            }
        }

        {
            std::lock_guard<std::mutex> lk(proc_mutex_);
            DbgProcCtx &ctx = get_or_create_ctx(pid);
            if (ctx.step_mode) {
                should_pause = true;
                pause_reason = "stepped";
                ctx.step_mode = false; // consumir el step
                std::ostringstream ev;
                ev << "{\"event\":\"stepped\","
                   << "\"pid\":" << pid << ","
                   << "\"pc\":" << pc << "}";
                broadcast_event(ev.str());
            }
        }

        if (!should_pause) return;

        // pausar el proceso hasta que el depurador emita continue/step
        {
            std::unique_lock<std::mutex> lk(proc_mutex_);
            DbgProcCtx &ctx = get_or_create_ctx(pid);
            ctx.state = DbgProcState::PAUSED;
            // bloquear hasta que state cambie a RUNNING
            ctx.pause_cv.wait(lk, [&ctx]() {
                return ctx.state != DbgProcState::PAUSED;
            });
        }
    }

    /* =====================================================================
     * Notificaciones de eventos de proceso
     * ===================================================================== */

    void Debugger::on_process_exit(uint64_t pid) {
        std::ostringstream ev;
        ev << "{\"event\":\"exit\",\"pid\":" << pid << "}";
        broadcast_event(ev.str());
        // limpiar contexto
        std::lock_guard<std::mutex> lk(proc_mutex_);
        proc_ctx_.erase(pid);
    }

    void Debugger::on_exception(uint64_t pid, const std::string &exc_class) {
        std::ostringstream ev;
        ev << "{\"event\":\"exception\","
           << "\"pid\":" << pid << ","
           << "\"class\":\"" << json_escape(exc_class) << "\"}";
        broadcast_event(ev.str());
    }

    void Debugger::on_process_spawn(uint64_t parent_pid, uint64_t child_pid) {
        std::ostringstream ev;
        ev << "{\"event\":\"spawned\","
           << "\"parent\":" << parent_pid << ","
           << "\"child\":" << child_pid << "}";
        broadcast_event(ev.str());
    }

    /* =====================================================================
     * Helpers de serializacion
     * ===================================================================== */

    /**
     * @brief Serializa los registros de un proceso a JSON.
     */
    std::string Debugger::regs_to_json(runtime::ProcessVM *proc) {
        if (!proc) return "{}";
        std::ostringstream o;
        o << "{";
        // registros r0-r15
        for (int i = 0; i < 16; i++) {
            if (i) o << ",";
            o << "\"r" << i << "\":" << proc->registers.regs[i].qword();
        }
        // PC, SP, BP
        o << ",\"pc\":" << proc->registers.rip.raw();
        o << ",\"sp\":" << proc->registers.stack_pointer.raw();
        o << ",\"bp\":" << proc->registers.base_pointer.raw();
        o << ",\"flags\":" << proc->registers.flags.raw;
        o << "}";
        return o.str();
    }

    /**
     * @brief Serializa un volcado de memoria VM a JSON.
     */
    std::string Debugger::mem_to_json(runtime::ProcessVM *proc,
                                       uint64_t addr, uint32_t length) {
        if (!proc || length == 0) return "[]";
        // limitar a 4096 bytes por peticion
        if (length > 4096) length = 4096;
        std::vector<uint8_t> buf(length);
        // leer via la interfaz de memoria VM del proceso
        proc->vm_mem.read_bytes(addr, buf.data(), length);
        std::ostringstream o;
        o << "[";
        for (uint32_t i = 0; i < length; i++) {
            if (i) o << ",";
            char hex[5];
            std::snprintf(hex, sizeof(hex), "%u",
                         static_cast<unsigned>(buf[i]));
            o << hex;
        }
        o << "]";
        return o.str();
    }

    /* =====================================================================
     * handle_command: dispatch de comandos
     * ===================================================================== */

    /**
     * @brief Procesa un mensaje JSON de comando del cliente.
     */
    void Debugger::handle_command(const std::string &json_msg, int client_fd) {
        std::string cmd_name = json_get(json_msg, "cmd");
        std::string seq_str  = json_get(json_msg, "seq");
        uint32_t seq = seq_str.empty() ? 0u :
                       static_cast<uint32_t>(std::stoul(seq_str));

        DebugCmd cmd = debug_cmd_parse(cmd_name);

        // plantilla de respuesta
        auto ok_resp = [&](const std::string &data) {
            std::ostringstream r;
            r << "{\"ok\":true,\"seq\":" << seq
              << ",\"data\":" << data << "}";
            send_msg(client_fd, r.str());
        };
        auto err_resp = [&](const std::string &msg) {
            std::ostringstream r;
            r << "{\"ok\":false,\"seq\":" << seq
              << ",\"error\":\"" << json_escape(msg) << "\"}";
            send_msg(client_fd, r.str());
        };

        switch (cmd) {

            case DebugCmd::ATTACH: {
                std::string pid_s = json_get(json_msg, "pid");
                if (pid_s.empty()) { err_resp("falta campo 'pid'"); return; }
                uint64_t pid = std::stoull(pid_s);
                {
                    std::lock_guard<std::mutex> lk(proc_mutex_);
                    DbgProcCtx &ctx = get_or_create_ctx(pid);
                    ctx.state = DbgProcState::RUNNING;
                }
                ok_resp("{\"pid\":" + pid_s + "}");
                break;
            }

            case DebugCmd::DETACH: {
                std::string pid_s = json_get(json_msg, "pid");
                if (!pid_s.empty()) {
                    uint64_t pid = std::stoull(pid_s);
                    std::lock_guard<std::mutex> lk(proc_mutex_);
                    auto it = proc_ctx_.find(pid);
                    if (it != proc_ctx_.end()) {
                        it->second.state    = DbgProcState::DETACHED;
                        it->second.step_mode = false;
                        it->second.pause_cv.notify_all(); // reanudar si pausado
                    }
                }
                ok_resp("{}");
                break;
            }

            case DebugCmd::SET_BREAK: {
                std::string addr_s = json_get(json_msg, "addr");
                if (addr_s.empty()) { err_resp("falta campo 'addr'"); return; }
                std::string pid_s  = json_get(json_msg, "pid");
                Breakpoint bp{};
                {
                    std::lock_guard<std::mutex> lk(bp_mutex_);
                    bp.id        = next_bp_id_++;
                    bp.addr      = std::stoull(addr_s);
                    bp.pid       = pid_s.empty() ? 0 : std::stoull(pid_s);
                    bp.enabled   = true;
                    bp.hit_count = 0;
                    breakpoints_.push_back(bp);
                    any_bp_.store(true, std::memory_order_relaxed);
                }
                std::ostringstream d;
                d << "{\"id\":" << bp.id << ",\"addr\":" << bp.addr << "}";
                ok_resp(d.str());
                break;
            }

            case DebugCmd::DEL_BREAK: {
                std::string id_s = json_get(json_msg, "id");
                if (id_s.empty()) { err_resp("falta campo 'id'"); return; }
                uint32_t bp_id = static_cast<uint32_t>(std::stoul(id_s));
                {
                    std::lock_guard<std::mutex> lk(bp_mutex_);
                    breakpoints_.erase(
                        std::remove_if(breakpoints_.begin(), breakpoints_.end(),
                            [bp_id](const Breakpoint &b){ return b.id == bp_id; }),
                        breakpoints_.end());
                    any_bp_.store(!breakpoints_.empty(), std::memory_order_relaxed);
                }
                ok_resp("{}");
                break;
            }

            case DebugCmd::LIST_BREAKS: {
                std::ostringstream d;
                d << "[";
                {
                    std::lock_guard<std::mutex> lk(bp_mutex_);
                    bool first = true;
                    for (const auto &bp : breakpoints_) {
                        if (!first) d << ",";
                        first = false;
                        d << "{\"id\":" << bp.id
                          << ",\"addr\":" << bp.addr
                          << ",\"pid\":" << bp.pid
                          << ",\"enabled\":" << (bp.enabled ? "true" : "false")
                          << ",\"hits\":" << bp.hit_count << "}";
                    }
                }
                d << "]";
                ok_resp(d.str());
                break;
            }

            case DebugCmd::CONTINUE: {
                std::string pid_s = json_get(json_msg, "pid");
                if (pid_s.empty()) { err_resp("falta campo 'pid'"); return; }
                uint64_t pid = std::stoull(pid_s);
                {
                    std::lock_guard<std::mutex> lk(proc_mutex_);
                    auto it = proc_ctx_.find(pid);
                    if (it != proc_ctx_.end()) {
                        it->second.state    = DbgProcState::RUNNING;
                        it->second.step_mode = false;
                        it->second.pause_cv.notify_all();
                    }
                }
                ok_resp("{}");
                break;
            }

            case DebugCmd::STEP: {
                std::string pid_s = json_get(json_msg, "pid");
                if (pid_s.empty()) { err_resp("falta campo 'pid'"); return; }
                uint64_t pid = std::stoull(pid_s);
                {
                    std::lock_guard<std::mutex> lk(proc_mutex_);
                    DbgProcCtx &ctx = get_or_create_ctx(pid);
                    ctx.step_mode = true;
                    ctx.state     = DbgProcState::RUNNING; // reanudar para un paso
                    ctx.pause_cv.notify_all();
                }
                ok_resp("{}");
                break;
            }

            case DebugCmd::REGISTERS: {
                std::string pid_s = json_get(json_msg, "pid");
                if (pid_s.empty()) { err_resp("falta campo 'pid'"); return; }
                // buscar ProcessVM* en la VM (requiere API de VM)
                // por simplicidad emitimos un placeholder si no encontramos el proceso
                ok_resp("{\"note\":\"use proc_ptr para acceder a registros en tiempo real\"}");
                break;
            }

            case DebugCmd::PAUSE: {
                std::string pid_s = json_get(json_msg, "pid");
                if (pid_s.empty()) { err_resp("falta campo 'pid'"); return; }
                uint64_t pid = std::stoull(pid_s);
                {
                    std::lock_guard<std::mutex> lk(proc_mutex_);
                    DbgProcCtx &ctx = get_or_create_ctx(pid);
                    ctx.state     = DbgProcState::PAUSED;
                    ctx.step_mode = true; // parar en la proxima instruccion
                }
                ok_resp("{}");
                break;
            }

            case DebugCmd::UNKNOWN:
            default:
                err_resp("comando desconocido: " + cmd_name);
                break;
        }
    }

} // namespace debug
