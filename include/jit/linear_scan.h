/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/linear_scan.h
 * @brief Register allocator linear-scan para el JIT (Phase D.7, commit 3).
 *        Ver doc/REGALLOC.md.
 *
 * Asigna a cada registro virtual un registro fisico O un spill slot de stack,
 * a partir de los live intervals (interval.h) y el descriptor del target
 * (target_reginfo.h).  Generico: NO usa literales @c MReg::*; lee los pools
 * de registros del @c TargetRegInfo.
 *
 * = Algoritmo (v1) =
 *
 * Linear-scan estilo Poletto-Sarkar, asignacion all-or-nothing por intervalo
 * (sin splitting a mitad de intervalo todavia; se anyade en una fase
 * posterior persiguiendo el ultimo %).  Por CLASE de registro (GP/FP) de
 * forma independiente:
 *
 *   1. Ordenar intervalos por inicio.
 *   2. Mantener una lista @c active (intervalos con reg asignado, ordenada
 *      por fin).  Al avanzar, EXPIRAR los que ya terminaron (liberan su reg).
 *   3. Para cada intervalo: si cruza un CALL, solo puede usar registros
 *      callee-saved (los caller-saved se destruyen en la llamada) -> esto
 *      empuja los valores live-across-call a R12-R15 o a spill, sin necesidad
 *      de fixed intervals explicitos.
 *   4. Si no hay registro usable libre: SPILL.  Victima = el intervalo (entre
 *      el actual y los activos robables) con el FIN mas lejano (Poletto).  El
 *      perdedor va a un spill slot para toda su vida.
 *
 * = Salida =
 *
 * Por vreg: un fisico (@c REG) o un spill slot (@c SPILL), o @c NONE si el
 * vreg esta muerto.  Mas la lista de callee-saved realmente usados (push/pop
 * en prologue/epilogue) y el numero de spill slots (tamano extra del frame).
 * El rewrite (commit 4) consume esto para reescribir los operandos VREG.
 */

#ifndef VESTA_JIT_LINEAR_SCAN_H
#define VESTA_JIT_LINEAR_SCAN_H

#include "jit/interval.h"
#include "jit/target_reginfo.h"

#include <cstdint>
#include <vector>

namespace jit {

    /**
     * @struct RegAlloc
     * @brief Resultado de la asignacion de registros.
     */
    struct RegAlloc {
        /// Ubicacion asignada a un vreg.
        enum class Loc : uint8_t {
            NONE  = 0,  ///< vreg muerto (interval vacio): sin asignacion
            REG   = 1,  ///< en un registro fisico (@c reg)
            SPILL = 2   ///< en un spill slot de stack (@c slot)
        };
        struct VAssign {
            Loc      loc  = Loc::NONE;
            uint8_t  reg  = 0;   ///< id fisico (valido si loc==REG)
            uint32_t slot = 0;   ///< indice de spill slot (valido si loc==SPILL)
        };
        /// Asignacion por vreg id (denso 0..vreg_count-1).
        std::vector<VAssign> assign;
        /// Registros callee-saved usados (deduplicados, orden estable).  El
        /// prologue/epilogue debe push/pop estos.
        std::vector<uint8_t> callee_saved_used;
        /// Numero de spill slots reservados (cada uno @c pointer_size bytes).
        uint32_t num_spill_slots = 0;

        /* ---- Accesores ---- */
        bool in_reg(uint32_t vid) const noexcept {
            return vid < assign.size() && assign[vid].loc == Loc::REG;
        }
        bool spilled(uint32_t vid) const noexcept {
            return vid < assign.size() && assign[vid].loc == Loc::SPILL;
        }
        uint8_t reg_of(uint32_t vid) const noexcept { return assign[vid].reg; }
        uint32_t slot_of(uint32_t vid) const noexcept { return assign[vid].slot; }
    };

    /**
     * @brief Ejecuta el linear-scan sobre los intervals de una funcion.
     *
     * @param ivs  Live intervals + posiciones de CALL (de @c build_intervals).
     * @param tri  Descriptor del target (pools de registros por clase).
     * @return     Asignacion vreg -> reg/slot.
     */
    RegAlloc linear_scan(const IntervalResult &ivs, const TargetRegInfo &tri);

} // namespace jit

#endif // VESTA_JIT_LINEAR_SCAN_H
