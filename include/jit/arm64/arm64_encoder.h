/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

#ifndef VESTA_JIT_ARM64_ENCODER_H
#define VESTA_JIT_ARM64_ENCODER_H

#include <cstdint>
#include <vector>

#include "jit/machine_ir.h"

namespace jit {
namespace arm64 {

    enum class Arm64Reg : uint8_t {
        X0, X1, X2, X3, X4, X5, X6, X7, X8, X9, X10, X11, X12, X13, X14, X15,
        X16, X17, X18, X19, X20, X21, X22, X23, X24, X25, X26, X27, X28,
        X29, X30, SP = 31, ZR = 31,
        W0 = 0, W1, W2, W3, W4, W5, W6, W7, W8, W9, W10, W11, W12, W13, W14, W15,
        W16, W17, W18, W19, W20, W21, W22, W23, W24, W25, W26, W27, W28,
        W29, W30, WSP = 31, WZR = 31,
        D0 = 0, D1, D2, D3, D4, D5, D6, D7, D8, D9, D10, D11, D12, D13, D14, D15,
        D16, D17, D18, D19, D20, D21, D22, D23, D24, D25, D26, D27, D28, D29, D30, D31,
        S0 = 0, S1, S2, S3, S4, S5, S6, S7, S8, S9, S10, S11, S12, S13, S14, S15,
        S16, S17, S18, S19, S20, S21, S22, S23, S24, S25, S26, S27, S28, S29, S30, S31,
    };

    static inline uint32_t reg_bits(Arm64Reg r) noexcept {
        return static_cast<uint32_t>(r) & 0x1F;
    }

    static inline bool is_xreg(Arm64Reg r) noexcept {
        return static_cast<uint32_t>(r) < 31;
    }

    static inline uint32_t sf_bit(bool is64) noexcept {
        return is64 ? 1u << 31 : 0u;
    }

    uint32_t encode_add_sub_imm(Arm64Reg rd, Arm64Reg rn, uint32_t imm12, bool is_sub, bool is64);
    uint32_t encode_add_sub_reg(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is_sub, bool is64);
    uint32_t encode_mul(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is64);
    uint32_t encode_sdiv(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is64);
    uint32_t encode_udiv(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is64);
    uint32_t encode_logical_imm(Arm64Reg rd, Arm64Reg rn, uint32_t imm, uint8_t opc, bool is64);
    uint32_t encode_logical_reg(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, uint8_t opc, bool is64);
    uint32_t encode_and(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is64);
    uint32_t encode_orr(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is64);
    uint32_t encode_eor(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is64);
    uint32_t encode_shift_reg(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, uint8_t shift_type, uint8_t amount, bool is64);
    uint32_t encode_mov_reg(Arm64Reg rd, Arm64Reg rm, bool is64);
    uint32_t encode_mov_imm(Arm64Reg rd, uint64_t value, bool is64);
    uint32_t encode_movn(Arm64Reg rd, uint16_t imm16, uint8_t shift, bool is64);
    uint32_t encode_movz(Arm64Reg rd, uint16_t imm16, uint8_t shift, bool is64);
    uint32_t encode_movk(Arm64Reg rd, uint16_t imm16, uint8_t shift, bool is64);
    uint32_t encode_cmp_imm(Arm64Reg rn, uint32_t imm12, bool is64);
    uint32_t encode_cmp_reg(Arm64Reg rn, Arm64Reg rm, bool is64);
    uint32_t encode_cmn_imm(Arm64Reg rn, uint32_t imm12, bool is64);
    uint32_t encode_cmn_reg(Arm64Reg rn, Arm64Reg rm, bool is64);
    uint32_t encode_ldr_imm(Arm64Reg rt, int32_t disp, bool is64);
    uint32_t encode_ldr_reg(Arm64Reg rt, Arm64Reg rn, Arm64Reg rm);
    uint32_t encode_ldr_reg64(Arm64Reg rt, Arm64Reg rn, Arm64Reg rm);
    uint32_t encode_str_imm(Arm64Reg rt, int32_t disp, bool is64);
    uint32_t encode_str_reg(Arm64Reg rt, Arm64Reg rn, Arm64Reg rm);
    uint32_t encode_str_reg64(Arm64Reg rt, Arm64Reg rn, Arm64Reg rm);
    uint32_t encode_ldp(Arm64Reg rt1, Arm64Reg rt2, Arm64Reg rn, int32_t disp, bool is64);
    uint32_t encode_stp(Arm64Reg rt1, Arm64Reg rt2, Arm64Reg rn, int32_t disp, bool is64);
    uint32_t encode_ldr_literal(Arm64Reg rt, int32_t offset, bool is64);
    uint32_t encode_b(int32_t offset);
    uint32_t encode_bl(int32_t offset);
    uint32_t encode_b_cond(uint32_t cond, int32_t offset);
    uint32_t encode_cbz(Arm64Reg rt, int32_t offset, bool is64);
    uint32_t encode_cbnz(Arm64Reg rt, int32_t offset, bool is64);
    uint32_t encode_csel(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, uint32_t cond, bool is64);
    uint32_t encode_cset(Arm64Reg rd, uint32_t cond, bool is64);
    uint32_t encode_csinc(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, uint32_t cond, bool is64);
    uint32_t encode_br(Arm64Reg rn);
    uint32_t encode_blr(Arm64Reg rn);
    uint32_t encode_ret(Arm64Reg rn = Arm64Reg::X30);
    uint32_t encode_stp_pre(Arm64Reg rt1, Arm64Reg rt2, Arm64Reg rn, int32_t disp, bool is64);
    uint32_t encode_ldp_post(Arm64Reg rt1, Arm64Reg rt2, Arm64Reg rn, int32_t disp, bool is64);
    uint32_t encode_sub_imm(Arm64Reg rd, Arm64Reg rn, uint32_t imm12, bool is64);
    uint32_t encode_add_imm(Arm64Reg rd, Arm64Reg rn, uint32_t imm12, bool is64);
    uint32_t encode_adr(Arm64Reg rd, int32_t offset);
    uint32_t encode_adrp(Arm64Reg rd, int32_t offset_page);

