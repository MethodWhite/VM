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
 * @file debugger.h
 * @brief Servidor de depuracion interactivo para VestaVM.
 *
 * Implementa un servidor de depuracion con protocolo propio sobre TCP.
 * Soporta dos modelos de despliegue:
 *
 *   In-process:  el servidor corre en un hilo de fondo dentro del proceso VM.
 *                Se activa con Debugger::start_inprocess(port).
 *
 *   Out-of-process: el cliente externo (un debugger o IDE) se conecta al
 *                   mismo puerto TCP.  El servidor es identico en ambos casos.
 *
 * Protocolo de red:
 *   Mensajes con framing de longitud: [uint32_t len LE][payload UTF-8 JSON]
 *   El cliente envia comandos JSON; el servidor responde con JSON.
 *   El servidor puede enviar eventos de forma assincrona (breakpoints, etc.).
 *
 * Formato de comando:
 * @code
 *   {"cmd": "nombre_comando", "seq": 1, ...campos especificos... }
 * @endcode
 *
 * Formato de respuesta:
 * @code
 *   {"ok": true,  "seq": 1, "data": {...}}
 *   {"ok": false, "seq": 1, "error": "mensaje"}
 * @endcode
 *
 * Formato de evento (push assincrono):
 * @code
 *   {"event": "nombre_evento", ...campos... }
 * @endcode
 *
 * Comandos soportados (ver DebugCmd):
 *   attach, detach, list_procs, set_break, del_break,
 *   continue, step, next, registers, memory, stack,
 *   info_proc, eval, pause, list_breaks
 *
 * Eventos emitidos (ver DebugEvent):
 *   break, exit, exception, spawned
 */

#ifndef DEBUGGER_H
#define DEBUGGER_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <unordered_set>

// Solo declaraciones hacia adelante para evitar dependencias circulares
namespace runtime {
    struct ProcessVM;
    class VM;
}

namespace debug {

    // =========================================================================
    //  Constantes del protocolo
    // =========================================================================

    /** @brief Puerto TCP por defecto del servidor de depuracion. */
    static constexpr uint16_t DBG_DEFAULT_PORT = 9229;

    /** @brief Tamano maximo de un mensaje del protocolo (4 MB). */
    static constexpr uint32_t DBG_MAX_MSG_SIZE = 4 * 1024 * 1024;

    /** @brief Magic de 4 bytes en la cabecera del handshake. */
    static constexpr uint32_t DBG_HANDSHAKE_MAGIC = 0x56444247u; // "VDBG"

    // =========================================================================
    //  Comandos del protocolo
    // =========================================================================

    /**
     * @brief Identificadores de comando del protocolo de depuracion.
     *
     * El cliente envia el nombre como cadena JSON; el servidor lo decodifica
     * en este enum para el dispatch interno.
     */
    enum class DebugCmd : uint8_t {
        UNKNOWN      = 0,
        ATTACH       = 1,  ///< adjuntarse a un proceso virtual
        DETACH       = 2,  ///< desadjuntarse
        LIST_PROCS   = 3,  ///< listar todos los procesos de la VM
        SET_BREAK    = 4,  ///< poner breakpoint en direccion VM
        DEL_BREAK    = 5,  ///< eliminar breakpoint por id
        LIST_BREAKS  = 6,  ///< listar breakpoints activos
        CONTINUE     = 7,  ///< continuar ejecucion tras break
        STEP         = 8,  ///< step-into (una instruccion)
        NEXT         = 9,  ///< step-over (salta llamadas)
        REGISTERS    = 10, ///< obtener estado de registros del proceso
        MEMORY       = 11, ///< leer bytes de memoria VM
        STACK        = 12, ///< obtener traza de pila
        INFO_PROC    = 13, ///< informacion de un proceso virtual
        EVAL         = 14, ///< evaluar expresion simple (nombre de registro)
        PAUSE        = 15, ///< forzar pausa del proceso
    };

    /**
     * @brief Convierte una cadena de nombre de comando a DebugCmd.
     * @param name Nombre del comando en texto.
     * @return Comando correspondiente, o UNKNOWN si no se reconoce.
     */
    DebugCmd debug_cmd_parse(const std::string &name);

