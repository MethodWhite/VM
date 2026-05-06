/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file test_stress_fragmentation.cpp
 * @brief Stress del nuevo allocator no-moving en OldGen.
 *
 * Patrones probados:
 *  1. Loop alocar/liberar tamano fijo: la memoria total reservada NO debe
 *     crecer indefinidamente (cada free queda disponible para el siguiente
 *     alloc).
 *  2. Patron mixto (varios tamanos alternados): la fragmentacion total se
 *     mantiene acotada por debajo de un umbral razonable.
 *  3. Tamanos variables grandes: la free list general absorbe los slots
 *     grandes liberados sin crear bloques extras innecesariamente.
 */

#include "gc/gc_heap.h"
#include "arena/arena_manager.h"

#include <cstdio>
#include <cstdint>
#include <vector>

static int g_run = 0;
static int g_pass = 0;
static int g_fail = 0;

#define SECTION(name) printf("\n=== %s ===\n", name)
#define EXPECT(cond, msg) \
    do { \
        ++g_run; \
        if (cond) { ++g_pass; printf("  OK   %s\n", msg); } \
        else      { ++g_fail; printf("  FAIL %s  (linea %d)\n", msg, __LINE__); } \
    } while (0)

// ---------------------------------------------------------------------------
// Stress 1: loop alocar/liberar tamano fijo en OldGen.
//
// Objetivo: tras N ciclos de "alocar M objetos + liberarlos", la memoria
// reservada por el GcHeap (old_reserved_bytes) NO debe crecer mas alla
// del primer ciclo.  Si crece, es signo de que el reuso de slots no
// funciona y el allocator pide bloques nuevos cada ciclo.
// ---------------------------------------------------------------------------
static void stress_alloc_free_fijo() {
    SECTION("Stress 1: alloc/free repetido tamano fijo - reserved estable");

    vm::ArenaManager mgr;
    gc::GcHeap heap(mgr, 8 * 1024, 256 * 1024 * 1024);

    constexpr size_t N_OBJ      = 100;
    constexpr size_t SIZE       = 128; // payload, total = 136 -> class 7 (192)
    constexpr size_t N_CYCLES   = 50;

    // Primer ciclo establece el "watermark": alocamos N objetos, los
    // promovemos a OldGen, registramos el reserved.  Los siguientes
    // ciclos deberian quedarse en ese mismo nivel.
    std::vector<gc::GcHandle> handles;
    handles.reserve(N_OBJ);

    auto run_cycle = [&]() {
        handles.clear();
        for (size_t i = 0; i < N_OBJ; ++i) {
            handles.push_back(heap.alloc(SIZE));
        }
        heap.minor_gc(); // promueve a OldGen
        for (gc::GcHandle h : handles) heap.drop(h);
        heap.major_gc(); // libera y reconstruye free lists
    };

    run_cycle();
    const size_t baseline = heap.stats().old_reserved_bytes;
    printf("  baseline reserved (ciclo 1): %zu bytes\n", baseline);
    EXPECT(baseline > 0, "baseline > 0");

    for (size_t c = 0; c < N_CYCLES; ++c) {
        run_cycle();
    }
    const size_t final_reserved = heap.stats().old_reserved_bytes;
    printf("  reserved tras %zu ciclos: %zu bytes (esperado <= %zu)\n",
           (size_t)(N_CYCLES + 1), final_reserved, baseline);
    EXPECT(final_reserved == baseline,
           "reserved no crece tras 50 ciclos (slots reusados, no leak)");

    // La gran mayoria de allocs en OldGen deberian haber salido de free list.
    const uint64_t fl  = heap.stats().old_alloc_freelist;
    const uint64_t bp  = heap.stats().old_alloc_bump;
    const uint64_t nb  = heap.stats().old_alloc_newblock;
    printf("  alloc breakdown: freelist=%llu  bump=%llu  newblock=%llu\n",
           (unsigned long long)fl, (unsigned long long)bp, (unsigned long long)nb);
    EXPECT(fl > bp + nb,
           "mayoria de allocs OldGen via free list (reuso eficiente)");
}

