/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file test_shared_handle_table.cpp
 * @brief Z.5: tests del SharedHandleTable lock-free.
 *
 * Cobertura:
 * - Single-thread: register / lookup / unregister.
 * - Multi-thread: register concurrente desde N threads.
 * - Free list: unregister + register reusa slots.
 * - ABA: stress register + unregister rapido.
 * - Bit 31: handles registrados tienen bit 31 set; SHARED_NULL_HANDLE = 0.
 */

#include "gc/shared_handle_table.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

static int g_tests_run    = 0;
static int g_tests_passed = 0;

#define CHECK(cond, msg) do {                                          \
    ++g_tests_run;                                                      \
    if (cond) {                                                         \
        ++g_tests_passed;                                               \
        std::printf("  [OK] %s\n", msg);                                \
    } else {                                                            \
        std::printf("  [FAIL] %s\n", msg);                              \
    }                                                                   \
} while (0)

// ----------------------------------------------------------------------
// Test 1: register / lookup / unregister single-thread
// ----------------------------------------------------------------------

static void test_basic() {
    std::printf("\n=== Test 1: register/lookup/unregister single-thread ===\n");
    gc::SharedHandleTable t;
    CHECK(t.init_ok(), "tabla inicializa OK");

    uint8_t obj1[32];
    uint8_t obj2[64];

    uint32_t h1 = t.register_object(obj1, sizeof(obj1));
    uint32_t h2 = t.register_object(obj2, sizeof(obj2));

    CHECK(h1 != gc::SHARED_NULL_HANDLE, "h1 != null");
    CHECK(h2 != gc::SHARED_NULL_HANDLE, "h2 != null");
    CHECK(h1 != h2, "h1 != h2");
    CHECK((h1 & gc::SHARED_HANDLE_BIT) != 0, "h1 tiene bit 31 SHARED set");
    CHECK((h2 & gc::SHARED_HANDLE_BIT) != 0, "h2 tiene bit 31 SHARED set");

    CHECK(t.lookup(h1) == obj1, "lookup(h1) == obj1");
    CHECK(t.lookup(h2) == obj2, "lookup(h2) == obj2");
    CHECK(t.live_count() == 2, "live_count == 2");

    t.unregister(h1);
    CHECK(t.lookup(h1) == nullptr, "lookup(h1) tras unregister == nullptr");
    CHECK(t.lookup(h2) == obj2, "h2 sigue valido tras unregister h1");
    CHECK(t.live_count() == 1, "live_count == 1");

    t.unregister(h2);
    CHECK(t.live_count() == 0, "live_count == 0 tras unregister total");
}

// ----------------------------------------------------------------------
// Test 2: handles invalidos
// ----------------------------------------------------------------------

static void test_invalid_handles() {
    std::printf("\n=== Test 2: handles invalidos ===\n");
    gc::SharedHandleTable t;
    CHECK(t.lookup(gc::SHARED_NULL_HANDLE) == nullptr,
          "lookup(NULL) == nullptr");
    CHECK(t.lookup(gc::SHARED_HANDLE_BIT | 9999999u) == nullptr,
          "lookup de idx no alocado == nullptr");
    // unregister de invalid handle no debe crashear
    t.unregister(0);
    t.unregister(gc::SHARED_HANDLE_BIT | 9999999u);
    CHECK(true, "unregister de handles invalidos no crashea");
}

// ----------------------------------------------------------------------
// Test 3: free list reusa slots
// ----------------------------------------------------------------------

static void test_free_list_reuse() {
    std::printf("\n=== Test 3: free list reusa slots tras unregister ===\n");
    gc::SharedHandleTable t;
    uint8_t buf[16];

    uint32_t first = t.register_object(buf, 16);
    t.unregister(first);
    uint32_t second = t.register_object(buf, 16);
    // El segundo register debe reusar el slot liberado (mismo indice)
    CHECK((first & gc::SHARED_HANDLE_MASK) == (second & gc::SHARED_HANDLE_MASK),
          "free list reusa el mismo indice tras unregister + register");
    t.unregister(second);
}

// ----------------------------------------------------------------------
// Test 4: many registers + verify uniqueness
// ----------------------------------------------------------------------

