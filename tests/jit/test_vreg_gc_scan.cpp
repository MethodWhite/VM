/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/jit/test_vreg_gc_scan.cpp
 * @brief Validacion end-to-end del commit 6 (GC en registros seguro): que el
 *        STACKMAP generado por el allocator vreg sea CORRECTO desde el punto de
 *        vista del GC.
 *
 * El test compila una funcion vreg con un GC root vivo a traves de un call
 * (que el allocator FUERZA a un slot + describe en un stackmap), simula su
 * frame en el stack, y ejecuta @c scan_jit_frames -- exactamente lo que el GC
 * real invoca durante un sweep.  Verifica que el walker encuentra el GC root
 * EN EL OFFSET que el commit 6 emitio (no solo que se preserva el valor: que
 * el GC sabe DONDE buscarlo).  Si el @c rbp_offset del stackmap mintiera, el
 * GC leeria basura -> este test lo detecta.
 */

#include "ir/ssa_ir.h"
#include "jit/code_cache.h"
#include "jit/interval.h"
#include "jit/jit_registry.h"
#include "jit/linear_scan.h"
#include "jit/machine_ir.h"
#include "jit/regalloc_rewrite.h"
#include "jit/stack_scan.h"
#include "jit/target_reginfo.h"
#include "jit/vreg_select.h"
#include "jit/x86_encoder.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace jit;

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, msg)                                                    \
    do { ++g_checks; if (!(cond)) { ++g_fails;                              \
        std::printf("  FAIL: %s  (linea %d)\n", (msg), __LINE__); } } while (0)

/* Resultados del scan. */
struct ScanResults {
    std::vector<uint64_t>       values;
    std::vector<StackmapGcKind> kinds;
};
extern "C" void scan_cb(void *ctx, uint64_t value, StackmapGcKind kind,
                        const uint8_t * /*slot_addr*/) {
    ScanResults *r = static_cast<ScanResults *>(ctx);
    r->values.push_back(value);
    r->kinds.push_back(kind);
}

/* Helpers IR. */
static ir::IrInstr bin(ir::IrOp op, ir::IrValueId d, ir::IrValueId a, ir::IrValueId b) {
    ir::IrInstr i; i.op = op; i.type = ir::IrType::I64; i.dst = d;
    i.operands = { a, b }; return i;
}
static ir::IrInstr ret1(ir::IrValueId v) {
    ir::IrInstr i; i.op = ir::IrOp::RET; i.type = ir::IrType::I64;
    i.operands = { v }; return i;
}

