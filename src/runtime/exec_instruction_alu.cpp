/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */
/**
 * @file exec_instruction_alu.cpp
 * @brief Implementacion de las instrucciones ALU, logicas, saltos y movimiento de VestaVM.
 *
 * Implementa las estructuras Op (AddOp, SubOp, CmpOp, MulOp, DivOp, AndOp, OrOp, XorOp,
 * ShlOp, ShrOp, SarOp, IncOp, DecOp, NotOp) y las funciones:
 *  - @c alu_core() / @c alu_core_unary() : nucleo de ejecucion ALU con flags
 *  - @c exec_instr_add/sub/mul/div/cmp/and/or/xor/shl/shr/sar/not/inc/dec : despacho de ALU
 *  - @c exec_instr_mov_reg / @c exec_instr_mov_imm / @c exec_instr_mov_sib : instrucciones MOV
 *  - @c exec_instr_movc / @c exec_instr_movc_reg                           : MOV condicional
 *  - @c exec_instr_push / @c exec_instr_pop                                : pila
 *  - @c exec_instr_jcc / @c exec_instr_jmp / @c exec_instr_jrel            : saltos
 *  - @c exec_instr_call / @c exec_instr_ret                                : llamada/retorno
 *  - @c exec_instr_callvm / @c exec_instr_calln / @c exec_instr_enter      : llamadas VM/nativas
 *  - @c exec_instr_syscall / @c exec_instr_int                             : llamadas al sistema
 */
#include "runtime/exec_instruction.h"

namespace runtime {

// =========================================================================
// Concepto Op (informal):
//   static constexpr bool is_compare;
//   static T compute(T a, T b);          operaciones binarias
//   static T compute(T a);               operaciones unarias
//   static void flags(ProcessVM*, T a, T b, T result, bool is_signed);  binaria
//   static void flags(ProcessVM*, T a, T result);                        unaria
// =========================================================================

// -------------------------------------------------------------------------
// LogicFlagsBase - flags() compartido para operaciones bitwise (AND, OR, XOR)
// -------------------------------------------------------------------------

/**
 * @brief Comportamiento comun de flags para operaciones binarias bitwise.
 *
 * AND / OR / XOR siempre limpian CF y OF (semantica Intel x86).
 * Las estructuras Op derivadas solo necesitan proveer compute(); flags() se hereda.
 */
struct LogicFlagsBase {
    /**
     * @brief Limpia CF y OF; ZF/SF son manejados por compute_with_flags.
     * @tparam T Ancho entero sin signo de los operandos.
     */
    template<typename T>
    static inline void flags(ProcessVM *vm, T /*a*/, T /*b*/, T /*result*/, bool /*is_signed*/) {
        vm->registers.flags.bits.CF = 0; // las operaciones bitwise nunca producen acarreo
        vm->registers.flags.bits.OF = 0; // las operaciones bitwise nunca desbordan
    }
};

// =========================================================================
// Estructuras Op unarias
// =========================================================================

/**
 * @brief INC: incrementa el operando en 1.
 *
 * CF se preserva (no se modifica) para coincidir con la semantica INC de x86.
 * OF se limpia porque INC se trata siempre como sin signo aqui.
 * ZF se establece explicitamente en flags() para evitar calcularlo dos veces.
 */
struct IncOp {
    static constexpr bool is_compare = false; // el resultado se escribe de vuelta

    /** @brief Returns a + 1. */
    template<typename T>
    static inline T compute(T a) { return a + 1; }

    /** @brief Sets ZF; preserves CF; clears OF. */
    template<typename T>
    static inline void flags(ProcessVM *vm, T /*a*/, T result) {
        vm->registers.flags.bits.CF = vm->registers.flags.bits.CF; // CF preservado (asignacion sin efecto)
        vm->registers.flags.bits.OF = 0;                           // INC sin signo no tiene flag de desbordamiento
        vm->registers.flags.bits.ZF = (result == 0);               // establecer ZF si el resultado es cero
    }
};

/**
 * @brief DEC: decrementa el operando en 1.
 *
 * Misma semantica CF/OF/ZF que INC, coincidiendo con el comportamiento DEC de x86.
 */
struct DecOp {
    static constexpr bool is_compare = false; // el resultado se escribe de vuelta

    /** @brief Returns a - 1. */
    template<typename T>
    static inline T compute(T a) { return a - 1; }

    /** @brief Sets ZF; preserves CF; clears OF. */
    template<typename T>
    static inline void flags(ProcessVM *vm, T /*a*/, T result) {
        vm->registers.flags.bits.CF = vm->registers.flags.bits.CF; // CF preservado
        vm->registers.flags.bits.OF = 0;                           // DEC sin signo no tiene flag de desbordamiento
        vm->registers.flags.bits.ZF = (result == 0);               // establecer ZF si el resultado es cero
    }
};

/**
 * @brief NOT: complemento bit a bit.
 *
 * CF y OF se limpian (misma convencion que AND/OR/XOR).
 */
struct NotOp {
    static constexpr bool is_compare = false; // el resultado se escribe de vuelta

    /** @brief Returns ~a. */
    template<typename T>
    static inline T compute(T a) { return ~a; }

    /** @brief Clears CF and OF; ZF/SF are set generically by alu_core_unary. */
    template<typename T>
    static inline void flags(ProcessVM *vm, T /*a*/, T /*result*/) {
        vm->registers.flags.bits.CF = 0; // NOT bitwise limpia el acarreo
        vm->registers.flags.bits.OF = 0; // NOT bitwise limpia el desbordamiento
    }
};

// =========================================================================
// Estructuras Op binarias
// =========================================================================

/**
 * @brief ADD: suma con deteccion de acarreo/desbordamiento.
 *
 * - Modo con signo:  OF = desbordamiento con signo, CF = 0.
 * - Modo sin signo: CF = acarreo sin signo, OF = 0.
 */
struct AddOp {
    static constexpr bool is_compare = false; // el resultado se escribe de vuelta

    /** @brief Returns a + b. */
    template<typename T>
    static inline T compute(T a, T b) { return a + b; }

    /** @brief Sets CF or OF depending on is_signed. */
    template<typename T>
    static inline void flags(ProcessVM *vm, T a, T b, T result, bool is_signed) {
        using ST = std::make_signed_t<T>;   // vista con signo para verificacion de desbordamiento
        using UT = std::make_unsigned_t<T>; // vista sin signo para verificacion de acarreo
        if (is_signed) {
            ST sa                       = (ST)a, sb = (ST)b, sres = (ST)result;
            vm->registers.flags.bits.OF = ((sa ^ sres) & (sb ^ sres)) < 0; // regla de desbordamiento con signo
            vm->registers.flags.bits.CF = 0;                                // IMUL no activa CF
        } else {
            UT ua                       = (UT)a, ub = (UT)b;
            vm->registers.flags.bits.CF = ((UT)(ua + ub)) < ua; // acarreo si hubo vuelta al inicio
            vm->registers.flags.bits.OF = 0;                    // ADD sin signo no activa OF
        }
    }
};

/**
 * @brief SUB: resta con deteccion de prestamo/desbordamiento.
 *
 * - Modo con signo:  OF = desbordamiento con signo, CF = 0.
 * - Modo sin signo: CF = prestamo (a < b), OF = 0.
 */
struct SubOp {
    static constexpr bool is_compare = false; // el resultado se escribe de vuelta

    /** @brief Returns a - b. */
    template<typename T>
    static inline T compute(T a, T b) { return a - b; }

