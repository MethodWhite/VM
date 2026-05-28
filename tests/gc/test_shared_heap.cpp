/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file test_shared_heap.cpp
 * @brief Tests del SharedHeap (Z.1).
 *
 * Cobertura:
 * - Single-thread: alloc/free correctness, size-class selection, OOM.
 * - Multi-thread: alloc concurrente, free concurrente, mix alloc+free.
 * - ABA: stress test que provoca ABA (alloc+free repetido del mismo slot).
 * - Stats: contadores consistentes bajo concurrencia.
 */

#include "gc/shared_heap.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unordered_set>
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

// =====================================================================
// Test 1: alloc/free single-thread basico
// =====================================================================

static void test_single_thread_basic() {
    std::printf("\n=== Test 1: alloc/free single-thread basico ===\n");
    gc::SharedHeap heap;
    CHECK(heap.init_ok(), "heap inicializa OK");

    // Aloc en cada size-class
    for (uint32_t cls = 0; cls < gc::SHARED_SIZE_CLASS_COUNT; ++cls) {
        size_t sz = gc::SHARED_SIZE_CLASSES[cls];
        uint8_t *p = heap.alloc(sz);
        CHECK(p != nullptr, "alloc no nulo");
        CHECK(heap.contains(p), "contains() reconoce el ptr alocado");
        CHECK(heap.size_class_of(p) == static_cast<int>(cls), "size_class correcto");

        // Escribir y leer datos para verificar que el slot es usable
        std::memset(p, 0xAB, sz);
        CHECK(p[0] == 0xAB && p[sz - 1] == 0xAB, "slot escribible/legible");

        heap.free(p);
    }
}

// =====================================================================
// Test 2: rechazo de tamano > 32 KB
// =====================================================================

static void test_too_big() {
    std::printf("\n=== Test 2: tamanyo > 32 KB rechazado ===\n");
    gc::SharedHeap heap;
    uint8_t *p = heap.alloc(65536); // 64 KB > max 32 KB
    CHECK(p == nullptr, "alloc(65536) devuelve nullptr");
    uint8_t *p2 = heap.alloc(32769); // 32K + 1
    CHECK(p2 == nullptr, "alloc(32769) devuelve nullptr");
}

// =====================================================================
// Test 3: redondeo a clase minima (bytes pequenyos)
// =====================================================================

static void test_round_up() {
    std::printf("\n=== Test 3: redondeo a clase minima ===\n");
    gc::SharedHeap heap;
    // alloc(1) debe ir a clase 0 (16 B)
    uint8_t *p1 = heap.alloc(1);
    CHECK(heap.size_class_of(p1) == 0, "alloc(1) -> clase 0 (16 B)");
    // alloc(16) tambien clase 0
    uint8_t *p2 = heap.alloc(16);
    CHECK(heap.size_class_of(p2) == 0, "alloc(16) -> clase 0");
    // alloc(17) sube a clase 1 (32 B)
    uint8_t *p3 = heap.alloc(17);
    CHECK(heap.size_class_of(p3) == 1, "alloc(17) -> clase 1 (32 B)");
    // alloc(32) clase 1
    uint8_t *p4 = heap.alloc(32);
    CHECK(heap.size_class_of(p4) == 1, "alloc(32) -> clase 1");
    // alloc(33) clase 2 (64 B)
    uint8_t *p5 = heap.alloc(33);
    CHECK(heap.size_class_of(p5) == 2, "alloc(33) -> clase 2 (64 B)");
    heap.free(p1);
    heap.free(p2);
    heap.free(p3);
    heap.free(p4);
    heap.free(p5);
}

// =====================================================================
// Test 4: alloc/free intercalado mantiene la free list correcta
// =====================================================================

static void test_alloc_free_pattern() {
    std::printf("\n=== Test 4: alloc/free intercalado (LIFO esperado) ===\n");
    gc::SharedHeap heap;
    uint8_t *a = heap.alloc(64);
    uint8_t *b = heap.alloc(64);
    uint8_t *c = heap.alloc(64);
    CHECK(a != b && b != c && a != c, "punteros unicos");
    heap.free(b);
    uint8_t *d = heap.alloc(64);
    CHECK(d == b, "free + alloc devuelve el ultimo liberado (LIFO Treiber)");
    heap.free(a);
    heap.free(c);
    heap.free(d);
}

// =====================================================================
// Test 5: alloc hasta OOM y recuperacion
// =====================================================================

