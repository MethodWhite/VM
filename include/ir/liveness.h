/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file liveness.h
 * @brief Analisis de vivacidad (liveness) sobre la SSA IR de VestaVM.
 *
 * Calcula intervalos de vida para cada valor SSA de una funcion, necesarios
 * para el asignador de registros de barrido lineal (linear scan).
 *
 * Algoritmo:
 *   1. Linealizar las instrucciones de todos los bloques en orden de emision.
 *   2. Para cada valor, registrar su posicion de definicion (def).
 *   3. Para cada uso de un valor, extender su intervalo hasta esa posicion.
 *   4. Los argumentos phi extienden el intervalo hasta el final del bloque
 *      predecesor correspondiente.
 */

#ifndef LIVENESS_H
#define LIVENESS_H

#include "ir/ssa_ir.h"
#include <vector>
#include <cstdint>

namespace ir {

/**
 * @brief Intervalo de vida de un valor SSA.
 *
 * Representa el rango continuo [def, end] (en indices de instruccion
 * linealizados) durante el cual el valor debe estar en un registro.
 */
struct LiveInterval {
    IrValueId id;   ///< identificador del valor SSA
    uint32_t  def;  ///< posicion de definicion (indice de instruccion)
    uint32_t  end;  ///< ultima posicion de uso (inclusive)

    /** @brief Compara intervalos por posicion de definicion. */
    bool operator<(const LiveInterval &o) const { return def < o.def; }
};

/**
 * @brief Resultado completo del analisis de vivacidad de una funcion.
 *
 * Contiene los intervalos de vida de todos los valores SSA y la
 * informacion de posicion de cada bloque basico en el orden lineal.
 */
struct LivenessResult {
    std::vector<LiveInterval> intervals;   ///< intervalos ordenados por def
    std::vector<uint32_t>     block_start; ///< block_start[b] = primer indice del bloque b
    std::vector<uint32_t>     block_end;   ///< block_end[b]   = ultimo indice del bloque b
    uint32_t                   num_instrs;  ///< total de instrucciones linealizadas
};

/**
 * @brief Calcula los intervalos de vida de todos los valores SSA de una funcion.
 *
 * @param fn Funcion SSA a analizar.
 * @return Resultado con los intervalos de vida y mapas de posicion de bloques.
 */
LivenessResult compute_liveness(const IrFunction &fn);

} // namespace ir

#endif // LIVENESS_H
