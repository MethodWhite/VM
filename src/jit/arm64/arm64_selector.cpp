/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

#include "jit/arm64/arm64_selector.h"
#include "jit/arm64/arm64_encoder.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace jit {
namespace arm64 {

    namespace {

        static constexpr uint32_t INVALID_LABEL = UINT32_MAX;

    static constexpr uint32_t PROC_REG = 19;
    static constexpr uint32_t SCRATCH_A = 0;
    static constexpr uint32_t SCRATCH_B = 1;
    static constexpr uint32_t SCRATCH_C = 2;

    static constexpr uint32_t FP_SCRATCH_A = 0;
    static constexpr uint32_t FP_SCRATCH_B = 1;
    static constexpr uint32_t FP_SCRATCH_C = 2;

    static constexpr uint32_t CALLEE_SAVED_GP_COUNT = 10;   // x19-x28
    static constexpr uint32_t CALLEE_SAVED_FP_COUNT = 8;    // d8-d15
    static constexpr uint32_t CALLEE_SAVED_SIZE =           // x29,x30 + x19-x28 + d8-d15
        (2 + CALLEE_SAVED_GP_COUNT + CALLEE_SAVED_FP_COUNT) * 8;

        static inline int32_t slot_offset(ir::IrValueId vid) noexcept {
            return -8 * (static_cast<int32_t>(vid) + 1);
        }

        static void emit_instr(std::vector<Arm64MBlock> &blocks, uint32_t instr) {
            if (!blocks.empty()) {
                blocks.back().instrs.push_back(instr);
            }
        }

        static void emit_fixup(std::vector<Arm64MFixup> &fixups,
                                uint32_t label_id, uint32_t patch_at,
                                uint32_t instr_end) {
            fixups.push_back({label_id, patch_at, instr_end, 4});
        }

        static uint32_t cond_for_cmp_op(ir::IrOp op) {
            switch (op) {
                case ir::IrOp::CMP_EQ:  return COND_EQ;
                case ir::IrOp::CMP_NE:  return COND_NE;
                case ir::IrOp::CMP_LT:  return COND_LT;
                case ir::IrOp::CMP_GT:  return COND_GT;
                case ir::IrOp::CMP_LE:  return COND_LE;
                case ir::IrOp::CMP_GE:  return COND_GE;
                case ir::IrOp::CMP_ULT: return COND_CC;
                case ir::IrOp::CMP_UGT: return COND_HI;
                case ir::IrOp::CMP_ULE: return COND_LS;
                case ir::IrOp::CMP_UGE: return COND_CS;
                default:                return COND_AL;
            }
        }

