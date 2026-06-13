/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

#ifndef VESTA_JIT_ARM64_ABI_H
#define VESTA_JIT_ARM64_ABI_H

#include <cstdint>

namespace jit {
namespace arm64_abi {

    static constexpr uint32_t STACK_ALIGNMENT = 16;
    static constexpr uint32_t RED_ZONE_SIZE   = 16;

    static constexpr uint32_t FP_REG = 29;
    static constexpr uint32_t LR_REG = 30;
    static constexpr uint32_t SP_REG = 31;

    static constexpr uint32_t NUM_ARG_REGS = 8;
    static constexpr uint32_t ARG_REGS[NUM_ARG_REGS] = {0, 1, 2, 3, 4, 5, 6, 7};

    static constexpr uint32_t NUM_FLOAT_ARG_REGS = 8;
    static constexpr uint32_t FLOAT_ARG_REGS[NUM_FLOAT_ARG_REGS] = {0, 1, 2, 3, 4, 5, 6, 7};

    static constexpr uint32_t CALLER_SAVED_GP[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    static constexpr uint32_t NUM_CALLER_SAVED_GP = 16;

    static constexpr uint32_t CALLEE_SAVED_GP[] = {19, 20, 21, 22, 23, 24, 25, 26, 27, 28};
    static constexpr uint32_t NUM_CALLEE_SAVED_GP = 10;

    static constexpr uint32_t CALLEE_SAVED_FP[] = {8, 9, 10, 11, 12, 13, 14, 15};
    static constexpr uint32_t NUM_CALLEE_SAVED_FP = 8;

    static constexpr uint32_t RETURN_REG = 0;
    static constexpr uint32_t FLOAT_RETURN_REG = 0;

    static constexpr uint32_t FRAME_PTR_REG = 29;
    static constexpr uint32_t LINK_REG      = 30;

    static constexpr uint32_t MAX_CALLED_ARGS_IN_REGS = 8;

} // namespace arm64_abi
} // namespace jit

#endif // VESTA_JIT_ARM64_ABI_H
