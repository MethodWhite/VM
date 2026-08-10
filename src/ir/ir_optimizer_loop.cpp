/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

#include "ir/ir_optimizer.h"
#include "ir/ir_optimizer_internal.h"
#include "ir/passes/unroll.h"
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

bool ir_pass_inline_loop_header(IrFunction &fn) {
    bool changed = false;
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        IrBlock &B = fn.blocks[bi];
        if (B.instrs.empty()) continue;
        IrInstr &term = B.instrs.back();
        if (term.op != IrOp::BR) continue; // solo BR incondicional
        IrBlockId hid = term.target_block;
        if (hid >= fn.blocks.size() || hid == static_cast<IrBlockId>(bi)) continue;
        IrBlock &H = fn.blocks[hid];
        // H debe terminar en BR_COND.  Las instrs pueden incluir CONSTs
        // literales que materializan valores usados solo por el CMP final.
        // Patron tipico: %k = const.i64 0; %z = cmp.ne.bool %x, %k; br.cond %z, T, F.
        // Tambien permitimos el CMP en penultima posicion con 0..N CONSTs antes.
        if (H.instrs.size() < 2) continue;
        const IrInstr &h_last = H.instrs.back();
        if (h_last.op != IrOp::BR_COND) continue;
        // Buscar el CMP penultimo (h_last - 1) o anterior.  Las instrs
        // entre el CMP y el BR_COND deben ser CONST puros (sin side effects).
        size_t cmp_idx = H.instrs.size() - 2;
        const IrInstr &h_cmp = H.instrs[cmp_idx];
        bool is_cmp = (h_cmp.op == IrOp::CMP_EQ  || h_cmp.op == IrOp::CMP_NE
                    || h_cmp.op == IrOp::CMP_LT  || h_cmp.op == IrOp::CMP_GT
                    || h_cmp.op == IrOp::CMP_LE  || h_cmp.op == IrOp::CMP_GE
                    || h_cmp.op == IrOp::CMP_ULT || h_cmp.op == IrOp::CMP_UGT
                    || h_cmp.op == IrOp::CMP_ULE || h_cmp.op == IrOp::CMP_UGE);
        if (!is_cmp) continue;
        // Verificar que las instrs antes del CMP sean todas CONST (puras).
        bool only_consts = true;
        for (size_t k = 0; k < cmp_idx; ++k) {
            if (H.instrs[k].op != IrOp::CONST) { only_consts = false; break; }
        }
        if (!only_consts) continue;
        // El cmp.dst debe ser el unico operand del BR_COND.
        if (h_last.operands.empty() || h_last.operands[0] != h_cmp.dst) continue;
        // Contar predecesores de H.
        int preds = 0;
        for (size_t pi = 0; pi < fn.blocks.size(); ++pi) {
            if (fn.blocks[pi].instrs.empty()) continue;
            const IrInstr &pterm = fn.blocks[pi].instrs.back();
            if (pterm.op == IrOp::BR && pterm.target_block == hid) ++preds;
            else if (pterm.op == IrOp::BR_COND
                  && (pterm.target_block == hid || pterm.false_block == hid))
                ++preds;
        }
        if (preds != 1) continue; // mas de 1 pred o 0 -> no fusionar
        // No tocar entry block: si H es entry, no podemos fusionarlo
        // (entry no tiene predecesores; ya filtrado por preds!=1, pero
        // doble check defensivo).
        if (hid == 0) continue;
        // El header NO debe tener PHI nodes (el primer instr debe ser CMP, no PHI).
        // Ya implicito en H.instrs.size() == 2 con h0 = CMP.

        // Aplicar la fusion:
        //   1. Eliminar BR de B.
        //   2. Append todas las instrs de H (CONSTs + CMP + BR_COND) al final de B.
        //   3. Hoist de CONSTs: mover todas las IrOp::CONST justo despues de
        //      las PHI nodes (CONSTs son puros, su orden es irrelevante para
        //      la semantica).  Esto agrupa los CONSTs y deja a SUB+CMP+BR_COND
        //      consecutivos en el final, habilitando peepholes como decjnz.
        //   4. Limpiar H (queda inalcanzable, lo barren los demas pases).
        //   5. Reescribir phi_args en sucesores de BR_COND: ref a H debe pasar a B.
        IrBlockId t_true  = h_last.target_block;
        IrBlockId t_false = h_last.false_block;

        std::vector<IrInstr> moved;
        moved.reserve(H.instrs.size());
        for (auto &ins : H.instrs) moved.push_back(std::move(ins));
        B.instrs.pop_back(); // remover BR de B
        for (auto &ins : moved) B.instrs.push_back(std::move(ins));
        H.instrs.clear();

        // Hoist de CONSTs: estabilizamos el orden en B asi:
        //   [PHIs...] [CONSTs...] [resto en orden original]
        // Stable_partition mantiene el orden relativo de cada grupo.  Los
        // CONSTs no tienen side effects ni dependen de instrucciones
        // anteriores (su unico operand_id es @c imm), asi que moverlos
        // hacia adelante no rompe SSA dominance: si un CONST se usaba a
        // X, ahora esta definido aun antes que X.
        {
            std::vector<IrInstr> phis, consts, rest;
            phis.reserve(4); consts.reserve(8); rest.reserve(B.instrs.size());
            for (auto &ins : B.instrs) {
                if (ins.op == IrOp::PHI)        phis.push_back(std::move(ins));
                else if (ins.op == IrOp::CONST) consts.push_back(std::move(ins));
                else                            rest.push_back(std::move(ins));
            }
            B.instrs.clear();
            B.instrs.reserve(phis.size() + consts.size() + rest.size());
            for (auto &ins : phis)   B.instrs.push_back(std::move(ins));
            for (auto &ins : consts) B.instrs.push_back(std::move(ins));
            for (auto &ins : rest)   B.instrs.push_back(std::move(ins));
        }

        auto rewrite_phi_block_ref = [&](IrBlockId target_id) {
            if (target_id >= fn.blocks.size()) return;
            IrBlock &T = fn.blocks[target_id];
            for (auto &ins : T.instrs) {
                if (ins.op != IrOp::PHI) break;
                for (auto &pa : ins.phi_args) {
                    if (pa.block == hid) pa.block = static_cast<IrBlockId>(bi);
                }
            }
        };
        rewrite_phi_block_ref(t_true);
        rewrite_phi_block_ref(t_false);

        changed = true;
        // No avanzar bi: re-procesar B porque puede haber cadena
        // (B -> H1 -> H2 inlinable transitivamente).
        --bi;
    }
    return changed;
}

// =========================================================================
//  Pase ir_pass_inline
// =========================================================================
//
// Function inlining a nivel modulo.  Para cada CALL site, si el callee
// cumple las heuristicas de inlineabilidad, sustituye la CALL con el
// cuerpo del callee, renombrando SSA values + bloques.
//
// = v1: Single-block callees =
//
// El callee debe tener exactamente 1 bloque que termine con RET (o no
// terminar si vacio).  Esto cubre casos muy comunes:
//   - Wrappers triviales: `i32 f() { return 0; }`
//   - Getters: `i32 get_x(T this) { return this.x; }`
//   - Helpers pequenos sin control de flujo.
//
// Multi-block callees y recursion requieren CFG manipulation mas
// compleja (CFG merge + phi insertion) -- deferred a v2.
//
// = Heuristica =
//
// Inlineable si:
//   - Callee esta definido en nuestro IrModule (no @c is_native).
//   - Callee tiene 1 bloque exactamente y termina con RET.
//   - Body del callee tiene < INLINE_THRESHOLD instrucciones.
//   - Callee NO es el caller (no self-inline).
//
// = Beneficios =
//
// JIT: skip CALL/RET overhead, regalloc puede ver mas contexto,
// const-fold se propaga a traves de la call.  Ejemplo: pruebas() en
// 100_reflection_full despues de dead-alloc elim queda como
// `return 0`.  Si se inline en main, la asignacion a val es trivial
// y el loop body se reduce a la comparacion + incremento.
//
// port-C: codigo destino mas legible, sin auxiliary functions ni
// goto-style returns.

