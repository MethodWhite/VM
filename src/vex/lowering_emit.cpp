#include "vex/lowering.h"

#include "vex/collection_intrinsics.h"

#include "ir/ir_optimizer.h"



#include <functional>

#include <set>

#include <sstream>

#include <utility>



namespace vex {

// Forward decls from other TUs
uint64_t intern_class_name(ir::IrModule &mod, const std::string &name);
uint64_t intern_class_cache_slot(ir::IrModule &mod, const std::string &name);
ir::IrValueId emit_field_addr(ir::IrFunction *fn, ir::IrBlockId block,
                               ir::IrValueId base, uint64_t offset,
                               uint32_t source_line);

// Forward decl: defined in lowering_builtin.cpp
uint64_t intern_class_name(ir::IrModule &mod, const std::string &name);

uint64_t intern_class_cache_slot(ir::IrModule &mod, const std::string &name);

    ir::IrValueId Lowering::emit_const(ir::IrType t, uint64_t imm, uint32_t source_line) {
        const ir::IrValueId dst    = fn_->new_value(t);
        fn_->values[dst].is_const  = true;
        fn_->values[dst].const_val = imm;
        ir::IrInstr c{};
        c.op          = ir::IrOp::CONST;
        c.type        = t;
        c.dst         = dst;
        c.imm         = imm;
        c.source_line = source_line;
        fn_->append(current_block_, std::move(c));
        return dst;
    }

    // -----------------------------------------------------------------------
    // Helpers para IR ops nativos (anteriormente RAW_ASM).  Migracion 2026-05-23.
    // -----------------------------------------------------------------------

    ir::IrValueId Lowering::emit_strmake(ir::IrValueId v_buf,
                                         ir::IrValueId v_len,
                                         uint32_t source_line) {
        // STRMAKE retorna el GcHandle uint32 zero-extended a i64.  El
        // handle es indice estable en la HandleTable (no se mueve con GC),
        // asi que NO se marca is_gc_object (esa flag indica "host_ptr a
        // payload" que SI se mueve y necesita gcderef en reloads).
        const ir::IrValueId v_str = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ins{};
        ins.op           = ir::IrOp::STRMAKE;
        ins.type         = ir::IrType::I64;
        ins.dst          = v_str;
        ins.operands     = {v_buf, v_len};
        ins.set_is_call_site(true);
        ins.source_line  = source_line;
        fn_->append(current_block_, std::move(ins));
        return v_str;
    }

    ir::IrValueId Lowering::emit_strcat(ir::IrValueId v_a,
                                        ir::IrValueId v_b,
                                        uint32_t source_line) {
        const ir::IrValueId v_str = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ins{};
        ins.op           = ir::IrOp::STRCAT;
        ins.type         = ir::IrType::I64;
        ins.dst          = v_str;
        ins.operands     = {v_a, v_b};
        ins.set_is_call_site(true);
        ins.source_line  = source_line;
        fn_->append(current_block_, std::move(ins));
        return v_str;
    }

    ir::IrValueId Lowering::emit_strraw(ir::IrValueId v_str, uint32_t source_line) {
        // STRRAW devuelve host_ptr al buffer data[] del StringObject.
        // Es PTR-typed con is_host_ptr=true para que LOAD/STORE posteriores
        // emitan movh (memoria host) en vez de mov (memoria VM).
        const ir::IrValueId v_ptr = fn_->new_value(ir::IrType::PTR);
        fn_->values[v_ptr].is_host_ptr = true;
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::STRRAW;
        ins.type        = ir::IrType::PTR;
        ins.dst         = v_ptr;
        ins.operands    = {v_str};
        ins.source_line = source_line;
        fn_->append(current_block_, std::move(ins));
        return v_ptr;
    }

    ir::IrValueId Lowering::emit_strconv(ir::IrValueId v_str,
                                         uint64_t      enc_imm,
                                         uint32_t source_line) {
        // STRCONV retorna GcHandle del nuevo StringObject re-encoded.
        const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ins{};
        ins.op           = ir::IrOp::STRCONV;
        ins.type         = ir::IrType::I64;
        ins.dst          = v_dst;
        ins.operands     = {v_str};
        ins.imm          = enc_imm;
        ins.set_is_call_site(true);
        ins.source_line  = source_line;
        fn_->append(current_block_, std::move(ins));
        return v_dst;
    }

    ir::IrValueId Lowering::emit_strgetbytes(ir::IrValueId v_str,
                                             uint32_t source_line) {
        const ir::IrValueId v_n = fn_->new_value(ir::IrType::U64);
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::STRGETBYTES;
        ins.type        = ir::IrType::U64;
        ins.dst         = v_n;
        ins.operands    = {v_str};
        ins.source_line = source_line;
        fn_->append(current_block_, std::move(ins));
        return v_n;
    }

    ir::IrValueId Lowering::emit_gc_handle_for_ptr(ir::IrValueId v_host_ptr,
                                                   uint32_t source_line) {
        // GC_HANDLE_FOR_PTR devuelve un GcHandle uint32 zero-extended a i64.
        // No es is_host_ptr (es un indice opaco), no es is_gc_object (no es
        // el host_ptr al payload).
        const ir::IrValueId v_h = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::GC_HANDLE_FOR_PTR;
        ins.type        = ir::IrType::I64;
        ins.dst         = v_h;
        ins.operands    = {v_host_ptr};
        ins.source_line = source_line;
        fn_->append(current_block_, std::move(ins));
        return v_h;
    }

    void Lowering::emit_mvtake(ir::IrValueId v_dst_addr,
                               ir::IrValueId v_src_addr,
                               uint32_t source_line) {
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::MVTAKE_IR;
        ins.type        = ir::IrType::VOID;
        ins.dst         = ir::IR_NO_VALUE;
        ins.operands    = {v_dst_addr, v_src_addr};
        ins.source_line = source_line;
        fn_->append(current_block_, std::move(ins));
    }

    // ---- Sprint 2: Phase Z + reflexion + static + AOP ----

    ir::IrValueId Lowering::emit_findmethod(ir::IrValueId v_params, uint32_t line) {
        const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        fn_->values[v].is_host_ptr = true; // MethodInfo* host
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::FINDMETHOD;
        ins.type        = ir::IrType::PTR;
        ins.dst         = v;
        ins.operands    = {v_params};
        ins.set_is_call_site(true);
        ins.source_line = line;
        fn_->append(current_block_, std::move(ins));
        return v;
    }

    ir::IrValueId Lowering::emit_findfield(ir::IrValueId v_params, uint32_t line) {
        const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        fn_->values[v].is_host_ptr = true;
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::FINDFIELD;
        ins.type        = ir::IrType::PTR;
        ins.dst         = v;
        ins.operands    = {v_params};
        ins.set_is_call_site(true);
        ins.source_line = line;
        fn_->append(current_block_, std::move(ins));
        return v;
    }

    ir::IrValueId Lowering::emit_gc_allocp(ir::IrValueId v_size, uint32_t line) {
        const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        fn_->values[v].is_host_ptr = true;
        ir::IrInstr ins{};
        ins.op           = ir::IrOp::GC_ALLOCP;
        ins.type         = ir::IrType::PTR;
        ins.dst          = v;
        ins.operands     = {v_size};
        ins.set_is_call_site(true);
        ins.source_line  = line;
        fn_->append(current_block_, std::move(ins));
        return v;
    }

    ir::IrValueId Lowering::emit_gc_promote(ir::IrValueId v_src, uint32_t line) {
        const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        fn_->values[v].is_host_ptr = true;
        ir::IrInstr ins{};
        ins.op           = ir::IrOp::GC_PROMOTE;
        ins.type         = ir::IrType::PTR;
        ins.dst          = v;
        ins.operands     = {v_src};
        ins.set_is_call_site(true);
        ins.source_line  = line;
        fn_->append(current_block_, std::move(ins));
        return v;
    }

    ir::IrValueId Lowering::emit_gc_demote(ir::IrValueId v_src, uint32_t line) {
        const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        fn_->values[v].is_host_ptr = true;
        ir::IrInstr ins{};
        ins.op           = ir::IrOp::GC_DEMOTE;
        ins.type         = ir::IrType::PTR;
        ins.dst          = v;
        ins.operands     = {v_src};
        ins.set_is_call_site(true);
        ins.source_line  = line;
        fn_->append(current_block_, std::move(ins));
        return v;
    }

