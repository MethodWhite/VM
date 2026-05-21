/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file proceso_runtime.cpp
 * @brief Implementacion del contexto de proceso virtual (ProcessVM) de VestaVM.
 *
 * Implementa @c ProcessVM: inicializacion de registros y pila, gestion del
 * estado del proceso (NEW, READY, RUNNING, BLOCKED, DEAD) y utilidades de
 * inspeccion del proceso en ejecucion.
 */#include "runtime/proceso_runtime.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <queue>
#include <thread>
#include <vector>

#include "arena/arena.h"
#include "arena/VirtualMemory.h"
#include "cli/sync_io.h"
#include "jit/jit_call.h"
#include "profiler/timer.h"
#include "runtime/rflags.h"

namespace loader {
    class Loader; ///< Declaracion adelantada del cargador de bytecode
}

namespace runtime {
    struct InstrFormat; ///< Declaracion adelantada del formato de instruccion
    class ManageVM;     ///< Declaracion adelantada del gestor de instancias
    class VM;           ///< Declaracion adelantada de la instancia VM

    /**
     * @brief hook function pointer para auto-JIT trigger.
     *        Default nullptr (sin JIT).  El main binario lo setea a
     *        @c &jit::maybe_compile_method en VM init si quiere habilitar
     *        JIT.  Ver @c include/runtime/proceso_runtime.h para detalle.
     */
    void (*g_callvirt_post_hook)(ProcessVM *vm, loader::MethodInfo *method) = nullptr;

    /**
     * @brief Puntero a la llamada nativa en curso (si existe alguna).
     *
     * Se usa para coordinar llamadas FFI desde instrucciones que necesitan
     * suspender el proceso hasta recibir el resultado de la funcion nativa.
     * El valor nullptr indica que no hay ninguna llamada pendiente.
     */
    PendingCall_t *pending_call = nullptr;

    /**
     * @brief Construye un proceso virtual y lo conecta al scheduler y al TLB.
     *
     * Inicializa vm_mem con el TLB y el ArenaManager privados del proceso.
     * El estado inicial es NEW hasta que se llame a make_ready().
     *
     * @param scheduler Scheduler propietario que ejecutara este proceso.
     * @param pid       Identificador global unico del proceso.
     */
    ProcessVM::ProcessVM(Scheduler &scheduler, GlobalPID pid)
        : pid(pid),
          vm_mem(tlb, manager_mem_priv), // combinar TLB privado con ArenaManager privado
          scheduler(scheduler) {
        // fix8 - el GC necesita conocer el ProcessVM owner para
        // escanear stack/regs durante major_gc.  Set una sola vez aqui;
        // el puntero permanece valido durante toda la vida del proceso.
        gc_heap.set_owner_process(this);
    }

    /**
     * @brief Destructor: marca el proceso como DEAD y libera su memoria.
     *
     * Libera todas las arenas del ArenaManager privado.  En teoria el
     * destructor de ArenaManager ya lo hace, pero la llamada explicita
     * garantiza la liberacion antes de que el objeto sea destruido.
     */
    ProcessVM::~ProcessVM() {
        state = DEAD;                // marcar estado terminal
        manager_mem_priv.free_all(); // liberar toda la memoria privada del proceso
        // Liberar @c ExceptionFrames del free list (reciclados por
        // @c tryleave).  El @c exc_frame_stack activo solo deberia tener
        // frames si el proceso muere mid-try (no normal, pero defensivo).
        while (exc_frame_stack != nullptr) {
            ExceptionFrame *tmp = exc_frame_stack;
            exc_frame_stack = tmp->prev;
            delete tmp;
        }
        while (exc_free_list != nullptr) {
            ExceptionFrame *tmp = exc_free_list;
            exc_free_list = tmp->prev;
            delete tmp;
        }
    }

    /**
     * @brief Invalida todas las entradas de la cache de instrucciones descodificadas.
     *
     * Pone el campo pc de cada entrada a UINT64_MAX (valor invalido) y
     * anula decoded_ptr para que el descodificador no reutilice instrucciones
     * de una ejecucion anterior o de codigo que haya cambiado.
     */
    void ProcessVM::reset_cache() {
        for (auto &entry: icache) {
            entry.pc    = UINT64_MAX; // marcar como entrada invalida
            decoded_ptr = nullptr;    // invalidar el puntero de descodificacion activo
        }
    }

    /**
     * @brief Carga codigo en bruto en la memoria virtual del proceso.
     *
     * Copia @p code a partir de la direccion virtual @p address usando
     * vm_to_host_memcpy() y configura el registro RIP para apuntar al inicio
     * del codigo cargado.
     *
     * @note No cambia el estado del proceso: debe estar en NEW cuando se llama.
     *       El scheduler cambiara el estado a READY cuando se llame a make_ready().
     *
     * @param address Direccion virtual de inicio donde se escribira el codigo.
     * @param code    Vector de bytes con las instrucciones a cargar.
     */
    void ProcessVM::load_raw_code(uint64_t address, const std::vector<uint8_t> &code) {
        vm_mem.vm_to_host_memcpy(address, code.data(), code.size()); // copiar codigo a memoria virtual

        registers.rip.qword(address); // apuntar el PC al inicio del codigo cargado

        // El estado permanece en NEW hasta que make_ready() lo ponga en READY.
        // make_ready() requiere que el proceso este en NEW para incrementar alive_count.
    }

