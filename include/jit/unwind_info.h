/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/unwind_info.h
 * @brief Estructuras de informacion de unwinding nativo para el JIT
 *        (Phase G - Native Exception Unwinding System).
 *
 * = Proposito =
 *
 * Cuando el JIT compila funciones a codigo maquina, el sistema operativo
 * y el runtime de C++ necesitan saber como desenrollar (unwind) la pila
 * para:
 *   1. C++ exceptions (__cxa_throw / try/catch cruzan codigo JIT).
 *   2. SEH (Windows Structured Exception Handling).
 *   3. Stack walking del debugger (backtraces precisos).
 *   4. GC stack scanning: el unwinder proporciona la cadena de frames
 *      para que el GC localice roots en codigo JIT.
 *
 * = Arquitectura =
 *
 * Se soportan dos formatos de unwind table:
 *   - Windows x64: .pdata + .xdata (RUNTIME_FUNCTION + UNWIND_INFO).
 *   - Linux x64:   .eh_frame (DWARF CFI) + optional .gcc_except_table.
 *
 * Ambos formatos comparten el mismo descriptor @c UnwindInfo que el
 * compilador JIT rellena durante la emision del prologo/epilogo.
 *
 * = Integracion con JitCompiler =
 *
 * 1. El selector/x86_encoder emite prologo y epilogo.  Durante la emision
 *    va registrando operaciones de unwind (push rbp, mov rbp,rsp, sub rsp,N)
 *    en un @c UnwindInfo temporal.
 * 2. Tras emitir, el compilador llama a @c generate_xdata() (PE) o
 *    @c generate_eh_frame() (ELF) para convertir @c UnwindInfo en los
 *    bytes de la tabla de unwind.
 * 3. Finalmente @c UnwindRegistry.register_function() instala la tabla
 *    en el SO (RtlAddFunctionTable en Windows, o registro en .eh_frame
 *    para dynamic unwinder).
 */

