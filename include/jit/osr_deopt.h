/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/osr_deopt.h
 * @brief Deoptimizacion OSR: vuelta del codigo compilado al interprete.
 *
 * Cuando el codigo JIT encuentra una condicion que no puede manejar
 * (type miss, array bounds, null check, class change, o cualquier
 * guarda especulativa que falle), necesita revertir al interprete.
 *
 * Este modulo reconstruye el estado del interprete (registros VM,
 * pila, PC) a partir del frame nativo y la informacion de
 * deoptimizacion registrada en el OsrFrame.
 */

#ifndef VESTA_JIT_OSR_DEOPT_H
#define VESTA_JIT_OSR_DEOPT_H

#include <cstdint>

#include "jit/osr_data.h"
#include "vesta_rt/public.h"

namespace jit {

    /**
     * @brief Construye un OsrFrame desde el estado actual del frame JIT.
     *
     * Cuando el codigo compilado detecta una condicion que requiere
     * deopt, construye un OsrFrame con la informacion necesaria para
     * reconstruir el estado del interprete.
     *
     * @param proc        Proceso actual.
     * @param deopt_pc    PC bytecode al que debe continuar el interprete
     *                    (tipicamente la instruccion que sigue al loop header).
     * @param reason      Motivo del deopt.
     * @param reason_data Dato extra (clase esperada, indice, etc).
     * @return Frame de deoptimizacion listo para aplicar.
     */
    OsrFrame osr_build_deopt_frame(vrt_proc *proc,
                                   uint64_t deopt_pc,
                                   OsrFrame::DeoptReason reason,
                                   uint64_t reason_data) noexcept;

    /**
     * @brief Aplica la deoptimizacion: reconstruye el estado del
     *        interprete desde el OsrFrame.
     *
     * Pasos:
     *   1. Restaura los registros VM desde el OsrFrame.
     *   2. Restaura stack_pointer y base_pointer.
     *   3. Restaura los slots de pila.
     *   4. Setea proc->registers.rip al continuation_pc.
     *   5. Limpia proc->jit_active = 0.
     *   6. Si hay excepcion pendiente, la restaura.
     *
     * @param proc  Proceso actual.
     * @param frame Frame de deoptimizacion.
     */
    void osr_deopt_to_interpreter(vrt_proc *proc,
                                  const OsrFrame &frame) noexcept;

    /**
     * @brief Punto de entrada llamado desde el codigo JIT cuando
     *        detecta una condicion de deopt.
     *
     * Es invocado por el codigo compilado via una llamada runtime.
     * Recibe el motivo y los datos necesarios para reconstruir
     * el estado del interprete.
     *
     * @param proc        Proceso actual.
     * @param deopt_reason   Codigo del motivo (1-6).
     * @param deopt_pc       PC bytecode de continuacion.
     * @param reason_data    Dato auxiliar.
     */
    void osr_deopt_handler(vrt_proc *proc,
                           uint32_t deopt_reason,
                           uint64_t deopt_pc,
                           uint64_t reason_data) noexcept;

} // namespace jit

#endif // VESTA_JIT_OSR_DEOPT_H
