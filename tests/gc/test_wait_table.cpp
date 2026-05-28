/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file test_wait_table.cpp
 * @brief Z.4: tests del WaitTable lock-free per-bucket.
 */

#include "gc/wait_table.h"

#include <atomic>
#include <cstdio>
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
// Test 1: push / pop single-thread, FIFO order
// ----------------------------------------------------------------------

static void test_fifo_single_thread() {
    std::printf("\n=== Test 1: push/pop FIFO single-thread ===\n");
    gc::WaitTable t;
    t.push(100, gc::WaitKind::MONITOR, 1);
    t.push(100, gc::WaitKind::MONITOR, 2);
    t.push(100, gc::WaitKind::MONITOR, 3);

    CHECK(t.pop_one(100, gc::WaitKind::MONITOR) == 1, "pop_one() devuelve el primero (1)");
    CHECK(t.pop_one(100, gc::WaitKind::MONITOR) == 2, "pop_one() devuelve el segundo (2)");
    CHECK(t.pop_one(100, gc::WaitKind::MONITOR) == 3, "pop_one() devuelve el tercero (3)");
    CHECK(t.pop_one(100, gc::WaitKind::MONITOR) == UINT64_MAX,
          "pop_one() empty -> UINT64_MAX (sentinel; 0 es PID valido)");
}

// ----------------------------------------------------------------------
// Test 2: pop_all drena en orden FIFO
// ----------------------------------------------------------------------

static void test_pop_all() {
    std::printf("\n=== Test 2: pop_all drena en FIFO ===\n");
    gc::WaitTable t;
    for (uint64_t i = 1; i <= 10; ++i) {
        t.push(42, gc::WaitKind::CONDVAR, i);
    }
    auto all = t.pop_all(42, gc::WaitKind::CONDVAR);
    CHECK(all.size() == 10, "pop_all devuelve 10 elementos");
    bool order_ok = true;
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i] != (uint64_t)(i + 1)) { order_ok = false; break; }
    }
    CHECK(order_ok, "orden FIFO preservado");
    auto empty = t.pop_all(42, gc::WaitKind::CONDVAR);
    CHECK(empty.empty(), "pop_all sobre cola vacia devuelve vacio");
}

// ----------------------------------------------------------------------
// Test 3: MONITOR y CONDVAR son colas independientes (fix t13)
// ----------------------------------------------------------------------

static void test_kinds_independent() {
    std::printf("\n=== Test 3: MONITOR/CONDVAR independientes para mismo handle ===\n");
    gc::WaitTable t;
    t.push(7, gc::WaitKind::MONITOR, 100);
    t.push(7, gc::WaitKind::MONITOR, 200);
    t.push(7, gc::WaitKind::CONDVAR, 300);
    t.push(7, gc::WaitKind::CONDVAR, 400);

    // pop_one del MONITOR no debe sacar nada del CONDVAR
    CHECK(t.pop_one(7, gc::WaitKind::MONITOR) == 100, "monitor pop devuelve 100");
    CHECK(t.pop_one(7, gc::WaitKind::CONDVAR) == 300, "condvar pop devuelve 300 (intacto)");
    CHECK(t.pop_one(7, gc::WaitKind::MONITOR) == 200, "monitor pop devuelve 200");
    CHECK(t.pop_one(7, gc::WaitKind::CONDVAR) == 400, "condvar pop devuelve 400");
    CHECK(t.pop_one(7, gc::WaitKind::MONITOR) == UINT64_MAX, "monitor cola vacia (UINT64_MAX)");
    CHECK(t.pop_one(7, gc::WaitKind::CONDVAR) == UINT64_MAX, "condvar cola vacia (UINT64_MAX)");
}

// ----------------------------------------------------------------------
// Test 4: handles distintos no se mezclan (hash collision safe)
// ----------------------------------------------------------------------

static void test_handles_independent() {
    std::printf("\n=== Test 4: handles distintos no comparten cola ===\n");
    gc::WaitTable t;
    // Probar varios handles incluyendo potenciales colisiones de hash
    for (uint32_t h = 1; h <= 100; ++h) {
        t.push(h, gc::WaitKind::MONITOR, h * 10);
    }
    bool all_ok = true;
    for (uint32_t h = 1; h <= 100; ++h) {
        uint64_t got = t.pop_one(h, gc::WaitKind::MONITOR);
        if (got != (uint64_t)(h * 10)) { all_ok = false; break; }
    }
    CHECK(all_ok, "cada handle devuelve su propio pid (100 distintos)");
    CHECK(t.total_waiters() == 0, "todas las colas vacias tras drain");
}

// ----------------------------------------------------------------------
// Test 5: push concurrente desde N threads sobre el mismo (handle, kind)
//          no pierde ningun encoded_pid
// ----------------------------------------------------------------------

