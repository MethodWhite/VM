/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

#ifndef PORT_WASM_WASM_BACKEND_H
#define PORT_WASM_WASM_BACKEND_H

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "ir/ssa_ir.h"
#include "port/transpiler_base.h"
#include "port/wasm/port_options.h"

namespace port {

    static constexpr uint32_t WASM_MAGIC = 0x6D736100;
    static constexpr uint32_t WASM_VERSION = 1;

    enum WasmSectionId : uint8_t {
        WASM_SEC_CUSTOM   = 0,
        WASM_SEC_TYPE     = 1,
        WASM_SEC_IMPORT   = 2,
        WASM_SEC_FUNCTION = 3,
        WASM_SEC_TABLE    = 4,
        WASM_SEC_MEMORY   = 5,
        WASM_SEC_GLOBAL   = 6,
        WASM_SEC_EXPORT   = 7,
        WASM_SEC_START    = 8,
        WASM_SEC_ELEMENT  = 9,
        WASM_SEC_CODE     = 10,
        WASM_SEC_DATA     = 11,
    };

    enum WasmValType : uint8_t {
        WASM_I32 = 0x7F,
        WASM_I64 = 0x7E,
        WASM_F32 = 0x7D,
        WASM_F64 = 0x7C,
    };

    enum WasmOp : uint8_t {
        WASM_OP_UNREACHABLE    = 0x00,
        WASM_OP_NOP            = 0x01,
        WASM_OP_BLOCK          = 0x02,
        WASM_OP_LOOP           = 0x03,
        WASM_OP_IF             = 0x04,
        WASM_OP_ELSE           = 0x05,
        WASM_OP_END            = 0x0B,
        WASM_OP_BR             = 0x0C,
        WASM_OP_BR_IF          = 0x0D,
        WASM_OP_RETURN         = 0x0F,
        WASM_OP_CALL           = 0x10,
        WASM_OP_CALL_INDIRECT  = 0x11,
        WASM_OP_DROP           = 0x1A,
        WASM_OP_LOCAL_GET      = 0x20,
        WASM_OP_LOCAL_SET      = 0x21,
        WASM_OP_LOCAL_TEE      = 0x22,
        WASM_OP_I32_LOAD       = 0x28,
        WASM_OP_I64_LOAD       = 0x29,
        WASM_OP_F32_LOAD       = 0x2A,
        WASM_OP_F64_LOAD       = 0x2B,
        WASM_OP_I32_LOAD8_S    = 0x2C,
        WASM_OP_I32_LOAD8_U    = 0x2D,
        WASM_OP_I32_LOAD16_S   = 0x2E,
        WASM_OP_I32_LOAD16_U   = 0x2F,
        WASM_OP_I64_LOAD8_S    = 0x30,
        WASM_OP_I64_LOAD8_U    = 0x31,
        WASM_OP_I64_LOAD16_S   = 0x32,
        WASM_OP_I64_LOAD16_U   = 0x33,
        WASM_OP_I64_LOAD32_S   = 0x34,
        WASM_OP_I64_LOAD32_U   = 0x35,
        WASM_OP_I32_STORE      = 0x36,
        WASM_OP_I64_STORE      = 0x37,
        WASM_OP_F32_STORE      = 0x38,
        WASM_OP_F64_STORE      = 0x39,
        WASM_OP_I32_STORE8     = 0x3A,
        WASM_OP_I32_STORE16    = 0x3B,
        WASM_OP_I64_STORE8     = 0x3C,
        WASM_OP_I64_STORE16    = 0x3D,
        WASM_OP_I64_STORE32    = 0x3E,
        WASM_OP_MEMORY_SIZE    = 0x3F,
        WASM_OP_MEMORY_GROW    = 0x40,
        WASM_OP_I32_CONST      = 0x41,
        WASM_OP_I64_CONST      = 0x42,
        WASM_OP_F32_CONST      = 0x43,
        WASM_OP_F64_CONST      = 0x44,
        WASM_OP_I32_EQZ        = 0x45,
        WASM_OP_I32_EQ         = 0x46,
        WASM_OP_I32_NE         = 0x47,
        WASM_OP_I32_LT_S       = 0x48,
        WASM_OP_I32_LT_U       = 0x49,
        WASM_OP_I32_GT_S       = 0x4A,
        WASM_OP_I32_GT_U       = 0x4B,
        WASM_OP_I32_LE_S       = 0x4C,
        WASM_OP_I32_LE_U       = 0x4D,
        WASM_OP_I32_GE_S       = 0x4E,
        WASM_OP_I32_GE_U       = 0x4F,
        WASM_OP_I64_EQZ        = 0x50,
        WASM_OP_I64_EQ         = 0x51,
        WASM_OP_I64_NE         = 0x52,
        WASM_OP_I64_LT_S       = 0x53,
        WASM_OP_I64_LT_U       = 0x54,
        WASM_OP_I64_GT_S       = 0x55,
        WASM_OP_I64_GT_U       = 0x56,
        WASM_OP_I64_LE_S       = 0x57,
        WASM_OP_I64_LE_U       = 0x58,
        WASM_OP_I64_GE_S       = 0x59,
        WASM_OP_I64_GE_U       = 0x5A,
        WASM_OP_F32_EQ         = 0x5B,
        WASM_OP_F32_NE         = 0x5C,
        WASM_OP_F32_LT         = 0x5D,
        WASM_OP_F32_GT         = 0x5E,
        WASM_OP_F32_LE         = 0x5F,
        WASM_OP_F32_GE         = 0x60,
        WASM_OP_F64_EQ         = 0x61,
        WASM_OP_F64_NE         = 0x62,
        WASM_OP_F64_LT         = 0x63,
        WASM_OP_F64_GT         = 0x64,
        WASM_OP_F64_LE         = 0x65,
        WASM_OP_F64_GE         = 0x66,
        WASM_OP_I32_CLZ        = 0x67,
        WASM_OP_I32_CTZ        = 0x68,
        WASM_OP_I32_POPCNT     = 0x69,
        WASM_OP_I32_ADD        = 0x6A,
        WASM_OP_I32_SUB        = 0x6B,
        WASM_OP_I32_MUL        = 0x6C,
        WASM_OP_I32_DIV_S      = 0x6D,
        WASM_OP_I32_DIV_U      = 0x6E,
        WASM_OP_I32_REM_S      = 0x6F,
        WASM_OP_I32_REM_U      = 0x70,
        WASM_OP_I32_AND        = 0x71,
        WASM_OP_I32_OR         = 0x72,
        WASM_OP_I32_XOR        = 0x73,
        WASM_OP_I32_SHL        = 0x74,
        WASM_OP_I32_SHR_S      = 0x75,
        WASM_OP_I32_SHR_U      = 0x76,
        WASM_OP_I32_ROTL       = 0x77,
        WASM_OP_I32_ROTR       = 0x78,
        WASM_OP_I64_ADD        = 0x7C,
        WASM_OP_I64_SUB        = 0x7D,
        WASM_OP_I64_MUL        = 0x7E,
        WASM_OP_I64_DIV_S      = 0x7F,
        WASM_OP_I64_DIV_U      = 0x80,
        WASM_OP_I64_REM_S      = 0x81,
        WASM_OP_I64_REM_U      = 0x82,
        WASM_OP_I64_AND        = 0x83,
        WASM_OP_I64_OR         = 0x84,
        WASM_OP_I64_XOR        = 0x85,
        WASM_OP_I64_SHL        = 0x86,
        WASM_OP_I64_SHR_S      = 0x87,
        WASM_OP_I64_SHR_U      = 0x88,
        WASM_OP_I64_ROTL       = 0x89,
        WASM_OP_I64_ROTR       = 0x8A,
        WASM_OP_I64_CLZ        = 0x8B,
        WASM_OP_I64_CTZ        = 0x8C,
        WASM_OP_I64_POPCNT     = 0x8D,
        WASM_OP_F32_ABS        = 0x8B,
        WASM_OP_F32_NEG        = 0x8C,
        WASM_OP_F32_COPY_SIGN  = 0x8D,
        WASM_OP_F32_CEIL       = 0x8E,
        WASM_OP_F32_FLOOR      = 0x8F,
        WASM_OP_F32_TRUNC      = 0x90,
        WASM_OP_F32_NEAREST    = 0x91,
        WASM_OP_F32_SQRT       = 0x92,
        WASM_OP_F32_ADD        = 0x93,
        WASM_OP_F32_SUB        = 0x94,
        WASM_OP_F32_MUL        = 0x95,
        WASM_OP_F32_DIV        = 0x96,
        WASM_OP_F32_MIN        = 0x97,
        WASM_OP_F32_MAX        = 0x98,
        WASM_OP_F64_ABS        = 0x99,
        WASM_OP_F64_NEG        = 0x9A,
        WASM_OP_F64_COPY_SIGN  = 0x9B,
        WASM_OP_F64_CEIL       = 0x9C,
        WASM_OP_F64_FLOOR      = 0x9D,
        WASM_OP_F64_TRUNC      = 0x9E,
        WASM_OP_F64_NEAREST    = 0x9F,
        WASM_OP_F64_SQRT       = 0xA0,
        WASM_OP_F64_ADD        = 0xA1,
        WASM_OP_F64_SUB        = 0xA2,
        WASM_OP_F64_MUL        = 0xA3,
        WASM_OP_F64_DIV        = 0xA4,
        WASM_OP_F64_MIN        = 0xA5,
        WASM_OP_F64_MAX        = 0xA6,
        WASM_OP_I32_WRAP_I64        = 0xA7,
        WASM_OP_I32_TRUNC_F32_S     = 0xA8,
        WASM_OP_I32_TRUNC_F32_U     = 0xA9,
        WASM_OP_I32_TRUNC_F64_S     = 0xAA,
        WASM_OP_I32_TRUNC_F64_U     = 0xAB,
        WASM_OP_I64_EXTEND_I32_S    = 0xAC,
        WASM_OP_I64_EXTEND_I32_U    = 0xAD,
        WASM_OP_I64_TRUNC_F32_S     = 0xAE,
        WASM_OP_I64_TRUNC_F32_U     = 0xAF,
        WASM_OP_I64_TRUNC_F64_S     = 0xB0,
        WASM_OP_I64_TRUNC_F64_U     = 0xB1,
        WASM_OP_F32_CONVERT_I32_S   = 0xB2,
        WASM_OP_F32_CONVERT_I32_U   = 0xB3,
        WASM_OP_F32_CONVERT_I64_S   = 0xB4,
        WASM_OP_F32_CONVERT_I64_U   = 0xB5,
        WASM_OP_F32_DEMOTE_F64      = 0xB6,
        WASM_OP_F64_CONVERT_I32_S   = 0xB7,
        WASM_OP_F64_CONVERT_I32_U   = 0xB8,
        WASM_OP_F64_CONVERT_I64_S   = 0xB9,
        WASM_OP_F64_CONVERT_I64_U   = 0xBA,
        WASM_OP_F64_PROMOTE_F32     = 0xBB,
        WASM_OP_I32_REINTERPRET_F32 = 0xBC,
        WASM_OP_I64_REINTERPRET_F64 = 0xBD,
        WASM_OP_F32_REINTERPRET_I32 = 0xBE,
        WASM_OP_F64_REINTERPRET_I64 = 0xBF,
    };