    ir::IrValueId Lowering::emit_atomic_ld_i64(ir::IrValueId v_addr, uint32_t line) {
        const ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::ATOMIC_LD_I64;
        ins.type        = ir::IrType::I64;
        ins.dst         = v;
        ins.operands    = {v_addr};
        ins.source_line = line;
        fn_->append(current_block_, std::move(ins));
        return v;
    }

    void Lowering::emit_atomic_st_i64(ir::IrValueId v_addr, ir::IrValueId v_val,
                                      uint32_t line) {
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::ATOMIC_ST_I64;
        ins.type        = ir::IrType::VOID;
        ins.dst         = ir::IR_NO_VALUE;
        ins.operands    = {v_addr, v_val};
        ins.source_line = line;
        fn_->append(current_block_, std::move(ins));
    }

    ir::IrValueId Lowering::emit_atomic_cas_i64(ir::IrValueId v_addr,
                                                ir::IrValueId v_exp,
                                                ir::IrValueId v_des,
                                                uint32_t line) {
        const ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::ATOMIC_CAS_I64;
        ins.type        = ir::IrType::I64;
        ins.dst         = v;
        ins.operands    = {v_addr, v_exp, v_des};
        ins.source_line = line;
        fn_->append(current_block_, std::move(ins));
        return v;
    }

    ir::IrValueId Lowering::emit_atomic_add_i64(ir::IrValueId v_addr,
                                                ir::IrValueId v_delta,
                                                uint32_t line) {
        const ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::ATOMIC_ADD_I64;
        ins.type        = ir::IrType::I64;
        ins.dst         = v;
        ins.operands    = {v_addr, v_delta};
        ins.source_line = line;
        fn_->append(current_block_, std::move(ins));
        return v;
    }

    ir::IrValueId Lowering::emit_getstatic(ir::IrValueId v_cls, uint64_t offset,
                                           uint32_t line) {
        const ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::GETSTATIC;
        ins.type        = ir::IrType::I64;
        ins.dst         = v;
        ins.operands    = {v_cls};
        ins.imm         = offset;
        ins.source_line = line;
        fn_->append(current_block_, std::move(ins));
        return v;
    }

    void Lowering::emit_setstatic(ir::IrValueId v_cls, ir::IrValueId v_val,
                                  uint64_t offset, uint32_t line) {
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::SETSTATIC;
        ins.type        = ir::IrType::VOID;
        ins.dst         = ir::IR_NO_VALUE;
        ins.operands    = {v_cls, v_val};
        ins.imm         = offset;
        ins.source_line = line;
        fn_->append(current_block_, std::move(ins));
    }

    ir::IrValueId Lowering::emit_proceed(uint32_t line) {
        const ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ins{};
        ins.op           = ir::IrOp::PROCEED;
        ins.type         = ir::IrType::I64;
        ins.dst          = v;
        ins.set_is_call_site(true);
        ins.source_line  = line;
        fn_->append(current_block_, std::move(ins));
        return v;
    }

    // ---- Sprint 3: label-addr + CLI args + async helper fusion ----

    ir::IrValueId Lowering::emit_label_addr(const std::string &label_name,
                                            uint32_t line) {
        const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::LABEL_ADDR;
        ins.type        = ir::IrType::PTR;
        ins.dst         = v;
        ins.func_name   = label_name;  // se interpreta como @Absolute("code.<name>")
        ins.source_line = line;
        fn_->append(current_block_, std::move(ins));
        return v;
    }

    ir::IrValueId Lowering::emit_getpid(uint32_t line) {
        const ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::GETPID;
        ins.type        = ir::IrType::I64;
        ins.dst         = v;
        ins.source_line = line;
        fn_->append(current_block_, std::move(ins));
        return v;
    }

    ir::IrValueId Lowering::emit_getargc(uint32_t line) {
        const ir::IrValueId v = fn_->new_value(ir::IrType::I64);
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::GETARGC;
        ins.type        = ir::IrType::I64;
        ins.dst         = v;
        ins.source_line = line;
        fn_->append(current_block_, std::move(ins));
        return v;
    }

    ir::IrValueId Lowering::emit_getarg(ir::IrValueId v_idx, uint32_t line) {
        const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        fn_->values[v].is_host_ptr = true; // host_ptr al string del arg
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::GETARG;
        ins.type        = ir::IrType::PTR;
        ins.dst         = v;
        ins.operands    = {v_idx};
        ins.source_line = line;
        fn_->append(current_block_, std::move(ins));
        return v;
    }

    void Lowering::emit_fulfill_hlt(ir::IrValueId v_fut, ir::IrValueId v_val,
                                    uint32_t line) {
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::FULFILL_HLT;
        ins.type        = ir::IrType::VOID;
        ins.dst         = ir::IR_NO_VALUE;
        ins.operands    = {v_fut, v_val};
        ins.source_line = line;
        fn_->append(current_block_, std::move(ins));
    }

    // ---- Sprint 4: meta-OOP ----

    ir::IrValueId Lowering::emit_findclass(ir::IrValueId v_params, uint32_t line) {
        const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        fn_->values[v].is_host_ptr = true;
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::FINDCLASS;
        ins.type        = ir::IrType::PTR;
        ins.dst         = v;
        ins.operands    = {v_params};
        ins.set_is_call_site(true);
        ins.source_line = line;
        fn_->append(current_block_, std::move(ins));
        return v;
    }

    ir::IrValueId Lowering::emit_defclass(ir::IrValueId v_params, uint32_t line) {
        const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        fn_->values[v].is_host_ptr = true;
        ir::IrInstr ins{};
        ins.op           = ir::IrOp::DEFCLASS;
        ins.type         = ir::IrType::PTR;
        ins.dst          = v;
        ins.operands     = {v_params};
        ins.set_is_call_site(true);
        ins.source_line  = line;
        fn_->append(current_block_, std::move(ins));
        return v;
    }

    void Lowering::emit_deffield(ir::IrValueId v_cls, ir::IrValueId v_params,
                                  uint32_t line) {
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::DEFFIELD;
        ins.type        = ir::IrType::VOID;
        ins.dst         = ir::IR_NO_VALUE;
        ins.operands    = {v_cls, v_params};
        ins.source_line = line;
        fn_->append(current_block_, std::move(ins));
    }

    void Lowering::emit_defmethod(ir::IrValueId v_cls, ir::IrValueId v_params,
                                   uint32_t line) {
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::DEFMETHOD;
        ins.type        = ir::IrType::VOID;
        ins.dst         = ir::IR_NO_VALUE;
        ins.operands    = {v_cls, v_params};
        ins.source_line = line;
        fn_->append(current_block_, std::move(ins));
    }

    void Lowering::emit_addadvice(ir::IrValueId v_target, ir::IrValueId v_advice,
                                   uint64_t kind, uint32_t line) {
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::ADDADVICE;
        ins.type        = ir::IrType::VOID;
        ins.dst         = ir::IR_NO_VALUE;
        ins.operands    = {v_target, v_advice};
        ins.imm         = kind;
        ins.source_line = line;
        fn_->append(current_block_, std::move(ins));
    }

    ir::IrValueId Lowering::emit_findclass_by_name(uint64_t name_idx,
                                                    uint32_t name_len,
                                                    uint32_t line) {
        // 1. ALLOCA 16 bytes para FindClassParams.
        ir::IrValueId v_params = fn_->new_value(ir::IrType::PTR);
        {
            ir::IrInstr al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8;
            al.dst         = v_params;
            al.imm         = 16;
            al.source_line = line;
            fn_->append(current_block_, std::move(al));
        }
        // 2. LABEL_ADDR @Absolute("code.s_<idx>") -> name_addr.
        ir::IrValueId v_name_addr = emit_label_addr(
            "s_" + std::to_string(name_idx), line);
        // 3. STORE name_addr at [v_params + 0].
        {
            ir::IrInstr st{};
            st.op          = ir::IrOp::STORE;
            st.type        = ir::IrType::I64;
            st.operands    = {v_name_addr, v_params};
            st.source_line = line;
            fn_->append(current_block_, std::move(st));
        }
        // 4. STORE name_len at [v_params + 8].
        ir::IrValueId v_name_len = emit_const(ir::IrType::I64,
                                              static_cast<uint64_t>(name_len), line);
        ir::IrValueId v_off8     = emit_const(ir::IrType::I64, 8, line);
        ir::IrValueId v_params8  = fn_->new_value(ir::IrType::PTR);
        {
            ir::IrInstr add{};
            add.op          = ir::IrOp::ADD;
            add.type        = ir::IrType::I64;
            add.dst         = v_params8;
            add.operands    = {v_params, v_off8};
            add.source_line = line;
            fn_->append(current_block_, std::move(add));
        }
        {
            ir::IrInstr st{};
            st.op          = ir::IrOp::STORE;
            st.type        = ir::IrType::I64;
            st.operands    = {v_name_len, v_params8};
            st.source_line = line;
            fn_->append(current_block_, std::move(st));
        }
        // 5. FINDCLASS -> ClassInfo*.
        return emit_findclass(v_params, line);
    }