#ifndef VESTA_JIT_UNWIND_INFO_H
#define VESTA_JIT_UNWIND_INFO_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jit {

    // -----------------------------------------------------------------------
    // Codigos de unwind pseudo-instrucciones (x64 Windows UWOP_*).
    // El encoder los emite como una secuencia en el .xdata.
    // -----------------------------------------------------------------------

    /** @brief UWOP (Unwind Operation) codes for x64 .xdata. */
    enum UWOP : uint8_t {
        UWOP_PUSH_NONVOL     = 0, ///< push nonvolatile integer reg
        UWOP_ALLOC_LARGE     = 1, ///< allocate large stack area
        UWOP_ALLOC_SMALL     = 2, ///< allocate small stack (8*N)
        UWOP_SET_FPREG       = 3, ///< establish frame pointer register
        UWOP_SAVE_NONVOL     = 4, ///< save nonvolatile at [rsp+offset]
        UWOP_SAVE_NONVOL_FAR = 5, ///< save nonvolatile with 32-bit offset
        UWOP_SAVE_XMM128     = 6, ///< save XMM at 128-bit aligned [rsp+off]
        UWOP_SAVE_XMM128_FAR = 7, ///< save XMM with 32-bit aligned offset
        UWOP_PUSH_MACHINE    = 8, ///< machine frame (exception handling)
    };

    /**
     * @struct UnwindCode
     * @brief Una pseudo-instruccion de unwind en el .xdata.
     *
     * Cada codigo ocupa 2 bytes (unwind code word).  La secuencia completa
     * describe como deshacer el prologo para restaurar registros y liberar
     * el stack frame.
     */
    struct UnwindCode {
        uint8_t offset_in_prologue; ///< offset desde el inicio del prologo
        uint8_t op_info;            ///< UWOP code (bits 0-3) + info (bits 4-7)

        UnwindCode() noexcept : offset_in_prologue(0), op_info(0) {}

        UnwindCode(uint8_t op, uint8_t info) noexcept
            : offset_in_prologue(0),
              op_info(static_cast<uint8_t>((op & 0xF) | (info << 4))) {}

        UnwindCode(uint8_t off, uint8_t op, uint8_t info) noexcept
            : offset_in_prologue(off),
              op_info(static_cast<uint8_t>((op & 0xF) | (info << 4))) {}

        uint8_t opcode() const noexcept { return op_info & 0xF; }
        uint8_t info()   const noexcept { return op_info >> 4; }
    };

    // -----------------------------------------------------------------------
    // UNWIND_INFO (x64 PE .xdata)
    // -----------------------------------------------------------------------

    /**
     * @struct RuntimeFunction
     * @brief Entrada de la tabla .pdata (RUNTIME_FUNCTION para Win64).
     *
     * Estructura de 12 bytes que el SO espera en la seccion .pdata.
     * El campo @c unwind_info apunta al bloque UNWIND_INFO (.xdata).
     */
    struct RuntimeFunction {
        uint32_t begin_addr;    ///< RVA del inicio de la funcion
        uint32_t end_addr;      ///< RVA del fin de la funcion (exclusive)
        uint32_t unwind_info;   ///< RVA del UNWIND_INFO

        RuntimeFunction() noexcept
            : begin_addr(0), end_addr(0), unwind_info(0) {}

        RuntimeFunction(uint32_t ba, uint32_t ea, uint32_t ui) noexcept
            : begin_addr(ba), end_addr(ea), unwind_info(ui) {}
    };

    /**
     * @struct XdataInfo
     * @brief Contiene el bloque .xdata para una funcion JIT.
     */
    struct XdataInfo {
        uint8_t               flags = 0;     ///< UNW_FLAG_*
        uint8_t               prolog_size = 0;
        uint8_t               frame_reg   = 0;
        uint8_t               frame_offset = 0;
        std::vector<UnwindCode> codes;

        uint32_t              personality_routine = 0;
        uint32_t              chained_function = 0;

        size_t serialized_size() const noexcept;
    };

    // -----------------------------------------------------------------------
    // .eh_frame / DWARF CFI (ELF)
    // -----------------------------------------------------------------------

    /**
     * @struct CieEntry
     * @brief Common Information Entry para .eh_frame.
     */
    struct CieEntry {
        std::string           augmentation;
        std::vector<uint8_t>  initial_instructions;
        uint64_t              personality_address = 0;
        uint8_t               fde_encoding  = 0x00;
        uint8_t               lsda_encoding = 0x00;
    };

    /**
     * @struct FdeEntry
     * @brief Frame Description Entry para .eh_frame.
     */
    struct FdeEntry {
        uint64_t              code_start = 0;
        uint64_t              code_size  = 0;
        std::vector<uint8_t>  cfi_instructions;
        uint32_t              lsda_offset = 0;
    };

    // -----------------------------------------------------------------------
    // Language-Specific Data Area (LSDA)
    // -----------------------------------------------------------------------

    /**
     * @struct CallSiteEntry
     * @brief Entrada de la tabla de call sites dentro del LSDA.
     */
    struct CallSiteEntry {
        uint32_t start_offset;
        uint32_t length;
        uint32_t landing_pad;    ///< 0 = no handler
        uint32_t action_index;   ///< 0 = no action
    };

    /**
     * @struct ActionEntry
     * @brief Entrada de la tabla de acciones del LSDA.
     */
    struct ActionEntry {
        uint64_t type_info    = 0;
        uint32_t next_action = 0;
    };

    /**
     * @struct LsdaInfo
     * @brief Language-Specific Data Area para una funcion JIT.
     */
    struct LsdaInfo {
        uint32_t                  lp_base = 0;
        std::vector<CallSiteEntry> call_sites;
        std::vector<ActionEntry>   actions;

        std::vector<uint8_t> encode() const;
    };

    // -----------------------------------------------------------------------
    // UnwindInfo principal
    // -----------------------------------------------------------------------

    /**
     * @struct UnwindInfo
     * @brief Descriptor completo de unwind para una funcion JIT.
     *
     * El compilador JIT rellena esta estructura durante la fase de
     * emision del prologo/epilogo.  El consumer (UnwindRegistry) genera
     * los bytes de .xdata o .eh_frame a partir de esta informacion.
     */
    struct UnwindInfo {
        uint32_t              frame_size   = 0;
        uint8_t               frame_reg    = 0;   ///< 0 = RBP, 0xFF = none
        uint8_t               frame_offset = 0;
        std::vector<uint8_t>  saved_regs;
        bool                  has_lsda     = false;
        LsdaInfo              lsda;
        uint8_t               prolog_bytes = 0;
        uint64_t              personality_address = 0;

        UnwindInfo() noexcept = default;

        bool has_frame_pointer() const noexcept {
            return frame_reg != 0xFF;
        }

        uint32_t unwind_code_count() const noexcept;
    };

    // =========================================================================
    // Inline implementations
    // =========================================================================

    inline uint32_t UnwindInfo::unwind_code_count() const noexcept {
        uint32_t count = static_cast<uint32_t>(saved_regs.size());
        if (frame_size > 0) ++count;
        if (has_frame_pointer()) ++count;
        return count;
    }

    inline size_t XdataInfo::serialized_size() const noexcept {
        size_t sz = 4;
        size_t code_bytes = codes.size() * 2;
        if (code_bytes & 2) code_bytes += 2;
        sz += code_bytes;
        if (flags & 1 || flags & 2) sz += 4;
        if (flags & 4) sz += 4;
        return sz;
    }

} // namespace jit

#endif // VESTA_JIT_UNWIND_INFO_H
