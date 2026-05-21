/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file ffi/virtual_lib_registry.h
 * @brief Registry in-process de "virtual libraries" -- libs ficticias
 *        cuyos simbolos viven en el ejecutable mismo (no en una DLL
 *        del SO) pero son callable via el mecanismo FFI estandar
 *        (`extern "lib.dll" fn name(...)`).
 *
 * Phase MC.20 (Camino A.cont): habilita exponer servicios compile-time
 * de @c vex_lib (static_assert, comptime_compile, comptime_type, etc.)
 * a las macros sin necesidad de DLL externa.  El macro escribe:
 *
 *   extern "vesta_comptime" {
 *       fn static_assert(cond: i64, msg: char*) -> i64;
 *   }
 *
 *   @Macro
 *   comptime string M() {
 *       static_assert(sizeof<i32>() == 4, "i32 debe ser 4 bytes");
 *       return "42";
 *   }
 *
 * Y el FFI dispatch (tanto AST eval como Loader VM) busca primero en
 * el registry virtual antes de intentar @c LoadLibrary / @c dlopen.
 *
 * Thread-safe via @c std::mutex.  Registration es one-shot global
 * (idempotente; re-registrar el mismo (lib, fn) sobreescribe el ptr).
 *
 * @note El uso intencionado es SOLO para servicios compile-time
 *       expuestos al macro engine.  Para libs reales del SO, usar
 *       LoadLibrary directo.
 */

#ifndef FFI_VIRTUAL_LIB_REGISTRY_H
#define FFI_VIRTUAL_LIB_REGISTRY_H

#include <string>

namespace ffi {

    /**
     * @brief Registra una funcion virtual bajo el par @c (lib, name).
     *
     * Si ya existe una entrada con el mismo par, se sobreescribe.
     * Threadsafe (registracion concurrente OK; el caller no necesita
     * sincronizar).
     *
     * @param lib  Nombre de la "lib virtual" (e.g. "vesta_comptime").
     * @param name Nombre de la funcion (e.g. "static_assert").
     * @param ptr  Puntero a la implementacion C (typename `uint64_t(*)(...)`).
     */
    void register_virtual_fn(const std::string &lib,
                              const std::string &name,
                              void              *ptr);

    /**
     * @brief Busca una funcion virtual registrada.
     *
     * Lookup O(1) amortizado en hash map global.  Threadsafe.
     *
     * @param lib  Nombre de la "lib virtual".
     * @param name Nombre de la funcion.
     * @return     Puntero a la fn o @c nullptr si no existe.
     */
    void *lookup_virtual_fn(const std::string &lib,
                             const std::string &name);

} // namespace ffi

#endif // FFI_VIRTUAL_LIB_REGISTRY_H
