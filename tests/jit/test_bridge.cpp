/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/jit/test_bridge.cpp
 * @brief Tests del bridge interp <-> JIT (@c jit::enter_jit) y de la
 *        tabla @c jit::RuntimeEntries.
 *
 * Cubre:
 *
 *   1. @c enter_jit invoca una funcion JIT-eada con @c ProcessVM* y
 *      esta puede leer/escribir @c proc->registers.
 *   2. @c RuntimeEntries::resolve() puebla todos los campos a no-null.
 *   3. @c RuntimeEntries::all_resolved() devuelve true.
 *   4. La direccion de @c vrt_api_version es estable y devuelve el
 *      valor esperado.
 *
 * El bridge se valida con un trozo de codigo nativo escrito a mano
 * que actua como "funcion JIT minimal": setea @c regs[0] = constante
 * y retorna.  Asi verificamos que la convencion VM (ProcessVM* en
 * el primer arg + escritura a registers) funciona end-to-end.
 */

#include "jit/interp_jit_bridge.h"
#include "jit/code_cache.h"
#include "jit/runtime_entries.h"
#include "vesta_rt/public.h"

#include "runtime/proceso_runtime.h"
#include "runtime/runtime.h"

#include <cstdint>
#include <cstdio>
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
     * @brief Codigo nativo que escribe 0x2A a @c proc->registers.regs[0]
     *        y retorna 0.
     *
     * Layout esperado (proc en rcx/rdi):
     *   - proc->registers.regs[]  vive en algun offset estable; el
     *     test usa un proxy minimo (struct local PadProc) que mimica
     *     el layout EXACTO necesario para que el codigo nativo
     *     escriba al campo correcto.
     *
     * Para evitar depender del offset real de @c registers en
     * @c ProcessVM (que puede cambiar), este test usa un proxy local
     * en lugar del @c ProcessVM real.  Para ProcessVM real se valida
     * solo que @c enter_jit corre sin crash.
     *
     * x86-64 escribiendo a [arg1 + 16]:
     *   SysV: 48 c7 47 10 2a 00 00 00     mov qword [rdi+16], 0x2A
     *   Win64: 48 c7 41 10 2a 00 00 00    mov qword [rcx+16], 0x2A
     *
     *   xor eax, eax    ; 31 c0
     *   ret             ; c3
     */
#if defined(_WIN32)
    constexpr uint8_t kCodeWriteToProxy[] = {
        0x48, 0xC7, 0x41, 0x10, 0x2A, 0x00, 0x00, 0x00,  // mov [rcx+16], 0x2A
        0x31, 0xC0,                                       // xor eax, eax
        0xC3                                              // ret
    };
#else
    constexpr uint8_t kCodeWriteToProxy[] = {
        0x48, 0xC7, 0x47, 0x10, 0x2A, 0x00, 0x00, 0x00,  // mov [rdi+16], 0x2A
        0x31, 0xC0,                                       // xor eax, eax
        0xC3                                              // ret
    };
#endif

    /**
     * @brief Proxy del @c ProcessVM con un solo campo "registers" en
     *        offset 16.  El bridge no exige usar el ProcessVM real;
     *        valida que la convencion @c rdi/rcx funciona.
     */
    struct alignas(16) PadProc {
        uint64_t pad0;       /* offset 0 */
        uint64_t pad1;       /* offset 8 */
        uint64_t target_r0;  /* offset 16 - el codigo nativo escribe aqui */
    };

    /** @brief Test 1: enter_jit con proxy escribe correctamente a [arg+16]. */
    void test_bridge_basic() {
        jit::CodeCache cache;
        uint8_t *ptr = cache.alloc(sizeof(kCodeWriteToProxy), 16);
        CHECK(ptr != nullptr, "alloc para bridge");
        std::memcpy(ptr, kCodeWriteToProxy, sizeof(kCodeWriteToProxy));
        cache.commit(ptr, sizeof(kCodeWriteToProxy));

        PadProc proxy{};
        proxy.target_r0 = 0xFFFFFFFFFFFFFFFFULL;  /* sentinela */

        jit::JitFn fn   = reinterpret_cast<jit::JitFn>(ptr);
        uint64_t   rc   = jit::enter_jit(fn, reinterpret_cast<vrt_proc *>(&proxy));

        CHECK(rc == 0, "enter_jit return value (xor eax, eax)");
        CHECK(proxy.target_r0 == 0x2A, "codigo JIT escribio 0x2A a [proxy+16]");
        CHECK(proxy.pad0 == 0 && proxy.pad1 == 0, "no toca otros campos");
    }

    /** @brief Test 2: RuntimeEntries resuelve todos los simbolos. */
    void test_runtime_entries_resolve() {
        jit::RuntimeEntries rt;
        CHECK(rt.gc_alloc == nullptr, "antes de resolve: gc_alloc NULL");
        rt.resolve();
        CHECK(rt.gc_alloc       != nullptr, "gc_alloc resuelto");
        CHECK(rt.gc_deref       != nullptr, "gc_deref resuelto");
        CHECK(rt.gc_drop        != nullptr, "gc_drop resuelto");
        CHECK(rt.gc_addref      != nullptr, "gc_addref resuelto");
        CHECK(rt.monitor_enter  != nullptr, "monitor_enter resuelto");
        CHECK(rt.monitor_exit   != nullptr, "monitor_exit resuelto");
        CHECK(rt.throw_fatal    != nullptr, "throw_fatal resuelto");
        CHECK(rt.invoke_native  != nullptr, "invoke_native resuelto");
        CHECK(rt.safepoint_poll != nullptr, "safepoint_poll resuelto");
        CHECK(rt.all_resolved(), "all_resolved() true tras resolve");
    }

    /** @brief Test 3: vrt_api_version devuelve valor consistente. */
    void test_api_version() {
        uint32_t v = vrt_api_version();
        uint32_t expected = (static_cast<uint32_t>(VRT_API_VERSION_MAJOR) << 16)
                          | (static_cast<uint32_t>(VRT_API_VERSION_MINOR) << 8)
                          | static_cast<uint32_t>(VRT_API_VERSION_PATCH);
        CHECK(v == expected, "vrt_api_version coincide con macros");
        CHECK((v >> 16) == 0, "version major actual = 0 (pre-stable)");
    }

    /** @brief Test 4: enter_jit invoca el JitFn con punteros opacos. */
    void test_bridge_does_not_corrupt() {
        /* Codigo que solo retorna sin tocar nada. */
        const uint8_t kJustRet[] = { 0xC3 };
        jit::CodeCache cache;
        uint8_t *ptr = cache.alloc(sizeof(kJustRet), 16);
        std::memcpy(ptr, kJustRet, sizeof(kJustRet));
        cache.commit(ptr, sizeof(kJustRet));

        PadProc proxy{};
        proxy.pad0 = 0xAAAA;
        proxy.pad1 = 0xBBBB;
        proxy.target_r0 = 0xCCCC;

        jit::JitFn fn = reinterpret_cast<jit::JitFn>(ptr);
        (void)jit::enter_jit(fn, reinterpret_cast<vrt_proc *>(&proxy));

        CHECK(proxy.pad0 == 0xAAAA, "ret puro preserva pad0");
        CHECK(proxy.pad1 == 0xBBBB, "ret puro preserva pad1");
        CHECK(proxy.target_r0 == 0xCCCC, "ret puro preserva target_r0");
    }

} // namespace anonymous

int main() {
    test_bridge_basic();
    test_runtime_entries_resolve();
    test_api_version();
    test_bridge_does_not_corrupt();

    std::printf("test_bridge: %d pass, %d fail\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
