/*
 * VestaVM - Maquina Virtual Distribuida
 * Copyright (C) 2026 David Lopez.T (DesmonHak); Licencia VMProject
 */
/**
 * @file src/runtime/profile.cpp
 * @brief Sprint D.6: implementacion del profile collector singleton.
 *
 * Ver @c include/runtime/profile.h para diseno + ABI del .vprof.
 */

#include "runtime/profile.h"

#include "loader/oop_types.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

namespace runtime {
namespace profile {

    /// Instancia singleton del collector.  Zero-init estatico garantiza
    /// que @c active es false al startup -> fast path inline en todos
    /// los hooks no paga overhead.
    ProfileCollector g_profile;

    namespace {
        std::once_flag g_atexit_once;

        /**
         * @brief Handler atexit registrado por @c profile_init.
         *
         * Llama @c profile_dump() si el collector tiene @c output_path.
         */
        void atexit_dump_handler() {
            if (!g_profile.output_path.empty()) {
                profile_dump();
            }
        }
    } // namespace

    void profile_init(const std::string &output_path) {
        // Idempotente: si ya esta activo con el mismo path, no-op.
        if (g_profile.active.load(std::memory_order_relaxed)
            && g_profile.output_path == output_path) {
            return;
        }

        g_profile.output_path = output_path;
        g_profile.active.store(true, std::memory_order_release);

        // Registrar atexit handler una sola vez.
        std::call_once(g_atexit_once, []() {
            std::atexit(atexit_dump_handler);
        });
    }

    void profile_reset() {
        std::lock_guard<std::mutex> lk(g_profile.collector_mtx);
        g_profile.branches.clear();
        g_profile.callsites.clear();
        g_profile.allocs.clear();
    }

    void profile_branch(uint64_t pc, bool taken) {
        // Lookup / insert bajo lock para evitar race con rehash.
        // El insert solo ocurre 1 vez por PC; los hits subsecuentes
        // son lock-free reads + atomic fetch_add.
        BranchCounter *bc = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_profile.collector_mtx);
            auto it = g_profile.branches.find(pc);
            if (it == g_profile.branches.end()) {
                // Emplace devuelve el iterador a la nueva entrada.
                auto res = g_profile.branches.emplace(
                    std::piecewise_construct,
                    std::forward_as_tuple(pc),
                    std::forward_as_tuple());
                bc = &res.first->second;
            } else {
                bc = &it->second;
            }
        }

        if (taken) {
            bc->taken.fetch_add(1, std::memory_order_relaxed);
        } else {
            bc->not_taken.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void profile_callvirt(uint64_t pc, loader::ClassInfo *class_ptr) {
        if (class_ptr == nullptr) {
            return; // Defensive: callvirt con receptor invalido no se trackea.
        }

        std::lock_guard<std::mutex> lk(g_profile.collector_mtx);

        auto it = g_profile.callsites.find(pc);
        if (it == g_profile.callsites.end()) {
            auto res = g_profile.callsites.emplace(pc, CallSiteCounter{});
            it = res.first;
        }
        CallSiteCounter &cs = it->second;

        // Buscar la clase ya observada.
        for (uint8_t i = 0; i < cs.n_types; ++i) {
            if (cs.types[i].class_ptr == class_ptr) {
                cs.types[i].count++;
                return;
            }
        }

        // No esta en la lista: si hay espacio, anyadir.
        if (cs.n_types < 4) {
            cs.types[cs.n_types].class_ptr = class_ptr;
            cs.types[cs.n_types].count = 1;
            // Capturar nombre AHORA (ClassRegistry vivo).  Al dump
            // ClassRegistry ya esta destruido y dereferenciar
            // class_ptr->name seria UB.
            if (class_ptr->name.size > 0 && class_ptr->name.data != nullptr) {
                cs.types[cs.n_types].class_name.assign(
                    reinterpret_cast<const char *>(class_ptr->name.data),
                    static_cast<size_t>(class_ptr->name.size));
            } else {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "0x%llx",
                              (unsigned long long)(uintptr_t)class_ptr);
                cs.types[cs.n_types].class_name.assign(buf);
            }
            cs.n_types++;
            return;
        }

        // Lista llena: megamorphic.
        cs.megamorphic_count++;
    }

