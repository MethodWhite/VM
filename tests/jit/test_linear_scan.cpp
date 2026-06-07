/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/jit/test_linear_scan.cpp
 * @brief Tests del register allocator linear-scan (Phase D.7, commit 3).
 *
 * Construye MFunctions de juguete, computa intervals y valida la asignacion:
 * baja presion (todos en regs, preferencia caller-saved), valor cross-call
 * (forzado a callee-saved), alta presion (spilling con un target diminuto), y
 * reuso de registro entre vregs de vida disjunta.  Standalone (sin runtime).
 */

#include "jit/interval.h"
#include "jit/linear_scan.h"
#include "jit/target_reginfo.h"

#include <cstdio>

using namespace jit;

static int g_checks = 0;
static int g_fails  = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        ++g_checks;                                                         \
        if (!(cond)) { ++g_fails;                                           \
            std::printf("  FAIL: %s  (linea %d)\n", (msg), __LINE__); }     \
    } while (0)

static MOperand imm(int32_t v) { return MOperand::make_imm32(v); }

/** @brief Target diminuto: 2 caller-saved (R8,R9) + 1 callee (R12). */
static TargetRegInfo tiny_target() {
    TargetRegInfo t;
    t.pointer_size = 8;
    t.is_two_address = true;
    const size_t GP = static_cast<size_t>(RegClass::GP);
    t.caller_saved[GP] = { reg_id(MReg::R8), reg_id(MReg::R9) };
    t.callee_saved[GP] = { reg_id(MReg::R12) };
    t.allocatable[GP]  = { reg_id(MReg::R8), reg_id(MReg::R9), reg_id(MReg::R12) };
    t.ret_reg[GP] = reg_id(MReg::RAX);
    return t;
}

static bool is_caller_saved_gp(const TargetRegInfo &t, uint8_t r) {
    for (uint8_t x : t.caller_saved[(size_t)RegClass::GP]) if (x == r) return true;
    return false;
}

/* ---- Test 1: baja presion -> todos en reg, preferencia caller-saved ---- */
static void test_low_pressure() {
    std::printf("[test] baja presion (sin spill)\n");
    MFunction mf;
    MOperand v0 = mf.new_vreg(RegClass::GP);
    MOperand v1 = mf.new_vreg(RegClass::GP);
    MOperand v2 = mf.new_vreg(RegClass::GP);
    MOperand v3 = mf.new_vreg(RegClass::GP);
    MBlock b;
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v0, imm(1)));
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v1, imm(2)));
    b.instrs.push_back(MInstr::make_binary(MOp::ADD, v2, v0, v1));
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v3, v2));
    b.instrs.push_back(MInstr::make_ret());
    mf.blocks.push_back(std::move(b));

    const TargetRegInfo &tri = target_x86_64_vm_abi();
    RegAlloc ra = linear_scan(build_intervals(mf, tri), tri);

    CHECK(ra.num_spill_slots == 0, "sin spills");
    CHECK(ra.in_reg(0), "v0 en reg");
    CHECK(ra.in_reg(1), "v1 en reg");
    CHECK(ra.in_reg(2), "v2 en reg");
    /* Sin calls -> debe preferir caller-saved. */
    CHECK(is_caller_saved_gp(tri, ra.reg_of(0)), "v0 en caller-saved (sin call)");
    CHECK(ra.callee_saved_used.empty(), "no usa callee-saved sin calls");
    /* v0 y v1 vivos a la vez -> regs distintos. */
    CHECK(ra.reg_of(0) != ra.reg_of(1), "v0 y v1 en regs distintos");
}

/* ---- Test 2: valor vivo a traves de CALL -> callee-saved -------------- */
static void test_cross_call() {
    std::printf("[test] cross-call -> callee-saved\n");
    MFunction mf;
    MOperand v0 = mf.new_vreg(RegClass::GP);
    MOperand v1 = mf.new_vreg(RegClass::GP);
    MBlock b;
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v0, imm(5)));   // def v0
    b.instrs.push_back(MInstr::make_call_label(0));                 // CALL (clobber)
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v1, v0));       // usa v0 tras call
    b.instrs.push_back(MInstr::make_ret());
    mf.blocks.push_back(std::move(b));

    const TargetRegInfo &tri = target_x86_64_vm_abi();
    RegAlloc ra = linear_scan(build_intervals(mf, tri), tri);

    CHECK(ra.in_reg(0), "v0 en reg (no spill: hay callee libres)");
    const uint8_t r = ra.reg_of(0);
    CHECK(tri.is_callee_saved(RegClass::GP, r),
          "v0 (cross-call) en callee-saved (R12-R15)");
    bool recorded = false;
    for (uint8_t x : ra.callee_saved_used) if (x == r) recorded = true;
    CHECK(recorded, "el reg callee-saved usado quedo registrado para push/pop");
}

