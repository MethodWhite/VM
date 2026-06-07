/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/jit/test_regalloc_e2e.cpp
 * @brief Test END-TO-END del path vreg (Phase D.7, commit 4a).
 *
 * Construye una funcion en MachineIR de registros virtuales, computa
 * intervals, asigna con linear-scan, reescribe a fisicos (rewrite_to_physical)
 * con two-address legalization + spill load/store + prologue/epilogue, encodea
 * con el X86Encoder, la carga en el CodeCache y la EJECUTA como funcion C
 * nativa, comprobando el resultado numerico.  Primer vreg -> fisico ->
 * codigo nativo que corre de verdad.
 */

#include "jit/code_cache.h"
#include "jit/interval.h"
#include "jit/linear_scan.h"
#include "jit/machine_ir.h"
#include "jit/regalloc_rewrite.h"
#include "jit/target_reginfo.h"
#include "jit/x86_encoder.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace jit;

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, msg)                                                    \
    do { ++g_checks; if (!(cond)) { ++g_fails;                              \
        std::printf("  FAIL: %s  (linea %d)\n", (msg), __LINE__); } } while (0)

static MOperand imm(int32_t v) { return MOperand::make_imm32(v); }
static MOperand pr(MReg r)     { return MOperand::make_reg(r, 8); }

/** @brief Compila una MFunction FISICA a nativo y devuelve el puntero. */
static uint8_t *compile(CodeCache &cc, MFunction &pf) {
    X86Encoder enc;
    std::vector<uint8_t> bytes;
    const size_t n = enc.encode(pf, bytes);
    if (n == 0 || bytes.empty()) return nullptr;
    uint8_t *code = cc.alloc(bytes.size(), 16);
    if (!code) return nullptr;
    std::memcpy(code, bytes.data(), bytes.size());
    cc.commit(code, bytes.size());
    return code;
}

/** @brief Target diminuto: 3 GP asignables (RAX,RCX,RDX) + scratch R10/R11. */
static TargetRegInfo small_target() {
    TargetRegInfo t;
    t.pointer_size = 8;
    t.is_two_address = true;
    const size_t GP = static_cast<size_t>(RegClass::GP);
    t.caller_saved[GP] = { reg_id(MReg::RAX), reg_id(MReg::RCX), reg_id(MReg::RDX) };
    t.callee_saved[GP] = {};
    t.allocatable[GP]  = { reg_id(MReg::RAX), reg_id(MReg::RCX), reg_id(MReg::RDX) };
    t.scratch[GP]      = { reg_id(MReg::R10), reg_id(MReg::R11) };
    t.ret_reg[GP] = reg_id(MReg::RAX);
    return t;
}

/* ---- Test A: sin spill (target completo) -> (40 + 2) = 42 ------------- */
static void test_no_spill() {
    std::printf("[e2e] sin spill -> 42\n");
    MFunction mf;
    MOperand v0 = mf.new_vreg(RegClass::GP);
    MOperand v1 = mf.new_vreg(RegClass::GP);
    MOperand t  = mf.new_vreg(RegClass::GP);
    MBlock b;
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v0, imm(40)));
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v1, imm(2)));
    b.instrs.push_back(MInstr::make_binary(MOp::ADD, t, v0, v1));   // t = 40+2
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, pr(MReg::RAX), t)); // ret value
    b.instrs.push_back(MInstr::make_ret());
    mf.blocks.push_back(std::move(b));

    const TargetRegInfo &tri = target_x86_64_vm_abi();
    RegAlloc ra = linear_scan(build_intervals(mf, tri), tri);
    MFunction pf = rewrite_to_physical(mf, ra, tri);

    CHECK(ra.num_spill_slots == 0, "sin spill con target completo");
    CodeCache cc;
    uint8_t *code = compile(cc, pf);
    CHECK(code != nullptr, "encode + commit OK (A)");
    if (code) {
        using Fn = int64_t (*)();
        const int64_t r = reinterpret_cast<Fn>(code)();
        CHECK(r == 42, "resultado A == 42");
        if (r != 42) std::printf("    obtuvo %lld\n", (long long)r);
    }
}

/* ---- Test B: CON spill (target diminuto) ------------------------------ *
 * v0..v3 = 10,20,30,40 ; t0=v0+v1 ; t1=v2+v3 ; t2=t0+t1 ; t3=t2-v0
 * resultado = (10+20)+(30+40) - 10 = 100 - 10 = 90.  4 vivos / 3 regs -> spill. */
static void test_with_spill() {
    std::printf("[e2e] con spill -> 90\n");
    MFunction mf;
    MOperand v0 = mf.new_vreg(RegClass::GP);
    MOperand v1 = mf.new_vreg(RegClass::GP);
    MOperand v2 = mf.new_vreg(RegClass::GP);
    MOperand v3 = mf.new_vreg(RegClass::GP);
    MOperand t0 = mf.new_vreg(RegClass::GP);
    MOperand t1 = mf.new_vreg(RegClass::GP);
    MOperand t2 = mf.new_vreg(RegClass::GP);
    MOperand t3 = mf.new_vreg(RegClass::GP);
    MBlock b;
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v0, imm(10)));
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v1, imm(20)));
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v2, imm(30)));
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v3, imm(40)));
    b.instrs.push_back(MInstr::make_binary(MOp::ADD, t0, v0, v1));  // 30
    b.instrs.push_back(MInstr::make_binary(MOp::ADD, t1, v2, v3));  // 70
    b.instrs.push_back(MInstr::make_binary(MOp::ADD, t2, t0, t1));  // 100
    b.instrs.push_back(MInstr::make_binary(MOp::SUB, t3, t2, v0));  // 100-10=90
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, pr(MReg::RAX), t3));
    b.instrs.push_back(MInstr::make_ret());
    mf.blocks.push_back(std::move(b));

    TargetRegInfo tri = small_target();
    RegAlloc ra = linear_scan(build_intervals(mf, tri), tri);
    MFunction pf = rewrite_to_physical(mf, ra, tri);

    CHECK(ra.num_spill_slots >= 1, "hubo spill (4 vivos, 3 regs)");
    CodeCache cc;
    uint8_t *code = compile(cc, pf);
    CHECK(code != nullptr, "encode + commit OK (B)");
    if (code) {
        using Fn = int64_t (*)();
        const int64_t r = reinterpret_cast<Fn>(code)();
        CHECK(r == 90, "resultado B == 90 (con spill load/store)");
        if (r != 90) std::printf("    obtuvo %lld\n", (long long)r);
    }
}

int main() {
    std::printf("=== test_regalloc_e2e (Phase D.7 commit 4a) ===\n");
    test_no_spill();
    test_with_spill();
    std::printf("--- %d checks, %d fallos ---\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
