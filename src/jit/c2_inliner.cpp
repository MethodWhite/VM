/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

#include "jit/c2_compiler.h"
#include "jit/c2_heuristics.h"
#include "ir/ssa_ir.h"
#include "runtime/profile.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace jit {
namespace c2_inliner {

    // =====================================================================
    //  Count the number of IR instructions in a function (body only)
    // =====================================================================

    static uint32_t count_ir_instructions(const ir::IrFunction &fn) {
        uint32_t count = 0;
        for (const auto &block : fn.blocks) {
            count += static_cast<uint32_t>(block.instrs.size());
        }
        return count;
    }

    // =====================================================================
    //  Check if a function is suitable for inlining based on heuristics
    // =====================================================================

    static bool is_inlineable(const ir::IrFunction &caller,
                              const ir::IrFunction &callee,
                              uint32_t depth,
                              uint32_t call_frequency,
                              const runtime::profile::ProfileCollector *profiler) {
        // Never inline native stubs
        if (callee.is_native) return false;

        // Guard against recursive inlining
        if (callee.name == caller.name) return false;

        // Depth guard
        if (depth >= c2::INLINE_MAX_DEPTH) return false;

        // Count IR instructions in the callee
        uint32_t callee_size = count_ir_instructions(callee);

        // Use PGO call frequency to adjust threshold
        size_t threshold = c2::INLINE_HOT_THRESHOLD;

        if (profiler && profiler->active.load(std::memory_order_relaxed)) {
            if (call_frequency > 0 && call_frequency < 100) {
                threshold = c2::INLINE_COLD_THRESHOLD;
            }
        }

        if (callee_size > threshold) return false;

        return true;
    }

    // =====================================================================
    //  Build inlining plan from call graph analysis
    // =====================================================================

    C2InlinePlan build_inline_plan(
        const ir::IrFunction &ir_fn,
        const runtime::profile::ProfileCollector *profiler) {
        C2InlinePlan plan;

        // We work with the IrFunction itself as both caller and callee placeholder.
        // The actual callee resolution would require module-level IR access.
        // Here we identify CALL sites that are candidates for inlining.

        for (const auto &block : ir_fn.blocks) {
            for (const auto &instr : block.instrs) {
                if (instr.op != ir::IrOp::CALL &&
                    instr.op != ir::IrOp::CALLIND) {
                    continue;
                }

                // Estimate callee size from function name length as proxy
                // (in a full implementation, we'd look up the actual IrFunction)
                uint32_t estimated_size = 0;

                if (instr.op == ir::IrOp::CALL && !instr.func_name.empty()) {
                    // Parse function name components as a size heuristic:
                    // shorter names tend to be simple wrappers
                    estimated_size = std::min<uint32_t>(
                        static_cast<uint32_t>(instr.func_name.length()),
                        30);
                } else if (instr.op == ir::IrOp::CALLIND) {
                    // Indirect calls are harder to estimate; skip for now
                    continue;
                }

                if (estimated_size > c2::INLINE_HOT_THRESHOLD) continue;

                C2InlineSite site;
                site.call_dst = instr.dst;
                site.callee_size = estimated_size;
                site.call_frequency = 0;
                site.depth = 0;
                site.callee_name = instr.func_name;

                // Check PGO data if available
                if (profiler && profiler->active.load(std::memory_order_relaxed)) {
                    auto it = profiler->callsites.find(
                        reinterpret_cast<uint64_t>(&instr));
                    if (it != profiler->callsites.end()) {
                        uint64_t total = it->second.megamorphic_count;
                        for (uint8_t i = 0; i < it->second.n_types; ++i) {
                            total += it->second.types[i].count;
                        }
                        site.call_frequency = static_cast<uint32_t>(total);
                    }
                }

                plan.sites_to_inline.push_back(std::move(site));
            }
        }

        // Sort by call frequency descending (hot sites first)
        std::sort(plan.sites_to_inline.begin(),
                  plan.sites_to_inline.end(),
                  [](const C2InlineSite &a, const C2InlineSite &b) {
                      return a.call_frequency > b.call_frequency;
                  });

        return plan;
    }