bool ir_pass_inline(IrModule &mod, size_t threshold) {
    /* Threshold de tamano del body del callee para inlinar.
     *
     * Por que 12 por defecto (en lugar de 8 o 16): el overhead del CALLVM (push
     * regs vivos, mov r1..rN args, callvm, pop regs, mov dst r0) en el peor caso
     * son ~24 instrucciones VM.  Cualquier callee cuyo cuerpo cabe en menos de
     * eso es candidato directo a inline ya que ahorramos mas de lo que crece el
     * caller.  12 es el balance que captura getters, setters y helpers
     * aritmeticos pequenos sin causar bloat material en el .velb de programas
     * tipicos.  El C2/OSR pasa un threshold mayor para inlinear las CALLs de un
     * loop CALIENTE (el code-size no importa cuando el loop domina el tiempo). */
    const size_t INLINE_THRESHOLD = threshold;
    bool changed = false;

    /* Build name -> index map. */
    std::unordered_map<std::string, size_t> name_to_idx;
    for (size_t i = 0; i < mod.functions.size(); ++i) {
        name_to_idx[mod.functions[i].name] = i;
    }

    /* Funciones que NUNCA se deben inlinear (entry points alternativos
     * o sintetic functions con calling conventions especiales). */
    auto is_blacklisted = [](const std::string &name) -> bool {
        /* __module_init: el Loader lo invoca via init_pc, no es un CALL
         * normal.  Inlinearlo en main duplica el defclass + deffield
         * + defmethod del bytecode. */
        if (name == "__module_init") return true;
        /* Lambda helpers: invocados via function pointer en CALLCLOSURE.
         * Sus IR son single-block + RET pero el calling convention es
         * distinta (env_addr en r14, etc). */
        if (name.size() > 9 && name.compare(0, 9, "__lambda_") == 0) return true;
        /* Spawn helpers: invocados por SPAWN op, no por CALL. */
        if (name.size() > 8 && name.compare(0, 8, "__spawn_") == 0) return true;
        /* Async helpers: invocados por @Async machinery. */
        if (name.size() > 8 && name.compare(0, 8, "__async_") == 0) return true;
        /* rspawn body helpers. */
        if (name.size() > 9 && name.compare(0, 9, "__rspawn_") == 0) return true;
        return false;
    };

    /* Pre-classify cada function: es inlineable? */
    auto is_inlineable = [&](const IrFunction &fn) -> bool {
        if (fn.is_native) return false;
        if (is_blacklisted(fn.name)) return false;
        if (fn.blocks.size() != 1) return false;
        if (fn.blocks[0].instrs.empty()) return false;
        /* Ultima instr debe ser RET. */
        const auto &last = fn.blocks[0].instrs.back();
        if (last.op != IrOp::RET) return false;
        if (fn.blocks[0].instrs.size() > INLINE_THRESHOLD) return false;
        /* No inlinear funciones que contengan CALLs recursivas a si mismas. */
        for (const auto &ins : fn.blocks[0].instrs) {
            if ((ins.op == IrOp::CALL || ins.op == IrOp::TAILCALL)
             && ins.func_name == fn.name) {
                return false;
            }
        }
        /* No inlinear funciones que tengan @c RAW_ASM en su body cuando
         * el RAW_ASM podria depender del calling convention especifico
         * de la callee.  Conservadoramente: skip si hay raw_asm. */
        for (const auto &ins : fn.blocks[0].instrs) {
            if (ins.op == IrOp::RAW_ASM) return false;
        }
        return true;
    };

    /* Cache de classification. */
    std::vector<bool> can_inline(mod.functions.size(), false);
    for (size_t i = 0; i < mod.functions.size(); ++i) {
        can_inline[i] = is_inlineable(mod.functions[i]);
    }

    for (size_t fi = 0; fi < mod.functions.size(); ++fi) {
        IrFunction &caller = mod.functions[fi];
        if (caller.is_native) continue;

        for (auto &bb : caller.blocks) {
            /* Procesar in-place; recolectar lista de cambios primero
             * para no invalidar iterators. */
            std::vector<IrInstr> new_instrs;
            new_instrs.reserve(bb.instrs.size());

            for (size_t i = 0; i < bb.instrs.size(); ++i) {
                IrInstr &ins = bb.instrs[i];
                if (ins.op != IrOp::CALL) {
                    new_instrs.push_back(std::move(ins));
                    continue;
                }
                /* CALL a function user.  Verificar si el callee esta
                 * en el modulo y es inlineable. */
                auto it = name_to_idx.find(ins.func_name);
                if (it == name_to_idx.end() || it->second == fi
                 || !can_inline[it->second]) {
                    new_instrs.push_back(std::move(ins));
                    continue;
                }
                const IrFunction &callee = mod.functions[it->second];
                const IrBlock &cbody = callee.blocks[0];
                /* Aridad must match: params.size() == operands.size(). */
                if (callee.params.size() != ins.operands.size()) {
                    new_instrs.push_back(std::move(ins));
                    continue;
                }

                /* Mapeo callee_vid -> caller_vid. */
                std::unordered_map<IrValueId, IrValueId> vmap;
                /* Params del callee se mapean a operandos del CALL. */
                for (size_t pi = 0; pi < callee.params.size(); ++pi) {
                    vmap[callee.params[pi]] = ins.operands[pi];
                }

                /* Helper: para cada SSA value que el callee DEFINE,
                 * allocar fresh en el caller. */
                auto remap_dst = [&](IrValueId cvid, IrType type,
                                     const std::string &name_hint) -> IrValueId {
                    if (cvid == IR_NO_VALUE) return IR_NO_VALUE;
                    auto vit = vmap.find(cvid);
                    if (vit != vmap.end()) return vit->second;
                    const IrValueId new_vid = static_cast<IrValueId>(caller.values.size());
                    IrValue nv{};
                    nv.id   = new_vid;
                    nv.type = type;
                    nv.name = "%inl_" + std::to_string(new_vid);
                    (void)name_hint;
                    if (cvid < callee.values.size()) {
                        const auto &cv = callee.values[cvid];
                        nv.is_const          = cv.is_const;
                        nv.const_val         = cv.const_val;
                        nv.is_host_ptr       = cv.is_host_ptr;
                        nv.pointee_is_host_ptr = cv.pointee_is_host_ptr;
                        nv.is_gc_object      = cv.is_gc_object;
                        nv.narrow_only       = cv.narrow_only;
                    }
                    caller.values.push_back(nv);
                    vmap[cvid] = new_vid;
                    return new_vid;
                };

                auto remap_op = [&](IrValueId cvid) -> IrValueId {
                    if (cvid == IR_NO_VALUE) return IR_NO_VALUE;
                    auto vit = vmap.find(cvid);
                    if (vit != vmap.end()) return vit->second;
                    /* Valor del callee que no fue param ni dst previo.
                     * Esto no deberia pasar si procesamos en orden. */
                    return IR_NO_VALUE;
                };

                /* Replicar todas las instrs del callee EXCEPTO el RET final. */
                IrValueId ret_value = IR_NO_VALUE;
                bool inline_ok = true;
                for (const auto &c_ins : cbody.instrs) {
                    if (c_ins.op == IrOp::RET) {
                        if (!c_ins.operands.empty()) {
                            ret_value = remap_op(c_ins.operands[0]);
                        }
                        continue;  /* skip RET; ret value resolved */
                    }
                    /* Clonar c_ins y remap operandos + dst. */
                    IrInstr ni = c_ins;
                    /* dst: si tiene resultado, mapear a nuevo VID en caller. */
                    if (ni.dst != IR_NO_VALUE) {
                        const IrType dst_type = (ni.dst < callee.values.size())
                            ? callee.values[ni.dst].type : ni.type;
                        ni.dst = remap_dst(ni.dst, dst_type, "");
                    }
                    /* operands. */
                    for (auto &op : ni.operands) {
                        op = remap_op(op);
                    }
                    /* func_ptr (CALLIND/CALLCLOSURE). */
                    if (ni.func_ptr != IR_NO_VALUE) {
                        ni.func_ptr = remap_op(ni.func_ptr);
                    }
                    /* phi_args: callee es single-block, no phis razonables.
                     * Si los hay, skip inline. */
                    if (!ni.phi_args.empty()) {
                        inline_ok = false;
                        break;
                    }
                    new_instrs.push_back(std::move(ni));
                }

                if (!inline_ok) {
                    /* Cancelar el inline: revertir lo que anyadimos.
                     * Conservativo: push el CALL original. */
                    /* Quitar las instrs recien anyadidas relacionadas con inline.
                     * Para simplicidad: NO retroceder; quedaria un mix
                     * incorrecto.  Marcar y emit CALL original al final. */
                    /* Reset estrategia: para evitar IR corrupto, hacemos un
                     * passthrough simple: no haber comenzado a anyadir.
                     * Como ya empezamos, no podemos limpiar facilmente.
                     * Por seguridad: rebuild new_instrs desde scratch
                     * usando bb.instrs[0..i]. */
                    new_instrs.clear();
                    for (size_t k = 0; k <= i; ++k) {
                        new_instrs.push_back(bb.instrs[k]);
                    }
                    continue;
                }

                /* Emitir el "resultado" del inline: si el CALL tenia dst,
                 * MOV dst <- ret_value. */
                if (ins.dst != IR_NO_VALUE && ret_value != IR_NO_VALUE) {
                    IrInstr mv{};
                    mv.op = IrOp::MOV;
                    mv.type = ins.type;
                    mv.dst = ins.dst;
                    mv.operands = {ret_value};
                    new_instrs.push_back(std::move(mv));
                } else if (ins.dst != IR_NO_VALUE) {
                    /* Callee no retorno valor pero CALL expected uno.
                     * Conservativo: CONST 0 placeholder. */
                    IrInstr cz{};
                    cz.op = IrOp::CONST;
                    cz.type = ins.type;
                    cz.dst = ins.dst;
                    cz.imm = 0;
                    new_instrs.push_back(std::move(cz));
                }

                changed = true;
            }
            bb.instrs = std::move(new_instrs);
        }
    }
    return changed;
}

// =========================================================================
//  Pase ir_pass_licm
// =========================================================================
//
// Loop-Invariant Code Motion.  Para cada loop simple detectado, mueve
// instrucciones cuyos operandos son TODOS invariantes (definidos fuera
// del loop O constantes) al predecesor del loop header.
//
// Beneficio: las invariantes se calculan UNA vez (en el predecesor) en
// vez de N veces (en cada iteracion del loop).  Tipico para patrones
// como `for(i; i<size; i++)` donde `size` es invariant.
//
// = v1: simple algorithm =
//
// 1. Detectar loops via back-edges (JMP/BR_COND a un bloque ANTERIOR
//    en orden lineal o via dataflow simple).
// 2. Para cada loop header H y back-edge from B:
//    - Conjunto de bloques en el loop: { H, ..., B } (BFS desde B
//      hasta H siguiendo predecesores).
//    - Pre-header: bloque que cae a H pero NO esta en el loop.
//      Si hay multiples, abortar (necesita split, deferred).
// 3. Para cada instr en el loop:
//    - Si pure_op AND todos los operands son CONSTs o definidos fuera
//      del loop AND la instr no es phi AND no tiene side effects:
//      Mover al pre-header.
//
// v1 conservativo: solo loops simples con UN solo back-edge y UN pre-
// header (case clasico while/for).

