/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/machine_ir.h
 * @brief MachineIR target-specific para x86-64.
 *
 * = Diseno cache-friendly =
 *
 * Cada @c MInstr ocupa EXACTAMENTE 32 bytes -> 2 instrucciones por
 * cache line de 64 bytes.  Iterar un @c MBlock durante el encoder
 * pass es secuencial puro: 1 cache miss por cada 2 instrs en el
 * mejor caso.  El selector NO crea nodos en heap sueltos; usa
 * @c std::vector<MInstr> contiguo dentro de cada @c MBlock.
 *
 * = Operandos =
 *
 * @c MOperand es 8 bytes (uno por slot).  Para inmediatos de 64 bits
 * (mov r64, imm64), se almacena un indice a @c MFunction::imm64_pool;
 * el imm cabe en el value de la MOperand y la decodificacion se hace
 * en el encoder (zero overhead vs imm64 inline en cada MInstr).
 *
 * Para operandos de memoria @c MOperand encoda:
 *   - reg     = base register (0..63)
 *   - width   = scale (bits[1:0]: 1/2/4/8) + index_reg (bits[7:2])
 *   - flags   = address size hint (32-bit / 64-bit)
 *   - value   = disp32 (sign-extended)
 *
 * = Indices, no punteros =
 *
 * @c MBlock se referencia por @c uint16_t (max 65535 bloques por
 * funcion - mucho mas que suficiente).  Predecesores/sucesores se
 * almacenan en pools compartidos del @c MFunction para evitar
 * @c std::vector::vector overhead.
 *
 * = Convencion VM =
 *
 * El JIT-eado de VestaVM usa una convencion FIJA:
 *
 *   Entry de funcion JIT:
 *     - rdi (SysV) / rcx (Win64): ProcessVM *proc
 *     - args reales viven en @c proc->registers.regs[1..N]
 *     - return se escribe a @c proc->registers.regs[0]
 *     - argc en @c proc->registers.regs[15]
 *
 * El selector materializa los args bytecode-VM en regs nativos via
 * LOAD desde el offset apropiado de @c ProcessVM, y al final hace
 * STORE del return.  Toda la calling convention se maneja en el
 * selector, no en MachineIR.
 *
 * = Phases =
 *
 * Phase D.1.a (este header): tipos + lifetime.  NO emite bytes aun.
 * Phase D.1.b: instruction selector @c ssa_ir::IrFunction -> @c MFunction.
 * Phase D.1.c: encoder @c MFunction -> @c std::vector<uint8_t>.
 * Phase D.1.d: label resolution + relocations.
 */

#ifndef VESTA_JIT_MACHINE_IR_H
#define VESTA_JIT_MACHINE_IR_H

#include <cstdint>
#include <string>
#include <vector>

namespace jit {

    /* ===================================================================== */
    /* Registros x86-64                                                       */
    /* ===================================================================== */

    /**
     * @enum MReg
     * @brief Identificadores de registros maquina x86-64.
     *
     * Encoding compacto: los GP regs ocupan 0..15, los XMM 16..31.
     * El @c uint8_t en @c MOperand.reg los acepta directos.
     *
     * Mapeo VM -> host (convencion):
     *   - bytecode R0  -> RAX (return value)
     *   - bytecode R1  -> R10 (caller-saved scratch en SysV/Win64)
     *   - bytecode R2  -> R11
     *   - bytecode R3..R7  -> R8/R9 + 3 reservados de la VM
     *   - bytecode R15 -> R15 (argc, callee-saved)
     *
     * Reservados por el host ABI:
     *   - RSP/RBP: stack frame del codigo JIT
     *   - RDI (SysV) / RCX (Win64): @c ProcessVM* (primer arg)
     *
     * NOTE: la asignacion concreta VM-reg -> host-reg vive en el
     * selector.  MachineIR es agnostico; aqui solo definimos
     * el espacio de regs disponibles.
     */
    /**
     * @enum RegClass
     * @brief Clase de un registro (virtual o fisico).  Phase D.7.
     *
     * El register allocator asigna cada clase de forma INDEPENDIENTE: un
     * vreg GP solo puede ir a un fisico GP, un vreg FP solo a un fisico FP.
     * Esto deja preparado el banco de coma flotante/vector (XMM en x86,
     * NEON en ARM) sin reescribir el core: anyadir floats = registrar la
     * clase FP con sus fisicos.  En D.7 v1 solo se asignan registros GP;
     * los valores float siguen con memory-roundtrip hasta la fase XMM.
     */
    enum class RegClass : uint8_t {
        GP    = 0,  ///< proposito general: enteros y punteros (RAX..R15 / X0..X30 / ...)
        FP    = 1,  ///< coma flotante / vector (XMM / NEON / ...) -- reservado en v1
        COUNT = 2   ///< numero de clases (para dimensionar tablas del TargetRegInfo)
    };

