/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/x86_encoder.cpp
 * @brief Implementacion del encoder hand-rolled x86-64.
 *
 * Reference primaria: Intel SDM Vol 2 (Instruction Set Reference) y
 * AMD APM Vol 3.  Convenciones de notacion: "REX.W" = bit W del prefix
 * REX (0x48 base); "/r" = ModR/M con reg field; "/N" = ModR/M con
 * subop N en reg field; "rel32" = displacement 32-bit signed.
 */

#include "jit/x86_encoder.h"

#include <cstring>

namespace jit {

    namespace {
        /* Opcodes de ALU (reg/reg, REX.W + opcode + ModR/M) y subops
         * para variantes reg/imm32 (REX.W + 0x81 /subop + imm32).
         *
         * Indice por enum MOp - MOp::ADD (0..5):
         *   0 = ADD, 1 = SUB, 2 = AND, 3 = OR, 4 = XOR, 5 = CMP
         * Pero usamos la enum MOp tal cual para mantenerlo simple. */

        struct AluEnc {
            uint8_t op_rr;   ///< opcode reg/reg (direccion 0: dst=r/m, src=reg)
            uint8_t subop;   ///< subop para variantes imm32 con opcode 0x81
        };

        /// Funcion helper que devuelve el AluEnc para un MOp dado.
        /// Mantiene el mapping ALU op -> (opcode reg/reg, alu_subop) sin
        /// usar designated initializers (no soportados en non-trivial
        /// init en gcc/mingw modo C++).
        inline AluEnc alu_enc_for(MOp op) noexcept {
            switch (op) {
                case MOp::ADD: return AluEnc{0x01, 0};
                case MOp::OR:  return AluEnc{0x09, 1};
                case MOp::AND: return AluEnc{0x21, 4};
                case MOp::SUB: return AluEnc{0x29, 5};
                case MOp::XOR: return AluEnc{0x31, 6};
                case MOp::CMP: return AluEnc{0x39, 7};
                default:       return AluEnc{0, 0};
            }
        }

        /// Opcode de Jcc para una condicion dada (corto: 0x70+cc, largo: 0x0F 0x80+cc).
        /// Devuelve byte de la variante larga.
        inline uint8_t jcc_long_opcode(MCond cc) {
            return static_cast<uint8_t>(0x80 + static_cast<uint8_t>(cc));
        }
    } // namespace anonymous

    /* ===================================================================== */
    /* encode (pasada principal + resolve)                                    */
    /* ===================================================================== */

    size_t X86Encoder::encode(MFunction &fn, std::vector<uint8_t> &out) {
        instr_count_ = 0;
        const size_t base = out.size();

        /* Reservar capacidad estimada: ~6 bytes promedio por instr. */
        size_t total_instrs = 0;
        for (const auto &b : fn.blocks) total_instrs += b.instrs.size();
        out.reserve(out.size() + total_instrs * 6);

        for (auto &block : fn.blocks) {
            block.byte_offset = static_cast<uint32_t>(out.size() - base);
            /* Si el bloque tiene label_id, registrarlo. */
            if (block.label_id != MLABEL_INVALID
             && block.label_id < fn.label_offsets.size()) {
                fn.label_offsets[block.label_id] =
                    static_cast<uint32_t>(out.size() - base);
            }
            for (const auto &mi : block.instrs) {
                ++instr_count_;
                if (!emit_instr(fn, mi, out)) {
                    /* fail-fast: opcode no soportado.  El INT3 hace que la
                     * ejecucion crasheee con SIGTRAP en lugar de seguir
                     * con basura. */
                    put8(out, 0xCC);
                    return 0;
                }
            }
        }

        resolve_fixups(fn, out, base);
        return out.size() - base;
    }

    /* ===================================================================== */
    /* Dispatcher                                                             */
    /* ===================================================================== */

