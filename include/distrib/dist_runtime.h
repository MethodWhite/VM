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
 * @file dist_runtime.h
 * @brief Coordinador del sistema de programacion distribuida de VestaVM.
 *
 * DistRuntime es el componente central que coordina todas las operaciones
 * distribuidas de una instancia VM.  Actua como router, gestor de sesiones
 * y punto de entrada para las instrucciones de bytecode distribuidas.
 *
 * Responsabilidades:
 *   - Gestionar el NodeRegistry (tabla de nodos conocidos).
 *   - Iniciar y mantener VdpSession hacia cada nodo del registro.
 *   - Enrutar mensajes entrantes al proceso destino correcto.
 *   - Implementar la logica de rspawn, msgsend, msgrecv, memsync.
 *   - Gestionar futuros distribuidos (rspawn devuelve un FutureObject).
 *   - Exponer un servidor VDP (VdpServer) para conexiones entrantes.
 *
 * Una instancia VM tiene exactamente un DistRuntime.
 * Se crea en VM::VM() y se destruye en ~VM().
 *
 * Uso desde bytecode (a traves de las funciones exec_instr_*):
 * @code
 *   // rspawn r_fn, r_node  ->  r0 = GcHandle del future
 *   exec_instr_rspawn(vm, decoded_instr);
 *   // msgsend r_pid, r_addr, r_len
 *   exec_instr_msgsend(vm, decoded_instr);
 *   // msgrecv r_buf, r_max_len  ->  r0 = bytes recibidos
 *   exec_instr_msgrecv(vm, decoded_instr);
 *   // memsync r_params
 *   exec_instr_memsync(vm, decoded_instr);
 * @endcode
 */

#ifndef DIST_RUNTIME_H
#define DIST_RUNTIME_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#include <memory>
#include <functional>

#include "distrib/vdp_protocol.h"
#include "distrib/node_registry.h"
#include "distrib/vdp_session.h"
#include "runtime/pid.h"

// Declaraciones adelantadas para evitar dependencias circulares
namespace runtime {
    class VM;
    class ProcessVM;
}
namespace loader {
    struct FutureObject;
}
class TCPServer;

namespace distrib {

class Mailbox;
class DirtyTracker;

// ---------------------------------------------------------------------------
// Entrada en la tabla de futuros distribuidos pendientes
// ---------------------------------------------------------------------------

/**
 * @brief Registro de un future pendiente de respuesta remota.
 *
 * Cuando rspawn envia un VDP_RSPAWN, crea esta entrada para poder
 * correlacionar el VDP_RSPAWN_ACK con el future local que debe resolverse.
 */
struct PendingFuture {
    uint64_t   future_id;    // ID opaco enviado en VDP_RSPAWN (indice en la tabla)
    uint32_t   gc_handle;    // GcHandle del FutureObject en el GC del proceso solicitante
    GlobalPID  requester;    // PID del proceso que ejecuto rspawn (para make_ready tras await)
};

// ---------------------------------------------------------------------------
// Entrada en la tabla de mensajes pendientes de ACK (at-least-once)
// ---------------------------------------------------------------------------

/**
 * @brief Mensaje pendiente de confirmacion de entrega.
 *
 * Para at-least-once: el emisor guarda el mensaje hasta recibir
 * VDP_MSGSEND_ACK.  Si no llega en retry_ms ms, reintenta.
 */
struct PendingMsg {
    uint32_t             seq_num;     // numero de secuencia del MSGSEND
    uint32_t             node_idx;    // nodo al que se envio
    std::vector<uint8_t> payload;     // payload completo del VDP_MSGSEND (para reintento)
    uint64_t             sent_at_ms;  // marca de tiempo en ms del ultimo envio
    uint32_t             retries;     // intentos realizados
    static constexpr uint32_t MAX_RETRIES = 5;   // maximo de reintentos
    static constexpr uint64_t RETRY_MS    = 2000; // ms entre reintentos
};

// ---------------------------------------------------------------------------
// Configuracion del DistRuntime
// ---------------------------------------------------------------------------

/**
 * @brief Configuracion de arranque del DistRuntime.
 */
struct DistRuntimeConfig {
    uint64_t       local_node_id;    // ID de este nodo (0 = generar automaticamente)
    char           local_node_name[32]; // nombre legible
    uint16_t       vdp_listen_port;  // puerto del servidor VDP (0 = usar VDP_LISTEN_PORT)
    uint16_t       discover_port;    // puerto UDP de descubrimiento (0 = usar VDP_DISCOVER_PORT)
    bool           enable_discovery; // true = iniciar descubrimiento dinamico UDP
    NodeAuthConfig server_auth;      // configuracion de autenticacion del servidor
};

// ---------------------------------------------------------------------------
// Clase principal DistRuntime
// ---------------------------------------------------------------------------

/**
 * @brief Coordinador del sistema de programacion distribuida.
 *
 * Instanciado una sola vez por VM.  Los exec_instr_* de las instrucciones
 * distribuidas obtienen el DistRuntime via vm->scheduler.vm->dist_runtime.
 */
class DistRuntime {
public:
    /**
     * @brief Construye el DistRuntime y lo asocia a la VM indicada.
     * @param vm     Instancia VM propietaria de este DistRuntime.
     * @param config Configuracion de arranque.
     */
    DistRuntime(runtime::VM &vm, const DistRuntimeConfig &config);