/* ---- Test 3: alta presion -> spilling (target diminuto) --------------- */
static void test_spill() {
    std::printf("[test] alta presion -> spill\n");
    MFunction mf;
    MOperand v0 = mf.new_vreg(RegClass::GP);
    MOperand v1 = mf.new_vreg(RegClass::GP);
    MOperand v2 = mf.new_vreg(RegClass::GP);
    MOperand v3 = mf.new_vreg(RegClass::GP);
    MOperand t0 = mf.new_vreg(RegClass::GP);
    MOperand t1 = mf.new_vreg(RegClass::GP);
    MOperand t2 = mf.new_vreg(RegClass::GP);
    MBlock b;
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v0, imm(1)));   // 0
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v1, imm(2)));   // 1
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v2, imm(3)));   // 2
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v3, imm(4)));   // 3  <- 4 vivos
    b.instrs.push_back(MInstr::make_binary(MOp::ADD, t0, v0, v1));  // 4
    b.instrs.push_back(MInstr::make_binary(MOp::ADD, t1, v2, v3));  // 5
    b.instrs.push_back(MInstr::make_binary(MOp::ADD, t2, t0, t1));  // 6
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v0, t2));       // 7 (usa t2; v0 reciclado)
    b.instrs.push_back(MInstr::make_ret());
    mf.blocks.push_back(std::move(b));

    /* Target con SOLO 3 GP asignables: 4 valores vivos a la vez -> 1 spill. */
    TargetRegInfo tri = tiny_target();
    RegAlloc ra = linear_scan(build_intervals(mf, tri), tri);

    CHECK(ra.num_spill_slots >= 1, "al menos 1 spill con 4 vivos y 3 regs");
    /* Cuenta cuantos de v0..v3 quedaron en reg (debe ser <= 3). */
    int in_reg = 0;
    for (uint32_t v = 0; v < 4; ++v) if (ra.in_reg(v)) ++in_reg;
    CHECK(in_reg <= 3, "no mas de 3 valores simultaneos en reg");
    CHECK(ra.callee_saved_used.size() >= 1, "uso R12 (callee) bajo presion");
}

/* ---- Test 4: reuso de reg entre vregs de vida disjunta ---------------- */
static void test_reuse() {
    std::printf("[test] reuso de reg (vidas disjuntas)\n");
    MFunction mf;
    MOperand v0 = mf.new_vreg(RegClass::GP);
    MOperand v1 = mf.new_vreg(RegClass::GP);
    MOperand v2 = mf.new_vreg(RegClass::GP);
    MBlock b;
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v0, imm(1)));   // def v0
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v1, v0));       // usa v0 (muere), def v1
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v2, v1));       // usa v1 (muere), def v2
    /* dar un uso a v2 para que viva: lo movemos a un reg y RET. */
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v0, v2));       // usa v2
    b.instrs.push_back(MInstr::make_ret());
    mf.blocks.push_back(std::move(b));

    const TargetRegInfo &tri = target_x86_64_vm_abi();
    RegAlloc ra = linear_scan(build_intervals(mf, tri), tri);

    CHECK(ra.num_spill_slots == 0, "sin spill (vidas disjuntas)");
    CHECK(ra.in_reg(0) && ra.in_reg(1), "v0 y v1 en reg");
    /* v0 muere antes de que v1 nazca -> pueden compartir el mismo fisico. */
    CHECK(ra.reg_of(0) == ra.reg_of(1), "v0 y v1 reusan el mismo reg");
}

int main() {
    std::printf("=== test_linear_scan (Phase D.7 commit 3) ===\n");
    test_low_pressure();
    test_cross_call();
    test_spill();
    test_reuse();
    std::printf("--- %d checks, %d fallos ---\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