        static uint64_t resolve_runtime_entry(const std::string &name,
                                               const RuntimeEntries *rt) {
            if (!rt) return 0;
            #define MATCH(n, field) \
                if (name == #n) return reinterpret_cast<uint64_t>(rt->field)
            MATCH(vrt_gc_alloc,         gc_alloc);
            MATCH(vrt_gc_alloc_pinned,  gc_alloc_pinned);
            MATCH(vrt_gc_deref,         gc_deref);
            MATCH(vrt_gc_handle_for_ptr,gc_handle_for_ptr);
            MATCH(vrt_gc_drop,          gc_drop);
            MATCH(vrt_gc_addref,        gc_addref);
            MATCH(vrt_gc_release,       gc_release);
            MATCH(vrt_gc_write_barrier, gc_write_barrier);
            MATCH(vrt_monitor_enter,    monitor_enter);
            MATCH(vrt_monitor_exit,     monitor_exit);
            MATCH(vrt_monitor_wait,     monitor_wait);
            MATCH(vrt_monitor_notify,   monitor_notify);
            MATCH(vrt_monitor_notify_all, monitor_notify_all);
            MATCH(vrt_throw_fatal,      throw_fatal);
            MATCH(vrt_tryenter,         tryenter);
            MATCH(vrt_tryleave,         tryleave);
            MATCH(vrt_invoke_native,    invoke_native);
            MATCH(vrt_safepoint_poll,   safepoint_poll);
            MATCH(vrt_safepoint_handler,safepoint_handler);
            #undef MATCH
            return 0;
        }

        static uint64_t ir_type_size_bytes(ir::IrType t) noexcept {
            switch (t) {
                case ir::IrType::I8:  case ir::IrType::U8:  case ir::IrType::BOOL: return 1;
                case ir::IrType::I16: case ir::IrType::U16: return 2;
                case ir::IrType::I32: case ir::IrType::U32: case ir::IrType::F32: return 4;
                default: return 8;
            }
        }

        static bool ir_type_is_signed_int(ir::IrType t) noexcept {
            return t == ir::IrType::I8  || t == ir::IrType::I16
                || t == ir::IrType::I32 || t == ir::IrType::I64;
        }

        static bool ir_type_is_float(ir::IrType t) noexcept {
            return t == ir::IrType::F32 || t == ir::IrType::F64;
        }

        static void emit_load(Arm64MFunction &mf, ir::IrValueId vid, uint32_t reg) {
            emit_instr(mf.blocks, encode_ldr_64((Arm64Reg)reg, slot_offset(vid)));
        }

        static void emit_store(Arm64MFunction &mf, ir::IrValueId vid, uint32_t reg) {
            if (vid == ir::IR_NO_VALUE) return;
            emit_instr(mf.blocks, encode_str_64((Arm64Reg)reg, slot_offset(vid)));
        }

        static void emit_load_fp(Arm64MFunction &mf, ir::IrValueId vid, uint32_t dreg) {
            emit_instr(mf.blocks, encode_ldr_64((Arm64Reg)dreg, slot_offset(vid)));
        }

        static void emit_store_fp(Arm64MFunction &mf, ir::IrValueId vid, uint32_t dreg) {
            if (vid == ir::IR_NO_VALUE) return;
            emit_instr(mf.blocks, encode_str_64((Arm64Reg)dreg, slot_offset(vid)));
        }

        static uint32_t cond_for_fcmp_op(ir::IrOp op) {
            switch (op) {
                case ir::IrOp::FCMP_EQ:  return COND_EQ;
                case ir::IrOp::FCMP_NE:  return COND_NE;
                case ir::IrOp::FCMP_LT:  return COND_LT;
                case ir::IrOp::FCMP_GT:  return COND_GT;
                case ir::IrOp::FCMP_LE:  return COND_LE;
                case ir::IrOp::FCMP_GE:  return COND_GE;
                default:                 return COND_AL;
            }
        }

    } // anonymous namespace