bool ir_pass_licm(IrFunction &fn) {
    if (fn.blocks.size() < 3) return false;  /* necesita pre-header + body + header */
    const size_t N = fn.blocks.size();

    /* Construir CFG: para cada bloque, sus sucesores y predecesores. */
    std::vector<std::vector<IrBlockId>> preds(N);
    std::vector<std::vector<IrBlockId>> succs(N);
    for (size_t b = 0; b < N; ++b) {
        const auto &bb = fn.blocks[b];
        if (bb.instrs.empty()) continue;
        const auto &last = bb.instrs.back();
        IrBlockId t1 = IR_NO_BLOCK, t2 = IR_NO_BLOCK;
        if (last.op == IrOp::BR) {
            t1 = last.target_block;
        } else if (last.op == IrOp::BR_COND) {
            t1 = last.target_block;
            t2 = last.false_block;
        }
        if (t1 != IR_NO_BLOCK && t1 < N) {
            preds[t1].push_back(static_cast<IrBlockId>(b));
            succs[b].push_back(t1);
        }
        if (t2 != IR_NO_BLOCK && t2 < N) {
            preds[t2].push_back(static_cast<IrBlockId>(b));
            succs[b].push_back(t2);
        }
    }

    /* Dominadores via Cooper-Harvey-Kennedy iterativo.
     * dom[entry] = entry, otros = UNDEF.  Procesar en reverse postorder
     * hasta punto fijo.  intersect(b1, b2) sube en el dom-tree hasta
     * encontrar el ancestro comun mas cercano. */
    const IrBlockId UNDEF = static_cast<IrBlockId>(N);
    const IrBlockId entry = 0;  /* convencion: bloque 0 es entry */

    /* DFS para reverse postorder. */
    std::vector<IrBlockId> rpo;
    rpo.reserve(N);
    {
        std::vector<bool> visited(N, false);
        std::function<void(IrBlockId)> dfs = [&](IrBlockId b) {
            if (b >= N || visited[b]) return;
            visited[b] = true;
            for (IrBlockId s : succs[b]) dfs(s);
            rpo.push_back(b);
        };
        dfs(entry);
        std::reverse(rpo.begin(), rpo.end());
    }
    /* rpo_pos[b] = posicion de b en rpo (mayor = mas adelante = mas alto).
     * Usado por intersect_dom.  Bloques no alcanzables tienen UNDEF rpo_pos. */
    std::vector<uint32_t> rpo_pos(N, UINT32_MAX);
    for (size_t i = 0; i < rpo.size(); ++i) rpo_pos[rpo[i]] = static_cast<uint32_t>(i);

    /* idom[b] = inmediato dominador.  UNDEF = no computado todavia. */
    std::vector<IrBlockId> idom(N, UNDEF);
    idom[entry] = entry;

    auto intersect_dom = [&](IrBlockId b1, IrBlockId b2) -> IrBlockId {
        while (b1 != b2) {
            while (b1 != UNDEF && rpo_pos[b1] > rpo_pos[b2]) b1 = idom[b1];
            while (b2 != UNDEF && rpo_pos[b2] > rpo_pos[b1]) b2 = idom[b2];
            if (b1 == UNDEF || b2 == UNDEF) return UNDEF;
        }
        return b1;
    };

    bool dom_changed = true;
    while (dom_changed) {
        dom_changed = false;
        for (IrBlockId b : rpo) {
            if (b == entry) continue;
            IrBlockId new_idom = UNDEF;
            for (IrBlockId p : preds[b]) {
                if (idom[p] != UNDEF) {
                    new_idom = (new_idom == UNDEF) ? p : intersect_dom(new_idom, p);
                    if (new_idom == UNDEF) break;
                }
            }
            if (new_idom != UNDEF && new_idom != idom[b]) {
                idom[b] = new_idom;
                dom_changed = true;
            }
        }
    }

    /* Helper: T domina B?  Camina la cadena idom desde B hasta entry o T. */
    auto dominates = [&](IrBlockId T, IrBlockId B) -> bool {
        if (T == B) return true;
        if (T >= N || B >= N || idom[B] == UNDEF) return false;
        IrBlockId cur = B;
        while (idom[cur] != cur) {
            cur = idom[cur];
            if (cur == T) return true;
        }
        return false;
    };

    /* Back-edge real: arista B->T donde T domina a B. */
    struct BackEdge { IrBlockId pred; IrBlockId header; };
    std::vector<BackEdge> backs;
    for (size_t b = 0; b < N; ++b) {
        for (IrBlockId s : succs[b]) {
            if (dominates(s, static_cast<IrBlockId>(b))) {
                backs.push_back({static_cast<IrBlockId>(b), s});
            }
        }
    }

    if (backs.empty()) return false;

    bool changed = false;

    /* Procesar cada back-edge (= 1 loop). */
    for (const auto &be : backs) {
        const IrBlockId header = be.header;
        const IrBlockId back   = be.pred;

        /* Bloques en el loop: BFS reverse desde back hasta header via
         * preds (en CFG reducible). */
        std::unordered_set<IrBlockId> loop_set;
        loop_set.insert(header);
        loop_set.insert(back);
        std::vector<IrBlockId> stack{back};
        while (!stack.empty()) {
            IrBlockId b = stack.back();
            stack.pop_back();
            if (b == header) continue;
            for (IrBlockId p : preds[b]) {
                if (!loop_set.count(p)) {
                    loop_set.insert(p);
                    stack.push_back(p);
                }
            }
        }

        /* Pre-header: predecesor de @c header que NO esta en el loop.
         * Tipicamente entry o un bloque previo al while. */
        IrBlockId pre_header = IR_NO_BLOCK;
        for (IrBlockId p : preds[header]) {
            if (!loop_set.count(p)) {
                if (pre_header == IR_NO_BLOCK) {
                    pre_header = p;
                } else {
                    /* Multiples pre-headers: skip (necesita split). */
                    pre_header = IR_NO_BLOCK;
                    break;
                }
            }
        }
        if (pre_header == IR_NO_BLOCK) continue;

        /* Conjunto de VIDs definidos DENTRO del loop. */
        std::unordered_set<IrValueId> defined_in_loop;
        for (IrBlockId b : loop_set) {
            for (const auto &ins : fn.blocks[b].instrs) {
                if (ins.dst != IR_NO_VALUE) {
                    defined_in_loop.insert(ins.dst);
                }
            }
        }

        /* Detectar si hay STORE/MEMCPY/SETFIELD/ARRAY_STORE/RAW_ASM/CALL
         * dentro del loop.  Si los hay, los LOADs no son hoistables sin
         * alias analysis (la memoria pudo cambiar entre iteraciones). */
        bool loop_has_memory_writes = false;
        for (IrBlockId b : loop_set) {
            for (const auto &ins : fn.blocks[b].instrs) {
                switch (ins.op) {
                    case IrOp::STORE: case IrOp::MEMCPY: case IrOp::SETFIELD:
                    case IrOp::ARRAY_STORE: case IrOp::STRFINALIZE:
                    case IrOp::RAW_ASM:
                    case IrOp::CALL: case IrOp::CALLN: case IrOp::CALLVIRT:
                    case IrOp::CALLIND: case IrOp::CALLM: case IrOp::CALLITF:
                    case IrOp::CALLCLOSURE:
                    case IrOp::TAILCALL:
                        loop_has_memory_writes = true;
                        break;
                    default: break;
                }
                if (loop_has_memory_writes) break;
            }
            if (loop_has_memory_writes) break;
        }

        /* Helper: instr es candidato para mover? */
        auto is_invariant_candidate = [&](const IrInstr &ins) -> bool {
            /* Sprint mem-perf string_hot: anyadir STRMAKE/STRCAT/STRINTERN/
             * STRCONV/STRRESERVE como hoistables a pesar de side-effecting.
             * El alloc-identity no es observable; el contenido si.  Ver
             * @c is_licm_hoistable_alloc. */
            if (!is_pure(ins.op) && !is_licm_hoistable_alloc(ins.op)) return false;
            if (ins.op == IrOp::PHI) return false;
            if (ins.preserve()) return false;
            if (ins.dst == IR_NO_VALUE) return false;
            /* Sprint string-perf-2 bug fix: STRMAKE solo es seguro hoistar
             * si su vm_addr operand apunta a memoria immutable (literal
             * en static_data via STR_LIT_ADDR).  Para mutable buffers
             * (ALLOCA + stores), hoistar = capturar bytes en momento
             * incorrecto. */
            if (ins.op == IrOp::STRMAKE) {
                if (ins.operands.empty()) return false;
                if (!strmake_reads_immutable(fn, ins.operands[0])) return false;
            }
            /* Ops que LEEN memoria: solo hoistables si NO hay writes en el
             * loop (v1 sin alias analysis).  Conservador pero correcto. */
            switch (ins.op) {
                case IrOp::LOAD: case IrOp::ARRAY_LOAD: case IrOp::GETFIELD:
                case IrOp::ARRAY_LEN:
                    if (loop_has_memory_writes) return false;
                    break;
                default: break;
            }
            /* Todos los operands deben ser invariant (CONST o definidos fuera). */
            for (IrValueId op : ins.operands) {
                if (op == IR_NO_VALUE) continue;
                if (defined_in_loop.count(op)) return false;
            }
            return true;
        };

        /* Recolectar instrucciones a mover (en orden) + remover de sus
         * bloques originales. */
        std::vector<IrInstr> moved;
        for (IrBlockId b : loop_set) {
            auto &bb = fn.blocks[b];
            std::vector<IrInstr> remaining;
            remaining.reserve(bb.instrs.size());
            for (auto &ins : bb.instrs) {
                if (is_invariant_candidate(ins)) {
                    /* Mover.  Despues de mover, el VID ya no esta
                     * definido en el loop -> otros usos del mismo
                     * VID dentro del loop necesitan que la def este
                     * en pre-header (ok, sera invariant alli). */
                    moved.push_back(std::move(ins));
                    defined_in_loop.erase(moved.back().dst);
                    changed = true;
                } else {
                    remaining.push_back(std::move(ins));
                }
            }
            bb.instrs = std::move(remaining);
        }

        if (moved.empty()) continue;

        /* Insertar las moved instrs al final del pre-header, ANTES de
         * su terminador (JMP a header). */
        auto &ph_bb = fn.blocks[pre_header];
        size_t insert_at = ph_bb.instrs.size();
        if (!ph_bb.instrs.empty()) {
            const auto &term = ph_bb.instrs.back();
            if (term.op == IrOp::BR || term.op == IrOp::BR_COND
             || term.op == IrOp::RET || term.op == IrOp::THROW) {
                insert_at = ph_bb.instrs.size() - 1;
            }
        }
        ph_bb.instrs.insert(ph_bb.instrs.begin() + insert_at,
            std::make_move_iterator(moved.begin()),
            std::make_move_iterator(moved.end()));
    }

    return changed;
}

// =========================================================================
//  Pase ir_pass_devirt_monomorphic (Phase D.7.opt)
// =========================================================================

/**
 * @brief Detecta si el modulo usa AOP escaneando raw_asm por "addadvice".
 *
 * Conservador: si encuentra cualquier raw_asm con texto "addadvice", el
 * modulo se considera AOP-enabled y se skip-ea el devirt.  Sin esto las
 * callvirt convertidas a call directo saltarian el advice chain runtime.
 */