    /**
     * @enum AbiKind
     * @brief Convencion con la que se genera el prologue/epilogue y el paso de
     *        argumentos de una funcion JIT (Phase D.7).
     */
    enum class AbiKind : uint8_t {
        HOST_LEAF = 0,  ///< funcion hoja host: args en arg_regs, return en RAX
                        ///< (tests aislados).
        VM        = 1   ///< VM_ABI: @c ProcessVM* en RCX(Win64)/RDI(SysV), args
                        ///< en @c proc->registers.regs[1..N], return en regs[0].
                        ///< RBX = @c ProcessVM* durante toda la funcion.
    };

    enum class MReg : uint8_t {
        /* GP 64-bit */
        RAX = 0, RCX = 1, RDX = 2, RBX = 3,
        RSP = 4, RBP = 5, RSI = 6, RDI = 7,
        R8  = 8, R9  = 9, R10 = 10, R11 = 11,
        R12 = 12, R13 = 13, R14 = 14, R15 = 15,
        /* XMM 128-bit (cubre f32/f64 escalar) */
        XMM0 = 16, XMM1 = 17, XMM2 = 18, XMM3 = 19,
        XMM4 = 20, XMM5 = 21, XMM6 = 22, XMM7 = 23,
        XMM8 = 24, XMM9 = 25, XMM10 = 26, XMM11 = 27,
        XMM12 = 28, XMM13 = 29, XMM14 = 30, XMM15 = 31,
        /* sentinela invalido */
        NONE = 63
    };

    /** @brief Convierte @c MReg a indice @c uint8_t para encoding. */
    inline uint8_t reg_id(MReg r) noexcept { return static_cast<uint8_t>(r); }

    /** @brief True si @p r es un registro GP (0..15). */
    inline bool is_gp(MReg r) noexcept { return reg_id(r) < 16; }

    /** @brief True si @p r es un registro XMM (16..31). */
    inline bool is_xmm(MReg r) noexcept {
        const uint8_t id = reg_id(r);
        return id >= 16 && id < 32;
    }

    /* ===================================================================== */
    /* Condiciones (para jcc / setcc / cmovcc)                                */
    /* ===================================================================== */

    /**
     * @enum MCond
     * @brief Codigos de condicion x86-64.  Los valores coinciden con la
     *        codificacion binaria del opcode (bits[3:0] de Jcc/SETcc).
     */
    enum class MCond : uint8_t {
        O    = 0x0,  ///< overflow
        NO   = 0x1,  ///< no overflow
        B    = 0x2,  ///< below (CF=1)
        AE   = 0x3,  ///< above-or-equal (CF=0)
        E    = 0x4,  ///< equal (ZF=1)
        NE   = 0x5,  ///< not equal (ZF=0)
        BE   = 0x6,  ///< below-or-equal
        A    = 0x7,  ///< above
        S    = 0x8,  ///< sign
        NS   = 0x9,  ///< not sign
        P    = 0xA,  ///< parity
        NP   = 0xB,  ///< not parity
        L    = 0xC,  ///< less (signed)
        GE   = 0xD,  ///< greater-or-equal (signed)
        LE   = 0xE,  ///< less-or-equal (signed)
        G    = 0xF,  ///< greater (signed)
        NONE = 0xFF
    };

    /* ===================================================================== */
    /* Operandos                                                              */
    /* ===================================================================== */

    /**
     * @enum MOperandKind
     * @brief Tipo del operando.
     */
    enum class MOperandKind : uint8_t {
        NONE      = 0,  ///< slot vacio
        REG       = 1,  ///< registro maquina
        IMM32     = 2,  ///< inmediato 32-bit sign-extended
        IMM64_IDX = 3,  ///< imm64 via indice a @c MFunction::imm64_pool
        MEM       = 4,  ///< memoria: base + index*scale + disp32
        LABEL     = 5,  ///< label_id (para JMP/JCC/CALL relativos)
        REL_RT    = 6,  ///< runtime entry slot (puntero resuelto en link)
        VREG      = 7   ///< registro VIRTUAL (Phase D.7): id en @c value, clase en @c flags
    };

    /**
     * @struct MOperand
     * @brief Operando de instruccion maquina (8 bytes, cache-friendly).
     *
     * Layout:
     *   +0 [1] kind     MOperandKind
     *   +1 [1] reg      MReg (REG/MEM.base)
     *   +2 [1] width    bytes operand (1/2/4/8) o MEM.scale+index packed
     *   +3 [1] flags    reservado / mem_size override
     *   +4 [4] value    imm32 / label_id / disp32 / imm64_pool_idx
     *
     * Para @c MEM:
     *   - reg     = base register
     *   - width   = bits[1:0] scale (1/2/4/8 = 0/1/2/3),
     *               bits[7:2] index_reg (0..63), o 63 = none
     *   - flags   = address size (0=64-bit, 1=32-bit)
     *   - value   = disp32 signed
     */
    struct MOperand {
        MOperandKind kind  = MOperandKind::NONE;
        uint8_t      reg   = static_cast<uint8_t>(MReg::NONE);
        uint8_t      width = 8;     ///< bytes (1/2/4/8) o packed MEM scale|index
        uint8_t      flags = 0;
        int32_t      value = 0;     ///< imm32 / label_id / disp / pool_idx

