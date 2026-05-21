/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file virtual_lib_registry.cpp
 * @brief Implementacion del registry global de virtual libraries
 *        (Phase MC.20).  Mapa singleton @c (lib, name) -> @c fn_ptr
 *        protegido por mutex.
 */

#include "ffi/virtual_lib_registry.h"

#include <mutex>
#include <unordered_map>

namespace ffi {

    namespace {
        /**
         * @brief Singleton getter del registry.  Construct-on-first-use
         * pattern: el mapa se construye en la primera llamada, lo que
         * evita el static-init-order fiasco si algun otro static
         * constructor intenta registrar funcs antes de main().
         */
        std::unordered_map<std::string, void *> &registry() {
            static std::unordered_map<std::string, void *> g;
            return g;
        }

        /** Mutex global del registry. */
        std::mutex &registry_mutex() {
            static std::mutex m;
            return m;
        }

        /** Construye la clave canonica `<lib>:<name>`. */
        std::string make_key(const std::string &lib,
                              const std::string &name) {
            std::string k;
            k.reserve(lib.size() + 1 + name.size());
            k.append(lib);
            k.push_back(':');
            k.append(name);
            return k;
        }
    } // namespace

    void register_virtual_fn(const std::string &lib,
                              const std::string &name,
                              void              *ptr) {
        std::lock_guard<std::mutex> lk(registry_mutex());
        registry()[make_key(lib, name)] = ptr;
    }

    void *lookup_virtual_fn(const std::string &lib,
                             const std::string &name) {
        std::lock_guard<std::mutex> lk(registry_mutex());
        auto it = registry().find(make_key(lib, name));
        if (it == registry().end()) return nullptr;
        return it->second;
    }

} // namespace ffi
