/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

#include "port/wasm/wasm_backend.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <sstream>

namespace port {

    // =========================================================================
    //  Constructor
    // =========================================================================

    WasmBackend::WasmBackend(const WasmPortOptions &opts)
        : opts_(opts)
        , has_open_function_(false)
        , block_counter_(0)
        , current_fn_(nullptr)
        , current_mod_(nullptr)
        , section_start_pos_(0)
    {
        current_body_.is_open = false;
        current_body_.local_count = 0;
    }

    // =========================================================================
    //  Identificacion
    // =========================================================================

    const char *WasmBackend::language_name() const {
        return "WebAssembly";
    }

    const char *WasmBackend::file_extension() const {
        return "wasm";
    }

    // =========================================================================
    //  Tipos
    // =========================================================================

    std::string WasmBackend::type_for(ir::IrType t, bool is_host_ptr) const {
        switch (t) {
            case ir::IrType::VOID:   return "void";
            case ir::IrType::I8:
            case ir::IrType::I16:
            case ir::IrType::I32:    return "i32";
            case ir::IrType::I64:    return "i64";
            case ir::IrType::U8:
            case ir::IrType::U16:
            case ir::IrType::U32:    return "i32";
            case ir::IrType::U64:    return "i64";
            case ir::IrType::F32:    return "f32";
            case ir::IrType::F64:    return "f64";
            case ir::IrType::PTR:    return "i32";
            case ir::IrType::HANDLE: return "i32";
            case ir::IrType::BOOL:   return "i32";
        }
        return "i64";
    }

    std::string WasmBackend::void_type() const { return "void"; }
    std::string WasmBackend::default_int_type() const { return "i64"; }

    // =========================================================================
    //  Helpers de encoding
    // =========================================================================

    void WasmBackend::write_u32(uint32_t v) {
        binary_.push_back(static_cast<uint8_t>(v));
        binary_.push_back(static_cast<uint8_t>(v >> 8));
        binary_.push_back(static_cast<uint8_t>(v >> 16));
        binary_.push_back(static_cast<uint8_t>(v >> 24));
    }

    void WasmBackend::write_u16(uint16_t v) {
        binary_.push_back(static_cast<uint8_t>(v));
        binary_.push_back(static_cast<uint8_t>(v >> 8));
    }

    void WasmBackend::write_u8(uint8_t v) {
        binary_.push_back(v);
    }

    void WasmBackend::write_leb128_u(uint64_t v) {
        do {
            uint8_t byte = v & 0x7F;
            v >>= 7;
            if (v) byte |= 0x80;
            binary_.push_back(byte);
        } while (v);
    }

    void WasmBackend::write_leb128_s(int64_t v) {
        bool more = true;
        while (more) {
            uint8_t byte = v & 0x7F;
            v >>= 7;
            if ((v == 0 && (byte & 0x40) == 0) || (v == -1 && (byte & 0x40))) {
                more = false;
            } else {
                byte |= 0x80;
            }
            binary_.push_back(byte);
        }
    }

    void WasmBackend::write_string(const std::string &s) {
        write_leb128_u(s.size());
        binary_.insert(binary_.end(), s.begin(), s.end());
    }

    void WasmBackend::write_val_type(WasmValType t) {
        write_u8(static_cast<uint8_t>(t));
    }

    void WasmBackend::write_block_type(uint8_t opcode, WasmValType bt) {
        write_u8(opcode);
        write_val_type(bt);
    }

    void WasmBackend::write_memarg(uint32_t align, uint32_t offset) {
        write_leb128_u(align);
        write_leb128_u(offset);
    }

    // =========================================================================
    //  Mapeo de tipos
    // =========================================================================

    WasmValType WasmBackend::ir_type_to_wasm(ir::IrType t) const {
        switch (t) {
            case ir::IrType::I8:
            case ir::IrType::I16:
            case ir::IrType::I32:
            case ir::IrType::U8:
            case ir::IrType::U16:
            case ir::IrType::U32:
            case ir::IrType::BOOL:
            case ir::IrType::PTR:
            case ir::IrType::HANDLE:
                return WASM_I32;
            case ir::IrType::I64:
            case ir::IrType::U64:
                return WASM_I64;
            case ir::IrType::F32:
                return WASM_F32;
            case ir::IrType::F64:
                return WASM_F64;
            default:
                return WASM_I64;
        }
    }

    WasmOp WasmBackend::load_op_for(ir::IrType t) const {
        switch (t) {
            case ir::IrType::I8:  return WASM_OP_I32_LOAD8_S;
            case ir::IrType::U8:  return WASM_OP_I32_LOAD8_U;
            case ir::IrType::I16: return WASM_OP_I32_LOAD16_S;
            case ir::IrType::U16: return WASM_OP_I32_LOAD16_U;
            case ir::IrType::I32:
            case ir::IrType::U32: return WASM_OP_I32_LOAD;
            case ir::IrType::I64:
            case ir::IrType::U64: return WASM_OP_I64_LOAD;
            case ir::IrType::F32: return WASM_OP_F32_LOAD;
            case ir::IrType::F64: return WASM_OP_F64_LOAD;
            case ir::IrType::PTR:
            case ir::IrType::HANDLE:
            case ir::IrType::BOOL: return WASM_OP_I32_LOAD;
            default: return WASM_OP_I64_LOAD;
        }
    }

    WasmOp WasmBackend::store_op_for(ir::IrType t) const {
        switch (t) {
            case ir::IrType::I8:
            case ir::IrType::U8:  return WASM_OP_I32_STORE8;
            case ir::IrType::I16:
            case ir::IrType::U16: return WASM_OP_I32_STORE16;
            case ir::IrType::I32:
            case ir::IrType::U32:
            case ir::IrType::PTR:
            case ir::IrType::HANDLE:
            case ir::IrType::BOOL: return WASM_OP_I32_STORE;
            case ir::IrType::I64:
            case ir::IrType::U64: return WASM_OP_I64_STORE;
            case ir::IrType::F32: return WASM_OP_F32_STORE;
            case ir::IrType::F64: return WASM_OP_F64_STORE;
            default: return WASM_OP_I64_STORE;
        }
    }

