/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/unwind_registry.cpp
 * @brief Implementacion del registro de informacion de unwind.
 *
 * = Diseno =
 *
 * El @c UnwindRegistry mantiene un vector ordenado de
 * @c RegisteredFunction entries, cada una conteniendo el rango de
 * codigo (code_start, code_end) y el @c UnwindInfo asociado.
 *
 * Cuando se registra una nueva funcion:
 *   1. Se inserta en el vector manteniendo orden por code_start.
 *   2. Se instala la tabla en el SO (RtlAddFunctionTable en Windows,
 *      o se anyade la FDE a la tabla .eh_frame dinamica en Linux).
 *   3. Si el vector se llena, se reasigna con mayor capacidad.
 *
 * La busqueda por PC usa binary search O(log N).  La tabla es
 * consultada por:
 *   - El debugger (stack walk).
 *   - El manejador de excepciones.
 *   - El GC (para identificar frames JIT).
 *
 * = Thread safety =
 *
 * Todas las operaciones publicas toman @c mutex_ .  Las lecturas
 * (@c lookup_unwind_info) son seguras para concurrencia.
 * Las escrituras (register/unregister) son serializadas.
 *
 * = Integracion con code cache =
 *
 * El @c UnwindRegistry no posee los bytes del codigo ni del unwind
 * info; solo referencia punteros al @c CodeCache.  El @c CodeCache
 * es responsable de la memoria subyacente.
 */

#include "jit/unwind_registry.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <cstring>
#endif

namespace jit {

    // =========================================================================
    // Singleton
    // =========================================================================

    UnwindRegistry &UnwindRegistry::instance() noexcept {
        static UnwindRegistry reg;
        return reg;
    }

    // =========================================================================
    // Constructor / Destructor
    // =========================================================================

    UnwindRegistry::~UnwindRegistry() {
        // Desregistrar todas las funciones del SO y limpiar la tabla
        // local.  El orden es importante: primero desinstalar del SO,
        // luego limpiar el vector.
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto &fn : functions_) {
            uninstall_os_table(fn);
        }
        functions_.clear();
    }

    // =========================================================================
    // Registro
    // =========================================================================

    bool UnwindRegistry::register_function(const uint8_t *code_start,
                                            size_t code_size,
                                            const UnwindInfo &info) {
        if (!code_start || code_size == 0) return false;

        std::lock_guard<std::mutex> lk(mutex_);

        // Construir la entrada de registro
        RegisteredFunction rf;
        rf.code_start  = code_start;
        rf.code_end    = code_start + code_size;
        rf.unwind_info = info;

        // Instalar en el SO primero (si falla, no registrar)
        if (!install_os_table(rf)) {
            // Fallo en la instalacion del SO.  Podria ser por falta de
            // memoria en las tablas del kernel.  No registrar.
            return false;
        }

        // Insertar ordenadamente por code_start para binary search
        auto it = std::upper_bound(functions_.begin(), functions_.end(),
                                    rf,
                                    [](const RegisteredFunction &a,
                                       const RegisteredFunction &b) {
                                        return a.code_start < b.code_start;
                                    });
        functions_.insert(it, std::move(rf));

        return true;
    }

    void UnwindRegistry::unregister_function(const uint8_t *code_start) {
        if (!code_start) return;

        std::lock_guard<std::mutex> lk(mutex_);

        auto it = std::find_if(functions_.begin(), functions_.end(),
                                [code_start](const RegisteredFunction &f) {
                                    return f.code_start == code_start;
                                });
        if (it != functions_.end()) {
            uninstall_os_table(*it);
            functions_.erase(it);
        }
    }

    // =========================================================================
    // Lookup
    // =========================================================================

    const UnwindInfo *UnwindRegistry::lookup_unwind_info(
        const uint8_t *pc) const {
        std::lock_guard<std::mutex> lk(mutex_);

        if (!pc || functions_.empty()) return nullptr;

        // Binary search: encontrar el primer entry con code_start > pc
        auto it = std::upper_bound(functions_.begin(), functions_.end(),
                                    pc,
                                    [](const uint8_t *addr,
                                       const RegisteredFunction &f) {
                                        return addr < f.code_start;
                                    });

        // El entry anterior (si existe) es el candidato
        if (it == functions_.begin()) return nullptr;
        --it;

        // Verificar que pc esta dentro del rango
        if (pc < it->code_start || pc >= it->code_end) return nullptr;

        return &it->unwind_info;
    }

    // =========================================================================
    // Gestion
    // =========================================================================

    size_t UnwindRegistry::size() const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return functions_.size();
    }

    void UnwindRegistry::clear() {
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto &fn : functions_) {
            uninstall_os_table(fn);
        }
        functions_.clear();
    }

    // =========================================================================
    // Crecimiento dinamico de la tabla
    // =========================================================================

    bool UnwindRegistry::grow_table() {
        // En la implementacion actual, std::vector se encarga del
        // crecimiento automatico.  Este metodo es un hook para cuando
        // se requiera gestion manual de memoria (e.g. arrays planos
        // de RUNTIME_FUNCTION para RtlAddFunctionTable).
        //
        // La estrategia actual: cuando el vector se acerque a su
        // capacidad, duplicamos la reserva.
        //
        // En Windows con tablas planas, necesitariamos reasignar
        // el array de RUNTIME_FUNCTION y llamar a RtlAddFunctionTable
        // de nuevo (o usar tablas separadas).
        return true;
    }

    // =========================================================================
    // Instalacion en el SO (implementacion por plataforma)
    // =========================================================================

