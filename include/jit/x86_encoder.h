/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/x86_encoder.h
 * @brief Encoder x86-64 hand-rolled (sin Keystone).
 *
 * = Diseno =
 *
 * Cero deps externas.  El encoder lee @c MFunction y emite bytes
 * directamente a un @c std::vector<uint8_t>.  Coste medido: ~50 ns
 * por instruccion en hardware moderno (vs ~10 us con Keystone ks_asm).
 *
 * = Cobertura v1 =
 *
 * (C1 baseline) basta con:
 *   - MOV reg/reg, reg/imm32, reg/imm64, reg/mem, mem/reg
 *   - ADD/SUB/AND/OR/XOR reg/reg, reg/imm32, reg/mem
 *   - IMUL reg/reg
 *   - CMP reg/reg, reg/imm32
 *   - JMP rel32, Jcc rel32, CALL rel32
 *   - PUSH/POP reg
 *   - RET, INT3
 *   - SHL/SHR/SAR reg/imm8
 *   - LEA reg/mem
 *
 * Floats (XMM ops) y SIMD se anyaden en sub-hito separado cuando
 * lo necesite.  Mientras tanto el JIT cae al interprete para
 * cualquier op no soportada.
 *
 * = Encoding x86-64 =
 *
 * Formato general:
 *   [Legacy prefix(es)?] [REX] [opcode] [ModR/M] [SIB] [disp] [imm]
 *
 *   - REX: 0x40 + W (8 byte) + R (extiende reg) + X (extiende index) + B (extiende base)
 *     REX.W=1 -> operand size 64-bit
 *     REX.R=1 -> reg field extended (regs >= R8)
 *     REX.B=1 -> r/m field extended
 *     REX.X=1 -> SIB.index extended
 *
 *   - ModR/M: mod (2 bits) + reg (3 bits) + r/m (3 bits)
 *     mod = 00: [reg]               (excepto r/m=4 -> SIB, r/m=5 -> [rip+disp32])
 *     mod = 01: [reg + disp8]
 *     mod = 10: [reg + disp32]
 *     mod = 11: reg-reg directo
 *
 *   - SIB: scale (2 bits) + index (3 bits) + base (3 bits)
 *     Necesario cuando ModR/M.r/m = 4 (sirve como escape)
 *
 * = API =
 *
 * @c X86Encoder::encode(fn, out) escribe los bytes del @c MFunction
 * @p fn en @p out.  Devuelve el numero total de bytes emitidos.
 * Si encuentra un opcode no soportado, anyade un comentario y un
 * INT3 + retorna 0 (falla).  Tras la pasada principal, ejecuta
 * @c resolve_fixups que patchea los offsets relativos pendientes.
 */

#ifndef VESTA_JIT_X86_ENCODER_H
#define VESTA_JIT_X86_ENCODER_H

#include <cstdint>
#include <vector>

#include "jit/machine_ir.h"

namespace jit {

    /**
     * @class X86Encoder
     * @brief Emite bytes maquina x86-64 desde un @c MFunction.
     */
    class X86Encoder {
    public:
        X86Encoder() = default;

        /**
         * @brief Codifica @p fn en bytes y los anyade a @p out.
         * @return numero total de bytes emitidos para esta funcion, o 0
         *         si encontro un opcode no soportado (en cuyo caso
         *         @p out queda parcialmente escrito hasta el punto del
         *         error mas un INT3 para fail-fast).
         *
         * Tras emit, hace @c resolve_fixups() para patchear todas las
         * branches rel32 con offsets correctos.
         */
        size_t encode(MFunction &fn, std::vector<uint8_t> &out);

        /** @brief Numero de instrucciones emitidas en la ultima llamada a encode(). */
        size_t instr_count() const noexcept { return instr_count_; }

    private:
        /* Helpers de emit individuales.  Cada uno actualiza @c fn.fixups
         * cuando emite una branch relativa. */
        bool emit_instr(MFunction &fn, const MInstr &mi, std::vector<uint8_t> &out);

