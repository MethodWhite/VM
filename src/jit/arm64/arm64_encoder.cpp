/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

#include "jit/arm64/arm64_encoder.h"
#include <cstdint>
#include <cstring>

namespace jit {
namespace arm64 {

    static inline uint32_t encode_r(uint32_t op31_24, uint32_t op23_21,
                                     uint32_t op15_10, uint32_t op4_0,
                                     uint32_t rd, uint32_t rn,
                                     uint32_t rm) noexcept {
        return op31_24 | op23_21 | (rm << 16) | op15_10 | (rn << 5) | rd;
    }

    static inline uint32_t encode_i(uint32_t op31_23, uint32_t op22_12,
                                     uint32_t op11_5, uint32_t op4_0,
                                     uint32_t rd, uint32_t rn,
                                     uint32_t imm) noexcept {
        return op31_23 | ((imm >> 7) & 0x7000) | (rn << 5) | rd;
    }

    static inline uint32_t encode_d(uint32_t op31_22, uint32_t op21_10,
                                     uint32_t op9_0,
                                     uint32_t rt, uint32_t rn,
                                     uint32_t imm12) noexcept {
        return op31_22 | (imm12 << 10) | (rn << 5) | rt;
    }

    static inline uint32_t encode_branch(uint32_t op31_26, uint32_t op25_0,
                                          int32_t offset) noexcept {
        return op31_26 | (static_cast<uint32_t>(offset >> 2) & 0x03FFFFFFu);
    }

    static inline uint32_t encode_cb(uint32_t op31_25, uint32_t op24_5,
                                      uint32_t op4_0,
                                      uint32_t rt, int32_t offset,
                                      uint32_t cond) noexcept {
        const uint32_t imm19 = (static_cast<uint32_t>(offset >> 2) & 0x7FFFFu);
        return op31_25 | (cond << 16) | (imm19 << 5) | rt;
    }

    uint32_t encode_add_sub_imm(Arm64Reg rd, Arm64Reg rn, uint32_t imm12,
                                 bool is_sub, bool is64) {
        return sf_bit(is64) | 0x11000000u | (is_sub ? 0x40000000u : 0u) |
               ((imm12 & 0xFFF) << 10) | (reg_bits(rn) << 5) | reg_bits(rd);
    }

    uint32_t encode_add_sub_reg(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm,
                                 bool is_sub, bool is64) {
        return sf_bit(is64) | 0x0B000000u | (is_sub ? 0x40000000u : 0u) |
               (reg_bits(rm) << 16) | (reg_bits(rn) << 5) | reg_bits(rd);
    }

    uint32_t encode_add_imm(Arm64Reg rd, Arm64Reg rn, uint32_t imm12, bool is64) {
        return encode_add_sub_imm(rd, rn, imm12, false, is64);
    }

    uint32_t encode_sub_imm(Arm64Reg rd, Arm64Reg rn, uint32_t imm12, bool is64) {
        return encode_add_sub_imm(rd, rn, imm12, true, is64);
    }

    uint32_t encode_mul(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is64) {
        return sf_bit(is64) | 0x1B000000u | (reg_bits(rm) << 16) |
               0x00007C00u | (reg_bits(rn) << 5) | reg_bits(rd);
    }

    uint32_t encode_sdiv(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is64) {
        return sf_bit(is64) | 0x1AC00C00u | (reg_bits(rm) << 16) |
               (reg_bits(rn) << 5) | reg_bits(rd);
    }

    uint32_t encode_udiv(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is64) {
        return sf_bit(is64) | 0x1AC00800u | (reg_bits(rm) << 16) |
               (reg_bits(rn) << 5) | reg_bits(rd);
    }

    uint32_t encode_logical_imm(Arm64Reg rd, Arm64Reg rn, uint32_t imm,
                                 uint8_t opc, bool is64) {
        return sf_bit(is64) | 0x12000000u | ((uint32_t)opc << 29) |
               ((imm & 0x1F) << 16) | ((imm >> 5) << 10) |
               (reg_bits(rn) << 5) | reg_bits(rd);
    }

