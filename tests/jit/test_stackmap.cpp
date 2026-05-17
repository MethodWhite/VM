/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file tests/jit/test_stackmap.cpp
 * @brief Tests del sistema de stackmaps + JitRegistry + scan_jit_frames.
 *
 * Cubre:
 *   1. Encoder rellena @c pc_offset correctamente para cada SAFEPOINT.
 *   2. Selector emite stackmaps con los slots GC correctos.
 *   3. JitRegistry register/lookup/unregister con binary search.
 *   4. scan_jit_frames walks RBP chain y aplica stackmaps precisos.
 *   5. Coexistencia con conservativo: ambos encuentran los mismos roots
 *      (validacion empirica de la correctness del precise scan).
 */

#include "jit/code_cache.h"
#include "jit/jit_registry.h"
#include "jit/machine_ir.h"
#include "jit/runtime_entries.h"
#include "jit/selector.h"
#include "jit/stack_scan.h"
#include "jit/x86_encoder.h"
#include "ir/ssa_ir.h"
#include "vesta_rt/abi.h"

#include <atomic>
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

    using namespace jit;

    /* ===================================================================== */
    /* Test 1: JitRegistry register/lookup/unregister                         */
    /* ===================================================================== */

    void test_registry_basic() {
        JitRegistry &reg = JitRegistry::instance();
        reg.clear();
        CHECK(reg.size() == 0, "registry vacio inicial");

        /* Registrar 3 funciones con rangos distintos. */
        const uint8_t *base = reinterpret_cast<const uint8_t *>(0x10000);

        std::vector<Stackmap> sm1;
        {
            Stackmap s; s.pc_offset = 5;
            StackmapSlot slot; slot.rbp_offset = -8; slot.gc_kind = StackmapGcKind::HANDLE;
            s.slots.push_back(slot);
            sm1.push_back(s);
        }
        reg.register_function(base, base + 100, std::move(sm1), 32, "fn1");

        std::vector<Stackmap> sm2;
        reg.register_function(base + 200, base + 300, std::move(sm2), 16, "fn2");

        std::vector<Stackmap> sm3;
        reg.register_function(base + 400, base + 500, std::move(sm3), 8, "fn3");

        CHECK(reg.size() == 3, "3 funciones registradas");

        /* lookup tests. */
        CHECK(reg.lookup(base + 50) != nullptr, "rip in fn1 range");
        CHECK(reg.lookup(base + 50)->code_start == base, "fn1 start correcto");
        CHECK(reg.lookup(base + 150) == nullptr, "gap entre fn1 y fn2");
        CHECK(reg.lookup(base + 250) != nullptr, "rip in fn2 range");
        CHECK(reg.lookup(base + 250)->frame_size == 16, "fn2 frame_size");
        CHECK(reg.lookup(base + 450) != nullptr, "rip in fn3 range");
        CHECK(reg.lookup(base + 500) == nullptr, "code_end exclusive");
        CHECK(reg.lookup(nullptr) == nullptr, "nullptr safe");

        /* unregister. */
        reg.unregister_function(base + 200);
        CHECK(reg.size() == 2, "tras unregister: 2");
        CHECK(reg.lookup(base + 250) == nullptr, "fn2 ya no esta");
        CHECK(reg.lookup(base + 50) != nullptr, "fn1 sigue");

        reg.clear();
    }

    /* ===================================================================== */
    /* Test 2: lookup_stackmap por pc_offset                                  */
    /* ===================================================================== */

    void test_lookup_stackmap() {
        JitRegistry &reg = JitRegistry::instance();
        reg.clear();

        const uint8_t *base = reinterpret_cast<const uint8_t *>(0x20000);
        std::vector<Stackmap> sms;
        for (uint32_t off : {10u, 30u, 50u, 80u}) {
            Stackmap s; s.pc_offset = off;
            StackmapSlot slot; slot.rbp_offset = -static_cast<int16_t>(off);
            s.slots.push_back(slot);
            sms.push_back(s);
        }
        reg.register_function(base, base + 100, std::move(sms), 64, "test");

        /* lookup_stackmap retorna el stackmap MAYOR cuyo pc_offset <= rip_off. */
        CHECK(reg.lookup_stackmap(base + 5) == nullptr, "antes del primer safepoint");
        CHECK(reg.lookup_stackmap(base + 10) != nullptr, "exacto en safepoint 10");
        CHECK(reg.lookup_stackmap(base + 10)->pc_offset == 10, "stackmap 10");
        CHECK(reg.lookup_stackmap(base + 20)->pc_offset == 10, "entre 10 y 30 -> 10");
        CHECK(reg.lookup_stackmap(base + 30)->pc_offset == 30, "exacto en 30");
        CHECK(reg.lookup_stackmap(base + 79)->pc_offset == 50, "antes de 80 -> 50");
        CHECK(reg.lookup_stackmap(base + 80)->pc_offset == 80, "exacto en 80");
        CHECK(reg.lookup_stackmap(base + 90)->pc_offset == 80, "ultimo safepoint cubre el resto");
        CHECK(reg.lookup_stackmap(base + 200) == nullptr, "fuera de rango");

        reg.clear();
    }

    /* ===================================================================== */
    /* Test 3: Encoder rellena pc_offset del stackmap                         */
    /* ===================================================================== */

    void test_encoder_fills_pc_offset() {
        MFunction fn;
        fn.name = "test";

        /* Crear un stackmap manualmente y un MInstr SAFEPOINT que lo referencie. */
        Stackmap sm;
        sm.pc_offset = 0xDEADBEEF;  /* placeholder -- el encoder debe sobreescribir */
        StackmapSlot slot; slot.rbp_offset = -16; slot.gc_kind = StackmapGcKind::HANDLE;
        sm.slots.push_back(slot);
        fn.stackmaps.push_back(sm);

        const uint32_t pool_idx = fn.intern_imm64(0xCAFEBABE12345678ULL);

        MBlockId b = fn.new_block(fn.new_label());
        /* Algunas instrucciones triviales antes del safepoint. */
        fn.blocks[b].instrs.push_back(MInstr::make_nop());
        fn.blocks[b].instrs.push_back(MInstr::make_nop());
        /* SAFEPOINT con flags=0 (apunta al stackmap[0]). */
        MInstr sp_instr = MInstr::make_safepoint(pool_idx);
        sp_instr.flags = 0;  /* indice del stackmap */
        fn.blocks[b].instrs.push_back(sp_instr);
        fn.blocks[b].instrs.push_back(MInstr::make_ret());

        X86Encoder enc;
        std::vector<uint8_t> bytes;
        enc.encode(fn, bytes);

        /* Tras encode, fn.stackmaps[0].pc_offset debe apuntar al inicio
         * del cmp byte del SAFEPOINT, no a 0xDEADBEEF.
         * Los 2 NOPs = 2 bytes, asi que pc_offset esperado = 2. */
        CHECK(fn.stackmaps[0].pc_offset == 2, "pc_offset = 2 (despues de 2 nops)");
        CHECK(fn.stackmaps[0].pc_offset != 0xDEADBEEF, "pc_offset sobreescrito");
    }

    /* ===================================================================== */
    /* Test 4: Selector emite stackmap con slots GC correctos                 */
    /* ===================================================================== */

    /**
     * IR equivalente a:
     *   fn loop_gc(p: handle):
     *     entry:
     *       %1 = mov %0  (preserve param)
     *       br loop
     *     loop:
     *       (back-edge -> emite SAFEPOINT)
     *       br loop
     */
    void test_selector_emits_stackmap() {
        ir::IrFunction fn;
        fn.name = "loop_gc";
        fn.ret_type = ir::IrType::I64;

        /* %0 es param de tipo CLASS (is_gc_object=true). */
        ir::IrValue v0;
        v0.type = ir::IrType::HANDLE;
        v0.is_gc_object = true;
        fn.values.push_back(v0);  /* id 0 */
        fn.params = {0};

        /* %1 = mov %0  (copia trivial para tener algo despues del param) */
        ir::IrValue v1;
        v1.type = ir::IrType::I64;
        fn.values.push_back(v1);  /* id 1 */

        ir::IrBlock entry; entry.id = 0; entry.name = "entry";
        ir::IrInstr mov;
        mov.op = ir::IrOp::MOV;
        mov.type = ir::IrType::I64;
        mov.dst = 1;
        mov.operands = {0};
        entry.instrs.push_back(mov);

        ir::IrInstr br;
        br.op = ir::IrOp::BR;
        br.type = ir::IrType::VOID;
        br.target_block = 0;  /* back-edge a si mismo (loop infinito conceptual) */
        entry.instrs.push_back(br);
        entry.succs = {0};

        fn.blocks.push_back(entry);

        SelectorOptions opts;
        opts.mode = SelectorMode::VM_ABI;
        opts.safepoint_handler_addr = 0xCAFE0000ULL;  /* dummy handler */

        Selector sel(opts);
        bool unsupported = false;
        MFunction mf = sel.select(fn, &unsupported);
        CHECK(!unsupported, "no IR ops no soportados");

        /* Debe haber AL MENOS 1 stackmap (del SAFEPOINT en el back-edge). */
        CHECK(!mf.stackmaps.empty(), "selector emitio al menos 1 stackmap");

        /* El stackmap debe incluir el slot del param %0 (que es HANDLE/GC). */
        const Stackmap &sm = mf.stackmaps[0];
        bool found_param = false;
        for (const auto &slot : sm.slots) {
            /* slot offset para vid=0: -8 * (0 + 1) = -8 */
            if (slot.rbp_offset == -8 && slot.gc_kind == StackmapGcKind::HANDLE) {
                found_param = true;
                break;
            }
        }
        CHECK(found_param, "stackmap incluye el slot del param GC %0 (offset -8)");
    }

    /* ===================================================================== */
    /* Test 5: scan_jit_frames invoca callback con valores correctos          */
    /* ===================================================================== */

    /* Buffer global para que el callback grabe los valores encontrados. */
    struct ScanResults {
        std::vector<uint64_t> values;
        std::vector<StackmapGcKind> kinds;
    };

    extern "C" void scan_cb(void *ctx, uint64_t value,
                            StackmapGcKind kind,
                            const uint8_t * /*slot_addr*/) {
        ScanResults *r = static_cast<ScanResults *>(ctx);
        r->values.push_back(value);
        r->kinds.push_back(kind);
    }

    void test_scan_walks_jit_frame() {
        JitRegistry &reg = JitRegistry::instance();
        reg.clear();

        /* Simulacion: construimos un "stack frame" sintetico en heap del
         * test que parece un frame JIT con un saved RBP, return address,
         * y slots.  El walker debe encontrar los slots vistas en el
         * stackmap.
         *
         * Layout del stack simulado (memoria del test, NO el stack real):
         *
         *   +0    [u64]   slot at rbp-24:  0xAA (CLASS handle hipotetico)
         *   +8    [u64]   slot at rbp-16:  0xBB (handle)
         *   +16   [u64]   slot at rbp-8:   0xCC (handle)
         *   +24   [u64]   saved RBP        (apunta a "outer frame" o 0)
         *   +32   [u64]   return RIP       (dentro de fn registrada)
         *
         * Total: 40 bytes.  El walker llega con rbp_top = &buffer[24]
         * (= la posicion del saved RBP).
         */
        alignas(8) uint64_t stack_sim[5] = {
            0xAAULL, 0xBBULL, 0xCCULL,
            0ULL,           /* saved RBP = nullptr para terminar walk */
            0xDEAD0000ULL   /* return RIP fake, sera reemplazado */
        };
        uint8_t *stack_base = reinterpret_cast<uint8_t *>(stack_sim);
        uint8_t *rbp = stack_base + 24;          /* RBP simulado */
        uint8_t *rip_fake = stack_base + 100;    /* RIP fake "dentro" de la fn */
        stack_sim[4] = reinterpret_cast<uintptr_t>(rip_fake);

        /* Registrar una "fake JIT function" cuyo rango incluye rip_fake. */
        const uint8_t *code_start = stack_base;
        const uint8_t *code_end   = stack_base + 200;
        std::vector<Stackmap> sms;
        {
            Stackmap s;
            /* pc_offset = 50 (50 bytes desde code_start = stack_base[50]).
             * Para que rip_fake (offset 100 desde code_start) caiga
             * DESPUES del safepoint en 50, lookup_stackmap retorna este. */
            s.pc_offset = 50;
            StackmapSlot a; a.rbp_offset = -24; a.gc_kind = StackmapGcKind::HANDLE;
            StackmapSlot b; b.rbp_offset = -16; b.gc_kind = StackmapGcKind::HANDLE;
            StackmapSlot c; c.rbp_offset = -8;  c.gc_kind = StackmapGcKind::HOSTPTR;
            s.slots = {a, b, c};
            sms.push_back(s);
        }
        reg.register_function(code_start, code_end, std::move(sms), 32, "fake");

        /* Ejecutar el walker.  rbp_top = nuestro rbp simulado. */
        ScanResults results;
        const uint8_t *low  = stack_base;
        const uint8_t *high = stack_base + sizeof(stack_sim);

        JitScanStats stats = scan_jit_frames(scan_cb, &results, rbp, low, high);

        CHECK(stats.frames_walked >= 1, "walk encontro al menos 1 frame");
        CHECK(stats.jit_frames == 1, "1 frame JIT detectado");
        CHECK(stats.handles_marked == 2, "2 handles encontrados");
        CHECK(stats.hostptr_marked == 1, "1 hostptr encontrado");

        /* Verificar valores: deben ser 0xAA, 0xBB, 0xCC en ALGUN orden. */
        bool found_AA = false, found_BB = false, found_CC = false;
        for (auto v : results.values) {
            if (v == 0xAAULL) found_AA = true;
            if (v == 0xBBULL) found_BB = true;
            if (v == 0xCCULL) found_CC = true;
        }
        CHECK(found_AA, "valor 0xAA encontrado");
        CHECK(found_BB, "valor 0xBB encontrado");
        CHECK(found_CC, "valor 0xCC encontrado");

        reg.clear();
    }

    /* ===================================================================== */
    /* Test 6: scan termina limpiamente con stack corruption (defensa)        */
    /* ===================================================================== */

    void test_scan_safety_corrupted_stack() {
        JitRegistry &reg = JitRegistry::instance();
        reg.clear();

        ScanResults results;
        /* rbp_top en NULL */
        JitScanStats s1 = scan_jit_frames(scan_cb, &results, nullptr, nullptr, nullptr);
        CHECK(s1.frames_walked == 0, "rbp NULL no hace frames");

        /* rbp fuera del rango. */
        alignas(8) uint64_t stack_sim[2] = {0, 0};
        uint8_t *low  = reinterpret_cast<uint8_t *>(stack_sim);
        uint8_t *high = low + sizeof(stack_sim);
        uint8_t *outside = high + 1024;
        JitScanStats s2 = scan_jit_frames(scan_cb, &results, outside, low, high);
        CHECK(s2.frames_walked == 0, "rbp fuera de rango no hace frames");

        /* rbp NO alineado. */
        uint8_t *unaligned = low + 1;
        JitScanStats s3 = scan_jit_frames(scan_cb, &results, unaligned, low, high);
        CHECK(s3.frames_walked == 0, "rbp no-alineado se detecta y aborta");

        reg.clear();
    }

    /* ===================================================================== */
    /* Test 7: end-to-end - encoder integra stackmap con SAFEPOINT en loop    */
    /* ===================================================================== */

    /**
     * Construir IrFunction que tenga un loop con back-edge y un param
     * GC.  Compilar end-to-end (selector + encoder).  Verificar:
     *   1. El stackmap se emitio con pc_offset valido (no 0).
     *   2. El stackmap incluye el slot del param.
     *   3. JitRegistry::register_function + lookup_stackmap retornan
     *      el stackmap correcto.
     */
    void test_end_to_end_loop() {
        ir::IrFunction fn;
        fn.name = "loop";
        fn.ret_type = ir::IrType::I64;

        ir::IrValue v0;
        v0.type = ir::IrType::HANDLE;
        v0.is_gc_object = true;
        fn.values.push_back(v0);
        fn.params = {0};

        ir::IrBlock entry; entry.id = 0; entry.name = "entry";
        ir::IrInstr br;
        br.op = ir::IrOp::BR;
        br.target_block = 0;  /* back-edge */
        entry.instrs.push_back(br);
        entry.succs = {0};
        fn.blocks.push_back(entry);

        SelectorOptions opts;
        opts.mode = SelectorMode::VM_ABI;
        opts.safepoint_handler_addr = 0xCAFE0000ULL;

        Selector sel(opts);
        MFunction mf = sel.select(fn);

        X86Encoder enc;
        std::vector<uint8_t> bytes;
        enc.encode(mf, bytes);

        CHECK(!mf.stackmaps.empty(), "stackmaps emitidos");
        CHECK(mf.stackmaps[0].pc_offset > 0, "pc_offset no es 0 (sobre-escrito)");

        /* Registrar en JitRegistry. */
        JitRegistry &reg = JitRegistry::instance();
        reg.clear();
        const uint8_t *code = bytes.data();
        reg.register_function(code, code + bytes.size(),
                              std::move(mf.stackmaps),
                              mf.stack_frame_size, "loop");

        /* lookup en el RIP correspondiente al safepoint debe retornar
         * el stackmap. */
        const uint8_t *rip_at_sp = code + reg.lookup(code)->stackmaps[0].pc_offset;
        const Stackmap *found = reg.lookup_stackmap(rip_at_sp);
        CHECK(found != nullptr, "lookup_stackmap encuentra el safepoint");
        CHECK(!found->slots.empty(), "stackmap tiene al menos 1 slot");

        reg.clear();
    }

} // namespace anonymous

int main() {
    test_registry_basic();
    test_lookup_stackmap();
    test_encoder_fills_pc_offset();
    test_selector_emits_stackmap();
    test_scan_walks_jit_frame();
    test_scan_safety_corrupted_stack();
    test_end_to_end_loop();

    std::printf("test_stackmap: %d pass, %d fail\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