    // =========================================================================
    //  Gestion de locales
    // =========================================================================

    ir::IrType WasmBackend::resolve_type(ir::IrValueId id) const {
        if (current_fn_ && id < current_fn_->values.size()) {
            return current_fn_->values[id].type;
        }
        return ir::IrType::I64;
    }

    uint32_t WasmBackend::local_for(ir::IrValueId id) {
        auto it = local_map_.find(id);
        if (it != local_map_.end()) return it->second;
        return 0;
    }

    void WasmBackend::ensure_local(ir::IrValueId id, ir::IrType t) {
        if (declared_locals_.count(id)) return;
        declared_locals_.insert(id);
        uint32_t idx = current_body_.local_count;
        local_map_[id] = idx;
        current_body_.local_types.push_back(ir_type_to_wasm(t));
        current_body_.local_count++;
    }

    // =========================================================================
    //  Gestion de secciones
    // =========================================================================

    void WasmBackend::begin_section(WasmSectionId id) {
        section_start_pos_ = binary_.size();
        write_u8(static_cast<uint8_t>(id));
        write_u32(0); // placeholder size, will be patched
    }

    void WasmBackend::end_section() {
        uint32_t end_pos = binary_.size();
        uint32_t content_size = end_pos - section_start_pos_ - 5;
        binary_[section_start_pos_ + 1] = content_size & 0xFF;
        binary_[section_start_pos_ + 2] = (content_size >> 8) & 0xFF;
        binary_[section_start_pos_ + 3] = (content_size >> 16) & 0xFF;
        binary_[section_start_pos_ + 4] = (content_size >> 24) & 0xFF;
    }

    // =========================================================================
    //  Construccion de secciones
    // =========================================================================

    void WasmBackend::emit_type_section() {
        if (func_types_.empty()) return;
        begin_section(WASM_SEC_TYPE);
        write_leb128_u(func_types_.size());
        for (const auto &ft : func_types_) {
            write_u8(0x60); // functype
            write_leb128_u(ft.params.size());
            for (auto p : ft.params) write_val_type(p);
            write_leb128_u(ft.results.size());
            for (auto r : ft.results) write_val_type(r);
        }
        end_section();
    }

    void WasmBackend::emit_function_section() {
        if (func_type_indices_.empty()) return;
        begin_section(WASM_SEC_FUNCTION);
        write_leb128_u(func_type_indices_.size());
        for (auto idx : func_type_indices_) {
            write_leb128_u(idx);
        }
        end_section();
    }

    void WasmBackend::emit_memory_section() {
        begin_section(WASM_SEC_MEMORY);
        write_leb128_u(1); // 1 memory
        if (opts_.max_memory_pages > 0) {
            write_u8(0x01); // flags: has max
            write_leb128_u(opts_.initial_memory_pages);
            write_leb128_u(opts_.max_memory_pages);
        } else {
            write_u8(0x00);
            write_leb128_u(opts_.initial_memory_pages);
        }
        end_section();
    }

    void WasmBackend::emit_export_section() {
        begin_section(WASM_SEC_EXPORT);
        size_t export_count = 0;

        std::vector<std::pair<std::string, uint32_t>> func_exports;
        for (size_t i = 0; i < func_names_.size(); ++i) {
            bool is_main = (func_names_[i] == "main");
            if (is_main || func_names_[i].find("vex_main") != std::string::npos) {
                func_exports.push_back({"main", static_cast<uint32_t>(i)});
            }
        }

        // Also memory export
        size_t total_exports = func_exports.size() + 1;
        if (opts_.emit_start_function) total_exports++;

        write_leb128_u(total_exports);

        for (const auto &fe : func_exports) {
            write_string(fe.first);
            write_u8(WASM_EXPORT_FUNC);
            write_leb128_u(fe.second);
        }

        // Export memory as "memory"
        write_string("memory");
        write_u8(WASM_EXPORT_MEMORY);
        write_leb128_u(0);

        if (opts_.emit_start_function) {
            write_string("_start");
            write_u8(WASM_EXPORT_FUNC);
            write_leb128_u(0);
        }

        end_section();
    }

    void WasmBackend::emit_code_section() {
        if (func_type_indices_.empty()) return;
        begin_section(WASM_SEC_CODE);

        // We've been accumulating bytecode per function in current_body_
        // but since the emit model gives us one function at a time,
        // we need to finalize each function separately.
        // For now, we emit a placeholder count and the actual bodies
        // are stored and emitted here.

        size_t func_count = func_type_indices_.size();
        write_leb128_u(func_count);

        // In this model, each function body is a separate entry.
        // Since we process functions one at a time through emit_fn_open/close,
        // the bytecode is accumulated in current_body_ and needs to be flushed.
        // However, due to how the transpiler calls emit_fn_signature before
        // each function, we batch all function bodies here.

        // For correctness, each function's body is written as a code entry.
        // We track function bodies in a list.
        // For this implementation, we assume at least one function.
        for (size_t i = 0; i < func_count; ++i) {
            // For the first version, we write a minimal valid function body
            // that does nothing but return.
            // In the future, this will contain the accumulated bytecode.
            std::vector<uint8_t> body_bytes;
            body_bytes.reserve(64);

            // Locals declaration: count of local declarations
            // Format: count (LEB128), then (count_type, val_type) pairs
            // Count locals grouped by type
            std::vector<uint8_t> local_decls;
            uint32_t i32_count = 0, i64_count = 0, f32_count = 0, f64_count = 0;

            // We need to count locals from our tracked per-function data
            for (auto lt : current_body_.local_types) {
                switch (lt) {
                    case WASM_I32: i32_count++; break;
                    case WASM_I64: i64_count++; break;
                    case WASM_F32: f32_count++; break;
                    case WASM_F64: f64_count++; break;
                }
            }

            // Write grouped local declarations
            auto emit_local_group = [&](uint32_t count, WasmValType type) {
                if (count == 0) return;
                uint8_t buf[16];
                size_t pos = 0;
                uint64_t v = count;
                do {
                    buf[pos++] = (v & 0x7F) | (v >> 7 ? 0x80 : 0);
                    v >>= 7;
                } while (v);
                local_decls.insert(local_decls.end(), buf, buf + pos);
                local_decls.push_back(static_cast<uint8_t>(type));
            };

            emit_local_group(i32_count, WASM_I32);
            emit_local_group(i64_count, WASM_I64);
            emit_local_group(f32_count, WASM_F32);
            emit_local_group(f64_count, WASM_F64);

            // Build full body: local declarations + bytecode + end
            std::vector<uint8_t> full_body;
            full_body.reserve(4 + local_decls.size() + current_body_.bytecode.size() + 1);

            // Number of local declaration groups
            uint32_t num_groups = (i32_count > 0) + (i64_count > 0) + (f32_count > 0) + (f64_count > 0);
            uint8_t num_groups_buf[16];
            size_t ng_pos = 0;
            uint64_t ng = num_groups;
            do {
                num_groups_buf[ng_pos++] = (ng & 0x7F) | (ng >> 7 ? 0x80 : 0);
                ng >>= 7;
            } while (ng);
            full_body.insert(full_body.end(), num_groups_buf, num_groups_buf + ng_pos);

            full_body.insert(full_body.end(), local_decls.begin(), local_decls.end());
            full_body.insert(full_body.end(), current_body_.bytecode.begin(), current_body_.bytecode.end());
            full_body.push_back(WASM_OP_END);

            write_leb128_u(full_body.size());
            binary_.insert(binary_.end(), full_body.begin(), full_body.end());
        }

        end_section();
    }

