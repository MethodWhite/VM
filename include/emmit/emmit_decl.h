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
 * @file emmit_decl.h
 * @brief Declaraciones de funciones de emision de bytecode y tipos auxiliares del ensamblador.
 *
 * Define los tipos y funciones que utiliza el emisor (Assembler) para traducir instrucciones
 * del AST a bytecode .velb:
 *
 *   - InstrSizeMode: enumeracion con los tamanos fijos o variables de cada instruccion.
 *   - ImmType: tipo del inmediato (tamano y signo).
 *   - AddressingMode: modos de direccionamiento soportados (REG, MEM, SIB, INMED, NONE).
 *   - InstrInfo: descriptor completo de una instruccion (opcodes, tamano, modo, funcion de emision).
 *   - emitInstr: tipo de puntero a funcion de emision.
 *   - special_reg_encoding: tabla de codificacion de registros especiales.
 *   - Funciones auxiliares: encode_special_register, detect_imm_type, immtype_bits, mode_to_bits,
 *     mode_to_bytes, encode_mode, AddressingMode_str, instr_size, is_signed.
 *   - emit_*: funciones de emision para cada familia de instrucciones.
 */

#ifndef EMMIT_DECL_H
#define EMMIT_DECL_H

#include <unordered_set>
#include <unordered_map>
#include <optional>
#include <string>

#include "bytewriter.h"
#include "runtime/vm_address_space.h"
#include "parser/parser.h"

namespace Assembly::Bytecode {

    class Assembler; ///< Declaracion adelantada del ensamblador

    /**
     * @brief Tamano en bytes de una instruccion del conjunto de instrucciones Vesta.
     *
     * La mayoria de instrucciones tienen tamano fijo; las que usan inmediatos
     * pueden tener tamano variable (MIXED_SIZE) segun el valor del operando.
     */
    enum class InstrSizeMode {
        FIXED_1  = 0, ///< Instruccion de tamano fijo de 1 byte
        FIXED_2  = 1, ///< Instruccion de tamano fijo de 2 bytes
        FIXED_4  = 2, ///< Instruccion de tamano fijo de 4 bytes
        FIXED_6  = 3, ///< Instruccion de tamano fijo de 6 bytes
        FIXED_8  = 4, ///< Instruccion de tamano fijo de 8 bytes
        FIXED_10 = 5, ///< Instruccion de tamano fijo de 10 bytes
        FIXED_11 = 6, ///< Instruccion de tamano fijo de 11 bytes
        COUNT,        ///< Numero de variantes de tamano fijo (usar como limite de tabla)

        /**
         * @brief Instruccion con tamano variable segun el inmediato.
         *
         * Las instrucciones con inmediato pueden tener los siguientes tamanos:
         * @code
         *   //  Total       opcode      inmediato
         *   //    4    (    3           + 1)  -- inmediato de 1 byte
         *   //    5    (    3           + 2)  -- inmediato de 2 bytes
         *   //    7    (    3           + 4)  -- inmediato de 4 bytes
         *   //    11   (    3           + 8)  -- inmediato de 8 bytes
         * @endcode
         */
        MIXED_SIZE
    };

    /**
     * @brief Tipo de un operando inmediato: tamano (8/16/32/64 bits) y signo.
     *
     * Se usa para determinar cuantos bytes debe emitir el ensamblador para
     * representar el valor inmediato de una instruccion.
     */
    enum class ImmType {
        U8,  ///< Entero sin signo de 8 bits
        S8,  ///< Entero con signo de 8 bits
        U16, ///< Entero sin signo de 16 bits
        S16, ///< Entero con signo de 16 bits
        U32, ///< Entero sin signo de 32 bits
        S32, ///< Entero con signo de 32 bits
        U64, ///< Entero sin signo de 64 bits
        S64  ///< Entero con signo de 64 bits
    };

