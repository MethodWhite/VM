/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/jit/test_osr.cpp
 * @brief Test de aislamiento del OSR-entry (on-stack replacement, Phase D.8, 2b).
 *
 * Construye un loop `sum(n) = 0+1+...+(n-1)` en SSA IR, lo compila por el path
 * vreg en modo VM_ABI con un OSR-entry para el loop header (rewrite_to_physical
 * + OsrEmit), y valida DOS entradas al mismo blob de codigo:
 *
 *   1. ENTRY NORMAL (offset 0): se ejecuta como una funcion entera; lee el
 *      parametro de proc->registers[1], corre el loop completo y deja el
 *      resultado en proc->registers[0].  Confirma que el loop compila y corre.
 *
 *   2. OSR-ENTRY (offset osr_entry_label): el 2o punto de entrada a mitad del
 *      loop.  Se le pre-rellena proc->osr_buffer con un estado intermedio
 *      ARBITRARIO {i=mid_i, acc=mid_acc, n=N}; al entrar, carga ese estado del
 *      buffer a sus registros/slots y reanuda el loop desde el header,
 *      produciendo mid_acc + sum(mid_i..N-1).  Confirma que el OSR-entry lee el
 *      buffer-por-VID y resume correctamente.
 *
 * El estado intermedio se elige NO-coincidente con ninguna iteracion real
 * (mid_acc arbitrario) para probar que el OSR-entry usa los valores EXACTOS del
 * buffer y no un estado "casual".
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

/** @brief Proxy con el layout que el JIT espera via RBX: registers @96 y el
 *  puntero osr_buffer @1312 (offsets fijados por abi.h + static_assert). */
struct ProcOsr {
    uint8_t  safepoint_flag;                              // offset 0
    uint8_t  _pad1[VESTA_PROC_REGISTERS_OFFSET - 1];      // hasta 96
    uint64_t regs[VESTA_PROC_REGISTER_COUNT];            // offset 96
    uint8_t  _pad2[VESTA_PROC_OSR_BUFFER_OFFSET
                   - VESTA_PROC_REGISTERS_OFFSET
                   - sizeof(uint64_t) * VESTA_PROC_REGISTER_COUNT];
    uint64_t *osr_buffer;                                // offset 1312
};
static_assert(offsetof(ProcOsr, regs) == VESTA_PROC_REGISTERS_OFFSET,
              "regs@96");
static_assert(offsetof(ProcOsr, osr_buffer) == VESTA_PROC_OSR_BUFFER_OFFSET,
              "osr_buffer@1312");