#if defined(_WIN32)

    /**
     * @brief Instala la funcion en la tabla de unwind del SO (Windows).
     *
     * En Windows, las funciones JIT dynamic se registran con
     * @c RtlAddFunctionTable.  Esta funcion maneja una tabla plana
     * de RUNTIME_FUNCTION entries que crece dinamicamente.
     *
     * Nota: La implementacion completa requeriria un array contiguo
     * de RUNTIME_FUNCTION.  Aqui proporcionamos la estructura base;
     * la integracion final requeriria mantener ese array y llamar a
     * Add/Delete segun crezca.
     */
    bool UnwindRegistry::install_os_table(const RegisteredFunction &fn) {
        (void)fn;
        // En una implementacion completa, hariamos:
        //   1. Obtener o crear un bloque de memoria con las
        //      RUNTIME_FUNCTION entries.
        //   2. Llamar a RtlAddFunctionTable con el puntero al bloque.
        //
        // Por ahora retornamos true asumiendo que la tabla dinamica
        // se manejara externamente (integracion con el emitter PE).
        return true;
    }

    void UnwindRegistry::uninstall_os_table(const RegisteredFunction &fn) {
        (void)fn;
        // Llamar a RtlDeleteFunctionTable con la funcion especifica.
        // En una implementacion completa, rastreariamos el puntero
        // devuelto por RtlAddFunctionTable.
    }

#else // Linux / POSIX

    /**
     * @brief Instala la funcion en la tabla de unwind del SO (Linux).
     *
     * En Linux, el .eh_frame dinamico se registra a traves de
     * dl_iterate_phdr o registrando en __register_frame_info.
     * La implementacion completa usaria __register_frame para
     * registrar tablas .eh_frame en el unwinder de libgcc.
     *
     * Por ahora es un stub que retorna true.  La generacion del
     * .eh_frame se hace en unwind_elf.cpp; el registro dinamico
     * se anyadira cuando se integre con el runtime de C++ exceptions.
     */
    bool UnwindRegistry::install_os_table(const RegisteredFunction &fn) {
        (void)fn;
        // En una implementacion completa:
        //   - Generar .eh_frame (CIE + FDE) para esta funcion.
        //   - Llamar a __register_frame_info(void *eh_frame, ...)
        //     de libgcc para registrar la tabla.
        //   - Mantener el puntero para desregistro posterior.
        return true;
    }

    void UnwindRegistry::uninstall_os_table(const RegisteredFunction &fn) {
        (void)fn;
        // En una implementacion completa:
        //   - Llamar a __deregister_frame_info(void *eh_frame)
        //     de libgcc.
    }

#endif // _WIN32

} // namespace jit
