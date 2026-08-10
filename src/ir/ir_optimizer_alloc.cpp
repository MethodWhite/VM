/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

#include "ir/ir_optimizer.h"
#include "ir/ir_optimizer_internal.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <queue>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <functional>
#include <sstream>
#include <algorithm>

namespace ir {

using ir::opt_internal::is_side_effecting;
using ir::opt_internal::is_terminator;
using ir::opt_internal::is_pure;
using ir::opt_internal::is_licm_hoistable_alloc;
using ir::opt_internal::strmake_reads_immutable;
using ir::opt_internal::is_pure_allocator_name;

bool ir_pass_dead_alloc_elim(IrFunction &fn) {
    /* Pasada 1: encontrar valores usados (mismo que DCE). */
    std::unordered_set<IrValueId> used;
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            for (IrValueId op : ins.operands) {
                if (op != IR_NO_VALUE) used.insert(op);
            }
            if ((ins.op == IrOp::CALLIND || ins.op == IrOp::CALLCLOSURE)
             && ins.func_ptr != IR_NO_VALUE) {
                used.insert(ins.func_ptr);
            }
            for (const auto &pa : ins.phi_args) {
                if (pa.value != IR_NO_VALUE) used.insert(pa.value);
            }
        }
    }

    /* Pasada 2: eliminar CALLs a allocators puros cuyo dst no se usa. */
    bool changed = false;
    for (auto &bb : fn.blocks) {
        auto &instrs = bb.instrs;
        size_t write = 0;
        for (size_t i = 0; i < instrs.size(); ++i) {
            const IrInstr &ins = instrs[i];
            bool keep = true;
            if (ins.op == IrOp::CALL
             && ins.dst != IR_NO_VALUE
             && !used.count(ins.dst)
             && !ins.preserve()
             && is_pure_allocator_name(ins.func_name)) {
                /* CALL a allocator puro, resultado no usado -> eliminar.
                 * El frontend Vex no espera efectos secundarios visibles
                 * de @c new X() salvo el handle/host_ptr (que se descarta). */
                keep = false;
                changed = true;
            }
            if (keep) {
                if (write != i) instrs[write] = std::move(instrs[i]);
                ++write;
            }
        }
        instrs.resize(write);
    }
    return changed;
}

// =========================================================================
//  Pase ir_pass_promote_callned_allocas
//
//  Phase D.jit-mem-model AUTO-PROMOTE: detecta `&local` (ALLOCAs) que
//  fluyen a CALLN (funciones nativas).  Esos ALLOCAs SE PROMUEVEN a host
//  stack via marca `is_host_ptr=true` en el dst del ALLOCA.  El JIT
//  selector consulta esa marca y emite host stack en lugar de VM-stack.
//
//  Sin esta promocion, &local seria una VM-addr que la funcion nativa
//  trataria como host_ptr -> garbage/crash.  Con la promocion, &local
//  es un host_ptr genuino dereferenciable directamente por code C.
//
//  Permite escribir codigo natural C-style:
//
//      u8[1024] buf;
//      ReadFile(handle, &buf[0], 1024, &bytes_read, null);
//
//  El frontend NO necesita anotaciones del usuario (@host etc).  El
//  analisis es backward-flow desde args PTR de cada CALLN.
//
//  Algoritmo:
//    1. Forward seed: por cada CALLN, marca todos sus operands como
//       "reaches_calln".
//    2. Backward fix-point: si dst ya marcado, propagar a operands a
//       traves de ADD/SUB/BITCAST/MOV/CAST/SEXT/ZEXT/TRUNC/PHI/LOAD.
//    3. Final: ALLOCAs cuyo dst esta marcado -> set is_host_ptr=true.
//       Tambien propagar is_host_ptr forward por la cadena de uses para
//       que LOAD/STORE de pointers derivados emita native mov.
// =========================================================================

