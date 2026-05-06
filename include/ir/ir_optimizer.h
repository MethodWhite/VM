/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file ir_optimizer.h
 * @brief Optimizador de la SSA IR de VestaVM con niveles O0-O3.
 *
 * Cada nivel activa un conjunto acumulativo de pases de transformacion:
 *
 *   O0: sin optimizacion (utl para depuracion)
 *   O1: eliminacion de codigo muerto (DCE) + propagacion de copias
 *   O2: O1 + plegado de constantes + eliminacion de bloques inalcanzables
 *   O3: O2 + eliminacion de subexpresiones comunes (CSE)
 *
 * Todos los pases operan directamente sobre IrFunction o IrModule en memoria.
 * No generan texto ni bytecode; son transformaciones puras sobre la IR.
 *
 * Los pases son seguros para SSA: no rompen la propiedad de asignacion
 * unica, salvo que se eliminen instrucciones completas.
 */

#ifndef IR_OPTIMIZER_H
#define IR_OPTIMIZER_H

#include "ir/ssa_ir.h"

namespace ir {

/**
 * @brief Nivel de optimizacion de la SSA IR.
 */
enum class OptLevel : int {
    O0 = 0, ///< sin optimizacion
    O1 = 1, ///< DCE + copia
    O2 = 2, ///< O1 + plegado + inalcanzables
    O3 = 3, ///< O2 + CSE
};

/**
 * @brief Convierte un entero 0-3 al nivel de optimizacion correspondiente.
 * @param n Nivel numerico (se satura a O3 si n>3).
 * @return OptLevel equivalente.
 */
inline OptLevel opt_level_from_int(int n) {
    if (n <= 0) return OptLevel::O0;
    if (n == 1) return OptLevel::O1;
    if (n == 2) return OptLevel::O2;
    return OptLevel::O3;
}

// =========================================================================
//  Interfaz publica principal
// =========================================================================

/**
 * @brief Aplica todos los pases de optimizacion del nivel indicado al modulo.
 *
 * Ejecuta los pases hasta punto fijo o hasta un maximo de 8 iteraciones para
 * capturar oportunidades encadenadas (p.ej., un DCE puede exponer nuevo CSE).
 *
 * @param mod   Modulo IR a optimizar (modificado en su lugar).
 * @param level Nivel de optimizacion.
 */
void ir_optimize(IrModule &mod, OptLevel level);

// =========================================================================
//  Pases individuales (se pueden invocar directamente si se desea)
// =========================================================================

/**
 * @brief Pase DCE: elimina instrucciones cuyo resultado nunca se usa.
 *
 * Solo elimina instrucciones puras (aritmetica, logica, MOV, CONST, CMP, PHI,
 * CAST, GETFIELD, etc.).  Instrucciones con efectos laterales (CALLN, STORE,
 * THROW, BR, monitores, async, distribuidas) nunca se eliminan.
 *
 * @param fn Funcion a optimizar.
 * @return true si se elimino al menos una instruccion.
 */
bool ir_pass_dce(IrFunction &fn);

/**
 * @brief Pase de propagacion de copias.
 *
 * Para cada %b = mov.T %a, sustituye todos los usos de %b por %a y
 * elimina el MOV.  Funciona en un unico recorrido.
 *
 * @param fn Funcion a optimizar.
 * @return true si se realizo al menos una sustitucion.
 */
bool ir_pass_copy_prop(IrFunction &fn);

/**
 * @brief Pase de plegado de constantes.
 *
 * Evalua en tiempo de compilacion expresiones cuyos operandos son todas
 * constantes literales (IrValue::is_const).  Soporta ADD/SUB/MUL/DIV/MOD/
 * AND/OR/XOR/NOT/SHL/SHR/SAR/NEG y todas las comparaciones enteras.
 *
 * @param fn Funcion a optimizar.
 * @return true si se plego al menos una instruccion.
 */
bool ir_pass_const_fold(IrFunction &fn);

/**
 * @brief Pase de eliminacion de bloques inalcanzables.
 *
 * Calcula los bloques alcanzables desde el bloque de entrada mediante BFS/DFS
 * y elimina los bloques que no son alcanzables.  Actualiza los predecesores
 * de los bloques restantes y elimina los argumentos phi correspondientes.
 *
 * @param fn Funcion a optimizar.
 * @return true si se elimino al menos un bloque.
 */
bool ir_pass_unreachable(IrFunction &fn);

/**
 * @brief Pase CSE: eliminacion de subexpresiones comunes.
 *
 * Para instrucciones puras con el mismo opcode, tipo y operandos (en orden),
 * sustituye el destino por el de la primera ocurrencia y elimina el duplicado.
 * Solo opera dentro del mismo bloque basico (CSE local).
 *
 * @param fn Funcion a optimizar.
 * @return true si se elimino al menos una instruccion.
 */
bool ir_pass_cse(IrFunction &fn);

/**
 * @brief Pase TCO: optimizacion de llamadas en cola (Tail Call Optimization).
 *
 * Detecta el patron CALL @f(args) seguido de RET %result (o RET void) y
 * convierte el CALL en TAILCALL, eliminando la instruccion RET subsiguiente.
 * Aplica tanto a llamadas recursivas como a llamadas a otras funciones.
 *
 * El pase se activa automaticamente en O2 y superiores.
 *
 * @param fn Funcion a optimizar.
 * @return true si se convirtio al menos una llamada en cola.
 */
bool ir_pass_tailcall(IrFunction &fn);

} // namespace ir

#endif // IR_OPTIMIZER_H