    ir::IrValueId Lowering::emit_getproc(uint32_t source_line) {
        const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
        ir::IrInstr         ip{};
        ip.op          = ir::IrOp::GETPROC;
        ip.type        = ir::IrType::PTR;
        ip.dst         = v;
        ip.source_line = source_line;
        fn_->append(current_block_, std::move(ip));
        return v;
    }

    /**
     * @brief Phase MC.17.2 -- obtiene (o aloca) el slot de @c static_data
     * para un comptime global.
     *
     * Lookup en @c comptime_global_slots_; si no esta, lee el valor
     * inicial desde @c tc_.comptime_const_values_, emite un slot de 8
     * bytes con esos bits y registra el mapping name -> idx.
     *
     * Solo soporta valores int (i64/u64/bool) en v1.  Strings/structs
     * requeririan inicializacion en @c __module_init via STRMAKE/etc.,
     * lo que es un sprint adicional.
     *
     * @return Indice valido (`s_<idx>` referenciable via STR_LIT_ADDR),
     *         o @c UINT64_MAX si el global no es soportado en v1.
     */
    // L2.2: slot para global runtime no-const.  Zero-init en static_data;
    // @c __module_init emite las instrucciones que copian el init real (e.g.
    // un STRMAKE para strings, una constante para ints) al slot.
    //
    // CRITICO: usar push_back directo (no intern_static_data) para evitar
    // dedup -- cada global necesita SU PROPIO slot aunque comparta bytes
    // iniciales (typicamente todos los globals empiezan en {0,0,...,0} y
    // si los dedupeamos colisionan en el mismo storage).
    uint64_t Lowering::get_or_create_runtime_global_slot(const std::string &name) {
        auto it = runtime_global_slots_.find(name);
        if (it != runtime_global_slots_.end()) return it->second;
        std::vector<uint8_t> bytes(8, 0);  // 8 bytes zero-init
        const uint64_t idx = static_cast<uint64_t>(
            out_mod_->static_data.push_back(std::move(bytes)));
        // Marcar el slot como NON_DEDUP para que el merge cross-module
        // no colapse multiples globals con bytes iniciales identicos.
        out_mod_->static_data.meta_at(idx).flags |=
            ir::IrModule::SD_FLAG_NON_DEDUP;
        runtime_global_slots_[name] = idx;
        return idx;
    }

    uint64_t Lowering::get_or_create_comptime_global_slot(const std::string &name) {
        auto it = comptime_global_slots_.find(name);
        if (it != comptime_global_slots_.end()) return it->second;
        const auto &cgv = tc_.comptime_const_values();
        auto cit = cgv.find(name);
        if (cit == cgv.end()) return UINT64_MAX;
        /* Solo int en v1 -- strings serializados requieren STRMAKE en
         * __module_init que no esta integrado todavia. */
        if (cit->second.is_str) return UINT64_MAX;
        /* Empaquetar el valor inicial como 8 bytes little-endian. */
        const uint64_t init_val = static_cast<uint64_t>(cit->second.value);
        std::vector<uint8_t> bytes(8);
        for (int i = 0; i < 8; ++i) {
            bytes[i] = static_cast<uint8_t>((init_val >> (i * 8)) & 0xFFu);
        }
        const uint64_t idx = out_mod_->intern_static_data(std::move(bytes));
        comptime_global_slots_[name] = idx;
        return idx;
    }

    ir::IrValueId Lowering::stringify_primitive_via_native(
            ir::IrValueId v_val,
            const char   *native_fn,
            uint32_t      source_line)
    {
        const int ln = static_cast<int>(source_line);
        /* 1. ALLOCA 32 bytes -- buffer en stack VM.  Suficiente para
         *    todos los tipos: i64=20+signo, hex=18, "false"=5, UTF-8 4 B. */
        ir::IrValueId v_buf = fn_->new_value(ir::IrType::PTR); {
            ir::IrInstr al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8;
            al.dst         = v_buf;
            al.imm         = 32;
            al.source_line = ln;
            fn_->append(current_block_, std::move(al));
        }
        /* 2. proc_ptr via getproc. */
        const ir::IrValueId v_proc = emit_getproc(ln);
        /* 3. CALLN al native: devuelve length escrita en buf. */
        out_mod_->register_native_import(
            std::string("stdlib/native/io/vesta_io"), native_fn);
        ir::IrValueId v_len = fn_->new_value(ir::IrType::I64); {
            ir::IrInstr cl{};
            cl.op          = ir::IrOp::CALLN;
            cl.type        = ir::IrType::I64;
            cl.dst         = v_len;
            cl.func_name   = std::string("stdlib/native/io/vesta_io:")
                          + native_fn;
            cl.operands    = {v_proc, v_buf, v_val};
            cl.source_line = ln;
            fn_->append(current_block_, std::move(cl));
        }
        /* 4. STRMAKE desde buf vm_mem. */
        ir::IrValueId v_h = emit_strmake(v_buf, v_len, ln);
        return v_h;
    }

