/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file ssa_ir_serialize.cpp
 * @brief Implementacion del serializer/deserializer binario del IR.
 *
 * Para cada @c IrInstr serializamos un superset de los campos que
 * podrian estar activos.  Los campos no usados por una op especifica
 * quedan a valores neutros (cero / vacios) y el deserializer no los
 * consulta.  Esto sacrifica ~10-20% de espacio por simplificacion del
 * formato.  Para compactar al maximo, una v2 podria usar un esquema
 * "op -> field set" (variable-length per op).
 */

#include "ir/ssa_ir_serialize.h"

#include <cstring>

namespace ir {

    /* ===================================================================== */
    /* IrValue                                                                */
    /* ===================================================================== */

    namespace {

        constexpr uint8_t IRVAL_FLAG_PARAM            = 1 << 0;
        constexpr uint8_t IRVAL_FLAG_CONST            = 1 << 1;
        constexpr uint8_t IRVAL_FLAG_HOST_PTR         = 1 << 2;
        constexpr uint8_t IRVAL_FLAG_POINTEE_HOST_PTR = 1 << 3;
        constexpr uint8_t IRVAL_FLAG_GC_OBJECT        = 1 << 4;

        constexpr uint8_t INSTR_FLAG_PRESERVE      = 1 << 0;
        constexpr uint8_t INSTR_FLAG_IS_CALL_SITE  = 1 << 1;

        constexpr uint8_t FN_FLAG_NATIVE   = 1 << 0;
        constexpr uint8_t FN_FLAG_VARIADIC = 1 << 1;

        void write_value(std::vector<uint8_t> &o, const IrValue &v) {
            write_u8(o, static_cast<uint8_t>(v.type));
            uint8_t flags = 0;
            if (v.is_param)            flags |= IRVAL_FLAG_PARAM;
            if (v.is_const)            flags |= IRVAL_FLAG_CONST;
            if (v.is_host_ptr)         flags |= IRVAL_FLAG_HOST_PTR;
            if (v.pointee_is_host_ptr) flags |= IRVAL_FLAG_POINTEE_HOST_PTR;
            if (v.is_gc_object)        flags |= IRVAL_FLAG_GC_OBJECT;
            write_u8(o, flags);
            /* const_val solo se emite si is_const para ahorrar 8 bytes
             * por valor no-constante.  El deserializer respeta el flag. */
            if (v.is_const) {
                write_u64(o, v.const_val);
            }
        }

        bool read_value(const std::vector<uint8_t> &in, size_t &off, IrValue &v) {
            uint8_t type_byte = 0, flags = 0;
            if (!read_u8(in, off, type_byte)) return false;
            if (!read_u8(in, off, flags))     return false;
            v.type      = static_cast<IrType>(type_byte);
            v.is_param            = (flags & IRVAL_FLAG_PARAM)            != 0;
            v.is_const            = (flags & IRVAL_FLAG_CONST)            != 0;
            v.is_host_ptr         = (flags & IRVAL_FLAG_HOST_PTR)         != 0;
            v.pointee_is_host_ptr = (flags & IRVAL_FLAG_POINTEE_HOST_PTR) != 0;
            v.is_gc_object        = (flags & IRVAL_FLAG_GC_OBJECT)        != 0;
            if (v.is_const) {
                if (!read_u64(in, off, v.const_val)) return false;
            }
            return true;
        }

        void write_instr(std::vector<uint8_t> &o, const IrInstr &i) {
            write_u16(o, static_cast<uint16_t>(i.op));
            write_u8(o,  static_cast<uint8_t>(i.type));
            write_u32(o, static_cast<uint32_t>(i.dst));
            uint8_t flags = 0;
            if (i.preserve)     flags |= INSTR_FLAG_PRESERVE;
            if (i.is_call_site) flags |= INSTR_FLAG_IS_CALL_SITE;
            write_u8(o, flags);
            write_u32(o, i.source_line);
            write_u64(o, i.imm);
            /* operands */
            const size_t opc = i.operands.size();
            write_u8(o, opc > 255 ? 255 : static_cast<uint8_t>(opc));
            for (size_t k = 0; k < opc && k < 255; ++k) {
                write_u32(o, static_cast<uint32_t>(i.operands[k]));
            }
            /* func_name */
            write_str(o, i.func_name);
            /* func_ptr / target_block / false_block */
            write_u32(o, static_cast<uint32_t>(i.func_ptr));
            write_u32(o, static_cast<uint32_t>(i.target_block));
            write_u32(o, static_cast<uint32_t>(i.false_block));
            /* phi_args */
            const size_t pc = i.phi_args.size();
            write_u8(o, pc > 255 ? 255 : static_cast<uint8_t>(pc));
            for (size_t k = 0; k < pc && k < 255; ++k) {
                write_u32(o, static_cast<uint32_t>(i.phi_args[k].value));
                write_u32(o, static_cast<uint32_t>(i.phi_args[k].block));
            }
        }