    void profile_newobj(uint64_t pc) {
        std::atomic<uint64_t> *cnt = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_profile.collector_mtx);
            auto it = g_profile.allocs.find(pc);
            if (it == g_profile.allocs.end()) {
                auto res = g_profile.allocs.emplace(
                    std::piecewise_construct,
                    std::forward_as_tuple(pc),
                    std::forward_as_tuple(0));
                cnt = &res.first->second;
            } else {
                cnt = &it->second;
            }
        }
        cnt->fetch_add(1, std::memory_order_relaxed);
    }

    namespace {
        /// Helpers para escritura binaria little-endian.
        void write_u16(std::ofstream &out, uint16_t v) {
            unsigned char b[2] = { (unsigned char)(v & 0xFF),
                                   (unsigned char)((v >> 8) & 0xFF) };
            out.write(reinterpret_cast<const char *>(b), 2);
        }
        void write_u32(std::ofstream &out, uint32_t v) {
            unsigned char b[4];
            for (int i = 0; i < 4; ++i) b[i] = (unsigned char)((v >> (i * 8)) & 0xFF);
            out.write(reinterpret_cast<const char *>(b), 4);
        }
        void write_u64(std::ofstream &out, uint64_t v) {
            unsigned char b[8];
            for (int i = 0; i < 8; ++i) b[i] = (unsigned char)((v >> (i * 8)) & 0xFF);
            out.write(reinterpret_cast<const char *>(b), 8);
        }

        // class_name_of: helper eliminado.  Ahora cacheamos el nombre
        // en TypeObservation::class_name al momento de la observacion
        // porque al dump (atexit) el ClassRegistry ya esta destruido.
    } // namespace

    void profile_dump() {
        if (g_profile.output_path.empty()) return;

        // Snapshot bajo lock; despues escribimos sin lock.
        std::lock_guard<std::mutex> lk(g_profile.collector_mtx);

        std::ofstream out(g_profile.output_path, std::ios::binary);
        if (!out) {
            std::fprintf(stderr,
                "[profile] error: no se pudo abrir '%s' para dump\n",
                g_profile.output_path.c_str());
            return;
        }

        // Magic 'VPRF' little-endian = 0x46525056
        const uint32_t magic = 0x46525056u;
        write_u32(out, magic);
        write_u16(out, 1); // version
        write_u16(out, 0); // flags
        write_u32(out, (uint32_t)g_profile.branches.size());
        write_u32(out, (uint32_t)g_profile.callsites.size());
        write_u32(out, (uint32_t)g_profile.allocs.size());
        // reserved 12 bytes
        unsigned char zeros[12] = {0};
        out.write(reinterpret_cast<const char *>(zeros), 12);

        // Branches: [pc u64][taken u64][not_taken u64]
        for (auto &kv : g_profile.branches) {
            write_u64(out, kv.first);
            write_u64(out, kv.second.taken.load(std::memory_order_relaxed));
            write_u64(out, kv.second.not_taken.load(std::memory_order_relaxed));
        }

        // CallSites: [pc u64][n_types u8][_pad 7B][megamorphic u64]
        //            then n_types entries of [name_len u32][name][count u64]
        for (auto &kv : g_profile.callsites) {
            write_u64(out, kv.first);
            unsigned char hdr[8] = {0};
            hdr[0] = kv.second.n_types;
            out.write(reinterpret_cast<const char *>(hdr), 8);
            write_u64(out, kv.second.megamorphic_count);
            for (uint8_t i = 0; i < kv.second.n_types; ++i) {
                // Usar el nombre cacheado (class_ptr puede estar invalid).
                const std::string &name = kv.second.types[i].class_name;
                write_u32(out, (uint32_t)name.size());
                if (!name.empty()) {
                    out.write(name.data(), (std::streamsize)name.size());
                }
                write_u64(out, kv.second.types[i].count);
            }
        }

        // Allocs: [pc u64][count u64]
        for (auto &kv : g_profile.allocs) {
            write_u64(out, kv.first);
            write_u64(out, kv.second.load(std::memory_order_relaxed));
        }

        out.flush();
        out.close();

        std::fprintf(stderr,
            "[profile] dump OK: %s (branches=%zu callsites=%zu allocs=%zu)\n",
            g_profile.output_path.c_str(),
            g_profile.branches.size(),
            g_profile.callsites.size(),
            g_profile.allocs.size());
    }

} // namespace profile
} // namespace runtime