        /* Helpers de construccion (zero overhead, todos inline). */
        static MOperand none() noexcept { return MOperand{}; }

        static MOperand make_reg(MReg r, uint8_t w = 8) noexcept {
            MOperand o; o.kind = MOperandKind::REG; o.reg = reg_id(r); o.width = w; return o;
        }

        static MOperand make_imm32(int32_t v, uint8_t w = 4) noexcept {
            MOperand o; o.kind = MOperandKind::IMM32; o.width = w; o.value = v; return o;
        }

        static MOperand make_imm64_idx(uint32_t pool_idx) noexcept {
            MOperand o; o.kind = MOperandKind::IMM64_IDX; o.width = 8;
            o.value = static_cast<int32_t>(pool_idx);
            return o;
        }

        static MOperand make_mem(MReg base, int32_t disp,
                                 MReg index = MReg::NONE,
                                 uint8_t scale = 1,
                                 uint8_t addr_size_32 = 0) noexcept {
            MOperand o;
            o.kind  = MOperandKind::MEM;
            o.reg   = reg_id(base);
            /* scale: 1->0, 2->1, 4->2, 8->3 */
            uint8_t scale_bits = 0;
            switch (scale) {
                case 1: scale_bits = 0; break;
                case 2: scale_bits = 1; break;
                case 4: scale_bits = 2; break;
                case 8: scale_bits = 3; break;
                default: scale_bits = 0; break;
            }
            o.width = static_cast<uint8_t>((reg_id(index) << 2) | (scale_bits & 0x3));
            o.flags = addr_size_32 ? 1 : 0;
            o.value = disp;
            return o;
        }

        static MOperand make_label(uint32_t label_id) noexcept {
            MOperand o; o.kind = MOperandKind::LABEL;
            o.value = static_cast<int32_t>(label_id);
            return o;
        }

        /**
         * @brief Construye un operando de registro VIRTUAL (Phase D.7).
         *
         * El id del vreg vive en @c value (no en @c reg, que es u8 y se
         * reserva para fisicos 0..63).  La clase (GP/FP) se guarda en el
         * bit0 de @c flags.  @c width es el ancho del operando en bytes.
         *
         * @param vid    Id del registro virtual (denso 0..vreg_count-1).
         * @param cls    Clase del registro (GP/FP).
         * @param w      Ancho en bytes (1/2/4/8).
         */
        static MOperand make_vreg(uint32_t vid, RegClass cls, uint8_t w = 8) noexcept {
            MOperand o;
            o.kind  = MOperandKind::VREG;
            o.value = static_cast<int32_t>(vid);
            o.flags = static_cast<uint8_t>(cls);
            o.width = w;
            return o;
        }

        /* Accessors VREG */
        bool     is_vreg() const noexcept { return kind == MOperandKind::VREG; }
        bool     is_reg()  const noexcept { return kind == MOperandKind::REG; }
        uint32_t vreg_id() const noexcept { return static_cast<uint32_t>(value); }
        RegClass vreg_class() const noexcept {
            return static_cast<RegClass>(flags & 0x1);
        }

        /* Accessors MEM */
        MReg    mem_base() const noexcept  { return static_cast<MReg>(reg); }
        MReg    mem_index() const noexcept { return static_cast<MReg>(width >> 2); }
        uint8_t mem_scale() const noexcept {
            static const uint8_t S[4] = {1, 2, 4, 8};
            return S[width & 0x3];
        }
        int32_t mem_disp() const noexcept  { return value; }
        bool    mem_addr32() const noexcept { return (flags & 1) != 0; }
    };

    static_assert(sizeof(MOperand) == 8, "MOperand debe ser 8 bytes");

    /* ===================================================================== */
    /* Opcodes maquina (x86-64 v1)                                            */
    /* ===================================================================== */

    /**
     * @enum MOp
     * @brief Opcodes maquina abstractos.  El encoder los traduce a la
     *        secuencia de bytes x86-64 correspondiente.
     *
     * Cobertura v1: el subset minimo para que un C1
     * baseline JIT pueda compilar funciones de aritmetica + control
     * de flujo + calls.  Floats, SIMD y sync atomic se anyaden en
     * fases posteriores (D.3+ para floats, D.8 para SIMD).
     */
    enum class MOp : uint8_t {
        NOP         = 0,

        /* Movimiento */
        MOV         = 1,    ///< MOV dst, src (reg/imm/mem)
        LEA         = 2,    ///< LEA dst, [mem]
        PUSH        = 3,    ///< PUSH src (reg/imm)
        POP         = 4,    ///< POP dst (reg)