    void WasmBackend::emit_name_section() {
        begin_section(WASM_SEC_CUSTOM);
        write_string("name");

        // Subsection: function names
        write_u8(0x01); // function name subsection
        uint32_t names_start = binary_.size();
        write_u32(0); // placeholder size

        write_leb128_u(func_names_.size());
        for (size_t i = 0; i < func_names_.size(); ++i) {
            write_leb128_u(i);
            write_string(func_names_[i]);
        }

        // Patch subsection size
        uint32_t names_end = binary_.size();
        uint32_t names_size = names_end - names_start - 4;
        binary_[names_start]     = names_size & 0xFF;
        binary_[names_start + 1] = (names_size >> 8) & 0xFF;
        binary_[names_start + 2] = (names_size >> 16) & 0xFF;
        binary_[names_start + 3] = (names_size >> 24) & 0xFF;

        end_section();
    }

    // =========================================================================
    //  Prelude / Postamble
    // =========================================================================

    void WasmBackend::emit_prelude(EmitContext &ctx, const ir::IrModule &mod) {
        (void)ctx;
        current_mod_ = &mod;

        // Write WASM header
        write_u32(WASM_MAGIC);
        write_u32(WASM_VERSION);

        // Reset state
        func_types_.clear();
        func_type_indices_.clear();
        func_names_.clear();
        current_body_ = FuncBody();
        current_body_.is_open = false;
        has_open_function_ = false;
        local_map_.clear();
        declared_locals_.clear();
        block_map_.clear();
        block_stack_.clear();
        block_counter_ = 0;
    }

    void WasmBackend::emit_postamble(EmitContext &ctx, const ir::IrModule &mod) {
        (void)ctx;
        (void)mod;

        // Finalize all sections
        emit_type_section();
        emit_function_section();

        // Emit table section (needed for call_indirect)
        begin_section(WASM_SEC_TABLE);
        write_leb128_u(1); // 1 table
        write_u8(0x70);    // funcref type
        write_u8(0x00);    // flags: has min, no max
        write_leb128_u(0); // min: 0 elements (will be filled at runtime)
        end_section();

        emit_memory_section();
        emit_export_section();
        emit_code_section();
        emit_name_section();
    }

    // =========================================================================
    //  Estructura de funcion
    // =========================================================================

    void WasmBackend::emit_fn_signature(EmitContext &ctx, const ir::IrFunction &fn) {
        current_fn_ = &fn;

        // Build function type
        FuncType ft;
        for (auto param_id : fn.params) {
            ir::IrType pt = resolve_type(param_id);
            ft.params.push_back(ir_type_to_wasm(pt));
        }
        if (fn.ret_type != ir::IrType::VOID) {
            ft.results.push_back(ir_type_to_wasm(fn.ret_type));
        }

        // Deduplicate: check if this type already exists
        uint32_t type_idx = 0;
        bool found = false;
        for (size_t i = 0; i < func_types_.size(); ++i) {
            if (func_types_[i].params.size() == ft.params.size() &&
                func_types_[i].results.size() == ft.results.size()) {
                bool match = true;
                for (size_t j = 0; j < ft.params.size(); ++j) {
                    if (func_types_[i].params[j] != ft.params[j]) { match = false; break; }
                }
                if (match) {
                    for (size_t j = 0; j < ft.results.size(); ++j) {
                        if (func_types_[i].results[j] != ft.results[j]) { match = false; break; }
                    }
                }
                if (match) { type_idx = static_cast<uint32_t>(i); found = true; break; }
            }
        }

        if (!found) {
            type_idx = static_cast<uint32_t>(func_types_.size());
            func_types_.push_back(ft);
        }

        func_type_indices_.push_back(type_idx);

        // Build function name
        std::string fname = fn.name;
        if (fname == "main") fname = "vex_main";
        func_names_.push_back(fname);

        // Map parameters to local indices
        uint32_t local_idx = 0;
        for (auto param_id : fn.params) {
            local_map_[param_id] = local_idx++;
            declared_locals_.insert(param_id);
        }
        current_body_.local_count = local_idx;

        ctx.out << ";; function " << fn.name << "\n";
    }

    void WasmBackend::emit_fn_open(EmitContext &ctx) {
        (void)ctx;
        has_open_function_ = true;
        current_body_.is_open = true;
        current_body_.bytecode.clear();
        current_body_.local_types.clear();
        block_map_.clear();
        block_stack_.clear();
        block_counter_ = 0;
    }

