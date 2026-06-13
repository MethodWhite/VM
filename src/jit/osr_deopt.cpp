/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/osr_deopt.cpp
 * @brief Implementacion de la deoptimizacion OSR (vuelta al interprete).
 *
 * = Diseno =
 *
 * Cuando el codigo compilado JIT encuentra una condicion que le impide
 * continuar (guarda especulativa fallida, tipo inesperado, array index
 * fuera de rango, null check, etc.), debe devolver el control al
 * interprete de forma segura.
 *
 * El proceso inverso al OSR transfer:
 *
 *   1. El codigo compilado llama a @c osr_deopt_handler (via runtime
 *      entry) con el motivo y el PC bytecode de continuacion.
 *
 *   2. Se construye un OsrFrame a partir del estado actual:
 *      - Los registros VM se leen de proc->registers (que el codigo
 *        JIT mantiene actualizados).
 *      - El PC de continuacion es la instruccion bytecode donde el
 *        interprete debe reanudar.
 *
 *   3. Se restaura el estado del interprete:
 *      - registers.rip = continuation_pc
 *      - jit_active = 0
 *      - Los flags y valores de pila se reconstruyen.
 *
 *   4. El interprete reanuda la ejecucion en continuation_pc.
 *
 * = Casos de deopt =
 *
 *   - DEOPT_TYPE_MISS (1): el tipo de un objeto no coincide con el
 *     esperado por la compilacion especulativa.  Se pasa el ClassInfo*
 *     esperado en reason_data.
 *
 *   - DEOPT_BOUNDS (2): array index out of bounds.  reason_data
 *     contiene el indice que fallo.
 *
 *   - DEOPT_NULL_CHECK (3): se intento dereferenciar un puntero null.
 *     El interprete lanzara FATAL_NULL_POINTER.
 *
 *   - DEOPT_CLASS_CHANGE (4): la clase de un objeto cambio (e.g.
 *     por class redefinition).  reason_data contiene el ClassInfo*
 *     anterior.
 *
 *   - DEOPT_GC_REQUEST (5): el GC solicito que todos los frames JIT
 *     vuelvan al interprete para un stack walk preciso.
 *
 *   - DEOPT_BAILOUT (6): el codigo compilado no puede continuar por
 *     una condicion no manejada (e.g., llamada a funcion no soportada).
 */

#include "jit/osr_deopt.h"

#include <cstdio>
#include <cstring>

#include "jit/osr_data.h"
#include "jit/osr_transfer.h"
#include "runtime/proceso_runtime.h"
#include "vesta_rt/abi.h"
#include "vesta_rt/public.h"

namespace jit {

    // =====================================================================
    //  Construccion del frame de deopt
    // =====================================================================

    OsrFrame osr_build_deopt_frame(vrt_proc *proc,
                                   uint64_t deopt_pc,
                                   OsrFrame::DeoptReason reason,
                                   uint64_t reason_data) noexcept {
        OsrFrame frame;
        frame.continuation_pc = deopt_pc;
        frame.reason          = reason;
        frame.reason_data     = reason_data;

        auto *p = reinterpret_cast<runtime::ProcessVM *>(proc);
        if (!p) return frame;

        /* Capturar el estado actual de los registros VM.
         * El codigo JIT mantiene proc->registers actualizado, asi que
         * podemos leer los valores directamente. */
        for (int i = 0; i < VESTA_PROC_REGISTER_COUNT; ++i) {
            frame.regs[i] = p->registers.regs[i].qword();
        }
        frame.stack_pointer = p->registers.stack_pointer.qword();
        frame.base_pointer  = p->registers.base_pointer.qword();

        /* Si el proceso estaba en modo OSR (jit_active != 0), marcamos
         * el frame como OSR para que el interprete pueda re-OSR si
         * el bucle sigue caliente. */
        frame.is_osr_frame = (p->jit_active != 0);

        /* Capturar los slots de pila cercanos al tope para reconstruir
         * valores que el interprete pueda necesitar. */
        const uint32_t stack_capture = 16;
        frame.stack_slots.reserve(stack_capture);
        uint64_t sp = frame.stack_pointer;
        for (uint32_t i = 0; i < stack_capture; ++i) {
            try {
                frame.stack_slots.push_back(
                    p->vm_mem.read_u64(sp + i * 8));
            } catch (...) {
                frame.stack_slots.push_back(0);
            }
        }

        /* Si habia una excepcion pendiente, preservarla. */
        if (p->current_exception != 0) {
            frame.reason = OsrFrame::DEOPT_BAILOUT;
        }

        return frame;
    }

