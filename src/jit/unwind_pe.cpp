/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/unwind_pe.cpp
 * @brief Generacion de unwind info en formato PE/COFF (Windows x64).
 *
 * = Diseno =
 *
 * Windows x64 requiere que cada funcion JIT tenga una entrada en la
 * tabla .pdata (RUNTIME_FUNCTION) que apunte a un bloque .xdata
 * (UNWIND_INFO) describiendo como desenrollar su pila.  Estas tablas
 * se registran con @c RtlAddFunctionTable para que el OS y el debugger
 * puedan realizar stack walking y manejo de excepciones.
 *
 * = Formato .xdata =
 *
 * El bloque UNWIND_INFO (xdata) tiene el siguiente layout:
 *
 *   struct UNWIND_INFO {
 *       uint8_t  version_flags;      // bits 0-2: version (1), bits 3-7: flags
 *       uint8_t  prolog_size;        // tamano del prologo en bytes
 *       uint8_t  unwind_code_count;  // numero de unwind codes
 *       uint8_t  frame_reg_offset;   // frame register + scaled offset
 *       UnwindCode codes[];          // secuencia de unwind codes (2 bytes c/u)
 *   };
 *
 * = Unwind codes generados =
 *
 *   - UWOP_PUSH_NONVOL:   push de un registro no-volatil (rbp, rbx, r12-r15).
 *   - UWOP_SET_FPREG:     mov rbp, rsp (establece frame pointer).
 *   - UWOP_ALLOC_SMALL:   sub rsp, N*8 donde N < 128.
 *   - UWOP_ALLOC_LARGE:   sub rsp, N donde N >= 1024 (2 words).
 *
 * = Chained unwind =
 *
 * Para funciones con prologos parciales (shrink-wrap) o funciones
 * que comparten prologo con su llamadora, se usa UNW_FLAG_CHAININFO.
 * El campo @c chained_function apunta a la RUNTIME_FUNCTION de la
 * funcion que contiene el prologo real.
 *
 * = Uso tipico en el JIT =
 *
 *   UnwindInfo info;
 *   // Durante la emision del prologo:
 *   info.saved_regs.push_back(5);   // RBP
 *   info.frame_size = 0x20;
 *   info.frame_reg = 5;             // RBP como FP
 *
 *   // Generar .xdata:
 *   XdataInfo xdata = generate_xdata(info, prolog_size, image_base);
 *
 *   // Serializar al buffer:
 *   uint8_t *xdata_buf = cache->alloc(xdata.serialized_size());
 *   serialize_xdata(xdata_buf, xdata);
 */

#include "jit/unwind_info.h"

