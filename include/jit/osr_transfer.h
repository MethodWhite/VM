/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/osr_transfer.h
 * @brief Transferencia de estado interprete -> JIT para OSR.
 *
 * Toma un OsrState capturado del interprete y lo transfiere al
 * codigo compilado JIT, re-mapeando los registros VM y la pila
 * al layout esperado por el codigo nativo.
 */

#ifndef VESTA_JIT_OSR_TRANSFER_H
#define VESTA_JIT_OSR_TRANSFER_H

#include <cstdint>

#include "jit/osr_data.h"
#include "vesta_rt/public.h"

namespace jit {

    /**
     * @brief Transfiere la ejecucion del interprete al codigo JIT
     *        compilado para OSR.
     *
     * Pasos:
     *   1. Toma el OsrState capturado.
     *   2. Escribe los valores en el osr_buffer del proceso
     *      (proc->osr_buffer[vid] = valor).
     *   3. Setea el entry point OSR en proc->osr_entry para que
     *      el prologo del codigo compilado lo lea.
     *   4. Ajusta proc->jit_active y proc->registers.rip para
     *      que el interprete salte al codigo compilado.
     *   5. Retorna la direccion del codigo compilado; el interprete
     *      debe saltar a ella inmediatamente.
     *
     * @param proc        Proceso actual.
     * @param state       Estado capturado del interprete.
     * @param entry       Entrada OSR compilada (con direccion y offset).
     * @return Direccion del codigo nativo al que saltar, o 0 si fallo.
     */
    uint64_t osr_transfer_to_compiled(vrt_proc *proc,
                                      const OsrState &state,
                                      const OsrEntry &entry) noexcept;

    /**
     * @brief Escribe el estado capturado en el osr_buffer del proceso.
     *
     * Cada celda del buffer corresponde a un IR VID.  El codigo
     * compilado OSR-entry lee estas celdas al inicio.
     *
     * @param proc  Proceso actual.
     * @param state Estado capturado.
     * @param entry Entrada OSR (contiene el mapeo vid->buffer slot).
     */
    void osr_write_buffer(vrt_proc *proc,
                          const OsrState &state,
                          const OsrEntry &entry) noexcept;

    /**
     * @brief Verifica que el estado capturado sea compatible con la
     *        entrada OSR compilada.
     *
     * Comprueba que todos los vids que el codigo compilado espera
     * esten presentes en el estado capturado.
     *
     * @return true si es compatible.
     */
    bool osr_validate_transfer(const OsrState &state,
                               const OsrEntry &entry) noexcept;

} // namespace jit

#endif // VESTA_JIT_OSR_TRANSFER_H
