/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/jit/test_vreg_call.cpp
 * @brief Test de CALL con argumentos en el path vreg (Phase D.7, commit 5b).
 *
 * Construye MachineIR vreg con ARG + CALL_ABS a una funcion nativa de prueba,
 * pasa por el pipeline (intervals -> linear-scan -> rewrite con shadow space +
 * parallel-move de args) y EJECUTA, validando que la llamada con marshalling
 * de argumentos produce el resultado correcto.
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

/** @brief Funcion nativa de prueba (convencion C host).  a*10 + b. */
extern "C" int64_t vex_test_addmul(int64_t a, int64_t b) {
    return a * 10 + b;
}
/** @brief 3 args: a + b*2 + c*3. */
extern "C" int64_t vex_test_three(int64_t a, int64_t b, int64_t c) {
    return a + b * 2 + c * 3;
}

static uint8_t *compile(CodeCache &cc, MFunction &pf) {
    X86Encoder enc;
    std::vector<uint8_t> bytes;
    if (enc.encode(pf, bytes) == 0 || bytes.empty()) return nullptr;
    uint8_t *code = cc.alloc(bytes.size(), 16);
    if (!code) return nullptr;
    std::memcpy(code, bytes.data(), bytes.size());
    cc.commit(code, bytes.size());
    return code;
}

/* ---- Test 1: CALL de 2 args -> addmul(5,7) = 57 ---------------------- */
static void test_call_two() {
    std::printf("[call] addmul(5,7) -> 57\n");
    MFunction mf;
    MOperand v0 = mf.new_vreg(RegClass::GP);
    MOperand v1 = mf.new_vreg(RegClass::GP);
    MOperand res = mf.new_vreg(RegClass::GP);
    const uint32_t addr = mf.intern_imm64(
        reinterpret_cast<uint64_t>(reinterpret_cast<void *>(&vex_test_addmul)));
    MBlock b;
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v0, imm(5)));
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v1, imm(7)));
    b.instrs.push_back(MInstr::make_arg(0, v0));
    b.instrs.push_back(MInstr::make_arg(1, v1));
    b.instrs.push_back(MInstr::make_call_abs(addr));
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, res, pr(MReg::RAX)));  // capturar
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, pr(MReg::RAX), res));  // return
    b.instrs.push_back(MInstr::make_ret());
    mf.blocks.push_back(std::move(b));

    const TargetRegInfo &tri = target_x86_64_vm_abi();
    RegAlloc ra = linear_scan(build_intervals(mf, tri), tri);
    MFunction pf = rewrite_to_physical(mf, ra, tri, AbiKind::HOST_LEAF);
    CodeCache cc;
    uint8_t *code = compile(cc, pf);
    CHECK(code != nullptr, "encode ok (2-arg call)");
    if (code) {
        const int64_t r = reinterpret_cast<int64_t (*)()>(code)();
        CHECK(r == 57, "addmul(5,7)==57");
        if (r != 57) std::printf("    obtuvo %lld\n", (long long)r);
    }
}

/* ---- Test 2: CALL de 3 args -> three(1,2,3) = 1+4+9 = 14 ------------- */
static void test_call_three() {
    std::printf("[call] three(1,2,3) -> 14\n");
    MFunction mf;
    MOperand v0 = mf.new_vreg(RegClass::GP);
    MOperand v1 = mf.new_vreg(RegClass::GP);
    MOperand v2 = mf.new_vreg(RegClass::GP);
    const uint32_t addr = mf.intern_imm64(
        reinterpret_cast<uint64_t>(reinterpret_cast<void *>(&vex_test_three)));
    MBlock b;
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v0, imm(1)));
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v1, imm(2)));
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v2, imm(3)));
    b.instrs.push_back(MInstr::make_arg(0, v0));
    b.instrs.push_back(MInstr::make_arg(1, v1));
    b.instrs.push_back(MInstr::make_arg(2, v2));
    b.instrs.push_back(MInstr::make_call_abs(addr));
    b.instrs.push_back(MInstr::make_ret());  // resultado ya en RAX
    mf.blocks.push_back(std::move(b));

    const TargetRegInfo &tri = target_x86_64_vm_abi();
    RegAlloc ra = linear_scan(build_intervals(mf, tri), tri);
    MFunction pf = rewrite_to_physical(mf, ra, tri, AbiKind::HOST_LEAF);
    CodeCache cc;
    uint8_t *code = compile(cc, pf);
    CHECK(code != nullptr, "encode ok (3-arg call)");
    if (code) {
        const int64_t r = reinterpret_cast<int64_t (*)()>(code)();
        CHECK(r == 14, "three(1,2,3)==14");
        if (r != 14) std::printf("    obtuvo %lld\n", (long long)r);
    }
}

int main() {
    std::setbuf(stdout, nullptr);
    std::printf("=== test_vreg_call (Phase D.7 commit 5b, CALL) ===\n");
    test_call_two();
    test_call_three();
    std::printf("--- %d checks, %d fallos ---\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
