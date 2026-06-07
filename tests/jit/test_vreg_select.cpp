/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/jit/test_vreg_select.cpp
 * @brief Test del selector vreg desde IR (Phase D.7, commit 4b).
 *
 * Construye IrFunctions, las selecciona a MachineIR de vregs, pasa por el
 * pipeline completo (intervals -> linear-scan -> rewrite) y EJECUTA el codigo
 * nativo, validando el resultado.  Tambien comprueba el fallback (false) ante
 * ops/estructuras fuera del subset de commit 4b.
 */

#include "ir/ssa_ir.h"
#include "jit/code_cache.h"
#include "jit/interval.h"
#include "jit/linear_scan.h"
#include "jit/regalloc_rewrite.h"
#include "jit/target_reginfo.h"
#include "jit/vreg_select.h"
#include "jit/x86_encoder.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace jit;

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, msg)                                                    \
    do { ++g_checks; if (!(cond)) { ++g_fails;                              \
        std::printf("  FAIL: %s  (linea %d)\n", (msg), __LINE__); } } while (0)

/* Helpers para construir IR. */
static ir::IrInstr konst(ir::IrValueId d, int64_t k) {
    ir::IrInstr i; i.op = ir::IrOp::CONST; i.type = ir::IrType::I64;
    i.dst = d; i.imm = static_cast<uint64_t>(k); return i;
}
static ir::IrInstr bin(ir::IrOp op, ir::IrValueId d, ir::IrValueId a, ir::IrValueId b) {
    ir::IrInstr i; i.op = op; i.type = ir::IrType::I64;
    i.dst = d; i.operands = { a, b }; return i;
}
static ir::IrInstr ret1(ir::IrValueId v) {
    ir::IrInstr i; i.op = ir::IrOp::RET; i.type = ir::IrType::I64;
    i.operands = { v }; return i;
}
static ir::IrInstr konst_t(ir::IrValueId d, int64_t k, ir::IrType t) {
    ir::IrInstr i; i.op = ir::IrOp::CONST; i.type = t;
    i.dst = d; i.imm = static_cast<uint64_t>(k); return i;
}
static ir::IrInstr conv(ir::IrOp op, ir::IrValueId d, ir::IrValueId s, ir::IrType dt) {
    ir::IrInstr i; i.op = op; i.type = dt; i.dst = d; i.operands = { s }; return i;
}
static ir::IrInstr cmp(ir::IrOp op, ir::IrValueId d, ir::IrValueId a, ir::IrValueId b) {
    ir::IrInstr i; i.op = op; i.type = ir::IrType::BOOL;
    i.dst = d; i.operands = { a, b }; return i;
}
static ir::IrInstr br(ir::IrBlockId t) {
    ir::IrInstr i; i.op = ir::IrOp::BR; i.target_block = t; return i;
}
static ir::IrInstr brc(ir::IrValueId c, ir::IrBlockId t, ir::IrBlockId f) {
    ir::IrInstr i; i.op = ir::IrOp::BR_COND; i.operands = { c };
    i.target_block = t; i.false_block = f; return i;
}
static ir::IrInstr phi2(ir::IrValueId d,
                        ir::IrValueId v0, ir::IrBlockId b0,
                        ir::IrValueId v1, ir::IrBlockId b1) {
    ir::IrInstr i; i.op = ir::IrOp::PHI; i.type = ir::IrType::I64; i.dst = d;
    i.phi_args = { { v0, b0 }, { v1, b1 } }; return i;
}

/** @brief Selecciona, asigna, reescribe, encodea y ejecuta @p fn. */
static bool jit_run(const ir::IrFunction &fn, int64_t &result_out) {
    MFunction mf;
    if (!vreg_select(fn, mf)) return false;
    const TargetRegInfo &tri = target_x86_64_vm_abi();
    RegAlloc ra = linear_scan(build_intervals(mf, tri), tri);
    MFunction pf = rewrite_to_physical(mf, ra, tri);

    X86Encoder enc;
    std::vector<uint8_t> bytes;
    if (enc.encode(pf, bytes) == 0 || bytes.empty()) return false;
    CodeCache cc;
    uint8_t *code = cc.alloc(bytes.size(), 16);
    if (!code) return false;
    std::memcpy(code, bytes.data(), bytes.size());
    cc.commit(code, bytes.size());
    using Fn = int64_t (*)();
    result_out = reinterpret_cast<Fn>(code)();
    /* Mantener viva la CodeCache HASTA despues de la llamada: a -O2 el
     * optimizador no ve que @c code apunta a memoria de @c cc y, sin esto,
     * podria ejecutar el destructor (VirtualFree) antes de la llamada. */
    asm volatile("" : : "r"(&cc) : "memory");
    return true;
}

