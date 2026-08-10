/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file loop_iv.h
 * @brief Hecho reutilizable: variable de induccion (IV) CONTADA de un bucle.
 *
 * Es INFRAESTRUCTURA DE ANALISIS, no de transformacion.  La consumen el
 * desenrollado, la vectorizacion, el peeling, el strength-reduction, etc.  El
 * transformador no descubre el IV; solo pide este hecho.
 */
#ifndef ANALYSIS_FACTS_LOOP_IV_H
#define ANALYSIS_FACTS_LOOP_IV_H

#include "ir/ssa_ir.h"

#include <cstdint>
#include <vector>

namespace analysis {

/**
 * @brief Descriptor de la variable de induccion CONTADA de un bucle.
 *
 * SOLO forma canonica CRECIENTE (stride > 0):  el header itera mientras
 * @c cmp_op(iv + cmp_offset, bound)  y en cada iteracion  iv += stride.  El
 * @c cmp_offset != 0 modela el lookahead del vectorizador (guarda
 * @c cmp(iv + W, N)).  Para bucles decrecientes (`for(i=n;i>0;--i)`) habra que
 * ampliar el descriptor con una direccion (Direction) mas adelante.
 */
struct LoopIV {
    ir::IrValueId phi = ir::IR_NO_VALUE;   ///< PHI del header (el IV).
    int phi_index = -1;                    ///< indice de esa PHI entre las del
                                           ///< header (para no re-localizarla).
    ir::IrValueId init = ir::IR_NO_VALUE;  ///< valor inicial (desde el preheader).
    int64_t stride = 0;                    ///< incremento por iteracion (> 0).
    ir::IrOp cmp_op = ir::IrOp::NOP;       ///< CMP_LT/LE/ULT/ULE de la guarda.
    int64_t cmp_offset = 0;                ///< c de cmp(iv + c, bound) (0 = iv).
    ir::IrValueId bound = ir::IR_NO_VALUE; ///< cota N (invariante del bucle).
    /// Operando del cmp derivado del IV (tras extensión/offset): es el valor
    /// que la cond-chain del header produce a partir del PHI.  El unroll lo
    /// usa para localizar la instrucción clonada en el guard del UH.
    ir::IrValueId cmp_a = ir::IR_NO_VALUE;
};

/**
 * @brief Descubre el IV canonico creciente de un bucle reducible.
 *
 * Lee las PHIs del @p header (init = arg desde @p preheader, back = arg desde
 * @p latch) y la guarda (el cmp que define la condicion del BR_COND del header).
 * Identifica la PHI cuyo valor de retorno es `phi + S` con S constante > 0 (el
 * IV), y la cota N del cmp (que puede comparar `iv` o `iv + c`).  Rellena @p out
 * (incluido @c out.phi_index, el indice del IV entre las PHIs del header en
 * orden de aparicion).
 *
 * NO verifica que @c out.bound sea invariante del bucle: eso es una comprobacion
 * ESTRUCTURAL (necesita el conjunto de bloques del bucle) que hace el llamante.
 *
 * @return true si hay un IV canonico creciente con cota reconocible.
 */
bool detect_loop_iv(const ir::IrFunction &fn,
                    const std::vector<int> &def_block, ir::IrBlockId header,
                    ir::IrBlockId preheader, ir::IrBlockId latch, LoopIV &out);

} // namespace analysis

#endif // ANALYSIS_FACTS_LOOP_IV_H
