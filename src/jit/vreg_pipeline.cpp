/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/vreg_pipeline.cpp
 * @brief Implementacion del orquestador vreg (Phase D.7, commit 5c).
 *        Ver vreg_pipeline.h y doc/REGALLOC.md.
 */

#include "jit/vreg_pipeline.h"

#include "ir/ssa_ir.h"
#include "jit/auto_jit.h"
#include "jit/code_cache.h"
#include "jit/interval.h"
#include "jit/jit_registry.h"
#include "jit/linear_scan.h"
#include "jit/machine_ir.h"
#include "jit/regalloc_rewrite.h"
#include "jit/target_reginfo.h"
#include "jit/vreg_select.h"
#include "jit/x86_encoder.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace jit {

    uint8_t *vreg_compile(const ir::IrFunction &fn, CodeCache &cc,
                          const CallResolver &resolve_call,
                          const VregEntries &ent,
                          const CallResolver &resolve_native,
                          const CallResolver &resolve_symbol) {
        /* 1. Seleccionar MachineIR de vregs (VM_ABI).  Si la funcion usa un
         *    op fuera del subset soportado, abortar -> fallback. */
        MFunction mf;
        if (!vreg_select(fn, mf, AbiKind::VM, resolve_call, ent, resolve_native,
                         resolve_symbol))
            return nullptr;

        const TargetRegInfo &tri = target_x86_64_vm_abi();

        /* 2. Intervalos + 3. asignacion (commit 6: el linear_scan FUERZA a
         *    slot los GC roots vivos a traves de un call). */
        IntervalResult ivs = build_intervals(mf, tri);
        RegAlloc ra = linear_scan(ivs, tri);

        /* 3b. Verificador adversarial (commit 6): TODO GC root vivo a traves
         *     de un call DEBE estar en un slot, para que su stackmap lo
         *     describa.  Si por algun bug del allocator quedara en un registro,
         *     el GC no lo veria -> corrupcion del heap.  En vez de arriesgarlo,
         *     hacemos FALLBACK seguro al path de slots.  Coste O(NV*calls),
         *     despreciable frente a encode/commit. */
        if (!ivs.call_positions.empty()) {
            for (uint32_t v = 0; v < mf.vreg_count; ++v) {
                const LiveInterval &lv = ivs.intervals[v];
                if (!lv.is_gc()) continue;
                for (uint32_t cp : ivs.call_positions) {
                    if (lv.covers(cp) && !ra.spilled(v)) {
#ifndef NDEBUG
                        std::fprintf(stderr,
                            "[vreg] GC root v%u vivo a traves de call no spilled "
                            "en '%s' -> fallback a slots\n", v, fn.name.c_str());
#endif
                        return nullptr;
                    }
                }
            }
        }

        /* 4. Rewrite a fisico (VM_ABI) + stackmaps de GC roots en cada CALL. */
        MFunction pf = rewrite_to_physical(mf, ra, tri, AbiKind::VM, &ivs);

        /* 3. Encode a bytes. */
        X86Encoder enc;
        std::vector<uint8_t> bytes;
        if (enc.encode(pf, bytes) == 0 || bytes.empty()) return nullptr;

        /* 4. Alojar en el code cache + commit (flush icache). */
        uint8_t *code = cc.alloc(bytes.size(), 16);
        if (!code) return nullptr;
        std::memcpy(code, bytes.data(), bytes.size());
        cc.commit(code, bytes.size());

        /* Disasm opt-in (VESTA_JIT_DISASM=1) del codigo vreg generado. */
        static const bool dis = []{
            const char *v = std::getenv("VESTA_JIT_DISASM");
            return v && v[0] != '\0' && v[0] != '0';
        }();
        if (dis) debug_dump_jit_code(fn.name + " [vreg]", code, bytes.size());

        /* 5. Registrar en el JitRegistry con los stackmaps reales (commit 6):
         *    describen los GC roots vivos a traves de cada CALL (en slots),
         *    para que el GC los escanee del stack durante un sweep. */
        JitRegistry::instance().register_function(
            code, code + bytes.size(), pf.stackmaps,
            static_cast<uint32_t>(8u * ra.num_spill_slots), "vreg");

        return code;
    }

    uint8_t *vreg_compile_osr(const ir::IrFunction &fn, CodeCache &cc,
                              const CallResolver &resolve_call,
                              const VregEntries &ent,
                              const CallResolver &resolve_native,
                              const CallResolver &resolve_symbol,
                              uint32_t header_block,
                              uint8_t **osr_entry_out,
                              const std::vector<uint32_t> *required_captures) {
        if (osr_entry_out) *osr_entry_out = nullptr;

        /* 1-3: identico a vreg_compile (selector + intervals + regalloc +
         * verificador adversarial de GC roots). */
        MFunction mf;
        if (!vreg_select(fn, mf, AbiKind::VM, resolve_call, ent, resolve_native,
                         resolve_symbol))
            return nullptr;
        const TargetRegInfo &tri = target_x86_64_vm_abi();
        IntervalResult ivs = build_intervals(mf, tri);
        RegAlloc ra = linear_scan(ivs, tri);
        if (!ivs.call_positions.empty()) {
            for (uint32_t vv = 0; vv < mf.vreg_count; ++vv) {
                const LiveInterval &lv = ivs.intervals[vv];
                if (!lv.is_gc()) continue;
                for (uint32_t cp : ivs.call_positions) {
                    if (lv.covers(cp) && !ra.spilled(vv)) return nullptr;
                }
            }
        }

        /* 4. Rewrite a fisico en modo OSR-ENTRY (suprime el trigger C1 y
         *    appendea el bloque OSR-entry para @p header_block). */
        OsrEmit osr;
        osr.mode = OsrEmit::C2_ENTRY;
        osr.header_block = static_cast<MBlockId>(header_block);
        osr.required_captures = required_captures;  // red de seguridad live-in
        MFunction pf = rewrite_to_physical(mf, ra, tri, AbiKind::VM, &ivs, &osr);
        if (!osr.osr_entry_valid) return nullptr;  // no se pudo emitir el entry

        /* 5. Encode. */
        X86Encoder enc;
        std::vector<uint8_t> bytes;
        if (enc.encode(pf, bytes) == 0 || bytes.empty()) return nullptr;

        /* 6. Alojar + commit. */
        uint8_t *code = cc.alloc(bytes.size(), 16);
        if (!code) return nullptr;
        std::memcpy(code, bytes.data(), bytes.size());
        cc.commit(code, bytes.size());

        /* 7. Resolver la direccion absoluta del OSR-entry via el offset que el
         *    encoder dejo en label_offsets. */
        if (osr.osr_entry_label < pf.label_offsets.size()
         && pf.label_offsets[osr.osr_entry_label] != UINT32_MAX) {
            if (osr_entry_out)
                *osr_entry_out = code + pf.label_offsets[osr.osr_entry_label];
        } else {
            return nullptr;  // offset no resuelto
        }

        static const bool dis = []{
            const char *v = std::getenv("VESTA_JIT_DISASM");
            return v && v[0] != '\0' && v[0] != '0';
        }();
        if (dis) debug_dump_jit_code(fn.name + " [vreg-osr]", code, bytes.size());

        /* 8. Registrar el blob C2 en el JitRegistry (stackmaps de sus CALLs). */
        JitRegistry::instance().register_function(
            code, code + bytes.size(), pf.stackmaps,
            static_cast<uint32_t>(8u * ra.num_spill_slots), "vreg-osr");

        return code;
    }

} // namespace jit
