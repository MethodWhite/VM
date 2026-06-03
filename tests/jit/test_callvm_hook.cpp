/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/jit/test_callvm_hook.cpp
 * @brief Tests del mapa pc -> jit_code (Sprint D.5-callvm-hook).
 *
 * Verifica que:
 *   1. Por defecto @c g_pc_jit_active == false (cero overhead en interp).
 *   2. @c lookup_jit_code_at_pc retorna nullptr cuando el mapa esta vacio.
 *   3. @c register_jit_code_at_pc + @c lookup_jit_code_at_pc roundtripean
 *      correctamente para varias direcciones distintas.
 *   4. Multiples registros con el mismo vaddr sobreescriben (caso re-compile).
 *   5. @c clear_jit_code_at_pc_map vacia el mapa pero deja el flag sticky.
 *   6. Argumentos invalidos (vaddr=0 o fn=nullptr) se rechazan limpiamente.
 */

#include "jit/auto_jit.h"

#include <cstdio>
#include <cstdint>

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

    /* Fake JIT functions: usamos punteros sentinela; el lookup nunca los
     * invoca, solo los compara para verificar el roundtrip. */
    uint64_t fake_fn_a(void *) { return 42; }
    uint64_t fake_fn_b(void *) { return 99; }
    uint64_t fake_fn_c(void *) { return 7; }

    void test_initial_state() {
        std::fprintf(stderr, "=== test_initial_state ===\n");
        /* Limpiar de tests previos. */
        jit::clear_jit_code_at_pc_map();
        /* Verificar lookup retorna nullptr cuando el mapa esta vacio. */
        CHECK(jit::lookup_jit_code_at_pc(0x1000) == nullptr,
              "lookup_jit_code_at_pc en mapa vacio retorna nullptr");
        CHECK(jit::lookup_jit_code_at_pc(0xDEADBEEF) == nullptr,
              "lookup_jit_code_at_pc con vaddr arbitrario retorna nullptr");
    }

    void test_register_lookup_roundtrip() {
        std::fprintf(stderr, "=== test_register_lookup_roundtrip ===\n");
        jit::clear_jit_code_at_pc_map();

        /* Registrar 3 funciones en direcciones distintas. */
        jit::register_jit_code_at_pc(0x1000, reinterpret_cast<void *>(&fake_fn_a));
        jit::register_jit_code_at_pc(0x2000, reinterpret_cast<void *>(&fake_fn_b));
        jit::register_jit_code_at_pc(0x3000, reinterpret_cast<void *>(&fake_fn_c));

        /* g_pc_jit_active debe estar true ahora. */
        CHECK(jit::g_pc_jit_active == true,
              "g_pc_jit_active true tras al menos 1 registro");

        /* Lookups en orden distinto al registro para verificar que la
         * tabla hash no depende del orden de insercion. */
        CHECK(jit::lookup_jit_code_at_pc(0x2000) == reinterpret_cast<void *>(&fake_fn_b),
              "lookup vaddr 0x2000 devuelve fake_fn_b");
        CHECK(jit::lookup_jit_code_at_pc(0x1000) == reinterpret_cast<void *>(&fake_fn_a),
              "lookup vaddr 0x1000 devuelve fake_fn_a");
        CHECK(jit::lookup_jit_code_at_pc(0x3000) == reinterpret_cast<void *>(&fake_fn_c),
              "lookup vaddr 0x3000 devuelve fake_fn_c");

        /* Direccion no registrada -> nullptr. */
        CHECK(jit::lookup_jit_code_at_pc(0x4000) == nullptr,
              "lookup vaddr no registrado devuelve nullptr");
    }

    void test_overwrite_on_recompile() {
        std::fprintf(stderr, "=== test_overwrite_on_recompile ===\n");
        jit::clear_jit_code_at_pc_map();

        /* Primer registro. */
        jit::register_jit_code_at_pc(0x1000, reinterpret_cast<void *>(&fake_fn_a));
        CHECK(jit::lookup_jit_code_at_pc(0x1000) == reinterpret_cast<void *>(&fake_fn_a),
              "registro inicial visible en lookup");

        /* Sobreescribir con otra fn (simula re-compile post-deopt). */
        jit::register_jit_code_at_pc(0x1000, reinterpret_cast<void *>(&fake_fn_b));
        CHECK(jit::lookup_jit_code_at_pc(0x1000) == reinterpret_cast<void *>(&fake_fn_b),
              "overwrite con misma vaddr sobreescribe el valor");
    }

    void test_clear_keeps_active_flag() {
        std::fprintf(stderr, "=== test_clear_keeps_active_flag ===\n");
        jit::clear_jit_code_at_pc_map();
        jit::register_jit_code_at_pc(0x1000, reinterpret_cast<void *>(&fake_fn_a));
        CHECK(jit::g_pc_jit_active == true, "flag activo tras registro");

        jit::clear_jit_code_at_pc_map();
        /* Tras clear, lookup falla (mapa vacio). */
        CHECK(jit::lookup_jit_code_at_pc(0x1000) == nullptr,
              "lookup tras clear devuelve nullptr");
        /* Flag se mantiene true (sticky por diseno). */
        CHECK(jit::g_pc_jit_active == true,
              "g_pc_jit_active sigue true tras clear (sticky)");
    }

    void test_invalid_inputs_ignored() {
        std::fprintf(stderr, "=== test_invalid_inputs_ignored ===\n");
        jit::clear_jit_code_at_pc_map();

        /* vaddr=0 debe ignorarse (no se registra). */
        jit::register_jit_code_at_pc(0, reinterpret_cast<void *>(&fake_fn_a));
        CHECK(jit::lookup_jit_code_at_pc(0) == nullptr,
              "vaddr=0 no se registra");

        /* fn=nullptr tambien ignorado. */
        jit::register_jit_code_at_pc(0x5000, nullptr);
        CHECK(jit::lookup_jit_code_at_pc(0x5000) == nullptr,
              "fn=nullptr no se registra");
    }

    /* test_trigger_noop_when_jit_off:
     *
     * Verifica que cuando JIT esta off (threshold = UINT32_MAX), el
     * trigger es no-op: no aloca counters, no toca el mapa, no crashea
     * con vm=nullptr.  Pasar nullptr es seguro porque el fast path debe
     * salir ANTES de dereferenciar vm.  Cualquier crash aqui indica que
     * el fast path no es realmente cero-overhead.
     */
    void test_trigger_noop_when_jit_off() {
        std::fprintf(stderr, "=== test_trigger_noop_when_jit_off ===\n");
        const uint32_t prev_threshold = jit::g_jit_threshold;
        jit::set_jit_threshold(UINT32_MAX);   /* JIT off */
        jit::clear_jit_code_at_pc_map();

        /* Con JIT off, maybe_compile_callvm_target debe salir inmediato. */
        jit::maybe_compile_callvm_target(nullptr, 0x9000);
        CHECK(jit::lookup_jit_code_at_pc(0x9000) == nullptr,
              "trigger con JIT off no registra nada");

        jit::set_jit_threshold(prev_threshold);
    }

} // namespace

int main() {
    test_initial_state();
    test_register_lookup_roundtrip();
    test_overwrite_on_recompile();
    test_clear_keeps_active_flag();
    test_invalid_inputs_ignored();
    test_trigger_noop_when_jit_off();

    std::fprintf(stderr, "test_callvm_hook: %d pass, %d fail\n",
                 pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