/* ---- Test 1: f() = 6 * 7 = 42 ---------------------------------------- */
static void test_mul() {
    std::printf("[select] f()=6*7 -> 42\n");
    ir::IrFunction fn;
    fn.name = "mul42";
    fn.ret_type = ir::IrType::I64;
    ir::IrValueId v0 = fn.new_value(ir::IrType::I64);
    ir::IrValueId v1 = fn.new_value(ir::IrType::I64);
    ir::IrValueId v2 = fn.new_value(ir::IrType::I64);
    ir::IrBlockId b = fn.new_block("entry");
    fn.append(b, konst(v0, 6));
    fn.append(b, konst(v1, 7));
    fn.append(b, bin(ir::IrOp::MUL, v2, v0, v1));
    fn.append(b, ret1(v2));

    int64_t r = 0;
    CHECK(jit_run(fn, r), "jit_run ok (mul)");
    CHECK(r == 42, "6*7 == 42");
    if (r != 42) std::printf("    obtuvo %lld\n", (long long)r);
}

/* ---- Test 2: f() = (10+20)+(30+40)-10 = 90 --------------------------- */
static void test_chain() {
    std::printf("[select] f()=(10+20)+(30+40)-10 -> 90\n");
    ir::IrFunction fn;
    fn.name = "chain90";
    fn.ret_type = ir::IrType::I64;
    ir::IrValueId v0 = fn.new_value(ir::IrType::I64);
    ir::IrValueId v1 = fn.new_value(ir::IrType::I64);
    ir::IrValueId v2 = fn.new_value(ir::IrType::I64);
    ir::IrValueId v3 = fn.new_value(ir::IrType::I64);
    ir::IrValueId t0 = fn.new_value(ir::IrType::I64);
    ir::IrValueId t1 = fn.new_value(ir::IrType::I64);
    ir::IrValueId t2 = fn.new_value(ir::IrType::I64);
    ir::IrValueId t3 = fn.new_value(ir::IrType::I64);
    ir::IrBlockId b = fn.new_block("entry");
    fn.append(b, konst(v0, 10));
    fn.append(b, konst(v1, 20));
    fn.append(b, konst(v2, 30));
    fn.append(b, konst(v3, 40));
    fn.append(b, bin(ir::IrOp::ADD, t0, v0, v1));   // 30
    fn.append(b, bin(ir::IrOp::ADD, t1, v2, v3));   // 70
    fn.append(b, bin(ir::IrOp::ADD, t2, t0, t1));   // 100
    fn.append(b, bin(ir::IrOp::SUB, t3, t2, v0));   // 90
    fn.append(b, ret1(t3));

    int64_t r = 0;
    CHECK(jit_run(fn, r), "jit_run ok (chain)");
    CHECK(r == 90, "(10+20)+(30+40)-10 == 90");
    if (r != 90) std::printf("    obtuvo %lld\n", (long long)r);
}

/* ---- Test 3: loop con PHI -> sum(0..9) = 45 --------------------------- *
 * entry: n=10, zero=0, one=1; BR header
 * header: i=PHI(zero|entry, i_next|body); sum=PHI(zero|entry, sum_next|body)
 *         cond = i < n; BR_COND cond -> body, exit
 * body:   sum_next = sum + i; i_next = i + one; BR header
 * exit:   RET sum   (= 0+1+...+9 = 45) */