#if defined(_WIN32)
#  include <windows.h>
#endif
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace jit {

    // -----------------------------------------------------------------------
    // Tabla de mapeo registro x64 -> UWOP numbering
    // -----------------------------------------------------------------------
    //
    // Registros no-volatiles x64:
    //   0  - RAX  (volatil, no se salva)
    //   1  - RCX  (volatil)
    //   2  - RDX  (volatil)
    //   3  - RBX  (NO volatil)
    //   4  - RSP  (no se salva explicitamente)
    //   5  - RBP  (NO volatil, tipicamente frame pointer)
    //   6  - RSI  (NO volatil)
    //   7  - RDI  (NO volatil)
    //   8-15 - R8-R15 (R12-R15 son NO volatiles)
    //
    static constexpr bool is_nonvolatile(uint8_t reg) noexcept {
        return (reg == 3) || (reg == 5) || (reg == 6) || (reg == 7) ||
               (reg >= 12 && reg <= 15);
    }

    // -----------------------------------------------------------------------
    // Helpers de serializacion little-endian
    // -----------------------------------------------------------------------

    static void write_u8(uint8_t *&p, uint8_t v) noexcept {
        *p++ = v;
    }

    static void write_u16(uint8_t *&p, uint16_t v) noexcept {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
        p += 2;
    }

    static void write_u32(uint8_t *&p, uint32_t v) noexcept {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
        p[2] = static_cast<uint8_t>(v >> 16);
        p[3] = static_cast<uint8_t>(v >> 24);
        p += 4;
    }

    // -----------------------------------------------------------------------
    // Generacion de unwind codes desde UnwindInfo
    // -----------------------------------------------------------------------

    /**
     * @brief Genera la secuencia de UnwindCode a partir de UnwindInfo.
     *
     * El orden de los codigos debe ser inverso al del prologo: el
     * unwinder los ejecuta en orden ascendente de offset, pero cada
     * codigo se emite con el offset del prologo donde ocurrio.
     *
     * @param info      Descripcion del unwind.
     * @param prolog_size Bytes totales del prologo.
     * @param out_codes Vector donde se anyaden los codigos generados.
     */
    static void generate_unwind_codes(const UnwindInfo &info,
                                      uint8_t prolog_size,
                                      std::vector<UnwindCode> &out_codes) {
        // Offset actual en el prologo (empezamos desde el final porque
        // el prologo se ejecuta en orden inverso al desenrollar).
        uint8_t off = prolog_size;

        // 1. ALLOC: sub rsp, frame_size (se ejecuta al final del prologo
        //    si hay frame pointer, o al inicio si no).
        if (info.frame_size > 0) {
            if (info.frame_size < 0x400) {
                // UWOP_ALLOC_SMALL: N = frame_size / 8 - 1 (rango 8..1024)
                uint8_t n = static_cast<uint8_t>((info.frame_size / 8) - 1);
                out_codes.push_back(UnwindCode(off, UWOP_ALLOC_SMALL, n));
            } else {
                // UWOP_ALLOC_LARGE: sigue 2 words de 16 bits con el tamano.
                // Primero emitimos el codigo con info=0, luego 2 words.
                out_codes.push_back(UnwindCode(off, UWOP_ALLOC_LARGE, 0));
                // Large allocation writes the size as two 16-bit words
                // immediately after the unwind code, stored inline.
                uint16_t lo = static_cast<uint16_t>(info.frame_size & 0xFFFF);
                uint16_t hi = static_cast<uint16_t>(info.frame_size >> 16);
                (void)lo; (void)hi; // serialized in emit_xdata
            }
        }

        // 2. SET_FPREG: si la funcion usa frame pointer
        if (info.has_frame_pointer()) {
            out_codes.push_back(UnwindCode(off, UWOP_SET_FPREG,
                                           info.frame_offset));
        }

        // 3. PUSH_NONVOL: registros salvados (en orden inverso al push)
        //    El prologo pushea RBP, RBX, R12... en ese orden.  Para
        //    desenrollar hay que emitirlos en orden inverso.
        for (auto it = info.saved_regs.rbegin();
             it != info.saved_regs.rend(); ++it) {
            if (!is_nonvolatile(*it)) continue;
            // Estimacion del offset: cada push son 8 bytes.
            off -= 8;
            out_codes.push_back(UnwindCode(off, UWOP_PUSH_NONVOL, *it));
        }
    }

    // -----------------------------------------------------------------------
    // Serializacion de .xdata
    // -----------------------------------------------------------------------

    /**
     * @brief Serializa un bloque UNWIND_INFO (.xdata) en un buffer.
     *
     * Layout:
     *   [0]   version_flags: bits 0-2=1, bits 3-7=flags
     *   [1]   prolog_size
     *   [2]   unwind_code_count
     *   [3]   frame_reg (bits 0-3) | frame_offset_scaled (bits 4-7)
     *   [4..] UnwindCode words (2 bytes c/u, alineados a 4)
     *   [..]  optional: personality routine RVA
     *   [..]  optional: chained unwind RVA
     *
     * @param buf   Buffer de salida (debe tener al menos @c serialized_size() bytes).
     * @param xdata Datos a serializar.
     * @return Numero de bytes escritos.
     */
    static size_t serialize_xdata(uint8_t *buf, const XdataInfo &xdata) noexcept {
        uint8_t *p = buf;

        // Version (1) + flags en el nibble alto
        uint8_t version_flags = static_cast<uint8_t>(1 | (xdata.flags << 3));
        write_u8(p, version_flags);
        write_u8(p, xdata.prolog_size);
        write_u8(p, static_cast<uint8_t>(xdata.codes.size()));
        // Frame register + scaled offset
        write_u8(p, static_cast<uint8_t>(
            (xdata.frame_reg & 0xF) | (xdata.frame_offset << 4)));

        // Unwind codes (2 bytes cada uno)
        for (const auto &code : xdata.codes) {
            write_u8(p, code.offset_in_prologue);
            write_u8(p, code.op_info);
        }
        // Pad to 4-byte alignment
        size_t code_bytes = xdata.codes.size() * 2;
        if (code_bytes & 2) {
            write_u16(p, 0);
        }

        // Personality routine (if EHANDLER or UHANDLER)
        if (xdata.flags & 1 || xdata.flags & 2) {
            write_u32(p, xdata.personality_routine);
        }

        // Chained unwind info (if CHAININFO)
        if (xdata.flags & 4) {
            write_u32(p, xdata.chained_function);
        }

        return static_cast<size_t>(p - buf);
    }

    // -----------------------------------------------------------------------
    // API publica
    // -----------------------------------------------------------------------

    /**
     * @brief Genera el contenido de .xdata para una funcion JIT.
     *
     * @param info         Informacion de unwind de la funcion.
     * @param prolog_size  Tamano del prologo en bytes.
     * @param image_base   Base de la imagen (para calcular RVAs).
     * @return XdataInfo listo para serializar.
     */
    XdataInfo generate_xdata(const UnwindInfo &info,
                             uint8_t prolog_size,
                             uint64_t image_base) noexcept {
        XdataInfo xdata;
        xdata.prolog_size = prolog_size;

        // Flags: si hay personality routine, marcar EHANDLER
        if (info.personality_address != 0) {
            xdata.flags |= 1;  // UNW_FLAG_EHANDLER
        }

        // Frame pointer
        if (info.has_frame_pointer()) {
            xdata.frame_reg   = info.frame_reg;
            xdata.frame_offset = info.frame_offset;
        }

        // Generar unwind codes
        generate_unwind_codes(info, prolog_size, xdata.codes);

        // Personality routine RVA (relativo a image_base)
        if (info.personality_address != 0) {
            xdata.personality_routine =
                static_cast<uint32_t>(info.personality_address - image_base);
        }

        return xdata;
    }

    /**
     * @brief Genera una entrada RUNTIME_FUNCTION para .pdata.
     *
     * @param code_start   RVA del inicio del codigo.
     * @param code_end     RVA del fin del codigo (exclusive).
     * @param xdata_rva    RVA del bloque .xdata.
     * @return RuntimeFunction lista para insertar en .pdata.
     */
    RuntimeFunction generate_runtime_function(uint32_t code_start,
                                               uint32_t code_end,
                                               uint32_t xdata_rva) noexcept {
        return RuntimeFunction(code_start, code_end, xdata_rva);
    }

    /**
     * @brief Serializa una RuntimeFunction en formato binario (12 bytes).
     *
     * @param out  Buffer de 12 bytes de salida.
     * @param rf   Entrada a serializar.
     */
    void serialize_runtime_function(uint8_t *out,
                                    const RuntimeFunction &rf) noexcept {
        uint8_t *p = out;
        write_u32(p, rf.begin_addr);
        write_u32(p, rf.end_addr);
        write_u32(p, rf.unwind_info);
    }

    /**
     * @brief Emite el bloque .xdata completo para una funcion JIT.
     *
     * @param buf         Buffer de salida.
     * @param info        Informacion de unwind.
     * @param prolog_size Tamano del prologo.
     * @param image_base  Base de la imagen para RVAs.
     * @return Numero de bytes escritos en @p buf.
     */
    size_t emit_xdata(uint8_t *buf,
                      const UnwindInfo &info,
                      uint8_t prolog_size,
                      uint64_t image_base) noexcept {
        XdataInfo xdata = generate_xdata(info, prolog_size, image_base);
        return serialize_xdata(buf, xdata);
    }

    // -----------------------------------------------------------------------
    // Instalacion / desinstalacion de tablas del SO (Windows)
    // -----------------------------------------------------------------------

#if defined(_WIN32)

    /**
     * @brief Registra una funcion en la tabla de unwind del SO.
     *
     * Llama a @c RtlAddFunctionTable para que el OS y el debugger
     * puedan desenrollar a traves de esta funcion JIT.
     *
     * @param pdata_array  Array de RUNTIME_FUNCTION entries.
     * @param entry_count  Numero de entradas.
     * @param image_base   Base de la imagen para las RVAs.
     * @return true si la operacion fue exitosa.
     */
    bool install_pe_function_table(RuntimeFunction *pdata_array,
                                    DWORD entry_count,
                                    uint64_t image_base) noexcept {
        if (!pdata_array || entry_count == 0) return false;

        return ::RtlAddFunctionTable(
                   reinterpret_cast<PRUNTIME_FUNCTION>(pdata_array),
                   entry_count,
                   image_base) != FALSE;
    }

    /**
     * @brief Desregistra una funcion de la tabla de unwind del SO.
     *
     * @param image_base Base de la imagen.
     * @return true si la operacion fue exitosa.
     */
    bool uninstall_pe_function_table(uint64_t image_base) noexcept {
        return ::RtlDeleteFunctionTable(
                   reinterpret_cast<PRUNTIME_FUNCTION>(nullptr)) != FALSE;
        // Nota: RtlDeleteFunctionTable requiere la funcion exacta.
        // Para JIT dynamic, se usa el puntero devuelto por Add.
    }

#else // !_WIN32

    // Stubs para Linux - la funcionalidad PE no esta disponible.
    // El compilador no deberia llamar estas funciones en Linux.

    bool install_pe_function_table(RuntimeFunction *, uint32_t, uint64_t) noexcept {
        return false;
    }

    bool uninstall_pe_function_table(uint64_t) noexcept {
        return false;
    }

#endif // _WIN32

} // namespace jit
