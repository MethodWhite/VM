/**
 * @file native_callback.cpp
 * @brief Generador de thunks C-callable para callbacks Vex -> C nativos.
 *
 * Sprint B.1 (2026-06-01).  Para cada (vex_fn_pc, argc) genera un thunk
 * que:
 *   1. Salva args nativos (calling convention C: Win64 o SysV).
 *   2. Lee ProcessVM* desde TLS via @c get_current_executing_process.
 *   3. Copia los args a @c proc->registers.regs[R01..R<argc>].
 *   4. Setea @c R15 = argc.
 *   5. Invoca el codigo JIT-compilado de la fn Vex (cc VM_ABI: arg0=proc).
 *   6. Lee @c proc->registers.regs[R00] a RAX (return value C).
 *   7. Restaura y retorna.
 *
 * El thunk es ~80 bytes de x86-64 nativo, alocado una sola vez por
 * (pc, argc) en el code_cache JIT.
 */

#include "runtime/native_callback.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "jit/auto_jit.h"
#include "jit/code_cache.h"
#include "runtime/exception_runtime.h"
#include "vesta_rt/abi.h"
#include "ffi/virtual_lib_registry.h"
#include <cstdio>
#include <cstdlib>

namespace runtime {

namespace {

    /* Cache global (vex_fn_pc, argc) -> thunk_ptr.  Sin overhead tras
     * la primera invocacion; mutex solo para concurrent registers. */
    std::mutex                                                   g_thunk_mtx;
    std::unordered_map<uint64_t, uint64_t>                       g_thunk_cache;

    /* key = (pc << 8) | argc.  Suficiente: pc cabe en 56 bits en
     * cualquier mapping VM real, argc <= 6 cabe en 8. */
    static uint64_t make_key(uint64_t pc, uint32_t argc) {
        return (pc << 8) | (argc & 0xFFu);
    }