    /** @brief Sets CF (borrow) or OF (signed overflow) depending on is_signed. */
    template<typename T>
    static inline void flags(ProcessVM *vm, T a, T b, T result, bool is_signed) {
        using ST = std::make_signed_t<T>;   // vista con signo para verificacion de desbordamiento
        using UT = std::make_unsigned_t<T>; // vista sin signo para verificacion de prestamo
        if (is_signed) {
            ST sa                       = (ST)a, sb = (ST)b, sres = (ST)result;
            vm->registers.flags.bits.OF = ((sa ^ sb) & (sa ^ sres)) < 0; // regla de desbordamiento de SUB con signo
            vm->registers.flags.bits.CF = 0;                              // SUB con signo no activa CF
        } else {
            UT ua                       = (UT)a, ub = (UT)b;
            vm->registers.flags.bits.CF = ua < ub; // prestamo cuando a < b
            vm->registers.flags.bits.OF = 0;       // SUB sin signo no activa OF
        }
    }
};

/**
 * @brief CMP: comparacion implementada como SUB sin escribir el resultado.
 *
 * is_compare = true impide que alu_core almacene el resultado.
 * Los flags son identicos a SubOp.
 */
struct CmpOp {
    static constexpr bool is_compare = true; // el resultado se descarta (solo comparacion)

    /** @brief Returns a - b (result is discarded by alu_core). */
    template<typename T>
    static inline T compute(T a, T b) { return a - b; }

    /** @brief Delegates to SubOp::flags (identical subtraction semantics). */
    template<typename T>
    static inline void flags(ProcessVM *vm, T a, T b, T result, bool is_signed) {
        SubOp::flags(vm, a, b, result, is_signed); // reutilizar la logica de flags de SUB
    }
};

/**
 * @brief MUL / IMUL: multiplicacion con deteccion de desbordamiento.
 *
 * - Con signo (IMUL): OF = desbordamiento con signo, CF = 0.
 * - Sin signo (MUL): CF = OF = flag de desbordamiento.
 */
struct MulOp {
    static constexpr bool is_compare = false; // el resultado se escribe de vuelta

    /** @brief Returns a * b. */
    template<typename T>
    static inline T compute(T a, T b) { return a * b; }

    /** @brief Detects multiplication overflow and sets CF/OF accordingly. */
    template<typename T>
    static inline void flags(ProcessVM *vm, T a, T b, T result, bool is_signed) {
        using ST = std::make_signed_t<T>;   // vista con signo para verificacion de IMUL
        using UT = std::make_unsigned_t<T>; // vista sin signo para verificacion de MUL
        bool overflow = false;              // tentativamente sin desbordamiento

        if (is_signed) {
            ST sa = (ST)a;
            ST sb = (ST)b;
            constexpr ST S_MIN = std::numeric_limits<ST>::min(); // valor mas negativo
            constexpr ST S_MAX = std::numeric_limits<ST>::max(); // valor mas positivo

            if (sa == 0 || sb == 0) {
                overflow = false; // el producto es cero, sin desbordamiento
            } else if (sa == -1) {
                overflow = (sb == S_MIN); // -1 * INT_MIN desborda
            } else if (sb == -1) {
                overflow = (sa == S_MIN); // INT_MIN * -1 desborda
            } else {
                // verificacion general de desbordamiento con signo usando division
                overflow = (sa > 0)
                               ? (sb > 0 ? sa > S_MAX / sb : sb < S_MIN / sa)
                               : (sb > 0 ? sa < S_MIN / sb : sa < S_MAX / sb);
            }
            vm->registers.flags.bits.OF = overflow; // IMUL activa OF en desbordamiento
            vm->registers.flags.bits.CF = 0;        // IMUL limpia CF
        } else {
            UT ua = (UT)a;
            UT ub = (UT)b;
            if (ua == 0 || ub == 0) {
                overflow = false; // trivialmente sin desbordamiento
            } else {
                constexpr UT U_MAX = std::numeric_limits<UT>::max(); // valor maximo sin signo
                overflow           = ua > (U_MAX / ub);              // desbordamiento si el cociente supera al divisor
            }
            vm->registers.flags.bits.CF = overflow; // MUL activa CF en desbordamiento
            vm->registers.flags.bits.OF = overflow; // MUL tambien activa OF en desbordamiento
        }
    }
};

/**
 * @brief DIV / IDIV: division entera.
 *
 * Solo se devuelve el cociente; el resto se descarta.
 * Division por cero o desbordamiento con signo establece OF y CF.
 */
struct DivOp {
    static constexpr bool is_compare = false; // el cociente se escribe de vuelta

    /** @brief Returns a / b (quotient only; divide-by-zero must be checked by caller). */
    template<typename T>
    static inline T compute(T a, T b) { return a / b; }

    /** @brief Sets OF/CF on divide-by-zero or IDIV INT_MIN/-1. */
    template<typename T>
    static inline void flags(ProcessVM *vm, T a, T b, T result, bool is_signed) {
        using ST = std::make_signed_t<T>; // vista con signo para verificacion de desbordamiento IDIV
        if (b == 0) {
            // la division por cero es una condicion excepcional
            vm->registers.flags.bits.OF = 1; // senalizar desbordamiento
            vm->registers.flags.bits.CF = 1; // senalizar acarreo
            return;
        }
        if (is_signed) {
            ST sa = (ST)a;
            ST sb = (ST)b;
            if (sa == std::numeric_limits<ST>::min() && sb == -1) {
                // IDIV INT_MIN / -1 desbordaria el registro de cociente
                vm->registers.flags.bits.OF = 1; // desbordamiento
                vm->registers.flags.bits.CF = 1; // acarreo
                return;
            }
            vm->registers.flags.bits.OF = 0; // division normal con signo
            vm->registers.flags.bits.CF = 0; // sin acarreo
        } else {
            vm->registers.flags.bits.OF = 0; // la division sin signo nunca desborda (excepto por cero, manejado arriba)
            vm->registers.flags.bits.CF = 0; // sin acarreo
        }
        (void)result; // resultado usado por alu_core; suprimir advertencia de parametro no usado
    }
};

// Operaciones binarias bitwise -- todas heredan LogicFlagsBase (CF=OF=0).

/** @brief AND: bitwise AND. Clears CF and OF via LogicFlagsBase. */
struct AndOp : LogicFlagsBase {
    static constexpr bool is_compare = false;
    template<typename T> static inline T compute(T a, T b) { return a & b; }
};

/** @brief OR: bitwise OR. Clears CF and OF via LogicFlagsBase. */
struct OrOp : LogicFlagsBase {
    static constexpr bool is_compare = false;
    template<typename T> static inline T compute(T a, T b) { return a | b; }
};

/** @brief XOR: bitwise XOR. Clears CF and OF via LogicFlagsBase. */
struct XorOp : LogicFlagsBase {
    static constexpr bool is_compare = false;
    template<typename T> static inline T compute(T a, T b) { return a ^ b; }
};

/**
 * @brief SHL: desplazamiento logico a la izquierda.
 *
 * CF = ultimo bit desplazado fuera del lado MSB.
 * OF = CF XOR MSB(resultado)  (definido solo para desplazamiento de 1).
 */
struct ShlOp {
    static constexpr bool is_compare = false;

    /** @brief Returns (unsigned)a << (b & (bits-1)). */
    template<typename T>
    static inline T compute(T a, T b) {
        using UT       = std::make_unsigned_t<T>;       // sin signo para comportamiento de desplazamiento definido
        uint32_t shift = (uint32_t)b & (sizeof(T)*8-1); // enmascarar el conteo de desplazamiento al rango valido
        return (T)((UT)a << shift);
    }

    /** @brief Computes CF from the vacated bit and OF from MSB after shift. */
    template<typename T>
    static inline void flags(ProcessVM *vm, T a, T b, T result, bool) {
        using UT       = std::make_unsigned_t<T>;
        uint32_t shift = (uint32_t)b & (sizeof(T)*8-1); // mismo enmascarado que compute()
        if (shift == 0) {
            vm->registers.flags.bits.CF = 0; // desplazamiento cero deja CF limpio
            vm->registers.flags.bits.OF = 0; // desplazamiento cero deja OF limpio
            return;
        }
        UT ua                       = (UT)a;
        UT cf_bit                   = (ua >> (sizeof(T)*8 - shift)) & 1; // bit que fue desplazado fuera
        vm->registers.flags.bits.CF = cf_bit;                            // acarreo = bit desplazado fuera
        UT msb                      = ((UT)result >> (sizeof(T)*8 - 1)) & 1;
        vm->registers.flags.bits.OF = (cf_bit ^ msb); // desbordamiento = CF XOR nuevo MSB (regla SHL de x86)
    }
};

/**
 * @brief SHR: desplazamiento logico a la derecha (sin signo).
 *
 * CF = ultimo bit desplazado fuera del lado LSB.
 * OF se limpia (regla x86 para SHR).
 */
struct ShrOp {
    static constexpr bool is_compare = false;

