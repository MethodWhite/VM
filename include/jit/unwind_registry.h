/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/unwind_registry.h
 * @brief Registro de informacion de unwind para funciones JIT.
 *
 * = Proposito =
 *
 * El @c UnwindRegistry es el nexo entre el JIT compiler y el OS runtime
 * de unwinding.  Cuando el JIT compila una funcion, registra su
 * informacion de desenrollado (unwind info) en el registro.  El registro
 * se encarga de:
 *
 *   1. Generar y mantener las tablas de unwind del SO (.pdata/.xdata en
 *      Windows, .eh_frame en Linux).
 *   2. Instalar las tablas via las APIs del sistema (RtlAddFunctionTable
 *      en Windows, o registro en el .eh_frame dynamic).
 *   3. Proveer lookup de UnwindInfo por PC para el debugger y el
 *      GC stack scan.
 *
 * = Concurrencia =
 *
 * El registro usa @c std::mutex para proteger todas las operaciones de
 * escritura (register/unregister).  Las lecturas (@c lookup_unwind_info)
 * tambien toman el mutex porque los datos subyacentes pueden ser
 * reasignados durante un register.
 *
 * = Integracion con JitRegistry =
 *
 * @c UnwindRegistry y @c JitRegistry son complementarios pero separados:
 *   - @c JitRegistry maneja stackmaps para GC precise scan.
 *   - @c UnwindRegistry maneja unwind tables para el OS/debugger.
 * Ambos se actualizan en tandem cuando se compila/invalida una funcion.
 */

#ifndef VESTA_JIT_UNWIND_REGISTRY_H
#define VESTA_JIT_UNWIND_REGISTRY_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "jit/unwind_info.h"

namespace jit {

    /**
     * @struct RegisteredFunction
     * @brief Entrada en el registro de unwind.
     */
    struct RegisteredFunction {
        const uint8_t *code_start    = nullptr;
        const uint8_t *code_end      = nullptr;
        UnwindInfo     unwind_info;
    };

    /**
     * @class UnwindRegistry
     * @brief Registro de informacion de unwind para funciones JIT.
     *
     * Proporciona una API thread-safe para registrar, desregistrar y
     * buscar informacion de unwind por direccion de codigo.
     *
     * Esta clase puede ser instanciada por VM (per-VM) o usarse como
     * singleton global.  La implementacion actual expone ambas opciones.
     */
    class UnwindRegistry {
    public:
        /** @brief Construye un registro vacio. */
        UnwindRegistry() noexcept = default;

        /** @brief Libera todos los recursos (desregistra todo). */
        ~UnwindRegistry();

        UnwindRegistry(const UnwindRegistry &) = delete;
        UnwindRegistry &operator=(const UnwindRegistry &) = delete;

        // -- Singleton -------------------------------------------------------

        /** @brief Acceso al singleton global. */
        static UnwindRegistry &instance() noexcept;

        // -- Registro --------------------------------------------------------

        /**
         * @brief Registra una funcion JIT con sus metadatos de unwind.
         *
         * @param code_start Puntero al inicio del codigo nativo.
         * @param code_size  Tamano del codigo en bytes.
         * @param info       Descriptor de unwind para la funcion.
         * @return true si el registro fue exitoso, false si fallo
         *         (e.g. tabla llena, error en API del SO).
         *
         * En Windows esto llama @c RtlAddFunctionTable.  En Linux
         * actualiza la tabla .eh_frame dinamica.
         */
        bool register_function(const uint8_t *code_start,
                               size_t code_size,
                               const UnwindInfo &info);

        /**
         * @brief Desregistra una funcion previamente registrada.
         *
         * @param code_start Puntero al inicio de la funcion.
         *                   No-op si no esta registrada.
         */
        void unregister_function(const uint8_t *code_start);

        // -- Lookup ----------------------------------------------------------

        /**
         * @brief Busca la informacion de unwind para una direccion.
         *
         * @param pc Direccion de codigo a buscar.
         * @return Puntero al @c UnwindInfo si se encontro, nullptr en
         *         otro caso.
         *
         * El puntero devuelto es valido mientras no se modifique el
         * registro (register/unregister).
         */
        const UnwindInfo *lookup_unwind_info(const uint8_t *pc) const;

        // -- Gestion ---------------------------------------------------------

        /** @brief Numero de funciones registradas actualmente. */
        size_t size() const noexcept;

        /** @brief Limpia todas las entradas (para testing). */
        void clear();

    private:
        /// Reallocacion de la tabla dinamica cuando se excede la capacidad.
        bool grow_table();

        /// Registra en el SO via RtlAddFunctionTable o equivalente.
        bool install_os_table(const RegisteredFunction &fn);

        /// Desregistra del SO via RtlDeleteFunctionTable o equivalente.
        void uninstall_os_table(const RegisteredFunction &fn);

        mutable std::mutex              mutex_;
        std::vector<RegisteredFunction> functions_;
    };

} // namespace jit

#endif // VESTA_JIT_UNWIND_REGISTRY_H
