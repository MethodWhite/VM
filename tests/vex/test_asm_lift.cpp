/*
 * VestaVM - Máquina Virtual Distribuida
 *
 * Copyright © 2026 David López.T (DesmonHak) (Castilla y León, ES)
 * Licencia VMProject
 */

/**
 * @file test_asm_lift.cpp
 * @brief Pruebas del reconocimiento de patrones atomicos del inline asm
 *        (port de asm_lift: lock cmpxchg -> ATOMIC_CAS, lock xadd -> ATOMIC_ADD).
 */

#include "vex/asm/asm_lift.h"

#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("  [FAIL] %s\n", msg); ++g_fail; } \
    else { std::printf("  [OK]   %s\n", msg); } \
} while (0)

int main() {
    std::printf("=== test_asm_lift ===\n");

    // lock cmpxchg [rax], rbx -> ATOMIC_CAS
    vex::AsmLift cas = vex::asm_lift_detect(
        vex::instr_db::Isa::X86, "lock cmpxchg [rax], rbx\n");
    CHECK(cas.op == vex::AsmLiftOp::AtomicCas, "lock cmpxchg -> AtomicCas");
    CHECK(cas.addr_reg == "rax", "cas addr = rax");
    CHECK(cas.des_reg == "rbx", "cas desired = rbx");
    CHECK(cas.exp_reg == "rax", "cas expected implicito = rax");

    // lock xadd [rcx], rdx -> ATOMIC_ADD
    vex::AsmLift add = vex::asm_lift_detect(
        vex::instr_db::Isa::X86, "lock xadd [rcx], rdx\n");
    CHECK(add.op == vex::AsmLiftOp::AtomicAdd, "lock xadd -> AtomicAdd");
    CHECK(add.addr_reg == "rcx", "add addr = rcx");
    CHECK(add.des_reg == "rdx", "add delta = rdx");

    // sin lock -> no se lifta (no es atomico cross-core)
    vex::AsmLift nolock = vex::asm_lift_detect(
        vex::instr_db::Isa::X86, "cmpxchg [rax], rbx\n");
    CHECK(nolock.op == vex::AsmLiftOp::None, "cmpxchg sin lock -> None");

    // mnemonico no reconocido -> None
    vex::AsmLift bad = vex::asm_lift_detect(
        vex::instr_db::Isa::X86, "mov rax, 5\n");
    CHECK(bad.op == vex::AsmLiftOp::None, "mov -> None (no es atomico)");

    std::printf("=== Resultado: %s ===\n", g_fail == 0 ? "TODOS PASS" : "HAY FALLOS");
    return g_fail;
}
