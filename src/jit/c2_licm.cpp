/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

#include "jit/c2_compiler.h"
#include "jit/c2_heuristics.h"
#include "ir/ssa_ir.h"

#include <algorithm>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jit {
namespace c2_licm {

    // =====================================================================
    //  CFG traversal utilities
    // =====================================================================

    /**
     * @brief Compute dominators for the CFG using a simple iterative algorithm.
     *
     * For each block, dom[b] = {b} ∪ (∩_{p ∈ preds(b)} dom[p]).
     * Iterates until stable.
     */
    static std::vector<std::unordered_set<ir::IrBlockId>> compute_dominators(
        const ir::IrFunction &ir_fn) {
        const size_t n_blocks = ir_fn.blocks.size();
        std::vector<std::unordered_set<ir::IrBlockId>> dom(n_blocks);

        if (n_blocks == 0) return dom;

        // Initialize: all blocks dominated by all blocks
        for (size_t i = 0; i < n_blocks; ++i) {
            for (size_t j = 0; j < n_blocks; ++j) {
                dom[i].insert(static_cast<ir::IrBlockId>(j));
            }
        }

        // dom[entry] = {entry}
        dom[0].clear();
        dom[0].insert(0);

        // Iterative refinement
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t i = 1; i < n_blocks; ++i) {
                std::unordered_set<ir::IrBlockId> new_dom;
                bool first = true;

                const auto &block = ir_fn.blocks[i];
                for (ir::IrBlockId pred : block.preds) {
                    if (pred >= n_blocks) continue;
                    if (first) {
                        new_dom = dom[pred];
                        first = false;
                    } else {
                        std::unordered_set<ir::IrBlockId> intersection;
                        for (ir::IrBlockId b : new_dom) {
                            if (dom[pred].count(b)) {
                                intersection.insert(b);
                            }
                        }
                        new_dom = std::move(intersection);
                    }
                }

                new_dom.insert(static_cast<ir::IrBlockId>(i));

                if (new_dom != dom[i]) {
                    dom[i] = std::move(new_dom);
                    changed = true;
                }
            }
        }

        return dom;
    }

    /**
     * @brief Check if block @p a dominates block @p b.
     */
    static bool dominates(
        ir::IrBlockId a,
        ir::IrBlockId b,
        const std::vector<std::unordered_set<ir::IrBlockId>> &dom) {
        if (b >= dom.size()) return false;
        return dom[b].count(a) > 0;
    }

    // =====================================================================
    //  Loop detection in the IR control flow graph
    // =====================================================================

    std::vector<C2LoopInfo> detect_loops(const ir::IrFunction &ir_fn) {
        std::vector<C2LoopInfo> loops;

        if (ir_fn.blocks.empty()) return loops;

        // Compute dominators
        auto dom = compute_dominators(ir_fn);

        // Find natural loops: for each back-edge (b -> h) where h dominates b
        for (size_t bi = 0; bi < ir_fn.blocks.size(); ++bi) {
            ir::IrBlockId b_id = static_cast<ir::IrBlockId>(bi);
            const auto &block = ir_fn.blocks[bi];

            for (ir::IrBlockId succ : block.succs) {
                // Check if this is a back-edge: succ dominates bi
                if (!dominates(succ, b_id, dom)) continue;

                // Found a back-edge: succ is the loop header, bi is the latch
                C2LoopInfo loop;
                loop.header = succ;
                loop.backedge_src = b_id;

                // Collect blocks in the loop by BFS backwards from latch
                std::queue<ir::IrBlockId> worklist;
                worklist.push(b_id);

                while (!worklist.empty()) {
                    ir::IrBlockId cur = worklist.front();
                    worklist.pop();

                    if (loop.body_blocks.count(cur)) continue;
                    loop.body_blocks.insert(cur);

                    if (cur == succ) continue; // Header is the limit

                    const auto &cur_block = ir_fn.blocks[cur];
                    for (ir::IrBlockId pred : cur_block.preds) {
                        if (!loop.body_blocks.count(pred)) {
                            worklist.push(pred);
                        }
                    }
                }

                // Ensure header is in the body
                loop.body_blocks.insert(succ);

                // Find pre-header: predecessor of header NOT in loop
                const auto &header = ir_fn.blocks[succ];
                for (ir::IrBlockId pred : header.preds) {
                    if (!loop.body_blocks.count(pred)) {
                        loop.preheader = pred;
                        break;
                    }
                }

                // If no pre-header found, create one conceptually (the entry)
                if (loop.preheader == ir::IR_NO_BLOCK) {
                    loop.preheader = 0; // Use entry block as pre-header
                }

                // Compute nesting depth
                loop.depth = 0;
                for (const auto &other : loops) {
                    if (other.header == loop.header) continue;
                    bool nested = true;
                    for (ir::IrBlockId bb : loop.body_blocks) {
                        if (!other.body_blocks.count(bb)) {
                            nested = false;
                            break;
                        }
                    }
                    if (nested) {
                        loop.depth = std::max(loop.depth, other.depth + 1);
                    }
                }

                loops.push_back(std::move(loop));
            }
        }

        return loops;
    }

    // =====================================================================
    //  Safety analysis: check if an instruction is side-effect-free
    // =====================================================================

    /**
     * @brief Check if an IR instruction is safe to hoist (no side effects).
     *
     * Safe operations are pure computations (arithmetic, logic, casts, constants).
     * Operations with side effects (calls, stores, allocations, throws, etc.)
     * or that depend on loop-variant memory cannot be hoisted.
     */
    static bool is_hoistable(const ir::IrInstr &instr) {
        switch (instr.op) {
            // Pure arithmetic - safe to hoist
            case ir::IrOp::CONST:
            case ir::IrOp::ADD:
            case ir::IrOp::SUB:
            case ir::IrOp::MUL:
            case ir::IrOp::AND:
            case ir::IrOp::OR:
            case ir::IrOp::XOR:
            case ir::IrOp::SHL:
            case ir::IrOp::SHR:
            case ir::IrOp::SAR:
            case ir::IrOp::NEG:
            case ir::IrOp::NOT:
            case ir::IrOp::IABS:
            case ir::IrOp::IMIN:
            case ir::IrOp::IMAX:
            case ir::IrOp::POPCNT:
            case ir::IrOp::CLZ:
            case ir::IrOp::CTZ:
            case ir::IrOp::ROTL:
            case ir::IrOp::ROTR:
            case ir::IrOp::BYTESWAP:

            // Casts - safe to hoist
            case ir::IrOp::CAST:
            case ir::IrOp::ZEXT:
            case ir::IrOp::SEXT:
            case ir::IrOp::TRUNC:
            case ir::IrOp::BITCAST:

            // Float arithmetic - safe to hoist (pure)
            case ir::IrOp::FADD:
            case ir::IrOp::FSUB:
            case ir::IrOp::FMUL:
            case ir::IrOp::FDIV:
            case ir::IrOp::FNEG:
            case ir::IrOp::FABS:
            case ir::IrOp::FSQRT:
            case ir::IrOp::FMIN:
            case ir::IrOp::FMAX:
            case ir::IrOp::FFLOOR:
            case ir::IrOp::FCEIL:
            case ir::IrOp::FROUND:
            case ir::IrOp::FTRUNC:
            case ir::IrOp::ITOF:
            case ir::IrOp::UITOF:
            case ir::IrOp::FTOI:
            case ir::IrOp::FTOUI:
            case ir::IrOp::F32TOF64:
            case ir::IrOp::F64TOF32:

            // Comparisons - safe to hoist
            case ir::IrOp::CMP_EQ:
            case ir::IrOp::CMP_NE:
            case ir::IrOp::CMP_LT:
            case ir::IrOp::CMP_GT:
            case ir::IrOp::CMP_LE:
            case ir::IrOp::CMP_GE:
            case ir::IrOp::CMP_ULT:
            case ir::IrOp::CMP_UGT:
            case ir::IrOp::CMP_ULE:
            case ir::IrOp::CMP_UGE:
            case ir::IrOp::FCMP_EQ:
            case ir::IrOp::FCMP_NE:
            case ir::IrOp::FCMP_LT:
            case ir::IrOp::FCMP_GT:
            case ir::IrOp::FCMP_LE:
            case ir::IrOp::FCMP_GE:

            // MOV - safe to hoist (copy propagation)
            case ir::IrOp::MOV:
                return true;

            // NOT safe to hoist:
            default:
                return false;
        }
    }

    /**
     * @brief Check if an instruction's operands are all loop-invariant.
     *
     * A value is loop-invariant if it is:
     *   - A constant (is_const == true)
     *   - A parameter (is_param == true)
     *   - Defined outside the loop (in a dominating block not in loop body)
     *   - Defined by another invariant instruction already hoisted
     */
    static bool are_operands_invariant(
        const ir::IrInstr &instr,
        const ir::IrFunction &ir_fn,
        const C2LoopInfo &loop,
        const std::unordered_set<ir::IrValueId> &invariant_values) {
        for (ir::IrValueId op : instr.operands) {
            if (op >= ir_fn.values.size()) continue;

            // Constants and params are always invariant
            if (ir_fn.values[op].is_const) continue;
            if (ir_fn.values[op].is_param) continue;

            // Check if already marked as invariant
            if (invariant_values.count(op)) continue;

            // Check where the value is defined
            bool defined_outside = true;
            for (const auto &block : ir_fn.blocks) {
                if (!loop.body_blocks.count(block.id)) continue;
                for (const auto &bi : block.instrs) {
                    if (bi.dst == op) {
                        defined_outside = false;
                        break;
                    }
                }
                if (!defined_outside) break;
            }

            if (defined_outside) continue;

            return false;
        }
        return true;
    }

    // =====================================================================
    //  Hoist invariant instructions to the loop pre-header
    // =====================================================================

    bool hoist_invariants(ir::IrFunction &ir_fn,
                          const std::vector<C2LoopInfo> &loops) {
        if (loops.empty()) return false;

        bool any_hoisted = false;

        for (const auto &loop : loops) {
            if (loop.preheader >= ir_fn.blocks.size()) continue;
            if (loop.header == ir::IR_NO_BLOCK) continue;

            auto &preheader = ir_fn.blocks[loop.preheader];
            size_t hoist_count = 0;

            // Set of values that are invariant (to handle chaining)
            std::unordered_set<ir::IrValueId> invariant_values;

            // Iterate to fixed point: hoistable instructions may enable more
            bool changed = true;
            while (changed) {
                changed = false;

                for (ir::IrBlockId b_id : loop.body_blocks) {
                    if (b_id >= ir_fn.blocks.size()) continue;
                    if (b_id == loop.header) continue; // Don't hoist from header
                    auto &block = ir_fn.blocks[b_id];

                    for (size_t i = 0; i < block.instrs.size(); ++i) {
                        const auto &instr = block.instrs[i];

                        // Skip terminators
                        if (instr.op == ir::IrOp::BR ||
                            instr.op == ir::IrOp::BR_COND ||
                            instr.op == ir::IrOp::RET ||
                            instr.op == ir::IrOp::UNREACHABLE) {
                            continue;
                        }

                        // Skip PHIs
                        if (instr.op == ir::IrOp::PHI) continue;

                        // Check hoistability
                        if (!is_hoistable(instr)) continue;
                        if (!are_operands_invariant(
                                instr, ir_fn, loop, invariant_values)) {
                            continue;
                        }

                        if (hoist_count >= c2::LICM_MAX_HOIST) break;

                        // Hoist: clone the instruction to preheader
                        ir::IrInstr hoisted = instr;
                        preheader.instrs.push_back(std::move(hoisted));

                        // Mark the destination value as invariant
                        if (instr.dst != ir::IR_NO_VALUE) {
                            invariant_values.insert(instr.dst);
                        }

                        // Remove from original position
                        block.instrs.erase(block.instrs.begin() +
                            static_cast<decltype(block.instrs)::difference_type>(i));
                        --i;

                        any_hoisted = true;
                        changed = true;
                        hoist_count++;
                    }

                    if (hoist_count >= c2::LICM_MAX_HOIST) break;
                }
            }
        }

        return any_hoisted;
    }

} // namespace c2_licm
} // namespace jit
