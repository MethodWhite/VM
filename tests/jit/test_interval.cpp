/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/jit/test_interval.cpp
 * @brief Tests del constructor de live intervals (Phase D.7, commit 2).
 *
 * Construye MFunctions de juguete a mano (forma vreg) y valida los rangos,
 * usos, posiciones de CALL y deteccion de codigo muerto.  Compila standalone
 * contra interval.cpp + headers (sin dependencias del runtime).
 */

#include "jit/interval.h"
#include "jit/target_reginfo.h"

#include <cstdio>

using namespace jit;

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        ++g_checks;                                                         \
        if (!(cond)) {                                                      \
            ++g_fails;                                                      \
            std::printf("  FAIL: %s  (linea %d)\n", (msg), __LINE__);       \
        }                                                                   \
    } while (0)

static MOperand imm(int32_t v) { return MOperand::make_imm32(v); }

/* ---- Test 1: straight-line def/use + codigo muerto -------------------- */
static void test_straight_line() {
    std::printf("[test] straight-line def/use + dead\n");
    MFunction mf;
    mf.name = "t1";
    MOperand v0 = mf.new_vreg(RegClass::GP);  // id 0
    MOperand v1 = mf.new_vreg(RegClass::GP);  // id 1
    MOperand v2 = mf.new_vreg(RegClass::GP);  // id 2
    MOperand v3 = mf.new_vreg(RegClass::GP);  // id 3 (def pero nunca usado)
    MBlock b;
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v0, imm(5)));
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v1, imm(7)));
    b.instrs.push_back(MInstr::make_binary(MOp::ADD, v2, v0, v1));
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v3, v2));
    b.instrs.push_back(MInstr::make_ret());
    mf.blocks.push_back(std::move(b));

    IntervalResult r = build_intervals(mf, target_x86_64_vm_abi());

    CHECK(r.max_pos == 10, "max_pos = 2*5 instrs = 10");
    CHECK(r.intervals.size() == 4, "4 intervals");
    /* v0: def@1, usado@4 -> [1,5) */
    CHECK(r.intervals[0].start() == 1, "v0.start == 1");
    CHECK(r.intervals[0].end()   == 5, "v0.end == 5");
    CHECK(r.intervals[0].covers(4), "v0 vivo en su uso (pos 4)");
    CHECK(!r.intervals[0].covers(0), "v0 NO vivo antes de su def (pos 0)");
    CHECK(!r.intervals[0].covers(5), "v0 NO vivo en pos 5 (def de v2)");
    /* v1: def@3 -> [3,5) */
    CHECK(r.intervals[1].start() == 3, "v1.start == 3");
    CHECK(r.intervals[1].end()   == 5, "v1.end == 5");
    /* v2: def@5, usado@6 -> [5,7) */
    CHECK(r.intervals[2].start() == 5, "v2.start == 5");
    CHECK(r.intervals[2].end()   == 7, "v2.end == 7");
    /* v3: definido (instr3, def_pos=7) pero nunca usado.  Con el modelo
     * per-bloque recibe un rango MINIMO [7,8) -- el def ocupa un reg
     * brevemente (correcto/seguro: sin rango el rewrite no podria resolverlo).
     * No interfiere con v0/v1 (que ya murieron en pos 5). */
    CHECK(r.intervals[3].start() == 7, "v3 (def muerto) nace en su def (7)");
    CHECK(r.intervals[3].end()   == 8, "v3 (def muerto) rango minimo [7,8)");
}

/* ---- Test 2: valor vivo a traves de un CALL --------------------------- */
static void test_live_across_call() {
    std::printf("[test] live across CALL\n");
    MFunction mf;
    mf.name = "t2";
    MOperand v0 = mf.new_vreg(RegClass::GP);  // id 0
    MOperand v1 = mf.new_vreg(RegClass::GP);  // id 1
    MBlock b;
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v0, imm(5)));   // 0
    b.instrs.push_back(MInstr::make_call_label(0));                 // 1 (CALL)
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v1, v0));       // 2 (usa v0)
    b.instrs.push_back(MInstr::make_ret());                         // 3
    mf.blocks.push_back(std::move(b));

    IntervalResult r = build_intervals(mf, target_x86_64_vm_abi());

    CHECK(r.call_positions.size() == 1, "1 call position");
    CHECK(!r.call_positions.empty() && r.call_positions[0] == 2,
          "call en use_pos 2 (instr 1)");
    /* v0 def@1, usado@4 -> [1,5); el CALL esta en pos 2 -> v0 cruza el call. */
    CHECK(r.intervals[0].covers(2), "v0 vivo a traves del CALL (pos 2)");
}