    /* Helpers para emitir bytes x86-64 directos. */
    static void emit_u8 (uint8_t **p, uint8_t  v) { *(*p)++ = v; }
    static void emit_u32(uint8_t **p, uint32_t v) {
        std::memcpy(*p, &v, 4);
        *p += 4;
    }
    static void emit_u64(uint8_t **p, uint64_t v) {
        std::memcpy(*p, &v, 8);
        *p += 8;
    }

} // namespace anonymous

uint64_t get_or_generate_native_thunk(uint64_t vex_fn_pc, uint32_t argc) {
    /* Limite: 12 args (mismo limite que la calling convention VM_ABI:
     * R01..R12 son arg regs, R13/R14 scratch, R15 = argc).  Args extras
     * vienen en stack del caller segun el ABI nativo:
     *   - Win64: args 5..N en [caller_rsp + 0x20 + (i-4)*8].
     *   - SysV : args 7..N en [caller_rsp + (i-6)*8].
     * El thunk los lee con un disp ajustado por su propio frame. */
    if (argc > 12) return 0;

    /* Resolver el JIT code ptr de la fn Vex.  Si no esta compilada
     * todavia, el callback no puede funcionar; el wrapper
     * `vex_get_native_thunk` fuerza el JIT compile antes de llegar aqui. */
    void *jit_code = jit::lookup_jit_code_at_pc(vex_fn_pc);
    if (jit_code == nullptr) return 0;

    /* Cache lookup. */
    uint64_t key = make_key(vex_fn_pc, argc);
    {
        std::lock_guard<std::mutex> lk(g_thunk_mtx);
        auto it = g_thunk_cache.find(key);
        if (it != g_thunk_cache.end()) return it->second;
    }

    /* Cache miss: generar el thunk. */
    jit::CodeCache *cc = jit::get_or_init_code_cache();
    if (cc == nullptr) return 0;

    /* Reservar 1024 bytes -- suficiente para argc=12 con SAVE/RESTORE
     * de proc->regs[R0..R15] (re-entrancia).  Caso peor: ~700 bytes;
     * dejamos margen para futuras extensiones (stackmaps, prologue
     * extendido, etc). */
    uint8_t *base = cc->alloc(1024, 16);
    if (base == nullptr) return 0;

    uint8_t *p = base;

    /* Direcciones que necesitamos como imm64. */
    const uint64_t addr_get_proc = reinterpret_cast<uint64_t>(
        &runtime::get_current_executing_process);
    const uint64_t addr_jit_fn   = reinterpret_cast<uint64_t>(jit_code);
    const int32_t  REGS_OFF      = VESTA_PROC_REGISTERS_OFFSET;
    const int32_t  R15_OFF       = REGS_OFF + 15 * 8;
    const int32_t  R0_OFF        = REGS_OFF + 0  * 8;

    /* Helper: emit `mov rax, [rsp + disp]` con seleccion automatica de
     * disp8 vs disp32 segun el rango.  Garantiza encoding compacto cuando
     * cabe en signed 8-bit. */
    auto emit_mov_rax_from_rsp = [&](int32_t disp) {
        if (disp >= -128 && disp <= 127) {
            /* mov rax, [rsp + disp8]: 48 8B 44 24 db */
            emit_u8(&p, 0x48); emit_u8(&p, 0x8B);
            emit_u8(&p, 0x44); emit_u8(&p, 0x24);
            emit_u8(&p, (uint8_t)(int8_t)disp);
        } else {
            /* mov rax, [rsp + disp32]: 48 8B 84 24 dd */
            emit_u8(&p, 0x48); emit_u8(&p, 0x8B);
            emit_u8(&p, 0x84); emit_u8(&p, 0x24);
            emit_u32(&p, (uint32_t)disp);
        }
    };
    /* Helper: emit `mov [rbx + disp32], rax` (siempre disp32 porque los
     * offsets de regs son > 127 al cubrir hasta R15). */
    auto emit_mov_rbx_off_from_rax = [&](int32_t disp) {
        emit_u8(&p, 0x48); emit_u8(&p, 0x89); emit_u8(&p, 0x83);
        emit_u32(&p, (uint32_t)disp);
    };

#if defined(_WIN32)
    /* Win64 native ABI:
     *   - Args 0..3 en RCX, RDX, R8, R9 (registros).
     *   - Args 4..N en stack del caller en [caller_rsp + 0x20 + (i-4)*8].
     *   - Callee-saved: RBX, RBP, RDI, RSI, R12-R15.
     *
     * Layout del thunk (alignment 16-byte garantizado):
     *
     *   Pre-thunk: rsp = X (siempre 16-aligned - 8, por el push del CALL).
     *   push rbx               -> rsp = X - 8  = 16-aligned.
     *   sub rsp, 0xC0 (192)    -> rsp = X - 200 = 16-aligned.
     *
     *   Frame del thunk (rsp+0..191):
     *     [rsp+0x00..0x1F] shadow space para sub-calls del thunk.
     *     [rsp+0x20] = saved RCX (arg0 nativo).
     *     [rsp+0x28] = saved RDX (arg1 nativo).
     *     [rsp+0x30] = saved R8  (arg2 nativo).
     *     [rsp+0x38] = saved R9  (arg3 nativo).
     *     [rsp+0x40..0xBF] = save area para proc->regs[R0..R15] (128 bytes).
     *
     *   El save area garantiza RE-ENTRANCIA del callback: si el caller
     *   Vex tenia state vivo en proc->regs[], el thunk lo restaura tras
     *   invocar la funcion callback.  Sin esto, una segunda invocacion
     *   del callback corrompe el state del caller. */

    /* Frame del thunk (Win64).  Layout:
     *   [rsp+0x00..0x1F]: SHADOW SPACE para CALLs internos del thunk.
     *   [rsp+0x20..0x3F]: arg saves (RCX/RDX/R8/R9 spillados).
     *   [rsp+0x40..0xBF]: save area para proc->regs[R0..R15] (128 bytes).
     *                     Garantiza re-entrancia del callback.
     *
     * Total: 192 bytes = 0xC0.
     *
     * Alignment del rsp interno (para que CALL al JIT mantenga ABI):
     *   - Pre-CALL al thunk: rsp = 16-aligned.
     *   - Post-CALL (entry): rsp = 16-aligned - 8.
     *   - push rbx (-8): rsp = 16-aligned - 16 = 16-aligned.
     *   - sub rsp, WIN_FRAME: rsp = 16-aligned - WIN_FRAME.
     *   Para que esto siga siendo 16-aligned, WIN_FRAME debe ser
     *   MULTIPLO de 16.  192 = 12 * 16 -> 16-aligned. */
    constexpr int32_t WIN_FRAME = 0xC0;     /* 192 bytes (multiplo de 16) */
    constexpr int32_t WIN_REG_SLOT_BASE = 0x20;  /* offset al primer slot de arg saves */
    constexpr int32_t WIN_PROC_SAVE_BASE = 0x40; /* offset al save area de proc->regs (128 bytes en 0x40..0xBF) */

    /* push rbx */
    emit_u8(&p, 0x53);
    /* sub rsp, WIN_FRAME.  Si <= 127 usa imm8 (4 bytes); sino imm32 (7 bytes). */
    if (WIN_FRAME <= 127) {
        emit_u8(&p, 0x48); emit_u8(&p, 0x83); emit_u8(&p, 0xEC); emit_u8(&p, (uint8_t)WIN_FRAME);
    } else {
        /* 48 81 EC id  -- sub rsp, imm32. */
        emit_u8(&p, 0x48); emit_u8(&p, 0x81); emit_u8(&p, 0xEC);
        emit_u32(&p, (uint32_t)WIN_FRAME);
    }

    /* Save args registrados (RCX/RDX/R8/R9) en slots del thunk. */
    /* mov [rsp+0x20], rcx */
    emit_u8(&p, 0x48); emit_u8(&p, 0x89); emit_u8(&p, 0x4C); emit_u8(&p, 0x24); emit_u8(&p, 0x20);
    /* mov [rsp+0x28], rdx */
    emit_u8(&p, 0x48); emit_u8(&p, 0x89); emit_u8(&p, 0x54); emit_u8(&p, 0x24); emit_u8(&p, 0x28);
    /* mov [rsp+0x30], r8 */
    emit_u8(&p, 0x4C); emit_u8(&p, 0x89); emit_u8(&p, 0x44); emit_u8(&p, 0x24); emit_u8(&p, 0x30);
    /* mov [rsp+0x38], r9 */
    emit_u8(&p, 0x4C); emit_u8(&p, 0x89); emit_u8(&p, 0x4C); emit_u8(&p, 0x24); emit_u8(&p, 0x38);

    /* TLS-direct (Win64): leer ProcessVM* desde gs:[0x1480 + idx*8].
     * Si el slot no esta en los primeros 64 (TLS_MINIMUM_AVAILABLE), o
     * la API no se inicializo, fallback al call. */
    const unsigned long tls_idx = runtime::jit_proc_tls_index();
    bool tls_direct_ok = false;
    if (tls_idx != 0xFFFFFFFFu && tls_idx < 64) {
        const uint32_t tls_disp = 0x1480u + tls_idx * 8u;
        emit_u8(&p, 0x65); emit_u8(&p, 0x48); emit_u8(&p, 0x8B);
        emit_u8(&p, 0x1C); emit_u8(&p, 0x25);
        emit_u32(&p, tls_disp);
        tls_direct_ok = true;
    }
    if (!tls_direct_ok) {
        /* mov rax, imm64; call rax; mov rbx, rax. */
        emit_u8(&p, 0x48); emit_u8(&p, 0xB8);
        emit_u64(&p, addr_get_proc);
        emit_u8(&p, 0xFF); emit_u8(&p, 0xD0);
        emit_u8(&p, 0x48); emit_u8(&p, 0x89); emit_u8(&p, 0xC3);
    }

    /* SAVE proc->regs[R0..R15] al save area.  Necesario para re-entrancia:
     * el caller Vex puede tener state vivo en regs[]; el callback lo
     * restaura al salir.  Sin esto, callbacks anidados o multiples
     * invocaciones consecutivas corrompen el state del caller. */
    for (uint32_t i = 0; i < 16; ++i) {
        /* mov rax, [rbx + REGS_OFF + i*8] */
        emit_u8(&p, 0x48); emit_u8(&p, 0x8B); emit_u8(&p, 0x83);
        emit_u32(&p, (uint32_t)(REGS_OFF + (int32_t)i * 8));
        /* mov [rsp + WIN_PROC_SAVE_BASE + i*8], rax */
        const int32_t save_off = WIN_PROC_SAVE_BASE + (int32_t)i * 8;
        if (save_off >= -128 && save_off <= 127) {
            emit_u8(&p, 0x48); emit_u8(&p, 0x89);
            emit_u8(&p, 0x44); emit_u8(&p, 0x24);
            emit_u8(&p, (uint8_t)(int8_t)save_off);
        } else {
            emit_u8(&p, 0x48); emit_u8(&p, 0x89);
            emit_u8(&p, 0x84); emit_u8(&p, 0x24);
            emit_u32(&p, (uint32_t)save_off);
        }
    }

    /* Copiar args 0..min(argc, 4)-1 desde slots del thunk a proc->regs[R1..R4]. */
    const uint32_t reg_args_count = (argc < 4) ? argc : 4;
    for (uint32_t i = 0; i < reg_args_count; ++i) {
        const int32_t slot_off = WIN_REG_SLOT_BASE + (int32_t)i * 8;
        emit_mov_rax_from_rsp(slot_off);
        emit_mov_rbx_off_from_rax(REGS_OFF + (int32_t)(i + 1) * 8);
    }

    /* Copiar args 4..argc-1 desde stack del CALLER. */
    const int32_t WIN_CALLER_STACK_BASE = WIN_FRAME + 8 + 8 + 0x20;
    for (uint32_t i = 4; i < argc; ++i) {
        const int32_t stack_off = WIN_CALLER_STACK_BASE + (int32_t)(i - 4) * 8;
        emit_mov_rax_from_rsp(stack_off);
        emit_mov_rbx_off_from_rax(REGS_OFF + (int32_t)(i + 1) * 8);
    }

    /* mov rax, argc (imm32 zero-extended) */
    emit_u8(&p, 0x48); emit_u8(&p, 0xC7); emit_u8(&p, 0xC0);
    emit_u32(&p, argc);
    /* mov [rbx + R15_OFF], rax  (proc->regs[R15] = argc) */
    emit_u8(&p, 0x48); emit_u8(&p, 0x89); emit_u8(&p, 0x83);
    emit_u32(&p, (uint32_t)R15_OFF);

    /* Llamar JIT code de la fn Vex.  cc VM_ABI: arg0 = proc en RCX. */
    emit_u8(&p, 0x48); emit_u8(&p, 0x89); emit_u8(&p, 0xD9);  /* mov rcx, rbx */
    emit_u8(&p, 0x48); emit_u8(&p, 0xB8);
    emit_u64(&p, addr_jit_fn);
    emit_u8(&p, 0xFF); emit_u8(&p, 0xD0);                      /* call rax */

    /* RBX preservada por VM_ABI.  Leer return value de proc->regs[R0] -> RAX
     * (eso es el return value del callback, que retornaremos al caller C). */
    emit_u8(&p, 0x48); emit_u8(&p, 0x8B); emit_u8(&p, 0x83);
    emit_u32(&p, (uint32_t)R0_OFF);

    /* RESTORE proc->regs[R1..R15] desde save area.  No restauramos R0
     * porque acabamos de leerlo como return value (y el caller no espera
     * que ese reg sobreviva al call).
     *
     * Usamos RCX como scratch (caller-saved en Win64, no es necesario
     * salvarlo dentro del thunk porque ya retornamos el valor en RAX). */
    for (uint32_t i = 1; i < 16; ++i) {
        /* mov rcx, [rsp + WIN_PROC_SAVE_BASE + i*8] */
        const int32_t save_off = WIN_PROC_SAVE_BASE + (int32_t)i * 8;
        if (save_off >= -128 && save_off <= 127) {
            emit_u8(&p, 0x48); emit_u8(&p, 0x8B);
            emit_u8(&p, 0x4C); emit_u8(&p, 0x24);
            emit_u8(&p, (uint8_t)(int8_t)save_off);
        } else {
            emit_u8(&p, 0x48); emit_u8(&p, 0x8B);
            emit_u8(&p, 0x8C); emit_u8(&p, 0x24);
            emit_u32(&p, (uint32_t)save_off);
        }
        /* mov [rbx + REGS_OFF + i*8], rcx */
        emit_u8(&p, 0x48); emit_u8(&p, 0x89); emit_u8(&p, 0x8B);
        emit_u32(&p, (uint32_t)(REGS_OFF + (int32_t)i * 8));
    }

    /* Epilogue: add rsp, WIN_FRAME ; pop rbx ; ret */
    if (WIN_FRAME <= 127) {
        emit_u8(&p, 0x48); emit_u8(&p, 0x83); emit_u8(&p, 0xC4); emit_u8(&p, (uint8_t)WIN_FRAME);
    } else {
        /* 48 81 C4 id -- add rsp, imm32. */
        emit_u8(&p, 0x48); emit_u8(&p, 0x81); emit_u8(&p, 0xC4);
        emit_u32(&p, (uint32_t)WIN_FRAME);
    }
    emit_u8(&p, 0x5B);
    emit_u8(&p, 0xC3);

#else
    /* System V AMD64 (Linux, macOS):
     *   - Args 0..5 en RDI, RSI, RDX, RCX, R8, R9 (registros).
     *   - Args 6..N en stack del caller en [caller_rsp + (i-6)*8].
     *   - Callee-saved: RBX, RBP, R12-R15.  Sin shadow space.
     *
     * Layout del thunk:
     *
     *   Pre-thunk: rsp = X (16-aligned - 8 por el CALL).
     *   push rbx               -> rsp = X - 8  = 16-aligned.
     *   sub rsp, 0x30 (48)     -> rsp = X - 56 = 16-aligned.
     *
     *   Frame del thunk (rsp+0..47):
     *     [rsp+0x00] = saved RDI (arg0).
     *     [rsp+0x08] = saved RSI (arg1).
     *     [rsp+0x10] = saved RDX (arg2).
     *     [rsp+0x18] = saved RCX (arg3).
     *     [rsp+0x20] = saved R8  (arg4).
     *     [rsp+0x28] = saved R9  (arg5).
     *
     *   Args 6+ en stack del caller (offset 56 + 8 = 64 desde rsp actual). */

    /* Frame del thunk (SysV).  Layout:
     *   [rsp+0x00..0x2F]: arg saves (RDI/RSI/RDX/RCX/R8/R9 spillados).
     *   [rsp+0x30..0xAF]: save area para proc->regs[R0..R15] (128 bytes).
     *
     * Total: 176 bytes = 0xB0 (multiplo de 16 -> 16-aligned tras
     * push rbx + sub rsp). */
    constexpr int32_t SV_FRAME = 0xB0;     /* 176 bytes (multiplo de 16) */
    constexpr int32_t SV_PROC_SAVE_BASE = 0x30; /* offset al save area de proc->regs (128 bytes en 0x30..0xAF) */

    /* push rbx */
    emit_u8(&p, 0x53);
    /* sub rsp, SV_FRAME (imm32 porque > 127). */
    emit_u8(&p, 0x48); emit_u8(&p, 0x81); emit_u8(&p, 0xEC);
    emit_u32(&p, (uint32_t)SV_FRAME);

    /* Save args 0..5 (RDI/RSI/RDX/RCX/R8/R9) en slots del thunk. */
    /* mov [rsp+0x00], rdi */
    emit_u8(&p, 0x48); emit_u8(&p, 0x89); emit_u8(&p, 0x3C); emit_u8(&p, 0x24);
    /* mov [rsp+0x08], rsi */
    emit_u8(&p, 0x48); emit_u8(&p, 0x89); emit_u8(&p, 0x74); emit_u8(&p, 0x24); emit_u8(&p, 0x08);
    /* mov [rsp+0x10], rdx */
    emit_u8(&p, 0x48); emit_u8(&p, 0x89); emit_u8(&p, 0x54); emit_u8(&p, 0x24); emit_u8(&p, 0x10);
    /* mov [rsp+0x18], rcx */
    emit_u8(&p, 0x48); emit_u8(&p, 0x89); emit_u8(&p, 0x4C); emit_u8(&p, 0x24); emit_u8(&p, 0x18);
    /* mov [rsp+0x20], r8 */
    emit_u8(&p, 0x4C); emit_u8(&p, 0x89); emit_u8(&p, 0x44); emit_u8(&p, 0x24); emit_u8(&p, 0x20);
    /* mov [rsp+0x28], r9 */
    emit_u8(&p, 0x4C); emit_u8(&p, 0x89); emit_u8(&p, 0x4C); emit_u8(&p, 0x24); emit_u8(&p, 0x28);

    /* mov rax, imm64 (get_current_executing_process); call rax; mov rbx, rax. */
    emit_u8(&p, 0x48); emit_u8(&p, 0xB8);
    emit_u64(&p, addr_get_proc);
    emit_u8(&p, 0xFF); emit_u8(&p, 0xD0);
    emit_u8(&p, 0x48); emit_u8(&p, 0x89); emit_u8(&p, 0xC3);

    /* SAVE proc->regs[R0..R15] al save area del thunk para re-entrancia. */
    for (uint32_t i = 0; i < 16; ++i) {
        /* mov rax, [rbx + REGS_OFF + i*8] */
        emit_u8(&p, 0x48); emit_u8(&p, 0x8B); emit_u8(&p, 0x83);
        emit_u32(&p, (uint32_t)(REGS_OFF + (int32_t)i * 8));
        /* mov [rsp + SV_PROC_SAVE_BASE + i*8], rax */
        const int32_t save_off = SV_PROC_SAVE_BASE + (int32_t)i * 8;
        if (save_off >= -128 && save_off <= 127) {
            emit_u8(&p, 0x48); emit_u8(&p, 0x89);
            emit_u8(&p, 0x44); emit_u8(&p, 0x24);
            emit_u8(&p, (uint8_t)(int8_t)save_off);
        } else {
            emit_u8(&p, 0x48); emit_u8(&p, 0x89);
            emit_u8(&p, 0x84); emit_u8(&p, 0x24);
            emit_u32(&p, (uint32_t)save_off);
        }
    }

    /* Copiar args 0..min(argc,6)-1 desde slots del thunk. */
    const uint32_t reg_args_count = (argc < 6) ? argc : 6;
    for (uint32_t i = 0; i < reg_args_count; ++i) {
        const int32_t slot_off = (int32_t)i * 8;
        emit_mov_rax_from_rsp(slot_off);
        emit_mov_rbx_off_from_rax(REGS_OFF + (int32_t)(i + 1) * 8);
    }

    /* Copiar args 6..argc-1 desde stack del CALLER.
     * Offset desde rsp actual:
     *   stack_off = SV_FRAME + 8 (push rbx) + 8 (retaddr) + (i-6)*8.
     *   Para SV_FRAME=192: stack_off = 208 + (i-6)*8. */
    const int32_t SV_CALLER_STACK_BASE = SV_FRAME + 8 + 8;
    for (uint32_t i = 6; i < argc; ++i) {
        const int32_t stack_off = SV_CALLER_STACK_BASE + (int32_t)(i - 6) * 8;
        emit_mov_rax_from_rsp(stack_off);
        emit_mov_rbx_off_from_rax(REGS_OFF + (int32_t)(i + 1) * 8);
    }

    /* mov rax, argc; mov [rbx + R15_OFF], rax. */
    emit_u8(&p, 0x48); emit_u8(&p, 0xC7); emit_u8(&p, 0xC0);
    emit_u32(&p, argc);
    emit_u8(&p, 0x48); emit_u8(&p, 0x89); emit_u8(&p, 0x83);
    emit_u32(&p, (uint32_t)R15_OFF);

    /* Llamar JIT code: cc VM_ABI arg0 = proc en RDI. */
    emit_u8(&p, 0x48); emit_u8(&p, 0x89); emit_u8(&p, 0xDF);  /* mov rdi, rbx */
    emit_u8(&p, 0x48); emit_u8(&p, 0xB8);
    emit_u64(&p, addr_jit_fn);
    emit_u8(&p, 0xFF); emit_u8(&p, 0xD0);                      /* call rax */

    /* Leer return value de proc->regs[R0] -> RAX. */
    emit_u8(&p, 0x48); emit_u8(&p, 0x8B); emit_u8(&p, 0x83);
    emit_u32(&p, (uint32_t)R0_OFF);

    /* RESTORE proc->regs[R1..R15] desde save area.  Usamos RCX como scratch
     * (caller-saved en SysV).  R0 lo dejamos pisado por el return value. */
    for (uint32_t i = 1; i < 16; ++i) {
        const int32_t save_off = SV_PROC_SAVE_BASE + (int32_t)i * 8;
        if (save_off >= -128 && save_off <= 127) {
            emit_u8(&p, 0x48); emit_u8(&p, 0x8B);
            emit_u8(&p, 0x4C); emit_u8(&p, 0x24);
            emit_u8(&p, (uint8_t)(int8_t)save_off);
        } else {
            emit_u8(&p, 0x48); emit_u8(&p, 0x8B);
            emit_u8(&p, 0x8C); emit_u8(&p, 0x24);
            emit_u32(&p, (uint32_t)save_off);
        }
        emit_u8(&p, 0x48); emit_u8(&p, 0x89); emit_u8(&p, 0x8B);
        emit_u32(&p, (uint32_t)(REGS_OFF + (int32_t)i * 8));
    }

    /* Epilogue: add rsp, SV_FRAME (imm32); pop rbx; ret. */
    emit_u8(&p, 0x48); emit_u8(&p, 0x81); emit_u8(&p, 0xC4);
    emit_u32(&p, (uint32_t)SV_FRAME);
    emit_u8(&p, 0x5B);
    emit_u8(&p, 0xC3);

#endif

    const size_t code_size = (size_t)(p - base);
    cc->commit(base, code_size);

    /* Debug opcional: VESTA_THUNK_DISASM=1 dumpea los bytes generados. */
    const char *dbg = std::getenv("VESTA_THUNK_DISASM");
    if (dbg && dbg[0] != '\0' && dbg[0] != '0') {
        std::fprintf(stderr, "[thunk] argc=%u size=%zu bytes addr=%p\n",
            argc, code_size, (void *)base);
        for (size_t i = 0; i < code_size; ++i) {
            if (i % 16 == 0) std::fprintf(stderr, "  %04zx:", i);
            std::fprintf(stderr, " %02x", base[i]);
            if ((i + 1) % 16 == 0) std::fprintf(stderr, "\n");
        }
        if (code_size % 16 != 0) std::fprintf(stderr, "\n");
        std::fflush(stderr);
    }

    const uint64_t thunk_ptr = reinterpret_cast<uint64_t>(base);
    {
        std::lock_guard<std::mutex> lk(g_thunk_mtx);
        g_thunk_cache[key] = thunk_ptr;
    }
    return thunk_ptr;
}

} // namespace runtime

