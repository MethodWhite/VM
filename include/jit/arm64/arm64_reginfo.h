/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

#ifndef VESTA_JIT_ARM64_REGINFO_H
#define VESTA_JIT_ARM64_REGINFO_H

#include "jit/machine_ir.h"
#include "jit/arm64/arm64_abi.h"

#include <cstdint>
#include <vector>

namespace jit {
namespace arm64 {

    struct Arm64TargetRegInfo {
        uint8_t pointer_size = 8;
        bool is_two_address = false;

        static constexpr size_t NCLASS = 2;

        std::vector<uint8_t> allocatable[NCLASS];
        std::vector<uint8_t> caller_saved[NCLASS];
        std::vector<uint8_t> callee_saved[NCLASS];
        std::vector<uint8_t> scratch[NCLASS];
        std::vector<uint8_t> arg_regs[NCLASS];
        uint8_t ret_reg[NCLASS] = {0, 0};
        std::vector<uint8_t> reserved;

        bool is_allocatable(uint8_t cls, uint8_t r) const noexcept {
            if (cls >= NCLASS) return false;
            for (uint8_t x : allocatable[cls]) if (x == r) return true;
            return false;
        }

        bool is_callee_saved(uint8_t cls, uint8_t r) const noexcept {
            if (cls >= NCLASS) return false;
            for (uint8_t x : callee_saved[cls]) if (x == r) return true;
            return false;
        }

        size_t num_allocatable(uint8_t cls) const noexcept {
            if (cls >= NCLASS) return 0;
            return allocatable[cls].size();
        }
    };

    inline const Arm64TargetRegInfo &target_arm64_vm_abi() {
        static const Arm64TargetRegInfo info = []() {
            Arm64TargetRegInfo t;
            t.pointer_size  = 8;
            t.is_two_address = false;

            const size_t GP = 0;
            const size_t FP = 1;

            for (uint8_t i = 0; i <= 15; ++i) {
                t.caller_saved[GP].push_back(i);
            }
            for (uint8_t i = 19; i <= 28; ++i) {
                t.callee_saved[GP].push_back(i);
            }
            t.scratch[GP] = {16, 17};
            t.allocatable[GP] = t.caller_saved[GP];
            t.allocatable[GP].insert(t.allocatable[GP].end(),
                                      t.callee_saved[GP].begin(),
                                      t.callee_saved[GP].end());
            t.ret_reg[GP] = 0;

            t.arg_regs[GP] = {0, 1, 2, 3, 4, 5, 6, 7};

            for (uint8_t i = 0; i <= 7; ++i) {
                t.caller_saved[FP].push_back(i);
            }
            for (uint8_t i = 8; i <= 15; ++i) {
                t.callee_saved[FP].push_back(i);
            }
            t.scratch[FP] = {16, 17};
            t.allocatable[FP] = t.caller_saved[FP];
            t.allocatable[FP].insert(t.allocatable[FP].end(),
                                      t.callee_saved[FP].begin(),
                                      t.callee_saved[FP].end());
            t.ret_reg[FP] = 0;
            t.arg_regs[FP] = {0, 1, 2, 3, 4, 5, 6, 7};

            t.reserved = {
                18, 29, 30, 31
            };

            return t;
        }();
        return info;
    }

} // namespace arm64
} // namespace jit

#endif // VESTA_JIT_ARM64_REGINFO_H
