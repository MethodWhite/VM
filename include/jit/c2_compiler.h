/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

#ifndef VESTA_JIT_C2_COMPILER_H
#define VESTA_JIT_C2_COMPILER_H

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ir/ssa_ir.h"
#include "jit/c2_heuristics.h"
#include "jit/code_cache.h"
#include "jit/jit_compiler.h"
#include "jit/jit_registry.h"
#include "jit/machine_ir.h"
#include "jit/runtime_entries.h"

namespace runtime { namespace profile { struct ProfileCollector; } }
namespace loader { struct ClassInfo; struct MethodInfo; }

namespace jit {

    // =====================================================================
    //  Forward declarations
    // =====================================================================

    struct C2CompileContext;
    struct C2InlinePlan;
    struct C2EscapeResult;
    struct C2LoopInfo;
    struct C2DevirtSite;
    struct C2DeoptMetadata;

    // =====================================================================
    //  C2CompileResult - extended compilation result
    // =====================================================================

    struct C2CompileResult {
        JitFn       fn          = nullptr;
        size_t      code_size   = 0;
        size_t      instr_count = 0;
        bool        succeeded   = false;

        /// Bitmask of optimizations applied.
        uint32_t    opt_flags   = 0;

        /// Number of inlined call sites.
        uint32_t    inlined_count     = 0;
        /// Number of objects promoted to stack allocation.
        uint32_t    stack_alloc_count = 0;
        /// Number of hoisted invariants.
        uint32_t    licm_hoisted      = 0;
        /// Number of devirtualized call sites.
        uint32_t    devirt_count      = 0;
        /// Number of eliminated bounds checks.
        uint32_t    bce_eliminated    = 0;
        /// Number of speculative optimizations applied.
        uint32_t    speculative_count = 0;

        /// Deoptimization metadata for this compilation.
        std::vector<C2DeoptMetadata> deopt_metadata;

        const uint8_t *code_start = nullptr;
    };

    constexpr uint32_t C2_OPT_INLINE     = 1u << 0;
    constexpr uint32_t C2_OPT_ESCAPE     = 1u << 1;
    constexpr uint32_t C2_OPT_LICM       = 1u << 2;
    constexpr uint32_t C2_OPT_DEVIRT     = 1u << 3;
    constexpr uint32_t C2_OPT_BCE        = 1u << 4;
    constexpr uint32_t C2_OPT_SPECULATIVE = 1u << 5;

    // =====================================================================
    //  C2DeoptMetadata - records for deoptimization
    // =====================================================================

    enum class DeoptReason : uint8_t {
        CLASS_CHECK     = 0,
        BOUNDS_CHECK    = 1,
        NULL_CHECK      = 2,
        TYPE_CHECK      = 3,
        ARRAY_STORE     = 4,
        COUNT           = 5
    };

    struct C2DeoptMetadata {
        /// PC offset in the emitted code for this deopt point.
        uint32_t            pc_offset     = 0;
        /// Reason for speculation.
        DeoptReason         reason        = DeoptReason::CLASS_CHECK;
        /// The IR value IDs that are live at the deopt point.
        std::vector<ir::IrValueId> live_values;
        /// Block ID in the IR that corresponds to the fallback (bailout).
        ir::IrBlockId       fallback_block = ir::IR_NO_BLOCK;
        /// Human-readable description of the speculation.
        std::string         description;
        /// Offset in the bytecode (for reconstructing interpreter state).
        uint32_t            bytecode_pc   = 0;
        /// Stack frame size at the deopt point.
        uint32_t            frame_size    = 0;
    };

    // =====================================================================
    //  C2Compiler - the optimizing JIT compiler
    // =====================================================================

    class C2Compiler {
    public:
        C2Compiler(CodeCache &cache, const RuntimeEntries &rt) noexcept
            : cache_(cache), rt_(rt) {}

        C2CompileResult compile(const ir::IrFunction &ir_fn) noexcept;

        /// Invalidate a previously compiled C2 function.
        void invalidate(const C2CompileResult &res) noexcept;

        /// Read PGO profile data to guide optimization decisions.
        void set_profile_collector(
            const runtime::profile::ProfileCollector *profiler) noexcept {
            profiler_ = profiler;
        }

        /// Set the C1 tier-up handler for recompilation.
        void set_tier_up_handler(std::function<uint64_t(uint64_t)> handler) noexcept {
            tier_up_handler_ = std::move(handler);
        }

        /// Check if a function should tier up from C1 to C2.
        static bool should_tier_up(uint32_t invocation_count,
                                   uint32_t threshold,
                                   uint32_t pgo_hotness) noexcept;

