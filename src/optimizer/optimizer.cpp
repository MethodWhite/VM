/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

#include "optimizer/optimizer.h"
#include "emmit/emmit_decl.h"
#include <cstring>

namespace Assembly::Bytecode::Optimizer {

    static bool is_nop(const uint8_t *code, size_t len) {
        if (len < 1) return false;
        switch (code[0]) {
            case 0x90: return true;                     // NOP (1 byte)
            case 0x66: return len >= 2 && code[1] == 0x90; // NOP (2 byte, 66 90)
            case 0x0F: return len >= 2 && code[1] == 0x1F; // NOP (multi-byte, 0F 1F ...)
            case 0x87: return len >= 3
                && code[1] == 0xC0                          // XCHG r0, r0
                && code[2] == 0x00;                         // (ADD r0, 0)
            default: return false;
        }
    }

    static size_t nop_size(const uint8_t *code, size_t len) {
        if (len < 1) return 0;
        switch (code[0]) {
            case 0x90: return 1;
            case 0x66: return (len >= 2 && code[1] == 0x90) ? 2 : 0;
            case 0x0F: return (len >= 2 && code[1] == 0x1F) ? (len >= 3 ? code[2] + 3 : 0) : 0;
            default: return 0;
        }
    }

    static void pass_remove_nops(std::vector<uint8_t> &code) {
        size_t write = 0;
        for (size_t read = 0; read < code.size(); ) {
            size_t ns = nop_size(&code[read], code.size() - read);
            if (ns > 0) {
                read += ns;
                continue;
            }
            code[write++] = code[read++];
        }
        code.resize(write);
    }

    /**
     * @brief Longitud en bytes de una instruccion en @p pos.
     * Soporta opcodes primarios y extendidos (prefijo 0x00).
     */
    static size_t instr_len_full(const std::vector<uint8_t> &code, size_t pos) {
        if (pos >= code.size()) return 1;
        uint8_t b0 = code[pos];

        // Opcode extendido (0x00 prefix)
        if (b0 == 0x00 && pos + 1 < code.size()) {
            uint8_t b1 = code[pos + 1];
            // MIXED_SIZE immediates (3 + imm)
            if (b1 == 0x06 || b1 == 0x09 || b1 == 0x0C || b1 == 0x0F ||
                b1 == 0x12 || b1 == 0x41) return 4; // default imm8: 3+1
            // FIXED_11
            if (b1 == 0x15 || b1 == 0x5A || b1 == 0x5B || b1 == 0xFA) return 11;
            // FIXED_8
            if (b1 == 0x2D || b1 == 0x60 || b1 == 0x61 || b1 == 0x68 ||
                b1 == 0x69 || b1 == 0x6A) return 8;
            // FIXED_6
            if (b1 == 0xC3) return 6;
            // Default FIXED_4 (mayoria de extendidos)
            return 4;
        }

        // Opcodes primarios
        if (b0 == 0x10 || b0 == 0x11 || b0 == 0x28) return 10; // callvm, jmp, enter
        if (b0 == 0x14) return 4;  // xchg
        if (b0 == 0x29) return 1;  // leave
        if (b0 == 0xC3) return 1;  // ret
        if (b0 == 0xE9) return 3;  // jmp short
        if (b0 == 0x90) return 1;  // nop
        if ((b0 >= 0x12 && b0 <= 0x13) || (b0 >= 0x15 && b0 <= 0x16)) return 2; // push, pop, jmpr, callvmr
        if ((b0 >= 0x50 && b0 <= 0x5F)) return 2; // push/pop short
        if ((b0 >= 0xB0 && b0 <= 0xBF)) return 2; // mov reg, imm8
        if (b0 == 0x04) return 2; // inc/dec
        if ((b0 >= 0x01 && b0 <= 0x03)) return 2; // vminfo etc
        return 1; // default
    }