/* ===================================================================== */
/* Validacion: el GC encuentra el root via el stackmap del commit 6        */
/* ===================================================================== */
static void test_gc_root_found_by_scan() {
    std::printf("[gc-scan] el stackmap del commit 6 guia al GC al GC root\n");

    /* 1. gcf(this_gc, x) = g() + this.  `this` (GC host_ptr) vive a traves
     *    del call -> el allocator lo spillea + emite stackmap. */
    ir::IrFunction fn;
    fn.name = "gcf"; fn.ret_type = ir::IrType::I64;
    auto I64 = ir::IrType::I64;
    ir::IrValueId thisp = fn.new_value(ir::IrType::PTR);
    ir::IrValueId x = fn.new_value(I64), r = fn.new_value(I64), sum = fn.new_value(I64);
    fn.values[thisp].is_gc_object = true;
    fn.values[thisp].is_host_ptr  = true;   // -> HOSTPTR
    fn.params = { thisp, x };
    ir::IrBlockId bb = fn.new_block("e");
    {
        ir::IrInstr c; c.op = ir::IrOp::CALL; c.type = I64; c.dst = r;
        c.func_name = "g";
        fn.append(bb, c);
    }
    fn.append(bb, bin(ir::IrOp::ADD, sum, r, thisp));  // this vivo a traves
    fn.append(bb, ret1(sum));

    /* Resolver dummy: el codigo NO se ejecuta (solo simulamos su frame). */
    CallResolver resolver = [](const std::string &n) -> uint64_t {
        return n == "g" ? 0x100000ULL : 0;   // direccion ficticia
    };

    /* 2. Pipeline -> pf con stackmaps reales. */
    MFunction mf;
    CHECK(vreg_select(fn, mf, AbiKind::VM, resolver), "vreg_select ok");
    const TargetRegInfo &tri = target_x86_64_vm_abi();
    IntervalResult ivs = build_intervals(mf, tri);
    RegAlloc ra = linear_scan(ivs, tri);
    CHECK(ra.spilled(thisp), "this (GC root) spilled");
    MFunction pf = rewrite_to_physical(mf, ra, tri, AbiKind::VM, &ivs);

    /* 3. Encode -> bytes (rellena pc_offset en pf.stackmaps). */
    X86Encoder enc;
    std::vector<uint8_t> bytes;
    CHECK(enc.encode(pf, bytes) != 0 && !bytes.empty(), "encode ok");
    CodeCache cc;
    uint8_t *code = cc.alloc(bytes.size(), 16);
    std::memcpy(code, bytes.data(), bytes.size());
    cc.commit(code, bytes.size());

    /* 4. Capturar el stackmap generado por commit 6. */
    CHECK(pf.stackmaps.size() == 1, "1 stackmap");
    if (pf.stackmaps.size() != 1) return;
    CHECK(pf.stackmaps[0].slots.size() == 1, "1 GC root en el stackmap");
    if (pf.stackmaps[0].slots.size() != 1) return;
    const int16_t       gc_off = pf.stackmaps[0].slots[0].rbp_offset;
    const StackmapGcKind kind  = pf.stackmaps[0].slots[0].gc_kind;
    const uint32_t       pc_off = pf.stackmaps[0].pc_offset;
    CHECK(kind == StackmapGcKind::HOSTPTR, "kind HOSTPTR");
    CHECK(pc_off > 0 && pc_off < bytes.size(), "pc_offset dentro del codigo");

    /* 5. Registrar la funcion en el JitRegistry (rango + stackmaps). */
    JitRegistry &reg = JitRegistry::instance();
    reg.clear();
    std::vector<Stackmap> sms = pf.stackmaps;   // copia (register_function la mueve)
    reg.register_function(code, code + bytes.size(), std::move(sms),
                          static_cast<uint32_t>(8u * ra.num_spill_slots), "gcf");

    /* 6. Simular el frame de gcf en el stack.  Layout (memoria del test):
     *      [rbp + gc_off]  <- el GC root (gc_off negativo)
     *      ...
     *      [rbp]           <- saved RBP = 0 (termina el walk)
     *      [rbp + 8]       <- return RIP = code + pc_off + 1 (DESPUES del call)
     *    El valor 0xCAFEBABE simula el host_ptr del objeto vivo. */
    constexpr uint64_t GC_VALUE = 0xCAFEBABEDEADBEEFULL;
    alignas(16) uint64_t stackbuf[64];
    std::memset(stackbuf, 0, sizeof(stackbuf));
    uint8_t *base = reinterpret_cast<uint8_t *>(stackbuf);
    uint8_t *rbp  = base + 8 * 48;              // RBP alto en el buffer
    *reinterpret_cast<uint64_t *>(rbp)       = 0ULL;   // saved RBP -> termina
    *reinterpret_cast<uint64_t *>(rbp + 8)   =
        reinterpret_cast<uintptr_t>(code + pc_off + 1);  // return RIP
    /* El GC root EN EL OFFSET que el commit 6 dijo. */
    *reinterpret_cast<uint64_t *>(rbp + gc_off) = GC_VALUE;

    /* 7. Ejecutar el walker (lo que el GC real hace). */
    ScanResults results;
    const uint8_t *low  = base;
    const uint8_t *high = base + sizeof(stackbuf);
    JitScanStats stats = scan_jit_frames(scan_cb, &results, rbp, low, high);

    /* 8. Verificar: el GC encontro el root EN EL OFFSET CORRECTO. */
    CHECK(stats.jit_frames == 1, "1 frame JIT reconocido");
    CHECK(results.values.size() == 1, "1 GC root escaneado");
    if (results.values.size() == 1) {
        CHECK(results.values[0] == GC_VALUE,
              "el GC leyo el host_ptr correcto del slot (offset del stackmap OK)");
        CHECK(results.kinds[0] == StackmapGcKind::HOSTPTR, "kind HOSTPTR en el scan");
        if (results.values[0] != GC_VALUE)
            std::printf("    leyo 0x%llx, esperado 0x%llx (gc_off=%d)\n",
                        (unsigned long long)results.values[0],
                        (unsigned long long)GC_VALUE, (int)gc_off);
    }
    asm volatile("" : : "r"(&cc) : "memory");  // cc viva
    reg.clear();
}

int main() {
    std::setbuf(stdout, nullptr);
    std::printf("=== test_vreg_gc_scan (Phase D.7 commit 6: stackmap <-> GC) ===\n");
    test_gc_root_found_by_scan();
    std::printf("--- %d checks, %d fallos ---\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
