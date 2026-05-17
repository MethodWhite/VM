/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/jit/test_x86_encoder.cpp
 * @brief Tests del encoder x86-64 hand-rolled.
 *
 * = Estrategia =
 *
 * Cada test construye un @c MFunction con una secuencia conocida de
 * @c MInstr, los emite a @c std::vector<uint8_t> con @c X86Encoder, y
 * compara los bytes resultantes contra una secuencia hardcodeada del
 * encoding x86-64 esperado.
 *
 * Adicionalmente, varios tests cargan los bytes generados en el
 * @c CodeCache y los EJECUTAN, verificando que el codigo se comporta
 * correctamente.  Esto valida no solo el encoding sintactico sino la
 * semantica completa end-to-end.
 *
 * = Referencias usadas =
 *
 * - Intel SDM Vol 2 (Instruction Set Reference)
 * - https://www.felixcloutier.com/x86/  (encoding reference online)
 */

#include "jit/machine_ir.h"
#include "jit/x86_encoder.h"
#include "jit/code_cache.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

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

    #define CHECK_BYTES(out, expected) do {                                      \
        const auto &__o = (out);                                                 \
        const std::vector<uint8_t> __e = (expected);                             \
        if (__o.size() != __e.size()) {                                          \
            std::fprintf(stderr, "FAIL linea %d: size %zu != %zu\n",             \
                         __LINE__, __o.size(), __e.size());                      \
            ++fail_count;                                                        \
        } else if (std::memcmp(__o.data(), __e.data(), __o.size()) != 0) {       \
            std::fprintf(stderr, "FAIL linea %d: bytes distintos\n  got: ",      \
                         __LINE__);                                              \
            for (auto b : __o) std::fprintf(stderr, "%02X ", b);                 \
            std::fprintf(stderr, "\n  exp: ");                                   \
            for (auto b : __e) std::fprintf(stderr, "%02X ", b);                 \
            std::fprintf(stderr, "\n");                                          \
            ++fail_count;                                                        \
        } else {                                                                 \
            ++pass_count;                                                        \
        }                                                                        \
    } while (0)

    using namespace jit;

    /* Helpers para construir un MFunction de 1 bloque con N instrs. */
    MFunction make_single_block_fn(std::initializer_list<MInstr> instrs) {
        MFunction f;
        f.name = "test";
        MBlockId b = f.new_block(f.new_label());
        for (const auto &i : instrs) {
            f.blocks[b].instrs.push_back(i);
        }
        return f;
    }

    /* ===================================================================== */
    /* Tests de encoding sintactico                                           */
    /* ===================================================================== */

    void test_encode_ret() {
        auto fn = make_single_block_fn({MInstr::make_ret()});
        X86Encoder enc;
        std::vector<uint8_t> out;
        const size_t n = enc.encode(fn, out);
        CHECK(n == 1, "ret emite 1 byte");
        CHECK_BYTES(out, std::vector<uint8_t>{0xC3});
    }

    void test_encode_int3() {
        MFunction fn = make_single_block_fn({
            MInstr{MOp::INT3, 0, 0, 0, {}, {}, {}}
        });
        X86Encoder enc;
        std::vector<uint8_t> out;
        enc.encode(fn, out);
        CHECK_BYTES(out, std::vector<uint8_t>{0xCC});
    }

    void test_encode_mov_reg_reg() {
        /* mov rax, rcx -> 48 89 c8 */
        MFunction fn = make_single_block_fn({
            MInstr::make_unary(MOp::MOV,
                MOperand::make_reg(MReg::RAX),
                MOperand::make_reg(MReg::RCX))
        });
        X86Encoder enc; std::vector<uint8_t> out;
        enc.encode(fn, out);
        CHECK_BYTES(out, (std::vector<uint8_t>{0x48, 0x89, 0xC8}));
    }

    void test_encode_mov_r8_to_rax() {
        /* mov rax, r8 -> 4C 89 C0  (REX.R=1 porque src=R8) */
        MFunction fn = make_single_block_fn({
            MInstr::make_unary(MOp::MOV,
                MOperand::make_reg(MReg::RAX),
                MOperand::make_reg(MReg::R8))
        });
        X86Encoder enc; std::vector<uint8_t> out;
        enc.encode(fn, out);
        CHECK_BYTES(out, (std::vector<uint8_t>{0x4C, 0x89, 0xC0}));
    }

    void test_encode_mov_imm32() {
        /* mov rax, 0x2A -> 48 C7 C0 2A 00 00 00 (REX.W + C7 /0 + imm32) */
        MFunction fn = make_single_block_fn({
            MInstr::make_unary(MOp::MOV,
                MOperand::make_reg(MReg::RAX),
                MOperand::make_imm32(0x2A))
        });
        X86Encoder enc; std::vector<uint8_t> out;
        enc.encode(fn, out);
        CHECK_BYTES(out, (std::vector<uint8_t>{0x48, 0xC7, 0xC0, 0x2A, 0x00, 0x00, 0x00}));
    }

    void test_encode_mov_imm64() {
        /* mov rax, 0x123456789ABCDEF0 -> 48 B8 F0 DE BC 9A 78 56 34 12 */
        MFunction fn;
        fn.name = "test";
        MBlockId b = fn.new_block(fn.new_label());
        const uint32_t idx = fn.intern_imm64(0x123456789ABCDEF0ULL);
        fn.blocks[b].instrs.push_back(
            MInstr::make_unary(MOp::MOV,
                MOperand::make_reg(MReg::RAX),
                MOperand::make_imm64_idx(idx)));

        X86Encoder enc; std::vector<uint8_t> out;
        enc.encode(fn, out);
        CHECK_BYTES(out, (std::vector<uint8_t>{
            0x48, 0xB8, 0xF0, 0xDE, 0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12
        }));
    }

    void test_encode_add_reg_reg() {
        /* add rax, rcx -> 48 01 c8 */
        MFunction fn = make_single_block_fn({
            MInstr::make_unary(MOp::ADD,
                MOperand::make_reg(MReg::RAX),
                MOperand::make_reg(MReg::RCX))
        });
        X86Encoder enc; std::vector<uint8_t> out;
        enc.encode(fn, out);
        CHECK_BYTES(out, (std::vector<uint8_t>{0x48, 0x01, 0xC8}));
    }

    void test_encode_add_imm8() {
        /* add rax, 5 -> 48 83 c0 05 (variante imm8 optimizada) */
        MFunction fn = make_single_block_fn({
            MInstr::make_unary(MOp::ADD,
                MOperand::make_reg(MReg::RAX),
                MOperand::make_imm32(5))
        });
        X86Encoder enc; std::vector<uint8_t> out;
        enc.encode(fn, out);
        CHECK_BYTES(out, (std::vector<uint8_t>{0x48, 0x83, 0xC0, 0x05}));
    }

    void test_encode_add_imm32() {
        /* add rax, 1000 -> 48 81 c0 e8 03 00 00 (1000=0x3E8) */
        MFunction fn = make_single_block_fn({
            MInstr::make_unary(MOp::ADD,
                MOperand::make_reg(MReg::RAX),
                MOperand::make_imm32(1000))
        });
        X86Encoder enc; std::vector<uint8_t> out;
        enc.encode(fn, out);
        CHECK_BYTES(out, (std::vector<uint8_t>{0x48, 0x81, 0xC0, 0xE8, 0x03, 0x00, 0x00}));
    }

    void test_encode_sub_reg_reg() {
        /* sub rax, rcx -> 48 29 c8 */
        MFunction fn = make_single_block_fn({
            MInstr::make_unary(MOp::SUB,
                MOperand::make_reg(MReg::RAX),
                MOperand::make_reg(MReg::RCX))
        });
        X86Encoder enc; std::vector<uint8_t> out;
        enc.encode(fn, out);
        CHECK_BYTES(out, (std::vector<uint8_t>{0x48, 0x29, 0xC8}));
    }

    void test_encode_imul() {
        /* imul rax, rcx -> 48 0F AF C1 */
        MFunction fn = make_single_block_fn({
            MInstr::make_unary(MOp::IMUL,
                MOperand::make_reg(MReg::RAX),
                MOperand::make_reg(MReg::RCX))
        });
        X86Encoder enc; std::vector<uint8_t> out;
        enc.encode(fn, out);
        CHECK_BYTES(out, (std::vector<uint8_t>{0x48, 0x0F, 0xAF, 0xC1}));
    }

    void test_encode_load_mem() {
        /* mov rax, [rcx+16] -> 48 8B 41 10 (mod=01, disp8=16) */
        MFunction fn = make_single_block_fn({
            MInstr::make_unary(MOp::MOV,
                MOperand::make_reg(MReg::RAX),
                MOperand::make_mem(MReg::RCX, 16))
        });
        X86Encoder enc; std::vector<uint8_t> out;
        enc.encode(fn, out);
        CHECK_BYTES(out, (std::vector<uint8_t>{0x48, 0x8B, 0x41, 0x10}));
    }

    void test_encode_store_mem() {
        /* mov [rcx+8], rax -> 48 89 41 08 */
        MFunction fn = make_single_block_fn({
            MInstr::make_unary(MOp::MOV,
                MOperand::make_mem(MReg::RCX, 8),
                MOperand::make_reg(MReg::RAX))
        });
        X86Encoder enc; std::vector<uint8_t> out;
        enc.encode(fn, out);
        CHECK_BYTES(out, (std::vector<uint8_t>{0x48, 0x89, 0x41, 0x08}));
    }

    void test_encode_push_pop() {
        /* push rax -> 50; pop rax -> 58 */
        MFunction fn = make_single_block_fn({
            MInstr::make_unary(MOp::PUSH, {}, MOperand::make_reg(MReg::RAX)),
            MInstr::make_unary(MOp::POP, MOperand::make_reg(MReg::RAX), {})
        });
        X86Encoder enc; std::vector<uint8_t> out;
        enc.encode(fn, out);
        CHECK_BYTES(out, (std::vector<uint8_t>{0x50, 0x58}));
    }

    void test_encode_jmp_label() {
        /* jmp .end ; nop ; .end: ret
         * jmp -> E9 02 00 00 00 ; nop -> 90 ; .end (offset 6) ; ret -> C3 */
        MFunction fn;
        fn.name = "test";
        MLabelId L_end = fn.new_label();
        MBlockId b1 = fn.new_block(fn.new_label());
        fn.blocks[b1].instrs.push_back(MInstr::make_jmp(L_end));
        fn.blocks[b1].instrs.push_back(MInstr::make_nop());

        MBlockId b2 = fn.new_block(L_end);
        fn.blocks[b2].instrs.push_back(MInstr::make_ret());

        X86Encoder enc; std::vector<uint8_t> out;
        enc.encode(fn, out);
        /* jmp E9 rel32 (5 bytes) + nop 90 (1 byte) + ret C3 (1 byte) = 7 bytes
         * rel32 = offset_to_target - instr_end = 6 - 5 = 1 */
        CHECK_BYTES(out, (std::vector<uint8_t>{
            0xE9, 0x01, 0x00, 0x00, 0x00,
            0x90,
            0xC3
        }));
    }

    void test_encode_jcc() {
        /* je .end ; nop ; .end: ret */
        MFunction fn;
        fn.name = "test";
        MLabelId L_end = fn.new_label();
        MBlockId b1 = fn.new_block(fn.new_label());
        fn.blocks[b1].instrs.push_back(MInstr::make_jcc(MCond::E, L_end));
        fn.blocks[b1].instrs.push_back(MInstr::make_nop());

        MBlockId b2 = fn.new_block(L_end);
        fn.blocks[b2].instrs.push_back(MInstr::make_ret());

        X86Encoder enc; std::vector<uint8_t> out;
        enc.encode(fn, out);
        /* je: 0F 84 rel32 (6 bytes) + nop (1) + ret (1) = 8 bytes
         * rel32 = 7 - 6 = 1 */
        CHECK_BYTES(out, (std::vector<uint8_t>{
            0x0F, 0x84, 0x01, 0x00, 0x00, 0x00,
            0x90,
            0xC3
        }));
    }

    void test_encode_cmp() {
        /* cmp rax, rcx -> 48 39 c8 */
        MFunction fn = make_single_block_fn({
            MInstr::make_unary(MOp::CMP,
                MOperand::make_reg(MReg::RAX),
                MOperand::make_reg(MReg::RCX))
        });
        X86Encoder enc; std::vector<uint8_t> out;
        enc.encode(fn, out);
        CHECK_BYTES(out, (std::vector<uint8_t>{0x48, 0x39, 0xC8}));
    }

    void test_encode_setcc() {
        /* sete al -> 0F 94 C0 */
        MFunction fn;
        fn.name = "test";
        MBlockId b = fn.new_block(fn.new_label());
        MInstr i; i.op = MOp::SETCC; i.variant = static_cast<uint8_t>(MCond::E);
        i.dst = MOperand::make_reg(MReg::RAX, 1);
        fn.blocks[b].instrs.push_back(i);

        X86Encoder enc; std::vector<uint8_t> out;
        enc.encode(fn, out);
        CHECK_BYTES(out, (std::vector<uint8_t>{0x0F, 0x94, 0xC0}));
    }

    /* ===================================================================== */
    /* Tests de ejecucion: emit -> code cache -> ejecutar                     */
    /* ===================================================================== */

    /** Funcion JIT minima que retorna 42. */
    void test_exec_return_const() {
        MFunction fn;
        fn.name = "ret42";
        MBlockId b = fn.new_block(fn.new_label());
        fn.blocks[b].instrs.push_back(
            MInstr::make_unary(MOp::MOV,
                MOperand::make_reg(MReg::RAX),
                MOperand::make_imm32(42)));
        fn.blocks[b].instrs.push_back(MInstr::make_ret());

        X86Encoder enc; std::vector<uint8_t> bytes;
        enc.encode(fn, bytes);

        CodeCache cc;
        uint8_t *code = cc.alloc(bytes.size(), 16);
        std::memcpy(code, bytes.data(), bytes.size());
        cc.commit(code, bytes.size());

        using Fn = uint64_t(*)();
        CHECK(reinterpret_cast<Fn>(code)() == 42, "fn() ejecutada devuelve 42");
    }

    /** Suma de dos enteros (calling convention nativa para portabilidad). */
    void test_exec_add() {
        /* funcion add(a,b): mov rax, [arg1]; add rax, [arg2]; ret
         * Convencion SysV: arg1=rdi, arg2=rsi
         * Convencion Win64: arg1=rcx, arg2=rdx
         * Emitimos: mov rax, arg1 ; add rax, arg2 ; ret */
        MFunction fn;
        fn.name = "add";
        MBlockId b = fn.new_block(fn.new_label());
#if defined(_WIN32)
        const MReg ARG1 = MReg::RCX;
        const MReg ARG2 = MReg::RDX;
#else
        const MReg ARG1 = MReg::RDI;
        const MReg ARG2 = MReg::RSI;
#endif
        fn.blocks[b].instrs.push_back(
            MInstr::make_unary(MOp::MOV,
                MOperand::make_reg(MReg::RAX),
                MOperand::make_reg(ARG1)));
        fn.blocks[b].instrs.push_back(
            MInstr::make_unary(MOp::ADD,
                MOperand::make_reg(MReg::RAX),
                MOperand::make_reg(ARG2)));
        fn.blocks[b].instrs.push_back(MInstr::make_ret());

        X86Encoder enc; std::vector<uint8_t> bytes;
        enc.encode(fn, bytes);

        CodeCache cc;
        uint8_t *code = cc.alloc(bytes.size(), 16);
        std::memcpy(code, bytes.data(), bytes.size());
        cc.commit(code, bytes.size());

        using AddFn = int64_t(*)(int64_t, int64_t);
        AddFn f = reinterpret_cast<AddFn>(code);
        CHECK(f(10, 32) == 42, "add(10,32) == 42");
        CHECK(f(-5, 5) == 0, "add(-5,5) == 0");
        CHECK(f(1000, 2000) == 3000, "add(1000,2000) == 3000");
    }

    /** if-else: max(a, b). */
    void test_exec_max() {
        /* fn max(a,b):
         *     mov rax, arg1
         *     cmp arg1, arg2
         *     jge .end
         *     mov rax, arg2
         *     .end:
         *     ret
         */
        MFunction fn;
        fn.name = "max";
        MLabelId L_end = fn.new_label();
        MBlockId b1 = fn.new_block(fn.new_label());
#if defined(_WIN32)
        const MReg ARG1 = MReg::RCX;
        const MReg ARG2 = MReg::RDX;
#else
        const MReg ARG1 = MReg::RDI;
        const MReg ARG2 = MReg::RSI;
#endif
        fn.blocks[b1].instrs.push_back(
            MInstr::make_unary(MOp::MOV,
                MOperand::make_reg(MReg::RAX),
                MOperand::make_reg(ARG1)));
        fn.blocks[b1].instrs.push_back(
            MInstr::make_unary(MOp::CMP,
                MOperand::make_reg(ARG1),
                MOperand::make_reg(ARG2)));
        fn.blocks[b1].instrs.push_back(MInstr::make_jcc(MCond::GE, L_end));
        fn.blocks[b1].instrs.push_back(
            MInstr::make_unary(MOp::MOV,
                MOperand::make_reg(MReg::RAX),
                MOperand::make_reg(ARG2)));

        MBlockId b2 = fn.new_block(L_end);
        fn.blocks[b2].instrs.push_back(MInstr::make_ret());

        X86Encoder enc; std::vector<uint8_t> bytes;
        enc.encode(fn, bytes);

        CodeCache cc;
        uint8_t *code = cc.alloc(bytes.size(), 16);
        std::memcpy(code, bytes.data(), bytes.size());
        cc.commit(code, bytes.size());

        using MaxFn = int64_t(*)(int64_t, int64_t);
        MaxFn f = reinterpret_cast<MaxFn>(code);
        CHECK(f(10, 20) == 20, "max(10,20) == 20");
        CHECK(f(50, 30) == 50, "max(50,30) == 50");
        CHECK(f(-5, -100) == -5, "max(-5,-100) == -5");
        CHECK(f(0, 0) == 0, "max(0,0) == 0");
    }

    /** Loop: suma de 1..N. */
    void test_exec_sum_loop() {
        /* fn sum(N):
         *     xor rax, rax       ; total = 0
         *     mov rcx, arg1      ; i = N
         *     .top:
         *     test rcx, rcx
         *     je .end
         *     add rax, rcx
         *     sub rcx, 1
         *     jmp .top
         *     .end:
         *     ret
         */
        MFunction fn;
        fn.name = "sum";
        MLabelId L_top = fn.new_label();
        MLabelId L_end = fn.new_label();
#if defined(_WIN32)
        const MReg ARG1 = MReg::RCX;  /* Win64 arg1 -- usaremos RDX como counter para no chocar */
#else
        const MReg ARG1 = MReg::RDI;
#endif
        const MReg COUNTER = MReg::RDX;

        MBlockId b1 = fn.new_block(fn.new_label());
        /* xor rax, rax -- usar SUB rax, rax como proxy (encoder no tiene XOR optimizado) */
        /* En su lugar: mov rax, 0 */
        fn.blocks[b1].instrs.push_back(
            MInstr::make_unary(MOp::MOV,
                MOperand::make_reg(MReg::RAX),
                MOperand::make_imm32(0)));
        fn.blocks[b1].instrs.push_back(
            MInstr::make_unary(MOp::MOV,
                MOperand::make_reg(COUNTER),
                MOperand::make_reg(ARG1)));

        MBlockId b2 = fn.new_block(L_top);
        fn.blocks[b2].instrs.push_back(
            MInstr::make_unary(MOp::TEST,
                MOperand::make_reg(COUNTER),
                MOperand::make_reg(COUNTER)));
        fn.blocks[b2].instrs.push_back(MInstr::make_jcc(MCond::E, L_end));
        fn.blocks[b2].instrs.push_back(
            MInstr::make_unary(MOp::ADD,
                MOperand::make_reg(MReg::RAX),
                MOperand::make_reg(COUNTER)));
        fn.blocks[b2].instrs.push_back(
            MInstr::make_unary(MOp::SUB,
                MOperand::make_reg(COUNTER),
                MOperand::make_imm32(1)));
        fn.blocks[b2].instrs.push_back(MInstr::make_jmp(L_top));

        MBlockId b3 = fn.new_block(L_end);
        fn.blocks[b3].instrs.push_back(MInstr::make_ret());

        X86Encoder enc; std::vector<uint8_t> bytes;
        enc.encode(fn, bytes);

        CodeCache cc;
        uint8_t *code = cc.alloc(bytes.size(), 16);
        std::memcpy(code, bytes.data(), bytes.size());
        cc.commit(code, bytes.size());

        using SumFn = int64_t(*)(int64_t);
        SumFn f = reinterpret_cast<SumFn>(code);
        CHECK(f(0) == 0, "sum(0) == 0");
        CHECK(f(1) == 1, "sum(1) == 1");
        CHECK(f(10) == 55, "sum(10) == 55 (1+2+...+10)");
        CHECK(f(100) == 5050, "sum(100) == 5050");
        CHECK(f(1000) == 500500, "sum(1000) == 500500");
    }

} // namespace anonymous

int main() {
    /* Encoding tests */
    test_encode_ret();
    test_encode_int3();
    test_encode_mov_reg_reg();
    test_encode_mov_r8_to_rax();
    test_encode_mov_imm32();
    test_encode_mov_imm64();
    test_encode_add_reg_reg();
    test_encode_add_imm8();
    test_encode_add_imm32();
    test_encode_sub_reg_reg();
    test_encode_imul();
    test_encode_load_mem();
    test_encode_store_mem();
    test_encode_push_pop();
    test_encode_jmp_label();
    test_encode_jcc();
    test_encode_cmp();
    test_encode_setcc();

    /* Execution tests (end-to-end) */
    test_exec_return_const();
    test_exec_add();
    test_exec_max();
    test_exec_sum_loop();

    std::printf("test_x86_encoder: %d pass, %d fail\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