    /** @brief Returns (unsigned)a >> (b & (bits-1)). */
    template<typename T>
    static inline T compute(T a, T b) {
        using UT       = std::make_unsigned_t<T>;        // desplazamiento derecho sin signo rellena con 0
        uint32_t shift = (uint32_t)b & (sizeof(T)*8-1); // enmascarar el conteo de desplazamiento
        return (T)((UT)a >> shift);
    }

    /** @brief Sets CF = bit shifted out; clears OF. */
    template<typename T>
    static inline void flags(ProcessVM *vm, T a, T b, T /*result*/, bool) {
        using UT       = std::make_unsigned_t<T>;
        uint32_t shift = (uint32_t)b & (sizeof(T)*8-1); // mismo enmascarado que compute()
        if (shift == 0) {
            vm->registers.flags.bits.CF = 0; // desplazamiento cero deja CF limpio
            vm->registers.flags.bits.OF = 0; // desplazamiento cero deja OF limpio
            return;
        }
        UT ua                       = (UT)a;
        vm->registers.flags.bits.CF = (ua >> (shift - 1)) & 1; // ultimo bit desplazado fuera
        vm->registers.flags.bits.OF = 0;                        // SHR nunca activa OF
    }
};

/**
 * @brief SAR: desplazamiento aritmetico a la derecha (con signo).
 *
 * Rellena con el bit de signo (extension de signo), equivalente a division con signo por 2^shift.
 * CF = ultimo bit desplazado fuera; OF = 0.
 */
struct SarOp {
    static constexpr bool is_compare = false;

    /** @brief Returns (signed)a >> (b & (bits-1)) with sign extension. */
    template<typename T>
    static inline T compute(T a, T b) {
        using ST       = std::make_signed_t<T>;          // desplazamiento derecho con signo para extension de signo
        uint32_t shift = (uint32_t)b & (sizeof(T)*8-1); // enmascarar el conteo de desplazamiento
        return (T)((ST)a >> shift);
    }

    /** @brief Sets CF = last bit shifted out; clears OF. */
    template<typename T>
    static inline void flags(ProcessVM *vm, T a, T b, T /*result*/, bool) {
        using UT       = std::make_unsigned_t<T>;
        uint32_t shift = (uint32_t)b & (sizeof(T)*8-1); // mismo enmascarado que compute()
        if (shift == 0) {
            vm->registers.flags.bits.CF = 0; // desplazamiento cero deja CF limpio
            vm->registers.flags.bits.OF = 0; // desplazamiento cero deja OF limpio
            return;
        }
        UT ua                       = (UT)a;
        vm->registers.flags.bits.CF = (ua >> (shift - 1)) & 1; // ultimo bit desplazado fuera
        vm->registers.flags.bits.OF = 0;                        // SAR nunca activa OF (regla x86)
    }
};

// =========================================================================
// Nucleo auxiliar ALU: compute_with_flags
// Centraliza la actualizacion de ZF/SF compartida por alu_core, binary_mem_imm_wrapper,
// y sib_mem_dst_wrapper para que la logica este en un solo lugar.
// =========================================================================

/**
 * @brief Ejecuta Op sobre (a, b), actualiza ZF/SF/CF/OF y devuelve el resultado.
 *
 * ZF y SF siempre se derivan del resultado.
 * CF y OF son especificos de Op y los establece Op::flags().
 *
 * @tparam T         Ancho entero sin signo de los operandos.
 * @tparam Op        Estructura de operacion que provee compute() y flags().
 * @param  vm        Puntero a la maquina virtual.
 * @param  a         Primer operando (destino / acumulador).
 * @param  b         Segundo operando (fuente / inmediato).
 * @param  is_signed Selecciona semantica de flags con signo vs sin signo.
 * @return           El resultado calculado (mismo tipo T).
 */
template<typename T, typename Op>
inline T compute_with_flags(ProcessVM *vm, T a, T b, bool is_signed) {
    using UT              = std::make_unsigned_t<T>;     // vista sin signo para verificaciones de flags a nivel de bits
    T res                 = Op::compute(a, b);           // ejecutar la operacion
    vm->registers.flags.bits.ZF = (res == 0);            // ZF: el resultado es cero
    constexpr int SIGN_BIT       = sizeof(T) * 8 - 1;   // indice del bit de mayor peso (signo)
    vm->registers.flags.bits.SF  = (static_cast<UT>(res) >> SIGN_BIT) & 1; // SF: bit de signo del resultado
    Op::flags(vm, a, b, res, is_signed);                 // actualizacion CF/OF especifica de Op
    return res;                                          // el llamante decide si almacenar el resultado
}

// =========================================================================
// alu_core -- ALU binaria con destino de registro
// =========================================================================

/**
 * @brief Nucleo ALU binario: calcula Op(a, b), actualiza flags y escribe en un registro.
 *
 * @tparam T             Ancho entero sin signo de los operandos.
 * @tparam Op            Estructura de operacion (compute + flags).
 * @param  vm            Puntero a la maquina virtual.
 * @param  a             Primer operando (valor actual del registro destino).
 * @param  b             Segundo operando (inmediato, valor de registro o de memoria).
 * @param  is_signed     Selecciona semantica con signo vs sin signo para CF/OF.
 * @param  dst_reg_index Indice del registro destino en vm->registers.regs[].
 *
 * @note Cuando Op::is_compare == true el resultado NO se escribe de vuelta (semantica CMP).
 */
template<typename T, typename Op>
inline void alu_core(ProcessVM *vm, T a, T b, bool is_signed, int dst_reg_index) {
    T result = compute_with_flags<T, Op>(vm, a, b, is_signed); // calcular y actualizar flags
    if constexpr (!Op::is_compare) {
        // escribir el resultado de vuelta en el registro destino al ancho correcto
        auto &dst = vm->registers.regs[dst_reg_index];
        if constexpr (sizeof(T) == 1)      dst.byte_lo((uint8_t)result);  // escritura de 8 bits
        else if constexpr (sizeof(T) == 2) dst.word_lo((uint16_t)result); // escritura de 16 bits
        else if constexpr (sizeof(T) == 4) dst.dword_lo((uint32_t)result);// escritura de 32 bits
        else                               dst.qword((uint64_t)result);    // escritura de 64 bits
    }
}

// =========================================================================
// alu_core_unary -- ALU unaria con destino de registro
// =========================================================================

/**
 * @brief Nucleo ALU unario: calcula Op(a), actualiza ZF/SF y escribe en un registro.
 *
 * ZF y SF se establecen genericamente aqui; Op::flags() maneja CF/OF.
 *
 * @tparam T             Ancho entero sin signo.
 * @tparam Op            Estructura de operacion unaria (compute + flags, sin parametro is_signed).
 * @param  vm            Puntero a la maquina virtual.
 * @param  a             Operando (valor actual del registro destino).
 * @param  dst_reg_index Indice del registro destino.
 */
template<typename T, typename Op>
inline void alu_core_unary(ProcessVM *vm, T a, int dst_reg_index) {
    using UT = std::make_unsigned_t<T>;                              // vista sin signo para extraccion de SF
    T result = Op::compute(a);                                       // aplicar la operacion unaria
    vm->registers.flags.bits.ZF = (result == 0);                     // ZF: el resultado es cero
    constexpr int SIGN_BIT       = sizeof(T) * 8 - 1;               // indice del bit de signo
    vm->registers.flags.bits.SF  = (static_cast<UT>(result) >> SIGN_BIT) & 1; // SF: sign bit
    Op::flags(vm, a, result);                                        // CF/OF update (Op-specific)
    auto &dst = vm->registers.regs[dst_reg_index];                   // referencia al registro destino
    if constexpr (sizeof(T) == 1)      dst.byte_lo((uint8_t)result); // escritura de 8 bits
    else if constexpr (sizeof(T) == 2) dst.word_lo((uint16_t)result);// escritura de 16 bits
    else if constexpr (sizeof(T) == 4) dst.dword_lo((uint32_t)result);// escritura de 32 bits
    else                               dst.qword((uint64_t)result);   // escritura de 64 bits
}

// =========================================================================
// Aliases de tipo para tablas de despacho
// =========================================================================

typedef GeneralRegister &Reg; // abreviatura para referencia de registro en firmas de tabla

/** @brief Signature for binary reg-reg (or reg-imm via wrapper) dispatch functions. */
using BinaryFn = void(*)(ProcessVM *, Reg, Reg, bool, int);

/** @brief Signature for unary (INC/DEC/NOT) dispatch functions. */
using UnaryFn  = void(*)(ProcessVM *, Reg, int);

/** @brief Signature for binary immediate dispatch functions. */
using BinaryImmFn = void(*)(ProcessVM *, Reg &, uint64_t, bool, int);

/** @brief Signature for binary memory+immediate dispatch functions. */
using BinaryMemImmFn = void(*)(ProcessVM *, uint64_t, uint64_t, bool);

// =========================================================================
// Plantillas wrapper -- adaptan los nucleos ALU genericos a tipos de tabla de despacho
// =========================================================================

/**
 * @brief Adapta alu_core para despacho de operandos inmediatos.
 *        Extrae el valor bruto de dst y convierte imm a T antes de llamar a alu_core.
 */
template<typename T, typename Op>
static void binary_imm_wrapper(ProcessVM *vm, Reg &dst, uint64_t imm, bool is_signed, int rdst) {
    alu_core<T, Op>(vm, dst.raw(), (T)imm, is_signed, rdst); // delegar con tipos correctos
}

/**
 * @brief Adapta compute_with_flags para operaciones inmediatas con destino de memoria.
 *        Lee de la memoria virtual, aplica Op y escribe el resultado (excepto CmpOp).
 */
template<typename T, typename Op>
static void binary_mem_imm_wrapper(ProcessVM *vm, uint64_t addr, uint64_t imm, bool is_signed) {
    T val = vm->vm_mem.read_any<T>(addr);                         // cargar el valor actual de la memoria VM
    T res = compute_with_flags<T, Op>(vm, val, (T)imm, is_signed);// calcular y actualizar flags
    if constexpr (!Op::is_compare)
        vm->vm_mem.write_any<T>(addr, res);                       // escribir de vuelta a menos que sea CMP
}

/**
 * @brief Wrapper MOV reg-a-reg: copia src a dst al ancho T; NO toca los flags.
 */
template<typename T>
static void mov_wrapper(ProcessVM *vm, Reg dst, Reg src, bool /*unused*/, int rdst) {
    T     value = src.raw();                      // leer registro fuente como valor de ancho T
    auto &d     = vm->registers.regs[rdst];       // referencia al registro destino
    if constexpr (sizeof(T) == 1)      d.byte_lo((uint8_t)value);  // copia de 8 bits
    else if constexpr (sizeof(T) == 2) d.word_lo((uint16_t)value); // copia de 16 bits
    else if constexpr (sizeof(T) == 4) d.dword_lo((uint32_t)value);// copia de 32 bits
    else                               d.qword((uint64_t)value);    // copia de 64 bits
    // MOV no modifica ningun flag
}

/**
 * @brief Adapta alu_core para despacho binario registro-registro.
 */
template<typename T, typename Op>
static void binary_wrapper(ProcessVM *vm, Reg &dst, Reg &src, bool is_signed, int rdst) {
    alu_core<T, Op>(vm, dst.raw(), src.raw(), is_signed, rdst); // delegar con ambos valores de registro
}

/**
 * @brief Adapta alu_core_unary para despacho unario.
 */
template<typename T, typename Op>
static void unary_wrapper(ProcessVM *vm, Reg &dst, int rdst) {
    alu_core_unary<T, Op>(vm, dst.raw(), rdst); // delegar con valor de registro
}

// =========================================================================
// Macros de declaracion de tablas de despacho
// Cada macro produce un array static constexpr de cuatro punteros a funcion
// (uno por ancho de operando: 8, 16, 32, 64 bits).
// =========================================================================

/** @brief Declares a binary reg-reg dispatch table for Op. */
#define DECL_BIN_TABLE(name, Op) \
    static constexpr BinaryFn name##_table[] = { \
        &binary_wrapper<uint8_t,  Op>, \
        &binary_wrapper<uint16_t, Op>, \
        &binary_wrapper<uint32_t, Op>, \
        &binary_wrapper<uint64_t, Op>  \
    }

