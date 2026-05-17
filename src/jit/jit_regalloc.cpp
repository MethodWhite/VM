/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/jit_regalloc.cpp
 * @brief Implementacion del linear scan regalloc para el JIT.
 *
 * = Algoritmo MVP =
 *
 * Phase v1 es deliberadamente conservadora.  Para cada VID:
 *   1. Recolectar use_count + live_range_start + live_range_end via
 *      linealizacion de las instrucciones IR.
 *   2. Detectar si el live range cruza algun @c IrOp::CALL.
 *   3. Filtrar: NO @c is_gc_object, NO @c is_host_ptr, NO float (F32/F64),
 *      NO PHI dst con args que tambien son pinned (parallel-move
 *      complexity), NO valores con un solo uso (no vale la pena).
 *   4. Ordenar candidatos por @c use_count desc.
 *   5. Asignar top-K (K=4) al pool R12, R13, R14, R15 sin que dos VIDs
 *      con live ranges solapados compartan reg.
 *
 * El v2 (Phase D.8) implementara linear scan completo con spill,
 * split, coalescing, etc.
 */

#include "jit/jit_regalloc.h"
#include "ir/liveness.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>

namespace jit {

    namespace {

        /// Pool de regs callee-saved usables (en ambos ABIs SysV+Win64).
        constexpr MReg CALLEE_SAVED_POOL[] = {
            MReg::R12, MReg::R13, MReg::R14, MReg::R15
        };
        constexpr size_t POOL_SIZE = sizeof(CALLEE_SAVED_POOL) / sizeof(CALLEE_SAVED_POOL[0]);

        /**
         * @struct VidInfo
         * @brief Info recolectada por VID durante el analisis.
         */
        struct VidInfo {
            ir::IrValueId vid = 0;
            uint32_t      use_count = 0;
            uint32_t      def_pos = UINT32_MAX;   ///< posicion lineal del def
            uint32_t      last_use_pos = 0;       ///< posicion lineal del ultimo uso
            bool          crosses_call = false;
            bool          excluded = false;       ///< excluido por filtros
        };

        /**
         * @brief Linealiza las instrucciones de @c fn en orden y devuelve
         *        info por VID.
         *
         * Reusa @c ir::compute_liveness que maneja correctamente
         * back-edges, PHI args, y dataflow live-in/live-out.  Los
         * live ranges resultantes son robustos para uso por linear
         * scan estandar.
         */
        std::vector<VidInfo> collect_vid_info(const ir::IrFunction &fn) {
            std::vector<VidInfo> info(fn.values.size());
            for (size_t i = 0; i < info.size(); ++i) {
                info[i].vid = static_cast<ir::IrValueId>(i);
            }

            /* Liveness completa (con back-edge + live-in/out dataflow). */
            const ir::LivenessResult live = ir::compute_liveness(fn);

            /* Volcar def/end de cada interval a info[vid]. */
            for (const auto &iv : live.intervals) {
                if (iv.id >= info.size()) continue;
                info[iv.id].def_pos = iv.def;
                info[iv.id].last_use_pos = iv.end;
            }

            /* Contar usos lexicos y detectar CALLs.
             *
             * use_count es heuristica para priorizar pinning (mas usos =
             * mas vale la pena).  Linealizacion identica a compute_liveness:
             * block_b ocupa [block_start[b], block_end[b]] inclusive. */
            std::vector<uint32_t> call_positions;
            for (size_t b = 0; b < fn.blocks.size(); ++b) {
                const uint32_t bs = (b < live.block_start.size())
                                    ? live.block_start[b] : 0;
                for (size_t i = 0; i < fn.blocks[b].instrs.size(); ++i) {
                    const auto &ins = fn.blocks[b].instrs[i];
                    const uint32_t pos = bs + static_cast<uint32_t>(i);
                    /* uses lexicos para heuristica de use_count */
                    for (auto opv : ins.operands) {
                        if (opv == ir::IR_NO_VALUE || opv >= info.size()) continue;
                        info[opv].use_count++;
                    }
                    for (const auto &arg : ins.phi_args) {
                        if (arg.value == ir::IR_NO_VALUE || arg.value >= info.size()) continue;
                        info[arg.value].use_count++;
                    }
                    /* CALLs (caller-saved clobber + GC poll potencial). */
                    bool is_call_like = false;
                    switch (ins.op) {
                        case ir::IrOp::CALL:
                        case ir::IrOp::CALLVIRT:
                        case ir::IrOp::CALLM:
                        case ir::IrOp::CALLN:
                        case ir::IrOp::CALLIND:
                        case ir::IrOp::CALLCLOSURE:
                        case ir::IrOp::NEWOBJ:
                        case ir::IrOp::GC_ALLOC:
                        case ir::IrOp::GCDEREF_IR:
                        case ir::IrOp::RAW_FREE:
                        case ir::IrOp::THROW:
                        case ir::IrOp::TAILCALL:
                            is_call_like = true;
                            break;
                        case ir::IrOp::RAW_ASM:
                            if (ins.is_call_site) is_call_like = true;
                            break;
                        default:
                            break;
                    }
                    if (is_call_like) call_positions.push_back(pos);
                }
            }

            /* Marcar VIDs cuyo live range cruza alguna CALL. */
            for (auto &vi : info) {
                if (vi.def_pos == UINT32_MAX) {
                    vi.excluded = true;
                    continue;
                }
                for (uint32_t cp : call_positions) {
                    if (cp > vi.def_pos && cp <= vi.last_use_pos) {
                        vi.crosses_call = true;
                        break;
                    }
                }
            }

            return info;
        }