    enum WasmExportKind : uint8_t {
        WASM_EXPORT_FUNC   = 0x00,
        WASM_EXPORT_TABLE  = 0x01,
        WASM_EXPORT_MEMORY = 0x02,
        WASM_EXPORT_GLOBAL = 0x03,
    };

    class WasmBackend : public IPortBackend {
    public:
        explicit WasmBackend(const WasmPortOptions &opts);

        const char *language_name() const override;
        const char *file_extension() const override;
        std::string type_for(ir::IrType t, bool is_host_ptr) const override;
        std::string void_type() const override;
        std::string default_int_type() const override;
        void emit_prelude(EmitContext &ctx, const ir::IrModule &mod) override;
        void emit_postamble(EmitContext &ctx, const ir::IrModule &mod) override;
        void emit_fn_signature(EmitContext &ctx, const ir::IrFunction &fn) override;
        void emit_fn_open(EmitContext &ctx) override;
        void emit_fn_close(EmitContext &ctx) override;
        void emit_local_decl(EmitContext &ctx, ir::IrValueId id,
                             const ir::IrValue &value) override;
        std::string label_for(ir::IrBlockId id) const override;
        void emit_label_def(EmitContext &ctx, ir::IrBlockId id) override;
        void emit_goto(EmitContext &ctx, ir::IrBlockId target) override;
        void emit_cond_branch(EmitContext &ctx, ir::IrValueId cond,
                              ir::IrBlockId true_id,
                              ir::IrBlockId false_id) override;
        void emit_return(EmitContext &ctx, ir::IrValueId val) override;
        void emit_const(EmitContext &ctx, ir::IrValueId dst, uint64_t imm,
                        ir::IrType t) override;
        void emit_mov(EmitContext &ctx, ir::IrValueId dst, ir::IrValueId src,
                      ir::IrType t) override;
        void emit_binop(EmitContext &ctx, ir::IrOp op, ir::IrValueId dst,
                        ir::IrValueId lhs, ir::IrValueId rhs,
                        ir::IrType t) override;
        void emit_unop(EmitContext &ctx, ir::IrOp op, ir::IrValueId dst,
                       ir::IrValueId src, ir::IrType t) override;
        void emit_cmp(EmitContext &ctx, ir::IrOp op, ir::IrValueId dst,
                      ir::IrValueId lhs, ir::IrValueId rhs,
                      ir::IrType operand_type) override;
        void emit_convert(EmitContext &ctx, ir::IrOp op, ir::IrValueId dst,
                          ir::IrValueId src, ir::IrType dst_type,
                          ir::IrType src_type) override;
        void emit_alloca(EmitContext &ctx, ir::IrValueId dst,
                         uint64_t size_bytes) override;
        void emit_load(EmitContext &ctx, ir::IrValueId dst, ir::IrValueId addr,
                       ir::IrType t, bool is_host_ptr) override;
        void emit_store(EmitContext &ctx, ir::IrValueId val, ir::IrValueId addr,
                        ir::IrType t, bool is_host_ptr) override;
        void emit_raw_alloc(EmitContext &ctx, ir::IrValueId dst,
                            ir::IrValueId size) override;
        void emit_raw_free(EmitContext &ctx, ir::IrValueId ptr) override;
        void emit_call(EmitContext &ctx, ir::IrValueId dst,
                       const std::string &func_name,
                       const std::vector<ir::IrValueId> &args,
                       ir::IrType ret_type) override;
        void emit_call_indirect(EmitContext &ctx, ir::IrValueId dst,
                                ir::IrValueId fn_ptr,
                                const std::vector<ir::IrValueId> &args,
                                ir::IrType ret_type) override;
        void emit_phi_copy(EmitContext &ctx, ir::IrValueId dst,
                           ir::IrValueId src, ir::IrType t) override;