    /**
     * @brief Genera una representacion textual detallada del proceso.
     * @return Cadena con el volcado completo del estado del proceso.
     */
    std::string ProcessVM::to_string() const {
        return vm_summary(); // delegar en vm_summary
    }

    /**
     * @brief Genera un resumen de diagnostico del proceso en texto.
     *
     * Incluye PID, registros generales R00-R15, registros ZMM f0..f15 con sus
     * bits IEEE 754 y valor double, registros cursor CUR0-CUR3, RIP/RSP/RBP,
     * banderas, reducciones, TSC, estado del icache y bytes de memoria asignados.
     * Util para depuracion e informes de crash.
     *
     * @return Cadena con el resumen formateado del proceso.
     */
    std::string ProcessVM::vm_summary() const {
        std::ostringstream ss;

        // encabezado: PID, ID del scheduler y estado actual
        ss << "PID LOCAL=" << vesta::hex64((uint64_t) pid.local_pid)
           << " ID SCHEDULER=" << vesta::hex64((uint64_t) pid.scheduler_id)
           << " st=" << vm_state_to_str(state) << "\n";

        // registros de proposito general R00-R15 (dos por linea)
        for (int i = 0; i < 16; ++i) {
            ss << " R" << std::setw(2) << std::setfill('0') << i
               << "=" << vesta::hex64(registers.regs[i].qword());
            if (i % 2 == 1) ss << "\n"; // salto de linea cada dos registros
        }
        ss << "\n";

        // registros ZMM f0..f15: bits crudos y valor double (dos por linea)
        for (int i = 0; i < 16; ++i) {
            double  dv   = registers.zmm[i].read_f64();   // valor como double IEEE 754
            uint64_t raw = 0;
            __builtin_memcpy(&raw, registers.zmm[i].data, 8); // bits crudos
            ss << " F" << std::setw(2) << std::setfill('0') << i
               << "=" << vesta::hex64(raw)
               << " (" << std::setfill(' ') << dv << ")";
            if (i % 2 == 1) ss << "\n";
        }
        ss << "\n";

        // registros cursor CUR0-CUR3 (dos por linea)
        for (int i = 0; i < 4; ++i) {
            ss << " CUR" << i << "=" << vesta::hex64(registers.cur[i].qword());
            if (i % 2 == 1) ss << "\n";
        }
        ss << "\n";

        // contadores de programa y pila
        ss << " RIP=" << vesta::component_to_string(registers.rip)
           << " RSP=" << vesta::component_to_string(registers.stack_pointer)
           << " RBP=" << vesta::component_to_string(registers.base_pointer)
           << "\n";

        // registro de banderas
        ss << " FLAGS=["
           << "CF=" << (int) registers.flags.bits.CF << " "
           << "OF=" << (int) registers.flags.bits.OF << " "
           << "SF=" << (int) registers.flags.bits.SF << " "
           << "ZF=" << (int) registers.flags.bits.ZF << " "
           << "DM=" << (int) registers.flags.bits.DM
           << "]\n";

        // contadores de ejecucion
        ss << " Reductions=" << reductions_remaining
           << " TSC=" << tsc
           << " SleepUntil=" << time_sleep
           << " LastErr=" << err_thread
           << "\n";

        // instruccion activa si decoded_ptr es valido
        if (decoded_ptr) {
            ss << " Instr: opcode=" << (int) decoded_ptr->flags_info.opcode_index
               << " size="     << (int) decoded_ptr->flags_info.size_instr
               << " mode="     << (int) decoded_ptr->flags_info.mode
               << " did_jump=" << decoded_ptr->flags_info.did_jump
               << " blocking=" << decoded_ptr->flags_info.blocking
               << " pc="       << vesta::hex64(decoded_ptr->pc)
               << "\n";
        }

        // estadisticas del icache
        size_t valid = 0;
        for (const auto &entry: icache)
            if (entry.metadata != nullptr) valid++; // contar entradas con metadatos validos

        ss << " ICACHE: valid=" << valid << "/" << ICACHE_SIZE
           << " (" << (100 * valid / ICACHE_SIZE) << "%)\n";

        // memoria privada del proceso
        ss << " MEM: total_allocated_bytes=" << manager_mem_priv.total_allocated_bytes_ << "\n";

        // estado del scheduler propietario
        ss << " SchedulerID=" << scheduler.id_scheduler
           << " Ready=" << scheduler.ready_queue.size()
           << "\n";

        ss << " Sleep=" << time_sleep << "\n";

        return ss.str();
    }

} // namespace runtime