    bool X86Encoder::emit_instr(MFunction &fn, const MInstr &mi,
                                std::vector<uint8_t> &out) {
        switch (mi.op) {
            case MOp::NOP:        put8(out, 0x90); return true;
            case MOp::INT3:       put8(out, 0xCC); return true;
            case MOp::RET:        emit_ret(out); return true;
            case MOp::MOV:        emit_mov(fn, mi, out); return true;
            case MOp::LEA:        emit_lea(fn, mi, out); return true;
            case MOp::PUSH:       emit_push(mi, out); return true;
            case MOp::POP:        emit_pop(mi, out); return true;
            case MOp::ADD:
            case MOp::SUB:
            case MOp::AND:
            case MOp::OR:
            case MOp::XOR: {
                const AluEnc enc = alu_enc_for(mi.op);
                emit_alu(fn, mi, out, enc.op_rr, enc.subop);
                return true;
            }
            case MOp::CMP:        emit_cmp(fn, mi, out); return true;
            case MOp::TEST:       emit_test(fn, mi, out); return true;
            case MOp::IMUL:       emit_imul(fn, mi, out); return true;
            case MOp::SHL:        emit_shift(fn, mi, out, 4); return true;
            case MOp::SHR:        emit_shift(fn, mi, out, 5); return true;
            case MOp::SAR:        emit_shift(fn, mi, out, 7); return true;
            case MOp::IDIV: {
                /* IDIV r/m64: REX.W + F7 /7 con divisor en src1.reg. */
                if (mi.src1.kind != MOperandKind::REG) {
                    put8(out, 0xCC);
                    return true;
                }
                const uint8_t rex = rex_byte(true, 0, mi.src1.reg);
                if (rex) put8(out, rex);
                put8(out, 0xF7);
                put8(out, modrm(3, 7, mi.src1.reg & 7));
                return true;
            }
            case MOp::CQO: {
                /* CQO: REX.W + 99 -- sign-extend RAX -> RDX:RAX. */
                put8(out, 0x48);
                put8(out, 0x99);
                return true;
            }
            case MOp::MOVZX:
            case MOp::MOVSX: {
                /* MOVZX/MOVSX dst64, src_mem<width>.  Encoding:
                 *   MOVZX r64, r/m8:  REX.W + 0F B6 /r
                 *   MOVZX r64, r/m16: REX.W + 0F B7 /r
                 *   MOVSX r64, r/m8:  REX.W + 0F BE /r
                 *   MOVSX r64, r/m16: REX.W + 0F BF /r
                 *   MOVSX r64, r/m32: REX.W + 63 /r    (MOVSXD)
                 *
                 * src.flags determina el ancho del operando fuente
                 * (1/2/4 bytes).  Para src REG, usa src.width.
                 * dst es siempre 64-bit. */
                if (mi.dst.kind != MOperandKind::REG) {
                    put8(out, 0xCC);
                    return true;
                }
                const uint8_t src_width = (mi.src1.kind == MOperandKind::MEM)
                                            ? mi.src1.flags
                                            : mi.src1.width;
                /* Determine opcode bytes. */
                uint8_t op_byte1 = 0x0F;
                uint8_t op_byte2 = 0;
                bool single_byte = false;  /* true para MOVSXD (sin 0F) */
                if (mi.op == MOp::MOVZX) {
                    if (src_width == 1) op_byte2 = 0xB6;
                    else if (src_width == 2) op_byte2 = 0xB7;
                    else { put8(out, 0xCC); return true; }
                } else {  /* MOVSX */
                    if (src_width == 1) op_byte2 = 0xBE;
                    else if (src_width == 2) op_byte2 = 0xBF;
                    else if (src_width == 4) { single_byte = true; }
                    else { put8(out, 0xCC); return true; }
                }
                if (mi.src1.kind == MOperandKind::REG) {
                    const uint8_t rex = rex_byte(true, mi.dst.reg, mi.src1.reg);
                    if (rex) put8(out, rex);
                    if (single_byte) {
                        put8(out, 0x63);
                    } else {
                        put8(out, op_byte1);
                        put8(out, op_byte2);
                    }
                    put8(out, modrm(3, mi.dst.reg & 7, mi.src1.reg & 7));
                } else if (mi.src1.kind == MOperandKind::MEM) {
                    const uint8_t base  = mi.src1.reg;
                    const uint8_t index = static_cast<uint8_t>(mi.src1.mem_index());
                    const bool has_index = (index != static_cast<uint8_t>(MReg::NONE));
                    const uint8_t rex = rex_byte(true, mi.dst.reg, base,
                                                  has_index ? index : 0);
                    if (rex) put8(out, rex);
                    if (single_byte) {
                        put8(out, 0x63);
                    } else {
                        put8(out, op_byte1);
                        put8(out, op_byte2);
                    }
                    emit_modrm_mem(mi.src1, mi.dst.reg & 7, out);
                } else {
                    put8(out, 0xCC);
                }
                return true;
            }
            case MOp::NEG:        emit_unary_alu(fn, mi, out, 3); return true;
            case MOp::NOT:        emit_unary_alu(fn, mi, out, 2); return true;
            case MOp::INC: {
                /* INC r64: REX.W + 0xFF /0.  3 bytes total. */
                const MOperand &dst = mi.dst;
                if (dst.kind != MOperandKind::REG) { put8(out, 0xCC); return true; }
                const uint8_t rex = rex_byte(true, 0, dst.reg);
                if (rex) put8(out, rex);
                put8(out, 0xFF);
                put8(out, modrm(3, 0, dst.reg & 7));
                return true;
            }
            case MOp::DEC: {
                /* DEC r64: REX.W + 0xFF /1.  3 bytes total. */
                const MOperand &dst = mi.dst;
                if (dst.kind != MOperandKind::REG) { put8(out, 0xCC); return true; }
                const uint8_t rex = rex_byte(true, 0, dst.reg);
                if (rex) put8(out, rex);
                put8(out, 0xFF);
                put8(out, modrm(3, 1, dst.reg & 7));
                return true;
            }
            case MOp::SETCC:      emit_setcc(fn, mi, out); return true;
            case MOp::CMOVCC:     emit_cmovcc(fn, mi, out); return true;
            case MOp::JMP:        emit_jmp(fn, mi, out); return true;
            case MOp::JCC:        emit_jcc(fn, mi, out); return true;
            case MOp::CALL: {
                /* si el CALL tiene stackmap asociado (flags
                 * != UINT16_MAX), rellenar su pc_offset.  Esto permite
                 * que el GC walker encuentre el stackmap correcto
                 * cuando el callee triggera GC. */
                if (mi.flags != UINT16_MAX
                 && mi.flags < fn.stackmaps.size()) {
                    fn.stackmaps[mi.flags].pc_offset =
                        static_cast<uint32_t>(out.size());
                }
                emit_call(fn, mi, out);
                return true;
            }
            case MOp::LABEL_DEF:
                /* No emite bytes; label_offset ya se setea en encode().
                 * Sin embargo, si el LABEL_DEF aparece A MITAD del bloque
                 * (insertado por el selector dentro de un BB grande)
                 * necesitamos registrar el offset aqui tambien. */
                if (mi.src1.kind == MOperandKind::LABEL) {
                    const uint32_t id = static_cast<uint32_t>(mi.src1.value);
                    if (id < fn.label_offsets.size()) {
                        /* Solo si no esta seteado por encode().  Calcular
                         * el offset relativo al base de la funcion. */
                        if (fn.label_offsets[id] == UINT32_MAX) {
                            fn.label_offsets[id] = static_cast<uint32_t>(out.size());
                        }
                    }
                }
                return true;
            case MOp::COMMENT:
                /* skip en release */
                return true;
            case MOp::SAFEPOINT: {
                /* poll expansion: lee safepoint_flag desde [rbx+0],
                 * si != 0 salta al slow path que llama al handler.
                 *
                 * Layout:
                 *
                 *     80 7B 00 00          cmp byte [rbx+0], 0
                 *     74 0F                je  skip                ; rel8
                 *     48 89 DF             mov rdi, rbx            ; SysV: proc en rdi
                 *     48 B8 <imm64>        mov rax, handler_addr
                 *     FF D0                call rax
                 *   skip:
                 *
                 * Total: 4 + 2 + 3 + 10 + 2 = 21 bytes.
                 * En Win64 cambia: mov rcx, rbx (3 bytes).
                 */
                /* rellenar pc_offset del stackmap asociado.
                 * MInstr::flags lleva el indice del stackmap en mf.stackmaps.
                 * El pc_offset que escribimos es la direccion DEL POLL, no
                 * la del handler -- asi el GC encuentra el stackmap correcto
                 * cuando captura RIP dentro o despues del poll. */
                if (mi.flags != UINT16_MAX
                 && mi.flags < fn.stackmaps.size()) {
                    fn.stackmaps[mi.flags].pc_offset =
                        static_cast<uint32_t>(out.size());
                }
                /* cmp byte [rbx+0], 0  -- usamos disp8=0 explicito */
                put8(out, 0x80);  /* cmp r/m8, imm8 */
                put8(out, modrm(1, 7, 3));  /* mod=01 (disp8), reg=7 (subop), r/m=3 (rbx) */
                put8(out, 0x00);  /* disp8 = 0 (safepoint_flag offset) */
                put8(out, 0x00);  /* imm8 = 0 */
                /* je rel8 = 0x74, salto al final de la secuencia (skip).
                 * El slow path mide: mov rdi/rcx, rbx (3) + mov rax, imm64 (10) + call rax (2) = 15.
                 * rel8 = 15. */
#if defined(_WIN32)
                put8(out, 0x74);  /* je rel8 */
                put8(out, 15);
                /* mov rcx, rbx (Win64 ABI: proc en rcx) */
                put8(out, 0x48);  /* REX.W */
                put8(out, 0x89);
                put8(out, modrm(3, 3, 1));  /* rcx <- rbx */
#else
                put8(out, 0x74);  /* je rel8 */
                put8(out, 15);
                /* mov rdi, rbx (SysV: proc en rdi) */
                put8(out, 0x48);  /* REX.W */
                put8(out, 0x89);
                put8(out, modrm(3, 3, 7));  /* rdi <- rbx */
#endif
                /* mov rax, imm64 = handler addr */
                {
                    const uint32_t pool_idx = static_cast<uint32_t>(mi.src1.value);
                    const uint64_t handler = (pool_idx < fn.imm64_pool.size())
                        ? fn.imm64_pool[pool_idx] : 0ULL;
                    put8(out, 0x48);  /* REX.W */
                    put8(out, 0xB8);  /* MOV RAX, imm64 */
                    put64(out, handler);
                }
                /* call rax */
                put8(out, 0xFF);
                put8(out, modrm(3, 2, 0));  /* /2 reg=rax */
                return true;
            }
            default:
                /* Opcode no implementado en este encoder. */
                return false;
        }
    }

