/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/jit_regalloc.h
 * @brief Linear scan register allocation para el JIT (Phase D.7).
 *
 * = Diseno =
 *
 * El selector v1 (C1 template) emite cada SSA value como un slot stack
 * (offset @c -8*(vid+1) desde RBP).  Cada uso genera @c mov scratch,
 * [rbp + slot] y cada def @c mov [rbp + slot], result.  Esto es CORRECTO
 * pero subOPTIMO para hot loops: cada IR op accede memoria stack en
 * lugar de regs.
 *
 * Phase D.7 introduce un asignador de registros que mantiene los SSA
 * values mas usados en registros callee-saved durante la vida de la
 * funcion, evitando el round-trip a stack.
 *
 * = Estrategia v1 (MVP) =
 *
 * Asignacion estatica simple basada en heuristica de uso:
 *   1. Contar usos de cada VID en la IR.
 *   2. Filtrar candidatos: NO @c is_gc_object (stackmap simplicity),
 *      NO @c is_host_ptr, NO ALLOCA-derived, NO @c IrType::F32/F64.
 *   3. Marcar VIDs cuyo live range cruza un @c IrOp::CALL como
 *      "needs callee-saved" para que tras el call sigan vivos.
 *   4. Ordenar candidatos por @c (use_count, !crosses_call) desc.
 *   5. Asignar top-K (K = num callee-saved disponibles = 4: R12-R15)
 *      al pool de regs callee-saved.
 *
 * VIDs no asignados siguen el path slot-stack original (fallback).
 *
 * = Rewrite pass =
 *
 * Tras @c Selector::select() produce un @c MFunction, llamamos
 * @c apply_rewrite() que recorre todas las MInstrs y reemplaza:
 *   - @c MOperand{MEM, base=RBP, disp=slot_offset(pinned_vid)}
 *     -> @c MOperand{REG, assigned_host_reg}
 *
 * Width preservada: @c MOperand.width se copia al nuevo REG operand.
 *
 * = Prologue/Epilogue adjustments =
 *
 * Los regs pinned (R12-R15) son callee-saved en ambos ABIs (SysV/Win64).
 * El selector ya hace @c push rbx + @c push rbp; con regalloc activo
 * anyadimos @c push r12/r13/r14/r15 (solo los usados) ANTES del @c push
 * @c rbp, y @c pop r15/r14/r13/r12 DESPUES del @c pop @c rbp en el
 * epilogue.  El alignment se mantiene (cada push es 8 bytes; 4 pushes
 * = 32 bytes = multiplo de 16).
 *
 * = Stackmaps =
 *
 * VIDs @c is_gc_object EXCLUIDOS del pinning en v1 -> los stackmaps
 * existentes siguen siendo correctos (solo describen slots).  En v2
 * (Phase D.8), extender los stackmaps con register-resident GC values.
 */

#ifndef VESTA_JIT_JIT_REGALLOC_H
#define VESTA_JIT_JIT_REGALLOC_H

#include "jit/machine_ir.h"
#include "ir/ssa_ir.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jit {

    /**
     * @struct JitRegalloc
     * @brief Resultado del analisis + asignacion regalloc del JIT.
     *
     * Contiene el mapping VID -> registro host para los SSA values
     * elegidos, mas la lista de regs callee-saved usados (necesarios
     * para push/pop en prologue/epilogue).
     */
    struct JitRegalloc {
        /// VID asignado -> MReg host.  VIDs no presentes usan slot stack.
        std::unordered_map<ir::IrValueId, MReg> vid_to_reg;
        /// Regs callee-saved que el codigo emitido modifica y debe
        /// preservar.  Lista deduplicada en orden estable (R12, R13, ...).
        std::vector<MReg> callee_saved_used;

        /// True si la asignacion esta vacia (todos los VIDs usan slot).
        bool empty() const noexcept { return vid_to_reg.empty(); }

        /// Lookup rapido: devuelve MReg si vid esta pinned, MReg::NONE sino.
        MReg lookup(ir::IrValueId vid) const noexcept {
            auto it = vid_to_reg.find(vid);
            if (it == vid_to_reg.end()) return MReg::NONE;
            return it->second;
        }
    };

    /**
     * @brief Computa la asignacion regalloc para una @c IrFunction.
     *
     * @param fn  Funcion IR sobre la que ejecutar el analisis.
     * @return    Asignacion (puede estar vacia si no hay candidatos).
     */
    JitRegalloc compute_jit_regalloc(const ir::IrFunction &fn);

    /**
     * @brief Aplica el rewrite slot->reg sobre un @c MFunction.
     *
     * Recorre todas las MInstrs.  Cualquier @c MOperand de tipo MEM con
     * base=RBP y disp coincidente con @c slot_offset(pinned_vid) se
     * reemplaza por un REG operand del reg asignado, preservando width.
     *
     * El caller es responsable de anyadir push/pop de
     * @c regalloc.callee_saved_used en prologue/epilogue.
     *
     * @param mf       MFunction post-selector.
     * @param fn       IrFunction original (para mapear offset <-> vid).
     * @param regalloc Resultado de @c compute_jit_regalloc.
     */
    void apply_jit_regalloc_rewrite(MFunction &mf,
                                    const ir::IrFunction &fn,
                                    const JitRegalloc &regalloc);

    /**
     * @brief Convierte slot_offset (-8*(vid+1)) a vid, o -1 si invalido.
     */
    inline int32_t slot_offset_to_vid(int32_t offset) noexcept {
        if (offset >= 0) return -1;
        if (offset % 8 != 0) return -1;
        const int32_t vid = (-offset / 8) - 1;
        if (vid < 0) return -1;
        return vid;
    }

} // namespace jit

#endif // VESTA_JIT_JIT_REGALLOC_H
