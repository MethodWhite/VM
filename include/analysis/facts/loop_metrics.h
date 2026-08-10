/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file loop_metrics.h
 * @brief Metricas NEUTRALES del cuerpo de un bucle (infraestructura reutilizable).
 *
 * SOLO MIDE; no decide ni pondera nada.  El "coste" depende del PASE que la
 * consume (para unroll una call pesa ~10; para vectorizar es prohibitiva; para
 * inline pesa 0; para software-pipelining lo caro son las ramas), asi que cada
 * optimizacion define SU PROPIA funcion de coste sobre estas metricas crudas.
 * Consumidores previstos: unroll, vectorizacion, peeling, unswitch, pipelining.
 *
 * Parte de la familia de hechos derivados del CFG (junto a @c LoopFacts).
 */
#ifndef ANALYSIS_FACTS_LOOP_METRICS_H
#define ANALYSIS_FACTS_LOOP_METRICS_H

#include "ir/ssa_ir.h"

#include <vector>

namespace analysis {

/**
 * @struct LoopMetrics
 * @brief Conteos CRUDOS del cuerpo de un bucle.  Sin ponderar (neutral).
 */
struct LoopMetrics {
    int instructions = 0; ///< instrucciones no-terminadoras, sin PHIs.
    int loads = 0;        ///< LOAD / GETFIELD / ARRAY_LOAD.
    int stores = 0;       ///< STORE / SETFIELD / ARRAY_STORE + VEC store-like.
    int calls = 0;        ///< CALL* (cualquier llamada).
    int branches = 0;     ///< BR_COND internos del cuerpo.
    int phis = 0;         ///< PHIs internos (merges de control interno).
    int vector_ops = 0;   ///< VEC_* (indica cuerpo YA vectorizado).
    int fp_ops = 0;       ///< aritmetica float (FADD/FSUB/FMUL/...).
    int expensive_ops = 0; ///< DIV/MOD/FDIV/FSQRT/... (alta latencia).
    int basic_blocks = 0; ///< bloques del cuerpo (CFG fragmentado -> explota al
                          ///< clonar; no es igual un cuerpo recto que uno con
                          ///< if/else/merge aunque tengan las mismas instr).
    int terminators = 0;  ///< terminadores del cuerpo (BR/BR_COND/RET/...).
    int live_across = 0;  ///< valores del cuerpo vivos en el back-edge (proxy de
                          ///< presion de registros).
    /// Resumen de si el cuerpo tiene algun efecto que impide reordenar/replicar
    /// con libertad (store, call, atomic, asm).  El compilador conoce SIEMPRE
    /// todos sus efectos via IR/SSA; esto solo los agrega en un flag para los
    /// pases.  NO se derivan campos redundantes (memory_ops=loads+stores y
    /// has_call=calls!=0 se calculan donde se usen: una sola fuente de verdad).
    bool has_side_effects = false;
};

/**
 * @brief Mide el cuerpo (bloques @p body) de un bucle de @p fn.  Neutral.
 * @param fn   funcion SSA.
 * @param body bloques del cuerpo (todos los del bucle salvo el header).
 *
 * @c live_across (proxy de presion) = valores definidos en el cuerpo y usados
 * FUERA de el (loop-carried via PHIs del header + live-out); no depende del
 * latch ni de temporales intra-iteracion.
 */
LoopMetrics compute_loop_metrics(const ir::IrFunction &fn,
                                 const std::vector<ir::IrBlockId> &body);

} // namespace analysis

#endif // ANALYSIS_FACTS_LOOP_METRICS_H