bool ir_pass_promote_callned_allocas(IrFunction &fn) {
    if (fn.is_native) return false;
    if (fn.values.empty()) return false;

    /* Step 1: detectar valores que llegan a args de CALLN. */
    std::vector<bool> reaches_calln(fn.values.size(), false);
    bool found_any_calln = false;
    for (const auto &blk : fn.blocks) {
        for (const auto &ins : blk.instrs) {
            if (ins.op == IrOp::CALLN) {
                found_any_calln = true;
                for (auto opv : ins.operands) {
                    if (opv != IR_NO_VALUE && opv < reaches_calln.size()) {
                        reaches_calln[opv] = true;
                    }
                }
            }
        }
    }
    if (!found_any_calln) return false;

    /* Step 2: backward fix-point a traves de ops ptr-arithmetic. */
    bool changed_bp = true;
    int max_iter = 16;
    while (changed_bp && max_iter-- > 0) {
        changed_bp = false;
        for (const auto &blk : fn.blocks) {
            for (const auto &ins : blk.instrs) {
                if (ins.dst == IR_NO_VALUE) continue;
                if (ins.dst >= reaches_calln.size()) continue;
                if (!reaches_calln[ins.dst]) continue;
                /* Propagar a operands segun op. */
                switch (ins.op) {
                    case IrOp::ADD:
                    case IrOp::SUB:
                    case IrOp::BITCAST:
                    case IrOp::MOV:
                    case IrOp::CAST:
                    case IrOp::SEXT:
                    case IrOp::ZEXT:
                    case IrOp::TRUNC:
                    case IrOp::LOAD:
                        for (auto opv : ins.operands) {
                            if (opv != IR_NO_VALUE && opv < reaches_calln.size()
                             && !reaches_calln[opv]) {
                                reaches_calln[opv] = true;
                                changed_bp = true;
                            }
                        }
                        break;
                    case IrOp::PHI:
                        for (const auto &pa : ins.phi_args) {
                            if (pa.value != IR_NO_VALUE && pa.value < reaches_calln.size()
                             && !reaches_calln[pa.value]) {
                                reaches_calln[pa.value] = true;
                                changed_bp = true;
                            }
                        }
                        break;
                    default: break;
                }
            }
        }
    }

    /* Step 3: marcar ALLOCAs cuyo dst alcanza CALLN con
     * `host_alloca=true` y recolectar sus dsts para insertar
     * `RAW_FREE` antes de cada RET de la funcion.  El interp YA respeta
     * `host_alloca` en su bytecode emit: emite `alloc N` (RAW_ALLOC
     * bytecode) en lugar de `subsp`, y el `free` correspondiente lo
     * provee el RAW_FREE insertado aqui. */
    bool changed = false;
    std::vector<IrValueId> promoted_dsts;
    for (auto &blk : fn.blocks) {
        for (auto &ins : blk.instrs) {
            /* preserve = "no transformes esta instruccion".  Cualquier
             * consumidor del IR puede marcar con preserve un ALLOCA que debe
             * quedarse en vm_addr (p.ej. buffers que un helper nativo espera
             * como vaddr): respetarlo aqui evita promoverlo a host stack y
             * romper ese contrato. */
            if (ins.op == IrOp::ALLOCA
             && ins.dst != IR_NO_VALUE
             && ins.dst < reaches_calln.size()
             && reaches_calln[ins.dst]
             && !ins.host_alloca()
             && !ins.preserve()) {
                ins.set_host_alloca(true);
                promoted_dsts.push_back(ins.dst);
                changed = true;
            }
        }
    }

    /* Step 4 (post-leak-fix 2026-06-01): el cleanup en exit-points
     * ahora lo hace el runtime via `htrack` + cleanup automatico al
     * destruir el frame.  El bytecode emit del case ALLOCA emite
     * `htrack r_dst` tras el `alloc`; el frame guarda los ptrs en una
     * lista lazy y los libera en RET / do_throw / TAILCALL.
     *
     * Ventajas vs la version anterior (RAW_FREE inserted en IR):
     *   - Cubre THROW cross-frame correctamente (do_throw libera al
     *     pop frames durante unwind).
     *   - Cubre todos los exit paths sin enumerar IR ops (los exits
     *     del runtime son responsables del cleanup).
     *   - Sin transformaciones IR extra: el IR queda mas limpio. */

    /* Step 5: propagar is_host_ptr=true forward por las ops derivadas
     * (ADD/SUB/BITCAST/MOV/CAST/*EXT/TRUNC/PHI) desde el dst de cada
     * ALLOCA promovida.  Necesario para que LOAD/STORE downstream
     * emitan `movh` (host mem) en lugar de `mov` (vm mem).
     *
     * Ahora que el interp tambien respeta `host_alloca` y emite `alloc
     * N` (RAW_ALLOC bytecode) en lugar de `subsp` VM-stack, el ptr ES
     * host genuino: propagar es seguro y correcto tanto para interp
     * como para JIT. */
    if (!promoted_dsts.empty()) {
        /* Seed: marcar los dsts promovidos. */
        for (auto vid : promoted_dsts) {
            if (vid < fn.values.size()) {
                fn.values[vid].is_host_ptr = true;
            }
        }
        /* Fix-point forward propagation. */
        bool prop_changed = true;
        while (prop_changed) {
            prop_changed = false;
            for (auto &blk : fn.blocks) {
                for (auto &ins : blk.instrs) {
                    if (ins.dst == IR_NO_VALUE
                     || ins.dst >= fn.values.size()) continue;
                    auto &dst_v = fn.values[ins.dst];
                    if (dst_v.is_host_ptr) continue;
                    bool any_host = false;
                    auto check = [&](IrValueId v) {
                        if (v == IR_NO_VALUE
                         || v >= fn.values.size()) return;
                        if (fn.values[v].is_host_ptr) any_host = true;
                    };
                    switch (ins.op) {
                        case IrOp::ADD: case IrOp::SUB:
                        case IrOp::BITCAST: case IrOp::MOV:
                        case IrOp::CAST: case IrOp::SEXT:
                        case IrOp::ZEXT: case IrOp::TRUNC:
                            for (auto v : ins.operands) check(v);
                            break;
                        case IrOp::PHI:
                            for (auto &pa : ins.phi_args) check(pa.value);
                            break;
                        default: break;
                    }
                    if (any_host) {
                        dst_v.is_host_ptr = true;
                        prop_changed = true;
                    }
                }
            }
        }
    }

    return changed;
}

