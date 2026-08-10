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
 * @file asm_lift.h
 * @brief Reconocimiento de patrones de inline asm que se pueden LIFTAR a una
 *        operacion tipada del IR (atomicos).
 *
 * Detecta secuencias de asm cuya semantica corresponde EXACTAMENTE a un op del
 * IR neutro (p.ej. @c lock @c cmpxchg -> @c ATOMIC_CAS, @c lock @c xadd ->
 * @c ATOMIC_ADD).  Cuando el patron encaja, el lowering puede emitir el op
 * tipado en lugar de una caja opaca @c INLINE_ASM: el interprete lo ejecuta como
 * opcode (sin trampolin), el JIT/AOT lo emiten con la instruccion atomica nativa
 * y el analizador lo entiende (contratos/coste).  Multi-arch: x86 y arm64 liftan
 * al MISMO op neutro; el backend re-emite por target.
 *
 * Este modulo SOLO RECONOCE (funcion pura); no toca el IR.  Devuelve los
 * registros canonicos implicados para que el lowering los mapee a sus valores
 * SSA (los ligados por @c register()).  La disciplina es conservadora: si el
 * patron no encaja con total certeza, no se lifta (op = None) y el bloque sigue
 * su ruta @c INLINE_ASM normal.
 */

#ifndef VX_ASM_LIFT_H
#define VX_ASM_LIFT_H

#include <cstdint>
#include <string>

#include "vex/asm/instr_db.h"

namespace vex {

/// Operacion tipada a la que un bloque de asm se puede liftar.
enum class AsmLiftOp : uint8_t {
    None,       ///< no se reconocio ningun patron liftible.
    AtomicCas,  ///< compare-and-swap (lock cmpxchg / bucle ldaxr-stlxr).
    AtomicAdd,  ///< fetch-and-add (lock xadd / bucle ldaxr-add-stlxr).
};

/// Resultado del reconocimiento.  Los registros van en forma CANONICA (rax..r15)
/// para que el lowering los cruce con los @c register() bindings.
struct AsmLift {
    AsmLiftOp op = AsmLiftOp::None;
    std::string addr_reg;   ///< registro que contiene la DIRECCION (puntero).
    std::string exp_reg;    ///< CAS: registro con el valor ESPERADO.
    std::string des_reg;    ///< CAS: valor DESEADO.  ADD: el DELTA.
    std::string result_reg; ///< registro que recibe el valor VIEJO (retorno).
    uint16_t width = 64;    ///< ancho en bits (8/16/32/64).
    std::string note;       ///< motivo por el que NO se lifto (diagnostico), o "".
};

/**
 * @brief Intenta reconocer un patron atomico liftible en @p body.
 *
 * @param isa  ISA del bloque.
 * @param body Cuerpo del bloque @c asm (sintaxis del ensamblador).
 * @return @ref AsmLift con @c op != None si encaja; en otro caso @c op = None
 *         (y @c note explica por que, para diagnostico).
 */
AsmLift asm_lift_detect(instr_db::Isa isa, const std::string &body);

} // namespace vex

#endif // VX_ASM_LIFT_H