        const std::vector<uint8_t> &binary() const { return binary_; }
        std::vector<uint8_t> take_binary() { return std::move(binary_); }

    private:
        WasmPortOptions opts_;
        std::vector<uint8_t> binary_;

        struct FuncType {
            std::vector<WasmValType> params;
            std::vector<WasmValType> results;
        };
        std::vector<FuncType> func_types_;
        std::vector<uint32_t> func_type_indices_;
        std::vector<std::string> func_names_;

        struct FuncBody {
            std::vector<uint8_t> bytecode;
            uint32_t local_count;
            std::vector<WasmValType> local_types;
            bool is_open;
        };
        FuncBody current_body_;
        bool has_open_function_;

        std::unordered_map<ir::IrValueId, uint32_t> local_map_;
        std::unordered_set<ir::IrValueId> declared_locals_;
        std::unordered_map<ir::IrBlockId, uint32_t> block_map_;

        struct BlockLabel {
            uint32_t depth;
            bool     is_loop;
        };
        std::vector<BlockLabel> block_stack_;
        uint32_t block_counter_;

        const ir::IrFunction *current_fn_;
        const ir::IrModule *current_mod_;

        uint32_t section_start_pos_;

        void write_u32(uint32_t v);
        void write_u16(uint16_t v);
        void write_u8(uint8_t v);
        void write_leb128_u(uint64_t v);
        void write_leb128_s(int64_t v);
        void write_string(const std::string &s);
        void write_val_type(WasmValType t);
        void write_block_type(uint8_t opcode, WasmValType bt);
        WasmValType ir_type_to_wasm(ir::IrType t) const;
        WasmOp load_op_for(ir::IrType t) const;
        WasmOp store_op_for(ir::IrType t) const;
        void write_memarg(uint32_t align, uint32_t offset);
        uint32_t local_for(ir::IrValueId id);
        void ensure_local(ir::IrValueId id, ir::IrType t);
        ir::IrType resolve_type(ir::IrValueId id) const;

        void emit_type_section();
        void emit_function_section();
        void emit_memory_section();
        void emit_export_section();
        void emit_code_section();
        void emit_name_section();
        void begin_section(WasmSectionId id);
        void end_section();
    };

} // namespace port

#endif // PORT_WASM_WASM_BACKEND_H
