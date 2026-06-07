/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/vreg_pipeline.h
 * @brief Orquestador del path de registros virtuales (Phase D.7, commit 5c).
 *
 * Encadena selector vreg -> intervalos -> linear-scan -> rewrite (VM_ABI) ->
 * encoder -> code cache -> registro en el JitRegistry.  Es el punto de entrada
 * que @c auto_jit invoca cuando @c VESTA_JIT_VREGS esta activo; si la funcion
 * no es del subset soportado por el selector vreg, devuelve @c nullptr y el
 * caller hace fallback al path de slots.
 */

#ifndef VESTA_JIT_VREG_PIPELINE_H
#define VESTA_JIT_VREG_PIPELINE_H

#include "jit/vreg_select.h"  // CallResolver

#include <cstdint>
#include <vector>

namespace ir { struct IrFunction; }

namespace jit {

    class CodeCache;

    /**
     * @brief Compila @p fn por el path de registros virtuales (VM_ABI) y la
     *        registra en el JitRegistry.
     *
     * @param fn  Funcion IR a compilar.
     * @param cc  Code cache donde alojar el codigo nativo.
     * @return    Puntero al codigo nativo (invocable via @c enter_jit), o
     *            @c nullptr si la funcion no esta soportada por el selector
     *            vreg (el caller debe hacer fallback).
     */
    uint8_t *vreg_compile(const ir::IrFunction &fn, CodeCache &cc,
                          const CallResolver &resolve_call = {},
                          const VregEntries &ent = {},
                          const CallResolver &resolve_native = {});

    /**
     * @brief Compila @p fn por el path vreg con un OSR-entry para el loop cuyo
     *        header es @p header_block (on-stack replacement, Phase D.8, 2c).
     *
     * Identico a @c vreg_compile pero (a) NO emite el contador/trigger C1
     * (suprimido en modo OSR) y (b) APPENDEA un bloque OSR-entry que carga el
     * estado del header desde @c proc->osr_buffer y salta al header.  El blob
     * resultante tiene DOS entradas: la normal (offset 0, no usada por el OSR)
     * y la OSR-entry, cuya direccion absoluta se devuelve en @p osr_entry_out.
     *
     * @param fn             Funcion IR (la misma que el C1; recompile plano).
     * @param cc             Code cache.
     * @param resolve_call   Resolver de CALLs a user-fns (igual que el C1).
     * @param ent            Entradas runtime del selector vreg.
     * @param resolve_native Resolver de CALLN nativas.
     * @param header_block   MBlock del loop header a reanudar (== IR block id).
     * @param osr_entry_out  [out] direccion absoluta del OSR-entry (o nullptr).
     * @param required_captures  Red de seguridad: VIDs que el C1 capturo.  Si
     *                       != nullptr, el OSR-entry verifica que su live-in sea
     *                       subconjunto; si no, no emite el entry (osr_entry_out
     *                       queda nullptr -> sin swap).  Critico para el C2
     *                       OPTIMIZADO (cuyo live-in puede diferir del C1).
     * @return               Codigo del blob C2 (entrada normal), o nullptr si
     *                       la funcion no es del subset vreg o no se emitio el
     *                       OSR-entry (incl. mismatch del live-in).
     */
    uint8_t *vreg_compile_osr(const ir::IrFunction &fn, CodeCache &cc,
                              const CallResolver &resolve_call,
                              const VregEntries &ent,
                              const CallResolver &resolve_native,
                              uint32_t header_block,
                              uint8_t **osr_entry_out,
                              const std::vector<uint32_t> *required_captures = nullptr);

} // namespace jit

#endif // VESTA_JIT_VREG_PIPELINE_H