    void WasmBackend::emit_fn_close(EmitContext &ctx) {
        (void)ctx;
        if (has_open_function_) {
            current_body_.is_open = false;
            has_open_function_ = false;
        }
        current_fn_ = nullptr;
    }

    void WasmBackend::emit_local_decl(EmitContext &ctx, ir::IrValueId id,
                                      const ir::IrValue &value) {
        (void)ctx;
        if (!has_open_function_) return;
        ensure_local(id, value.type);
    }

    // =========================================================================
    //  Bloques y control flow
    // =========================================================================

    std::string WasmBackend::label_for(ir::IrBlockId id) const {
        return "bb_" + std::to_string(id);
    }

    void WasmBackend::emit_label_def(EmitContext &ctx, ir::IrBlockId id) {
        (void)ctx;
        if (!current_body_.is_open) return;

        // Begin a WASM block for this basic block
        // Track the block depth for later br/br_if
        uint32_t depth = block_counter_++;
        block_map_[id] = depth;
        block_stack_.push_back({depth, false});

        // In WASM, we use block/end to delimit blocks.
        // We emit a block instruction now, and will emit end when the
        // block's code is done (in emit_goto or emit_fn_close).
        // Actually, since we process blocks linearly, we emit block
        // before instructions and end after the terminators.
        write_block_type(WASM_OP_BLOCK, WASM_I32);
    }

    void WasmBackend::emit_goto(EmitContext &ctx, ir::IrBlockId target) {
        (void)ctx;
        if (!current_body_.is_open) return;

        // Find the target block depth
        auto it = block_map_.find(target);
        if (it != block_map_.end()) {
            uint32_t depth = block_counter_ - it->second - 1;
            write_u8(WASM_OP_BR);
            write_leb128_u(depth);
        } else {
            // Forward reference: use depth 0 (relative to current block)
            write_u8(WASM_OP_BR);
            write_leb128_u(0);
        }
    }

    void WasmBackend::emit_cond_branch(EmitContext &ctx, ir::IrValueId cond,
                                       ir::IrBlockId true_id,
                                       ir::IrBlockId false_id) {
        if (!current_body_.is_open) return;

        // Emit condition value
        write_u8(WASM_OP_LOCAL_GET);
        write_leb128_u(local_for(cond));

        // Build if/else/end structure
        write_block_type(WASM_OP_IF, WASM_I32);

        // True branch: br to true_id, then skip else
        auto tit = block_map_.find(true_id);
        if (tit != block_map_.end()) {
            write_u8(WASM_OP_BR);
            write_leb128_u(block_counter_ - tit->second - 1);
        }
        write_u8(WASM_OP_ELSE);

        // False branch
        auto fit = block_map_.find(false_id);
        if (fit != block_map_.end()) {
            write_u8(WASM_OP_BR);
            write_leb128_u(block_counter_ - fit->second - 1);
        }
        write_u8(WASM_OP_END);
    }

    void WasmBackend::emit_return(EmitContext &ctx, ir::IrValueId val) {
        if (!current_body_.is_open) return;

        if (val != ir::IR_NO_VALUE) {
            write_u8(WASM_OP_LOCAL_GET);
            write_leb128_u(local_for(val));
        }
        write_u8(WASM_OP_RETURN);
    }

    // =========================================================================
    //  Instrucciones escalares
    // =========================================================================

    void WasmBackend::emit_const(EmitContext &ctx, ir::IrValueId dst,
                                 uint64_t imm, ir::IrType t) {
        if (!current_body_.is_open) return;
        ensure_local(dst, t);

        WasmValType wt = ir_type_to_wasm(t);
        switch (wt) {
            case WASM_I32:
                write_u8(WASM_OP_I32_CONST);
                write_leb128_s(static_cast<int32_t>(imm));
                break;
            case WASM_I64:
                write_u8(WASM_OP_I64_CONST);
                write_leb128_s(static_cast<int64_t>(imm));
                break;
            case WASM_F32: {
                write_u8(WASM_OP_F32_CONST);
                float f;
                std::memcpy(&f, &imm, 4);
                uint32_t bits;
                std::memcpy(&bits, &f, 4);
                write_u32(bits);
                break;
            }
            case WASM_F64: {
                write_u8(WASM_OP_F64_CONST);
                double d;
                std::memcpy(&d, &imm, 8);
                uint64_t bits;
                std::memcpy(&bits, &d, 8);
                write_u32(static_cast<uint32_t>(bits));
                write_u32(static_cast<uint32_t>(bits >> 32));
                break;
            }
        }

        // Store result to local
        write_u8(WASM_OP_LOCAL_SET);
        write_leb128_u(local_for(dst));

        if (opts_.emit_comments) {
            ctx.out << "    ;; " << dst << " = const " << imm << "\n";
        }
    }

    void WasmBackend::emit_mov(EmitContext &ctx, ir::IrValueId dst,
                               ir::IrValueId src, ir::IrType t) {
        if (!current_body_.is_open) return;
        ensure_local(dst, t);

        write_u8(WASM_OP_LOCAL_GET);
        write_leb128_u(local_for(src));
        write_u8(WASM_OP_LOCAL_SET);
        write_leb128_u(local_for(dst));

        if (opts_.emit_comments) {
            ctx.out << "    ;; " << dst << " = mov " << src << "\n";
        }
    }