    ir::IrValueId Lowering::cast_if_needed(ir::IrValueId v,
                                           ir::IrType    from, ir::IrType to,
                                           uint32_t      source_line,
                                           bool          is_explicit) {
        if (from == to || v == ir::IR_NO_VALUE) return v;
        // Warning de seguridad para casts implicitos que pueden perder
        // informacion: narrowing entero, float -> int, int -> float (los
        // grandes pierden mantissa).  Solo se emite cuando el usuario NO
        // escribio el cast explicitamente: `i32 x = i64_val` avisa, pero
        // `i32 x = (i32) i64_val` no.  Misma politica que -Wconversion en
        // GCC/Clang.
        if (!is_explicit) {
            auto bytes_for = [](ir::IrType t) -> int {
                switch (t) {
                    case ir::IrType::I8:  case ir::IrType::U8:
                    case ir::IrType::BOOL:                       return 1;
                    case ir::IrType::I16: case ir::IrType::U16:  return 2;
                    case ir::IrType::I32: case ir::IrType::U32:
                    case ir::IrType::F32:                        return 4;
                    default:                                      return 8;
                }
            };
            const bool from_is_float = (from == ir::IrType::F32 || from == ir::IrType::F64);
            const bool to_is_float   = (to   == ir::IrType::F32 || to   == ir::IrType::F64);
            const bool from_is_int = (from == ir::IrType::I8  || from == ir::IrType::I16
                                   || from == ir::IrType::I32 || from == ir::IrType::I64
                                   || from == ir::IrType::U8  || from == ir::IrType::U16
                                   || from == ir::IrType::U32 || from == ir::IrType::U64
                                   || from == ir::IrType::BOOL);
            const bool to_is_int   = (to   == ir::IrType::I8  || to   == ir::IrType::I16
                                   || to   == ir::IrType::I32 || to   == ir::IrType::I64
                                   || to   == ir::IrType::U8  || to   == ir::IrType::U16
                                   || to   == ir::IrType::U32 || to   == ir::IrType::U64
                                   || to   == ir::IrType::BOOL);
            const int from_bytes = bytes_for(from);
            const int to_bytes   = bytes_for(to);
            std::string warn_msg;
            if (from_is_float && to_is_int) {
                warn_msg = "conversion implicita float -> int trunca la parte fraccionaria; "
                           "usa cast explicito si es intencional";
            } else if (from_is_int && to_is_float) {
                // Solo avisar para enteros grandes a f32 (perdida de mantissa).
                // i64/u64 -> f32: ~24 bits de mantissa, perdida garantizada para magnitudes >2^24.
                // i32/u32 -> f32: tambien puede perder.  i*->f64 es exacto hasta 2^53.
                if (to == ir::IrType::F32 && from_bytes >= 4) {
                    warn_msg = "conversion implicita int -> f32 puede perder precision; "
                               "usa cast explicito si es intencional";
                }
            } else if (from_is_int && to_is_int) {
                if (to_bytes < from_bytes) {
                    warn_msg = "conversion implicita reduce el ancho del entero "
                               "(narrowing); usa cast explicito si es intencional";
                }
            } else if (from_is_float && to_is_float) {
                if (to == ir::IrType::F32 && from == ir::IrType::F64) {
                    warn_msg = "conversion implicita f64 -> f32 reduce precision; "
                               "usa cast explicito si es intencional";
                }
            }
            if (!warn_msg.empty()) {
                SourceLoc loc;
                loc.file   = current_file_;
                loc.line   = source_line;
                loc.column = 1;
                diags_.warning(loc, warn_msg);
            }
        }

        // Elegir el opcode de conversion correcto segun categoria.
        ir::IrOp   op         = ir::IrOp::CAST;
        const bool from_float = (from == ir::IrType::F32 || from == ir::IrType::F64);
        const bool to_float   = (to == ir::IrType::F32 || to == ir::IrType::F64);

        if (from_float && to_float) {
            op = (from == ir::IrType::F32 && to == ir::IrType::F64)
                     ? ir::IrOp::F32TOF64
                     : ir::IrOp::F64TOF32;
        } else if (from_float && !to_float) {
            // Heuristica: si el destino es signed -> FTOI; si unsigned -> FTOUI.
            const bool to_signed = (to == ir::IrType::I8 || to == ir::IrType::I16
                || to == ir::IrType::I32 || to == ir::IrType::I64);
            op = to_signed ? ir::IrOp::FTOI : ir::IrOp::FTOUI;
        } else if (!from_float && to_float) {
            // Heuristica simetrica: si origen signed -> ITOF; si unsigned -> UITOF.
            const bool from_signed = (from == ir::IrType::I8 || from == ir::IrType::I16
                || from == ir::IrType::I32 || from == ir::IrType::I64);
            // Bug fix 2026-05-23: ITOF/UITOF baja a `fcvt rd_gp, f0` que
            // opera sobre el reg de 64 bits.  Si el operando es i8/i16/i32,
            // los bits altos no estan extendidos correctamente -> el float
            // resultante es incorrecto.  Para i32 -7 -> trunc deja
            // 0xFFFFFFF9 con bits altos = 0 -> ITOF lo lee como 4294967289
            // (no -7).  Fix: SEXT (signed) o ZEXT (unsigned) a i64 ANTES
            // del ITOF/UITOF.
            auto bytes_of_local = [](ir::IrType t) -> int {
                switch (t) {
                    case ir::IrType::I8:  case ir::IrType::U8:
                    case ir::IrType::BOOL:                       return 1;
                    case ir::IrType::I16: case ir::IrType::U16:  return 2;
                    case ir::IrType::I32: case ir::IrType::U32:  return 4;
                    default:                                      return 8;
                }
            };
            if (bytes_of_local(from) < 8) {
                ir::IrValueId v_ext = fn_->new_value(ir::IrType::I64);
                ir::IrInstr ext{};
                ext.op = from_signed ? ir::IrOp::SEXT : ir::IrOp::ZEXT;
                ext.type = ir::IrType::I64;
                ext.dst = v_ext;
                ext.operands.push_back(v);
                ext.source_line = source_line;
                fn_->append(current_block_, std::move(ext));
                v = v_ext;
            }
            op = from_signed ? ir::IrOp::ITOF : ir::IrOp::UITOF;
        } else {
            // Entero -> entero: elegir TRUNC, ZEXT o SEXT segun el cambio
            // de ancho y la signedness de la fuente.  Sin esto, el
            // emitter recibia siempre CAST y emitia un mov plano que NO
            // truncaba ni extendia: `i32 x = i64_value` dejaba los 8
            // bytes originales en el registro (bug de truncacion).
            auto bytes_of = [](ir::IrType t) -> int {
                switch (t) {
                    case ir::IrType::I8:  case ir::IrType::U8:
                    case ir::IrType::BOOL:                       return 1;
                    case ir::IrType::I16: case ir::IrType::U16:  return 2;
                    case ir::IrType::I32: case ir::IrType::U32:  return 4;
                    default:                                      return 8;
                }
            };
            const int from_b = bytes_of(from);
            const int to_b   = bytes_of(to);
            const bool from_signed = (from == ir::IrType::I8
                                   || from == ir::IrType::I16
                                   || from == ir::IrType::I32
                                   || from == ir::IrType::I64);
            if (to_b < from_b) {
                op = ir::IrOp::TRUNC;
            } else if (to_b > from_b) {
                op = from_signed ? ir::IrOp::SEXT : ir::IrOp::ZEXT;
            } else {
                // Mismo ancho: nada que extender ni truncar; un BITCAST
                // (mov plano) es lo correcto a nivel de bytecode.  Esto
                // cubre cambios de signedness sin reinterpretacion (e.g.
                // i32 -> u32) y casts entre PTR e i64.
                op = ir::IrOp::BITCAST;
            }
        }

        const ir::IrValueId dst = fn_->new_value(to);
        ir::IrInstr         ins{};
        ins.op          = op;
        ins.type        = to;
        ins.dst         = dst;
        ins.operands    = {v};
        ins.source_line = source_line;
        fn_->append(current_block_, std::move(ins));
        return dst;
    }

    // ---------------------------------------------------------------------
    // Scopes.
    // ---------------------------------------------------------------------

    void Lowering::push_scope() {
        scopes_.emplace_back();
    }

    void Lowering::pop_scope() {
        scopes_.pop_back();
    }

    void Lowering::bind(const std::string &name, ir::IrValueId v) {
        scopes_.back()[name] = v;
    }

