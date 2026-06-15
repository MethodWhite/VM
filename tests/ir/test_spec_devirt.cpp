/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/ir/test_spec_devirt.cpp
 * @brief Test del pase C2 @c ir_pass_spec_devirt (TAREA 2): devirt especulativa
 *        ESTATICA via guard-chain de K candidatos sobre un dispatch CALLITF.
 *
 * Construye a mano una funcion con un CALLITF de interfaz y 3 candidatos
 * registrados en @c fn.spec_devirt_sites, y verifica que el pase emite el
 * guard-chain + fallback:
 *
 *     caller(obj, arg0) { r = callitf obj, buf, arg0; s = r + 1; ret s }
 *
 *   ->  B(entry):  cls = load[obj]; cmp cls, T0; br_cond fast0/guard1
 *       guard1:    cmp cls, T1; br_cond fast1/guard2
 *       guard2:    cmp cls, T2; br_cond fast2/fallback
 *       fast0:     r0 = call Circle__area(obj, arg0);   br merge
 *       fast1:     r1 = call Rect__area(obj, arg0);     br merge
 *       fast2:     r2 = call Triangle__area(obj, arg0); br merge
 *       fallback:  rs = callitf obj, buf, arg0 (copia); br merge
 *       merge:     r = phi[r0,r1,r2,rs]; s = r + 1; ret s
 *
 * CORRECTO POR CONSTRUCCION: el fallback conserva el CALLITF original ->
 * cualquier tipo no candidato (incl. loadmodule) usa el dispatch normal.
 *
 * El test valida la FORMA del CFG (bloques, guard-chain, CALLs directos,
 * fallback, PHI), no ejecuta el IR (eso se valida end-to-end en pic_real).
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
    /* --- Construir caller(obj: ptr, arg0: i64) -> i64 con un CALLITF. --- */
    IrFunction fn;
    fn.name     = "caller";
    fn.ret_type = IrType::I64;

    const IrValueId obj  = fn.new_value(IrType::PTR, "obj");
    const IrValueId arg0 = fn.new_value(IrType::I64, "arg0");
    fn.params.push_back(obj);
    fn.params.push_back(arg0);
    const IrBlockId entry = fn.new_block("entry");

    /* Los ClassInfo* de los candidatos se simulan como CONST en el entry
     * (en produccion son el resultado de findclass via slot-cache lazy). */
    const IrValueId vC0 = fn.new_value(IrType::I64, "clsCircle");
    const IrValueId vC1 = fn.new_value(IrType::I64, "clsRect");
    const IrValueId vC2 = fn.new_value(IrType::I64, "clsTriangle");
    const IrValueId buf = fn.new_value(IrType::PTR, "params");
    const IrValueId r   = fn.new_value(IrType::I64, "r");
    const IrValueId one = fn.new_value(IrType::I64, "one");
    const IrValueId s   = fn.new_value(IrType::I64, "s");

    auto emit_const = [&](IrValueId dst, uint64_t k) {
        IrInstr c; c.op = IrOp::CONST; c.type = IrType::I64; c.dst = dst; c.imm = k;
        fn.blocks[entry].instrs.push_back(c);
    };
    emit_const(vC0, 0x1000);
    emit_const(vC1, 0x2000);
    emit_const(vC2, 0x3000);
    {
        IrInstr al; al.op = IrOp::ALLOCA; al.type = IrType::I8; al.imm = 32; al.dst = buf;
        fn.blocks[entry].instrs.push_back(al);
    }
    {
        /* operands = [obj, params_ptr, arg0].  El CALL directo del fast path
         * debe quitar params_ptr (ops[1]) -> [obj, arg0]. */
        IrInstr ci; ci.op = IrOp::CALLITF; ci.type = IrType::I64; ci.dst = r;
        ci.func_name = "Shape\x1f" "area";
        ci.operands = {obj, buf, arg0};
        fn.blocks[entry].instrs.push_back(ci);
    }
    {
        IrInstr c; c.op = IrOp::CONST; c.type = IrType::I64; c.dst = one; c.imm = 1;
        fn.blocks[entry].instrs.push_back(c);
    }
    {
        IrInstr a; a.op = IrOp::ADD; a.type = IrType::I64; a.dst = s; a.operands = {r, one};
        fn.blocks[entry].instrs.push_back(a);
    }
    {
        IrInstr rt; rt.op = IrOp::RET; rt.type = IrType::I64; rt.operands = {s};
        fn.blocks[entry].instrs.push_back(rt);
    }

    /* --- Registrar el site con 3 candidatos. --- */
    fn.spec_devirt_sites[r] = {
        DevirtCandidate{ vC0, "Circle__area"   },
        DevirtCandidate{ vC1, "Rect__area"     },
        DevirtCandidate{ vC2, "Triangle__area" },
    };

    /* --- Ejecutar el pase. --- */
    const bool changed = ir_pass_spec_devirt(fn);

    check(changed, "el pase reporta cambio");
    /* B(entry) + 3 fast + 2 guard nuevos + 1 fallback + 1 merge = 8 bloques. */
    check(fn.blocks.size() == 8, "split K=3: entry + 3 fast + 2 guard + fallback + merge = 8");

    int n_load = 0, n_cmp = 0, n_brcond = 0, n_callitf = 0, n_phi = 0, n_add = 0;
    int n_call_circle = 0, n_call_rect = 0, n_call_tri = 0;
    bool callitf_new_dst = false, phi_dst_is_r = false, add_uses_r = false;
    int phi_args = 0;
    bool fast_call_drops_params = true;  /* los CALL directos NO deben llevar buf */
    int  n_cmp_uses_c0 = 0, n_cmp_uses_c1 = 0, n_cmp_uses_c2 = 0;

    for (const auto &b : fn.blocks) {
        for (const auto &in : b.instrs) {
            switch (in.op) {
                case IrOp::LOAD:    ++n_load; break;
                case IrOp::CMP_EQ:
                    ++n_cmp;
                    for (auto o : in.operands) {
                        if (o == vC0) ++n_cmp_uses_c0;
                        if (o == vC1) ++n_cmp_uses_c1;
                        if (o == vC2) ++n_cmp_uses_c2;
                    }
                    break;
                case IrOp::BR_COND: ++n_brcond; break;
                case IrOp::CALL:
                    if (in.func_name == "Circle__area")   ++n_call_circle;
                    if (in.func_name == "Rect__area")     ++n_call_rect;
                    if (in.func_name == "Triangle__area") ++n_call_tri;
                    /* call_ops esperado = {obj, arg0}: no debe contener buf. */
                    for (auto o : in.operands) if (o == buf) fast_call_drops_params = false;
                    break;
                case IrOp::CALLITF:
                    ++n_callitf;
                    if (in.dst != r && in.dst != IR_NO_VALUE) callitf_new_dst = true;
                    break;
                case IrOp::PHI:
                    ++n_phi;
                    if (in.dst == r) phi_dst_is_r = true;
                    phi_args = static_cast<int>(in.phi_args.size());
                    break;
                case IrOp::ADD:
                    ++n_add;
                    for (auto o : in.operands) if (o == r) add_uses_r = true;
                    break;
                default: break;
            }
        }
    }

    check(n_load == 1,       "1 LOAD del class_ptr (guard, computado una vez en B)");
    check(n_cmp == 3,        "3 CMP_EQ (un guard por candidato)");
    check(n_brcond == 3,     "3 BR_COND (guard-chain)");
    check(n_call_circle == 1 && n_call_rect == 1 && n_call_tri == 1,
                             "3 CALL directos: Circle/Rect/Triangle__area");
    check(fast_call_drops_params,
                             "los CALL directos quitan el params_ptr (solo obj + args)");
    check(n_callitf == 1,    "1 CALLITF conservado (fallback)");
    check(callitf_new_dst,   "el CALLITF fallback tiene dst NUEVO (no el original)");
    check(n_phi == 1,        "1 PHI en el merge");
    check(phi_dst_is_r,      "el PHI reusa el dst ORIGINAL del CALLITF");
    check(phi_args == 4,     "el PHI tiene 4 args (3 fast + 1 fallback)");
    check(n_add == 1 && add_uses_r,
                             "el ADD (tail) usa el resultado del merge (dst original)");
    check(n_cmp_uses_c0 == 1 && n_cmp_uses_c1 == 1 && n_cmp_uses_c2 == 1,
                             "cada guard compara contra su cls_value de candidato");

    /* Re-ejecutar: el site ya no apunta a un CALLITF (r es ahora el PHI) ->
     * sin transformacion. */
    const size_t after = fn.blocks.size();
    const bool changed2 = ir_pass_spec_devirt(fn);
    check(!changed2, "re-ejecutar no re-transforma (r ya no es un call dinamico)");
    check(fn.blocks.size() == after, "no se crean bloques de mas");

    /* --- Caso K=1 (un solo candidato): fallback va directo tras el guard. --- */
    {
        IrFunction g;
        g.name = "caller1"; g.ret_type = IrType::I64;
        const IrValueId o  = g.new_value(IrType::PTR, "o");
        g.params.push_back(o);
        const IrBlockId e  = g.new_block("entry");
        const IrValueId c0 = g.new_value(IrType::I64, "c0");
        const IrValueId pb = g.new_value(IrType::PTR, "pb");
        const IrValueId rr = g.new_value(IrType::I64, "rr");
        { IrInstr c; c.op = IrOp::CONST; c.type = IrType::I64; c.dst = c0; c.imm = 0x1000;
          g.blocks[e].instrs.push_back(c); }
        { IrInstr al; al.op = IrOp::ALLOCA; al.type = IrType::I8; al.imm = 32; al.dst = pb;
          g.blocks[e].instrs.push_back(al); }
        { IrInstr ci; ci.op = IrOp::CALLITF; ci.type = IrType::I64; ci.dst = rr;
          ci.func_name = "Shape\x1f" "area"; ci.operands = {o, pb};
          g.blocks[e].instrs.push_back(ci); }
        { IrInstr rt; rt.op = IrOp::RET; rt.type = IrType::I64; rt.operands = {rr};
          g.blocks[e].instrs.push_back(rt); }
        g.spec_devirt_sites[rr] = { DevirtCandidate{ c0, "Circle__area" } };
        const bool ch = ir_pass_spec_devirt(g);
        check(ch, "K=1: el pase transforma");
        /* entry + 1 fast + 0 guard nuevos + fallback + merge = 4 bloques. */
        check(g.blocks.size() == 4, "K=1: entry + fast + fallback + merge = 4 bloques");
    }

    std::printf("test_spec_devirt: %d checks, %d fails\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
