/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

#include "optimizer/optimizer.h"
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

    static void pass_dead_code(std::vector<uint8_t> &code) {
        if (code.size() < 2) return;

        auto instr_len = [&](size_t pos) -> size_t {
            if (pos >= code.size()) return 1;
            uint8_t op = code[pos];
            uint8_t group = op >> 4;
            if (op == 0x90) return 1;
            if (group == 0x1) return 2;
            if (op >= 0x50 && op <= 0x5F) return 2;
            if (op >= 0x58 && op <= 0x67) return 2;
            if (op == 0xE9) return 3;
            if (op >= 0xB0 && op <= 0xBF) return 2;
            return 1;
        };

        int16_t last_def[16];
        bool last_def_used[16];
        for (int r = 0; r < 16; r++) {
            last_def[r] = -1;
            last_def_used[r] = false;
        }

        std::vector<bool> dead(code.size(), false);

        size_t i = 0;
        while (i < code.size()) {
            size_t len = instr_len(i);
            if (i + len > code.size()) break;

            uint8_t op = code[i];
            uint8_t group = op >> 4;
            bool has_side_effect = true;
            uint8_t dst = 0xFF;
            uint8_t src = 0xFF;
            bool reads_src = false;

            if (op == 0x90) {
                has_side_effect = false;
                dead[i] = true;
            } else if (group == 0x1 && len >= 2) {
                has_side_effect = false;
                dst = (code[i+1] >> 4) & 0xF;
                src = code[i+1] & 0xF;
                reads_src = true;
            } else if (op >= 0xB0 && op <= 0xBF && len >= 2) {
                has_side_effect = false;
                dst = op & 0xF;
            }

            if (reads_src && src < 16 && last_def[src] >= 0) {
                last_def_used[src] = true;
            }

            if (has_side_effect) {
                for (int r = 0; r < 16; r++) {
                    if (last_def[r] >= 0) last_def_used[r] = true;
                }
            }

            if (dst < 16) {
                if (last_def[dst] >= 0 && !last_def_used[dst]) {
                    size_t prev_len = instr_len(last_def[dst]);
                    for (size_t j = 0; j < prev_len; j++) {
                        dead[last_def[dst] + j] = true;
                    }
                }
                last_def[dst] = (int16_t)i;
                last_def_used[dst] = false;
            }

            i += len;
        }

        for (int r = 0; r < 16; r++) {
            if (last_def[r] >= 0 && !last_def_used[r]) {
                size_t prev_len = instr_len(last_def[r]);
                for (size_t j = 0; j < prev_len; j++) {
                    dead[last_def[r] + j] = true;
                }
            }
        }

        size_t write = 0;
        for (size_t read = 0; read < code.size(); read++) {
            if (!dead[read]) {
                code[write++] = code[read];
            }
        }
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
