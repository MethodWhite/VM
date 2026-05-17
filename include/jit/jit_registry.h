/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/jit_registry.h
 * @brief Registro global de funciones JIT-compiladas + sus stackmaps.
 *
 * = Diseno =
 *
 * El GC necesita identificar JIT frames durante stack walk para usar
 * stackmaps precisos (cero falsos positivos).  Para eso:
 *
 *   1. Al compilar una funcion, el JIT registra
 *      @c (code_start, code_end, stackmaps) en este registro.
 *   2. Cuando el GC walks el RBP chain de un proceso, por cada frame
 *      consulta @c lookup(rip): si encuentra match, ese frame es JIT
 *      y se usa su stackmap; si no, el frame es interpreted y cae al
 *      scan conservativo.
 *
 * = Concurrencia =
 *
 * (C2 multi-thread compile): solo el compile thread
 * escribe al registro, los GC threads leen.  Usamos @c std::mutex en
 * @c register_function/unregister_function pero @c lookup es lock-free
 * usando snapshot vector (re-scrito ante cada register) -- el GC scan
 * captura un snapshot via shared_ptr y lo lee sin sincronizacion.
 *
 * NO hay compile en background, solo
 * el main thread.  Asi que en este sprint usamos un simple mutex.
 *
 * = Lookup =
 *
 * @c functions_ ordenado por code_start (ascending).  @c lookup hace
 * binary search O(log N).  Para N=100K funciones JIT-eadas, ~17
 * comparaciones.  Despreciable durante un GC cycle (~500 us total).
 */

#ifndef VESTA_JIT_REGISTRY_H
#define VESTA_JIT_REGISTRY_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "jit/machine_ir.h"

namespace jit {

    /**
     * @struct JitFunctionInfo
     * @brief Descriptor de una funcion JIT-compilada activa.
     */
    struct JitFunctionInfo {
        const uint8_t       *code_start = nullptr;
        const uint8_t       *code_end   = nullptr;  ///< exclusive
        /// Stackmaps ordenados por @c pc_offset (ascending).  El GC
        /// usa @c binary_search para encontrar el stackmap que cubre
        /// un RIP dado.
        std::vector<Stackmap> stackmaps;
        /// Tamano del frame stack (sub rsp, frame_size).  Usado por el
        /// GC scan para identificar limites de slots dentro del frame.
        uint32_t              frame_size = 0;
        /// Nombre legible (debugging).
        const char           *name = "";
    };

    /**
     * @class JitRegistry
     * @brief Singleton global con las funciones JIT activas.
     */
    class JitRegistry {
    public:
        static JitRegistry &instance() noexcept;

        /**
         * @brief Registra una funcion JIT recien compilada.
         *
         * Toma ownership del @c stackmaps por @c std::move; el caller
         * no debe seguir usandolo tras el registro.
         */
        void register_function(const uint8_t *code_start,
                               const uint8_t *code_end,
                               std::vector<Stackmap> stackmaps,
                               uint32_t frame_size,
                               const char *name = "");

        /**
         * @brief Elimina el registro de una funcion (e.g. cuando se
         *        invalida via deopt).  No-op si no existe.
         */
        void unregister_function(const uint8_t *code_start);

        /**
         * @brief Busca la funcion JIT que contiene @p rip.
         * @return puntero al descriptor (estable durante la vida del
         *         registro) o @c nullptr si @p rip no esta en ninguna
         *         funcion JIT activa.
         *
         * Coste: O(log N) con N = funciones registradas.
         */
        const JitFunctionInfo *lookup(const uint8_t *rip) const;

        /**
         * @brief Busca el Stackmap correspondiente al @p rip dentro de
         *        una funcion JIT (helper combinado).
         * @return puntero al stackmap si encontrado, o @c nullptr.
         *
         * Para que el GC scan no tenga que hacer la busqueda en dos
         * pasos.  Si @c rip no pertenece a ninguna funcion JIT o la
         * funcion no tiene stackmap exacto en ese pc_offset, retorna
         * el stackmap MAS RECIENTE @c <= pc_offset (el ultimo safepoint
         * antes del PC actual).
         */
        const Stackmap *lookup_stackmap(const uint8_t *rip) const;

        /** @brief Numero de funciones registradas actualmente. */
        size_t size() const;

        /** @brief Borrar todas las entradas (testing). */
        void clear();

    private:
        JitRegistry() = default;
        ~JitRegistry() = default;
        JitRegistry(const JitRegistry &) = delete;
        JitRegistry &operator=(const JitRegistry &) = delete;

        mutable std::mutex                mutex_;
        std::vector<JitFunctionInfo>      functions_;  ///< sorted by code_start
    };

} // namespace jit

#endif // VESTA_JIT_REGISTRY_H