        bool read_instr(const std::vector<uint8_t> &in, size_t &off, IrInstr &i) {
            uint16_t op_v = 0;
            uint8_t  type_v = 0, flags = 0;
            uint32_t dst_v = 0, source_line = 0;
            uint64_t imm = 0;
            if (!read_u16(in, off, op_v))         return false;
            if (!read_u8 (in, off, type_v))       return false;
            if (!read_u32(in, off, dst_v))        return false;
            if (!read_u8 (in, off, flags))        return false;
            if (!read_u32(in, off, source_line))  return false;
            if (!read_u64(in, off, imm))          return false;
            i.op            = static_cast<IrOp>(op_v);
            i.type          = static_cast<IrType>(type_v);
            i.dst           = static_cast<IrValueId>(dst_v);
            i.preserve      = (flags & INSTR_FLAG_PRESERVE)      != 0;
            i.is_call_site  = (flags & INSTR_FLAG_IS_CALL_SITE)  != 0;
            i.source_line   = source_line;
            i.imm           = imm;
            /* operands */
            uint8_t opc = 0;
            if (!read_u8(in, off, opc)) return false;
            i.operands.clear();
            i.operands.reserve(opc);
            for (uint8_t k = 0; k < opc; ++k) {
                uint32_t v = 0;
                if (!read_u32(in, off, v)) return false;
                i.operands.push_back(static_cast<IrValueId>(v));
            }
            /* func_name */
            if (!read_str(in, off, i.func_name)) return false;
            /* func_ptr / target_block / false_block */
            uint32_t fp = 0, tb = 0, fb = 0;
            if (!read_u32(in, off, fp)) return false;
            if (!read_u32(in, off, tb)) return false;
            if (!read_u32(in, off, fb)) return false;
            i.func_ptr     = static_cast<IrValueId>(fp);
            i.target_block = static_cast<IrBlockId>(tb);
            i.false_block  = static_cast<IrBlockId>(fb);
            /* phi_args */
            uint8_t pc = 0;
            if (!read_u8(in, off, pc)) return false;
            i.phi_args.clear();
            i.phi_args.reserve(pc);
            for (uint8_t k = 0; k < pc; ++k) {
                uint32_t v = 0, b = 0;
                if (!read_u32(in, off, v)) return false;
                if (!read_u32(in, off, b)) return false;
                IrPhiArg a{static_cast<IrValueId>(v),
                           static_cast<IrBlockId>(b)};
                i.phi_args.push_back(a);
            }
            return true;
        }

        void write_block(std::vector<uint8_t> &o, const IrBlock &b) {
            write_str(o, b.name);
            write_u32(o, static_cast<uint32_t>(b.instrs.size()));
            for (const auto &i : b.instrs) write_instr(o, i);
            write_u32(o, static_cast<uint32_t>(b.preds.size()));
            for (auto p : b.preds) write_u32(o, static_cast<uint32_t>(p));
            write_u32(o, static_cast<uint32_t>(b.succs.size()));
            for (auto s : b.succs) write_u32(o, static_cast<uint32_t>(s));
        }

        bool read_block(const std::vector<uint8_t> &in, size_t &off, IrBlock &b) {
            if (!read_str(in, off, b.name)) return false;
            uint32_t n_instrs = 0;
            if (!read_u32(in, off, n_instrs)) return false;
            b.instrs.clear();
            b.instrs.reserve(n_instrs);
            for (uint32_t k = 0; k < n_instrs; ++k) {
                IrInstr i;
                if (!read_instr(in, off, i)) return false;
                b.instrs.push_back(std::move(i));
            }
            uint32_t n_preds = 0;
            if (!read_u32(in, off, n_preds)) return false;
            b.preds.clear();
            b.preds.reserve(n_preds);
            for (uint32_t k = 0; k < n_preds; ++k) {
                uint32_t v = 0;
                if (!read_u32(in, off, v)) return false;
                b.preds.push_back(static_cast<IrBlockId>(v));
            }
            uint32_t n_succs = 0;
            if (!read_u32(in, off, n_succs)) return false;
            b.succs.clear();
            b.succs.reserve(n_succs);
            for (uint32_t k = 0; k < n_succs; ++k) {
                uint32_t v = 0;
                if (!read_u32(in, off, v)) return false;
                b.succs.push_back(static_cast<IrBlockId>(v));
            }
            return true;
        }

    } // namespace anonymous

    /* ===================================================================== */
    /* serialize_function / deserialize_function                              */
    /* ===================================================================== */

    size_t serialize_function(const IrFunction &fn,
                              std::vector<uint8_t> &out) {
        const size_t start = out.size();

        write_str(out, fn.name);
        write_u8(out, static_cast<uint8_t>(fn.ret_type));
        uint8_t fn_flags = 0;
        if (fn.is_native)   fn_flags |= FN_FLAG_NATIVE;
        if (fn.is_variadic) fn_flags |= FN_FLAG_VARIADIC;
        write_u8(out, fn_flags);

        /* params */
        write_u32(out, static_cast<uint32_t>(fn.params.size()));
        for (auto p : fn.params) write_u32(out, static_cast<uint32_t>(p));

        /* values */
        write_u32(out, static_cast<uint32_t>(fn.values.size()));
        for (const auto &v : fn.values) write_value(out, v);

        /* blocks */
        write_u32(out, static_cast<uint32_t>(fn.blocks.size()));
        for (const auto &b : fn.blocks) write_block(out, b);

        /* B.3 metadata */
        write_str(out, fn.generic_template_name);
        write_u32(out, static_cast<uint32_t>(fn.generic_type_args.size()));
        for (const auto &s : fn.generic_type_args) write_str(out, s);

        return out.size() - start;
    }