    uint32_t encode_logical_reg(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm,
                                 uint8_t opc, bool is64) {
        return sf_bit(is64) | 0x0A000000u | ((uint32_t)opc << 29) |
               (reg_bits(rm) << 16) | (reg_bits(rn) << 5) | reg_bits(rd);
    }

    uint32_t encode_and(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is64) {
        return encode_logical_reg(rd, rn, rm, 0, is64);
    }

    uint32_t encode_orr(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is64) {
        return encode_logical_reg(rd, rn, rm, 1, is64);
    }

    uint32_t encode_eor(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is64) {
        return encode_logical_reg(rd, rn, rm, 2, is64);
    }

    uint32_t encode_shift_reg(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm,
                               uint8_t shift_type, uint8_t amount, bool is64) {
        return sf_bit(is64) | 0x0A000000u | (reg_bits(rm) << 16) |
               ((uint32_t)shift_type << 22) | ((uint32_t)amount << 10) |
               (reg_bits(rn) << 5) | reg_bits(rd);
    }

    uint32_t encode_mov_reg(Arm64Reg rd, Arm64Reg rm, bool is64) {
        return encode_orr(rd, Arm64Reg::ZR, rm, is64);
    }

    uint32_t encode_movn(Arm64Reg rd, uint16_t imm16, uint8_t shift, bool is64) {
        return sf_bit(is64) | 0x12800000u | ((uint32_t)(shift >> 4) << 21) |
               ((uint32_t)imm16 << 5) | reg_bits(rd);
    }

    uint32_t encode_movz(Arm64Reg rd, uint16_t imm16, uint8_t shift, bool is64) {
        return sf_bit(is64) | 0x52800000u | ((uint32_t)(shift >> 4) << 21) |
               ((uint32_t)imm16 << 5) | reg_bits(rd);
    }

    uint32_t encode_movk(Arm64Reg rd, uint16_t imm16, uint8_t shift, bool is64) {
        return sf_bit(is64) | 0x72800000u | ((uint32_t)(shift >> 4) << 21) |
               ((uint32_t)imm16 << 5) | reg_bits(rd);
    }

    uint32_t encode_mov_imm(Arm64Reg rd, uint64_t value, bool is64) {
        if (!is64 && (value & 0xFFFFFFFFu) <= 0xFFFFu) {
            return encode_movz(rd, (uint16_t)value, 0, false);
        }
        if (value <= 0xFFFFu) {
            return encode_movz(rd, (uint16_t)value, 0, true);
        }
        return encode_movz(rd, (uint16_t)value, 0, is64);
    }

    uint32_t encode_cmp_imm(Arm64Reg rn, uint32_t imm12, bool is64) {
        return encode_add_sub_imm(Arm64Reg::ZR, rn, imm12, true, is64);
    }

    uint32_t encode_cmp_reg(Arm64Reg rn, Arm64Reg rm, bool is64) {
        return encode_add_sub_reg(Arm64Reg::ZR, rn, rm, true, is64);
    }

    uint32_t encode_cmn_imm(Arm64Reg rn, uint32_t imm12, bool is64) {
        return encode_add_sub_imm(Arm64Reg::ZR, rn, imm12, false, is64);
    }

    uint32_t encode_cmn_reg(Arm64Reg rn, Arm64Reg rm, bool is64) {
        return encode_add_sub_reg(Arm64Reg::ZR, rn, rm, false, is64);
    }

    uint32_t encode_ldr_imm(Arm64Reg rt, int32_t disp, bool is64) {
        const uint32_t size = is64 ? 3u : 2u;
        const uint32_t imm12 = (static_cast<uint32_t>(disp >> (is64 ? 3 : 2))) & 0xFFFu;
        return (size << 30) | 0x39000000u | (imm12 << 10) |
               (reg_bits(rt) << 5) | reg_bits(rt);
    }

