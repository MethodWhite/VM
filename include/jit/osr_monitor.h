/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/osr_monitor.h
 * @brief Monitor OSR: conteo de back-edges y disparo de compilacion.
 *
 * El monitor se integra en el interprete para contar las iteraciones
 * de cada bucle.  Cuando un bucle supera el umbral, se captura el
 * estado y se solicita la compilacion OSR.
 */

#ifndef VESTA_JIT_OSR_MONITOR_H
#define VESTA_JIT_OSR_MONITOR_H

#include <cstdint>

#include "jit/code_cache.h"
#include "jit/osr_data.h"
#include "jit/runtime_entries.h"
#include "vesta_rt/public.h"

namespace jit {

    // =====================================================================
    //  Initializacion y configuracion
    // =====================================================================

    /**
     * @brief Inicializa el subsistema OSR.
     * @param cache Code cache para la compilacion OSR.
     * @param rt    Runtime entries.
     */
    void init_osr_monitor(CodeCache &cache,
                          const RuntimeEntries &rt) noexcept;

    /**
     * @brief Setea el umbral de back-edges para OSR.
     */
    void set_osr_threshold(uint32_t threshold) noexcept;

    /**
     * @brief Consulta si OSR esta habilitado.
     */
    bool osr_enabled() noexcept;

    // =====================================================================
    //  Back-edge monitoring (llamado por el interprete)
    // =====================================================================

    /**
     * @brief Notifica al monitor que se ejecuto un back-edge.
     *
     * LLamado por el interprete cada vez que se toma un salto hacia
     * atras (back-edge).  Si el contador supera el umbral, se dispara
     * la compilacion OSR.
     *
     * @param bytecode_pc    PC del loop header (destino del salto).
     * @param function_vaddr Direccion VM del inicio de la funcion.
     * @param function_hash  Hash de la funcion (para desambiguar).
     * @return Contador de back-edges actualizado.
     */
    uint32_t osr_monitor_backedge(uint64_t bytecode_pc,
                                  uint64_t function_vaddr,
                                  uint32_t function_hash) noexcept;

    // =====================================================================
    //  Captura de estado
    // =====================================================================

    /**
     * @brief Captura el estado actual del interprete para OSR.
     *
     * Guarda registros VM, stack pointer, base pointer y una porcion
     * de la pila en un OsrState.
     *
     * @param proc        Proceso actual.
     * @param bytecode_pc PC del punto de captura (loop header).
     * @param loop_id     ID del bucle.
     * @return Estado capturado.
     */
    OsrState capture_osr_state(vrt_proc *proc,
                               uint64_t bytecode_pc,
                               uint32_t loop_id) noexcept;

    // =====================================================================
    //  Compilation trigger
    // =====================================================================

    /**
     * @brief Dispara la compilacion OSR para un punto dado.
     *
     * Crea una solicitud de compilacion y la encola (o ejecuta
     * sincronicamente) para que el compilador JIT genere el codigo
     * con entry OSR.
     *
     * @param pt             Punto OSR que cruzo el umbral.
     * @param function_vaddr Direccion VM de la funcion contenedora.
     * @return true si la compilacion se inicio correctamente.
     */
    bool trigger_osr_compilation(OsrPoint &pt,
                                 uint64_t function_vaddr) noexcept;

    // =====================================================================
    //  Lookup helpers
    // =====================================================================

    /**
     * @brief Busca la direccion del entry OSR compilado para un loop.
     * @return Direccion absoluta del codigo nativo, o 0 si no disponible.
     */
    uint64_t osr_lookup_entry(uint32_t loop_id) noexcept;

    /**
     * @brief Obtiene la tabla OSR global (lectura).
     */
    const OsrTable &get_osr_table() noexcept;

    /**
     * @brief Obtiene la tabla OSR global (modificacion).
     */
    OsrTable &get_osr_table_mutable() noexcept;

} // namespace jit

#endif // VESTA_JIT_OSR_MONITOR_H