/* ---- Test 3: liveness cross-block (def en B0, uso en B1) --------------- */
static void test_cross_block() {
    std::printf("[test] cross-block liveness\n");
    MFunction mf;
    mf.name = "t3";
    MOperand v0 = mf.new_vreg(RegClass::GP);  // id 0
    MOperand v1 = mf.new_vreg(RegClass::GP);  // id 1
    MLabelId l1 = mf.new_label();

    MBlock b0;
    b0.instrs.push_back(MInstr::make_unary(MOp::MOV, v0, imm(5)));  // 0
    b0.instrs.push_back(MInstr::make_jmp(l1));                       // 1
    b0.succ_a = 1;                                                   // -> B1
    mf.blocks.push_back(std::move(b0));

    MBlock b1;
    b1.label_id = l1;
    b1.instrs.push_back(MInstr::make_unary(MOp::MOV, v1, v0));       // 2 (usa v0)
    b1.instrs.push_back(MInstr::make_ret());                        // 3
    mf.blocks.push_back(std::move(b1));

    IntervalResult r = build_intervals(mf, target_x86_64_vm_abi());

    /* v0 def@1 en B0, usado@4 en B1 -> debe estar vivo a traves del salto:
     * rango coalescido [1,5). */
    CHECK(r.intervals[0].start() == 1, "v0.start == 1 (def en B0)");
    CHECK(r.intervals[0].end()   == 5, "v0.end == 5 (uso en B1)");
    CHECK(r.intervals[0].covers(2), "v0 vivo en el JMP (pos 2)");
    CHECK(r.intervals[0].covers(4), "v0 vivo en su uso cross-block (pos 4)");
    /* El rango debe ser contiguo (un solo rango tras coalescer B0+B1). */
    CHECK(r.intervals[0].ranges.size() == 1, "v0 rango contiguo (coalescido)");
    /* v1 = MOV v1,v0 (instr2, def_pos=5): definido pero nunca usado ->
     * rango minimo [5,6) con el modelo per-bloque (correcto/seguro). */
    CHECK(!r.intervals[1].empty() && r.intervals[1].start() == 5,
          "v1 (def muerto) rango minimo desde su def (5)");
}

/* ---- Test 4: interferencia (overlap) ---------------------------------- */
static void test_overlap() {
    std::printf("[test] overlap / interferencia\n");
    MFunction mf;
    mf.name = "t4";
    MOperand v0 = mf.new_vreg(RegClass::GP);  // 0
    MOperand v1 = mf.new_vreg(RegClass::GP);  // 1
    MOperand v2 = mf.new_vreg(RegClass::GP);  // 2
    MOperand v3 = mf.new_vreg(RegClass::GP);  // 3
    MBlock b;
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v0, imm(1)));   // 0
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v1, imm(2)));   // 1
    b.instrs.push_back(MInstr::make_binary(MOp::ADD, v2, v0, v1));  // 2 usa v0,v1; def v2
    b.instrs.push_back(MInstr::make_unary(MOp::MOV, v3, v2));       // 3 usa v2; def v3
    b.instrs.push_back(MInstr::make_ret());
    mf.blocks.push_back(std::move(b));
    IntervalResult r = build_intervals(mf, target_x86_64_vm_abi());
    /* v0=[1,5) v1=[3,5) v2=[5,7): v0 y v1 vivos a la vez en el ADD -> interfieren. */
    CHECK(r.intervals[0].first_overlap_from(r.intervals[1], 0) != UINT32_MAX,
          "v0 y v1 interfieren (ambos vivos en el ADD)");
    /* v0 muere en pos 5 (exclusive) y v2 nace en pos 5 -> NO interfieren
     * (gracias al desfase use/def: el dst puede reusar el reg del src). */
    CHECK(r.intervals[0].first_overlap_from(r.intervals[2], 0) == UINT32_MAX,
          "v0 y v2 NO interfieren (v2 reusa el reg de v0)");
}

int main() {
    std::printf("=== test_interval (Phase D.7 commit 2) ===\n");
    test_straight_line();
    test_live_across_call();
    test_cross_block();
    test_overlap();
    std::printf("--- %d checks, %d fallos ---\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
