/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file vx/asm/asm_phys_reg.h
 * @brief Nombres de registro FiSICO por (clase, indice, ancho) + sustitucion de
 *        placeholders @c $N de una @c ASM_MICRO.
 *
 * Header-only: lo comparten el LIFTER (vx_lib: nombre -> indice fisico) y el
 * BACKEND JIT/AOT (vm/vesta_ffi: indice fisico -> nombre, sustituye @c $N en la
 * plantilla NASM) sin dependencia de enlace entre libs.  Cubre x86 GP
 * (rax..r15 en ORDEN DE ENCODING, igual que @c MReg y el encoder); las demas
 * clases (FP/VEC) y arch (arm64) llegan despues.
 */
#ifndef VEX_ASM_PHYS_REG_H
#define VEX_ASM_PHYS_REG_H

#include "ir/ssa_ir.h"

#include <cctype>
#include <cstdint>
#include <string>

namespace vex {

/// Clases de registro arch-neutras (== @c AsmMicroOperand::regclass).
enum : uint8_t {
    ASM_RC_GP = 0,
    ASM_RC_FP = 1,
    ASM_RC_VEC = 2,
    ASM_RC_PRED = 3,
    ASM_RC_FLAGS = 4,
};

/**
 * @brief Nombre del GPR x86 @p phys (0..15, ORDEN DE ENCODING) al ancho
 *        @p width_bits (64/32/16/8).  @c nullptr si fuera de rango.
 */
inline const char *asm_x86_gp_name(int phys, uint16_t width_bits) {
    if (phys < 0 || phys > 15) return nullptr;
    static const char *k64[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp",
                                  "rsi", "rdi", "r8",  "r9",  "r10", "r11",
                                  "r12", "r13", "r14", "r15"};
    static const char *k32[16] = {"eax",  "ecx",  "edx",  "ebx", "esp", "ebp",
                                  "esi",  "edi",  "r8d",  "r9d", "r10d", "r11d",
                                  "r12d", "r13d", "r14d", "r15d"};
    static const char *k16[16] = {"ax",   "cx",   "dx",   "bx",  "sp",  "bp",
                                  "si",   "di",   "r8w",  "r9w", "r10w", "r11w",
                                  "r12w", "r13w", "r14w", "r15w"};
    static const char *k8[16] = {"al",   "cl",   "dl",   "bl",  "spl", "bpl",
                                 "sil",  "dil",  "r8b",  "r9b", "r10b", "r11b",
                                 "r12b", "r13b", "r14b", "r15b"};
    switch (width_bits) {
    case 64: return k64[phys];
    case 32: return k32[phys];
    case 16: return k16[phys];
    case 8: return k8[phys];
    default: return k64[phys]; // 0/desconocido -> 64-bit (caso comun)
    }
}

/**
 * @brief indice fisico (0..15, orden de encoding) de un nombre de GPR x86.
 *        Rellena @p out_width con el ancho en bits.  -1 si no es un GPR.
 */
inline int asm_x86_gp_index(const std::string &tok, uint16_t *out_width) {
    std::string s;
    s.reserve(tok.size());
    for (char c : tok)
        s += static_cast<char>(std::tolower((unsigned char)c));
    for (uint16_t w : {(uint16_t)64, (uint16_t)32, (uint16_t)16, (uint16_t)8})
        for (int i = 0; i < 16; ++i)
            if (s == asm_x86_gp_name(i, w)) {
                if (out_width) *out_width = w;
                return i;
            }
    return -1;
}

/**
 * @brief Nombre de un operando REG por su (clase, indice fisico, ancho).  @c ""
 *        si la clase no esta soportada o el operando no es fisico.
 */
inline std::string asm_phys_reg_name(uint8_t isa, uint8_t regclass, int phys,
                                     uint16_t width_bits) {
    // x86 (isa 0/1/2) GP.  FP/VEC/arm64 pendientes.
    if (regclass == ASM_RC_GP && isa <= 2) {
        const char *n = asm_x86_gp_name(phys, width_bits);
        return n ? std::string(n) : std::string();
    }
    return std::string();
}

/**
 * @brief Sustituye @c $0,$1,... de @c am.tmpl por el nombre fisico de cada
 *        operando (indexado por posicion en @c am.operands).  Devuelve @c false
 *        si algun @c $N referenciado no es nombrable (operando no fisico o clase
 *        no soportada) -> el llamador hace fallback.
 *
 * todos los operandos son de FiSICO FIJO (@c fixed_phys >= 0).  El
 * threading SSA + asignador (@c fixed_phys == -1) llega despues.
 */
inline bool asm_micro_subst_phys(const ir::AsmMicro &am, std::string &out) {
    out.clear();
    out.reserve(am.tmpl.size() + 16);
    for (size_t i = 0; i < am.tmpl.size();) {
        char c = am.tmpl[i];
        if (c != '$') {
            out += c;
            ++i;
            continue;
        }
        // Leer el indice decimal tras '$'.
        size_t j = i + 1;
        uint32_t idx = 0;
        bool any = false;
        while (j < am.tmpl.size() && std::isdigit((unsigned char)am.tmpl[j])) {
            idx = idx * 10 + (uint32_t)(am.tmpl[j] - '0');
            ++j;
            any = true;
        }
        if (!any || idx >= am.operands.size()) return false; // $ suelto / fuera
        const ir::AsmMicroOperand &op = am.operands[idx];
        if (op.kind != ir::AsmOperandKind::REG || op.fixed_phys < 0)
            return false; // MEM/IMM o no fisico
        std::string name =
            asm_phys_reg_name(am.isa, op.regclass, op.fixed_phys, op.width);
        if (name.empty()) return false;
        out += name;
        i = j;
    }
    return true;
}

/**
 * @brief Sustituye @c $0,$1,... por el registro GREEDY (por defecto) del binding
 *        @c reg_auto con ese @c ph_index.  Para el INTERP, que no tiene RA: usa
 *        el pick greedy guardado en @c AsmRegBinding::reg (nombre de 64 bits).
 *        Los @c $N sin binding correspondiente quedan verbatim.
 *
 * El JIT/AOT NO usan esto: rellenan @c $N con el registro OPTIMO del asignador
 * via el ensamblado diferido (@c AsmBlob::deferred).  Este helper mantiene el
 * interp correcto (aunque no optimo) con el MISMO cuerpo $N.
 */
inline std::string asm_body_subst_greedy(
    const std::string &body, const std::vector<ir::AsmRegBinding> &binds) {
    std::string out;
    out.reserve(body.size() + 16);
    for (size_t i = 0; i < body.size();) {
        if (body[i] != '$') { out += body[i++]; continue; }
        size_t j = i + 1;
        int idx = 0;
        bool any = false;
        while (j < body.size() &&
               std::isdigit((unsigned char)body[j])) {
            idx = idx * 10 + (body[j] - '0');
            ++j;
            any = true;
        }
        if (!any) { out += body[i++]; continue; }
        std::string reg;
        for (const ir::AsmRegBinding &b : binds)
            if (b.reg_auto && b.ph_index == idx) { reg = b.reg; break; }
        if (reg.empty()) { out += body[i]; ++i; continue; } // $N sin binding
        out += reg;
        i = j;
    }
    return out;
}

} // namespace vex

#endif // VEX_ASM_PHYS_REG_H