/* ---- Helpers IR (mismo estilo que test_vreg_vm) ---- */
static ir::IrInstr konst(ir::IrValueId d, int64_t k) {
    ir::IrInstr i; i.op = ir::IrOp::CONST; i.type = ir::IrType::I64;
    i.dst = d; i.imm = static_cast<uint64_t>(k); return i;
}
static ir::IrInstr bin(ir::IrOp op, ir::IrValueId d, ir::IrValueId a,
                       ir::IrValueId b) {
    ir::IrInstr i; i.op = op; i.type = ir::IrType::I64;
    i.dst = d; i.operands = { a, b }; return i;
}
static ir::IrInstr cmplt(ir::IrValueId d, ir::IrValueId a, ir::IrValueId b) {
    ir::IrInstr i; i.op = ir::IrOp::CMP_LT; i.type = ir::IrType::BOOL;
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
static ir::IrInstr ret1(ir::IrValueId v) {
    ir::IrInstr i; i.op = ir::IrOp::RET; i.type = ir::IrType::I64;
    i.operands = { v }; return i;
}

/**
 * @brief Construye `i64 sum(i64 n)` con un while-loop SSA.
 *
 * VIDs (orden de creacion):
 *   0 = n (param)         3 = phi_i      6 = acc'(=acc+i)
 *   1 = const 0 (i ini)   4 = phi_acc    7 = const 1 (en body)
 *   2 = const 0 (acc ini) 5 = cmp        8 = i'(=i+1)
 * Bloques: 0=entry 1=header 2=body 3=exit.  El header (bb1) es el loop header.
 */
struct LoopVids { ir::IrValueId n, pi, pa; };
static LoopVids build_sum_loop(ir::IrFunction &fn) {
    fn.name = "sum"; fn.ret_type = ir::IrType::I64;
    const ir::IrValueId n  = fn.new_value(ir::IrType::I64);  // 0
    fn.params = { n };
    const ir::IrValueId c0 = fn.new_value(ir::IrType::I64);  // 1
    const ir::IrValueId a0 = fn.new_value(ir::IrType::I64);  // 2
    const ir::IrValueId pi = fn.new_value(ir::IrType::I64);  // 3
    const ir::IrValueId pa = fn.new_value(ir::IrType::I64);  // 4
    const ir::IrValueId cc = fn.new_value(ir::IrType::I64);  // 5
    const ir::IrValueId a2 = fn.new_value(ir::IrType::I64);  // 6
    const ir::IrValueId c1 = fn.new_value(ir::IrType::I64);  // 7
    const ir::IrValueId i2 = fn.new_value(ir::IrType::I64);  // 8

    const ir::IrBlockId entry  = fn.new_block("entry");   // 0
    const ir::IrBlockId header = fn.new_block("header");  // 1
    const ir::IrBlockId body   = fn.new_block("body");    // 2
    const ir::IrBlockId exitb  = fn.new_block("exit");    // 3

    /* entry: i=0, acc=0, br header */
    fn.append(entry, konst(c0, 0));
    fn.append(entry, konst(a0, 0));
    fn.append(entry, br(header));
    fn.blocks[entry].succs = { header };
    /* header: phi i, phi acc, cmp i<n, br_cond body/exit */
    fn.append(header, phi2(pi, c0, entry, i2, body));
    fn.append(header, phi2(pa, a0, entry, a2, body));
    fn.append(header, cmplt(cc, pi, n));
    fn.append(header, brc(cc, body, exitb));
    fn.blocks[header].preds = { entry, body };
    fn.blocks[header].succs = { body, exitb };
    /* body: acc'=acc+i, const 1, i'=i+1, br header (back-edge) */
    fn.append(body, bin(ir::IrOp::ADD, a2, pa, pi));
    fn.append(body, konst(c1, 1));
    fn.append(body, bin(ir::IrOp::ADD, i2, pi, c1));
    fn.append(body, br(header));
    fn.blocks[body].preds = { header };
    fn.blocks[body].succs = { header };
    /* exit: ret acc */
    fn.append(exitb, ret1(pa));
    fn.blocks[exitb].preds = { header };

    return { n, pi, pa };
}

int main() {
    std::printf("=== test_osr (Phase D.8 2b: OSR-entry en aislamiento) ===\n");

    ir::IrFunction fn;
    const LoopVids v = build_sum_loop(fn);

    /* --- Pipeline vreg (VM_ABI) con OSR-entry para el header (bb1). --- */
    MFunction mf;
    VregEntries ent;
    if (!vreg_select(fn, mf, AbiKind::VM, {}, ent, {})) {
        std::printf("  FAIL: vreg_select rechazo el loop\n");
        std::printf("--- %d checks, %d fallos ---\n", g_checks, ++g_fails);
        return 1;
    }
    const TargetRegInfo &tri = target_x86_64_vm_abi();
    IntervalResult ivs = build_intervals(mf, tri);
    RegAlloc ra = linear_scan(ivs, tri);

    OsrEmit osr;
    osr.mode = OsrEmit::C2_ENTRY;
    osr.header_block = 1;  // el loop header
    MFunction pf = rewrite_to_physical(mf, ra, tri, AbiKind::VM, &ivs, &osr);

    CHECK(osr.osr_entry_valid, "se emitio el bloque OSR-entry");

    X86Encoder enc;
    std::vector<uint8_t> bytes;
    const size_t n = enc.encode(pf, bytes);
    CHECK(n != 0 && !bytes.empty(), "encode OK");
    if (bytes.empty()) {
        std::printf("--- %d checks, %d fallos ---\n", g_checks, g_fails);
        return 1;
    }

    CodeCache cc;
    uint8_t *code = cc.alloc(bytes.size(), 16);
    CHECK(code != nullptr, "alloc code cache");
    std::memcpy(code, bytes.data(), bytes.size());
    cc.commit(code, bytes.size());

    uint8_t *normal_entry = code;  // bloque 0 arranca en offset 0
    uint8_t *osr_entry = nullptr;
    if (osr.osr_entry_valid
     && osr.osr_entry_label < pf.label_offsets.size()
     && pf.label_offsets[osr.osr_entry_label] != UINT32_MAX) {
        osr_entry = code + pf.label_offsets[osr.osr_entry_label];
    }
    CHECK(osr_entry != nullptr, "OSR-entry resuelto a una direccion");
    CHECK(osr_entry != normal_entry, "OSR-entry distinto del entry normal");

    using Fn = void (*)(void *);
    const int64_t N = 100;
    const int64_t full = N * (N - 1) / 2;  // sum(0..99) = 4950

    /* --- (1) ENTRY NORMAL: corre el loop completo, param en regs[1]. --- */
    {
        ProcOsr px; std::memset(&px, 0, sizeof(px));
        std::vector<uint64_t> buf(VESTA_OSR_BUFFER_N, 0);
        px.osr_buffer = buf.data();
        px.regs[1] = static_cast<uint64_t>(N);  // n
        reinterpret_cast<Fn>(normal_entry)(&px);
        CHECK(static_cast<int64_t>(px.regs[0]) == full,
              "entry normal: sum(0..99) == 4950");
        if (static_cast<int64_t>(px.regs[0]) != full)
            std::printf("    obtuvo %lld (esperaba %lld)\n",
                        (long long)px.regs[0], (long long)full);
    }

    /* --- (2) OSR-ENTRY: estado intermedio ARBITRARIO desde el buffer. --- */
    {
        ProcOsr px; std::memset(&px, 0, sizeof(px));
        std::vector<uint64_t> buf(VESTA_OSR_BUFFER_N, 0);
        px.osr_buffer = buf.data();
        const int64_t mid_i   = 40;
        const int64_t mid_acc = 1000;  // arbitrario (no coincide con ningun iter real)
        /* sum(mid_i..N-1) = sum(40..99) = full - sum(0..39) = 4950 - 780 = 4170 */
        const int64_t expected = mid_acc + (full - (mid_i * (mid_i - 1) / 2));
        /* Pre-rellenar el buffer-por-VID con el live-in del header. */
        buf[v.pi] = static_cast<uint64_t>(mid_i);    // phi_i
        buf[v.pa] = static_cast<uint64_t>(mid_acc);  // phi_acc
        buf[v.n]  = static_cast<uint64_t>(N);        // n (invariante)
        /* regs[1] (param n) NO debe usarse en el OSR-entry: lo ponemos basura. */
        px.regs[1] = 0xDEADBEEF;
        reinterpret_cast<Fn>(osr_entry)(&px);
        CHECK(static_cast<int64_t>(px.regs[0]) == expected,
              "OSR-entry: mid_acc + sum(mid_i..N-1) == 5170");
        if (static_cast<int64_t>(px.regs[0]) != expected)
            std::printf("    obtuvo %lld (esperaba %lld)\n",
                        (long long)px.regs[0], (long long)expected);
    }

    /* --- (3) OSR-ENTRY de nuevo con otro estado, mismo blob. --- */
    {
        ProcOsr px; std::memset(&px, 0, sizeof(px));
        std::vector<uint64_t> buf(VESTA_OSR_BUFFER_N, 0);
        px.osr_buffer = buf.data();
        const int64_t mid_i = 99, mid_acc = 7;  // casi terminado
        const int64_t expected = mid_acc + (full - (mid_i * (mid_i - 1) / 2)); // 7 + 99 = 106
        buf[v.pi] = static_cast<uint64_t>(mid_i);
        buf[v.pa] = static_cast<uint64_t>(mid_acc);
        buf[v.n]  = static_cast<uint64_t>(N);
        reinterpret_cast<Fn>(osr_entry)(&px);
        CHECK(static_cast<int64_t>(px.regs[0]) == expected,
              "OSR-entry (estado 2): 7 + sum(99..99) == 106");
        if (static_cast<int64_t>(px.regs[0]) != expected)
            std::printf("    obtuvo %lld (esperaba %lld)\n",
                        (long long)px.regs[0], (long long)expected);
    }
    asm volatile("" : : "r"(&cc) : "memory");  // mantener cc viva

    std::printf("--- %d checks, %d fallos ---\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