/* Wrapper extern "C" para que el builtin Vex `as_native_callback(fn)` lo
 * invoque via CALLN.  Args en proc->registers.regs[R01]=fn_pc, R02=argc.
 * Retorno en proc->registers.regs[R00] = thunk_ptr (host_ptr).
 *
 * Registrado en TypeChecker init via `register_virtual_fn("vesta_runtime",
 * "vex_get_native_thunk", ...)`. */
extern "C" uint64_t vex_get_native_thunk(uint64_t fn_pc, uint64_t argc) {
    /* Forzar JIT compile de la fn Vex si aun no esta compilada.  El
     * thunk requiere JIT activo; sin JIT compile el callback no puede
     * funcionar.
     *
     * Sprint JIT-cross-fn 2026-06-01: tras anyadir `register_jit_code_at_pc`
     * al cascade resolver (auto_jit.cpp), las fns compiladas via cascade
     * desde otra fn JIT-eada YA quedan registradas.  Si todavia
     * `lookup_jit_code_at_pc` retorna null, forzamos un compile via
     * `maybe_compile_callvm_target`. */
    if (jit::lookup_jit_code_at_pc(fn_pc) == nullptr) {
        runtime::ProcessVM *proc = runtime::get_current_executing_process();
        if (proc != nullptr) {
            uint32_t saved = jit::g_jit_threshold;
            if (saved == UINT32_MAX) jit::set_jit_threshold(1);
            jit::maybe_compile_callvm_target(proc, fn_pc);
            if (saved == UINT32_MAX) jit::set_jit_threshold(saved);
        }
    }
    return runtime::get_or_generate_native_thunk(fn_pc,
                                                  (uint32_t)(argc & 0xFFFFFFFFu));
}