    Arm64MFunction Arm64Selector::select(const ir::IrFunction &ir_fn,
                                          bool *out_unsupported) {
        Arm64MFunction mf;
        mf.name = ir_fn.name;
        bool unsupported = false;

        std::vector<uint32_t> block_labels(ir_fn.blocks.size(), INVALID_LABEL);
        for (size_t i = 0; i < ir_fn.blocks.size(); ++i) {
            block_labels[i] = mf.next_label();
        }

        uint32_t prologue_lbl = mf.next_label();
        uint32_t prologue = mf.new_block(prologue_lbl);

        uint32_t local_frame = static_cast<uint32_t>(ir_fn.values.size() * 8);
        local_frame = (local_frame + 15) & ~15u;
        uint32_t total_frame = local_frame + CALLEE_SAVED_SIZE;
        total_frame = (total_frame + 15) & ~15u;
        mf.stack_frame_size = local_frame;

        emit_instr(mf.blocks, encode_stp_pre((Arm64Reg)29, (Arm64Reg)30,
                                               (Arm64Reg)31, -(int32_t)total_frame, true));
        emit_instr(mf.blocks, encode_mov_reg((Arm64Reg)29, (Arm64Reg)31, true));

        {
            const int32_t gp_base = (int32_t)total_frame - 16;
            emit_instr(mf.blocks, encode_stp((Arm64Reg)27, (Arm64Reg)28,
                                              (Arm64Reg)29, gp_base - 16, true));
            emit_instr(mf.blocks, encode_stp((Arm64Reg)25, (Arm64Reg)26,
                                              (Arm64Reg)29, gp_base - 32, true));
            emit_instr(mf.blocks, encode_stp((Arm64Reg)23, (Arm64Reg)24,
                                              (Arm64Reg)29, gp_base - 48, true));
            emit_instr(mf.blocks, encode_stp((Arm64Reg)21, (Arm64Reg)22,
                                              (Arm64Reg)29, gp_base - 64, true));
            emit_instr(mf.blocks, encode_stp((Arm64Reg)19, (Arm64Reg)20,
                                              (Arm64Reg)29, gp_base - 80, true));
        }

        {
            const int32_t fp_base = (int32_t)total_frame - 16 - 80;
            emit_instr(mf.blocks, encode_simd_stp((Arm64Reg)14, (Arm64Reg)15,
                                                   (Arm64Reg)29, fp_base - 16, true));
            emit_instr(mf.blocks, encode_simd_stp((Arm64Reg)12, (Arm64Reg)13,
                                                   (Arm64Reg)29, fp_base - 32, true));
            emit_instr(mf.blocks, encode_simd_stp((Arm64Reg)10, (Arm64Reg)11,
                                                   (Arm64Reg)29, fp_base - 48, true));
            emit_instr(mf.blocks, encode_simd_stp((Arm64Reg)8, (Arm64Reg)9,
                                                   (Arm64Reg)29, fp_base - 64, true));
        }

        uint32_t safepoint_pool_idx = UINT32_MAX;
        if (opts_.mode == Arm64SelectorMode::VM_ABI
         && opts_.safepoint_handler_addr != 0) {
            safepoint_pool_idx = mf.intern_imm64(opts_.safepoint_handler_addr);
        }

        for (size_t bi = 0; bi < ir_fn.blocks.size(); ++bi) {
            const auto &ir_block = ir_fn.blocks[bi];
            uint32_t mblock_id = mf.new_block(block_labels[bi]);

            for (const auto &ins : ir_block.instrs) {
                switch (ins.op) {
                    case ir::IrOp::CONST: {
                        if (ins.type == ir::IrType::F32 || ins.type == ir::IrType::F64) {
                            uint64_t val = ins.imm;
                            bool is_double = (ins.type == ir::IrType::F64);
                            if (val == 0) {
                                emit_instr(mf.blocks,
                                    encode_eor((Arm64Reg)SCRATCH_A,
                                                (Arm64Reg)SCRATCH_A,
                                                (Arm64Reg)SCRATCH_A, true));
                                emit_instr(mf.blocks,
                                    encode_fmov_reg((Arm64Reg)FP_SCRATCH_A,
                                                    (Arm64Reg)SCRATCH_A, true));
                            } else {
                                uint32_t low  = (uint16_t)(val & 0xFFFF);
                                uint32_t imm2 = (uint16_t)((val >> 16) & 0xFFFF);
                                uint32_t imm3 = (uint16_t)((val >> 32) & 0xFFFF);
                                uint32_t imm4 = (uint16_t)((val >> 48) & 0xFFFF);
                                emit_instr(mf.blocks, encode_movz((Arm64Reg)SCRATCH_A,
                                                                  low, 0, true));
                                if (imm2) {
                                    emit_instr(mf.blocks, encode_movk((Arm64Reg)SCRATCH_A,
                                                                      imm2, 16, true));
                                }
                                if (imm3) {
                                    emit_instr(mf.blocks, encode_movk((Arm64Reg)SCRATCH_A,
                                                                      imm3, 32, true));
                                }
                                if (imm4) {
                                    emit_instr(mf.blocks, encode_movk((Arm64Reg)SCRATCH_A,
                                                                      imm4, 48, true));
                                }
                                emit_instr(mf.blocks,
                                    encode_fmov_reg((Arm64Reg)FP_SCRATCH_A,
                                                    (Arm64Reg)SCRATCH_A, is_double));
                            }
                            emit_store_fp(mf, ins.dst, FP_SCRATCH_A);
                        } else {
                            const int64_t cv = static_cast<int64_t>(ins.imm);
                            if (cv == 0) {
                                emit_instr(mf.blocks,
                                    encode_eor((Arm64Reg)SCRATCH_A,
                                                (Arm64Reg)SCRATCH_A,
                                                (Arm64Reg)SCRATCH_A, true));
                            } else if (cv >= -0xFFFF && cv <= 0xFFFF) {
                                emit_instr(mf.blocks,
                                    encode_movz((Arm64Reg)SCRATCH_A,
                                                 (uint16_t)(cv & 0xFFFF), 0, true));
                            } else {
                                uint32_t low  = (uint16_t)(ins.imm & 0xFFFF);
                                uint32_t imm2 = (uint16_t)((ins.imm >> 16) & 0xFFFF);
                                uint32_t imm3 = (uint16_t)((ins.imm >> 32) & 0xFFFF);
                                uint32_t imm4 = (uint16_t)((ins.imm >> 48) & 0xFFFF);
                                emit_instr(mf.blocks, encode_movz((Arm64Reg)SCRATCH_A,
                                                                  low, 0, true));
                                if (imm2) {
                                    emit_instr(mf.blocks, encode_movk((Arm64Reg)SCRATCH_A,
                                                                      imm2, 16, true));
                                }
                                if (imm3) {
                                    emit_instr(mf.blocks, encode_movk((Arm64Reg)SCRATCH_A,
                                                                      imm3, 32, true));
                                }
                                if (imm4) {
                                    emit_instr(mf.blocks, encode_movk((Arm64Reg)SCRATCH_A,
                                                                      imm4, 48, true));
                                }
                            }
                            emit_store(mf, ins.dst, SCRATCH_A);
                        }
                        break;
                    }
                    case ir::IrOp::ADD: {
                        emit_load(mf, ins.operands[0], SCRATCH_A);
                        emit_load(mf, ins.operands[1], SCRATCH_B);
                        emit_instr(mf.blocks,
                            encode_add_sub_reg((Arm64Reg)SCRATCH_A,
                                                (Arm64Reg)SCRATCH_A,
                                                (Arm64Reg)SCRATCH_B,
                                                false, true));
                        emit_store(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::SUB: {
                        emit_load(mf, ins.operands[0], SCRATCH_A);
                        emit_load(mf, ins.operands[1], SCRATCH_B);
                        emit_instr(mf.blocks,
                            encode_add_sub_reg((Arm64Reg)SCRATCH_A,
                                                (Arm64Reg)SCRATCH_A,
                                                (Arm64Reg)SCRATCH_B,
                                                true, true));
                        emit_store(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::MUL: {
                        emit_load(mf, ins.operands[0], SCRATCH_A);
                        emit_load(mf, ins.operands[1], SCRATCH_B);
                        emit_instr(mf.blocks,
                            encode_mul((Arm64Reg)SCRATCH_A, (Arm64Reg)SCRATCH_A,
                                       (Arm64Reg)SCRATCH_B, true));
                        emit_store(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::DIV: {
                        emit_load(mf, ins.operands[0], SCRATCH_A);
                        emit_load(mf, ins.operands[1], SCRATCH_B);
                        bool is_signed = ir_type_is_signed_int(ins.type);
                        if (is_signed) {
                            emit_instr(mf.blocks,
                                encode_sdiv((Arm64Reg)SCRATCH_A,
                                            (Arm64Reg)SCRATCH_A,
                                            (Arm64Reg)SCRATCH_B, true));
                        } else {
                            emit_instr(mf.blocks,
                                encode_udiv((Arm64Reg)SCRATCH_A,
                                            (Arm64Reg)SCRATCH_A,
                                            (Arm64Reg)SCRATCH_B, true));
                        }
                        emit_store(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::MOD: {
                        emit_load(mf, ins.operands[0], SCRATCH_A);
                        emit_load(mf, ins.operands[1], SCRATCH_B);
                        bool is_signed = ir_type_is_signed_int(ins.type);
                        if (is_signed) {
                            emit_instr(mf.blocks,
                                encode_sdiv((Arm64Reg)SCRATCH_C,
                                            (Arm64Reg)SCRATCH_A,
                                            (Arm64Reg)SCRATCH_B, true));
                        } else {
                            emit_instr(mf.blocks,
                                encode_udiv((Arm64Reg)SCRATCH_C,
                                            (Arm64Reg)SCRATCH_A,
                                            (Arm64Reg)SCRATCH_B, true));
                        }
                        emit_instr(mf.blocks,
                            encode_mul((Arm64Reg)SCRATCH_C, (Arm64Reg)SCRATCH_C,
                                       (Arm64Reg)SCRATCH_B, true));
                        emit_instr(mf.blocks,
                            encode_add_sub_reg((Arm64Reg)SCRATCH_A,
                                                (Arm64Reg)SCRATCH_A,
                                                (Arm64Reg)SCRATCH_C,
                                                true, true));
                        emit_store(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::AND: {
                        emit_load(mf, ins.operands[0], SCRATCH_A);
                        emit_load(mf, ins.operands[1], SCRATCH_B);
                        emit_instr(mf.blocks,
                            encode_and((Arm64Reg)SCRATCH_A, (Arm64Reg)SCRATCH_A,
                                       (Arm64Reg)SCRATCH_B, true));
                        emit_store(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::OR: {
                        emit_load(mf, ins.operands[0], SCRATCH_A);
                        emit_load(mf, ins.operands[1], SCRATCH_B);
                        emit_instr(mf.blocks,
                            encode_orr((Arm64Reg)SCRATCH_A, (Arm64Reg)SCRATCH_A,
                                       (Arm64Reg)SCRATCH_B, true));
                        emit_store(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::XOR: {
                        emit_load(mf, ins.operands[0], SCRATCH_A);
                        emit_load(mf, ins.operands[1], SCRATCH_B);
                        emit_instr(mf.blocks,
                            encode_eor((Arm64Reg)SCRATCH_A, (Arm64Reg)SCRATCH_A,
                                       (Arm64Reg)SCRATCH_B, true));
                        emit_store(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::SHL: {
                        emit_load(mf, ins.operands[0], SCRATCH_A);
                        emit_load(mf, ins.operands[1], SCRATCH_B);
                        emit_instr(mf.blocks,
                            encode_shift_reg((Arm64Reg)SCRATCH_A,
                                             (Arm64Reg)SCRATCH_A,
                                             (Arm64Reg)SCRATCH_B,
                                             SHIFT_LSL, 0, true));
                        emit_store(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::SHR: {
                        emit_load(mf, ins.operands[0], SCRATCH_A);
                        emit_load(mf, ins.operands[1], SCRATCH_B);
                        emit_instr(mf.blocks,
                            encode_shift_reg((Arm64Reg)SCRATCH_A,
                                             (Arm64Reg)SCRATCH_A,
                                             (Arm64Reg)SCRATCH_B,
                                             SHIFT_LSR, 0, true));
                        emit_store(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::SAR: {
                        emit_load(mf, ins.operands[0], SCRATCH_A);
                        emit_load(mf, ins.operands[1], SCRATCH_B);
                        emit_instr(mf.blocks,
                            encode_shift_reg((Arm64Reg)SCRATCH_A,
                                             (Arm64Reg)SCRATCH_A,
                                             (Arm64Reg)SCRATCH_B,
                                             SHIFT_ASR, 0, true));
                        emit_store(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::NEG: {
                        emit_load(mf, ins.operands[0], SCRATCH_A);
                        emit_instr(mf.blocks,
                            encode_add_sub_reg((Arm64Reg)SCRATCH_A,
                                                (Arm64Reg)SCRATCH_A,
                                                (Arm64Reg)31,
                                                true, true));
                        emit_store(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::NOT: {
                        emit_load(mf, ins.operands[0], SCRATCH_A);
                        emit_instr(mf.blocks,
                            encode_eor((Arm64Reg)SCRATCH_A, (Arm64Reg)SCRATCH_A,
                                       (Arm64Reg)31, true));
                        emit_store(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::LOAD: {
                        emit_load(mf, ins.operands[0], SCRATCH_A);
                        if (ir_type_is_float(ins.type)) {
                            emit_instr(mf.blocks,
                                encode_ldr_imm((Arm64Reg)FP_SCRATCH_A, 0, true));
                            emit_store_fp(mf, ins.dst, FP_SCRATCH_A);
                        } else {
                            emit_instr(mf.blocks,
                                encode_ldr_imm((Arm64Reg)SCRATCH_A, 0, true));
                            emit_store(mf, ins.dst, SCRATCH_A);
                        }
                        break;
                    }
                    case ir::IrOp::STORE: {
                        emit_load(mf, ins.operands[0], SCRATCH_A);
                        bool val_is_float = (ins.operands.size() > 1 &&
                            ir_type_is_float(ir_fn.values[ins.operands[1]].type));
                        if (val_is_float) {
                            emit_load_fp(mf, ins.operands[1], FP_SCRATCH_B);
                            emit_instr(mf.blocks,
                                encode_str_imm((Arm64Reg)FP_SCRATCH_B, 0, true));
                        } else {
                            emit_load(mf, ins.operands[1], SCRATCH_B);
                            emit_instr(mf.blocks,
                                encode_str_imm((Arm64Reg)SCRATCH_B, 0, true));
                        }
                        break;
                    }
                    case ir::IrOp::BR: {
                        uint32_t target_lbl = block_labels[ins.target_block];
                        if (target_lbl != INVALID_LABEL) {
                            uint32_t patch_at = 0;
                            emit_instr(mf.blocks, encode_b(0));
                            patch_at = static_cast<uint32_t>(
                                mf.blocks.back().instrs.size() - 1);
                            emit_fixup(mf.fixups, target_lbl,
                                       patch_at * 4, (patch_at + 1) * 4);
                        }
                        break;
                    }
                    case ir::IrOp::BR_COND: {
                        emit_load(mf, ins.operands[0], SCRATCH_A);
                        emit_instr(mf.blocks,
                            encode_cmp_imm((Arm64Reg)SCRATCH_A, 0, true));
                        uint32_t true_lbl = block_labels[ins.target_block];
                        uint32_t false_lbl = block_labels[ins.false_block];
                        if (true_lbl != INVALID_LABEL) {
                            uint32_t patch_at = 0;
                            emit_instr(mf.blocks, encode_b_cond(COND_NE, 0));
                            patch_at = static_cast<uint32_t>(
                                mf.blocks.back().instrs.size() - 1);
                            emit_fixup(mf.fixups, true_lbl,
                                       patch_at * 4, (patch_at + 1) * 4);
                        }
                        if (false_lbl != INVALID_LABEL) {
                            uint32_t patch_at = 0;
                            emit_instr(mf.blocks, encode_b(0));
                            patch_at = static_cast<uint32_t>(
                                mf.blocks.back().instrs.size() - 1);
                            emit_fixup(mf.fixups, false_lbl,
                                       patch_at * 4, (patch_at + 1) * 4);
                        }
                        break;
                    }
                    case ir::IrOp::CMP_EQ:
                    case ir::IrOp::CMP_NE:
                    case ir::IrOp::CMP_LT:
                    case ir::IrOp::CMP_GT:
                    case ir::IrOp::CMP_LE:
                    case ir::IrOp::CMP_GE:
                    case ir::IrOp::CMP_ULT:
                    case ir::IrOp::CMP_UGT:
                    case ir::IrOp::CMP_ULE:
                    case ir::IrOp::CMP_UGE: {
                        emit_load(mf, ins.operands[0], SCRATCH_A);
                        emit_load(mf, ins.operands[1], SCRATCH_B);
                        emit_instr(mf.blocks,
                            encode_cmp_reg((Arm64Reg)SCRATCH_A,
                                           (Arm64Reg)SCRATCH_B, true));
                        uint32_t cond = cond_for_cmp_op(ins.op);
                        emit_instr(mf.blocks,
                            encode_cset((Arm64Reg)SCRATCH_A, cond, true));
                        emit_store(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::FADD:
                    case ir::IrOp::FSUB:
                    case ir::IrOp::FMUL:
                    case ir::IrOp::FDIV: {
                        emit_load_fp(mf, ins.operands[0], FP_SCRATCH_A);
                        emit_load_fp(mf, ins.operands[1], FP_SCRATCH_B);
                        bool is_double = (ins.type == ir::IrType::F64);
                        uint32_t instr = 0;
                        switch (ins.op) {
                            case ir::IrOp::FADD: instr = encode_fadd((Arm64Reg)FP_SCRATCH_A, (Arm64Reg)FP_SCRATCH_A, (Arm64Reg)FP_SCRATCH_B, is_double); break;
                            case ir::IrOp::FSUB: instr = encode_fsub((Arm64Reg)FP_SCRATCH_A, (Arm64Reg)FP_SCRATCH_A, (Arm64Reg)FP_SCRATCH_B, is_double); break;
                            case ir::IrOp::FMUL: instr = encode_fmul((Arm64Reg)FP_SCRATCH_A, (Arm64Reg)FP_SCRATCH_A, (Arm64Reg)FP_SCRATCH_B, is_double); break;
                            case ir::IrOp::FDIV: instr = encode_fdiv((Arm64Reg)FP_SCRATCH_A, (Arm64Reg)FP_SCRATCH_A, (Arm64Reg)FP_SCRATCH_B, is_double); break;
                            default: break;
                        }
                        emit_instr(mf.blocks, instr);
                        emit_store_fp(mf, ins.dst, FP_SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::FNEG:
                    case ir::IrOp::FABS:
                    case ir::IrOp::FSQRT: {
                        emit_load_fp(mf, ins.operands[0], FP_SCRATCH_A);
                        bool is_double = (ins.type == ir::IrType::F64);
                        uint32_t instr = 0;
                        switch (ins.op) {
                            case ir::IrOp::FNEG:  instr = encode_fneg((Arm64Reg)FP_SCRATCH_A, (Arm64Reg)FP_SCRATCH_A, is_double); break;
                            case ir::IrOp::FABS:  instr = encode_fabs((Arm64Reg)FP_SCRATCH_A, (Arm64Reg)FP_SCRATCH_A, is_double); break;
                            case ir::IrOp::FSQRT: instr = encode_fsqrt((Arm64Reg)FP_SCRATCH_A, (Arm64Reg)FP_SCRATCH_A, is_double); break;
                            default: break;
                        }
                        emit_instr(mf.blocks, instr);
                        emit_store_fp(mf, ins.dst, FP_SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::FMIN:
                    case ir::IrOp::FMAX: {
                        emit_load_fp(mf, ins.operands[0], FP_SCRATCH_A);
                        emit_load_fp(mf, ins.operands[1], FP_SCRATCH_B);
                        bool is_double = (ins.type == ir::IrType::F64);
                        uint32_t instr;
                        if (ins.op == ir::IrOp::FMIN) {
                            instr = encode_fmin((Arm64Reg)FP_SCRATCH_A, (Arm64Reg)FP_SCRATCH_A, (Arm64Reg)FP_SCRATCH_B, is_double);
                        } else {
                            instr = encode_fmax((Arm64Reg)FP_SCRATCH_A, (Arm64Reg)FP_SCRATCH_A, (Arm64Reg)FP_SCRATCH_B, is_double);
                        }
                        emit_instr(mf.blocks, instr);
                        emit_store_fp(mf, ins.dst, FP_SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::FCMP_EQ:
                    case ir::IrOp::FCMP_NE:
                    case ir::IrOp::FCMP_LT:
                    case ir::IrOp::FCMP_GT:
                    case ir::IrOp::FCMP_LE:
                    case ir::IrOp::FCMP_GE: {
                        emit_load_fp(mf, ins.operands[0], FP_SCRATCH_A);
                        emit_load_fp(mf, ins.operands[1], FP_SCRATCH_B);
                        bool is_double = (ins.type == ir::IrType::F64);
                        emit_instr(mf.blocks,
                            encode_fcmp((Arm64Reg)FP_SCRATCH_A, (Arm64Reg)FP_SCRATCH_B, is_double));
                        uint32_t cond = cond_for_fcmp_op(ins.op);
                        emit_instr(mf.blocks,
                            encode_cset((Arm64Reg)SCRATCH_A, cond, true));
                        emit_store(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::F32TOF64: {
                        emit_load_fp(mf, ins.operands[0], FP_SCRATCH_A);
                        emit_instr(mf.blocks,
                            encode_fcvt_s2d((Arm64Reg)FP_SCRATCH_A, (Arm64Reg)FP_SCRATCH_A));
                        emit_store_fp(mf, ins.dst, FP_SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::F64TOF32: {
                        emit_load_fp(mf, ins.operands[0], FP_SCRATCH_A);
                        emit_instr(mf.blocks,
                            encode_fcvt_d2s((Arm64Reg)FP_SCRATCH_A, (Arm64Reg)FP_SCRATCH_A));
                        emit_store_fp(mf, ins.dst, FP_SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::ITOF:
                    case ir::IrOp::UITOF:
                    case ir::IrOp::FTOI:
                    case ir::IrOp::FTOUI: {
                        // TODO: implement SCVTF/UCVTF/FCVTZS/FCVTZU for int<->float conversions
                        unsupported = true;
                        break;
                    }
                    case ir::IrOp::MOV: {
                        if (ir_type_is_float(ins.type)) {
                            emit_load_fp(mf, ins.operands[0], FP_SCRATCH_A);
                            emit_store_fp(mf, ins.dst, FP_SCRATCH_A);
                        } else {
                            emit_load(mf, ins.operands[0], SCRATCH_A);
                            emit_store(mf, ins.dst, SCRATCH_A);
                        }
                        break;
                    }
                    case ir::IrOp::RET: {
                        if (!ins.operands.empty()) {
                            ir::IrValueId ret_val = ins.operands[0];
                            if (ret_val != ir::IR_NO_VALUE) {
                                if (ir_type_is_float(ir_fn.values[ret_val].type)) {
                                    emit_load_fp(mf, ret_val, FP_SCRATCH_A);
                                } else {
                                    emit_load(mf, ret_val, 0);
                                }
                            }
                        }
                        {
                            uint32_t total_frame = mf.stack_frame_size + CALLEE_SAVED_SIZE;
                            total_frame = (total_frame + 15) & ~15u;
                            int32_t total = (int32_t)total_frame;

                            const int32_t gp_base = total - 16;
                            emit_instr(mf.blocks, encode_simd_ldp((Arm64Reg)8, (Arm64Reg)9,
                                                                   (Arm64Reg)29, gp_base - 80 - 64, true));
                            emit_instr(mf.blocks, encode_simd_ldp((Arm64Reg)10, (Arm64Reg)11,
                                                                   (Arm64Reg)29, gp_base - 80 - 48, true));
                            emit_instr(mf.blocks, encode_simd_ldp((Arm64Reg)12, (Arm64Reg)13,
                                                                   (Arm64Reg)29, gp_base - 80 - 32, true));
                            emit_instr(mf.blocks, encode_simd_ldp((Arm64Reg)14, (Arm64Reg)15,
                                                                   (Arm64Reg)29, gp_base - 80 - 16, true));

                            emit_instr(mf.blocks, encode_ldp((Arm64Reg)19, (Arm64Reg)20,
                                                              (Arm64Reg)29, gp_base - 80, true));
                            emit_instr(mf.blocks, encode_ldp((Arm64Reg)21, (Arm64Reg)22,
                                                              (Arm64Reg)29, gp_base - 64, true));
                            emit_instr(mf.blocks, encode_ldp((Arm64Reg)23, (Arm64Reg)24,
                                                              (Arm64Reg)29, gp_base - 48, true));
                            emit_instr(mf.blocks, encode_ldp((Arm64Reg)25, (Arm64Reg)26,
                                                              (Arm64Reg)29, gp_base - 32, true));
                            emit_instr(mf.blocks, encode_ldp((Arm64Reg)27, (Arm64Reg)28,
                                                              (Arm64Reg)29, gp_base - 16, true));

                            emit_instr(mf.blocks,
                                encode_ldp_post((Arm64Reg)29, (Arm64Reg)30,
                                                (Arm64Reg)31, total, true));
                        }
                        emit_instr(mf.blocks, encode_ret((Arm64Reg)30));
                        break;
                    }
                    case ir::IrOp::ALLOCA: {
                        uint64_t bytes = ins.imm;
                        uint64_t aligned = (bytes + 15) & ~15ULL;
                        emit_instr(mf.blocks,
                            encode_sub_imm((Arm64Reg)31, (Arm64Reg)31,
                                            static_cast<uint32_t>(aligned), true));
                        emit_instr(mf.blocks,
                            encode_mov_reg((Arm64Reg)SCRATCH_A,
                                           (Arm64Reg)31, true));
                        emit_store(mf, ins.dst, SCRATCH_A);
                        break;
                    }
                    case ir::IrOp::CALL: {
                        uint64_t target_addr = 0;
                        if (!ins.func_name.empty()) {
                            target_addr = resolve_runtime_entry(
                                ins.func_name, opts_.runtime);
                            if (target_addr == 0 && opts_.resolve_user_fn) {
                                target_addr = opts_.resolve_user_fn(ins.func_name);
                            }
                            if (target_addr == 0 && opts_.resolve_native_fn) {
                                target_addr = opts_.resolve_native_fn(ins.func_name);
                            }
                        }
                        if (target_addr != 0) {
                            uint32_t idx = mf.intern_imm64(target_addr);
                            uint64_t addr = ins.imm;
                            uint32_t low = (uint16_t)(addr & 0xFFFF);
                            uint32_t imm2 = (uint16_t)((addr >> 16) & 0xFFFF);
                            uint32_t imm3 = (uint16_t)((addr >> 32) & 0xFFFF);
                            uint32_t imm4 = (uint16_t)((addr >> 48) & 0xFFFF);
                            emit_instr(mf.blocks,
                                encode_movz((Arm64Reg)16, low, 0, true));
                            if (imm2) {
                                emit_instr(mf.blocks,
                                    encode_movk((Arm64Reg)16, imm2, 16, true));
                            }
                            if (imm3) {
                                emit_instr(mf.blocks,
                                    encode_movk((Arm64Reg)16, imm3, 32, true));
                            }
                            if (imm4) {
                                emit_instr(mf.blocks,
                                    encode_movk((Arm64Reg)16, imm4, 48, true));
                            }
                            emit_instr(mf.blocks, encode_blr((Arm64Reg)16));
                        }
                        if (ins.dst != ir::IR_NO_VALUE) {
                            emit_store(mf, ins.dst, 0);
                        }
                        break;
                    }
                    case ir::IrOp::NOP:
                    case ir::IrOp::UNREACHABLE:
                        break;
                    default:
                        if (!unsupported) {
                            unsupported = true;
                            if (out_unsupported) *out_unsupported = true;
                        }
                        break;
                }
            }
        }

        return mf;
    }

    Arm64MFunction arm64_select(const ir::IrFunction &ir_fn,
                                 const RuntimeEntries *rt,
                                 bool *out_unsupported) {
        Arm64SelectorOptions opts;
        opts.mode = Arm64SelectorMode::VM_ABI;
        opts.runtime = rt;
        Arm64Selector sel(opts);
        return sel.select(ir_fn, out_unsupported);
    }

} // namespace arm64
} // namespace jit