/** @brief Declares a unary dispatch table for Op. */
#define DECL_UNARY_TABLE(name, Op) \
    static constexpr UnaryFn name##_table[] = { \
        &unary_wrapper<uint8_t,  Op>, \
        &unary_wrapper<uint16_t, Op>, \
        &unary_wrapper<uint32_t, Op>, \
        &unary_wrapper<uint64_t, Op>  \
    }

/** @brief Declares the paired reg-imm and mem-imm dispatch tables for Op. */
#define DECL_IMM_TABLES(name, Op) \
    static constexpr BinaryImmFn name##_imm_table[] = { \
        &binary_imm_wrapper<uint8_t,  Op>, \
        &binary_imm_wrapper<uint16_t, Op>, \
        &binary_imm_wrapper<uint32_t, Op>, \
        &binary_imm_wrapper<uint64_t, Op>  \
    }; \
    static constexpr BinaryMemImmFn name##_mem_imm_table[] = { \
        &binary_mem_imm_wrapper<uint8_t,  Op>, \
        &binary_mem_imm_wrapper<uint16_t, Op>, \
        &binary_mem_imm_wrapper<uint32_t, Op>, \
        &binary_mem_imm_wrapper<uint64_t, Op>  \
    }

// --- Tablas registro-registro (11 operaciones) ---
DECL_BIN_TABLE(mul, MulOp);
DECL_BIN_TABLE(add, AddOp);
DECL_BIN_TABLE(sub, SubOp);
DECL_BIN_TABLE(cmp, CmpOp);
DECL_BIN_TABLE(div, DivOp);
DECL_BIN_TABLE(and, AndOp);
DECL_BIN_TABLE(or,  OrOp);
DECL_BIN_TABLE(xor, XorOp);
DECL_BIN_TABLE(shl, ShlOp);
DECL_BIN_TABLE(shr, ShrOp);
DECL_BIN_TABLE(sar, SarOp);

// mov_table es especial: usa mov_wrapper, no binary_wrapper
static constexpr BinaryFn mov_table[] = {
    &mov_wrapper<uint8_t>,   // MOV de 8 bits
    &mov_wrapper<uint16_t>,  // MOV de 16 bits
    &mov_wrapper<uint32_t>,  // MOV de 32 bits
    &mov_wrapper<uint64_t>   // MOV de 64 bits
};

// --- Tablas unarias (3 operaciones) ---
DECL_UNARY_TABLE(inc, IncOp);
DECL_UNARY_TABLE(dec, DecOp);
DECL_UNARY_TABLE(not, NotOp);

// --- Tablas de inmediatos (5 operaciones emparejadas) ---
DECL_IMM_TABLES(add, AddOp);
DECL_IMM_TABLES(sub, SubOp);
DECL_IMM_TABLES(mul, MulOp);
DECL_IMM_TABLES(div, DivOp);
DECL_IMM_TABLES(cmp, CmpOp);

#undef DECL_BIN_TABLE
#undef DECL_UNARY_TABLE
#undef DECL_IMM_TABLES

// =========================================================================
// Macro de funcion exec para inmediatos
// Genera exec_instr_<nombre>_imm:
//   direction==0 -> el operando es un registro
//   direction==1 -> el operando es una direccion de memoria (reg base, sin indice/escala)
// =========================================================================

