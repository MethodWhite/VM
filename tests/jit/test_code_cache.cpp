/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/jit/test_code_cache.cpp
 * @brief Tests del @c jit::CodeCache: alocacion, ejecucion de codigo
 *        nativo escrito a mano, commit + flush icache, invalidacion.
 *
 * Cubre el contrato basico que necesita Phase D (JIT):
 *
 *   1. @c alloc devuelve memoria escribible.
 *   2. Tras @c commit la memoria es ejecutable.
 *   3. Codigo x86-64 trivial (@c mov eax, IMM ; ret) corre y devuelve
 *      el valor esperado.
 *   4. @c contains detecta correctamente punteros del cache.
 *   5. @c invalidate sobrescribe con INT3.
 *   6. @c used_bytes refleja el bump pointer.
 */

#include "jit/code_cache.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

    int fail_count = 0;
    int pass_count = 0;

    #define CHECK(cond, msg) do {                                            \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL: %s (linea %d)\n", msg, __LINE__);    \
            ++fail_count;                                                    \
        } else {                                                             \
            ++pass_count;                                                    \
        }                                                                    \
    } while (0)

    /**
     * @brief Codigo x86-64 que retorna 42 como @c uint32_t.
     *
     *   b8 2a 00 00 00     mov  eax, 0x2A
     *   c3                 ret
     */
    constexpr uint8_t kCodeReturn42[] = {
        0xB8, 0x2A, 0x00, 0x00, 0x00,   // mov eax, 42
        0xC3                              // ret
    };

    /**
     * @brief Codigo x86-64 que retorna @c rcx (Win64) o @c rdi (SysV)
     *        sin tocarlo: arg1 -> rax -> ret.  Util para verificar el
     *        bridge.
     *
     *   SysV: mov rax, rdi ; ret      -> 48 89 f8 c3
     *   Win64: mov rax, rcx ; ret      -> 48 89 c8 c3
     */
#if defined(_WIN32)
    constexpr uint8_t kCodeIdentity[] = {
        0x48, 0x89, 0xC8,    // mov rax, rcx
        0xC3                  // ret
    };
#else
    constexpr uint8_t kCodeIdentity[] = {
        0x48, 0x89, 0xF8,    // mov rax, rdi
        0xC3                  // ret
    };