        /**
         * @brief Filtra los candidatos a pinning, marcando @c excluded
         *        para los que no califican.
         *
         * Tras integrar @c ir::compute_liveness los live ranges son
         * correctos para PHI dst/args (back-edge handling + live-out
         * dataflow).  Eso permite incluirlos como candidatos siempre que
         * los demas filtros (GC, host_ptr, float) se respeten.
         */
        void filter_candidates(const ir::IrFunction &fn,
                               std::vector<VidInfo> &info) {
            (void)fn;
            for (auto &vi : info) {
                if (vi.excluded) continue;
                if (vi.vid >= fn.values.size()) {
                    vi.excluded = true;
                    continue;
                }
                const auto &val = fn.values[vi.vid];
                /* Excluir is_gc_object: stackmaps en v1 solo describen
                 * slots; pin a reg complicaria GC walk. */
                if (val.is_gc_object) {
                    vi.excluded = true;
                    continue;
                }
                /* Excluir is_host_ptr: tambien evita complicaciones de
                 * GC + safe pra ya que estos VIDs suelen ser cortos. */
                if (val.is_host_ptr) {
                    vi.excluded = true;
                    continue;
                }
                /* Excluir floats: el selector no los soporta plenamente
                 * todavia, y pinning a XMM regs es otro pool. */
                if (val.type == ir::IrType::F32 || val.type == ir::IrType::F64) {
                    vi.excluded = true;
                    continue;
                }
                /* Excluir VIDs sin usos. */
                if (vi.use_count < 1) {
                    vi.excluded = true;
                    continue;
                }
            }
        }

        /**
         * @brief Detecta si dos live ranges se solapan ESTRICTAMENTE.
         *
         * Touching (a.end == b.def o b.end == a.def) NO cuenta como
         * overlap: en ese punto exacto la instruccion lee uno y
         * escribe el otro, asi que compartir reg es legal.  Estandar
         * en SSA destruction con coalescing.
         */
        inline bool ranges_overlap(const VidInfo &a, const VidInfo &b) noexcept {
            if (a.last_use_pos <= b.def_pos) return false;
            if (b.last_use_pos <= a.def_pos) return false;
            return true;
        }

