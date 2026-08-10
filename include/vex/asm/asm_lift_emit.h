/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file asm_lift_emit.h
 * @brief Emision del IR tipado resultante de liftar un bloque de inline asm.
 *
 * Separa la EMISION (que necesita el IR) del RECONOCIMIENTO (funcion pura en
 * vx/asm_lift.h), para mantener el lowering minimo.  Dado el cuerpo de un bloque
 * @c asm y el mapa de registros canonicos -> slot ALLOCA de sus @c register()
 * bindings, si el bloque encaja con un patron atomico (@ref asm_lift_detect),
 * emite las instrucciones tipadas (LOAD de operandos + ATOMIC_CAS/ADD + STORE
 * del resultado) directamente en el bloque IR y devuelve true.  En otro caso no
 * toca el IR y devuelve false (el lowering sigue con la ruta @c INLINE_ASM).
 */

#ifndef VX_ASM_LIFT_EMIT_H
#define VX_ASM_LIFT_EMIT_H

#include <cstdint>
#include <string>
#include <unordered_map>

#include "ir/ssa_ir.h"
#include "vex/asm/asm_lift.h"

namespace vex {

/**
 * @brief Intenta liftar @p body a IR tipado y emitirlo en @p block de @p fn.
 *
 * @param fn      Funcion IR donde emitir.
 * @param block   Indice del bloque basico actual.
 * @param isa     ISA del asm.
 * @param body    Cuerpo NASM final del bloque (consts/simbolos ya sustituidos).
 * @param slot_of Mapa registro canonico (rax..r15) -> valor SSA del ALLOCA del
 *                @c register() binding correspondiente (EN SCOPE).
 * @param line    Linea fuente para las instrucciones emitidas.
 * @return true si el bloque se lifto (se emitio IR tipado); false si no encaja
 *         ningun patron o falta algun registro del patron en @p slot_of.
 */
bool asm_lift_emit(
    ir::IrFunction &fn, uint32_t block, instr_db::Isa isa,
    const std::string &body,
    const std::unordered_map<std::string, ir::IrValueId> &slot_of,
    uint32_t line);

} // namespace vex

#endif // VX_ASM_LIFT_EMIT_H