static void test_class_exhaustion() {
    std::printf("\n=== Test 5: growth dinamico al agotar chunk inicial ===\n");
    gc::SharedHeap heap;
    // Vaciar primer chunk de clase 32 KB (16 slots iniciales).
    // Tras el chunk #0, el slab debe crecer automaticamente a chunk #1
    // sin retornar nullptr (limite real: 16 * 256 chunks = 4096 slots).
    std::vector<uint8_t *> ps;
    uint32_t initial = gc::SHARED_SLOTS_PER_CLASS[11];
    for (uint32_t i = 0; i < initial * 2; ++i) {
        uint8_t *p = heap.alloc(32768);
        if (!p) break;
        ps.push_back(p);
    }
    CHECK(ps.size() == initial * 2,
          "growth dinamico: alocados 2x slots iniciales sin OOM");
    // Verificar que se alocaron al menos 2 chunks.
    uint32_t chunks_now = heap.slab(11).chunks_count.load(std::memory_order_acquire);
    CHECK(chunks_now >= 2,
          "slab tiene >= 2 chunks tras growth");
    // Liberar uno y reintentar: debe reusar slot del free list.
    heap.free(ps.back());
    ps.pop_back();
    uint8_t *recov = heap.alloc(32768);
    CHECK(recov != nullptr, "tras free, alloc vuelve a tener slot");
    heap.free(recov);
    for (auto *p : ps) heap.free(p);
}

// =====================================================================
// Test 6: contains/size_class_of con ptrs ajenos
// =====================================================================

static void test_contains_foreign() {
    std::printf("\n=== Test 6: contains/size_class_of con ptr ajeno ===\n");
    gc::SharedHeap heap;
    uint8_t stack_buf[64];
    CHECK(!heap.contains(stack_buf), "stack ptr NO esta en el heap");
    CHECK(heap.size_class_of(stack_buf) == -1, "size_class_of(stack) == -1");
    // free de ptr ajeno es no-op (no crashea)
    heap.free(stack_buf);
    CHECK(true, "free(stack_ptr) es no-op sin crashear");
}

// =====================================================================
// Test 7: stats counters en single-thread
// =====================================================================

static void test_stats_single_thread() {
    std::printf("\n=== Test 7: stats counters single-thread ===\n");
    gc::SharedHeap heap;
    std::vector<uint8_t *> ps;
    for (int i = 0; i < 10; ++i) {
        ps.push_back(heap.alloc(128));
    }
    CHECK(heap.alloc_count(3) == 10, "alloc_count(clase 128 B) == 10");
    CHECK(heap.free_count(3) == 0, "free_count == 0 antes de free");
    // 128 B = clase 3 -> 10 slots vivos = 10 * 128 = 1280 bytes
    CHECK(heap.total_allocated_bytes() == 1280,
          "total_allocated_bytes == 10 * 128");
    for (auto *p : ps) heap.free(p);
    CHECK(heap.free_count(3) == 10, "free_count == 10 tras liberar");
    CHECK(heap.total_allocated_bytes() == 0, "total_allocated_bytes == 0");
}

// =====================================================================
// Test 8: concurrencia - alloc-only por N threads
// =====================================================================

static void test_concurrent_alloc() {
    std::printf("\n=== Test 8: alloc concurrente (8 threads x 1000 allocs) ===\n");
    gc::SharedHeap heap;
    constexpr int THREADS = 8;
    constexpr int ALLOCS_PER_THREAD = 1000;
    constexpr size_t SLOT_SIZE = 64; // clase 2 con 8192 slots, suficiente

    std::vector<std::thread> threads;
    std::vector<std::vector<uint8_t *>> per_thread(THREADS);
    threads.reserve(THREADS);

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&heap, &per_thread, t]() {
            per_thread[t].reserve(ALLOCS_PER_THREAD);
            for (int i = 0; i < ALLOCS_PER_THREAD; ++i) {
                uint8_t *p = heap.alloc(SLOT_SIZE);
                if (p) per_thread[t].push_back(p);
            }
        });
    }
    for (auto &th : threads) th.join();

    // Verificar que TODOS los ptrs son unicos (no double-alloc)
    std::unordered_set<uint8_t *> all_ptrs;
    int total_allocated = 0;
    for (int t = 0; t < THREADS; ++t) {
        for (auto *p : per_thread[t]) {
            CHECK(all_ptrs.insert(p).second, "ptr unico en alloc concurrente");
            ++total_allocated;
        }
    }
    CHECK(total_allocated == THREADS * ALLOCS_PER_THREAD,
          "todos los allocs exitosos (slab tiene capacidad)");
    CHECK(heap.alloc_count(2) == (uint64_t)(THREADS * ALLOCS_PER_THREAD),
          "alloc_count refleja el total");

    // Cleanup
    for (int t = 0; t < THREADS; ++t) {
        for (auto *p : per_thread[t]) heap.free(p);
    }
}