static void test_many_registers() {
    std::printf("\n=== Test 4: 10K registers + verify unicidad ===\n");
    gc::SharedHandleTable t;
    constexpr int N = 10000;
    std::vector<uint32_t> handles;
    std::vector<uint8_t *> ptrs;
    handles.reserve(N);
    ptrs.reserve(N);
    for (int i = 0; i < N; ++i) {
        uint8_t *p = new uint8_t[32];
        ptrs.push_back(p);
        handles.push_back(t.register_object(p, 32));
    }
    // Verificar que cada handle resuelve a su propio ptr
    bool ok = true;
    for (int i = 0; i < N; ++i) {
        if (t.lookup(handles[i]) != ptrs[i]) { ok = false; break; }
    }
    CHECK(ok, "cada handle resuelve a su propio ptr (10K)");
    // Verificar que todos son distintos
    std::set<uint32_t> unique(handles.begin(), handles.end());
    CHECK(unique.size() == (size_t)N, "10K handles unicos");
    // Cleanup
    for (int i = 0; i < N; ++i) {
        t.unregister(handles[i]);
        delete[] ptrs[i];
    }
}

// ----------------------------------------------------------------------
// Test 5: concurrent register desde N threads
// ----------------------------------------------------------------------

static void test_concurrent_register() {
    std::printf("\n=== Test 5: 8 threads x 5K register concurrente ===\n");
    gc::SharedHandleTable t;
    constexpr int THREADS = 8;
    constexpr int PER = 5000;

    std::vector<std::thread> threads;
    std::vector<std::vector<uint32_t>> handles(THREADS);
    std::vector<std::vector<uint8_t *>> ptrs(THREADS);
    threads.reserve(THREADS);

    for (int tid = 0; tid < THREADS; ++tid) {
        handles[tid].reserve(PER);
        ptrs[tid].reserve(PER);
        threads.emplace_back([&t, &handles, &ptrs, tid]() {
            for (int i = 0; i < PER; ++i) {
                uint8_t *p = new uint8_t[24];
                ptrs[tid].push_back(p);
                handles[tid].push_back(t.register_object(p, 24));
            }
        });
    }
    for (auto &th : threads) th.join();

    // Recolectar todos los handles + verificar unicidad + lookup correcto
    std::set<uint32_t> all;
    int errors = 0;
    for (int tid = 0; tid < THREADS; ++tid) {
        for (int i = 0; i < PER; ++i) {
            uint32_t h = handles[tid][i];
            if (h == gc::SHARED_NULL_HANDLE) { errors++; continue; }
            if (!all.insert(h).second) errors++; // duplicado
            if (t.lookup(h) != ptrs[tid][i]) errors++;
        }
    }
    CHECK(errors == 0, "0 errores en register concurrente (unicidad + lookup)");
    CHECK(all.size() == (size_t)(THREADS * PER),
          "total handles registrados == THREADS * PER");

    // Cleanup
    for (int tid = 0; tid < THREADS; ++tid) {
        for (auto h : handles[tid]) t.unregister(h);
        for (auto *p : ptrs[tid]) delete[] p;
    }
}

// ----------------------------------------------------------------------
// Test 6: ABA stress (register + unregister rapido en threads distintos)
// ----------------------------------------------------------------------

static void test_aba_stress() {
    std::printf("\n=== Test 6: ABA stress 4 threads x 20K ops ===\n");
    gc::SharedHandleTable t;
    constexpr int THREADS = 4;
    constexpr int OPS = 20000;

    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    threads.reserve(THREADS);

    for (int tid = 0; tid < THREADS; ++tid) {
        threads.emplace_back([&t, &errors, tid]() {
            uint8_t buf[16];
            // Marcar pattern unico
            std::memset(buf, (uint8_t)(tid & 0xFF), sizeof(buf));
            for (int i = 0; i < OPS; ++i) {
                uint32_t h = t.register_object(buf, 16);
                if (h == gc::SHARED_NULL_HANDLE) { errors.fetch_add(1); continue; }
                if (t.lookup(h) != buf) errors.fetch_add(1);
                t.unregister(h);
            }
        });
    }
    for (auto &th : threads) th.join();

    CHECK(errors.load() == 0, "0 errores en ABA stress");
    CHECK(t.live_count() == 0, "tabla vacia tras balance register/unregister");
    std::printf("  (info) chunks_alocados=%u\n", t.chunk_count());
}

// ----------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------

int main() {
    std::printf("=== Z.5: SharedHandleTable tests ===\n");
    test_basic();
    test_invalid_handles();
    test_free_list_reuse();
    test_many_registers();
    test_concurrent_register();
    test_aba_stress();

    std::printf("\n=== Resultados ===\n");
    std::printf("Pasaron: %d/%d\n", g_tests_passed, g_tests_run);
    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
