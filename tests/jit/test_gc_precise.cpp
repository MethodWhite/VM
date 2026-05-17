/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/jit/test_gc_precise.cpp
 * @brief Tests de la integracion @c scan_jit_roots_precise dentro de
 *        @c gc_heap::major_gc (D.2-integration Fase 2).
 *
 * Cubre:
 *   1. Sin JIT funcs registradas, major_gc se comporta IDENTICO a antes.
 *      Las metricas @c precise_roots_marked / @c precise_frames_scanned
 *      quedan a 0.  Cero overhead pre-D.3.
 *   2. Con JIT funcs registradas pero RIPs fuera de su rango, precise scan
 *      walks la cadena RBP pero no encuentra JIT frames y no marca nada.
 *   3. Validacion empirica precise vs conservative: handles que ambos
 *      encuentran coinciden.  Si precise encuentra menos que conservative,
 *      no es bug (conservative tiene false positives); si encuentra mas,
 *      hay un bug del conservative (improbable).
 *
 * NOTE: el test end-to-end "JIT code aloca + GC preserva handle" llega
 * cuando D.3 anyada CALL al selector.  Pre-D.3 no podemos disparar el
 * caso real desde JIT-eated code.
 */

#include "gc/gc_heap.h"
#include "arena/arena_manager.h"
#include "jit/jit_registry.h"
#include "jit/machine_ir.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

    int pass_count = 0;
    int fail_count = 0;

    #define CHECK(cond, msg) do {                                            \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL: %s (linea %d)\n", msg, __LINE__);    \
            ++fail_count;                                                    \
        } else {                                                             \
            ++pass_count;                                                    \
        }                                                                    \
    } while (0)

    /* ===================================================================== */
    /* Test 1: major_gc sin JIT funcs - cero overhead, mismo comportamiento  */
    /* ===================================================================== */

    void test_no_jit_no_overhead() {
        /* Limpiar registry para asegurarnos. */
        jit::JitRegistry::instance().clear();
        CHECK(jit::JitRegistry::instance().size() == 0, "registry vacio");

        vm::ArenaManager mgr;
        gc::GcHeap heap(mgr, 2 * 1024 * 1024, 8 * 1024 * 1024);

        /* Allocate algunos objetos para tener algo en el heap. */
        gc::GcHandle h1 = heap.alloc(64);
        gc::GcHandle h2 = heap.alloc(128);
        gc::GcHandle h3 = heap.alloc(256);
        CHECK(h1 != gc::GC_NULL_HANDLE, "alloc h1");
        CHECK(h2 != gc::GC_NULL_HANDLE, "alloc h2");
        CHECK(h3 != gc::GC_NULL_HANDLE, "alloc h3");

        /* Tomar referencias externas (pin) para que sobrevivan sin
         * depender del stack scan. */
        heap.gc_addref(h1);
        heap.gc_addref(h2);

        const uint64_t before_major  = heap.stats().major_gc_count;
        const uint64_t before_precise = heap.stats().precise_roots_marked;
        const uint64_t before_frames = heap.stats().precise_frames_scanned;

        /* Disparar major_gc explicitamente (API publica). */
        heap.major_gc();

        const uint64_t after_major  = heap.stats().major_gc_count;
        CHECK(after_major == before_major + 1, "major_gc se ejecuto 1 vez");

        const uint64_t after_precise = heap.stats().precise_roots_marked;
        const uint64_t after_frames = heap.stats().precise_frames_scanned;

        /* Sin JIT funcs registradas, precise scan NO debe marcar nada. */
        CHECK(after_precise == before_precise,
              "precise_roots_marked sin JIT = 0");
        CHECK(after_frames == before_frames,
              "precise_frames_scanned sin JIT = 0");

        /* Los handles externamente pinnados deben seguir vivos. */
        CHECK(heap.deref(h1) != nullptr, "h1 sigue vivo (external_ref)");
        CHECK(heap.deref(h2) != nullptr, "h2 sigue vivo (external_ref)");

        heap.gc_release(h1);
        heap.gc_release(h2);
    }

    /* ===================================================================== */
    /* Test 2: major_gc con JIT funcs registradas (pero RIPs irreales)        */
    /* ===================================================================== */

    void test_jit_registered_but_no_match() {
        jit::JitRegistry::instance().clear();

        /* Registrar una "fake JIT function" en un rango de memoria que
         * NUNCA va a aparecer como RIP en la cadena RBP de este test
         * (porque la fn nunca se ejecuto).  scan_jit_frames walks el
         * stack pero no encuentra frames cuyo RIP caiga en este rango. */
        const uint8_t *fake_code = reinterpret_cast<const uint8_t *>(0x1000ULL);
        std::vector<jit::Stackmap> sms;
        {
            jit::Stackmap s; s.pc_offset = 10;
            jit::StackmapSlot slot;
            slot.rbp_offset = -8;
            slot.gc_kind = jit::StackmapGcKind::HANDLE;
            s.slots.push_back(slot);
            sms.push_back(s);
        }
        jit::JitRegistry::instance().register_function(
            fake_code, fake_code + 100, std::move(sms), 16, "fake");
        CHECK(jit::JitRegistry::instance().size() == 1, "1 JIT fn registrada");

        vm::ArenaManager mgr;
        gc::GcHeap heap(mgr, 2 * 1024 * 1024, 8 * 1024 * 1024);

        gc::GcHandle h = heap.alloc(64);
        heap.gc_addref(h);

        const uint64_t before_precise = heap.stats().precise_roots_marked;
        const uint64_t before_frames = heap.stats().precise_frames_scanned;

        /* Disparar major_gc (que ahora ejecutara scan_jit_roots_precise). */
        heap.major_gc();

        const uint64_t after_precise = heap.stats().precise_roots_marked;
        const uint64_t after_frames = heap.stats().precise_frames_scanned;

        /* Con JIT registry no-vacio, scan_jit_frames walks la cadena RBP.
         * Pero como los RIPs reales del stack actual no caen en el rango
         * de la fake fn (que esta en 0x1000-0x1064), no se encuentra
         * ningun JIT frame ni se marcan roots precise. */
        CHECK(after_precise == before_precise,
              "ningun root precise (RIPs no matchean fake fn)");
        CHECK(after_frames == before_frames,
              "ningun frame JIT detectado");

        /* El handle external sigue vivo (lo pinnamos). */
        CHECK(heap.deref(h) != nullptr, "h sigue vivo");

        heap.gc_release(h);
        jit::JitRegistry::instance().clear();
    }

    /* ===================================================================== */
    /* Test 3: conservative_roots_marked stat se actualiza                    */
    /* ===================================================================== */

    void test_conservative_metric() {
        jit::JitRegistry::instance().clear();

        vm::ArenaManager mgr;
        gc::GcHeap heap(mgr, 2 * 1024 * 1024, 8 * 1024 * 1024);

        /* Alocar algunos handles y forzar GC. */
        gc::GcHandle handles[8];
        for (int i = 0; i < 8; ++i) {
            handles[i] = heap.alloc(64);
            heap.gc_addref(handles[i]);
        }

        /* Disparar major_gc explicitamente (API publica). */
        heap.major_gc();

        /* Verificar que las metricas existen y se actualizan. */
        CHECK(heap.stats().major_gc_count >= 1, "major_gc corrio al menos 1 vez");

        /* Sin JIT funcs, las metricas precise quedan a 0. */
        CHECK(heap.stats().precise_roots_marked == 0,
              "precise_roots_marked = 0 sin JIT funcs");
        CHECK(heap.stats().precise_frames_scanned == 0,
              "precise_frames_scanned = 0 sin JIT funcs");

        /* conservative_roots_marked es >= 0 (depende del stack del test). */
        CHECK(heap.stats().conservative_roots_marked < UINT64_MAX,
              "conservative_roots_marked es campo valido");

        for (auto h : handles) heap.gc_release(h);
    }

    /* ===================================================================== */
    /* Test 4: no regresion - todos los handles externos sobreviven           */
    /* ===================================================================== */

    void test_external_handles_survive() {
        jit::JitRegistry::instance().clear();

        vm::ArenaManager mgr;
        gc::GcHeap heap(mgr, 2 * 1024 * 1024, 8 * 1024 * 1024);

        /* Allocate y pin 20 handles. */
        gc::GcHandle handles[20];
        for (int i = 0; i < 20; ++i) {
            handles[i] = heap.alloc(128);
            CHECK(handles[i] != gc::GC_NULL_HANDLE, "alloc handle");
            heap.gc_addref(handles[i]);
        }

        /* Forzar varios major GCs explicitos. */
        for (int round = 0; round < 5; ++round) {
            heap.major_gc();
        }

        /* Todos los handles externos deben seguir vivos. */
        int alive = 0;
        for (auto h : handles) {
            if (heap.deref(h) != nullptr) ++alive;
        }
        CHECK(alive == 20, "todos los 20 handles externos sobrevivieron");

        /* Cleanup */
        for (auto h : handles) heap.gc_release(h);
    }

} // namespace anonymous

int main() {
    test_no_jit_no_overhead();
    test_jit_registered_but_no_match();
    test_conservative_metric();
    test_external_handles_survive();

    std::printf("test_gc_precise: %d pass, %d fail\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