/* Auto-registro en el virtual_lib_registry al cargar el binario.  Asi
 * tanto el path --vex (compile) como --run (ejecutar .velb) tienen la
 * fn disponible sin necesidad de llamar al TypeChecker constructor.
 *
 * `__attribute__((used))` evita que `-Wl,--gc-sections` elimine el
 * objeto (no es referenciado desde main).  `__attribute__((constructor))`
 * fuerza ejecucion antes de main, garantizado por GCC/Clang. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((used, constructor))
static void vex_callback_register_virtual_fn() {
    ffi::register_virtual_fn(
        "vesta_runtime", "vex_get_native_thunk",
        reinterpret_cast<void *>(&::vex_get_native_thunk));
}
#else
namespace {
    struct VexCallbackAutoRegister {
        VexCallbackAutoRegister() {
            ffi::register_virtual_fn(
                "vesta_runtime", "vex_get_native_thunk",
                reinterpret_cast<void *>(&::vex_get_native_thunk));
        }
    };
    static VexCallbackAutoRegister _vex_callback_auto_register;
}
#endif

/* Helper que el main.cpp puede llamar explicitamente para forzar el
 * registro (defense-in-depth si el constructor no se ejecuta por algun
 * motivo de linking). */
extern "C" void runtime_ensure_vex_callback_registered(void) {
    ffi::register_virtual_fn(
        "vesta_runtime", "vex_get_native_thunk",
        reinterpret_cast<void *>(&::vex_get_native_thunk));
}
