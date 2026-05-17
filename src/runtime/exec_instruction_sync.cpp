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
 * @file exec_instruction_sync.cpp
 * @brief Implementacion de las instrucciones de sincronizacion de VestaVM.
 *
 * Implementa monitores reentrantes (como synchronized de Java) sobre objetos GC:
 *
 *  MONENTER (0x00 0x35): adquiere el monitor del objeto GC.
 *                         Si esta libre o ya lo posee el proceso: lock_depth++.
 *                         Si lo posee otro: el proceso se bloquea y re-ejecuta MONENTER
 *                         cuando sea despertado por MONEXIT.
 *
 *  MONEXIT  (0x00 0x36): libera el monitor.  Si lock_depth llega a 0, despierta al
 *                         primer proceso de la cola de espera.
 *
 *  MONWAIT  (0x00 0x37): libera completamente el monitor (lock_depth -> 0) y suspende
 *                         el proceso en la cola de espera del objeto.  El proceso debe
 *                         llamar MONENTER de nuevo tras ser despertado.
 *
 *  MONNOTI  (0x00 0x38): despierta exactamente un proceso de la cola de espera.
 *
 *  MONNOTA  (0x00 0x39): despierta todos los procesos de la cola de espera.
 *
 * Estado del monitor en ObjectHeader (24 bytes):
 *   owner_pid  (uint32_t) -- local_pid del proceso propietario; 0 = libre.
 *   lock_depth (uint16_t) -- contador de bloqueos reentrantes.
 *
 * PID codificado: (scheduler_id << 32) | local_pid  (uint64_t).
 */

#include "runtime/exec_instruction.h"
#include "runtime/proceso_runtime.h"
#include "runtime/scheduler.h"
#include "runtime/runtime.h"
#include "gc/gc_heap.h"
#include "loader/oop_types.h"
#include "debug/debugger.h"  // Debugger::on_monitor_contention

namespace runtime {

    // -------------------------------------------------------------------------
    // Utilitario: codificar/decodificar PID del proceso actual
    // -------------------------------------------------------------------------

    /** @brief Codifica el PID del proceso como uint64_t para almacenarlo en colas. */
    static inline uint64_t encode_pid(const ProcessVM *vm) {
        return (static_cast<uint64_t>(vm->pid.scheduler_id) << 32)
             | static_cast<uint64_t>(vm->pid.local_pid);
    }

    /** @brief Decodifica un PID codificado y llama make_ready en el scheduler padre. */
    static inline void wake_pid(ProcessVM *vm, uint64_t encoded_pid) {
        if (encoded_pid == 0) return;
        GlobalPID target;
        target.scheduler_id = static_cast<uint32_t>(encoded_pid >> 32);
        target.local_pid    = static_cast<uint64_t>(encoded_pid & 0xFFFFFFFFu);
        vm->scheduler.vm_reference.make_ready(target);
    }

    // -------------------------------------------------------------------------
    // MONENTER (0x35)
    // -------------------------------------------------------------------------

    /**
     * @brief Ejecuta MONENTER: adquiere el monitor del objeto GC.
     *
     * Llama a gc_heap.monitor_try_acquire(handle, local_pid):
     *   - Retorna true  -> monitor adquirido; continua normalmente.
     *   - Retorna false -> monitor ocupado; el proceso se registra en la cola de
     *     espera y se bloquea (blocking=true) sin avanzar el PC.
     *     Cuando MONEXIT lo despierte, MONENTER se re-ejecuta.
     *
     * @param vm    Proceso virtual que ejecuta MONENTER.
     * @param instr reg1 = GcHandle del objeto cuyo monitor se quiere adquirir.
     */
    void exec_instr_monenter(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_handle = instr.data_instruction.reg_data.reg1;
        const auto h = static_cast<gc::GcHandle>(vm->registers.regs[r_handle].qword());

        if (vm->gc_heap.monitor_try_acquire(h, vm->pid.local_pid)) {
            return; // monitor adquirido correctamente
        }

        // monitor ocupado: registrar en la cola de espera y bloquear
        vm->gc_heap.monitor_add_waiter(h, encode_pid(vm));
        // Hook del debugger: notificar contention al monitor.  Sin overhead
        // si break_on_mon_=false (atomic load + branch).
        if (vm->scheduler.vm_reference.debugger) {
            // Leer owner_pid del ObjectHeader del objeto referenciado (offset
            // 16 dentro del header v2: class_ptr@0, flags@8, hash@12, owner@16).
            uint64_t owner_pid = 0;
            uint8_t *payload = vm->gc_heap.deref(h);
            if (payload) {
                uint8_t *header = payload - 24; // ObjectHeader v2 size = 24
                owner_pid = *reinterpret_cast<uint32_t *>(header + 16);
            }
            vm->scheduler.vm_reference.debugger->on_monitor_contention(
                vm->pid.local_pid, static_cast<uint64_t>(h), owner_pid);
        }
        vm->decoded_ptr->flags_info.blocking = true; // bloquear sin avanzar PC
        vm->scheduler.on_event(EVT_IO_WAIT);         // transicion al estado WAIT_IO
    }

