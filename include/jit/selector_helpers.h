/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/selector_helpers.h
 * @brief Declaraciones de helpers del instruction selector (extracted from
 *        selector.cpp into selector_helpers.cpp for modularity).
 */

#ifndef VESTA_JIT_SELECTOR_HELPERS_H
#define VESTA_JIT_SELECTOR_HELPERS_H

#include "jit/machine_ir.h"
#include "ir/ssa_ir.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jit {

    struct RuntimeEntries;

    /* ----- Tipos auxiliares ----- */

    /// Magic-number para division signed por constante de 32 bits
    /// (Hacker's Delight fig. 10-1).
    struct DivMagicS32 {
        int32_t M; ///< Multiplicador magico
        int     s; ///< Shift
    };

    /* ----- Helpers para el mini-parser de raw_asm ----- */

    std::string sanitize_label_name(const std::string &s);
    int         vm_reg_slot_index(const std::string &name) noexcept;
    int32_t     vm_reg_offset(int slot) noexcept;
    std::string trim_str(const std::string &s);
    std::vector<std::string> split_csv(const std::string &s);
    bool        parse_imm_int(const std::string &s, int64_t &out);
    std::string parse_mem_operand(const std::string &s);

    /* ----- Regs scratch y convencion ----- */

    constexpr MReg SCRATCH_A    = MReg::RAX;
    constexpr MReg SCRATCH_B    = MReg::RCX;
    constexpr MReg SCRATCH_C    = MReg::RDX;
    constexpr MReg JIT_PROC_REG = MReg::RBX;
#if defined(_WIN32)
    constexpr MReg NATIVE_ARG0  = MReg::RCX;
    constexpr MReg NATIVE_ARG1  = MReg::RDX;
    constexpr MReg NATIVE_ARG2  = MReg::R8;
#else
    constexpr MReg NATIVE_ARG0  = MReg::RDI;
    constexpr MReg NATIVE_ARG1  = MReg::RSI;
    constexpr MReg NATIVE_ARG2  = MReg::RDX;
#endif

    /* ----- Slot/load/store helpers ----- */

    int32_t  slot_offset(ir::IrValueId vid) noexcept;
    MOperand slot_mem(ir::IrValueId vid) noexcept;
    void     load_op(MFunction &mf, ir::IrValueId vid, MReg dst);
    void     load_op_rematerializable(MFunction &mf, const ir::IrFunction &fn,
                                      ir::IrValueId vid, MReg dst);
    void     store_op(MFunction &mf, ir::IrValueId vid, MReg src);

    /* ----- Runtime entry resolution ----- */

    uint64_t resolve_runtime_entry(const std::string &name,
                                    const RuntimeEntries *rt);

    /* ----- Type helpers ----- */

    uint64_t ir_type_size_bytes(ir::IrType t) noexcept;
    bool     ir_type_is_signed_int(ir::IrType t) noexcept;

    /* ----- Magic number division ----- */

    DivMagicS32 compute_magic_s32(int32_t d) noexcept;

    /* ----- Callback ABI helpers ----- */

    bool cb_is_leaf_safe_op(ir::IrOp op) noexcept;
    MCond cond_for_cmp_op(ir::IrOp op);

} // namespace jit

#endif // VESTA_JIT_SELECTOR_HELPERS_H