    void WasmBackend::emit_binop(EmitContext &ctx, ir::IrOp op, ir::IrValueId dst,
                                 ir::IrValueId lhs, ir::IrValueId rhs,
                                 ir::IrType t) {
        if (!current_body_.is_open) return;
        ensure_local(dst, t);

        // Push operands
        write_u8(WASM_OP_LOCAL_GET);
        write_leb128_u(local_for(lhs));
        write_u8(WASM_OP_LOCAL_GET);
        write_leb128_u(local_for(rhs));

        WasmValType wt = ir_type_to_wasm(t);
        bool is_float = (wt == WASM_F32 || wt == WASM_F64);
        bool is_64 = (wt == WASM_I64 || wt == WASM_F64);

        // Select the correct WASM opcode
        auto i32_op = [&](WasmOp s, WasmOp u) -> WasmOp {
            // Signed vs unsigned only applies to division, rem, comparison
            return (t == ir::IrType::I32 || t == ir::IrType::I8 || t == ir::IrType::I16) ? s : u;
        };

        WasmOp wop = WASM_OP_NOP;
        switch (op) {
            case ir::IrOp::ADD: wop = is_float ? (is_64 ? WASM_OP_F64_ADD : WASM_OP_F32_ADD) : (is_64 ? WASM_OP_I64_ADD : WASM_OP_I32_ADD); break;
            case ir::IrOp::SUB: wop = is_float ? (is_64 ? WASM_OP_F64_SUB : WASM_OP_F32_SUB) : (is_64 ? WASM_OP_I64_SUB : WASM_OP_I32_SUB); break;
            case ir::IrOp::MUL: wop = is_float ? (is_64 ? WASM_OP_F64_MUL : WASM_OP_F32_MUL) : (is_64 ? WASM_OP_I64_MUL : WASM_OP_I32_MUL); break;
            case ir::IrOp::DIV:
                if (is_float) wop = is_64 ? WASM_OP_F64_DIV : WASM_OP_F32_DIV;
                else if (t == ir::IrType::U32 || t == ir::IrType::U64 || t == ir::IrType::U8 || t == ir::IrType::U16)
                    wop = is_64 ? WASM_OP_I64_DIV_U : WASM_OP_I32_DIV_U;
                else
                    wop = is_64 ? WASM_OP_I64_DIV_S : WASM_OP_I32_DIV_S;
                break;
            case ir::IrOp::FADD: wop = is_64 ? WASM_OP_F64_ADD : WASM_OP_F32_ADD; break;
            case ir::IrOp::FSUB: wop = is_64 ? WASM_OP_F64_SUB : WASM_OP_F32_SUB; break;
            case ir::IrOp::FMUL: wop = is_64 ? WASM_OP_F64_MUL : WASM_OP_F32_MUL; break;
            case ir::IrOp::FDIV: wop = is_64 ? WASM_OP_F64_DIV : WASM_OP_F32_DIV; break;
            case ir::IrOp::AND: wop = is_64 ? WASM_OP_I64_AND : WASM_OP_I32_AND; break;
            case ir::IrOp::OR:  wop = is_64 ? WASM_OP_I64_OR  : WASM_OP_I32_OR; break;
            case ir::IrOp::XOR: wop = is_64 ? WASM_OP_I64_XOR : WASM_OP_I32_XOR; break;
            case ir::IrOp::SHL: wop = is_64 ? WASM_OP_I64_SHL : WASM_OP_I32_SHL; break;
            case ir::IrOp::SHR:
                if (is_64) wop = WASM_OP_I64_SHR_U;
                else wop = WASM_OP_I32_SHR_U;
                break;
            case ir::IrOp::SAR:
                if (is_64) wop = WASM_OP_I64_SHR_S;
                else wop = WASM_OP_I32_SHR_S;
                break;
            case ir::IrOp::FMIN: wop = is_64 ? WASM_OP_F64_MIN : WASM_OP_F32_MIN; break;
            case ir::IrOp::FMAX: wop = is_64 ? WASM_OP_F64_MAX : WASM_OP_F32_MAX; break;
            default: wop = WASM_OP_NOP; break;
        }

        if (wop != WASM_OP_NOP) {
            write_u8(static_cast<uint8_t>(wop));
        } else {
            write_u8(WASM_OP_UNREACHABLE);
        }

        write_u8(WASM_OP_LOCAL_SET);
        write_leb128_u(local_for(dst));

        if (opts_.emit_comments) {
            ctx.out << "    ;; " << dst << " = binop(" << static_cast<int>(op) << ")\n";
        }
    }

    void WasmBackend::emit_unop(EmitContext &ctx, ir::IrOp op, ir::IrValueId dst,
                                ir::IrValueId src, ir::IrType t) {
        if (!current_body_.is_open) return;
        ensure_local(dst, t);

        write_u8(WASM_OP_LOCAL_GET);
        write_leb128_u(local_for(src));

        WasmValType wt = ir_type_to_wasm(t);
        bool is_float = (wt == WASM_F32 || wt == WASM_F64);
        bool is_64 = (wt == WASM_I64 || wt == WASM_F64);

        switch (op) {
            case ir::IrOp::NEG:
                // WASM has no i32.neg; use 0 - x
                if (is_float) {
                    write_u8(static_cast<uint8_t>(is_64 ? WASM_OP_F64_NEG : WASM_OP_F32_NEG));
                } else {
                    // x = 0 - x
                    write_u8(static_cast<uint8_t>(is_64 ? WASM_OP_I64_CONST : WASM_OP_I32_CONST));
                    write_leb128_s(0);
                    write_u8(WASM_OP_LOCAL_GET);
                    write_leb128_u(local_for(src));
                    write_u8(static_cast<uint8_t>(is_64 ? WASM_OP_I64_SUB : WASM_OP_I32_SUB));
                }
                break;
            case ir::IrOp::NOT:
                write_u8(static_cast<uint8_t>(is_64 ? WASM_OP_I64_CONST : WASM_OP_I32_CONST));
                write_leb128_s(-1);
                write_u8(static_cast<uint8_t>(is_64 ? WASM_OP_I64_XOR : WASM_OP_I32_XOR));
                break;
            case ir::IrOp::FNEG:
                write_u8(static_cast<uint8_t>(is_64 ? WASM_OP_F64_NEG : WASM_OP_F32_NEG));
                break;
            case ir::IrOp::FABS:
                write_u8(static_cast<uint8_t>(is_64 ? WASM_OP_F64_ABS : WASM_OP_F32_ABS));
                break;
            case ir::IrOp::FSQRT:
                write_u8(static_cast<uint8_t>(is_64 ? WASM_OP_F64_SQRT : WASM_OP_F32_SQRT));
                break;
            default:
                write_u8(WASM_OP_UNREACHABLE);
                break;
        }

        write_u8(WASM_OP_LOCAL_SET);
        write_leb128_u(local_for(dst));

        if (opts_.emit_comments) {
            ctx.out << "    ;; " << dst << " = unop(" << static_cast<int>(op) << ")\n";
        }
    }

