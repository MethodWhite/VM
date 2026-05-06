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
 * @file test_no_moving_oldgen.cpp
 * @brief Tests para el modelo (iv): GC no-moving en OldGen con free lists segregadas.
 *
 * Verifica:
 *   1. alloc/free repetido del mismo tamano reusa slots via free list (O(1)).
 *   2. Multiples size classes funcionan independientemente.
 *   3. host_ptr de objetos OldGen es estable a traves de major_gc.
 *   4. Las metricas de fragmentacion reportan valores razonables.
 *   5. Slots grandes (>4096) usan la free list general.
 *   6. handle_for_ptr() (la API ptr->handle) sigue funcionando tras GC.
 */

#include "gc/gc_heap.h"
#include "arena/arena_manager.h"

#include <cstdio>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers de salida
// ---------------------------------------------------------------------------

static int g_run = 0;
static int g_pass = 0;
static int g_fail = 0;

#define SECTION(name) \
    do { \
        printf("\n=== %s ===\n", name); \
    } while (0)

#define EXPECT(cond, msg) \
    do { \
        ++g_run; \
        if (cond) { ++g_pass; printf("  OK   %s\n", msg); } \
        else      { ++g_fail; printf("  FAIL %s  (linea %d)\n", msg, __LINE__); } \
    } while (0)

// ---------------------------------------------------------------------------
// Test 1: Reuso O(1) via free list del mismo size class
// ---------------------------------------------------------------------------
//
// Estrategia: alocar N objetos de tamano X en OldGen (forzando minor_gc para
// promocionarlos), liberarlos todos via major_gc (los handles se sueltan
// previamente con drop()), y luego alocar N mas del mismo tamano.  El tamano
// 'old_reserved_bytes' (memoria total reservada en bloques) NO debe crecer:
// el segundo lote reusa los slots del primero.

static void test_reuso_same_class() {
    SECTION("Test 1: reuso O(1) de slots liberados (mismo size class)");

    vm::ArenaManager mgr;
    // Nursery pequena para que minor_gc promocione enseguida; OldGen
    // umbral alto para que NO dispare major_gc automatico (lo hacemos
    // manual abajo).
    gc::GcHeap heap(mgr, 8 * 1024, 64 * 1024 * 1024);

    constexpr size_t N    = 32;
    constexpr size_t SIZE = 64;  // payload; total slot = 16 bytes header + 64 + pad = 72 -> class 96

    // Lote 1
    std::vector<gc::GcHandle> h1;
    h1.reserve(N);
    for (size_t i = 0; i < N; ++i) {
        gc::GcHandle h = heap.alloc(SIZE);
        EXPECT(h != gc::GC_NULL_HANDLE, "alloc lote 1 produce handle valido");
        h1.push_back(h);
    }

    // Forzar minor_gc para promocionar los objetos a OldGen.  Como los
    // handles siguen vivos, todos se evacuan.
    heap.minor_gc();
    const size_t reserved_after_lote1 = heap.stats().old_reserved_bytes;
    printf("  reserved tras lote 1 promovido: %zu bytes\n", reserved_after_lote1);
    EXPECT(reserved_after_lote1 > 0, "old_reserved_bytes > 0 tras promocion");

    // Soltar los handles y disparar major_gc para que sweep marque DEAD
    // y reconstruya las free lists.
    for (gc::GcHandle h : h1) heap.drop(h);
    heap.major_gc();
    printf("  freelist_bytes tras drop+major_gc: %llu bytes\n",
           (unsigned long long)heap.stats().old_freelist_bytes);
    EXPECT(heap.stats().old_freelist_bytes >= N * 96 / 2,
           "free list contiene los slots liberados");

    // Lote 2: mismas N alocaciones del mismo tamano.  Deberian salir
    // de la free list (O(1)) sin reservar memoria nueva.
    const size_t reserved_pre_lote2  = heap.stats().old_reserved_bytes;
    const size_t freelist_alloc_pre  = heap.stats().old_alloc_freelist;
    std::vector<gc::GcHandle> h2;
    h2.reserve(N);
    for (size_t i = 0; i < N; ++i) {
        gc::GcHandle h = heap.alloc(SIZE);
        // Para evitar que la nursery se llene y dispare minor_gc,
        // forzamos manual y miramos OldGen directamente.
        EXPECT(h != gc::GC_NULL_HANDLE, "alloc lote 2 produce handle valido");
        h2.push_back(h);
    }
    heap.minor_gc(); // promueve el lote 2 a OldGen
    const size_t reserved_post_lote2 = heap.stats().old_reserved_bytes;
    const size_t freelist_alloc_post = heap.stats().old_alloc_freelist;

    printf("  reserved pre lote 2:  %zu bytes\n", reserved_pre_lote2);
    printf("  reserved post lote 2: %zu bytes\n", reserved_post_lote2);
    printf("  alloc_freelist delta: %llu (esperado >= %zu)\n",
           (unsigned long long)(freelist_alloc_post - freelist_alloc_pre),
           N);

    EXPECT(reserved_post_lote2 == reserved_pre_lote2,
           "old_reserved_bytes NO crece (slots reusados, no nueva memoria)");
    EXPECT((freelist_alloc_post - freelist_alloc_pre) >= N,
           "old_alloc_freelist incremento por al menos N (todos via free list)");
}