    bool deserialize_function(const std::vector<uint8_t> &in,
                              size_t &off,
                              IrFunction &out) {
        out = IrFunction{};

        if (!read_str(in, off, out.name)) return false;
        uint8_t ret_type_b = 0, fn_flags = 0;
        if (!read_u8(in, off, ret_type_b)) return false;
        if (!read_u8(in, off, fn_flags))   return false;
        out.ret_type    = static_cast<IrType>(ret_type_b);
        out.is_native   = (fn_flags & FN_FLAG_NATIVE)   != 0;
        out.is_variadic = (fn_flags & FN_FLAG_VARIADIC) != 0;

        /* params */
        uint32_t n_params = 0;
        if (!read_u32(in, off, n_params)) return false;
        out.params.clear();
        out.params.reserve(n_params);
        for (uint32_t k = 0; k < n_params; ++k) {
            uint32_t v = 0;
            if (!read_u32(in, off, v)) return false;
            out.params.push_back(static_cast<IrValueId>(v));
        }

        /* values */
        uint32_t n_values = 0;
        if (!read_u32(in, off, n_values)) return false;
        out.values.clear();
        out.values.reserve(n_values);
        for (uint32_t k = 0; k < n_values; ++k) {
            IrValue v;
            v.id = static_cast<IrValueId>(k);  /* id es el indice en el vector */
            if (!read_value(in, off, v)) return false;
            out.values.push_back(std::move(v));
        }

        /* blocks */
        uint32_t n_blocks = 0;
        if (!read_u32(in, off, n_blocks)) return false;
        out.blocks.clear();
        out.blocks.reserve(n_blocks);
        for (uint32_t k = 0; k < n_blocks; ++k) {
            IrBlock b;
            b.id = static_cast<IrBlockId>(k);  /* id es el indice */
            if (!read_block(in, off, b)) return false;
            out.blocks.push_back(std::move(b));
        }

        /* metadata */
        if (!read_str(in, off, out.generic_template_name)) return false;
        uint32_t n_args = 0;
        if (!read_u32(in, off, n_args)) return false;
        out.generic_type_args.clear();
        out.generic_type_args.reserve(n_args);
        for (uint32_t k = 0; k < n_args; ++k) {
            std::string s;
            if (!read_str(in, off, s)) return false;
            out.generic_type_args.push_back(std::move(s));
        }
        return true;
    }

    /* ===================================================================== */
    /* emit_ir_section / parse_ir_section                                     */
    /* ===================================================================== */

    std::vector<uint8_t> emit_ir_section(const std::vector<IrFunction> &functions) {
        std::vector<uint8_t> out;
        out.reserve(64 + functions.size() * 512);  /* estimacion conservadora */

        /* Header de la seccion. */
        write_u32(out, IR_SECTION_MAGIC);
        write_u16(out, IR_SECTION_VERSION);
        write_u16(out, 0);  /* reserved */
        write_u32(out, static_cast<uint32_t>(functions.size()));

        /* Funciones concatenadas. */
        for (const auto &fn : functions) {
            (void)serialize_function(fn, out);
        }

        return out;
    }

    bool parse_ir_section(const std::vector<uint8_t> &data,
                          size_t offset,
                          size_t section_size,
                          std::vector<IrFunction> &functions) {
        functions.clear();

        if (section_size < 12) return false;  /* header minimo */
        if (offset + section_size > data.size()) return false;

        size_t off = offset;
        uint32_t magic = 0;
        if (!read_u32(data, off, magic)) return false;
        if (magic != IR_SECTION_MAGIC) return false;

        uint16_t version = 0;
        uint16_t reserved = 0;
        if (!read_u16(data, off, version)) return false;
        if (!read_u16(data, off, reserved)) return false;
        if (version != IR_SECTION_VERSION) return false;

        uint32_t fn_count = 0;
        if (!read_u32(data, off, fn_count)) return false;

        /* Hard cap defensivo: programas con >100000 funciones IR son
         * imposibles en la practica.  Protege contra section_size
         * malicioso. */
        if (fn_count > 100000) return false;

        functions.reserve(fn_count);
        const size_t section_end = offset + section_size;
        for (uint32_t i = 0; i < fn_count; ++i) {
            if (off >= section_end) return false;
            IrFunction fn;
            if (!deserialize_function(data, off, fn)) return false;
            if (off > section_end) return false;
            functions.push_back(std::move(fn));
        }
        return true;
    }

} // namespace ir