    /* ===================================================================== */
    /* MOV (formas: r/r, r/imm32, r/imm64, r/m, m/r)                          */
    /* ===================================================================== */

    void X86Encoder::emit_mov(MFunction &fn, const MInstr &mi,
                              std::vector<uint8_t> &out) {
        const MOperand &dst = mi.dst;
        const MOperand &src = mi.src1;

        /* Caso 1: MOV reg, reg */
        if (dst.kind == MOperandKind::REG && src.kind == MOperandKind::REG) {
            const uint8_t rex = rex_byte(true, src.reg, dst.reg);
            if (rex) put8(out, rex);
            put8(out, 0x89);  /* MOV r/m64, r64 */
            put8(out, modrm(3, src.reg & 7, dst.reg & 7));
            return;
        }

        /* Caso 2: MOV reg, imm32 (sign-extended -> reg64) */
        if (dst.kind == MOperandKind::REG && src.kind == MOperandKind::IMM32) {
            /* Optimizacion: si valor cabe en 32-bit y no necesitamos
             * sign-ext especifico, usar MOV r32, imm32 (no REX.W).
             * Pero como la mayoria del codigo VM usa i64, emitimos REX.W
             * + 0xC7 /0 + imm32 (sign-extended a 64). */
            const uint8_t rex = rex_byte(true, 0, dst.reg);
            if (rex) put8(out, rex);
            put8(out, 0xC7);
            put8(out, modrm(3, 0, dst.reg & 7));
            put32(out, static_cast<uint32_t>(src.value));
            return;
        }

        /* Caso 3: MOV reg, imm64 (via pool) */
        if (dst.kind == MOperandKind::REG && src.kind == MOperandKind::IMM64_IDX) {
            const uint32_t idx = static_cast<uint32_t>(src.value);
            const uint64_t v64 = (idx < fn.imm64_pool.size()) ? fn.imm64_pool[idx] : 0;
            /* MOV r64, imm64: REX.W + B8+r + imm64 */
            put8(out, rex_byte(true, 0, dst.reg));
            put8(out, 0xB8 + (dst.reg & 7));
            put64(out, v64);
            return;
        }

        /* Caso 4: MOV reg, [mem].  Ancho controlado por dst.width:
         *   8 -> mov r64, [mem]  (REX.W + 0x8B)  -- qword load
         *   4 -> mov r32, [mem]  (sin REX.W + 0x8B) -- dword load, zero-extend
         *   2 -> mov r16, [mem]  (66 prefix + 0x8B)
         *   1 -> mov r8,  [mem]  (0x8A) */
        if (dst.kind == MOperandKind::REG && src.kind == MOperandKind::MEM) {
            const uint8_t base  = src.reg;
            const uint8_t index = static_cast<uint8_t>(src.mem_index());
            const bool has_index = (index != static_cast<uint8_t>(MReg::NONE));
            const uint8_t w = dst.width;
            if (w == 2) put8(out, 0x66);  /* 16-bit override */
            const bool need_rex_w = (w == 8);
            const uint8_t rex = rex_byte(need_rex_w, dst.reg, base,
                                          has_index ? index : 0);
            if (rex) put8(out, rex);
            put8(out, (w == 1) ? 0x8A : 0x8B);
            emit_modrm_mem(src, dst.reg & 7, out);
            return;
        }

        /* Caso 5: MOV [mem], reg.  Ancho via src.width. */
        if (dst.kind == MOperandKind::MEM && src.kind == MOperandKind::REG) {
            const uint8_t base  = dst.reg;
            const uint8_t index = static_cast<uint8_t>(dst.mem_index());
            const bool has_index = (index != static_cast<uint8_t>(MReg::NONE));
            const uint8_t w = src.width;
            if (w == 2) put8(out, 0x66);
            const bool need_rex_w = (w == 8);
            const uint8_t rex = rex_byte(need_rex_w, src.reg, base,
                                          has_index ? index : 0);
            if (rex) put8(out, rex);
            put8(out, (w == 1) ? 0x88 : 0x89);
            emit_modrm_mem(dst, src.reg & 7, out);
            return;
        }

        /* Caso 6: MOV [mem], imm32 (sign-extended) */
        if (dst.kind == MOperandKind::MEM && src.kind == MOperandKind::IMM32) {
            const uint8_t base  = dst.reg;
            const uint8_t index = static_cast<uint8_t>(dst.mem_index());
            const bool has_index = (index != static_cast<uint8_t>(MReg::NONE));
            const uint8_t rex = rex_byte(true, 0, base, has_index ? index : 0);
            if (rex) put8(out, rex);
            put8(out, 0xC7);  /* MOV r/m64, imm32 (sign-ext) */
            emit_modrm_mem(dst, 0, out);
            put32(out, static_cast<uint32_t>(src.value));
            return;
        }

        /* Cualquier otra combinacion no soportada -> INT3 para fail-fast. */
        put8(out, 0xCC);
    }

