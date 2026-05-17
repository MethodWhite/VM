/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/code_cache.cpp
 * @brief Implementacion del @c jit::CodeCache.
 *
 * Win32: usa @c VirtualAlloc(MEM_RESERVE|MEM_COMMIT, PAGE_EXECUTE_READWRITE).
 * POSIX: usa @c mmap(MAP_ANON|MAP_PRIVATE, PROT_READ|PROT_WRITE|PROT_EXEC).
 *
 * Flush de icache:
 *   - Win32: @c FlushInstructionCache(GetCurrentProcess(), ptr, size).
 *   - POSIX: @c __builtin___clear_cache(ptr, ptr+size).
 *
 * Invalidacion: @c memset(ptr, 0xCC, size) -- @c INT3 en x86-64.
 */

#include "jit/code_cache.h"

#include <cstring>
#include <cstdlib>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <unistd.h>
#endif

namespace jit {

    namespace {
        /** @brief Round-up entero a multiplo de @p align (potencia de 2). */
        inline size_t round_up(size_t value, size_t align) noexcept {
            return (value + align - 1) & ~(align - 1);
        }
    } // namespace anonymous

    CodeCache::CodeCache(size_t chunk_bytes, size_t max_total_bytes)
        : chunk_bytes_(chunk_bytes),
          max_total_(max_total_bytes),
          used_(0) {
        /* Validar potencia de 2 (chunk_bytes debe ser >= page size). */
        if (chunk_bytes_ < 4096u) chunk_bytes_ = 4096u;
        /* Si max_total < chunk -> imposible alocar un solo chunk. */
        if (max_total_ < chunk_bytes_) max_total_ = chunk_bytes_;
    }

    CodeCache::~CodeCache() {
        for (auto &c : chunks_) {
            if (!c.base) continue;
#if defined(_WIN32)
            ::VirtualFree(c.base, 0, MEM_RELEASE);
#else
            ::munmap(c.base, c.size);
#endif
        }
        chunks_.clear();
    }

    bool CodeCache::reserve_chunk() {
        /* Verificar que cabe dentro del limite total. */
        const size_t total_reserved = chunks_.size() * chunk_bytes_;
        if (total_reserved + chunk_bytes_ > max_total_) {
            return false;
        }
#if defined(_WIN32)
        void *p = ::VirtualAlloc(nullptr,
                                 chunk_bytes_,
                                 MEM_RESERVE | MEM_COMMIT,
                                 PAGE_EXECUTE_READWRITE);
        if (!p) return false;
#else
        void *p = ::mmap(nullptr,
                         chunk_bytes_,
                         PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS,
                         -1, 0);
        if (p == MAP_FAILED) return false;
#endif
        Chunk c;
        c.base = static_cast<uint8_t *>(p);
        c.size = chunk_bytes_;
        c.used = 0;
        chunks_.push_back(c);
        return true;
    }

    uint8_t *CodeCache::alloc(size_t size, size_t align) {
        if (size == 0) return nullptr;
        if (align == 0) align = 1;
        /* Asegurar que align es potencia de 2.  Si no, redondear arriba
         * al siguiente power-of-two (bit_ceil estilo C++20 simplificado). */
        if ((align & (align - 1)) != 0) {
            size_t a = 1;
            while (a < align) a <<= 1;
            align = a;
        }
        if (size > chunk_bytes_) return nullptr;  /* alocacion super-chunk no soportada */

        /* Intentar en el ultimo chunk; si no cabe, reservar otro. */
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (!chunks_.empty()) {
                Chunk      &c    = chunks_.back();
                const size_t base = round_up(reinterpret_cast<uintptr_t>(c.base + c.used), align)
                                  - reinterpret_cast<uintptr_t>(c.base);
                if (base + size <= c.size) {
                    uint8_t *ptr      = c.base + base;
                    const size_t prev = c.used;
                    c.used            = base + size;
                    used_            += c.used - prev;
                    return ptr;
                }
            }
            /* No cabe en el chunk actual; reservar el siguiente. */
            if (!reserve_chunk()) return nullptr;
        }
        return nullptr;
    }

    void CodeCache::commit(const uint8_t *ptr, size_t size) {
        if (!ptr || size == 0) return;
        /* En modo RWX simple no hay transicion de permisos.
         * En modo W^X (a implementar): mprotect a PROT_READ|PROT_EXEC. */
        transition_to_executable(const_cast<uint8_t *>(ptr), size);
        flush_icache(ptr, size);
    }

    void CodeCache::invalidate(uint8_t *ptr, size_t size) {
        if (!ptr || size == 0) return;
        if (!contains(ptr)) return;
        std::memset(ptr, 0xCC, size);  /* INT3 = breakpoint x86-64 */
        flush_icache(ptr, size);
    }

    bool CodeCache::contains(const uint8_t *ptr) const noexcept {
        if (!ptr) return false;
        for (const auto &c : chunks_) {
            if (ptr >= c.base && ptr < c.base + c.size) return true;
        }
        return false;
    }

    void CodeCache::transition_to_executable(uint8_t *ptr, size_t size) {
        /* Modo RWX: no-op.  Si en el futuro se anyade modo W^X,
         * esta funcion hace mprotect/VirtualProtect a RX. */
        (void)ptr; (void)size;
    }

    void CodeCache::flush_icache(const uint8_t *ptr, size_t size) {
#if defined(_WIN32)
        ::FlushInstructionCache(::GetCurrentProcess(), ptr, size);
#elif defined(__GNUC__) || defined(__clang__)
        /* Sin GCC builtin x86 hace lock+fence implicito al ejecutar tras
         * write (modelo coherente).  Pero __builtin___clear_cache es el
         * portable correcto y no hace nada en x86 (ya coherente). */
        __builtin___clear_cache(const_cast<char *>(reinterpret_cast<const char *>(ptr)),
                                const_cast<char *>(reinterpret_cast<const char *>(ptr + size)));
#else
        (void)ptr; (void)size;
#endif
    }

} // namespace jit
