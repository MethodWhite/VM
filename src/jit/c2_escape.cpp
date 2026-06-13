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
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jit {
namespace c2_escape {

    // =====================================================================
    //  Escape graph node types
    // =====================================================================

    enum class EscapeNodeKind : uint8_t {
        ALLOCATION,   ///< NEWOBJ, GC_ALLOC, ARRAY_ALLOC
        FORMAL_PARAM, ///< Function parameter
        CALL_RETURN,  ///< Return value from a call
        FIELD,        ///< Field of an object (GETFIELD-derived)
        PHI,          ///< Phi node
        UNKNOWN       ///< Unknown/unanalyzable (treats as escaping)
    };

    struct EscapeNode {
        ir::IrValueId   value_id  = ir::IR_NO_VALUE;
        EscapeNodeKind  kind      = EscapeNodeKind::UNKNOWN;
        bool            escapes   = false;
        bool            visited   = false;

        /// Set of value IDs that this node points to.
        std::unordered_set<ir::IrValueId> points_to;
        /// Set of value IDs that point to this node.
        std::unordered_set<ir::IrValueId> pointed_by;
    };

    // =====================================================================
    //  Escape graph
    // =====================================================================

    struct EscapeGraph {
        std::unordered_map<ir::IrValueId, EscapeNode> nodes;
        size_t object_count = 0;
    };

    // =====================================================================
    //  Build the escape graph from the IR
    // =====================================================================

    static EscapeGraph build_escape_graph(const ir::IrFunction &ir_fn) {
        EscapeGraph graph;

        // Check max objects limit
        if (ir_fn.values.size() > c2::ESCAPE_ANALYSIS_MAX_OBJECTS) {
            return graph;
        }

        // First pass: identify allocation sites
        for (const auto &block : ir_fn.blocks) {
            for (const auto &instr : block.instrs) {
                if (instr.dst == ir::IR_NO_VALUE) continue;

                EscapeNodeKind kind = EscapeNodeKind::UNKNOWN;

                if (instr.op == ir::IrOp::NEWOBJ ||
                    instr.op == ir::IrOp::GC_ALLOC ||
                    instr.op == ir::IrOp::ARRAY_ALLOC) {
                    kind = EscapeNodeKind::ALLOCATION;
                }

                if (kind == EscapeNodeKind::UNKNOWN) continue;

                EscapeNode node;
                node.value_id = instr.dst;
                node.kind = kind;
                graph.nodes[instr.dst] = std::move(node);
                graph.object_count++;
            }
        }

        // Second pass: track pointer flows
        for (const auto &block : ir_fn.blocks) {
            for (const auto &instr : block.instrs) {
                if (instr.op == ir::IrOp::STORE) {
                    // STORE val, ptr -> ptr's pointee now points to val
                    if (instr.operands.size() < 2) continue;
                    ir::IrValueId ptr = instr.operands[1];

                    auto it = graph.nodes.find(ptr);
                    if (it == graph.nodes.end()) continue;

                    it->second.points_to.insert(instr.operands[0]);
                }

                if (instr.op == ir::IrOp::LOAD) {
                    // LOAD ptr -> if ptr is an allocation, result flows from it
                    if (instr.operands.empty()) continue;
                    ir::IrValueId ptr = instr.operands[0];

                    auto it = graph.nodes.find(ptr);
                    if (it == graph.nodes.end()) continue;

                    // The loaded value flows from the allocation
                    EscapeNode load_node;
                    load_node.value_id = instr.dst;
                    load_node.kind = EscapeNodeKind::FIELD;
                    graph.nodes[instr.dst] = std::move(load_node);
                }

                if (instr.op == ir::IrOp::SETFIELD) {
                    // SETFIELD obj, field_off, val -> obj escapes via field store
                    if (instr.operands.size() < 2) continue;
                    ir::IrValueId obj = instr.operands[0];

                    auto it = graph.nodes.find(obj);
                    if (it != graph.nodes.end()) {
                        // On-stack objects with field stores to GC heap escape
                        it->second.points_to.insert(instr.operands[1]);
                    }
                }

                if (instr.op == ir::IrOp::GETFIELD) {
                    // GETFIELD obj, field_off -> result flows from obj
                    if (instr.operands.empty()) continue;
                    ir::IrValueId obj = instr.operands[0];

                    auto it = graph.nodes.find(obj);
                    if (it != graph.nodes.end()) {
                        EscapeNode field_node;
                        field_node.value_id = instr.dst;
                        field_node.kind = EscapeNodeKind::FIELD;
                        graph.nodes[instr.dst] = std::move(field_node);
                    }
                }

                if (instr.op == ir::IrOp::CALL ||
                    instr.op == ir::IrOp::CALLVIRT ||
                    instr.op == ir::IrOp::CALLIND) {
                    // If any operand is an allocation, it escapes via the call
                    for (ir::IrValueId op : instr.operands) {
                        auto it = graph.nodes.find(op);
                        if (it != graph.nodes.end()) {
                            it->second.escapes = true;
                        }
                    }
                }

                if (instr.op == ir::IrOp::RET) {
                    // Returned value escapes the function
                    if (!instr.operands.empty()) {
                        auto it = graph.nodes.find(instr.operands[0]);
                        if (it != graph.nodes.end()) {
                            it->second.escapes = true;
                        }
                    }
                }

                if (instr.op == ir::IrOp::MOV) {
                    // MOV propagates pointer flows
                    if (!instr.operands.empty()) {
                        ir::IrValueId src = instr.operands[0];
                        ir::IrValueId dst = instr.dst;

                        auto src_it = graph.nodes.find(src);
                        if (src_it != graph.nodes.end()) {
                            src_it->second.points_to.insert(dst);
                            graph.nodes[dst] = src_it->second;
                            graph.nodes[dst].value_id = dst;
                        }
                    }
                }

                if (instr.op == ir::IrOp::PHI) {
                    // PHI merges pointer flows
                    for (const auto &phi_arg : instr.phi_args) {
                        auto it = graph.nodes.find(phi_arg.value);
                        if (it != graph.nodes.end()) {
                            it->second.points_to.insert(instr.dst);
                        }
                    }
                }

                if (instr.op == ir::IrOp::ARRAY_STORE) {
                    // ARRAY_STORE arr, idx, val -> arr escapes via element store
                    if (instr.operands.size() < 3) continue;
                    ir::IrValueId arr = instr.operands[0];

                    auto it = graph.nodes.find(arr);
                    if (it != graph.nodes.end()) {
                        it->second.points_to.insert(instr.operands[2]);
                    }
                }
            }
        }

        return graph;
    }

    // =====================================================================
    //  Propagate escape state transitively through the graph
    // =====================================================================

    static void propagate_escape(EscapeGraph &graph) {
        // Worklist: start with all nodes marked as escaping
        std::vector<EscapeNode *> worklist;

        for (auto &[id, node] : graph.nodes) {
            if (node.escapes) {
                worklist.push_back(&node);
            }
        }

        // Propagate: if A escapes, anything that points to A also escapes
        // (because if A reaches an escape point, its referrers also reach it)
        while (!worklist.empty()) {
            EscapeNode *node = worklist.back();
            worklist.pop_back();

            if (node->visited) continue;
            node->visited = true;

            // Transitively: if this node escapes, all nodes it points to
            // also become reachable from the outside
            for (ir::IrValueId pointed : node->points_to) {
                auto it = graph.nodes.find(pointed);
                if (it != graph.nodes.end() && !it->second.escapes) {
                    it->second.escapes = true;
                    it->second.visited = false;
                    worklist.push_back(&it->second);
                }
            }

            // Also, if this node escapes, anything that flows INTO it
            // (inverse direction) also effectively escapes
            for (ir::IrValueId ptr : node->pointed_by) {
                auto it = graph.nodes.find(ptr);
                if (it != graph.nodes.end() && !it->second.escapes) {
                    it->second.escapes = true;
                    it->second.visited = false;
                    worklist.push_back(&it->second);
                }
            }
        }
    }

    // =====================================================================
    //  Mark non-escaping allocations as stack-allocatable
    // =====================================================================

    static void mark_stack_allocatable(
        ir::IrFunction &ir_fn,
        const EscapeGraph &graph,
        std::vector<ir::IrValueId> &non_escaping) {
        for (const auto &[id, node] : graph.nodes) {
            if (node.kind != EscapeNodeKind::ALLOCATION) continue;
            if (node.escapes) continue;

            non_escaping.push_back(id);
        }
    }

    // =====================================================================
    //  Transform IR: replace NEWOBJ with ALLOCA for non-escaping objects
    // =====================================================================

    static void transform_to_stack_allocation(
        ir::IrFunction &ir_fn,
        const std::vector<ir::IrValueId> &non_escaping) {
        std::unordered_set<ir::IrValueId> target_set(
            non_escaping.begin(), non_escaping.end());

        for (auto &block : ir_fn.blocks) {
            for (auto &instr : block.instrs) {
                if (target_set.find(instr.dst) == target_set.end()) continue;
                if (instr.op == ir::IrOp::NEWOBJ) {
                    // Replace NEWOBJ with ALLOCA (stack allocation)
                    // ALLOCA uses type I64 and the count is a fixed size
                    instr.op = ir::IrOp::ALLOCA;
                    instr.type = ir::IrType::I64;
                    instr.imm = 64;

                    // Mark as GC-managed stack allocation
                    if (instr.dst < ir_fn.values.size()) {
                        ir_fn.values[instr.dst].is_host_ptr = true;
                        ir_fn.values[instr.dst].is_gc_object = false;
                    }
                }
                if (instr.op == ir::IrOp::GC_ALLOC ||
                    instr.op == ir::IrOp::ARRAY_ALLOC) {
                    // For GC_ALLOC/ARRAY_ALLOC, replace with ALLOCA
                    instr.op = ir::IrOp::ALLOCA;
                    instr.type = ir::IrType::I64;
                    instr.imm = 64;

                    if (instr.dst < ir_fn.values.size()) {
                        ir_fn.values[instr.dst].is_host_ptr = true;
                        ir_fn.values[instr.dst].is_gc_object = false;
                    }
                }
            }
        }
    }

    // =====================================================================
    //  Main entry point: analyze and transform
    // =====================================================================

    C2EscapeResult analyze_and_transform(ir::IrFunction &ir_fn) {
        C2EscapeResult result;

        // Early exit: function too large for analysis
        if (ir_fn.values.size() > c2::ESCAPE_ANALYSIS_MAX_OBJECTS) {
            return result;
        }

        // Build escape graph from IR
        EscapeGraph graph = build_escape_graph(ir_fn);

        if (graph.nodes.empty()) {
            return result;
        }

        // Propagate escape information transitively
        propagate_escape(graph);

        // Mark non-escaping allocations
        mark_stack_allocatable(ir_fn, graph, result.non_escaping_allocations);

        // Transform eligible allocations to stack allocation
        if (!result.non_escaping_allocations.empty()) {
            transform_to_stack_allocation(ir_fn, result.non_escaping_allocations);
        }

        return result;
    }

} // namespace c2_escape
} // namespace jit
