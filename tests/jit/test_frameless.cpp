/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/jit/test_frameless.cpp
 * @brief Test del codegen FRAMELESS de hojas en el path vreg (Phase D.8 / C2).
 *
 * Una hoja (funcion sin CALLs) que no spillea ni reserva allocas no necesita
 * frame pointer: el rewrite (@c regalloc_rewrite.cpp) omite
 * @c push rbp / mov rbp,rsp / sub rsp / lea / pop rbp.  Esto es el codegen MAS
 * safety-critical del JIT: si una funcion que PUEDE estar viva en la pila
 * durante un safepoint perdiera su frame, el precise stack-scan de la GC (que
 * camina la cadena RBP) corromperia el heap.  Es seguro SOLO para hojas: una
 * hoja no tiene safepoint (no llama a nada), asi que nunca esta en la pila
 * cuando corre la GC.
 *
 * Este test fija el invariante:
 *   - una HOJA vm_abi se compila SIN @c push rbp (frameless) y ejecuta correcto
 *     (balance exacto de los push/pop de RBX y callee-saved);
 *   - una funcion CON un CALL conserva @c push rbp / mov rbp,rsp (frame intacto
 *     -> la cadena RBP que la GC camina sigue valida);
 *   - el gate @c VESTA_JIT_NO_FRAMELESS=1 fuerza el frame completo aun en hojas.
 *
 * Es un test AISLADO (no usa el runtime ni @c test_vreg_vm, que tiene un crash
 * pre-existente en @c test_vm_gc_stackmap que enmascararia el resumen).
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
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace jit;

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, msg)                                                    \
    do { ++g_checks; if (!(cond)) { ++g_fails;                              \
        std::printf("  FAIL: %s  (linea %d)\n", (msg), __LINE__); } } while (0)

/** @brief Proxy con el layout que el codigo JIT VM_ABI espera (RBX=ProcessVM*,
 *  safepoint_flag@0, registers@96). */
struct Proxy {
    uint8_t  safepoint_flag;
    uint8_t  _pad[VESTA_PROC_REGISTERS_OFFSET - 1];
    uint64_t regs[VESTA_PROC_REGISTER_COUNT];
};
static_assert(offsetof(Proxy, regs) == VESTA_PROC_REGISTERS_OFFSET,
              "regs debe quedar en el offset que usa el codigo JIT");

/* ---- Helpers IR minimos ----------------------------------------------- */
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

/** @brief Compila @p fn (VM_ABI), captura los bytes en @p bytes y la ejecuta
 *  con RBX=&px.  Devuelve false si algun paso del pipeline falla. */
static bool compile_run(const ir::IrFunction &fn, Proxy &px,
                        std::vector<uint8_t> &bytes,
                        const CallResolver &resolve = {}) {
    MFunction mf;
    if (!vreg_select(fn, mf, AbiKind::VM, resolve, {}, {})) return false;
    const TargetRegInfo &tri = target_x86_64_vm_abi();
    RegAlloc ra = linear_scan(build_intervals(mf, tri), tri);
    MFunction pf = rewrite_to_physical(mf, ra, tri, AbiKind::VM);
    X86Encoder enc;
    if (enc.encode(pf, bytes) == 0 || bytes.empty()) return false;
    CodeCache cc;
    uint8_t *code = cc.alloc(bytes.size(), 16);
    if (!code) return false;
    std::memcpy(code, bytes.data(), bytes.size());
    cc.commit(code, bytes.size());
    reinterpret_cast<void (*)(void *)>(code)(&px);
    asm volatile("" : : "r"(&cc) : "memory");  // mantener cc viva
    return true;
}

/** @brief True si los bytes empiezan con @c push rbp; mov rbp,rsp
 *  (0x55 0x48 0x89 0xe5) -> hay frame pointer. */
static bool has_frame_pointer(const std::vector<uint8_t> &b) {
    return b.size() >= 4 && b[0] == 0x55 && b[1] == 0x48 &&
           b[2] == 0x89 && b[3] == 0xe5;
}
/** @brief True si los bytes terminan con @c pop rbp; ret (0x5d 0xc3). */
static bool ends_with_pop_rbp(const std::vector<uint8_t> &b) {
    return b.size() >= 2 && b[b.size() - 2] == 0x5d && b[b.size() - 1] == 0xc3;
}
/** @brief True si los bytes contienen @c mov rbp,rsp (0x48 0x89 0xe5) en
 *  cualquier posicion -> uso del frame pointer. */
static bool contains_mov_rbp_rsp(const std::vector<uint8_t> &b) {
    for (size_t i = 0; i + 2 < b.size(); ++i)
        if (b[i] == 0x48 && b[i + 1] == 0x89 && b[i + 2] == 0xe5) return true;
    return false;
}

/** @brief Construye una hoja vm_abi simple: add(a,b) = a + b. */
static ir::IrFunction make_leaf_add() {
    ir::IrFunction fn; fn.name = "add"; fn.ret_type = ir::IrType::I64;
    auto a = fn.new_value(ir::IrType::I64);
    auto b = fn.new_value(ir::IrType::I64);
    auto c = fn.new_value(ir::IrType::I64);
    fn.params = { a, b };
    auto bb = fn.new_block("e");
    fn.append(bb, bin(ir::IrOp::ADD, c, a, b));
    fn.append(bb, ret1(c));
    return fn;
}