    // =========================================================================
    //  Eventos del protocolo
    // =========================================================================

    /**
     * @brief Tipos de evento que el servidor puede enviar al cliente.
     */
    enum class DebugEvent : uint8_t {
        BREAK     = 1,  ///< proceso detenido en breakpoint
        EXIT      = 2,  ///< proceso terminado
        EXCEPTION = 3,  ///< excepcion no capturada
        SPAWNED   = 4,  ///< proceso hijo creado
        STEPPED   = 5,  ///< paso de instruccion completado
    };

    // =========================================================================
    //  Breakpoint
    // =========================================================================

    /**
     * @brief Descriptor de un breakpoint.
     *
     * Un breakpoint se identifica por su id unico (asignado al insertarlo)
     * y por la direccion VM absoluta donde se activa.
     */
    struct Breakpoint {
        uint32_t id;       ///< identificador unico (1-based, 0 = invalido)
        uint64_t addr;     ///< direccion VM absoluta
        uint64_t pid;      ///< 0 = todos los procesos; != 0 = proceso especifico
        bool     enabled;  ///< false = desactivado temporalmente
        uint32_t hit_count;///< numero de veces que se ha activado
    };

    // =========================================================================
    //  Estado de depuracion de un proceso
    // =========================================================================

    /**
     * @brief Estado del ciclo de depuracion de un proceso virtual.
     */
    enum class DbgProcState : uint8_t {
        RUNNING  = 0, ///< en ejecucion normal
        PAUSED   = 1, ///< detenido en breakpoint o paso
        DETACHED = 2, ///< no adjunto al depurador
    };

    /**
     * @brief Contexto de depuracion por proceso.
     */
    struct DbgProcCtx {
        uint64_t      pid;         ///< PID local del proceso
        DbgProcState  state;       ///< estado actual de depuracion
        bool          step_mode;   ///< true = parar en cada instruccion
        bool          next_mode;   ///< true = step-over (no entrar en llamadas)
        uint32_t      call_depth;  ///< profundidad de llamada al iniciar next
        std::mutex    pause_mu;    ///< mutex para la pausa/reanudacion
        std::condition_variable pause_cv; ///< cv para bloquear el proceso en pausa
    };

    // =========================================================================
    //  Servidor de depuracion
    // =========================================================================

    /**
     * @brief Servidor de depuracion TCP in-process para VestaVM.
     *
     * El servidor acepta conexiones TCP entrantes de clientes de depuracion.
     * Cada conexion se atiende en un hilo independiente.
     *
     * Uso tipico (in-process):
     * @code
     *   Debugger dbg(vm);
     *   dbg.start(9229);
     *   // ... ejecutar la VM normalmente ...
     *   dbg.stop();
     * @endcode
     *
     * El servidor es thread-safe: puede ser llamado desde cualquier hilo
     * de la VM mientras el servidor esta activo.
     */
    class Debugger {
    public:
        /**
         * @brief Constructor.
         * @param vm Instancia VM a depurar.
         */
        explicit Debugger(runtime::VM &vm);

        /**
         * @brief Destructor. Llama a stop() automaticamente.
         */
        ~Debugger();

        /**
         * @brief Inicia el servidor de depuracion en el puerto indicado.
         *
         * Lanza un hilo de aceptacion TCP en background.  El servidor
         * sigue activo hasta llamar a stop().
         *
         * @param port Puerto TCP a escuchar (por defecto DBG_DEFAULT_PORT).
         * @return true si el servidor arranco correctamente.
         */
        bool start(uint16_t port = DBG_DEFAULT_PORT);

        /**
         * @brief Detiene el servidor y cierra todas las conexiones activas.
         */
        void stop();

        /**
         * @brief Devuelve true si el servidor esta activo.
         */
        bool is_running() const { return running_.load(); }

        /**
         * @brief Llama a este metodo desde el bucle de ejecucion de la VM,
         *        justo antes de ejecutar cada instruccion.
         *
         * Si el proceso esta en step_mode o su PC coincide con un breakpoint,
         * suspende el hilo hasta que el depurador emita CONTINUE o STEP.
         *
         * Esta funcion debe ser de bajo overhead en el caso normal (sin
         * breakpoints ni step): comprueba solo un atomic antes de retornar.
         *
         * @param proc Proceso que va a ejecutar la siguiente instruccion.
         */
        void on_before_exec(runtime::ProcessVM *proc);