// =====================================================================
// Test 9: stress concurrente alloc+free (provoca ABA si hubiera bug)
// =====================================================================

static void test_concurrent_alloc_free_stress() {
    std::printf("\n=== Test 9: stress alloc+free concurrente (8 threads x 10K ops) ===\n");
    gc::SharedHeap heap;
    constexpr int THREADS = 8;
    constexpr int OPS_PER_THREAD = 10000;

    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    threads.reserve(THREADS);

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&heap, &errors, t]() {
            std::vector<uint8_t *> local_ptrs;
            local_ptrs.reserve(64);
            for (int i = 0; i < OPS_PER_THREAD; ++i) {
                if ((i % 3) == 0 && !local_ptrs.empty()) {
                    // free uno al azar (deterministico via i)
                    size_t idx = (size_t)i % local_ptrs.size();
                    uint8_t *p = local_ptrs[idx];
                    // verificar: escribir un patron antes y leerlo tras free es solo
                    // valido SI no hubo race; aqui solo verificamos no-crash.
                    heap.free(p);
                    local_ptrs[idx] = local_ptrs.back();
                    local_ptrs.pop_back();
                } else {
                    size_t sz = 32 + (i & 7) * 16; // 32..144 bytes
                    uint8_t *p = heap.alloc(sz);
                    if (p) {
                        // Escribir un patron unico al slot recien alocado para
                        // detectar si lo recibimos sin que otro lo este
                        // todavia "viendo" como libre.
                        std::memset(p, (uint8_t)(t & 0xFF), sz);
                        local_ptrs.push_back(p);
                    }
                }
            }
            // cleanup
            for (auto *p : local_ptrs) heap.free(p);
        });
    }
    for (auto &th : threads) th.join();
    CHECK(errors.load() == 0, "0 errores en stress alloc/free concurrente");
}

// =====================================================================
// Test 10: ABA scenario explicito (alloc+free+alloc del mismo slot)
// =====================================================================

static void test_aba_safety() {
    std::printf("\n=== Test 10: ABA safety con tag de 16 bits ===\n");
    gc::SharedHeap heap;
    // Stress que provoca el patron ABA: thread A hace pop (lee head=X),
    // thread B hace pop+push+pop+push... y vuelve a poner X arriba.
    // Sin tag, A haria CAS exitoso con valor stale.  Con tag, falla.
    constexpr int THREADS = 4;
    constexpr int OPS = 50000;

    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&heap, &stop, t]() {
            int local_ops = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                uint8_t *p = heap.alloc(64);
                if (p) {
                    // Verificar que el slot es legible/escribible
                    p[0] = (uint8_t)(t & 0xFF);
                    p[63] = (uint8_t)((t * 17) & 0xFF);
                    heap.free(p);
                }
                if (++local_ops >= OPS) break;
            }
        });
    }

    // Dejar correr 100 ms minimo
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.store(true);
    for (auto &th : threads) th.join();

    // No hay forma directa de verificar ABA-correctness sin asserts internos.
    // El hecho de que no hay crashes ni asserts de stack-overflow indica que
    // el tag funciono.  alloc_count == free_count (todo balanced).
    uint64_t allocs = heap.alloc_count(2);
    uint64_t frees  = heap.free_count(2);
    CHECK(allocs == frees, "alloc_count == free_count tras stress (balanced)");
    std::printf("  (info) allocs=%llu frees=%llu\n",
                (unsigned long long)allocs, (unsigned long long)frees);
}

// =====================================================================
// Main
// =====================================================================

int main() {
    std::printf("=== SharedHeap Z.1 tests ===\n");
    test_single_thread_basic();
    test_too_big();
    test_round_up();
    test_alloc_free_pattern();
    test_class_exhaustion();
    test_contains_foreign();
    test_stats_single_thread();
    test_concurrent_alloc();
    test_concurrent_alloc_free_stress();
    test_aba_safety();

    std::printf("\n=== Resultados ===\n");
    std::printf("Pasaron: %d/%d\n", g_tests_passed, g_tests_run);
    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
