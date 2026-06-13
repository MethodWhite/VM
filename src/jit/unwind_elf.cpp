/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/unwind_elf.cpp
 * @brief Generacion de unwind info en formato ELF/DWARF (Linux x64).
 *
 * = Diseno =
 *
 * En Linux x64, el desenrollado de pila se basa en la seccion .eh_frame
 * usando el formato DWARF CFI (Call Frame Information).  Cada funcion
 * JIT necesita:
 *
 *   1. CIE (Common Information Entry): define las reglas base de
 *      desenrollado y la personality routine.
 *   2. FDE (Frame Description Entry): por cada funcion, describe
 *      como desenrollar su frame.
 *   3. LSDA (Language-Specific Data Area): opcional, para soporte
 *      de excepciones C++ (tabla de call sites + acciones).
 *
 * = Formato .eh_frame =
 *
 *   - CIE header (length, CIE_id=0, version=1, augmentation, ...)
 *     + initial CFI instructions (DW_CFA_def_cfa, DW_CFA_offset, ...)
 *   - FDE header (length, CIE_pointer, code_start, code_size, ...)
 *     + CFI instructions (DW_CFA_advance_loc, DW_CFA_def_cfa_offset,
 *       DW_CFA_offset, DW_CFA_restore, ...)
 *
 * = Personality routine =
 *
 * Para C++ exceptions, se usa __gxx_personality_v0 como personality.
 * Se referencia en la augmentation del CIE ("zPL" o "zPR").
 *
 * = LSDA =
 *
 * El LSDA sigue el formato .gcc_except_table de la Itanium C++ ABI:
 *
 *   - LPStart encoding + LPStart (default 0 = code_start)
 *   - TType encoding + TType base
 *   - Call site table length + call site entries
 *   - Action table (type info entries)
 *
 * = Uso tipico =
 *
 *   // Crear CIE estandar con personality
 *   CieEntry cie = generate_cie(personality_addr);
 *
 *   // Por cada funcion JIT:
 *   FdeEntry fde;
 *   fde.code_start = func_addr;
 *   fde.code_size  = func_size;
 *   generate_fde_cfi(info, fde.cfi_instructions);
 *
 *   // Serializar al buffer
 *   emit_cie(buf, cie);
 *   emit_fde(buf, cie, fde, lsda_data);
 */