/* ---- Test 1: hoja simple -> frameless --------------------------------- */
static void test_leaf_is_frameless() {
    std::printf("[frameless] hoja add(a,b): sin push rbp, ejecuta 40+2=42\n");
    ir::IrFunction fn = make_leaf_add();
    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 40; px.regs[2] = 2;
    std::vector<uint8_t> bytes;
    CHECK(compile_run(fn, px, bytes), "compila + ejecuta (add)");
    CHECK(!bytes.empty(), "emitio bytes");
    CHECK(!has_frame_pointer(bytes), "hoja NO empieza con push rbp/mov rbp,rsp");
    CHECK(!contains_mov_rbp_rsp(bytes), "hoja NO usa el frame pointer en ningun sitio");
    CHECK(!ends_with_pop_rbp(bytes), "hoja NO termina con pop rbp; ret");
    /* vm_abi conserva RBX (callee-saved host = ProcessVM*): primer byte = push rbx. */
    CHECK(!bytes.empty() && bytes[0] == 0x53, "primer byte = push rbx (0x53), no push rbp");
    CHECK(px.regs[0] == 42, "add(40,2)==42 (balance push/pop correcto)");
    if (px.regs[0] != 42)
        std::printf("    regs[0]=%llu\n", (unsigned long long)px.regs[0]);
}

/* ---- Test 2: hoja con loop -> frameless + ejecucion correcta ----------- *
 * sum(n) = 0+1+...+(n-1).  Sin CALLs (hoja); el control de flujo (saltos)
 * no afecta al frameless.  Valida que el balance push/pop es correcto aun
 * con un loop.  n=10 -> 45. */
static void test_leaf_loop_is_frameless() {
    std::printf("[frameless] hoja sum(n) con loop: sin push rbp, sum(10)=45\n");
    ir::IrFunction fn; fn.name = "sum_n"; fn.ret_type = ir::IrType::I64;
    auto T = ir::IrType::I64;
    ir::IrValueId n = fn.new_value(T);
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
    std::vector<uint8_t> bytes;
    CHECK(compile_run(fn, px, bytes), "compila + ejecuta (sum)");
    CHECK(!has_frame_pointer(bytes), "hoja con loop NO empieza con push rbp");
    CHECK(!contains_mov_rbp_rsp(bytes), "hoja con loop NO usa el frame pointer");
    CHECK(px.regs[0] == 45, "sum(0..9)==45 (balance correcto con saltos)");
    if (px.regs[0] != 45)
        std::printf("    regs[0]=%llu\n", (unsigned long long)px.regs[0]);
}

/** @brief Stub nativo: pone regs[0]=50 (simula un callee que clobbea
 *  caller-saved como cualquier funcion C). */
extern "C" void frameless_call_stub(void *proc) {
    Proxy *p = static_cast<Proxy *>(proc);
    p->regs[0] = 50;
}

/* ---- Test 3: funcion CON call -> conserva el frame (GC-safety) --------- *
 * caller(x) = g() + x.  Tiene un CALL -> NO es hoja -> DEBE conservar
 * push rbp/mov rbp,rsp para que el precise stack-scan de la GC (que camina la
 * cadena RBP) encuentre un frame valido si esta funcion esta en la pila
 * durante un safepoint (el call a g es justo un punto donde la GC puede
 * correr).  g()=50, x=7 -> 57. */
static void test_call_preserves_frame() {
    std::printf("[frameless] funcion con CALL conserva push rbp (GC-safety)\n");
    ir::IrFunction fn; fn.name = "caller"; fn.ret_type = ir::IrType::I64;
    auto I64 = ir::IrType::I64;
    ir::IrValueId x = fn.new_value(I64), r = fn.new_value(I64), s = fn.new_value(I64);
    fn.params = { x };
    ir::IrBlockId bb = fn.new_block("e");
    { ir::IrInstr c; c.op = ir::IrOp::CALL; c.type = I64; c.dst = r;
      c.func_name = "g"; fn.append(bb, c); }
    fn.append(bb, bin(ir::IrOp::ADD, s, r, x));   // x vivo a traves del call
    fn.append(bb, ret1(s));

    CallResolver resolver = [](const std::string &n) -> uint64_t {
        if (n == "g")
            return reinterpret_cast<uint64_t>(
                reinterpret_cast<void *>(&frameless_call_stub));
        return 0;
    };
    Proxy px; std::memset(&px, 0, sizeof(px));
    px.regs[1] = 7;
    std::vector<uint8_t> bytes;
    CHECK(compile_run(fn, px, bytes, resolver), "compila + ejecuta (caller)");
    CHECK(has_frame_pointer(bytes),
          "funcion CON call SI empieza con push rbp/mov rbp,rsp (frame intacto)");
    CHECK(ends_with_pop_rbp(bytes), "funcion CON call termina con pop rbp; ret");
    CHECK(px.regs[0] == 57, "caller(7) == g()(50) + 7 == 57");
    if (px.regs[0] != 57)
        std::printf("    regs[0]=%llu\n", (unsigned long long)px.regs[0]);
}

/* El gate VESTA_JIT_NO_FRAMELESS (valvula de diagnostico que fuerza el frame
 * completo) NO se testea aqui: se lee UNA sola vez (static const cacheado en
 * el primer compile), asi que no se puede ejercitar ON y OFF en el mismo
 * proceso.  Su efecto esta validado por el benchmark A/B (hoja en loop). */

int main() {
    std::setbuf(stdout, nullptr);
    std::printf("=== test_frameless (Phase D.8 / C2: codegen frameless de hojas) ===\n");
    test_leaf_is_frameless();
    test_leaf_loop_is_frameless();
    test_call_preserves_frame();
    std::printf("--- %d checks, %d fallos ---\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
