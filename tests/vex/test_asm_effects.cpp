/*
 * VestaVM - Máquina Virtual Distribuida
 *
 * Copyright © 2026 David López.T (DesmonHak) (Castilla y León, ES)
 * Licencia VMProject
 */

/**
 * @file test_asm_effects.cpp
 * @brief Pruebas del modulo de efectos de inline asm (port de asm_effects.h).
 *
 * Verifica:
 *  - asm_canonical_reg: canonicaliza eax/r8d/xmm3 a su registro fisico.
 *  - asm_effects_for: tabla por mnemonico (add escribe op1, rdtsc escribe
 *    rax:rdx implicitos, mnemonico desconocido -> known=false).
 *  - asm_infer_clobbers: infiere clobbers de un cuerpo NASM y excluye los
 *    registros ligados por register().
 */

#include "vex/asm/asm_effects.h"

#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("  [FAIL] %s\n", msg); ++g_fail; } \
    else { std::printf("  [OK]   %s\n", msg); } \
} while (0)

int main() {
    std::printf("=== test_asm_effects ===\n");

    // Canonicalizacion de registros
    CHECK(vex::asm_canonical_reg("eax") == "rax", "eax -> rax");
    CHECK(vex::asm_canonical_reg("r8d") == "r8", "r8d -> r8");
    CHECK(vex::asm_canonical_reg("xmm3") == "v3", "xmm3 -> v3 (SIMD)");
    CHECK(vex::asm_canonical_reg("RAX") == "rax", "RAX -> rax (case-insensitive)");
    CHECK(vex::asm_canonical_reg("bogus") == "", "bogus -> vacio");

    // Tabla de efectos
    vex::AsmEffects add = vex::asm_effects_for("add", "x86_64");
    CHECK(add.known, "add esta en la tabla");
    CHECK(add.touches_flags, "add toca flags");
    CHECK((add.operand_write_mask & 0x1) != 0, "add escribe el 1er operando");

    vex::AsmEffects rdtsc = vex::asm_effects_for("rdtsc", "x86_64");
    CHECK(rdtsc.known, "rdtsc esta en la tabla");
    CHECK(rdtsc.implicit_write.size() >= 2, "rdtsc escribe rax:rdx implicitos");

    vex::AsmEffects unknown = vex::asm_effects_for("frobnicate", "x86_64");
    CHECK(!unknown.known, "mnemonico desconocido -> known=false");

    // Inferencia de clobbers
    auto inf = vex::asm_infer_clobbers(
        "mov rax, [rbx]\nadd rax, 5\n", {"rbx"});
    CHECK(inf.clobber_memory, "acceso a [rbx] marca clobber_memory");
    CHECK(inf.clobber_flags, "add toca flags -> clobber_flags");
    // rbx esta ligado por register() -> no debe aparecer como clobber
    bool has_rbx = false;
    for (auto &c : inf.clobber_regs)
        if (c == "rbx") has_rbx = true;
    CHECK(!has_rbx, "rbx ligado por register() no es clobber");

    // call marca clobber de caller-saved
    auto inf_call = vex::asm_infer_clobbers("call some_fn\n", {});
    CHECK(inf_call.clobber_regs.size() > 0, "call -> clobber caller-saved");

    std::printf("=== Resultado: %s ===\n", g_fail == 0 ? "TODOS PASS" : "HAY FALLOS");
    return g_fail;
}