    /* ===================================================================== */
    /* LEA reg, [mem]                                                         */
    /* ===================================================================== */

    void X86Encoder::emit_lea(MFunction & /*fn*/, const MInstr &mi,
                              std::vector<uint8_t> &out) {
        const MOperand &dst = mi.dst;
        const MOperand &src = mi.src1;
        if (dst.kind != MOperandKind::REG || src.kind != MOperandKind::MEM) {
            put8(out, 0xCC);
            return;
        }
        const uint8_t base  = src.reg;
        const uint8_t index = static_cast<uint8_t>(src.mem_index());
        const bool has_index = (index != static_cast<uint8_t>(MReg::NONE));
        const uint8_t rex = rex_byte(true, dst.reg, base, has_index ? index : 0);
        if (rex) put8(out, rex);
        put8(out, 0x8D);  /* LEA r64, m */
        emit_modrm_mem(src, dst.reg & 7, out);
    }

    /* ===================================================================== */
    /* ALU binarias (ADD/SUB/AND/OR/XOR/CMP)                                  */
    /* ===================================================================== */

    void X86Encoder::emit_alu(MFunction & /*fn*/, const MInstr &mi,
                              std::vector<uint8_t> &out,
                              uint8_t op_byte, uint8_t alu_subop) {
        const MOperand &dst = mi.dst;
        const MOperand &src = mi.src1;

        /* ALU reg/reg: REX.W + op_byte + ModR/M(3, src, dst) */
        if (dst.kind == MOperandKind::REG && src.kind == MOperandKind::REG) {
            const uint8_t rex = rex_byte(true, src.reg, dst.reg);
            if (rex) put8(out, rex);
            put8(out, op_byte);
            put8(out, modrm(3, src.reg & 7, dst.reg & 7));
            return;
        }
        /* ALU reg/imm32: REX.W + 0x81 /subop + imm32 (sign-ext) */
        if (dst.kind == MOperandKind::REG && src.kind == MOperandKind::IMM32) {
            const uint8_t rex = rex_byte(true, 0, dst.reg);
            if (rex) put8(out, rex);
            /* Optimizacion: si imm32 cabe en imm8 sign-ext, usar 0x83
             * que reduce 3 bytes.  Encoding: REX.W + 0x83 /subop + imm8. */
            if (src.value >= -128 && src.value <= 127) {
                put8(out, 0x83);
                put8(out, modrm(3, alu_subop, dst.reg & 7));
                put8(out, static_cast<uint8_t>(src.value & 0xFF));
            } else {
                put8(out, 0x81);
                put8(out, modrm(3, alu_subop, dst.reg & 7));
                put32(out, static_cast<uint32_t>(src.value));
            }
            return;
        }
        /* ALU reg/mem: REX.W + (op_byte+2) + ModR/M */
        if (dst.kind == MOperandKind::REG && src.kind == MOperandKind::MEM) {
            const uint8_t base  = src.reg;
            const uint8_t index = static_cast<uint8_t>(src.mem_index());
            const bool has_index = (index != static_cast<uint8_t>(MReg::NONE));
            const uint8_t rex = rex_byte(true, dst.reg, base, has_index ? index : 0);
            if (rex) put8(out, rex);
            put8(out, op_byte + 2);  /* dir bit flipped */
            emit_modrm_mem(src, dst.reg & 7, out);
            return;
        }
        /* ALU mem/reg: REX.W + op_byte + ModR/M */
        if (dst.kind == MOperandKind::MEM && src.kind == MOperandKind::REG) {
            const uint8_t base  = dst.reg;
            const uint8_t index = static_cast<uint8_t>(dst.mem_index());
            const bool has_index = (index != static_cast<uint8_t>(MReg::NONE));
            const uint8_t rex = rex_byte(true, src.reg, base, has_index ? index : 0);
            if (rex) put8(out, rex);
            put8(out, op_byte);
            emit_modrm_mem(dst, src.reg & 7, out);
            return;
        }
        /* ALU mem/imm32: REX.W + 0x81 /subop + ModR/M + imm32.
         * Forma optimizada con imm8 sign-ext: REX.W + 0x83 /subop + ModR/M + imm8.
         * Usado por @c add qword [&counter], N en el JIT MIPS profiler. */
        if (dst.kind == MOperandKind::MEM && src.kind == MOperandKind::IMM32) {
            const uint8_t base  = dst.reg;
            const uint8_t index = static_cast<uint8_t>(dst.mem_index());
            const bool has_index = (index != static_cast<uint8_t>(MReg::NONE));
            const uint8_t rex = rex_byte(true, 0, base, has_index ? index : 0);
            if (rex) put8(out, rex);
            if (src.value >= -128 && src.value <= 127) {
                put8(out, 0x83);
                emit_modrm_mem(dst, alu_subop, out);
                put8(out, static_cast<uint8_t>(src.value & 0xFF));
            } else {
                put8(out, 0x81);
                emit_modrm_mem(dst, alu_subop, out);
                put32(out, static_cast<uint32_t>(src.value));
            }
            return;
        }
        put8(out, 0xCC);
    }