    /**
     * @brief Tabla de codificacion de registros especiales o extendidos.
     *
     * Los cursores (cur0-cur3), el puntero de instruccion (rip), el puntero de pila
     * (rsp), el puntero de marco (rbp) y el registro de flags (rflags) se codifican
     * con valores especificos distintos a los registros generales R00-R15.
     */
    static const std::unordered_map<std::string, uint8_t> special_reg_encoding = {
        {"cur0",   0b000000}, ///< Cursor 0: puntero a memoria host
        {"cur1",   0b000001}, ///< Cursor 1
        {"cur2",   0b000010}, ///< Cursor 2
        {"cur3",   0b000011}, ///< Cursor 3
        {"rip",    0b001000}, ///< Puntero de instruccion (Program Counter)
        {"rbp",    0b001001}, ///< Puntero de marco de pila (Base Pointer)
        {"rsp",    0b001010}, ///< Puntero de pila (Stack Pointer)
        {"rflags", 0b001011}, ///< Registro de flags de condicion
    };

    /**
     * @brief Obtiene la codificacion de un registro especial o extendido.
     *
     * @param reg Nombre del registro a codificar (p.ej. "rip", "cur0").
     * @return Codificacion del registro si es especial; std::nullopt si es un registro general.
     */
    inline std::optional<uint8_t> encode_special_register(const std::string &reg) {
        auto it = special_reg_encoding.find(reg);
        if (it != special_reg_encoding.end())
            return it->second;
        return std::nullopt; // no es un registro especial
    }

    /**
     * @brief Determina el tipo ImmType de un valor inmediato segun su magnitud y signo.
     *
     * Usa el valor mas pequeno que pueda representar el inmediato para minimizar
     * el tamano de la instruccion emitida.
     *
     * @param value     Valor del inmediato (interpretado como int64_t).
     * @param is_signed true si la instruccion es con signo; false si es sin signo.
     * @return ImmType correspondiente al tamano minimo necesario.
     */
    inline ImmType detect_imm_type(int64_t value, bool is_signed) {
        if (is_signed) {
            if ((int8_t)  value == value) return ImmType::S8;
            if ((int16_t) value == value) return ImmType::S16;
            if ((int32_t) value == value) return ImmType::S32;
            return ImmType::S64;
        } else {
            uint64_t v = (uint64_t) value;
            if ((uint8_t)  v == v) return ImmType::U8;
            if ((uint16_t) v == v) return ImmType::U16;
            if ((uint32_t) v == v) return ImmType::U32;
            return ImmType::U64;
        }
    }

    /**
     * @brief Devuelve el numero de bits del tipo de inmediato indicado.
     *
     * @param t Tipo de inmediato.
     * @return 8, 16, 32 o 64 segun el tipo.
     */
    inline int immtype_bits(ImmType t) {
        switch (t) {
            case ImmType::U8:
            case ImmType::S8:  return 8;
            case ImmType::U16:
            case ImmType::S16: return 16;
            case ImmType::U32:
            case ImmType::S32: return 32;
            default:           return 64;
        }
    }

    /**
     * @brief Convierte el campo mode de 2 bits al tamano en bits del operando.
     *
     * Codificacion del campo mode (2 bits):
     * @code
     *   modo 0 (0b00) -> 8  bits
     *   modo 1 (0b01) -> 16 bits
     *   modo 2 (0b10) -> 32 bits
     *   modo 3 (0b11) -> 64 bits
     * @endcode
     *
     * @param mode Campo mode de 2 bits (0-3).
     * @return Tamano en bits del operando.
     */
    inline int mode_to_bits(uint8_t mode) {
        return 1 << (3 + mode); // 2^(3+mode)
    }

    /**
     * @brief Convierte el campo mode de 2 bits al tamano en bytes del operando.
     *
     * Codificacion del campo mode (2 bits):
     * @code
     *   modo 0 (0b00) -> 1 byte
     *   modo 1 (0b01) -> 2 bytes
     *   modo 2 (0b10) -> 4 bytes
     *   modo 3 (0b11) -> 8 bytes
     * @endcode
     *
     * @param mode Campo mode de 2 bits (0-3).
     * @return Tamano en bytes del operando.
     */
    inline int mode_to_bytes(uint8_t mode) {
        return 1 << mode; // 2^mode
    }