//==============================================================================
//  Sprint string-perf-8 (2026-06-02): ir_pass_promote_local_allocas
//
//  Promueve ALLOCAs LOCALES (no escapan al interp, no se pasan a CALL*,
//  no se almacenan en memoria heap) a `host_alloca=true`.  Esto permite
//  que el JIT emita `sub rsp, N` en host stack y los LOAD/STORE
//  derivados usen `mov [rbp+offset]` nativo (1 instr) en lugar del
//  inline page cache hit + fallback runtime call (~10 instr).
//
//  Caso tipico: struct value-type local (e.g. `Vec3 v = {1,2,3};` con
//  field access en hot loop).  bench_struct_field paga ~10 instrs por
//  cada `v.x` o `v.x = ...`; tras la promocion, 1 instr.
//
//  Algoritmo:
//    1. Recolectar ALLOCAs candidatas (no `host_alloca` ya, dst valido).
//    2. Por cada candidate, calcular el set transitivo `derived` via
//       forward-flow desde su dst a traves de ADD/SUB/BITCAST/MOV/etc.
//    3. Por cada uso del candidate o sus derivados, clasificar:
//       - SAFE: LOAD/STORE addr/ADD/SUB/CMP/BITCAST/MOV/CAST/etc.
//       - UNSAFE: CALL*/RET/THROW/TAILCALL si operand esta en derived;
//         STORE val (no addr) en derived implica escape.
//    4. Si TODOS los usos son SAFE, set host_alloca=true.
//
//  Diferencias con `ir_pass_promote_callned_allocas`:
//    - Aquel promueve para CALLN nativos (host_ptr REQUERIDO para
//      pasarlo a la fn nativa).  Este promueve por OPORTUNIDAD (host
//      stack es mas rapido que VM stack en JIT).
//    - Aquel marca ALLOCAs que ALCANZAN un CALLN.  Este marca ALLOCAs
//      que NO escapan a ningun sitio.
//==============================================================================