/**
 * @brief Macro que genera una funcion exec_instr_<nombre>_imm.
 *
 * @param name     Mnemonic de instruccion (ej: add, sub).
 * @param imm_tbl  Nombre de la tabla de despacho BinaryImmFn.
 * @param mem_tbl  Nombre de la tabla de despacho BinaryMemImmFn.
 */
#define DEFINE_IMM_EXEC(name, imm_tbl, mem_tbl)                                     \
    void exec_instr_##name##_imm(ProcessVM *vm, const DecodedInstr &instr) {         \
        const int rdst = instr.data_instruction.inmmed_data.reg;                     \
        uint64_t  imm  = instr.data_instruction.inmmed_data.inmmed;                  \
        if (instr.flags_info.direction == 0) {                                       \
            /* direction=0: el destino es un registro */                             \
            imm_tbl[instr.flags_info.mode](                                          \
                vm, vm->registers.regs[rdst], imm,                                  \
                instr.flags_info._signed_instruct, rdst);                            \
            return;                                                                  \
        }                                                                            \
        /* direction=1: el destino es memoria en la direccion almacenada en el registro rdst */  \
        uint64_t addr = vm->registers.regs[rdst].raw();                              \
        mem_tbl[instr.flags_info.mode](vm, addr, imm, instr.flags_info._signed_instruct); \
    }

DEFINE_IMM_EXEC(add, add_imm_table, add_mem_imm_table)
DEFINE_IMM_EXEC(sub, sub_imm_table, sub_mem_imm_table)
DEFINE_IMM_EXEC(mul, mul_imm_table, mul_mem_imm_table)
DEFINE_IMM_EXEC(div, div_imm_table, div_mem_imm_table)
DEFINE_IMM_EXEC(cmp, cmp_imm_table, cmp_mem_imm_table)

#undef DEFINE_IMM_EXEC

// =========================================================================
// Macros de funciones exec registro-registro
// DEFINE_REG_EXEC:        pasa is_signed desde los flags de instruccion
// DEFINE_REG_EXEC_NOSIGN: siempre pasa false (operaciones bitwise y de desplazamiento)
// =========================================================================

/**
 * @brief Genera exec_instr_<nombre>_reg para operaciones binarias con/sin signo.
 *        Lee reg1 (dst) y reg2 (src) de la instruccion decodificada.
 */
#define DEFINE_REG_EXEC(name) \
    void exec_instr_##name##_reg(ProcessVM *vm, const DecodedInstr &instr) { \
        const int rdst = instr.data_instruction.reg_data.reg1; /* indice del registro destino */ \
        const int rsrc = instr.data_instruction.reg_data.reg2; /* indice del registro fuente */      \
        name##_table[instr.flags_info.mode](                                                    \
            vm, vm->registers.regs[rdst], vm->registers.regs[rsrc],                            \
            instr.flags_info._signed_instruct, rdst);  /* despacho por modo (ancho) */           \
    }

/**
 * @brief Genera exec_instr_<nombre>_reg para operaciones bitwise/desplazamiento sin semantica de signo.
 *        Siempre pasa false como is_signed; CF/OF son definidos por la estructura Op.
 */
#define DEFINE_REG_EXEC_NOSIGN(name) \
    void exec_instr_##name##_reg(ProcessVM *vm, const DecodedInstr &instr) { \
        const int rdst = instr.data_instruction.reg_data.reg1; /* indice del registro destino */ \
        const int rsrc = instr.data_instruction.reg_data.reg2; /* indice del registro fuente */      \
        name##_table[instr.flags_info.mode](                                                    \
            vm, vm->registers.regs[rdst], vm->registers.regs[rsrc],                            \
            false, rdst); /* sin semantica de signo para operaciones bitwise/desplazamiento */                        \
    }

// Operaciones binarias que distinguen entre con/sin signo
DEFINE_REG_EXEC(add)
DEFINE_REG_EXEC(mul)
DEFINE_REG_EXEC(sub)
DEFINE_REG_EXEC(cmp)
DEFINE_REG_EXEC(div)

// Operaciones bitwise y de desplazamiento: is_signed es irrelevante para CF/OF (ver estructuras Op)
DEFINE_REG_EXEC_NOSIGN(and)
DEFINE_REG_EXEC_NOSIGN(or)
DEFINE_REG_EXEC_NOSIGN(xor)
DEFINE_REG_EXEC_NOSIGN(shl)
DEFINE_REG_EXEC_NOSIGN(shr)
DEFINE_REG_EXEC_NOSIGN(sar)

#undef DEFINE_REG_EXEC
#undef DEFINE_REG_EXEC_NOSIGN

// =========================================================================
// Funciones exec especiales (no pueden generarse con los macros anteriores)
// =========================================================================

/**
 * @brief Ejecuta una instruccion MOV reg-reg o MOV reg_ext-reg.
 *
 * Cuando el bit 's' es 0: MOV estandar reg1, reg2.
 * Cuando el bit 's' es 1: MOV con registro especial/extendido.
 *   d=0 -> escribir registro especial desde registro general.
 *   d=1 -> leer registro especial a registro general.
 *
 * @param vm    Puntero a la maquina virtual.
 * @param instr Instruccion decodificada con campos reg_data y flags_info.
 */
void exec_instr_mov_reg(ProcessVM *vm, const DecodedInstr &instr) {
    uint8_t s    = instr.flags_info._signed_instruct; // s=1 significa que un operando es un registro especial
    uint8_t d    = instr.flags_info.direction;         // 0=escribir especial, 1=leer especial
    uint8_t mode = instr.flags_info.mode;              // selector de ancho de operando
    uint8_t reg1 = instr.data_instruction.reg_data.reg1; // primer campo de registro
    uint8_t reg2 = instr.data_instruction.reg_data.reg2; // segundo campo de registro

    if (s == 0) {
        // MOV estandar reg1, reg2 -- sin registros especiales
        mov_table[mode](vm, vm->registers.regs[reg1], vm->registers.regs[reg2], false, reg1);
        return;
    }

    // s=1: variante reg_ext -- todos los codigos de registros especiales caben en 4 bits
    // (cur0..cur3 = 0..3, rip/rbp/rsp/rflags = 8..11), por lo que el selector se halla
    // unicamente en el nibble alto de byte2 (campo reg2 tras decodificar).  El campo
    // mode del ctrl byte indica el ancho real del operando general y se respeta para
    // mov_table[mode], evitando truncamientos accidentales (antes mode siempre era 0
    // por venir de bits[5:4] del codigo especial, lo que truncaba a 8 bits).
    uint8_t special = (uint8_t)(reg2 & 0xF); // codigo de registro especial (4 bits)
    if (d == 0) {
        // MOV reg_ext, reg -- escribir valor de registro general en registro especial
        write_special(vm, special, vm->registers.regs[reg1].raw());
    } else {
        // MOV reg, reg_ext -- leer valor de registro especial a registro general
        uint64_t val = read_special(vm, special); // obtener el registro especial
        GeneralRegister tmp;
        tmp.qword(val);                           // envolver en registro general temporal
        mov_table[mode](vm, vm->registers.regs[reg1], tmp, false, reg1); // copiar al destino
    }
}

/**
 * @brief Ejecuta NOT reg: complemento bitwise de un solo registro.
 *
 * @param vm    Puntero a la maquina virtual.
 * @param instr Instruccion decodificada; reg1 es el operando y el destino.
 */
void exec_instr_not_reg(ProcessVM *vm, const DecodedInstr &instr) {
    const int rdst = instr.data_instruction.reg_data.reg1; // el registro es tanto fuente como destino
    not_table[instr.flags_info.mode](vm, vm->registers.regs[rdst], rdst); // despacho por ancho
}

/**
 * @brief Ejecuta INC o DEC sobre un registro.
 *
 * El bit 's' codifica la variante: 0 = INC, 1 = DEC.
 * Este encoding reutiliza el bit de signo ya que INC/DEC no tienen semantica de signo.
 *
 * @param vm    Puntero a la maquina virtual.
 * @param instr Decoded instruction; reg1 is the operand/destination.
 */