static void test_loop_sum() {
    std::printf("[select] loop con PHI: sum(0..9) -> 45\n");
    ir::IrFunction fn;
    fn.name = "sum_to_n";
    fn.ret_type = ir::IrType::I64;
    auto T = ir::IrType::I64;
    ir::IrValueId n  = fn.new_value(T), zero = fn.new_value(T), one = fn.new_value(T);
    ir::IrValueId i  = fn.new_value(T), sum  = fn.new_value(T);
    ir::IrValueId cond = fn.new_value(ir::IrType::BOOL);
    ir::IrValueId sum_next = fn.new_value(T), i_next = fn.new_value(T);

    ir::IrBlockId b0 = fn.new_block("entry");
    ir::IrBlockId b1 = fn.new_block("header");
    ir::IrBlockId b2 = fn.new_block("body");
    ir::IrBlockId b3 = fn.new_block("exit");

    fn.append(b0, konst(n, 10)); fn.append(b0, konst(zero, 0));
    fn.append(b0, konst(one, 1)); fn.append(b0, br(b1));

    fn.append(b1, phi2(i,   zero, b0, i_next,   b2));
    fn.append(b1, phi2(sum, zero, b0, sum_next, b2));
    fn.append(b1, cmp(ir::IrOp::CMP_LT, cond, i, n));
    fn.append(b1, brc(cond, b2, b3));

    fn.append(b2, bin(ir::IrOp::ADD, sum_next, sum, i));
    fn.append(b2, bin(ir::IrOp::ADD, i_next,   i,   one));
    fn.append(b2, br(b1));

    fn.append(b3, ret1(sum));

    int64_t r = 0;
    CHECK(jit_run(fn, r), "jit_run ok (loop)");
    CHECK(r == 45, "sum(0..9) == 45");
    if (r != 45) std::printf("    obtuvo %lld\n", (long long)r);
}

/* ---- Test conversiones: SEXT/ZEXT desde i8/u8 ------------------------ *
 * f() = sext_i8(0xFB) + zext_u8(0xFB) = -5 + 251 = 246 */
static void test_conversions() {
    std::printf("[select] conversiones: sext(-5) + zext(251) -> 246\n");
    ir::IrFunction fn;
    fn.name = "conv";
    fn.ret_type = ir::IrType::I64;
    ir::IrValueId i8v  = fn.new_value(ir::IrType::I8);
    ir::IrValueId u8v  = fn.new_value(ir::IrType::U8);
    ir::IrValueId s    = fn.new_value(ir::IrType::I64);
    ir::IrValueId z    = fn.new_value(ir::IrType::I64);
    ir::IrValueId r    = fn.new_value(ir::IrType::I64);
    ir::IrBlockId b = fn.new_block("entry");
    fn.append(b, konst_t(i8v, 0xFB, ir::IrType::I8));   // 0xFB = -5 (i8)
    fn.append(b, konst_t(u8v, 0xFB, ir::IrType::U8));   // 0xFB = 251 (u8)
    fn.append(b, conv(ir::IrOp::SEXT, s, i8v, ir::IrType::I64));  // -5
    fn.append(b, conv(ir::IrOp::ZEXT, z, u8v, ir::IrType::I64));  // 251
    fn.append(b, bin(ir::IrOp::ADD, r, s, z));                    // 246
    fn.append(b, ret1(r));

    int64_t v = 0;
    CHECK(jit_run(fn, v), "jit_run ok (conv)");
    CHECK(v == 246, "sext(-5)+zext(251)==246");
    if (v != 246) std::printf("    obtuvo %lld\n", (long long)v);
}

/* ---- Test 4: fallback ante op no soportado (LOAD) -------------------- */
static void test_fallback() {
    std::printf("[select] fallback (op no soportado) -> false\n");
    ir::IrFunction fn;
    fn.name = "unsup";
    fn.ret_type = ir::IrType::I64;
    ir::IrValueId v0 = fn.new_value(ir::IrType::I64);
    ir::IrValueId v1 = fn.new_value(ir::IrType::I64);
    ir::IrBlockId b0 = fn.new_block("entry");
    fn.append(b0, konst(v0, 1));
    ir::IrInstr ld; ld.op = ir::IrOp::LOAD; ld.dst = v1; ld.operands = { v0 };
    fn.append(b0, ld);
    fn.append(b0, ret1(v1));

    MFunction mf;
    CHECK(!vreg_select(fn, mf), "op LOAD -> vreg_select false (fallback)");
}

int main() {
    std::setbuf(stdout, nullptr);
    std::printf("=== test_vreg_select (Phase D.7 commit 4b/4c) ===\n");
    test_mul();
    test_chain();
    test_loop_sum();
    test_conversions();
    test_fallback();
    std::printf("--- %d checks, %d fallos ---\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