static bool module_uses_aop(const IrModule &mod) {
    for (const auto &cls : mod.classes) {
        if (cls.is_aspect) return true;
    }
    for (const auto &fn : mod.functions) {
        for (const auto &bb : fn.blocks) {
            for (const auto &ins : bb.instrs) {
                if (ins.op == IrOp::RAW_ASM
                 && ins.func_name.find("addadvice") != std::string::npos) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool ir_pass_devirt_monomorphic(IrModule &mod) {
    if (module_uses_aop(mod)) return false;

    /* Indice por nombre de clase. */
    std::unordered_map<std::string, const IrClass *> class_by_name;
    for (const auto &c : mod.classes) class_by_name[c.name] = &c;
    if (class_by_name.empty()) return false;

    /* "Effectively final": clase sin subclases dentro del modulo.
     * Conservador (closed world).  El loadmodule dinamico podria
     * cargar una subclase, pero los tests/e2e no usan ese patron. */
    std::unordered_set<std::string> has_subclass;
    for (const auto &c : mod.classes) {
        if (!c.super_name.empty() && c.super_name != "Object") {
            has_subclass.insert(c.super_name);
        }
    }

    bool changed = false;
    for (auto &fn : mod.functions) {
        if (fn.is_native) continue;
        if (fn.blocks.empty()) continue;

        /* class_of: para cada SSA value cuyo origen es conocido como una
         * clase concreta, mapea vid -> class_name.  Se construye iterando
         * en orden lineal de bloques con propagacion a traves de MOV y
         * PHI (si todos los inputs comparten clase). */
        std::unordered_map<IrValueId, std::string> class_of;

        /* Multiples pasadas para propagar a traves de PHIs (loops). */
        const int MAX_ITERS = 4;
        for (int iter = 0; iter < MAX_ITERS; ++iter) {
            bool grew = false;
            for (auto &bb : fn.blocks) {
                for (auto &ins : bb.instrs) {
                    if (ins.dst == IR_NO_VALUE) continue;
                    if (class_of.count(ins.dst)) continue;

                    /* Origen: call @__new_<X> */
                    if (ins.op == IrOp::CALL
                     && ins.func_name.rfind("__new_", 0) == 0) {
                        std::string cn = ins.func_name.substr(6);
                        if (class_by_name.count(cn)) {
                            class_of[ins.dst] = cn;
                            grew = true;
                        }
                        continue;
                    }
                    /* NEWOBJ no carga class_name directamente; ignorar. */
                    /* MOV: hereda clase del source */
                    if (ins.op == IrOp::MOV && !ins.operands.empty()) {
                        auto it = class_of.find(ins.operands[0]);
                        if (it != class_of.end()) {
                            class_of[ins.dst] = it->second;
                            grew = true;
                        }
                        continue;
                    }
                    /* PHI: si TODOS los inputs comparten clase, hereda. */
                    if (ins.op == IrOp::PHI && !ins.phi_args.empty()) {
                        std::string c;
                        bool ok = true;
                        for (const auto &pa : ins.phi_args) {
                            if (pa.value == IR_NO_VALUE
                             || pa.value == ins.dst) continue;
                            auto it = class_of.find(pa.value);
                            if (it == class_of.end()) { ok = false; break; }
                            if (c.empty()) c = it->second;
                            else if (c != it->second) { ok = false; break; }
                        }
                        if (ok && !c.empty()) {
                            class_of[ins.dst] = c;
                            grew = true;
                        }
                        continue;
                    }
                }
            }
            if (!grew) break;
        }

        /* Aplicar devirt. */
        for (auto &bb : fn.blocks) {
            for (auto &ins : bb.instrs) {
                if (ins.op != IrOp::CALLVIRT) continue;
                if (ins.operands.empty()) continue;
                auto it = class_of.find(ins.operands[0]);
                if (it == class_of.end()) continue;
                const std::string &cn = it->second;
                auto ct = class_by_name.find(cn);
                if (ct == class_by_name.end()) continue;
                const IrClass *cls = ct->second;
                /* Safe: clase final O sin subclases EN ESTE MODULO. */
                const bool safe_class = cls->is_final
                                     || !has_subclass.count(cn);
                /* Buscar el metodo en el vtable_index. */
                const IrMethod *mtd = nullptr;
                for (const auto &m : cls->methods) {
                    if (m.vtable_index == static_cast<int32_t>(ins.imm)) {
                        mtd = &m;
                        break;
                    }
                }
                if (!mtd) continue;
                if (mtd->ir_fn_name.empty()) continue;
                const bool safe_method = mtd->is_final || safe_class;
                if (!safe_method) continue;

                /* Rewrite CALLVIRT -> CALL */
                ins.op        = IrOp::CALL;
                ins.func_name = mtd->ir_fn_name;
                ins.imm       = 0;
                /* operands sin cambio: [obj, args...] */
                changed = true;
            }
        }
    }
    return changed;
}

// =========================================================================
//  Pase ir_pass_speculative_devirt (C2): devirt especulativa guiada por IC
// =========================================================================

bool ir_pass_speculative_devirt(IrFunction &fn,
                                const std::vector<SpecDevirtSite> &sites) {
    if (fn.blocks.empty() || sites.empty()) return false;
    bool changed = false;

    /* Reservar de antemano para evitar realocaciones de fn.blocks durante la
     * cirugia (3 bloques nuevos por site).  Aun asi accedemos por INDICE, no
     * por referencia, por seguridad. */
    fn.blocks.reserve(fn.blocks.size() + sites.size() * 3 + 4);

    for (const auto &site : sites) {
        if (site.callvirt_dst == IR_NO_VALUE) continue;  /* void: sin PHI */

        /* Localizar el CALLVIRT objetivo por su dst (unico + estable). */
        IrBlockId bidx = IR_NO_BLOCK;
        size_t    i    = 0;
        for (size_t b = 0; b < fn.blocks.size() && bidx == IR_NO_BLOCK; ++b) {
            auto &bb = fn.blocks[b];
            for (size_t k = 0; k < bb.instrs.size(); ++k) {
                if (bb.instrs[k].op == IrOp::CALLVIRT
                 && bb.instrs[k].dst == site.callvirt_dst) {
                    bidx = static_cast<IrBlockId>(b);
                    i    = k;
                    break;
                }
            }
        }
        if (bidx == IR_NO_BLOCK) continue;  /* no encontrado: skip */

        /* Capturar datos del CALLVIRT (copia) antes de la cirugia. */
        const IrInstr cv             = fn.blocks[bidx].instrs[i];
        const IrType  rtype          = cv.type;
        const IrValueId orig_dst     = cv.dst;
        const std::vector<IrValueId> ops = cv.operands;  /* [obj, args...] */
        const uint32_t srcline       = cv.source_line;
        if (ops.empty()) continue;  /* sin receptor: no especulable */

        /* Capturar los sucesores ORIGINALES de B (los del terminador que va
         * en el tail) antes de sobreescribir B.succs. */
        const std::vector<IrBlockId> orig_succs = fn.blocks[bidx].succs;

        /* Crear los 3 bloques (append; los indices existentes no se mueven). */
        const IrBlockId fastb  = fn.new_block("spec_fast");
        const IrBlockId slowb  = fn.new_block("spec_slow");
        const IrBlockId mergeb = fn.new_block("spec_merge");

        /* Mover el tail [i+1 ..] al merge; truncar B a [0 .. i-1]. */
        {
            auto &Binstrs = fn.blocks[bidx].instrs;
            std::vector<IrInstr> tail(Binstrs.begin() + static_cast<long>(i) + 1,
                                      Binstrs.end());
            fn.blocks[mergeb].instrs = std::move(tail);
            Binstrs.resize(i);  /* descarta el CALLVIRT en i + el tail */
        }

        /* --- Guard en B: cls = load[obj]; cmp cls, T; br_cond fast/slow --- */
        const IrValueId vcls = fn.new_value(IrType::I64, "spec_cls");
        {
            IrInstr ld; ld.op = IrOp::LOAD; ld.type = IrType::I64;
            ld.dst = vcls; ld.operands = {ops[0]}; ld.source_line = srcline;
            fn.blocks[bidx].instrs.push_back(ld);
        }
        const IrValueId vt = fn.new_value(IrType::I64, "spec_T");
        {
            IrInstr c; c.op = IrOp::CONST; c.type = IrType::I64;
            c.dst = vt; c.imm = site.class_ptr; c.source_line = srcline;
            fn.blocks[bidx].instrs.push_back(c);
        }
        const IrValueId vg = fn.new_value(IrType::BOOL, "spec_g");
        {
            IrInstr cm; cm.op = IrOp::CMP_EQ; cm.type = IrType::BOOL;
            cm.dst = vg; cm.operands = {vcls, vt}; cm.source_line = srcline;
            fn.blocks[bidx].instrs.push_back(cm);
        }
        {
            IrInstr br; br.op = IrOp::BR_COND; br.operands = {vg};
            br.target_block = fastb; br.false_block = slowb;
            br.source_line = srcline;
            fn.blocks[bidx].instrs.push_back(br);
        }
        fn.blocks[bidx].succs = {fastb, slowb};

        /* --- Fast: CALL directo al callee (ir_pass_inline lo inlinea). --- */
        const IrValueId rfast = fn.new_value(rtype, "spec_rfast");
        {
            IrInstr call; call.op = IrOp::CALL; call.type = rtype;
            call.dst = rfast; call.func_name = site.callee_ir_name;
            call.operands = ops; call.source_line = srcline;
            fn.blocks[fastb].instrs.push_back(call);
        }
        {
            IrInstr br; br.op = IrOp::BR; br.target_block = mergeb;
            fn.blocks[fastb].instrs.push_back(br);
        }
        fn.blocks[fastb].preds = {bidx};
        fn.blocks[fastb].succs = {mergeb};

        /* --- Slow: CALLVIRT original (copia) -> r_slow. --- */
        const IrValueId rslow = fn.new_value(rtype, "spec_rslow");
        {
            IrInstr cv2 = cv; cv2.dst = rslow;
            fn.blocks[slowb].instrs.push_back(cv2);
        }
        {
            IrInstr br; br.op = IrOp::BR; br.target_block = mergeb;
            fn.blocks[slowb].instrs.push_back(br);
        }
        fn.blocks[slowb].preds = {bidx};
        fn.blocks[slowb].succs = {mergeb};

        /* --- Merge: PHI(orig_dst) = [r_fast@fast, r_slow@slow] + tail. --- */
        {
            IrInstr phi; phi.op = IrOp::PHI; phi.type = rtype; phi.dst = orig_dst;
            phi.phi_args = { IrPhiArg{rfast, fastb}, IrPhiArg{rslow, slowb} };
            phi.source_line = srcline;
            fn.blocks[mergeb].instrs.insert(fn.blocks[mergeb].instrs.begin(), phi);
        }
        fn.blocks[mergeb].preds = {fastb, slowb};
        fn.blocks[mergeb].succs = orig_succs;

        /* Repuntar los sucesores originales de B: ahora su predecesor es
         * merge (el terminador del tail vive ahi).  Tambien sus PHIs. */
        for (IrBlockId s : orig_succs) {
            if (s == IR_NO_BLOCK || s >= fn.blocks.size()) continue;
            auto &sb = fn.blocks[s];
            for (auto &p : sb.preds) if (p == bidx) p = mergeb;
            for (auto &ins : sb.instrs) {
                if (ins.op != IrOp::PHI) continue;
                for (auto &pa : ins.phi_args) if (pa.block == bidx) pa.block = mergeb;
            }
        }

        changed = true;
    }

    return changed;
}

// =========================================================================
//  Pase ir_pass_spec_devirt (TAREA 2 / C2): devirt especulativa ESTATICA
//  via guard-chain de K candidatos + fallback al dispatch original.
// =========================================================================

bool ir_pass_spec_devirt(IrFunction &fn) {
    if (fn.blocks.empty() || fn.spec_devirt_sites.empty()) return false;
    bool changed = false;

    /* Reservar de antemano espacio para los bloques nuevos (2K+1 por site:
     * K fast + K-1 guard + 1 fallback + 1 merge) para evitar realocaciones de
     * fn.blocks durante la cirugia.  Accedemos por INDICE igualmente. */
    size_t extra = 4;
    for (const auto &kv : fn.spec_devirt_sites)
        extra += kv.second.size() * 2 + 1;
    fn.blocks.reserve(fn.blocks.size() + extra);

    for (const auto &kv : fn.spec_devirt_sites) {
        const IrValueId site_dst = kv.first;
        const std::vector<DevirtCandidate> &cands = kv.second;
        if (site_dst == IR_NO_VALUE || cands.empty()) continue;  /* void: skip */

        /* Localizar el call dinamico objetivo por su dst (unico + estable). */
        IrBlockId bidx   = IR_NO_BLOCK;
        size_t    i      = 0;
        IrOp      callop = IrOp::NOP;
        for (size_t b = 0; b < fn.blocks.size() && bidx == IR_NO_BLOCK; ++b) {
            auto &bb = fn.blocks[b];
            for (size_t k = 0; k < bb.instrs.size(); ++k) {
                const IrOp o = bb.instrs[k].op;
                if ((o == IrOp::CALLITF || o == IrOp::CALLVIRT || o == IrOp::CALLM)
                 && bb.instrs[k].dst == site_dst) {
                    bidx = static_cast<IrBlockId>(b);
                    i = k; callop = o;
                    break;
                }
            }
        }
        if (bidx == IR_NO_BLOCK) continue;  /* no encontrado: skip */

        /* Capturar datos del call (copia) antes de la cirugia. */
        const IrInstr   callins  = fn.blocks[bidx].instrs[i];
        const IrType    rtype    = callins.type;
        const IrValueId orig_dst = callins.dst;
        const std::vector<IrValueId> ops = callins.operands;  /* [obj, (meta), args...] */
        const uint32_t  srcline  = callins.source_line;
        if (ops.empty()) continue;  /* sin receptor: no especulable */

        /* Operands del CALL directo del fast path: receptor + args, sin el
         * operando de metadata del dispatch.  CALLITF lleva params_ptr en
         * ops[1] y CALLM lleva el method_ptr en ops[1] -> se quitan; CALLVIRT
         * no tiene metadata -> se mantienen todos.  El SRET retbuf (cuando
         * aplica) va tras ops[1], asi que se conserva. */
        std::vector<IrValueId> call_ops;
        if (callop == IrOp::CALLVIRT) {
            call_ops = ops;
        } else {
            call_ops.push_back(ops[0]);
            for (size_t a = 2; a < ops.size(); ++a) call_ops.push_back(ops[a]);
        }

        /* Capturar los sucesores ORIGINALES de B antes de sobreescribirlos. */
        const std::vector<IrBlockId> orig_succs = fn.blocks[bidx].succs;

        const size_t K = cands.size();

        /* Crear los bloques nuevos (append; los indices existentes no se
         * mueven gracias al reserve previo). */
        std::vector<IrBlockId> fastb(K);
        std::vector<IrBlockId> gblk(K, IR_NO_BLOCK);  /* gblk[0]=B; gblk[n>=1] nuevos */
        for (size_t n = 0; n < K; ++n) fastb[n] = fn.new_block("spec_fast");
        for (size_t n = 1; n < K; ++n) gblk[n]  = fn.new_block("spec_guard");
        const IrBlockId fbackb = fn.new_block("spec_fallback");
        const IrBlockId mergeb = fn.new_block("spec_merge");
        gblk[0] = bidx;  /* el primer guard va en B (in-place) */

        /* Mover el tail [i+1 ..] al merge; truncar B a [0 .. i-1]. */
        {
            auto &Binstrs = fn.blocks[bidx].instrs;
            std::vector<IrInstr> tail(Binstrs.begin() + static_cast<long>(i) + 1,
                                      Binstrs.end());
            fn.blocks[mergeb].instrs = std::move(tail);
            Binstrs.resize(i);  /* descarta el call en i + el tail */
        }

        /* cls = load[obj], computado UNA vez en B (domina toda la cadena). */
        const IrValueId vcls = fn.new_value(IrType::I64, "spec_cls");
        {
            IrInstr ld; ld.op = IrOp::LOAD; ld.type = IrType::I64;
            ld.dst = vcls; ld.operands = {ops[0]}; ld.source_line = srcline;
            fn.blocks[bidx].instrs.push_back(ld);
        }

        /* Cadena de guardas: por candidato n en gblk[n]:
         *   g = (cls == cls_value_n);  br_cond fast_n / next
         * donde next = gblk[n+1] (si lo hay) o el fallback. */
        std::vector<IrValueId> rfast(K);
        for (size_t n = 0; n < K; ++n) {
            const IrBlockId gb   = gblk[n];
            const IrBlockId next = (n + 1 < K) ? gblk[n + 1] : fbackb;

            const IrValueId vg = fn.new_value(IrType::BOOL, "spec_g");
            {
                IrInstr cm; cm.op = IrOp::CMP_EQ; cm.type = IrType::BOOL;
                cm.dst = vg; cm.operands = {vcls, cands[n].cls_value};
                cm.source_line = srcline;
                fn.blocks[gb].instrs.push_back(cm);
            }
            {
                IrInstr br; br.op = IrOp::BR_COND; br.operands = {vg};
                br.target_block = fastb[n]; br.false_block = next;
                br.source_line = srcline;
                fn.blocks[gb].instrs.push_back(br);
            }
            fn.blocks[gb].succs = {fastb[n], next};
            if (n > 0) fn.blocks[gb].preds = {gblk[n - 1]};  /* gblk[0]=B: preds intactos */

            /* fast_n: CALL directo al callee (ir_pass_inline lo inlinea) + br merge. */
            rfast[n] = fn.new_value(rtype, "spec_rfast");
            {
                IrInstr call; call.op = IrOp::CALL; call.type = rtype;
                call.dst = rfast[n]; call.func_name = cands[n].callee_ir_name;
                call.operands = call_ops; call.source_line = srcline;
                fn.blocks[fastb[n]].instrs.push_back(call);
            }
            {
                IrInstr br; br.op = IrOp::BR; br.target_block = mergeb;
                fn.blocks[fastb[n]].instrs.push_back(br);
            }
            fn.blocks[fastb[n]].preds = {gb};
            fn.blocks[fastb[n]].succs = {mergeb};
        }

        /* Fallback: el call dinamico ORIGINAL (copia) -> r_slow + br merge. */
        const IrValueId rslow = fn.new_value(rtype, "spec_rslow");
        {
            IrInstr cv2 = callins; cv2.dst = rslow;
            fn.blocks[fbackb].instrs.push_back(cv2);
        }
        {
            IrInstr br; br.op = IrOp::BR; br.target_block = mergeb;
            fn.blocks[fbackb].instrs.push_back(br);
        }
        fn.blocks[fbackb].preds = {gblk[K - 1]};
        fn.blocks[fbackb].succs = {mergeb};

        /* Merge: PHI(orig_dst) = [rfast_n@fast_n..., rslow@fallback] + tail. */
        {
            IrInstr phi; phi.op = IrOp::PHI; phi.type = rtype; phi.dst = orig_dst;
            phi.phi_args.reserve(K + 1);
            for (size_t n = 0; n < K; ++n)
                phi.phi_args.push_back(IrPhiArg{rfast[n], fastb[n]});
            phi.phi_args.push_back(IrPhiArg{rslow, fbackb});
            phi.source_line = srcline;
            fn.blocks[mergeb].instrs.insert(fn.blocks[mergeb].instrs.begin(), phi);
        }
        {
            std::vector<IrBlockId> mpreds;
            mpreds.reserve(K + 1);
            for (size_t n = 0; n < K; ++n) mpreds.push_back(fastb[n]);
            mpreds.push_back(fbackb);
            fn.blocks[mergeb].preds = std::move(mpreds);
        }
        fn.blocks[mergeb].succs = orig_succs;

        /* Repuntar los sucesores originales de B: ahora su predecesor es merge
         * (el terminador del tail vive ahi).  Tambien sus PHIs. */
        for (IrBlockId s : orig_succs) {
            if (s == IR_NO_BLOCK || s >= fn.blocks.size()) continue;
            auto &sb = fn.blocks[s];
            for (auto &p : sb.preds) if (p == bidx) p = mergeb;
            for (auto &ins : sb.instrs) {
                if (ins.op != IrOp::PHI) continue;
                for (auto &pa : ins.phi_args) if (pa.block == bidx) pa.block = mergeb;
            }
        }

        changed = true;
    }

    return changed;
}

// =========================================================================
//  Pase Load Narrow: elide SEXT redundante tras LOAD i8/i16/i32
// =========================================================================

bool ir_pass_load_narrow(IrFunction &fn) {
    if (fn.blocks.empty()) return false;

    /* Ops "narrow-safe": dado inputs con bits bajos correctos (sin importar
     * bits altos), producen un resultado cuyos bits bajos siguen siendo
     * correctos.  ADD/SUB/MUL/AND/OR/XOR son bit-parallel en los bits bajos. */
    auto is_narrow_safe_arith = [](IrOp op) -> bool {
        switch (op) {
            case IrOp::ADD:
            case IrOp::SUB:
            case IrOp::MUL:
            case IrOp::AND:
            case IrOp::OR:
            case IrOp::XOR:
                return true;
            default:
                return false;
        }
    };

    /* Construir lista de usos (vid -> [(block_idx, instr_idx, kind)]).
     * kind: 0=operands, 1=phi_args.  Solo necesitamos saber QUE instrucciones
     * referencian cada valor para inspeccionar su op. */
    struct UseRef {
        size_t  bi;
        size_t  ii;
    };
    std::unordered_map<IrValueId, std::vector<UseRef>> uses;
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto &bb = fn.blocks[bi];
        for (size_t ii = 0; ii < bb.instrs.size(); ++ii) {
            const auto &ins = bb.instrs[ii];
            for (IrValueId op : ins.operands) {
                if (op != IR_NO_VALUE) uses[op].push_back({bi, ii});
            }
            for (const auto &pa : ins.phi_args) {
                if (pa.value != IR_NO_VALUE) uses[pa.value].push_back({bi, ii});
            }
        }
    }

    /* Para cada LOAD i8/i16/i32 (signed), computar el cierre transitivo
     * de valores derivados via ops narrow-safe.  Si TODOS los usos terminales
     * son STORE/RET del mismo tipo y todos los usos intermedios son ops
     * narrow-safe o STORE/RET, marcar el LOAD como narrow_only. */
    /* Re-analizar en cada invocacion: el flag puede REVOCARSE si pasos
     * posteriores (CSE/copy_prop) exponen usos que antes no eran visibles
     * (e.g. %42 = add %38, %41 cuya operand %41 luego se rewrite a %29). */
    bool changed = false;
    for (auto &bb : fn.blocks) {
        for (auto &ins : bb.instrs) {
            if (ins.op != IrOp::LOAD) continue;
            if (ins.dst == IR_NO_VALUE) continue;
            if (ins.type != IrType::I8 && ins.type != IrType::I16
             && ins.type != IrType::I32) continue;

            const IrType narrow_type = ins.type;

            /* Cierre transitivo via BFS. */
            std::unordered_set<IrValueId> closure;
            std::vector<IrValueId> worklist;
            closure.insert(ins.dst);
            worklist.push_back(ins.dst);

            bool safe = true;
            while (safe && !worklist.empty()) {
                IrValueId v = worklist.back();
                worklist.pop_back();

                auto it = uses.find(v);
                if (it == uses.end()) continue; // no uses -> trivially safe

                for (const UseRef &u : it->second) {
                    const IrInstr &user = fn.blocks[u.bi].instrs[u.ii];

                    /* STORE del mismo tipo: el valor solo se usa como
                     * operand[0] (val).  Truncar al ancho de tipo es seguro. */
                    if (user.op == IrOp::STORE) {
                        if (user.type != narrow_type) { safe = false; break; }
                        /* El valor solo es seguro si esta en operand[0] (val);
                         * si esta en operand[1] (ptr), eso seria un puntero
                         * derivado del LOAD lo cual es UNSAFE (no es nuestro
                         * caso esperado pero por seguridad). */
                        if (user.operands.size() < 2) { safe = false; break; }
                        if (user.operands[0] != v) { safe = false; break; }
                        continue;
                    }

                    /* RET del mismo tipo: el caller espera el ancho declarado,
                     * el VM trunca al hacer return.  Conservadoramente solo
                     * permitimos cuando fn.ret_type coincide. */
                    if (user.op == IrOp::RET) {
                        if (fn.ret_type != narrow_type) { safe = false; break; }
                        continue;
                    }

                    /* Op narrow-safe del mismo tipo: propagar al closure. */
                    if (is_narrow_safe_arith(user.op) && user.type == narrow_type) {
                        if (user.dst != IR_NO_VALUE && closure.insert(user.dst).second) {
                            worklist.push_back(user.dst);
                        }
                        continue;
                    }

                    /* PHI del mismo tipo: el PHI mismo produce un valor i32
                     * cuyos bits altos pueden ser garbage si NUESTRO valor
                     * llega.  Pero si TODOS los usos del PHI son seguros, el
                     * garbage no importa.  Propagar el dst del PHI al closure
                     * para que la BFS verifique sus usos transitivamente.
                     * Los ciclos en PHIs de loops se manejan via el set
                     * @c closure (no se re-procesa lo ya visitado).
                     * Otros inputs del PHI no nos importan: solo nos
                     * preocupa como nuestro valor se propaga a partir del
                     * PHI hacia adelante. */
                    if (user.op == IrOp::PHI && user.type == narrow_type) {
                        if (user.dst != IR_NO_VALUE && closure.insert(user.dst).second) {
                            worklist.push_back(user.dst);
                        }
                        continue;
                    }

                    /* MOV del mismo tipo (e.g. copy_prop residual): propagar. */
                    if (user.op == IrOp::MOV && user.type == narrow_type) {
                        if (user.dst != IR_NO_VALUE && closure.insert(user.dst).second) {
                            worklist.push_back(user.dst);
                        }
                        continue;
                    }

                    /* Cualquier otro uso (CMP, SEXT, ZEXT, CAST, BITCAST,
                     * TRUNC, SHL/SHR/SAR, NEG/NOT, SDIV/UDIV/SMOD/UMOD,
                     * CALL, STORE/LOAD/RET de tipo distinto, etc.) aborta
                     * la elision -- los bits altos pueden ser necesarios. */
                    safe = false;
                    break;
                }
            }

            /* Establecer/revocar el flag segun analisis actual. */
            const bool prev = fn.values[ins.dst].narrow_only;
            if (prev != safe) {
                fn.values[ins.dst].narrow_only = safe;
                changed = true;
            }
        }
    }

    return changed;
}

// =========================================================================
//  Pase List Scheduling: reordena para exponer ILP
// =========================================================================

/* Determina si la instruccion es una "barrera" (no se puede reordenar
 * a traves de ella en NINGUNA direccion: ni mover instrucciones hacia
 * arriba de la barrera, ni hacia abajo).  Usado para CALLs, RAW_ASM,
 * NEWOBJ, etc. donde el side-effect es opaco al scheduler. */
static bool is_sched_barrier(IrOp op) {
    switch (op) {
        case IrOp::CALL: case IrOp::CALLN: case IrOp::CALLVIRT:
        case IrOp::CALLIND: case IrOp::CALLM: case IrOp::CALLITF:
        case IrOp::CALLCLOSURE:
        case IrOp::TAILCALL: case IrOp::CALLSUPER:
        case IrOp::RAW_ASM:
        case IrOp::NEWOBJ: case IrOp::NEWOBJS: case IrOp::GC_ALLOC: case IrOp::GC_ALLOCP:
        case IrOp::RAW_ALLOC: case IrOp::RAW_FREE:
        case IrOp::THROW: case IrOp::TRYENTER: case IrOp::TRYLEAVE:
        case IrOp::SETFIELD: case IrOp::ARRAY_STORE:
        case IrOp::MEMCPY:
        case IrOp::STRFINALIZE: case IrOp::GCWB_IR:
        // Sprint string-perf-2 bug fix (2026-06-02): STRMAKE LEE
        // bytes desde vm_mem en runtime (via vm_addr).  Sin marcarla
        // como barrera, el scheduler podia reordenar STOREs a vm_mem
        // PASADO la STRMAKE, leyendo bytes stale.  Bug capturado en
        // patron `buf[i]=X; STRMAKE(buf); buf[i]=Y; STRMAKE(buf)`
        // donde el segundo STORE quedaba post-STRMAKE.
        // STRCAT/STRCONV/STRFLAT NO leen vm_mem (operan sobre handles),
        // pero pueden disparar alloc -> GC -> rearrange objects.  Mas
        // seguro tratar TODAS las str ops alloc-side como barreras
        // hasta confirmar safety por op.
        case IrOp::STRMAKE: case IrOp::STRCAT: case IrOp::STRCONV:
        case IrOp::STRFLAT: case IrOp::STRINTERN: case IrOp::STRRESERVE:
        case IrOp::FUTURE: case IrOp::AWAIT: case IrOp::FULFILL: case IrOp::REJECT:
        case IrOp::FULFILL_HLT:
        case IrOp::MSGSEND: case IrOp::MSGRECV:
        // Recuperados fase B: instrucciones que LEEN structs de params
        // construidos por STOREs previos.  Sin barrera, el scheduler puede
        // moverlas antes de los STOREs y leer basura.  Cubre tambien las
        // operaciones GC/atomic/static que mutan estado global.
        case IrOp::MVTAKE_IR:
        case IrOp::GC_PROMOTE: case IrOp::GC_DEMOTE:
        case IrOp::GC_HANDLE_FOR_PTR:
        case IrOp::ATOMIC_LD_I64: case IrOp::ATOMIC_ST_I64:
        case IrOp::ATOMIC_CAS_I64: case IrOp::ATOMIC_ADD_I64:
        case IrOp::GETSTATIC: case IrOp::SETSTATIC:
        case IrOp::FINDCLASS: case IrOp::DEFCLASS:
        case IrOp::DEFFIELD:  case IrOp::DEFMETHOD:  case IrOp::ADDADVICE:
        case IrOp::FINDMETHOD: case IrOp::FINDFIELD:
        case IrOp::SETMETHDBG:
        case IrOp::PROCEED:
        case IrOp::SPAWN_ON: case IrOp::HLT: case IrOp::PANIC:
        case IrOp::GETPID:  case IrOp::GETARGC: case IrOp::GETARG:
        case IrOp::STRGETBYTES:
            return true;
        default:
            return false;
    }
}

/* STORE no es barrera total pero sirve de "memory barrier" suave:
 * LOADs posteriores podrian alias, asi que LOAD depende de todos los
 * STOREs previos del mismo bloque (conservativo).  Otros STOREs tambien
 * dependen del previo (orden de escritura es observable). */
static bool is_store_like(IrOp op) {
    return op == IrOp::STORE || op == IrOp::SETFIELD
        || op == IrOp::ARRAY_STORE || op == IrOp::MEMCPY;
}

static bool is_load_like(IrOp op) {
    return op == IrOp::LOAD || op == IrOp::GETFIELD
        || op == IrOp::ARRAY_LOAD || op == IrOp::ARRAY_LEN;
}

/* Terminadores: deben quedar al final del bloque. */
static bool is_sched_terminator(IrOp op) {
    return op == IrOp::BR || op == IrOp::BR_COND
        || op == IrOp::RET || op == IrOp::THROW;
}

bool ir_pass_schedule(IrFunction &fn) {
    bool changed = false;

    for (auto &bb : fn.blocks) {
        const size_t N = bb.instrs.size();
        if (N <= 2) continue; // nada que reordenar

        /* Identificar prefijo de PHIs (fijo al inicio) y terminador. */
        size_t first_movable = 0;
        while (first_movable < N && bb.instrs[first_movable].op == IrOp::PHI) {
            ++first_movable;
        }
        size_t last_movable = N;
        if (last_movable > 0 && is_sched_terminator(bb.instrs[last_movable - 1].op)) {
            --last_movable;
        }
        if (last_movable - first_movable < 2) continue; // <2 instrucciones movibles

        const size_t M = last_movable - first_movable;

        /* Construir DAG de dependencias.  Nodos = indices [0..M) en el
         * rango movible.  Edges: pred[i] = lista de nodos que i depende.
         * succ[i] = lista de nodos que dependen de i. */
        std::vector<std::vector<size_t>> preds(M);
        std::vector<std::vector<size_t>> succs(M);
        std::vector<size_t> in_degree(M, 0);

        /* Map: def_vid -> index dentro de [0..M) que lo define. */
        std::unordered_map<IrValueId, size_t> def_of;
        for (size_t i = 0; i < M; ++i) {
            const auto &ins = bb.instrs[first_movable + i];
            if (ins.dst != IR_NO_VALUE) def_of[ins.dst] = i;
        }

        /* Tracking de "ultima barrera/store/load" para deps de memoria. */
        long last_barrier = -1;
        long last_store   = -1;
        std::vector<size_t> loads_after_last_store; // LOADs posteriores al ultimo store

        auto add_edge = [&](size_t from, size_t to) {
            /* Evitar duplicados.  Sanity: from != to. */
            if (from == to) return;
            for (size_t p : preds[to]) if (p == from) return;
            preds[to].push_back(from);
            succs[from].push_back(to);
        };

        for (size_t i = 0; i < M; ++i) {
            const auto &ins = bb.instrs[first_movable + i];

            /* Data deps: para cada operando con def en este bloque, edge def->i. */
            for (IrValueId op : ins.operands) {
                if (op == IR_NO_VALUE) continue;
                auto it = def_of.find(op);
                if (it != def_of.end() && it->second < i) {
                    add_edge(it->second, i);
                }
            }
            for (const auto &pa : ins.phi_args) {
                if (pa.value == IR_NO_VALUE) continue;
                auto it = def_of.find(pa.value);
                if (it != def_of.end() && it->second < i) {
                    add_edge(it->second, i);
                }
            }
            if (ins.func_ptr != IR_NO_VALUE) {
                auto it = def_of.find(ins.func_ptr);
                if (it != def_of.end() && it->second < i) {
                    add_edge(it->second, i);
                }
            }

            /* Memory/side-effect deps. */
            const bool is_barr = is_sched_barrier(ins.op);
            const bool is_st   = is_store_like(ins.op);
            const bool is_ld   = is_load_like(ins.op);

            if (is_barr) {
                /* Barrera: depende de todo lo previo, bloquea todo lo posterior.
                 * Conservador: anyadir edge desde TODOS los nodos previos. */
                for (size_t j = 0; j < i; ++j) add_edge(j, i);
                last_barrier = static_cast<long>(i);
                last_store   = static_cast<long>(i);
                loads_after_last_store.clear();
            } else if (is_st) {
                /* STORE depende de la ultima barrera, del ultimo store, y de
                 * todos los LOADs posteriores al ultimo store (orden W-after-R). */
                if (last_barrier >= 0) add_edge(static_cast<size_t>(last_barrier), i);
                if (last_store >= 0)   add_edge(static_cast<size_t>(last_store), i);
                for (size_t ld_idx : loads_after_last_store) add_edge(ld_idx, i);
                last_store = static_cast<long>(i);
                loads_after_last_store.clear();
            } else if (is_ld) {
                /* LOAD depende de la ultima barrera y del ultimo store. */
                if (last_barrier >= 0) add_edge(static_cast<size_t>(last_barrier), i);
                if (last_store >= 0)   add_edge(static_cast<size_t>(last_store), i);
                loads_after_last_store.push_back(i);
            } else {
                /* Pure ops: data deps + dep contra ultima barrera para
                 * evitar hoist sobre CALL/etc.  Sin esto, un @c const.i32
                 * declarado DESPUES de un CALL podria moverse ANTES del
                 * CALL, aumentando register pressure (el const queda vivo
                 * across el call y debe salvarse en push/pop).  El bench
                 * fib regresiono al hoistar @c const.i32 2 sobre el call
                 * recursivo.  La barrera actua como "muro de scheduling"
                 * en ambas direcciones (no entran ni salen ops a traves). */
                if (last_barrier >= 0) add_edge(static_cast<size_t>(last_barrier), i);
            }
        }

        /* Computar in_degree desde preds. */
        for (size_t i = 0; i < M; ++i) in_degree[i] = preds[i].size();

        /* Computar critical path length (CPL): CPL[i] = 1 + max(CPL[succ]).
         * Calculado en orden topologico inverso (de hojas a raices).
         * Hacemos topo sort primero. */
        std::vector<size_t> topo;
        topo.reserve(M);
        std::vector<size_t> in_deg_copy = in_degree;
        std::vector<size_t> q;
        for (size_t i = 0; i < M; ++i) {
            if (in_deg_copy[i] == 0) q.push_back(i);
        }
        while (!q.empty()) {
            size_t n = q.back(); q.pop_back();
            topo.push_back(n);
            for (size_t s : succs[n]) {
                if (--in_deg_copy[s] == 0) q.push_back(s);
            }
        }
        if (topo.size() != M) continue; // ciclo detectado (no deberia pasar en SSA + DAG)

        std::vector<uint32_t> cpl(M, 0);
        for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
            size_t n = *it;
            uint32_t best = 0;
            for (size_t s : succs[n]) {
                if (cpl[s] > best) best = cpl[s];
            }
            cpl[n] = best + 1;
        }

        /* List scheduling: ready set ordenado por (CPL desc, indice asc para estable). */
        std::vector<size_t> new_order;
        new_order.reserve(M);
        std::vector<size_t> ready;
        ready.reserve(M);
        std::vector<size_t> rem_in = in_degree;
        for (size_t i = 0; i < M; ++i) {
            if (rem_in[i] == 0) ready.push_back(i);
        }

        while (!ready.empty()) {
            /* Elegir el de mayor CPL (criterio clasico de Sethi-Ullman /
             * critical-path scheduling).  Tie-break:
             *   (a) menos sucesores primero -- "leaf" nodes que rellenan
             *       latencia sin extender el chain critico;
             *   (b) indice original menor para estabilidad. */
            size_t best_idx = 0;
            for (size_t j = 1; j < ready.size(); ++j) {
                const size_t a = ready[best_idx];
                const size_t b = ready[j];
                if (cpl[b] > cpl[a]) {
                    best_idx = j;
                } else if (cpl[b] == cpl[a]) {
                    if (succs[b].size() < succs[a].size()) {
                        best_idx = j;
                    } else if (succs[b].size() == succs[a].size() && b < a) {
                        best_idx = j;
                    }
                }
            }
            size_t pick = ready[best_idx];
            ready[best_idx] = ready.back();
            ready.pop_back();
            new_order.push_back(pick);
            for (size_t s : succs[pick]) {
                if (--rem_in[s] == 0) ready.push_back(s);
            }
        }

        if (new_order.size() != M) continue; // sanity

        /* Detectar si el orden cambio.  Si no, skip. */
        bool different = false;
        for (size_t i = 0; i < M; ++i) {
            if (new_order[i] != i) { different = true; break; }
        }
        if (!different) continue;

        /* Aplicar reordenamiento: mover bb.instrs[first_movable + new_order[i]]
         * a posicion first_movable + i.  Hacer una copia temporal porque las
         * indices originales se invalidan al mover. */
        std::vector<IrInstr> reordered;
        reordered.reserve(M);
        for (size_t i = 0; i < M; ++i) {
            reordered.push_back(std::move(bb.instrs[first_movable + new_order[i]]));
        }
        for (size_t i = 0; i < M; ++i) {
            bb.instrs[first_movable + i] = std::move(reordered[i]);
        }
        changed = true;
    }

    return changed;
}

// =========================================================================
//  Pase ir_pass_loop_memcpy_idiom (Sprint mem-perf 2026-06-02)
// =========================================================================
//
// Reconocimiento de loop-idiom byte-a-byte de la forma:
//
//   while_header:
//       %i_phi = phi.u64 [%i_init, pred]  [%i_next, body]
//       %cond  = cmp.ult.bool %i_phi, %N_ub
//       br.cond %cond, body, exit
//   body:
//       (%i_or_cast = bitcast %i_phi)?
//       %src_p = add.ptr %src_base, %i_or_cast
//       %dst_p = add.ptr %dst_base, %i_or_cast
//       %v     = load.u8 %src_p
//       store %v, %dst_p
//       %i_next = add %i_phi, %1_const
//       br while_header
//
// Lo reemplaza por una sola @c CALLN a @c vio_memcpy.  La libc nativa
// vectoriza con SSE/AVX/AVX-512 segun la CPU, asi el copy loop se
// acelera ~50-100x sin necesidad de SIMD codegen explicito.
//
// Pre-condiciones:
//   - El bloque body tiene EXACTAMENTE los 6-7 instrs del patron.
//   - El PHI del header solo tiene un valor de loop-carry (no PHIs
//     multiples).
//   - %i_init es CONST 0 (o cualquier const; usado como offset inicial
//     que ignoramos -- el memcpy copia [0, N)).  Por simplicidad
//     exigimos 0.
//   - %i_next = add %i_phi, 1 (step de 1).
//
// Tras el match: el body se reemplaza por `CALLN vio_memcpy(dst, src,
// N) + br exit`.  El header sigue invocando body solo la primera vez;
// la siguiente iteracion el cond falla porque body cambio el flow.
// Mas correcto: el body NO retorna a header (br exit directo), asi
// el loop nunca itera.
bool ir_pass_loop_memcpy_idiom(IrFunction &fn) {
    bool changed = false;
    if (fn.is_native) return false;

    for (size_t hi = 0; hi < fn.blocks.size(); ++hi) {
        IrBlock &header = fn.blocks[hi];
        // Header debe tener exactamente: 1 phi, 1 cmp, 1 br.cond.
        if (header.instrs.size() != 3) continue;
        const IrInstr &phi    = header.instrs[0];
        const IrInstr &cmp_in = header.instrs[1];
        const IrInstr &brc    = header.instrs[2];
        if (phi.op != IrOp::PHI) continue;
        if (phi.phi_args.size() != 2) continue;
        if (cmp_in.op != IrOp::CMP_ULT && cmp_in.op != IrOp::CMP_LT) continue;
        if (brc.op != IrOp::BR_COND) continue;
        if (brc.operands.empty() || brc.operands[0] != cmp_in.dst) continue;
        if (cmp_in.operands.size() != 2 || cmp_in.operands[0] != phi.dst) continue;
        IrValueId v_N = cmp_in.operands[1];
        IrBlockId body_id = brc.target_block;
        IrBlockId exit_id = brc.false_block;
        if (body_id >= fn.blocks.size() || exit_id >= fn.blocks.size()) continue;

        // Identificar el predecessor (entry) y el body en los phi_args.
        IrValueId v_init = IR_NO_VALUE;
        IrBlockId pred_id = IR_NO_VALUE;
        IrValueId v_next = IR_NO_VALUE;
        IrBlockId loop_pred = IR_NO_VALUE;
        for (const auto &pa : phi.phi_args) {
            if (pa.block == body_id) { v_next = pa.value; loop_pred = pa.block; }
            else                      { v_init = pa.value; pred_id   = pa.block; }
        }
        if (v_init == IR_NO_VALUE || v_next == IR_NO_VALUE) continue;

        // %i_init debe ser CONST 0.
        if (v_init >= fn.values.size() || !fn.values[v_init].is_const) continue;
        if (fn.values[v_init].const_val != 0) continue;

        // Body matching.
        IrBlock &body = fn.blocks[body_id];
        // Patron flexible: 5 o 6 instrs (con o sin bitcast).
        if (body.instrs.size() < 5 || body.instrs.size() > 7) continue;
        // Ultima instr debe ser br header.
        const IrInstr &body_term = body.instrs.back();
        if (body_term.op != IrOp::BR) continue;
        if (body_term.target_block != header.id) continue;

        // Buscar: load.u8, store, add (= i+1).
        IrValueId v_src_p = IR_NO_VALUE, v_dst_p = IR_NO_VALUE;
        IrValueId v_loaded = IR_NO_VALUE;
        IrValueId v_inc_step = IR_NO_VALUE;
        IrValueId v_index_used = IR_NO_VALUE;
        bool found_load = false, found_store = false, found_inc = false;
        IrValueId v_src_base = IR_NO_VALUE, v_dst_base = IR_NO_VALUE;
        for (const auto &ins : body.instrs) {
            if (ins.op == IrOp::LOAD && ins.type == IrType::U8
             && ins.operands.size() == 1) {
                v_src_p  = ins.operands[0];
                v_loaded = ins.dst;
                found_load = true;
            } else if (ins.op == IrOp::STORE && ins.operands.size() >= 2) {
                if (ins.operands[0] != v_loaded) { found_store = false; break; }
                v_dst_p = ins.operands[1];
                found_store = true;
            } else if (ins.op == IrOp::ADD && ins.operands.size() == 2
                    && ins.dst == v_next) {
                if (ins.operands[0] != phi.dst) continue;
                v_inc_step = ins.operands[1];
                found_inc = true;
            } else if (ins.op == IrOp::ADD && ins.operands.size() == 2) {
                // posible add.ptr base + index -- lo procesamos despues.
            } else if (ins.op == IrOp::BITCAST && ins.operands.size() == 1
                    && ins.operands[0] == phi.dst) {
                // ok, lo trataremos como un alias del index.
            } else if (ins.op == IrOp::BR) {
                // terminator OK
            } else {
                // instr inesperada -> rechazar.
                found_load = false;
                break;
            }
        }
        if (!found_load || !found_store || !found_inc) continue;

        // v_inc_step debe ser const 1.
        if (v_inc_step >= fn.values.size() || !fn.values[v_inc_step].is_const) continue;
        if (fn.values[v_inc_step].const_val != 1) continue;

        // Identificar bases via los ADDs.  Cada add.ptr produce v_src_p o v_dst_p.
        // operands son (base, index).  Index debe ser phi.dst o un bitcast de phi.dst.
        auto resolve_base = [&](IrValueId pv) -> IrValueId {
            for (const auto &ins : body.instrs) {
                if (ins.op == IrOp::ADD && ins.dst == pv
                 && ins.operands.size() == 2) {
                    IrValueId idx = ins.operands[1];
                    bool ok = (idx == phi.dst);
                    if (!ok) {
                        // chequear bitcast
                        for (const auto &b2 : body.instrs) {
                            if (b2.op == IrOp::BITCAST && b2.dst == idx
                             && !b2.operands.empty() && b2.operands[0] == phi.dst) {
                                ok = true; break;
                            }
                        }
                    }
                    if (ok) return ins.operands[0];
                }
            }
            return IR_NO_VALUE;
        };
        v_src_base = resolve_base(v_src_p);
        v_dst_base = resolve_base(v_dst_p);
        if (v_src_base == IR_NO_VALUE || v_dst_base == IR_NO_VALUE) continue;

        // OK match completo.  Reemplazar el body por:
        //   CALLN vio_memcpy(dst, src, N) + br exit
        IrInstr call_ins;
        call_ins.op = IrOp::CALLN;
        call_ins.type = IrType::I64;
        call_ins.dst = IR_NO_VALUE;
        call_ins.func_name = "stdlib/native/io/vesta_io:vio_memcpy";
        call_ins.operands = { v_dst_base, v_src_base, v_N };
        call_ins.source_line = body.instrs.front().source_line;

        IrInstr br_exit;
        br_exit.op = IrOp::BR;
        br_exit.target_block = exit_id;
        br_exit.dst = IR_NO_VALUE;

        body.instrs.clear();
        body.instrs.push_back(call_ins);
        body.instrs.push_back(br_exit);

        // Fix succs/preds: body ya no apunta a header.
        body.succs.clear();
        body.succs.push_back(exit_id);
        // Quitar body de header.preds (mantener el original pred).
        auto &hpreds = header.preds;
        hpreds.erase(std::remove(hpreds.begin(), hpreds.end(), body_id),
                     hpreds.end());
        // Anyadir body a exit.preds si no esta.
        IrBlock &exit_blk = fn.blocks[exit_id];
        if (std::find(exit_blk.preds.begin(), exit_blk.preds.end(), body_id)
            == exit_blk.preds.end()) {
            exit_blk.preds.push_back(body_id);
        }
        // Quitar el phi_arg de body en el PHI del header (ahora 1-arg).
        IrInstr &header_phi = fn.blocks[hi].instrs[0];
        header_phi.phi_args.erase(
            std::remove_if(header_phi.phi_args.begin(), header_phi.phi_args.end(),
                [body_id](const IrPhiArg &pa) { return pa.block == body_id; }),
            header_phi.phi_args.end());

        changed = true;
    }
    return changed;
}

// =========================================================================
//  Punto de entrada principal
// =========================================================================

// ---------------------------------------------------------------------------
// Verificacion de forma SSA tras cada fase del pipeline (control de errores
// avanzado).  ir_verify() valida que el IR siga siendo SSA correcto (def
// unica, terminadores, operandos en rango, phi args).  Llamarla tras cada
// pasada de optimizacion detecta un bug de transformacion en el instante en
// que corrompe el IR, en vez de un crash/silent-corruption muchas pasadas
// despues (el caso tipico: una pass deja un valor sin definir o un terminator
// roto y el siguiente consumidor produce codigo basura).
//
// Control:
//   VESTA_IR_VERIFY=1      verificar tras CADA fase del fix-point + finales
//   VESTA_IR_VERIFY_ABORT=1  abort() ante el primer error (detener en el
//                           punto exacto de corrupcion para un core-dump)
//
// Siempre imprime los errores encontrados (aunque no aborte) para que el
// diagnostico no se pierda silenciosamente.
// ---------------------------------------------------------------------------
namespace {

bool ir_verify_enabled() {
    static const bool v = [] {
        const char *e = std::getenv("VESTA_IR_VERIFY");
        return e != nullptr && e[0] != '\0' && e[0] != '0';
    }();
    return v;
}

bool ir_verify_abort_on_error() {
    static const bool v = [] {
        const char *e = std::getenv("VESTA_IR_VERIFY_ABORT");
        return e != nullptr && e[0] != '\0' && e[0] != '0';
    }();
    return v;
}

void ir_verify_phase(const IrModule &mod, const char *phase) {
    if (!ir_verify_enabled()) return;
    std::vector<std::string> errors;
    if (ir::ir_verify(mod, errors)) return;
    std::fprintf(stderr,
        "[ir-verify] FALLO tras la fase '%s' (%zu errores):\n",
        phase ? phase : "?", errors.size());
    for (size_t i = 0; i < errors.size() && i < 50; ++i)
        std::fprintf(stderr, "    %s\n", errors[i].c_str());
    if (errors.size() > 50)
        std::fprintf(stderr, "    ... y %zu mas\n", errors.size() - 50);
    if (ir_verify_abort_on_error()) {
        std::fprintf(stderr, "[ir-verify] abort() por VESTA_IR_VERIFY_ABORT\n");
        std::abort();
    }
}

} // namespace

void ir_optimize(IrModule &mod, OptLevel level) {
    if (level == OptLevel::O0) return; // sin optimizacion

    /* Phase D.7.opt: inline a nivel modulo ANTES del fix-point loop.
     * Despues del inline, los passes per-function se re-aplican sobre
     * el codigo expandido. */
    if (level >= OptLevel::O1) {
        ir_pass_inline(mod);
    }
    ir_verify_phase(mod, "inline-inicial");

    /* Unroll de bucles contados (port de Desmon): amortiza el overhead de
     * dispatch del loop en el interp y expone ILP en el JIT/AOT.  El factor es
     * automatico (segun el tamano del cuerpo).
     *
     * IMPORTANTE: DESACTIVADO por defecto.  El JIT de Vesta despacha cada
     * instruccion via helper (sin register allocation), asi que el unroll solo
     * replica los dispatch points y anade spills -> regresion medible en ambos
     * modos (bench array_sum: 0.07s -> 0.09s).  Activable para A/B testing con
     * VESTA_UNROLL=1 (o VESTA_NO_UNROLL=1 para desactivar si se fuerza en build). */
    if (level >= OptLevel::O2) {
        const char *on = std::getenv("VESTA_UNROLL");
        const bool enabled = on && on[0] != '\0' && on[0] != '0';
        const char *off = std::getenv("VESTA_NO_UNROLL");
        const bool disabled = off && off[0] != '\0' && off[0] != '0';
        if (enabled && !disabled) {
            for (auto &fn : mod.functions) {
                if (!fn.is_native) ir_pass_unroll(fn);
            }
        }
        ir_verify_phase(mod, "unroll");
    }

    /* Phase D.jit-mem-model AUTO-PROMOTE: marca ALLOCAs que fluyen a
     * CALLN como is_host_ptr=true.  El JIT selector las emite en host
     * stack; el ptr resultante es directamente dereferenciable por
     * funciones nativas (Win API, libc, etc.).  Sin esto, `&local`
     * pasado a CALLN seria VM-addr -> garbage.  Cero anotaciones del
     * usuario: el analisis es backward-flow desde args PTR de CALLN.
     * UNA pasada (no se itera con el resto). */
    if (level >= OptLevel::O1) {
        for (auto &fn : mod.functions) {
            if (!fn.is_native) ir_pass_promote_callned_allocas(fn);
        }
    }

    /* Sprint string-perf-8 (2026-06-02): promueve ALLOCAs LOCALES (no
     * escapan a CALL*, RET, THROW, etc.) a `host_alloca=true`.  El JIT
     * emite `sub rsp, N` en host stack y los LOAD/STORE usan native mov
     * directo (1 instr) en lugar del inline cache check (~10 instr).
     * Skippable via VESTA_NO_PROMOTE_LOCAL_ALLOCAS=1 para A/B testing. */
    if (level >= OptLevel::O1) {
        const char *skip = std::getenv("VESTA_NO_PROMOTE_LOCAL_ALLOCAS");
        if (!skip || skip[0] == '\0' || skip[0] == '0') {
            for (auto &fn : mod.functions) {
                if (!fn.is_native) ir_pass_promote_local_allocas(fn);
            }
        }
    }

    /* Promocionar malloc(N_const)+free(p) locales sin escape a
     * ALLOCA host_alloca.  Convierte ~200-500 ns por alloc/free en
     * loops a ~1 ns (sub/add rsp del host stack).  UNA pasada.
     * Skippable via VESTA_NO_PROMOTE_RAW_ALLOC=1 para A/B testing. */
    if (level >= OptLevel::O1) {
        const char *skip = std::getenv("VESTA_NO_PROMOTE_RAW_ALLOC");
        const bool do_promote = !(skip && skip[0] != '\0' && skip[0] != '0');
        if (do_promote) {
            for (auto &fn : mod.functions) {
                if (!fn.is_native) ir_pass_promote_local_raw_alloc(fn);
            }
        }
    }
    ir_verify_phase(mod, "promote-allocas");

    // Iterar hasta punto fijo o maximo 8 pasadas
    for (int pass = 0; pass < 8; ++pass) {
        bool any = false;

        for (auto &fn : mod.functions) {
            if (fn.is_native) continue; // no optimizar stubs nativos

            // O1: copy + simplify + SR + reassoc + dead-alloc + DCE
            any |= ir_pass_copy_prop(fn);
            any |= ir_pass_simplify(fn);            /* algebraic + cast fold + phi simp */
            any |= ir_pass_strength_reduction(fn);  /* mul/div/mod power-of-2 -> shifts */
            any |= ir_pass_reassoc(fn);             /* (x op c1) op c2 -> x op (c1 op c2) */
            any |= ir_pass_licm(fn);                /* LICM con dominators reales */
            any |= ir_pass_dead_alloc_elim(fn);
            any |= ir_pass_dce(fn, &mod.native_imports);

            if (level >= OptLevel::O2) {
                // O2: plegado de constantes + bloques inalcanzables + TCO.
                any |= ir_pass_const_fold(fn);
                any |= ir_pass_unreachable(fn);
                any |= ir_pass_tailcall(fn);
                // Inline de header trivial de loop -> habilita decjnz fusion.
                any |= ir_pass_inline_loop_header(fn);
                // Dead store elimination: limpia STOREs muertos consecutivos.
                any |= ir_pass_dse(fn);
                // Global const CSE solamente (safer than full CSE).
                // El full CSE local tiene bugs sutiles con LOAD/STORE alias
                // que necesitan alias analysis (deferido a O3+).
                // Global const dedup via DIRECT rewrite (no MOV+copy_prop
                // intermedio).  La version vieja con MOV dejaba is_const
                // stale en fn.values causando fallos no-deterministicos en
                // test 110 (smart pointers SRET).  Esta version sustituye
                // operandos directamente y elimina las CONSTs duplicadas.
                any |= ir_pass_const_cse_entry(fn);
                // CSE local de aritmetica pura (ADD/SUB/MUL/etc.) -- dedupea
                // `add.ptr this, off` triplicados en getters/setters.  Tiene
                // invalidacion correcta para LOAD via side-effects.  Habilita
                // store-to-load forwarding al unificar punteros equivalentes.
                any |= ir_pass_cse(fn);
                // Load Narrow: elide SEXT redundante tras LOAD i8/i16/i32
                // cuando todos los usos son arith narrow-safe (ADD/SUB/MUL/
                // AND/OR/XOR) + STORE/RET del mismo ancho.  Ahorra 3 instr VM
                // por LOAD elidido.  Bench struct_field: ~270M instr ahorradas.
                any |= ir_pass_load_narrow(fn);
                // Segunda ronda de DCE tras plegado/TCO/loop header inline/CSE.
                any |= ir_pass_dce(fn, &mod.native_imports);
            }

            if (level >= OptLevel::O3) {
                // O3: pasadas mas costosas (GVN, scheduling, etc.) -- TBD.
            }
        }

        /* Devirt + inline @ O2 al final de cada iteracion del fix-point.
         * Importante hacerlo despues de las per-function passes para que
         * la inline pass vea las callees OPTIMIZADAS (e.g. Counter.inc
         * con 6 instrs en vez de 12), aprobando inline bajo el threshold. */
        if (level >= OptLevel::O2) {
            if (ir_pass_devirt_monomorphic(mod)) any = true;

            /* (C2): devirt especulativa ESTATICA via guard-chain.
             * Corre tras el devirt monomorfico (que ya resolvio los sites
             * de clase concreta) y ANTES del inline, para que este ultimo
             * procese los CALL directos del fast path.  Lee los candidatos
             * que el lowering registro en fn.spec_devirt_sites.
             * Skippable via VESTA_NO_SPEC_DEVIRT=1 para A/B testing. */
            {
                const char *skip = std::getenv("VESTA_NO_SPEC_DEVIRT");
                const bool do_sd = !(skip && skip[0] != '\0' && skip[0] != '0');
                if (do_sd) {
                    for (auto &fn : mod.functions) {
                        if (fn.is_native) continue;
                        if (ir_pass_spec_devirt(fn)) any = true;
                    }
                }
            }

            if (ir_pass_inline(mod))             any = true;

            /* Phase C2.13: Scalar Replacement de objetos GC no-escapantes.
             * Corre DESPUES del inline (que junta el alloc + los field-access
             * en la misma fn).  Sus reescrituras (loads -> trunc/mov/const)
             * las limpia el const_fold/dce de la siguiente iteracion del
             * fix-point; el alloc GC desaparece por completo.
             * Skippable via VESTA_NO_ESCAPE_SCALAR=1 para A/B testing. */
            {
                const char *skip = std::getenv("VESTA_NO_ESCAPE_SCALAR");
                const bool do_sr = !(skip && skip[0] != '\0' && skip[0] != '0');
                if (do_sr) {
                    for (auto &fn : mod.functions) {
                        if (fn.is_native) continue;
                        if (ir_pass_scalar_replace_gc(fn, mod)) any = true;
                    }
                }
            }
        }
        ir_verify_phase(mod, "fix-point");

        if (!any) break; // punto fijo alcanzado
    }

    /* Phase C2.13: DETECCION (log-only) de objetos GC no-escapantes.  Corre
     * tras el fix-point (con el IR ya inlineado + optimizado, que es donde el
     * escape es visible: el alloc + los field-access estan en la misma fn).
     * No transforma el IR; solo loguea bajo VESTA_ESCAPE_DEBUG. */
    if (level >= OptLevel::O2) {
        for (auto &fn : mod.functions) {
            if (fn.is_native) continue;
            ir_pass_escape_detect_gc(fn);
        }
    }

    /* Final pass: list scheduling para ILP.  Una sola pasada despues del
     * fix-point porque el reordenamiento NO produce mas oportunidades de
     * optimizacion (es semanticamente neutro -- mismo DAG, distinto orden).
     * Solo aplica @ O2+ por seguridad. */
    if (level >= OptLevel::O2) {
        for (auto &fn : mod.functions) {
            if (fn.is_native) continue;
            ir_pass_schedule(fn);
        }
    }
    ir_verify_phase(mod, "final");
}

// Set global de helpers @c __new_<X> marcados como puros por el frontend.
// El frontend lo invoca cuando detecta un ctor trivial (sin @c callvirt al
// ctor user-defined); el DCE puede eliminar la llamada si el handle no se
// usa.  Coste: lookup O(1) amortizado en una sola posicion del pipeline.
static std::unordered_set<std::string> g_pure_new_helpers;

void register_pure_new_helper(const std::string &fn_name) {
    g_pure_new_helpers.insert(fn_name);
}

bool is_pure_new_helper(const std::string &fn_name) {
    return g_pure_new_helpers.count(fn_name) != 0;
}

} // namespace ir