        /**
         * @brief Asigna candidatos a regs siguiendo el algoritmo MVP.
         *
         * @return @c JitRegalloc con vid_to_reg + callee_saved_used.
         */
        JitRegalloc assign_regs(const ir::IrFunction &fn,
                                std::vector<VidInfo> &info) {
            JitRegalloc result;

            /* Recolectar candidatos no excluidos. */
            std::vector<size_t> candidates;
            candidates.reserve(info.size());
            for (size_t i = 0; i < info.size(); ++i) {
                if (!info[i].excluded) candidates.push_back(i);
            }

            /* Ordenar por use_count desc, then by def_pos asc para
             * estabilidad. */
            std::sort(candidates.begin(), candidates.end(),
                [&](size_t a, size_t b) {
                    if (info[a].use_count != info[b].use_count)
                        return info[a].use_count > info[b].use_count;
                    return info[a].def_pos < info[b].def_pos;
                });

            /* Construir partners de coalesce via relaciones PHI.
             * VIDs conectados por una arista PHI (dst <-> arg) son
             * candidatos a compartir el mismo reg.  En el chequeo de
             * overlap, ignoramos parejas coalescibles porque su
             * "solape" en el punto del phi-copy es benigno (la copia
             * cambia el valor en el reg de un VID al otro). */
            std::unordered_map<ir::IrValueId, std::unordered_set<ir::IrValueId>>
                phi_partners;
            for (const auto &bb : fn.blocks) {
                for (const auto &ins : bb.instrs) {
                    if (ins.op != ir::IrOp::PHI) continue;
                    if (ins.dst == ir::IR_NO_VALUE) continue;
                    for (const auto &arg : ins.phi_args) {
                        if (arg.value == ir::IR_NO_VALUE) continue;
                        phi_partners[ins.dst].insert(arg.value);
                        phi_partners[arg.value].insert(ins.dst);
                    }
                }
            }

            /* Helper: comprueba si dos VIDs (info indices) tienen
             * overlap REAL considerando si son coalesce-partners. */
            auto effective_overlap = [&](size_t a_idx, size_t b_idx) -> bool {
                const auto &a = info[a_idx];
                const auto &b = info[b_idx];
                if (!ranges_overlap(a, b)) return false;
                /* Son partners? -> overlap benigno. */
                auto it = phi_partners.find(a.vid);
                if (it != phi_partners.end() && it->second.count(b.vid)) {
                    return false;
                }
                return true;
            };

            /* Pool de regs callee-saved: each holds a list of VIDs
             * already assigned to that reg (sus live ranges no se
             * solapan).  Asignamos al primer reg cuya carga sea
             * compatible. */
            std::vector<std::vector<size_t>> reg_assignments(POOL_SIZE);

            /* Helper: indice del reg en POOL, -1 si no esta. */
            auto pool_index_of = [&](MReg r) -> int {
                for (size_t i = 0; i < POOL_SIZE; ++i) {
                    if (CALLEE_SAVED_POOL[i] == r) return static_cast<int>(i);
                }
                return -1;
            };

            /* Mapa VID -> indice en info para lookup rapido. */
            std::unordered_map<ir::IrValueId, size_t> vid_to_info_idx;
            for (size_t i = 0; i < info.size(); ++i) {
                vid_to_info_idx[info[i].vid] = i;
            }

            for (size_t cidx : candidates) {
                /* Pre-coalesce: si este VID es phi_partner de alguno ya
                 * asignado, preferir el MISMO reg si no hay conflicto.
                 *
                 * Esto realiza coalescing online: el primer VID asignado
                 * en cada clase de coalesce determina el reg; los
                 * siguientes intentan unirse. */
                bool placed = false;
                int preferred_r = -1;
                auto pit = phi_partners.find(info[cidx].vid);
                if (pit != phi_partners.end()) {
                    for (auto pv : pit->second) {
                        auto vit = result.vid_to_reg.find(pv);
                        if (vit == result.vid_to_reg.end()) continue;
                        const int r = pool_index_of(vit->second);
                        if (r < 0) continue;
                        /* Verificar que cidx encaja en r sin conflictar
                         * con VIDs ya asignados a r (que no sean
                         * coalesce-partners del propio cidx). */
                        bool conflict = false;
                        for (size_t other : reg_assignments[r]) {
                            if (effective_overlap(cidx, other)) {
                                conflict = true; break;
                            }
                        }
                        if (!conflict) {
                            preferred_r = r;
                            break;
                        }
                    }
                }
                if (preferred_r >= 0) {
                    reg_assignments[preferred_r].push_back(cidx);
                    result.vid_to_reg[info[cidx].vid] =
                        CALLEE_SAVED_POOL[preferred_r];
                    placed = true;
                }
                if (!placed) {
                    for (size_t r = 0; r < POOL_SIZE; ++r) {
                        bool overlaps_any = false;
                        for (size_t other : reg_assignments[r]) {
                            if (effective_overlap(cidx, other)) {
                                overlaps_any = true;
                                break;
                            }
                        }
                        if (!overlaps_any) {
                            reg_assignments[r].push_back(cidx);
                            result.vid_to_reg[info[cidx].vid] = CALLEE_SAVED_POOL[r];
                            placed = true;
                            break;
                        }
                    }
                }
                /* Si no encaja en ningun reg, sigue al spill (slot). */
                (void)placed;
            }

            (void)vid_to_info_idx; /* reservado para optimizaciones futuras. */

            /* Determinar que regs callee-saved usamos (no vacios). */
            for (size_t r = 0; r < POOL_SIZE; ++r) {
                if (!reg_assignments[r].empty()) {
                    result.callee_saved_used.push_back(CALLEE_SAVED_POOL[r]);
                }
            }

            /* Forzar conteo PAR para mantener alignment del stack.
             *
             * Cada @c push es 8 bytes.  Para que rsp quede 16-aligned
             * antes del @c sub rsp, FRAME del prologue, el numero total
             * de @c push debe respetar la formula del frame_size que el
             * selector ya calcula.  Como el selector usa @c ALIGN_PAD=8
             * fijo, los pushes adicionales deben ser un numero PAR para
             * no romper el alignment.
             *
             * Si tenemos un numero impar de regs asignados, descartamos
             * el ultimo (sus VIDs caen al slot fallback). */
            if ((result.callee_saved_used.size() & 1u) != 0u) {
                const MReg dropped = result.callee_saved_used.back();
                result.callee_saved_used.pop_back();
                /* Demote VIDs asignados a este reg de vuelta a slots. */
                for (auto it = result.vid_to_reg.begin();
                     it != result.vid_to_reg.end(); ) {
                    if (it->second == dropped) {
                        it = result.vid_to_reg.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            return result;
        }

    } // namespace anonymous

    /* ===================================================================== */
    /* API publica                                                            */
    /* ===================================================================== */

    JitRegalloc compute_jit_regalloc(const ir::IrFunction &fn) {
        /* Fast path: regalloc desactivado via env var. */
        static const bool s_disabled = []() {
            const char *e = std::getenv("VESTA_JIT_NO_REGALLOC");
            return e && e[0] != '\0' && e[0] != '0';
        }();
        if (s_disabled) return JitRegalloc{};

        /* Debug via @c VESTA_JIT_REGALLOC_DEBUG=1: imprime asignaciones a
         * stderr.  Util para diagnosticar pinning de VIDs especificos. */
        static const bool s_debug = []() {
            const char *e = std::getenv("VESTA_JIT_REGALLOC_DEBUG");
            return e && e[0] != '\0' && e[0] != '0';
        }();

        auto info = collect_vid_info(fn);
        filter_candidates(fn, info);
        auto result = assign_regs(fn, info);

        if (s_debug) {
            std::fprintf(stderr, "[jit-regalloc] fn=%s assigned=%zu callee_saved=%zu\n",
                fn.name.c_str(),
                result.vid_to_reg.size(),
                result.callee_saved_used.size());
            for (const auto &p : result.vid_to_reg) {
                uint32_t uc = (p.first < info.size()) ? info[p.first].use_count : 0;
                bool xc = (p.first < info.size()) ? info[p.first].crosses_call : false;
                std::fprintf(stderr, "  vid=%u reg_id=%u use=%u xcall=%d\n",
                    p.first, static_cast<unsigned>(p.second), uc, xc ? 1 : 0);
            }
        }
        return result;
    }

    void apply_jit_regalloc_rewrite(MFunction &mf,
                                    const ir::IrFunction &fn,
                                    const JitRegalloc &regalloc) {
        if (regalloc.empty()) return;

        /* Helper: rewrite un MEM operand a REG si su offset corresponde
         * a un VID pinned. */
        auto try_rewrite = [&](MOperand &op) {
            if (op.kind != MOperandKind::MEM) return;
            if (op.reg != static_cast<uint8_t>(MReg::RBP)) return;
            /* MEM con index distinto de NONE no es slot stack. */
            if (op.mem_index() != MReg::NONE) return;
            const int32_t vid = slot_offset_to_vid(op.value);
            if (vid < 0) return;
            auto it = regalloc.vid_to_reg.find(static_cast<ir::IrValueId>(vid));
            if (it == regalloc.vid_to_reg.end()) return;
            /* Preservar width original del operand. */
            const uint8_t orig_width = op.width;
            /* MEM.width tiene packed scale|index info que NO es el
             * tamano del operand; el tamano viene del dst/src REG del
             * MInstr.  Para preservar correctness del encoder, usar
             * width=8 (default) ya que el slot es siempre 8 bytes. */
            op = MOperand::make_reg(it->second, 8);
            (void)orig_width;
        };

        for (auto &block : mf.blocks) {
            for (auto &mi : block.instrs) {
                try_rewrite(mi.dst);
                try_rewrite(mi.src1);
                try_rewrite(mi.src2);
            }
        }

        /* Suprimir el unused warning si fn no es necesaria. */
        (void)fn;
    }

} // namespace jit