        /* ALU enteros */
        ADD         = 10,
        SUB         = 11,
        IMUL        = 12,   ///< IMUL dst, src (2-op signed)
        AND         = 13,
        OR          = 14,
        XOR         = 15,
        SHL         = 16,   ///< SHL dst, imm8/cl
        SHR         = 17,
        SAR         = 18,
        NEG         = 19,
        NOT         = 20,
        IDIV        = 21,   ///< IDIV src (RDX:RAX / src -> RAX, RDX = rem)
        CQO         = 22,   ///< sign-extend RAX into RDX:RAX (CDQ for 32-bit)
        MOVZX       = 23,   ///< MOVZX dst, src (zero-extend u8/u16 -> u64)
        MOVSX       = 24,   ///< MOVSX dst, src (sign-extend i8/i16/i32 -> i64)
        INC         = 25,   ///< INC dst (++dst, mas compacto que ADD dst, 1)
        DEC         = 26,   ///< DEC dst (--dst, mas compacto que SUq dst, 1)

        /* Bit ops universales (Math-IR-promote v2.2a).
         * Cada uno es 1 instr nativa en x86 (BMI1/SSE4.2) y ARM/RISC-V. */
        POPCNT      = 27,   ///< POPCNT dst, src (F3 0F B8 /r REX.W) -- count set bits (SSE4.2)
        LZCNT       = 28,   ///< LZCNT  dst, src (F3 0F BD /r REX.W) -- count leading zeros (BMI1)
        TZCNT       = 29,   ///< TZCNT  dst, src (F3 0F BC /r REX.W) -- count trailing zeros (BMI1)

        /* Comparacion + condicionales */
        CMP         = 30,
        TEST        = 31,
        SETCC       = 32,   ///< variant=MCond
        CMOVCC      = 33,   ///< variant=MCond

        /* Control de flujo */
        JMP         = 40,   ///< JMP rel32 (operand=LABEL)
        JCC         = 41,   ///< Jcc rel32 (variant=MCond)
        CALL        = 42,   ///< CALL rel32 (LABEL) o reg
        RET         = 43,
        INT3        = 44,   ///< debug trap (0xCC)

        /* Pseudo-ops */
        LABEL_DEF   = 50,   ///< marca posicion de un label (no emite bytes)
        SAFEPOINT   = 51,   ///< polling check: cmp byte [rbx], 0 + jne handler
        COMMENT     = 52,   ///< no-op con texto debug (skipped en release)
        CALL_ABS    = 53,   ///< CALL a direccion absoluta (via mov rax, imm64 + call rax)
        ARG         = 54,   ///< pseudo (D.7): marca un argumento de la siguiente CALL.
                            ///< variant = indice del arg; src1 = vreg.  No emite bytes;
                            ///< el rewrite recolecta los ARG y hace el parallel-move a
                            ///< los arg_regs justo antes del CALL.
        LOAD        = 55,   ///< pseudo (D.7 commit 7): dst = [addr_vreg] (disp 0).
                            ///< dst = dst_vreg, src1 = addr_vreg; flags = (width<<1)|signed.
                            ///< El rewrite lo baja a MOV/MOVZX/MOVSX con [reg].
        STORE       = 56,   ///< pseudo (D.7 commit 7): [addr_vreg] = val_vreg (disp 0).
                            ///< src1 = addr_vreg, src2 = val_vreg; flags = width.
        ALLOCA      = 57,   ///< pseudo (D.7 commit 8): dst = host_ptr a @c value
                            ///< bytes reservados en el frame JIT.  dst = dst_vreg,
                            ///< value = size.  El rewrite lo baja a LEA [rbp-off].

        /* Bit ops adicionales (Math-IR-promote v2.2a, continuacion). */
        BSWAP       = 60,   ///< BSWAP dst (0F C8+rd REX.W) -- byte swap full register
        ROL         = 61,   ///< ROL dst, cl/imm (D3/C1 /0 REX.W) -- rotate left
        ROR         = 62,   ///< ROR dst, cl/imm (D3/C1 /1 REX.W) -- rotate right

        /* FP scalar ops (Math-IR-promote v2.2b).
         * Mov data entre GP regs y XMM regs via memory roundtrip o MOVQ
         * directo.  XMM regs estan FUERA del regalloc D.7 (que solo conoce
         * GP); las cases del Selector hardcodean XMM0/XMM1 como scratch. */
        MOVQ_GP_XMM = 63,   ///< MOVQ xmm, rGP (66 REX.W 0F 6E /r) -- GP -> XMM
        SQRTSD      = 65,   ///< SQRTSD xmm_dst, xmm_src (F2 0F 51 /r)
        MINSD       = 66,   ///< MINSD  xmm_dst, xmm_src (F2 0F 5D /r) -- NaN: returns src2
        MAXSD       = 67,   ///< MAXSD  xmm_dst, xmm_src (F2 0F 5F /r)
        ROUNDSD     = 68,   ///< ROUNDSD xmm_dst, xmm_src, imm8 (66 0F 3A 0B /r ib)
                            ///<   imm8 modes: 0=round-to-nearest, 1=floor, 2=ceil, 3=trunc.
                            ///<   variant field carga el mode.  Requiere SSE4.1 (universal x86-64).
        MOVQ_XMM_GP = 69,   ///< MOVQ rGP, xmm (66 REX.W 0F 7E /r) -- XMM -> GP