void exec_instr_inc_dec_reg(ProcessVM *vm, const DecodedInstr &instr) {
    const int rdst = instr.data_instruction.reg_data.reg1; // operand register index
    if (instr.flags_info._signed_instruct == 0)
        inc_table[instr.flags_info.mode](vm, vm->registers.regs[rdst], rdst); // INC variant
    else
        dec_table[instr.flags_info.mode](vm, vm->registers.regs[rdst], rdst); // DEC variant
}

// =========================================================================
// SIB (Scale-Index-Base) memory access helpers
// =========================================================================

/**
 * @brief Computes the effective address for an SIB-encoded memory operand.
 *
 * scale field encoding: bits[2] = has_index flag, bits[1:0] = shift amount
 *   0 -> index * 1   (shift 0)
 *   1 -> index * 2   (shift 1)
 *   2 -> index * 4   (shift 2)
 *   3 -> index * 8   (shift 3)
 *
 * @param vm    Puntero a la maquina virtual.
 * @param instr Decoded instruction with mem_data fields.
 * @return      Effective address as a 64-bit value.
 */
inline uint64_t sib_effective_addr(ProcessVM *vm, const DecodedInstr &instr) {
    uint64_t base      = vm->registers.regs[instr.data_instruction.mem_data.reg_base].raw(); // base address
    uint8_t  has_index = (instr.data_instruction.mem_data.scale >> 2) & 1; // index present flag
    if (!has_index) return base;                                            // simple base-only addressing
    uint64_t index = vm->registers.regs[instr.data_instruction.mem_data.reg_index].raw(); // index value
    uint8_t  scale = instr.data_instruction.mem_data.scale & 0x3; // 2-bit scale: 0=*1, 1=*2, 2=*4, 3=*8
    return base + (index << scale);                                // base + index * (1<<scale)
}

/**
 * @brief SIB direction=0 wrapper: dst_reg = Op(dst_reg, mem[sib]).
 *        Reads the memory operand and feeds it to alu_core as source.
 */
template<typename T, typename Op>
static void sib_reg_dst_wrapper(ProcessVM *vm, uint64_t addr, uint64_t dst_val, bool is_signed, int dst_reg) {
    T src = vm->vm_mem.read_any<T>(addr);          // read source value from VM memory
    alu_core<T, Op>(vm, (T)dst_val, src, is_signed, dst_reg); // compute and store into dst_reg
}

/**
 * @brief SIB direction=1 wrapper: mem[sib] = Op(mem[sib], reg).
 *        Reads, computes, and writes back to virtual memory (unless CmpOp).
 */
template<typename T, typename Op>
static void sib_mem_dst_wrapper(ProcessVM *vm, uint64_t addr, uint64_t reg_val, bool is_signed, int) {
    T mem_val = vm->vm_mem.read_any<T>(addr);                           // load target from VM memory
    T res     = compute_with_flags<T, Op>(vm, mem_val, (T)reg_val, is_signed); // compute and set flags
    if constexpr (!Op::is_compare)
        vm->vm_mem.write_any<T>(addr, res); // write back (CMP discards result)
}

/** @brief Signature for SIB dispatch functions. */
using SIBFn = void(*)(ProcessVM *, uint64_t, uint64_t, bool, int);

// MOV [sib] variants (not arithmetic -- no flags involved)

/** @brief SIB MOV direction=0: load from VM memory into a general register. */
template<typename T>
static void sib_mov_to_reg(ProcessVM *vm, uint64_t addr, uint64_t, bool, int dst_reg) {
    T val   = vm->vm_mem.read_any<T>(addr); // read T-width value from VM memory
    auto &d = vm->registers.regs[dst_reg];  // referencia al registro destino
    if constexpr (sizeof(T)==1)      d.byte_lo((uint8_t)val);   // escritura de 8 bits
    else if constexpr (sizeof(T)==2) d.word_lo((uint16_t)val);  // escritura de 16 bits
    else if constexpr (sizeof(T)==4) d.dword_lo((uint32_t)val); // escritura de 32 bits
    else                             d.qword((uint64_t)val);     // escritura de 64 bits
}

/** @brief SIB MOV direction=1: store a general register value into VM memory. */
template<typename T>
static void sib_mov_to_mem(ProcessVM *vm, uint64_t addr, uint64_t reg_val, bool, int) {
    vm->vm_mem.write_any<T>(addr, (T)reg_val); // write truncated register value to VM memory
}

/** @brief MOVH direction=0: load from host (native) memory into a general register. */
template<typename T>
static void movh_to_reg(ProcessVM *vm, uint64_t addr, uint64_t, bool, int dst_reg) {
    T val   = *reinterpret_cast<const T*>(addr); // dereference native pointer -- bypasses VM memory
    auto &d = vm->registers.regs[dst_reg];
    if constexpr (sizeof(T)==1)      d.byte_lo((uint8_t)val);   // escritura de 8 bits
    else if constexpr (sizeof(T)==2) d.word_lo((uint16_t)val);  // escritura de 16 bits
    else if constexpr (sizeof(T)==4) d.dword_lo((uint32_t)val); // escritura de 32 bits
    else                             d.qword((uint64_t)val);     // escritura de 64 bits
}

/** @brief MOVH direction=1: store a register value into host (native) memory. */
template<typename T>
static void movh_to_mem(ProcessVM *, uint64_t addr, uint64_t reg_val, bool, int) {
    *reinterpret_cast<T*>(addr) = static_cast<T>(reg_val); // write to native address
}

// SIB dispatch tables for MOV and MOVH
static constexpr SIBFn mov_sib_to_reg_table[]  = {
    &sib_mov_to_reg<uint8_t>, &sib_mov_to_reg<uint16_t>,
    &sib_mov_to_reg<uint32_t>, &sib_mov_to_reg<uint64_t>
};
static constexpr SIBFn mov_sib_to_mem_table[]  = {
    &sib_mov_to_mem<uint8_t>, &sib_mov_to_mem<uint16_t>,
    &sib_mov_to_mem<uint32_t>, &sib_mov_to_mem<uint64_t>
};
static constexpr SIBFn movh_to_reg_table[] = {
    &movh_to_reg<uint8_t>, &movh_to_reg<uint16_t>,
    &movh_to_reg<uint32_t>, &movh_to_reg<uint64_t>
};
static constexpr SIBFn movh_to_mem_table[] = {
    &movh_to_mem<uint8_t>, &movh_to_mem<uint16_t>,
    &movh_to_mem<uint32_t>, &movh_to_mem<uint64_t>
};

/**
 * @brief Generic SIB execution helper shared by all arithmetic SIB instructions.
 *
 * Reads direction from the decoded instruction to choose between:
 *   direction=0 -> reg = Op(reg, mem[sib])
 *   direction=1 -> mem[sib] = Op(mem[sib], reg)
 *
 * @tparam Op     Operation struct (not used directly here; embedded in the table).
 * @param  vm     Pointer to the virtual machine.
 * @param  instr  Decoded instruction.
 * @param  reg_dst Table for direction=0 (reg destination).
 * @param  mem_dst Table for direction=1 (memory destination).
 */
template<typename Op>
static void exec_sib_generic(ProcessVM *vm, const DecodedInstr &instr,
                              const SIBFn reg_dst[4], const SIBFn mem_dst[4]) {
    const int dst  = instr.data_instruction.mem_data.reg_final; // final (non-SIB) register index
    uint64_t  addr = sib_effective_addr(vm, instr);              // compute effective address
    uint64_t  rval = vm->registers.regs[dst].raw();              // current register value
    bool      sign = instr.flags_info._signed_instruct;          // signed/unsigned selector
    int       mode = instr.flags_info.mode;                      // selector de ancho de operando
    if (instr.flags_info.direction == 0)
        reg_dst[mode](vm, addr, rval, sign, dst); // reg = Op(reg, mem)
    else
        mem_dst[mode](vm, addr, rval, sign, dst); // mem = Op(mem, reg)
}

