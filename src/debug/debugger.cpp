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
#include "runtime/scheduler.h"
#include "loader/loader.h"
#include "emmit/emmit_decl.h"
#include "runtime/decode_instruction.h"
#include "loader/oop_types.h"

#include <cstring>
#include <cstdio>
#include <sstream>
#include <algorithm>

// Tablas externas de decodificacion (definidas en src/runtime/decode_table.cpp).
namespace runtime {
    extern InstrFormat decode_table_primary[0x100];
    extern InstrFormat decode_table_extended[0x100];
}

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
        for (unsigned char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                default:
                    if (c < 0x20) {
                        // Control char: escape como \u00XX (JSON exige
                        // que cualquier U+0000..U+001F este escapado).
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
                    break;
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
            {"set_break_src", DebugCmd::SET_BREAK_SRC},
            {"info_source",   DebugCmd::INFO_SOURCE},
            {"gc_stats",      DebugCmd::GC_STATS},
            {"gc_handles",    DebugCmd::GC_HANDLES},
            {"gc_inspect",    DebugCmd::GC_INSPECT},
            {"flags",         DebugCmd::FLAGS},
            {"fregs",         DebugCmd::FREGS},
            {"dump_stack",    DebugCmd::DUMP_STACK},
            {"frame_info",    DebugCmd::FRAME_INFO},
            {"backtrace",     DebugCmd::BACKTRACE},
            {"continue",    DebugCmd::CONTINUE},
            {"step",        DebugCmd::STEP},
            {"next",        DebugCmd::NEXT},
            {"registers",   DebugCmd::REGISTERS},
            {"memory",      DebugCmd::MEMORY},
            {"stack",       DebugCmd::STACK},
            {"info_proc",   DebugCmd::INFO_PROC},
            {"eval",        DebugCmd::EVAL},
            {"pause",       DebugCmd::PAUSE},
            {"disasm",        DebugCmd::DISASM},
            {"locals",        DebugCmd::LOCALS},
            {"gc_run",        DebugCmd::GC_RUN},
            {"step_out",      DebugCmd::STEP_OUT},
            {"finish",        DebugCmd::STEP_OUT},
            {"step_until",    DebugCmd::STEP_UNTIL},
            {"until",         DebugCmd::STEP_UNTIL},
            {"set_watch",     DebugCmd::SET_WATCH},
            {"del_watch",     DebugCmd::DEL_WATCH},
            {"list_watches",  DebugCmd::LIST_WATCHES},
            {"trace_msgs",    DebugCmd::TRACE_MSGS},
            {"break_mon",     DebugCmd::BREAK_MON},
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
        // Adquirir client_mutex_ alrededor del envio completo (header +
        // body) para evitar interleaving entre las respuestas sincronas
        // de un cliente y los eventos broadcast desde otro hilo.  Sin
        // este lock, dos hilos enviando simultaneamente al mismo fd
        // pueden intercalar sus bytes y corromper el framing.
        std::lock_guard<std::mutex> lk(client_mutex_);
        uint32_t len = static_cast<uint32_t>(json.size());
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
     *
     * Llama a send_msg para cada cliente conectado.  send_msg adquiere
     * client_mutex_ internamente y serializa la copia ENTERA del marco
     * (header + body), evitando interleaving con las respuestas sincronas.
     */
    void Debugger::broadcast_event(const std::string &json) {
        // Snapshot de los fds bajo lock para evitar acceder al vector
        // mientras otro hilo lo modifica (accept_loop / cleanup).  Una
        // vez tenemos la copia, send_msg adquiere el mismo lock por fd.
        std::vector<int> fds_copy;
        {
            std::lock_guard<std::mutex> lk(client_mutex_);
            fds_copy = client_fds_;
        }
        for (int fd : fds_copy) {
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
     * eval_simple_condition
     *
     * Evalua una condicion sencilla del estilo "<reg> <op> <num>" donde
     * <reg> puede ser r0..r15 / rsp / rbp / rip / pc, <op> uno de
     *   == != < <= > >= (signed por defecto)
     * y <num> es un entero (decimal, 0x..., 0b..., 0o..., '-' opcional).
     *
     * Devuelve true si la condicion se cumple.  Si la cadena no se puede
     * parsear, devuelve true tambien (fail-open: no silenciar bps por
     * errores de sintaxis -- mejor pausar y que el usuario vea).
     *
     * Coste: una sola comparacion en runtime; usado SOLO cuando un bp con
     * condicion no vacia hace match en pc.  Cero overhead en bps incond.
     * ===================================================================== */
    static int reg_index_from_name(const std::string &n) {
        if (n.empty()) return -1;
        if (n == "rsp" || n == "sp") return 100;
        if (n == "rbp" || n == "bp") return 101;
        if (n == "rip" || n == "pc") return 102;
        if (n[0] == 'r' && n.size() <= 3) {
            try { int v = std::stoi(n.substr(1)); if (v >= 0 && v <= 15) return v; }
            catch (...) {}
        }
        return -1;
    }

    static bool eval_simple_condition(const std::string &cond,
                                       runtime::ProcessVM *p) {
        if (cond.empty() || !p) return true;
        // Tokenizar muy basico: trim, split por espacios.
        std::string s = cond;
        // colapsar espacios para simplificar
        std::string out; out.reserve(s.size());
        bool prev_space = true;
        for (char c : s) {
            if (c == ' ' || c == '\t') {
                if (!prev_space) { out.push_back(' '); prev_space = true; }
            } else {
                out.push_back(c);
                prev_space = false;
            }
        }
        s = out;
        if (!s.empty() && s.back() == ' ') s.pop_back();
        // separar operador: buscar primero un op de 2 chars, luego 1 char
        const char *ops2[] = {"==", "!=", "<=", ">="};
        size_t op_pos = std::string::npos;
        size_t op_len = 0;
        for (const char *op : ops2) {
            size_t k = s.find(op);
            if (k != std::string::npos) { op_pos = k; op_len = 2; break; }
        }
        if (op_pos == std::string::npos) {
            for (char c : std::string("<>")) {
                size_t k = s.find(c);
                if (k != std::string::npos) { op_pos = k; op_len = 1; break; }
            }
        }
        if (op_pos == std::string::npos) return true; // sin op -> fail-open
        std::string lhs = s.substr(0, op_pos);
        std::string op  = s.substr(op_pos, op_len);
        std::string rhs = s.substr(op_pos + op_len);
        // trim
        while (!lhs.empty() && lhs.back() == ' ') lhs.pop_back();
        while (!rhs.empty() && rhs.front() == ' ') rhs.erase(rhs.begin());
        int idx = reg_index_from_name(lhs);
        if (idx < 0) return true;
        int64_t lv = 0;
        if (idx >= 0 && idx <= 15) lv = static_cast<int64_t>(p->registers.regs[idx].qword());
        else if (idx == 100) lv = static_cast<int64_t>(p->registers.stack_pointer.raw());
        else if (idx == 101) lv = static_cast<int64_t>(p->registers.base_pointer.raw());
        else if (idx == 102) lv = static_cast<int64_t>(p->registers.rip.raw());
        int64_t rv = 0;
        try {
            if (rhs.size() >= 2 && rhs[0] == '0' && (rhs[1] == 'x' || rhs[1] == 'X'))
                rv = std::stoll(rhs.substr(2), nullptr, 16);
            else if (rhs.size() >= 2 && rhs[0] == '0' && (rhs[1] == 'b' || rhs[1] == 'B'))
                rv = std::stoll(rhs.substr(2), nullptr, 2);
            else if (rhs.size() >= 2 && rhs[0] == '0' && (rhs[1] == 'o' || rhs[1] == 'O'))
                rv = std::stoll(rhs.substr(2), nullptr, 8);
            else
                rv = std::stoll(rhs, nullptr, 10);
        } catch (...) { return true; }
        if (op == "==") return lv == rv;
        if (op == "!=") return lv != rv;
        if (op == "<")  return lv <  rv;
        if (op == "<=") return lv <= rv;
        if (op == ">")  return lv >  rv;
        if (op == ">=") return lv >= rv;
        return true;
    }

    /* =====================================================================
     * find_process_by_pid: busca el ProcessVM por su local_pid en todos
     * los schedulers de la VM.  Devuelve nullptr si no existe.
     *
     * No adquirimos lock sobre los procesos: estamos asumiendo que el
     * cliente del debugger quiere leer estado de un proceso que esta
     * pausado (PAUSED) y por tanto no esta siendo modificado por el
     * scheduler.  Para procesos en RUNNING, los reads pueden leer
     * estado parcial pero seguro (registros son atomic, memoria VM es
     * accesible read-only).  El uso tipico es: cliente recibe evento
     * BREAK con pid + pc, luego pide REGISTERS/MEMORY del mismo pid.
     * ===================================================================== */
    static runtime::ProcessVM *find_process_by_pid(runtime::VM &vm,
                                                    uint64_t pid) {
        for (auto &sched : vm.schedulers) {
            for (auto &p : sched->processes) {
                if (p && p->pid.local_pid == pid) {
                    return p.get();
                }
            }
        }
        return nullptr;
    }

    /* =====================================================================
     * recompute_any_step
     *
     * Llamado tras cualquier cambio de estado que pueda haber limpiado el
     * ultimo step_mode o pausa.  Si NINGUN proceso queda en step_mode ni
     * PAUSED, baja el flag global y el fast path de on_before_exec puede
     * retornar inmediatamente sin tocar mutex.
     *
     * Debe llamarse con `proc_mutex_` ya adquirido por el caller.
     * ===================================================================== */
    static bool any_step_or_paused_locked(
        const std::unordered_map<uint64_t, DbgProcCtx> &m) {
        for (const auto &kv : m) {
            if (kv.second.step_mode) return true;
            if (kv.second.state == DbgProcState::PAUSED) return true;
        }
        return false;
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

        // FAST PATH: zero-lock, zero-allocation.  Solo dos atomic loads.
        // Si NO hay breakpoints registrados Y NO hay procesos en
        // step/pause, retornamos inmediatamente sin tocar mutex.  Cuando
        // el cliente emite SET_BREAK por primera vez, `any_bp_` pasa a
        // true y entramos en el slow path; cuando emite STEP/NEXT/PAUSE,
        // `any_step_` pasa a true.  Ambos flags se desactivan cuando el
        // ultimo break/step/pause termina (CONTINUE limpia step_mode y
        // recomputa any_step_ -> false si no quedan procesos pausados).
        // Coste runtime con debugger inactivo: 2 loads relaxed + 1 branch
        // (~1 ns por instruccion VM).  Equivale a `if (!has_hooks) return`.
        if (__builtin_expect(!any_bp_.load(std::memory_order_relaxed)
                          && !any_step_.load(std::memory_order_relaxed)
                          && !any_watch_.load(std::memory_order_relaxed), 1)) {
            return;
        }

        // Loop: tras un wait (el usuario hizo continue/step), debemos
        // RE-chequear si hay un breakpoint en el PC actual antes de
        // ejecutar la siguiente instruccion.  Esto cubre el caso comun:
        //   1. Proceso PAUSED por pause_at_start (step_mode=true).
        //   2. Cliente conecta, agrega bp en pc=N.
        //   3. Cliente envia continue.
        //   4. on_before_exec retorna -- pero si pc actual == N, deberia
        //      pausar de nuevo por el bp.  Sin loop, perderiamos ese bp.
        // El loop se repite hasta que no haya bp/step pendiente, momento
        // en el que retornamos para ejecutar la instruccion.
        uint64_t pid = proc->pid.local_pid; // PID local del proceso
    recheck:
        uint64_t pc  = proc->registers.rip.raw(); // Program Counter actual

        // slow path: verificar breakpoints y step mode
        bool should_pause = false;
        std::string pause_reason;

        {
            // Chequeo del skip-bp-pc: si el proceso fue pausado por un
            // bp en ESTE pc en la iteracion anterior y el usuario emitio
            // continue, NO debemos re-pausar -- ejecutamos la instruccion
            // y limpiamos el flag.  Sin esto, "continue" tras hit de bp
            // causaria loop infinito.
            uint64_t skip_pc = UINT64_MAX;
            {
                std::lock_guard<std::mutex> lk2(proc_mutex_);
                auto it = proc_ctx_.find(pid);
                if (it != proc_ctx_.end()) {
                    skip_pc = it->second.last_bp_pc;
                    it->second.last_bp_pc = UINT64_MAX; // consumir
                }
            }
            std::lock_guard<std::mutex> lk(bp_mutex_);
            Breakpoint *bp = (pc == skip_pc) ? nullptr
                                              : find_breakpoint(pc, pid);
            if (bp) {
                // Evaluar condicion (vacio = incondicional).  Si la cond
                // se evalua a false, NO pausamos, NO incrementamos hits.
                bool cond_ok = bp->condition.empty() ||
                               eval_simple_condition(bp->condition, proc);
                if (cond_ok) {
                    bp->hit_count++;
                    should_pause = true;
                    pause_reason = "break";
                    uint32_t bp_id_for_event = bp->id;
                    bool was_one_shot = bp->one_shot;
                    {
                        std::lock_guard<std::mutex> lk2(proc_mutex_);
                        DbgProcCtx &ctx = get_or_create_ctx(pid);
                        ctx.last_bp_pc = pc;
                    }
                    // emitir evento de breakpoint
                    std::ostringstream ev;
                    ev << "{\"event\":\"break\","
                       << "\"pid\":" << pid << ","
                       << "\"pc\":" << pc << ","
                       << "\"bp_id\":" << bp_id_for_event << "}";
                    broadcast_event(ev.str());
                    // Auto-delete si one_shot.  Hacemos esto AHORA mientras
                    // tenemos el mutex; el iterador `bp` quedara invalidado
                    // pero ya no lo usamos.
                    if (was_one_shot) {
                        breakpoints_.erase(
                            std::remove_if(breakpoints_.begin(), breakpoints_.end(),
                                [bp_id_for_event](const Breakpoint &b){
                                    return b.id == bp_id_for_event;
                                }),
                            breakpoints_.end());
                        any_bp_.store(!breakpoints_.empty(),
                                      std::memory_order_relaxed);
                    }
                }
            }
        }

        // Watchpoints: chequeo polling.  Solo si any_watch_=true y solo
        // dentro del slow path (ya estamos aqui por any_bp_/any_step_).
        // Esto significa que si NO hay bps ni step, los watchpoints
        // necesitan que algo mas active el slow path.  En la practica,
        // poner un watchpoint setea any_watch_ y FORZA una pausa-step
        // implicita en el primer chequeo (el cliente recibe el evento).
        if (any_watch_.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lk(watch_mutex_);
            for (auto &wp : watchpoints_) {
                if (!wp.enabled) continue;
                if (wp.pid != 0 && wp.pid != pid) continue;
                bool alive = proc->gc_heap.is_handle_live(
                    static_cast<gc::GcHandle>(wp.handle));
                uint64_t addr = 0;
                if (alive) {
                    addr = reinterpret_cast<uint64_t>(
                        proc->gc_heap.deref(
                            static_cast<gc::GcHandle>(wp.handle)));
                }
                bool moved   = (alive && addr != wp.last_addr && wp.was_alive);
                bool died    = (!alive && wp.was_alive);
                bool resurr  = (alive && !wp.was_alive);
                if (moved || died || resurr) {
                    std::ostringstream ev;
                    ev << "{\"event\":\"watch\","
                       << "\"pid\":" << pid << ","
                       << "\"pc\":" << pc << ","
                       << "\"wp_id\":" << wp.id << ","
                       << "\"handle\":" << wp.handle << ","
                       << "\"reason\":\""
                       << (died ? "died" : (moved ? "moved" : "alive"))
                       << "\","
                       << "\"old_addr\":" << wp.last_addr << ","
                       << "\"new_addr\":" << addr << "}";
                    broadcast_event(ev.str());
                    wp.last_addr = addr;
                    wp.was_alive = alive;
                    should_pause = true;
                    pause_reason = "watch";
                }
            }
        }

        {
            std::lock_guard<std::mutex> lk(proc_mutex_);
            DbgProcCtx &ctx = get_or_create_ctx(pid);
            // Skip-step-pc: si en la iteracion anterior pausamos por
            // step_mode en este mismo pc y el cliente emitio STEP de
            // nuevo, NO debemos re-pausar -- ejecutamos la instruccion
            // y limpiamos el flag.  Sin esto, "step" tras stepped causa
            // bucle: pausa-stepped -> wake -> pausa-stepped -> ...
            // siempre en el mismo PC (la instruccion nunca corre).
            uint64_t skip_step_pc = ctx.last_step_pc;
            ctx.last_step_pc = UINT64_MAX; // consumir el flag
            if (ctx.step_mode && pc != skip_step_pc) {
                should_pause = true;
                pause_reason = "stepped";
                ctx.step_mode = false; // consumir el step
                ctx.last_step_pc = pc; // recordar para evitar re-pausa
                // Recompute any_step_ tras consumir el step_mode.
                any_step_.store(any_step_or_paused_locked(proc_ctx_),
                                std::memory_order_release);
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
            // PAUSED activa el fast-path de on_before_exec.
            any_step_.store(true, std::memory_order_release);
            // bloquear hasta que state cambie a RUNNING
            ctx.pause_cv.wait(lk, [&ctx]() {
                return ctx.state != DbgProcState::PAUSED;
            });
            // Tras despertar (el cliente envio CONTINUE/STEP), recompute
            // any_step_: si quedan otros procesos en PAUSED o step_mode,
            // sigue true; si no, vuelve a false y el fast path retorna.
            any_step_.store(any_step_or_paused_locked(proc_ctx_),
                            std::memory_order_release);
        }
        // Re-check: tras el wait, el usuario podria haber agregado un bp
        // que matchea el pc actual.  Sin esta re-iteracion el bp se
        // perderia (el caso clasico: pause_at_start -> cliente agrega
        // bp en pc=0 -> cliente continue -> debiamos hit el bp).
        goto recheck;
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

    void Debugger::pause_at_start(uint64_t pid) {
        // Marca el proceso para que se detenga ANTES de ejecutar su
        // primera instruccion.  Como state=PAUSED + step_mode=true,
        // la primera entrada a on_before_exec para este pid:
        //   1. Detecta step_mode -> emite evento "stepped" + clear step_mode.
        //   2. Detecta should_pause=true -> setea state=PAUSED.
        //   3. Bloquea en cv.wait hasta que llegue CONTINUE.
        //
        // Cuando el cliente envia "continue", el handler del CONTINUE
        // setea state=RUNNING + notifica el cv y el proceso sigue.
        // Entre tanto el cliente puede setear breakpoints, listar
        // procesos, leer registros, etc.
        std::lock_guard<std::mutex> lk(proc_mutex_);
        DbgProcCtx &ctx = get_or_create_ctx(pid);
        ctx.state     = DbgProcState::PAUSED;
        ctx.step_mode = true;
        any_step_.store(true, std::memory_order_release);
    }

    void Debugger::on_process_spawn(uint64_t parent_pid, uint64_t child_pid) {
        std::ostringstream ev;
        ev << "{\"event\":\"spawned\","
           << "\"parent\":" << parent_pid << ","
           << "\"child\":" << child_pid << "}";
        broadcast_event(ev.str());
    }

    void Debugger::on_message(uint64_t pid, bool is_send,
                              uint64_t target, uint64_t payload64) {
        // Fast path: si nadie esta tracing nada, retornamos.
        if (!any_msg_trace_.load(std::memory_order_relaxed)) return;
        // Slow path: comprobar si ESTE pid esta en la lista.
        bool active = false;
        {
            std::lock_guard<std::mutex> lk(trace_mutex_);
            active = (traced_msg_pids_.count(pid) != 0);
        }
        if (!active) return;
        std::ostringstream ev;
        ev << "{\"event\":\"msg_trace\","
           << "\"pid\":" << pid << ","
           << "\"dir\":\"" << (is_send ? "send" : "recv") << "\","
           << "\"peer\":" << target << ","
           << "\"value\":" << payload64 << "}";
        broadcast_event(ev.str());
    }

    void Debugger::on_monitor_contention(uint64_t pid, uint64_t handle,
                                          uint64_t owner) {
        if (!break_on_mon_.load(std::memory_order_relaxed)) return;
        // Emitir evento.
        std::ostringstream ev;
        ev << "{\"event\":\"mon_block\","
           << "\"pid\":" << pid << ","
           << "\"handle\":" << handle << ","
           << "\"owner\":" << owner << "}";
        broadcast_event(ev.str());
        // Forzar pausa: ponemos step_mode + state=PAUSED.  El proceso ya
        // esta en WAIT_IO esperando al monitor; cuando el scheduler lo
        // re-active tras un monexit, on_before_exec lo pausara aqui.
        std::lock_guard<std::mutex> lk(proc_mutex_);
        DbgProcCtx &ctx = get_or_create_ctx(pid);
        ctx.state     = DbgProcState::PAUSED;
        ctx.step_mode = true;
        any_step_.store(true, std::memory_order_release);
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
                        any_step_.store(any_step_or_paused_locked(proc_ctx_),
                                        std::memory_order_release);
                    }
                }
                ok_resp("{}");
                break;
            }

            case DebugCmd::SET_BREAK: {
                std::string addr_s = json_get(json_msg, "addr");
                if (addr_s.empty()) { err_resp("falta campo 'addr'"); return; }
                std::string pid_s   = json_get(json_msg, "pid");
                std::string cond_s  = json_get(json_msg, "cond");
                std::string one_s   = json_get(json_msg, "one_shot");
                Breakpoint bp{};
                {
                    std::lock_guard<std::mutex> lk(bp_mutex_);
                    bp.id        = next_bp_id_++;
                    bp.addr      = std::stoull(addr_s);
                    bp.pid       = pid_s.empty() ? 0 : std::stoull(pid_s);
                    bp.enabled   = true;
                    bp.hit_count = 0;
                    bp.condition = cond_s;
                    bp.one_shot  = (one_s == "true" || one_s == "1");
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
                          << ",\"hits\":" << bp.hit_count
                          << ",\"one_shot\":" << (bp.one_shot ? "true" : "false")
                          << ",\"cond\":\"" << json_escape(bp.condition) << "\"}";
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
                        any_step_.store(any_step_or_paused_locked(proc_ctx_),
                                        std::memory_order_release);
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
                    // step_mode activo => fast path debe entrar en slow.
                    any_step_.store(true, std::memory_order_release);
                }
                ok_resp("{}");
                break;
            }

            case DebugCmd::REGISTERS: {
                std::string pid_s = json_get(json_msg, "pid");
                if (pid_s.empty()) { err_resp("falta campo 'pid'"); return; }
                uint64_t pid = std::stoull(pid_s);
                runtime::ProcessVM *p = find_process_by_pid(vm_, pid);
                if (!p) { err_resp("proceso no encontrado"); return; }
                ok_resp(regs_to_json(p));
                break;
            }

            case DebugCmd::MEMORY: {
                std::string pid_s = json_get(json_msg, "pid");
                std::string addr_s = json_get(json_msg, "addr");
                std::string len_s = json_get(json_msg, "len");
                if (pid_s.empty() || addr_s.empty() || len_s.empty()) {
                    err_resp("memory requiere pid, addr y len"); return;
                }
                uint64_t pid    = std::stoull(pid_s);
                uint64_t addr   = std::stoull(addr_s);
                uint32_t length = static_cast<uint32_t>(std::stoul(len_s));
                if (length > 4096) { err_resp("len max 4096"); return; }
                runtime::ProcessVM *p = find_process_by_pid(vm_, pid);
                if (!p) { err_resp("proceso no encontrado"); return; }
                ok_resp(mem_to_json(p, addr, length));
                break;
            }

            case DebugCmd::STACK: {
                std::string pid_s = json_get(json_msg, "pid");
                if (pid_s.empty()) { err_resp("falta campo 'pid'"); return; }
                uint64_t pid = std::stoull(pid_s);
                runtime::ProcessVM *p = find_process_by_pid(vm_, pid);
                if (!p) { err_resp("proceso no encontrado"); return; }
                // Stack trace: recorrer frame_stack para devolver pares (pc, return_pc).
                std::ostringstream d;
                d << "{\"frames\":[";
                bool first = true;
                d << "{\"pc\":" << p->registers.rip.raw() << ",\"is_top\":true}";
                first = false;
                for (loader::FrameHeader *f = p->frame_stack;
                     f != nullptr;
                     f = f->prev) {
                    d << ",{\"return_pc\":" << f->return_pc << "}";
                }
                d << "]}";
                ok_resp(d.str());
                break;
            }

            case DebugCmd::LIST_PROCS: {
                std::ostringstream d;
                d << "[";
                bool first = true;
                for (auto &sched : vm_.schedulers) {
                    for (auto &p : sched->processes) {
                        if (!p) continue;
                        if (!first) d << ",";
                        first = false;
                        d << "{\"pid\":" << p->pid.local_pid
                          << ",\"sched_id\":" << p->pid.scheduler_id
                          << ",\"state\":\""
                          << runtime::vm_state_to_str(p->state.load())
                          << "\",\"pc\":" << p->registers.rip.raw()
                          << ",\"reductions\":" << p->reductions_remaining
                          << "}";
                    }
                }
                d << "]";
                ok_resp(d.str());
                break;
            }

            case DebugCmd::INFO_PROC: {
                std::string pid_s = json_get(json_msg, "pid");
                if (pid_s.empty()) { err_resp("falta campo 'pid'"); return; }
                uint64_t pid = std::stoull(pid_s);
                runtime::ProcessVM *p = find_process_by_pid(vm_, pid);
                if (!p) { err_resp("proceso no encontrado"); return; }
                std::ostringstream d;
                d << "{\"pid\":" << p->pid.local_pid
                  << ",\"sched_id\":" << p->pid.scheduler_id
                  << ",\"state\":\""
                  << runtime::vm_state_to_str(p->state.load())
                  << "\",\"pc\":" << p->registers.rip.raw()
                  << ",\"reductions\":" << p->reductions_remaining
                  << ",\"err_thread\":" << static_cast<int>(p->err_thread)
                  << ",\"tsc\":" << p->tsc
                  << "}";
                ok_resp(d.str());
                break;
            }

            case DebugCmd::EVAL: {
                std::string pid_s = json_get(json_msg, "pid");
                std::string expr  = json_get(json_msg, "expr");
                if (pid_s.empty() || expr.empty()) {
                    err_resp("eval requiere pid y expr"); return;
                }
                uint64_t pid = std::stoull(pid_s);
                runtime::ProcessVM *p = find_process_by_pid(vm_, pid);
                if (!p) { err_resp("proceso no encontrado"); return; }
                // Soporte minimo: si expr empieza con 'r' y un digito,
                // devolvemos el valor del registro (r0..r15, rip, rsp, rbp).
                std::ostringstream d;
                if (expr == "rip" || expr == "pc") {
                    d << "{\"name\":\"" << expr << "\",\"value\":"
                      << p->registers.rip.raw() << "}";
                } else if (expr.size() >= 2 && expr[0] == 'r'
                        && std::isdigit(static_cast<unsigned char>(expr[1]))) {
                    int idx = std::atoi(expr.c_str() + 1);
                    if (idx < 0 || idx > 15) {
                        err_resp("indice de registro fuera de rango (0..15)");
                        return;
                    }
                    d << "{\"name\":\"" << expr << "\",\"value\":"
                      << p->registers.regs[idx].qword() << "}";
                } else {
                    err_resp("eval soporta r0..r15 / rip / pc por ahora");
                    return;
                }
                ok_resp(d.str());
                break;
            }

            case DebugCmd::SET_BREAK_SRC: {
                std::string file_s = json_get(json_msg, "file");
                std::string line_s = json_get(json_msg, "line");
                std::string pid_s  = json_get(json_msg, "pid");
                if (file_s.empty() || line_s.empty()) {
                    err_resp("set_break_src requiere 'file' y 'line'"); return;
                }
                uint32_t target_line = static_cast<uint32_t>(std::stoul(line_s));
                // Recorrer todos los Executable cargados y preguntar a su
                // DebugInfo si tienen un offset para (file, line).  El
                // primer match gana.  Si ninguno tiene info de debug,
                // devolver error claro pidiendo recompilar con --vex-debug.
                uint32_t best_off = UINT32_MAX;
                for (const auto &exe : vm_.loader_public.executables) {
                    if (!exe || !exe->debug_info) continue;
                    uint32_t off = exe->debug_info->lookup_offset_for_line(
                        file_s, target_line);
                    if (off != UINT32_MAX) {
                        best_off = off;
                        break;
                    }
                }
                if (best_off == UINT32_MAX) {
                    err_resp("set_break_src: no se encontro (file, line) en "
                             "la info de debug.  Compila con --vex-debug "
                             "para incluir la tabla bytecode->source-line.");
                    return;
                }
                // Crear el breakpoint con el addr resuelto.
                std::string cond_s = json_get(json_msg, "cond");
                std::string one_s  = json_get(json_msg, "one_shot");
                Breakpoint bp{};
                {
                    std::lock_guard<std::mutex> lk(bp_mutex_);
                    bp.id        = next_bp_id_++;
                    bp.addr      = best_off;
                    bp.pid       = pid_s.empty() ? 0 : std::stoull(pid_s);
                    bp.enabled   = true;
                    bp.hit_count = 0;
                    bp.condition = cond_s;
                    bp.one_shot  = (one_s == "true" || one_s == "1");
                    breakpoints_.push_back(bp);
                    any_bp_.store(true, std::memory_order_relaxed);
                }
                std::ostringstream d;
                d << "{\"id\":" << bp.id
                  << ",\"addr\":" << bp.addr
                  << ",\"file\":\"" << json_escape(file_s) << "\""
                  << ",\"line\":" << target_line << "}";
                ok_resp(d.str());
                break;
            }

            case DebugCmd::INFO_SOURCE: {
                std::string pid_s = json_get(json_msg, "pid");
                if (pid_s.empty()) { err_resp("falta campo 'pid'"); return; }
                uint64_t pid = std::stoull(pid_s);
                runtime::ProcessVM *p = find_process_by_pid(vm_, pid);
                if (!p) { err_resp("proceso no encontrado"); return; }
                uint64_t pc = p->registers.rip.raw();
                // Buscar en TODOS los executables el que tiene info de
                // debug para ese PC.  Devolvemos el primero que matchea.
                for (const auto &exe : vm_.loader_public.executables) {
                    if (!exe || !exe->debug_info) continue;
                    debug::LineInfo li = exe->debug_info->lookup_line(
                        static_cast<uint32_t>(pc));
                    if (li.found) {
                        std::ostringstream d;
                        d << "{\"file\":\""
                          << json_escape(li.file ? li.file : "")
                          << "\",\"line\":" << li.line
                          << ",\"pc\":" << pc << "}";
                        ok_resp(d.str());
                        return;
                    }
                }
                // No hay info de debug -> respuesta vacia indicando addr
                // pero sin file/line.
                std::ostringstream d;
                d << "{\"file\":null,\"line\":0,\"pc\":" << pc << "}";
                ok_resp(d.str());
                break;
            }

            case DebugCmd::GC_STATS: {
                std::string pid_s = json_get(json_msg, "pid");
                if (pid_s.empty()) { err_resp("falta campo 'pid'"); return; }
                uint64_t pid = std::stoull(pid_s);
                runtime::ProcessVM *p = find_process_by_pid(vm_, pid);
                if (!p) { err_resp("proceso no encontrado"); return; }
                const gc::GcStats &s = p->gc_heap.stats();
                // Conteo de handles vivos: scan O(N) sobre el slot
                // table.  Aceptable porque debugger no es hot path.
                uint64_t live_count = 0;
                const size_t total_slots = p->gc_heap.handle_table_size();
                for (size_t i = 0; i < total_slots; ++i) {
                    if (p->gc_heap.is_handle_live(static_cast<gc::GcHandle>(i))) {
                        live_count++;
                    }
                }
                std::ostringstream d;
                d << "{"
                  << "\"alloc_count\":"     << s.alloc_count
                  << ",\"alloc_bytes\":"    << s.alloc_bytes
                  << ",\"freed_count\":"    << s.freed_count
                  << ",\"freed_bytes\":"    << s.freed_bytes
                  << ",\"promoted_count\":" << s.promoted_count
                  << ",\"promoted_bytes\":" << s.promoted_bytes
                  << ",\"minor_gc_count\":" << s.minor_gc_count
                  << ",\"major_gc_count\":" << s.major_gc_count
                  << ",\"peak_nursery\":"   << s.peak_nursery
                  << ",\"peak_old\":"       << s.peak_old
                  << ",\"old_reserved_bytes\":" << s.old_reserved_bytes
                  << ",\"old_freelist_bytes\":" << s.old_freelist_bytes
                  << ",\"nursery_used\":"   << p->gc_heap.nursery_used()
                  << ",\"nursery_total\":"  << p->gc_heap.nursery_total()
                  << ",\"live_handles\":"   << live_count
                  << ",\"handle_slots\":"   << total_slots
                  << "}";
                ok_resp(d.str());
                break;
            }

            case DebugCmd::GC_HANDLES: {
                std::string pid_s   = json_get(json_msg, "pid");
                std::string limit_s = json_get(json_msg, "limit");
                if (pid_s.empty()) { err_resp("falta campo 'pid'"); return; }
                uint64_t pid = std::stoull(pid_s);
                runtime::ProcessVM *p = find_process_by_pid(vm_, pid);
                if (!p) { err_resp("proceso no encontrado"); return; }
                size_t limit = limit_s.empty() ? 256
                    : static_cast<size_t>(std::stoul(limit_s));
                if (limit > 4096) limit = 4096; // proteccion
                const size_t total_slots = p->gc_heap.handle_table_size();
                std::ostringstream d;
                d << "[";
                size_t printed = 0;
                for (size_t i = 0; i < total_slots && printed < limit; ++i) {
                    auto h = static_cast<gc::GcHandle>(i);
                    if (!p->gc_heap.is_handle_live(h)) continue;
                    uint8_t *payload = p->gc_heap.deref(h);
                    uint32_t  sz     = p->gc_heap.handle_payload_size(h);
                    gc::GcGen gen    = p->gc_heap.handle_generation(h);
                    // class_ptr -> ClassInfo -> name (puede ser null si
                    // el payload no es un ObjectHeader-prefixed).
                    std::string cname = "?";
                    if (payload && sz >= sizeof(loader::ObjectHeader)) {
                        const auto *oh = reinterpret_cast<
                            const loader::ObjectHeader *>(payload);
                        if (oh->class_ptr && oh->class_ptr->name.data
                         && oh->class_ptr->name.size > 0) {
                            cname.assign(
                                reinterpret_cast<const char *>(
                                    oh->class_ptr->name.data),
                                oh->class_ptr->name.size);
                        }
                    }
                    if (printed > 0) d << ",";
                    d << "{\"h\":" << static_cast<unsigned long long>(h)
                      << ",\"size\":" << sz
                      << ",\"gen\":\""
                      << (gen == gc::GcGen::YOUNG ? "young" : "old") << "\""
                      << ",\"addr\":" << reinterpret_cast<uintptr_t>(payload)
                      << ",\"class\":\"" << json_escape(cname) << "\"}";
                    printed++;
                }
                d << "]";
                ok_resp(d.str());
                break;
            }

            case DebugCmd::GC_INSPECT: {
                std::string pid_s = json_get(json_msg, "pid");
                std::string h_s   = json_get(json_msg, "h");
                if (pid_s.empty() || h_s.empty()) {
                    err_resp("gc_inspect requiere pid y h"); return;
                }
                uint64_t pid = std::stoull(pid_s);
                gc::GcHandle h = static_cast<gc::GcHandle>(std::stoul(h_s));
                runtime::ProcessVM *p = find_process_by_pid(vm_, pid);
                if (!p) { err_resp("proceso no encontrado"); return; }
                if (!p->gc_heap.is_handle_live(h)) {
                    err_resp("handle muerto o invalido"); return;
                }
                uint8_t *payload = p->gc_heap.deref(h);
                uint32_t sz      = p->gc_heap.handle_payload_size(h);
                if (!payload) { err_resp("payload null"); return; }
                // Cap dump a 256 bytes para no sobrecargar el frame del
                // protocolo TCP (4 MB max pero queremos respuestas
                // pequenas para REPL fluido).
                uint32_t dump_n = sz > 256 ? 256 : sz;
                std::ostringstream d;
                d << "{\"h\":" << static_cast<unsigned long long>(h)
                  << ",\"size\":" << sz
                  << ",\"gen\":\""
                  << (p->gc_heap.handle_generation(h) == gc::GcGen::YOUNG
                        ? "young" : "old")
                  << "\",\"addr\":" << reinterpret_cast<uintptr_t>(payload);
                // Header del objeto (24 B).
                if (sz >= sizeof(loader::ObjectHeader)) {
                    const auto *oh = reinterpret_cast<
                        const loader::ObjectHeader *>(payload);
                    std::string cname = "?";
                    if (oh->class_ptr && oh->class_ptr->name.data
                     && oh->class_ptr->name.size > 0) {
                        cname.assign(
                            reinterpret_cast<const char *>(
                                oh->class_ptr->name.data),
                            oh->class_ptr->name.size);
                    }
                    d << ",\"class\":\"" << json_escape(cname) << "\""
                      << ",\"flags\":" << oh->flags
                      << ",\"hash_code\":" << oh->hash_code
                      << ",\"owner_pid\":" << oh->owner_pid
                      << ",\"lock_depth\":" << oh->lock_depth;
                }
                // Bytes raw (en hex como array).
                d << ",\"bytes\":[";
                for (uint32_t i = 0; i < dump_n; i++) {
                    if (i) d << ",";
                    d << static_cast<unsigned>(payload[i]);
                }
                d << "],\"truncated\":"
                  << (sz > dump_n ? "true" : "false") << "}";
                ok_resp(d.str());
                break;
            }

            case DebugCmd::FLAGS: {
                std::string pid_s = json_get(json_msg, "pid");
                if (pid_s.empty()) { err_resp("falta campo 'pid'"); return; }
                uint64_t pid = std::stoull(pid_s);
                runtime::ProcessVM *p = find_process_by_pid(vm_, pid);
                if (!p) { err_resp("proceso no encontrado"); return; }
                const auto &fl = p->registers.flags;
                std::ostringstream d;
                d << "{\"raw\":" << fl.raw
                  << ",\"sf\":" << static_cast<unsigned>(fl.bits.SF)
                  << ",\"zf\":" << static_cast<unsigned>(fl.bits.ZF)
                  << ",\"cf\":" << static_cast<unsigned>(fl.bits.CF)
                  << ",\"of\":" << static_cast<unsigned>(fl.bits.OF)
                  << ",\"dm\":" << static_cast<unsigned>(fl.bits.DM)
                  << "}";
                ok_resp(d.str());
                break;
            }

            case DebugCmd::FREGS: {
                std::string pid_s = json_get(json_msg, "pid");
                if (pid_s.empty()) { err_resp("falta campo 'pid'"); return; }
                uint64_t pid = std::stoull(pid_s);
                runtime::ProcessVM *p = find_process_by_pid(vm_, pid);
                if (!p) { err_resp("proceso no encontrado"); return; }
                std::ostringstream d;
                d << "{";
                for (int i = 0; i < 16; i++) {
                    if (i) d << ",";
                    // bytes bajos = double scalar; los emitimos como bits
                    // y como valor f64.  El cliente decide formato.  El
                    // banco ZMM tiene 64 bytes; nos quedamos con los 8
                    // primeros (representacion f64 escalar segun A.20).
                    uint64_t bits = 0;
                    const uint8_t *raw = p->registers.zmm[i].raw();
                    if (raw) std::memcpy(&bits, raw, sizeof(uint64_t));
                    double as_f64 = 0.0;
                    std::memcpy(&as_f64, &bits, sizeof(double));
                    d << "\"f" << i << "_bits\":" << bits;
                    // f64 puede ser NaN/Inf -> protegemos con %.17g
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%.17g", as_f64);
                    d << ",\"f" << i << "_f64\":\"" << buf << "\"";
                }
                d << "}";
                ok_resp(d.str());
                break;
            }

            case DebugCmd::DUMP_STACK: {
                std::string pid_s = json_get(json_msg, "pid");
                std::string n_s   = json_get(json_msg, "n");
                if (pid_s.empty()) { err_resp("falta campo 'pid'"); return; }
                uint64_t pid = std::stoull(pid_s);
                runtime::ProcessVM *p = find_process_by_pid(vm_, pid);
                if (!p) { err_resp("proceso no encontrado"); return; }
                uint32_t n = n_s.empty() ? 16
                    : static_cast<uint32_t>(std::stoul(n_s));
                if (n > 256) n = 256;
                uint64_t rsp = p->registers.stack_pointer.raw();
                uint64_t rbp = p->registers.base_pointer.raw();
                std::ostringstream d;
                d << "{\"rsp\":" << rsp << ",\"rbp\":" << rbp
                  << ",\"qwords\":[";
                for (uint32_t i = 0; i < n; i++) {
                    uint64_t addr = rsp + static_cast<uint64_t>(i) * 8;
                    uint64_t v = 0;
                    p->vm_mem.read_bytes(addr, &v, sizeof(uint64_t));
                    if (i) d << ",";
                    d << "{\"off\":" << (i * 8)
                      << ",\"addr\":" << addr
                      << ",\"v\":" << v << "}";
                }
                d << "]}";
                ok_resp(d.str());
                break;
            }

            case DebugCmd::FRAME_INFO: {
                std::string pid_s = json_get(json_msg, "pid");
                if (pid_s.empty()) { err_resp("falta campo 'pid'"); return; }
                uint64_t pid = std::stoull(pid_s);
                runtime::ProcessVM *p = find_process_by_pid(vm_, pid);
                if (!p) { err_resp("proceso no encontrado"); return; }
                uint64_t rsp = p->registers.stack_pointer.raw();
                uint64_t rbp = p->registers.base_pointer.raw();
                // Saved rbp esta en [rbp] (push rbp del prologue).
                // ret_addr en [rbp+8] (push del callvm previo).
                uint64_t saved_rbp = 0, ret_addr = 0;
                p->vm_mem.read_bytes(rbp, &saved_rbp, sizeof(uint64_t));
                p->vm_mem.read_bytes(rbp + 8, &ret_addr, sizeof(uint64_t));
                std::ostringstream d;
                d << "{\"rsp\":" << rsp
                  << ",\"rbp\":" << rbp
                  << ",\"saved_rbp\":" << saved_rbp
                  << ",\"ret_addr\":" << ret_addr
                  << ",\"frame_size\":" << (rbp >= rsp ? rbp - rsp : 0)
                  << "}";
                ok_resp(d.str());
                break;
            }

            case DebugCmd::BACKTRACE: {
                std::string pid_s = json_get(json_msg, "pid");
                if (pid_s.empty()) { err_resp("falta campo 'pid'"); return; }
                uint64_t pid = std::stoull(pid_s);
                runtime::ProcessVM *p = find_process_by_pid(vm_, pid);
                if (!p) { err_resp("proceso no encontrado"); return; }
                // Helper: resolver pc -> {file, line} via DebugInfo.
                auto resolve_src = [&](uint64_t pc) -> std::string {
                    for (const auto &exe : vm_.loader_public.executables) {
                        if (!exe || !exe->debug_info) continue;
                        debug::LineInfo li = exe->debug_info->lookup_line(
                            static_cast<uint32_t>(pc));
                        if (li.found) {
                            std::ostringstream s;
                            s << "\"" << json_escape(li.file ? li.file : "")
                              << "\"," << "\"line\":" << li.line;
                            return s.str();
                        }
                    }
                    return "null,\"line\":0";
                };
                std::ostringstream d;
                d << "{\"frames\":[";
                // Frame 0 = top (PC actual).
                uint64_t pc0 = p->registers.rip.raw();
                d << "{\"depth\":0,\"pc\":" << pc0
                  << ",\"file\":" << resolve_src(pc0);
                // Tratar de obtener nombre del metodo del frame_stack si existe.
                if (p->frame_stack && p->frame_stack->method
                 && p->frame_stack->method->name.data
                 && p->frame_stack->method->name.size > 0) {
                    std::string mn(
                        reinterpret_cast<const char *>(
                            p->frame_stack->method->name.data),
                        p->frame_stack->method->name.size);
                    d << ",\"method\":\"" << json_escape(mn) << "\"";
                } else {
                    d << ",\"method\":null";
                }
                d << "}";
                int depth = 1;
                for (loader::FrameHeader *f = p->frame_stack;
                     f != nullptr && depth < 64;
                     f = f->prev, depth++) {
                    d << ",{\"depth\":" << depth
                      << ",\"return_pc\":" << f->return_pc
                      << ",\"file\":" << resolve_src(f->return_pc);
                    if (f->method && f->method->name.data
                     && f->method->name.size > 0) {
                        std::string mn(
                            reinterpret_cast<const char *>(
                                f->method->name.data),
                            f->method->name.size);
                        d << ",\"method\":\"" << json_escape(mn) << "\"";
                    } else {
                        d << ",\"method\":null";
                    }
                    d << "}";
                }
                d << "]}";
                ok_resp(d.str());
                break;
            }

            case DebugCmd::DISASM: {
                std::string pid_s   = json_get(json_msg, "pid");
                std::string addr_s  = json_get(json_msg, "addr");
                std::string count_s = json_get(json_msg, "count");
                if (pid_s.empty()) { err_resp("falta 'pid'"); return; }
                uint64_t pid    = std::stoull(pid_s);
                runtime::ProcessVM *p = find_process_by_pid(vm_, pid);
                if (!p) { err_resp("proceso no encontrado"); return; }
                uint64_t addr   = addr_s.empty() ? p->registers.rip.raw()
                                                 : std::stoull(addr_s);
                uint32_t count  = count_s.empty() ? 16
                                  : static_cast<uint32_t>(std::stoul(count_s));
                if (count > 256) count = 256;
                static constexpr size_t size_bytes_table[] =
                    {1, 2, 4, 6, 8, 10, 11};
                std::ostringstream d;
                d << "{\"start\":" << addr << ",\"items\":[";
                bool first = true;
                for (uint32_t i = 0; i < count; ++i) {
                    uint8_t b0 = 0;
                    p->vm_mem.read_bytes(addr, &b0, 1);
                    runtime::InstrFormat *fmt = nullptr;
                    uint8_t op_ext = 0;
                    if (b0 == 0x00) {
                        p->vm_mem.read_bytes(addr + 1, &op_ext, 1);
                        fmt = &runtime::decode_table_extended[op_ext];
                    } else {
                        fmt = &runtime::decode_table_primary[b0];
                    }
                    size_t sz = 1;
                    int sm = static_cast<int>(fmt->size);
                    if (sm >= 0 && sm < (int)(sizeof(size_bytes_table)/sizeof(size_bytes_table[0]))) {
                        sz = size_bytes_table[sm];
                    }
                    if (sz < 1) sz = 1;
                    if (sz > 16) sz = 16;
                    uint8_t bytes[16] = {0};
                    p->vm_mem.read_bytes(addr, bytes, sz);
                    if (!first) d << ",";
                    first = false;
                    d << "{\"addr\":" << addr
                      << ",\"opcode\":" << (b0 == 0x00 ? (int)op_ext : (int)b0)
                      << ",\"ext\":" << (b0 == 0x00 ? "true" : "false")
                      << ",\"name\":\""
                      << json_escape(fmt && fmt->name ? fmt->name : "?")
                      << "\",\"size\":" << sz
                      << ",\"bytes\":\"";
                    for (size_t k = 0; k < sz; ++k) {
                        char buf[4];
                        std::snprintf(buf, sizeof(buf), "%02x", bytes[k]);
                        d << buf;
                    }
                    d << "\"}";
                    addr += sz;
                }
                d << "]}";
                ok_resp(d.str());
                break;
            }

            case DebugCmd::LOCALS: {
                std::string pid_s = json_get(json_msg, "pid");
                if (pid_s.empty()) { err_resp("falta 'pid'"); return; }
                uint64_t pid = std::stoull(pid_s);
                runtime::ProcessVM *p = find_process_by_pid(vm_, pid);
                if (!p) { err_resp("proceso no encontrado"); return; }
                uint64_t pc  = p->registers.rip.raw();
                uint64_t rbp = p->registers.base_pointer.raw();
                std::vector<debug::VarInfo> vars;
                for (const auto &exe : vm_.loader_public.executables) {
                    if (!exe || !exe->debug_info) continue;
                    auto v = exe->debug_info->lookup_vars(
                        static_cast<uint32_t>(pc));
                    if (!v.empty()) { vars = std::move(v); break; }
                }
                std::ostringstream d;
                d << "{\"pc\":" << pc << ",\"rbp\":" << rbp << ",\"vars\":[";
                bool first = true;
                for (const auto &v : vars) {
                    if (!first) d << ",";
                    first = false;
                    int64_t off = static_cast<int64_t>(v.stack_offset);
                    uint64_t addr = (off >= 0)
                        ? rbp + static_cast<uint64_t>(off)
                        : rbp - static_cast<uint64_t>(-off);
                    uint64_t value = 0;
                    p->vm_mem.read_bytes(addr, &value, sizeof(uint64_t));
                    d << "{\"name\":\"" << json_escape(v.name ? v.name : "")
                      << "\",\"offset\":" << off
                      << ",\"addr\":" << addr
                      << ",\"value\":" << value
                      << ",\"type\":" << (int)v.type_kind
                      << ",\"flags\":" << (int)v.var_flags << "}";
                }
                d << "]}";
                ok_resp(d.str());
                break;
            }

            case DebugCmd::GC_RUN: {
                std::string pid_s = json_get(json_msg, "pid");
                if (pid_s.empty()) { err_resp("falta 'pid'"); return; }
                uint64_t pid = std::stoull(pid_s);
                runtime::ProcessVM *p = find_process_by_pid(vm_, pid);
                if (!p) { err_resp("proceso no encontrado"); return; }
                auto count_live = [&]() -> size_t {
                    size_t n = 0;
                    size_t total = p->gc_heap.handle_table_size();
                    for (size_t i = 0; i < total; ++i) {
                        if (p->gc_heap.is_handle_live(static_cast<gc::GcHandle>(i))) ++n;
                    }
                    return n;
                };
                size_t before = count_live();
                p->gc_heap.major_gc();
                size_t after  = count_live();
                std::ostringstream d;
                d << "{\"before\":" << before
                  << ",\"after\":" << after
                  << ",\"freed\":" << (before > after ? before - after : 0)
                  << "}";
                ok_resp(d.str());
                break;
            }

            case DebugCmd::STEP_OUT: {
                std::string pid_s = json_get(json_msg, "pid");
                if (pid_s.empty()) { err_resp("falta 'pid'"); return; }
                uint64_t pid = std::stoull(pid_s);
                runtime::ProcessVM *p = find_process_by_pid(vm_, pid);
                if (!p) { err_resp("proceso no encontrado"); return; }
                if (!p->frame_stack) {
                    err_resp("step_out: el proceso no tiene frame OOP activo");
                    return;
                }
                uint64_t ret_pc = p->frame_stack->return_pc;
                Breakpoint bp{};
                {
                    std::lock_guard<std::mutex> lk(bp_mutex_);
                    bp.id        = next_bp_id_++;
                    bp.addr      = ret_pc;
                    bp.pid       = pid;
                    bp.enabled   = true;
                    bp.hit_count = 0;
                    bp.one_shot  = true;
                    breakpoints_.push_back(bp);
                    any_bp_.store(true, std::memory_order_relaxed);
                }
                {
                    std::lock_guard<std::mutex> lk(proc_mutex_);
                    auto it = proc_ctx_.find(pid);
                    if (it != proc_ctx_.end()) {
                        it->second.state    = DbgProcState::RUNNING;
                        it->second.step_mode = false;
                        it->second.pause_cv.notify_all();
                        any_step_.store(any_step_or_paused_locked(proc_ctx_),
                                        std::memory_order_release);
                    }
                }
                std::ostringstream d;
                d << "{\"target_pc\":" << ret_pc
                  << ",\"bp_id\":" << bp.id << "}";
                ok_resp(d.str());
                break;
            }

            case DebugCmd::STEP_UNTIL: {
                std::string pid_s   = json_get(json_msg, "pid");
                std::string file_s  = json_get(json_msg, "file");
                std::string line_s  = json_get(json_msg, "line");
                std::string addr_s  = json_get(json_msg, "addr");
                if (pid_s.empty()) { err_resp("falta 'pid'"); return; }
                uint64_t pid = std::stoull(pid_s);
                uint64_t target = 0;
                if (!addr_s.empty()) {
                    target = std::stoull(addr_s);
                } else if (!file_s.empty() && !line_s.empty()) {
                    uint32_t target_line = (uint32_t)std::stoul(line_s);
                    uint32_t off = UINT32_MAX;
                    for (const auto &exe : vm_.loader_public.executables) {
                        if (!exe || !exe->debug_info) continue;
                        off = exe->debug_info->lookup_offset_for_line(
                            file_s, target_line);
                        if (off != UINT32_MAX) break;
                    }
                    if (off == UINT32_MAX) {
                        err_resp("until: file:line no encontrado en debug info");
                        return;
                    }
                    target = off;
                } else {
                    err_resp("until: requiere 'addr' o ('file'+'line')");
                    return;
                }
                Breakpoint bp{};
                {
                    std::lock_guard<std::mutex> lk(bp_mutex_);
                    bp.id        = next_bp_id_++;
                    bp.addr      = target;
                    bp.pid       = pid;
                    bp.enabled   = true;
                    bp.hit_count = 0;
                    bp.one_shot  = true;
                    breakpoints_.push_back(bp);
                    any_bp_.store(true, std::memory_order_relaxed);
                }
                {
                    std::lock_guard<std::mutex> lk(proc_mutex_);
                    auto it = proc_ctx_.find(pid);
                    if (it != proc_ctx_.end()) {
                        it->second.state    = DbgProcState::RUNNING;
                        it->second.step_mode = false;
                        it->second.pause_cv.notify_all();
                        any_step_.store(any_step_or_paused_locked(proc_ctx_),
                                        std::memory_order_release);
                    }
                }
                std::ostringstream d;
                d << "{\"target_pc\":" << target
                  << ",\"bp_id\":" << bp.id << "}";
                ok_resp(d.str());
                break;
            }

            case DebugCmd::SET_WATCH: {
                std::string pid_s   = json_get(json_msg, "pid");
                std::string h_s     = json_get(json_msg, "handle");
                if (h_s.empty()) { err_resp("falta 'handle'"); return; }
                uint64_t pid = pid_s.empty() ? 0 : std::stoull(pid_s);
                uint64_t h   = std::stoull(h_s);
                Watchpoint wp{};
                {
                    std::lock_guard<std::mutex> lk(watch_mutex_);
                    wp.id        = next_watch_id_++;
                    wp.pid       = pid;
                    wp.handle    = h;
                    wp.enabled   = true;
                    runtime::ProcessVM *p = pid ? find_process_by_pid(vm_, pid)
                                                : nullptr;
                    if (!p) {
                        for (auto &sched : vm_.schedulers) {
                            for (auto &px : sched->processes) {
                                if (px) { p = px.get(); break; }
                            }
                            if (p) break;
                        }
                    }
                    if (p && p->gc_heap.is_handle_live(
                            static_cast<gc::GcHandle>(h))) {
                        wp.was_alive = true;
                        wp.last_addr = reinterpret_cast<uint64_t>(
                            p->gc_heap.deref(static_cast<gc::GcHandle>(h)));
                    } else {
                        wp.was_alive = false;
                        wp.last_addr = 0;
                    }
                    watchpoints_.push_back(wp);
                    any_watch_.store(true, std::memory_order_relaxed);
                }
                std::ostringstream d;
                d << "{\"id\":" << wp.id
                  << ",\"handle\":" << wp.handle
                  << ",\"alive\":" << (wp.was_alive ? "true" : "false")
                  << ",\"addr\":" << wp.last_addr << "}";
                ok_resp(d.str());
                break;
            }

            case DebugCmd::DEL_WATCH: {
                std::string id_s = json_get(json_msg, "id");
                if (id_s.empty()) { err_resp("falta 'id'"); return; }
                uint32_t wid = (uint32_t)std::stoul(id_s);
                {
                    std::lock_guard<std::mutex> lk(watch_mutex_);
                    watchpoints_.erase(
                        std::remove_if(watchpoints_.begin(), watchpoints_.end(),
                            [wid](const Watchpoint &w){ return w.id == wid; }),
                        watchpoints_.end());
                    any_watch_.store(!watchpoints_.empty(),
                                     std::memory_order_relaxed);
                }
                ok_resp("{}");
                break;
            }

            case DebugCmd::LIST_WATCHES: {
                std::ostringstream d;
                d << "[";
                {
                    std::lock_guard<std::mutex> lk(watch_mutex_);
                    bool first = true;
                    for (const auto &w : watchpoints_) {
                        if (!first) d << ",";
                        first = false;
                        d << "{\"id\":" << w.id
                          << ",\"pid\":" << w.pid
                          << ",\"handle\":" << w.handle
                          << ",\"enabled\":" << (w.enabled ? "true" : "false")
                          << ",\"alive\":" << (w.was_alive ? "true" : "false")
                          << ",\"addr\":" << w.last_addr << "}";
                    }
                }
                d << "]";
                ok_resp(d.str());
                break;
            }

            case DebugCmd::TRACE_MSGS: {
                std::string pid_s = json_get(json_msg, "pid");
                std::string en_s  = json_get(json_msg, "enabled");
                if (pid_s.empty()) { err_resp("falta 'pid'"); return; }
                uint64_t pid = std::stoull(pid_s);
                bool en = (en_s == "true" || en_s == "1");
                {
                    std::lock_guard<std::mutex> lk(trace_mutex_);
                    if (en) traced_msg_pids_.insert(pid);
                    else    traced_msg_pids_.erase(pid);
                    any_msg_trace_.store(!traced_msg_pids_.empty(),
                                         std::memory_order_relaxed);
                }
                ok_resp("{}");
                break;
            }

            case DebugCmd::BREAK_MON: {
                std::string en_s = json_get(json_msg, "enabled");
                bool en = (en_s == "true" || en_s == "1");
                break_on_mon_.store(en, std::memory_order_relaxed);
                ok_resp("{}");
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
                    any_step_.store(true, std::memory_order_release);
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