    void WasmBackend::emit_cmp(EmitContext &ctx, ir::IrOp op, ir::IrValueId dst,
                               ir::IrValueId lhs, ir::IrValueId rhs,
                               ir::IrType operand_type) {
        if (!current_body_.is_open) return;
        ensure_local(dst, ir::IrType::BOOL);

        write_u8(WASM_OP_LOCAL_GET);
        write_leb128_u(local_for(lhs));
        write_u8(WASM_OP_LOCAL_GET);
        write_leb128_u(local_for(rhs));

        WasmValType wt = ir_type_to_wasm(operand_type);
        bool is_float = (wt == WASM_F32 || wt == WASM_F64);
        bool is_64 = (wt == WASM_I64 || wt == WASM_F64);
        bool is_unsigned = (operand_type == ir::IrType::U8 ||
                           operand_type == ir::IrType::U16 ||
                           operand_type == ir::IrType::U32 ||
                           operand_type == ir::IrType::U64);

        auto cmp_op = [&](WasmOp eq, WasmOp ne, WasmOp lt_s, WasmOp lt_u,
                          WasmOp gt_s, WasmOp gt_u, WasmOp le_s, WasmOp le_u,
                          WasmOp ge_s, WasmOp ge_u) -> WasmOp {
            switch (op) {
                case ir::IrOp::CMP_EQ: case ir::IrOp::FCMP_EQ: return eq;
                case ir::IrOp::CMP_NE: case ir::IrOp::FCMP_NE: return ne;
                case ir::IrOp::CMP_LT: case ir::IrOp::FCMP_LT: return lt_s;
                case ir::IrOp::CMP_GT: case ir::IrOp::FCMP_GT: return gt_s;
                case ir::IrOp::CMP_LE: case ir::IrOp::FCMP_LE: return le_s;
                case ir::IrOp::CMP_GE: case ir::IrOp::FCMP_GE: return ge_s;
                case ir::IrOp::CMP_ULT: return lt_u;
                case ir::IrOp::CMP_UGT: return gt_u;
                case ir::IrOp::CMP_ULE: return le_u;
                case ir::IrOp::CMP_UGE: return ge_u;
                default: return eq;
            }
        };

        WasmOp wop;
        if (is_float) {
            if (is_64) {
                wop = cmp_op(WASM_OP_F64_EQ, WASM_OP_F64_NE, WASM_OP_F64_LT,
                             WASM_OP_F64_LT, WASM_OP_F64_GT, WASM_OP_F64_GT,
                             WASM_OP_F64_LE, WASM_OP_F64_LE, WASM_OP_F64_GE, WASM_OP_F64_GE);
            } else {
                wop = cmp_op(WASM_OP_F32_EQ, WASM_OP_F32_NE, WASM_OP_F32_LT,
                             WASM_OP_F32_LT, WASM_OP_F32_GT, WASM_OP_F32_GT,
                             WASM_OP_F32_LE, WASM_OP_F32_LE, WASM_OP_F32_GE, WASM_OP_F32_GE);
            }
        } else if (is_64) {
            wop = cmp_op(WASM_OP_I64_EQ, WASM_OP_I64_NE, WASM_OP_I64_LT_S,
                         WASM_OP_I64_LT_U, WASM_OP_I64_GT_S, WASM_OP_I64_GT_U,
                         WASM_OP_I64_LE_S, WASM_OP_I64_LE_U, WASM_OP_I64_GE_S, WASM_OP_I64_GE_U);
        } else {
            wop = cmp_op(WASM_OP_I32_EQ, WASM_OP_I32_NE, WASM_OP_I32_LT_S,
                         WASM_OP_I32_LT_U, WASM_OP_I32_GT_S, WASM_OP_I32_GT_U,
                         WASM_OP_I32_LE_S, WASM_OP_I32_LE_U, WASM_OP_I32_GE_S, WASM_OP_I32_GE_U);
        }

        write_u8(static_cast<uint8_t>(wop));

        write_u8(WASM_OP_LOCAL_SET);
        write_leb128_u(local_for(dst));

        if (opts_.emit_comments) {
            ctx.out << "    ;; " << dst << " = cmp(" << static_cast<int>(op) << ")\n";
        }
    }

