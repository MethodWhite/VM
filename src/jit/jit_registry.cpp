/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/jit_registry.cpp
 * @brief Implementacion del @c JitRegistry.
 *
 * Vector @c functions_ ordenado por @c code_start.  @c lookup hace
 * binary search.  @c register_function inserta en su posicion correcta
 * y reordena el vector si es necesario (O(N) en el peor caso por el
 * shift, aceptable porque los registros suceden 1 vez por funcion JIT
 * y son raros).
 *
 * @c lookup_stackmap busca el stackmap MAYOR cuyo @c pc_offset <= rip_offset.
 * Esto es porque el RIP capturado en el handler suele apuntar a la
 * INSTRUCCION SIGUIENTE al call que disparo el safepoint.  El stackmap
 * que aplica es el del SAFEPOINT ANTERIOR, no el siguiente.
 */

#include "jit/jit_registry.h"

#include <algorithm>

namespace jit {

    JitRegistry &JitRegistry::instance() noexcept {
        static JitRegistry inst;
        return inst;
    }

    void JitRegistry::register_function(const uint8_t *code_start,
                                        const uint8_t *code_end,
                                        std::vector<Stackmap> stackmaps,
                                        uint32_t frame_size,
                                        const char *name) {
        if (!code_start || !code_end || code_start >= code_end) return;
        std::lock_guard<std::mutex> lk(mutex_);

        /* Sort stackmaps por pc_offset para que lookup_stackmap pueda
         * hacer binary search. */
        std::sort(stackmaps.begin(), stackmaps.end(),
                  [](const Stackmap &a, const Stackmap &b) {
                      return a.pc_offset < b.pc_offset;
                  });

        JitFunctionInfo info;
        info.code_start = code_start;
        info.code_end   = code_end;
        info.stackmaps  = std::move(stackmaps);
        info.frame_size = frame_size;
        info.name       = name ? name : "";

        /* Insercion ordenada por code_start. */
        auto it = std::upper_bound(functions_.begin(), functions_.end(),
                                   info,
                                   [](const JitFunctionInfo &a, const JitFunctionInfo &b) {
                                       return a.code_start < b.code_start;
                                   });
        functions_.insert(it, std::move(info));
    }

    void JitRegistry::unregister_function(const uint8_t *code_start) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = std::find_if(functions_.begin(), functions_.end(),
                               [code_start](const JitFunctionInfo &f) {
                                   return f.code_start == code_start;
                               });
        if (it != functions_.end()) {
            functions_.erase(it);
        }
    }

    const JitFunctionInfo *JitRegistry::lookup(const uint8_t *rip) const {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!rip || functions_.empty()) return nullptr;

        /* Binary search: el primer entry cuyo code_start > rip nos da el
         * candidato (anterior).  Verificamos que rip < code_end. */
        auto it = std::upper_bound(functions_.begin(), functions_.end(),
                                   rip,
                                   [](const uint8_t *r, const JitFunctionInfo &f) {
                                       return r < f.code_start;
                                   });
        if (it == functions_.begin()) return nullptr;
        --it;
        if (rip < it->code_start || rip >= it->code_end) return nullptr;
        return &(*it);
    }

    const Stackmap *JitRegistry::lookup_stackmap(const uint8_t *rip) const {
        const JitFunctionInfo *info = lookup(rip);
        if (!info) return nullptr;

        const uint32_t pc_offset = static_cast<uint32_t>(rip - info->code_start);

        /* Buscar el stackmap mas alto con pc_offset <= rip_offset.
         * upper_bound con clave rip_offset+1 - 1, equivalente.
         * Mas claro: lower_bound de (pc_offset+1) - 1. */
        auto it = std::upper_bound(info->stackmaps.begin(), info->stackmaps.end(),
                                   pc_offset,
                                   [](uint32_t off, const Stackmap &s) {
                                       return off < s.pc_offset;
                                   });
        if (it == info->stackmaps.begin()) {
            /* Ningun safepoint anterior al RIP; nada que escanear. */
            return nullptr;
        }
        --it;
        return &(*it);
    }

    size_t JitRegistry::size() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return functions_.size();
    }

    void JitRegistry::clear() {
        std::lock_guard<std::mutex> lk(mutex_);
        functions_.clear();
    }

} // namespace jit
