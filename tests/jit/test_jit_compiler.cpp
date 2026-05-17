/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/jit/test_jit_compiler.cpp
 * @brief Tests del @c JitCompiler + PHI elimination + benchmark JIT vs C.
 *
 * Cubre Phase D.3-A:
 *   1. JitCompiler basico: IR trivial -> codigo nativo ejecutable.
 *   2. PHI elimination en BR (back-edge de loop simple).
 *   3. PHI elimination en BR_COND (if-merge con valores distintos).
 *   4. Loop completo: sum_to_n(N) = N*(N+1)/2.
 *   5. Benchmark: JIT vs C nativo gold standard.
 */

#include "jit/code_cache.h"
#include "jit/jit_compiler.h"
#include "jit/jit_registry.h"
#include "jit/runtime_entries.h"
#include "ir/ssa_ir.h"

#include <chrono>
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

    /* Helpers para construir IR a mano. */
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

    /* ===================================================================== */
    /* Test 1: JitCompiler basico                                              */
    /* ===================================================================== */

    void test_compile_simple() {
        ir::IrFunction fn;
        fn.name = "ret42_jit";
        fn.ret_type = ir::IrType::I64;

        const ir::IrValueId v = mk_value(fn, ir::IrType::I64);
        ir::IrBlock entry; entry.id = 0; entry.name = "entry";
        ir::IrInstr c = mk_instr(ir::IrOp::CONST, ir::IrType::I64, v);
        c.imm = 42; entry.instrs.push_back(c);
        ir::IrInstr r = mk_instr(ir::IrOp::RET, ir::IrType::I64, ir::IR_NO_VALUE);
        r.operands.push_back(v); entry.instrs.push_back(r);
        fn.blocks.push_back(entry);

        jit::JitRegistry::instance().clear();
        jit::CodeCache cache;
        jit::RuntimeEntries rt;
        rt.resolve();

        jit::JitCompiler comp(cache, rt);
        jit::CompileResult res = comp.compile(fn, jit::SelectorMode::NATIVE_ABI);
        CHECK(res.fn != nullptr, "compilacion ok");
        CHECK(!res.unsupported, "no ops unsupported");
        CHECK(res.code_size > 0, "code_size > 0");

        using F = int64_t(*)();
        CHECK(reinterpret_cast<F>(res.fn)() == 42, "ret42_jit() == 42");

        /* Registrada correctamente. */
        CHECK(jit::JitRegistry::instance().size() == 1, "1 fn registrada");
        CHECK(jit::JitRegistry::instance().lookup(res.code_start) != nullptr,
              "lookup encuentra la fn");

        jit::JitRegistry::instance().clear();
    }

    /* ===================================================================== */
    /* Test 2: PHI elimination en if/else merge                                */
    /* ===================================================================== */

    /**
     * IR:
     *   fn select(a, b, cond) -> i64:
     *     entry: br.cond cond, then, else
     *     then:  br merge   (phi_arg: a)
     *     else:  br merge   (phi_arg: b)
     *     merge: %r = phi [a, then], [b, else]
     *            ret %r
     */
    void test_phi_if_else() {
        ir::IrFunction fn;
        fn.name = "select";
        fn.ret_type = ir::IrType::I64;

        /* params: a, b, cond */
        const auto a = mk_value(fn, ir::IrType::I64);
        const auto b = mk_value(fn, ir::IrType::I64);
        const auto c = mk_value(fn, ir::IrType::I64);
        fn.params = {a, b, c};

        const auto r = mk_value(fn, ir::IrType::I64);

        /* entry */
        ir::IrBlock entry; entry.id = 0; entry.name = "entry";
        ir::IrInstr br = mk_instr(ir::IrOp::BR_COND, ir::IrType::VOID, ir::IR_NO_VALUE);
        br.operands.push_back(c);
        br.target_block = 1;  /* then */
        br.false_block  = 2;  /* else */
        entry.instrs.push_back(br);
        entry.succs = {1, 2};

        /* then */
        ir::IrBlock then_bb; then_bb.id = 1; then_bb.name = "then";
        ir::IrInstr br_t = mk_instr(ir::IrOp::BR, ir::IrType::VOID, ir::IR_NO_VALUE);
        br_t.target_block = 3;
        then_bb.instrs.push_back(br_t);
        then_bb.preds = {0}; then_bb.succs = {3};

        /* else */
        ir::IrBlock else_bb; else_bb.id = 2; else_bb.name = "else";
        ir::IrInstr br_e = mk_instr(ir::IrOp::BR, ir::IrType::VOID, ir::IR_NO_VALUE);
        br_e.target_block = 3;
        else_bb.instrs.push_back(br_e);
        else_bb.preds = {0}; else_bb.succs = {3};

        /* merge: %r = phi [a, 1], [b, 2]; ret %r */
        ir::IrBlock merge; merge.id = 3; merge.name = "merge";
        ir::IrInstr phi = mk_instr(ir::IrOp::PHI, ir::IrType::I64, r);
        phi.phi_args = {{a, 1}, {b, 2}};
        merge.instrs.push_back(phi);
        ir::IrInstr ret = mk_instr(ir::IrOp::RET, ir::IrType::I64, ir::IR_NO_VALUE);
        ret.operands.push_back(r);
        merge.instrs.push_back(ret);
        merge.preds = {1, 2};

        fn.blocks = {entry, then_bb, else_bb, merge};

        jit::JitRegistry::instance().clear();
        jit::CodeCache cache;
        jit::RuntimeEntries rt; rt.resolve();
        jit::JitCompiler comp(cache, rt);
        jit::CompileResult res = comp.compile(fn, jit::SelectorMode::NATIVE_ABI);
        CHECK(res.fn != nullptr, "compile select ok");

        using F = int64_t(*)(int64_t, int64_t, int64_t);
        F f = reinterpret_cast<F>(res.fn);
        CHECK(f(10, 20, 1) == 10, "select(10,20,1) == 10 (then)");
        CHECK(f(10, 20, 0) == 20, "select(10,20,0) == 20 (else)");
        CHECK(f(-5, 99, 42) == -5, "select(-5,99,42) == -5 (cond != 0)");

        jit::JitRegistry::instance().clear();
    }

    /* ===================================================================== */
    /* Test 3: Loop con PHI - sum_to_n(N) = 1+2+...+N                          */
    /* ===================================================================== */

    /**
     * IR:
     *   fn sum_to_n(N) -> i64:
     *     entry:
     *       %zero = const 0
     *       %one  = const 1
     *       br loop_header
     *     loop_header:
     *       %i   = phi [%one, entry], [%i_next, loop_body]
     *       %sum = phi [%zero, entry], [%sum_next, loop_body]
     *       %cont = cmp_le %i, N
     *       br.cond %cont, loop_body, exit
     *     loop_body:
     *       %sum_next = add %sum, %i
     *       %i_next   = add %i, %one
     *       br loop_header
     *     exit:
     *       ret %sum
     */
    void test_loop_phi_sum_to_n() {
        ir::IrFunction fn;
        fn.name = "sum_to_n";
        fn.ret_type = ir::IrType::I64;

        const auto N    = mk_value(fn, ir::IrType::I64);
        fn.params = {N};

        const auto zero = mk_value(fn, ir::IrType::I64);
        const auto one  = mk_value(fn, ir::IrType::I64);
        const auto i    = mk_value(fn, ir::IrType::I64);
        const auto sum  = mk_value(fn, ir::IrType::I64);
        const auto cont = mk_value(fn, ir::IrType::BOOL);
        const auto sum_next = mk_value(fn, ir::IrType::I64);
        const auto i_next   = mk_value(fn, ir::IrType::I64);

        /* entry */
        ir::IrBlock e; e.id = 0; e.name = "entry";
        ir::IrInstr c0 = mk_instr(ir::IrOp::CONST, ir::IrType::I64, zero); c0.imm = 0;
        e.instrs.push_back(c0);
        ir::IrInstr c1 = mk_instr(ir::IrOp::CONST, ir::IrType::I64, one);  c1.imm = 1;
        e.instrs.push_back(c1);
        ir::IrInstr br0 = mk_instr(ir::IrOp::BR, ir::IrType::VOID, ir::IR_NO_VALUE);
        br0.target_block = 1;
        e.instrs.push_back(br0);
        e.succs = {1};

        /* loop_header */
        ir::IrBlock h; h.id = 1; h.name = "loop_header";
        ir::IrInstr phi_i = mk_instr(ir::IrOp::PHI, ir::IrType::I64, i);
        phi_i.phi_args = {{one, 0}, {i_next, 2}};
        h.instrs.push_back(phi_i);
        ir::IrInstr phi_sum = mk_instr(ir::IrOp::PHI, ir::IrType::I64, sum);
        phi_sum.phi_args = {{zero, 0}, {sum_next, 2}};
        h.instrs.push_back(phi_sum);
        ir::IrInstr cmp = mk_instr(ir::IrOp::CMP_LE, ir::IrType::BOOL, cont);
        cmp.operands = {i, N};
        h.instrs.push_back(cmp);
        ir::IrInstr brc = mk_instr(ir::IrOp::BR_COND, ir::IrType::VOID, ir::IR_NO_VALUE);
        brc.operands.push_back(cont);
        brc.target_block = 2;  /* loop_body */
        brc.false_block  = 3;  /* exit */
        h.instrs.push_back(brc);
        h.preds = {0, 2}; h.succs = {2, 3};

        /* loop_body */
        ir::IrBlock body; body.id = 2; body.name = "loop_body";
        ir::IrInstr add_sum = mk_instr(ir::IrOp::ADD, ir::IrType::I64, sum_next);
        add_sum.operands = {sum, i};
        body.instrs.push_back(add_sum);
        ir::IrInstr add_i = mk_instr(ir::IrOp::ADD, ir::IrType::I64, i_next);
        add_i.operands = {i, one};
        body.instrs.push_back(add_i);
        ir::IrInstr br_back = mk_instr(ir::IrOp::BR, ir::IrType::VOID, ir::IR_NO_VALUE);
        br_back.target_block = 1;
        body.instrs.push_back(br_back);
        body.preds = {1}; body.succs = {1};

        /* exit */
        ir::IrBlock x; x.id = 3; x.name = "exit";
        ir::IrInstr ret = mk_instr(ir::IrOp::RET, ir::IrType::I64, ir::IR_NO_VALUE);
        ret.operands.push_back(sum);
        x.instrs.push_back(ret);
        x.preds = {1};

        fn.blocks = {e, h, body, x};

        jit::JitRegistry::instance().clear();
        jit::CodeCache cache;
        jit::RuntimeEntries rt; rt.resolve();
        jit::JitCompiler comp(cache, rt);
        jit::CompileResult res = comp.compile(fn, jit::SelectorMode::NATIVE_ABI);
        CHECK(res.fn != nullptr, "compile sum_to_n ok");

        using F = int64_t(*)(int64_t);
        F f = reinterpret_cast<F>(res.fn);
        CHECK(f(0)   == 0,      "sum(0) == 0");
        CHECK(f(1)   == 1,      "sum(1) == 1");
        CHECK(f(10)  == 55,     "sum(10) == 55");
        CHECK(f(100) == 5050,   "sum(100) == 5050");
        CHECK(f(1000) == 500500, "sum(1000) == 500500");

        /* ----- Benchmark JIT vs C nativo + comparacion con interp -----
         *
         * Para que C no folde el loop (sum_to_n cerrado N*(N+1)/2),
         * insertamos asm volatile barrier que fuerza al compilador a
         * emitir cada iteracion. */
        constexpr int64_t N_BENCH = 100'000'000LL;  /* 100M iter */

        auto t0 = std::chrono::high_resolution_clock::now();
        volatile int64_t result_jit = f(N_BENCH);
        auto t1 = std::chrono::high_resolution_clock::now();

        auto t2 = std::chrono::high_resolution_clock::now();
        int64_t result_c = 0;
        {
            int64_t s = 0;
            for (int64_t k = 1; k <= N_BENCH; ++k) {
                s += k;
                /* Barrier: el compilador no puede asumir que s no cambio
                 * desde fuera, asi que tiene que recargar/computar en
                 * cada iteracion.  Asegura un loop comparable al JIT. */
                __asm__ volatile("" : "+r"(s) : : "memory");
            }
            result_c = s;
        }
        auto t3 = std::chrono::high_resolution_clock::now();

        const double jit_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const double c_ms   = std::chrono::duration<double, std::milli>(t3 - t2).count();
        const double ratio  = (c_ms > 0) ? (jit_ms / c_ms) : 0.0;
        const double jit_mips = (jit_ms > 0)
            ? (static_cast<double>(N_BENCH) / 1e6 / (jit_ms / 1000.0))
            : 0.0;

        /* Estimacion del interprete:
         *   ~105 MIPS por thread  (snapshot pre-JIT).
         *   Speedup estimado = jit_mips / 105. */
        const double interp_mips = 105.0;
        const double speedup_vs_interp = jit_mips / interp_mips;

        std::printf("    [bench sum_to_n(%lld)]:\n", static_cast<long long>(N_BENCH));
        std::printf("      JIT C1 template: %.1f ms  -> %.0f MIPS (iters/sec)\n",
                    jit_ms, jit_mips);
        std::printf("      C nativo -O3:    %.1f ms  -> ratio JIT/C = %.2fx\n",
                    c_ms, ratio);
        std::printf("      vs interp (~105 MIPS estimado): speedup %.1fx\n",
                    speedup_vs_interp);

        CHECK(result_jit == result_c, "JIT y C dan mismo resultado");
        CHECK(speedup_vs_interp > 2.0,
              "JIT supera 2x al interprete (objetivo D.3 C1 baseline)");
        /* JIT C1 template puede ser bastante lento vs C (cada op = load+op+store) - aceptable hasta 30x slower. */
        CHECK(ratio < 30.0, "JIT < 30x slower que C (C1 template)");

        jit::JitRegistry::instance().clear();
    }

    /* ===================================================================== */
    /* Test 4: Unsupported op returns nullptr cleanly                          */
    /* ===================================================================== */

    void test_compile_unsupported() {
        /* IR con FADD (no soportado en v1 selector; float aritmetica
         * requiere lowering memory-roundtrip GP<->ZMM como en el interp).
         * DIV/MOD/SHL/ALLOCA SI estan soportadas tras Fase 5. */
        ir::IrFunction fn;
        fn.name = "fadd_unsupported";
        fn.ret_type = ir::IrType::F64;

        const auto a = mk_value(fn, ir::IrType::F64);
        const auto b = mk_value(fn, ir::IrType::F64);
        const auto q = mk_value(fn, ir::IrType::F64);
        fn.params = {a, b};

        ir::IrBlock e; e.id = 0; e.name = "entry";
        ir::IrInstr fadd = mk_instr(ir::IrOp::FADD, ir::IrType::F64, q);
        fadd.operands = {a, b};
        e.instrs.push_back(fadd);
        ir::IrInstr r = mk_instr(ir::IrOp::RET, ir::IrType::F64, ir::IR_NO_VALUE);
        r.operands.push_back(q);
        e.instrs.push_back(r);
        fn.blocks.push_back(e);

        jit::JitRegistry::instance().clear();
        jit::CodeCache cache;
        jit::RuntimeEntries rt; rt.resolve();
        jit::JitCompiler comp(cache, rt);
        jit::CompileResult res = comp.compile(fn, jit::SelectorMode::NATIVE_ABI);

        CHECK(res.unsupported, "FADD marca unsupported=true");
        CHECK(res.fn == nullptr, "fn = nullptr cuando unsupported");

        jit::JitRegistry::instance().clear();
    }

    /* ===================================================================== */
    /* Test 5: invalidate                                                      */
    /* ===================================================================== */

    void test_invalidate() {
        ir::IrFunction fn;
        fn.name = "ret_for_invalidate";
        fn.ret_type = ir::IrType::I64;

        const ir::IrValueId v = mk_value(fn, ir::IrType::I64);
        ir::IrBlock entry; entry.id = 0; entry.name = "entry";
        ir::IrInstr c = mk_instr(ir::IrOp::CONST, ir::IrType::I64, v); c.imm = 7;
        entry.instrs.push_back(c);
        ir::IrInstr r = mk_instr(ir::IrOp::RET, ir::IrType::I64, ir::IR_NO_VALUE);
        r.operands.push_back(v); entry.instrs.push_back(r);
        fn.blocks.push_back(entry);

        jit::JitRegistry::instance().clear();
        jit::CodeCache cache;
        jit::RuntimeEntries rt; rt.resolve();
        jit::JitCompiler comp(cache, rt);
        jit::CompileResult res = comp.compile(fn, jit::SelectorMode::NATIVE_ABI);
        CHECK(res.fn != nullptr, "compilado");
        CHECK(jit::JitRegistry::instance().size() == 1, "1 fn en registry");

        comp.invalidate(res);
        CHECK(jit::JitRegistry::instance().size() == 0, "registry vacio tras invalidate");
        /* Verificar que los bytes son 0xCC. */
        CHECK(res.code_start[0] == 0xCC, "bytes invalidados con INT3");

        jit::JitRegistry::instance().clear();
    }

} // namespace anonymous

int main() {
    test_compile_simple();
    test_phi_if_else();
    test_loop_phi_sum_to_n();
    test_compile_unsupported();
    test_invalidate();

    std::printf("test_jit_compiler: %d pass, %d fail\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