    /**
     * @brief Analiza una instruccion en @p pos para DCE.
     * Determina si tiene efectos secundarios, que registro escribe y lee.
     */
    static void analyze_instr(const std::vector<uint8_t> &code, size_t pos, size_t len,
                               bool &has_side_effect, uint8_t &dst, uint8_t &src, bool &reads_src) {
        has_side_effect = true;
        dst = 0xFF; src = 0xFF; reads_src = false;
        if (pos >= code.size() || len < 1) return;

        uint8_t b0 = code[pos];

        if (b0 == 0x00 && pos + 1 < code.size()) {
            uint8_t b1 = code[pos + 1];
            // ALU reg,reg / mov reg,reg: pure
            if ((b1 >= 0x05 && b1 <= 0x14) || (b1 >= 0x17 && b1 <= 0x1D) || b1 == 0x40) {
                has_side_effect = false;
                if (len >= 4) {
                    uint8_t regs = code[pos + 3];
                    dst = (regs >> 4) & 0xF;
                    src = regs & 0xF;
                    reads_src = true;
                }
                return;
            }
            // ALU imm: pure, dst in ctrl byte
            if (b1 == 0x06 || b1 == 0x09 || b1 == 0x0C || b1 == 0x0F ||
                b1 == 0x12 || b1 == 0x41) {
                has_side_effect = false;
                if (len >= 3) dst = code[pos + 2] & 0xF;
                return;
            }
            // MOV imm (inmed_mov): pure
            if (b1 == 0x15) {
                has_side_effect = false;
                if (len >= 3) dst = code[pos + 2] & 0xF;
                return;
            }
            // Float ops: pure
            if ((b1 >= 0xF0 && b1 <= 0xF9)) {
                has_side_effect = false;
                if (len >= 4) {
                    uint8_t regs = code[pos + 3];
                    dst = (regs >> 4) & 0xF;
                    src = regs & 0xF;
                    reads_src = true;
                }
                return;
            }
            // Float load/store/mowi: memory side effect
            if (b1 == 0xFA || b1 == 0xFB || b1 == 0xFC) return;
            // SIB memory access: side effect
            if (b1 == 0x07 || b1 == 0x0A || b1 == 0x0D || b1 == 0x10 ||
                b1 == 0x13 || b1 == 0x16 || b1 == 0x1E || b1 == 0x1F ||
                b1 == 0x42) return;
            // Default: assume side effects
            return;
        }

        // Primary opcodes
        if (b0 == 0x90) { has_side_effect = false; return; } // NOP
        if ((b0 >= 0xB0 && b0 <= 0xBF) && len >= 2) {
            has_side_effect = false;
            dst = b0 & 0xF;
            return;
        }
        if (b0 == 0x04 && len >= 2) {
            has_side_effect = false;
            dst = code[pos + 1] & 0xF;
            return;
        }
        // Everything else has side effects
    }

    static void pass_dead_code(std::vector<uint8_t> &code) {
        if (code.size() < 2) return;

        int16_t last_def[16];
        bool last_def_used[16];
        for (int r = 0; r < 16; r++) { last_def[r] = -1; last_def_used[r] = false; }

        std::vector<bool> dead(code.size(), false);

        size_t i = 0;
        while (i < code.size()) {
            size_t len = instr_len_full(code, i);
            if (i + len > code.size()) break;

            bool has_side_effect, reads_src;
            uint8_t dst, src;
            analyze_instr(code, i, len, has_side_effect, dst, src, reads_src);

            if (reads_src && src < 16 && last_def[src] >= 0)
                last_def_used[src] = true;

            if (has_side_effect) {
                for (int r = 0; r < 16; r++)
                    if (last_def[r] >= 0) last_def_used[r] = true;
            }

            if (dst < 16) {
                if (last_def[dst] >= 0 && !last_def_used[dst]) {
                    size_t plen = instr_len_full(code, last_def[dst]);
                    for (size_t j = 0; j < plen; j++)
                        dead[last_def[dst] + j] = true;
                }
                last_def[dst] = (int16_t)i;
                last_def_used[dst] = false;
            }
            i += len;
        }

        for (int r = 0; r < 16; r++) {
            if (last_def[r] >= 0 && !last_def_used[r]) {
                size_t plen = instr_len_full(code, last_def[r]);
                for (size_t j = 0; j < plen; j++)
                    dead[last_def[r] + j] = true;
            }
        }

        size_t write = 0;
        for (size_t read = 0; read < code.size(); read++)
            if (!dead[read]) code[write++] = code[read];
        code.resize(write);
    }

    static void pass_peephole(std::vector<uint8_t> &code) {
        if (code.size() < 2) return;
        size_t write = 0;
        size_t read = 0;
        while (read < code.size()) {
            // mov r, r (mismo registro) -> eliminar
            // Formato .vel: 0x10 = MOV, sigue reg_dst (4 bits) + reg_src (4 bits)
            if (read + 2 <= code.size() && (code[read] & 0xF0) == 0x10) {
                uint8_t dst = (code[read + 1] >> 4) & 0xF;
                uint8_t src = code[read + 1] & 0xF;
                if (dst == src) {
                    read += 2;
                    continue;
                }
            }

            // push followed by pop (same reg) -> eliminate
            if (read + 4 <= code.size() && code[read] == 0x50 + (code[read+1] & 0xF)
                && code[read+2] == 0x58 + (code[read+3] & 0xF)
                && (code[read+1] & 0xF) == (code[read+3] & 0xF)) {
                read += 4;
                continue;
            }

            // jmp next_instruction -> eliminate
            if (read + 2 <= code.size() && code[read] == 0xE9) {
                int16_t disp = (int16_t)(code[read+1] | (read+2 < code.size() ? (code[read+2] << 8) : 0));
                if (disp == 2) { // jmp to next byte = no-op
                    read += 3;
                    continue;
                }
            }

            code[write++] = code[read++];
        }
        code.resize(write);
    }

    void BytecodeOptimizer::optimize(std::vector<uint8_t> &code) {
        if (code.empty()) return;

        pass_remove_nops(code);
        pass_peephole(code);
        pass_dead_code(code);
    }

} // namespace Assembly::Bytecode::Optimizer