        void emit_mov(MFunction &fn, const MInstr &mi, std::vector<uint8_t> &out);
        void emit_lea(MFunction &fn, const MInstr &mi, std::vector<uint8_t> &out);
        void emit_alu(MFunction &fn, const MInstr &mi, std::vector<uint8_t> &out, uint8_t op_byte, uint8_t alu_subop);
        void emit_imul(MFunction &fn, const MInstr &mi, std::vector<uint8_t> &out);
        void emit_shift(MFunction &fn, const MInstr &mi, std::vector<uint8_t> &out, uint8_t shift_subop);
        void emit_unary_alu(MFunction &fn, const MInstr &mi, std::vector<uint8_t> &out, uint8_t subop);
        void emit_cmp(MFunction &fn, const MInstr &mi, std::vector<uint8_t> &out);
        void emit_test(MFunction &fn, const MInstr &mi, std::vector<uint8_t> &out);
        void emit_setcc(MFunction &fn, const MInstr &mi, std::vector<uint8_t> &out);
        void emit_cmovcc(MFunction &fn, const MInstr &mi, std::vector<uint8_t> &out);
        void emit_jmp(MFunction &fn, const MInstr &mi, std::vector<uint8_t> &out);
        void emit_jcc(MFunction &fn, const MInstr &mi, std::vector<uint8_t> &out);
        void emit_call(MFunction &fn, const MInstr &mi, std::vector<uint8_t> &out);
        void emit_ret(std::vector<uint8_t> &out);
        void emit_push(const MInstr &mi, std::vector<uint8_t> &out);
        void emit_pop(const MInstr &mi, std::vector<uint8_t> &out);

        /* Helpers de bajo nivel x86. */
        static void put8(std::vector<uint8_t> &o, uint8_t b)  { o.push_back(b); }
        static void put16(std::vector<uint8_t> &o, uint16_t v) {
            o.push_back(static_cast<uint8_t>(v));
            o.push_back(static_cast<uint8_t>(v >> 8));
        }
        static void put32(std::vector<uint8_t> &o, uint32_t v) {
            o.push_back(static_cast<uint8_t>(v));
            o.push_back(static_cast<uint8_t>(v >> 8));
            o.push_back(static_cast<uint8_t>(v >> 16));
            o.push_back(static_cast<uint8_t>(v >> 24));
        }
        static void put64(std::vector<uint8_t> &o, uint64_t v) {
            for (int i = 0; i < 8; ++i) o.push_back(static_cast<uint8_t>(v >> (i * 8)));
        }

        /// Calcula el byte REX para una combinacion @p rex_w (W bit) +
        /// @p reg (reg field, 3 LSB) + @p rm (r/m field) + @p index.
        /// @return 0 si REX no es necesario (todos los bits altos = 0
        ///         y rex_w = false); sino REX = 0x40 | mask.
        static uint8_t rex_byte(bool rex_w, uint8_t reg, uint8_t rm, uint8_t index = 0) {
            uint8_t r = (rex_w ? 0x48 : 0x40);
            if (reg   & 0x8) r |= 0x4;    /* REX.R */
            if (index & 0x8) r |= 0x2;    /* REX.X */
            if (rm    & 0x8) r |= 0x1;    /* REX.B */
            /* Emit REX si W es seteado O si cualquier bit alto esta */
            return (r == 0x40) ? 0 : r;
        }

        /// Construye un byte ModR/M con mod + reg + rm.
        static uint8_t modrm(uint8_t mod, uint8_t reg, uint8_t rm) {
            return static_cast<uint8_t>(((mod & 3) << 6) | ((reg & 7) << 3) | (rm & 7));
        }

        /// Construye un byte SIB con scale + index + base.
        static uint8_t sib(uint8_t scale_bits, uint8_t index, uint8_t base) {
            return static_cast<uint8_t>(((scale_bits & 3) << 6) | ((index & 7) << 3) | (base & 7));
        }

        /// Emite el grupo ModR/M + SIB + disp para un operando @c MEM.
        /// El parametro @p reg_field son los 3 LSB del reg que va en el
        /// campo reg del ModR/M (el otro operando del binary).  Esta
        /// funcion NO emite REX -- el caller lo debe haber emitido ya
        /// tras calcular los bits altos.
        void emit_modrm_mem(const MOperand &mem, uint8_t reg_field,
                            std::vector<uint8_t> &out);

        /// Resuelve todos los @c MFunction::fixups patcheando rel32 in-place.
        /// @c base es el offset del comienzo de @p fn dentro del buffer
        /// global (para soportar emit de varias funciones consecutivas).
        void resolve_fixups(MFunction &fn, std::vector<uint8_t> &out, size_t base);

        size_t instr_count_ = 0;
    };

} // namespace jit

#endif // VESTA_JIT_X86_ENCODER_H