    void WasmBackend::emit_convert(EmitContext &ctx, ir::IrOp op, ir::IrValueId dst,
                                   ir::IrValueId src, ir::IrType dst_type,
                                   ir::IrType src_type) {
        if (!current_body_.is_open) return;
        ensure_local(dst, dst_type);

        write_u8(WASM_OP_LOCAL_GET);
        write_leb128_u(local_for(src));

        WasmValType swt = ir_type_to_wasm(src_type);
        WasmValType dwt = ir_type_to_wasm(dst_type);
        bool src_float = (swt == WASM_F32 || swt == WASM_F64);
        bool dst_float = (dwt == WASM_F32 || dwt == WASM_F64);
        bool src_unsigned = (src_type == ir::IrType::U8 || src_type == ir::IrType::U16 ||
                            src_type == ir::IrType::U32 || src_type == ir::IrType::U64);

        switch (op) {
            case ir::IrOp::ZEXT:
            case ir::IrOp::CAST:
                if (swt == WASM_I32 && dwt == WASM_I64) {
                    if (src_unsigned) write_u8(WASM_OP_I64_EXTEND_I32_U);
                    else write_u8(WASM_OP_I64_EXTEND_I32_S);
                }
                // Same size: no-op
                break;

            case ir::IrOp::SEXT:
                if (swt == WASM_I32 && dwt == WASM_I64) {
                    write_u8(WASM_OP_I64_EXTEND_I32_S);
                }
                break;

            case ir::IrOp::TRUNC:
                if (swt == WASM_I64 && dwt == WASM_I32) {
                    write_u8(WASM_OP_I32_WRAP_I64);
                }
                break;

            case ir::IrOp::ITOF:
                if (dwt == WASM_F64 && swt == WASM_I32) {
                    if (src_unsigned) write_u8(WASM_OP_F64_CONVERT_I32_U);
                    else write_u8(WASM_OP_F64_CONVERT_I32_S);
                } else if (dwt == WASM_F64 && swt == WASM_I64) {
                    if (src_unsigned) write_u8(WASM_OP_F64_CONVERT_I64_U);
                    else write_u8(WASM_OP_F64_CONVERT_I64_S);
                } else if (dwt == WASM_F32 && swt == WASM_I32) {
                    if (src_unsigned) write_u8(WASM_OP_F32_CONVERT_I32_U);
                    else write_u8(WASM_OP_F32_CONVERT_I32_S);
                } else if (dwt == WASM_F32 && swt == WASM_I64) {
                    if (src_unsigned) write_u8(WASM_OP_F32_CONVERT_I64_U);
                    else write_u8(WASM_OP_F32_CONVERT_I64_S);
                }
                break;

            case ir::IrOp::UITOF:
                if (dwt == WASM_F64 && swt == WASM_I32) write_u8(WASM_OP_F64_CONVERT_I32_U);
                else if (dwt == WASM_F64 && swt == WASM_I64) write_u8(WASM_OP_F64_CONVERT_I64_U);
                else if (dwt == WASM_F32 && swt == WASM_I32) write_u8(WASM_OP_F32_CONVERT_I32_U);
                else if (dwt == WASM_F32 && swt == WASM_I64) write_u8(WASM_OP_F32_CONVERT_I64_U);
                break;

            case ir::IrOp::FTOI:
                if (swt == WASM_F64 && dwt == WASM_I32) {
                    if (dst_type == ir::IrType::U32) write_u8(WASM_OP_I32_TRUNC_F64_U);
                    else write_u8(WASM_OP_I32_TRUNC_F64_S);
                } else if (swt == WASM_F64 && dwt == WASM_I64) {
                    if (dst_type == ir::IrType::U64) write_u8(WASM_OP_I64_TRUNC_F64_U);
                    else write_u8(WASM_OP_I64_TRUNC_F64_S);
                } else if (swt == WASM_F32 && dwt == WASM_I32) {
                    if (dst_type == ir::IrType::U32) write_u8(WASM_OP_I32_TRUNC_F32_U);
                    else write_u8(WASM_OP_I32_TRUNC_F32_S);
                } else if (swt == WASM_F32 && dwt == WASM_I64) {
                    if (dst_type == ir::IrType::U64) write_u8(WASM_OP_I64_TRUNC_F32_U);
                    else write_u8(WASM_OP_I64_TRUNC_F32_S);
                }
                break;

            case ir::IrOp::FTOUI:
                if (swt == WASM_F64 && dwt == WASM_I32) write_u8(WASM_OP_I32_TRUNC_F64_U);
                else if (swt == WASM_F64 && dwt == WASM_I64) write_u8(WASM_OP_I64_TRUNC_F64_U);
                else if (swt == WASM_F32 && dwt == WASM_I32) write_u8(WASM_OP_I32_TRUNC_F32_U);
                else if (swt == WASM_F32 && dwt == WASM_I64) write_u8(WASM_OP_I64_TRUNC_F32_U);
                break;

            case ir::IrOp::F32TOF64:
                write_u8(WASM_OP_F64_PROMOTE_F32);
                break;

            case ir::IrOp::F64TOF32:
                write_u8(WASM_OP_F32_DEMOTE_F64);
                break;

            case ir::IrOp::BITCAST:
                if (swt == WASM_I32 && dwt == WASM_F32)
                    write_u8(WASM_OP_F32_REINTERPRET_I32);
                else if (swt == WASM_F32 && dwt == WASM_I32)
                    write_u8(WASM_OP_I32_REINTERPRET_F32);
                else if (swt == WASM_I64 && dwt == WASM_F64)
                    write_u8(WASM_OP_F64_REINTERPRET_I64);
                else if (swt == WASM_F64 && dwt == WASM_I64)
                    write_u8(WASM_OP_I64_REINTERPRET_F64);
                break;

            default:
                write_u8(WASM_OP_NOP);
                break;
        }

        write_u8(WASM_OP_LOCAL_SET);
        write_leb128_u(local_for(dst));

        if (opts_.emit_comments) {
            ctx.out << "    ;; " << dst << " = convert(" << static_cast<int>(op) << ")\n";
        }
    }

    // =========================================================================
    //  Memoria
    // =========================================================================

    void WasmBackend::emit_alloca(EmitContext &ctx, ir::IrValueId dst,
                                  uint64_t size_bytes) {
        if (!current_body_.is_open) return;
        ensure_local(dst, ir::IrType::PTR);

        // Allocate stack space by growing the WASM local stack
        // In WASM, alloca is emulated by tracking a stack pointer in memory.
        // Simple approach: use current memory size and grow as needed.
        // For a minimal implementation, we just store the allocated size
        // and use a bump allocator in linear memory.
        write_u8(WASM_OP_MEMORY_SIZE);
        write_u8(0); // reserved byte
        write_u8(WASM_OP_I32_CONST);
        write_leb128_s(static_cast<int32_t>(size_bytes));
        write_u8(WASM_OP_I32_ADD);
        write_u8(WASM_OP_LOCAL_SET);
        write_leb128_u(local_for(dst));

        if (opts_.emit_comments) {
            ctx.out << "    ;; " << dst << " = alloca " << size_bytes << "\n";
        }
    }

