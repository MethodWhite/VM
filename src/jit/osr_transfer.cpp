/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/osr_transfer.cpp
 * @brief Implementacion de la transferencia interprete -> JIT (OSR).
 *
 * = Diseno =
 *
 * Cuando el monitor OSR decide que un bucle debe ser reemplazado por
 * su version compilada JIT, este modulo:
 *
 *   1. Toma el OsrState (registros VM, pila, PC) capturado en el
 *      loop header.
 *
 *   2. Escribe los valores en el osr_buffer del ProcessVM, un array
 *      de uint64_t indexado por IR VID que el codigo compilado
 *      OSR-entry lee al arrancar.
 *
 *   3. Setea el flag jit_active y el rip para que el interprete
 *      salte al entry point OSR del codigo nativo.
 *
 *   4. Si la transferencia falla (buffer lleno, estado incompatible),
 *      retorna 0 y el interprete continua la ejecucion normal.
 *
 * El codigo compilado OSR-entry tiene un prologue especial que:
 *   - Lee proc->osr_buffer[vid] para cada valor vivo.
 *   - Los mueve a sus registros nativos / spill slots.
 *   - Salta al loop header dentro del codigo compilado.
 *
 * = Seguridad =
 *
 * La escritura del buffer se hace con memcpy directo (el buffer esta
 * pre-asignado en el ProcessVM).  No hay riesgo de desbordamiento:
 * el tamano del buffer es VESTA_OSR_BUFFER_N celdas y se verifica
 * que los indices esten en rango.
 */

#include "jit/osr_transfer.h"

#include <cstdio>
#include <cstring>

#include "jit/osr_data.h"
#include "runtime/proceso_runtime.h"
#include "vesta_rt/abi.h"
#include "vesta_rt/public.h"

namespace jit {

    // =====================================================================
    //  Validacion
    // =====================================================================

    bool osr_validate_transfer(const OsrState &state,
                               const OsrEntry &entry) noexcept {
        /* El loop ID debe coincidir. */
        if (state.loop_id != entry.loop_id) {
            return false;
        }

        /* La entrada debe estar marcada como valida. */
        if (!entry.valid) {
            return false;
        }

        /* La direccion compilada debe ser no-cero. */
        if (entry.compiled_address == 0) {
            return false;
        }

        /* Verificar que los vids que el entry espera esten dentro
         * del rango del buffer. */
        for (uint32_t i = 0; i < entry.live_count; ++i) {
            const int16_t slot = entry.vid_to_buffer_slot[i];
            if (slot < 0 || slot >= static_cast<int16_t>(VESTA_OSR_BUFFER_N)) {
                return false;
            }
        }

        return true;
    }

    // =====================================================================
    //  Escritura del buffer
    // =====================================================================

    void osr_write_buffer(vrt_proc *proc,
                          const OsrState &state,
                          const OsrEntry &entry) noexcept {
        if (!proc) return;

        auto *p = reinterpret_cast<runtime::ProcessVM *>(proc);

        /* Obtener el puntero al osr_buffer del proceso.
         * Si es nullptr (OSR no inicializado), no hacer nada. */
        uint64_t *buffer = p->osr_buffer;
        if (!buffer) return;

        /* Escribir en el buffer los valores de los registros VM
         * segun el mapeo vid->buffer_slot de la entrada OSR. */
        for (uint32_t i = 0; i < entry.live_count && i < OSR_MAX_LIVE_REGS; ++i) {
            const int16_t slot = entry.vid_to_buffer_slot[i];
            if (slot < 0 || slot >= static_cast<int16_t>(VESTA_OSR_BUFFER_N)) {
                continue;
            }
            /* El slot i en el entry corresponde al registro VM i-esimo
             * (orden definido por el compilador).  El estado capturado
             * tiene los 16 registros en state.regs. */
            if (i < VESTA_PROC_REGISTER_COUNT) {
                buffer[slot] = state.regs[i];
            }
        }
    }

    // =====================================================================
    //  Transferencia completa
    // =====================================================================

    uint64_t osr_transfer_to_compiled(vrt_proc *proc,
                                      const OsrState &state,
                                      const OsrEntry &entry) noexcept {
        if (!proc) return 0;

        /* Validar que la transferencia sea posible. */
        if (!osr_validate_transfer(state, entry)) {
            return 0;
        }

        auto *p = reinterpret_cast<runtime::ProcessVM *>(proc);

        /* Escribir los valores en el buffer OSR. */
        osr_write_buffer(proc, state, entry);

        /* Marcar el proceso como en modo JIT. */
        p->jit_active = 1;

        /* Nota: en la implementacion completa, el interprete debe:
         *   1. Salvar su estado actual (para posible deopt).
         *   2. Setear proc->registers.rip al PC del bytecode donde
         *      continuar si el codigo JIT devuelve control.
         *   3. Saltar a entry.compiled_address.
         *
         * El salto real se hace desde el interprete (normalmente en
         * el dispatch de LOOP/JUMP).  Esta funcion retorna la direccion
         * y el interprete la usa como target de salto.
         *
         * En la implementacion actual, el salto se efectua via el
         * mecanismo existente de enter_jit (interp_jit_bridge.h).
         */

        /* Si la entrada tiene un offset valido, combinamos con la
         * direccion base.  El offset tipicamente es 0 (la entrada
         * OSR es un punto de entrada separado al inicio del codigo). */
        uint64_t target = entry.compiled_address;
        if (entry.byte_offset != 0) {
            target += entry.byte_offset;
        }

        /* En una implementacion completa, aqui se llamaria a
         * enter_jit(reinterpret_cast<JitFn>(target), proc).
         * Por ahora retornamos la direccion para que el interprete
         * haga el salto. */
        return target;
    }

} // namespace jit