    /**
     * @brief Destructor: detiene el servidor VDP, cierra sesiones y limpia recursos.
     */
    ~DistRuntime();

    // Sin copia ni movimiento
    DistRuntime(const DistRuntime &) = delete;
    DistRuntime &operator=(const DistRuntime &) = delete;

    // ----------------------------------------------------------------
    // Arranque y parada
    // ----------------------------------------------------------------

    /**
     * @brief Inicia el servidor VDP y el descubrimiento dinamico si esta habilitado.
     *
     * Lanza el hilo del servidor TCP que acepta conexiones VDP entrantes.
     * Si enable_discovery==true, inicia el hilo de broadcast UDP.
     *
     * @return true si el arranque fue exitoso.
     */
    bool start();

    /**
     * @brief Detiene el servidor VDP, el descubrimiento y cierra todas las sesiones.
     */
    void stop();

    // ----------------------------------------------------------------
    // Gestion de nodos y sesiones
    // ----------------------------------------------------------------

    /**
     * @brief Agrega un nodo estatico al registro y conecta inmediatamente.
     *
     * Llama a NodeRegistry::add_static() y lanza una conexion saliente.
     *
     * @param ip    IP o hostname del nodo.
     * @param port  Puerto TCP del servidor VDP.
     * @param auth  Configuracion de autenticacion.
     * @param name  Nombre legible del nodo.
     * @return Indice del nodo en el registro.
     */
    uint32_t add_node(const char *ip, uint16_t port,
                      const NodeAuthConfig &auth,
                      const char *name = "");

    /**
     * @brief Conecta a un nodo del registro (por indice) si no esta conectado.
     * @param node_idx Indice del nodo en NodeRegistry.
     * @return true si la conexion y autenticacion fueron exitosas.
     */
    bool connect_node(uint32_t node_idx);

    /**
     * @brief Referencia al registro de nodos.
     */
    NodeRegistry &registry() { return registry_; }

    /**
     * @brief Configuracion de arranque (solo lectura).
     */
    const DistRuntimeConfig &config() const { return config_; }

    // ----------------------------------------------------------------
    // Operaciones distribuidas (llamadas desde exec_instr_*)
    // ----------------------------------------------------------------

    /**
     * @brief Implementa la logica de la instruccion rspawn.
     *
     * Serializa el bytecode del proceso, envia VDP_RSPAWN al nodo indicado
     * y retorna el GcHandle de un FutureObject pendiente.
     *
     * @param proc     Proceso que ejecuta la instruccion.
     * @param fn_addr  Direccion virtual del bytecode a ejecutar en el remoto.
     * @param node_idx Indice del nodo destino en NodeRegistry.
     * @return GcHandle del FutureObject creado (0xFFFFFFFF si error).
     */
    uint32_t rspawn(runtime::ProcessVM *proc,
                    uint64_t fn_addr,
                    uint32_t node_idx);