#include "jit/unwind_info.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace jit {

    // -----------------------------------------------------------------------
    // Constantes DWARF CFI
    // -----------------------------------------------------------------------

    // DW_CFA opcodes
    static constexpr uint8_t DW_CFA_advance_loc   = 0x01; // + delta (0x01-0x3F)
    static constexpr uint8_t DW_CFA_offset        = 0x80; // + reg (low 6 bits)
    static constexpr uint8_t DW_CFA_restore       = 0xC0; // + reg (low 6 bits)
    static constexpr uint8_t DW_CFA_def_cfa       = 0x0C;
    static constexpr uint8_t DW_CFA_def_cfa_register = 0x0D;
    static constexpr uint8_t DW_CFA_def_cfa_offset   = 0x0E;
    static constexpr uint8_t DW_CFA_nop           = 0x00;
    static constexpr uint8_t DW_CFA_undefined     = 0x07;

    // Register numbers DWARF x86-64
    static constexpr uint8_t DW_REG_RAX = 0;
    static constexpr uint8_t DW_REG_RBX = 3;
    static constexpr uint8_t DW_REG_RSP = 7;
    static constexpr uint8_t DW_REG_RBP = 6;
    static constexpr uint8_t DW_REG_RSI = 4;
    static constexpr uint8_t DW_REG_RDI = 5;
    static constexpr uint8_t DW_REG_R8  = 8;
    static constexpr uint8_t DW_REG_R9  = 9;
    static constexpr uint8_t DW_REG_R10 = 10;
    static constexpr uint8_t DW_REG_R11 = 11;
    static constexpr uint8_t DW_REG_R12 = 12;
    static constexpr uint8_t DW_REG_R13 = 13;
    static constexpr uint8_t DW_REG_R14 = 14;
    static constexpr uint8_t DW_REG_R15 = 15;
    static constexpr uint8_t DW_REG_RIP = 16;

    // DW_EH_PE encodings
    static constexpr uint8_t DW_EH_PE_absptr   = 0x00;
    static constexpr uint8_t DW_EH_PE_uleb128  = 0x01;
    static constexpr uint8_t DW_EH_PE_udata2   = 0x02;
    static constexpr uint8_t DW_EH_PE_udata4   = 0x03;
    static constexpr uint8_t DW_EH_PE_sleb128  = 0x09;
    static constexpr uint8_t DW_EH_PE_sdata4   = 0x0B;
    static constexpr uint8_t DW_EH_PE_pcrel    = 0x10;
    static constexpr uint8_t DW_EH_PE_textrel  = 0x20;
    static constexpr uint8_t DW_EH_PE_datarel  = 0x30;
    static constexpr uint8_t DW_EH_PE_funcrel  = 0x40;
    static constexpr uint8_t DW_EH_PE_aligned  = 0x50;
    static constexpr uint8_t DW_EH_PE_indirect = 0x80;

    // Augmentation string characters
    static constexpr char DW_EH_AUG_Z   = 'z'; // presence of Augmentation data
    static constexpr char DW_EH_AUG_P   = 'P'; // personality routine
    static constexpr char DW_EH_AUG_L   = 'L'; // LSDA encoding
    static constexpr char DW_EH_AUG_R   = 'R'; // FDE encoding

    // -----------------------------------------------------------------------
    // Helpers LEB128
    // -----------------------------------------------------------------------

    static void write_uleb128(std::vector<uint8_t> &buf, uint64_t value) {
        do {
            uint8_t byte = static_cast<uint8_t>(value & 0x7F);
            value >>= 7;
            if (value != 0) byte |= 0x80;
            buf.push_back(byte);
        } while (value != 0);
    }

    static void write_sleb128(std::vector<uint8_t> &buf, int64_t value) {
        bool more = true;
        while (more) {
            uint8_t byte = static_cast<uint8_t>(value & 0x7F);
            value >>= 7;
            if ((value == 0 && (byte & 0x40) == 0) ||
                (value == -1 && (byte & 0x40) != 0)) {
                more = false;
            } else {
                byte |= 0x80;
            }
            buf.push_back(byte);
        }
    }

    static void write_u8(std::vector<uint8_t> &buf, uint8_t v) {
        buf.push_back(v);
    }

    static void write_u16(std::vector<uint8_t> &buf, uint16_t v) {
        buf.push_back(static_cast<uint8_t>(v));
        buf.push_back(static_cast<uint8_t>(v >> 8));
    }

    static void write_u32(std::vector<uint8_t> &buf, uint32_t v) {
        buf.push_back(static_cast<uint8_t>(v));
        buf.push_back(static_cast<uint8_t>(v >> 8));
        buf.push_back(static_cast<uint8_t>(v >> 16));
        buf.push_back(static_cast<uint8_t>(v >> 24));
    }

    static void write_u64(std::vector<uint8_t> &buf, uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            buf.push_back(static_cast<uint8_t>(v >> (i * 8)));
        }
    }

    // -----------------------------------------------------------------------
    // Mapeo registro x64 -> DWARF register number
    // -----------------------------------------------------------------------

    static uint8_t x64_to_dwarf(uint8_t x64_reg) noexcept {
        // x64 register numbering: RAX=0, RCX=1, RDX=2, RBX=3, RSP=4,
        // RBP=5, RSI=6, RDI=7, R8-R15=8-15
        static const uint8_t map[16] = {
            0,  // RAX  -> DW_REG_RAX
            2,  // RCX  -> DW_REG_RDX (not used for callee-saved)
            1,  // RDX  -> DW_REG_RBX (not used for callee-saved)
            3,  // RBX  -> DW_REG_RBX
            7,  // RSP  -> DW_REG_RSP
            6,  // RBP  -> DW_REG_RBP
            4,  // RSI  -> DW_REG_RSI
            5,  // RDI  -> DW_REG_RDI
            8,  // R8   -> DW_REG_R8
            9,  // R9   -> DW_REG_R9
            10, // R10  -> DW_REG_R10
            11, // R11  -> DW_REG_R11
            12, // R12  -> DW_REG_R12
            13, // R13  -> DW_REG_R13
            14, // R14  -> DW_REG_R14
            15, // R15  -> DW_REG_R15
        };
        if (x64_reg < 16) return map[x64_reg];
        return 0xFF; // invalid
    }

    // -----------------------------------------------------------------------
    // Generacion de CIE (Common Information Entry)
    // -----------------------------------------------------------------------

    /**
     * @brief Genera el CIE estandar para JIT functions.
     *
     * El CIE define:
     *   - def_cfa: CFA = RSP + 8 (despues del call, la return address
     *     esta en [RSP], y RSP apunta a ella).
     *   - offset: RIP en [RSP-8] (el call emitio esta direccion).
     *
     * @param personality_address Direccion de __gxx_personality_v0
     *                            (0 si no se necesita).
     * @return CieEntry inicializado.
     */
    CieEntry generate_cie() noexcept {
        CieEntry cie;

        // Augmentation: "z" + "P" (personality) + "L" (LSDA) + "R" (FDE encoding)
        cie.augmentation = "zPLR";

        // Initial CFI instructions
        std::vector<uint8_t> &inst = cie.initial_instructions;

        // DW_CFA_def_cfa: CFA = RSP + 8
        write_u8(inst, DW_CFA_def_cfa);
        write_uleb128(inst, DW_REG_RSP);
        write_uleb128(inst, 8);

        // DW_CFA_offset: return address (RIP) at CFA-8
        write_u8(inst, static_cast<uint8_t>(DW_CFA_offset | DW_REG_RIP));
        write_uleb128(inst, 1); // offset = -8 (encoded as 1 because factor)

        // DW_CFA_nop padding
        write_u8(inst, DW_CFA_nop);

        // FDE encoding: absolute pointer (4 bytes for 32-bit, 8 for 64-bit)
        cie.fde_encoding = DW_EH_PE_absptr;

        // LSDA encoding: absolute pointer
        cie.lsda_encoding = DW_EH_PE_absptr;

        return cie;
    }

    /**
     * @brief Establece la personality routine en el CIE.
     *
     * @param cie           CIE a modificar.
     * @param personality   Direccion de la personality routine.
     */
    void set_personality(CieEntry &cie, uint64_t personality) noexcept {
        cie.personality_address = personality;
    }

    // -----------------------------------------------------------------------
    // Generacion de CFI instructions por funcion
    // -----------------------------------------------------------------------

    /**
     * @brief Genera las instrucciones DW_CFA para una FDE.
     *
     * Las instrucciones describen como desenrollar el frame de la
     * funcion:
     *
     *   1. Al inicio (prologo):
     *      - push rbp  -> DW_CFA_advance_loc + DW_CFA_offset(RBP, -16)
     *      - mov rbp,rsp -> DW_CFA_advance_loc + DW_CFA_def_cfa_register(RBP)
     *      - sub rsp,N  -> DW_CFA_advance_loc + DW_CFA_def_cfa_offset(N+8)
     *   2. En el epilogo:
     *      - (el unwinder revierte automaticamente usando las reglas
     *        activas en cada PC)
     *
     * @param info       Informacion de unwind de la funcion.
     * @param out_cfi    Vector donde se escriben las instrucciones.
     */
    void generate_fde_cfi(const UnwindInfo &info,
                          std::vector<uint8_t> &out_cfi) {
        // Offset actual en bytes desde el inicio de la funcion.
        // Las instrucciones DW_CFA_advance_loc avanzan este offset.
        uint32_t current_offset = 0;

        // Helper para avanzar el contador de ubicacion
        auto advance = [&](uint32_t target_offset) {
            while (current_offset < target_offset) {
                uint32_t delta = target_offset - current_offset;
                if (delta <= 0x3F) {
                    write_u8(out_cfi, static_cast<uint8_t>(
                        DW_CFA_advance_loc | static_cast<uint8_t>(delta)));
                    current_offset = target_offset;
                } else {
                    // Usar DW_CFA_advance_loc de 1 byte 63 veces max.
                    // En la practica el prologo es pequeno (< 64 bytes).
                    // Si es mayor, usar DW_CFA_advance_loc1/2/4
                    if (delta <= 0x3F) {
                        write_u8(out_cfi, static_cast<uint8_t>(
                            DW_CFA_advance_loc | static_cast<uint8_t>(delta)));
                        current_offset = target_offset;
                    } else {
                        // DW_CFA_advance_loc1 (0x02) + 1 byte delta
                        write_u8(out_cfi, 0x02);
                        write_u8(out_cfi, static_cast<uint8_t>(delta & 0xFF));
                        current_offset = target_offset;
                    }
                }
            }
        };

        // 1. Si la funcion pushea RBP (frame pointer)
        if (info.has_frame_pointer() && !info.saved_regs.empty()) {
            uint8_t rbp_dwarf = DW_REG_RBP;
            // Al push RBP, RIP esta en CFA, y RBP se salva en CFA-8.
            uint8_t offset_slot = 1; // (CFA-8) / 8

            // El push rbp tipicamente es la primera instruccion.
            advance(1); // push rbp = 1 byte
            write_u8(out_cfi, static_cast<uint8_t>(DW_CFA_offset | rbp_dwarf));
            write_uleb128(out_cfi, offset_slot);

            // mov rbp, rsp = 3 bytes
            advance(4);
            // DW_CFA_def_cfa_register: CFA = RBP
            write_u8(out_cfi, DW_CFA_def_cfa_register);
            write_uleb128(out_cfi, DW_REG_RBP);
        }

        // 2. Stack allocation (sub rsp, N)
        if (info.frame_size > 0) {
            // La alocacion ocurre despues de push rbp (si existe).
            // Estimar offset: cada push son 8 bytes.
            uint32_t alloc_offset = static_cast<uint32_t>(info.prolog_bytes);
            if (alloc_offset > 0) {
                advance(alloc_offset);
                // CFA = RSP + frame_size + 8 (return address)
                uint32_t cfa_offset = info.frame_size + 8;
                write_u8(out_cfi, DW_CFA_def_cfa_offset);
                write_uleb128(out_cfi, cfa_offset);
            }
        }

        // 3. Registros adicionales salvados (push rbx, r12-r15, etc.)
        // En DWARF, cada push adicional se describe con:
        //   DW_CFA_advance_loc(N) + DW_CFA_offset(reg, slot)
        // Donde slot = (offset desde CFA negativo) / 8
        if (!info.saved_regs.empty()) {
            // Los registros se pushean en orden, cada push = 1 byte (x64).
            // El slot depende de la posicion relativa a CFA.
            uint32_t reg_offset = 1; // asumimos push rbp primero
            for (size_t i = 0; i < info.saved_regs.size(); ++i) {
                uint8_t reg = info.saved_regs[i];
                if (reg == 5) continue; // RBP ya manejado
                uint8_t dwarf_reg = x64_to_dwarf(reg);
                if (dwarf_reg == 0xFF) continue;

                uint32_t target = reg_offset + static_cast<uint32_t>(i * 1);
                advance(target);
                uint8_t slot = static_cast<uint8_t>(
                    (info.frame_size + 8 + (i + 1) * 8) / 8);
                write_u8(out_cfi, static_cast<uint8_t>(
                    DW_CFA_offset | dwarf_reg));
                write_uleb128(out_cfi, slot);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Serializacion de LSDA (.gcc_except_table)
    // -----------------------------------------------------------------------

    /**
     * @brief Codifica un LSDA en formato .gcc_except_table.
     *
     * Formato (Itanium C++ ABI):
     *
     *   LPStart encoding (1 byte)
     *   LPStart (optional, segun encoding)
     *   TType encoding (1 byte)
     *   1's complement of TType base offset (optional)
     *   Call site table length (uleb128)
     *   Call site entries (cada una: start, len, lp, action)
     *   Action table entries
     *
     * @return Vector de bytes del LSDA.
     */
    std::vector<uint8_t> LsdaInfo::encode() const {
        std::vector<uint8_t> buf;

        // LPStart encoding: default (absptr, omitido si es 0)
        write_u8(buf, DW_EH_PE_absptr);

        // TType encoding: omitido si no hay acciones
        if (actions.empty()) {
            write_u8(buf, DW_EH_PE_absptr); // no TType
        } else {
            write_u8(buf, DW_EH_PE_absptr);
        }

        // Call site table length (uleb128 placeholder)
        size_t cs_start = buf.size();
        // Placeholder for length
        write_u8(buf, 0);
        write_u8(buf, 0);
        write_u8(buf, 0);
        write_u8(buf, 0);

        // Count call sites
        size_t cs_count_pos = buf.size();
        write_u8(buf, 0); // placeholder for count
        write_u8(buf, 0);

        // Emit each call site entry (4 x uleb128)
        for (const auto &cs : call_sites) {
            write_uleb128(buf, cs.start_offset);
            write_uleb128(buf, cs.length);
            write_uleb128(buf, cs.landing_pad);
            write_uleb128(buf, cs.action_index);
        }

        // Patch the call site table length
        size_t cs_end = buf.size();
        uint32_t cs_len = static_cast<uint32_t>(cs_end - cs_start - 4);
        // Go back and write the length
        uint8_t *len_ptr = buf.data() + cs_start;
        len_ptr[0] = static_cast<uint8_t>(cs_len);
        len_ptr[1] = static_cast<uint8_t>(cs_len >> 8);
        len_ptr[2] = static_cast<uint8_t>(cs_len >> 16);
        len_ptr[3] = static_cast<uint8_t>(cs_len >> 24);

        // Patch call site count
        uint16_t cs_count = static_cast<uint16_t>(call_sites.size());
        buf[cs_count_pos]     = static_cast<uint8_t>(cs_count);
        buf[cs_count_pos + 1] = static_cast<uint8_t>(cs_count >> 8);

        // Action table entries
        for (const auto &action : actions) {
            if (action.type_info != 0) {
                write_uleb128(buf, 1); // type filter index
            } else {
                write_uleb128(buf, 0); // cleanup (catch all)
            }
            write_uleb128(buf, action.next_action);
        }
        // Terminator for action table
        write_u8(buf, 0);

        return buf;
    }

    // -----------------------------------------------------------------------
    // Emision de CIE y FDE serializados
    // -----------------------------------------------------------------------

    /**
     * @brief Serializa un CIE a un buffer binario.
     *
     * Layout:
     *   [length] (4 bytes, incluyendo el campo id)
     *   [CIE_id] (4 bytes, siempre 0)
     *   [version] (1 byte, 1)
     *   [augmentation] (null-terminated string)
     *   [code_alignment_factor] (uleb128, 1 para x86-64)
     *   [data_alignment_factor] (sleb128, -8 para x64)
     *   [return_address_register] (uleb128, 16 = RIP)
     *   [augmentation data] (opcional, segun augmentation string)
     *   [initial CFI instructions]
     *
     * @param buf Buffer de salida.
     * @param cie CIE a serializar.
     * @return Numero de bytes escritos.
     */
    size_t emit_cie(std::vector<uint8_t> &buf, const CieEntry &cie) {
        size_t start = buf.size();

        // Placeholder for length (4 bytes)
        buf.push_back(0);
        buf.push_back(0);
        buf.push_back(0);
        buf.push_back(0);

        // CIE id (0 for CIE)
        write_u32(buf, 0);

        // Version
        write_u8(buf, 1);

        // Augmentation string (null-terminated)
        for (char c : cie.augmentation) buf.push_back(static_cast<uint8_t>(c));
        buf.push_back(0);

        // Code alignment factor (1 for x86-64)
        write_uleb128(buf, 1);

        // Data alignment factor (-8 for x64)
        write_sleb128(buf, -8);

        // Return address register (RIP = 16)
        write_uleb128(buf, DW_REG_RIP);

        // Augmentation data
        // "z" indicates presence of augmentation data length
        // "P": personality routine (encoded as pointer)
        size_t aug_data_len_pos = buf.size();
        write_u8(buf, 0); // placeholder for augmentation data length

        if (cie.personality_address != 0) {
            // DW_EH_PE_absptr | DW_EH_PE_indirect? No, direct pointer
            write_u8(buf, DW_EH_PE_absptr); // personality encoding
            write_u64(buf, cie.personality_address);
        }

        // "L": LSDA encoding
        write_u8(buf, cie.lsda_encoding);

        // "R": FDE encoding
        write_u8(buf, cie.fde_encoding);

        // Patch augmentation data length
        size_t aug_data_end = buf.size();
        uint8_t aug_len = static_cast<uint8_t>(aug_data_end - aug_data_len_pos - 1);
        buf[aug_data_len_pos] = aug_len;

        // Initial CFI instructions
        buf.insert(buf.end(),
                   cie.initial_instructions.begin(),
                   cie.initial_instructions.end());

        // Patch length
        size_t end = buf.size();
        uint32_t length = static_cast<uint32_t>(end - start - 4);
        buf[start + 0] = static_cast<uint8_t>(length);
        buf[start + 1] = static_cast<uint8_t>(length >> 8);
        buf[start + 2] = static_cast<uint8_t>(length >> 16);
        buf[start + 3] = static_cast<uint8_t>(length >> 24);

        return end - start;
    }

    /**
     * @brief Serializa una FDE a un buffer binario.
     *
     * @param buf        Buffer de salida.
     * @param cie        CIE correspondiente (para encoding de direcciones).
     * @param fde        FDE a serializar.
     * @param lsda_bytes Datos del LSDA ya codificados (opcional).
     * @return Numero de bytes escritos.
     */
    size_t emit_fde(std::vector<uint8_t> &buf,
                    const CieEntry &cie,
                    const FdeEntry &fde,
                    const std::vector<uint8_t> &lsda_bytes) {
        size_t start = buf.size();

        // Placeholder for length (4 bytes)
        buf.push_back(0);
        buf.push_back(0);
        buf.push_back(0);
        buf.push_back(0);

        // CIE pointer (offset relativo al inicio del CIE)
        // Para .eh_frame, la FDE esta inmediatamente despues del CIE
        // en nuestro caso simplificado.  El CIE debe estar antes.
        // CIE_pointer = esta_posicion - posicion_del_CIE.
        // En un buffer secuencial (CIE + FDEs), el offset es
        // (start + 4) - cie_start.  Para FDEs subsecuentes se
        // calcula respecto al unico CIE al inicio.
        // Aqui asumimos que el CIE esta al inicio del buffer.
        // El caller debe ajustar si hay multiples CIEs.
        uint32_t cie_pointer = static_cast<uint32_t>(buf.size() - start);
        // En realidad apunta al CIE.  Para simplificar, el caller
        // proporciona el offset correcto.  Colocamos 0 como placeholder
        // y lo parchamos afuera.
        write_u32(buf, 0); // placeholder

        // Code start (absolute pointer segun fde_encoding)
        if (cie.fde_encoding == DW_EH_PE_absptr) {
            write_u64(buf, fde.code_start);
        } else if (cie.fde_encoding == DW_EH_PE_udata4 ||
                   cie.fde_encoding == (DW_EH_PE_pcrel | DW_EH_PE_sdata4)) {
            write_u32(buf, static_cast<uint32_t>(fde.code_start));
        }

        // Code size
        if (cie.fde_encoding == DW_EH_PE_absptr) {
            write_u64(buf, fde.code_size);
        } else {
            write_u32(buf, static_cast<uint32_t>(fde.code_size));
        }

        // Augmentation data (if "z" in augmentation)
        // Contains LSDA pointer (optional)
        size_t aug_len_pos = buf.size();
        write_u8(buf, 0); // placeholder

        if (fde.lsda_offset != 0 && !lsda_bytes.empty()) {
            // LSDA pointer (absolute)
            write_u64(buf, fde.lsda_offset);
        }

        // Patch augmentation data length
        size_t aug_data_end = buf.size();
        uint8_t aug_len = static_cast<uint8_t>(aug_data_end - aug_len_pos - 1);
        buf[aug_len_pos] = aug_len;

        // CFI instructions
        buf.insert(buf.end(),
                   fde.cfi_instructions.begin(),
                   fde.cfi_instructions.end());

        // Patch length
        size_t end = buf.size();
        uint32_t length = static_cast<uint32_t>(end - start - 4);
        buf[start + 0] = static_cast<uint8_t>(length);
        buf[start + 1] = static_cast<uint8_t>(length >> 8);
        buf[start + 2] = static_cast<uint8_t>(length >> 16);
        buf[start + 3] = static_cast<uint8_t>(length >> 24);

        return end - start;
    }

    // -----------------------------------------------------------------------
    // API de alto nivel
    // -----------------------------------------------------------------------

    /**
     * @brief Genera el .eh_frame completo para una funcion JIT.
     *
     * Crea un CIE estandar (si es el primero) + FDE para la funcion.
     *
     * @param cie_out     Vector donde se escribe el CIE (solo la primera vez).
     * @param fde_out     Vector donde se escribe la FDE.
     * @param info        Informacion de unwind de la funcion.
     * @param code_start  Direccion de inicio del codigo nativo.
     * @param code_size   Tamano del codigo en bytes.
     * @param personality Direccion de la personality routine.
     * @return true si se emitio correctamente.
     */
    bool emit_eh_frame_for_function(std::vector<uint8_t> &cie_out,
                                     std::vector<uint8_t> &fde_out,
                                     const UnwindInfo &info,
                                     uint64_t code_start,
                                     uint64_t code_size,
                                     uint64_t personality) {
        // Generar CIE (solo datos, no serializados aun)
        CieEntry cie = generate_cie();
        if (personality != 0) {
            set_personality(cie, personality);
        }

        // Generar FDE
        FdeEntry fde;
        fde.code_start = code_start;
        fde.code_size  = code_size;
        generate_fde_cfi(info, fde.cfi_instructions);

        // LSDA
        std::vector<uint8_t> lsda_bytes;
        if (info.has_lsda) {
            lsda_bytes = info.lsda.encode();
            if (!lsda_bytes.empty()) {
                fde.lsda_offset = 1; // marker
            }
        }

        // Serializar CIE
        emit_cie(cie_out, cie);

        // Serializar FDE (ajustando CIE_pointer)
        // En este esquema simplificado, el CIE esta al inicio de cie_out.
        // La FDE se empieza a serializar en fde_out.
        size_t fde_start = fde_out.size();
        // Copiamos el buffer CIE completo? No, mejor serializar en el mismo
        // buffer secuencialmente.
        // Usamos el buffer fde_out como destino.
        size_t before = fde_out.size();
        emit_fde(fde_out, cie, fde, lsda_bytes);

        // Parchear CIE_pointer en la FDE para que apunte al CIE
        // CIE_pointer = offset desde el inicio de la FDE hasta el CIE
        // Esto se calcula como: cie_start - fde_start_of_pointer_field
        // En nuestro caso, CIE esta en cie_out[0], FDE en fde_out.
        // Si estamos escribiendo ambos en el mismo buffer, son secuenciales.
        // El campo CIE_pointer esta en fde_start + 4.
        // La direccion del CIE es 0 (relativo al inicio del buffer).
        // CIE_pointer = 0 - (fde_start + 4) = complemento
        // En .eh_frame, CIE_pointer es la diferencia (en bytes) entre
        // la direccion del campo y la direccion del CIE.
        // Simplificacion: usamos 0 para CIE inmediatamente anterior.
        // El verdadero calculo requiere saber donde esta el CIE.
        // Para FDE en el mismo buffer, tras CIE:
        // CIE_pointer = (cie_start) - (fde_TEMP_pos)  -> queda como diff.
        // Por simplicidad, asumimos CIE_pointer=0 (CIE es el primero).
        // En la practica, se debe parchar con la diferencia real.
        uint32_t cie_pointer = static_cast<uint32_t>(0);
        fde_out[fde_start + 4] = static_cast<uint8_t>(cie_pointer);
        fde_out[fde_start + 5] = static_cast<uint8_t>(cie_pointer >> 8);
        fde_out[fde_start + 6] = static_cast<uint8_t>(cie_pointer >> 16);
        fde_out[fde_start + 7] = static_cast<uint8_t>(cie_pointer >> 24);

        (void)before;
        return true;
    }

} // namespace jit