    private:
        // =================================================================
        //  Optimization pipeline steps
        // =================================================================

        /// Phase 1: Inlining - inline small functions at call sites.
        C2InlinePlan analyze_inlining(const ir::IrFunction &ir_fn) const;

        /// Phase 2: Escape Analysis - find non-escaping allocations.
        C2EscapeResult analyze_escape(ir::IrFunction &ir_fn) const;

        /// Phase 3: LICM - hoist loop invariants.
        std::vector<C2LoopInfo> find_loops(const ir::IrFunction &ir_fn) const;
        void apply_licm(ir::IrFunction &ir_fn,
                        const std::vector<C2LoopInfo> &loops) const;

        /// Phase 4: Devirtualization - use PGO to devirtualize CALLVIRT.
        std::vector<C2DevirtSite> find_devirt_sites(
            const ir::IrFunction &ir_fn) const;
        void apply_devirt(ir::IrFunction &ir_fn,
                          const std::vector<C2DevirtSite> &sites) const;

        /// Phase 5: Bounds Check Elimination.
        void apply_bce(ir::IrFunction &ir_fn) const;

        /// Phase 6: Speculative optimizations with deopt guards.
        void apply_speculative_ops(
            ir::IrFunction &ir_fn,
            std::vector<C2DeoptMetadata> &deopt_meta) const;

        /// Emit stackmaps for GC safepoints in the optimized code.
        std::vector<Stackmap> emit_stackmaps(
            const ir::IrFunction &ir_fn,
            const C2CompileContext &ctx) const;

        /// Encode the optimized IR to native code via the C1 pipeline.
        CompileResult lower_to_native(ir::IrFunction &opt_ir) const;

        /// Record deoptimization metadata for a guarded optimization.
        void record_deopt_metadata(
            C2CompileResult &result,
            DeoptReason reason,
            uint32_t bytecode_pc,
            const std::vector<ir::IrValueId> &live_values,
            ir::IrBlockId fallback_block,
            const std::string &desc) const;

        CodeCache            &cache_;
        const RuntimeEntries &rt_;
        const runtime::profile::ProfileCollector *profiler_ = nullptr;
        std::function<uint64_t(uint64_t)> tier_up_handler_;
    };

    // =====================================================================
    //  C2InlinePlan - result of inlining analysis
    // =====================================================================

    struct C2InlineSite {
        ir::IrValueId  call_dst;       ///< SSA value of the CALL result.
        uint32_t       callee_size;    ///< Number of IR instructions in callee.
        uint32_t       call_frequency; ///< From PGO if available.
        uint32_t       depth;          ///< Current inlining depth.
        std::string    callee_name;    ///< Function name to inline.
    };

    struct C2InlinePlan {
        std::vector<C2InlineSite> sites_to_inline;
    };

    // =====================================================================
    //  C2EscapeResult - result of escape analysis
    // =====================================================================

    struct C2EscapeResult {
        /// List of SSA value IDs for allocations that do NOT escape.
        std::vector<ir::IrValueId> non_escaping_allocations;

        /// True if the analysis found at least one candidate.
        bool has_candidates() const noexcept {
            return !non_escaping_allocations.empty();
        }
    };

    // =====================================================================
    //  C2LoopInfo - loop descriptor for LICM
    // =====================================================================

    struct C2LoopInfo {
        ir::IrBlockId header;     ///< Loop header block.
        ir::IrBlockId preheader;  ///< Pre-header block (where to hoist).
        std::unordered_set<ir::IrBlockId> body_blocks; ///< Blocks inside loop.
        ir::IrBlockId backedge_src; ///< Block with back-edge to header.
        size_t depth;             ///< Nesting depth (0 = outer).
    };

    // =====================================================================
    //  C2DevirtSite - devirtualization candidate
    // =====================================================================

    struct C2DevirtSite {
        ir::IrValueId  callvirt_dst;     ///< SSA dst of the CALLVIRT.
        uint64_t       observed_class_ptr; ///< ClassInfo* from PGO.
        std::string    callee_ir_name;    ///< Resolved direct callee.
        uint64_t       call_count;        ///< From PGO call site counter.
    };

    // =====================================================================
    //  Profile reading interface for PGO integration
    // =====================================================================

    struct PGOData {
        uint64_t branch_taken(const uint64_t *pc) const;
        uint64_t branch_not_taken(const uint64_t *pc) const;
        uint64_t callvirt_count(const uint64_t *pc) const;
        uint32_t callvirt_type_count(const uint64_t *pc) const;
        uint64_t alloc_count(const uint64_t *pc) const;
    };

} // namespace jit

#endif // VESTA_JIT_C2_COMPILER_H