// =========================================================================
// DEFINE_SIB_EXEC macro
// Declares both SIB dispatch tables and the exec function in one shot.
// =========================================================================

/**
 * @brief Declares SIB tables and the exec function for a binary ALU instruction.
 *
 * Expands to:
 *   - static constexpr SIBFn <name>_sib_reg_dst[4]  (direction=0 table)
 *   - static constexpr SIBFn <name>_sib_mem_dst[4]  (direction=1 table)
 *   - void exec_instr_<name>_sib(ProcessVM*, const DecodedInstr&)
 */
#define DEFINE_SIB_EXEC(name, Op) \
    static constexpr SIBFn name##_sib_reg_dst[] = { \
        &sib_reg_dst_wrapper<uint8_t,  Op>, &sib_reg_dst_wrapper<uint16_t, Op>, \
        &sib_reg_dst_wrapper<uint32_t, Op>, &sib_reg_dst_wrapper<uint64_t, Op>  \
    }; \
    static constexpr SIBFn name##_sib_mem_dst[] = { \
        &sib_mem_dst_wrapper<uint8_t,  Op>, &sib_mem_dst_wrapper<uint16_t, Op>, \
        &sib_mem_dst_wrapper<uint32_t, Op>, &sib_mem_dst_wrapper<uint64_t, Op>  \
    }; \
    void exec_instr_##name##_sib(ProcessVM *vm, const DecodedInstr &instr) { \
        exec_sib_generic<Op>(vm, instr, name##_sib_reg_dst, name##_sib_mem_dst); \
    }

DEFINE_SIB_EXEC(add, AddOp)
DEFINE_SIB_EXEC(sub, SubOp)
DEFINE_SIB_EXEC(mul, MulOp)
DEFINE_SIB_EXEC(div, DivOp)
DEFINE_SIB_EXEC(cmp, CmpOp)

#undef DEFINE_SIB_EXEC

// =========================================================================
// MOV SIB (special: s=1 means MOVH -- access host/native memory)
// =========================================================================

/**
 * @brief Executes a MOV or MOVH SIB instruction.
 *
 * s=0 (MOVC): accesses virtual machine memory.
 * s=1 (MOVH): accesses host process memory directly via native pointer.
 *
 * @param vm    Puntero a la maquina virtual.
 * @param instr Decoded instruction with mem_data, flags_info fields.
 */
void exec_instr_mov_sib(ProcessVM *vm, const DecodedInstr &instr) {
    const int dst  = instr.data_instruction.mem_data.reg_final;      // register counterpart
    uint64_t  addr = sib_effective_addr(vm, instr);                   // effective address
    uint64_t  rval = vm->registers.regs[dst].raw();                   // register value
    int       mode = instr.flags_info.mode;                           // operand width
    bool      host = instr.flags_info._signed_instruct;               // s=1 selects MOVH (host memory)

    if (!host) {
        // MOVC: access virtual machine memory
        if (instr.flags_info.direction == 0)
            mov_sib_to_reg_table[mode](vm, addr, rval, false, dst);  // load from VM mem
        else
            mov_sib_to_mem_table[mode](vm, addr, rval, false, dst);  // store to VM mem
    } else {
        // MOVH: access host native memory
        if (instr.flags_info.direction == 0)
            movh_to_reg_table[mode](vm, addr, rval, false, dst);     // load from host mem
        else
            movh_to_mem_table[mode](vm, addr, rval, false, dst);     // store to host mem
    }
}

// =========================================================================
// MOVC / MOVCH -- conditional move based on a flag
// =========================================================================

/**
 * @brief Reads a flag register value by its 3-bit code.
 *
 * Flag code encoding: SF=0, ZF=1, CF=2, OF=3, DM=4.
 *
 * @param vm        Pointer to the virtual machine.
 * @param flag_code 3-bit flag selector.
 * @return          Boolean value of the selected flag.
 */
inline bool read_flag(ProcessVM *vm, uint8_t flag_code) {
    switch (flag_code & 0x7) {
        case 0: return vm->registers.flags.bits.SF;  // sign flag
        case 1: return vm->registers.flags.bits.ZF;  // zero flag
        case 2: return vm->registers.flags.bits.CF;  // acarreo flag
        case 3: return vm->registers.flags.bits.OF;  // desbordamiento flag
        case 4: return vm->registers.flags.bits.DM;  // direction / mode flag
        default: return false;                        // unknown code: treated as not set
    }
}

/**
 * @brief Executes MOVC/MOVCH with a memory operand (opcode2 = 0x1E).
 *
 * Encoding: [0x00][0x1E][ctrl][byte4]
 *   ctrl: host_bits(2)|d(1)|r_nonbracket(5)   bits[7:6]=0b10->MOVCH, 0b00->MOVC
 *   byte4: flag_code(3)|r_bracket(5)
 *
 * Moves data only if the specified flag is set.
 *
 * @param vm    Puntero a la maquina virtual.
 * @param instr Decoded instruction fields (see above encoding).
 */
void exec_instr_movc_mem(ProcessVM *vm, const DecodedInstr &instr) {
    uint8_t flag_code = instr.data_instruction.reg_data.reg2;    // 3-bit flag selector from byte4
    uint8_t reg1      = instr.data_instruction.reg_data.reg1;    // the non-bracketed register
    uint8_t reg2      = instr.data_instruction.inmmed_data.reg;  // the register inside []
    bool    host      = instr.flags_info._signed_instruct;       // 0=MOVC (VM mem), 1=MOVCH (host mem)
    bool    dir       = instr.flags_info.direction;              // 0=[reg2]->reg1, 1=reg2->[reg1]

    if (!read_flag(vm, flag_code)) return; // condition not met: skip the move

    // determine which register holds the memory address and which holds the value
    uint64_t addr = vm->registers.regs[dir ? reg1 : reg2].raw(); // address register
    uint64_t val  = vm->registers.regs[dir ? reg2 : reg1].raw(); // value register

    if (!host) {
        // MOVC: use virtual machine memory
        if (!dir) {
            uint64_t v = vm->vm_mem.read_u64(addr);    // load 64-bit value from VM memory
            vm->registers.regs[reg1].qword(v);          // write to destination register
        } else {
            vm->vm_mem.write_u64(addr, val);             // write register value to VM memory
        }
    } else {
        // MOVCH: use host process memory
        if (!dir) {
            uint64_t v = *reinterpret_cast<const uint64_t*>(addr); // load from native pointer
            vm->registers.regs[reg1].qword(v);                      // write to destination register
        } else {
            *reinterpret_cast<uint64_t*>(addr) = val; // write register value to native address
        }
    }
}

/**
 * @brief Executes MOVC reg, reg, flag (opcode2 = 0x1F).
 *
 * Encoding: [0x00][0x1F][ctrl][byte4]
 *   ctrl: 0b00|0|reg1(4)
 *   byte4: flag_code(3)|reg2(5)
 *
 * Copies reg2 to reg1 only if the specified flag is set; uses mode for width.
 *
 * @param vm    Puntero a la maquina virtual.
 * @param instr Decoded instruction fields.
 */
void exec_instr_movc_reg(ProcessVM *vm, const DecodedInstr &instr) {
    uint8_t flag_code = instr.data_instruction.reg_data.reg2;   // 3-bit flag selector
    uint8_t reg1      = instr.data_instruction.reg_data.reg1;   // destination register index
    uint8_t reg2      = instr.data_instruction.inmmed_data.reg; // source register index

    if (!read_flag(vm, flag_code)) return; // condition not met: skip the move

    GeneralRegister tmp;
    tmp.qword(vm->registers.regs[reg2].raw());                          // copy source into temp (full 64-bit)
    mov_table[instr.flags_info.mode](vm, vm->registers.regs[reg1], tmp, false, reg1); // conditional MOV at mode width
}

// =========================================================================
// ModOp -- modulo entero (resto de la division)
//
// mods/modu: r_dst = r_dst % r_src
// Resultado indefinido si el divisor es cero; en ese caso se activan CF y OF
// para que el programa pueda detectar el error.
// =========================================================================

