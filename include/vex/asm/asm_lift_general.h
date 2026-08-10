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
 * @file vx/asm/asm_lift_general.h
 * @brief Lift GENERAL de un bloque de asm a IR SSA: en lugar de reconocer un
 *        patron concreto, LIFTA CADA INSTRUCCION a su operacion IR y enhebra el
 *        valor SSA de cada registro.  El bloque asm deja de ser una caja opaca
 *        y se vuelve IR real que participa del optimizador (plegado de
 *        constantes, DCE, reduccion de fuerza...) -> del asm del usuario se
 *        genera codigo mas eficiente.
 *
 * Es la base del refactor completo del asm inline (no solo atomicos): mientras
 * el reconocedor de patrones (@ref asm_lift_detect) mapea secuencias conocidas a
 * ops tipados de alto nivel (ATOMIC_CAS...), este lifter cubre el caso general
 * instruccion-a-instruccion.  Crece bajo demanda: instruccion fuera del subset
 * soportado -> devuelve false (el llamador cae a la emision INLINE_ASM opaca).
 */

#ifndef VESTA_VX_ASM_ASM_LIFT_GENERAL_H
#define VESTA_VX_ASM_ASM_LIFT_GENERAL_H

#include "vex/asm/instr_db.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace ir {
struct IrFunction;
using IrValueId = uint32_t;
enum class IrType : uint8_t;
} // namespace ir

namespace vex {

/**
 * @brief Registro ligado a una variable Vesta via @c register(): su slot ALLOCA
 *        y el ancho (bits) del tipo de la variable.  El ancho es imprescindible
 *        para cargar/escribir el slot EXACTO: @c register("eax") @c u32 tiene un
 *        slot de 4 bytes, @c register("rax") @c u64 uno de 8.
 */
struct AsmBoundReg {
    ir::IrValueId slot;   ///< dst del ALLOCA del var
    int width_bits = 64;  ///< 8/16/32/64 segun el tipo de la variable
};

/**
 * @brief Lifta el bloque @p body (asm de la ISA @p isa) a IR SSA en @p block,
 *        enhebrando el valor de cada registro.  Los registros ligados por
 *        @c register() (en @p bound) se cargan de su slot al entrar (al ancho
 *        del tipo) y se escriben de vuelta al salir; los demas son temporales
 *        SSA puros.  Modela los anchos x86 (una escritura de 32 bits pone a
 *        cero los 32 altos; de 8/16 preserva los altos; SAR usa el signo al
 *        ancho correcto) para no miscompilar operaciones sub-64.
 *
 * @return true si TODO el bloque se lifto (0 INLINE_ASM); false si aparece una
 *         instruccion/forma fuera del subset -> el llamador emite INLINE_ASM.
 */
bool asm_lift_general(
    ir::IrFunction &fn, uint32_t block, instr_db::Isa isa,
    const std::string &body,
    const std::unordered_map<std::string, AsmBoundReg> &bound, uint32_t line,
    uint32_t *out_exit = nullptr);

} // namespace vex

#endif // VESTA_VX_ASM_ASM_LIFT_GENERAL_H
