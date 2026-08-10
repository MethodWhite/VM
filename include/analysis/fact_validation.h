/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/fact_validation.h
 * @brief Vocabulario de AUTOCERTIFICACION de los Facts.
 *
 * Cada productor de conocimiento puede validarse a SI MISMO antes de que el
 * resto del compilador lo use.  La validacion devuelve DATOS (@c FactIssue:
 * codigo + operandos), NUNCA mensajes -- el consumidor los mapea al catalogo
 * i18n si los muestra.  Son invariantes internos (mas cerca de un ICE que de un
 * diagnostico de usuario), pero se modelan como datos por coherencia.
 *
 * Arquitectura (cada Fact se autocertifica; el Snapshot agrega):
 *
 *      LoopFacts.validate()  ProfileFacts.validate()  Liveness check
 *              \                     |                    /
 *               \                    |                   /
 *                +---------> FunctionSnapshot.validate() <----- audit_requirements
 *                                    |
 *                              vector<FactIssue>   (vacio = conocimiento sano)
 *
 * Ejemplos de invariantes por Fact:
 *   LoopFacts   : un header esta in_loop; depth>0 <=> in_loop; ids en rango;
 *                 padre != si mismo.
 *   ProfileFacts: pesos y trip-counts no negativos.
 *   Liveness    : def <= end en todo intervalo.
 */

#ifndef VESTA_ANALYSIS_FACT_VALIDATION_H
#define VESTA_ANALYSIS_FACT_VALIDATION_H

#include <cstdint>
#include <vector>

namespace analysis {

/**
 * @enum FactCheck
 * @brief Codigo (DATO) de un invariante de Fact violado.
 */
enum class FactCheck : uint16_t {
    OK = 0,

    // --- LoopFacts ---
    LOOP_HEADER_NOT_IN_LOOP,   ///< un bloque header no esta marcado in_loop.
    LOOP_DEPTH_INLOOP_MISMATCH,///< loop_depth>0 XOR in_loop (deben coincidir).
    LOOP_ID_OUT_OF_RANGE,      ///< loop_id de un bloque fuera de [0,loop_count).
    LOOP_PARENT_OUT_OF_RANGE,  ///< parent_loop fuera de rango.
    LOOP_PARENT_SELF,          ///< un bucle es su propio padre.
    LOOP_HEADER_BLOCK_OOR,     ///< header_block de un bucle fuera de rango.

    // --- ProfileFacts ---
    PROFILE_NEGATIVE_WEIGHT,   ///< block_weight negativo.
    PROFILE_NEGATIVE_TRIP,     ///< trip_count negativo.

    // --- Liveness ---
    LIVE_DEF_AFTER_END,        ///< un intervalo con def > end.
};

/**
 * @struct FactIssue
 * @brief Una violacion de invariante: codigo + hasta dos operandos (ids).
 */
struct FactIssue {
    FactCheck check = FactCheck::OK;
    uint64_t  a     = 0; ///< primer id implicado (bloque/bucle/valor).
    uint64_t  b     = 0; ///< segundo (si aplica).
};

} // namespace analysis

#endif // VESTA_ANALYSIS_FACT_VALIDATION_H