bool ir_pass_promote_local_allocas(IrFunction &fn) {
    if (fn.is_native) return false;
    if (fn.values.empty()) return false;

    /* Step 1: identificar ALLOCAs candidatas (no host_alloca ya). */
    std::vector<IrValueId> candidates;
    candidates.reserve(8);
    for (const auto &blk : fn.blocks) {
        for (const auto &ins : blk.instrs) {
            if (ins.op == IrOp::ALLOCA
             && ins.dst != IR_NO_VALUE
             && !ins.host_alloca()) {
                candidates.push_back(ins.dst);
            }
        }
    }
    if (candidates.empty()) return false;

    /* Step 2: forward-flow del conjunto "derivado" desde TODAS las
     * ALLOCAs candidatas.  Mantenemos una map vid -> origen (uno de los
     * candidates) para que el escape de UN candidato no contamine los
     * otros.
     *
     * Simplificacion: usamos un solo set "all_derived" + map dst->src.
     * Si un dst tiene multiple sources (PHI con args de distintos
     * candidates), conservativo: marcamos AMBOS como unsafe. */
    std::vector<int8_t> derived_from(fn.values.size(), -1);  /* -1 = no, >=0 = idx en candidates */
    std::vector<bool>   ambiguous(fn.values.size(), false);  /* derivado de >1 candidate */

    auto set_derived = [&](IrValueId v, int8_t origin) {
        if (v >= fn.values.size()) return false;
        if (derived_from[v] == -1) {
            derived_from[v] = origin;
            return true;
        }
        if (derived_from[v] != origin) {
            ambiguous[v] = true;
        }
        return false;
    };
    /* Seed: cada candidate es derived from itself. */
    for (size_t i = 0; i < candidates.size(); ++i) {
        set_derived(candidates[i], static_cast<int8_t>(i & 0x7F));
    }

    /* Propagacion forward.  Cota dura 16 iter para convergencia. */
    bool changed = true;
    int it = 16;
    while (changed && it-- > 0) {
        changed = false;
        for (const auto &blk : fn.blocks) {
            for (const auto &ins : blk.instrs) {
                if (ins.dst == IR_NO_VALUE || ins.dst >= fn.values.size()) continue;
                if (derived_from[ins.dst] >= 0) continue;  /* ya marcado */
                auto from_op = [&](IrValueId v) -> int {
                    if (v == IR_NO_VALUE || v >= fn.values.size()) return -1;
                    return derived_from[v];
                };
                switch (ins.op) {
                    case IrOp::ADD: case IrOp::SUB:
                    case IrOp::BITCAST: case IrOp::MOV:
                    case IrOp::CAST: case IrOp::SEXT:
                    case IrOp::ZEXT: case IrOp::TRUNC:
                        for (auto opv : ins.operands) {
                            int from = from_op(opv);
                            if (from >= 0) {
                                if (set_derived(ins.dst, static_cast<int8_t>(from)))
                                    changed = true;
                                break;
                            }
                        }
                        break;
                    case IrOp::PHI:
                        for (const auto &pa : ins.phi_args) {
                            int from = from_op(pa.value);
                            if (from >= 0) {
                                if (set_derived(ins.dst, static_cast<int8_t>(from)))
                                    changed = true;
                                break;
                            }
                        }
                        break;
                    default: break;
                }
            }
        }
    }

    /* Step 3: clasificar usos.  Para cada candidate, escape=true si
     * cualquier uso del candidate o sus derivados es UNSAFE.
     *
     * UNSAFE ops:
     *   - CALL/CALLVIRT/CALLM/CALLN/CALLIND/CALLCLOSURE: cualquier operand
     *     que sea derived es escape (el callee puede ser interp).
     *   - RET/THROW/TAILCALL: cualquier operand derived es escape.
     *   - STORE: si el VAL (operands[0]) es derived (pero NO el addr en
     *     operands[1]), escapa a memoria fuera del candidato.  Si addr
     *     ES derived, OK (es access local).
     *
     * SAFE ops sobre derived:
     *   - LOAD addr=derived: OK (lee del slot local).
     *   - STORE addr=derived val=non-derived: OK (escribe al slot local).
     *   - ADD/SUB/BITCAST/MOV/CAST/SEXT/ZEXT/TRUNC/PHI: ya tracked.
     *   - CMP: read-only.
     *   - ALLOCA itself: el propio seed.
     */
    /* uint8_t en lugar de bool para evitar std::vector<bool>::reference. */
    std::vector<uint8_t> escapes(candidates.size(), 0u);
    auto mark_escape = [&](IrValueId v) {
        if (v == IR_NO_VALUE || v >= derived_from.size()) return;
        if (derived_from[v] < 0) return;
        if (ambiguous[v]) {
            /* multiple candidates -> escapan TODOS por conservadurismo. */
            for (size_t k = 0; k < escapes.size(); ++k) escapes[k] = 1u;
            return;
        }
        int idx = derived_from[v];
        if (idx >= 0 && static_cast<size_t>(idx) < escapes.size()) {
            escapes[idx] = 1u;
        }
    };

    auto is_derived = [&](IrValueId v) -> bool {
        if (v == IR_NO_VALUE || v >= derived_from.size()) return false;
        return derived_from[v] >= 0;
    };

    /* Whitelist de SAFE ops: solo estas pueden tener operands derived
     * sin que el ptr "escape" del alcance JIT-local.  Cualquier OTRA op
     * (RAW_ASM, CALL*, GC_*, FINDCLASS, etc.) se trata como UNSAFE por
     * defecto (los operands derived marcan escape). */
    auto is_safe_op = [](IrOp op) -> bool {
        switch (op) {
            /* ALLOCA: seed.  No escapa por si misma. */
            case IrOp::ALLOCA:
            /* Aritmetica entera: produce nuevo valor, tracked via
             * derived_from. */
            case IrOp::ADD: case IrOp::SUB: case IrOp::MUL:
            case IrOp::DIV: case IrOp::MOD: case IrOp::NEG:
            case IrOp::AND: case IrOp::OR:  case IrOp::XOR: case IrOp::NOT:
            case IrOp::SHL: case IrOp::SHR: case IrOp::SAR:
            /* Casts: forward. */
            case IrOp::BITCAST: case IrOp::MOV:
            case IrOp::CAST: case IrOp::SEXT:
            case IrOp::ZEXT: case IrOp::TRUNC:
            /* CMP: read-only. */
            case IrOp::CMP_EQ: case IrOp::CMP_NE:
            case IrOp::CMP_LT: case IrOp::CMP_GT:
            case IrOp::CMP_LE: case IrOp::CMP_GE:
            case IrOp::CMP_ULT: case IrOp::CMP_UGT:
            case IrOp::CMP_ULE: case IrOp::CMP_UGE:
            /* Control flow: no escapa el ptr. */
            case IrOp::BR: case IrOp::BR_COND:
            case IrOp::NOP: case IrOp::CONST:
                return true;
            default:
                return false;
        }
    };

    for (const auto &blk : fn.blocks) {
        for (const auto &ins : blk.instrs) {
            if (ins.op == IrOp::LOAD) {
                /* LOAD addr=operands[0]: addr derived OK (lee slot).
                 * dst NO se considera derived del candidate (es el
                 * valor cargado de memoria, no el ptr). */
                continue;
            }
            if (ins.op == IrOp::STORE) {
                /* STORE val=operands[0], addr=operands[1].
                 * addr derived -> OK (escribe al slot).
                 * val derived -> ESCAPA (escribe el PTR a memoria). */
                if (ins.operands.size() >= 2 && is_derived(ins.operands[0])) {
                    mark_escape(ins.operands[0]);
                }
                continue;
            }
            if (ins.op == IrOp::PHI) {
                /* PHI: si el dst NO es derived pero phi_args SI lo son,
                 * los args "salen" del dominio local -> escape. */
                if (ins.dst < derived_from.size()
                 && derived_from[ins.dst] < 0) {
                    for (const auto &pa : ins.phi_args) mark_escape(pa.value);
                }
                continue;
            }
            if (is_safe_op(ins.op)) continue;

            /* UNSAFE op (CALL*, RAW_ASM, GC_*, FINDCLASS, RET, THROW, ...).
             * Cualquier operand derived escapa. */
            for (auto opv : ins.operands) mark_escape(opv);
            for (const auto &pa : ins.phi_args) mark_escape(pa.value);
        }
    }

    /* Step 4: promover ALLOCAs que NO escapan. */
    bool any_promoted = false;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (escapes[i]) continue;
        IrValueId v = candidates[i];
        for (auto &blk : fn.blocks) {
            for (auto &ins : blk.instrs) {
                if (ins.op == IrOp::ALLOCA && ins.dst == v && !ins.host_alloca()) {
                    ins.set_host_alloca(true);
                    /* NO explicit_free: el JIT libera con leave/ret;
                     * el interp con htrack + frame cleanup. */
                    any_promoted = true;
                    /* Propagar is_host_ptr al dst para que el dataflow
                     * de host_in_jit del JIT lo recoja. */
                    if (v < fn.values.size()) {
                        fn.values[v].is_host_ptr = true;
                    }
                }
            }
        }
    }
    if (!any_promoted) return false;

    /* Step 5: propagar is_host_ptr forward por las cadenas de derived. */
    bool prop = true; int p_it = 16;
    while (prop && p_it-- > 0) {
        prop = false;
        for (auto &blk : fn.blocks) {
            for (auto &ins : blk.instrs) {
                if (ins.dst == IR_NO_VALUE || ins.dst >= fn.values.size()) continue;
                auto &dv = fn.values[ins.dst];
                if (dv.is_host_ptr) continue;
                bool any_host = false;
                auto chk = [&](IrValueId v) {
                    if (v != IR_NO_VALUE && v < fn.values.size()
                     && fn.values[v].is_host_ptr) any_host = true;
                };
                switch (ins.op) {
                    case IrOp::ADD: case IrOp::SUB:
                    case IrOp::BITCAST: case IrOp::MOV:
                    case IrOp::CAST: case IrOp::SEXT:
                    case IrOp::ZEXT: case IrOp::TRUNC:
                        for (auto opv : ins.operands) { chk(opv); if (any_host) break; }
                        break;
                    case IrOp::PHI:
                        for (const auto &pa : ins.phi_args) { chk(pa.value); if (any_host) break; }
                        break;
                    default: break;
                }
                if (any_host) {
                    dv.is_host_ptr = true;
                    prop = true;
                }
            }
        }
    }
    return true;
}


