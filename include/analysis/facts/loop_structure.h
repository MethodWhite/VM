/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file loop_structure.h
 * @brief Hecho reutilizable: forma estructural de un bucle CONTADO simple.
 *
 * Verifica que un bucle (identificado por LoopFacts) es reducible, con un unico
 * latch, una unica salida, un unico preheader y un header "limpio" (solo PHIs +
 * la condicion + el BR_COND), y que esta en forma loop-closed SSA (ningun valor
 * del cuerpo se usa fuera del bucle salvo por las PHIs del header).  Es
 * INFRAESTRUCTURA DE ANALISIS: la consumen el desenrollado, el peeling, el
 * versioning, etc.  No transforma nada.
 */
#ifndef ANALYSIS_FACTS_LOOP_STRUCTURE_H
#define ANALYSIS_FACTS_LOOP_STRUCTURE_H

#include "analysis/facts/loop_facts.h"
#include "ir/ssa_ir.h"

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace analysis {

/// PHI del header: valor + su arg de entrada (preheader) y de retorno (latch).
struct HeaderPhi {
    ir::IrValueId dst = ir::IR_NO_VALUE;
    ir::IrValueId init = ir::IR_NO_VALUE; ///< arg desde el preheader.
    ir::IrValueId back = ir::IR_NO_VALUE; ///< arg desde el latch (loop-carried).
};

/// Forma estructural de un bucle contado.  @c valid=false si no es elegible.
struct LoopStructure {
    bool valid = false;
    ir::IrBlockId header = ir::IR_NO_BLOCK;
    ir::IrBlockId preheader = ir::IR_NO_BLOCK; ///< unico pred del header fuera.
    ir::IrBlockId latch = ir::IR_NO_BLOCK;     ///< unico bloque con back-edge.
    ir::IrBlockId body_entry = ir::IR_NO_BLOCK; ///< sucesor del header DENTRO.
    ir::IrBlockId exit = ir::IR_NO_BLOCK;       ///< sucesor del header FUERA.
    std::vector<ir::IrBlockId> body;            ///< bloques del bucle salvo H.
    std::unordered_set<ir::IrBlockId> loop_blocks; ///< header + body (membresia).
    std::vector<HeaderPhi> phis;                ///< PHIs del header, en orden.
    /// Instrucciones del header que alimentan transitivamente a la condicion
    /// del BR_COND (cond-chain), sin PHIs ni el terminador.  P.ej. el frontend
    /// baja `while (i < N)` con i i32 como PHI(i32)->SEXT->CMP_LT->BR_COND, con
    /// el SEXT dentro del header.  El unroll las re-clona en cada copia.
    std::vector<ir::IrInstr> header_body;

    bool contains(ir::IrBlockId b) const { return loop_blocks.count(b) != 0; }
};

/**
 * @brief Analiza la forma estructural del bucle @p loop_id (innermost).
 * @param fn      funcion SSA.
 * @param lf      hechos de bucles ya calculados.
 * @param loop_id id del bucle (innermost) en @p lf.
 * @return LoopStructure con @c valid=true si es un bucle contado simple.
 */
LoopStructure detect_loop_structure(const ir::IrFunction &fn,
                                    const analysis::LoopFacts &lf,
                                    uint32_t loop_id);

} // namespace analysis

#endif // ANALYSIS_FACTS_LOOP_STRUCTURE_H
