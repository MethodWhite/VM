/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/jit/test_selector.cpp
 * @brief Tests del @c jit::Selector (IrFunction -> MFunction).
 *
 * Verifica end-to-end:
 *   1. Construir un @c IrFunction trivial a mano.
 *   2. Selector emite @c MFunction.
 *   3. Encoder emite bytes.
 *   4. CodeCache + bridge ejecuta el codigo.
 *   5. El resultado coincide con la semantica esperada del IR.
 *
 * Este es el primer test end-to-end del pipeline JIT completo y valida
 * que las cuatro piezas (D.1.a + D.1.b + D.1.c + D.0) funcionan
 * coordinadamente.
 */

#include "jit/code_cache.h"
#include "jit/machine_ir.h"
#include "jit/selector.h"
#include "jit/x86_encoder.h"
#include "ir/ssa_ir.h"

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

    /* Helpers para construir IrFunction a mano. */
    ir::IrValueId mk_value(ir::IrFunction &fn, ir::IrType t) {
        ir::IrValue v;
        v.type = t;
        fn.values.push_back(v);
        return static_cast<ir::IrValueId>(fn.values.size() - 1);
    }

    ir::IrInstr mk_instr(ir::IrOp op, ir::IrType t, ir::IrValueId dst) {
        ir::IrInstr i;
        i.op = op;
        i.type = t;
        i.dst = dst;
        return i;
    }

    /* Compila un IrFunction y devuelve un ptr ejecutable.  En caso de
     * error pone *err=true. */
    uint8_t *compile_to_native(jit::CodeCache &cc, const ir::IrFunction &fn,
                                bool *out_unsupported = nullptr) {
        jit::Selector sel;
        jit::MFunction mf = sel.select(fn, out_unsupported);

        jit::X86Encoder enc;
        std::vector<uint8_t> bytes;
        const size_t n = enc.encode(mf, bytes);
        if (n == 0 || bytes.empty()) return nullptr;

        uint8_t *code = cc.alloc(bytes.size(), 16);
        if (!code) return nullptr;
        std::memcpy(code, bytes.data(), bytes.size());
        cc.commit(code, bytes.size());
        return code;
    }

    /* ===================================================================== */
    /* Test: const + ret                                                       */
    /* ===================================================================== */

    /**
     * @brief IR equivalente a:  fn () -> i64 { return 42; }
     */
    void test_const_ret() {
        ir::IrFunction fn;
        fn.name = "ret42";
        fn.ret_type = ir::IrType::I64;

        ir::IrBlock entry;
        entry.id = 0; entry.name = "entry";

        const ir::IrValueId v_42 = mk_value(fn, ir::IrType::I64);
        ir::IrInstr c = mk_instr(ir::IrOp::CONST, ir::IrType::I64, v_42);
        c.imm = 42;
        entry.instrs.push_back(c);

        ir::IrInstr r = mk_instr(ir::IrOp::RET, ir::IrType::I64, ir::IR_NO_VALUE);
        r.operands.push_back(v_42);
        entry.instrs.push_back(r);

        fn.blocks.push_back(entry);

        jit::CodeCache cc;
        uint8_t *code = compile_to_native(cc, fn);
        CHECK(code != nullptr, "compile_to_native ok");
        if (!code) return;

        using Fn = int64_t(*)();
        CHECK(reinterpret_cast<Fn>(code)() == 42, "ret42() == 42");
    }

    /* ===================================================================== */
    /* Test: add(a, b) -> a + b                                                */
    /* ===================================================================== */

    void test_add() {
        ir::IrFunction fn;
        fn.name = "add";
        fn.ret_type = ir::IrType::I64;

        /* %0, %1 son params */
        const ir::IrValueId p0 = mk_value(fn, ir::IrType::I64);
        const ir::IrValueId p1 = mk_value(fn, ir::IrType::I64);
        fn.params = {p0, p1};

        ir::IrBlock entry;
        entry.id = 0; entry.name = "entry";

        const ir::IrValueId sum = mk_value(fn, ir::IrType::I64);
        ir::IrInstr addi = mk_instr(ir::IrOp::ADD, ir::IrType::I64, sum);
        addi.operands = {p0, p1};
        entry.instrs.push_back(addi);

        ir::IrInstr reti = mk_instr(ir::IrOp::RET, ir::IrType::I64, ir::IR_NO_VALUE);
        reti.operands.push_back(sum);
        entry.instrs.push_back(reti);

        fn.blocks.push_back(entry);

        jit::CodeCache cc;
        uint8_t *code = compile_to_native(cc, fn);
        CHECK(code != nullptr, "compile add");
        if (!code) return;

        using AddFn = int64_t(*)(int64_t, int64_t);
        AddFn f = reinterpret_cast<AddFn>(code);
        CHECK(f(10, 32) == 42, "add(10,32) == 42");
        CHECK(f(-5, 5)  == 0,  "add(-5,5) == 0");
        CHECK(f(1, 2)   == 3,  "add(1,2) == 3");
    }

    /* ===================================================================== */
    /* Test: mul + sub combinados                                              */
    /* ===================================================================== */

    /**
     * IR equivalente a:  fn (a, b, c) -> a*b - c
     */
    void test_mul_sub() {
        ir::IrFunction fn;
        fn.name = "muladdsub";
        fn.ret_type = ir::IrType::I64;

        const ir::IrValueId a = mk_value(fn, ir::IrType::I64);
        const ir::IrValueId b = mk_value(fn, ir::IrType::I64);
        const ir::IrValueId c = mk_value(fn, ir::IrType::I64);
        fn.params = {a, b, c};

        ir::IrBlock entry;
        entry.id = 0; entry.name = "entry";

        const ir::IrValueId t1 = mk_value(fn, ir::IrType::I64);
        ir::IrInstr m = mk_instr(ir::IrOp::MUL, ir::IrType::I64, t1);
        m.operands = {a, b};
        entry.instrs.push_back(m);

        const ir::IrValueId t2 = mk_value(fn, ir::IrType::I64);
        ir::IrInstr s = mk_instr(ir::IrOp::SUB, ir::IrType::I64, t2);
        s.operands = {t1, c};
        entry.instrs.push_back(s);

        ir::IrInstr r = mk_instr(ir::IrOp::RET, ir::IrType::I64, ir::IR_NO_VALUE);
        r.operands.push_back(t2);
        entry.instrs.push_back(r);

        fn.blocks.push_back(entry);

        jit::CodeCache cc;
        uint8_t *code = compile_to_native(cc, fn);
        CHECK(code != nullptr, "compile muladdsub");
        if (!code) return;

        using F = int64_t(*)(int64_t, int64_t, int64_t);
        F f = reinterpret_cast<F>(code);
        CHECK(f(6, 7, 0)  == 42, "f(6,7,0) == 42");
        CHECK(f(10, 5, 8) == 42, "f(10,5,8) == 42");
        CHECK(f(3, 4, 2)  == 10, "f(3,4,2) == 10");
    }

    /* ===================================================================== */
    /* Test: if (a < b) return a else return b  -- pseudo-min                 */
    /* ===================================================================== */

    /**
     * IR equivalente a:
     *   fn min(a, b):
     *     %2 = cmp.lt a, b
     *     br.cond %2, B_then, B_else
     *   B_then:
     *     ret a
     *   B_else:
     *     ret b
     */
    void test_min() {
        ir::IrFunction fn;
        fn.name = "min";
        fn.ret_type = ir::IrType::I64;

        const ir::IrValueId a = mk_value(fn, ir::IrType::I64);
        const ir::IrValueId b = mk_value(fn, ir::IrType::I64);
        fn.params = {a, b};

        const ir::IrValueId cond = mk_value(fn, ir::IrType::BOOL);

        ir::IrBlock entry; entry.id = 0; entry.name = "entry";
        ir::IrInstr cmp = mk_instr(ir::IrOp::CMP_LT, ir::IrType::BOOL, cond);
        cmp.operands = {a, b};
        entry.instrs.push_back(cmp);

        ir::IrInstr br = mk_instr(ir::IrOp::BR_COND, ir::IrType::VOID, ir::IR_NO_VALUE);
        br.operands.push_back(cond);
        br.target_block = 1;   /* B_then */
        br.false_block  = 2;   /* B_else */
        entry.instrs.push_back(br);
        entry.succs = {1, 2};

        ir::IrBlock b_then; b_then.id = 1; b_then.name = "then";
        ir::IrInstr ret_a = mk_instr(ir::IrOp::RET, ir::IrType::I64, ir::IR_NO_VALUE);
        ret_a.operands.push_back(a);
        b_then.instrs.push_back(ret_a);
        b_then.preds = {0};

        ir::IrBlock b_else; b_else.id = 2; b_else.name = "else";
        ir::IrInstr ret_b = mk_instr(ir::IrOp::RET, ir::IrType::I64, ir::IR_NO_VALUE);
        ret_b.operands.push_back(b);
        b_else.instrs.push_back(ret_b);
        b_else.preds = {0};

        fn.blocks = {entry, b_then, b_else};

        jit::CodeCache cc;
        uint8_t *code = compile_to_native(cc, fn);
        CHECK(code != nullptr, "compile min");
        if (!code) return;

        using F = int64_t(*)(int64_t, int64_t);
        F f = reinterpret_cast<F>(code);
        CHECK(f(10, 20) == 10, "min(10,20) == 10");
        CHECK(f(50, 30) == 30, "min(50,30) == 30");
        CHECK(f(-5, 5)  == -5, "min(-5,5) == -5");
        CHECK(f(0, 0)   == 0,  "min(0,0) == 0");
    }

    /* ===================================================================== */
    /* Test: bitwise ops (AND/OR/XOR)                                          */
    /* ===================================================================== */

    /** IR: fn (a, b) -> (a & b) | (a ^ b) */
    void test_bitwise() {
        ir::IrFunction fn;
        fn.name = "bitops";
        fn.ret_type = ir::IrType::I64;

        const ir::IrValueId a = mk_value(fn, ir::IrType::I64);
        const ir::IrValueId b = mk_value(fn, ir::IrType::I64);
        fn.params = {a, b};

        ir::IrBlock entry; entry.id = 0; entry.name = "entry";

        const ir::IrValueId t_and = mk_value(fn, ir::IrType::I64);
        ir::IrInstr i_and = mk_instr(ir::IrOp::AND, ir::IrType::I64, t_and);
        i_and.operands = {a, b}; entry.instrs.push_back(i_and);

        const ir::IrValueId t_xor = mk_value(fn, ir::IrType::I64);
        ir::IrInstr i_xor = mk_instr(ir::IrOp::XOR, ir::IrType::I64, t_xor);
        i_xor.operands = {a, b}; entry.instrs.push_back(i_xor);

        const ir::IrValueId t_or = mk_value(fn, ir::IrType::I64);
        ir::IrInstr i_or = mk_instr(ir::IrOp::OR, ir::IrType::I64, t_or);
        i_or.operands = {t_and, t_xor}; entry.instrs.push_back(i_or);

        ir::IrInstr r = mk_instr(ir::IrOp::RET, ir::IrType::I64, ir::IR_NO_VALUE);
        r.operands.push_back(t_or);
        entry.instrs.push_back(r);
        fn.blocks.push_back(entry);

        jit::CodeCache cc;
        uint8_t *code = compile_to_native(cc, fn);
        CHECK(code != nullptr, "compile bitops");
        if (!code) return;

        using F = uint64_t(*)(uint64_t, uint64_t);
        F f = reinterpret_cast<F>(code);
        /* (a & b) | (a ^ b) == a | b */
        CHECK(f(0b1100, 0b1010) == (0b1100u | 0b1010u), "bitops(1100, 1010)");
        CHECK(f(0xFF, 0xF0) == 0xFF, "bitops(0xFF, 0xF0) == 0xFF");
    }

} // namespace anonymous

int main() {
    test_const_ret();
    test_add();
    test_mul_sub();
    test_min();
    test_bitwise();

    std::printf("test_selector: %d pass, %d fail\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