    // -------------------------------------------------------------------------
    // MONEXIT (0x36)
    // -------------------------------------------------------------------------

    /**
     * @brief Ejecuta MONEXIT: libera el monitor del objeto GC.
     *
     * Decrementa lock_depth.  Si llega a 0, despierta al primer proceso de la
     * cola de espera (si la hay) llamando make_ready().
     *
     * @param vm    Proceso virtual que ejecuta MONEXIT.
     * @param instr reg1 = GcHandle del objeto cuyo monitor se libera.
     */
    void exec_instr_monexit(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_handle = instr.data_instruction.reg_data.reg1;
        const auto h = static_cast<gc::GcHandle>(vm->registers.regs[r_handle].qword());

        const uint64_t next = vm->gc_heap.monitor_release(h, vm->pid.local_pid);
        wake_pid(vm, next); // despertar al siguiente proceso en cola (0 = no hay ninguno)
    }

    // -------------------------------------------------------------------------
    // MONWAIT (0x37)
    // -------------------------------------------------------------------------

    /**
     * @brief Ejecuta MONWAIT: libera completamente el monitor y suspende el proceso.
     *
     * Libera el monitor independientemente de lock_depth (vaciado completo).
     * El proceso queda en la cola de espera del monitor.  Cuando MONNOTI o
     * MONNOTA lo despierten, el proceso debe llamar MONENTER de nuevo para
     * readquirir el lock antes de operar sobre el objeto protegido.
     *
     * @param vm    Proceso virtual que ejecuta MONWAIT.
     * @param instr reg1 = GcHandle del objeto.
     */
    void exec_instr_monwait(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_handle = instr.data_instruction.reg_data.reg1;
        const auto h = static_cast<gc::GcHandle>(vm->registers.regs[r_handle].qword());

        // limpiar el monitor directamente (forzar lock_depth = 0)
        uint8_t *ptr = vm->gc_heap.deref(h);
        if (ptr != nullptr) {
            auto *hdr = reinterpret_cast<loader::ObjectHeader *>(ptr);
            if (hdr->owner_pid == vm->pid.local_pid) {
                // despertar al proximo en la cola de MONENTER antes de ceder el lock
                uint64_t next = vm->gc_heap.monitor_pop_waiter(h);
                hdr->owner_pid  = 0; // liberar el monitor completamente
                hdr->lock_depth = 0;
                wake_pid(vm, next); // si alguien esperaba en MONENTER, despertarlo
            }
        }

        // registrar este proceso en la cola de espera del monitor (para MONNOTI/MONNOTA)
        vm->gc_heap.monitor_add_waiter(h, encode_pid(vm));
        vm->decoded_ptr->flags_info.blocking = true; // suspender el proceso
        vm->scheduler.on_event(EVT_IO_WAIT);
    }

    // -------------------------------------------------------------------------
    // MONNOTI (0x38)
    // -------------------------------------------------------------------------

    /**
     * @brief Ejecuta MONNOTI: despierta un proceso de la cola de espera del monitor.
     *
     * Extrae el primer PID de la cola de espera del objeto y llama make_ready.
     * No transfiere la propiedad del monitor; el proceso despertado debe llamar
     * MONENTER para adquirirlo.
     *
     * @param vm    Proceso virtual que ejecuta MONNOTI.
     * @param instr reg1 = GcHandle del objeto.
     */
    void exec_instr_monnoti(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_handle = instr.data_instruction.reg_data.reg1;
        const auto h = static_cast<gc::GcHandle>(vm->registers.regs[r_handle].qword());

        const uint64_t next = vm->gc_heap.monitor_pop_waiter(h);
        wake_pid(vm, next); // despertar al primero en la cola (0 = nadie esperando)
    }

    // -------------------------------------------------------------------------
    // MONNOTA (0x39)
    // -------------------------------------------------------------------------

    /**
     * @brief Ejecuta MONNOTA: despierta todos los procesos de la cola de espera.
     *
     * Extrae todos los PIDs de la cola de espera del objeto y llama make_ready
     * en cada uno.  La cola queda vacia tras la llamada.
     *
     * @param vm    Proceso virtual que ejecuta MONNOTA.
     * @param instr reg1 = GcHandle del objeto.
     */
    void exec_instr_monnota(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_handle = instr.data_instruction.reg_data.reg1;
        const auto h = static_cast<gc::GcHandle>(vm->registers.regs[r_handle].qword());

        std::vector<uint64_t> waiters = vm->gc_heap.monitor_pop_all_waiters(h);
        for (uint64_t pid : waiters) {
            wake_pid(vm, pid); // despertar a cada proceso en la cola
        }
    }

} // namespace runtime