    // =====================================================================
    //  Aplicacion del deopt (vuelta al interprete)
    // =====================================================================

    void osr_deopt_to_interpreter(vrt_proc *proc,
                                  const OsrFrame &frame) noexcept {
        if (!proc) return;

        auto *p = reinterpret_cast<runtime::ProcessVM *>(proc);

        /* Restaurar los registros VM desde el frame. */
        for (int i = 0; i < VESTA_PROC_REGISTER_COUNT; ++i) {
            p->registers.regs[i].qword(frame.regs[i]);
        }
        p->registers.stack_pointer.qword(frame.stack_pointer);
        p->registers.base_pointer.qword(frame.base_pointer);

        /* Restaurar los slots de pila capturados. */
        uint64_t sp = frame.stack_pointer;
        for (uint32_t i = 0; i < frame.stack_slots.size(); ++i) {
            try {
                p->vm_mem.write_u64(sp + i * 8, frame.stack_slots[i]);
            } catch (...) {
                /* Si falla la escritura, ignorar (la pila puede estar
                 * en un estado inconsistente durante el deopt). */
            }
        }

        /* Setear el PC de continuacion en el interprete. */
        p->registers.rip.qword(frame.continuation_pc);

        /* Desmarcar el modo JIT: el interprete tomara el control. */
        p->jit_active = 0;

        /* Si el motivo es null check, el interprete lanzara la
         * excepcion correspondiente al intentar ejecutar la siguiente
         * instruccion.  No hacemos nada especial aqui. */

        /* Log de deopt en debug. */
        #ifndef NDEBUG
        static const char *kReasonNames[] = {
            "UNKNOWN", "TYPE_MISS", "BOUNDS", "NULL_CHECK",
            "CLASS_CHANGE", "GC_REQUEST", "BAILOUT"
        };
        const char *rname = "UNKNOWN";
        if (frame.reason >= 0 && frame.reason <= 6) {
            rname = kReasonNames[frame.reason];
        }
        std::fprintf(stderr,
            "[osr] deopt: reason=%s, continuation_pc=0x%lx, "
            "is_osr_frame=%d\n",
            rname,
            (unsigned long)frame.continuation_pc,
            frame.is_osr_frame ? 1 : 0);
        #endif
    }

    // =====================================================================
    //  Handler invocado desde codigo JIT
    // =====================================================================

    void osr_deopt_handler(vrt_proc *proc,
                           uint32_t deopt_reason,
                           uint64_t deopt_pc,
                           uint64_t reason_data) noexcept {
        if (!proc) return;

        /* Convertir el codigo de motivo al enum. */
        OsrFrame::DeoptReason reason = OsrFrame::DEOPT_UNKNOWN;
        switch (deopt_reason) {
            case 1: reason = OsrFrame::DEOPT_TYPE_MISS;    break;
            case 2: reason = OsrFrame::DEOPT_BOUNDS;       break;
            case 3: reason = OsrFrame::DEOPT_NULL_CHECK;   break;
            case 4: reason = OsrFrame::DEOPT_CLASS_CHANGE; break;
            case 5: reason = OsrFrame::DEOPT_GC_REQUEST;   break;
            case 6: reason = OsrFrame::DEOPT_BAILOUT;      break;
            default: reason = OsrFrame::DEOPT_UNKNOWN;     break;
        }

        /* Construir el frame de deopt. */
        OsrFrame frame = osr_build_deopt_frame(proc, deopt_pc,
                                                reason, reason_data);

        /* Aplicar la deoptimizacion. */
        osr_deopt_to_interpreter(proc, frame);
    }

    // =====================================================================
    //  Runtime entry para deopt (invocable desde codigo JIT)
    // =====================================================================

    /**
     * @brief Version C-linkage para ser usada como runtime entry.
     *
     * El codigo JIT llama a esta funcion cuando encuentra una condicion
     * que requiere deopt.  Los argumentos siguen la convencion C:
     *
     *   rdi/rcx = vrt_proc*
     *   rsi/rdx = deopt_reason
     *   rdx/r8  = deopt_pc
     *   rcx/r9  = reason_data
     */
    extern "C" void vrt_osr_deopt(vrt_proc *proc,
                                  uint64_t deopt_reason,
                                  uint64_t deopt_pc,
                                  uint64_t reason_data) {
        osr_deopt_handler(proc,
                          static_cast<uint32_t>(deopt_reason),
                          deopt_pc,
                          reason_data);
    }

} // namespace jit