#endif

    /** @brief Test 1: aloc + ejecucion de codigo trivial. */
    void test_alloc_and_run() {
        jit::CodeCache cache;
        CHECK(cache.chunk_count() == 0, "cache vacio al inicio");
        CHECK(cache.used_bytes() == 0, "used_bytes=0 al inicio");

        uint8_t *ptr = cache.alloc(sizeof(kCodeReturn42), 16);
        CHECK(ptr != nullptr, "alloc devuelve no-null");
        CHECK(cache.contains(ptr), "contains() reconoce ptr alocado");
        CHECK(cache.chunk_count() == 1, "primer alloc reserva 1 chunk");

        std::memcpy(ptr, kCodeReturn42, sizeof(kCodeReturn42));
        cache.commit(ptr, sizeof(kCodeReturn42));

        using Fn = uint32_t(*)();
        Fn fn   = reinterpret_cast<Fn>(ptr);
        uint32_t r = fn();
        CHECK(r == 42, "codigo nativo devuelve 42");
    }

    /** @brief Test 2: bridge con un argumento (identity function). */
    void test_identity_with_arg() {
        jit::CodeCache cache;
        uint8_t *ptr = cache.alloc(sizeof(kCodeIdentity), 16);
        CHECK(ptr != nullptr, "alloc identity");
        std::memcpy(ptr, kCodeIdentity, sizeof(kCodeIdentity));
        cache.commit(ptr, sizeof(kCodeIdentity));

        using Fn = uint64_t(*)(uint64_t);
        Fn fn = reinterpret_cast<Fn>(ptr);

        CHECK(fn(0xDEADBEEF) == 0xDEADBEEF, "identity preserva 0xDEADBEEF");
        CHECK(fn(0)          == 0,          "identity preserva 0");
        CHECK(fn(UINT64_MAX) == UINT64_MAX, "identity preserva UINT64_MAX");
    }

    /** @brief Test 3: usar el cache para alocaciones multiples y diferentes alignments. */
    void test_multiple_allocs() {
        jit::CodeCache cache;
        uint8_t *a = cache.alloc(16, 16);
        uint8_t *b = cache.alloc(32, 16);
        uint8_t *c = cache.alloc(8, 8);
        CHECK(a != nullptr && b != nullptr && c != nullptr, "tres allocs ok");
        CHECK(a != b && b != c, "punteros distintos");
        CHECK(b > a, "bump pointer crece hacia arriba");
        CHECK((reinterpret_cast<uintptr_t>(a) % 16) == 0, "a alineado a 16");
        CHECK((reinterpret_cast<uintptr_t>(b) % 16) == 0, "b alineado a 16");
        CHECK((reinterpret_cast<uintptr_t>(c) % 8)  == 0, "c alineado a 8");
        CHECK(cache.used_bytes() >= 16 + 32 + 8, "used >= sum");
    }

    /** @brief Test 4: invalidar codigo emite INT3 (0xCC). */
    void test_invalidate() {
        jit::CodeCache cache;
        uint8_t *ptr = cache.alloc(sizeof(kCodeReturn42), 16);
        std::memcpy(ptr, kCodeReturn42, sizeof(kCodeReturn42));
        cache.commit(ptr, sizeof(kCodeReturn42));

        cache.invalidate(ptr, sizeof(kCodeReturn42));
        for (size_t i = 0; i < sizeof(kCodeReturn42); ++i) {
            CHECK(ptr[i] == 0xCC, "byte tras invalidate = 0xCC");
        }
    }

    /** @brief Test 5: alocacion grande que fuerza un segundo chunk. */
    void test_multi_chunk() {
        const size_t kSmallChunk = 4096;
        const size_t kMaxTotal   = 16 * 4096;
        jit::CodeCache cache(kSmallChunk, kMaxTotal);

        /* Aloc casi todo el primer chunk. */
        uint8_t *a = cache.alloc(kSmallChunk - 64, 16);
        CHECK(a != nullptr, "primer alloc grande");
        CHECK(cache.chunk_count() == 1, "1 chunk tras alloc grande");

        /* Siguiente alloc no cabe en este chunk -> nuevo chunk. */
        uint8_t *b = cache.alloc(2000, 16);
        CHECK(b != nullptr, "segundo alloc disparara nuevo chunk");
        CHECK(cache.chunk_count() == 2, "2 chunks reservados");

        CHECK(cache.contains(a), "contains() reconoce a");
        CHECK(cache.contains(b), "contains() reconoce b");
    }

    /** @brief Test 6: OOM cuando se supera max_total_bytes. */
    void test_oom() {
        const size_t kChunk = 4096;
        const size_t kMax   = 2 * 4096;
        jit::CodeCache cache(kChunk, kMax);

        uint8_t *a = cache.alloc(kChunk - 64, 16);  /* fills chunk 1 */
        uint8_t *b = cache.alloc(kChunk - 64, 16);  /* chunk 2 */
        CHECK(a != nullptr && b != nullptr, "primeros dos allocs ok");
        CHECK(cache.chunk_count() == 2, "ambos chunks reservados");

        /* Tercer alloc deberia exceder kMax. */
        uint8_t *c = cache.alloc(kChunk - 64, 16);
        CHECK(c == nullptr, "tercer alloc retorna NULL (OOM)");
    }

} // namespace anonymous

int main() {
    test_alloc_and_run();
    test_identity_with_arg();
    test_multiple_allocs();
    test_invalidate();
    test_multi_chunk();
    test_oom();

    std::printf("test_code_cache: %d pass, %d fail\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
