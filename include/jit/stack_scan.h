/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/stack_scan.h
 * @brief Stack walker preciso para JIT frames usando stackmaps.
 *
 * = Diseno =
 *
 * Cuando el GC quiere encontrar todos los GC roots de un proceso,
 * camina la cadena de frames @c RBP -> [RBP] -> [[RBP]] -> ...  Por
 * cada frame:
 *
 *   1. Captura el RIP del caller (almacenado en @c [RBP+8] por la
 *      convencion x86-64: @c push rbp pone caller's saved RBP, y
 *      la @c call que vino antes puso el return address en @c [RBP+8]).
 *   2. Lookup en @c JitRegistry: ¿es @p rip parte de una funcion JIT?
 *      Si SI: este es un JIT frame, usar stackmap para precise scan.
 *      Si NO: este es un interp frame (o C runtime), saltar (scan
 *      conservativo cubrira sus slots).
 *   3. Avanzar al siguiente frame: @c rbp = [rbp].  Parar cuando
 *      @c rbp == nullptr o sale del rango del stack.
 *
 * = Coexistencia con scan conservativo =
 *
 * (integration): este scan corre EN PARALELO con
 * el conservativo (@c gc_heap::scan_stack_roots).  Los GC roots de
 * JIT frames se marcan precise + tambien conservativo los marca
 * "por accidente" pero como mismo target -> sin efecto.
 *
 * (futuro): el conservativo se restringe a NO escanear
 * rangos de stack cubiertos por JIT frames -> elimina falsos
 * positivos por completo.
 */

#ifndef VESTA_JIT_STACK_SCAN_H
#define VESTA_JIT_STACK_SCAN_H

#include <cstdint>

#include "jit/machine_ir.h"

namespace jit {

    /**
     * @struct JitScanStats
     * @brief Estadisticas de un scan de JIT frames.  Util para
     *        comparar precise vs conservative en la fase 1.
     */
    struct JitScanStats {
        uint32_t frames_walked      = 0;  ///< total frames en la cadena RBP
        uint32_t jit_frames         = 0;  ///< frames JIT (con stackmap)
        uint32_t non_jit_frames     = 0;  ///< frames no-JIT (saltados aqui)
        uint32_t handles_marked     = 0;  ///< slots GC marcados precise
        uint32_t hostptr_marked     = 0;  ///< slots host_ptr marcados precise
    };

    /**
     * @brief Callback invocado por @c scan_jit_frames para cada root
     *        encontrado.  El caller (GcHeap o test) decide que hacer
     *        con la informacion (marcar handle, encolar en worklist,
     *        log para debug, etc.).
     *
     * @param ctx       contexto opaco del caller.
     * @param value     valor leido del slot (uint64).
     * @param kind      tipo segun el stackmap.
     * @param slot_addr direccion del slot (para debugging / forward refs).
     */
    using JitRootCallback = void (*)(void *ctx,
                                     uint64_t value,
                                     StackmapGcKind kind,
                                     const uint8_t *slot_addr);

    /**
     * @brief Lee 8 bytes (uint64) desde @p ptr.  Usado por el walker.
     */
    inline uint64_t read_u64(const uint8_t *ptr) noexcept {
        uint64_t v;
        __builtin_memcpy(&v, ptr, sizeof(v));
        return v;
    }

    /**
     * @brief Walk de la cadena RBP de un thread y precise-scan de los
     *        JIT frames encontrados.
     *
     * El callback @p cb se invoca para cada slot GC del stackmap,
     * con el valor leido y su categoria.
     *
     * @param cb         callback a invocar por cada root encontrado.
     * @param cb_ctx     contexto opaco para @p cb.
     * @param rbp_top    RBP actual del thread.  En el handler de
     *                   safepoint este es @c __builtin_frame_address(0)
     *                   del caller del handler.
     * @param stack_low  limite inferior del stack (no leer mas abajo).
     * @param stack_high limite superior del stack (no leer mas arriba).
     * @return estadisticas del scan.
     */
    JitScanStats scan_jit_frames(JitRootCallback cb,
                                 void *cb_ctx,
                                 const uint8_t *rbp_top,
                                 const uint8_t *stack_low,
                                 const uint8_t *stack_high) noexcept;

} // namespace jit

#endif // VESTA_JIT_STACK_SCAN_H