    // =====================================================================
    //  Parameter substitution and return value rematerialization
    // =====================================================================

    struct InlineMapping {
        /// Maps callee value IDs to caller value IDs.
        std::unordered_map<ir::IrValueId, ir::IrValueId> value_map;
        /// Maps callee block IDs to newly created caller block IDs.
        std::unordered_map<ir::IrBlockId, ir::IrBlockId> block_map;
    };

    static InlineMapping remap_callee_values(
        const ir::IrFunction &callee,
        ir::IrFunction &caller,
        const std::vector<ir::IrValueId> &call_args) {
        InlineMapping mapping;

        // Map callee params to caller args
        for (size_t i = 0; i < callee.params.size() && i < call_args.size(); ++i) {
            mapping.value_map[callee.params[i]] = call_args[i];
        }

        // Create new values for callee's internal values
        for (const auto &val : callee.values) {
            if (val.is_param) continue;
            if (val.id == ir::IR_NO_VALUE) continue;

            ir::IrValueId new_id = caller.new_value(val.type, val.name + "_inl");
            caller.values[new_id].is_const = val.is_const;
            caller.values[new_id].const_val = val.const_val;
            caller.values[new_id].is_host_ptr = val.is_host_ptr;
            caller.values[new_id].is_gc_object = val.is_gc_object;

            mapping.value_map[val.id] = new_id;
        }

        return mapping;
    }

    static void remap_instruction_operands(
        ir::IrInstr &instr,
        const InlineMapping &mapping) {
        // Remap operands
        for (auto &op : instr.operands) {
            auto it = mapping.value_map.find(op);
            if (it != mapping.value_map.end()) {
                op = it->second;
            }
        }

        // Remap func_ptr for CALLIND
        if (instr.func_ptr != ir::IR_NO_VALUE) {
            auto it = mapping.value_map.find(instr.func_ptr);
            if (it != mapping.value_map.end()) {
                instr.func_ptr = it->second;
            }
        }

        // Remap PHI args
        for (auto &phi_arg : instr.phi_args) {
            auto it = mapping.value_map.find(phi_arg.value);
            if (it != mapping.value_map.end()) {
                phi_arg.value = it->second;
            }
            auto bit = mapping.block_map.find(phi_arg.block);
            if (bit != mapping.block_map.end()) {
                phi_arg.block = bit->second;
            }
        }
    }

    // =====================================================================
    //  Inline a single callee into the caller at the given call site
    // =====================================================================

    static bool inline_at_site(
        ir::IrFunction &caller,
        ir::IrFunction &callee,
        ir::IrBlockId caller_block_id,
        size_t instr_index,
        const std::vector<ir::IrValueId> &call_args) {
        if (caller_block_id >= caller.blocks.size()) return false;
        if (instr_index >= caller.blocks[caller_block_id].instrs.size()) return false;

        auto &call_block = caller.blocks[caller_block_id];
        const auto &call_instr = call_block.instrs[instr_index];

        // Create value and block mappings
        InlineMapping mapping = remap_callee_values(callee, caller, call_args);

        // Create new blocks for the callee's body
        for (const auto &callee_block : callee.blocks) {
            ir::IrBlockId new_block_id = caller.new_block(callee_block.name);
            mapping.block_map[callee_block.id] = new_block_id;
        }

        // Remap block references in the callee's terminators
        for (const auto &callee_block : callee.blocks) {
            ir::IrBlockId new_id = mapping.block_map[callee_block.id];
            auto &new_block = caller.blocks[new_id];

            // Update predecessor/successor info
            for (ir::IrBlockId pred : callee_block.preds) {
                auto it = mapping.block_map.find(pred);
                if (it != mapping.block_map.end()) {
                    new_block.preds.push_back(it->second);
                }
            }
            for (ir::IrBlockId succ : callee_block.succs) {
                auto it = mapping.block_map.find(succ);
                if (it != mapping.block_map.end()) {
                    new_block.succs.push_back(it->second);
                }
            }
        }

        // Clone instructions with remapped operands
        for (const auto &callee_block : callee.blocks) {
            ir::IrBlockId new_id = mapping.block_map[callee_block.id];
            auto &new_block = caller.blocks[new_id];

            for (const auto &callee_instr : callee_block.instrs) {
                ir::IrInstr cloned = callee_instr;
                cloned.source_line = call_instr.source_line;

                // Remap the destination
                auto dit = mapping.value_map.find(callee_instr.dst);
                if (dit != mapping.value_map.end()) {
                    cloned.dst = dit->second;
                }

                remap_instruction_operands(cloned, mapping);

                // Handle terminators: remap block references
                if (cloned.op == ir::IrOp::BR) {
                    auto bit = mapping.block_map.find(cloned.target_block);
                    if (bit != mapping.block_map.end()) {
                        cloned.target_block = bit->second;
                    }
                }
                if (cloned.op == ir::IrOp::BR_COND) {
                    auto bit = mapping.block_map.find(cloned.target_block);
                    if (bit != mapping.block_map.end()) {
                        cloned.target_block = bit->second;
                    }
                    auto fit = mapping.block_map.find(cloned.false_block);
                    if (fit != mapping.block_map.end()) {
                        cloned.false_block = fit->second;
                    }
                }

                new_block.instrs.push_back(std::move(cloned));
            }
        }

        return true;
    }