        /**
         * @brief Notifica al servidor que un proceso ha terminado.
         * @param pid PID local del proceso terminado.
         */
        void on_process_exit(uint64_t pid);

        /**
         * @brief Notifica al servidor que un proceso lanzo una excepcion.
         * @param pid       PID local del proceso.
         * @param exc_class Nombre de la clase de excepcion.
         */
        void on_exception(uint64_t pid, const std::string &exc_class);

        /**
         * @brief Notifica que un proceso hijo fue creado.
         * @param parent_pid PID del padre.
         * @param child_pid  PID del hijo.
         */
        void on_process_spawn(uint64_t parent_pid, uint64_t child_pid);

    private:
        runtime::VM &vm_;     ///< referencia a la instancia VM
        uint16_t     port_;   ///< puerto TCP del servidor

        std::atomic<bool> running_{false};  ///< estado del servidor
        std::atomic<bool> any_bp_{false};   ///< hay breakpoints activos (fast path)

        std::thread accept_thread_;  ///< hilo de aceptacion TCP

        /* --- estado de breakpoints --- */
        std::mutex                            bp_mutex_;
        std::vector<Breakpoint>               breakpoints_;
        uint32_t                              next_bp_id_{1};

        /* --- estado por proceso depurado --- */
        std::mutex                            proc_mutex_;
        std::unordered_map<uint64_t, DbgProcCtx> proc_ctx_; ///< pid -> contexto

        /* --- clientes conectados --- */
        std::mutex                            client_mutex_;
        std::vector<int>                      client_fds_;  ///< sockets activos

        /* --- hilo de aceptacion --- */
        int server_fd_{-1};  ///< socket de escucha

        /**
         * @brief Bucle principal del hilo de aceptacion TCP.
         */
        void accept_loop();

        /**
         * @brief Atiende a un cliente conectado (en su propio hilo).
         * @param client_fd Descriptor del socket del cliente.
         */
        void client_loop(int client_fd);

        /**
         * @brief Procesa un comando JSON recibido del cliente.
         * @param json_msg  Payload JSON del comando.
         * @param client_fd Socket del cliente para la respuesta.
         */
        void handle_command(const std::string &json_msg, int client_fd);

        /**
         * @brief Envía una respuesta JSON al cliente.
         * @param client_fd Socket destino.
         * @param json      Payload JSON de la respuesta.
         */
        void send_msg(int client_fd, const std::string &json);

        /**
         * @brief Emite un evento JSON a todos los clientes conectados.
         * @param json Payload JSON del evento.
         */
        void broadcast_event(const std::string &json);

        /**
         * @brief Busca si el PC indicado tiene un breakpoint activo.
         * @param pc  Direccion VM del Program Counter.
         * @param pid PID del proceso (para filtrar por proceso especifico).
         * @return Puntero al Breakpoint encontrado, o nullptr si no hay.
         */
        Breakpoint *find_breakpoint(uint64_t pc, uint64_t pid);

        /**
         * @brief Obtiene o crea el contexto de depuracion de un proceso.
         * @param pid PID del proceso.
         * @return Referencia al DbgProcCtx.
         */
        DbgProcCtx &get_or_create_ctx(uint64_t pid);

        /* --- helpers de serializacion JSON minima --- */

        /**
         * @brief Construye el JSON de respuesta de estado de registros.
         * @param proc Proceso a inspeccionar.
         * @return Cadena JSON con los 16 registros + PC + SP + BP.
         */
        std::string regs_to_json(runtime::ProcessVM *proc);

        /**
         * @brief Construye el JSON de un volcado de memoria.
         * @param proc   Proceso cuya memoria VM leer.
         * @param addr   Direccion VM inicial.
         * @param length Numero de bytes a leer.
         * @return Cadena JSON con el array de bytes en hexadecimal.
         */
        std::string mem_to_json(runtime::ProcessVM *proc,
                                uint64_t addr, uint32_t length);
    };

} // namespace debug

#endif /* DEBUGGER_H */