    /* ===================================================================== */
    /* IMUL reg, reg / reg, reg, imm32                                        */
    /* ===================================================================== */

    void X86Encoder::emit_imul(MFunction & /*fn*/, const MInstr &mi,
                               std::vector<uint8_t> &out) {
        const MOperand &dst = mi.dst;
        const MOperand &src = mi.src1;
        const MOperand &src2 = mi.src2;
        /* Forma 1: IMUL reg, reg -> REX.W + 0x0F 0xAF /r */
        if (dst.kind == MOperandKind::REG && src.kind == MOperandKind::REG
         && src2.kind == MOperandKind::NONE) {
            const uint8_t rex = rex_byte(true, dst.reg, src.reg);
            if (rex) put8(out, rex);
            put8(out, 0x0F);
            put8(out, 0xAF);
            put8(out, modrm(3, dst.reg & 7, src.reg & 7));
            return;
        }
        /* Forma 2: IMUL reg, reg, imm32 -> REX.W + 0x69 /r + imm32 */
        if (dst.kind == MOperandKind::REG && src.kind == MOperandKind::REG
         && src2.kind == MOperandKind::IMM32) {
            const uint8_t rex = rex_byte(true, dst.reg, src.reg);
            if (rex) put8(out, rex);
            /* imm8 si cabe (0x6B), imm32 si no (0x69) */
            if (src2.value >= -128 && src2.value <= 127) {
                put8(out, 0x6B);
                put8(out, modrm(3, dst.reg & 7, src.reg & 7));
                put8(out, static_cast<uint8_t>(src2.value & 0xFF));
            } else {
                put8(out, 0x69);
                put8(out, modrm(3, dst.reg & 7, src.reg & 7));
                put32(out, static_cast<uint32_t>(src2.value));
            }
            return;
        }
        put8(out, 0xCC);
    }