    uint32_t encode_ldr_reg(Arm64Reg rt, Arm64Reg rn, Arm64Reg rm) {
        return 0x3CE00000u | (reg_bits(rm) << 16) | (reg_bits(rn) << 5) |
               reg_bits(rt);
    }

    uint32_t encode_str_imm(Arm64Reg rt, int32_t disp, bool is64) {
        const uint32_t size = is64 ? 3u : 2u;
        const uint32_t imm12 = (static_cast<uint32_t>(disp >> (is64 ? 3 : 2))) & 0xFFFu;
        return (size << 30) | 0x38000000u | (imm12 << 10) |
               (reg_bits(rt) << 5) | reg_bits(rt);
    }

    uint32_t encode_str_reg(Arm64Reg rt, Arm64Reg rn, Arm64Reg rm) {
        return 0x3C800000u | (reg_bits(rm) << 16) | (reg_bits(rn) << 5) |
               reg_bits(rt);
    }

    uint32_t encode_ldp(Arm64Reg rt1, Arm64Reg rt2, Arm64Reg rn,
                         int32_t disp, bool is64) {
        const uint32_t size = is64 ? 2u : 1u;
        const uint32_t imm7 = (static_cast<uint32_t>(disp >> (is64 ? 3 : 2))) & 0x7Fu;
        return (size << 30) | 0x29400000u | (imm7 << 15) |
               (reg_bits(rt2) << 10) | (reg_bits(rn) << 5) | reg_bits(rt1);
    }

    uint32_t encode_stp(Arm64Reg rt1, Arm64Reg rt2, Arm64Reg rn,
                         int32_t disp, bool is64) {
        const uint32_t size = is64 ? 2u : 1u;
        const uint32_t imm7 = (static_cast<uint32_t>(disp >> (is64 ? 3 : 2))) & 0x7Fu;
        return (size << 30) | 0x29000000u | (imm7 << 15) |
               (reg_bits(rt2) << 10) | (reg_bits(rn) << 5) | reg_bits(rt1);
    }

    uint32_t encode_stp_pre(Arm64Reg rt1, Arm64Reg rt2, Arm64Reg rn,
                             int32_t disp, bool is64) {
        const uint32_t size = is64 ? 2u : 1u;
        const uint32_t imm7 = (static_cast<uint32_t>((-disp) >> (is64 ? 3 : 2))) & 0x7Fu;
        return (size << 30) | 0x29800000u | (imm7 << 15) |
               (reg_bits(rt2) << 10) | (reg_bits(rn) << 5) | reg_bits(rt1);
    }

    uint32_t encode_ldp_post(Arm64Reg rt1, Arm64Reg rt2, Arm64Reg rn,
                              int32_t disp, bool is64) {
        const uint32_t size = is64 ? 2u : 1u;
        const uint32_t imm7 = (static_cast<uint32_t>(disp >> (is64 ? 3 : 2))) & 0x7Fu;
        return (size << 30) | 0x28C00000u | (imm7 << 15) |
               (reg_bits(rt2) << 10) | (reg_bits(rn) << 5) | reg_bits(rt1);
    }

    uint32_t encode_ldr_literal(Arm64Reg rt, int32_t offset, bool is64) {
        const uint32_t imm19 = (static_cast<uint32_t>(offset >> 2)) & 0x7FFFFu;
        return (is64 ? 0x58000000u : 0x18000000u) | (imm19 << 5) | reg_bits(rt);
    }

    uint32_t encode_b(int32_t offset) {
        return 0x14000000u | (static_cast<uint32_t>(offset >> 2) & 0x03FFFFFFu);
    }

    uint32_t encode_bl(int32_t offset) {
        return 0x94000000u | (static_cast<uint32_t>(offset >> 2) & 0x03FFFFFFu);
    }