    /**
     * @brief Envia un fichero .velb completo al nodo remoto para ejecutarlo.
     *
     * A diferencia de rspawn(), que lee bytecode ya resuelto de la memoria del
     * proceso, rspawn_velb() envia los bytes raw del fichero .velb para que el
     * nodo remoto llame a load_executable() y resuelva sus propias importaciones.
     * Esto permite que instrucciones calln funcionen en el remoto con las
     * bibliotecas nativas instaladas en ese nodo.
     *
     * @param proc       Proceso cuyo gc_heap albergara el FutureObject de resultado.
     * @param velb_bytes Contenido completo del fichero .velb.
     * @param node_idx   Indice del nodo destino en NodeRegistry.
     * @return GcHandle del FutureObject creado (0xFFFFFFFF si error).
     */
    uint32_t rspawn_velb(runtime::ProcessVM *proc,
                         const std::vector<uint8_t> &velb_bytes,
                         uint32_t node_idx);

    /**
     * @brief Notifica al nodo origen que un proceso remoto (creado por rspawn) ha terminado.
     *
     * Llamado por el scheduler cuando un proceso con rspawn_future_id != 0 alcanza
     * el estado HALT o DEAD.  Envia VDP_FUTURE_FULFILL con r0 al nodo origen para
     * que el await del proceso cliente se desbloquee con el valor real de retorno.
     *
     * @param proc Proceso que termino; debe tener rspawn_future_id != 0.
     */
    void notify_rspawn_halt(runtime::ProcessVM *proc);

    /**
     * @brief Implementa la logica de la instruccion msgsend.
     *
     * Si el PID es local: deposita el mensaje en el mailbox del proceso.
     * Si el PID es remoto: envia VDP_MSGSEND y registra el mensaje como pendiente
     *   de ACK para reintento at-least-once.
     *
     * @param proc       Proceso que ejecuta la instruccion (emisor).
     * @param target_pid PID codificado del proceso destino (local o remoto).
     * @param vm_addr    Direccion virtual del buffer del mensaje en VM memory.
     * @param len        Longitud del mensaje en bytes.
     * @return true si el mensaje fue encolado o enviado.
     */
    bool msgsend(runtime::ProcessVM *proc,
                 uint64_t target_pid,
                 uint64_t vm_addr,
                 uint64_t len);

    /**
     * @brief Implementa la logica de la instruccion msgrecv.
     *
     * Intenta extraer un mensaje del mailbox del proceso.
     * Si el mailbox esta vacio: bloquea el proceso (blocking=true) y retorna 0.
     * El proceso sera despertado (make_ready) cuando llegue un mensaje.
     *
     * @param proc      Proceso que ejecuta la instruccion (receptor).
     * @param buf_addr  Direccion virtual del buffer de destino en VM memory.
     * @param max_len   Tamano maximo del buffer en bytes.
     * @return Bytes copiados al buffer, o 0 si el proceso quedo bloqueado.
     */
    uint64_t msgrecv(runtime::ProcessVM *proc,
                     uint64_t buf_addr,
                     uint64_t max_len);

    /**
     * @brief Implementa la logica de la instruccion memsync.
     *
     * Lee MemsyncParams de VM memory, detecta paginas modificadas via DirtyTracker
     * y sincroniza con el nodo remoto usando el protocolo MEMSYNC_HASH/DIFF/DATA/ACK.
     *
     * Si params.future_handle != 0: la sincronizacion es asincrona y resuelve
     *   el future cuando termine.  Si es 0: bloquea hasta completar.
     *
     * @param proc       Proceso que ejecuta la instruccion.
     * @param params_addr Direccion virtual de la estructura MemsyncParams en VM memory.
     */
    void memsync(runtime::ProcessVM *proc, uint64_t params_addr);