    ir::IrValueId Lowering::lookup(const std::string &name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return found->second;
        }
        return ir::IR_NO_VALUE;
    }

    // Wrapper publico para que helpers estaticos (e.g.
    // @c collect_spawn_captures_in_expr) puedan resolver un nombre en
    // todos los scopes activos del lowering sin tener acceso directo a
    // @c lookup (que es @c const private).  Delega a @c lookup.
    ir::IrValueId Lowering::spawn_capture_resolve(const std::string &name) {
        return lookup(name);
    }

    void Lowering::update_scope(const std::string &name, ir::IrValueId v) {
        // Buscar de mas interno a global y actualizar in-place.  Esto evita
        // crear sombras accidentales (que pasaria si simplemente hicieramos
        // bind() sobre el scope actual en lugar del scope donde la variable
        // se declaro).
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) {
                found->second = v;
                return;
            }
        }
        // Fallback: si no existe en ningun scope, registrar en el actual.
        // El type checker normalmente atrapa esto antes, pero defendemos
        // para no perder informacion en caso de un AST malformado.
        bind(name, v);
    }

    // ---------------------------------------------------------------------
    // Address-taken locals: pre-pase + accesos via LOAD/STORE.
    //
    // Cuando el usuario escribe `&x`, x debe vivir en una direccion estable
    // (memoria), no en un registro virtual SSA.  Hacemos un escaneo previo
    // del cuerpo de cada funcion para detectar todas las variables cuya
    // direccion se toma; lower_var_decl emite ALLOCA para esas, y los
    // accesos lectura/escritura pasan por LOAD/STORE en lugar de scope SSA.
    //
    // El conjunto address_taken_locals_ es por-funcion (limpiado al inicio
    // de lower_function via run() / lower_function).  Solo registramos los
    // nombres; el control de scope inner-shadowing es resposabilidad del
    // type checker en hitos posteriores (no usamos shadowing).
    // ---------------------------------------------------------------------

    void Lowering::scan_address_taken(ast::Stmt *s) {
        if (!s) return;
        // Recorrido recursivo de stmts y exprs.  Definimos lambdas locales
        // para mantener las dependencias contenidas.
        std::function<void(ast::Expr *)> visit_expr;
        std::function<void(ast::Stmt *)> visit_stmt;

        visit_expr = [&](ast::Expr *e) {
            if (!e) return;
            switch (e->kind) {
                case ast::NodeKind::UnaryExpr: {
                    auto *u = static_cast<ast::UnaryExpr *>(e);
                    if (u->op == ast::UnOp::AddrOf
                        && u->operand
                        && u->operand->kind == ast::NodeKind::IdentExpr) {
                        auto *id = static_cast<ast::IdentExpr *>(u->operand.get());
                        address_taken_locals_.insert(id->name);
                    }
                    visit_expr(u->operand.get());
                    return;
                }
                case ast::NodeKind::LambdaExpr: {
                    // Captures mutables: las variables modificadas dentro
                    // del cuerpo de la lambda deben ser address-taken en
                    // el outer scope para que el modelo de captura-por-
                    // referencia funcione.  El env block guarda el PUNTERO
                    // al slot del owner; el helper de la lambda hace
                    // LOAD/STORE indirectos sobre ese puntero, de modo
                    // que las mutaciones se ven desde fuera del lambda.
                    auto *lam = static_cast<ast::LambdaExpr *>(e);
                    for (const auto &nm: lam->mutable_captures) {
                        address_taken_locals_.insert(nm);
                    }
                    if (lam->body) visit_stmt(lam->body.get());
                    return;
                }
                case ast::NodeKind::BinaryExpr: {
                    auto *b = static_cast<ast::BinaryExpr *>(e);
                    visit_expr(b->lhs.get());
                    visit_expr(b->rhs.get());
                    return;
                }
                case ast::NodeKind::AssignExpr: {
                    auto *a = static_cast<ast::AssignExpr *>(e);
                    visit_expr(a->target.get());
                    visit_expr(a->value.get());
                    return;
                }
                case ast::NodeKind::CallExpr: {
                    auto *c = static_cast<ast::CallExpr *>(e);
                    // Borrow checker: lend(x) / lend_mut(x) sobre una
                    // variable local plain requiere tomar su direccion
                    // (un borrow ES, en runtime, un host_ptr al slot
                    // donde vive el local; cero overhead vs un T*).
                    // Forzamos address-taken promotion para que el lowering
                    // deje el local en stack via ALLOCA + LOAD/STORE en
                    // lugar de en registro SSA puro.  Sin esto, lend(local)
                    // devuelve un valor (no una direccion) y read_borrow/
                    // write_borrow dereferencian basura.  EXCEPCION: si la
                    // var ya es de tipo borrow<T>/borrow_mut<T> (es un
                    // borrow_var, no un local plain), NO la promocionamos
                    // (su SSA value ya es host_ptr; el lend lo bypassa).
                    if (c->callee
                     && c->callee->kind == ast::NodeKind::IdentExpr
                     && c->args.size() == 1
                     && c->args[0]->kind == ast::NodeKind::IdentExpr) {
                        auto *cid = static_cast<ast::IdentExpr *>(c->callee.get());
                        if (cid->name == "lend" || cid->name == "lend_mut") {
                            auto *aid = static_cast<ast::IdentExpr *>(c->args[0].get());
                            const Type at = aid->result_type;
                            if (at.kind != PrimitiveKind::BORROW
                             && at.kind != PrimitiveKind::BORROW_MUT
                             && at.kind != PrimitiveKind::UNIQUE_PTR
                             && at.kind != PrimitiveKind::SHARED_PTR) {
                                address_taken_locals_.insert(aid->name);
                            }
                        }
                    }
                    visit_expr(c->callee.get());
                    for (auto &arg: c->args) visit_expr(arg.get());
                    return;
                }
                case ast::NodeKind::FieldAccessExpr: {
                    auto *fa = static_cast<ast::FieldAccessExpr *>(e);
                    visit_expr(fa->base.get());
                    return;
                }
                case ast::NodeKind::IndexExpr: {
                    auto *ix = static_cast<ast::IndexExpr *>(e);
                    visit_expr(ix->base.get());
                    visit_expr(ix->index.get());
                    return;
                }
                default:
                    return; // literales, IdentExpr puro, etc. no aportan
            }
        };

        visit_stmt = [&](ast::Stmt *st) {
            if (!st) return;
            switch (st->kind) {
                case ast::NodeKind::BlockStmt: {
                    auto *b = static_cast<ast::BlockStmt *>(st);
                    for (auto &child: b->body) visit_stmt(child.get());
                    return;
                }
                case ast::NodeKind::VarDeclStmt: {
                    auto *vd = static_cast<ast::VarDeclStmt *>(st);
                    if (vd->init) visit_expr(vd->init.get());
                    return;
                }
                case ast::NodeKind::ExprStmt: {
                    auto *es = static_cast<ast::ExprStmt *>(st);
                    visit_expr(es->expr.get());
                    return;
                }
                case ast::NodeKind::IfStmt: {
                    auto *si = static_cast<ast::IfStmt *>(st);
                    visit_expr(si->cond.get());
                    visit_stmt(si->then_branch.get());
                    visit_stmt(si->else_branch.get());
                    return;
                }
                case ast::NodeKind::WhileStmt: {
                    auto *w = static_cast<ast::WhileStmt *>(st);
                    visit_expr(w->cond.get());
                    visit_stmt(w->body.get());
                    return;
                }
                case ast::NodeKind::DoWhileStmt: {
                    auto *dw = static_cast<ast::DoWhileStmt *>(st);
                    visit_stmt(dw->body.get());
                    visit_expr(dw->cond.get());
                    return;
                }
                case ast::NodeKind::ForStmt: {
                    auto *f = static_cast<ast::ForStmt *>(st);
                    visit_stmt(f->init.get());
                    visit_expr(f->cond.get());
                    visit_expr(f->step.get());
                    visit_stmt(f->body.get());
                    return;
                }
                case ast::NodeKind::TryStmt: {
                    // Sin esta rama, las variables declaradas dentro de un
                    // try/catch/finally no se promocionan a address-taken
                    // aunque aparezca `&var` en el body (error: '&x' sobre
                    // variable no promocionada).  Y el cascade de errores
                    // "nombre no resuelto" surge porque el lowering del
                    // var-decl falla al evaluar `&var` y deja el binding
                    // sin registrar.
                    auto *ts = static_cast<ast::TryStmt *>(st);
                    visit_stmt(ts->body.get());
                    for (auto &cc: ts->catches) visit_stmt(cc.body.get());
                    if (ts->finally_body) visit_stmt(ts->finally_body.get());
                    return;
                }
                case ast::NodeKind::ReturnStmt: {
                    auto *r = static_cast<ast::ReturnStmt *>(st);
                    visit_expr(r->value.get());
                    return;
                }
                case ast::NodeKind::SynchronizedStmt: {
                    auto *sy = static_cast<ast::SynchronizedStmt *>(st);
                    visit_expr(sy->target.get());
                    visit_stmt(sy->body.get());
                    return;
                }
                default:
                    return;
            }
        };
        visit_stmt(s);
    }

    // ---------------------------------------------------------------------
    // scan_escaping_locals: pre-pase que recorre el body buscando
    // patrones donde el handle de un local escapa del scope:
    //
    //   - return ident;            -> ident escapa via valor de retorno.
    //   - this.field   = ident;    -> ident escapa via campo de objeto.
    //   - obj.field    = ident;    -> idem.
    //   - *ptr         = ident;    -> escapa via deref-store.
    //   - arr[i]       = ident;    -> escapa via slot de array.
    //   - p->field     = ident;    -> escapa via field deref.
    //
    // Los locales detectados se anyaden a @c escaping_locals_; el cleanup
    // automatico los omite y queda como responsabilidad del caller (o del
    // futuro GC roots) liberar el handle.
    //
    // Conservador: solo detecta escape via los patrones listados.  Pasar el
    // local como argumento a una funcion NO se considera escape (el callee
    // tipicamente solo lee el handle; si retiene una copia es responsabilidad
    // suya marcar el escape via su propio analisis).
    // ---------------------------------------------------------------------
    void Lowering::scan_escaping_locals(ast::Stmt *body) {
        if (!body) return;
        std::function<void(ast::Expr *)> visit_expr;
        std::function<void(ast::Stmt *)> visit_stmt;

        // Grafo de aliasing local-to-local: alias_graph[A] = {B, C, ...}
        // significa "A puede contener un valor que vino de B, C, ..." (a
        // traves de asignaciones `A = B;`).  Tras la primera pasada
        // propagamos el escape hacia atras: si A es escaping, todos los
        // que feed-en a A tambien escapan.
        std::unordered_map<std::string, std::vector<std::string>> alias_graph;

        // Helper: si @p e es IdentExpr, marca el nombre como escaping.
        auto mark_if_ident = [&](ast::Expr *e) {
            if (e && e->kind == ast::NodeKind::IdentExpr) {
                auto *id = static_cast<ast::IdentExpr *>(e);
                escaping_locals_.insert(id->name);
            }
        };

        visit_expr = [&](ast::Expr *e) {
            if (!e) return;
            switch (e->kind) {
                case ast::NodeKind::AssignExpr: {
                    auto *a = static_cast<ast::AssignExpr *>(e);
                    // El target NO escapa por la asignacion misma.  El value
                    // SI escapa cuando el target es un campo/slot/deref:
                    //   - FieldAccessExpr: this.x = value, obj.x = value
                    //   - IndexExpr:       arr[i] = value, p[i] = value
                    //   - UnaryExpr Deref: *p = value
                    if (a->target) {
                        switch (a->target->kind) {
                            case ast::NodeKind::FieldAccessExpr:
                            case ast::NodeKind::IndexExpr:
                                mark_if_ident(a->value.get());
                                break;
                            case ast::NodeKind::UnaryExpr: {
                                auto *u = static_cast<ast::UnaryExpr *>(a->target.get());
                                if (u->op == ast::UnOp::Deref) {
                                    mark_if_ident(a->value.get());
                                }
                                break;
                            }
                            case ast::NodeKind::IdentExpr: {
                                // Asignacion local-a-local: `target = source`.
                                // No marcamos escape ahora; registramos en el
                                // grafo de alias para propagacion transitiva.
                                // Si `target` resulta escaping al final, `source`
                                // tambien lo sera.
                                auto *id_t = static_cast<ast::IdentExpr *>(a->target.get());
                                if (a->value && a->value->kind == ast::NodeKind::IdentExpr) {
                                    auto *id_v = static_cast<ast::IdentExpr *>(a->value.get());
                                    alias_graph[id_t->name].push_back(id_v->name);
                                }
                                break;
                            }
                            default: break;
                        }
                    }
                    visit_expr(a->target.get());
                    visit_expr(a->value.get());
                    return;
                }
                case ast::NodeKind::BinaryExpr: {
                    auto *b = static_cast<ast::BinaryExpr *>(e);
                    visit_expr(b->lhs.get());
                    visit_expr(b->rhs.get());
                    return;
                }
                case ast::NodeKind::UnaryExpr: {
                    auto *u = static_cast<ast::UnaryExpr *>(e);
                    visit_expr(u->operand.get());
                    return;
                }
                case ast::NodeKind::CallExpr: {
                    auto *c = static_cast<ast::CallExpr *>(e);
                    visit_expr(c->callee.get());
                    for (auto &arg: c->args) visit_expr(arg.get());
                    return;
                }
                case ast::NodeKind::FieldAccessExpr: {
                    auto *fa = static_cast<ast::FieldAccessExpr *>(e);
                    visit_expr(fa->base.get());
                    return;
                }
                case ast::NodeKind::IndexExpr: {
                    auto *ix = static_cast<ast::IndexExpr *>(e);
                    visit_expr(ix->base.get());
                    visit_expr(ix->index.get());
                    return;
                }
                default: return;
            }
        };

        visit_stmt = [&](ast::Stmt *st) {
            if (!st) return;
            switch (st->kind) {
                case ast::NodeKind::BlockStmt: {
                    auto *b = static_cast<ast::BlockStmt *>(st);
                    for (auto &child: b->body) visit_stmt(child.get());
                    return;
                }
                case ast::NodeKind::VarDeclStmt: {
                    auto *vd = static_cast<ast::VarDeclStmt *>(st);
                    // `T target = source;` propaga alias para tracking transitivo.
                    if (vd->init && vd->init->kind == ast::NodeKind::IdentExpr) {
                        auto *id_v = static_cast<ast::IdentExpr *>(vd->init.get());
                        alias_graph[vd->name].push_back(id_v->name);
                    }
                    if (vd->init) visit_expr(vd->init.get());
                    return;
                }
                case ast::NodeKind::ExprStmt: {
                    auto *es = static_cast<ast::ExprStmt *>(st);
                    visit_expr(es->expr.get());
                    return;
                }
                case ast::NodeKind::IfStmt: {
                    auto *si = static_cast<ast::IfStmt *>(st);
                    visit_expr(si->cond.get());
                    visit_stmt(si->then_branch.get());
                    visit_stmt(si->else_branch.get());
                    return;
                }
                case ast::NodeKind::WhileStmt: {
                    auto *w = static_cast<ast::WhileStmt *>(st);
                    visit_expr(w->cond.get());
                    visit_stmt(w->body.get());
                    return;
                }
                case ast::NodeKind::DoWhileStmt: {
                    auto *dw = static_cast<ast::DoWhileStmt *>(st);
                    visit_stmt(dw->body.get());
                    visit_expr(dw->cond.get());
                    return;
                }
                case ast::NodeKind::ForStmt: {
                    auto *f = static_cast<ast::ForStmt *>(st);
                    visit_stmt(f->init.get());
                    visit_expr(f->cond.get());
                    visit_expr(f->step.get());
                    visit_stmt(f->body.get());
                    return;
                }
                case ast::NodeKind::TryStmt: {
                    // Recursar tambien en try para detectar escapes de
                    // locales dentro de body, catches y finally.
                    auto *ts = static_cast<ast::TryStmt *>(st);
                    visit_stmt(ts->body.get());
                    for (auto &cc: ts->catches) visit_stmt(cc.body.get());
                    if (ts->finally_body) visit_stmt(ts->finally_body.get());
                    return;
                }
                case ast::NodeKind::SynchronizedStmt: {
                    auto *sy = static_cast<ast::SynchronizedStmt *>(st);
                    visit_expr(sy->target.get());
                    visit_stmt(sy->body.get());
                    return;
                }
                case ast::NodeKind::ReturnStmt: {
                    auto *r = static_cast<ast::ReturnStmt *>(st);
                    // return ident; -> ident escapa.
                    mark_if_ident(r->value.get());
                    visit_expr(r->value.get());
                    return;
                }
                default: return;
            }
        };
        visit_stmt(body);

        // ----- Propagacion transitiva del escape via alias_graph -----
        // Si `target = source` y target ya esta marcado como escaping, source
        // tambien debe estarlo (aliasing semantico).  Iteramos hasta punto fijo.
        // Coste: O(N*M) donde N=#locales escaping, M=longitud cadena alias.
        // En la practica las cadenas son cortas (1-3 hops); converge rapido.
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto &kv : alias_graph) {
                const std::string &target = kv.first;
                if (escaping_locals_.count(target) == 0) continue;
                for (const std::string &source : kv.second) {
                    if (escaping_locals_.insert(source).second) {
                        changed = true;
                    }
                }
            }
        }
    }

    ir::IrValueId Lowering::read_local(const std::string &name, ir::IrType ir_ty,
                                       uint32_t           source_line) {
        const ir::IrValueId v = lookup(name);
        if (v == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        if (!address_taken_locals_.count(name)) return v;
        // Address-taken: el scope guarda la direccion de un ALLOCA;
        // emitimos un LOAD para obtener el valor actual.
        const ir::IrValueId dst = fn_->new_value(ir_ty);
        ir::IrInstr         ins{};
        ins.op          = ir::IrOp::LOAD;
        ins.type        = ir_ty;
        ins.dst         = dst;
        ins.operands    = {v};
        ins.source_line = source_line;
        fn_->append(current_block_, std::move(ins));
        // Limitacion (cerrada): si el local fue marcado como host-bearing
        // (al menos un write_local le grabo un valor con is_host_ptr=true),
        // el LOAD reconstruye el bit en el SSA value resultante.  Sin esto
        // el round-trip `T* p = malloc(); ...; LOAD &p` perderia el bit y
        // el siguiente LOAD/STORE indirecto emitiria mov en vez de movh.
        if (host_bearing_locals_.count(name)) {
            fn_->values[dst].is_host_ptr = true;
        }
        return dst;
    }

    void Lowering::write_local(const std::string &name, ir::IrValueId v,
                               ir::IrType         ir_ty, uint32_t     source_line) {
        if (!address_taken_locals_.count(name)) {
            update_scope(name, v);
            // Si la variable tiene slot activo en un try, ADICIONALMENTE
            // emitir STORE al slot.  El motivo: cuando ocurre un @c throw
            // dentro del body del try, el handler en el catch necesita el
            // ultimo valor de la variable, pero do_throw restaura el RSP
            // y descarta cualquier @c push del save/restore alrededor de
            // los CALLs.  Sin este STORE redundante, el catch leeria un
            // registro con un valor obsoleto o corrupto.
            auto it_slot = try_spill_slots_.find(name);
            if (it_slot != try_spill_slots_.end()) {
                // Usar el tipo real del valor para que el STORE escriba
                // exactamente N bytes y no contamine los bytes altos
                // del slot 8-byte alloca.
                ir::IrType st_ty = ir_ty;
                if (v < fn_->values.size()) {
                    st_ty = fn_->values[v].type;
                }
                ir::IrInstr st{};
                st.op          = ir::IrOp::STORE;
                st.type        = st_ty;
                st.dst         = ir::IR_NO_VALUE;
                st.operands    = {v, it_slot->second};
                st.source_line = source_line;
                fn_->append(current_block_, std::move(st));
            }
            return;
        }
        // Address-taken: emitir STORE a la direccion guardada en scope.
        const ir::IrValueId addr = lookup(name);
        if (addr == ir::IR_NO_VALUE) {
            update_scope(name, v); // fallback defensivo
            return;
        }
        ir::IrInstr st{};
        st.op          = ir::IrOp::STORE;
        st.type        = ir_ty;
        st.dst         = ir::IR_NO_VALUE;
        st.operands    = {v, addr}; // STORE: operands[0]=val, operands[1]=ptr
        st.source_line = source_line;
        fn_->append(current_block_, std::move(st));
        // Limitacion (cerrada): registrar host-bearing si el valor escrito
        // proviene de heap host (malloc o aritmetica derivada).  read_local
        // consulta este set para propagar is_host_ptr al LOAD del slot.
        // Ademas marcamos el SSA value del slot (addr) con pointee_is_host_ptr
        // para que el caso indirecto @c &p; *pp tambien propague is_host_ptr
        // al destino del LOAD via el ir_emitter.  Sticky por simplicidad: una
        // vez marcado, el local queda host-bearing aunque despues le asignen
        // un valor VM.  Aceptable porque en la practica los locales mantienen
        // su naturaleza a lo largo de su vida.
        if (v != ir::IR_NO_VALUE && fn_->values[v].is_host_ptr) {
            host_bearing_locals_.insert(name);
            fn_->values[addr].pointee_is_host_ptr = true;
        }
    }

    // ---------------------------------------------------------------------
    // Errores y helpers de diagnostico.
    // ---------------------------------------------------------------------

    void Lowering::unsupported(SourceLoc loc, const char *feature) {
        diags_.error(std::move(loc),
                     std::string("lowering: caracteristica aun no soportada: ") + feature);
    }

    void Lowering::error_at(SourceLoc loc, std::string msg) {
        diags_.error(std::move(loc), std::move(msg));
    }

    // ---------------------------------------------------------------------
    // Exportacion de metadata POO al IrModule (para port transpilers).
    // ---------------------------------------------------------------------

    void Lowering::export_classes_to_ir(ir::IrModule &out) {
        const auto &layouts = tc_.class_layouts();
        out.classes.reserve(layouts.size());
        for (const auto &kv : layouts) {
            const auto &cl = kv.second;
            // Saltar clases predefinidas en runtime (e.g. FatalError):
            // el port no debe re-emitirlas; el runtime las provee.
            if (cl.is_runtime_predefined) continue;

            ir::IrClass icls;
            icls.name           = cl.name;
            icls.super_name     = cl.super_name;
            icls.interfaces     = cl.interface_names;
            icls.size_bytes     = cl.size_bytes;
            icls.is_final       = false; /* Vex frontend lo trackea por metodo;
                                            agregado lo deducimos en transpiler
                                            via hierarchy analysis cuando es
                                            necesario.  Default false = seguro. */
            icls.is_interface   = cl.is_interface;
            icls.is_aspect      = cl.is_aspect;
            icls.has_destructor = cl.has_destructor;
            icls.has_destructible_field = cl.has_destructible_field;
            icls.is_runtime_predefined  = false;

            // Convertir fields de instancia.  Mantenemos el orden del
            // ClassLayout (heredados primero, luego propios) -- el
            // transpiler los emite tal cual en el struct C.
            icls.fields.reserve(cl.fields.size());
            for (const auto &f : cl.fields) {
                ir::IrField ifld;
                ifld.name        = f.name;
                ifld.type        = ir_type_from_primitive(f.type.kind);
                ifld.offset      = f.offset;
                ifld.size_bytes  = f.size;
                ifld.is_static   = false;
                /* Si el tipo del field es CLASS, registrar el nombre de la
                 * clase apuntada -- el transpiler lo necesita para emitir
                 * el tipo C correcto (`ClassY *` vs `void *`). */
                if (f.type.kind == PrimitiveKind::CLASS) {
                    ifld.class_type_name = f.type.struct_name;
                }
                icls.fields.push_back(std::move(ifld));
            }

            // Static fields.
            icls.static_fields.reserve(cl.static_fields.size());
            for (const auto &f : cl.static_fields) {
                ir::IrField ifld;
                ifld.name        = f.name;
                ifld.type        = ir_type_from_primitive(f.type.kind);
                ifld.offset      = f.offset;
                ifld.size_bytes  = f.size;
                ifld.is_static   = true;
                if (f.type.kind == PrimitiveKind::CLASS) {
                    ifld.class_type_name = f.type.struct_name;
                }
                icls.static_fields.push_back(std::move(ifld));
            }

            // Convertir metodos.  El @c ir_fn_name sigue el mangling de
            // @c lower_class_methods: "<Class>__ctor" para constructores,
            // "<Class>__<name>" para el resto (destructor usa name="__dtor"
            // -> ir_fn_name="<Class>____dtor" con 4 underscores).
            icls.methods.reserve(cl.methods.size());
            for (const auto &m : cl.methods) {
                ir::IrMethod imeth;
                imeth.name           = m.name;
                if (m.is_constructor) {
                    imeth.ir_fn_name = cl.name + "__ctor";
                } else {
                    // Si el metodo es heredado puro (no override), apuntar al
                    // simbolo del defining_class para evitar emitir referencia
                    // a un Class__method que no existe.  El transpiler C usa
                    // este nombre como label de funcion.
                    const std::string &defc = m.defining_class;
                    const std::string &owner = (!defc.empty() && defc != cl.name)
                                                   ? defc : cl.name;
                    imeth.ir_fn_name = owner + "__" + m.name;
                }
                imeth.return_type     = ir_type_from_primitive(m.return_type.kind);
                imeth.param_types.reserve(m.param_types.size());
                for (const auto &pt : m.param_types) {
                    imeth.param_types.push_back(ir_type_from_primitive(pt.kind));
                }
                imeth.vtable_index   = static_cast<int32_t>(m.vtable_index);
                imeth.is_static      = m.is_static;
                imeth.is_final       = m.is_final;
                imeth.is_constructor = m.is_constructor;
                imeth.is_destructor  = m.is_destructor;
                imeth.is_inline      = m.is_inline;
                imeth.defining_class = m.defining_class;
                icls.methods.push_back(std::move(imeth));
            }

            out.classes.push_back(std::move(icls));
        }
    }

    // =====================================================================
    // BugFix R1: super(args) y super.method(args)
    // =====================================================================
    //
    // super(args) (SuperCallExpr): dentro de un ctor de clase derivada,
    // invoca el ctor del super con this como receptor.  El this implicito
    // se obtiene del primer parametro (lookup("this")).
    //
    // Implementacion correcta: emitir el opcode bytecode `callsuper`
    // (0xFC) que dispatcha a la vtable de la SUPER class (no del
    // receiver dinamico).  Esto evita:
    //   (1) Recursion infinita en ctor: `callvirt this, 0` con un
    //       Derived as receiver resolveria vtable[0]=Derived.__ctor
    //       (no Base.__ctor) -> recursion.
    //   (2) Override-en-medio: si Derived overridea un metodo de Base,
    //       `super.foo()` desde Derived debe llamar Base.foo, NO
    //       Derived.foo.  CALLVIRT lo haria mal; CALLSUPER lo hace bien.
    //
    // El IR no tiene un IrOp::CALLSUPER dedicado, asi que el lowering
    // usa RAW_ASM con la sintaxis textual `callsuper r_cls, vtable_idx`
    // del assembler de la VM.  El bloque RAW_ASM emite:
    //   findclass <super_name> -> r_cls (host_ptr a ClassInfo del super)
    //   mov r1, this
    //   mov r2..rN, args
    //   mov r15, argc
    //   callsuper r_cls, vtable_idx
    //
    // super.method(args) (SuperMethodCallExpr): igual patron pero busca
    // el metodo non-ctor por nombre en la cadena super (BFS) y emite
    // callsuper con su vtable_index dentro de la SUPER class.  Si hay
    // overrides intermedios entre Derived y la clase que define el
    // metodo, el dispatch va a la clase mas cercana en la jerarquia
    // super (Java's `super.method` semantica).
    ir::IrValueId Lowering::lower_super_call_expr(ast::SuperCallExpr *e) {
        // Resolver this implicito.
        const ir::IrValueId v_this = lookup("this");
        if (v_this == ir::IR_NO_VALUE) {
            error_at(e->loc, "super(...): no se encontro 'this' en el scope");
            return ir::IR_NO_VALUE;
        }
        // Buscar el super_name del current_class_.
        if (current_class_lowering_.empty()) {
            error_at(e->loc, "super(...) fuera de cuerpo de clase");
            return ir::IR_NO_VALUE;
        }
        auto it = tc_.class_layouts().find(current_class_lowering_);
        if (it == tc_.class_layouts().end() || it->second.super_name.empty()) {
            error_at(e->loc, "super(...) en clase sin super");
            return ir::IR_NO_VALUE;
        }
        const std::string &super_name = it->second.super_name;
        auto it_s = tc_.class_layouts().find(super_name);
        if (it_s == tc_.class_layouts().end()) {
            error_at(e->loc, "super clase '" + super_name + "' desconocida");
            return ir::IR_NO_VALUE;
        }
        // Buscar el ctor PROPIO del super (no heredado de su super-super).
        // BugFix R1.fix: si el super tambien deriva de otra clase, sus
        // methods comienzan con la inherited ctor del super-super.  Sin
        // priorizar el ctor cuyo defining_class == super_name, el callsuper
        // dispatcharia a Mid.vtable[0] = Base.__ctor (inherited) en lugar
        // de Mid.__ctor (own), con la aridad de Base.__ctor en vez de Mid.
        const ClassMethodInfo *super_ctor = nullptr;
        for (const auto &m : it_s->second.methods) {
            if (m.is_constructor && m.defining_class == super_name) {
                super_ctor = &m;
                break;
            }
        }
        // Fallback: si no hay ctor propio en super, usar el primero.
        if (!super_ctor) {
            for (const auto &m : it_s->second.methods) {
                if (m.is_constructor) { super_ctor = &m; break; }
            }
        }
        if (!super_ctor) {
            error_at(e->loc,
                "super(...): la clase super '" + super_name +
                "' no tiene constructor");
            return ir::IR_NO_VALUE;
        }
        // Bajar args.
        std::vector<ir::IrValueId> arg_vals;
        arg_vals.reserve(e->args.size());
        for (auto &a : e->args) {
            const ir::IrValueId av = lower_expr(a.get());
            if (av == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            arg_vals.push_back(av);
        }
        // Resolver ClassInfo* del super via findclass inline (mismo patron
        // que forName).  Resultado en v_cls.  Luego emitir CALLSUPER IR.
        const uint64_t super_name_idx = intern_class_name(*out_mod_, super_name);
        const uint32_t super_name_len = static_cast<uint32_t>(super_name.size());
        // Sprint 5: findclass via IR ops.
        const ir::IrValueId v_cls = emit_findclass_by_name(
            super_name_idx, super_name_len, e->loc.line);
        // Emit CALLSUPER IR: layout = [cls, this, args...], imm=vtbl_idx.
        // El emisor IR coloca obj en r1, args en r2..r_{N+1}, cls en r13,
        // y emite `callsuper r13, vtable_idx`.  Sin RAW_ASM: el regalloc,
        // DCE y otros pases ven la operacion como un CALL real.
        ir::IrInstr cs{};
        cs.op           = ir::IrOp::CALLSUPER;
        cs.type         = ir::IrType::VOID;
        cs.dst          = ir::IR_NO_VALUE;
        cs.operands.push_back(v_cls);
        cs.operands.push_back(v_this);
        for (auto av : arg_vals) cs.operands.push_back(av);
        cs.imm          = static_cast<uint64_t>(super_ctor->vtable_index);
        cs.source_line  = e->loc.line;
        fn_->append(current_block_, std::move(cs));
        return ir::IR_NO_VALUE;
    }

    ir::IrValueId Lowering::lower_super_method_call_expr(
        ast::SuperMethodCallExpr *e)
    {
        const ir::IrValueId v_this = lookup("this");
        if (v_this == ir::IR_NO_VALUE) {
            error_at(e->loc,
                "super." + e->method_name + "(...): no se encontro 'this'");
            return ir::IR_NO_VALUE;
        }
        if (current_class_lowering_.empty()) {
            error_at(e->loc, "super.<metodo>(...) fuera de cuerpo de clase");
            return ir::IR_NO_VALUE;
        }
        auto it = tc_.class_layouts().find(current_class_lowering_);
        if (it == tc_.class_layouts().end() || it->second.super_name.empty()) {
            error_at(e->loc, "super.<metodo>(...) en clase sin super");
            return ir::IR_NO_VALUE;
        }
        // Buscar el metodo en la cadena super (BFS).
        std::string cur = it->second.super_name;
        const ClassMethodInfo *found = nullptr;
        for (int depth = 0; depth < 32; ++depth) {
            auto it_s = tc_.class_layouts().find(cur);
            if (it_s == tc_.class_layouts().end()) break;
            for (const auto &m : it_s->second.methods) {
                if (!m.is_constructor && m.name == e->method_name) {
                    found = &m;
                    break;
                }
            }
            if (found) break;
            if (it_s->second.super_name.empty()) break;
            cur = it_s->second.super_name;
        }
        if (!found) {
            error_at(e->loc,
                "super." + e->method_name + ": metodo no encontrado");
            return ir::IR_NO_VALUE;
        }
        std::vector<ir::IrValueId> arg_vals;
        arg_vals.reserve(e->args.size());
        for (auto &a : e->args) {
            const ir::IrValueId av = lower_expr(a.get());
            if (av == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            arg_vals.push_back(av);
        }
        const ir::IrType ret_ir = ir_type_from_primitive(found->return_type.kind);
        const ir::IrValueId dst =
            (ret_ir == ir::IrType::VOID) ? ir::IR_NO_VALUE
                                          : fn_->new_value(ret_ir);
        // Resolver ClassInfo* del super via findclass inline.
        const std::string &super_name = it->second.super_name;
        const uint64_t super_name_idx = intern_class_name(*out_mod_, super_name);
        const uint32_t super_name_len = static_cast<uint32_t>(super_name.size());
        // Sprint 5: findclass via IR ops.
        const ir::IrValueId v_cls = emit_findclass_by_name(
            super_name_idx, super_name_len, e->loc.line);
        // Emit CALLSUPER IR (mismo patron que super(args) ctor).
        ir::IrInstr cs{};
        cs.op           = ir::IrOp::CALLSUPER;
        cs.type         = ret_ir;
        cs.dst          = dst;
        cs.operands.push_back(v_cls);
        cs.operands.push_back(v_this);
        for (auto av : arg_vals) cs.operands.push_back(av);
        cs.imm          = static_cast<uint64_t>(found->vtable_index);
        cs.source_line  = e->loc.line;
        fn_->append(current_block_, std::move(cs));
        return dst;
    }
} // namespace vex
