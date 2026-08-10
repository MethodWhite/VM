/*
 * VestaVM - Máquina Virtual Distribuida
 *
 * Copyright © 2026 David López.T (DesmonHak) (Castilla y León, ES)
 * Licencia VMProject
 */

/**
 * @file test_asm_lift_micro.cpp
 * @brief Pruebas del lifting micro de inline asm (port de asm_lift_micro).
 *
 * Verifica que un bloque asm se levanta a IR ASM_MICRO (o a ops tipadas)
 * con el pool asm_micros poblado, y que el emisor los materializa.
 */

#include "vex/asm/asm_lift_micro.h"
#include "ir/ssa_ir.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("  [FAIL] %s\n", msg); ++g_fail; } \
    else { std::printf("  [OK]   %s\n", msg); } \
} while (0)

int main() {
    std::printf("=== test_asm_lift_micro ===\n");

    ir::IrFunction fn;
    fn.name = "test";
    fn.values.resize(8);
    ir::IrBlock bb;
    bb.id = 0;
    fn.blocks.push_back(std::move(bb));

    // Bloque asm con una instruccion sin operandos (path sin-ops del
    // lifter micro: mfence se modela como ASM_MICRO con su form_id).
    const bool ok = vex::asm_lift_micro(
        fn, 0, vex::instr_db::Isa::X86, "mfence\n", 1, {});
    CHECK(ok, "asm_lift_micro acepta mfence");

    // Debe haber ASM_MICRO en el bloque y el pool poblado.
    bool has_micro = false;
    for (const auto &ins : fn.blocks[0].instrs) {
        if (ins.op == ir::IrOp::ASM_MICRO) has_micro = true;
    }
    CHECK(has_micro, "se emitio ASM_MICRO en el IR");
    CHECK(!fn.asm_micros.empty(), "el pool asm_micros se pobló");
    if (!fn.asm_micros.empty()) {
        const auto &am = fn.asm_micros[0];
        CHECK(am.isa == (uint8_t)vex::instr_db::Isa::X86, "isa = x86");
        CHECK(!am.tmpl.empty(), "tmpl no vacio");
        std::printf("  [info] tmpl: %s\n", am.tmpl.c_str());
    }

    std::printf("=== Resultado: %s ===\n", g_fail == 0 ? "TODOS PASS" : "HAY FALLOS");
    return g_fail;
}