        /* FP arith scalar (Math-IR-promote v2.2b cont). */
        ADDSD       = 70,   ///< ADDSD xmm_dst, xmm_src (F2 0F 58 /r) -- f64 add
        SUBSD       = 71,   ///< SUBSD xmm_dst, xmm_src (F2 0F 5C /r) -- f64 sub
        MULSD       = 72,   ///< MULSD xmm_dst, xmm_src (F2 0F 59 /r) -- f64 mul
        DIVSD       = 73,   ///< DIVSD xmm_dst, xmm_src (F2 0F 5E /r) -- f64 div
        CVTSI2SD    = 74,   ///< CVTSI2SD xmm, r64 (F2 REX.W 0F 2A /r) -- int signed -> f64
        CVTTSD2SI   = 75,   ///< CVTTSD2SI r64, xmm (F2 REX.W 0F 2C /r) -- f64 -> int truncado
        UCOMISD     = 76,   ///< UCOMISD xmm_a, xmm_b (66 0F 2E /r) -- f64 compare, set ZF/PF/CF
        CVTSS2SD    = 77,   ///< CVTSS2SD xmm, xmm (F3 0F 5A /r) -- f32 -> f64 (widen)
        CVTSD2SS    = 78,   ///< CVTSD2SS xmm, xmm (F2 0F 5A /r) -- f64 -> f32 (narrow)

        /* Pseudo D.7 perf (2026-06-06): division/modulo entero en vregs.
         * dst = src1 / src2 (variant 0 = DIV, 1 = MOD).  El rewrite lo
         * expande a la secuencia x86 RAX:RDX-fija usando R11 (scratch
         * reservado) para el divisor, evitando el aliasing operando<->RAX/RDX
         * sin necesitar fixed intervals en el regalloc.  Se marca como
         * call-position (clobber RAX/RDX -> live-across van a callee-saved). */
        DIVMOD_V    = 79,

        /* Pseudo (callback-ABI 2026-06-06): carga el @c ProcessVM* del
         * proceso actual en @c dst.reg (siempre RBX).  Encapsula la
         * decision TLS-direct-vs-call que el thunk hacia a mano:
         *   - src1.value != -1: @c mov dst, gs:[src1.value]  (TLS-direct
         *     Win64; src1.value = 0x1480 + tls_idx*8).
         *   - src1.value == -1: @c mov rax, imm64(get_proc) ; call rax ;
         *     mov dst, rax  (fallback portable; la direccion de
         *     @c get_current_executing_process vive en
         *     @c imm64_pool[src2.value]).
         * El selector resuelve TLS/addr (puede llamar al runtime) y los
         * hornea aqui; el encoder solo emite bytes -> sin dependencia
         * del runtime en el encoder. */
        LOAD_PROC   = 80,

        COUNT       = 81
    };

    /* ===================================================================== */
    /* MInstr (32 bytes, 2 por cache line)                                    */
    /* ===================================================================== */

    /**
     * @struct MInstr
     * @brief Instruccion maquina.  EXACTAMENTE 32 bytes para cache locality.
     *
     * Layout:
     *   +0  [1]  op           MOp
     *   +1  [1]  variant      sub-tipo (MCond para JCC/SETCC, etc.)
     *   +2  [2]  flags        flags de encoding (rex.W override, prefix...)
     *   +4  [4]  source_pc    bytecode_pc original (debug)
     *   +8  [8]  dst          MOperand
     *   +16 [8]  src1         MOperand
     *   +24 [8]  src2         MOperand (para 3-operand ALU)
     */
    struct MInstr {
        MOp      op       = MOp::NOP;
        uint8_t  variant  = 0;
        uint16_t flags    = 0;
        uint32_t source_pc = 0;
        MOperand dst;
        MOperand src1;
        MOperand src2;

        /* Helpers de construccion (zero overhead). */
        static MInstr make_nop() noexcept {
            MInstr i; return i;
        }

        static MInstr make_unary(MOp op, MOperand dst, MOperand src) noexcept {
            MInstr i; i.op = op; i.dst = dst; i.src1 = src; return i;
        }

        static MInstr make_binary(MOp op, MOperand dst, MOperand a, MOperand b) noexcept {
            MInstr i; i.op = op; i.dst = dst; i.src1 = a; i.src2 = b; return i;
        }

        static MInstr make_ret() noexcept {
            MInstr i; i.op = MOp::RET; return i;
        }

        static MInstr make_jmp(uint32_t label_id) noexcept {
            MInstr i; i.op = MOp::JMP; i.src1 = MOperand::make_label(label_id); return i;
        }