    uint32_t encode_b_cond(uint32_t cond, int32_t offset) {
        const uint32_t imm19 = (static_cast<uint32_t>(offset >> 2)) & 0x7FFFFu;
        return 0x54000000u | (cond << 16) | (imm19 << 5);
    }

    uint32_t encode_csel(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm,
                          uint32_t cond, bool is64) {
        return sf_bit(is64) | 0x1A800000u | (reg_bits(rm) << 16) |
               (cond << 12) | (reg_bits(rn) << 5) | reg_bits(rd);
    }

    uint32_t encode_cset(Arm64Reg rd, uint32_t cond, bool is64) {
        return encode_csinc(rd, Arm64Reg::ZR, Arm64Reg::ZR,
                            static_cast<uint32_t>(cond ^ 1), is64);
    }

    uint32_t encode_csinc(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm,
                           uint32_t cond, bool is64) {
        return sf_bit(is64) | 0x1A800400u | (reg_bits(rm) << 16) |
               (cond << 12) | (reg_bits(rn) << 5) | reg_bits(rd);
    }

    uint32_t encode_br(Arm64Reg rn) {
        return 0xD61F0000u | (reg_bits(rn) << 5);
    }

    uint32_t encode_blr(Arm64Reg rn) {
        return 0xD63F0000u | (reg_bits(rn) << 5);
    }

    uint32_t encode_ret(Arm64Reg rn) {
        return 0xD65F0000u | (reg_bits(rn) << 5);
    }

    uint32_t encode_adr(Arm64Reg rd, int32_t offset) {
        const uint32_t immlo = (static_cast<uint32_t>(offset >> 2)) & 0x3u;
        const uint32_t immhi = (static_cast<uint32_t>(offset >> 4)) & 0x7FFFFu;
        return 0x10000000u | (immlo << 29) | (immhi << 5) | reg_bits(rd);
    }

    uint32_t encode_adrp(Arm64Reg rd, int32_t offset_page) {
        const uint32_t immlo = (static_cast<uint32_t>(offset_page >> 2)) & 0x3u;
        const uint32_t immhi = (static_cast<uint32_t>(offset_page >> 4)) & 0x7FFFFu;
        return 0x90000000u | (immlo << 29) | (immhi << 5) | reg_bits(rd);
    }

    uint32_t encode_ldr_reg64(Arm64Reg rt, Arm64Reg rn, Arm64Reg rm) {
        return 0xF8600000u | (reg_bits(rm) << 16) | (reg_bits(rn) << 5) | reg_bits(rt);
    }

    uint32_t encode_str_reg64(Arm64Reg rt, Arm64Reg rn, Arm64Reg rm) {
        return 0xF8200000u | (reg_bits(rm) << 16) | (reg_bits(rn) << 5) | reg_bits(rt);
    }

    uint32_t encode_cbz(Arm64Reg rt, int32_t offset, bool is64) {
        const uint32_t imm19 = (static_cast<uint32_t>(offset >> 2)) & 0x7FFFFu;
        return sf_bit(is64) | 0x34000000u | (imm19 << 5) | reg_bits(rt);
    }

    uint32_t encode_cbnz(Arm64Reg rt, int32_t offset, bool is64) {
        const uint32_t imm19 = (static_cast<uint32_t>(offset >> 2)) & 0x7FFFFu;
        return sf_bit(is64) | 0x35000000u | (imm19 << 5) | reg_bits(rt);
    }

    uint32_t encode_simd_stp(Arm64Reg rt1, Arm64Reg rt2, Arm64Reg rn, int32_t disp, bool is_double) {
        const uint32_t imm7 = (static_cast<uint32_t>(disp >> 3)) & 0x7Fu;
        const uint32_t sz = is_double ? (1u << 30) : 0u;
        return sz | 0x6D000000u | (imm7 << 15) |
               (reg_bits(rt2) << 10) | (reg_bits(rn) << 5) | reg_bits(rt1);
    }

