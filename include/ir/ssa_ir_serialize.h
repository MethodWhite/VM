/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file ssa_ir_serialize.h
 * @brief Serializacion binaria compacta de @c IrFunction para persistir
 *        IR junto al bytecode en `.velb`
 *
 * = Diseno =
 *
 * Formato binario little-endian, sin padding, sin alineacion (cada
 * lectura va con memcpy 1/2/4/8 bytes).  Las cstrings llevan u32 length
 * + bytes (sin terminador nulo).  Las constantes pequenas son u8/u16
 * donde caben; los IDs de valores/blocks van como u32.
 *
 * = Layout =
 *
 *   IrFunction:
 *     name            cstring  (u32 len + bytes)
 *     ret_type        u8       (IrType enum value)
 *     flags           u8       (bit0=is_native, bit1=is_variadic)
 *     param_count     u32
 *     params          u32 * param_count
 *     value_count     u32
 *     for each value: u8 type + u8 flags(host_ptr|gc_object|pointee_host_ptr|...)
 *                     + u64 const_val (solo si bit is_const)
 *     block_count     u32
 *     for each block:
 *         name        cstring
 *         instr_count u32
 *         for each instr:
 *             op           u16
 *             type         u8
 *             dst          u32 (IR_NO_VALUE = 0xFFFFFFFF)
 *             flags        u8 (bit0=preserve, bit1=is_call_site)
 *             source_line  u32
 *             imm          u64
 *             operand_count u8
 *             operands     u32 * operand_count
 *             func_name    cstring (vacia si no aplica)
 *             func_ptr     u32 (IR_NO_VALUE si no aplica)
 *             target_block u32 (IR_NO_BLOCK si no aplica)
 *             false_block  u32 (idem)
 *             phi_count    u8
 *             phi_args     (u32 value + u32 block) * phi_count
 *         pred_count       u32
 *         preds            u32 * pred_count
 *         succ_count       u32
 *         succs            u32 * succ_count
 *     template_name        cstring (vacia si no aplica - B.3)
 *     type_args_count      u32
 *     type_args            cstring * type_args_count
 *
 * = Tamano tipico =
 *
 *   Funcion pequena (~10 instrs): ~300 bytes serializada.
 *   Funcion media (100 instrs): ~3 KB.
 *   Modulo Vex tipico (`vexed_modular.vex` ~30 fns x 50 instrs/fn):
 *      ~30 * 1.5 KB = 45 KB de IR vs ~150 KB de bytecode -> 1.3x total.
 *
 *   La compresion LZ4 (no implementada en v1) reduce IR ~3x adicional.
 */

#ifndef VESTA_IR_SSA_IR_SERIALIZE_H
#define VESTA_IR_SSA_IR_SERIALIZE_H

#include <cstdint>
#include <vector>

#include "ir/ssa_ir.h"

namespace ir {

    /**
     * @brief Serializa una @c IrFunction a bytes en @p out.
     *        Append (no clear) -- permite serializar varias funciones
     *        a un mismo buffer.
     * @return numero de bytes escritos para esta funcion.
     */
    size_t serialize_function(const IrFunction &fn,
                              std::vector<uint8_t> &out);

    /**
     * @brief Deserializa una @c IrFunction desde @p in empezando en
     *        @p offset.  Avanza @p offset a la posicion siguiente.
     * @return true si la deserializacion fue exitosa.  En caso de
     *         error, @p out puede quedar en estado parcial.
     */
    bool deserialize_function(const std::vector<uint8_t> &in,
                              size_t &offset,
                              IrFunction &out);

    /* ===================================================================== */
    /* Seccion @ir del archivo .velb                                         */
    /* ===================================================================== */

    /**
     * @brief Magic "VEIR" (little-endian) que abre la seccion @ir.
     *        El loader valida esto antes de deserializar para detectar
     *        corrupcion / version incorrecta de manera temprana.
     */
    static constexpr uint32_t IR_SECTION_MAGIC = 0x52494556U; /* 'V''E''I''R' */