        static MInstr make_jcc(MCond cc, uint32_t label_id) noexcept {
            MInstr i; i.op = MOp::JCC; i.variant = static_cast<uint8_t>(cc);
            i.src1 = MOperand::make_label(label_id);
            return i;
        }

        static MInstr make_call_label(uint32_t label_id) noexcept {
            MInstr i; i.op = MOp::CALL;
            i.src1 = MOperand::make_label(label_id);
            return i;
        }

        /** @brief Pseudo ARG: argumento @p idx de la siguiente CALL (D.7). */
        static MInstr make_arg(uint8_t idx, MOperand src) noexcept {
            MInstr i; i.op = MOp::ARG; i.variant = idx; i.src1 = src; return i;
        }

        /** @brief CALL a direccion absoluta (en @c imm64_pool[@p imm64_idx]). */
        static MInstr make_call_abs(uint32_t imm64_idx) noexcept {
            MInstr i; i.op = MOp::CALL_ABS;
            i.src1 = MOperand::make_imm64_idx(imm64_idx);
            return i;
        }

        /**
         * @brief Pseudo LOAD_PROC: carga @c ProcessVM* en @p dst (RBX).
         * @param dst            Registro destino (RBX por convencion).
         * @param tls_gs_disp    Desplazamiento @c gs:[disp] para TLS-direct
         *                       (Win64), o -1 para usar el fallback por call.
         * @param getproc_pool_idx  Indice en @c imm64_pool de la direccion de
         *                       @c get_current_executing_process (usado solo
         *                       en el fallback).
         */
        static MInstr make_load_proc(MReg dst, int32_t tls_gs_disp,
                                     uint32_t getproc_pool_idx) noexcept {
            MInstr i; i.op = MOp::LOAD_PROC;
            i.dst  = MOperand::make_reg(dst);
            i.src1 = MOperand::make_imm32(tls_gs_disp);
            i.src2 = MOperand::make_imm64_idx(getproc_pool_idx);
            return i;
        }

        /** @brief LOAD: @p dst = [@p addr] (host memory, disp 0).  @p width =
         *  1/2/4/8 bytes; @p sgn = sign-extend (i*) vs zero-extend (u*). */
        static MInstr make_load(MOperand dst, MOperand addr,
                                uint8_t width, bool sgn) noexcept {
            MInstr i; i.op = MOp::LOAD; i.dst = dst; i.src1 = addr;
            i.flags = static_cast<uint16_t>((width << 1) | (sgn ? 1u : 0u));
            return i;
        }
        /** @brief STORE: [@p addr] = @p val (host memory, disp 0). */
        static MInstr make_store(MOperand addr, MOperand val,
                                 uint8_t width) noexcept {
            MInstr i; i.op = MOp::STORE; i.src1 = addr; i.src2 = val;
            i.flags = width;
            return i;
        }
        /** @brief ALLOCA: @p dst = host_ptr a @p size bytes del frame JIT. */
        static MInstr make_alloca(MOperand dst, uint32_t size) noexcept {
            MInstr i; i.op = MOp::ALLOCA; i.dst = dst;
            i.src1 = MOperand::make_imm32(static_cast<int32_t>(size));
            return i;
        }

        static MInstr make_label_def(uint32_t label_id) noexcept {
            MInstr i; i.op = MOp::LABEL_DEF;
            i.src1 = MOperand::make_label(label_id);
            return i;
        }

        /**
         * @brief Crea un MInstr SAFEPOINT.  El encoder lo expande a la
         *        secuencia de poll x86-64 documentada en @c MOp::SAFEPOINT.
         *
         * @c handler_imm64_idx es el indice en @c MFunction::imm64_pool
         * que contiene la direccion absoluta de @c vrt_safepoint_handler
         * (resuelta por el Selector via @c RuntimeEntries::safepoint_handler).
         */
        static MInstr make_safepoint(uint32_t handler_imm64_idx) noexcept {
            MInstr i;
            i.op = MOp::SAFEPOINT;
            i.src1 = MOperand::make_imm64_idx(handler_imm64_idx);
            return i;
        }
    };

    static_assert(sizeof(MInstr) == 32, "MInstr debe ser 32 bytes para cache locality");

    /* ===================================================================== */
    /* MBlock                                                                 */
    /* ===================================================================== */

    using MBlockId = uint16_t;
    using MLabelId = uint32_t;

    static constexpr MBlockId MBLOCK_INVALID = static_cast<MBlockId>(-1);
    static constexpr MLabelId MLABEL_INVALID = static_cast<MLabelId>(-1);

    /**
     * @struct MBlock
     * @brief Bloque basico de MachineIR.
     *
     * Las instrucciones viven contiguas en @c instrs (vector).  Las
     * aristas del CFG se representan via @c succs (max 2 sucesores en
     * v1: caso normal o branch + fallthrough).  Predecesores se
     * calculan on-demand (no se mantienen para simplicidad).
     *
     * @c label_id es el label que apunta al inicio del bloque; permite
     * jumps internos a este bloque sin extra book-keeping.
     */
    struct MBlock {
        MLabelId               label_id = MLABEL_INVALID;
        std::vector<MInstr>    instrs;
        MBlockId               succ_a = MBLOCK_INVALID;
        MBlockId               succ_b = MBLOCK_INVALID;
        /// Offset en bytes desde el inicio del code cache donde se
        /// emite la primera instr de este bloque.  Lo poblea el encoder.
        uint32_t               byte_offset = 0;
    };