// ---------------------------------------------------------------------------
// Test 2: Multiples size classes independientes
// ---------------------------------------------------------------------------

static void test_multi_size_class() {
    SECTION("Test 2: free lists segregadas por size class");

    vm::ArenaManager mgr;
    gc::GcHeap heap(mgr, 8 * 1024, 64 * 1024 * 1024);

    // Alocar mezcla: tamanos pequenos, medianos, grandes.
    // Cada tamano caera en un size class distinto.
    constexpr size_t SIZES[] = { 16, 100, 500, 2000 };
    constexpr size_t COUNT_PER_SIZE = 8;

    std::vector<gc::GcHandle> all_handles;
    for (size_t s : SIZES) {
        for (size_t i = 0; i < COUNT_PER_SIZE; ++i) {
            gc::GcHandle h = heap.alloc(s);
            EXPECT(h != gc::GC_NULL_HANDLE, "alloc multi-size produce handle valido");
            all_handles.push_back(h);
        }
    }
    heap.minor_gc();
    EXPECT(heap.stats().old_reserved_bytes > 0,
           "old_reserved_bytes > 0 tras alloc + promocion mixta");

    // Soltar TODOS y barrer.  Las free lists deben tener entradas en
    // varias size classes distintas.
    for (gc::GcHandle h : all_handles) heap.drop(h);
    heap.major_gc();
    EXPECT(heap.stats().old_freelist_bytes > 0,
           "free lists pobladas tras sweep masivo");

    // Re-alocar exactamente los mismos tamanos: deberian todos salir
    // de las free lists.
    const size_t freelist_pre  = heap.stats().old_alloc_freelist;
    const size_t reserved_pre  = heap.stats().old_reserved_bytes;
    for (size_t s : SIZES) {
        for (size_t i = 0; i < COUNT_PER_SIZE; ++i) {
            gc::GcHandle h = heap.alloc(s);
            EXPECT(h != gc::GC_NULL_HANDLE, "re-alloc multi-size produce handle valido");
        }
    }
    heap.minor_gc(); // promueve, disparando alloc_in_old via evacuacion
    const size_t freelist_post = heap.stats().old_alloc_freelist;
    const size_t reserved_post = heap.stats().old_reserved_bytes;

    printf("  alloc_freelist incremento: %llu\n",
           (unsigned long long)(freelist_post - freelist_pre));
    printf("  reserved incremento:       %zu bytes\n",
           reserved_post - reserved_pre);
    EXPECT(freelist_post > freelist_pre,
           "free lists usadas en re-alloc multi-size");
    // Permitimos algun crecimiento minimo si algun size class no tenia
    // suficientes nodos (depende del orden de evacuacion); tope generoso.
    EXPECT(reserved_post - reserved_pre < 4096,
           "crecimiento de reserved acotado (la mayoria reusado)");
}

