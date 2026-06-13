/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

#ifndef VESTA_JIT_ARM64_SELECTOR_H
#define VESTA_JIT_ARM64_SELECTOR_H

#include "jit/machine_ir.h"
#include "jit/runtime_entries.h"
#include "ir/ssa_ir.h"
#include "jit/arm64/arm64_abi.h"

#include <functional>
#include <string>
#include <vector>
#include <cstdint>

namespace jit {
namespace arm64 {

    enum class Arm64SelectorMode {
        NATIVE_ABI,
        VM_ABI
    };

    struct Arm64SelectorOptions {
        Arm64SelectorMode mode = Arm64SelectorMode::VM_ABI;
        uint64_t safepoint_handler_addr = 0;
        const RuntimeEntries *runtime = nullptr;
        std::function<uint64_t(const std::string &)> resolve_user_fn{};
        std::function<uint64_t(const std::string &)> resolve_symbol{};
        std::function<uint64_t(const std::string &)> resolve_native_fn{};
        uint64_t jit_instr_counter_addr = 0;
    };

    struct Arm64StackmapSlot {
        int16_t rbp_offset = 0;
        uint8_t gc_kind = 0;
    };

    struct Arm64Stackmap {
        uint32_t pc_offset = 0;
        std::vector<Arm64StackmapSlot> slots;
    };

    struct Arm64MFixup {
        uint32_t label_id;
        uint32_t patch_at;
        uint32_t instr_end;
        uint8_t width;
    };

    struct Arm64MBlock {
        uint32_t label_id;
        std::vector<uint32_t> instrs;
        uint32_t byte_offset = 0;
    };

    struct Arm64MFunction {
        std::string name;
        std::vector<Arm64MBlock> blocks;
        std::vector<uint64_t> imm64_pool;
        std::vector<Arm64MFixup> fixups;
        std::vector<uint32_t> label_offsets;
        uint32_t stack_frame_size = 0;
        uint32_t next_label_id = 0;
        std::vector<Arm64Stackmap> stackmaps;
        uint32_t next_label() {
            uint32_t id = next_label_id++;
            if (label_offsets.size() <= id) {
                label_offsets.resize(id + 1, UINT32_MAX);
            }
            return id;
        }
        uint32_t new_block(uint32_t lbl) {
            Arm64MBlock b;
            b.label_id = lbl;
            blocks.push_back(b);
            return static_cast<uint32_t>(blocks.size() - 1);
        }
        uint32_t intern_imm64(uint64_t value) {
            for (uint32_t i = 0; i < imm64_pool.size(); ++i) {
                if (imm64_pool[i] == value) return i;
            }
            imm64_pool.push_back(value);
            return static_cast<uint32_t>(imm64_pool.size() - 1);
        }
    };

    class Arm64Selector {
    public:
        explicit Arm64Selector(Arm64SelectorOptions opts = {}) : opts_(opts) {}

        Arm64MFunction select(const ir::IrFunction &ir_fn, bool *out_unsupported = nullptr);

    private:
        Arm64SelectorOptions opts_;
    };

    Arm64MFunction arm64_select(const ir::IrFunction &ir_fn,
                                 const RuntimeEntries *rt,
                                 bool *out_unsupported = nullptr);

} // namespace arm64
} // namespace jit

#endif // VESTA_JIT_ARM64_SELECTOR_H
