/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file loop_trip_count.cpp
 * @brief Implementacion del hecho trip-count (ver loop_trip_count.h).
 */

#include "analysis/facts/loop_trip_count.h"

namespace analysis {

using ir::IrInstr;
using ir::IrOp;
using ir::IrValueId;
using ir::IR_NO_VALUE;

namespace {

// Resuelve el valor CONSTANTE de @p v (la CONST que lo define en su bloque).
bool const_of(const ir::IrFunction &fn, const std::vector<int> &def_block,
              IrValueId v, int64_t &out) {
    if (v == IR_NO_VALUE || v >= fn.values.size()) return false;
    const int db = (v < def_block.size()) ? def_block[v] : -1;
    if (db < 0 || (size_t)db >= fn.blocks.size()) return false;
    for (const IrInstr &in : fn.blocks[db].instrs) {
        if (in.dst != v) continue;
        if (in.op == IrOp::CONST) {
            out = (int64_t)in.imm;
            return true;
        }
        // El frontend materializa literales i32 como TRUNC/CAST de un CONST i64.
        if ((in.op == IrOp::TRUNC || in.op == IrOp::CAST || in.op == IrOp::SEXT ||
             in.op == IrOp::ZEXT) &&
            in.operands.size() == 1)
            return const_of(fn, def_block, in.operands[0], out);
        return false;
    }
    return false;
}

} // namespace

LoopTripInfo compute_trip_count(const ir::IrFunction &fn,
                                const std::vector<int> &def_block,
                                const LoopIV &iv) {
    LoopTripInfo info;
    int64_t init_v = 0, bound_v = 0;
    if (iv.stride <= 0) return info;
    if (!const_of(fn, def_block, iv.init, init_v)) return info;
    if (!const_of(fn, def_block, iv.bound, bound_v)) return info;

    // room = cuanto queda desde el primer iv (mas el offset de la guarda) hasta N.
    const int64_t room = bound_v - iv.cmp_offset - init_v;
    if (room <= 0) {
        info.trip = 0; // no itera (guarda falsa de entrada).
        return info;
    }
    if (iv.cmp_op == IrOp::CMP_LT || iv.cmp_op == IrOp::CMP_ULT) {
        info.trip = (room + iv.stride - 1) / iv.stride; // ceil
    } else if (iv.cmp_op == IrOp::CMP_LE || iv.cmp_op == IrOp::CMP_ULE) {
        info.trip = room / iv.stride + 1;               // floor + 1
    }
    // Guarda no soportada -> trip queda en -1 (desconocido).
    return info;
}

} // namespace analysis