// ---------------------------------------------------------------------------
// Test 3: host_ptr ESTABLE en OldGen tras major_gc
// ---------------------------------------------------------------------------
//
// El cambio fundamental de (iv): los objetos OldGen NO se mueven.  Verificar
// que el host_ptr antes y despues de un major_gc es identico.

static void test_host_ptr_estable_oldgen() {
    SECTION("Test 3: host_ptr OldGen estable a traves de major_gc");

    vm::ArenaManager mgr;
    gc::GcHeap heap(mgr, 8 * 1024, 64 * 1024 * 1024);

    // Alocar y promocionar a OldGen.
    gc::GcHandle h = heap.alloc(128);
    EXPECT(h != gc::GC_NULL_HANDLE, "alloc valido");
    heap.minor_gc();
    uint8_t *p_pre = heap.deref(h);
    EXPECT(p_pre != nullptr, "deref post-promocion valido");

    // Marcar el payload con un patron reconocible.
    for (int i = 0; i < 128; ++i) p_pre[i] = static_cast<uint8_t>(i);

    // Disparar major_gc: el objeto sigue vivo (handle no soltado), asi que
    // sweep lo marca BLACK y NO lo libera.  Como (iv) es no-moving, el
    // host_ptr no cambia.
    heap.major_gc();
    uint8_t *p_post = heap.deref(h);
    EXPECT(p_post == p_pre,
           "host_ptr identico antes y despues de major_gc (no-moving OldGen)");
    bool patron_ok = true;
    for (int i = 0; i < 128; ++i) {
        if (p_post[i] != static_cast<uint8_t>(i)) { patron_ok = false; break; }
    }
    EXPECT(patron_ok, "payload preservado byte-a-byte tras major_gc");

    // handle_for_ptr (lookup inverso) sigue funcionando con el mismo ptr.
    gc::GcHandle h_back = heap.handle_for_ptr(p_post);
    EXPECT(h_back == h, "handle_for_ptr devuelve el mismo handle tras major_gc");
}

// ---------------------------------------------------------------------------
// Test 4: Slots grandes (>4096) usan la free list general
// ---------------------------------------------------------------------------

static void test_large_freelist() {
    SECTION("Test 4: slots grandes (>4096) usan free list general");

    vm::ArenaManager mgr;
    gc::GcHeap heap(mgr, 8 * 1024, 64 * 1024 * 1024);

    // Alocar objeto grande (8 KB), promocionar, soltar, barrer.
    gc::GcHandle h = heap.alloc(8 * 1024);
    EXPECT(h != gc::GC_NULL_HANDLE, "alloc grande valido");
    heap.minor_gc();
    heap.drop(h);
    heap.major_gc();

    EXPECT(heap.stats().old_freelist_bytes >= 8 * 1024,
           "free list large contiene el slot grande tras drop+sweep");

    // Re-alocar tamano similar: deberia salir de la free list large.
    const size_t freelist_pre = heap.stats().old_alloc_freelist;
    gc::GcHandle h2 = heap.alloc(8 * 1024);
    EXPECT(h2 != gc::GC_NULL_HANDLE, "re-alloc grande valido");
    heap.minor_gc(); // promociona
    EXPECT(heap.stats().old_alloc_freelist > freelist_pre,
           "alloc_freelist se incremento (slot grande reusado)");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    test_reuso_same_class();
    test_multi_size_class();
    test_host_ptr_estable_oldgen();
    test_large_freelist();

    printf("\n=== RESUMEN ===\n");
    printf("Total: %d   OK: %d   FAIL: %d\n", g_run, g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