    /* ===================================================================== */
    /* SHL/SHR/SAR reg, imm8                                                  */
    /* ===================================================================== */

    void X86Encoder::emit_shift(MFunction & /*fn*/, const MInstr &mi,
                                std::vector<uint8_t> &out,
                                uint8_t shift_subop) {
        const MOperand &dst = mi.dst;
        const MOperand &src = mi.src1;
        if (dst.kind != MOperandKind::REG || src.kind != MOperandKind::IMM32) {
            put8(out, 0xCC);
            return;
        }
        const uint8_t rex = rex_byte(true, 0, dst.reg);
        if (rex) put8(out, rex);
        if ((src.value & 0xFF) == 1) {
            /* Variante por 1: REX.W + 0xD1 /subop -- mas compacta */
            put8(out, 0xD1);
            put8(out, modrm(3, shift_subop, dst.reg & 7));
        } else {
            /* Variante imm8: REX.W + 0xC1 /subop + imm8 */
            put8(out, 0xC1);
            put8(out, modrm(3, shift_subop, dst.reg & 7));
            put8(out, static_cast<uint8_t>(src.value & 0xFF));
        }
    }

    /* ===================================================================== */
    /* NEG/NOT reg                                                            */
    /* ===================================================================== */

    void X86Encoder::emit_unary_alu(MFunction & /*fn*/, const MInstr &mi,
                                    std::vector<uint8_t> &out, uint8_t subop) {
        const MOperand &dst = mi.dst;
        if (dst.kind != MOperandKind::REG) { put8(out, 0xCC); return; }
        const uint8_t rex = rex_byte(true, 0, dst.reg);
        if (rex) put8(out, rex);
        put8(out, 0xF7);
        put8(out, modrm(3, subop, dst.reg & 7));
    }

    /* ===================================================================== */
    /* CMP / TEST                                                             */
    /* ===================================================================== */

    void X86Encoder::emit_cmp(MFunction &fn, const MInstr &mi,
                              std::vector<uint8_t> &out) {
        /* CMP usa la misma tabla que ADD/SUB/etc.  El alu_subop=7 y
         * op_byte=0x39 estan en la tabla bajo MOp::CMP. */
        emit_alu(fn, mi, out, 0x39, 7);
    }

    void X86Encoder::emit_test(MFunction & /*fn*/, const MInstr &mi,
                               std::vector<uint8_t> &out) {
        const MOperand &dst = mi.dst;
        const MOperand &src = mi.src1;
        /* TEST reg, reg: REX.W + 0x85 /r */
        if (dst.kind == MOperandKind::REG && src.kind == MOperandKind::REG) {
            const uint8_t rex = rex_byte(true, src.reg, dst.reg);
            if (rex) put8(out, rex);
            put8(out, 0x85);
            put8(out, modrm(3, src.reg & 7, dst.reg & 7));
            return;
        }
        /* TEST reg, imm32: REX.W + 0xF7 /0 + imm32 */
        if (dst.kind == MOperandKind::REG && src.kind == MOperandKind::IMM32) {
            const uint8_t rex = rex_byte(true, 0, dst.reg);
            if (rex) put8(out, rex);
            put8(out, 0xF7);
            put8(out, modrm(3, 0, dst.reg & 7));
            put32(out, static_cast<uint32_t>(src.value));
            return;
        }
        put8(out, 0xCC);
    }

