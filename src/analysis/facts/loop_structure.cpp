/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file loop_structure.cpp
 * @brief Implementacion del verificador estructural de bucles (loop_structure.h).
 */

#include "analysis/facts/loop_structure.h"

namespace analysis {

using ir::IrBlockId;
using ir::IrInstr;
using ir::IrOp;
using ir::IrValueId;
using ir::IR_NO_BLOCK;
using ir::IR_NO_VALUE;

LoopStructure detect_loop_structure(const ir::IrFunction &fn,
                                    const analysis::LoopFacts &lf,
                                    uint32_t loop_id) {
    LoopStructure st;
    const IrBlockId H = lf.header_block_of(loop_id);
    if (H == (IrBlockId)IR_NO_BLOCK || H >= fn.blocks.size()) return st;
    st.header = H;

    // Bloques del bucle (innermost == loop_id).  Cuerpo = todos salvo H.
    const size_t Nb = fn.blocks.size();
    st.loop_blocks.insert(H);
    for (size_t bi = 0; bi < Nb; ++bi) {
        if (lf.innermost((IrBlockId)bi) == loop_id) {
            st.loop_blocks.insert((IrBlockId)bi);
            if ((IrBlockId)bi != H) st.body.push_back((IrBlockId)bi);
        }
    }
    if (st.body.empty()) return st; // self-loop degenerado.

    // Header limpio: [PHIs...] + [instr que define cond] + [br_cond].  El
    // BR_COND debe tener exactamente un sucesor DENTRO y otro FUERA del bucle.
    const auto &hins = fn.blocks[H].instrs;
    if (hins.size() < 2) return st;
    const IrInstr &term = hins.back();
    if (term.op != IrOp::BR_COND || term.operands.empty()) return st;
    const IrValueId cond = term.operands[0];
    const bool t_in = st.contains(term.target_block);
    const bool f_in = st.contains(term.false_block);
    if (t_in == f_in) return st; // exactamente uno dentro, uno fuera.
    st.body_entry = t_in ? term.target_block : term.false_block;
    st.exit = t_in ? term.false_block : term.target_block;

    // Header: las instrucciones no-PHI deben alimentar transitivamente a la
    // condicion del BR_COND (cond-chain).  El frontend baja bucles contados con
    // la extension del IV (SEXT/ZEXT) y el CMP dentro del header, asique exigir
    // un unico "definidor de cond" rechazaria casi todos los bucles reales.
    // Recolectamos la cadena en header_body para que el unroll la re-clone.
    st.header_body.clear();
    {
        // Backward-slice desde `cond` a traves de def-use dentro del header:
        // `used` = valores que (transitivamente) alimentan a la condicion.
        std::unordered_set<IrValueId> used;
        used.insert(cond);
        for (bool progress = true; progress;) {
            progress = false;
            for (const IrInstr &in : hins) {
                if (in.op == IrOp::PHI) continue; // el slice para en las PHIs.
                if (in.dst == IR_NO_VALUE) continue; // instr sin dst (BR/STORE...).
                if (!used.count(in.dst)) continue;
                for (IrValueId o : in.operands)
                    if (!used.count(o)) { used.insert(o); progress = true; }
            }
        }
        for (const IrInstr &in : hins) {
            if (in.op == IrOp::PHI) continue;
            if (in.dst == IR_NO_VALUE) continue;
            if (in.dst == cond) continue; // el CMP final se reconstruye.
            if (!used.count(in.dst)) return st;
            st.header_body.push_back(in);
        }
    }

    // Latch: unico bloque del bucle cuyo terminador salta (BR) al header.
    for (IrBlockId b : st.body) {
        const auto &bi = fn.blocks[b].instrs;
        if (bi.empty()) continue;
        const IrInstr &bt = bi.back();
        if (bt.op == IrOp::BR && bt.target_block == H) {
            if (st.latch != IR_NO_BLOCK) return st; // >1 latch.
            st.latch = b;
        }
    }
    if (st.latch == IR_NO_BLOCK) return st;

    // Salida UNICA: ningun bloque del cuerpo salta FUERA del bucle.
    for (IrBlockId b : st.body) {
        const auto &bi = fn.blocks[b].instrs;
        if (bi.empty()) return st;
        const IrInstr &bt = bi.back();
        if (bt.op == IrOp::BR) {
            if (!st.contains(bt.target_block)) return st;
        } else if (bt.op == IrOp::BR_COND) {
            if (!st.contains(bt.target_block) || !st.contains(bt.false_block))
                return st;
        } else {
            return st; // RET/THROW/etc. -> multi-salida.
        }
    }

    // Preheader: unico pred del header FUERA del bucle.  Se calcula LOCALMENTE
    // desde los terminadores (no desde fn.blocks[].preds, que un pase previo pudo
    // dejar obsoletos) para no depender de mutar el CFG de la funcion.
    for (size_t p = 0; p < Nb; ++p) {
        if (st.contains((IrBlockId)p)) continue;
        const auto &pins = fn.blocks[p].instrs;
        if (pins.empty()) continue;
        const IrInstr &pt = pins.back();
        const bool goes_to_H =
            (pt.op == IrOp::BR && pt.target_block == H) ||
            (pt.op == IrOp::BR_COND &&
             (pt.target_block == H || pt.false_block == H));
        if (goes_to_H) {
            if (st.preheader != IR_NO_BLOCK) return st; // >1 entrada.
            st.preheader = (IrBlockId)p;
        }
    }
    if (st.preheader == IR_NO_BLOCK) return st;

    // PHIs del header: cada una con {arg preheader, arg latch}.
    for (const IrInstr &in : hins) {
        if (in.op != IrOp::PHI) continue;
        if (in.phi_args.size() != 2) return st;
        HeaderPhi hp;
        hp.dst = in.dst;
        for (const auto &pa : in.phi_args) {
            if (pa.block == st.preheader) hp.init = pa.value;
            else if (pa.block == st.latch) hp.back = pa.value;
            else return st; // arg desde un bloque inesperado.
        }
        if (hp.init == IR_NO_VALUE || hp.back == IR_NO_VALUE) return st;
        st.phis.push_back(hp);
    }
    if (st.phis.empty()) return st;

    // Loop-closed SSA: ningun valor del CUERPO se usa fuera del bucle (los
    // live-out salen por las PHIs del header).  Sin esto el remainder podria
    // dejar un valor indefinido en el exit.
    std::unordered_set<IrValueId> body_defs;
    for (IrBlockId b : st.body)
        for (const IrInstr &in : fn.blocks[b].instrs)
            if (in.dst != IR_NO_VALUE) body_defs.insert(in.dst);
    for (size_t bi = 0; bi < Nb; ++bi) {
        if (st.contains((IrBlockId)bi)) continue; // dentro del bucle: OK.
        for (const IrInstr &in : fn.blocks[bi].instrs) {
            for (IrValueId o : in.operands)
                if (body_defs.count(o)) return st;
            for (const auto &pa : in.phi_args) {
                if (st.contains(pa.block) && body_defs.count(pa.value)) return st;
                if (body_defs.count(pa.value) && !st.contains(pa.block)) return st;
            }
        }
    }

    st.valid = true;
    return st;
}

} // namespace analysis
