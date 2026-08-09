/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/jit/test_vreg_vm.cpp
 * @brief Test del path vreg en modo VM_ABI (Phase D.7, commit 5a).
 *
 * Compila funciones IR con la convencion VM_ABI (args en
 * @c proc->registers.regs[1..N], return en @c regs[0], @c ProcessVM* en RBX)
 * y las ejecuta usando un PROXY que reproduce el layout (safepoint_flag en
 * offset 0, registers en offset 96), sin arrastrar el runtime completo.
 * Valida una funcion con args y un loop con args.
 */

#include "ir/ssa_ir.h"
#include "jit/code_cache.h"
#include "jit/interval.h"
#include "jit/linear_scan.h"
#include "jit/machine_ir.h"
#include "jit/regalloc_rewrite.h"
#include "jit/target_reginfo.h"
#include "jit/vreg_select.h"
#include "jit/x86_encoder.h"
#include "vesta_rt/abi.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace jit;

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, msg)                                                    \
    do { ++g_checks; if (!(cond)) { ++g_fails;                              \
        std::printf("  FAIL: %s  (linea %d)\n", (msg), __LINE__); } } while (0)

/** @brief Proxy que reproduce el layout que el codigo JIT espera (RBX). */
struct Proxy {
    uint8_t  safepoint_flag;                               // offset 0
    uint8_t  _pad[VESTA_PROC_REGISTERS_OFFSET - 1];        // hasta offset 96
    uint64_t regs[VESTA_PROC_REGISTER_COUNT];              // offset 96
};
static_assert(offsetof(Proxy, regs) == VESTA_PROC_REGISTERS_OFFSET,
              "regs debe quedar en el offset que usa el codigo JIT");