    /* ===================================================================== */
    /* MFunction                                                              */
    /* ===================================================================== */

    /**
     * @struct MFixup
     * @brief Pendiente de patch: una referencia a @c label_id en el
     *        codigo emitido que se debe rellenar tras conocerse el
     *        offset destino.
     *
     * Layout x86-64: las branches relativas usan @c rel32 al final de
     * la instruccion (justo despues del opcode + ModR/M).  El encoder
     * registra MFixup{label_id, offset_in_bytes, size=4, instr_end=
     * offset+4} y en una segunda pasada calcula el delta y patchea.
     */
    struct MFixup {
        MLabelId label_id   = MLABEL_INVALID;
        uint32_t patch_at   = 0;   ///< byte offset donde escribir rel32
        uint32_t instr_end  = 0;   ///< byte offset del final del instr (base del rel)
        uint8_t  width      = 4;   ///< 1, 2, o 4 (siempre 4 en v1)
    };

    /* ===================================================================== */
    /* Stackmaps (D.2-integration)                                            */
    /* ===================================================================== */

    /**
     * @enum StackmapGcKind
     * @brief Categoria del valor GC almacenado en un slot.
     */
    enum class StackmapGcKind : uint8_t {
        HANDLE  = 0,  ///< GcHandle (uint32_t en los 32 bits bajos del slot)
        HOSTPTR = 1,  ///< host_ptr crudo apuntando al payload de un objeto GC
        STRING  = 2   ///< StringObject* (host_ptr, scan especial)
    };

    /**
     * @struct StackmapSlot
     * @brief Descriptor de un slot stack que contiene un valor GC en un
     *        punto de safepoint determinado.
     *
     * Layout: 4 bytes empaquetados.
     *   +0 [2]  rbp_offset: offset signed desde RBP donde vive el slot
     *                       (tipicamente negativo: -8, -16, -24, ...).
     *   +2 [1]  gc_kind:    StackmapGcKind
     *   +3 [1]  _pad:       alineacion
     */
    struct StackmapSlot {
        int16_t       rbp_offset = 0;
        StackmapGcKind gc_kind   = StackmapGcKind::HANDLE;
        uint8_t       _pad       = 0;
    };

    static_assert(sizeof(StackmapSlot) == 4, "StackmapSlot debe ser 4 bytes");

    /**
     * @struct Stackmap
     * @brief Snapshot de slots GC vivos en un punto de safepoint.
     *
     * Cada @c MOp::SAFEPOINT y cada @c MOp::CALL genera un Stackmap que
     * el Selector poblea con los slots de SSA values @c is_gc_object=true
     * definidos hasta ese punto.  El @c pc_offset se rellena por el
     * encoder (tras saber donde quedo el poll en el code cache).
     *
     * El GC durante un sweep:
     *   1. Walk RBP chain.
     *   2. Por cada frame JIT, lookup en @c JitRegistry para encontrar
     *      su @c MFunction (o equivalente runtime).
     *   3. Binary search en @c stackmaps por @c pc_offset == frame.rip.
     *   4. Para cada slot del stackmap match: leer valor del slot
     *      (@c [rbp + rbp_offset]) y marcar segun @c gc_kind.
     */
    struct Stackmap {
        uint32_t                  pc_offset = 0;  ///< byte offset en code cache
        std::vector<StackmapSlot> slots;
    };

    /**
     * @struct MFunction
     * @brief Funcion completa en MachineIR.
     *
     * Layout efficiente:
     *   - blocks: contiguos en memoria (vector).
     *   - imm64_pool: pool compartido de inmediatos 64-bit.
     *   - fixups: vector de patches pendientes tras la pasada de emit.
     *   - labels: mapping label_id -> byte_offset (poblado por encoder).
     */
    struct MFunction {
        std::string                name;
        std::vector<MBlock>        blocks;
        std::vector<uint64_t>      imm64_pool;
        std::vector<MFixup>        fixups;
        /// Offset de cada label en el code cache.  Indexado por label_id.
        /// Si el label no esta resuelto aun, contiene UINT32_MAX.
        std::vector<uint32_t>      label_offsets;
        /// Tamano del frame stack (bytes) reservado por enter (sub rsp, N).
        /// Lo poblea el selector tras analizar locales/spills.
        uint32_t                   stack_frame_size = 0;
        /// Siguiente label_id disponible para new_label().
        uint32_t                   next_label_id = 0;
        /// D.2-int: stackmaps emitidos por el Selector en cada SAFEPOINT y
        /// CALL.  El encoder rellena @c pc_offset a medida que emite los
        /// bytes correspondientes (@c MOp::SAFEPOINT / @c MOp::CALL).
        ///
        /// Sorted post-encode por @c pc_offset para que el GC pueda hacer
        /// binary search durante stack walk.  Cada Stackmap describe los
        /// slots GC vivos en ese punto especifico.
        std::vector<Stackmap>      stackmaps;