    /**
     * @brief Codifica el tamano en bits de un registro al campo mode de 2 bits.
     *
     * Equivalencia:
     * @code
     *   8  bits  (1 byte)  -> mode = 0 (0b00)
     *   16 bits  (2 bytes) -> mode = 1 (0b01)
     *   32 bits  (4 bytes) -> mode = 2 (0b10)
     *   64 bits  (8 bytes) -> mode = 3 (0b11)
     * @endcode
     *
     * @param size_bits Tamano en bits del registro (8, 16, 32 o 64).
     * @return Campo mode de 2 bits (0-3).
     */
    inline uint8_t encode_mode(int size_bits) {
        uint8_t n_mode = (size_bits / 8) >> 1; // tamano en bytes desplazado a la derecha
        return (n_mode >= 4) ? 3 : n_mode;     // saturar en 3 para registros de 64 bits
    }

    /**
     * @brief Modos de direccionamiento de las instrucciones del ISA Vesta.
     *
     * Determina como se codifican y decodifican los operandos de una instruccion.
     */
    enum class AddressingMode {
        /**
         * @brief Operacion entre registros generales.
         *
         * Un bit indica la direccionalidad cuando hay dos registros:
         *   adds r0, r1   -- reg1, reg2
         *   adds r1, r0   -- reg2, reg1
         */
        REG,

        /**
         * @brief Operacion con memoria.
         *
         * Incluye operaciones de registro a memoria y de memoria a registro:
         *   adds reg1, [mem]
         *   adds [mem], reg1
         */
        MEM,

        /**
         * @brief Operacion con direccionamiento SIB (Scale-Index-Base).
         *
         * El segundo operando usa la formula [base + index * scale + despl]:
         *   adds reg, [reg1 * reg2 + reg3]
         */
        SIB,

        /**
         * @brief Operacion con inmediato.
         *
         * El segundo operando es un valor constante o direccion de memoria fija:
         *   adds r00, 0x1234
         */
        INMED,

        /**
         * @brief Sin modos de direccionamiento (instruccion sin operandos).
         */
        NONE,

        COUNT ///< Numero de modos (usar como limite de tabla)
    };

    /**
     * @brief Convierte un AddressingMode a su representacion en cadena.
     *
     * @param mode Modo de direccionamiento a convertir.
     * @return Cadena con el nombre del modo ("REG", "MEM", "SIB", "INMED" o "NONE").
     */
    static std::string AddressingMode_str(AddressingMode mode) {
        static const std::string table[(size_t) AddressingMode::COUNT] = {"REG", "MEM", "SIB", "INMED", "NONE"};
        return table[static_cast<uint8_t>(mode)];
    }

    /**
     * @brief Devuelve el tamano en bytes de una instruccion a partir de su InstrSizeMode.
     *
     * @param mode Modo de tamano de la instruccion (debe ser un valor FIXED_*).
     * @return Tamano en bytes de la instruccion.
     */
    constexpr size_t instr_size(InstrSizeMode mode) {
        constexpr size_t table[(size_t) InstrSizeMode::COUNT] = {1, 2, 4, 6, 8, 10, 11};
        return table[static_cast<uint8_t>(mode)];
    }