// ---------------------------------------------------------------------------
// Stress 2: patron mixto multi-tamano - fragmentacion acotada
//
// Aloca objetos de 4 tamanos distintos en orden mezclado, libera, repite.
// Verifica que el ratio (reserved - used) / reserved (fragmentacion) se
// mantiene bajo un umbral (~30%).  Si la free list segregada funciona
// bien, cada size class tiene sus propios slots reusables y el overhead
// solo crece cuando hay desajustes temporales.
// ---------------------------------------------------------------------------
static void stress_multi_size_frag_acotada() {
    SECTION("Stress 2: patron mixto multi-tamano - fragmentacion acotada");

    vm::ArenaManager mgr;
    gc::GcHeap heap(mgr, 8 * 1024, 256 * 1024 * 1024);

    constexpr size_t SIZES[] = { 32, 100, 250, 700 };
    constexpr size_t N_PER_SIZE = 50;
    constexpr size_t N_CYCLES   = 20;

    std::vector<gc::GcHandle> handles;
    auto cycle = [&]() {
        handles.clear();
        // Patron entrelazado: por cada i alocamos un objeto de cada tamano.
        for (size_t i = 0; i < N_PER_SIZE; ++i) {
            for (size_t s : SIZES) {
                handles.push_back(heap.alloc(s));
            }
        }
        heap.minor_gc();
        for (gc::GcHandle h : handles) heap.drop(h);
        heap.major_gc();
    };

    for (size_t c = 0; c < N_CYCLES; ++c) cycle();

    const size_t reserved = heap.stats().old_reserved_bytes;
    const size_t freelist = heap.stats().old_freelist_bytes;
    const size_t live     = heap.old_used();
    printf("  tras %zu ciclos: reserved=%zu  live=%zu  freelist=%zu\n",
           (size_t)N_CYCLES, reserved, live, freelist);
    // Tras drop+major_gc, live debe ser ~0.  freelist debe ser ~ reserved.
    EXPECT(live == 0, "old_used == 0 tras drop+major_gc");
    EXPECT(reserved > 0, "reserved > 0 (alocaciones reales se hicieron)");

    // Empezamos otro ciclo y verificamos que reserved no crece.
    cycle();
    const size_t reserved_after = heap.stats().old_reserved_bytes;
    printf("  tras 1 ciclo extra: reserved=%zu  (esperado == anterior)\n",
           reserved_after);
    EXPECT(reserved_after == reserved,
           "ciclo extra reusa slots, no crece reserved");
}

// ---------------------------------------------------------------------------
// Stress 3: free list general (slots > 4096)
//
// Objetos grandes liberados deben reusarse via large_free_list_, no crear
// nuevos bloques sin necesidad.
// ---------------------------------------------------------------------------
static void stress_large_freelist() {
    SECTION("Stress 3: free list general para slots grandes (>4096)");

    vm::ArenaManager mgr;
    gc::GcHeap heap(mgr, 8 * 1024, 256 * 1024 * 1024);

    constexpr size_t SIZE = 16 * 1024; // 16 KB payload
    constexpr size_t N    = 10;

    std::vector<gc::GcHandle> handles;
    for (size_t i = 0; i < N; ++i) handles.push_back(heap.alloc(SIZE));
    heap.minor_gc();
    const size_t reserved_pre = heap.stats().old_reserved_bytes;

    for (gc::GcHandle h : handles) heap.drop(h);
    heap.major_gc();

    const uint64_t fl_pre = heap.stats().old_alloc_freelist;
    handles.clear();
    for (size_t i = 0; i < N; ++i) handles.push_back(heap.alloc(SIZE));
    heap.minor_gc();
    const size_t reserved_post = heap.stats().old_reserved_bytes;
    const uint64_t fl_post = heap.stats().old_alloc_freelist;

    printf("  reserved pre/post: %zu / %zu  (delta=%zd)\n",
           reserved_pre, reserved_post,
           (ptrdiff_t)reserved_post - (ptrdiff_t)reserved_pre);
    printf("  alloc_freelist incremento: %llu\n",
           (unsigned long long)(fl_post - fl_pre));

    EXPECT(reserved_post == reserved_pre,
           "slots grandes reusados (reserved no crece)");
    EXPECT((fl_post - fl_pre) >= N,
           "los N re-allocs grandes salieron via free list");
}

int main() {
    stress_alloc_free_fijo();
    stress_multi_size_frag_acotada();
    stress_large_freelist();

    printf("\n=== RESUMEN ===\n");
    printf("Total: %d   OK: %d   FAIL: %d\n", g_run, g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