//==============================================================================
//  Pase ir_pass_promote_local_raw_alloc
//
//  Convierte `malloc(N_const) + ... + free(p)` locales sin escape a un
//  `ALLOCA host_alloca`.  El JIT selector emite `sub rsp, N` (host stack);
//  el RAW_FREE correspondiente se reemplaza por NOP porque el stack se
//  libera automaticamente al exit de la funcion (mov rsp, rbp + pop rbp).
//
//  Beneficio: malloc/free de ~200-500 ns por iter en hot loops -> ~1 ns
//  (sub/add rsp).  Speedup del alloc puro ~100-500x.
//==============================================================================

bool ir_pass_promote_local_raw_alloc(IrFunction &fn) {
    if (fn.is_native) return false;
    if (fn.values.empty()) return false;

    constexpr uint64_t MAX_PROMOTE_SIZE = 65536; // bytes

    // Collect RAW_ALLOC candidates (CONST size, size razonable).
    struct AllocSite {
        size_t       block_idx;
        size_t       ins_idx;
        IrValueId    dst;
        uint64_t     size_bytes;
    };
    std::vector<AllocSite> candidates;

    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto &blk = fn.blocks[bi];
        for (size_t ii = 0; ii < blk.instrs.size(); ++ii) {
            const auto &ins = blk.instrs[ii];
            if (ins.op != IrOp::RAW_ALLOC) continue;
            if (ins.operands.empty()) continue;
            if (ins.dst == IR_NO_VALUE) continue;
            const IrValueId size_vid = ins.operands[0];
            if (size_vid >= fn.values.size()) continue;
            const auto &size_v = fn.values[size_vid];
            if (!size_v.is_const) continue;
            const uint64_t bytes = size_v.const_val;
            if (bytes == 0 || bytes > MAX_PROMOTE_SIZE) continue;
            candidates.push_back({bi, ii, ins.dst, bytes});
        }
    }
    if (candidates.empty()) return false;

    // Para cada candidato, hacer escape analysis:
    //   - forward fix-point: marcar valores derivados del dst.
    //   - rechazar si algun derivado llega a un uso prohibido (RETURN,
    //     STORE a memoria GC, CALL externo distinto del RAW_FREE).
    bool changed = false;
    for (auto &c : candidates) {
        // derivados[v] = true si v puede ser un alias del dst (forward
        // flow desde el RAW_ALLOC dst a traves de ADD/SUB/BITCAST/MOV/
        // CAST/*EXT/TRUNC/PHI).
        std::vector<bool> derivados(fn.values.size(), false);
        derivados[c.dst] = true;
        bool prop = true;
        int iters = 0;
        while (prop && iters++ < 32) {
            prop = false;
            for (const auto &blk : fn.blocks) {
                for (const auto &ins : blk.instrs) {
                    if (ins.dst == IR_NO_VALUE) continue;
                    if (ins.dst >= derivados.size()) continue;
                    if (derivados[ins.dst]) continue;
                    auto check = [&](IrValueId v) -> bool {
                        return v != IR_NO_VALUE
                            && v < derivados.size()
                            && derivados[v];
                    };
                    bool any_d = false;
                    switch (ins.op) {
                        case IrOp::ADD:    case IrOp::SUB:
                        case IrOp::BITCAST:case IrOp::MOV:
                        case IrOp::CAST:   case IrOp::SEXT:
                        case IrOp::ZEXT:   case IrOp::TRUNC:
                            for (auto v : ins.operands) if (check(v)) { any_d = true; break; }
                            break;
                        case IrOp::PHI:
                            for (auto &pa : ins.phi_args) if (check(pa.value)) { any_d = true; break; }
                            break;
                        default: break;
                    }
                    if (any_d) {
                        derivados[ins.dst] = true;
                        prop = true;
                    }
                }
            }
        }

        // Escape check: contar RAW_FREEs que reciben un derivado, y
        // rechazar si hay usos prohibidos.
        bool escapes = false;
        std::vector<std::pair<size_t,size_t>> free_sites;  // (block_idx, ins_idx)

        auto is_derived = [&](IrValueId v) -> bool {
            return v != IR_NO_VALUE
                && v < derivados.size()
                && derivados[v];
        };

        for (size_t bi = 0; bi < fn.blocks.size() && !escapes; ++bi) {
            const auto &blk = fn.blocks[bi];
            for (size_t ii = 0; ii < blk.instrs.size() && !escapes; ++ii) {
                const auto &ins = blk.instrs[ii];

                switch (ins.op) {
                    case IrOp::RET:
                    case IrOp::TAILCALL:
                    case IrOp::RSPAWN_RETURN:
                    case IrOp::FULFILL_HLT:
                        // El ptr llega a return / tailcall / fulfill -> escapa.
                        for (auto v : ins.operands) {
                            if (is_derived(v)) { escapes = true; break; }
                        }
                        break;

                    case IrOp::RAW_FREE:
                        // Free directo: marca el site para eliminar
                        // si la promocion procede.  NO marca escape.
                        if (!ins.operands.empty() && is_derived(ins.operands[0])) {
                            free_sites.push_back({bi, ii});
                        }
                        break;

                    case IrOp::STORE: {
                        // STORE val, addr.  Si val es derivado y la addr
                        // apunta a memoria GC o globals, ESCAPA.  Si addr
                        // es local (otro ALLOCA / RAW_ALLOC del mismo
                        // scope), tracking conservativo: marcar escape
                        // SOLO si la addr es is_host_ptr=false (= memoria
                        // GC) o si el target es un global.
                        //
                        // MVP conservativo: cualquier STORE de un derivado
                        // a una direccion que NO sea otro derivado del
                        // mismo ALLOCA cuenta como escape.
                        if (ins.operands.size() >= 2) {
                            const IrValueId val_v = ins.operands[0];
                            const IrValueId addr_v = ins.operands[1];
                            if (is_derived(val_v) && !is_derived(addr_v)) {
                                escapes = true;
                            }
                        }
                        break;
                    }

                    case IrOp::CALL:
                    case IrOp::CALLVIRT:
                    case IrOp::CALLM:
                    case IrOp::CALLITF:
                    case IrOp::CALLIND:
                    case IrOp::CALLCLOSURE:
                    case IrOp::CALLN: {
                        // El ptr pasado a otra fn ESCAPA conservativamente.
                        // Excepcion: si fuera el callee es de tipo "trampoline
                        // pure" (no toca el ptr), seria seguro -- pero no
                        // tenemos esa info aqui.  Conservador: escape.
                        for (auto v : ins.operands) {
                            if (is_derived(v)) { escapes = true; break; }
                        }
                        if (ins.func_ptr != IR_NO_VALUE && is_derived(ins.func_ptr)) {
                            escapes = true;
                        }
                        break;
                    }

                    case IrOp::THROW:
                        // El ptr llega a throw -> escapa cross-frame.
                        for (auto v : ins.operands) {
                            if (is_derived(v)) { escapes = true; break; }
                        }
                        break;

                    default: break;
                }
            }
        }

        if (escapes) continue;
        if (free_sites.empty()) continue;  // sin free -> es leak, no promovemos

        // Promote: convertir RAW_ALLOC en ALLOCA con host_alloca=true.
        // El bytecode emitter ya respeta host_alloca y emite el path
        // de `alloc N + htrack` (que el runtime libera al RET del frame
        // automaticamente).  El JIT selector emite `sub rsp, N` (host
        // stack directo, sin allocator).
        auto &alloc_ins = fn.blocks[c.block_idx].instrs[c.ins_idx];
        alloc_ins.op = IrOp::ALLOCA;
        alloc_ins.imm = c.size_bytes;
        alloc_ins.type = IrType::I8;  // ALLOCA convencion: type=I8, imm=N bytes
        alloc_ins.set_host_alloca(true);
        alloc_ins.operands.clear();  // ALLOCA no toma operands (tamano en imm)

        // Marcar el dst como is_host_ptr para que LOAD/STORE emitan movh.
        if (c.dst < fn.values.size()) {
            fn.values[c.dst].is_host_ptr = true;
        }

        // Sprint mem-loop-fix (2026-06-02): PRESERVAR los RAW_FREE
        // explicitos en lugar de eliminarlos.  Bottleneck encontrado
        // en bench mem_malloc_free: el path original eliminaba RAW_FREE
        // y dependia de `host_alloca_release_all` al RET del frame,
        // pero si el ALLOCA esta DENTRO de un loop (inlineado o no),
        // el vector @c host_allocas del frame acumula N punteros
        // tracked sin liberar -- O(N) memoria + O(N) cleanup al RET.
        //
        // Con el RAW_FREE preservado, el alloc/free emparejan
        // correctamente DENTRO de cada iteracion del loop.  El ALLOCA
        // se marca con @c host_alloca_explicit_free=true para que el
        // bytecode emit del interp SKIPE el `htrack` (porque el free
        // explicito ya libera el ptr en su sitio).
        alloc_ins.set_host_alloca_explicit_free(true);
        // NO eliminar los RAW_FREE: dejarlos para que el bytecode emit
        // los convierta en `free` opcodes correctamente.

        changed = true;
    }

    // Compactar bloques: eliminar instrucciones NOP introducidas por
    // este pass.  El bytecode emitter de NOP emite `nop1` que NO esta
    // soportado correctamente por el decoder (decode_fn=nullptr).
    // Mas seguro eliminar fisicamente las RAW_FREE convertidas.
    if (changed) {
        for (auto &blk : fn.blocks) {
            auto &is = blk.instrs;
            is.erase(std::remove_if(is.begin(), is.end(), [](const IrInstr &i) {
                return i.op == IrOp::NOP && i.operands.empty() && i.dst == IR_NO_VALUE;
            }), is.end());
        }
    }

    return changed;
}