    /* ===================================================================== */
    /* SETcc / CMOVcc                                                          */
    /* ===================================================================== */

    void X86Encoder::emit_setcc(MFunction & /*fn*/, const MInstr &mi,
                                std::vector<uint8_t> &out) {
        const MOperand &dst = mi.dst;
        const MCond cc = static_cast<MCond>(mi.variant);
        if (dst.kind != MOperandKind::REG) { put8(out, 0xCC); return; }
        /* SETcc r/m8: 0x0F 0x90+cc /0 -- NO REX.W (es 8-bit). */
        const uint8_t rex = rex_byte(false, 0, dst.reg);
        if (rex) put8(out, rex);
        put8(out, 0x0F);
        put8(out, 0x90 + static_cast<uint8_t>(cc));
        put8(out, modrm(3, 0, dst.reg & 7));
    }

    void X86Encoder::emit_cmovcc(MFunction & /*fn*/, const MInstr &mi,
                                 std::vector<uint8_t> &out) {
        const MOperand &dst = mi.dst;
        const MOperand &src = mi.src1;
        const MCond cc = static_cast<MCond>(mi.variant);
        if (dst.kind != MOperandKind::REG || src.kind != MOperandKind::REG) {
            put8(out, 0xCC);
            return;
        }
        /* CMOVcc r64, r/m64: REX.W + 0x0F 0x40+cc /r */
        const uint8_t rex = rex_byte(true, dst.reg, src.reg);
        if (rex) put8(out, rex);
        put8(out, 0x0F);
        put8(out, 0x40 + static_cast<uint8_t>(cc));
        put8(out, modrm(3, dst.reg & 7, src.reg & 7));
    }

    /* ===================================================================== */
    /* JMP / Jcc / CALL (rel32)                                                */
    /* ===================================================================== */

    void X86Encoder::emit_jmp(MFunction &fn, const MInstr &mi,
                              std::vector<uint8_t> &out) {
        if (mi.src1.kind == MOperandKind::LABEL) {
            put8(out, 0xE9);
            const uint32_t patch_at = static_cast<uint32_t>(out.size());
            put32(out, 0);  /* placeholder rel32 */
            const uint32_t instr_end = static_cast<uint32_t>(out.size());
            fn.fixups.push_back(MFixup{
                static_cast<MLabelId>(mi.src1.value),
                patch_at, instr_end, 4
            });
            return;
        }
        /* JMP reg/mem absoluto no soportado en v1. */
        put8(out, 0xCC);
    }

    void X86Encoder::emit_jcc(MFunction &fn, const MInstr &mi,
                              std::vector<uint8_t> &out) {
        if (mi.src1.kind != MOperandKind::LABEL) {
            put8(out, 0xCC);
            return;
        }
        const MCond cc = static_cast<MCond>(mi.variant);
        put8(out, 0x0F);
        put8(out, jcc_long_opcode(cc));
        const uint32_t patch_at = static_cast<uint32_t>(out.size());
        put32(out, 0);
        const uint32_t instr_end = static_cast<uint32_t>(out.size());
        fn.fixups.push_back(MFixup{
            static_cast<MLabelId>(mi.src1.value),
            patch_at, instr_end, 4
        });
    }

    void X86Encoder::emit_call(MFunction &fn, const MInstr &mi,
                               std::vector<uint8_t> &out) {
        if (mi.src1.kind == MOperandKind::LABEL) {
            put8(out, 0xE8);
            const uint32_t patch_at = static_cast<uint32_t>(out.size());
            put32(out, 0);
            const uint32_t instr_end = static_cast<uint32_t>(out.size());
            fn.fixups.push_back(MFixup{
                static_cast<MLabelId>(mi.src1.value),
                patch_at, instr_end, 4
            });
            return;
        }
        /* CALL reg/mem: FF /2 */
        if (mi.src1.kind == MOperandKind::REG) {
            const uint8_t rex = rex_byte(false, 0, mi.src1.reg);
            if (rex) put8(out, rex);
            put8(out, 0xFF);
            put8(out, modrm(3, 2, mi.src1.reg & 7));
            return;
        }
        if (mi.src1.kind == MOperandKind::MEM) {
            const uint8_t base  = mi.src1.reg;
            const uint8_t index = static_cast<uint8_t>(mi.src1.mem_index());
            const bool has_index = (index != static_cast<uint8_t>(MReg::NONE));
            const uint8_t rex = rex_byte(false, 0, base, has_index ? index : 0);
            if (rex) put8(out, rex);
            put8(out, 0xFF);
            emit_modrm_mem(mi.src1, 2, out);
            return;
        }
        put8(out, 0xCC);
    }

    /* ===================================================================== */
    /* RET / PUSH / POP                                                       */
    /* ===================================================================== */

    void X86Encoder::emit_ret(std::vector<uint8_t> &out) {
        put8(out, 0xC3);
    }

