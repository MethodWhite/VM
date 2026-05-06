/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file regalloc.h
 * @brief Asignador de registros VM por barrido lineal (linear scan).
 *
 * Asigna los 16 registros VM (r0-r15) a los valores SSA de una funcion.
 * Los registros r14 y r15 estan reservados para el emisor (scratch y argc).
 * Los registros r0-r13 estan disponibles para asignacion general.
 *
 * Convencion de llamada pre-asignada:
 *   - param[0] -> r1, param[1] -> r2, ..., param[11] -> r12
 *   - r0: valor de retorno (se asigna antes del ret)
 *   - r13: disponible para el asignador
 *   - r14: scratch del emisor (NO asignado)
 *   - r15: argc para llamadas (NO asignado)
 *
 * Cuando el numero de valores vivos simultaneamente supera ALLOC_REGS,
 * el asignador derrama (spill) los valores menos prioritarios a slots
 * de pila. El emisor es responsable de emitir los push/pop de spill.
 */

#ifndef REGALLOC_H
#define REGALLOC_H

#include "ir/liveness.h"
#include <unordered_map>
#include <string>

namespace ir {

/** @brief Numero de registros VM disponibles para asignacion (r0-r12). */
static constexpr int ALLOC_REGS = 13;

/** @brief Registro scratch primario del emisor (no asignado por el allocator). */
static constexpr int SCRATCH_REG  = 14; // r14

/** @brief Registro scratch secundario para derrames de segundo operando. */
static constexpr int SCRATCH2_REG = 13; // r13 (reservado junto con r14)

/** @brief Registro reservado para argc en llamadas. */
static constexpr int ARGC_REG = 15;    // r15

/**
 * @brief Resultado de la asignacion de registros para una funcion.
 *
 * Cada valor SSA queda asignado a un registro VM (reg_map) o a un slot
 * de pila (spill_map).  El campo spill_count indica cuantos slots de pila
 * reservar en el prologo de la funcion (cada slot = 8 bytes).
 */
struct AllocResult {
    std::unordered_map<IrValueId, int>      reg_map;    ///< valor -> registro VM (0-13)
    std::unordered_map<IrValueId, uint32_t> spill_map;  ///< valor -> indice de slot pila
    uint32_t                                 spill_count; ///< numero de slots usados
    bool                                     ok;          ///< false si hay error irrecuperable
    std::string                              error;       ///< descripcion del error si !ok
};

/**
 * @brief Asigna registros VM a los valores SSA mediante barrido lineal.
 *
 * Implementa el algoritmo de Poletto & Sarkar (1999).
 * Pre-asigna parametros a r1-r12 siguiendo la convencion de llamada.
 * Derrama valores cuando se agotan los ALLOC_REGS registros disponibles.
 *
 * @param fn        Funcion SSA con sus valores y bloques.
 * @param liveness  Resultado del analisis de vivacidad.
 * @return Resultado con la asignacion de registros y slots de pila.
 */
AllocResult allocate_regs(const IrFunction &fn, const LivenessResult &liveness);

/**
 * @brief Obtiene el nombre de texto de un registro VM (p.ej. "r0", "r14").
 * @param reg Numero de registro (0-15).
 * @return Cadena del registro.
 */
const char *reg_name(int reg);

} // namespace ir

#endif // REGALLOC_H