//==============================================================================
//  Phase C2.13: Escape Analysis + Scalar Replacement de objetos GC
//
//  Detecta objetos `new X(...)` (emitidos como `call @__new_X(args)`) que NO
//  ESCAPAN del frame en el que se crean: su host_ptr solo se usa para leer/
//  escribir campos locales, nunca se retorna, almacena en memoria heap, ni
//  pasa a otra funcion.  Un objeto asi puede materializarse SIN tocar el GC
//  heap (scalar replacement): el alloc se elimina y los `load (obj+off)` se
//  reemplazan por el valor con el que el ctor inicializo ese campo.
//
//  ADVERTENCIA DE SEGURIDAD: marcar como no-escapante un objeto que SI escapa
//  produce use-after-free / corrupcion de heap silenciosa.  El analisis es
//  CONSERVADOR por diseno: asume que el objeto escapa salvo prueba explicita.
//  La clasificacion de usos reusa exactamente el patron validado de
//  @c ir_pass_promote_local_allocas (whitelist de safe-ops; cualquier op no
//  reconocida marca escape).
//
//  Esta primera parte implementa SOLO la DETECCION (log-only via la env var
//  VESTA_ESCAPE_DEBUG).  La transformacion (scalar replacement) se construye
//  encima del mismo analisis una vez validada la deteccion.
//==============================================================================


} // namespace ir