static void test_concurrent_push_same_queue() {
    std::printf("\n=== Test 5: 8 threads x 5K push sobre misma cola, drain final ===\n");
    gc::WaitTable t;
    constexpr int THREADS = 8;
    constexpr int PER = 5000;

    std::vector<std::thread> threads;
    threads.reserve(THREADS);
    for (int tid = 0; tid < THREADS; ++tid) {
        threads.emplace_back([&t, tid]() {
            uint64_t base = (uint64_t)(tid + 1) * 1000000ULL;
            for (int i = 0; i < PER; ++i) {
                t.push(42, gc::WaitKind::MONITOR, base + i);
            }
        });
    }
    for (auto &th : threads) th.join();

    auto all = t.pop_all(42, gc::WaitKind::MONITOR);
    CHECK(all.size() == (size_t)(THREADS * PER),
          "pop_all devuelve THREADS*PER elementos (sin perdida)");

    // Verificar unicidad: cada encoded_pid es unico
    std::set<uint64_t> unique(all.begin(), all.end());
    CHECK(unique.size() == all.size(),
          "todos los encoded_pids son unicos (sin duplicados)");
}

// ----------------------------------------------------------------------
// Test 6: push/pop concurrente sobre colas distintas (zero contention path)
// ----------------------------------------------------------------------

static void test_concurrent_push_pop_disjoint() {
    std::printf("\n=== Test 6: 8 threads x 10K ops en colas distintas (cero contention) ===\n");
    gc::WaitTable t;
    constexpr int THREADS = 8;
    constexpr int OPS = 10000;

    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    threads.reserve(THREADS);
    for (int tid = 0; tid < THREADS; ++tid) {
        threads.emplace_back([&t, &errors, tid]() {
            uint32_t my_handle = (uint32_t)(tid + 1000); // handle unico por thread
            for (int i = 0; i < OPS; ++i) {
                uint64_t pid = (uint64_t)(tid + 1) * 1000000ULL + (uint64_t)i;
                t.push(my_handle, gc::WaitKind::CONDVAR, pid);
                uint64_t got = t.pop_one(my_handle, gc::WaitKind::CONDVAR);
                if (got != pid) errors.fetch_add(1);
            }
        });
    }
    for (auto &th : threads) th.join();

    CHECK(errors.load() == 0, "0 errores en ops alternadas (FIFO mantenido por thread)");
    CHECK(t.total_waiters() == 0, "tabla vacia tras balance push/pop");
}

// ----------------------------------------------------------------------
// Test 7: stress simultaneo MONITOR + CONDVAR sobre mismo handle
// ----------------------------------------------------------------------

static void test_concurrent_mixed_kinds() {
    std::printf("\n=== Test 7: 6 threads, 2 kinds, mismo handle, 5K push/pop ===\n");
    gc::WaitTable t;
    constexpr int THREADS = 6;
    constexpr int OPS = 5000;

    std::vector<std::thread> threads;
    threads.reserve(THREADS);
    std::atomic<int> pushes_mon{0}, pops_mon{0}, pushes_con{0}, pops_con{0};

    for (int tid = 0; tid < THREADS; ++tid) {
        threads.emplace_back([&, tid]() {
            gc::WaitKind k = (tid % 2 == 0) ? gc::WaitKind::MONITOR
                                            : gc::WaitKind::CONDVAR;
            uint64_t base = (uint64_t)(tid + 1) * 1000000ULL;
            for (int i = 0; i < OPS; ++i) {
                t.push(99, k, base + i);
                if (k == gc::WaitKind::MONITOR) pushes_mon.fetch_add(1);
                else pushes_con.fetch_add(1);
                if ((i & 1) == 0) {
                    uint64_t got = t.pop_one(99, k);
                    if (got != 0) {
                        if (k == gc::WaitKind::MONITOR) pops_mon.fetch_add(1);
                        else pops_con.fetch_add(1);
                    }
                }
            }
        });
    }
    for (auto &th : threads) th.join();

    // Verificar que la tabla queda en estado consistente
    auto mon_rest = t.pop_all(99, gc::WaitKind::MONITOR);
    auto con_rest = t.pop_all(99, gc::WaitKind::CONDVAR);

    int total_mon = pushes_mon.load() - pops_mon.load();
    int total_con = pushes_con.load() - pops_con.load();
    CHECK((int)mon_rest.size() == total_mon,
          "monitor rest size == pushes - pops");
    CHECK((int)con_rest.size() == total_con,
          "condvar rest size == pushes - pops");
    CHECK(t.total_waiters() == 0, "tabla vacia tras drain final");
    std::printf("  (info) pushes_mon=%d pops_mon=%d  pushes_con=%d pops_con=%d\n",
                pushes_mon.load(), pops_mon.load(),
                pushes_con.load(), pops_con.load());
}

// ----------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------

int main() {
    std::printf("=== Z.4: WaitTable lock-free tests ===\n");
    test_fifo_single_thread();
    test_pop_all();
    test_kinds_independent();
    test_handles_independent();
    test_concurrent_push_same_queue();
    test_concurrent_push_pop_disjoint();
    test_concurrent_mixed_kinds();

    std::printf("\n=== Resultados ===\n");
    std::printf("Pasaron: %d/%d\n", g_tests_passed, g_tests_run);
    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
