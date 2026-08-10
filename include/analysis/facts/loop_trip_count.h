/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file loop_trip_count.h
 * @brief Hecho reutilizable: numero de iteraciones (trip count) de un bucle
 *        contado, a partir del descriptor de su variable de induccion.
 *
 * Es INFRAESTRUCTURA DE ANALISIS, no de transformacion: lo consumen el
 * desenrollado, la vectorizacion, el peeling y el unswitch por igual.  El
 * transformador no calcula el trip; solo pide este hecho.
 */
#ifndef ANALYSIS_FACTS_LOOP_TRIP_COUNT_H
#define ANALYSIS_FACTS_LOOP_TRIP_COUNT_H

#include "analysis/facts/loop_iv.h" // LoopIV (descriptor del IV)
#include "ir/ssa_ir.h"

#include <cstdint>
#include <vector>

namespace analysis {

/// Numero de iteraciones del header si es CONSTANTE conocido.  Un solo campo:
/// el estado no puede contradecirse (nunca "known pero trip=-1").
struct LoopTripInfo {
    int64_t trip = -1; ///< iteraciones del header (>= 0); < 0 = desconocido.
    bool known() const { return trip >= 0; }
};

/**
 * @brief Calcula el trip count constante de un bucle a partir de su IV.
 *
 * Resuelve @c iv.init y @c iv.bound a constantes (via las CONST que los definen
 * en @p def_block).  Si ambas lo son y el stride es positivo:
 *   - `<`  (CMP_LT/ULT):  trip = ceil((bound - offset - init) / stride)
 *   - `<=` (CMP_LE/ULE):  trip = floor((bound - offset - init) / stride) + 1
 * Cualquier duda (no constante, decreciente, cuerpo vacio) -> @c known=false.
 *
 * @param fn        funcion SSA.
 * @param def_block def_block[v] = bloque que define v (-1 si ninguno).
 * @param iv        descriptor de la variable de induccion.
 */
LoopTripInfo compute_trip_count(const ir::IrFunction &fn,
                                const std::vector<int> &def_block,
                                const LoopIV &iv);

} // namespace analysis

#endif // ANALYSIS_FACTS_LOOP_TRIP_COUNT_H
