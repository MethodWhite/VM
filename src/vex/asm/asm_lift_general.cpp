/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file vx/asm/asm_lift_general.cpp
 * @brief DISPATCHER por ISA del lift general de asm inline a IR.  Este fichero
 *        es NEUTRO: NO contiene codigo especifico de ninguna arquitectura.
 *        Cada ISA aporta su frontend (mnemonicos + registros + direccionamiento)
 *        en su propio fichero -- x86 en asm_lift_x86.cpp -- reusando el core
 *        neutro (asm_lift_core.h: emisores de IR genericos, register-file y el
 *        driver del CFG del asm a IR-CFG).  Anadir arm64/arm32/riscv = anadir su
 *        lift_<isa> + una rama en este switch; el core NO cambia.
 */

#include "vex/asm/asm_lift_general.h"

#include "vex/asm/asm_lift_x86.h" // lift_x86 (frontend x86)

namespace vex {

bool asm_lift_general(
    ir::IrFunction &fn, uint32_t block, instr_db::Isa isa,
    const std::string &body,
    const std::unordered_map<std::string, AsmBoundReg> &bound, uint32_t line,
    uint32_t *out_exit) {
    switch (isa) {
    case instr_db::Isa::X86:
        return lift_x86(fn, block, body, bound, line, out_exit);
    default:
        // ISA sin frontend de lift aun -> el llamador cae a ASM_MICRO/INLINE_ASM.
        return false;
    }
}

} // namespace vex