    uint32_t encode_simd_ldp(Arm64Reg rt1, Arm64Reg rt2, Arm64Reg rn, int32_t disp, bool is_double);
    uint32_t encode_simd_stp(Arm64Reg rt1, Arm64Reg rt2, Arm64Reg rn, int32_t disp, bool is_double);

    uint32_t encode_fadd(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is_double);
    uint32_t encode_fsub(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is_double);
    uint32_t encode_fmul(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is_double);
    uint32_t encode_fdiv(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is_double);
    uint32_t encode_fneg(Arm64Reg rd, Arm64Reg rn, bool is_double);
    uint32_t encode_fabs(Arm64Reg rd, Arm64Reg rn, bool is_double);
    uint32_t encode_fsqrt(Arm64Reg rd, Arm64Reg rn, bool is_double);
    uint32_t encode_fmin(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is_double);
    uint32_t encode_fmax(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm, bool is_double);
    uint32_t encode_fcmp(Arm64Reg rn, Arm64Reg rm, bool is_double);
    uint32_t encode_fmov_reg(Arm64Reg rd, Arm64Reg rn, bool is_double);
    uint32_t encode_fcvt_d2s(Arm64Reg rd_s, Arm64Reg rn_d);
    uint32_t encode_fcvt_s2d(Arm64Reg rd_d, Arm64Reg rn_s);

    static inline uint32_t encode_ldr_32(Arm64Reg rt, int32_t disp) {
        return encode_ldr_imm(rt, disp, false);
    }

    static inline uint32_t encode_ldr_64(Arm64Reg rt, int32_t disp) {
        return encode_ldr_imm(rt, disp, true);
    }

    static inline uint32_t encode_str_32(Arm64Reg rt, int32_t disp) {
        return encode_str_imm(rt, disp, false);
    }

    static inline uint32_t encode_str_64(Arm64Reg rt, int32_t disp) {
        return encode_str_imm(rt, disp, true);
    }

    static constexpr uint32_t COND_EQ = 0;
    static constexpr uint32_t COND_NE = 1;
    static constexpr uint32_t COND_CS = 2;
    static constexpr uint32_t COND_CC = 3;
    static constexpr uint32_t COND_MI = 4;
    static constexpr uint32_t COND_PL = 5;
    static constexpr uint32_t COND_VS = 6;
    static constexpr uint32_t COND_VC = 7;
    static constexpr uint32_t COND_HI = 8;
    static constexpr uint32_t COND_LS = 9;
    static constexpr uint32_t COND_GE = 10;
    static constexpr uint32_t COND_LT = 11;
    static constexpr uint32_t COND_GT = 12;
    static constexpr uint32_t COND_LE = 13;
    static constexpr uint32_t COND_AL = 14;

    static constexpr uint32_t SHIFT_LSL = 0;
    static constexpr uint32_t SHIFT_LSR = 1;
    static constexpr uint32_t SHIFT_ASR = 2;
    static constexpr uint32_t SHIFT_ROR = 3;

} // namespace arm64
} // namespace jit

#endif // VESTA_JIT_ARM64_ENCODER_H