/* Helpers IR. */
static ir::IrInstr konst(ir::IrValueId d, int64_t k) {
    ir::IrInstr i; i.op = ir::IrOp::CONST; i.type = ir::IrType::I64;
    i.dst = d; i.imm = (uint64_t)k; return i;
}
static ir::IrInstr bin(ir::IrOp op, ir::IrValueId d, ir::IrValueId a, ir::IrValueId b) {
    ir::IrInstr i; i.op = op; i.type = ir::IrType::I64;
    i.dst = d; i.operands = { a, b }; return i;
}
static ir::IrInstr ret1(ir::IrValueId v) {
    ir::IrInstr i; i.op = ir::IrOp::RET; i.type = ir::IrType::I64;
    i.operands = { v }; return i;
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
static ir::IrInstr phi2(ir::IrValueId d, ir::IrValueId v0, ir::IrBlockId b0,
                        ir::IrValueId v1, ir::IrBlockId b1) {
    ir::IrInstr i; i.op = ir::IrOp::PHI; i.type = ir::IrType::I64; i.dst = d;
    i.phi_args = { { v0, b0 }, { v1, b1 } }; return i;
}
static ir::IrInstr conv(ir::IrOp op, ir::IrValueId d, ir::IrValueId s, ir::IrType dt) {
    ir::IrInstr i; i.op = op; i.type = dt; i.dst = d; i.operands = { s }; return i;
}

/** @brief Compila @p fn en modo VM y la ejecuta con @p px (RBX = &px). */
static bool jit_vm(const ir::IrFunction &fn, Proxy &px,
                   const CallResolver &resolve = {}, uint64_t callvirt_addr = 0,
                   const CallResolver &resolve_native = {},
                   const CallResolver &resolve_symbol = {}) {
    MFunction mf;
    VregEntries ent; ent.callvirt = callvirt_addr;
    if (!vreg_select(fn, mf, AbiKind::VM, resolve, ent, resolve_native,
                     resolve_symbol)) return false;
    const TargetRegInfo &tri = target_x86_64_vm_abi();
    RegAlloc ra = linear_scan(build_intervals(mf, tri), tri);
    MFunction pf = rewrite_to_physical(mf, ra, tri, AbiKind::VM);
    X86Encoder enc;
    std::vector<uint8_t> bytes;
    if (enc.encode(pf, bytes) == 0 || bytes.empty()) return false;
    CodeCache cc;
    uint8_t *code = cc.alloc(bytes.size(), 16);
    if (!code) return false;
    std::memcpy(code, bytes.data(), bytes.size());
    cc.commit(code, bytes.size());
    reinterpret_cast<void (*)(void *)>(code)(&px);
    asm volatile("" : : "r"(&cc) : "memory");  // mantener cc viva hasta aqui
    return true;
}

/** @brief Como jit_vm pero con un VregEntries completo (cluster strings). */
static bool jit_vm_ent(const ir::IrFunction &fn, Proxy &px, const VregEntries &ent) {
    MFunction mf;
    if (!vreg_select(fn, mf, AbiKind::VM, {}, ent, {}, {})) return false;
    const TargetRegInfo &tri = target_x86_64_vm_abi();
    RegAlloc ra = linear_scan(build_intervals(mf, tri), tri);
    MFunction pf = rewrite_to_physical(mf, ra, tri, AbiKind::VM);
    X86Encoder enc; std::vector<uint8_t> bytes;
    if (enc.encode(pf, bytes) == 0 || bytes.empty()) return false;
    CodeCache cc;
    uint8_t *code = cc.alloc(bytes.size(), 16);
    if (!code) return false;
    std::memcpy(code, bytes.data(), bytes.size());
    cc.commit(code, bytes.size());
    reinterpret_cast<void (*)(void *)>(code)(&px);
    asm volatile("" : : "r"(&cc) : "memory");
    return true;
}

/* ---- Test 1: add(a,b) con args desde proc->registers ------------------ */
static void test_vm_add() {
    std::printf("[vm] add(a,b): regs[1]=40, regs[2]=2 -> regs[0]=42\n");
    ir::IrFunction fn;
    fn.name = "add"; fn.ret_type = ir::IrType::I64;
    auto a = fn.new_value(ir::IrType::I64);
    auto b = fn.new_value(ir::IrType::I64);
    auto c = fn.new_value(ir::IrType::I64);
    fn.params = { a, b };
    auto bb = fn.new_block("e");
    fn.append(bb, bin(ir::IrOp::ADD, c, a, b));
    fn.append(bb, ret1(c));

    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 40; px.regs[2] = 2;
    CHECK(jit_vm(fn, px), "jit_vm ok (add)");
    CHECK(px.regs[0] == 42, "add(40,2)==42 en regs[0]");
    if (px.regs[0] != 42) std::printf("    regs[0]=%llu\n", (unsigned long long)px.regs[0]);
}

/* ---- Test 2: loop con arg -> sum(0..n-1), n en regs[1] ---------------- */
static void test_vm_loop() {
    std::printf("[vm] sum(n): regs[1]=10 -> regs[0]=45\n");
    ir::IrFunction fn;
    fn.name = "sum_n"; fn.ret_type = ir::IrType::I64;
    auto T = ir::IrType::I64;
    ir::IrValueId n = fn.new_value(T);            // param
    ir::IrValueId zero = fn.new_value(T), one = fn.new_value(T);
    ir::IrValueId i = fn.new_value(T), sum = fn.new_value(T);
    ir::IrValueId cond = fn.new_value(ir::IrType::BOOL);
    ir::IrValueId sum_next = fn.new_value(T), i_next = fn.new_value(T);
    fn.params = { n };

    ir::IrBlockId b0 = fn.new_block("entry");
    ir::IrBlockId b1 = fn.new_block("header");
    ir::IrBlockId b2 = fn.new_block("body");
    ir::IrBlockId b3 = fn.new_block("exit");

    fn.append(b0, konst(zero, 0)); fn.append(b0, konst(one, 1)); fn.append(b0, br(b1));
    fn.append(b1, phi2(i, zero, b0, i_next, b2));
    fn.append(b1, phi2(sum, zero, b0, sum_next, b2));
    fn.append(b1, cmp(ir::IrOp::CMP_LT, cond, i, n));
    fn.append(b1, brc(cond, b2, b3));
    fn.append(b2, bin(ir::IrOp::ADD, sum_next, sum, i));
    fn.append(b2, bin(ir::IrOp::ADD, i_next, i, one));
    fn.append(b2, br(b1));
    fn.append(b3, ret1(sum));

    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 10;
    CHECK(jit_vm(fn, px), "jit_vm ok (loop)");
    CHECK(px.regs[0] == 45, "sum(0..9)==45 en regs[0]");
    if (px.regs[0] != 45) std::printf("    regs[0]=%llu\n", (unsigned long long)px.regs[0]);
}

/* ---- Test 3: loop con SEXT loop-invariante (repro widen) ------------- *
 * widen(y): acc=0; for(i=0;i<1000;i++) acc += (i64)y;  return acc.
 * Con y=7 -> 7000.  El (i64)y es invariante (SEXT en entry); el i<1000
 * usa SEXT del contador. */
static void test_vm_sext_loop() {
    std::printf("[vm] widen(7): regs[1]=7 -> regs[0]=7000\n");
    ir::IrFunction fn;
    fn.name = "widen"; fn.ret_type = ir::IrType::I64;
    auto I64 = ir::IrType::I64; auto I32 = ir::IrType::I32;
    ir::IrValueId thisp  = fn.new_value(ir::IrType::PTR);  // param 0 (this, no usado)
    ir::IrValueId y      = fn.new_value(I32);   // param 1
    ir::IrValueId acc0   = fn.new_value(I64), i0 = fn.new_value(I32);
    ir::IrValueId binv   = fn.new_value(I64);   // (i64)y invariante
    ir::IrValueId one    = fn.new_value(I32), n = fn.new_value(I64);
    ir::IrValueId i_phi  = fn.new_value(I32), acc_phi = fn.new_value(I64);
    ir::IrValueId i_ext  = fn.new_value(I64);
    ir::IrValueId cond   = fn.new_value(ir::IrType::BOOL);
    ir::IrValueId acc_n  = fn.new_value(I64), i_n = fn.new_value(I32);
    fn.params = { thisp, y };   // this=param0 (regs[1]), y=param1 (regs[2])

    ir::IrBlockId b0 = fn.new_block("entry");
    ir::IrBlockId b1 = fn.new_block("header");
    ir::IrBlockId b2 = fn.new_block("body");
    ir::IrBlockId b3 = fn.new_block("exit");

    auto konst_i32 = [](ir::IrValueId d, int64_t k) {
        ir::IrInstr i; i.op = ir::IrOp::CONST; i.type = ir::IrType::I32;
        i.dst = d; i.imm = static_cast<uint64_t>(k); return i;
    };
    fn.append(b0, konst(acc0, 0));
    fn.append(b0, konst_i32(i0, 0));
    fn.append(b0, conv(ir::IrOp::SEXT, binv, y, I64));   // binv = (i64)y
    fn.append(b0, konst_i32(one, 1));
    fn.append(b0, konst(n, 1000));
    fn.append(b0, br(b1));

    fn.append(b1, phi2(i_phi, i0, b0, i_n, b2));
    fn.append(b1, phi2(acc_phi, acc0, b0, acc_n, b2));
    fn.append(b1, conv(ir::IrOp::SEXT, i_ext, i_phi, I64));
    fn.append(b1, cmp(ir::IrOp::CMP_LT, cond, i_ext, n));
    fn.append(b1, brc(cond, b2, b3));

    fn.append(b2, bin(ir::IrOp::ADD, acc_n, acc_phi, binv));
    {
        ir::IrInstr a; a.op = ir::IrOp::ADD; a.type = I32;
        a.dst = i_n; a.operands = { i_phi, one }; fn.append(b2, a);
    }
    fn.append(b2, br(b1));

    fn.append(b3, ret1(acc_phi));

    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 0x1234;  // this (no usado)
    px.regs[2] = 7;       // y
    CHECK(jit_vm(fn, px), "jit_vm ok (widen)");
    CHECK(px.regs[0] == 7000, "widen(7)==7000 (SEXT invariante)");
    if (px.regs[0] != 7000) std::printf("    regs[0]=%llu\n", (unsigned long long)px.regs[0]);
}

/** @brief Callee de prueba con convencion VM: lee args de proc->registers
 *  (regs[1], regs[2]) y escribe el resultado en regs[0].  a*3 + b. */
extern "C" void vm_helper(void *proc) {
    Proxy *p = static_cast<Proxy *>(proc);
    p->regs[0] = p->regs[1] * 3 + p->regs[2];
}

/* ---- Test 4: CALL a otra funcion (convencion VM) --------------------- *
 * caller(a,b) = helper(a,b) = a*3 + b.  Con a=10,b=5 -> 35. */
static void test_vm_call() {
    std::printf("[vm] caller(10,5) -> helper -> regs[0]=35\n");
    ir::IrFunction fn;
    fn.name = "caller"; fn.ret_type = ir::IrType::I64;
    auto I64 = ir::IrType::I64;
    ir::IrValueId a = fn.new_value(I64), b = fn.new_value(I64);
    ir::IrValueId r = fn.new_value(I64);
    fn.params = { a, b };
    ir::IrBlockId bb = fn.new_block("e");
    {
        ir::IrInstr c; c.op = ir::IrOp::CALL; c.type = I64; c.dst = r;
        c.func_name = "helper"; c.operands = { a, b };
        fn.append(bb, c);
    }
    fn.append(bb, ret1(r));

    CallResolver resolver = [](const std::string &name) -> uint64_t {
        if (name == "helper")
            return reinterpret_cast<uint64_t>(reinterpret_cast<void *>(&vm_helper));
        return 0;
    };

    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 10; px.regs[2] = 5;
    CHECK(jit_vm(fn, px, resolver), "jit_vm ok (call)");
    CHECK(px.regs[0] == 35, "caller(10,5)==35 (CALL VM)");
    if (px.regs[0] != 35) std::printf("    regs[0]=%llu\n", (unsigned long long)px.regs[0]);
}

/** @brief Stub de vrt_callvirt: lee args de proc->registers (regs[2]=arg) +
 *  vtbl_idx, escribe el resultado en regs[0].  arg*10 + idx. */
extern "C" uint64_t vm_callvirt_stub(void *proc, void * /*obj*/, uint32_t idx) {
    Proxy *p = static_cast<Proxy *>(proc);
    p->regs[0] = p->regs[2] * 10 + idx;
    return p->regs[0];
}

/* ---- Test 5: CALLVIRT (dispatch via vrt_callvirt stub) --------------- *
 * caller(this,x) = callvirt(this, idx=5)(x) = x*10 + 5.  Con x=7 -> 75. */
static void test_vm_callvirt() {
    std::printf("[vm] caller(this,7) -> callvirt idx=5 -> regs[0]=75\n");
    ir::IrFunction fn;
    fn.name = "cv_caller"; fn.ret_type = ir::IrType::I64;
    auto I64 = ir::IrType::I64;
    ir::IrValueId thisp = fn.new_value(ir::IrType::PTR);
    ir::IrValueId x = fn.new_value(I64), r = fn.new_value(I64);
    fn.values[thisp].is_gc_object = true;   // receptor GC (muere en el call)
    fn.params = { thisp, x };
    ir::IrBlockId bb = fn.new_block("e");
    {
        ir::IrInstr c; c.op = ir::IrOp::CALLVIRT; c.type = I64; c.dst = r;
        c.imm = 5; c.operands = { thisp, x };  // operands[0]=this, [1]=x; imm=vtbl_idx
        fn.append(bb, c);
    }
    fn.append(bb, ret1(r));

    const uint64_t cv = reinterpret_cast<uint64_t>(
        reinterpret_cast<void *>(&vm_callvirt_stub));

    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 0xABCD; px.regs[2] = 7;
    CHECK(jit_vm(fn, px, {}, cv), "jit_vm ok (callvirt)");
    CHECK(px.regs[0] == 75, "caller(this,7)==75 (CALLVIRT)");
    if (px.regs[0] != 75) std::printf("    regs[0]=%llu\n", (unsigned long long)px.regs[0]);
}

/* ---- Test STRMAKE (cluster strings) -------------------------------- *
 * f() = strmake(0x100, 5) -> stub devuelve handle 0x7777 y registra los
 * args (proc, vm_addr, byte_len).  Fuerza el path JIT REAL con ent.str_make
 * apuntando al stub. */
static uint64_t g_strmake_calls = 0;
static uint64_t g_strmake_vaddr = 0, g_strmake_len = 0;
extern "C" uint64_t vm_strmake_stub(void *proc, uint64_t vaddr, uint32_t len) {
    (void)proc;
    ++g_strmake_calls; g_strmake_vaddr = vaddr; g_strmake_len = len;
    return 0x7777ULL;  // handle falso
}
static void test_vm_strmake() {
    std::printf("[vm] strmake(0x100, 5) -> stub handle 0x7777\n");
    ir::IrFunction fn;
    fn.name = "smk"; fn.ret_type = ir::IrType::I64;
    auto T = ir::IrType::I64;
    ir::IrValueId buf = fn.new_value(T), len = fn.new_value(T), h = fn.new_value(T);
    ir::IrBlockId bb = fn.new_block("e");
    fn.append(bb, konst(buf, 0x100));
    fn.append(bb, konst(len, 5));
    {
        ir::IrInstr c; c.op = ir::IrOp::STRMAKE; c.type = T; c.dst = h;
        c.operands = { buf, len }; c.imm = 0; c.set_is_call_site(true);
        fn.append(bb, c);
    }
    fn.append(bb, ret1(h));

    MFunction mf;
    VregEntries ent; ent.str_make =
        reinterpret_cast<uint64_t>(reinterpret_cast<void *>(&vm_strmake_stub));
    bool ok = vreg_select(fn, mf, AbiKind::VM, {}, ent, {}, {});
    CHECK(ok, "vreg_select strmake ok");
    if (!ok) return;
    const TargetRegInfo &tri = target_x86_64_vm_abi();
    RegAlloc ra = linear_scan(build_intervals(mf, tri), tri);
    MFunction pf = rewrite_to_physical(mf, ra, tri, AbiKind::VM);
    X86Encoder enc; std::vector<uint8_t> bytes;
    CHECK(enc.encode(pf, bytes) != 0 && !bytes.empty(), "encode strmake");
    if (bytes.empty()) return;
    CodeCache cc; uint8_t *code = cc.alloc(bytes.size(), 16);
    if (!code) { CHECK(false, "code cache strmake"); return; }
    std::memcpy(code, bytes.data(), bytes.size()); cc.commit(code, bytes.size());
    Proxy px; std::memset(&px, 0, sizeof(px));
    g_strmake_calls = 0; g_strmake_vaddr = 0; g_strmake_len = 0;
    reinterpret_cast<void (*)(void *)>(code)(&px);
    asm volatile("" : : "r"(&cc) : "memory");
    CHECK(g_strmake_calls == 1, "strmake llama al runtime 1 vez");
    CHECK(g_strmake_vaddr == 0x100, "strmake pasa vm_addr correcto");
    CHECK(g_strmake_len == 5, "strmake pasa byte_len correcto");
    CHECK(px.regs[0] == 0x7777, "strmake result handle en regs[0]");
    if (px.regs[0] != 0x7777)
        std::printf("    regs[0]=%llu\n", (unsigned long long)px.regs[0]);
}

/* ---- Test STRLEN (cluster strings, 1-arg) -------------------------- *
 * f(s) = strlen(s).  s en regs[1].  Stub registra el handle y devuelve 99. */
static uint64_t g_strlen_h = 0;
extern "C" uint64_t vm_strlen_stub(void *proc, uint64_t h) {
    (void)proc; g_strlen_h = h; return 99;
}
static void test_vm_strlen() {
    std::printf("[vm] strlen(s): regs[1]=0x55 -> stub -> regs[0]=99\n");
    ir::IrFunction fn;
    fn.name = "sln"; fn.ret_type = ir::IrType::I64;
    auto T = ir::IrType::I64;
    ir::IrValueId s = fn.new_value(T), r = fn.new_value(T);
    fn.params = { s };
    ir::IrBlockId bb = fn.new_block("e");
    { ir::IrInstr c; c.op = ir::IrOp::STRLEN; c.type = T; c.dst = r;
      c.operands = { s }; fn.append(bb, c); }
    fn.append(bb, ret1(r));
    VregEntries ent; ent.str_len =
        reinterpret_cast<uint64_t>(reinterpret_cast<void *>(&vm_strlen_stub));
    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 0x55; g_strlen_h = 0;
    CHECK(jit_vm_ent(fn, px, ent), "jit_vm strlen ok");
    CHECK(g_strlen_h == 0x55, "strlen pasa handle correcto");
    CHECK(px.regs[0] == 99, "strlen result en regs[0]");
}

/* ---- Test STRCAT (cluster strings, 2-arg + GC handle) -------------- *
 * f(s,t) = strcat(s,t).  s,t en regs[1],regs[2].  Stub registra a/b. */
static uint64_t g_strcat_a = 0, g_strcat_b = 0;
extern "C" uint64_t vm_strcat_stub(void *proc, uint64_t a, uint64_t b) {
    (void)proc; g_strcat_a = a; g_strcat_b = b; return 0xC0FFEEULL;
}
static void test_vm_strcat() {
    std::printf("[vm] strcat(s,t): regs[1]=0x11 regs[2]=0x22 -> 0xC0FFEE\n");
    ir::IrFunction fn;
    fn.name = "sct"; fn.ret_type = ir::IrType::I64;
    auto T = ir::IrType::I64;
    ir::IrValueId s = fn.new_value(T), t = fn.new_value(T), r = fn.new_value(T);
    fn.params = { s, t };
    ir::IrBlockId bb = fn.new_block("e");
    { ir::IrInstr c; c.op = ir::IrOp::STRCAT; c.type = T; c.dst = r;
      c.operands = { s, t }; c.set_is_call_site(true); fn.append(bb, c); }
    fn.append(bb, ret1(r));
    VregEntries ent; ent.str_cat =
        reinterpret_cast<uint64_t>(reinterpret_cast<void *>(&vm_strcat_stub));
    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 0x11; px.regs[2] = 0x22; g_strcat_a = 0; g_strcat_b = 0;
    CHECK(jit_vm_ent(fn, px, ent), "jit_vm strcat ok");
    CHECK(g_strcat_a == 0x11, "strcat pasa arg a correcto");
    CHECK(g_strcat_b == 0x22, "strcat pasa arg b correcto");
    CHECK(px.regs[0] == 0xC0FFEEULL, "strcat result handle en regs[0]");
}

/* ---- Test ADT markers (MAKE_VARIANT/MATCH_VARIANT son no-op) -------- *
 * f(a,b) = make_variant(); add; match_variant(); -> a+b.  Los markers no
 * deben perturbar el codegen circundante. */
static void test_vm_variant_markers() {
    std::printf("[vm] variant markers: f(40,2) -> regs[0]=42 (markers no-op)\n");
    ir::IrFunction fn;
    fn.name = "varm"; fn.ret_type = ir::IrType::I64;
    auto T = ir::IrType::I64;
    ir::IrValueId a = fn.new_value(T), b = fn.new_value(T), r = fn.new_value(T);
    fn.params = { a, b };
    ir::IrBlockId bb = fn.new_block("e");
    { ir::IrInstr mk; mk.op = ir::IrOp::MAKE_VARIANT; mk.type = ir::IrType::VOID;
      mk.dst = ir::IR_NO_VALUE; mk.func_name = "E.V"; mk.imm = 0;
      mk.operands = { a }; fn.append(bb, mk); }
    fn.append(bb, bin(ir::IrOp::ADD, r, a, b));
    { ir::IrInstr mt; mt.op = ir::IrOp::MATCH_VARIANT; mt.type = ir::IrType::VOID;
      mt.dst = ir::IR_NO_VALUE; mt.func_name = "E"; mt.imm = 1;
      mt.operands = { a }; fn.append(bb, mt); }
    fn.append(bb, ret1(r));
    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 40; px.regs[2] = 2;
    VregEntries ent;
    CHECK(jit_vm_ent(fn, px, ent), "jit_vm variant markers ok");
    CHECK(px.regs[0] == 42, "markers no-op: f(40,2)==42");
}

/* ---- Test SMARTPTR_FREE kind 1 (EXTERN_CALLN) ---------------------- *
 * f(ptr) { smartptr_free.kind=1(ptr) "lib:del" }.  Si ptr!=0 llama al
 * deleter nativo con ptr en arg0; si ptr==0 NO lo llama (null-safe). */
static uint64_t g_spnat_ptr = 0; static int g_spnat_calls = 0;
extern "C" void vm_sp_native_del(uint64_t p) { g_spnat_ptr = p; ++g_spnat_calls; }
static ir::IrInstr ret_void() {
    ir::IrInstr i; i.op = ir::IrOp::RET; i.type = ir::IrType::VOID; return i;
}
static void test_vm_smartptr_free_extern() {
    std::printf("[vm] smartptr_free kind=1 (extern): null-safe + arg0=ptr\n");
    ir::IrFunction fn;
    fn.name = "spf1"; fn.ret_type = ir::IrType::VOID;
    ir::IrValueId ptr = fn.new_value(ir::IrType::PTR);
    fn.values[ptr].is_host_ptr = true;
    fn.params = { ptr };
    ir::IrBlockId bb = fn.new_block("e");
    { ir::IrInstr c; c.op = ir::IrOp::SMARTPTR_FREE; c.type = ir::IrType::VOID;
      c.dst = ir::IR_NO_VALUE; c.operands = { ptr }; c.imm = 1;
      c.func_name = "lib:del"; c.set_is_call_site(true); fn.append(bb, c); }
    fn.append(bb, ret_void());
    CallResolver rn = [](const std::string &n) -> uint64_t {
        return n == "lib:del"
            ? reinterpret_cast<uint64_t>(reinterpret_cast<void *>(&vm_sp_native_del))
            : 0;
    };
    /* ptr != 0 -> deleter llamado con ptr. */
    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 0xDEAD; g_spnat_ptr = 0; g_spnat_calls = 0;
    CHECK(jit_vm(fn, px, {}, 0, rn), "jit_vm smartptr_free k1 ok");
    CHECK(g_spnat_calls == 1, "k1: deleter llamado 1 vez (ptr!=0)");
    CHECK(g_spnat_ptr == 0xDEAD, "k1: deleter recibe ptr en arg0");
    /* ptr == 0 -> deleter NO llamado. */
    Proxy px0; std::memset(&px0, 0, sizeof(px0));
    px0.regs[1] = 0; g_spnat_calls = 0;
    CHECK(jit_vm(fn, px0, {}, 0, rn), "jit_vm smartptr_free k1 null ok");
    CHECK(g_spnat_calls == 0, "k1: deleter NO llamado (ptr==0, null-safe)");
}

/* ---- Test SMARTPTR_FREE kind 2 (VESTA_CALLVM) --------------------- *
 * f(ptr) { smartptr_free.kind=2(ptr) "del" }.  Si ptr!=0 stage ptr a
 * regs[1] + call vesta(proc); el deleter lee regs[1]. */
static uint64_t g_spves_ptr = 0; static int g_spves_calls = 0;
extern "C" void vm_sp_vesta_del(void *proc) {
    Proxy *p = static_cast<Proxy *>(proc);
    g_spves_ptr = p->regs[1]; ++g_spves_calls;
}
static void test_vm_smartptr_free_vesta() {
    std::printf("[vm] smartptr_free kind=2 (vesta): null-safe + regs[1]=ptr\n");
    ir::IrFunction fn;
    fn.name = "spf2"; fn.ret_type = ir::IrType::VOID;
    ir::IrValueId ptr = fn.new_value(ir::IrType::PTR);
    fn.values[ptr].is_host_ptr = true;
    fn.params = { ptr };
    ir::IrBlockId bb = fn.new_block("e");
    { ir::IrInstr c; c.op = ir::IrOp::SMARTPTR_FREE; c.type = ir::IrType::VOID;
      c.dst = ir::IR_NO_VALUE; c.operands = { ptr }; c.imm = 2;
      c.func_name = "del"; c.set_is_call_site(true); fn.append(bb, c); }
    fn.append(bb, ret_void());
    CallResolver rc = [](const std::string &n) -> uint64_t {
        return n == "del"
            ? reinterpret_cast<uint64_t>(reinterpret_cast<void *>(&vm_sp_vesta_del))
            : 0;
    };
    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 0xBEEF; g_spves_ptr = 0; g_spves_calls = 0;
    CHECK(jit_vm(fn, px, rc), "jit_vm smartptr_free k2 ok");
    CHECK(g_spves_calls == 1, "k2: deleter llamado 1 vez (ptr!=0)");
    CHECK(g_spves_ptr == 0xBEEF, "k2: deleter lee ptr de regs[1]");
    Proxy px0; std::memset(&px0, 0, sizeof(px0));
    px0.regs[1] = 0; g_spves_calls = 0;
    CHECK(jit_vm(fn, px0, rc), "jit_vm smartptr_free k2 null ok");
    CHECK(g_spves_calls == 0, "k2: deleter NO llamado (ptr==0, null-safe)");
}

/* ---- Test CALLCLOSURE ---------------------------------------------- *
 * f(arg) = callclosure(fn=0xFACE, env=0xE0, arg).  El stub registra
 * fn_addr, env, regs[1]=arg, regs[15]=nargs y devuelve 0xABBA. */
static uint64_t g_clo_fn = 0, g_clo_env = 0, g_clo_arg = 0, g_clo_nargs = 0;
extern "C" uint64_t vm_callclosure_stub(void *proc, uint64_t fn, uint64_t env) {
    Proxy *p = static_cast<Proxy *>(proc);
    g_clo_fn = fn; g_clo_env = env;
    g_clo_arg = p->regs[1]; g_clo_nargs = p->regs[15];
    return 0xABBAULL;
}
static void test_vm_callclosure() {
    std::printf("[vm] callclosure(fn=0xFACE, env=0xE0, arg) -> 0xABBA\n");
    ir::IrFunction fn;
    fn.name = "clo"; fn.ret_type = ir::IrType::I64;
    auto T = ir::IrType::I64;
    ir::IrValueId arg = fn.new_value(T), fnp = fn.new_value(T);
    ir::IrValueId env = fn.new_value(T), r = fn.new_value(T);
    fn.params = { arg };
    ir::IrBlockId bb = fn.new_block("e");
    fn.append(bb, konst(fnp, 0xFACE));
    fn.append(bb, konst(env, 0xE0));
    { ir::IrInstr c; c.op = ir::IrOp::CALLCLOSURE; c.type = T; c.dst = r;
      c.func_ptr = fnp; c.operands = { env, arg }; c.set_is_call_site(true);
      fn.append(bb, c); }
    fn.append(bb, ret1(r));
    VregEntries ent; ent.callclosure =
        reinterpret_cast<uint64_t>(reinterpret_cast<void *>(&vm_callclosure_stub));
    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 0x123;
    g_clo_fn = g_clo_env = g_clo_arg = g_clo_nargs = 0;
    CHECK(jit_vm_ent(fn, px, ent), "jit_vm callclosure ok");
    CHECK(g_clo_fn == 0xFACE, "callclosure pasa fn_addr");
    CHECK(g_clo_env == 0xE0, "callclosure pasa env");
    CHECK(g_clo_arg == 0x123, "callclosure stage arg en regs[1]");
    CHECK(g_clo_nargs == 1, "callclosure stage nargs=1 en regs[15]");
    CHECK(px.regs[0] == 0xABBAULL, "callclosure result en regs[0]");
}

/* ---- Test READ_VM_REG ---------------------------------------------- *
 * f() = read_vm_reg(3).  regs[3]=0x1234 -> regs[0]=0x1234. */
static void test_vm_read_vm_reg() {
    std::printf("[vm] read_vm_reg(3): regs[3]=0x1234 -> regs[0]=0x1234\n");
    ir::IrFunction fn;
    fn.name = "rvr"; fn.ret_type = ir::IrType::I64;
    auto T = ir::IrType::I64;
    ir::IrValueId r = fn.new_value(T);
    ir::IrBlockId bb = fn.new_block("e");
    { ir::IrInstr c; c.op = ir::IrOp::READ_VM_REG; c.type = T; c.dst = r;
      c.imm = 3; fn.append(bb, c); }
    fn.append(bb, ret1(r));
    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[3] = 0x1234;
    VregEntries ent;
    CHECK(jit_vm_ent(fn, px, ent), "jit_vm read_vm_reg ok");
    CHECK(px.regs[0] == 0x1234, "read_vm_reg(3) -> regs[0]==0x1234");
}

/** @brief Callee de prueba: pone regs[0]=50 (simula trabajo + clobbea
 *  caller-saved como cualquier funcion C). */
extern "C" void g_stub(void *proc) {
    Proxy *p = static_cast<Proxy *>(proc);
    p->regs[0] = 50;
}

/* ===================================================================== *
 * LOAD_VM / STORE_VM (cobertura vm_mem, 2026-06-09): acceso a memoria del
 * VM (vaddr, is_host_ptr=false) via page-cache INLINE + fallback al runtime.
 *
 * Estrategia de validacion (path JIT REAL, no interp enmascarado):
 *   - Proxy = buffer crudo dimensionado con kProcVmMemOffset (runtime const,
 *     resuelto al enlazar abi_checks.cpp).  cached_page_vaddr/host escritos
 *     por offset; regs@96.
 *   - vm_read/write_u64 = STUBS propios (no el runtime real, que necesitaria
 *     un VirtualMemory mapeado) -> el page-miss es determinista.
 *   - HIT: cached_page = page(vaddr) -> lee/escribe el host buffer SIN llamar
 *     al stub.  MISS: cache mismatch -> llama al stub con (vaddr[,val]).
 * ===================================================================== */

/* Stubs del fallback page-miss. */
static int      g_vmstub_rd_calls = 0;
static uint64_t g_vmstub_rd_vaddr = 0;
static uint64_t stub_vm_read_u64(void * /*proc*/, uint64_t vaddr) {
    ++g_vmstub_rd_calls; g_vmstub_rd_vaddr = vaddr; return 0xCAFEULL;
}
static int      g_vmstub_wr_calls = 0;
static uint64_t g_vmstub_wr_vaddr = 0, g_vmstub_wr_val = 0;
static void stub_vm_write_u64(void * /*proc*/, uint64_t vaddr, uint64_t val) {
    ++g_vmstub_wr_calls; g_vmstub_wr_vaddr = vaddr; g_vmstub_wr_val = val;
}

/** @brief Compila @p fn en VM_ABI con @p ent custom y la ejecuta con RBX=@p proc. */
static bool jit_vm_mem(const ir::IrFunction &fn, void *proc, VregEntries &ent) {
    MFunction mf;
    if (!vreg_select(fn, mf, AbiKind::VM, {}, ent, {}, {})) return false;
    const TargetRegInfo &tri = target_x86_64_vm_abi();
    RegAlloc ra = linear_scan(build_intervals(mf, tri), tri);
    MFunction pf = rewrite_to_physical(mf, ra, tri, AbiKind::VM);
    X86Encoder enc; std::vector<uint8_t> bytes;
    if (enc.encode(pf, bytes) == 0 || bytes.empty()) return false;
    CodeCache cc;
    uint8_t *code = cc.alloc(bytes.size(), 16);
    if (!code) return false;
    std::memcpy(code, bytes.data(), bytes.size());
    cc.commit(code, bytes.size());
    reinterpret_cast<void (*)(void *)>(code)(proc);
    asm volatile("" : : "r"(&cc) : "memory");
    return true;
}

static void test_vm_load_vm() {
    std::printf("[vm] LOAD_VM: page-cache hit (sin runtime) + miss (fallback)\n");
    /* IR: ldvm(ptr) { v = vm_mem[ptr]; return v; }  ptr.is_host_ptr=false. */
    ir::IrFunction fn; fn.name = "ldvm"; fn.ret_type = ir::IrType::I64;
    ir::IrValueId ptr = fn.new_value(ir::IrType::PTR);  // vm-addr (no host)
    ir::IrValueId v   = fn.new_value(ir::IrType::I64);
    fn.params = { ptr };
    ir::IrBlockId bb = fn.new_block("e");
    { ir::IrInstr i; i.op = ir::IrOp::LOAD; i.type = ir::IrType::I64;
      i.dst = v; i.operands = { ptr }; fn.append(bb, i); }
    fn.append(bb, ret1(v));

    const size_t PROC_SZ = static_cast<size_t>(vesta_rt::kProcVmMemOffset) + 512;
    std::vector<uint8_t> proc(PROC_SZ, 0);
    auto regs = reinterpret_cast<uint64_t *>(proc.data() + VESTA_PROC_REGISTERS_OFFSET);
    auto pcv  = reinterpret_cast<uint64_t *>(proc.data() + vesta_rt::kProcVmMemOffset
                                           + vesta_rt::kVmMemCachedPageVaddrOffset);
    auto pch  = reinterpret_cast<uint8_t **>(proc.data() + vesta_rt::kProcVmMemOffset
                                           + vesta_rt::kVmMemCachedPageHostOffset);
    std::vector<uint8_t> page(4096, 0);
    const uint64_t vaddr = 0x20040ULL;
    *reinterpret_cast<uint64_t *>(page.data() + (vaddr & 0xFFF)) = 0x1234567890ULL;

    VregEntries ent; ent.vm_read_u64 = reinterpret_cast<uint64_t>(&stub_vm_read_u64);

    /* HIT: pagina cacheada == page(vaddr). */
    *pcv = vaddr & ~0xFFFULL;
    *pch = page.data();
    regs[1] = vaddr;
    g_vmstub_rd_calls = 0;
    CHECK(jit_vm_mem(fn, proc.data(), ent), "LOAD_VM compila/ejecuta (hit)");
    CHECK(regs[0] == 0x1234567890ULL, "LOAD_VM hit lee del host buffer");
    CHECK(g_vmstub_rd_calls == 0, "LOAD_VM hit NO llama al runtime");
    if (regs[0] != 0x1234567890ULL)
        std::printf("    regs[0]=0x%llx\n", (unsigned long long)regs[0]);

    /* MISS: pagina cacheada != page(vaddr) -> fallback al stub. */
    *pcv = 0x99000ULL;
    regs[1] = vaddr; regs[0] = 0;
    g_vmstub_rd_calls = 0; g_vmstub_rd_vaddr = 0;
    CHECK(jit_vm_mem(fn, proc.data(), ent), "LOAD_VM compila/ejecuta (miss)");
    CHECK(g_vmstub_rd_calls == 1, "LOAD_VM miss llama al runtime 1 vez");
    CHECK(g_vmstub_rd_vaddr == vaddr, "LOAD_VM miss pasa vaddr correcto");
    CHECK(regs[0] == 0xCAFEULL, "LOAD_VM miss propaga el resultado");
}

static void test_vm_store_vm() {
    std::printf("[vm] STORE_VM: page-cache hit (sin runtime) + miss (fallback)\n");
    /* IR: stvm(ptr, val) { vm_mem[ptr] = val; }  ptr.is_host_ptr=false. */
    ir::IrFunction fn; fn.name = "stvm"; fn.ret_type = ir::IrType::VOID;
    ir::IrValueId ptr = fn.new_value(ir::IrType::PTR);
    ir::IrValueId val = fn.new_value(ir::IrType::I64);
    fn.params = { ptr, val };
    ir::IrBlockId bb = fn.new_block("e");
    { ir::IrInstr i; i.op = ir::IrOp::STORE; i.type = ir::IrType::I64;
      i.operands = { val, ptr }; fn.append(bb, i); }   // [0]=val, [1]=ptr
    { ir::IrInstr i; i.op = ir::IrOp::RET; i.type = ir::IrType::VOID;
      fn.append(bb, i); }

    const size_t PROC_SZ = static_cast<size_t>(vesta_rt::kProcVmMemOffset) + 512;
    std::vector<uint8_t> proc(PROC_SZ, 0);
    auto regs = reinterpret_cast<uint64_t *>(proc.data() + VESTA_PROC_REGISTERS_OFFSET);
    auto pcv  = reinterpret_cast<uint64_t *>(proc.data() + vesta_rt::kProcVmMemOffset
                                           + vesta_rt::kVmMemCachedPageVaddrOffset);
    auto pch  = reinterpret_cast<uint8_t **>(proc.data() + vesta_rt::kProcVmMemOffset
                                           + vesta_rt::kVmMemCachedPageHostOffset);
    std::vector<uint8_t> page(4096, 0);
    const uint64_t vaddr = 0x30080ULL;

    VregEntries ent; ent.vm_write_u64 = reinterpret_cast<uint64_t>(&stub_vm_write_u64);

    /* HIT: escribe directo al host buffer, sin runtime. */
    *pcv = vaddr & ~0xFFFULL;
    *pch = page.data();
    regs[1] = vaddr; regs[2] = 0xDEADBEEFULL;
    g_vmstub_wr_calls = 0;
    CHECK(jit_vm_mem(fn, proc.data(), ent), "STORE_VM compila/ejecuta (hit)");
    const uint64_t stored =
        *reinterpret_cast<uint64_t *>(page.data() + (vaddr & 0xFFF));
    CHECK(stored == 0xDEADBEEFULL, "STORE_VM hit escribe al host buffer");
    CHECK(g_vmstub_wr_calls == 0, "STORE_VM hit NO llama al runtime");
    if (stored != 0xDEADBEEFULL)
        std::printf("    stored=0x%llx\n", (unsigned long long)stored);

    /* MISS: fallback al stub con (vaddr, val). */
    *pcv = 0x99000ULL;
    regs[1] = vaddr; regs[2] = 0xBEEFULL;
    g_vmstub_wr_calls = 0; g_vmstub_wr_vaddr = 0; g_vmstub_wr_val = 0;
    CHECK(jit_vm_mem(fn, proc.data(), ent), "STORE_VM compila/ejecuta (miss)");
    CHECK(g_vmstub_wr_calls == 1, "STORE_VM miss llama al runtime 1 vez");
    CHECK(g_vmstub_wr_vaddr == vaddr, "STORE_VM miss pasa vaddr correcto");
    CHECK(g_vmstub_wr_val == 0xBEEFULL, "STORE_VM miss pasa val correcto");
}

/* ---- Test self-recursion: CALL con func_name==fn.name -> rel32 a code+0 - *
 * fact(n) = (n < 2) ? n : n * fact(n-1).  fact(5) = 120.  El self-call NO
 * pasa por el resolver (is_self): emite CALL rel32 al prologue (label del
 * bloque 0).  El prologue recarga el param de proc->registers (que el caller
 * acaba de escribir) y monta un frame fresco -> recursion real en JIT. */
static void test_vm_self_recursion() {
    std::printf("[vm] fact(5) self-recursivo (CALL rel32 a code+0) -> regs[0]=120\n");
    ir::IrFunction fn;
    fn.name = "fact"; fn.ret_type = ir::IrType::I64;
    auto I64 = ir::IrType::I64;
    ir::IrValueId n   = fn.new_value(I64);              // param (regs[1])
    ir::IrValueId c2  = fn.new_value(I64);
    ir::IrValueId cnd = fn.new_value(ir::IrType::BOOL);
    ir::IrValueId c1  = fn.new_value(I64);
    ir::IrValueId nm1 = fn.new_value(I64);
    ir::IrValueId r   = fn.new_value(I64);
    ir::IrValueId res = fn.new_value(I64);
    fn.params = { n };

    ir::IrBlockId b0 = fn.new_block("entry");
    ir::IrBlockId b1 = fn.new_block("base");
    ir::IrBlockId b2 = fn.new_block("rec");

    fn.append(b0, konst(c2, 2));
    fn.append(b0, cmp(ir::IrOp::CMP_LT, cnd, n, c2));
    fn.append(b0, brc(cnd, b1, b2));
    fn.append(b1, ret1(n));                              // base: return n
    fn.append(b2, konst(c1, 1));
    fn.append(b2, bin(ir::IrOp::SUB, nm1, n, c1));       // n-1
    {
        ir::IrInstr c; c.op = ir::IrOp::CALL; c.type = I64; c.dst = r;
        c.func_name = "fact"; c.operands = { nm1 };      // SELF-call (func_name==fn.name)
        fn.append(b2, c);
    }
    fn.append(b2, bin(ir::IrOp::MUL, res, n, r));        // n * fact(n-1)
    fn.append(b2, ret1(res));

    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 5;
    CHECK(jit_vm(fn, px), "jit_vm ok (self-recursion)");
    CHECK(px.regs[0] == 120, "fact(5)==120 via self-call rel32");
    if (px.regs[0] != 120) std::printf("    regs[0]=%llu\n", (unsigned long long)px.regs[0]);
}

/* ---- Test self-tail-call (TCO): IrOp::TAILCALL -> reuso de frame -------- *
 * sum_tc(acc, n) = (n==0) ? acc : sum_tc(acc+n, n-1).  Con acc=0, n=100 ->
 * 5050.  El TAILCALL self emite (en el rewrite) epilogue + jmp a code+0 ->
 * reusa el frame (O(1) stack).  El RET de la base retorna al caller original. */
static void test_vm_tailcall_self() {
    std::printf("[vm] sum_tc(0,100) self-tail-call (TCO frame-reuse) -> regs[0]=5050\n");
    ir::IrFunction fn;
    fn.name = "sum_tc"; fn.ret_type = ir::IrType::I64;
    auto I64 = ir::IrType::I64;
    ir::IrValueId acc  = fn.new_value(I64);            // param0 (regs[1])
    ir::IrValueId n    = fn.new_value(I64);            // param1 (regs[2])
    ir::IrValueId zero = fn.new_value(I64);
    ir::IrValueId cnd  = fn.new_value(ir::IrType::BOOL);
    ir::IrValueId one  = fn.new_value(I64);
    ir::IrValueId acc2 = fn.new_value(I64);
    ir::IrValueId n2   = fn.new_value(I64);
    fn.params = { acc, n };

    ir::IrBlockId b0 = fn.new_block("entry");
    ir::IrBlockId b1 = fn.new_block("base");
    ir::IrBlockId b2 = fn.new_block("rec");

    fn.append(b0, konst(zero, 0));
    fn.append(b0, cmp(ir::IrOp::CMP_EQ, cnd, n, zero));
    fn.append(b0, brc(cnd, b1, b2));
    fn.append(b1, ret1(acc));                           // base: return acc
    fn.append(b2, konst(one, 1));
    fn.append(b2, bin(ir::IrOp::ADD, acc2, acc, n));    // acc + n
    fn.append(b2, bin(ir::IrOp::SUB, n2, n, one));      // n - 1
    {
        ir::IrInstr tc; tc.op = ir::IrOp::TAILCALL; tc.type = I64;
        tc.func_name = "sum_tc"; tc.operands = { acc2, n2 };  // SELF tail-call
        fn.append(b2, tc);
    }

    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 0; px.regs[2] = 100;
    CHECK(jit_vm(fn, px), "jit_vm ok (self-tail-call)");
    CHECK(px.regs[0] == 5050, "sum_tc(0,100)==5050 via TAILCALL frame-reuse");
    if (px.regs[0] != 5050) std::printf("    regs[0]=%llu\n", (unsigned long long)px.regs[0]);
}

/* ---- Test 6 (commit 6): GC root vivo a traves de un call --------------- *
 * gcf(this_gc, x) = g() + this.  `this` (GC, host_ptr) se usa DESPUES del
 * call -> vivo a traves -> el allocator lo SPILLEA a slot y emite un stackmap
 * que lo describe.  Verifica: (1) compila, (2) this spilled, (3) stackmap con
 * 1 slot HOSTPTR, (4) ejecuta (el spill/reload preserva el valor): 50+7=57. */
static void test_vm_gc_stackmap() {
    std::printf("[vm] GC root vivo a traves de call -> spilled + stackmap\n");
    ir::IrFunction fn;
    fn.name = "gcf"; fn.ret_type = ir::IrType::I64;
    auto I64 = ir::IrType::I64;
    ir::IrValueId thisp = fn.new_value(ir::IrType::PTR);
    ir::IrValueId x = fn.new_value(I64), r = fn.new_value(I64), sum = fn.new_value(I64);
    fn.values[thisp].is_gc_object = true;
    fn.values[thisp].is_host_ptr  = true;   // -> StackmapGcKind::HOSTPTR
    fn.params = { thisp, x };
    ir::IrBlockId bb = fn.new_block("e");
    {
        ir::IrInstr c; c.op = ir::IrOp::CALL; c.type = I64; c.dst = r;
        c.func_name = "g";  // sin args
        fn.append(bb, c);
    }
    fn.append(bb, bin(ir::IrOp::ADD, sum, r, thisp));  // this vivo a traves del call
    fn.append(bb, ret1(sum));

    CallResolver resolver = [](const std::string &n) -> uint64_t {
        if (n == "g") return reinterpret_cast<uint64_t>(reinterpret_cast<void *>(&g_stub));
        return 0;
    };

    MFunction mf;
    CHECK(vreg_select(fn, mf, AbiKind::VM, resolver), "vreg_select ok (gcf)");
    const TargetRegInfo &tri = target_x86_64_vm_abi();
    IntervalResult ivs = build_intervals(mf, tri);
    RegAlloc ra = linear_scan(ivs, tri);
    CHECK(ra.spilled(thisp), "this (GC root vivo a traves) spilled a slot");
    MFunction pf = rewrite_to_physical(mf, ra, tri, AbiKind::VM, &ivs);
    CHECK(pf.stackmaps.size() == 1, "1 stackmap (1 call)");
    if (pf.stackmaps.size() == 1) {
        CHECK(pf.stackmaps[0].slots.size() == 1, "stackmap describe 1 GC root");
        if (pf.stackmaps[0].slots.size() == 1)
            CHECK(pf.stackmaps[0].slots[0].gc_kind == StackmapGcKind::HOSTPTR,
                  "gc_kind == HOSTPTR");
    }
    X86Encoder enc; std::vector<uint8_t> bytes;
    CHECK(enc.encode(pf, bytes) != 0 && !bytes.empty(), "encode ok");
    CodeCache cc; uint8_t *code = cc.alloc(bytes.size(), 16);
    std::memcpy(code, bytes.data(), bytes.size()); cc.commit(code, bytes.size());
    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 7; px.regs[2] = 99;   // this=7, x=99 (no usado)
    reinterpret_cast<void (*)(void *)>(code)(&px);
    asm volatile("" : : "r"(&cc) : "memory");
    CHECK(px.regs[0] == 57, "gcf == g()(50) + this(7) == 57");
    if (px.regs[0] != 57) std::printf("    regs[0]=%llu\n", (unsigned long long)px.regs[0]);
}

/* ---- Test 7 (commit 7): LOAD/STORE sobre memoria host ----------------- *
 * ls(ptr) carga buf[0], le suma 100, lo guarda de vuelta y lo retorna.
 * buf[0]=5 -> buf[0]=105, ret 105. */
static void test_vm_load_store() {
    std::printf("[vm] load/store host: buf[0]=5 -> +100 -> 105\n");
    ir::IrFunction fn;
    fn.name = "ls"; fn.ret_type = ir::IrType::I64;
    auto I64 = ir::IrType::I64;
    ir::IrValueId ptr = fn.new_value(ir::IrType::PTR);
    ir::IrValueId v = fn.new_value(I64), c = fn.new_value(I64), w = fn.new_value(I64);
    fn.values[ptr].is_host_ptr = true;
    fn.params = { ptr };
    ir::IrBlockId bb = fn.new_block("e");
    { ir::IrInstr i; i.op = ir::IrOp::LOAD; i.type = I64; i.dst = v;
      i.operands = { ptr }; fn.append(bb, i); }
    fn.append(bb, konst(c, 100));
    fn.append(bb, bin(ir::IrOp::ADD, w, v, c));
    { ir::IrInstr i; i.op = ir::IrOp::STORE; i.type = I64;
      i.operands = { w, ptr }; fn.append(bb, i); }   // [0]=val, [1]=ptr
    fn.append(bb, ret1(w));

    uint64_t buf[2] = { 5, 0 };
    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = reinterpret_cast<uint64_t>(&buf[0]);
    CHECK(jit_vm(fn, px), "jit_vm ok (load/store)");
    CHECK(buf[0] == 105, "buf[0] == 105 (STORE escribio)");
    CHECK(px.regs[0] == 105, "ret == 105 (LOAD leyo)");
    if (buf[0] != 105) std::printf("    buf[0]=%llu\n", (unsigned long long)buf[0]);
}

/* ---- Test 8 (commit 8): ALLOCA host + STORE/LOAD sobre el frame -------- *
 * f(x): p = alloca(8); [p] = x; v = [p]; return v + 100.  f(5) -> 105. */
static void test_vm_alloca() {
    std::printf("[vm] alloca host: [p]=x, load p, +100.  f(5) -> 105\n");
    ir::IrFunction fn;
    fn.name = "al"; fn.ret_type = ir::IrType::I64;
    auto I64 = ir::IrType::I64;
    ir::IrValueId x = fn.new_value(I64);
    ir::IrValueId p = fn.new_value(ir::IrType::PTR);
    ir::IrValueId v = fn.new_value(I64), c = fn.new_value(I64), r = fn.new_value(I64);
    fn.values[p].is_host_ptr = true;   // alloca host -> host_ptr
    fn.params = { x };
    ir::IrBlockId bb = fn.new_block("e");
    { ir::IrInstr i; i.op = ir::IrOp::ALLOCA; i.type = ir::IrType::I8; i.dst = p;
      i.imm = 8; i.set_host_alloca(true); fn.append(bb, i); }
    { ir::IrInstr i; i.op = ir::IrOp::STORE; i.type = I64;
      i.operands = { x, p }; fn.append(bb, i); }          // [p] = x
    { ir::IrInstr i; i.op = ir::IrOp::LOAD; i.type = I64; i.dst = v;
      i.operands = { p }; fn.append(bb, i); }             // v = [p]
    fn.append(bb, konst(c, 100));
    fn.append(bb, bin(ir::IrOp::ADD, r, v, c));
    fn.append(bb, ret1(r));

    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 5;
    CHECK(jit_vm(fn, px), "jit_vm ok (alloca)");
    CHECK(px.regs[0] == 105, "f(5) == 105 (alloca store/load)");
    if (px.regs[0] != 105) std::printf("    regs[0]=%llu\n", (unsigned long long)px.regs[0]);
}

/* ---- Test: RAW_FREE sobre ALLOCA host-stack es NO-OP (no crash) ------- *
 * Bug 59_arraylist: free() hacia que push() cayera al slot regalloc buggy.
 * En vregs un RAW_FREE sobre un host-alloca debe ser no-op (lo libera el
 * epilogue), sin llamar a vrt_raw_free (que crashearia). */
static void test_vm_raw_free_host_alloca() {
    std::printf("[vm] RAW_FREE sobre host-alloca = NO-OP (f(5)->105, sin crash)\n");
    ir::IrFunction fn;
    fn.name = "rf"; fn.ret_type = ir::IrType::I64;
    auto I64 = ir::IrType::I64;
    ir::IrValueId x = fn.new_value(I64);
    ir::IrValueId p = fn.new_value(ir::IrType::PTR);
    ir::IrValueId v = fn.new_value(I64), c = fn.new_value(I64), r = fn.new_value(I64);
    fn.values[p].is_host_ptr = true;
    fn.params = { x };
    ir::IrBlockId bb = fn.new_block("e");
    { ir::IrInstr i; i.op = ir::IrOp::ALLOCA; i.type = ir::IrType::I8; i.dst = p;
      i.imm = 8; i.set_host_alloca(true); fn.append(bb, i); }
    { ir::IrInstr i; i.op = ir::IrOp::STORE; i.type = I64;
      i.operands = { x, p }; fn.append(bb, i); }            // [p] = x
    { ir::IrInstr i; i.op = ir::IrOp::LOAD; i.type = I64; i.dst = v;
      i.operands = { p }; fn.append(bb, i); }               // v = [p]
    { ir::IrInstr i; i.op = ir::IrOp::RAW_FREE; i.type = ir::IrType::VOID;
      i.operands = { p }; fn.append(bb, i); }               // free(p) -> no-op
    fn.append(bb, konst(c, 100));
    fn.append(bb, bin(ir::IrOp::ADD, r, v, c));
    fn.append(bb, ret1(r));

    Proxy px; std::memset(&px, 0, sizeof(px)); px.regs[1] = 5;
    /* ent.raw_free = 0: el host-alloca NO debe necesitar la entry (es no-op). */
    CHECK(jit_vm(fn, px), "jit_vm ok (raw_free host-alloca, sin entry)");
    CHECK(px.regs[0] == 105, "f(5)==105 (free no-op no corrompe)");
}

/* ---- Test 9 (commit 9): TRUNC i32 (sign-ext) y u32 (zero-ext) --------- */
static void test_vm_trunc() {
    std::printf("[vm] trunc.i32(0xFFFFFFFF)=-1 (sign), u32(-1)=0xFFFFFFFF (zero)\n");
    {   /* trunc i64 -> i32 (signed): 0xFFFFFFFF -> -1 sign-extended. */
        ir::IrFunction fn; fn.name = "ti"; fn.ret_type = ir::IrType::I64;
        auto x = fn.new_value(ir::IrType::I64);
        auto t = fn.new_value(ir::IrType::I32);
        fn.params = { x };
        auto bb = fn.new_block("e");
        { ir::IrInstr i; i.op = ir::IrOp::TRUNC; i.type = ir::IrType::I32;
          i.dst = t; i.operands = { x }; fn.append(bb, i); }
        fn.append(bb, ret1(t));
        Proxy px; std::memset(&px, 0, sizeof(px)); px.regs[1] = 0xFFFFFFFFull;
        CHECK(jit_vm(fn, px), "jit_vm ok (trunc i32)");
        CHECK(static_cast<int64_t>(px.regs[0]) == -1,
              "trunc.i32(0xFFFFFFFF) == -1 (sign-ext)");
        if (static_cast<int64_t>(px.regs[0]) != -1)
            std::printf("    regs[0]=0x%llx\n", (unsigned long long)px.regs[0]);
    }
    {   /* trunc i64 -> u32 (unsigned): -1 -> 0xFFFFFFFF zero-extended. */
        ir::IrFunction fn; fn.name = "tu"; fn.ret_type = ir::IrType::I64;
        auto x = fn.new_value(ir::IrType::I64);
        auto t = fn.new_value(ir::IrType::U32);
        fn.params = { x };
        auto bb = fn.new_block("e");
        { ir::IrInstr i; i.op = ir::IrOp::TRUNC; i.type = ir::IrType::U32;
          i.dst = t; i.operands = { x }; fn.append(bb, i); }
        fn.append(bb, ret1(t));
        Proxy px; std::memset(&px, 0, sizeof(px));
        px.regs[1] = 0xFFFFFFFFFFFFFFFFull;
        CHECK(jit_vm(fn, px), "jit_vm ok (trunc u32)");
        CHECK(px.regs[0] == 0xFFFFFFFFull,
              "trunc.u32(-1) == 0xFFFFFFFF (zero-ext)");
        if (px.regs[0] != 0xFFFFFFFFull)
            std::printf("    regs[0]=0x%llx\n", (unsigned long long)px.regs[0]);
    }
}

/** @brief Funcion nativa de prueba (convencion C: args en arg_regs). */
extern "C" uint64_t vm_native_add(uint64_t a, uint64_t b) { return a + b; }

/* ---- Test 10 (commit 10): CALLN directo a funcion nativa --------------- *
 * caller(a,b) = calln "add"(a,b).  CALL DIRECTO (no via runtime vrt_calln).
 * a=10, b=32 -> 42. */
static void test_vm_calln() {
    std::printf("[vm] CALLN directo: add(10,32) -> 42 (sin runtime)\n");
    ir::IrFunction fn;
    fn.name = "cn"; fn.ret_type = ir::IrType::I64;
    auto I64 = ir::IrType::I64;
    ir::IrValueId a = fn.new_value(I64), b = fn.new_value(I64), r = fn.new_value(I64);
    fn.params = { a, b };
    ir::IrBlockId bb = fn.new_block("e");
    {
        ir::IrInstr c; c.op = ir::IrOp::CALLN; c.type = I64; c.dst = r;
        c.func_name = "add"; c.operands = { a, b };
        fn.append(bb, c);
    }
    fn.append(bb, ret1(r));
    CallResolver rn = [](const std::string &n) -> uint64_t {
        return n == "add" ? reinterpret_cast<uint64_t>(
            reinterpret_cast<void *>(&vm_native_add)) : 0;
    };
    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 10; px.regs[2] = 32;
    CHECK(jit_vm(fn, px, {}, 0, rn), "jit_vm ok (calln)");
    CHECK(px.regs[0] == 42, "calln add(10,32)==42 (CALL directo)");
    if (px.regs[0] != 42) std::printf("    regs[0]=%llu\n", (unsigned long long)px.regs[0]);
}

/* ---- Test 11 (commit 10): vmath_abs INLINE (sar/xor/sub, sin runtime) ---- */
static void test_vm_abs() {
    std::printf("[vm] vmath_abs INLINE: abs(-5)=5, abs(7)=7 (sin CALL)\n");
    ir::IrFunction fn;
    fn.name = "ab"; fn.ret_type = ir::IrType::I64;
    auto I64 = ir::IrType::I64;
    ir::IrValueId x = fn.new_value(I64), r = fn.new_value(I64);
    fn.params = { x };
    ir::IrBlockId bb = fn.new_block("e");
    {
        ir::IrInstr c; c.op = ir::IrOp::CALLN; c.type = I64; c.dst = r;
        c.func_name = "stdlib/native/math/vesta_math:vmath_abs"; c.operands = { x };
        fn.append(bb, c);
    }
    fn.append(bb, ret1(r));
    /* Sin resolve_native: si NO se inline-ara, CALLN fallaria (resolver={}).
     * Que compile y de el resultado correcto prueba que se inline. */
    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = static_cast<uint64_t>(-5);
    CHECK(jit_vm(fn, px), "jit_vm ok (abs inline, sin resolver native)");
    CHECK(static_cast<int64_t>(px.regs[0]) == 5, "abs(-5)==5");
    Proxy px2; std::memset(&px2, 0, sizeof(px2));
    px2.regs[1] = 7;
    jit_vm(fn, px2);
    CHECK(px2.regs[0] == 7, "abs(7)==7");
}

/* ---- Test: inline de GC_DEREF_HOST contra una HandleTable falsa ------- *
 * Phase D.7 (principio "JIT inline > runtime"): verifica que el codegen
 * inline de GC_DEREF_HOST computa EXACTAMENTE lo que @c GcHeap::deref:
 *   - handle local vivo            -> data_[h].addr + sizeof(GcHeader)=8
 *   - handle fuera de rango        -> 0
 *   - handle con live=false        -> 0
 * El path shared (bit31) usa el fallback CALL y requiere runtime; aqui
 * todos los handles tienen bit31 limpio (local), asi que se ejercita el
 * camino inline puro. */

/* Mirror del layout que el inline asume.  El layout REAL esta fijado por
 * static_assert en gc_heap.h; estos re-aseguran el del proxy del test. */
struct FakeEntry { uint8_t *addr; bool live; };
struct FakeTable { FakeEntry *data_; uint32_t count_; uint32_t cap_; };
static_assert(sizeof(FakeEntry) == 16, "FakeEntry debe medir 16 (stride del inline)");
static_assert(offsetof(FakeEntry, addr) == 0, "addr@0");
static_assert(offsetof(FakeEntry, live) == 8, "live@8");
static_assert(offsetof(FakeTable, data_) == 0, "data_@0");
static_assert(offsetof(FakeTable, count_) == 8, "count_@8");

/* Proxy con @c jit_handle_table en su offset ABI (1304), ademas de
 * safepoint_flag@0 y regs@96. */
struct ProcGc {
    uint8_t  safepoint_flag;
    uint8_t  _pad1[VESTA_PROC_REGISTERS_OFFSET - 1];
    uint64_t regs[VESTA_PROC_REGISTER_COUNT];
    uint8_t  _pad2[VESTA_PROC_JIT_HANDLE_TABLE_OFFSET
                   - VESTA_PROC_REGISTERS_OFFSET
                   - sizeof(uint64_t) * VESTA_PROC_REGISTER_COUNT];
    void    *jit_handle_table;
};
static_assert(offsetof(ProcGc, regs) == VESTA_PROC_REGISTERS_OFFSET, "regs@96");
static_assert(offsetof(ProcGc, jit_handle_table)
              == VESTA_PROC_JIT_HANDLE_TABLE_OFFSET,
              "jit_handle_table en su offset ABI");

/** @brief Compila @c deref_test(h) por vregs y la ejecuta con RBX=&px. */
static uint64_t run_gc_deref(ProcGc &px, uint32_t handle) {
    ir::IrFunction fn;
    fn.name = "deref_test"; fn.ret_type = ir::IrType::I64;
    ir::IrValueId h = fn.new_value(ir::IrType::I64);
    ir::IrValueId r = fn.new_value(ir::IrType::I64);
    fn.values[r].is_host_ptr  = true;   // resultado: host_ptr a objeto GC
    fn.values[r].is_gc_object = true;   // -> spill+stackmap si cruza el call
    fn.params = { h };
    ir::IrBlockId bb = fn.new_block("e");
    { ir::IrInstr i; i.op = ir::IrOp::GC_DEREF_HOST; i.type = ir::IrType::I64;
      i.dst = r; i.operands = { h }; fn.append(bb, i); }
    fn.append(bb, ret1(r));

    MFunction mf;
    VregEntries ent;
    ent.gc_deref = 0x1000;  // !=0 (path shared nunca se ejecuta aqui)
    if (!vreg_select(fn, mf, AbiKind::VM, {}, ent, {})) return UINT64_MAX;
    const TargetRegInfo &tri = target_x86_64_vm_abi();
    RegAlloc ra = linear_scan(build_intervals(mf, tri), tri);
    MFunction pf = rewrite_to_physical(mf, ra, tri, AbiKind::VM);
    X86Encoder enc; std::vector<uint8_t> bytes;
    if (enc.encode(pf, bytes) == 0 || bytes.empty()) return UINT64_MAX;
    CodeCache cc;
    uint8_t *code = cc.alloc(bytes.size(), 16);
    if (!code) return UINT64_MAX;
    std::memcpy(code, bytes.data(), bytes.size());
    cc.commit(code, bytes.size());
    px.regs[1] = handle;
    px.regs[0] = 0xDEAD;   // centinela: detecta que el codigo escribe regs[0]
    reinterpret_cast<void (*)(void *)>(code)(&px);
    asm volatile("" : : "r"(&cc) : "memory");
    return px.regs[0];
}

static void test_vm_gc_deref_inline() {
    std::printf("[vm] GC_DEREF_HOST INLINE: lookup directo en HandleTable\n");
    static uint8_t obj_buf[64];          // GcHeader + payload del objeto idx2
    FakeEntry entries[4];
    std::memset(entries, 0, sizeof(entries));
    entries[2].addr = obj_buf; entries[2].live = true;   // vivo
    entries[1].addr = nullptr; entries[1].live = false;  // muerto
    FakeTable table; table.data_ = entries; table.count_ = 4; table.cap_ = 4;

    ProcGc px; std::memset(&px, 0, sizeof(px));
    px.jit_handle_table = &table;

    const uint64_t expect = reinterpret_cast<uint64_t>(obj_buf) + 8;  // + sizeof(GcHeader)
    CHECK(run_gc_deref(px, 2) == expect, "deref(2 vivo) == addr + 8");
    CHECK(run_gc_deref(px, 1) == 0, "deref(1 muerto, live=false) == 0");
    CHECK(run_gc_deref(px, 5) == 0, "deref(5 fuera de rango) == 0");
    CHECK(run_gc_deref(px, 0) == 0, "deref(0 slot vacio) == 0");
}

/* ---- Test: inline de math intrinsics (vmath_*) en vregs --------------- *
 * Sin resolve_native: si NO se inline-aran, el CALLN fallaria (resolver={})
 * -> jit_vm devuelve false.  Que compile y de el resultado correcto prueba
 * que se inline sin tocar el runtime. */
static uint64_t run_calln1(const std::string &func, uint64_t a0) {
    ir::IrFunction fn; fn.name = "m1"; fn.ret_type = ir::IrType::I64;
    const auto I64 = ir::IrType::I64;
    ir::IrValueId x = fn.new_value(I64), r = fn.new_value(I64);
    fn.params = { x };
    ir::IrBlockId bb = fn.new_block("e");
    { ir::IrInstr c; c.op = ir::IrOp::CALLN; c.type = I64; c.dst = r;
      c.func_name = func; c.operands = { x }; fn.append(bb, c); }
    fn.append(bb, ret1(r));
    Proxy px; std::memset(&px, 0, sizeof(px)); px.regs[1] = a0;
    if (!jit_vm(fn, px)) return UINT64_MAX;
    return px.regs[0];
}
static uint64_t run_calln2(const std::string &func, uint64_t a0, uint64_t a1) {
    ir::IrFunction fn; fn.name = "m2"; fn.ret_type = ir::IrType::I64;
    const auto I64 = ir::IrType::I64;
    ir::IrValueId x = fn.new_value(I64), y = fn.new_value(I64), r = fn.new_value(I64);
    fn.params = { x, y };
    ir::IrBlockId bb = fn.new_block("e");
    { ir::IrInstr c; c.op = ir::IrOp::CALLN; c.type = I64; c.dst = r;
      c.func_name = func; c.operands = { x, y }; fn.append(bb, c); }
    fn.append(bb, ret1(r));
    Proxy px; std::memset(&px, 0, sizeof(px)); px.regs[1] = a0; px.regs[2] = a1;
    if (!jit_vm(fn, px)) return UINT64_MAX;
    return px.regs[0];
}
/* rotl/rotr con count CONSTANTE: el count es un CONST del IR (v_is_const). */
static uint64_t run_rot_const(const std::string &func, uint64_t v, int64_t n) {
    ir::IrFunction fn; fn.name = "mr"; fn.ret_type = ir::IrType::I64;
    const auto I64 = ir::IrType::I64;
    ir::IrValueId x = fn.new_value(I64), k = fn.new_value(I64), r = fn.new_value(I64);
    fn.params = { x };
    ir::IrBlockId bb = fn.new_block("e");
    fn.append(bb, konst(k, n));
    { ir::IrInstr c; c.op = ir::IrOp::CALLN; c.type = I64; c.dst = r;
      c.func_name = func; c.operands = { x, k }; fn.append(bb, c); }
    fn.append(bb, ret1(r));
    Proxy px; std::memset(&px, 0, sizeof(px)); px.regs[1] = v;
    if (!jit_vm(fn, px)) return UINT64_MAX;
    return px.regs[0];
}

/* Variantes que construyen el IrOp DEDICADO (el frontend baja imin/imax/
 * ilog2/clz/... a estos, no a CALLN). */
static uint64_t run_irop1(ir::IrOp op, uint64_t a0) {
    ir::IrFunction fn; fn.name = "o1"; fn.ret_type = ir::IrType::I64;
    const auto I64 = ir::IrType::I64;
    ir::IrValueId x = fn.new_value(I64), r = fn.new_value(I64);
    fn.params = { x };
    ir::IrBlockId bb = fn.new_block("e");
    { ir::IrInstr c; c.op = op; c.type = I64; c.dst = r; c.operands = { x };
      fn.append(bb, c); }
    fn.append(bb, ret1(r));
    Proxy px; std::memset(&px, 0, sizeof(px)); px.regs[1] = a0;
    if (!jit_vm(fn, px)) return UINT64_MAX;
    return px.regs[0];
}
static uint64_t run_irop2(ir::IrOp op, uint64_t a0, uint64_t a1) {
    ir::IrFunction fn; fn.name = "o2"; fn.ret_type = ir::IrType::I64;
    const auto I64 = ir::IrType::I64;
    ir::IrValueId x = fn.new_value(I64), y = fn.new_value(I64), r = fn.new_value(I64);
    fn.params = { x, y };
    ir::IrBlockId bb = fn.new_block("e");
    { ir::IrInstr c; c.op = op; c.type = I64; c.dst = r; c.operands = { x, y };
      fn.append(bb, c); }
    fn.append(bb, ret1(r));
    Proxy px; std::memset(&px, 0, sizeof(px)); px.regs[1] = a0; px.regs[2] = a1;
    if (!jit_vm(fn, px)) return UINT64_MAX;
    return px.regs[0];
}
static uint64_t run_irop_rot(ir::IrOp op, uint64_t v, int64_t n) {
    ir::IrFunction fn; fn.name = "or"; fn.ret_type = ir::IrType::I64;
    const auto I64 = ir::IrType::I64;
    ir::IrValueId x = fn.new_value(I64), k = fn.new_value(I64), r = fn.new_value(I64);
    fn.params = { x };
    ir::IrBlockId bb = fn.new_block("e");
    fn.append(bb, konst(k, n));
    { ir::IrInstr c; c.op = op; c.type = I64; c.dst = r; c.operands = { x, k };
      fn.append(bb, c); }
    fn.append(bb, ret1(r));
    Proxy px; std::memset(&px, 0, sizeof(px)); px.regs[1] = v;
    if (!jit_vm(fn, px)) return UINT64_MAX;
    return px.regs[0];
}

static void test_vm_math_irops() {
    std::printf("[vm] math IrOps INLINE: IABS/IMIN/IMAX/ILOG2/CLZ/CTZ/POPCNT/BSWAP/ROT\n");
    using O = ir::IrOp;
    CHECK(static_cast<int64_t>(run_irop1(O::IABS, static_cast<uint64_t>(-5))) == 5, "iabs(-5)==5");
    CHECK(run_irop1(O::IABS, 7) == 7, "iabs(7)==7");
    CHECK(static_cast<int64_t>(run_irop2(O::IMIN, static_cast<uint64_t>(-3), 5)) == -3, "imin(-3,5)==-3");
    CHECK(static_cast<int64_t>(run_irop2(O::IMAX, static_cast<uint64_t>(-3), 5)) == 5, "imax(-3,5)==5");
    CHECK(run_irop2(O::IMINU, 10, 0xFFFFFFFFFFFFFFFFull) == 10, "iminu(10,UMAX)==10");
    CHECK(run_irop2(O::IMAXU, 10, 0xFFFFFFFFFFFFFFFFull) == 0xFFFFFFFFFFFFFFFFull, "imaxu(10,UMAX)==UMAX");
    CHECK(run_irop1(O::ILOG2, 256) == 8, "ilog2(256)==8");
    CHECK(run_irop1(O::CLZ, 1) == 63, "clz(1)==63");
    CHECK(run_irop1(O::CTZ, 8) == 3, "ctz(8)==3");
    CHECK(run_irop1(O::POPCNT, 7) == 3, "popcnt(7)==3");
    CHECK(run_irop1(O::BYTESWAP, 0x0102030405060708ull) == 0x0807060504030201ull, "bswap==reversed");
    CHECK(run_irop_rot(O::ROTL, 1, 4) == 16, "rotl(1,4)==16");
    CHECK(run_irop_rot(O::ROTR, 16, 4) == 1, "rotr(16,4)==1");
}

static void test_vm_math_inline() {
    std::printf("[vm] math intrinsics INLINE: ilog2/min/max/minu/maxu/rotl/rotr\n");
    const std::string P = "stdlib/native/math/vesta_math:";
    /* ilog2(n) = 63 - lzcnt(n). */
    CHECK(run_calln1(P + "vmath_ilog2", 256) == 8, "ilog2(256)==8");
    CHECK(run_calln1(P + "vmath_ilog2", 1) == 0, "ilog2(1)==0");
    CHECK(run_calln1(P + "vmath_ilog2", 0xFFFFFFFFFFFFFFFFull) == 63, "ilog2(UMAX)==63");
    /* min/max SIGNED. */
    CHECK(static_cast<int64_t>(run_calln2(P + "vmath_min",
        static_cast<uint64_t>(-3), 5)) == -3, "min(-3,5)==-3");
    CHECK(static_cast<int64_t>(run_calln2(P + "vmath_max",
        static_cast<uint64_t>(-3), 5)) == 5, "max(-3,5)==5");
    /* minu/maxu UNSIGNED. */
    CHECK(run_calln2(P + "vmath_minu", 10, 0xFFFFFFFFFFFFFFFFull) == 10,
          "minu(10,UMAX)==10");
    CHECK(run_calln2(P + "vmath_maxu", 10, 0xFFFFFFFFFFFFFFFFull)
          == 0xFFFFFFFFFFFFFFFFull, "maxu(10,UMAX)==UMAX");
    /* rotl/rotr con count constante. */
    CHECK(run_rot_const(P + "vmath_rotl", 1, 4) == 16, "rotl(1,4)==16");
    CHECK(run_rot_const(P + "vmath_rotr", 16, 4) == 1, "rotr(16,4)==1");
}

/* ---- Test DIVMOD: div + mod enteros via pseudo DIVMOD_V (gate ON) ----- */
static void test_vm_divmod() {
    std::printf("[vm] divmod: regs[1]=17, regs[2]=5 -> div=3, mod=2\n");
    /* DIV: q = a / b */
    {
        ir::IrFunction fn; fn.name = "dv"; fn.ret_type = ir::IrType::I64;
        auto a = fn.new_value(ir::IrType::I64);
        auto b = fn.new_value(ir::IrType::I64);
        auto q = fn.new_value(ir::IrType::I64);
        fn.params = { a, b };
        auto bb = fn.new_block("e");
        fn.append(bb, bin(ir::IrOp::DIV, q, a, b));
        fn.append(bb, ret1(q));
        Proxy px; std::memset(&px, 0, sizeof(px));
        px.regs[1] = 17; px.regs[2] = 5;
        CHECK(jit_vm(fn, px), "jit_vm ok (div)");
        CHECK(px.regs[0] == 3, "17/5==3");
        if (px.regs[0] != 3) std::printf("    div regs[0]=%llu\n", (unsigned long long)px.regs[0]);
    }
    /* MOD: r = a % b */
    {
        ir::IrFunction fn; fn.name = "md"; fn.ret_type = ir::IrType::I64;
        auto a = fn.new_value(ir::IrType::I64);
        auto b = fn.new_value(ir::IrType::I64);
        auto r = fn.new_value(ir::IrType::I64);
        fn.params = { a, b };
        auto bb = fn.new_block("e");
        fn.append(bb, bin(ir::IrOp::MOD, r, a, b));
        fn.append(bb, ret1(r));
        Proxy px; std::memset(&px, 0, sizeof(px));
        px.regs[1] = 17; px.regs[2] = 5;
        CHECK(jit_vm(fn, px), "jit_vm ok (mod)");
        CHECK(px.regs[0] == 2, "17%5==2");
        if (px.regs[0] != 2) std::printf("    mod regs[0]=%llu\n", (unsigned long long)px.regs[0]);
    }
    /* COMBINADO: f(a,b) = (a/b) + (a%b).  Dos DIVMOD_V; a,b vivos a traves
     * de ambos (call-positions), q vivo a traves del 2o.  Reproduce el patron
     * de la funcion divmod del bench que crasheaba. */
    {
        ir::IrFunction fn; fn.name = "dm2"; fn.ret_type = ir::IrType::I64;
        auto a = fn.new_value(ir::IrType::I64);
        auto b = fn.new_value(ir::IrType::I64);
        auto q = fn.new_value(ir::IrType::I64);
        auto r = fn.new_value(ir::IrType::I64);
        auto s = fn.new_value(ir::IrType::I64);
        fn.params = { a, b };
        auto bb = fn.new_block("e");
        fn.append(bb, bin(ir::IrOp::DIV, q, a, b));
        fn.append(bb, bin(ir::IrOp::MOD, r, a, b));
        fn.append(bb, bin(ir::IrOp::ADD, s, q, r));
        fn.append(bb, ret1(s));
        Proxy px; std::memset(&px, 0, sizeof(px));
        px.regs[1] = 17; px.regs[2] = 5;   // 17/5=3, 17%5=2, +=5
        CHECK(jit_vm(fn, px), "jit_vm ok (div+mod combinado)");
        CHECK(px.regs[0] == 5, "(17/5)+(17%5)==5");
        if (px.regs[0] != 5) std::printf("    dm2 regs[0]=%llu\n", (unsigned long long)px.regs[0]);
    }
    /* COMBINADO i32: replica EXACTA del IR real del frontend que crasheaba:
     *   f(a:i32,b:i32) = (a/b) + (a%b).  Mismo patron pero tipo i32. */
    {
        ir::IrFunction fn; fn.name = "dm2i32"; fn.ret_type = ir::IrType::I32;
        auto a = fn.new_value(ir::IrType::I32);
        auto b = fn.new_value(ir::IrType::I32);
        auto q = fn.new_value(ir::IrType::I32);
        auto r = fn.new_value(ir::IrType::I32);
        auto s = fn.new_value(ir::IrType::I32);
        fn.params = { a, b };
        auto bb = fn.new_block("e");
        ir::IrInstr di; di.op = ir::IrOp::DIV; di.type = ir::IrType::I32;
        di.dst = q; di.operands = { a, b }; fn.append(bb, di);
        ir::IrInstr mi; mi.op = ir::IrOp::MOD; mi.type = ir::IrType::I32;
        mi.dst = r; mi.operands = { a, b }; fn.append(bb, mi);
        ir::IrInstr ai; ai.op = ir::IrOp::ADD; ai.type = ir::IrType::I32;
        ai.dst = s; ai.operands = { q, r }; fn.append(bb, ai);
        ir::IrInstr ri; ri.op = ir::IrOp::RET; ri.type = ir::IrType::I32;
        ri.operands = { s }; fn.append(bb, ri);
        Proxy px; std::memset(&px, 0, sizeof(px));
        px.regs[1] = 17; px.regs[2] = 5;
        CHECK(jit_vm(fn, px), "jit_vm ok (i32 div+mod)");
        CHECK((px.regs[0] & 0xFFFFFFFFu) == 5, "i32 (17/5)+(17%5)==5");
        if ((px.regs[0] & 0xFFFFFFFFu) != 5) std::printf("    dm2i32 regs[0]=%llu\n", (unsigned long long)px.regs[0]);
    }
    /* DIV con dividendo negativo: -17 / 5 == -3 (trunc hacia cero). */
    {
        ir::IrFunction fn; fn.name = "dvn"; fn.ret_type = ir::IrType::I64;
        auto a = fn.new_value(ir::IrType::I64);
        auto b = fn.new_value(ir::IrType::I64);
        auto q = fn.new_value(ir::IrType::I64);
        fn.params = { a, b };
        auto bb = fn.new_block("e");
        fn.append(bb, bin(ir::IrOp::DIV, q, a, b));
        fn.append(bb, ret1(q));
        Proxy px; std::memset(&px, 0, sizeof(px));
        px.regs[1] = static_cast<uint64_t>(-17); px.regs[2] = 5;
        CHECK(jit_vm(fn, px), "jit_vm ok (div neg)");
        CHECK(static_cast<int64_t>(px.regs[0]) == -3, "-17/5==-3");
        if (static_cast<int64_t>(px.regs[0]) != -3) std::printf("    divneg regs[0]=%lld\n", (long long)px.regs[0]);
    }
}

/* ---- Regresion IMUL+spill: 3 div/mod en el body de un loop ------------
 * acc=0; for(i=1;i<N;i++){ a=i*7; b=(i%9)+1; acc += (a/b)+(a%b); }  ret acc.
 * Los 3 DIVMOD_V (call-positions) suben la presion -> el regalloc spillea
 * la constante 7 del `i*7` -> el rewrite generaba IMUL reg,[mem] -> el
 * encoder caia al 0xCC (INT3) -> SIGTRAP.  Este test lo reproduce y guarda
 * contra la regresion (el fix carga el operando spilled a R11 antes del
 * imul).  Debe completar y dar la suma correcta. */
static void test_vm_divmod_loop() {
    std::printf("[vm] divmod-loop (regresion IMUL+spill): 3 div/mod en loop\n");
    auto I64 = ir::IrType::I64;
    ir::IrFunction fn; fn.name = "dml"; fn.ret_type = I64;
    ir::IrValueId nparam = fn.new_value(I64);
    fn.params = { nparam };
    ir::IrValueId acc0 = fn.new_value(I64), i0 = fn.new_value(I64);
    ir::IrValueId k7 = fn.new_value(I64), k9 = fn.new_value(I64), k1 = fn.new_value(I64);
    ir::IrValueId i_phi = fn.new_value(I64), acc_phi = fn.new_value(I64);
    ir::IrValueId cond = fn.new_value(ir::IrType::BOOL);
    ir::IrValueId a = fn.new_value(I64), m9 = fn.new_value(I64), b = fn.new_value(I64);
    ir::IrValueId q = fn.new_value(I64), r = fn.new_value(I64), fr = fn.new_value(I64);
    ir::IrValueId acc_n = fn.new_value(I64), i_n = fn.new_value(I64);

    ir::IrBlockId b0 = fn.new_block("entry");
    ir::IrBlockId b1 = fn.new_block("header");
    ir::IrBlockId b2 = fn.new_block("body");
    ir::IrBlockId b3 = fn.new_block("exit");

    fn.append(b0, konst(acc0, 0));
    fn.append(b0, konst(i0, 1));
    fn.append(b0, konst(k7, 7));
    fn.append(b0, konst(k9, 9));
    fn.append(b0, konst(k1, 1));
    fn.append(b0, br(b1));

    fn.append(b1, phi2(i_phi, i0, b0, i_n, b2));
    fn.append(b1, phi2(acc_phi, acc0, b0, acc_n, b2));
    fn.append(b1, cmp(ir::IrOp::CMP_LT, cond, i_phi, nparam));
    fn.append(b1, brc(cond, b2, b3));

    auto dm = [&](ir::IrOp op, ir::IrValueId d, ir::IrValueId x, ir::IrValueId y) {
        ir::IrInstr i; i.op = op; i.type = I64; i.dst = d; i.operands = { x, y };
        return i;
    };
    fn.append(b2, bin(ir::IrOp::MUL, a, i_phi, k7));   // a = i*7  (imul + spill)
    fn.append(b2, dm(ir::IrOp::MOD, m9, i_phi, k9));   // m9 = i%9
    fn.append(b2, bin(ir::IrOp::ADD, b, m9, k1));      // b = m9+1
    fn.append(b2, dm(ir::IrOp::DIV, q, a, b));         // q = a/b
    fn.append(b2, dm(ir::IrOp::MOD, r, a, b));         // r = a%b
    fn.append(b2, bin(ir::IrOp::ADD, fr, q, r));       // fr = q+r
    fn.append(b2, bin(ir::IrOp::ADD, acc_n, acc_phi, fr));
    fn.append(b2, bin(ir::IrOp::ADD, i_n, i_phi, k1));
    fn.append(b2, br(b1));
    fn.append(b3, ret1(acc_phi));

    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 1000;
    int64_t expect = 0;
    for (int64_t i = 1; i < 1000; ++i) {
        int64_t aa = i * 7, bb = (i % 9) + 1;
        expect += (aa / bb) + (aa % bb);
    }
    CHECK(jit_vm(fn, px), "jit_vm ok (divmod-loop)");
    CHECK((int64_t)px.regs[0] == expect, "divmod-loop suma correcta (sin INT3)");
    if ((int64_t)px.regs[0] != expect)
        std::printf("    dml regs[0]=%lld (esperado %lld)\n",
                    (long long)px.regs[0], (long long)expect);
}

/* ---- Test: STR_LIT_ADDR resuelve la direccion via resolve_symbol --------
 * Fn de 1 bloque: v0 = STR_LIT_ADDR(imm=5); ret v0.  El resolve_symbol mock
 * mapea "code.s_5" -> una direccion conocida; verificamos que el codigo vreg
 * pone ESA direccion en regs[0].  Valida (a) el plumbing del resolver al case
 * nuevo, (b) el codegen `mov dst, imm64(addr)`, (c) que sin resolver el vreg
 * RECHAZA (fallback), no compila basura. */
static void test_vm_str_lit_addr() {
    std::printf("[vm] str_lit_addr: code.s_5 -> direccion resuelta en regs[0]\n");
    const uint64_t kAddr = 0xCAFEBABE12345678ULL;
    ir::IrFunction fn;
    fn.name = "get_str"; fn.ret_type = ir::IrType::PTR;
    ir::IrValueId v = fn.new_value(ir::IrType::PTR);
    auto bb = fn.new_block("e");
    {
        ir::IrInstr s; s.op = ir::IrOp::STR_LIT_ADDR; s.type = ir::IrType::PTR;
        s.dst = v; s.imm = 5;
        fn.append(bb, s);
    }
    fn.append(bb, ret1(v));

    CallResolver sym = [&](const std::string &n) -> uint64_t {
        return n == "code.s_5" ? kAddr : 0;
    };
    Proxy px; std::memset(&px, 0, sizeof(px));
    CHECK(jit_vm(fn, px, {}, 0, {}, sym), "jit_vm ok (str_lit_addr)");
    CHECK(px.regs[0] == kAddr, "str_lit_addr devuelve la direccion resuelta");
    if (px.regs[0] != kAddr)
        std::printf("    regs[0]=0x%llx esperado 0x%llx\n",
                    (unsigned long long)px.regs[0], (unsigned long long)kAddr);

    /* Sin resolver -> vreg_select debe rechazar (fallback seguro a slots). */
    Proxy px2; std::memset(&px2, 0, sizeof(px2));
    CHECK(!jit_vm(fn, px2, {}, 0, {}, {}),
          "str_lit_addr sin resolver -> fallback (vreg_select false)");
}

/* ---- Test: LABEL_ADDR resuelve la direccion del label via resolve_symbol --
 * Analogo a str_lit_addr pero la clave es "code.<func_name>".  Valida (a) el
 * codegen `mov dst, imm64(addr)`, (b) que func_name vacio -> fallback. */
static void test_vm_label_addr() {
    std::printf("[vm] label_addr: code.helper -> direccion resuelta en regs[0]\n");
    const uint64_t kAddr = 0x00007FFE12340000ULL;
    ir::IrFunction fn;
    fn.name = "get_fn"; fn.ret_type = ir::IrType::PTR;
    ir::IrValueId v = fn.new_value(ir::IrType::PTR);
    auto bb = fn.new_block("e");
    {
        ir::IrInstr s; s.op = ir::IrOp::LABEL_ADDR; s.type = ir::IrType::PTR;
        s.dst = v; s.func_name = "helper";
        fn.append(bb, s);
    }
    fn.append(bb, ret1(v));

    CallResolver sym = [&](const std::string &n) -> uint64_t {
        return n == "code.helper" ? kAddr : 0;
    };
    Proxy px; std::memset(&px, 0, sizeof(px));
    CHECK(jit_vm(fn, px, {}, 0, {}, sym), "jit_vm ok (label_addr)");
    CHECK(px.regs[0] == kAddr, "label_addr devuelve la direccion resuelta");
    if (px.regs[0] != kAddr)
        std::printf("    regs[0]=0x%llx esperado 0x%llx\n",
                    (unsigned long long)px.regs[0], (unsigned long long)kAddr);

    /* func_name vacio -> vreg_select debe rechazar (fallback). */
    ir::IrFunction fn2;
    fn2.name = "get_fn2"; fn2.ret_type = ir::IrType::PTR;
    ir::IrValueId v2 = fn2.new_value(ir::IrType::PTR);
    auto bb2 = fn2.new_block("e");
    {
        ir::IrInstr s; s.op = ir::IrOp::LABEL_ADDR; s.type = ir::IrType::PTR;
        s.dst = v2;  /* func_name vacio */
        fn2.append(bb2, s);
    }
    fn2.append(bb2, ret1(v2));
    Proxy px2; std::memset(&px2, 0, sizeof(px2));
    CHECK(!jit_vm(fn2, px2, {}, 0, {}, sym),
          "label_addr sin func_name -> fallback (vreg_select false)");
}

/* ---- Test: GETPROC devuelve el ProcessVM* (RBX en VM_ABI) ---------------
 * Fn de 1 bloque: v0 = GETPROC; ret v0.  En VM_ABI RBX = &px, asi que el
 * resultado debe ser la direccion del proxy. */
static void test_vm_getproc() {
    std::printf("[vm] getproc: regs[0] == &proc (RBX)\n");
    ir::IrFunction fn;
    fn.name = "get_proc"; fn.ret_type = ir::IrType::PTR;
    ir::IrValueId v = fn.new_value(ir::IrType::PTR);
    auto bb = fn.new_block("e");
    {
        ir::IrInstr s; s.op = ir::IrOp::GETPROC; s.type = ir::IrType::PTR;
        s.dst = v;
        fn.append(bb, s);
    }
    fn.append(bb, ret1(v));
    Proxy px; std::memset(&px, 0, sizeof(px));
    CHECK(jit_vm(fn, px), "jit_vm ok (getproc)");
    CHECK(px.regs[0] == reinterpret_cast<uint64_t>(&px),
          "getproc devuelve el ProcessVM* (RBX = &px)");
    if (px.regs[0] != reinterpret_cast<uint64_t>(&px))
        std::printf("    regs[0]=0x%llx esperado 0x%llx\n",
                    (unsigned long long)px.regs[0],
                    (unsigned long long)reinterpret_cast<uint64_t>(&px));
}

/* ---- Test: RET void en VM_ABI inicializa regs[0]=0 (exit code) -----------
 * Una fn `void` con RET sin operando debe dejar regs[0]=0 (no la basura
 * previa).  Cubre el caso `void main` cuyo R0 es el exit code observable;
 * sin esto quedaria el ultimo CALL VM_ABI (regresion encontrada con GETPROC
 * + test_print_formats). */
static void test_vm_ret_void() {
    std::printf("[vm] ret void: regs[0] se pone a 0\n");
    ir::IrFunction fn;
    fn.name = "voidfn"; fn.ret_type = ir::IrType::VOID;
    auto bb = fn.new_block("e");
    { ir::IrInstr r; r.op = ir::IrOp::RET; r.type = ir::IrType::VOID;
      fn.append(bb, r); }  /* ret sin operando */
    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[0] = 0xDEADBEEFCAFEULL;  /* basura previa */
    CHECK(jit_vm(fn, px), "jit_vm ok (ret void)");
    CHECK(px.regs[0] == 0, "ret void inicializa regs[0]=0");
    if (px.regs[0] != 0)
        std::printf("    regs[0]=0x%llx esperado 0\n",
                    (unsigned long long)px.regs[0]);
}

int main() {
    std::setbuf(stdout, nullptr);
    /* Forzar el gate de DIV/MOD en vregs para este test (default OFF). */
#if defined(_WIN32)
    _putenv("VESTA_JIT_VREG_IDIV=1");
#else
    setenv("VESTA_JIT_VREG_IDIV", "1", 1);
#endif
    std::printf("=== test_vreg_vm (Phase D.7 commit 5a, VM_ABI) ===\n");
    test_vm_divmod();
    test_vm_divmod_loop();
    test_vm_add();
    test_vm_loop();
    test_vm_self_recursion();  /* self-call rel32; antes de tests que crashean (callvirt/gc_stackmap) */
    test_vm_tailcall_self();   /* TCO self-tail-call (frame-reuse) */
    std::fflush(stdout);
    test_vm_str_lit_addr();  /* antes del crash pre-existente de gc_stackmap */
    test_vm_label_addr();
    test_vm_getproc();
    test_vm_ret_void();
    test_vm_load_vm();   /* antes del crash pre-existente de gc_stackmap */
    test_vm_store_vm();
    test_vm_strmake();   /* antes del crash pre-existente de gc_stackmap */
    test_vm_strlen();
    test_vm_strcat();
    test_vm_variant_markers();
    test_vm_read_vm_reg();
    test_vm_smartptr_free_extern();
    test_vm_smartptr_free_vesta();
    test_vm_callclosure();
    test_vm_sext_loop();
    test_vm_call();
    test_vm_callvirt();
    test_vm_gc_stackmap();
    test_vm_load_store();
    test_vm_alloca();
    test_vm_raw_free_host_alloca();
    test_vm_trunc();
    test_vm_calln();
    test_vm_abs();
    test_vm_gc_deref_inline();
    test_vm_math_inline();
    test_vm_math_irops();
    std::printf("--- %d checks, %d fallos ---\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
