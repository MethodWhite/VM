/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

#ifndef VESTA_JIT_C2_HEURISTICS_H
#define VESTA_JIT_C2_HEURISTICS_H

#include <cstdint>
#include <cstddef>

namespace jit {
namespace c2 {

    // =====================================================================
    //  Optimization thresholds and budgets
    // =====================================================================

    /// Max IR instructions for a function to be considered "hot" for inlining.
    /// Functions smaller than this are inlined at call sites.
    static constexpr size_t INLINE_HOT_THRESHOLD = 20;

    /// Max IR instructions for a "cold" function to be inlined.
    static constexpr size_t INLINE_COLD_THRESHOLD = 10;

    /// Maximum depth of recursive inlining (guard against infinite recursion).
    static constexpr uint32_t INLINE_MAX_DEPTH = 3;

    /// Maximum loop unroll factor for small loops.
    static constexpr uint32_t LOOP_UNROLL_FACTOR = 4;

    /// Maximum number of objects analyzed per function in escape analysis.
    static constexpr size_t ESCAPE_ANALYSIS_MAX_OBJECTS = 50;

    /// Maximum compilation budget per function (10ms).
    static constexpr uint64_t COMPILE_BUDGET_US = 10000;

    /// Devirtualization: only when PGO shows monomorphic (1 type, >1000 calls).
    static constexpr uint64_t DEVIRT_MIN_CALLS = 1000;

    /// Maximum number of types for polymorphic inline cache (PIC).
    static constexpr uint32_t PIC_MAX_TYPES = 4;

    /// Threshold for loop detection: min back-edge distance.
    static constexpr size_t LOOP_MIN_BACKEDGE_DIST = 2;

    /// Maximum number of speculative optimizations per function.
    static constexpr uint32_t MAX_SPECULATIVE_OPS = 8;

    /// Stack allocation max object size (bytes).
    static constexpr size_t STACK_ALLOC_MAX_SIZE = 1024;

    /// PGO branch taken threshold for branch prediction hints (0.0 - 1.0).
    static constexpr double BRANCH_TAKEN_THRESHOLD = 0.9;

    /// Maximum side-effect-free instructions to hoist per loop in LICM.
    static constexpr size_t LICM_MAX_HOIST = 32;

} // namespace c2
} // namespace jit

#endif // VESTA_JIT_C2_HEURISTICS_H