    void X86Encoder::emit_push(const MInstr &mi, std::vector<uint8_t> &out) {
        if (mi.src1.kind == MOperandKind::REG) {
            const uint8_t r = mi.src1.reg;
            if (r & 0x8) put8(out, 0x41);  /* REX.B para R8..R15 */
            put8(out, 0x50 + (r & 7));
            return;
        }
        /* PUSH imm32: 0x68 + imm32 (sign-ext a 64) */
        if (mi.src1.kind == MOperandKind::IMM32) {
            if (mi.src1.value >= -128 && mi.src1.value <= 127) {
                put8(out, 0x6A);
                put8(out, static_cast<uint8_t>(mi.src1.value & 0xFF));
            } else {
                put8(out, 0x68);
                put32(out, static_cast<uint32_t>(mi.src1.value));
            }
            return;
        }
        put8(out, 0xCC);
    }

    void X86Encoder::emit_pop(const MInstr &mi, std::vector<uint8_t> &out) {
        if (mi.dst.kind == MOperandKind::REG) {
            const uint8_t r = mi.dst.reg;
            if (r & 0x8) put8(out, 0x41);
            put8(out, 0x58 + (r & 7));
            return;
        }
        put8(out, 0xCC);
    }

    /* ===================================================================== */
    /* emit_modrm_mem (ModR/M + SIB? + disp para operandos MEM)                */
    /* ===================================================================== */

    void X86Encoder::emit_modrm_mem(const MOperand &mem, uint8_t reg_field,
                                    std::vector<uint8_t> &out) {
        const uint8_t base   = mem.reg;
        const MReg    idx    = mem.mem_index();
        const uint8_t scale  = mem.mem_scale();
        const int32_t disp   = mem.mem_disp();
        const bool    has_index = (idx != MReg::NONE);

        /* scale_bits: 1->0, 2->1, 4->2, 8->3 */
        uint8_t scale_bits = 0;
        switch (scale) {
            case 1: scale_bits = 0; break;
            case 2: scale_bits = 1; break;
            case 4: scale_bits = 2; break;
            case 8: scale_bits = 3; break;
        }

        const uint8_t base_3 = base & 7;
        const uint8_t reg_3  = reg_field & 7;

        /* Decidir mod segun disp + casos especiales:
         *   mod=00 si disp==0 y base != RBP (5) y base != R13 (13)
         *   mod=01 si disp cabe en int8
         *   mod=10 si disp es int32 */
        uint8_t mod = 0;
        bool emit_disp8 = false;
        bool emit_disp32 = false;
        if (disp == 0 && base_3 != 5) {
            mod = 0;
        } else if (disp >= -128 && disp <= 127) {
            mod = 1;
            emit_disp8 = true;
        } else {
            mod = 2;
            emit_disp32 = true;
        }

        /* Si hay index O si base es RSP (4) o R12 (12 & 7 == 4), debemos
         * usar SIB. */
        const bool needs_sib = has_index || base_3 == 4;

        if (needs_sib) {
            put8(out, modrm(mod, reg_3, 4));  /* r/m = 4 -> SIB sigue */
            const uint8_t idx_3 = has_index ? (static_cast<uint8_t>(idx) & 7) : 4;
            /* SIB con index=4 = no-index */
            put8(out, sib(scale_bits, idx_3, base_3));
        } else {
            put8(out, modrm(mod, reg_3, base_3));
        }

        if (emit_disp8) {
            put8(out, static_cast<uint8_t>(disp & 0xFF));
        } else if (emit_disp32) {
            put32(out, static_cast<uint32_t>(disp));
        } else if (mod == 0 && base_3 == 5) {
            /* base = RBP / R13 -> disp32 forzado por encoding */
            put32(out, 0);
        }
    }

    /* ===================================================================== */
    /* resolve_fixups                                                          */
    /* ===================================================================== */

    void X86Encoder::resolve_fixups(MFunction &fn, std::vector<uint8_t> &out,
                                    size_t base) {
        for (const auto &fx : fn.fixups) {
            if (fx.label_id >= fn.label_offsets.size()) continue;
            const uint32_t target_off = fn.label_offsets[fx.label_id];
            if (target_off == UINT32_MAX) continue;  /* unresolved (bug) */
            const int64_t rel = static_cast<int64_t>(target_off)
                              - static_cast<int64_t>(fx.instr_end);
            /* Comprobar rango int32. */
            if (rel < INT32_MIN || rel > INT32_MAX) continue;
            const uint32_t rel32 = static_cast<uint32_t>(static_cast<int32_t>(rel));
            const size_t at = base + fx.patch_at;
            out[at]     = static_cast<uint8_t>(rel32);
            out[at + 1] = static_cast<uint8_t>(rel32 >> 8);
            out[at + 2] = static_cast<uint8_t>(rel32 >> 16);
            out[at + 3] = static_cast<uint8_t>(rel32 >> 24);
        }
    }

} // namespace jit
