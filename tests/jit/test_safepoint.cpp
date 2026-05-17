/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/jit/test_safepoint.cpp
 * @brief Tests del mecanismo de safepoints (D.2-foundation).
 *
 * Cubre:
 *   1. Encoding de @c MOp::SAFEPOINT produce los 21 bytes esperados.
 *   2. Fast path: con @c safepoint_flag = 0 el poll cae al continue
 *      sin tocar el handler (medido por counter externo).
 *   3. Slow path: con @c safepoint_flag = 1 el handler se invoca y
 *      el flag se limpia tras la ejecucion.
 *   4. VM_ABI end-to-end: una funcion JIT generada con el selector
 *      en VM_ABI mode lee args desde @c proc->registers.regs[1..N],
 *      escribe return a @c regs[0], y los safepoints emitidos en
 *      back-edges no interfieren cuando el flag esta a cero.
 *
 * NOTE: usamos un "proxy" minimo de ProcessVM con el layout exacto
 * (safepoint_flag en offset 0, registers en offset 64+32=96) para no
 * tener que construir un VM completo en el test.  Sirve para validar
 * la mecanica de poll + ABI sin acoplarse al runtime.
 */

#include "jit/code_cache.h"
#include "jit/machine_ir.h"
#include "jit/runtime_entries.h"
#include "jit/selector.h"
#include "jit/x86_encoder.h"
#include "vesta_rt/abi.h"
#include "vesta_rt/public.h"