    void WasmBackend::emit_load(EmitContext &ctx, ir::IrValueId dst,
                                ir::IrValueId addr, ir::IrType t,
                                bool is_host_ptr) {
        if (!current_body_.is_open) return;
        ensure_local(dst, t);

        write_u8(WASM_OP_LOCAL_GET);
        write_leb128_u(local_for(addr));

        WasmOp lop = load_op_for(t);
        write_u8(static_cast<uint8_t>(lop));

        // Alignment and offset
        uint32_t align = 0;
        switch (t) {
            case ir::IrType::I8:
            case ir::IrType::U8:  align = 0; break;
            case ir::IrType::I16:
            case ir::IrType::U16: align = 1; break;
            case ir::IrType::I32:
            case ir::IrType::U32:
            case ir::IrType::F32:  align = 2; break;
            case ir::IrType::I64:
            case ir::IrType::U64:
            case ir::IrType::F64:  align = 3; break;
            default:               align = 2; break;
        }
        write_leb128_u(align);
        write_leb128_u(0); // offset

        write_u8(WASM_OP_LOCAL_SET);
        write_leb128_u(local_for(dst));

        if (opts_.emit_comments) {
            ctx.out << "    ;; " << dst << " = load " << ir::ir_type_name(t) << "\n";
        }
    }

    void WasmBackend::emit_store(EmitContext &ctx, ir::IrValueId val,
                                 ir::IrValueId addr, ir::IrType t,
                                 bool is_host_ptr) {
        if (!current_body_.is_open) return;

        write_u8(WASM_OP_LOCAL_GET);
        write_leb128_u(local_for(addr));
        write_u8(WASM_OP_LOCAL_GET);
        write_leb128_u(local_for(val));

        WasmOp sop = store_op_for(t);
        write_u8(static_cast<uint8_t>(sop));

        uint32_t align = 0;
        switch (t) {
            case ir::IrType::I8:
            case ir::IrType::U8:  align = 0; break;
            case ir::IrType::I16:
            case ir::IrType::U16: align = 1; break;
            case ir::IrType::I32:
            case ir::IrType::U32:
            case ir::IrType::F32:  align = 2; break;
            case ir::IrType::I64:
            case ir::IrType::U64:
            case ir::IrType::F64:  align = 3; break;
            default:               align = 2; break;
        }
        write_leb128_u(align);
        write_leb128_u(0); // offset

        if (opts_.emit_comments) {
            ctx.out << "    ;; store " << ir::ir_type_name(t) << "\n";
        }
    }

    void WasmBackend::emit_raw_alloc(EmitContext &ctx, ir::IrValueId dst,
                                     ir::IrValueId size) {
        if (!current_body_.is_open) return;
        ensure_local(dst, ir::IrType::PTR);

        // Emulate malloc via memory.grow
        write_u8(WASM_OP_LOCAL_GET);
        write_leb128_u(local_for(size));
        write_u8(WASM_OP_MEMORY_GROW);
        write_u8(0);
        write_u8(WASM_OP_LOCAL_SET);
        write_leb128_u(local_for(dst));

        if (opts_.emit_comments) {
            ctx.out << "    ;; " << dst << " = raw_alloc\n";
        }
    }

    void WasmBackend::emit_raw_free(EmitContext &ctx, ir::IrValueId ptr) {
        if (!current_body_.is_open) return;
        // WASM has no free; just a no-op
        write_u8(WASM_OP_NOP);

        if (opts_.emit_comments) {
            ctx.out << "    ;; raw_free (no-op in WASM)\n";
        }
    }

    // =========================================================================
    //  Llamadas
    // =========================================================================

    void WasmBackend::emit_call(EmitContext &ctx, ir::IrValueId dst,
                                const std::string &func_name,
                                const std::vector<ir::IrValueId> &args,
                                ir::IrType ret_type) {
        if (!current_body_.is_open) return;
        if (ret_type != ir::IrType::VOID) {
            ensure_local(dst, ret_type);
        }

        // Push arguments
        for (auto arg : args) {
            write_u8(WASM_OP_LOCAL_GET);
            write_leb128_u(local_for(arg));
        }

        // Find function index
        uint32_t fn_idx = 0;
        for (size_t i = 0; i < func_names_.size(); ++i) {
            if (func_names_[i] == func_name) {
                fn_idx = static_cast<uint32_t>(i);
                break;
            }
        }

        write_u8(WASM_OP_CALL);
        write_leb128_u(fn_idx);

        // Store result
        if (ret_type != ir::IrType::VOID) {
            write_u8(WASM_OP_LOCAL_SET);
            write_leb128_u(local_for(dst));
        }

        if (opts_.emit_comments) {
            ctx.out << "    ;; call " << func_name << "\n";
        }
    }

    void WasmBackend::emit_call_indirect(EmitContext &ctx, ir::IrValueId dst,
                                         ir::IrValueId fn_ptr,
                                         const std::vector<ir::IrValueId> &args,
                                         ir::IrType ret_type) {
        if (!current_body_.is_open) return;
        if (ret_type != ir::IrType::VOID) {
            ensure_local(dst, ret_type);
        }

        // Push arguments
        for (auto arg : args) {
            write_u8(WASM_OP_LOCAL_GET);
            write_leb128_u(local_for(arg));
        }

        // Push function pointer
        write_u8(WASM_OP_LOCAL_GET);
        write_leb128_u(local_for(fn_ptr));

        write_u8(WASM_OP_CALL_INDIRECT);
        write_leb128_u(0); // type index (use first function type)
        write_u8(0);       // reserved

        // Store result
        if (ret_type != ir::IrType::VOID) {
            write_u8(WASM_OP_LOCAL_SET);
            write_leb128_u(local_for(dst));
        }

        if (opts_.emit_comments) {
            ctx.out << "    ;; call_indirect\n";
        }
    }

    // =========================================================================
    //  Phi copies
    // =========================================================================

    void WasmBackend::emit_phi_copy(EmitContext &ctx, ir::IrValueId dst,
                                    ir::IrValueId src, ir::IrType t) {
        if (!current_body_.is_open) return;
        ensure_local(dst, t);

        write_u8(WASM_OP_LOCAL_GET);
        write_leb128_u(local_for(src));
        write_u8(WASM_OP_LOCAL_SET);
        write_leb128_u(local_for(dst));

        if (opts_.emit_comments) {
            ctx.out << "    ;; phi " << dst << " = " << src << "\n";
        }
    }

} // namespace port