    uint32_t encode_simd_ldp(Arm64Reg rt1, Arm64Reg rt2, Arm64Reg rn, int32_t disp, bool is_double) {
        const uint32_t imm7 = (static_cast<uint32_t>(disp >> 3)) & 0x7Fu;
        const uint32_t sz = is_double ? (1u << 30) : 0u;
        return sz | 0x6D400000u | (imm7 << 15) |
               (reg_bits(rt2) << 10) | (reg_bits(rn) << 5) | reg_bits(rt1);
    }

    static inline uint32_t encode_fp_binary(uint32_t subop, Arm64Reg rd,
                                             Arm64Reg rn, Arm64Reg rm,
                                             bool is_double) {
        const uint32_t sz = is_double ? (1u << 22) : 0u;
        return 0x7C000000u | sz | (1u << 21) | (reg_bits(rm) << 16) |
               (subop << 10) | (reg_bits(rn) << 5) | reg_bits(rd);
    }

    static inline uint32_t encode_fp_unary(uint32_t subop, Arm64Reg rd,
                                            Arm64Reg rn, bool is_double) {
        const uint32_t sz = is_double ? (1u << 22) : 0u;
        return 0x7C000000u | sz | (1u << 21) | (subop << 10) |
               (reg_bits(rn) << 5) | reg_bits(rd);
    }

    uint32_t encode_fadd(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is_double) {
        return encode_fp_binary(0x2C, rd, rn, rm, is_double);
    }

    uint32_t encode_fsub(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is_double) {
        return encode_fp_binary(0x2E, rd, rn, rm, is_double);
    }

    uint32_t encode_fmul(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is_double) {
        return encode_fp_binary(0x28, rd, rn, rm, is_double);
    }

    uint32_t encode_fdiv(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is_double) {
        return encode_fp_binary(0x26, rd, rn, rm, is_double);
    }

    uint32_t encode_fneg(Arm64Reg rd, Arm64Reg rn, bool is_double) {
        return encode_fp_unary(0x22, rd, rn, is_double);
    }

    uint32_t encode_fabs(Arm64Reg rd, Arm64Reg rn, bool is_double) {
        return encode_fp_unary(0x20, rd, rn, is_double);
    }

    uint32_t encode_fsqrt(Arm64Reg rd, Arm64Reg rn, bool is_double) {
        return encode_fp_unary(0x23, rd, rn, is_double);
    }

    uint32_t encode_fmin(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is_double) {
        return encode_fp_binary(0x25, rd, rn, rm, is_double);
    }

    uint32_t encode_fmax(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is_double) {
        return encode_fp_binary(0x29, rd, rn, rm, is_double);
    }

    uint32_t encode_fcmp(Arm64Reg rn, Arm64Reg rm, bool is_double) {
        const uint32_t sz = is_double ? (1u << 22) : 0u;
        return 0x7C000000u | sz | (1u << 21) | (reg_bits(rm) << 16) |
               (0x0Eu << 10) | (reg_bits(rn) << 5);
    }

    uint32_t encode_fmov_reg(Arm64Reg rd, Arm64Reg rn, bool is_double) {
        const uint32_t sz = is_double ? (1u << 22) : 0u;
        return 0x7C000000u | sz | (1u << 21) | (0x28u << 10) |
               (reg_bits(rn) << 5) | reg_bits(rd);
    }

    uint32_t encode_fcvt_d2s(Arm64Reg rd_s, Arm64Reg rn_d) {
        return 0x1E658000u | (reg_bits(rn_d) << 5) | reg_bits(rd_s);
    }

    uint32_t encode_fcvt_s2d(Arm64Reg rd_d, Arm64Reg rn_s) {
        return 0x1E298000u | (reg_bits(rn_s) << 5) | reg_bits(rd_d);
    }

} // namespace arm64
} // namespace jit