        /// Sprint mem-loop-fix-v2 / fib-recursion (2026-06-02):
        /// indices del @c imm64_pool que contienen referencias a la
        /// PROPIA funcion (self-recursive call).  El encoder, al emitir
        /// un MOV reg,imm64 cuyo @c IMM64_IDX matchea, registra la
        /// posicion del imm64 en @c self_ref_byte_offsets.  Tras alocar
        /// el code cache, el JitCompiler escribe @c code_start en cada
        /// una de esas posiciones para resolver las self-refs.
        std::vector<uint32_t>      self_ref_imm64_indices;
        /// Posiciones (byte offsets) DENTRO de la code emitida donde
        /// hay un imm64 self-ref que necesita patching.  Poblado por
        /// el encoder.
        std::vector<size_t>        self_ref_byte_offsets;

        /// Phase D.7 (regalloc por vregs): numero de registros virtuales
        /// reservados en esta funcion.  Los ids son densos 0..vreg_count-1.
        /// Solo se usa en el path VREG (flag @c VESTA_JIT_VREGS); el path de
        /// slots lo deja en 0.
        uint32_t                   vreg_count = 0;
        /// OSR (Phase D.8): numero de valores IR originales (== fn.values.size()
        /// al compilar).  Los vregs [0, ir_value_count) corresponden 1:1 a IR
        /// value ids (mapeo identidad en @c vr()); los vregs >= ir_value_count
        /// son temporales internos del selector (intra-instruccion, nunca vivos
        /// a traves de un bloque).  El state-transfer del OSR (buffer-por-VID)
        /// SOLO captura vregs < ir_value_count: esos ids son ESTABLES entre C1 y
        /// C2 (el clon C2 preserva los IR VIDs), garantizando que C1 escribe y
        /// C2 lee la misma celda del buffer para el mismo valor logico.
        uint32_t                   ir_value_count = 0;
        /// Phase D.7: clase (GP/FP) de cada registro virtual, indexado por
        /// vreg id.  @c vreg_class.size() == @c vreg_count.  El register
        /// allocator la consulta para asignar del pool fisico correcto.
        std::vector<RegClass>      vreg_class;
        /// Phase D.7 commit 5f: 1 si el vreg contiene un valor GESTIONADO por
        /// el GC (handle/host_ptr a objeto GC).  Lo poblea el selector desde
        /// @c IrValue::is_gc_object.  El pipeline lo usa para rechazar (sin
        /// stackmaps todavia) funciones donde un valor GC esta VIVO a traves
        /// de un call -> el GC no veria esa raiz si esta en un registro.
        /// @c vreg_is_gc.size() == @c vreg_count cuando esta poblado.
        std::vector<uint8_t>       vreg_is_gc;

        /** @brief Reserva un nuevo label_id (zero overhead). */
        MLabelId new_label() {
            const uint32_t id = next_label_id++;
            if (label_offsets.size() <= id) {
                label_offsets.resize(id + 1, UINT32_MAX);
            }
            return id;
        }

        /** @brief Crea un nuevo bloque vacio.  Devuelve su id. */
        MBlockId new_block(MLabelId lbl) {
            MBlock b;
            b.label_id = lbl;
            blocks.push_back(std::move(b));
            return static_cast<MBlockId>(blocks.size() - 1);
        }

        /** @brief Anyade un imm64 al pool y devuelve su indice. */
        uint32_t intern_imm64(uint64_t value) {
            /* O(n) lookup para deduplicar.  Aceptable para el v1: el
             * codigo Vex tipico tiene < 100 imm64 distintos por funcion. */
            for (uint32_t i = 0; i < imm64_pool.size(); ++i) {
                if (imm64_pool[i] == value) return i;
            }
            imm64_pool.push_back(value);
            return static_cast<uint32_t>(imm64_pool.size() - 1);
        }

        /**
         * @brief Reserva un nuevo registro virtual de la clase dada (D.7).
         *
         * Devuelve el operando @c VREG listo para usar como dst/src de una
         * MInstr.  Los temporales que introduce el selector (no derivados
         * directamente de un SSA value) se piden por aqui.
         *
         * @param cls    Clase del registro (GP/FP).
         * @param width  Ancho en bytes (1/2/4/8).  Default 8.
         * @return       Operando VREG con el id recien reservado.
         */
        MOperand new_vreg(RegClass cls, uint8_t width = 8) {
            const uint32_t id = vreg_count++;
            vreg_class.push_back(cls);
            return MOperand::make_vreg(id, cls, width);
        }
    };

} // namespace jit

#endif // VESTA_JIT_MACHINE_IR_H
