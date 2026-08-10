/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file loop_metrics.cpp
 * @brief Implementacion del medidor NEUTRAL del cuerpo de un bucle.
 */

#include "analysis/facts/loop_metrics.h"

#include <unordered_set>

namespace analysis {

using ir::IrBlockId;
using ir::IrInstr;
using ir::IrOp;
using ir::IrValueId;
using ir::IR_NO_VALUE;

namespace {

bool is_load_like(IrOp op) {
    return op == IrOp::LOAD || op == IrOp::GETFIELD || op == IrOp::ARRAY_LOAD;
}
bool is_store_like(IrOp op) {
    switch (op) {
    case IrOp::STORE:
    case IrOp::SETFIELD:
    case IrOp::ARRAY_STORE:
    case IrOp::MEMCPY:
    return true;
    default: return false;
    }
}
bool is_call_like(IrOp op) {
    switch (op) {
    case IrOp::CALL:
    case IrOp::CALLN:
    case IrOp::CALLIND:
    case IrOp::CALLVIRT:
    case IrOp::CALLM:
    case IrOp::CALLITF:
    case IrOp::CALLCLOSURE:
    case IrOp::CALLSUPER:
    case IrOp::TAILCALL: return true;
    default: return false;
    }
}
bool is_vec(IrOp op) {
    (void)op;
    return false; // el IR local no tiene ops vectoriales propias.
}
bool is_fp(IrOp op) {
    switch (op) {
    case IrOp::FADD:
    case IrOp::FSUB:
    case IrOp::FMUL:
    case IrOp::FDIV:
    case IrOp::FNEG:
    case IrOp::FABS:
    case IrOp::FSQRT:
    case IrOp::FMIN:
    case IrOp::FMAX:
    return true;
    default: return false;
    }
}
bool is_expensive(IrOp op) {
    switch (op) {
    case IrOp::DIV:
    case IrOp::MOD:
    case IrOp::FDIV:
    case IrOp::FSQRT: return true;
    default: return false;
    }
}

} // namespace

LoopMetrics compute_loop_metrics(const ir::IrFunction &fn,
                                 const std::vector<IrBlockId> &body) {
    LoopMetrics m;
    std::unordered_set<IrValueId> body_defs;
    std::unordered_set<IrBlockId> body_set(body.begin(), body.end());

    for (IrBlockId b : body) {
        if (b >= fn.blocks.size()) continue;
        ++m.basic_blocks;
        for (const IrInstr &in : fn.blocks[b].instrs) {
            const bool is_term =
                (in.op == IrOp::BR || in.op == IrOp::BR_COND ||
                 in.op == IrOp::RET || in.op == IrOp::THROW ||
                 in.op == IrOp::UNREACHABLE);
            if (is_term) ++m.terminators;
            if (in.op == IrOp::PHI) {
                ++m.phis;
            } else if (!is_term) {
                ++m.instructions;
                if (is_load_like(in.op)) ++m.loads;
                if (is_store_like(in.op)) {
                    ++m.stores;
                    m.has_side_effects = true;
                }
                if (is_call_like(in.op)) {
                    ++m.calls;
                    m.has_side_effects = true;
                }
                if (is_vec(in.op)) ++m.vector_ops;
                if (is_fp(in.op)) ++m.fp_ops;
                if (is_expensive(in.op)) ++m.expensive_ops;
                // Efectos no capturados por los conteos: atomics, io, barreras.
                if (in.op == IrOp::ATOMIC_LD_I64 ||
                    in.op == IrOp::ATOMIC_ST_I64 ||
                    in.op == IrOp::ATOMIC_CAS_I64 ||
                    in.op == IrOp::ATOMIC_ADD_I64 || in.op == IrOp::RAW_ASM)
                    m.has_side_effects = true;
            }
            if (in.op == IrOp::BR_COND) ++m.branches;
            if (in.dst != IR_NO_VALUE) body_defs.insert(in.dst);
        }
    }

    // Presion de registros (proxy): valores DEFINIDOS en el cuerpo que se usan
    // FUERA del cuerpo -- los back-args de las PHIs del header (loop-carried) mas
    // los live-out.  Son los unicos que cada copia del unroll mantiene vivos a la
    // vez; los temporales intra-iteracion se consumen dentro de su copia y NO
    // cuentan.  (El proxy anterior miraba el latch: en un bucle de un solo bloque
    // latch == cuerpo, contaba los temporales intra-iteracion e inflaba la
    // presion, capando el factor.)
    std::unordered_set<IrValueId> live;
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        if (body_set.count((IrBlockId)b)) continue; // solo bloques FUERA del cuerpo
        for (const IrInstr &in : fn.blocks[b].instrs) {
            for (IrValueId o : in.operands)
                if (body_defs.count(o)) live.insert(o);
            for (const auto &pa : in.phi_args)
                if (body_defs.count(pa.value)) live.insert(pa.value);
        }
    }
    m.live_across = (int)live.size();
    return m;
}

} // namespace analysis