    /**
     * @brief Obtiene o crea el Mailbox del proceso indicado.
     *
     * Si el proceso no tiene mailbox aun, lo crea y lo asocia.
     *
     * @param proc Proceso para el que se quiere el mailbox.
     * @return Puntero al Mailbox del proceso.
     */
    Mailbox *get_or_create_mailbox(runtime::ProcessVM *proc);

    /**
     * @brief ID de este nodo VDP.
     */
    uint64_t local_node_id() const { return config_.local_node_id; }

    /**
     * @brief Registra una sesion entrante ya autenticada.
     *
     * Llamado por VdpServer tras completar el handshake de un cliente entrante.
     * Inicia el hilo lector de la sesion y actualiza el NodeRegistry.
     *
     * @param sess Sesion autenticada (la propiedad pasa a DistRuntime).
     */
    void on_inbound_session_(VdpSession *sess);

private:
    runtime::VM            &vm_;         // VM propietaria de este DistRuntime
    DistRuntimeConfig       config_;     // configuracion de arranque
    NodeRegistry            registry_;   // tabla de nodos conocidos

    // sesiones activas indexadas por node_idx
    mutable std::mutex                            sessions_mtx_;
    std::unordered_map<uint32_t, VdpSession *>    sessions_;  // node_idx -> sesion

    // tabla de futuros pendientes de ACK de rspawn
    mutable std::mutex                             futures_mtx_;
    std::unordered_map<uint64_t, PendingFuture>   pending_futures_; // future_id -> entrada
    std::atomic<uint64_t>                          next_future_id_{1};

    // tabla de mensajes pendientes de ACK (at-least-once)
    mutable std::mutex                             msgs_mtx_;
    std::unordered_map<uint32_t, PendingMsg>       pending_msgs_; // seq_num -> entrada

    // hilo de reintentos de mensajes (at-least-once)
    std::atomic<bool>  retry_running_{false};
    std::thread        retry_thread_;
    void               retry_loop_();

    // servidor VDP de escucha (acepta conexiones entrantes)
    TCPServer  *vdp_server_{nullptr};

    // manejador de mensajes VDP entrantes
    void on_vdp_msg_(const VdpHeader &hdr,
                     const std::vector<uint8_t> &payload,
                     uint32_t node_idx);

    // manejadores por tipo de mensaje
    void handle_rspawn_ack_(const VdpPayloadRspawnAck &ack);
    void handle_rspawn_(uint32_t node_idx,
                        const VdpPayloadRspawn &req,
                        const uint8_t *code, size_t code_size);
    void handle_msgsend_(uint32_t node_idx,
                         const VdpPayloadMsgsend &req,
                         const uint8_t *data, size_t data_size,
                         uint32_t seq_num);
    void handle_msgsend_ack_(const VdpPayloadMsgsendAck &ack);
    void handle_memsync_hash_(uint32_t node_idx,
                              const VdpPayloadMemsyncHash &req,
                              const VdpPageHash *hashes, size_t hash_count,
                              uint32_t seq_num);
    void handle_memsync_data_(uint32_t node_idx,
                              const VdpPayloadMemsyncData &req,
                              const uint8_t *data, size_t data_size,
                              uint32_t seq_num);
    void handle_future_fulfill_(uint32_t node_idx,
                                const VdpPayloadFutureFulfill &msg);
    void handle_future_reject_(uint32_t node_idx,
                               const VdpPayloadFutureReject &msg);

    // resuelve un future local con el valor dado y despierta al proceso esperador
    void resolve_future_(uint64_t future_id, uint64_t result, bool rejected);

    // conecta de forma asincrona a un nodo nuevo descubierto
    void on_new_node_(const NodeInfo &node);

    // genera un ID unico de nodo (hash de IP + puerto + timestamp)
    static uint64_t generate_node_id_();
};

} // namespace distrib

#endif // DIST_RUNTIME_H