    /**
     * @brief Tipo puntero a funcion de emision de una instruccion.
     *
     * Cada familia de instrucciones tiene su propia funcion de emision que recibe
     * la instruccion del parser, el escritor de bytes, el descriptor de la instruccion
     * y el contexto global del ensamblador.
     *
     * @param instruction_parser Instruccion parseada del AST.
     * @param code_final         Escritor donde se emite el bytecode.
     * @param now_instr          Descriptor de la instruccion con opcodes y modo.
     * @param assembly_ctx       Contexto global del ensamblador (tablas de simbolos, etc.).
     */
    typedef void (*emitInstr)(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const struct InstrInfo *now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Descriptor completo de una instruccion del ensamblador Vesta.
     *
     * Contiene los opcodes, el tamano, el modo de direccionamiento y la funcion
     * de emision necesaria para traducir la instruccion del AST a bytecode.
     */
    typedef struct InstrInfo {
        uint8_t        opcode1;  ///< Primer byte de opcode; si es 0x00 se usa el prefijo extendido
        uint8_t        opcode2;  ///< Segundo byte de opcode (solo valido cuando opcode1 == 0x00)
        InstrSizeMode  sizeMode; ///< Tamano de la instruccion codificada
        AddressingMode mode;     ///< Modo de direccionamiento de los operandos
        emitInstr      emit;     ///< Funcion de emision de la instruccion
    } InstrInfo;

    /**
     * @brief Conjunto de mnemonicos de instrucciones con signo.
     *
     * Permite consultar en O(1) si una instruccion es la variante con signo,
     * lo que determina como se interpreta el inmediato (int64_t vs uint64_t).
     */
    static const std::unordered_set<std::string> signed_ops = {
        "adds", "subs", "muls", "divs", "cmps", "mods"
    };

    /**
     * @brief Mnemonicos cuya variante SIB requiere acceder a memoria HOST en
     *        lugar de memoria VM.  Reutiliza el bit @c _signed_instruct del
     *        ctrl byte (bit 5): para MOV SIB, s=1 selecciona MOVH (memoria
     *        del proceso host) y s=0 selecciona MOVC (memoria VM).
     *        El emisor SIB consulta is_signed() OR is_host_sib() para
     *        decidir el bit; al ser conjuntos disjuntos por construccion,
     *        no hay colision de semanticas.
     */
    static const std::unordered_set<std::string> host_sib_ops = {
        "movh"
    };

    /**
     * @brief Indica si el mnemonico corresponde a un MOV SIB sobre memoria host.
     */
    static bool is_host_sib(const std::string &opcode) {
        return host_sib_ops.count(opcode) > 0;
    }

    /**
     * @brief Indica si el mnemonico corresponde a una instruccion con signo.
     *
     * @param opcode Nombre del mnemonico a comprobar.
     * @return true si es una instruccion de la familia "signed" (adds, subs, etc.).
     */
    static bool is_signed(const std::string &opcode) {
        return signed_ops.count(opcode) > 0; // O(1)
    }

    /**
     * @brief Emite instrucciones PUSH y POP.
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_pop_push(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite instrucciones INC y DEC.
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_inc_dec(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite instrucciones con modo de direccionamiento REG (registro a registro).
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_instr_reg(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite instrucciones con modo de direccionamiento MEM (registro a memoria o viceversa).
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_instr_mem(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite instrucciones con modo de direccionamiento SIB (Scale-Index-Base).
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_instr_sib(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite la instruccion XCHG (intercambio de dos registros).
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_xchg(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite instrucciones MOV de registro a registro.
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_instr_mov_reg(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite instrucciones MOV con inmediato (MOV reg, imm).
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_instr_mov_inmed(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite instrucciones MOV con SIB (MOVH y variantes con memoria host).
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_instr_mov_sib(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite instrucciones MOVC y MOVCH (copia entre memoria VM y host).
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_instr_movc(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite instrucciones con inmediato (p.ej. MOV r12, 0x1234, ADD r0, 5).
     *
     * Si el segundo operando es una etiqueta o anotacion, delega en
     * emit_instr_inmed_with_annotation() para registrar la relocalizacion necesaria.
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_instr_inmed(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite instrucciones con inmediato que es una etiqueta o anotacion.
     *
     * Registra una entrada en la tabla de relocalizaciones para que el linker
     * pueda resolver la referencia en tiempo de enlace o de carga.
     * No se llama directamente; lo invoca emit_instr_inmed() cuando detecta
     * que el operando no es un valor literal.
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_instr_inmed_with_annotation(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite la instruccion CALLN para llamadas a metodos nativos del host.
     *
     * Genera la instruccion con un placeholder de 8 bytes que el linker parchea
     * con el indice de la tabla de importaciones.  En tiempo de carga el loader
     * aplaza la resolucion de la direccion real del metodo (lazy loading).
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_instr_calln_inmmed(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite instrucciones con un unico operando de tipo registro general.
     *
     * Cubre: newobj, gcconfig, drop, gcwb, alloc, free.
     *
     * Formato (FIXED_4, prefijo extendido 0x00):
     * @code
     *   [0x00][opcode][ctrl_byte][reg_byte]
     *   ctrl_byte: bits 7-6 = mode (tamano del registro), bits 5-0 = 0
     *   reg_byte:  bits 3-0 = registro general, bits 7-4 = 0
     * @endcode
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_instr_one_reg(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite instrucciones con tres registros (ej. msgsend r_pid, r_addr, r_len).
     *
     * Codificacion FIXED_4 de la forma [op1][op2][b2][b3]:
     * @code
     *   b2 = (r1 << 4) | r2
     *   b3 = (r3 << 4)
     * @endcode
     *
     * @param instruction_parser Instruccion del AST con tres operandos registro.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_instr_three_reg(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite instrucciones de lectura/escritura a memoria real via cursor.
     *
     * Cubre: readcur dest_reg, curN  y  writecur curN, src_reg.
     *
     * Codificacion:
     * @code
     *   ctrl_byte = (mode << 6) | (cur_idx << 4)
     *   reg_byte  = gen_reg_index
     * @endcode
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_cursor_rw(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite la instruccion gcderef curN, handle_reg.
     *
     * Codificacion:
     * @code
     *   ctrl_byte = (cur_idx << 4)
     *   reg_byte  = handle_reg_index
     * @endcode
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_gcderef(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite la instruccion addcur curN, imm16.
     *
     * Suma un inmediato con signo de 16 bits al registro cursor indicado.
     * Avanzar: imm > 0.  Retroceder: imm < 0.
     *
     * Codificacion (FIXED_6):
     * @code
     *   [0x00][0xC3][ctrl][0x00][imm_lo][imm_hi]
     *   ctrl: bits 5-4 = cur_idx, resto = 0
     * @endcode
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_addcur(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite la instruccion vmcopy curN, rSrc, rLen (VM memory -> host memory).
     *
     * Codificacion (FIXED_4):
     * @code
     *   [0x00][0xC4][byte_A][byte_B]
     *   byte_A: bits 5-4 = cur_idx, bits 3-0 = rSrc
     *   byte_B: bits 7-4 = rLen,    bits 3-0 = 0
     * @endcode
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_vmcopy(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite la instruccion vcopyh rDst, curN, rLen (host memory -> VM memory).
     *
     * Codificacion (FIXED_4):
     * @code
     *   [0x00][0xC5][byte_A][byte_B]
     *   byte_A: bits 5-4 = cur_idx, bits 3-0 = rDst
     *   byte_B: bits 7-4 = rLen,    bits 3-0 = 0
     * @endcode
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_vcopyh(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite instrucciones con direccion absoluta de 64 bits.
     *
     * Formato emitido: [opcode2][8 bytes de direccion absoluta].
     * Se usa para: callvm, enter y variantes condicionales de jmp.
     * Si el operando es un LabelOperand se registra una relocalizacion Absolute64.
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_instr_abs64(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite el cuerpo de la instruccion jrel (salto relativo condicional).
     *
     * Formato emitido: [cond][pad=0x00][disp32].
     * La condicion se deduce del sufijo del mnemonico (jrel.je, jrel.jne, etc.).
     * Si el operando es un LabelOperand se registra una relocalizacion Relative32.
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_jrel(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite instrucciones OOP de la forma [reg, imm8].
     *
     * Cubre: callvirt, callsuper, getfield, getmethod.
     *
     * Formato (FIXED_4, prefijo extendido 0x00):
     * @code
     *   [0x00][opcode][reg_byte][imm8]
     *   reg_byte : bits 3-0 = registro general (operando 0)
     *   imm8     : valor inmediato uint8 (operando 1)
     * @endcode
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_instr_reg_imm8(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite una instruccion de pila con inmediato (subsp / addsp).
     *
     * Operandos: [rsp|rbp], inmediato64.
     * Byte ctrl: bits 7-6 = modo (siempre 3 = 64 bits), bits 1-0 = sp_bp (0=RSP,1=RBP).
     * Formato: [0x00][opcode2][ctrl][imm64] = 11 bytes.
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_instr_spimm(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite una instruccion de tabla de saltos (jumptable / typeswitch).
     *
     * Operandos: r_val, r_table, count.
     * Formato: [0x00][opcode2][byte2][byte3] = FIXED_4.
     *   byte2: bits 7-4 = r_val, bits 3-0 = r_table.
     *   byte3: count (numero de entradas, uint8).
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode destino.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_instr_jumptable(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite los operandos de @c addadvice (FIXED_4, convencion B).
     *
     * Toma 3 operandos textuales: r_target, r_advice, kind (imm).  Empaqueta
     * registros en byte2 = (r_advice<<4)|r_target y kind en byte3.  El opcode
     * prefix (0x00, 0xCE) lo emite el helper estandar antes de invocar este.
     */
    void emit_instr_addadvice(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite super-instrucciones ALU 3-operandos (adds3, subs3, etc.).
     *
     * Toma 3 operandos textuales: r_dst, r_src1, r_src2.  Empaqueta
     * byte2 = (r_src1<<4)|r_dst y byte3 = (r_src2<<4)|flags.  El opcode prefix
     * (0x00, opcode2 segun mnemonic) lo emite el helper estandar antes.
     */
    void emit_instr_alu3(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite super-instruccion LOAD con zero-extend (loadz / loadzh).
     *
     * Fusion de @c mov rd,0 + @c mov rd_sized,[rs] en una sola instruccion VM.
     * 2 operandos: r_dst (sized) y r_src (puntero base 64-bit).  El mode del
     * ctrl byte se deriva de @c reg_dst->size_bits, el bit s indica si la
     * carga es de memoria HOST (1) o VM (0).
     */
    void emit_instr_loadz(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite los operandos de @c getstatic / @c setstatic (FIXED_8).
     *
     * 3 operandos textuales: dos registros y un offset (uint32).  Empaqueta
     * los registros en byte2 = (r0<<4)|r1 y emite el offset uint32 en los
     * 4 bytes finales.  El opcode prefix (0x00, 0x60 o 0x61) lo emite el
     * helper estandar antes de invocar este emisor.
     *
     * Formato fisico:
     *   [0x00][0x60|0x61][regs_byte][_pad8=0][offset_u32_LE]
     *
     * Convencion de operandos:
     *   getstatic: op0=r_dst,   op1=r_class, op2=offset
     *   setstatic: op0=r_class, op1=r_value, op2=offset
     */
    void emit_instr_static(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite los operandos de @c dlopen (FIXED_4, 3 regs).
     *
     * Operandos textuales: r_dst, r_path_addr, r_path_len.
     * Encoding fisico:
     *   [0x00][0x62][b2][b3]
     *     b2 = (r_dst<<4) | r_path_addr
     *     b3 = (r_path_len<<4) | 0
     */
    void emit_instr_dlopen(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite los operandos de @c dlsym (FIXED_4, 4 regs).
     *
     * Operandos textuales: r_dst, r_handle, r_name_addr, r_name_len.
     * Encoding fisico:
     *   [0x00][0x63][b2][b3]
     *     b2 = (r_dst<<4) | r_handle
     *     b3 = (r_name_addr<<4) | r_name_len
     */
    void emit_instr_dlsym(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite los operandos de @c callni (FIXED_4, 1 reg).
     *
     * Operando textual: r_fn (puntero a funcion nativa ya resuelto via dlsym).
     * Encoding fisico:
     *   [0x00][0x64][b2][b3]   b2 = (r_fn<<4) | 0,  b3 = 0
     *
     * Calling convention: argc en R15, args en R01..R12, retorno en R00
     * (mismo que CALLN estatico).
     */
    void emit_instr_callni(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite los operandos de @c gcallocp (extended 0x65, FIXED_4, 2 regs).
     *
     * Encoding fisico: [0x00][0x65][b2][0x00] con b2 = (r_dst<<4) | r_size.
     * Aloca en GcHeap y deja host_ptr al payload en r_dst (1 instr VM vs 3
     * de la secuencia gcalloc + gcderef + xchg).
     */
    void emit_instr_gcallocp(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite los operandos de @c spawnargs (extended 0x66, FIXED_4, 1 reg).
     *
     * Encoding fisico: [0x00][0x66][b2][0x00] con b2 = (r_pc<<4).  Copia
     * R1..R[R15] del padre al child antes de make_ready.  Calling convention
     * identica a CALLVM (argc en R15, args en R1..R12).
     */
    void emit_instr_spawnargs(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite los operandos de @c fulfillhlt (extended 0x67, FIXED_4, 2 regs).
     *
     * Encoding fisico: [0x00][0x67][b2][0x00] con b2 = (r_fut<<4) | r_value.
     * Combina fulfill + hlt en 1 instruccion para el path critico del helper
     * @Async (cada `return X` del body emite un solo fulfillhlt).
     */
    void emit_instr_fulfillhlt(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite los operandos de @c cmpjmp.cc (extended 0x68, FIXED_8, 2 regs + label).
     *
     * Encoding fisico: [0x00][0x68][b2][cond_byte][target_u32_LE].
     *   b2 = (r_a<<4) | r_b
     *   cond_byte = 0x00..0x0D (mismo set que jmp.j*).
     *   target_u32 = direccion absoluta del label (4 bytes LE; falla si VA > 4GB).
     *
     * El handler extrae el sufijo del mnemonic (".je", ".jne", etc.) para
     * determinar @c cond_byte.  Comparacion signed (cmps).
     */
    void emit_instr_cmpjmp_signed(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite los operandos de @c cmpjmpu.cc (extended 0x69, FIXED_8, 2 regs + label).
     *
     * Identico a @c cmpjmp_signed pero comparacion unsigned (cmpu).
     */
    void emit_instr_cmpjmp_unsigned(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite los operandos de @c decjnz (extended 0x6A, FIXED_8, 1 reg + label).
     *
     * Encoding fisico: [0x00][0x6A][b2][0x00][target_u32_LE].
     *   b2 = (r_counter<<4) | 0
     *   target_u32 = direccion absoluta del label (4 bytes LE).
     *
     * Decremento + branch-if-not-zero en 1 instr.  Ahorra 2 instr por iter
     * en loops `for (i = N; i > 0; i--)`.
     */
    void emit_instr_decjnz(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite los 4 bytes de fastpush / fastpop con bitmask de registros.
     *
     * Formato fisico: el prefijo (0x00 + opcode2) ya fue emitido por el dispatcher
     * de assemble_instruction.  Este emit produce los 2 bytes de mascara:
     *   byte 0: mask & 0xFF       (bits r0..r7)
     *   byte 1: (mask >> 8) & 0xFF (bits r8..r15)
     *
     * El operando debe ser un NumberOperand con valor 0..0xFFFF.
     */
    void emit_instr_fastmask(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    // -------------------------------------------------------------------------
    // Helpers para registros ZMM (f/xmm/ymm/zmm)
    // -------------------------------------------------------------------------

    /**
     * @brief Extrae el indice numerico (0-15) de un nombre de registro ZMM.
     *
     * Acepta los prefijos: "f", "xmm", "ymm", "zmm" seguidos de un numero 0-15.
     * Ejemplo: "xmm3" -> 3, "f7" -> 7, "zmm15" -> 15.
     *
     * @param name Nombre del registro (p.ej. "f0", "xmm3", "ymm15").
     * @return Indice del registro (0-15).
     * @throws std::runtime_error Si el nombre no es un registro ZMM valido.
     */
    inline uint8_t zmm_reg_index(const std::string &name) {
        size_t prefix_len = 0;
        if (!name.empty() && name[0] == 'f') prefix_len = 1;
        else if (name.size() >= 3 && (name.substr(0,3) == "xmm" ||
                                      name.substr(0,3) == "ymm" ||
                                      name.substr(0,3) == "zmm")) prefix_len = 3;
        else throw std::runtime_error("zmm_reg_index: nombre invalido: " + name);

        int idx = std::stoi(name.substr(prefix_len)); // parte numerica
        if (idx < 0 || idx > 15)
            throw std::runtime_error("zmm_reg_index: indice fuera de rango: " + name);
        return static_cast<uint8_t>(idx);
    }

    /**
     * @brief Obtiene el campo mode (2 bits) a partir del nombre de un registro ZMM.
     *
     * mode 0 = escalar f64  -> prefijo "f"
     * mode 1 = XMM (128b)   -> prefijo "xmm"
     * mode 2 = YMM (256b)   -> prefijo "ymm"
     * mode 3 = ZMM (512b)   -> prefijo "zmm"
     *
     * @param name Nombre del registro ZMM.
     * @return Campo mode de 2 bits (0-3).
     */
    inline uint8_t zmm_reg_mode(const std::string &name) {
        if (!name.empty() && name[0] == 'f')    return 0; // escalar f64
        if (name.size() >= 3 && name.substr(0,3) == "xmm") return 1; // XMM 128 bits
        if (name.size() >= 3 && name.substr(0,3) == "ymm") return 2; // YMM 256 bits
        if (name.size() >= 3 && name.substr(0,3) == "zmm") return 3; // ZMM 512 bits
        return 0;
    }

    /**
     * @brief Comprueba si un nombre de registro pertenece al banco ZMM.
     * @param name Nombre del registro a comprobar.
     * @return true si es f0-f15, xmm0-15, ymm0-15 o zmm0-15.
     */
    inline bool is_zmm_register(const std::string &name) {
        if (!name.empty() && name[0] == 'f' && name.size() >= 2 && isdigit((unsigned char)name[1]))
            return true;
        if (name.size() >= 4 && (name.substr(0,3) == "xmm" ||
                                  name.substr(0,3) == "ymm" ||
                                  name.substr(0,3) == "zmm"))
            return true;
        return false;
    }

    // -------------------------------------------------------------------------
    // Emision para instrucciones de corutinas y flotante
    // -------------------------------------------------------------------------

    /**
     * @brief Emite una instruccion de registro ZMM-ZMM (fadd, fsub, fmul, etc.).
     *
     * Byte ctrl: mode(2) | is_f32(1) | 00000
     * Byte regs: idx2(4) | idx1(4)
     *
     * El campo mode se extrae del nombre del registro (f/xmm/ymm/zmm).
     * El campo is_f32 se detecta del sufijo del mnemonico (.ps = float, .pd = double).
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_instr_freg(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite instrucciones ZMM unarias (fsqrt, fabs, fneg) con un solo operando.
     *
     * El registro destino actua tambien como fuente: regs = (idx << 4) | idx.
     * Formato identico a emit_instr_freg pero con un solo operando en el AST.
     *
     * @param instruction_parser Instruccion del AST (un operando ZMM).
     * @param code_final         Escritor de bytecode.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_instr_freg_unary(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite la instruccion FMOWI (inmediato IEEE 754 de 64 bits en ZMM).
     *
     * Formato (11 bytes tras el prefijo 0x00 0xFA):
     *   Byte ctrl: mode(2) | is_f32(1) | zmm_idx(4)
     *   Bytes 1..8: imm64 (IEEE 754 double en bits brutos)
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_instr_fmowi(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite las instrucciones FLOAD y FSTORE (acceso a memoria VM).
     *
     * Byte ctrl: mode(2) | 000000
     * Byte regs: gp_idx(4) | zmm_idx(4)
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_instr_fmem(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite la instruccion FCVT (conversion int<->float entre bancos).
     *
     * Byte ctrl: mode(2) | is_f32(1) | direction(1) | 0000
     * Byte regs: zmm_idx(4) | gp_idx(4)  (o inverso segun direction)
     *
     * @param instruction_parser Instruccion del AST.
     * @param code_final         Escritor de bytecode.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_instr_fcvt(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite instrucciones de string con dos registros (Convention B).
     *
     * Formato FIXED_4: [0x00][opcode2][b2][b3=0]
     *   b2 = (r_dst << 4) | r_src
     *
     * Usos: strlen, strflat, strhash, strintern, strgetenc, strgetbytes,
     *       strgetkind, strreserve, strfinalize, strraw.
     *
     * @param instruction_parser Instruccion con dos operandos registro.
     * @param code_final         Escritor de bytecode.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_str_two_reg(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite STRCONV r_dst, r_src, enc_literal (Convention B + inmediato 4 bits).
     *
     * Formato FIXED_4: [0x00][0x4A][b2][b3]
     *   b2 = (r_dst << 4) | r_src
     *   b3 = (enc_literal << 4)  (nibble alto)
     *
     * @param instruction_parser Instruccion con dos registros y un literal numerico.
     * @param code_final         Escritor de bytecode.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_strconv(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

    /**
     * @brief Emite SETCC r_dst, cond_literal (Convention B + inmediato 4 bits).
     *
     * Formato FIXED_4: [0x00][0x43][b2][b3=0]
     *   b2 = (cond_literal << 4) | r_dst
     *
     * @param instruction_parser Instruccion con un registro y un literal de condicion.
     * @param code_final         Escritor de bytecode.
     * @param now_instr          Descriptor de la instruccion.
     * @param assembly_ctx       Contexto del ensamblador.
     */
    void emit_setcc(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    );

} // namespace Assembly::Bytecode

#endif // EMMIT_DECL_H