/**
 * @brief MOD / UMOD: resto de la division entera.
 *
 * - Con signo  (mods): resultado tiene el signo del dividendo (semantica C %).
 * - Sin signo  (modu): resultado siempre no negativo.
 * Division por cero: resultado = 0, CF = 1, OF = 1.
 */
struct ModOp {
    static constexpr bool is_compare = false; // el resultado se escribe de vuelta

    /** @brief Returns a % b (divide-by-zero must be checked externally; we return 0). */
    template<typename T>
    static inline T compute(T a, T b) {
        if (b == 0) return static_cast<T>(0); // division por cero: resultado definido como 0
        return a % b;
    }

    /** @brief Sets CF/OF on divide-by-zero; clears them otherwise. */
    template<typename T>
    static inline void flags(ProcessVM *vm, T /*a*/, T b, T /*result*/, bool /*is_signed*/) {
        if (b == 0) {
            vm->registers.flags.bits.OF = 1; // senalizar condicion excepcional
            vm->registers.flags.bits.CF = 1;
        } else {
            vm->registers.flags.bits.OF = 0; // operacion normal sin desbordamiento
            vm->registers.flags.bits.CF = 0;
        }
    }
};

// re-declarar los macros de tablas para ModOp (fueron indefinidos tras las instrucciones ALU base)
#define DECL_BIN_TABLE(n, Op) \
    static constexpr BinaryFn n##_table[] = { \
        &binary_wrapper<uint8_t,  Op>, \
        &binary_wrapper<uint16_t, Op>, \
        &binary_wrapper<uint32_t, Op>, \
        &binary_wrapper<uint64_t, Op>  \
    }

#define DECL_IMM_TABLES(n, Op) \
    static constexpr BinaryImmFn n##_imm_table[] = { \
        &binary_imm_wrapper<uint8_t,  Op>, \
        &binary_imm_wrapper<uint16_t, Op>, \
        &binary_imm_wrapper<uint32_t, Op>, \
        &binary_imm_wrapper<uint64_t, Op>  \
    }; \
    static constexpr BinaryMemImmFn n##_mem_imm_table[] = { \
        &binary_mem_imm_wrapper<uint8_t,  Op>, \
        &binary_mem_imm_wrapper<uint16_t, Op>, \
        &binary_mem_imm_wrapper<uint32_t, Op>, \
        &binary_mem_imm_wrapper<uint64_t, Op>  \
    }

#define DEFINE_REG_EXEC(n) \
    void exec_instr_##n##_reg(ProcessVM *vm, const DecodedInstr &instr) { \
        const int rdst = instr.data_instruction.reg_data.reg1; \
        const int rsrc = instr.data_instruction.reg_data.reg2; \
        n##_table[instr.flags_info.mode]( \
            vm, vm->registers.regs[rdst], vm->registers.regs[rsrc], \
            instr.flags_info._signed_instruct, rdst); \
    }

#define DEFINE_IMM_EXEC(n, imm_tbl, mem_tbl) \
    void exec_instr_##n##_imm(ProcessVM *vm, const DecodedInstr &instr) { \
        const int rdst = instr.data_instruction.inmmed_data.reg; \
        uint64_t  imm  = instr.data_instruction.inmmed_data.inmmed; \
        if (instr.flags_info.direction == 0) { \
            imm_tbl[instr.flags_info.mode]( \
                vm, vm->registers.regs[rdst], imm, \
                instr.flags_info._signed_instruct, rdst); \
            return; \
        } \
        uint64_t addr = vm->registers.regs[rdst].raw(); \
        mem_tbl[instr.flags_info.mode](vm, addr, imm, instr.flags_info._signed_instruct); \
    }

#define DEFINE_SIB_EXEC(n, Op) \
    static constexpr SIBFn n##_sib_reg_dst[] = { \
        &sib_reg_dst_wrapper<uint8_t,  Op>, &sib_reg_dst_wrapper<uint16_t, Op>, \
        &sib_reg_dst_wrapper<uint32_t, Op>, &sib_reg_dst_wrapper<uint64_t, Op>  \
    }; \
    static constexpr SIBFn n##_sib_mem_dst[] = { \
        &sib_mem_dst_wrapper<uint8_t,  Op>, &sib_mem_dst_wrapper<uint16_t, Op>, \
        &sib_mem_dst_wrapper<uint32_t, Op>, &sib_mem_dst_wrapper<uint64_t, Op>  \
    }; \
    void exec_instr_##n##_sib(ProcessVM *vm, const DecodedInstr &instr) { \
        exec_sib_generic<Op>(vm, instr, n##_sib_reg_dst, n##_sib_mem_dst); \
    }

DECL_BIN_TABLE(mod, ModOp);
DECL_IMM_TABLES(mod, ModOp);
DEFINE_SIB_EXEC(mod, ModOp)
DEFINE_REG_EXEC(mod)
DEFINE_IMM_EXEC(mod, mod_imm_table, mod_mem_imm_table)

#undef DEFINE_REG_EXEC
#undef DEFINE_IMM_EXEC
#undef DEFINE_SIB_EXEC
#undef DECL_BIN_TABLE
#undef DECL_IMM_TABLES

// =========================================================================
// SETCC -- almacena el resultado de una condicion de flags en un registro
//
// Encoding FIXED_4: [0x00][0x43][ctrl][0x00]
//   ctrl = (cond<<4) | dst_reg    (cond: mismo codigo que JCC)
// El registro destino recibe 1 si la condicion es verdadera, 0 si no.
// =========================================================================

/**
 * @brief Ejecuta SETCC r_dst, cond: escribe 0 o 1 segun la condicion de flags.
 *
 * Usa el mismo codigo de condicion que JCC (0x00 = JO, 0x01 = JNO, ..., 0x0E = JLE, 0x0F = JMP/true).
 *
 * @param vm    Puntero a la maquina virtual.
 * @param instr Instruccion descodificada; cond en reg1>>4, dst en reg1&0xF (o via byte2).
 */
void exec_instr_setcc(ProcessVM *vm, const DecodedInstr &instr) {
    // byte2: bits[7:4] = condicion, bits[3:0] = registro destino
    const uint8_t cond    = (instr.data_instruction.reg_data.reg1 >> 4) & 0xF; // codigo de condicion
    const uint8_t dst_reg = instr.data_instruction.reg_data.reg1        & 0xF; // indice del registro destino

    auto &f   = vm->registers.flags.bits;
    bool taken = false; // resultado de la condicion

    switch (cond) {
        case 0x00: taken = f.OF;                                break; // JO
        case 0x01: taken = !f.OF;                               break; // JNO
        case 0x02: taken = f.CF;                                break; // JB/JNAE
        case 0x03: taken = !f.CF;                               break; // JNB/JAE
        case 0x04: taken = f.ZF;                                break; // JE/JZ
        case 0x05: taken = !f.ZF;                               break; // JNE/JNZ
        case 0x06: taken = f.CF || f.ZF;                        break; // JBE/JNA
        case 0x07: taken = !f.CF && !f.ZF;                      break; // JNBE/JA
        case 0x08: taken = f.SF;                                break; // JS
        case 0x09: taken = !f.SF;                               break; // JNS
        case 0x0A: taken = f.ZF && !(f.SF ^ f.OF);             break; // JE y sin desbordamiento con signo
        case 0x0B: taken = !f.ZF;                               break; // JNE (alias JNLE)
        case 0x0C: taken = (f.SF ^ f.OF);                       break; // JL/JNGE
        case 0x0D: taken = !(f.SF ^ f.OF);                      break; // JNL/JGE
        case 0x0E: taken = f.ZF || (f.SF ^ f.OF);              break; // JLE/JNG
        case 0x0F: taken = true;                                 break; // siempre verdadero
        default:   taken = false;                                break;
    }

    vm->registers.regs[dst_reg].qword(taken ? 1 : 0); // escribir resultado booleano en el registro
}

} // namespace runtime