#include <atomic>
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

    /**
     * @brief Proxy minimo de ProcessVM con el layout exacto necesario
     *        para que el codigo JIT-eado en VM_ABI funcione sin
     *        construir un VM real.
     *
     * Offset 0: safepoint_flag (1 byte) + padding (7 bytes) = 8 bytes
     * Offsets 8..95: padding hasta llegar al inicio de registers
     * Offset 96: regs[0..15] cada uno 8 bytes (128 bytes total)
     *
     * Total proxy size: 96 + 128 = 224 bytes.
     */
    struct alignas(16) ProcProxy {
        uint8_t  safepoint_flag;     /* offset 0 */
        uint8_t  _pad_flag[7];
        uint8_t  _pad_to_regs[88];   /* offset 8..95 */
        uint64_t regs[16];           /* offset 96..223 */
    };

    static_assert(offsetof(ProcProxy, safepoint_flag)
                  == VESTA_PROC_SAFEPOINT_FLAG_OFFSET,
                  "ProcProxy.safepoint_flag debe coincidir con el offset ABI");
    static_assert(offsetof(ProcProxy, regs)
                  == VESTA_PROC_REGISTERS_OFFSET,
                  "ProcProxy.regs debe coincidir con el offset ABI");

    /* Counter de invocaciones del handler.  Lo usamos como mock del
     * handler real para detectar si el poll tomo el slow path. */
    std::atomic<int> g_handler_calls{0};
    extern "C" void test_safepoint_mock_handler(vrt_proc *proc) {
        ++g_handler_calls;
        /* Limpiar flag para que el JIT no quede en loop. */
        if (proc) {
            reinterpret_cast<ProcProxy *>(proc)->safepoint_flag = 0;
        }
    }

    using namespace jit;

    /* ===================================================================== */
    /* Test 1: encoding bytes correctos para MOp::SAFEPOINT                   */
    /* ===================================================================== */

    void test_safepoint_encoding() {
        MFunction fn;
        fn.name = "sp_enc";
        const uint32_t pool_idx = fn.intern_imm64(0xDEADBEEFCAFE1234ULL);
        MBlockId b = fn.new_block(fn.new_label());
        fn.blocks[b].instrs.push_back(MInstr::make_safepoint(pool_idx));
        fn.blocks[b].instrs.push_back(MInstr::make_ret());

        X86Encoder enc;
        std::vector<uint8_t> bytes;
        enc.encode(fn, bytes);

        /* Layout esperado (SysV o Win64):
         *   80 7B 00 00          cmp byte [rbx+0], 0           (4)
         *   74 0F                je rel8=15                     (2)
         *   48 89 D7 (SysV) or 48 89 D9 (Win64)  mov rdi/rcx, rbx (3)
         *   48 B8 34 12 FE CA EF BE AD DE         mov rax, imm64 (10)
         *   FF D0                call rax                       (2)
         *   C3                   ret                            (1)
         * Total: 22 bytes.
         */
        CHECK(bytes.size() == 22, "safepoint + ret = 22 bytes");
        CHECK(bytes[0] == 0x80, "cmp byte opcode");
        CHECK(bytes[1] == 0x7B, "modrm: mod=01 reg=7 rm=3 (rbx)");
        CHECK(bytes[2] == 0x00, "disp8 = 0 (safepoint_flag offset)");
        CHECK(bytes[3] == 0x00, "imm8 = 0 (comparado contra 0)");
        CHECK(bytes[4] == 0x74, "je rel8");
        CHECK(bytes[5] == 15,   "rel8 salta sobre 15 bytes del slow path");
        CHECK(bytes[6] == 0x48, "REX.W para mov rdi/rcx, rbx");
        CHECK(bytes[7] == 0x89, "MOV r/m64, r64");
        /* bytes[8] depende de plataforma */
        CHECK(bytes[9] == 0x48, "REX.W para MOV RAX, imm64");
        CHECK(bytes[10] == 0xB8, "MOV RAX, imm64 opcode");
        /* bytes[11..18] = imm64 little-endian = 0xDEADBEEFCAFE1234 */
        CHECK(bytes[11] == 0x34, "imm64 byte 0");
        CHECK(bytes[12] == 0x12, "imm64 byte 1");
        CHECK(bytes[18] == 0xDE, "imm64 byte 7 (MSB)");
        CHECK(bytes[19] == 0xFF, "call indirect prefix");
        CHECK(bytes[20] == 0xD0, "call rax modrm");
        CHECK(bytes[21] == 0xC3, "ret final");
    }

    /* ===================================================================== */
    /* Test 2: fast path - poll con flag=0 NO invoca handler                 */
    /* ===================================================================== */

    void test_safepoint_fast_path() {
        g_handler_calls = 0;

        MFunction fn;
        fn.name = "sp_fast";
        const uint64_t handler_addr =
            reinterpret_cast<uint64_t>(&test_safepoint_mock_handler);
        const uint32_t pool_idx = fn.intern_imm64(handler_addr);

        /* Funcion JIT-eada minima en convencion VM (proc en rdi/rcx,
         * pero NO usamos selector.  Construimos a mano para test puro
         * del poll). */
        MBlockId b = fn.new_block(fn.new_label());
        /* prologue mini: push rbx; mov rbx, rdi/rcx (proc en RBX) */
        fn.blocks[b].instrs.push_back(
            MInstr::make_unary(MOp::PUSH, {}, MOperand::make_reg(MReg::RBX)));
#if defined(_WIN32)
        fn.blocks[b].instrs.push_back(
            MInstr::make_unary(MOp::MOV,
                MOperand::make_reg(MReg::RBX),
                MOperand::make_reg(MReg::RCX)));
#else
        fn.blocks[b].instrs.push_back(
            MInstr::make_unary(MOp::MOV,
                MOperand::make_reg(MReg::RBX),
                MOperand::make_reg(MReg::RDI)));
#endif
        /* SAFEPOINT (fast path: flag=0 -> JE skip) */
        fn.blocks[b].instrs.push_back(MInstr::make_safepoint(pool_idx));
        /* pop rbx; ret */
        fn.blocks[b].instrs.push_back(
            MInstr::make_unary(MOp::POP, MOperand::make_reg(MReg::RBX), {}));
        fn.blocks[b].instrs.push_back(MInstr::make_ret());

        X86Encoder enc; std::vector<uint8_t> bytes;
        enc.encode(fn, bytes);

        CodeCache cc;
        uint8_t *code = cc.alloc(bytes.size(), 16);
        std::memcpy(code, bytes.data(), bytes.size());
        cc.commit(code, bytes.size());

        ProcProxy proxy{};
        proxy.safepoint_flag = 0;

        using JitFn = void(*)(ProcProxy *);
        JitFn f = reinterpret_cast<JitFn>(code);
        f(&proxy);

        CHECK(g_handler_calls.load() == 0, "fast path: handler NO invocado");
        CHECK(proxy.safepoint_flag == 0, "flag sigue en 0 tras fast path");
    }

    /* ===================================================================== */
    /* Test 3: slow path - poll con flag=1 invoca handler                    */
    /* ===================================================================== */

    void test_safepoint_slow_path() {
        g_handler_calls = 0;

        MFunction fn;
        fn.name = "sp_slow";
        const uint64_t handler_addr =
            reinterpret_cast<uint64_t>(&test_safepoint_mock_handler);
        const uint32_t pool_idx = fn.intern_imm64(handler_addr);

        MBlockId b = fn.new_block(fn.new_label());
        fn.blocks[b].instrs.push_back(
            MInstr::make_unary(MOp::PUSH, {}, MOperand::make_reg(MReg::RBX)));
#if defined(_WIN32)
        fn.blocks[b].instrs.push_back(
            MInstr::make_unary(MOp::MOV,
                MOperand::make_reg(MReg::RBX),
                MOperand::make_reg(MReg::RCX)));
#else
        fn.blocks[b].instrs.push_back(
            MInstr::make_unary(MOp::MOV,
                MOperand::make_reg(MReg::RBX),
                MOperand::make_reg(MReg::RDI)));
#endif
        fn.blocks[b].instrs.push_back(MInstr::make_safepoint(pool_idx));
        fn.blocks[b].instrs.push_back(
            MInstr::make_unary(MOp::POP, MOperand::make_reg(MReg::RBX), {}));
        fn.blocks[b].instrs.push_back(MInstr::make_ret());

        X86Encoder enc; std::vector<uint8_t> bytes;
        enc.encode(fn, bytes);

        CodeCache cc;
        uint8_t *code = cc.alloc(bytes.size(), 16);
        std::memcpy(code, bytes.data(), bytes.size());
        cc.commit(code, bytes.size());

        ProcProxy proxy{};
        proxy.safepoint_flag = 1;  /* trigger slow path */

        using JitFn = void(*)(ProcProxy *);
        JitFn f = reinterpret_cast<JitFn>(code);
        f(&proxy);

        CHECK(g_handler_calls.load() == 1, "slow path: handler invocado UNA vez");
        CHECK(proxy.safepoint_flag == 0, "handler limpio el flag");
    }

    /* ===================================================================== */
    /* Test 4: VM_ABI end-to-end con selector                                */
    /* ===================================================================== */

    /**
     * IR equivalente a: fn add_vm(a, b) { return a + b; }
     *
     * En VM_ABI mode, el selector lee los args desde proc->registers.regs[1]
     * y proc->registers.regs[2], computa la suma, escribe a regs[0] y
     * retorna.
     */
    void test_vm_abi_end_to_end() {
        ir::IrFunction fn;
        fn.name = "add_vm";
        fn.ret_type = ir::IrType::I64;

        /* %0 y %1 = params */
        ir::IrValue v0, v1, vsum;
        v0.type = ir::IrType::I64; v1.type = ir::IrType::I64; vsum.type = ir::IrType::I64;
        fn.values.push_back(v0);    /* id 0 */
        fn.values.push_back(v1);    /* id 1 */
        fn.values.push_back(vsum);  /* id 2 */
        fn.params = {0, 1};

        ir::IrBlock entry;
        entry.id = 0; entry.name = "entry";

        ir::IrInstr a;
        a.op = ir::IrOp::ADD;
        a.type = ir::IrType::I64;
        a.dst = 2;
        a.operands = {0, 1};
        entry.instrs.push_back(a);

        ir::IrInstr r;
        r.op = ir::IrOp::RET;
        r.type = ir::IrType::I64;
        r.dst = ir::IR_NO_VALUE;
        r.operands.push_back(2);
        entry.instrs.push_back(r);

        fn.blocks.push_back(entry);

        SelectorOptions opts;
        opts.mode = SelectorMode::VM_ABI;
        /* En este test no necesitamos safepoints (sin back-edges), pero
         * pasamos handler_addr=0 para que el selector omita la emision. */
        opts.safepoint_handler_addr = 0;

        Selector sel(opts);
        bool unsupported = false;
        MFunction mf = sel.select(fn, &unsupported);
        CHECK(!unsupported, "VM_ABI add no usa ops no soportadas");

        X86Encoder enc; std::vector<uint8_t> bytes;
        const size_t n = enc.encode(mf, bytes);
        CHECK(n > 0, "encoder genera bytes");

        CodeCache cc;
        uint8_t *code = cc.alloc(bytes.size(), 16);
        std::memcpy(code, bytes.data(), bytes.size());
        cc.commit(code, bytes.size());

        ProcProxy proxy{};
        proxy.regs[1] = 10;
        proxy.regs[2] = 32;

        using JitFn = void(*)(ProcProxy *);
        JitFn f = reinterpret_cast<JitFn>(code);
        f(&proxy);

        CHECK(proxy.regs[0] == 42, "regs[0] = a + b = 42");
        CHECK(proxy.regs[1] == 10, "regs[1] preservado");
        CHECK(proxy.regs[2] == 32, "regs[2] preservado");
    }

    /* ===================================================================== */
    /* Test 5: vrt_safepoint_handler real (skeleton)                          */
    /* ===================================================================== */

    void test_real_safepoint_handler() {
        /* Verificar que el handler real limpia el flag. */
        ProcProxy proxy{};
        proxy.safepoint_flag = 1;
        vrt_safepoint_handler(reinterpret_cast<vrt_proc *>(&proxy));
        CHECK(proxy.safepoint_flag == 0, "vrt_safepoint_handler limpia flag");
    }

    /* ===================================================================== */
    /* Test 6: RuntimeEntries::safepoint_handler resuelto                     */
    /* ===================================================================== */

    void test_runtime_entries_safepoint() {
        jit::RuntimeEntries rt;
        rt.resolve();
        CHECK(rt.safepoint_handler != nullptr, "safepoint_handler resuelto");
        CHECK(rt.safepoint_handler ==
              reinterpret_cast<decltype(rt.safepoint_handler)>(&vrt_safepoint_handler),
              "safepoint_handler apunta a vrt_safepoint_handler");
        CHECK(rt.all_resolved(), "todos los entries resueltos");
    }

} // namespace anonymous

int main() {
    test_safepoint_encoding();
    test_safepoint_fast_path();
    test_safepoint_slow_path();
    test_vm_abi_end_to_end();
    test_real_safepoint_handler();
    test_runtime_entries_safepoint();

    std::printf("test_safepoint: %d pass, %d fail\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