    /**
     * @brief Version del formato @ir.  Bump cuando cambia el layout.
     */
    static constexpr uint16_t IR_SECTION_VERSION = 1;

    /**
     * @brief Emit del bytes de la seccion @c @ir lista para append a
     *        un `.velb`.  Layout:
     *
     *            +0  [4]  magic "VEIR"
     *            +4  [2]  version (u16, IR_SECTION_VERSION)
     *            +6  [2]  reserved (0)
     *            +8  [4]  function_count
     *            +12 [..] functions (concat de serialize_function output)
     *
     * @param functions IR functions a incluir en la seccion.
     * @return bytes serializados.  El caller los anyade al .velb y
     *         escribe el offset/size en el header.
     */
    std::vector<uint8_t> emit_ir_section(const std::vector<IrFunction> &functions);

    /**
     * @brief Parse de la seccion @c @ir desde @p data leyendo
     *        @p section_size bytes a partir de @p offset.
     *
     * Valida magic + version antes de deserializar las funciones.
     * Si magic/version no coinciden, retorna false y deja @c functions
     * vacio (puede ser un .velb v2 sin IR -> graceful degradation).
     *
     * @return true si parseo exitoso; false si magic/version invalido
     *         o si alguna funcion fallo deserializacion.
     */
    bool parse_ir_section(const std::vector<uint8_t> &data,
                          size_t offset,
                          size_t section_size,
                          std::vector<IrFunction> &functions);

    /* Helpers expuestos para tests / linker (escritura en buffer). */
    inline void write_u8(std::vector<uint8_t> &o, uint8_t v) {
        o.push_back(v);
    }
    inline void write_u16(std::vector<uint8_t> &o, uint16_t v) {
        o.push_back(static_cast<uint8_t>(v));
        o.push_back(static_cast<uint8_t>(v >> 8));
    }
    inline void write_u32(std::vector<uint8_t> &o, uint32_t v) {
        for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    inline void write_u64(std::vector<uint8_t> &o, uint64_t v) {
        for (int i = 0; i < 8; ++i) o.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    inline void write_str(std::vector<uint8_t> &o, const std::string &s) {
        write_u32(o, static_cast<uint32_t>(s.size()));
        o.insert(o.end(), s.begin(), s.end());
    }

    /* Helpers de lectura (con bounds checking). */
    inline bool read_u8(const std::vector<uint8_t> &in, size_t &off, uint8_t &out) {
        if (off >= in.size()) return false;
        out = in[off]; ++off; return true;
    }
    inline bool read_u16(const std::vector<uint8_t> &in, size_t &off, uint16_t &out) {
        if (off + 2 > in.size()) return false;
        out = static_cast<uint16_t>(in[off])
            | (static_cast<uint16_t>(in[off + 1]) << 8);
        off += 2; return true;
    }
    inline bool read_u32(const std::vector<uint8_t> &in, size_t &off, uint32_t &out) {
        if (off + 4 > in.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            out |= static_cast<uint32_t>(in[off + i]) << (i * 8);
        }
        off += 4; return true;
    }
    inline bool read_u64(const std::vector<uint8_t> &in, size_t &off, uint64_t &out) {
        if (off + 8 > in.size()) return false;
        out = 0;
        for (int i = 0; i < 8; ++i) {
            out |= static_cast<uint64_t>(in[off + i]) << (i * 8);
        }
        off += 8; return true;
    }
    inline bool read_str(const std::vector<uint8_t> &in, size_t &off, std::string &out) {
        uint32_t len = 0;
        if (!read_u32(in, off, len)) return false;
        if (off + len > in.size()) return false;
        out.assign(reinterpret_cast<const char *>(in.data() + off), len);
        off += len; return true;
    }

} // namespace ir

#endif // VESTA_IR_SSA_IR_SERIALIZE_H
