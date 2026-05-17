/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/code_cache.h
 * @brief Code cache para alojar bytes de codigo nativo generados en
 *        runtime por el JIT (Phase D).
 *
 * = Diseno =
 *
 * El code cache reserva memoria con permisos @c PROT_READ|PROT_WRITE|
 * @c PROT_EXEC (POSIX) o @c PAGE_EXECUTE_READWRITE (Win32) en chunks
 * de 1 MiB, hasta un total maximo configurable (default 64 MiB).  Los
 * bytes emitidos por el JIT se copian al cache y se hace flush de la
 * instruction cache antes de la primera ejecucion.
 *
 * = W^X awareness =
 *
 * macOS y algunos Linux endurecidos prohiben paginas RWX simultaneas
 * (mitigacion W^X).  Para esos targets, esta clase soporta un modo
 * dual mediante @c mprotect/VirtualProtect: las paginas comienzan
 * RW durante la emision y se transicionan a RX antes de la primera
 * ejecucion.  Activado via define @c VESTA_JIT_WX_DUAL_MODE en
 * tiempo de compilacion.  Default: RWX simple (compatible con
 * Windows + Linux estandar).
 *
 * = Alineacion =
 *
 * Las alocaciones se alinean al @c align solicitado (default 16
 * bytes, suficiente para funciones).  El cache mantiene un bump
 * pointer por chunk; cuando no cabe, se reserva el siguiente chunk.
 *
 * = Concurrencia =
 *
 * En v1 el cache es SINGLE-WRITER: solo el thread compilador del JIT
 * hace @c alloc + @c commit.  Multiples threads pueden
 * leer/ejecutar codigo concurrentemente sin sincronizacion (es read-
 * only post-commit).  En Phase D.8 (C2 con multi-thread compile) se
 * anyadira un mutex interno o se shardara el cache por compile worker.
 *
 * = Cleanup =
 *
 * El destructor libera todos los chunks via @c VirtualFree/munmap.
 * Codigo nativo se invalida automaticamente.  Para invalidar una
 * funcion individual (deopt), llama @c invalidate(ptr) que rellena
 * los bytes con @c 0xCC (INT3 = breakpoint, garantiza crash si se
 * vuelve a llamar inadvertidamente).
 */

#ifndef VESTA_JIT_CODE_CACHE_H
#define VESTA_JIT_CODE_CACHE_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace jit {

    /**
     * @class CodeCache
     * @brief Pool de paginas executable-writable para codigo JIT.
     */
    class CodeCache {
    public:
        /**
         * @brief Construye un cache con maximo @p max_total_bytes.
         * @param chunk_bytes tamano de cada chunk de reserva (default
         *                    1 MiB).  Debe ser potencia de 2.
         * @param max_total_bytes capacidad total (default 64 MiB).  Cuando
         *                    se alcanza, @c alloc devuelve nullptr.
         */
        explicit CodeCache(size_t chunk_bytes     = 1u * 1024u * 1024u,
                           size_t max_total_bytes = 64u * 1024u * 1024u);

        /** @brief Libera todos los chunks. */
        ~CodeCache();

        CodeCache(const CodeCache &)            = delete;
        CodeCache &operator=(const CodeCache &) = delete;

        /**
         * @brief Reserva @p size bytes alineados a @p align.
         * @return puntero al primer byte reservado, o @c nullptr si OOM
         *         (alcanzado el limite total).  La memoria devuelta es
         *         R/W (escribible para emit).  El caller debe llamar
         *         @c commit cuando termine para hacer flush de icache.
         *
         * Multi-page alocations: si el chunk actual no cabe, se reserva
         * el siguiente y se aloca alli (el espacio restante del chunk
         * anterior se descarta - simple, sin compactacion).
         */
        uint8_t *alloc(size_t size, size_t align = 16);

        /**
         * @brief Marca @p size bytes desde @p ptr como listos para
         *        ejecutar.  Hace @c FlushInstructionCache (Win) o
         *        @c __builtin___clear_cache (POSIX) y, si se activo
         *        @c VESTA_JIT_WX_DUAL_MODE, transiciona la pagina a
         *        permisos RX.
         */
        void commit(const uint8_t *ptr, size_t size);

        /**
         * @brief Sobrescribe @p size bytes desde @p ptr con @c 0xCC
         *        (INT3 = breakpoint x86) para invalidar el codigo.
         *        Cualquier intento de ejecutar el codigo invalidado
         *        crasheara controladamente con SIGTRAP/EXCEPTION_BREAKPOINT.
         *
         * Usado para deopt: cuando una funcion C2 falla un
         * guard, se invalida y el caller se redirige al interprete.
         */
        void invalidate(uint8_t *ptr, size_t size);

        /** @brief Bytes alocados hasta ahora (suma de todos los chunks usados). */
        size_t used_bytes() const noexcept { return used_; }

        /** @brief Bytes maximos que el cache puede alcanzar. */
        size_t max_bytes() const noexcept { return max_total_; }

        /** @brief Numero de chunks reservados al sistema. */
        size_t chunk_count() const noexcept { return chunks_.size(); }

        /**
         * @brief Comprueba si @p ptr esta dentro del cache (cualquier chunk).
         *        Util para validar punteros antes de llamar @c invalidate.
         */
        bool contains(const uint8_t *ptr) const noexcept;

    private:
        struct Chunk {
            uint8_t *base   = nullptr; ///< inicio de la pagina (page-aligned)
            size_t   size   = 0;       ///< tamano total reservado
            size_t   used   = 0;       ///< bump pointer dentro del chunk
        };

        /// Reserva un nuevo chunk del SO con permisos RWX (o RW si W^X).
        bool reserve_chunk();

        /// Activa permisos de ejecucion para una pagina (no-op en modo RWX).
        void transition_to_executable(uint8_t *ptr, size_t size);

        /// Flush icache (CPU-specific).
        void flush_icache(const uint8_t *ptr, size_t size);

        std::vector<Chunk> chunks_;
        size_t             chunk_bytes_;
        size_t             max_total_;
        size_t             used_;
    };

} // namespace jit

#endif // VESTA_JIT_CODE_CACHE_H
