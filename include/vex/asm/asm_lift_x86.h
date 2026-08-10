/* * VestaVM -- core NEUTRO del lift de asm a IR-CFG (compartido por ISAs).
 * Copyright (C) 2026 David Lopez.T (DesmonHak).  GPLv2 + excepcion de runtime. */

/** @file vx/asm/asm_lift_x86.h
 *  @brief Frontend x86/x86-64 del lift de asm: reconoce el subset (mnemonicos,
 *  registros, direccionamiento x86) y lo baja al IR NEUTRO via el core
 *  (@ref asm_lift_core.h).  Anadir un ISA = anadir su lift_<isa>; el core y el
 *  dispatcher no cambian. */
#ifndef VESTA_VX_ASM_ASM_LIFT_X86_H
#define VESTA_VX_ASM_ASM_LIFT_X86_H

#include "vex/asm/asm_lift_general.h" // AsmBoundReg
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ir { struct IrFunction; }

namespace vex {
/** @brief Lifta el bloque asm x86 @p body a IR (recto o con ramas/bucles).  Ver
 *  asm_lift_general para la semantica de @p bound / @p out_exit. */
bool lift_x86(ir::IrFunction &fn, uint32_t block, const std::string &body,
              const std::unordered_map<std::string, AsmBoundReg> &bound,
              uint32_t line, uint32_t *out_exit);
} // namespace vex

#endif // VESTA_VX_ASM_ASM_LIFT_X86_H
