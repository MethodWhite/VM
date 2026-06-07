/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/ir/test_speculative_devirt.cpp
 * @brief Test del pase C2 @c ir_pass_speculative_devirt (devirt especulativa
 *        guiada por IC).
 *
 * Construye a mano una funcion con un CALLVIRT y verifica que el pase emite el
 * dispatch guardado:
 *
 *     caller(obj) { r = callvirt obj, vtbl=2 (); s = r + 1; ret s }
 *
 *   ->  entry:  cls = load[obj]; cmp cls, T; br_cond fast/slow
 *       fast:   r_fast = call Impl__val(obj); br merge
 *       slow:   r_slow = callvirt obj, 2 ();  br merge
 *       merge:  r = phi[r_fast, r_slow]; s = r + 1; ret s
 *
 * Es CORRECTO POR CONSTRUCCION: el slow path conserva el CALLVIRT original.
 */

#include "ir/ssa_ir.h"
#include "ir/ir_optimizer.h"

#include <cstdio>
#include <vector>

using namespace ir;

static int g_checks = 0;
static int g_fails  = 0;
static void check(bool cond, const char *msg) {
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL: %s\n", msg); }
    else        std::printf("  ok:   %s\n", msg);
}

int main() {
    /* --- Construir caller(obj: ptr) -> i32 con un CALLVIRT. --- */
    IrFunction fn;
    fn.name     = "caller";
    fn.ret_type = IrType::I32;

    const IrValueId obj = fn.new_value(IrType::PTR, "obj");
    fn.params.push_back(obj);
    const IrBlockId entry = fn.new_block("entry");

    const IrValueId r   = fn.new_value(IrType::I32, "r");
    const IrValueId one = fn.new_value(IrType::I32, "one");
    const IrValueId s   = fn.new_value(IrType::I32, "s");

    {
        IrInstr cv; cv.op = IrOp::CALLVIRT; cv.type = IrType::I32;
        cv.dst = r; cv.operands = {obj}; cv.imm = 2;
        fn.blocks[entry].instrs.push_back(cv);
    }
    {
        IrInstr c; c.op = IrOp::CONST; c.type = IrType::I32; c.dst = one; c.imm = 1;
        fn.blocks[entry].instrs.push_back(c);
    }
    {
        IrInstr a; a.op = IrOp::ADD; a.type = IrType::I32; a.dst = s;
        a.operands = {r, one};
        fn.blocks[entry].instrs.push_back(a);
    }
    {
        IrInstr rt; rt.op = IrOp::RET; rt.type = IrType::I32; rt.operands = {s};
        fn.blocks[entry].instrs.push_back(rt);
    }

    /* --- Ejecutar el pase. --- */
    std::vector<SpecDevirtSite> sites = {
        SpecDevirtSite{ r, /*class_ptr=*/0x1000ULL, /*callee=*/"Impl__val" }
    };
    const bool changed = ir_pass_speculative_devirt(fn, sites);

    check(changed, "el pase reporta cambio");
    check(fn.blocks.size() == 4, "split: entry + fast + slow + merge = 4 bloques");

    int n_load = 0, n_cmp = 0, n_brcond = 0, n_call_impl = 0, n_callvirt = 0;
    int n_phi = 0, n_add = 0;
    bool phi_dst_is_r = false, add_uses_r = false, callvirt_new_dst = false;
    for (const auto &b : fn.blocks) {
        for (const auto &in : b.instrs) {
            switch (in.op) {
                case IrOp::LOAD:    ++n_load; break;
                case IrOp::CMP_EQ:  ++n_cmp; break;
                case IrOp::BR_COND: ++n_brcond; break;
                case IrOp::CALL:
                    if (in.func_name == "Impl__val") ++n_call_impl;
                    break;
                case IrOp::CALLVIRT:
                    ++n_callvirt;
                    if (in.dst != r && in.dst != IR_NO_VALUE) callvirt_new_dst = true;
                    break;
                case IrOp::PHI:
                    ++n_phi;
                    if (in.dst == r) phi_dst_is_r = true;
                    break;
                case IrOp::ADD:
                    ++n_add;
                    for (auto o : in.operands) if (o == r) add_uses_r = true;
                    break;
                default: break;
            }
        }
    }

    check(n_load == 1,        "1 LOAD del class_ptr (guard)");
    check(n_cmp == 1,         "1 CMP_EQ (guard clase == T)");
    check(n_brcond == 1,      "1 BR_COND (guard)");
    check(n_call_impl == 1,   "1 CALL directo a Impl__val (fast path)");
    check(n_callvirt == 1,    "1 CALLVIRT conservado (slow path fallback)");
    check(callvirt_new_dst,   "el CALLVIRT slow tiene un dst NUEVO (no el original)");
    check(n_phi == 1,         "1 PHI en el merge");
    check(phi_dst_is_r,       "el PHI reusa el dst ORIGINAL del CALLVIRT");
    check(n_add == 1 && add_uses_r,
                              "el ADD (tail) usa el resultado del merge (dst original)");

    /* Re-ejecutar con el dst del CALLVIRT slow (nuevo) NO debe re-encontrar el
     * site original (ya transformado): no debe partir mas alla de lo razonable. */
    const size_t blocks_after_first = fn.blocks.size();
    std::vector<SpecDevirtSite> none = {
        SpecDevirtSite{ r, 0x1000ULL, "Impl__val" }   /* r ahora es el PHI, no un CALLVIRT */
    };
    const bool changed2 = ir_pass_speculative_devirt(fn, none);
    check(!changed2, "re-ejecutar con un dst que ya no es CALLVIRT no transforma");
    check(fn.blocks.size() == blocks_after_first, "no se crean bloques de mas");

    std::printf("test_speculative_devirt: %d checks, %d fails\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