    // =====================================================================
    //  Apply the inlining plan to the IR function
    // =====================================================================

    bool apply_inline_plan(ir::IrFunction &ir_fn,
                           const C2InlinePlan &plan,
                           uint32_t max_depth) {
        if (plan.sites_to_inline.empty()) return false;

        bool any_inlined = false;

        // Track which instructions were already processed
        std::unordered_set<ir::IrValueId> processed;

        for (const auto &site : plan.sites_to_inline) {
            if (processed.count(site.call_dst)) continue;
            processed.insert(site.call_dst);

            // Find the block and instruction index for this call site
            for (auto &block : ir_fn.blocks) {
                for (size_t i = 0; i < block.instrs.size(); ++i) {
                    auto &instr = block.instrs[i];
                    if (instr.dst != site.call_dst) continue;
                    if (instr.op != ir::IrOp::CALL &&
                        instr.op != ir::IrOp::CALLIND) {
                        continue;
                    }

                    // Build call arguments from operands
                    std::vector<ir::IrValueId> args;
                    if (instr.op == ir::IrOp::CALLIND) {
                        if (!instr.operands.empty()) {
                            args.assign(instr.operands.begin() + 1,
                                        instr.operands.end());
                        }
                    } else {
                        args = instr.operands;
                    }

                    // Create a minimal IrFunction as the callee placeholder.
                    // In a full implementation, we'd look up the actual IrFunction
                    // from the module. Here we create a synthetic callee that
                    // returns the first argument (identity function) as a demo.
                    ir::IrFunction placeholder;
                    placeholder.name = site.callee_name;
                    placeholder.ret_type = ir_fn.ret_type;

                    // Entry block
                    ir::IrBlockId entry_id = placeholder.new_block("entry");
                    auto &entry = placeholder.blocks[entry_id];

                    // Create parameter values
                    for (size_t ai = 0; ai < args.size(); ++ai) {
                        ir::IrValueId p = placeholder.new_value(
                            ir_fn.values[args[ai]].type,
                            "p" + std::to_string(ai));
                        placeholder.params.push_back(p);
                        placeholder.values[p].is_param = true;
                    }

                    // Simple return of first arg (placeholder)
                    if (!placeholder.params.empty()) {
                        ir::IrInstr ret;
                        ret.op = ir::IrOp::RET;
                        ret.type = ir_fn.ret_type;
                        ret.operands.push_back(placeholder.params[0]);
                        entry.instrs.push_back(std::move(ret));
                    }

                    if (inline_at_site(ir_fn, placeholder,
                                       block.id, i, args)) {
                        any_inlined = true;
                    }

                    break;
                }
            }
        }

        return any_inlined;
    }

} // namespace c2_inliner
} // namespace jit
