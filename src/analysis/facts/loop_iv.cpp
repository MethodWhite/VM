/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file loop_iv.cpp
 * @brief Implementacion del descubridor de la variable de induccion (loop_iv.h).
 */

#include "analysis/facts/loop_iv.h"

namespace analysis {

using ir::IrBlockId;
using ir::IrInstr;
using ir::IrOp;
using ir::IrValueId;
using ir::IR_NO_BLOCK;
using ir::IR_NO_VALUE;

namespace {

// Guarda creciente reconocida: `iv < N` / `iv <= N` (signed y unsigned).
bool is_lt_cmp(IrOp op) {
    return op == IrOp::CMP_LT || op == IrOp::CMP_LE ||
           op == IrOp::CMP_ULT || op == IrOp::CMP_ULE;
}

// Resuelve el valor CONSTANTE de @p v (la CONST que lo define en su bloque).
bool const_of(const ir::IrFunction &fn, const std::vector<int> &def_block,
              IrValueId v, int64_t &out) {
    if (v == IR_NO_VALUE || v >= fn.values.size()) return false;
    const int db = (v < def_block.size()) ? def_block[v] : -1;
    if (db < 0 || (size_t)db >= fn.blocks.size()) return false;
    for (const IrInstr &in : fn.blocks[db].instrs)
        if (in.dst == v && in.op == IrOp::CONST) {
            out = (int64_t)in.imm;
            return true;
        }
    return false;
}

// Descompone @p v = ADD(base, const) (en cualquier orden).  Devuelve base y c.
bool add_of(const ir::IrFunction &fn, const std::vector<int> &def_block,
            IrValueId v, IrValueId &base, int64_t &c) {
    const int db =
        (v < def_block.size() && v != IR_NO_VALUE) ? def_block[v] : -1;
    if (db < 0 || (size_t)db >= fn.blocks.size()) return false;
    for (const IrInstr &in : fn.blocks[db].instrs) {
        if (in.dst != v || in.op != IrOp::ADD || in.operands.size() != 2)
            continue;
        int64_t k;
        if (const_of(fn, def_block, in.operands[1], k)) {
            base = in.operands[0];
            c = k;
            return true;
        }
        if (const_of(fn, def_block, in.operands[0], k)) {
            base = in.operands[1];
            c = k;
            return true;
        }
    }
    return false;
}

} // namespace

bool detect_loop_iv(const ir::IrFunction &fn,
                    const std::vector<int> &def_block, IrBlockId header,
                    IrBlockId preheader, IrBlockId latch, LoopIV &out) {
    out.phi_index = -1;
    if (header == (IrBlockId)IR_NO_BLOCK || header >= fn.blocks.size())
        return false;
    const auto &hins = fn.blocks[header].instrs;
    if (hins.empty()) return false;

    // 1) La guarda: el cmp (creciente) que define la condicion del BR_COND.
    const IrInstr &term = hins.back();
    if (term.op != IrOp::BR_COND || term.operands.empty()) return false;
    const IrValueId cond = term.operands[0];
    IrOp cmp_op = IrOp::NOP;
    IrValueId cmp_a = IR_NO_VALUE, cmp_b = IR_NO_VALUE;
    for (const IrInstr &in : hins) {
        if (in.dst == cond && is_lt_cmp(in.op) && in.operands.size() == 2) {
            cmp_op = in.op;
            cmp_a = in.operands[0];
            cmp_b = in.operands[1];
            break;
        }
    }
    if (cmp_op == IrOp::NOP) return false;

    // 2) El IV: PHI del header cuyo valor de retorno (arg desde el latch) es
    //    `phi + S` con S constante > 0.  init = arg desde el preheader.
    int phi_index = -1;
    for (const IrInstr &in : hins) {
        if (in.op != IrOp::PHI) continue;
        ++phi_index; // indice entre las PHIs, en orden de aparicion.
        IrValueId init = IR_NO_VALUE, back = IR_NO_VALUE;
        for (const auto &pa : in.phi_args) {
            if (pa.block == preheader) init = pa.value;
            else if (pa.block == latch) back = pa.value;
        }
        if (init == IR_NO_VALUE || back == IR_NO_VALUE) continue;
        IrValueId base;
        int64_t s;
        if (add_of(fn, def_block, back, base, s) && base == in.dst && s > 0) {
            // 3) La cota: el cmp compara `iv` o `iv + c` con N (el otro lado).
            //    El frontend puede extender el IV antes del cmp (SEXT/ZEXT si el
            //    IV es i32 y la cota i64): entonces `cmp_a` es SEXT(iv).  El
            //    analisis lee a traves de la extension para hallar el PHI.
            int64_t off = 0;
            IrValueId bound = IR_NO_VALUE;
            IrValueId cbase = cmp_a;
            IrOp ext_op = IrOp::NOP;
            for (;;) {
                if (cbase == in.dst) break; // cmp compara el IV directo.
                const int ci = (cbase < def_block.size()) ? def_block[cbase] : -1;
                if (ci < 0 || ci >= (int)fn.blocks.size()) { cbase = IR_NO_VALUE; break; }
                const auto &blk = fn.blocks[ci].instrs;
                const IrInstr *def = nullptr;
                for (const auto &b : blk)
                    if (b.dst == cbase) { def = &b; break; }
                if (!def) { cbase = IR_NO_VALUE; break; }
                // Extension de signo/cero del IV (i32->i64) antes del cmp.
                if (def->op == IrOp::SEXT || def->op == IrOp::ZEXT ||
                    def->op == IrOp::CAST || def->op == IrOp::TRUNC) {
                    if (def->operands.empty()) { cbase = IR_NO_VALUE; break; }
                    if (ext_op == IrOp::NOP) ext_op = def->op;
                    cbase = def->operands[0];
                    continue;
                }
                // IV + c antes del cmp.
                IrValueId cb;
                int64_t c;
                if (add_of(fn, def_block, cbase, cb, c) && cb == in.dst) {
                    off = c;
                    cbase = in.dst;
                    break;
                }
                cbase = IR_NO_VALUE;
                break;
            }
            if (cbase == IR_NO_VALUE) return false; // cmp_b seria el IV (decreciente).
            bound = cmp_b;
            out.phi = in.dst;
            out.phi_index = phi_index;
            out.init = init;
            out.stride = s;
            out.cmp_op = cmp_op;
            out.cmp_offset = off;
            out.bound = bound;
            out.cmp_a = cmp_a;
            out.iv_ext_op = ext_op;
            return true;
        }
    }
    return false;
}

} // namespace analysis
