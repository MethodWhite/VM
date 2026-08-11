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
 * @file lowering.cpp
 * @brief Implementacion del pase AST -> ir::IrModule de Vex.
 */

#include "vex/lowering.h"
#include "vex/collection_intrinsics.h"  // tabla de tipos coleccion
#include "vex/comptime_introspect.h"   // helpers compartidos rama A
#include "vex/lexer.h"                  // parse de fragments para @Macro
#include "vex/parser.h"                 // parse_one_expr para @Macro
#include "ir/ir_optimizer.h"            // register_pure_new_helper

#include <functional>
#include <set>
#include <sstream>
#include <utility>

namespace vex {

namespace {
    /**
     * @brief Tamano en bytes de un IrType escalar.
     *
     * Replica local de la funcion estatica del emisor (src/ir/ir_emitter.cpp);
     * lo necesitamos para dimensionar ALLOCA en el lowering de variables
     * address-taken.  Mantener una copia es preferible a exponer el helper
     * del emisor para no acoplar el frontend al detalle de codegen.
     */
    inline uint64_t ir_type_size(::ir::IrType t) noexcept {
        switch (t) {
            case ::ir::IrType::I8:
            case ::ir::IrType::U8:
            case ::ir::IrType::BOOL: return 1;
            case ::ir::IrType::I16:
            case ::ir::IrType::U16: return 2;
            case ::ir::IrType::I32:
            case ::ir::IrType::U32:
            case ::ir::IrType::F32:
            case ::ir::IrType::HANDLE: return 4;
            case ::ir::IrType::I64:
            case ::ir::IrType::U64:
            case ::ir::IrType::F64:
            case ::ir::IrType::PTR: return 8;
            default: return 8; // VOID u otros: defecto seguro
        }
    }
    }

    // Forward decl: definida al final de este TU (non-static, visible
    // desde lowering.cpp via extern forward decl).
    uint64_t intern_class_name(ir::IrModule &mod, const std::string &name);

    bool Lowering::try_lower_builtin_call(ast::CallExpr *e, ir::IrValueId &out_value) {
        // Solo manejamos identifier-callees (validado en lower_call).
        if (!e->callee || e->callee->kind != ast::NodeKind::IdentExpr) return false;
        const auto *       id   = static_cast<const ast::IdentExpr *>(e->callee.get());
        const std::string &name = id->name;

        // -----------------------------------------------------------------
        // Sprint 1: builtins comptime de introspection.
        // Disparan SOLO cuando hay type_args.size()>=1.  Devuelven UN
        // valor constante computado a partir del tipo resuelto:
        //   sizeof<T>()   -> u64
        //   alignof<T>()  -> u64
        //   typename<T>() -> string (StringObject)
        //   type_id<T>()  -> u32
        //   kind<T>()     -> i32 (ComptimeKind enum)
        // Cero overhead runtime: la salida es un solo IrOp::CONST (o
        // STRMAKE para strings).
        // -----------------------------------------------------------------
        // Sprint B.1: as_native_callback(fn) -> i64 (host_ptr al thunk).
        //
        // Lowering: emite CALLN a vesta_runtime:vex_get_native_thunk con:
        //   r1 = @Absolute("code.<fn_name>")  (PC virtual de la fn Vex)
        //   r2 = argc (numero de parametros que la fn Vex recibe)
        // El runtime genera (o reusa) un thunk x86-64 callable con cc C
        // nativa y devuelve el host_ptr.
        if (name == "as_native_callback" && e->args.size() == 1) {
            auto *fn_id = dynamic_cast<ast::IdentExpr *>(e->args[0].get());
            if (fn_id == nullptr) {
                error_at(e->loc, "as_native_callback: arg debe ser identificador de fn Vex");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            /* Resolver la signature de la fn Vex para conocer argc. */
            uint32_t argc = 0;
            const FunctionSig *fsig = tc_.function_sig_by_name(fn_id->name);
            if (fsig != nullptr) {
                argc = (uint32_t)fsig->param_types.size();
            }
            const uint32_t src_line = e->loc.line;
            /* v_fn_pc = LABEL_ADDR("code.<fn_name>") */
            ir::IrValueId v_fn_pc = emit_label_addr(fn_id->name, src_line);
            /* v_argc = CONST i64 */
            ir::IrValueId v_argc = emit_const(ir::IrType::I64, (uint64_t)argc, src_line);
            /* CALLN @Method("vesta_runtime:vex_get_native_thunk", v_fn_pc, v_argc). */
            out_mod_->register_native_import("vesta_runtime", "vex_get_native_thunk");
            ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr cl{};
            cl.op          = ir::IrOp::CALLN;
            cl.type        = ir::IrType::I64;
            cl.dst         = v_dst;
            cl.func_name   = "vesta_runtime:vex_get_native_thunk";
            cl.operands    = {v_fn_pc, v_argc};
            cl.source_line = src_line;
            fn_->append(current_block_, std::move(cl));
            out_value = v_dst;
            return true;
        }

        if (!e->type_args.empty()
         && (name == "sizeof" || name == "alignof"
          || name == "typename" || name == "type_id"
          || name == "kind")) {
            const Type t = tc_.resolve_type_node(e->type_args[0].get());
            const uint32_t src_line = e->loc.line;
            if (name == "sizeof") {
                const uint64_t v = comptime_type_size(tc_, t);
                out_value = emit_const(ir::IrType::U64, v, src_line);
                return true;
            }
            if (name == "alignof") {
                const uint64_t v = comptime_type_align(tc_, t);
                out_value = emit_const(ir::IrType::U64, v, src_line);
                return true;
            }
            if (name == "type_id") {
                const uint32_t v = comptime_type_id(tc_, t);
                out_value = emit_const(ir::IrType::U32, static_cast<uint64_t>(v), src_line);
                return true;
            }
            if (name == "kind") {
                const ComptimeKind k = comptime_type_kind(t);
                out_value = emit_const(ir::IrType::I32,
                    static_cast<uint64_t>(static_cast<int32_t>(k)), src_line);
                return true;
            }
            /* typename<T>() -> StringObject construido inline desde el
             * nombre canonico.  Reusa el mismo patron que el path no-
             * interpolado de @c lower_string_literal_to_string_object:
             * STR_LIT_ADDR a static_data + RAW_ASM strmake. */
            if (name == "typename") {
                const std::string nm = comptime_type_name(tc_, t);
                std::vector<uint8_t> bytes(nm.begin(), nm.end());
                const uint64_t idx = out_mod_->intern_static_data(std::move(bytes));
                ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
                {
                    ir::IrInstr ins{};
                    ins.op          = ir::IrOp::STR_LIT_ADDR;
                    ins.type        = ir::IrType::PTR;
                    ins.dst         = v_addr;
                    ins.imm         = idx;
                    ins.source_line = src_line;
                    fn_->append(current_block_, std::move(ins));
                }
                ir::IrValueId v_len = emit_const(
                    ir::IrType::I64, static_cast<uint64_t>(nm.size()), src_line);
                ir::IrValueId v_str = emit_strmake(v_addr, v_len, src_line);
                /* NO marcar is_gc_object: STRMAKE devuelve un GcHandle
                 * (uint32 zero-extended a i64).  Los handles son estables
                 * cross-GC -- la HandleTable los redirige tras evacuacion.
                 * Marcarlo como host_ptr provocaria que el regalloc emita
                 * gchandle/gcderef innecesarios y, peor, leyera la entrada
                 * de ptr_to_handle_ para un handle (no un host_ptr) -> NULL.
                 * Mismo patron que lower_string_literal_to_string_object. */
                out_value = v_str;
                return true;
            }
        }

        // -----------------------------------------------------------------
        // A.39: builtins comptime sobre strings.  Args ya validados como
        // comptime-evaluables por type_checker.  Aqui evaluamos y emitimos:
        //   comptime_concat -> STRMAKE inline con bytes concatenados
        //   comptime_streq  -> CONST bool con resultado
        //   comptime_strlen -> CONST u64 con size
        // -----------------------------------------------------------------
        if (name == "comptime_concat"
         || name == "comptime_streq"
         || name == "comptime_strlen") {
            const ComptimeEvalResult r = comptime_eval_expr(tc_, e);
            if (!r.ok) {
                /* Phase MC.24: si el comptime eval falla (e.g. arg es
                 * un comptime var que solo se resuelve en call-site
                 * del macro padre), NO erroreamos.  En su lugar, dejamos
                 * que el path runtime (str_concat/str_equals/str_length
                 * via STRCAT/STRCMP/STRLEN bytecode) maneje el call.
                 * El macro corre via VM al invocarse y produce el
                 * resultado correcto.  Solo fallback si el caller es
                 * NO un macro (en cuyo caso si hay error real). */
                /* Caer al lowering normal abajo via is_str_concat/etc. */
                /* Fall through. */
            } else {
            const uint32_t src_line = e->loc.line;
            if (r.is_str) {
                /* Materializar como StringObject inline. */
                std::vector<uint8_t> bytes(r.str.begin(), r.str.end());
                const uint64_t idx = out_mod_->intern_static_data(
                    std::move(bytes));
                ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR); {
                    ir::IrInstr is{};
                    is.op          = ir::IrOp::STR_LIT_ADDR;
                    is.type        = ir::IrType::PTR;
                    is.dst         = v_addr;
                    is.imm         = idx;
                    is.source_line = src_line;
                    fn_->append(current_block_, std::move(is));
                }
                ir::IrValueId v_len = emit_const(ir::IrType::I64,
                    (uint64_t)r.str.size(), src_line);
                ir::IrValueId v_str = emit_strmake(v_addr, v_len, src_line);
                out_value = v_str;
                return true;
            }
            /* Int result: comptime_streq -> bool, comptime_strlen -> u64. */
            ir::IrType t = (name == "comptime_streq")
                ? ir::IrType::BOOL
                : ir::IrType::U64;
            out_value = emit_const(t, (uint64_t)r.value, src_line);
            return true;
            }  /* end else block (r.ok=true path) */
        }

        // -----------------------------------------------------------------
        // A.38 - static_assert(cond, "msg") -- compile-time only.
        // El type checker ya valido la cond (emite error si es false o no
        // evaluable).  Aqui simplemente no emitimos codigo: la asercion es
        // un no-op en runtime, su efecto fue rechazar la compilacion.
        // -----------------------------------------------------------------
        if (name == "static_assert") {
            out_value = ir::IR_NO_VALUE;
            return true;
        }

        // -----------------------------------------------------------------
        // Sprint 4 (A.37.s4): builtins runtime de introspection.
        //   find_type("Lit")                   -> direct mov al chunk
        //   find_type(s)                       -> CALL al resolver runtime (TODO)
        //   type_info_kind/size/align/field_count -> LOAD u32 con offset fijo
        //   type_info_name(p) / _field_name(p, i) -> construye StringObject
        //   type_info_field_offset / _field_size  -> LOAD u32 con stride 16
        // -----------------------------------------------------------------
        {
            const bool is_find = (name == "find_type");
            const bool is_simple_u32 =
                name == "type_info_size"
             || name == "type_info_align"
             || name == "type_info_field_count";
            const bool is_kind_i32 = (name == "type_info_kind");
            const bool is_name_q   = (name == "type_info_name");
            const bool is_field_name = (name == "type_info_field_name");
            const bool is_field_u32 =
                name == "type_info_field_offset"
             || name == "type_info_field_size";
            if (is_find || is_simple_u32 || is_kind_i32 || is_name_q
             || is_field_name || is_field_u32) {
                const uint32_t src_line = e->loc.line;
                if (is_find) {
                    /* Resolver literal -> chunk idx en compile-time.
                     * Caso runtime string deferido a Sprint 5. */
                    auto *slit = e->args.empty()
                        ? nullptr
                        : dynamic_cast<ast::StringLitExpr *>(e->args[0].get());
                    if (slit && !slit->is_interpolated()) {
                        auto it = introspect_idx_by_name_.find(slit->value);
                        if (it == introspect_idx_by_name_.end()) {
                            /* Tipo no registrado con @Introspect -> 0. */
                            out_value = emit_const(ir::IrType::I64, 0, src_line);
                            return true;
                        }
                        // raw_asm-elim wave 2: usar IrOp::STR_LIT_ADDR
                        // que ya emite `mov {dst}, @Absolute("code.s_N")`.
                        ir::IrValueId dst = fn_->new_value(ir::IrType::I64);
                        ir::IrInstr sl{};
                        sl.op          = ir::IrOp::STR_LIT_ADDR;
                        sl.type        = ir::IrType::PTR;
                        sl.dst         = dst;
                        sl.imm         = static_cast<uint64_t>(it->second);
                        sl.source_line = src_line;
                        fn_->append(current_block_, std::move(sl));
                        out_value = dst;
                        return true;
                    }
                    /* Runtime string: para MVP devolvemos 0 (no soportado).
                     * Sprint 5 anyade resolver sintetico. */
                    error_at(e->loc,
                        "find_type: en MVP solo se soporta literal string "
                        "(runtime resolver pendiente en Sprint 5)");
                    out_value = emit_const(ir::IrType::I64, 0, src_line);
                    return true;
                }
                /* Resto: el primer arg es el handle del IntrospectInfo. */
                if (e->args.empty()) {
                    out_value = ir::IR_NO_VALUE;
                    return true;
                }
                ir::IrValueId info_ptr = lower_expr(e->args[0].get());
                if (info_ptr == ir::IR_NO_VALUE) {
                    out_value = ir::IR_NO_VALUE;
                    return true;
                }
                /* Helper: emite ADD ptr + offset_const + LOAD U32. */
                auto emit_load_u32_at = [&](uint32_t offset) -> ir::IrValueId {
                    ir::IrValueId addr;
                    if (offset == 0) {
                        addr = info_ptr;
                    } else {
                        ir::IrValueId off_val = emit_const(
                            ir::IrType::I64, offset, src_line);
                        addr = fn_->new_value(ir::IrType::PTR);
                        ir::IrInstr ad{};
                        ad.op          = ir::IrOp::ADD;
                        ad.type        = ir::IrType::I64;
                        ad.dst         = addr;
                        ad.operands    = {info_ptr, off_val};
                        ad.source_line = src_line;
                        fn_->append(current_block_, std::move(ad));
                    }
                    ir::IrValueId dst = fn_->new_value(ir::IrType::U32);
                    ir::IrInstr ld{};
                    ld.op          = ir::IrOp::LOAD;
                    ld.type        = ir::IrType::U32;
                    ld.dst         = dst;
                    ld.operands    = {addr};
                    ld.source_line = src_line;
                    fn_->append(current_block_, std::move(ld));
                    return dst;
                };
                if (is_kind_i32) {
                    /* kind vive en offset 0 como u32; el tipo de retorno
                     * declarado es i32 asi que el caller ve un i32 (mismos
                     * bits). */
                    ir::IrValueId v = emit_load_u32_at(0);
                    out_value = v;
                    return true;
                }
                if (is_simple_u32) {
                    uint32_t off = 0;
                    if (name == "type_info_size")        off = 4;
                    else if (name == "type_info_align")  off = 8;
                    else if (name == "type_info_field_count") off = 12;
                    out_value = emit_load_u32_at(off);
                    return true;
                }
                if (is_name_q) {
                    /* type_info_name(p): name_off = LOAD u32 [p+16],
                     * name_len = LOAD u32 [p+20], addr = p + name_off,
                     * STRMAKE(addr, name_len). */
                    ir::IrValueId name_off = emit_load_u32_at(16);
                    ir::IrValueId name_len = emit_load_u32_at(20);
                    /* Promote name_off a i64 antes del ADD. */
                    ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
                    ir::IrInstr ad{};
                    ad.op          = ir::IrOp::ADD;
                    ad.type        = ir::IrType::I64;
                    ad.dst         = addr;
                    ad.operands    = {info_ptr, name_off};
                    ad.source_line = src_line;
                    fn_->append(current_block_, std::move(ad));
                    /* STRMAKE necesita addr y len.  Para name_len que es u32
                     * lo usamos como i64 directamente; en la VM ambos caben
                     * en qword. */
                    ir::IrValueId v_str = emit_strmake(addr, name_len, src_line);
                    out_value = v_str;
                    return true;
                }
                /* type_info_field_*: segundo arg es idx (u32). */
                if (e->args.size() < 2) {
                    out_value = ir::IR_NO_VALUE;
                    return true;
                }
                ir::IrValueId idx_val = lower_expr(e->args[1].get());
                /* field_addr = info_ptr + 24 + idx * 16 */
                ir::IrValueId v16 = emit_const(ir::IrType::I64, 16, src_line);
                ir::IrValueId idx_x16 = fn_->new_value(ir::IrType::I64); {
                    ir::IrInstr mu{};
                    mu.op          = ir::IrOp::MUL;
                    mu.type        = ir::IrType::I64;
                    mu.dst         = idx_x16;
                    mu.operands    = {idx_val, v16};
                    mu.source_line = src_line;
                    fn_->append(current_block_, std::move(mu));
                }
                ir::IrValueId v24 = emit_const(ir::IrType::I64, 24, src_line);
                ir::IrValueId field_off = fn_->new_value(ir::IrType::I64); {
                    ir::IrInstr ad{};
                    ad.op          = ir::IrOp::ADD;
                    ad.type        = ir::IrType::I64;
                    ad.dst         = field_off;
                    ad.operands    = {idx_x16, v24};
                    ad.source_line = src_line;
                    fn_->append(current_block_, std::move(ad));
                }
                ir::IrValueId field_addr = fn_->new_value(ir::IrType::PTR); {
                    ir::IrInstr ad{};
                    ad.op          = ir::IrOp::ADD;
                    ad.type        = ir::IrType::I64;
                    ad.dst         = field_addr;
                    ad.operands    = {info_ptr, field_off};
                    ad.source_line = src_line;
                    fn_->append(current_block_, std::move(ad));
                }
                /* Helper interno LOAD u32 at field_addr + offset. */
                auto load_u32_field = [&](uint32_t off) -> ir::IrValueId {
                    ir::IrValueId off_val = emit_const(
                        ir::IrType::I64, off, src_line);
                    ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
                    {
                        ir::IrInstr ad{};
                        ad.op          = ir::IrOp::ADD;
                        ad.type        = ir::IrType::I64;
                        ad.dst         = addr;
                        ad.operands    = {field_addr, off_val};
                        ad.source_line = src_line;
                        fn_->append(current_block_, std::move(ad));
                    }
                    ir::IrValueId dst = fn_->new_value(ir::IrType::U32);
                    ir::IrInstr ld{};
                    ld.op          = ir::IrOp::LOAD;
                    ld.type        = ir::IrType::U32;
                    ld.dst         = dst;
                    ld.operands    = {addr};
                    ld.source_line = src_line;
                    fn_->append(current_block_, std::move(ld));
                    return dst;
                };
                if (name == "type_info_field_offset") {
                    out_value = load_u32_field(0);
                    return true;
                }
                if (name == "type_info_field_size") {
                    out_value = load_u32_field(4);
                    return true;
                }
                if (is_field_name) {
                    /* field_addr+8 = name_off; field_addr+12 = name_len */
                    ir::IrValueId fname_off = load_u32_field(8);
                    ir::IrValueId fname_len = load_u32_field(12);
                    ir::IrValueId addr = fn_->new_value(ir::IrType::PTR); {
                        ir::IrInstr ad{};
                        ad.op          = ir::IrOp::ADD;
                        ad.type        = ir::IrType::I64;
                        ad.dst         = addr;
                        ad.operands    = {info_ptr, fname_off};
                        ad.source_line = src_line;
                        fn_->append(current_block_, std::move(ad));
                    }
                    ir::IrValueId v_str = emit_strmake(addr, fname_len, src_line);
                    out_value = v_str;
                    return true;
                }
            }
        }

        // -----------------------------------------------------------------
        // Sprint 3-C introspection: for_each_field<T>(cb) / for_each_method.
        // Loop completamente unrolled en compile-time: por cada field/
        // method de T emitimos UNA invocacion CALLCLOSURE al callback
        // con el nombre como string.  Cero overhead de loop runtime
        // (vs map dinamico), pero N llamadas reales al callback.
        // -----------------------------------------------------------------
        if (!e->type_args.empty()
         && (name == "for_each_field" || name == "for_each_method")) {
            const bool is_fields = (name == "for_each_field");
            const Type t = tc_.resolve_type_node(e->type_args[0].get());
            if (e->args.empty()) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            /* Lower el callback una sola vez -> fv_addr (16 bytes en stack). */
            const ir::IrValueId fv_addr = lower_expr(e->args[0].get());
            if (fv_addr == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            /* LOAD fn_addr = [fv_addr]; LOAD env_addr = [fv_addr + 8]. */
            ir::IrValueId fn_addr = fn_->new_value(ir::IrType::I64); {
                ir::IrInstr ld{};
                ld.op          = ir::IrOp::LOAD;
                ld.type        = ir::IrType::I64;
                ld.dst         = fn_addr;
                ld.operands    = {fv_addr};
                ld.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ld));
            }
            ir::IrValueId env_addr = fn_->new_value(ir::IrType::I64); {
                ir::IrValueId fv_plus_8 = fn_->new_value(ir::IrType::PTR);
                ir::IrValueId off8 = emit_const(ir::IrType::I64, 8, e->loc.line);
                {
                    ir::IrInstr ad{};
                    ad.op          = ir::IrOp::ADD;
                    ad.type        = ir::IrType::I64;
                    ad.dst         = fv_plus_8;
                    ad.operands    = {fv_addr, off8};
                    ad.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(ad));
                }
                ir::IrInstr ld{};
                ld.op          = ir::IrOp::LOAD;
                ld.type        = ir::IrType::I64;
                ld.dst         = env_addr;
                ld.operands    = {fv_plus_8};
                ld.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ld));
            }
            /* Iterar fields/methods y emitir una CALLCLOSURE por cada uno. */
            const uint32_t n = is_fields
                ? comptime_field_count(tc_, t)
                : comptime_method_count(tc_, t);
            for (uint32_t i = 0; i < n; ++i) {
                const std::string nm = is_fields
                    ? comptime_field_name(tc_, t, i)
                    : (i < comptime_method_count(tc_, t)
                        ? [&](){
                            /* Buscar el i-esimo method name. */
                            if (t.kind == PrimitiveKind::STRUCT
                             || t.kind == PrimitiveKind::CLASS) {
                                auto it = tc_.class_layouts().find(t.struct_name);
                                if (it != tc_.class_layouts().end()
                                 && i < it->second.methods.size()) {
                                    return it->second.methods[i].name;
                                }
                            }
                            return std::string();
                          }()
                        : std::string());
                if (nm.empty()) continue;
                /* Build StringObject para el nombre. */
                std::vector<uint8_t> bytes(nm.begin(), nm.end());
                const uint64_t idx = out_mod_->intern_static_data(
                    std::move(bytes));
                ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR); {
                    ir::IrInstr is{};
                    is.op          = ir::IrOp::STR_LIT_ADDR;
                    is.type        = ir::IrType::PTR;
                    is.dst         = v_addr;
                    is.imm         = idx;
                    is.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(is));
                }
                ir::IrValueId v_len = emit_const(
                    ir::IrType::I64,
                    static_cast<uint64_t>(nm.size()), e->loc.line);
                ir::IrValueId v_str = emit_strmake(v_addr, v_len, e->loc.line);
                /* CALLCLOSURE(env_addr, v_str) -- void return. */
                ir::IrInstr cl{};
                cl.op          = ir::IrOp::CALLCLOSURE;
                cl.type        = ir::IrType::VOID;
                cl.dst         = ir::IR_NO_VALUE;
                cl.func_ptr    = fn_addr;
                cl.operands    = {env_addr, v_str};
                cl.source_line = e->loc.line;
                fn_->append(current_block_, std::move(cl));
            }
            out_value = ir::IR_NO_VALUE;
            return true;
        }

        // -----------------------------------------------------------------
        // Sprint 3-A introspection: field_get<T>(obj, "f") / field_set.
        // Bypass de getfield/setfield: usa offset compile-time via
        // comptime_field_offset.  El type checker ya valido tipos y
        // que el segundo arg sea string literal.
        // -----------------------------------------------------------------
        if (!e->type_args.empty()
         && (name == "field_get" || name == "field_set")) {
            const bool is_get = (name == "field_get");
            const Type t = tc_.resolve_type_node(e->type_args[0].get());
            std::string fname;
            if (e->args.size() >= 2) {
                if (auto *slit = dynamic_cast<ast::StringLitExpr *>(
                        e->args[1].get())) {
                    fname = slit->value;
                }
            }
            const int64_t off = comptime_field_offset(tc_, t, fname);
            if (off < 0) {
                error_at(e->loc,
                    name + ": el tipo '" + comptime_type_name(tc_, t)
                    + "' no tiene campo '" + fname + "'");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const Type     ftype = comptime_field_type(tc_, t, fname);
            const ir::IrType ir_t = ir_type_from_primitive(ftype.kind);
            /* Lower obj: el primer arg.  Para CLASS el SSA value es un
             * host_ptr al ObjectHeader; para STRUCT inline es la direccion
             * VM del slot (resultado de la ALLOCA o del campo padre).
             * Detectamos por el TIPO declarado en T (no por el resultado de
             * check_expr del arg, que podria ser COUNT/inferido). */
            const ir::IrValueId obj = lower_expr(e->args[0].get());
            if (obj == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            /* Para CLASS la convencion del codegen es is_host_ptr=true sobre
             * el resultado de NEWOBJ/__new_<X> (host_ptr al ObjectHeader).
             * Para STRUCT la convencion es is_host_ptr=false (slot VM).
             * NO usamos emit_field_addr aqui porque su shortcut offset==0
             * MUTA el flag is_host_ptr del base SSA value (rompe init-lists
             * anteriores que comparten el binding).  En su lugar emitimos
             * un ADD i64 explicito que produce un nuevo SSA value distinto
             * del base, y propagamos is_host_ptr/pointee_is_host_ptr segun
             * la naturaleza de T. */
            const bool t_is_class = (t.kind == PrimitiveKind::CLASS);
            ir::IrValueId addr;
            if (off == 0) {
                addr = obj;
            } else {
                ir::IrValueId off_val = fn_->new_value(ir::IrType::I64);
                fn_->values[off_val].is_const  = true;
                fn_->values[off_val].const_val = static_cast<uint64_t>(off);
                {
                    ir::IrInstr c{};
                    c.op          = ir::IrOp::CONST;
                    c.type        = ir::IrType::I64;
                    c.dst         = off_val;
                    c.imm         = static_cast<uint64_t>(off);
                    c.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(c));
                }
                addr = fn_->new_value(ir::IrType::PTR);
                fn_->values[addr].is_host_ptr = t_is_class;
                ir::IrInstr ad{};
                ad.op          = ir::IrOp::ADD;
                ad.type        = ir::IrType::I64;
                ad.dst         = addr;
                ad.operands    = {obj, off_val};
                ad.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ad));
            }
            /* Si offset==0 y T es CLASS, el obj YA debe tener is_host_ptr.
             * Si T es STRUCT con offset==0, el slot VM se mantiene sin
             * tocar el flag (heredamos el state del obj, que ya es lo
             * correcto). */
            if (is_get) {
                const ir::IrValueId dst = fn_->new_value(ir_t);
                ir::IrInstr ld{};
                ld.op          = ir::IrOp::LOAD;
                ld.type        = ir_t;
                ld.dst         = dst;
                ld.operands    = {addr};
                ld.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ld));
                /* Propagar is_host_ptr para campos PTR no virtuales (mismo
                 * tratamiento que lower_class_field_load). */
                if (ftype.kind == PrimitiveKind::PTR && !ftype.is_virtual) {
                    fn_->values[dst].is_host_ptr = true;
                }
                /* Campo CLASS: el slot guarda un GcHandle, no un host_ptr.
                 * Hacemos gcderef para obtener host_ptr fresco post-GC. */
                if (ftype.kind == PrimitiveKind::CLASS) {
                    // raw_asm-elim 2026-05-28: gcderef + xchg -> IrOp::GC_DEREF_HOST.
                    ir::IrValueId v_host = fn_->new_value(ir::IrType::I64);
                    fn_->values[v_host].is_host_ptr  = true;
                    fn_->values[v_host].is_gc_object = true;
                    ir::IrInstr deref{};
                    deref.op          = ir::IrOp::GC_DEREF_HOST;
                    deref.type        = ir::IrType::PTR;
                    deref.dst         = v_host;
                    deref.operands    = {dst};
                    deref.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(deref));
                    out_value = v_host;
                    return true;
                }
                out_value = dst;
                return true;
            }
            /* field_set: lower value y emit STORE. */
            if (e->args.size() < 3) {
                /* Type checker ya emitio error; salir limpio. */
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            ir::IrValueId val = lower_expr(e->args[2].get());
            if (val == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            /* Coerce el valor al tipo del campo si difiere.  El SSA value
             * de val ya tiene su tipo en fn_->values[val].type; el cast
             * inserta truncate/sext/zext segun signos y anchos. */
            const ir::IrType val_t = fn_->values[val].type;
            val = cast_if_needed(val, val_t, ir_t, e->loc.line);
            ir::IrInstr st{};
            st.op          = ir::IrOp::STORE;
            st.type        = ir_t;
            st.dst         = ir::IR_NO_VALUE;
            /* Convencion IR: operands = {value, addr} (no al reves). */
            st.operands    = {val, addr};
            st.source_line = e->loc.line;
            fn_->append(current_block_, std::move(st));
            out_value = ir::IR_NO_VALUE;
            return true;
        }

        // -----------------------------------------------------------------
        // Sprint 2 introspection: 12 builtins de fields/methods/types.
        // Mismas garantias que Sprint 1: cada llamada baja a UN solo
        // IrOp::CONST (o STR_LIT_ADDR + STRMAKE para los que devuelven
        // string).  El type checker ya valido aridad + que los args
        // runtime sean literales compile-time.
        // -----------------------------------------------------------------
        {
            const bool one_targ_no_args =
                name == "field_count" || name == "method_count"
             || name == "is_class"    || name == "is_struct"
             || name == "is_primitive"
             || name == "is_newtype"  || name == "is_opaque"
             || name == "underlying_of";
            const bool one_targ_str_arg =
                name == "offsetof"    || name == "has_field"
             || name == "has_method"  || name == "field_type";
            const bool one_targ_int_arg = (name == "field_name");
            const bool two_targ_no_args =
                name == "is_subtype"  || name == "is_same";

            if ((one_targ_no_args || one_targ_str_arg
              || one_targ_int_arg || two_targ_no_args)
             && !e->type_args.empty()) {
                const uint32_t src_line = e->loc.line;
                const Type t1 = tc_.resolve_type_node(e->type_args[0].get());
                /* Helper local: emite STRMAKE con el nombre canonico recibido. */
                auto emit_strmake_for = [&](const std::string &nm)
                                            -> ir::IrValueId {
                    std::vector<uint8_t> bytes(nm.begin(), nm.end());
                    const uint64_t idx = out_mod_->intern_static_data(
                        std::move(bytes));
                    ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR); {
                        ir::IrInstr is{};
                        is.op          = ir::IrOp::STR_LIT_ADDR;
                        is.type        = ir::IrType::PTR;
                        is.dst         = v_addr;
                        is.imm         = idx;
                        is.source_line = src_line;
                        fn_->append(current_block_, std::move(is));
                    }
                    ir::IrValueId v_len = emit_const(
                        ir::IrType::I64,
                        static_cast<uint64_t>(nm.size()), src_line);
                    ir::IrValueId v_str = emit_strmake(v_addr, v_len, src_line);
                    return v_str;
                };

                /* Extraer el arg literal compile-time si lo hay. */
                std::string slit_arg;
                uint64_t    ilit_arg = 0;
                if (one_targ_str_arg) {
                    auto *slit = dynamic_cast<ast::StringLitExpr *>(
                        e->args[0].get());
                    if (slit) slit_arg = slit->value;
                }
                if (one_targ_int_arg) {
                    auto *ilit = dynamic_cast<ast::IntLitExpr *>(
                        e->args[0].get());
                    if (ilit) ilit_arg = ilit->value;
                }

                if (name == "field_count") {
                    const uint32_t v = comptime_field_count(tc_, t1);
                    out_value = emit_const(ir::IrType::U32, v, src_line);
                    return true;
                }
                if (name == "method_count") {
                    const uint32_t v = comptime_method_count(tc_, t1);
                    out_value = emit_const(ir::IrType::U32, v, src_line);
                    return true;
                }
                if (name == "is_class") {
                    const bool v = comptime_is_class(t1);
                    out_value = emit_const(ir::IrType::BOOL,
                        v ? 1ULL : 0ULL, src_line);
                    return true;
                }
                if (name == "is_struct") {
                    const bool v = comptime_is_struct(tc_, t1);
                    out_value = emit_const(ir::IrType::BOOL,
                        v ? 1ULL : 0ULL, src_line);
                    return true;
                }
                if (name == "is_primitive") {
                    const bool v = comptime_is_primitive(t1);
                    out_value = emit_const(ir::IrType::BOOL,
                        v ? 1ULL : 0ULL, src_line);
                    return true;
                }
                if (name == "is_newtype") {
                    const bool v = comptime_is_newtype(t1);
                    out_value = emit_const(ir::IrType::BOOL,
                        v ? 1ULL : 0ULL, src_line);
                    return true;
                }
                if (name == "is_opaque") {
                    const bool v = comptime_is_opaque(t1);
                    out_value = emit_const(ir::IrType::BOOL,
                        v ? 1ULL : 0ULL, src_line);
                    return true;
                }
                if (name == "underlying_of") {
                    const std::string un = comptime_underlying_name(tc_, t1);
                    out_value = emit_strmake_for(un);
                    return true;
                }
                if (name == "offsetof") {
                    const int64_t off = comptime_field_offset(tc_, t1, slit_arg);
                    /* off==-1 (campo no existe): emitir error claro y
                     * usar 0 como fallback para no romper el flujo. */
                    if (off < 0) {
                        diags_.error(e->loc,
                            "offsetof: el tipo '" + comptime_type_name(tc_, t1)
                            + "' no tiene campo '" + slit_arg + "'");
                    }
                    out_value = emit_const(ir::IrType::U64,
                        off < 0 ? 0ULL : static_cast<uint64_t>(off), src_line);
                    return true;
                }
                if (name == "has_field") {
                    const bool v = comptime_has_field(tc_, t1, slit_arg);
                    out_value = emit_const(ir::IrType::BOOL,
                        v ? 1ULL : 0ULL, src_line);
                    return true;
                }
                if (name == "has_method") {
                    const bool v = comptime_has_method(tc_, t1, slit_arg);
                    out_value = emit_const(ir::IrType::BOOL,
                        v ? 1ULL : 0ULL, src_line);
                    return true;
                }
                if (name == "field_type") {
                    const std::string tn = comptime_field_type_name(
                        tc_, t1, slit_arg);
                    if (tn.empty()) {
                        diags_.error(e->loc,
                            "field_type: el tipo '" + comptime_type_name(tc_, t1)
                            + "' no tiene campo '" + slit_arg + "'");
                    }
                    out_value = emit_strmake_for(tn);
                    return true;
                }
                if (name == "field_name") {
                    const std::string nm_v = comptime_field_name(
                        tc_, t1, static_cast<uint32_t>(ilit_arg));
                    if (nm_v.empty()) {
                        diags_.error(e->loc,
                            "field_name: el tipo '" + comptime_type_name(tc_, t1)
                            + "' no tiene campo en indice "
                            + std::to_string(ilit_arg));
                    }
                    out_value = emit_strmake_for(nm_v);
                    return true;
                }
                if (name == "is_same") {
                    const Type t2 = tc_.resolve_type_node(
                        e->type_args[1].get());
                    const bool v = comptime_is_same(tc_, t1, t2);
                    out_value = emit_const(ir::IrType::BOOL,
                        v ? 1ULL : 0ULL, src_line);
                    return true;
                }
                if (name == "is_subtype") {
                    const Type t2 = tc_.resolve_type_node(
                        e->type_args[1].get());
                    const bool v = comptime_is_subtype(tc_, t1, t2);
                    out_value = emit_const(ir::IrType::BOOL,
                        v ? 1ULL : 0ULL, src_line);
                    return true;
                }
            }
        }

        // Conjunto de nombres builtin reconocidos.  Si el nombre no esta
        // aqui devolvemos false para que lower_call siga con la ruta
        // generica (CALL a una funcion del usuario).
        const bool is_print     = (name == "print");
        /* Phase MC.18: `comptime_print` / `ct_print` se aliasan a
         * `println` -- en el path VM-lowered es exactamente eso (print
         * a stderr).  El macro corre en compile time (porque la
         * ComptimeRuntime ejecuta el body al type-checkear el call
         * site), asi que el output aparece durante la compilacion
         * igual que el AST eval. */
        const bool is_println   = (name == "println"
                                    || name == "comptime_print"
                                    || name == "ct_print");
        const bool is_echo      = (name == "echo");  // alias de print
        const bool is_flush     = (name == "flush"); // vio_flush() sin args
        const bool is_print_int = (name == "print_int");
        // builtins de I/O explicitos por tipo (sin newline; usar
        // println o print + "\n" si lo necesitas).
        const bool is_print_uint  = (name == "print_uint");
        const bool is_print_hex   = (name == "print_hex");
        const bool is_print_float = (name == "print_float");
        const bool is_print_bool  = (name == "print_bool");
        const bool is_print_char  = (name == "print_char");
        const bool is_print_color = (name == "print_color");
        const bool is_print_cstr  = (name == "print_cstr");
        // formatos numericos alternativos (binario / octal) y impresion
        // de punteros / handles de objetos GC + padding para alineacion.
        const bool is_print_bin      = (name == "print_bin");
        const bool is_print_oct      = (name == "print_oct");
        const bool is_print_ptr      = (name == "print_ptr");
        const bool is_print_gchandle = (name == "print_gchandle");
        const bool is_print_pad      = (name == "print_pad");
        // Builtins de terminal/ANSI (azucar para escapes VT100 comunes).
        // Cada uno emite secuencias estaticas via vio_print sin necesitar
        // hardcodear los escapes en el codigo del usuario.
        const bool is_term_clear         = (name == "term_clear");
        const bool is_term_clear_line    = (name == "term_clear_line");
        const bool is_term_move          = (name == "term_move");
        const bool is_term_save_cursor   = (name == "term_save_cursor");
        const bool is_term_restore_cursor= (name == "term_restore_cursor");
        const bool is_term_hide_cursor   = (name == "term_hide_cursor");
        const bool is_term_show_cursor   = (name == "term_show_cursor");
        const bool is_term_reset         = (name == "term_reset");
        const bool is_fopen       = (name == "fopen");
        const bool is_fwrite      = (name == "fwrite");
        const bool is_fclose      = (name == "fclose");
        const bool is_malloc      = (name == "malloc");
        const bool is_free        = (name == "free");
        // Builtins de reflexion y AOP
        const bool is_forName     = (name == "forName");
        const bool is_getClass    = (name == "getClass");
        const bool is_getField    = (name == "getField");
        const bool is_getMethod   = (name == "getMethod");
        const bool is_newInstance = (name == "newInstance");
        const bool is_invoke      = (name == "invoke");
        const bool is_proceed     = (name == "proceed");
        // Optional via instrucciones VM isnull/unwrap (referencias).
        const bool is_isPresent = (name == "isPresent");
        const bool is_unwrap    = (name == "unwrap");
        // Optional/Result builtins del compilador (stack values).
        const bool is_Some  = (name == "Some");
        const bool is_None  = (name == "None");
        const bool is_Ok    = (name == "Ok");
        const bool is_Err   = (name == "Err");
        const bool is_isOk  = (name == "isOk");
        const bool is_value = (name == "value");
        const bool is_error = (name == "error");
        // monitor builtins.  Cada uno baja a 1 instruccion bytecode.
        const bool is_wait      = (name == "wait");
        const bool is_notify    = (name == "notify");
        const bool is_notifyAll = (name == "notifyAll");
        // procesos / IPC builtins.
        const bool is_pid     = (name == "pid");
        const bool is_msgsend = (name == "msgsend");
        const bool is_msgrecv = (name == "msgrecv");
        // argv del script: bajan a getargc/getarg.
        const bool is_args_count = (name == "args_count");
        const bool is_args_get   = (name == "args_get");
        // Introspeccion runtime: enumeracion de miembros de clase.
        const bool is_getMethods    = (name == "getMethods");
        const bool is_getMethodAt   = (name == "getMethodAt");
        const bool is_getFields     = (name == "getFields");
        const bool is_getFieldAt    = (name == "getFieldAt");
        // futures builtins.
        const bool is_future_alloc = (name == "future_alloc");
        const bool is_fulfill      = (name == "fulfill");
        // carga dinamica de modulos.
        const bool is_loadmodule   = (name == "loadmodule");
        const bool is_unloadmodule = (name == "unloadmodule");
        const bool is_dispose    = (name == "dispose");
        // constructor de tipo coleccion primitivo (arraylist, hashmap,
        // hashset, queue, deque, treemap, treeset, stack).  Si find_col_ctor
        // devuelve no-null, el lowering emite CALLN al native_new_fn del
        // plugin vesta_collections con el argumento de capacidad inicial
        // (o sin args para tipos sin default_cap como TreeMap).
        const ColType *col_ctor    = find_col_ctor(name);
        const bool     is_col_ctor = (col_ctor != nullptr);
        // FFI runtime dinamico: builtins sintaxis-VSH para cargar
        // DLLs y resolver/llamar simbolos en tiempo de ejecucion.
        const bool is_ffi_open = (name == "ffi_open");
        const bool is_ffi_sym  = (name == "ffi_sym");
        const bool is_ffi_call = (name == "ffi_call");
        // panic("msg") -> opcode panic con FATAL_USER_ABORT.
        const bool is_panic = (name == "panic");
        // Math builtins (delegan a stdlib/native/math/vesta_math).
        const bool is_math_sqrt  = (name == "sqrt");
        const bool is_math_pow   = (name == "pow");
        const bool is_math_fabs  = (name == "fabs");
        const bool is_math_floor = (name == "floor");
        const bool is_math_ceil  = (name == "ceil");
        const bool is_math_round = (name == "round");
        const bool is_math_fmin  = (name == "fmin");
        const bool is_math_fmax  = (name == "fmax");
        const bool is_math_log   = (name == "log");
        const bool is_math_log2  = (name == "log2");
        const bool is_math_log10 = (name == "log10");
        const bool is_math_sin   = (name == "sin");
        const bool is_math_cos   = (name == "cos");
        const bool is_math_tan   = (name == "tan");
        const bool is_math_abs   = (name == "abs");
        const bool is_math_imin  = (name == "imin");
        const bool is_math_imax  = (name == "imax");
        const bool is_math_clamp = (name == "clamp");
        // Math-IR-promote v2.2a: bit ops + new int/float ops.
        const bool is_math_trunc    = (name == "trunc");
        const bool is_math_iminu    = (name == "iminu");
        const bool is_math_imaxu    = (name == "imaxu");
        const bool is_math_ilog2    = (name == "ilog2");
        const bool is_math_popcount = (name == "popcount");
        const bool is_math_clz      = (name == "clz");
        const bool is_math_ctz      = (name == "ctz");
        const bool is_math_bswap    = (name == "bswap");
        const bool is_math_rotl     = (name == "rotl");
        const bool is_math_rotr     = (name == "rotr");
        const bool is_any_math   = is_math_sqrt || is_math_pow || is_math_fabs
                || is_math_floor || is_math_ceil || is_math_round
                || is_math_fmin || is_math_fmax
                || is_math_log || is_math_log2 || is_math_log10
                || is_math_sin || is_math_cos || is_math_tan
                || is_math_abs || is_math_imin || is_math_imax
                || is_math_clamp
                || is_math_trunc || is_math_iminu || is_math_imaxu
                || is_math_ilog2 || is_math_popcount
                || is_math_clz || is_math_ctz || is_math_bswap
                || is_math_rotl || is_math_rotr;
        // smart pointers builtins (unique<T> y shared<T>).
        const bool is_unique_box  = (name == "unique_box");
        const bool is_shared_box  = (name == "shared_box");
        const bool is_unique_with = (name == "unique_with");
        const bool is_shared_with = (name == "shared_with");
        // Borrow builtins: lend/lend_mut son operaciones zero-overhead
        // que devuelven el ptr_of del owner (slot+0).  El borrow checker
        // ya valido las reglas en compile-time, asi que aqui solo emitimos
        // la lectura del puntero.  read_borrow/write_borrow son
        // *p y *p=v respectivamente.
        const bool is_lend         = (name == "lend");
        const bool is_lend_mut     = (name == "lend_mut");
        const bool is_read_borrow  = (name == "read_borrow");
        const bool is_write_borrow = (name == "write_borrow");
        const bool is_move       = (name == "move");
        const bool is_get        = (name == "ptr_of");
        const bool is_use_count  = (name == "use_count");
        // Z.6 builtins: is_shared / share / unshare.
        const bool is_z6_isshared = (name == "is_shared");
        const bool is_z6_share    = (name == "share");
        const bool is_z6_unshare  = (name == "unshare");
        // Z.8 builtins: atomic primitives + shared raw allocator.
        const bool is_z8_atomic_load  = (name == "atomic_load_i64");
        const bool is_z8_atomic_store = (name == "atomic_store_i64");
        const bool is_z8_atomic_cas   = (name == "atomic_cas_i64");
        const bool is_z8_atomic_add   = (name == "atomic_add_i64");
        const bool is_z8_shared_malloc = (name == "shared_malloc");
        const bool is_z8_shared_free   = (name == "shared_free");
        // Z.10 builtins: introspeccion + GC placeholder del SharedHeap.
        const bool is_z10_live_count = (name == "shared_heap_live_count");
        const bool is_z10_bytes      = (name == "shared_heap_bytes");
        const bool is_z10_gc_collect = (name == "shared_gc_collect");
        // Builtins de string: cada uno baja a una sola instruccion bytecode
        // dedicada (STRLEN, STRGETBYTES, STRRAW, etc.) sin pasar por CALLN.
        /* Phase MC.15B: alias comptime_* a sus equivalentes runtime str_*
         * cuando aparecen en cuerpos de @Macro lowereados a IR.  El
         * type_checker ya valido el call con el comptime evaluator; aqui
         * solo emitimos el bytecode que la VM ejecutara al invocar el
         * macro lowereado.  Mismo path que el runtime str_* user-facing. */
        const bool is_str_length  = (name == "str_length"  || name == "comptime_strlen");
        const bool is_str_bytes   = (name == "str_bytes");
        const bool is_str_cstr    = (name == "str_cstr");
        const bool is_str_wstr    = (name == "str_wstr");
        const bool is_str_hash    = (name == "str_hash");
        const bool is_str_intern  = (name == "str_intern");
        const bool is_str_concat  = (name == "str_concat" || name == "comptime_concat");
        const bool is_str_equals  = (name == "str_equals" || name == "comptime_streq");
        const bool is_str_make    = (name == "str_make");
        const bool is_str_convert = (name == "str_convert");
        // Builtins runtime de busqueda/slice sobre string.  str_substr baja
        // a STRSLICE (vista sin copia); los de busqueda delegan en el plugin
        // vesta_collections (zero-copy sobre buffers host via memmem/memcmp).
        const bool is_str_substr      = (name == "str_substr");
        const bool is_str_starts_with = (name == "str_starts_with");
        const bool is_str_ends_with   = (name == "str_ends_with");
        const bool is_str_index_of    = (name == "str_index_of");
        /* Phase MC.15C: aliases comptime adicionales que lowerean a
         * codigo runtime (eliminando rejection en macro pre-validation). */
        const bool is_to_str       = (name == "to_str" || name == "comptime_to_str");
        const bool is_chr_b        = (name == "chr"    || name == "comptime_chr");
        const bool is_ord_b        = (name == "ord"    || name == "comptime_ord");
        const bool is_substr_b     = (name == "substr" || name == "comptime_substr" || is_str_substr);
        const bool is_gensym_b     = (name == "gensym");
        const bool is_repeat_b     = (name == "repeat"   || name == "comptime_repeat");
        const bool is_replace_b    = (name == "replace"  || name == "comptime_replace");
        const bool is_contains_b   = (name == "contains" || name == "comptime_contains");
        const bool is_static_assert_b = (name == "static_assert");
        const bool is_any_builtin = is_print || is_println || is_echo
                || is_flush || is_print_int
                || is_print_uint || is_print_hex
                || is_print_float || is_print_bool
                || is_print_char || is_print_color
                || is_print_cstr
                || is_print_bin || is_print_oct
                || is_print_ptr || is_print_gchandle || is_print_pad
                || is_fopen || is_fwrite || is_fclose
                || is_malloc || is_free
                || is_forName || is_getClass || is_getField
                || is_getMethod || is_newInstance || is_invoke
                || is_proceed
                || is_isPresent || is_unwrap
                || is_Some || is_None
                || is_Ok || is_Err || is_isOk
                || is_value || is_error
                || is_wait || is_notify || is_notifyAll
                || is_pid || is_msgsend || is_msgrecv
                || is_args_count || is_args_get
                || is_getMethods || is_getMethodAt
                || is_getFields || is_getFieldAt
                || is_term_clear || is_term_clear_line || is_term_move
                || is_term_save_cursor || is_term_restore_cursor
                || is_term_hide_cursor || is_term_show_cursor || is_term_reset
                || is_future_alloc || is_fulfill
                || is_loadmodule || is_unloadmodule
                || is_ffi_open || is_ffi_sym || is_ffi_call
                || is_panic
                || is_str_length || is_str_bytes
                || is_str_cstr || is_str_wstr
                || is_str_hash || is_str_intern
                || is_str_concat || is_str_equals
                || is_str_make || is_str_convert
                || is_str_substr || is_str_starts_with
                || is_str_ends_with || is_str_index_of
                || is_to_str || is_chr_b || is_ord_b
                || is_substr_b || is_gensym_b
                || is_repeat_b || is_replace_b || is_contains_b
                || is_static_assert_b
                || is_any_math
                || is_col_ctor
                || is_dispose
                || is_unique_box || is_shared_box
                || is_unique_with || is_shared_with
                || is_move || is_get || is_use_count
                || is_lend || is_lend_mut
                || is_read_borrow || is_write_borrow
                || is_z6_isshared || is_z6_share || is_z6_unshare // Z.6
                || is_z8_atomic_load || is_z8_atomic_store         // Z.8
                || is_z8_atomic_cas  || is_z8_atomic_add
                || is_z8_shared_malloc || is_z8_shared_free
                || is_z10_live_count || is_z10_bytes                // Z.10
                || is_z10_gc_collect;
        if (!is_any_builtin) return false;

        // Helper interno para registrar un literal de string en static_data
        // y emitir un STR_LIT_ADDR + CONST(len) en el bloque actual.
        // Devuelve par (str_ptr_ir_value, len_ir_value).
        auto emit_string_lit = [&](ast::StringLitExpr *slit)
            -> std::pair<ir::IrValueId, ir::IrValueId> {
            std::vector<uint8_t> bytes(slit->value.begin(), slit->value.end());
            const uint64_t       lit_idx = out_mod_->intern_static_data(std::move(bytes));
            const uint64_t       lit_len = (uint64_t) slit->value.size();
            const ir::IrValueId  v_str   = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr          is{};
            is.op          = ir::IrOp::STR_LIT_ADDR;
            is.type        = ir::IrType::PTR;
            is.dst         = v_str;
            is.imm         = lit_idx;
            is.source_line = slit->loc.line;
            fn_->append(current_block_, std::move(is));
            const ir::IrValueId v_len = emit_const(ir::IrType::I64, lit_len, slit->loc.line);
            return {v_str, v_len};
        };

        // Helper que emite getproc en el bloque actual.
        auto emit_getproc = [&](uint32_t line) -> ir::IrValueId {
            const ir::IrValueId v_proc = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr         ip{};
            ip.op          = ir::IrOp::GETPROC;
            ip.type        = ir::IrType::PTR;
            ip.dst         = v_proc;
            ip.source_line = line;
            fn_->append(current_block_, std::move(ip));
            return v_proc;
        };

        const std::string lib = "stdlib/native/io/vesta_io";

        // -----------------------------------------------------------------
        // Helpers para emitir un fragmento de salida.
        //
        // emit_print_string_literal(text):  CALLN vio_print(proc, addr, len)
        //   con text registrado en static_data.  Si text vacio, no-op.
        //
        // emit_print_typed_value(expr):  segun el tipo de expr, despacha a
        //   vio_print_int / _uint / _hex / _float / _bool / _char / o
        //   vio_print(proc, addr, len) si el tipo es PTR (string).  Solo
        //   un CALLN por valor; cero overhead intermedio.
        //
        // emit_print_arg(expr): si expr es StringLitExpr interpolado,
        //   itera parts/exprs y emite UN CALLN por fragmento.  Si es un
        //   string simple emite UN solo CALLN.  Si es escalar despacha
        //   por tipo via emit_print_typed_value.
        //
        // emit_print_newline():  CALLN vio_print_newline (cero args).
        // -----------------------------------------------------------------
        auto emit_print_string_literal = [&](const std::string &text,
                                             uint32_t           line) {
            if (text.empty()) return;
            std::vector<uint8_t> bytes(text.begin(), text.end());
            const uint64_t       lit_idx = out_mod_->intern_static_data(std::move(bytes));
            const uint64_t       lit_len = (uint64_t) text.size();
            const ir::IrValueId  v_str   = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr          is{};
            is.op          = ir::IrOp::STR_LIT_ADDR;
            is.type        = ir::IrType::PTR;
            is.dst         = v_str;
            is.imm         = lit_idx;
            is.source_line = line;
            fn_->append(current_block_, std::move(is));
            const ir::IrValueId v_len = emit_const(ir::IrType::I64,
                                                   lit_len, line);
            const ir::IrValueId v_proc = emit_getproc(line);
            out_mod_->register_native_import(lib, "vio_print");
            ir::IrInstr ins{};
            ins.op          = ir::IrOp::CALLN;
            ins.type        = ir::IrType::VOID;
            ins.dst         = ir::IR_NO_VALUE;
            ins.func_name   = lib + ":vio_print";
            ins.operands    = {v_proc, v_str, v_len};
            ins.source_line = line;
            fn_->append(current_block_, std::move(ins));
        };

        auto emit_print_newline = [&](uint32_t line) {
            out_mod_->register_native_import(lib, "vio_print_newline");
            ir::IrInstr ins{};
            ins.op          = ir::IrOp::CALLN;
            ins.type        = ir::IrType::VOID;
            ins.dst         = ir::IR_NO_VALUE;
            ins.func_name   = lib + ":vio_print_newline";
            ins.source_line = line;
            fn_->append(current_block_, std::move(ins));
        };

        // Tabla de ANSI codes (duplicada con lower_ident por simplicidad).
        // Lookup O(N=33) ejecutado solo en compile time.
        static const struct {
            const char *name;
            const char *seq;
        } ANSI_LU[] = {
                    {"BLACK", "\x1b[30m"}, {"RED", "\x1b[31m"}, {"GREEN", "\x1b[32m"},
                    {"YELLOW", "\x1b[33m"}, {"BLUE", "\x1b[34m"}, {"MAGENTA", "\x1b[35m"},
                    {"CYAN", "\x1b[36m"}, {"WHITE", "\x1b[37m"},
                    {"BR_BLACK", "\x1b[90m"}, {"BR_RED", "\x1b[91m"},
                    {"BR_GREEN", "\x1b[92m"}, {"BR_YELLOW", "\x1b[93m"},
                    {"BR_BLUE", "\x1b[94m"}, {"BR_MAGENTA", "\x1b[95m"},
                    {"BR_CYAN", "\x1b[96m"}, {"BR_WHITE", "\x1b[97m"},
                    {"BG_BLACK", "\x1b[40m"}, {"BG_RED", "\x1b[41m"},
                    {"BG_GREEN", "\x1b[42m"}, {"BG_YELLOW", "\x1b[43m"},
                    {"BG_BLUE", "\x1b[44m"}, {"BG_MAGENTA", "\x1b[45m"},
                    {"BG_CYAN", "\x1b[46m"}, {"BG_WHITE", "\x1b[47m"},
                    {"BOLD", "\x1b[1m"}, {"DIM", "\x1b[2m"},
                    {"ITALIC", "\x1b[3m"}, {"UNDERLINE", "\x1b[4m"},
                    {"BLINK", "\x1b[5m"}, {"REVERSE", "\x1b[7m"},
                    {"RESET", "\x1b[0m"}, {"CLEAR_SCREEN", "\x1b[2J"},
                    {"CURSOR_HOME", "\x1b[H"},
                };

        // Helper local: parsea una cadena de formato `${expr:fmt}` en
        // secciones separadas por `:`.  Devuelve un struct con kind
        // (hex/bin/oct/dec/ptr/gc/char/bool/auto), align (left/right/none),
        // width y fill char.  La forma sin `:` (formato vacio) deja todo
        // en defaults (auto + sin alineacion).
        struct FmtSpec {
            enum class Kind {
                AUTO,   // dispatch por tipo (comportamiento default)
                DEC,    // entero decimal con signo correcto
                HEX,    // 0x + 16 hex fixed
                BIN,    // 0b + bits compactos
                OCT,    // 0o + dig compactos
                PTR,    // 0x + hex compacto
                GC,     // <gc:N>
                CHAR,   // codepoint -> UTF-8
                BOOL    // "true"/"false"
            };
            enum class Align { NONE, LEFT, RIGHT };
            Kind     kind   = Kind::AUTO;
            Align    align  = Align::NONE;
            uint32_t width  = 0;
            uint32_t fill_cp = 32; // espacio por defecto
        };
        auto parse_fmt_spec = [&](const std::string &s,
                                   const SourceLoc &loc) -> FmtSpec {
            FmtSpec out;
            size_t i = 0;
            while (i < s.size()) {
                // Saltar espacios.
                while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
                if (i >= s.size()) break;
                // Detectar alineacion: primer caracter '<' / '>' = left/right,
                // seguido de digitos para el width, y opcionalmente un char
                // de fill.
                if (s[i] == '<' || s[i] == '>') {
                    out.align = (s[i] == '<') ? FmtSpec::Align::LEFT
                                              : FmtSpec::Align::RIGHT;
                    ++i;
                    uint32_t w = 0;
                    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                        w = w * 10 + (uint32_t)(s[i] - '0');
                        ++i;
                    }
                    out.width = w;
                    // Optional fill char (cualquier caracter no `:` ni final).
                    if (i < s.size() && s[i] != ':') {
                        // tomar UN char (asumimos ASCII; multibyte no
                        // soportado en este parser simple).
                        out.fill_cp = (uint32_t)(uint8_t)s[i];
                        ++i;
                    }
                } else {
                    // Detectar keyword.
                    size_t start = i;
                    while (i < s.size() && s[i] != ':' && s[i] != ' '
                                          && s[i] != '\t') ++i;
                    std::string kw = s.substr(start, i - start);
                    if      (kw == "hex")  out.kind = FmtSpec::Kind::HEX;
                    else if (kw == "bin")  out.kind = FmtSpec::Kind::BIN;
                    else if (kw == "oct")  out.kind = FmtSpec::Kind::OCT;
                    else if (kw == "dec")  out.kind = FmtSpec::Kind::DEC;
                    else if (kw == "ptr")  out.kind = FmtSpec::Kind::PTR;
                    else if (kw == "gc")   out.kind = FmtSpec::Kind::GC;
                    else if (kw == "char") out.kind = FmtSpec::Kind::CHAR;
                    else if (kw == "bool") out.kind = FmtSpec::Kind::BOOL;
                    else {
                        diags_.warning(loc,
                            std::string("formato '") + kw +
                            "' desconocido en ${...:fmt}; usando default");
                    }
                }
                // Saltar separador `:`.
                while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
                if (i < s.size() && s[i] == ':') ++i;
            }
            return out;
        };

        auto emit_print_typed_value = [&](ast::Expr *ex,
                                            const std::string &fmt_str) {
            if (!ex) return;
            const Type t = ex->result_type;
            // Parsear formato y, si hay alineacion right, calcular y emitir
            // padding ANTES del valor (para left-align se emite DESPUES).
            // Como no medimos el ancho exacto del valor a emitir (eso
            // requeriria itoa+len al vuelo), aceptamos un sub-set: el
            // usuario pasa un ancho que va a quedar como margen superior
            // al ancho real.  Para alineacion exacta de columnas con
            // valores variables, usar print_pad explicito.
            FmtSpec fs = parse_fmt_spec(fmt_str, ex->loc);
            // Caso especial: identificador ANSI magico -> emit la cadena
            // directamente como string literal (sin pasar por print_int).
            if (ex->kind == ast::NodeKind::IdentExpr) {
                auto *id_ex = static_cast<ast::IdentExpr *>(ex);
                for (const auto &m: ANSI_LU) {
                    if (id_ex->name == m.name) {
                        emit_print_string_literal(m.seq, ex->loc.line);
                        return;
                    }
                }
            }
            // Caso especial: string literal directo (PTR a static_data).
            // NO recursamos en interpolacion anidada (raro y requeriria
            // std::function para auto-call).  Si llega un string
            // interpolado dentro de ${...}, error claro.
            if (ex->kind == ast::NodeKind::StringLitExpr) {
                auto *sl = static_cast<ast::StringLitExpr *>(ex);
                if (sl->is_interpolated()) {
                    error_at(ex->loc,
                             "interpolacion anidada dentro de ${...} no soportada (asignar a variable y usar la variable)");
                    return;
                }
                emit_print_string_literal(sl->value, ex->loc.line);
                return;
            }
            // Lower la expr a un SSA value y despachar por tipo.
            ir::IrValueId v = lower_expr(ex);
            if (v == ir::IR_NO_VALUE) return;
            const ir::IrType vt = fn_->values[v].type;

            // Format spec ${expr:fmt}: si el formato pide un kind concreto
            // (hex/bin/oct/dec/ptr/gc/char/bool) o alineacion, usamos el
            // helper unificado @c vio_print_fmt(value, kind, width,
            // fill, align) que combina formateo + padding en un solo
            // CALLN.  Sin formato (default), caemos al dispatch normal
            // por tipo abajo.
            const bool has_fmt = fs.kind != FmtSpec::Kind::AUTO
                              || fs.align != FmtSpec::Align::NONE;
            if (has_fmt && t.kind != PrimitiveKind::STRING) {
                // Determinar el kind code para vio_print_fmt.  Si AUTO,
                // derivar del tipo de la expresion.  La logica es la
                // misma del switch de abajo.
                int kind_code = -1;
                if (fs.kind == FmtSpec::Kind::HEX)       kind_code = 2;
                else if (fs.kind == FmtSpec::Kind::BIN)  kind_code = 3;
                else if (fs.kind == FmtSpec::Kind::OCT)  kind_code = 4;
                else if (fs.kind == FmtSpec::Kind::PTR)  kind_code = 5;
                else if (fs.kind == FmtSpec::Kind::GC)   kind_code = 6;
                else if (fs.kind == FmtSpec::Kind::BOOL) kind_code = 7;
                else if (fs.kind == FmtSpec::Kind::CHAR) kind_code = 8;
                else if (fs.kind == FmtSpec::Kind::DEC) {
                    const bool unsigned_t = (t.kind == PrimitiveKind::CHAR
                            || t.kind == PrimitiveKind::U8
                            || t.kind == PrimitiveKind::U16
                            || t.kind == PrimitiveKind::U32
                            || t.kind == PrimitiveKind::U64);
                    kind_code = unsigned_t ? 1 : 0;
                } else {
                    // AUTO: dispatch por tipo del operando.
                    switch (t.kind) {
                        case PrimitiveKind::BOOL: kind_code = 7; break;
                        case PrimitiveKind::CHAR:
                        case PrimitiveKind::U8: case PrimitiveKind::U16:
                        case PrimitiveKind::U32: case PrimitiveKind::U64:
                            kind_code = 1; break;
                        case PrimitiveKind::I8: case PrimitiveKind::I16:
                        case PrimitiveKind::I32: case PrimitiveKind::I64:
                            kind_code = 0; break;
                        case PrimitiveKind::F32:
                        case PrimitiveKind::F64: kind_code = 9; break;
                        case PrimitiveKind::PTR:
                        case PrimitiveKind::ARRAY: kind_code = 5; break;
                        case PrimitiveKind::CLASS: kind_code = 6; break;
                        default: kind_code = 1; break;
                    }
                }
                // Convertir el valor al uint64 que espera vio_print_fmt.
                ir::IrValueId v_arg = v;
                if (t.kind == PrimitiveKind::F32) {
                    // F32 -> F64 (re-encoding) -> i64 bits.
                    ir::IrValueId f64v = fn_->new_value(ir::IrType::F64);
                    ir::IrInstr ext{};
                    ext.op = ir::IrOp::F32TOF64; ext.type = ir::IrType::F64;
                    ext.dst = f64v; ext.operands = {v_arg};
                    ext.source_line = ex->loc.line;
                    fn_->append(current_block_, std::move(ext));
                    ir::IrValueId bits = fn_->new_value(ir::IrType::I64);
                    ir::IrInstr bc{};
                    bc.op = ir::IrOp::BITCAST; bc.type = ir::IrType::I64;
                    bc.dst = bits; bc.operands = {f64v};
                    bc.source_line = ex->loc.line;
                    fn_->append(current_block_, std::move(bc));
                    v_arg = bits;
                } else if (t.kind == PrimitiveKind::F64 && vt != ir::IrType::I64) {
                    ir::IrValueId bits = fn_->new_value(ir::IrType::I64);
                    ir::IrInstr bc{};
                    bc.op = ir::IrOp::BITCAST; bc.type = ir::IrType::I64;
                    bc.dst = bits; bc.operands = {v_arg};
                    bc.source_line = ex->loc.line;
                    fn_->append(current_block_, std::move(bc));
                    v_arg = bits;
                } else if (t.kind == PrimitiveKind::CLASS) {
                    // CLASS -> GcHandle via instruccion `gchandle`.
                    v_arg = emit_gc_handle_for_ptr(v_arg, ex->loc.line);
                } else {
                    // Numeros enteros y punteros: cast (silencioso) a I64.
                    v_arg = cast_if_needed(v_arg, vt, ir::IrType::I64,
                                           ex->loc.line, /*is_explicit=*/true);
                }
                // Constantes para kind, width, fill, align.
                ir::IrValueId v_kind  = emit_const(ir::IrType::I64,
                        (uint64_t)(uint32_t)kind_code, ex->loc.line);
                ir::IrValueId v_width = emit_const(ir::IrType::I64,
                        (uint64_t)fs.width, ex->loc.line);
                ir::IrValueId v_fill  = emit_const(ir::IrType::I64,
                        (uint64_t)fs.fill_cp, ex->loc.line);
                int align_code = (fs.align == FmtSpec::Align::LEFT) ? 1
                                : (fs.align == FmtSpec::Align::RIGHT) ? 2
                                : 0;
                ir::IrValueId v_align = emit_const(ir::IrType::I64,
                        (uint64_t)align_code, ex->loc.line);
                out_mod_->register_native_import(lib, "vio_print_fmt");
                ir::IrInstr ins{};
                ins.op          = ir::IrOp::CALLN;
                ins.type        = ir::IrType::VOID;
                ins.dst         = ir::IR_NO_VALUE;
                ins.func_name   = lib + ":vio_print_fmt";
                ins.operands    = {v_arg, v_kind, v_width, v_fill, v_align};
                ins.source_line = ex->loc.line;
                fn_->append(current_block_, std::move(ins));
                return;
            }

            // Caso STRING: el valor es GcHandle a un StringObject.  Emitir
            // STRRAW para obtener host_ptr al buffer + STRGETBYTES para la
            // longitud en bytes, y usar vio_print_buf para emitir el bloque
            // sin cortar en NUL (binary-safe; preserva multi-byte UTF-8).
            if (t.kind == PrimitiveKind::STRING) {
                ir::IrValueId v_ptr = emit_strraw(v, ex->loc.line);
                ir::IrValueId v_len = emit_strgetbytes(v, ex->loc.line);
                // Item 17: format spec en STRING.  Si align != NONE Y
                // width > 0, calcular padding = max(0, width - len) y
                // emitirlo antes (RIGHT) o despues (LEFT) del print_buf.
                // No support para kind=HEX/BIN/etc en strings (no aplica).
                const bool need_pad = (fs.align != FmtSpec::Align::NONE)
                                   && (fs.width > 0);
                ir::IrValueId v_pad = ir::IR_NO_VALUE;
                if (need_pad) {
                    // pad_count = (width > len) ? (width - len) : 0
                    // Implementado via SUB + CMOV.  Como no tengo CMOV en
                    // el IR, uso: pad = width - len; if (pad < 0) pad = 0.
                    // Cmps signed: si len > width, sub queda negativo.
                    ir::IrValueId v_width = emit_const(ir::IrType::I64,
                            (uint64_t)fs.width, ex->loc.line);
                    ir::IrValueId v_sub = fn_->new_value(ir::IrType::I64);
                    ir::IrInstr s{};
                    s.op = ir::IrOp::SUB; s.type = ir::IrType::I64;
                    s.dst = v_sub; s.operands = {v_width, v_len};
                    s.source_line = ex->loc.line;
                    fn_->append(current_block_, std::move(s));
                    // Clamp a 0: si v_sub < 0, usar 0.  Patron:
                    // cmps v_sub, 0 -> SF; setcc gt -> 1 si positivo;
                    // mul v_sub * mask = clamp.  Mas simple: usar
                    // CMP_GT v_sub, 0 -> bool; cast a i64 (0 o 1);
                    // mul v_sub * bool.
                    ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0,
                            ex->loc.line);
                    ir::IrValueId v_pos = fn_->new_value(ir::IrType::BOOL);
                    ir::IrInstr cgt{};
                    cgt.op = ir::IrOp::CMP_GT; cgt.type = ir::IrType::BOOL;
                    cgt.dst = v_pos; cgt.operands = {v_sub, v_zero};
                    cgt.source_line = ex->loc.line;
                    fn_->append(current_block_, std::move(cgt));
                    ir::IrValueId v_mask = cast_if_needed(v_pos, ir::IrType::BOOL,
                            ir::IrType::I64, ex->loc.line, /*is_explicit=*/true);
                    ir::IrValueId v_clamped = fn_->new_value(ir::IrType::I64);
                    ir::IrInstr m{};
                    m.op = ir::IrOp::MUL; m.type = ir::IrType::I64;
                    m.dst = v_clamped; m.operands = {v_sub, v_mask};
                    m.source_line = ex->loc.line;
                    fn_->append(current_block_, std::move(m));
                    v_pad = v_clamped;
                }
                // Emit padding LEADING si align==RIGHT.
                auto emit_pad_call = [&](ir::IrValueId v_count) {
                    ir::IrValueId v_fill = emit_const(ir::IrType::I64,
                            (uint64_t)fs.fill_cp, ex->loc.line);
                    out_mod_->register_native_import(lib, "vio_print_pad");
                    ir::IrInstr pc{};
                    pc.op = ir::IrOp::CALLN;
                    pc.type = ir::IrType::VOID;
                    pc.dst = ir::IR_NO_VALUE;
                    pc.func_name = lib + ":vio_print_pad";
                    pc.operands = {v_fill, v_count};
                    pc.source_line = ex->loc.line;
                    fn_->append(current_block_, std::move(pc));
                };
                if (need_pad && fs.align == FmtSpec::Align::RIGHT) {
                    emit_pad_call(v_pad);
                }
                out_mod_->register_native_import(lib, "vio_print_buf");
                ir::IrInstr ins{};
                ins.op          = ir::IrOp::CALLN;
                ins.type        = ir::IrType::VOID;
                ins.dst         = ir::IR_NO_VALUE;
                ins.func_name   = lib + ":vio_print_buf";
                ins.operands    = {v_ptr, v_len};
                ins.source_line = ex->loc.line;
                fn_->append(current_block_, std::move(ins));
                if (need_pad && fs.align == FmtSpec::Align::LEFT) {
                    emit_pad_call(v_pad);
                }
                return;
            }
            std::string func;
            ir::IrType  promote = ir::IrType::I64;
            switch (t.kind) {
                case PrimitiveKind::BOOL:
                    func = "vio_print_bool";
                    break;
                case PrimitiveKind::CHAR:
                case PrimitiveKind::U8:
                case PrimitiveKind::U16:
                case PrimitiveKind::U32:
                case PrimitiveKind::U64:
                    func = "vio_print_uint";
                    break;
                case PrimitiveKind::I8:
                case PrimitiveKind::I16:
                case PrimitiveKind::I32:
                case PrimitiveKind::I64:
                    func = "vio_print_int";
                    break;
                case PrimitiveKind::F32:
                case PrimitiveKind::F64:
                    func = "vio_print_float";
                    break;
                case PrimitiveKind::PTR:
                case PrimitiveKind::ARRAY:
                    // Imprime "0x<hex>" compacto sin ceros lider.  El
                    // mismo formato funciona tanto para punteros host
                    // como virtuales: el numero es la direccion bruta.
                    func = "vio_print_ptr";
                    break;
                case PrimitiveKind::CLASS:
                    // Para CLASS imprimimos el GcHandle como "<gc:N>".
                    // Antes del CALLN debemos convertir el host_ptr al
                    // handle via la instruccion @c gchandle.  Esto se
                    // hace abajo en el bloque de F32/F64; aqui solo
                    // marcamos el func.
                    func = "vio_print_gchandle";
                    break;
                default:
                    // Fallback: trata como puntero a cstring (no len).
                    // Por ahora no soportado; reportar error claro.
                    error_at(ex->loc,
                             "tipo de la expresion ${...} no es imprimible");
                    return;
            }
            // Para floats el ABI de vio_print_float es "uint64_t bits"
            // (IEEE 754 raw f64).  Para F32 hay que extender primero a
            // F64 (cambia el patron de bits) antes del bitcast a I64.
            // Para F64 basta el bitcast (mismo ancho).  Para enteros
            // pequenos hace SEXT/ZEXT/TRUNC normal via cast_if_needed.
            if (t.kind == PrimitiveKind::F32) {
                ir::IrValueId f64v = fn_->new_value(ir::IrType::F64);
                ir::IrInstr   ext{};
                ext.op          = ir::IrOp::F32TOF64;
                ext.type        = ir::IrType::F64;
                ext.dst         = f64v;
                ext.operands    = {v};
                ext.source_line = ex->loc.line;
                fn_->append(current_block_, std::move(ext));
                ir::IrValueId bits = fn_->new_value(ir::IrType::I64);
                ir::IrInstr   bc{};
                bc.op          = ir::IrOp::BITCAST;
                bc.type        = ir::IrType::I64;
                bc.dst         = bits;
                bc.operands    = {f64v};
                bc.source_line = ex->loc.line;
                fn_->append(current_block_, std::move(bc));
                v = bits;
            } else if (t.kind == PrimitiveKind::F64) {
                if (vt != ir::IrType::I64) {
                    ir::IrValueId bits = fn_->new_value(ir::IrType::I64);
                    ir::IrInstr   bc{};
                    bc.op          = ir::IrOp::BITCAST;
                    bc.type        = ir::IrType::I64;
                    bc.dst         = bits;
                    bc.operands    = {v};
                    bc.source_line = ex->loc.line;
                    fn_->append(current_block_, std::move(bc));
                    v = bits;
                }
            } else if (t.kind == PrimitiveKind::CLASS) {
                // El SSA value `v` es un host_ptr al objeto.  Convertir
                // a GcHandle (uint32) via la instruccion @c gchandle
                // antes de pasar al native.  vio_print_gchandle espera
                // el handle como uint64 zero-extended.
                v = emit_gc_handle_for_ptr(v, ex->loc.line);
            } else if (t.kind == PrimitiveKind::PTR
                    || t.kind == PrimitiveKind::ARRAY) {
                // Punteros pasan tal cual; el ABI uint64 de
                // vio_print_ptr ya espera la direccion bruta.  Sin
                // cast_if_needed para no emitir un mov espureo.
            } else {
                v = cast_if_needed(v, vt, promote, ex->loc.line, /*is_explicit=*/true);
            }
            out_mod_->register_native_import(lib, func);
            ir::IrInstr ins{};
            ins.op          = ir::IrOp::CALLN;
            ins.type        = ir::IrType::VOID;
            ins.dst         = ir::IR_NO_VALUE;
            ins.func_name   = lib + ":" + func;
            ins.operands    = {v};
            ins.source_line = ex->loc.line;
            fn_->append(current_block_, std::move(ins));
        };

        auto emit_print_arg = [&](ast::Expr *ex) {
            if (!ex) return;
            if (ex->kind == ast::NodeKind::StringLitExpr) {
                auto *sl = static_cast<ast::StringLitExpr *>(ex);
                if (sl->is_interpolated()) {
                    // Iteracion: parts[0] + exprs[0] + parts[1] + exprs[1]
                    // + ... + parts[N].  Cada fragmento -> 1 CALLN.  Si
                    // hay format spec por interpolacion (interp_formats[i]
                    // no vacio), se pasa al typed_value para dispatch a
                    // vio_print_fmt.
                    const size_t ne = sl->interp_exprs.size();
                    const size_t np = sl->interp_parts.size();
                    const size_t nf = sl->interp_formats.size();
                    for (size_t i = 0; i < ne; ++i) {
                        if (i < np && !sl->interp_parts[i].empty()) {
                            emit_print_string_literal(sl->interp_parts[i],
                                                      ex->loc.line);
                        }
                        const std::string &fmt = (i < nf)
                                ? sl->interp_formats[i]
                                : std::string();
                        emit_print_typed_value(sl->interp_exprs[i].get(), fmt);
                    }
                    if (np > ne) {
                        const auto &last = sl->interp_parts.back();
                        if (!last.empty()) {
                            emit_print_string_literal(last, ex->loc.line);
                        }
                    }
                    return;
                }
                emit_print_string_literal(sl->value, ex->loc.line);
                return;
            }
            emit_print_typed_value(ex, std::string());
        };

        // ----- print(arg) / echo(arg) / println(arg) -----
        // Los tres aceptan UN argumento que puede ser:
        //   - String literal (con o sin interpolacion ${expr}).
        //   - Cualquier expresion escalar (i32/i64/f64/bool/char).
        // print y echo son sinonimos; println anade '\n' al final.
        if (is_print || is_println || is_echo) {
            if (e->args.size() != 1 || !e->args[0]) {
                error_at(e->loc,
                         std::string("'") + name +
                         "' requiere exactamente un argumento");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            emit_print_arg(e->args[0].get());
            if (is_println) emit_print_newline(e->loc.line);
            out_value = ir::IR_NO_VALUE;
            return true;
        }

        // ----- flush() -----
        // Vacia el buffer global de vesta_io ahora mismo.  Util para TUIs.
        if (is_flush) {
            if (!e->args.empty()) {
                error_at(e->loc, "'flush' no acepta argumentos");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            out_mod_->register_native_import(lib, "vio_flush");
            ir::IrInstr ins{};
            ins.op          = ir::IrOp::CALLN;
            ins.type        = ir::IrType::VOID;
            ins.dst         = ir::IR_NO_VALUE;
            ins.func_name   = lib + ":vio_flush";
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            out_value = ir::IR_NO_VALUE;
            return true;
        }

        // ----- print_uint(n) / print_hex(n) / print_float(bits) /
        //       print_bool(b) / print_char(cp) / print_color(code) -----
        // Variantes explicitas por tipo: el caller fuerza el dispatch.
        // print_int sigue funcionando (rama mas abajo) por compat.
        if (is_print_uint || is_print_hex || is_print_float
            || is_print_bool || is_print_char || is_print_color
            || is_print_cstr
            || is_print_bin || is_print_oct
            || is_print_ptr || is_print_gchandle) {
            if (e->args.size() != 1) {
                error_at(e->loc,
                         std::string("'") + name +
                         "' requiere exactamente un argumento");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            ir::IrValueId v = lower_expr(e->args[0].get());
            if (v == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // Caso especial: print_gchandle recibe un objeto CLASS y debe
            // emitir la instruccion @c gchandle r_dst, r_src para
            // convertir el host_ptr al GcHandle (uint32) antes de
            // pasarlo al native como uint64 zero-extended.
            if (is_print_gchandle && e->args[0]->result_type.kind == PrimitiveKind::CLASS) {
                v = emit_gc_handle_for_ptr(v, e->loc.line);
            }
            v = cast_if_needed(v, fn_->values[v].type, ir::IrType::I64,
                               e->loc.line, /*is_explicit=*/true);
            std::string func;
            if (is_print_uint) func = "vio_print_uint";
            else if (is_print_hex) func = "vio_print_hex";
            else if (is_print_float) func = "vio_print_float";
            else if (is_print_bool) func = "vio_print_bool";
            else if (is_print_char) func = "vio_print_char";
            else if (is_print_color) func = "vio_print_color";
            else if (is_print_bin) func = "vio_print_bin";
            else if (is_print_oct) func = "vio_print_oct";
            else if (is_print_ptr) func = "vio_print_ptr";
            else if (is_print_gchandle) func = "vio_print_gchandle";
            else func                     = "vio_print_cstr"; // host_ptr -> bytes hasta NUL
            out_mod_->register_native_import(lib, func);
            ir::IrInstr ins{};
            ins.op          = ir::IrOp::CALLN;
            ins.type        = ir::IrType::VOID;
            ins.dst         = ir::IR_NO_VALUE;
            ins.func_name   = lib + ":" + func;
            ins.operands    = {v};
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // ----- print_pad(fill_cp, width) -----
        // Emite @p width copias del codepoint @p fill_cp al buffer.  Util
        // para construir alineacion manual de columnas (TUI / tablas).
        if (is_print_pad) {
            if (e->args.size() != 2) {
                error_at(e->loc,
                         "'print_pad' requiere (fill_cp, width)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            ir::IrValueId v_fill = lower_expr(e->args[0].get());
            ir::IrValueId v_w    = lower_expr(e->args[1].get());
            if (v_fill == ir::IR_NO_VALUE || v_w == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            v_fill = cast_if_needed(v_fill, fn_->values[v_fill].type,
                                    ir::IrType::I64, e->loc.line, true);
            v_w    = cast_if_needed(v_w, fn_->values[v_w].type,
                                    ir::IrType::I64, e->loc.line, true);
            out_mod_->register_native_import(lib, "vio_print_pad");
            ir::IrInstr ins{};
            ins.op          = ir::IrOp::CALLN;
            ins.type        = ir::IrType::VOID;
            ins.dst         = ir::IR_NO_VALUE;
            ins.func_name   = lib + ":vio_print_pad";
            ins.operands    = {v_fill, v_w};
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            out_value = ir::IR_NO_VALUE;
            return true;
        }

        // ----- print_int(n) -----
        // Imprime un entero con signo seguido de '\n'.  Pasa el valor
        // numerico directamente, sin VAs.
        if (is_print_int) {
            if (e->args.size() != 1) {
                error_at(e->loc, "'print_int' requiere exactamente un argumento entero");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            ir::IrValueId v = lower_expr(e->args[0].get());
            if (v == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // Forzar i64 para coincidir con la firma C de vio_print_int.
            v = cast_if_needed(v, fn_->values[v].type, ir::IrType::I64, e->loc.line);
            out_mod_->register_native_import(lib, "vio_print_int");
            ir::IrInstr ins{};
            ins.op          = ir::IrOp::CALLN;
            ins.type        = ir::IrType::VOID;
            ins.dst         = ir::IR_NO_VALUE;
            ins.func_name   = lib + ":vio_print_int";
            ins.operands    = {v};
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            out_value = ir::IR_NO_VALUE;
            return true;
        }

        // ----- fopen(path, mode) -> i64 -----
        // Args: (proc_ptr, path_addr, path_len, mode_addr, mode_len) = 5 args.
        // Devuelve uint64_t (FILE*).  Ambos args deben ser literales de string.
        if (is_fopen) {
            if (e->args.size() != 2
                || !e->args[0] || e->args[0]->kind != ast::NodeKind::StringLitExpr
                || !e->args[1] || e->args[1]->kind != ast::NodeKind::StringLitExpr) {
                error_at(e->loc,
                         "'fopen' requiere dos argumentos literales de string (path, mode)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            auto *path = static_cast<ast::StringLitExpr *>(e->args[0].get());
            auto *mode = static_cast<ast::StringLitExpr *>(e->args[1].get());
            out_mod_->register_native_import(lib, "vio_fopen");

            const ir::IrValueId v_proc               = emit_getproc(e->loc.line);
            auto                [v_path, v_path_len] = emit_string_lit(path);
            auto                [v_mode, v_mode_len] = emit_string_lit(mode);

            const ir::IrValueId dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr         ins{};
            ins.op          = ir::IrOp::CALLN;
            ins.type        = ir::IrType::I64;
            ins.dst         = dst;
            ins.func_name   = lib + ":vio_fopen";
            ins.operands    = {v_proc, v_path, v_path_len, v_mode, v_mode_len};
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            out_value = dst;
            return true;
        }

        // ----- fwrite(fp, buf) -> i64 -----
        // En Vex la firma natural es fwrite(fp, buf), pero la firma C
        // de vesta_io es vio_fwrite(proc_ptr, vm_addr, size, handle).
        // El lowering reordena: (proc, buf_addr, buf_len, fp).
        if (is_fwrite) {
            if (e->args.size() != 2
                || !e->args[1] || e->args[1]->kind != ast::NodeKind::StringLitExpr) {
                error_at(e->loc,
                         "'fwrite' requiere (FILE*, literal_string) - el buffer debe ser literal");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            ir::IrValueId v_fp = lower_expr(e->args[0].get());
            if (v_fp == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            v_fp      = cast_if_needed(v_fp, fn_->values[v_fp].type, ir::IrType::I64, e->loc.line);
            auto *buf = static_cast<ast::StringLitExpr *>(e->args[1].get());
            out_mod_->register_native_import(lib, "vio_fwrite");

            const ir::IrValueId v_proc             = emit_getproc(e->loc.line);
            auto                [v_buf, v_buf_len] = emit_string_lit(buf);

            const ir::IrValueId dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr         ins{};
            ins.op        = ir::IrOp::CALLN;
            ins.type      = ir::IrType::I64;
            ins.dst       = dst;
            ins.func_name = lib + ":vio_fwrite";
            // Orden de args segun signature C: (proc, vm_addr, size, handle).
            ins.operands    = {v_proc, v_buf, v_buf_len, v_fp};
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            out_value = dst;
            return true;
        }

        // ----- fclose(fp) -> i32 -----
        if (is_fclose) {
            if (e->args.size() != 1) {
                error_at(e->loc, "'fclose' requiere un argumento (FILE*)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            ir::IrValueId v_fp = lower_expr(e->args[0].get());
            if (v_fp == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            v_fp = cast_if_needed(v_fp, fn_->values[v_fp].type, ir::IrType::I64, e->loc.line);
            out_mod_->register_native_import(lib, "vio_fclose");

            const ir::IrValueId dst = fn_->new_value(ir::IrType::I32);
            ir::IrInstr         ins{};
            ins.op          = ir::IrOp::CALLN;
            ins.type        = ir::IrType::I32;
            ins.dst         = dst;
            ins.func_name   = lib + ":vio_fclose";
            ins.operands    = {v_fp};
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            out_value = dst;
            return true;
        }

        // ----- malloc(size) -----
        // Reserva un bloque host de `size` bytes y devuelve un void* con
        // is_host_ptr=true (LOAD/STORE consultan el flag para emitir movh).
        if (is_malloc) {
            if (e->args.size() != 1) {
                error_at(e->loc, "'malloc' requiere exactamente un argumento de tamano");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            ir::IrValueId v_size = lower_expr(e->args[0].get());
            if (v_size == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            v_size = cast_if_needed(v_size, fn_->values[v_size].type, ir::IrType::I64,
                                    e->loc.line);
            const ir::IrValueId dst = fn_->new_value(ir::IrType::PTR);
            // Marcar el resultado como puntero a memoria host: cualquier
            // LOAD/STORE posterior cuyo puntero descienda de este value
            // emitira movh en el ir_emitter.
            fn_->values[dst].is_host_ptr = true;
            ir::IrInstr ins{};
            ins.op          = ir::IrOp::RAW_ALLOC;
            ins.type        = ir::IrType::PTR;
            ins.dst         = dst;
            ins.operands    = {v_size};
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            out_value = dst;
            return true;
        }

        // ----- free(ptr) -----
        if (is_free) {
            if (e->args.size() != 1) {
                error_at(e->loc, "'free' requiere exactamente un puntero");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_ptr = lower_expr(e->args[0].get());
            if (v_ptr == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            ir::IrInstr ins{};
            ins.op          = ir::IrOp::RAW_FREE;
            ins.type        = ir::IrType::VOID;
            ins.dst         = ir::IR_NO_VALUE;
            ins.operands    = {v_ptr};
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            out_value = ir::IR_NO_VALUE;
            return true;
        }

        // ----- loadmodule(string_lit) ----- 
        // Carga dinamica de un .velb adicional.  Acepta solo string literal
        // (path al archivo en el filesystem del host).  Genera RAW_ASM que:
        //   1. Carga la direccion del path (interned en static_data) en un reg.
        //   2. Carga la longitud en bytes en otro reg.
        //   3. Emite `loadmod r_path, r_len`.  El opcode loadmod abre el file,
        //      llama a Loader::load_module_dynamic + automaticamente hace el
        //      callvm-equivalente al init_pc del modulo cargado (cuyo prologo
        //      registra clases via __module_init).  Cuando el main del modulo
        //      hace RET, el flujo continua aqui con R0 = init_pc (success) o
        //      0 (failure file not found / parse error).
        //   4. Captura R0 al SSA value.
        if (is_loadmodule) {
            if (e->args.size() != 1
                || !e->args[0]
                || e->args[0]->kind != ast::NodeKind::StringLitExpr) {
                error_at(e->loc,
                         "loadmodule: requiere un string literal con la ruta al .velb");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            auto *             slit     = static_cast<ast::StringLitExpr *>(e->args[0].get());
            const uint64_t     path_idx = intern_class_name(*out_mod_, slit->value);
            const uint32_t     path_len = static_cast<uint32_t>(slit->value.size());
            // raw_asm-elim wave 2: usar STR_LIT_ADDR + emit_const + MOD_LOAD.
            const ir::IrValueId v_path_addr = fn_->new_value(ir::IrType::PTR);
            {
                ir::IrInstr sl{};
                sl.op          = ir::IrOp::STR_LIT_ADDR;
                sl.type        = ir::IrType::PTR;
                sl.dst         = v_path_addr;
                sl.imm         = path_idx;
                sl.source_line = e->loc.line;
                fn_->append(current_block_, std::move(sl));
            }
            const ir::IrValueId v_path_len = emit_const(
                ir::IrType::I64, path_len, e->loc.line);
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr ml{};
            ml.op           = ir::IrOp::MOD_LOAD;
            ml.type         = ir::IrType::I64;
            ml.dst          = v_dst;
            ml.operands     = {v_path_addr, v_path_len};
            ml.imm          = 0;   /* loadmod */
            ml.source_line  = e->loc.line;
            ml.set_is_call_site(true);
            fn_->append(current_block_, std::move(ml));
            out_value = v_dst;
            return true;
        }

        // ----- unloadmodule(string_lit) -> i32 -----
        // Descarga modulo dinamico previamente cargado.  Mismo patron que
        // loadmodule: path interned en static_data, opcode unloadmod r_addr, r_len.
        // Devuelve 1 si descargado, 0 si no encontrado.
        if (is_unloadmodule) {
            if (e->args.size() != 1
                || !e->args[0]
                || e->args[0]->kind != ast::NodeKind::StringLitExpr) {
                error_at(e->loc,
                         "unloadmodule: requiere un string literal con la ruta al .velb");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            auto *             slit     = static_cast<ast::StringLitExpr *>(e->args[0].get());
            const uint64_t     path_idx = intern_class_name(*out_mod_, slit->value);
            const uint32_t     path_len = static_cast<uint32_t>(slit->value.size());
            // raw_asm-elim wave 2: MOD_LOAD con kind=1 (unloadmod).
            const ir::IrValueId v_path_addr = fn_->new_value(ir::IrType::PTR);
            {
                ir::IrInstr sl{};
                sl.op          = ir::IrOp::STR_LIT_ADDR;
                sl.type        = ir::IrType::PTR;
                sl.dst         = v_path_addr;
                sl.imm         = path_idx;
                sl.source_line = e->loc.line;
                fn_->append(current_block_, std::move(sl));
            }
            const ir::IrValueId v_path_len = emit_const(
                ir::IrType::I64, path_len, e->loc.line);
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I32);
            ir::IrInstr ml{};
            ml.op           = ir::IrOp::MOD_LOAD;
            ml.type         = ir::IrType::I32;
            ml.dst          = v_dst;
            ml.operands     = {v_path_addr, v_path_len};
            ml.imm          = 1;   /* unloadmod */
            ml.source_line  = e->loc.line;
            fn_->append(current_block_, std::move(ml));
            out_value = v_dst;
            return true;
        }

        // ===== Builtin dispose(xs) =====
        // Libera explicitamente una coleccion antes del exit del scope.
        // Emite CALLN al free fn correspondiente al tipo del local + reescribe
        // el binding local a 0 para que el cleanup automatico al exit pase
        // 0 al free fn (que es no-op por null-check interno).  Asi se evita
        // double-free.
        if (is_dispose) {
            if (e->args.size() != 1
                || !e->args[0]
                || e->args[0]->kind != ast::NodeKind::IdentExpr) {
                error_at(e->loc,
                         "dispose: requiere un IdentExpr local de tipo coleccion");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            auto *     id_arg = static_cast<ast::IdentExpr *>(e->args[0].get());
            const Type arg_t  = id_arg->result_type;
            if (!is_col_kind(arg_t.kind)) {
                error_at(e->loc, "dispose: el argumento no es de tipo coleccion");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ColType *ct = find_col_type(arg_t.kind);
            if (!ct) {
                error_at(e->loc, "dispose: tipo coleccion sin entry en COL_TYPES");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // 1. Lower del IdentExpr para obtener el handle actual.
            const ir::IrValueId v_handle = lower_expr(id_arg);
            if (v_handle == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // elegir variante *_free_gc cuando el local fue
            // declarado con tipo de elemento GC (ArrayList<string> etc.).
            PrimitiveKind elem_k = PrimitiveKind::VOID;
            PrimitiveKind val_k  = PrimitiveKind::VOID;
            if (arg_t.pointee) elem_k = arg_t.pointee->kind;
            if (arg_t.pointee2) val_k = arg_t.pointee2->kind;
            const bool gc_aware = (ct->native_free_fn_gc != nullptr)
                    && col_needs_gc_aware(arg_t.kind, elem_k, val_k);
            const char *fn_name = gc_aware ? ct->native_free_fn_gc : ct->native_free_fn;
            // 2. CALLN al free fn (idempotente por null-check del plugin).
            out_mod_->register_native_import(COL_NATIVE_LIB, fn_name);
            std::vector<ir::IrValueId> args;
            if (gc_aware) {
                args.reserve(2);
                args.push_back(emit_getproc(e->loc.line));
            } else {
                args.reserve(1);
            }
            args.push_back(v_handle);
            ir::IrInstr ins{};
            ins.op          = ir::IrOp::CALLN;
            ins.type        = ir::IrType::VOID;
            ins.dst         = ir::IR_NO_VALUE;
            ins.func_name   = std::string(COL_NATIVE_LIB) + ":" + fn_name;
            ins.operands    = std::move(args);
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            // 3. Reescribir el binding local a 0 (handle invalido).  El
            // cleanup al exit del scope vera este 0 (via refresh_name) y
            // sera no-op.  Evita double-free.
            const ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0, e->loc.line);
            write_local(id_arg->name, v_zero, ir::IrType::I64, e->loc.line);
            out_value = ir::IR_NO_VALUE;
            return true;
        }

        // ===== Constructor de coleccion primitiva =====
        // arraylist(N) -> CALLN vcol_alist_new(N), retorno i64 handle.
        // Para tipos sin default_cap (TreeMap/TreeSet) la firma del builtin
        // no toma argumentos; emitimos CALLN con argc=0.
        if (is_col_ctor) {
            std::vector<ir::IrValueId> arg_ids;
            for (auto &a: e->args) {
                arg_ids.push_back(lower_expr(a.get()));
            }
            // Si el ctor tiene default_cap > 0 y el usuario llamo sin args,
            // sintetizamos la cap por defecto.  El type checker valida que
            // siempre haya 1 arg para los ctors con default_cap; pero por
            // seguridad emitimos default cuando el array de args esta vacio.
            if (arg_ids.empty() && col_ctor->default_cap > 0) {
                arg_ids.push_back(emit_const(ir::IrType::I64,
                                             static_cast<uint64_t>(col_ctor->default_cap), e->loc.line));
            }
            out_mod_->register_native_import(COL_NATIVE_LIB, col_ctor->native_new_fn);
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr         ins{};
            ins.op          = ir::IrOp::CALLN;
            ins.type        = ir::IrType::I64;
            ins.dst         = v_dst;
            ins.func_name   = std::string(COL_NATIVE_LIB) + ":" + col_ctor->native_new_fn;
            ins.operands    = std::move(arg_ids);
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            out_value = v_dst;
            return true;
        }

        // ----- ffi_open(string lit) -----
        // Carga DLL en runtime via opcode dlopen (extended 0x62).  Path
        // siempre como string literal (interned en static_data).  Devuelve
        // handle host como i64.
        if (is_ffi_open) {
            if (e->args.size() != 1
                || !e->args[0]
                || e->args[0]->kind != ast::NodeKind::StringLitExpr) {
                error_at(e->loc,
                         "ffi_open: requiere un string literal con el nombre/path de la DLL");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            auto *             slit     = static_cast<ast::StringLitExpr *>(e->args[0].get());
            const uint64_t     path_idx = intern_class_name(*out_mod_, slit->value);
            const uint32_t     path_len = static_cast<uint32_t>(slit->value.size());
            // raw_asm-elim wave 2: DLOPEN IR op.
            const ir::IrValueId v_path_addr = fn_->new_value(ir::IrType::PTR);
            {
                ir::IrInstr sl{};
                sl.op          = ir::IrOp::STR_LIT_ADDR;
                sl.type        = ir::IrType::PTR;
                sl.dst         = v_path_addr;
                sl.imm         = path_idx;
                sl.source_line = e->loc.line;
                fn_->append(current_block_, std::move(sl));
            }
            const ir::IrValueId v_path_len = emit_const(
                ir::IrType::I64, path_len, e->loc.line);
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr dl{};
            dl.op           = ir::IrOp::DLOPEN;
            dl.type         = ir::IrType::I64;
            dl.dst          = v_dst;
            dl.operands     = {v_path_addr, v_path_len};
            dl.source_line  = e->loc.line;
            fn_->append(current_block_, std::move(dl));
            out_value = v_dst;
            return true;
        }

        // ----- ffi_sym(handle, string lit) -----
        // Resuelve simbolo en una DLL cargada.  El handle viene de un SSA
        // value (resultado de ffi_open o expression i64); el name es
        // string literal (interned en static_data).  Devuelve fn_addr i64.
        if (is_ffi_sym) {
            if (e->args.size() != 2
                || !e->args[0]
                || !e->args[1]
                || e->args[1]->kind != ast::NodeKind::StringLitExpr) {
                error_at(e->loc,
                         "ffi_sym: requiere (i64 handle, string lit name)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_handle = lower_expr(e->args[0].get());
            if (v_handle == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            auto *         slit     = static_cast<ast::StringLitExpr *>(e->args[1].get());
            const uint64_t name_idx = intern_class_name(*out_mod_, slit->value);
            const uint32_t name_len = static_cast<uint32_t>(slit->value.size());
            // raw_asm-elim wave 2: DLSYM IR op.
            const ir::IrValueId v_name_addr = fn_->new_value(ir::IrType::PTR);
            {
                ir::IrInstr sl{};
                sl.op          = ir::IrOp::STR_LIT_ADDR;
                sl.type        = ir::IrType::PTR;
                sl.dst         = v_name_addr;
                sl.imm         = name_idx;
                sl.source_line = e->loc.line;
                fn_->append(current_block_, std::move(sl));
            }
            const ir::IrValueId v_name_len = emit_const(
                ir::IrType::I64, name_len, e->loc.line);
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr ds{};
            ds.op           = ir::IrOp::DLSYM;
            ds.type         = ir::IrType::I64;
            ds.dst          = v_dst;
            ds.operands     = {v_handle, v_name_addr, v_name_len};
            ds.source_line  = e->loc.line;
            fn_->append(current_block_, std::move(ds));
            out_value = v_dst;
            return true;
        }

        // ----- ffi_call(fn, ...args) -----  (variadic 0-12 args)
        // Invoca funcion nativa via puntero (resuelto por ffi_sym/dlsym o
        // pasado como handle).  Calling convention espejo a CALLN estatico:
        // argc en R15, args en R01..R12, retorno en R00.
        //
        // Implementacion: emitir IrInstr CALLN con func_name="__callni__:"
        // y operands=[fn, args...].  El emitter detecta el prefix y emite
        // la secuencia completa (push regs vivos + parallel-move args ->
        // R1..RN + mov r15, N + callni reg_fn + capturar R0 + pop regs).
        // Reusa toda la maquinaria de CALLN para mantener una sola ruta.
        if (is_ffi_call) {
            if (e->args.empty()) {
                error_at(e->loc, "ffi_call: requiere al menos el puntero a funcion");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            if (e->args.size() > 13) {
                error_at(e->loc, "ffi_call: maximo 12 args ademas del puntero");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            std::vector<ir::IrValueId> arg_ids;
            arg_ids.reserve(e->args.size());
            for (auto &a: e->args) {
                arg_ids.push_back(lower_expr(a.get()));
            }
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr         ins{};
            ins.op          = ir::IrOp::CALLN;
            ins.type        = ir::IrType::I64;
            ins.dst         = v_dst;
            ins.func_name   = "__callni__:"; // prefix detectado en emitter
            ins.operands    = std::move(arg_ids);
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            out_value = v_dst;
            return true;
        }

        // ----- panic("msg") -----
        // dispara FatalError(USER_ABORT, msg).  Capturable con
        // try/catch FatalError; si no hay handler, mata el proceso.
        // Acepta string literal (interna en static_data + emite panic
        // directo) o expresion string-typed (no soportado todavia).
        // Math builtins -> CALLN a vesta_math.dll.  ABI: bits IEEE 754
        // como uint64_t en r1..rN, retorno (bits) en r0.  Para funciones
        // con tipo de retorno float (sqrt, pow, sin, ...), el callee
        // devuelve los bits f64.  Para funciones que devuelven int (abs,
        // imin, imax, clamp), el valor se devuelve como i64 directo.
        if (is_any_math) {
            const std::string lib_math = "stdlib/native/math/vesta_math";

            // Math-IR-promote (raw_asm-elim wave 4): para builtins con IR
            // op nativa (FSQRT/FABS/FMIN/FMAX/FFLOOR/FCEIL/FROUND/FTRUNC),
            // emitir el IR op directamente.  Beneficios:
            //   (a) Constant folding: sqrt(2.0) -> literal compile-time.
            //   (b) Selector JIT puede emitir sqrtsd/andpd/roundsd nativos
            //       (~4 ciclos) en lugar de CALLN (~50ns).
            //   (c) Cross-target: cuando llegue ARM Selector, emitira fsqrt.d
            //       sin tocar el IR.
            // Fallback CALLN sigue activo para transcendentales (log/sin/cos/
            // tan/pow/exp): libm los implementa mejor que cualquier inline.
            auto emit_float_irop = [&](ir::IrOp op, size_t nargs) -> bool {
                if (e->args.size() != nargs) {
                    error_at(e->loc, std::string("'") + name + "': "
                             + std::to_string(nargs) + " arg(s)");
                    out_value = ir::IR_NO_VALUE;
                    return true;
                }
                std::vector<ir::IrValueId> ops;
                ops.reserve(nargs);
                for (auto &a: e->args) {
                    ir::IrValueId v = lower_expr(a.get());
                    if (v == ir::IR_NO_VALUE) {
                        out_value = ir::IR_NO_VALUE;
                        return true;
                    }
                    // Promover f32 a f64 si hace falta (IR ops trabajan en f64).
                    const ir::IrType vt = fn_->values[v].type;
                    if (vt == ir::IrType::F32) {
                        ir::IrValueId f64v = fn_->new_value(ir::IrType::F64);
                        ir::IrInstr   ext{};
                        ext.op          = ir::IrOp::F32TOF64;
                        ext.type        = ir::IrType::F64;
                        ext.dst         = f64v;
                        ext.operands    = {v};
                        ext.source_line = e->loc.line;
                        fn_->append(current_block_, std::move(ext));
                        v = f64v;
                    } else if (vt != ir::IrType::F64) {
                        // Si el arg no es float, lo dejamos como esta (el
                        // emitter trata bits como i64 o el caller hizo cast).
                    }
                    ops.push_back(v);
                }
                const ir::IrValueId v_dst = fn_->new_value(ir::IrType::F64);
                ir::IrInstr in{};
                in.op          = op;
                in.type        = ir::IrType::F64;
                in.dst         = v_dst;
                in.operands    = std::move(ops);
                in.source_line = e->loc.line;
                fn_->append(current_block_, std::move(in));
                out_value = v_dst;
                return true;
            };
            // Math-IR-promote: despachar a IR op directamente para los que
            // tienen instr hardware nativa (target-agnostico).  Beneficios:
            //   (a) FSQRT/FABS/FNEG bajan a bytecode VM nativo (fsqrt/fabs/
            //       fneg, ~5ns) en lugar de CALLN (~50ns).
            //   (b) Para FMIN/FMAX/FFLOOR/FCEIL/FROUND/FTRUNC el bytecode
            //       todavia no tiene opcodes; el IR emitter (ir_emitter.cpp)
            //       tiene un pre-pase que los convierte a CALLN equivalente.
            //   (c) El Selector JIT (futuro) emite sqrtsd/andpd/minsd/roundsd
            //       nativos sin tocar el frontend.
            //   (d) Constant folding (cuando se anyada) funciona uniforme.
            if (is_math_sqrt)  return emit_float_irop(ir::IrOp::FSQRT,  1);
            if (is_math_fabs)  return emit_float_irop(ir::IrOp::FABS,   1);
            if (is_math_fmin)  return emit_float_irop(ir::IrOp::FMIN,   2);
            if (is_math_fmax)  return emit_float_irop(ir::IrOp::FMAX,   2);
            if (is_math_floor) return emit_float_irop(ir::IrOp::FFLOOR, 1);
            if (is_math_ceil)  return emit_float_irop(ir::IrOp::FCEIL,  1);
            if (is_math_round) return emit_float_irop(ir::IrOp::FROUND, 1);
            if (is_math_trunc) return emit_float_irop(ir::IrOp::FTRUNC, 1);

            // Math-IR-promote v2.2a: bit ops + int ops adicionales.
            // Producen i64 (no float).  Lambda paralela a emit_float_irop.
            auto emit_int_irop = [&](ir::IrOp op, size_t nargs) -> bool {
                if (e->args.size() != nargs) {
                    error_at(e->loc, std::string("'") + name + "': "
                             + std::to_string(nargs) + " arg(s)");
                    out_value = ir::IR_NO_VALUE;
                    return true;
                }
                std::vector<ir::IrValueId> ops;
                ops.reserve(nargs);
                for (auto &a : e->args) {
                    ir::IrValueId v = lower_expr(a.get());
                    if (v == ir::IR_NO_VALUE) {
                        out_value = ir::IR_NO_VALUE;
                        return true;
                    }
                    ops.push_back(cast_if_needed(v, fn_->values[v].type,
                                                  ir::IrType::I64, e->loc.line));
                }
                const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
                ir::IrInstr in{};
                in.op          = op;
                in.type        = ir::IrType::I64;
                in.dst         = v_dst;
                in.operands    = std::move(ops);
                in.source_line = e->loc.line;
                fn_->append(current_block_, std::move(in));
                out_value = v_dst;
                return true;
            };
            if (is_math_iminu)    return emit_int_irop(ir::IrOp::IMINU,    2);
            if (is_math_imaxu)    return emit_int_irop(ir::IrOp::IMAXU,    2);
            if (is_math_ilog2)    return emit_int_irop(ir::IrOp::ILOG2,    1);
            if (is_math_popcount) return emit_int_irop(ir::IrOp::POPCNT,   1);
            if (is_math_clz)      return emit_int_irop(ir::IrOp::CLZ,      1);
            if (is_math_ctz)      return emit_int_irop(ir::IrOp::CTZ,      1);
            if (is_math_bswap)    return emit_int_irop(ir::IrOp::BYTESWAP, 1);
            if (is_math_rotl)     return emit_int_irop(ir::IrOp::ROTL,     2);
            if (is_math_rotr)     return emit_int_irop(ir::IrOp::ROTR,     2);
            // Promocion IMIN/IMAX/IABS a IR op tambien (los wires antiguos
            // CALLN siguen activos abajo pero el pre-pase los re-wirea).
            if (is_math_abs)  return emit_int_irop(ir::IrOp::IABS, 1);
            if (is_math_imin) return emit_int_irop(ir::IrOp::IMIN, 2);
            if (is_math_imax) return emit_int_irop(ir::IrOp::IMAX, 2);

            // Camino CALLN tradicional para transcendentales (log/exp/sin/cos/tan/pow)
            // e ints (abs/imin/imax/clamp).  libm los implementa mejor que cualquier
            // inline que podamos emitir.
            std::string       func_name;
            size_t            expected_args = 1;
            ir::IrType        ret_ir        = ir::IrType::F64;
            ir::IrType        arg_ir        = ir::IrType::I64; // por defecto pasa bits f64 como i64
            bool              dst_is_float  = true;
            if (is_math_pow) {
                func_name     = "vmath_pow";
                expected_args = 2;
            } else if (is_math_log) {
                func_name = "vmath_log";
            } else if (is_math_log2) {
                func_name = "vmath_log2";
            } else if (is_math_log10) {
                func_name = "vmath_log10";
            } else if (is_math_sin) {
                func_name = "vmath_sin";
            } else if (is_math_cos) {
                func_name = "vmath_cos";
            } else if (is_math_tan) {
                func_name = "vmath_tan";
            } else if (is_math_abs) {
                func_name    = "vmath_abs";
                ret_ir       = ir::IrType::I64;
                dst_is_float = false;
            } else if (is_math_imin) {
                func_name     = "vmath_min";
                expected_args = 2;
                ret_ir        = ir::IrType::I64;
                dst_is_float  = false;
            } else if (is_math_imax) {
                func_name     = "vmath_max";
                expected_args = 2;
                ret_ir        = ir::IrType::I64;
                dst_is_float  = false;
            } else if (is_math_clamp) {
                func_name     = "vmath_clamp";
                expected_args = 3;
                ret_ir        = ir::IrType::I64;
                dst_is_float  = false;
            }
            if (e->args.size() != expected_args) {
                error_at(e->loc, std::string("'") + name + "': "
                         + std::to_string(expected_args) + " arg(s)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            std::vector<ir::IrValueId> ops;
            ops.reserve(expected_args);
            for (auto &a: e->args) {
                ir::IrValueId v = lower_expr(a.get());
                if (v == ir::IR_NO_VALUE) {
                    out_value = ir::IR_NO_VALUE;
                    return true;
                }
                // ABI nativo: vmath_* recibe bits IEEE 754 como uint64_t.
                // Para floats el value YA esta en GP como bits (lower_expr
                // de un f64 produce un i64 en GP); pasamos tal cual via
                // BITCAST (NO cast_if_needed/FTOI, que convertiria VALOR).
                // Para int builtins (abs/imin/imax/clamp) un cast normal
                // i32->i64 es lo correcto.
                const ir::IrType vt = fn_->values[v].type;
                if ((vt == ir::IrType::F64 || vt == ir::IrType::F32)
                    && arg_ir == ir::IrType::I64) {
                    if (vt == ir::IrType::F32) {
                        ir::IrValueId f64v = fn_->new_value(ir::IrType::F64);
                        ir::IrInstr   ext{};
                        ext.op          = ir::IrOp::F32TOF64;
                        ext.type        = ir::IrType::F64;
                        ext.dst         = f64v;
                        ext.operands    = {v};
                        ext.source_line = e->loc.line;
                        fn_->append(current_block_, std::move(ext));
                        v = f64v;
                    }
                    ir::IrValueId bits = fn_->new_value(ir::IrType::I64);
                    ir::IrInstr   bc{};
                    bc.op          = ir::IrOp::BITCAST;
                    bc.type        = ir::IrType::I64;
                    bc.dst         = bits;
                    bc.operands    = {v};
                    bc.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(bc));
                    v = bits;
                } else {
                    v = cast_if_needed(v, vt, arg_ir, e->loc.line);
                }
                ops.push_back(v);
            }
            out_mod_->register_native_import(lib_math, func_name);
            const ir::IrValueId dst = fn_->new_value(ret_ir);
            ir::IrInstr         ins{};
            ins.op          = ir::IrOp::CALLN;
            ins.type        = ret_ir;
            ins.dst         = dst;
            ins.func_name   = lib_math + ":" + func_name;
            ins.operands    = std::move(ops);
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            (void) dst_is_float;
            out_value = dst;
            return true;
        }

        if (is_panic) {
            if (e->args.size() != 1
                || !e->args[0]
                || e->args[0]->kind != ast::NodeKind::StringLitExpr) {
                error_at(e->loc,
                         "panic: requiere un string literal con el mensaje");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            auto *             slit    = static_cast<ast::StringLitExpr *>(e->args[0].get());
            const uint64_t     msg_idx = intern_class_name(*out_mod_, slit->value);
            const uint32_t     msg_len = static_cast<uint32_t>(slit->value.size());
            // Sprint 6.D: panic via IR op puro (LABEL_ADDR + CONST + PANIC).
            const ir::IrValueId v_addr = emit_label_addr(
                "s_" + std::to_string(msg_idx), e->loc.line);
            const ir::IrValueId v_len = emit_const(ir::IrType::I64,
                                                    static_cast<uint64_t>(msg_len),
                                                    e->loc.line);
            ir::IrInstr p{};
            p.op          = ir::IrOp::PANIC;
            p.type        = ir::IrType::VOID;
            p.dst         = ir::IR_NO_VALUE;
            p.operands    = {v_addr, v_len};
            p.source_line = e->loc.line;
            fn_->append(current_block_, std::move(p));
            block_terminated_ = true; // panic es terminador (no retorna salvo via catch)
            out_value = ir::IR_NO_VALUE;
            return true;
        }

        /* Phase MC.15C: builtins comptime aliasados a codigo runtime.
         * Cuando aparecen en cuerpos de @Macro lowereados a IR, se
         * compilan a una secuencia de bytecode equivalente al AST eval. */

        if (is_to_str) {
            /* to_str(int) -> string.  Reusa el helper
             * stringify_primitive_via_native con vio_int_to_vmbuf. */
            if (e->args.size() != 1) {
                error_at(e->loc, "to_str: se esperaba 1 argumento");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_val = lower_expr(e->args[0].get());
            if (v_val == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            out_value = stringify_primitive_via_native(
                v_val, "vio_int_to_vmbuf", e->loc.line);
            return true;
        }

        if (is_chr_b) {
            /* chr(codepoint) -> string.  Reusa vio_char_to_vmbuf
             * (codepoint -> UTF-8 bytes -> STRMAKE). */
            if (e->args.size() != 1) {
                error_at(e->loc, "chr: se esperaba 1 argumento (codepoint)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_cp = lower_expr(e->args[0].get());
            if (v_cp == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            out_value = stringify_primitive_via_native(
                v_cp, "vio_char_to_vmbuf", e->loc.line);
            return true;
        }

        if (is_ord_b) {
            /* ord(s) -> u64.  Devuelve el primer codepoint del string.
             * Fast path ASCII: emit strraw + LOAD u8 (host).  Para
             * multi-byte UTF-8 retorna solo el primer byte (lead byte);
             * el caller puede decodear si necesita el codepoint real. */
            if (e->args.size() != 1) {
                error_at(e->loc, "ord: se esperaba 1 argumento (string)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_str = lower_expr(e->args[0].get());
            if (v_str == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            /* strraw r_raw, r_str  -> host_ptr a bytes */
            ir::IrValueId v_raw = emit_strraw(v_str, e->loc.line);
            /* LOAD.u8 al primer byte (host).  El IR LOAD con is_host_ptr
             * en la fuente emite `movh` automaticamente. */
            ir::IrValueId v_byte = fn_->new_value(ir::IrType::U64);
            {
                ir::IrInstr ld{};
                ld.op          = ir::IrOp::LOAD;
                ld.type        = ir::IrType::U8;
                ld.dst         = v_byte;
                ld.operands    = {v_raw};
                ld.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ld));
            }
            out_value = v_byte;
            return true;
        }

        if (is_substr_b) {
            /* substr(s, start, len) -> string.  Empaqueta start+len en
             * un u64 (hi<<32 | lo) y emite strslice. */
            if (e->args.size() != 3) {
                error_at(e->loc, "substr: se esperaba 3 argumentos (string, start, len)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_str   = lower_expr(e->args[0].get());
            const ir::IrValueId v_start = lower_expr(e->args[1].get());
            const ir::IrValueId v_len   = lower_expr(e->args[2].get());
            if (v_str == ir::IR_NO_VALUE || v_start == ir::IR_NO_VALUE
             || v_len == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            /* Pack: r_range = (start << 32) | len */
            ir::IrValueId v_shifted = fn_->new_value(ir::IrType::U64);
            {
                ir::IrInstr sh{};
                sh.op          = ir::IrOp::SHL;
                sh.type        = ir::IrType::U64;
                sh.dst         = v_shifted;
                sh.operands    = {v_start, emit_const(ir::IrType::U64, 32, e->loc.line)};
                sh.source_line = e->loc.line;
                fn_->append(current_block_, std::move(sh));
            }
            ir::IrValueId v_range = fn_->new_value(ir::IrType::U64);
            {
                ir::IrInstr orop{};
                orop.op          = ir::IrOp::OR;
                orop.type        = ir::IrType::U64;
                orop.dst         = v_range;
                orop.operands    = {v_shifted, v_len};
                orop.source_line = e->loc.line;
                fn_->append(current_block_, std::move(orop));
            }
            ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
            {
                ir::IrInstr sl{};
                sl.op           = ir::IrOp::STRSLICE;
                sl.type         = ir::IrType::I64;
                sl.dst          = v_dst;
                sl.operands     = {v_str, v_range};
                sl.source_line  = e->loc.line;
                sl.set_is_call_site(true);
                fn_->append(current_block_, std::move(sl));
            }
            out_value = v_dst;
            return true;
        }

        if (is_static_assert_b) {
            /* Phase MC.20: `static_assert(cond, msg)` se baja a CALLN
             * a la virtual lib `vesta_comptime:static_assert`.  El fn
             * recibe (cond_i64, msg_cstr) y emite diagnostic error si
             * cond es 0.  Cuando el macro corre via VM en compile time,
             * la check se ejecuta tambien en compile time -- mismo
             * resultado que el AST eval inline. */
            if (e->args.size() != 2) {
                error_at(e->loc, "static_assert: se esperaba 2 args (cond, msg)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_cond = lower_expr(e->args[0].get());
            if (v_cond == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            /* msg: solo soportamos string literal no interpolado.  Lo
             * pasamos como host_ptr al buffer estable de static_data
             * (NUL-terminated por construccion). */
            const ast::Expr *msg_e = e->args[1].get();
            if (!msg_e || msg_e->kind != ast::NodeKind::StringLitExpr) {
                error_at(e->loc, "static_assert: el msg debe ser string literal");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const auto *slit = static_cast<const ast::StringLitExpr *>(msg_e);
            if (slit->is_interpolated()) {
                error_at(e->loc, "static_assert: msg no puede ser interpolado");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            /* Intern el msg como bytes + NUL terminator (asi c_str
             * funciona sobre el host_ptr exportado por STR_LIT_ADDR). */
            std::vector<uint8_t> bytes(slit->value.begin(), slit->value.end());
            bytes.push_back('\0');
            const uint64_t idx = out_mod_->intern_static_data(std::move(bytes));
            ir::IrValueId v_msg = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_msg].is_host_ptr = true;
            {
                ir::IrInstr is{};
                is.op          = ir::IrOp::STR_LIT_ADDR;
                is.type        = ir::IrType::PTR;
                is.dst         = v_msg;
                is.imm         = idx;
                is.source_line = e->loc.line;
                fn_->append(current_block_, std::move(is));
            }
            /* CALLN @Method("vesta_comptime:static_assert") con (cond, msg). */
            out_mod_->register_native_import("vesta_comptime", "static_assert");
            ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr cl{};
            cl.op          = ir::IrOp::CALLN;
            cl.type        = ir::IrType::I64;
            cl.dst         = v_dst;
            cl.func_name   = "vesta_comptime:static_assert";
            cl.operands    = {v_cond, v_msg};
            cl.source_line = e->loc.line;
            fn_->append(current_block_, std::move(cl));
            out_value = v_dst;
            return true;
        }

        if (is_gensym_b) {
            /* gensym() -> u64.  Counter incrementado en cada call.
             * Implementado via CALLN a vio_gensym() en el plugin
             * vesta_io que mantiene un counter estatico. */
            if (!e->args.empty()) {
                error_at(e->loc, "gensym: no acepta argumentos");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            out_mod_->register_native_import(
                std::string("stdlib/native/io/vesta_io"), "vio_gensym");
            ir::IrValueId v_dst = fn_->new_value(ir::IrType::U64);
            ir::IrInstr cl{};
            cl.op          = ir::IrOp::CALLN;
            cl.type        = ir::IrType::U64;
            cl.dst         = v_dst;
            cl.func_name   = "stdlib/native/io/vesta_io:vio_gensym";
            cl.operands    = {};
            cl.source_line = e->loc.line;
            fn_->append(current_block_, std::move(cl));
            out_value = v_dst;
            return true;
        }

        if (is_repeat_b || is_replace_b || is_contains_b
            || is_str_starts_with || is_str_ends_with || is_str_index_of) {
            /* Phase MC.15D: builtins de string que requieren acceso a
             * los bytes RAW de StringObjects (via STRRAW) y un buffer
             * destino en vm_mem.  Layout comun:
             *   1. Resolver SSA values de cada arg (string -> handle).
             *      AUTO-PROMOCION: literals string como `"{a}"` no son
             *      StringObjects; los promovemos via STRMAKE antes de
             *      hacer STRRAW.  Sin esto, STRRAW recibe un raw ptr a
             *      static_data y devuelve garbage.  Mismo patron que
             *      str_concat / str_equals.
             *   2. Para cada string arg: emitir STRRAW + STRGETBYTES
             *      para obtener host_ptr + length.  Pasar host_ptr como
             *      vm_addr al native (que internamente lo trata como
             *      direccion VM via vm_read_bytes).
             *
             * NOTA: STRRAW devuelve host_ptr, no vm_addr.  Pero los
             * helpers usan `vm_read_bytes` que toma direcciones VM.
             * Para evitar confusion, copiamos cada string a un buffer
             * VM via ALLOCA + copia byte-por-byte... mucho overhead.
             *
             * Alternativa: el helper acepta DIRECTAMENTE el host_ptr
             * (uint64) y lo dereferencea como tal.  Re-disenamos los
             * natives para tomar host_ptr en lugar de vm_addr.  Para
             * mantener consistencia con vio_*_to_vmbuf, los repeat/
             * replace todavia usan vm_addr para el DESTINO; el caller
             * debe pasar un buffer ALLOCA fresco.
             *
             * Plan v1 simplificado: TODOS los args string se materializan
             * a buffer VM via ALLOCA + write.  Costoso para strings
             * grandes pero correcto.  Optimizable despues. */

            // Helper para auto-promote string literals a StringObjects
            // antes de aplicar strraw.  Mismo patron que en str_concat/equals.
            auto coerce_str_arg = [&](ast::Expr *ex) -> ir::IrValueId {
                if (ex && ex->kind == ast::NodeKind::StringLitExpr) {
                    auto *sl = static_cast<ast::StringLitExpr *>(ex);
                    return lower_string_literal_to_string_object(sl);
                }
                return lower_expr(ex);
            };

            auto materialize_str_to_vmbuf =
                [&](ir::IrValueId v_str, int ln) ->
                std::pair<ir::IrValueId, ir::IrValueId> {
                /* Returns (vm_addr, byte_len).  Aloca buffer VM,
                 * llama STRRAW + STRGETBYTES, copia bytes a buffer VM. */
                ir::IrValueId v_raw      = emit_strraw(v_str, ln);
                ir::IrValueId v_byte_len = emit_strgetbytes(v_str, ln);
                /* v_raw es host_ptr -- los helpers nativos lo aceptan
                 * directamente via `(void *)(uint64_t)host_ptr` y
                 * leen con memcpy.  Pero g_api->vm_read_bytes toma
                 * VM address, no host_ptr.  Para usar vm_read_bytes
                 * necesitamos un VM address.
                 *
                 * Workaround: ya que los helpers necesitan VM address,
                 * vamos a alocar un buffer en VM (ALLOCA) y copiar via
                 * un nuevo intrinsic 'memcpyh_to_v' que copia desde
                 * host_ptr a vm_mem.  PERO ese intrinsic no existe.
                 *
                 * Solucion simple: cambiar los helpers nativos para
                 * que tomen host_ptr.  Asi pasamos v_raw directo. */
                return {v_raw, v_byte_len};
            };

            if (is_repeat_b) {
                if (e->args.size() != 2) {
                    error_at(e->loc, "repeat: se esperaba 2 argumentos (string, n)");
                    out_value = ir::IR_NO_VALUE;
                    return true;
                }
                const ir::IrValueId v_str = coerce_str_arg(e->args[0].get());
                const ir::IrValueId v_n   = lower_expr(e->args[1].get());
                if (v_str == ir::IR_NO_VALUE || v_n == ir::IR_NO_VALUE) {
                    out_value = ir::IR_NO_VALUE;
                    return true;
                }
                auto [v_src_addr, v_src_len] =
                    materialize_str_to_vmbuf(v_str, e->loc.line);
                /* Aloca buffer destino (max 16 MB).  Tamano runtime no
                 * conocido en compile-time; reservamos ALLOCA grande
                 * (64 KB) como cap razonable.  El helper devuelve la
                 * longitud escrita y abortara con 0 si excede 16 MB. */
                ir::IrValueId v_dst_buf = fn_->new_value(ir::IrType::PTR);
                {
                    ir::IrInstr al{};
                    al.op          = ir::IrOp::ALLOCA;
                    al.type        = ir::IrType::I8;
                    al.dst         = v_dst_buf;
                    al.imm         = 65536;
                    al.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(al));
                }
                const ir::IrValueId v_proc = emit_getproc(e->loc.line);
                out_mod_->register_native_import(
                    "stdlib/native/io/vesta_io", "vstr_repeat_to_vmbuf");
                ir::IrValueId v_len = fn_->new_value(ir::IrType::U64);
                {
                    ir::IrInstr cl{};
                    cl.op          = ir::IrOp::CALLN;
                    cl.type        = ir::IrType::U64;
                    cl.dst         = v_len;
                    cl.func_name   = "stdlib/native/io/vesta_io:vstr_repeat_to_vmbuf";
                    cl.operands    = {v_proc, v_dst_buf, v_src_addr, v_src_len, v_n};
                    cl.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(cl));
                }
                /* STRMAKE desde el buffer dst. */
                ir::IrValueId v_h = emit_strmake(v_dst_buf, v_len, e->loc.line);
                out_value = v_h;
                return true;
            }

            if (is_contains_b) {
                if (e->args.size() != 2) {
                    error_at(e->loc, "contains: se esperaba 2 argumentos (string, substring)");
                    out_value = ir::IR_NO_VALUE;
                    return true;
                }
                const ir::IrValueId v_hay    = coerce_str_arg(e->args[0].get());
                const ir::IrValueId v_needle = coerce_str_arg(e->args[1].get());
                if (v_hay == ir::IR_NO_VALUE || v_needle == ir::IR_NO_VALUE) {
                    out_value = ir::IR_NO_VALUE;
                    return true;
                }
                auto [v_h_addr, v_h_len] =
                    materialize_str_to_vmbuf(v_hay, e->loc.line);
                auto [v_n_addr, v_n_len] =
                    materialize_str_to_vmbuf(v_needle, e->loc.line);
                const ir::IrValueId v_proc = emit_getproc(e->loc.line);
                out_mod_->register_native_import(
                    "stdlib/native/io/vesta_io", "vstr_contains");
                ir::IrValueId v_dst = fn_->new_value(ir::IrType::BOOL);
                {
                    ir::IrInstr cl{};
                    cl.op          = ir::IrOp::CALLN;
                    cl.type        = ir::IrType::BOOL;
                    cl.dst         = v_dst;
                    cl.func_name   = "stdlib/native/io/vesta_io:vstr_contains";
                    cl.operands    = {v_proc, v_h_addr, v_h_len, v_n_addr, v_n_len};
                    cl.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(cl));
                }
                out_value = v_dst;
                return true;
            }

            if (is_replace_b) {
                if (e->args.size() != 3) {
                    error_at(e->loc, "replace: se esperaba 3 argumentos (string, from, to)");
                    out_value = ir::IR_NO_VALUE;
                    return true;
                }
                // Auto-promote string literals a StringObjects.  Sin esto,
                // un literal como `"{a}"` se pasa como raw static_data ptr
                // a STRRAW que lo trata como GcHandle invalido -> garbage.
                const ir::IrValueId v_src  = coerce_str_arg(e->args[0].get());
                const ir::IrValueId v_from = coerce_str_arg(e->args[1].get());
                const ir::IrValueId v_to   = coerce_str_arg(e->args[2].get());
                if (v_src == ir::IR_NO_VALUE || v_from == ir::IR_NO_VALUE
                 || v_to == ir::IR_NO_VALUE) {
                    out_value = ir::IR_NO_VALUE;
                    return true;
                }
                auto [v_src_addr,  v_src_len ] =
                    materialize_str_to_vmbuf(v_src,  e->loc.line);
                auto [v_from_addr, v_from_len] =
                    materialize_str_to_vmbuf(v_from, e->loc.line);
                auto [v_to_addr,   v_to_len  ] =
                    materialize_str_to_vmbuf(v_to,   e->loc.line);
                /* Buffer destino (64 KB ALLOCA). */
                ir::IrValueId v_dst_buf = fn_->new_value(ir::IrType::PTR);
                {
                    ir::IrInstr al{};
                    al.op          = ir::IrOp::ALLOCA;
                    al.type        = ir::IrType::I8;
                    al.dst         = v_dst_buf;
                    al.imm         = 65536;
                    al.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(al));
                }
                const ir::IrValueId v_proc = emit_getproc(e->loc.line);
                out_mod_->register_native_import(
                    "stdlib/native/io/vesta_io", "vstr_replace_to_vmbuf");
                ir::IrValueId v_len = fn_->new_value(ir::IrType::U64);
                {
                    ir::IrInstr cl{};
                    cl.op          = ir::IrOp::CALLN;
                    cl.type        = ir::IrType::U64;
                    cl.dst         = v_len;
                    cl.func_name   = "stdlib/native/io/vesta_io:vstr_replace_to_vmbuf";
                    cl.operands    = {v_proc, v_dst_buf,
                                       v_src_addr,  v_src_len,
                                       v_from_addr, v_from_len,
                                       v_to_addr,   v_to_len};
                    cl.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(cl));
                }
                ir::IrValueId v_h = emit_strmake(v_dst_buf, v_len, e->loc.line);
                out_value = v_h;
                return true;
            }

            // str_starts_with / str_ends_with / str_index_of:
            // busqueda zero-copy via natives de vesta_collections.  Los
            // helpers aceptan DIRECTAMENTE los host_ptr de STRRAW (mismo
            // patron que vstr_contains de vesta_io) y no reciben proc.
            if (is_str_starts_with || is_str_ends_with || is_str_index_of) {
                const char *native = is_str_starts_with ? "vstr_starts_with"
                                 : (is_str_ends_with ? "vstr_ends_with"
                                                     : "vstr_indexof");
                if (e->args.size() != 2) {
                    error_at(e->loc, std::string("'") + name
                                     + "': se esperaban 2 argumentos (string, substring)");
                    out_value = ir::IR_NO_VALUE;
                    return true;
                }
                const ir::IrValueId v_hay = coerce_str_arg(e->args[0].get());
                const ir::IrValueId v_ndl = coerce_str_arg(e->args[1].get());
                if (v_hay == ir::IR_NO_VALUE || v_ndl == ir::IR_NO_VALUE) {
                    out_value = ir::IR_NO_VALUE;
                    return true;
                }
                auto [v_h_addr, v_h_len] =
                    materialize_str_to_vmbuf(v_hay, e->loc.line);
                auto [v_n_addr, v_n_len] =
                    materialize_str_to_vmbuf(v_ndl, e->loc.line);
                const std::string lib = "stdlib/native/collections/vesta_collections";
                out_mod_->register_native_import(lib, native);
                ir::IrType rt = is_str_index_of ? ir::IrType::I64 : ir::IrType::BOOL;
                ir::IrValueId v_dst = fn_->new_value(rt);
                {
                    ir::IrInstr cl{};
                    cl.op          = ir::IrOp::CALLN;
                    cl.type        = rt;
                    cl.dst         = v_dst;
                    cl.func_name   = lib + ":" + native;
                    cl.operands    = {v_h_addr, v_h_len, v_n_addr, v_n_len};
                    cl.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(cl));
                }
                out_value = v_dst;
                return true;
            }
        }

        // ----- builtins de string -----
        // Cada uno se baja a un solo opcode bytecode mediante RAW_ASM
        // con substitucion {dst}/{src0}/{src1}.  Cero overhead vs .vel
        // crudo; el regalloc decide los registros.
        if (is_str_length || is_str_bytes || is_str_cstr || is_str_wstr
            || is_str_hash || is_str_intern) {
            if (e->args.size() != 1) {
                error_at(e->loc, std::string("'") + name + "': 1 arg");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // coerce string literal (PTR) a StringObject (STRING
            // handle) inline via STRMAKE.  Sin esto pasar un literal directo
            // a str_cstr("wb") emitia STRRAW sobre el ptr raw del literal en
            // static_data, retornando garbage.  Mismo patron que fix3
            // hace en lower_call para args de funciones top-level.
            ast::Expr *   ae = e->args[0].get();
            ir::IrValueId v_str;
            if (ae && ae->kind == ast::NodeKind::StringLitExpr) {
                auto *sl = static_cast<ast::StringLitExpr *>(ae);
                // Tanto literales puros como interpolados: el helper
                // construye el StringObject correcto.
                v_str = lower_string_literal_to_string_object(sl);
            } else {
                v_str = lower_expr(ae);
            }
            if (v_str == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // wstr requiere strconv UTF16 + strraw (2 ops).
            if (is_str_wstr) {
                // strconv(s, ENC_UTF16=3) + strraw -> host_ptr a wchar_t* para Win32 *W.
                ir::IrValueId v_conv = emit_strconv(v_str, /*enc=UTF16*/3, e->loc.line);
                ir::IrValueId v_raw  = emit_strraw(v_conv, e->loc.line);
                out_value = v_raw;
                return true;
            }
            // Resto: 1 sola instruccion bytecode mediante IR ops dedicados.
            if (is_str_length) {
                ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
                ir::IrInstr ins{};
                ins.op          = ir::IrOp::STRLEN;
                ins.type        = ir::IrType::I64;
                ins.dst         = v_dst;
                ins.operands    = {v_str};
                ins.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ins));
                out_value = v_dst;
            } else if (is_str_bytes) {
                out_value = emit_strgetbytes(v_str, e->loc.line);
            } else if (is_str_cstr) {
                out_value = emit_strraw(v_str, e->loc.line);
            } else if (is_str_hash) {
                ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
                ir::IrInstr ins{};
                ins.op          = ir::IrOp::STRHASH;
                ins.type        = ir::IrType::I64;
                ins.dst         = v_dst;
                ins.operands    = {v_str};
                ins.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ins));
                out_value = v_dst;
            } else {
                // str_intern: aloca nuevo StringObject canonical o reusa pool.
                // Retorna GcHandle (no host_ptr), por eso no is_gc_object.
                ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
                ir::IrInstr ins{};
                ins.op           = ir::IrOp::STRINTERN;
                ins.type         = ir::IrType::I64;
                ins.dst          = v_dst;
                ins.operands     = {v_str};
                ins.set_is_call_site(true);
                ins.source_line  = e->loc.line;
                fn_->append(current_block_, std::move(ins));
                out_value = v_dst;
            }
            return true;
        }

        if (is_str_concat || is_str_equals) {
            if (e->args.size() != 2) {
                error_at(e->loc, std::string("'") + name + "': 2 args");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // Coerce string literals (PTR) a StringObject
            // (STRING handle) inline via STRMAKE.  Sin esto pasar un
            // literal directamente a str_concat/str_equals enviaria un
            // puntero raw como handle (UB).
            auto coerce_to_string_handle = [&](ast::Expr *ex) -> ir::IrValueId {
                if (ex && ex->kind == ast::NodeKind::StringLitExpr) {
                    auto *sl = static_cast<ast::StringLitExpr *>(ex);
                    // Tanto literales puros como interpolados.
                    return lower_string_literal_to_string_object(sl);
                }
                return lower_expr(ex);
            };
            ir::IrValueId v_a = coerce_to_string_handle(e->args[0].get());
            ir::IrValueId v_b = coerce_to_string_handle(e->args[1].get());
            if (v_a == ir::IR_NO_VALUE || v_b == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // Sprint 6.D: usar STRCAT/STRCMP IR ops directos.  El helper
            // emit_strcat ya emite IR op puro con is_call_site=true.
            ir::IrValueId v_dst;
            if (is_str_concat) {
                v_dst = emit_strcat(v_a, v_b, e->loc.line);
            } else {
                v_dst = fn_->new_value(ir::IrType::I64);
                ir::IrInstr cmp{};
                cmp.op          = ir::IrOp::STRCMP;
                cmp.type        = ir::IrType::I64;
                cmp.dst         = v_dst;
                cmp.operands    = {v_a, v_b};
                cmp.source_line = e->loc.line;
                fn_->append(current_block_, std::move(cmp));
            }
            // str_equals returns -1/0/1 (strcmp).  Convertir a bool: 0 == equal.
            if (is_str_equals) {
                ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0, e->loc.line);
                ir::IrValueId v_eq   = fn_->new_value(ir::IrType::BOOL);
                ir::IrInstr   cmp{};
                cmp.op          = ir::IrOp::CMP_EQ;
                cmp.type        = ir::IrType::BOOL;
                cmp.dst         = v_eq;
                cmp.operands    = {v_dst, v_zero};
                cmp.source_line = e->loc.line;
                fn_->append(current_block_, std::move(cmp));
                out_value = v_eq;
            } else {
                out_value = v_dst;
            }
            return true;
        }

        if (is_str_make) {
            if (e->args.size() != 2) {
                error_at(e->loc, "str_make: 2 args (ptr, len)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            ir::IrValueId v_ptr = lower_expr(e->args[0].get());
            ir::IrValueId v_len = lower_expr(e->args[1].get());
            if (v_ptr == ir::IR_NO_VALUE || v_len == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // Auto-detect: si el puntero proviene de memoria HOST (malloc,
            // str_cstr, gcallocp, etc.) emitimos `strmake_h` que lee bytes
            // del host.  Si es VM (subsp+&local, STR_LIT_ADDR, etc.) usamos
            // `strmake` original que lee de vm_mem.  Esto cierra el bug
            // historico en el que `str_make(buffer.data, len)` con `data`
            // mallocado retornaba zeros
            // Sprint 6.D: STRMAKE IR op.  El emitter elige strmake vs
            // strmake_h segun el flag is_host_ptr del SSA value v_ptr,
            // lo que reemplaza el if-else explicito anterior.
            out_value = emit_strmake(v_ptr, v_len, e->loc.line);
            return true;
        }

        // str_convert(s, enc) -> nuevo string con encoding
        // seleccionado.  El opcode strconv requiere encoding como inmediato
        // en el bytecode (no via registro), asi que el segundo arg debe
        // ser una constante numerica resuelta en compile time (literal int
        // o constante ENC_*).  Si no lo es, error claro.
        if (is_str_convert) {
            if (e->args.size() != 2) {
                error_at(e->loc, "str_convert: 2 args (string, encoding)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            ir::IrValueId v_s = lower_expr(e->args[0].get());
            if (v_s == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // Segundo arg debe ser literal int o constante ENC_* conocida.
            int32_t enc_val = -1;
            if (e->args[1] && e->args[1]->kind == ast::NodeKind::IntLitExpr) {
                auto *il = static_cast<ast::IntLitExpr *>(e->args[1].get());
                enc_val  = (int32_t) il->value;
            } else if (e->args[1] && e->args[1]->kind == ast::NodeKind::IdentExpr) {
                // Constante ENC_*: lookup en type checker.
                auto *id = static_cast<ast::IdentExpr *>(e->args[1].get());
                static const struct {
                    const char *name;
                    int32_t     v;
                } ENC_LU[] = {
                            {"ENC_ASCII", 0}, {"ENC_ANSI", 1}, {"ENC_UTF8", 2},
                            {"ENC_UTF16", 3}, {"ENC_UTF32", 4},
                        };
                for (const auto &m: ENC_LU) {
                    if (id->name == m.name) {
                        enc_val = m.v;
                        break;
                    }
                }
            }
            if (enc_val < 0 || enc_val > 4) {
                error_at(e->loc,
                         "str_convert: encoding debe ser literal int o ENC_ASCII/ANSI/UTF8/UTF16/UTF32");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            out_value = emit_strconv(v_s, static_cast<uint64_t>(enc_val), e->loc.line);
            return true;
        }

        // ----- forName(string_lit) -----
        // Reflexion: devuelve ClassInfo* (i64 opaco) registrado en el
        // ClassRegistry por nombre.  Acepta SOLO un string literal.
        // Internamos el nombre en static_data (deduplicado:
        // si la clase ya esta declarada en __module_init, comparten idx).
        if (is_forName) {
            if (e->args.size() != 1
                || !e->args[0]
                || e->args[0]->kind != ast::NodeKind::StringLitExpr) {
                error_at(e->loc,
                         "forName: requiere un string literal con el nombre de la clase");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            auto *         slit     = static_cast<ast::StringLitExpr *>(e->args[0].get());
            const uint64_t name_idx = intern_class_name(*out_mod_, slit->value);
            const uint32_t name_len = static_cast<uint32_t>(slit->value.size());

            // RAW_ASM con dst = SSA val.  Usamos {dst} como placeholder
            // Sprint 5: emit_findclass_by_name reemplaza emit_findclass_inline
            // (textual) por secuencia IR pura.
            out_value = emit_findclass_by_name(name_idx, name_len, e->loc.line);
            return true;
        }

        // ----- getField(cls, "name") -----
        // Reflexion: devuelve FieldInfo* (i64 opaco) buscando el campo
        // por nombre dentro de la clase indicada.  Args: cls = i64
        // (ClassInfo* obtenido via forName/getClass), name = string lit.
        // El lowering construye FindMethodParamsLayout (mismo shape que
        // findfield espera) en stack y emite la instruccion findfield.
        if (is_getField) {
            if (e->args.size() != 2
                || !e->args[0]
                || !e->args[1]
                || e->args[1]->kind != ast::NodeKind::StringLitExpr) {
                error_at(e->loc,
                         "getField: requiere (i64 cls, string lit name)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_cls = lower_expr(e->args[0].get());
            if (v_cls == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            auto *         slit     = static_cast<ast::StringLitExpr *>(e->args[1].get());
            const uint64_t name_idx = intern_class_name(*out_mod_, slit->value);
            const uint32_t name_len = static_cast<uint32_t>(slit->value.size());

            // Construir FindFieldParams (24 bytes: class_ptr, name_addr,
            // name_len, _pad) en stack y llamar findfield.  El SCRATCH r12
            // se usa como puntero a la struct y receptor del resultado.
            std::ostringstream oss;
            oss << "subsp rsp, 24\n";
            oss << "mov r12, rsp\n";
            // [+0] class_ptr -- usamos un mov via emit_store: lo cargamos
            // de un valor SSA conocido v_cls.  Como en el bloque RAW_ASM
            // no podemos referenciar SSA values, primero materializamos
            // v_cls en r0 via un MOV IR que precede al RAW_ASM.

            // Materializar v_cls en r0 antes del RAW_ASM.  Usamos MOV IR
            // que el regalloc resolvera moviendo el reg de v_cls a r0.
            ir::IrInstr mv_cls{};
            mv_cls.op   = ir::IrOp::MOV;
            mv_cls.type = ir::IrType::I64;
            // Sin dst SSA: solo queremos el side effect de poner v_cls en
            // r0.  Usamos un nuevo SSA con regalloc forzado a r0 mediante
            // RAW_ASM con {dst} - mas simple: incrustamos en RAW_ASM un
            // load directo del SSA reg via la convencion de load_src.
            //
            // Alternativa mas limpia: emitimos un solo RAW_ASM que toma
            // el reg de v_cls como string y lo usa.  Pero RAW_ASM no
            // expone los regs de operandos.
            //
            // Solucion: usamos CALL a una funcion sintetica? No, demasiado.
            //
            // Plan B: construir la struct via un RAW_ASM previo + un
            // MOV IR que pone v_cls en SCRATCH (r14) que el RAW_ASM puede
            // referenciar literalmente.  Hack: emitimos un MOV IR
            // (dst=v_cls, op=MOV, src=v_cls) -- no-op, pero garantiza que
            // v_cls este en su reg asignado.  Luego en RAW_ASM hacemos
            // mov r14, <reg_of_v_cls> -- pero no sabemos su nombre.
            //
            // La forma correcta: usar un nuevo IR op CONST_ADDR que
            // construye la struct.  Es un overhead bajo pero requiere
            // mas cambios.  limitado a soportar el caso
            // mas comun: el primer arg viene de forName (que fija el
            // resultado en r0 antes de la captura {dst}).  Pero v_cls ya
            // esta en su propio reg post-regalloc.
            //
            // Workaround: emitir un MOV IR explicito a un nuevo SSA con
            // la pista de que su reg sera r0.  Esto no esta soportado
            // limpiamente; usaremos un patron similar al de CALL:
            // emitir CALL a un nombre magico que sabe escribir la
            // struct.  Demasiado.
            //
            // SOLUCION FINAL: emitir un STORE de v_cls en una posicion
            // de stack fija, luego el RAW_ASM lee desde alli.  Mantiene
            // todo dentro del IR.
            (void) mv_cls; // descartar el plan abortado anterior

            // Reservar 24 bytes en stack: usamos ALLOCA i8 con count 24.
            const ir::IrValueId v_buf = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr         al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8;
            al.imm         = 24;
            al.dst         = v_buf;
            al.source_line = e->loc.line;
            fn_->append(current_block_, std::move(al));
            // STORE v_cls en buf+0 (8 bytes).
            ir::IrInstr st0{};
            st0.op          = ir::IrOp::STORE;
            st0.type        = ir::IrType::I64;
            st0.dst         = ir::IR_NO_VALUE;
            st0.operands    = {v_cls, v_buf};
            st0.source_line = e->loc.line;
            fn_->append(current_block_, std::move(st0));
            // STORE name_addr en buf+8.  Para esto necesitamos un puntero
            // a buf+8 -- usamos ADD.
            const ir::IrValueId v_eight = emit_const(ir::IrType::I64, 8, e->loc.line);
            const ir::IrValueId v_buf8  = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr         add_8{};
            add_8.op          = ir::IrOp::ADD;
            add_8.type        = ir::IrType::I64;
            add_8.dst         = v_buf8;
            add_8.operands    = {v_buf, v_eight};
            add_8.source_line = e->loc.line;
            fn_->append(current_block_, std::move(add_8));
            // Cargar name_addr via STR_LIT_ADDR.
            const ir::IrValueId v_name = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr         ns{};
            ns.op          = ir::IrOp::STR_LIT_ADDR;
            ns.type        = ir::IrType::PTR;
            ns.dst         = v_name;
            ns.imm         = name_idx;
            ns.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ns));
            ir::IrInstr st8{};
            st8.op          = ir::IrOp::STORE;
            st8.type        = ir::IrType::I64;
            st8.dst         = ir::IR_NO_VALUE;
            st8.operands    = {v_name, v_buf8};
            st8.source_line = e->loc.line;
            fn_->append(current_block_, std::move(st8));
            // STORE name_len en buf+16.
            const ir::IrValueId v_sixteen = emit_const(ir::IrType::I64, 16, e->loc.line);
            const ir::IrValueId v_buf16   = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr         add_16{};
            add_16.op          = ir::IrOp::ADD;
            add_16.type        = ir::IrType::I64;
            add_16.dst         = v_buf16;
            add_16.operands    = {v_buf, v_sixteen};
            add_16.source_line = e->loc.line;
            fn_->append(current_block_, std::move(add_16));
            const ir::IrValueId v_len = emit_const(ir::IrType::I64,
                                                   static_cast<uint64_t>(name_len),
                                                   e->loc.line);
            ir::IrInstr st16{};
            st16.op          = ir::IrOp::STORE;
            st16.type        = ir::IrType::I64;
            st16.dst         = ir::IR_NO_VALUE;
            st16.operands    = {v_len, v_buf16};
            st16.source_line = e->loc.line;
            fn_->append(current_block_, std::move(st16));
            // findfield via RAW_ASM: r12 = buf, dst = SSA capturado con {dst}.
            // El reg de v_buf debe ir a r12; lo movemos via patron MOV.
            // Mas simple: emitimos `mov r12, <reg_v_buf>` mediante MOV IR
            // explicito y luego `findfield {dst}, r12` en RAW_ASM.
            //
            // El RAW_ASM con {dst} substitution maneja el destino, pero
            // los operandos (v_buf) requieren que sepamos su reg.  Como
            // no conocemos el reg en lowering time, usamos otro truco:
            // emitir un MOV IR que tenga como source v_buf y dst sera
            // un nuevo SSA cuya regalloc no controlamos.  Sin embargo el
            // emisor IR ya emite mov rA, rB donde rA es el reg de dst.
            //
            // Para forzar v_buf en r12 antes del findfield, agregamos
            // una IR_op especial... no la tengo.  Hagamos: usar el RAW_ASM
            // pero referirlo a memoria via dirección absoluta.  Imposible
            // sin acceso al reg.
            //
            // SOLUCION SIMPLE: usar un CALL falso a una funcion sintetica
            // implementada como RAW_ASM body, pasando v_buf como arg.
            // El IR emitter colocara v_buf en r1 segun la calling
            // convention.  Luego el RAW_ASM mueve r1 a r12 y llama findfield.
            //
            // Pero CALL requiere una funcion declarada.  Generemos una
            // helper inline que cumpla este rol; mas limpio: emitir un
            // RAW_ASM que use load_src... no es accesible.
            //
            // Alternativa minimal: mover v_buf a r12 via STORE+LOAD por
            // medio de un slot reservado (ALLOCA otro de 8 bytes), o
            // forzando regalloc.  El regalloc no soporta hints de reg.
            //
            // Estrategia FINAL: emitir el RAW_ASM con substitucion {src0}
            // que el emisor reemplaza por el reg del primer operando.
            // Requiere extender RAW_ASM para conocer operands tambien.
            // Lo implemento ahora.
            out_value = emit_findfield(v_buf, e->loc.line);
            return true;
        }

        // ----- getMethod(cls, "name") -----
        // Reflexion: devuelve MethodInfo* (i64 opaco) buscando el metodo
        // por nombre dentro de la clase.  Args: cls = i64 (ClassInfo*),
        // name = string lit.  Misma estructura que getField pero usando
        // el opcode bytecode `findmethod` (0xCD).  El struct param es
        // FindMethodParams (24 bytes: class_ptr, name_addr, name_len).
        if (is_getMethod) {
            if (e->args.size() != 2
                || !e->args[0]
                || !e->args[1]
                || e->args[1]->kind != ast::NodeKind::StringLitExpr) {
                error_at(e->loc,
                         "getMethod: requiere (i64 cls, string lit name)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_cls = lower_expr(e->args[0].get());
            if (v_cls == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            auto *         slit     = static_cast<ast::StringLitExpr *>(e->args[1].get());
            const uint64_t name_idx = intern_class_name(*out_mod_, slit->value);
            const uint32_t name_len = static_cast<uint32_t>(slit->value.size());
            // ALLOCA 24 bytes para FindMethodParams.
            const ir::IrValueId v_buf = fn_->new_value(ir::IrType::PTR); {
                ir::IrInstr al{};
                al.op          = ir::IrOp::ALLOCA;
                al.type        = ir::IrType::I8;
                al.imm         = 24;
                al.dst         = v_buf;
                al.source_line = e->loc.line;
                fn_->append(current_block_, std::move(al));
            }
            // [+0] class_ptr.
            {
                ir::IrInstr st0{};
                st0.op          = ir::IrOp::STORE;
                st0.type        = ir::IrType::I64;
                st0.operands    = {v_cls, v_buf};
                st0.source_line = e->loc.line;
                fn_->append(current_block_, std::move(st0));
            }
            // [+8] name_addr.
            const ir::IrValueId v_eight = emit_const(ir::IrType::I64, 8, e->loc.line);
            const ir::IrValueId v_buf8  = fn_->new_value(ir::IrType::PTR); {
                ir::IrInstr add_8{};
                add_8.op          = ir::IrOp::ADD;
                add_8.type        = ir::IrType::I64;
                add_8.dst         = v_buf8;
                add_8.operands    = {v_buf, v_eight};
                add_8.source_line = e->loc.line;
                fn_->append(current_block_, std::move(add_8));
            }
            const ir::IrValueId v_name = fn_->new_value(ir::IrType::PTR); {
                ir::IrInstr ns{};
                ns.op          = ir::IrOp::STR_LIT_ADDR;
                ns.type        = ir::IrType::PTR;
                ns.dst         = v_name;
                ns.imm         = name_idx;
                ns.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ns));
            } {
                ir::IrInstr st8{};
                st8.op          = ir::IrOp::STORE;
                st8.type        = ir::IrType::I64;
                st8.operands    = {v_name, v_buf8};
                st8.source_line = e->loc.line;
                fn_->append(current_block_, std::move(st8));
            }
            // [+16] name_len.
            const ir::IrValueId v_sixteen = emit_const(ir::IrType::I64, 16, e->loc.line);
            const ir::IrValueId v_buf16   = fn_->new_value(ir::IrType::PTR); {
                ir::IrInstr add_16{};
                add_16.op          = ir::IrOp::ADD;
                add_16.type        = ir::IrType::I64;
                add_16.dst         = v_buf16;
                add_16.operands    = {v_buf, v_sixteen};
                add_16.source_line = e->loc.line;
                fn_->append(current_block_, std::move(add_16));
            }
            const ir::IrValueId v_len = emit_const(ir::IrType::I64,
                                                   static_cast<uint64_t>(name_len),
                                                   e->loc.line); {
                ir::IrInstr st16{};
                st16.op          = ir::IrOp::STORE;
                st16.type        = ir::IrType::I64;
                st16.operands    = {v_len, v_buf16};
                st16.source_line = e->loc.line;
                fn_->append(current_block_, std::move(st16));
            }
            out_value = emit_findmethod(v_buf, e->loc.line);
            return true;
        }

        // ----- newInstance(cls) -----
        // Crea una instancia nueva de la clase indicada (sin invocar
        // ningun constructor).  Equivalente a `Object.newInstance` de Java.
        // El usuario es responsable de inicializar los campos despues.
        // El opcode `newobj r_dst, r_cls` (0xC9) aloca un objeto en el
        // GC heap con espacio para todos los fields y devuelve un GcHandle
        // en R0.  Convertimos a host_ptr via gcderef + xchg (igual que
        // hace __new_<X>) para que el resultado sea utilizable como objeto.
        if (is_newInstance) {
            if (e->args.size() != 1 || !e->args[0]) {
                error_at(e->loc,
                         "newInstance: requiere un argumento (i64 cls)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // Fix #1 (caso estatico): si el arg es un IdentExpr con origen
            // conocido (`Class cls = Class.forName("X")`), emitir `new X()`
            // que invoca el constructor via `__new_<X>` synthetic.  Cero
            // overhead vs newInstance directo (mismo bytecode que el frontend
            // genera para `new X()`).  Para casos dinamicos donde el origen
            // no se conoce, fallback a NEWOBJ raw (sin ctor; documentado).
            if (e->args[0]->kind == ast::NodeKind::IdentExpr) {
                auto *id = static_cast<ast::IdentExpr *>(e->args[0].get());
                auto it = class_origin_of_local_.find(id->name);
                if (it != class_origin_of_local_.end()) {
                    const std::string &class_name = it->second;
                    // Verificar que la clase existe en class_layouts y
                    // tiene un constructor sin argumentos.  Si no, fallback.
                    const auto &layouts = tc_.class_layouts();
                    auto it2 = layouts.find(class_name);
                    if (it2 != layouts.end()) {
                        // Sintetizar NewExpr equivalente a `new X()` y
                        // delegar en lower_new_expr (que invoca el helper
                        // sintetico __new_X que SI llama al ctor).
                        ast::NewExpr nx;
                        nx.loc        = e->loc;
                        nx.class_name = class_name;
                        // Sin args (no-arg constructor).
                        out_value = lower_new_expr(&nx);
                        if (out_value != ir::IR_NO_VALUE) return true;
                        // Si lower_new_expr fallo, caer al path NEWOBJ raw.
                    }
                }
            }
            const ir::IrValueId v_cls = lower_expr(e->args[0].get());
            if (v_cls == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // Usar IR_OP NEWOBJ con operands[0] = cls.
            const ir::IrValueId v_handle = fn_->new_value(ir::IrType::I64); {
                ir::IrInstr no{};
                no.op          = ir::IrOp::NEWOBJ;
                no.type        = ir::IrType::I64;
                no.dst         = v_handle;
                no.operands    = {v_cls};
                no.source_line = e->loc.line;
                fn_->append(current_block_, std::move(no));
            }
            // Convertir handle a host_ptr (igual que __new_<X> antes del
            // ctor).  El resultado es un host_ptr GC-managed.
            const ir::IrValueId v_host       = fn_->new_value(ir::IrType::I64);
            fn_->values[v_host].is_host_ptr  = true;
            fn_->values[v_host].is_gc_object = true; {
                // raw_asm-elim 2026-05-28: gcderef + xchg -> IrOp::GC_DEREF_HOST.
                ir::IrInstr ra{};
                ra.op          = ir::IrOp::GC_DEREF_HOST;
                ra.type        = ir::IrType::PTR;
                ra.dst         = v_host;
                ra.operands    = {v_handle};
                ra.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ra));
            }
            out_value = v_host;
            return true;
        }

        // ----- invoke(method, this, args...) -----
        // Reflexion completa: invoca un MethodInfo* obtenido via getMethod
        // sobre un receiver `this`, con N args.  Equivalente a
        // `Method.invoke(receiver, args...)` de Java.  La ABI sigue el
        // patron CALLVIRT: r1 = this, r2..r12 = args, r15 = argc, r0 = ret.
        // Internamente usa el opcode bytecode `callm r_obj, r_method`
        // (0xFD) que dispara la cadena AOP advice_chain como CALLVIRT.
        if (is_invoke) {
            if (e->args.size() < 2 || !e->args[0] || !e->args[1]) {
                error_at(e->loc,
                         "invoke: requiere (i64 method, T this, args...)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_method = lower_expr(e->args[0].get());
            if (v_method == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_this = lower_expr(e->args[1].get());
            if (v_this == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            std::vector<ir::IrValueId> v_args;
            v_args.reserve(e->args.size() - 2);
            for (size_t k = 2; k < e->args.size(); ++k) {
                if (!e->args[k]) {
                    out_value = ir::IR_NO_VALUE;
                    return true;
                }
                ir::IrValueId av = lower_expr(e->args[k].get());
                if (av == ir::IR_NO_VALUE) {
                    out_value = ir::IR_NO_VALUE;
                    return true;
                }
                v_args.push_back(av);
            }
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr         cm{};
            cm.op   = ir::IrOp::CALLM;
            cm.type = ir::IrType::I64;
            cm.dst  = v_dst;
            cm.operands.push_back(v_this);   // [0] = obj
            cm.operands.push_back(v_method); // [1] = method
            for (auto av: v_args) cm.operands.push_back(av);
            cm.source_line = e->loc.line;
            fn_->append(current_block_, std::move(cm));
            out_value = v_dst;
            return true;
        }

        // Helper local: alocar un buffer de N bytes en STACK via ALLOCA.
        // El buffer vive solo durante la funcion actual; cuando la
        // funcion retorna, el frame se libera y la memoria se reusa.
        // Para funciones que DEVUELVEN Optional/Result, lower_return
        // copia el contenido del buffer local al retbuf del caller
        // (sret-style ABI) ANTES de que el callee desaparezca.  Asi
        // evitamos heap allocation y leaks: el lifecycle es estrictamente
        // tied al stack frame que lo creo.
        // BugFix sret-cross-mem (2026-06-04): el flag `for_optres` activa
        // host_alloca SOLO para Optional/Result (no para smart-ptr).  Asi
        // los buffers de Some/Ok/Err viven en host mem consistente con los
        // retbuf SRET del caller, sin afectar el lowering de unique<T>.
        auto stack_alloc_buf = [&](uint64_t bytes, uint32_t line,
                                    bool for_optres = false) -> ir::IrValueId {
            const ir::IrValueId v_buf = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr         al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8;
            al.imm         = bytes;
            al.dst         = v_buf;
            al.source_line = line;
            if (for_optres) {
                al.set_host_alloca(true);
            }
            fn_->append(current_block_, std::move(al));
            if (for_optres) {
                fn_->values[v_buf].is_host_ptr = true;
            }
            return v_buf;
        };

        // ----- Some(x) -----  Optional<T> en heap.
        //   Layout: [+0 i64 flag=1][+8 T payload].  Total 16 bytes.
        if (is_Some) {
            if (e->args.size() != 1) {
                error_at(e->loc, "Some: requiere 1 argumento");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_payload = lower_expr(e->args[0].get());
            if (v_payload == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_buf = stack_alloc_buf(16, e->loc.line, /*for_optres=*/true);
            // Store flag = 1 at +0.
            const ir::IrValueId v_one = emit_const(ir::IrType::I64, 1, e->loc.line);
            ir::IrInstr         st0{};
            st0.op          = ir::IrOp::STORE;
            st0.type        = ir::IrType::I64;
            st0.operands    = {v_one, v_buf};
            st0.source_line = e->loc.line;
            fn_->append(current_block_, std::move(st0));
            // Compute buf+8 and store payload there.
            const ir::IrValueId v_eight = emit_const(ir::IrType::I64, 8, e->loc.line);
            const ir::IrValueId v_buf8  = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr         add{};
            add.op          = ir::IrOp::ADD;
            add.type        = ir::IrType::I64;
            add.dst         = v_buf8;
            add.operands    = {v_buf, v_eight};
            add.source_line = e->loc.line;
            fn_->append(current_block_, std::move(add));
            const ir::IrType payload_t = fn_->values[v_payload].type;
            ir::IrInstr      st1{};
            st1.op          = ir::IrOp::STORE;
            st1.type        = payload_t;
            st1.operands    = {v_payload, v_buf8};
            st1.source_line = e->loc.line;
            fn_->append(current_block_, std::move(st1));
            out_value = v_buf;
            return true;
        }
        // ----- None() -----  Optional vacio (flag=0) en stack.
        if (is_None) {
            const ir::IrValueId v_buf  = stack_alloc_buf(16, e->loc.line, /*for_optres=*/true);
            const ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0, e->loc.line);
            ir::IrInstr         st0{};
            st0.op          = ir::IrOp::STORE;
            st0.type        = ir::IrType::I64;
            st0.operands    = {v_zero, v_buf};
            st0.source_line = e->loc.line;
            fn_->append(current_block_, std::move(st0));
            out_value = v_buf;
            return true;
        }
        // ----- Ok(v) ----- / ----- Err(e) -----  Result<V,E> en heap.
        //   Layout: [+0 i64 tag (1=ok, 0=err)][+8 V][+16 E]. 24 bytes.
        if (is_Ok || is_Err) {
            if (e->args.size() != 1) {
                error_at(e->loc, (is_Ok ? "Ok" : "Err") +
                         std::string(": requiere 1 argumento"));
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // Bug fix 2026-05-23: si el payload esperado del Result es STRING
            // y el arg es StringLitExpr no interpolado, promover a StringObject
            // ANTES del STORE.  Sin esto, el handle guardado seria el ptr
            // raw del literal -> garbage en isOk/value/error.  El expected_type
            // viene de e->result_type que ya fue calculado en check_call.
            ir::IrValueId v_payload;
            bool need_str_promo = false;
            if (e->args[0]
             && e->args[0]->kind == ast::NodeKind::StringLitExpr
             && e->result_type.kind == PrimitiveKind::RESULT) {
                const Type *target = is_Ok
                    ? e->result_type.pointee.get()
                    : e->result_type.pointee2.get();
                if (target && target->kind == PrimitiveKind::STRING) {
                    need_str_promo = true;
                }
            }
            if (need_str_promo) {
                auto *slit = static_cast<ast::StringLitExpr *>(e->args[0].get());
                v_payload = lower_string_literal_to_string_object(slit);
            } else {
                v_payload = lower_expr(e->args[0].get());
            }
            if (v_payload == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_buf = stack_alloc_buf(24, e->loc.line, /*for_optres=*/true);
            // Tag.
            const ir::IrValueId v_tag = emit_const(ir::IrType::I64,
                                                   is_Ok ? 1 : 0, e->loc.line);
            ir::IrInstr st0{};
            st0.op          = ir::IrOp::STORE;
            st0.type        = ir::IrType::I64;
            st0.operands    = {v_tag, v_buf};
            st0.source_line = e->loc.line;
            fn_->append(current_block_, std::move(st0));
            // Payload offset: V en +8 (Ok), E en +16 (Err).
            const uint64_t      off   = is_Ok ? 8 : 16;
            const ir::IrValueId v_off = emit_const(ir::IrType::I64, off, e->loc.line);
            const ir::IrValueId v_at  = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr         add{};
            add.op          = ir::IrOp::ADD;
            add.type        = ir::IrType::I64;
            add.dst         = v_at;
            add.operands    = {v_buf, v_off};
            add.source_line = e->loc.line;
            fn_->append(current_block_, std::move(add));
            // BugFix sret-cross-mem (2026-06-04): propagar is_host_ptr.
            fn_->values[v_at].is_host_ptr = fn_->values[v_buf].is_host_ptr;
            const ir::IrType payload_t = fn_->values[v_payload].type;
            ir::IrInstr      st1{};
            st1.op          = ir::IrOp::STORE;
            st1.type        = payload_t;
            st1.operands    = {v_payload, v_at};
            st1.source_line = e->loc.line;
            fn_->append(current_block_, std::move(st1));
            out_value = v_buf;
            return true;
        }
        // ----- isOk(r) -----  LOAD i64 at +0; returns 1/0 as i32.
        if (is_isOk) {
            if (e->args.size() != 1) {
                error_at(e->loc, "isOk: requiere 1 argumento");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_buf = lower_expr(e->args[0].get());
            if (v_buf == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I32);
            ir::IrInstr         ld{};
            ld.op          = ir::IrOp::LOAD;
            ld.type        = ir::IrType::I32;
            ld.dst         = v_dst;
            ld.operands    = {v_buf};
            ld.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ld));
            out_value = v_dst;
            return true;
        }
        // ----- value(r) -----  LOAD V from r+8 (sin tag check en MVP).
        // ----- error(r) -----  LOAD E from r+16 (sin tag check en MVP).
        if (is_value || is_error) {
            if (e->args.size() != 1) {
                error_at(e->loc, "value/error: requiere 1 argumento");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_buf = lower_expr(e->args[0].get());
            if (v_buf == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const Type at         = e->args[0]->result_type;
            Type       payload_st = (is_value
                                         ? (at.pointee ? *at.pointee : Type{PrimitiveKind::I64})
                                         : (at.pointee2 ? *at.pointee2 : Type{PrimitiveKind::I64}));
            const ir::IrType    payload_t = ir_type_from_primitive(payload_st.kind);
            const uint64_t      off       = is_value ? 8 : 16;
            const ir::IrValueId v_off     = emit_const(ir::IrType::I64, off, e->loc.line);
            const ir::IrValueId v_at      = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr         add{};
            add.op          = ir::IrOp::ADD;
            add.type        = ir::IrType::I64;
            add.dst         = v_at;
            add.operands    = {v_buf, v_off};
            add.source_line = e->loc.line;
            fn_->append(current_block_, std::move(add));
            // BugFix sret-cross-mem (2026-06-04): propagar is_host_ptr de
            // v_buf al v_at para que el LOAD downstream emita `movh`/`loadzh`.
            fn_->values[v_at].is_host_ptr = fn_->values[v_buf].is_host_ptr;
            const ir::IrValueId v_dst = fn_->new_value(payload_t);
            ir::IrInstr         ld{};
            ld.op          = ir::IrOp::LOAD;
            ld.type        = payload_t;
            ld.dst         = v_dst;
            ld.operands    = {v_at};
            ld.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ld));
            out_value = v_dst;
            return true;
        }

        // ----- isPresent(x) -----
        // Para Optional<T> builtin: LOAD i64 al offset 0 del buffer.
        // Para referencias (CLASS/PTR) legacy: usa la instruccion VM
        // @c isnull (0x25) invertida con XOR.
        if (is_isPresent) {
            if (e->args.size() != 1) {
                error_at(e->loc, "isPresent: requiere exactamente 1 argumento");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_arg = lower_expr(e->args[0].get());
            if (v_arg == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // Optional<T>: LOAD i32 (flag) directamente del buffer stack.
            if (e->args[0]->result_type.kind == PrimitiveKind::OPTIONAL) {
                const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I32);
                ir::IrInstr         ld{};
                ld.op          = ir::IrOp::LOAD;
                ld.type        = ir::IrType::I32;
                ld.dst         = v_dst;
                ld.operands    = {v_arg};
                ld.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ld));
                out_value = v_dst;
                return true;
            }
            // raw_asm-elim 2026-05-28: isPresent(p) = (p != null) implementado
            // como secuencia de 2 IR ops:
            //   v_is_null = ISNULL(v_arg)   -> i32 (0 = not null, 1 = null)
            //   v_dst     = XOR(v_is_null, 1) -> i32 (invierte el bit 0)
            // Esto reemplaza el RAW_ASM original que hacia `isnull + mov r14,1 + xor`.
            // Beneficios: DCE puede eliminar la cadena si v_dst no se usa, el
            // Selector JIT trata cada paso natively, y CSE puede fundir
            // multiples isPresent del mismo arg.  Mismo bytecode emitido.
            const ir::IrValueId v_is_null = fn_->new_value(ir::IrType::I32);
            {
                ir::IrInstr in{};
                in.op          = ir::IrOp::ISNULL;
                in.type        = ir::IrType::I32;
                in.dst         = v_is_null;
                in.operands    = {v_arg};
                in.source_line = e->loc.line;
                fn_->append(current_block_, std::move(in));
            }
            const ir::IrValueId v_one = emit_const(ir::IrType::I32, 1, e->loc.line);
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I32);
            {
                ir::IrInstr xr{};
                xr.op          = ir::IrOp::XOR;
                xr.type        = ir::IrType::I32;
                xr.dst         = v_dst;
                xr.operands    = {v_is_null, v_one};
                xr.source_line = e->loc.line;
                fn_->append(current_block_, std::move(xr));
            }
            out_value = v_dst;
            return true;
        }

        // ----- unwrap(x) -----
        // Para Optional<T> builtin: LOAD flag at +0; pasa por VM
        // `unwrap` (genera NPE si flag==0); luego LOAD payload at +8.
        // Para referencias (CLASS/PTR) legacy: VM `unwrap` directo sobre
        // el puntero (0 = null).
        if (is_unwrap) {
            if (e->args.size() != 1) {
                error_at(e->loc, "unwrap: requiere exactamente 1 argumento");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_arg = lower_expr(e->args[0].get());
            if (v_arg == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // Optional<T>: LOAD flag, unwrap (lanza si 0), LOAD payload.
            if (e->args[0]->result_type.kind == PrimitiveKind::OPTIONAL) {
                const Type at         = e->args[0]->result_type;
                const Type payload_st = at.pointee
                                            ? *at.pointee
                                            : Type{PrimitiveKind::I64};
                const ir::IrType payload_t = ir_type_from_primitive(payload_st.kind);
                // Load flag (i64) from buf+0.
                const ir::IrValueId v_flag = fn_->new_value(ir::IrType::I64);
                ir::IrInstr         ldf{};
                ldf.op          = ir::IrOp::LOAD;
                ldf.type        = ir::IrType::I64;
                ldf.dst         = v_flag;
                ldf.operands    = {v_arg};
                ldf.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ldf));
                // VM unwrap on flag: throws NPE if 0, returns 1 otherwise.
                const ir::IrValueId v_chk = fn_->new_value(ir::IrType::I64);
                ir::IrInstr         uw{};
                uw.op          = ir::IrOp::UNWRAP;
                uw.type        = ir::IrType::I64;
                uw.dst         = v_chk;
                uw.operands    = {v_flag};
                uw.source_line = e->loc.line;
                fn_->append(current_block_, std::move(uw));
                // Load payload from buf+8.
                const ir::IrValueId v_eight = emit_const(ir::IrType::I64, 8, e->loc.line);
                const ir::IrValueId v_at    = fn_->new_value(ir::IrType::PTR);
                ir::IrInstr         add{};
                add.op          = ir::IrOp::ADD;
                add.type        = ir::IrType::I64;
                add.dst         = v_at;
                add.operands    = {v_arg, v_eight};
                add.source_line = e->loc.line;
                fn_->append(current_block_, std::move(add));
                const ir::IrValueId v_dst = fn_->new_value(payload_t);
                ir::IrInstr         ldp{};
                ldp.op          = ir::IrOp::LOAD;
                ldp.type        = payload_t;
                ldp.dst         = v_dst;
                ldp.operands    = {v_at};
                ldp.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ldp));
                out_value = v_dst;
                return true;
            }
            // Referencias legacy: VM `unwrap` directo.
            const ir::IrType    t     = fn_->values[v_arg].type;
            const ir::IrValueId v_dst = fn_->new_value(t);
            ir::IrInstr         ra{};
            ra.op          = ir::IrOp::UNWRAP;
            ra.type        = t;
            ra.dst         = v_dst;
            ra.operands    = {v_arg};
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
            if (fn_->values[v_arg].is_host_ptr) {
                fn_->values[v_dst].is_host_ptr = true;
            }
            out_value = v_dst;
            return true;
        }

        // ----- proceed() -----
        // Re-invoca el target original de un advice @Around.  El opcode
        // `proceed` lee `frame.proceed_target` y dispatcha como CALLM con
        // los registros actuales (r1=this, args en r2..rN, ya colocados
        // por el caller del advice).  Devuelve r0 = resultado del target.
        // Capturamos r0 en el SSA dst via el patron RAW_ASM `{dst}`.
        if (is_proceed) {
            out_value = emit_proceed(e->loc.line);
            return true;
        }

        // ----- getClass(obj) -----
        // ObjectHeader::class_ptr esta en offset 0 del objeto.  El objeto
        // es un host_ptr (resultado del gcderef en __new_<X>), por lo que
        // marcamos el operando con is_host_ptr=true para que el emisor IR
        // genere `movh` en vez de `mov` y lea desde memoria HOST.
        if (is_getClass) {
            if (e->args.size() != 1) {
                error_at(e->loc, "getClass: requiere exactamente 1 argumento");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_obj = lower_expr(e->args[0].get());
            if (v_obj == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // Las instancias creadas con `new` viven en memoria host (gcderef
            // ya las convirtio a host_ptr en __new_<X>); el flag puede
            // perderse al pasar por una variable local con register-allocation,
            // asi que lo forzamos aqui antes del LOAD.
            fn_->values[v_obj].is_host_ptr = true;
            const ir::IrValueId v_dst      = fn_->new_value(ir::IrType::I64);
            ir::IrInstr         ld{};
            ld.op          = ir::IrOp::LOAD;
            ld.type        = ir::IrType::I64;
            ld.dst         = v_dst;
            ld.operands    = {v_obj};
            ld.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ld));
            out_value = v_dst;
            return true;
        }

        // ----- wait(obj) / notify(obj) / notifyAll(obj) -----
        // El argumento es CLASS (host pointer); las instrucciones monwait/
        // monnoti/monnota requieren GcHandle.  Convertimos primero via
        // gchandle (O(1) en el GcHeap) y luego ejecutamos la operacion.
        // Devuelven void; no participan en expresiones.
        if (is_wait || is_notify || is_notifyAll) {
            if (e->args.size() != 1) {
                error_at(e->loc, name + ": requiere exactamente 1 argumento");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_obj = lower_expr(e->args[0].get());
            if (v_obj == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // raw_asm-elim 2026-05-28: reemplazado el blob RAW_ASM original
            // por una secuencia de IR ops nativos (GC_HANDLE_FOR_PTR +
            // MONWAIT/MONNOTI/MONNOTA + MONENTER opcional).  Beneficios:
            //   (a) DCE puede eliminar el handle si la op se elimina.
            //   (b) El Selector JIT no tiene que parsear texto raw_asm.
            //   (c) Cada paso es individualmente reorderable por el optimizer.
            //   (d) Cero overhead vs el RAW_ASM previo: mismo bytecode emitido.
            //
            // BugFix t13 preservado: wait(obj) re-adquiere el monitor tras
            // despertar (semantica Java/POSIX condvar) via MONENTER explicito.
            const ir::IrValueId v_handle = emit_gc_handle_for_ptr(v_obj, e->loc.line);
            ir::IrOp mop = is_wait ? ir::IrOp::MONWAIT
                         : is_notify ? ir::IrOp::MONNOTI
                                     : ir::IrOp::MONNOTA;
            {
                ir::IrInstr mi{};
                mi.op          = mop;
                mi.type        = ir::IrType::VOID;
                mi.dst         = ir::IR_NO_VALUE;
                mi.operands    = {v_handle};
                mi.source_line = e->loc.line;
                fn_->append(current_block_, std::move(mi));
            }
            if (is_wait) {
                // Re-adquirir el monitor tras wake.
                ir::IrInstr me{};
                me.op          = ir::IrOp::MONENTER;
                me.type        = ir::IrType::VOID;
                me.dst         = ir::IR_NO_VALUE;
                me.operands    = {v_handle};
                me.source_line = e->loc.line;
                fn_->append(current_block_, std::move(me));
            }
            out_value = ir::IR_NO_VALUE;
            return true;
        }

        // =====================================================================
        // Smart pointers builtins: unique<T> / shared<T>.
        // =====================================================================
        //
        // Modelo de slot: una variable @c unique<T> p esta bound a un SSA
        // value que es la DIRECCION de un slot stack de 8 bytes que
        // contiene el host_ptr al recurso.  Todas las operaciones acceden
        // al recurso via ese slot:
        //   get(p)            -> LOAD [slot]
        //   move(p) -> q      -> mvtake [q_slot], [p_slot]  (1 instr VM)
        //   cleanup scope exit -> LOAD ptr; CMP_EQ 0; CALL free(ptr) si no-null
        //
        // Para shared<T> el slot contiene un host_ptr al control block
        // gestionado por GC.  El control block tiene refcount@0, deleter@8,
        // payload inline desde +16.

        // ----- unique_box(value) -----  unique<T> Tier 0 con deleter=free.
        // Layout: ALLOCA 8 bytes (slot) + malloc(sizeof(T)) (host) +
        // STORE value en host + STORE host_ptr en slot.  Cleanup al exit
        // del scope: LOAD slot; CMP_EQ 0; CALL free(ptr) si no-null.
        if (is_unique_box || is_shared_box) {
            if (e->args.size() != 1) {
                error_at(e->loc, name + ": requiere 1 argumento");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // Construccion IN-PLACE para `unique<Punto> p = {.x=10, .y=20}`:
            // si el arg es un InitListExpr con target_type_name anotado
            // (desugar de Opcion B en check_var_decl), alocamos host heap
            // PRIMERO y escribimos los campos DIRECTO sobre el host_ptr.
            // Cero memcpy stack -> heap.  Coste = solo los STOREs del init
            // list, igual que un struct value-type normal.
            //
            // El path generico mas abajo (lower_expr del arg + memcpy)
            // sigue cubriendo `unique_box(struct_var_existente)` y otros
            // casos donde el arg ya es un PTR a struct construido.
            if (is_unique_box
             && e->args[0]->kind == ast::NodeKind::InitListExpr) {
                auto *il = static_cast<ast::InitListExpr *>(e->args[0].get());
                if (!il->target_type_name.empty()) {
                    const auto &layouts = tc_.struct_layouts();
                    auto it_lay = layouts.find(il->target_type_name);
                    if (it_lay != layouts.end()) {
                        const StructLayout &lay = it_lay->second;
                        // 1. Slot del unique<T> Tier 1 (16 bytes).  M7:
                        //    si lower_return seteo unique_box_target_slot_,
                        //    construimos directo en el retbuf del caller
                        //    (saltamos el stack_alloc_buf intermedio).
                        const ir::IrValueId v_slot =
                            (unique_box_target_slot_ != ir::IR_NO_VALUE)
                                ? unique_box_target_slot_
                                : stack_alloc_buf(16, e->loc.line);
                        // 2. RAW_ALLOC(sizeof_struct) -> host_ptr.
                        const ir::IrValueId v_size = emit_const(ir::IrType::I64,
                            static_cast<int64_t>(lay.size_bytes), e->loc.line);
                        const ir::IrValueId v_host = fn_->new_value(ir::IrType::PTR);
                        fn_->values[v_host].is_host_ptr = true;
                        {
                            ir::IrInstr ins{};
                            ins.op          = ir::IrOp::RAW_ALLOC;
                            ins.type        = ir::IrType::PTR;
                            ins.dst         = v_host;
                            ins.operands    = {v_size};
                            ins.source_line = e->loc.line;
                            fn_->append(current_block_, std::move(ins));
                        }
                        // 3. STOREs por campo directo al host_ptr.  Soporta
                        //    designated (.x=10) y posicional ({10, 20}).
                        for (size_t i = 0; i < il->elements.size(); ++i) {
                            // Encontrar el StructFieldInfo correspondiente.
                            const StructFieldInfo *fld = nullptr;
                            if (il->is_designated && i < il->field_names.size()) {
                                const std::string &fname = il->field_names[i];
                                for (const auto &f : lay.fields) {
                                    if (f.name == fname) { fld = &f; break; }
                                }
                                if (!fld) {
                                    error_at(il->elements[i]->loc,
                                        "init list: campo '" + fname
                                        + "' no existe en struct '"
                                        + il->target_type_name + "'");
                                    continue;
                                }
                            } else {
                                if (i >= lay.fields.size()) {
                                    error_at(il->elements[i]->loc,
                                        "init list: demasiados elementos para struct '"
                                        + il->target_type_name + "'");
                                    continue;
                                }
                                fld = &lay.fields[i];
                            }
                            // Lower el valor.
                            const ir::IrValueId v_val = lower_expr(il->elements[i].get());
                            if (v_val == ir::IR_NO_VALUE) continue;
                            const ir::IrType ft = ir_type_from_primitive(fld->type.kind);
                            // Cast si hace falta (literal int -> i32 del field, etc.).
                            const ir::IrType vt_from = fn_->values[v_val].type;
                            const ir::IrValueId v_casted = cast_if_needed(v_val, vt_from, ft,
                                                                          il->elements[i]->loc.line,
                                                                          /*is_explicit=*/true);
                            // Calcular addr destino = v_host + fld->offset.
                            ir::IrValueId v_dst = v_host;
                            if (fld->offset > 0) {
                                const ir::IrValueId v_off = emit_const(ir::IrType::I64,
                                    static_cast<int64_t>(fld->offset), e->loc.line);
                                const ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
                                fn_->values[v_addr].is_host_ptr = true;
                                ir::IrInstr ad{};
                                ad.op          = ir::IrOp::ADD;
                                ad.type        = ir::IrType::I64;
                                ad.dst         = v_addr;
                                ad.operands    = {v_host, v_off};
                                ad.source_line = e->loc.line;
                                fn_->append(current_block_, std::move(ad));
                                v_dst = v_addr;
                            }
                            // STORE val at [v_dst].
                            ir::IrInstr st{};
                            st.op          = ir::IrOp::STORE;
                            st.type        = ft;
                            st.operands    = {v_casted, v_dst};
                            st.source_line = il->elements[i]->loc.line;
                            fn_->append(current_block_, std::move(st));
                        }
                        // 4. STORE host_ptr al slot+0 del unique<T>.
                        {
                            ir::IrInstr st{};
                            st.op          = ir::IrOp::STORE;
                            st.type        = ir::IrType::I64;
                            st.operands    = {v_host, v_slot};
                            st.source_line = e->loc.line;
                            fn_->append(current_block_, std::move(st));
                        }
                        // 5. STORE deleter=0 (sentinel RAW_FREE) al slot+8.
                        {
                            const ir::IrValueId v_eight = emit_const(ir::IrType::I64, 8, e->loc.line);
                            const ir::IrValueId v_slot8 = fn_->new_value(ir::IrType::PTR);
                            ir::IrInstr ad{};
                            ad.op          = ir::IrOp::ADD;
                            ad.type        = ir::IrType::I64;
                            ad.dst         = v_slot8;
                            ad.operands    = {v_slot, v_eight};
                            ad.source_line = e->loc.line;
                            fn_->append(current_block_, std::move(ad));
                            const ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0, e->loc.line);
                            ir::IrInstr st{};
                            st.op          = ir::IrOp::STORE;
                            st.type        = ir::IrType::I64;
                            st.operands    = {v_zero, v_slot8};
                            st.source_line = e->loc.line;
                            fn_->append(current_block_, std::move(st));
                        }
                        out_value = v_slot;
                        return true;
                    }
                }
            }
            const ir::IrValueId v_payload = lower_expr(e->args[0].get());
            if (v_payload == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrType payload_t = fn_->values[v_payload].type;
            // Determinar el tipo Vex semantico para saber si es struct value
            // (necesita memcpy a host heap) o si es CLASS/primitivo.
            const Type sem_payload = e->args[0]->result_type;
            const bool payload_is_struct_value =
                (sem_payload.kind == PrimitiveKind::STRUCT)
             && (tc_.struct_layouts().find(sem_payload.struct_name)
                 != tc_.struct_layouts().end());
            // sizeof(T): para primitivos usar ir_type_size; para structs
            // value-type consultar struct_layouts; para PTR/CLASS no se usa
            // (no alocamos memoria extra, guardamos el host_ptr directo).
            uint64_t payload_size = ir_type_size(payload_t);
            if (payload_is_struct_value) {
                payload_size = static_cast<uint64_t>(
                    tc_.struct_layouts().at(sem_payload.struct_name).size_bytes);
            }
            if (is_unique_box) {
                // unique<T> Tier 1 (16 bytes):
                //   [+0 i64 ptr][+8 i64 deleter_addr]
                // deleter_addr = 0 (sentinel) -> cleanup hace RAW_FREE.
                // Layout 16 bytes para que el deleter info sobreviva
                // cuando la funcion devuelve el unique<T> via SRET.
                // M7: si lower_return seteo target slot, usar el retbuf del
                // caller directamente (skip allocacion intermedia en stack).
                const ir::IrValueId v_slot =
                    (unique_box_target_slot_ != ir::IR_NO_VALUE)
                        ? unique_box_target_slot_
                        : stack_alloc_buf(16, e->loc.line);

                // Bug fix bug2: si el payload ya es un host_ptr (e.g.
                // `new Recurso(1)` devuelve PTR), guardarlo DIRECTAMENTE
                // en slot[+0] sin doble indireccion via RAW_ALLOC.  De
                // lo contrario, el cleanup CALLVIRT al destructor opera
                // sobre el malloc'd region (8 bytes basura) en vez del
                // objeto Recurso real, y el dtor nunca se invoca.
                //
                // Para primitivos (i32, f64, etc.) seguimos usando
                // RAW_ALLOC porque `ptr_of(p)` debe devolver `T*` (host
                // memory).  Para PTR el `ptr_of` devuelve el mismo
                // host_ptr almacenado.
                ir::IrValueId v_to_store = v_payload;
                if (payload_is_struct_value) {
                    // Struct value-type: RAW_ALLOC(sizeof_struct) + memcpy
                    // qword-by-qword desde v_payload (PTR al slot stack)
                    // hacia v_payload_ptr (host heap).  Asi el unique<T>
                    // ES dueno exclusivo de una copia en heap; el slot
                    // stack original puede morir al exit del scope sin
                    // afectar la copia.
                    const ir::IrValueId v_size = emit_const(ir::IrType::I64,
                        static_cast<int64_t>(payload_size), e->loc.line);
                    const ir::IrValueId v_payload_ptr = fn_->new_value(ir::IrType::PTR);
                    fn_->values[v_payload_ptr].is_host_ptr = true;
                    {
                        ir::IrInstr ins{};
                        ins.op          = ir::IrOp::RAW_ALLOC;
                        ins.type        = ir::IrType::PTR;
                        ins.dst         = v_payload_ptr;
                        ins.operands    = {v_size};
                        ins.source_line = e->loc.line;
                        fn_->append(current_block_, std::move(ins));
                    }
                    // Copy qword-by-qword (size redondeado hacia arriba a
                    // multiplos de 8 bytes; el ultimo qword puede tener
                    // padding pero no afecta correctness porque escribimos
                    // sobre RAW_ALLOC zero-init y leemos desde el slot
                    // ALLOCA que tiene tamano >= size_bytes).
                    const uint64_t qwords = (payload_size + 7) / 8;
                    for (uint64_t i = 0; i < qwords; ++i) {
                        const ir::IrValueId v_off = emit_const(ir::IrType::I64,
                            static_cast<int64_t>(i * 8), e->loc.line);
                        const ir::IrValueId v_src_p = fn_->new_value(ir::IrType::PTR);
                        {
                            ir::IrInstr ad{};
                            ad.op          = ir::IrOp::ADD;
                            ad.type        = ir::IrType::I64;
                            ad.dst         = v_src_p;
                            ad.operands    = {v_payload, v_off};
                            ad.source_line = e->loc.line;
                            fn_->append(current_block_, std::move(ad));
                        }
                        const ir::IrValueId v_word = fn_->new_value(ir::IrType::I64);
                        {
                            ir::IrInstr ld{};
                            ld.op          = ir::IrOp::LOAD;
                            ld.type        = ir::IrType::I64;
                            ld.dst         = v_word;
                            ld.operands    = {v_src_p};
                            ld.source_line = e->loc.line;
                            fn_->append(current_block_, std::move(ld));
                        }
                        const ir::IrValueId v_dst_p = fn_->new_value(ir::IrType::PTR);
                        fn_->values[v_dst_p].is_host_ptr = true;
                        {
                            ir::IrInstr ad{};
                            ad.op          = ir::IrOp::ADD;
                            ad.type        = ir::IrType::I64;
                            ad.dst         = v_dst_p;
                            ad.operands    = {v_payload_ptr, v_off};
                            ad.source_line = e->loc.line;
                            fn_->append(current_block_, std::move(ad));
                        }
                        {
                            ir::IrInstr st{};
                            st.op          = ir::IrOp::STORE;
                            st.type        = ir::IrType::I64;
                            st.operands    = {v_word, v_dst_p};
                            st.source_line = e->loc.line;
                            fn_->append(current_block_, std::move(st));
                        }
                    }
                    v_to_store = v_payload_ptr;
                } else if (payload_t != ir::IrType::PTR) {
                    // RAW_ALLOC(payload_size) -> v_payload_ptr (host ptr).
                    const ir::IrValueId v_size = emit_const(ir::IrType::I64,
                        static_cast<int64_t>(payload_size), e->loc.line);
                    const ir::IrValueId v_payload_ptr = fn_->new_value(ir::IrType::PTR);
                    fn_->values[v_payload_ptr].is_host_ptr = true;
                    {
                        ir::IrInstr ins{};
                        ins.op          = ir::IrOp::RAW_ALLOC;
                        ins.type        = ir::IrType::PTR;
                        ins.dst         = v_payload_ptr;
                        ins.operands    = {v_size};
                        ins.source_line = e->loc.line;
                        fn_->append(current_block_, std::move(ins));
                    }
                    // STORE payload at [v_payload_ptr] (host memory).
                    {
                        ir::IrInstr st{};
                        st.op          = ir::IrOp::STORE;
                        st.type        = payload_t;
                        st.operands    = {v_payload, v_payload_ptr};
                        st.source_line = e->loc.line;
                        fn_->append(current_block_, std::move(st));
                    }
                    v_to_store = v_payload_ptr;
                }
                // STORE v_to_store at [v_slot+0].
                //   Para primitivos: v_to_store = malloc'd ptr -> RAW_FREE valido.
                //   Para PTR (class/struct): v_to_store = host_ptr al objeto;
                //                            cleanup hace CALLVIRT dtor + skip free.
                {
                    ir::IrInstr st{};
                    st.op          = ir::IrOp::STORE;
                    st.type        = ir::IrType::I64;
                    st.operands    = {v_to_store, v_slot};
                    st.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(st));
                }
                // STORE deleter=0 at [v_slot+8] (sentinel = RAW_FREE).
                {
                    const ir::IrValueId v_eight = emit_const(ir::IrType::I64, 8, e->loc.line);
                    const ir::IrValueId v_slot8 = fn_->new_value(ir::IrType::PTR);
                    ir::IrInstr add{};
                    add.op          = ir::IrOp::ADD;
                    add.type        = ir::IrType::I64;
                    add.dst         = v_slot8;
                    add.operands    = {v_slot, v_eight};
                    add.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(add));
                    const ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0, e->loc.line);
                    ir::IrInstr st{};
                    st.op          = ir::IrOp::STORE;
                    st.type        = ir::IrType::I64;
                    st.operands    = {v_zero, v_slot8};
                    st.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(st));
                }
                fn_->values[v_slot].pointee_is_host_ptr = true;
                out_value = v_slot;
                return true;
            } else {
                // shared<T>: gcallocp(16 + 8) - control block + payload inline.
                // Layout: [+0 i64 refcount=1][+8 u64 deleter=0][+16 T payload].
                // El slot stack guarda host_ptr al control block.
                const ir::IrValueId v_slot = stack_alloc_buf(8, e->loc.line);
                const ir::IrValueId v_ctrl_size = emit_const(ir::IrType::I64,
                    16 + 8, e->loc.line); // 24 bytes total
                // gcallocp -> host_ptr a payload
                const ir::IrValueId v_ctrl = emit_gc_allocp(v_ctrl_size, e->loc.line);
                // STORE refcount=1 at [v_ctrl + 0].
                {
                    const ir::IrValueId v_one = emit_const(ir::IrType::I64, 1, e->loc.line);
                    ir::IrInstr st{};
                    st.op          = ir::IrOp::STORE;
                    st.type        = ir::IrType::I64;
                    st.operands    = {v_one, v_ctrl};
                    st.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(st));
                }
                // STORE deleter=0 at [v_ctrl + 8] (placeholder; cleanup usa free literal).
                {
                    const ir::IrValueId v_eight = emit_const(ir::IrType::I64, 8, e->loc.line);
                    const ir::IrValueId v_ctrl8 = fn_->new_value(ir::IrType::PTR);
                    fn_->values[v_ctrl8].is_host_ptr = true;
                    ir::IrInstr add{};
                    add.op          = ir::IrOp::ADD;
                    add.type        = ir::IrType::I64;
                    add.dst         = v_ctrl8;
                    add.operands    = {v_ctrl, v_eight};
                    add.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(add));
                    const ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0, e->loc.line);
                    ir::IrInstr st{};
                    st.op          = ir::IrOp::STORE;
                    st.type        = ir::IrType::I64;
                    st.operands    = {v_zero, v_ctrl8};
                    st.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(st));
                }
                // STORE payload at [v_ctrl + 16].
                {
                    const ir::IrValueId v_sixteen = emit_const(ir::IrType::I64, 16, e->loc.line);
                    const ir::IrValueId v_ctrl16  = fn_->new_value(ir::IrType::PTR);
                    fn_->values[v_ctrl16].is_host_ptr = true;
                    ir::IrInstr add{};
                    add.op          = ir::IrOp::ADD;
                    add.type        = ir::IrType::I64;
                    add.dst         = v_ctrl16;
                    add.operands    = {v_ctrl, v_sixteen};
                    add.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(add));
                    ir::IrInstr st{};
                    st.op          = ir::IrOp::STORE;
                    st.type        = payload_t;
                    st.operands    = {v_payload, v_ctrl16};
                    st.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(st));
                }
                // STORE v_ctrl at [v_slot] (VM memory).
                {
                    ir::IrInstr st{};
                    st.op          = ir::IrOp::STORE;
                    st.type        = ir::IrType::I64;
                    st.operands    = {v_ctrl, v_slot};
                    st.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(st));
                }
                fn_->values[v_slot].pointee_is_host_ptr = true;
                out_value = v_slot;
                return true;
            }
        }

        // ----- unique_with(value, deleter_fn) / shared_with(...) -----
        // Forma generica donde el programador especifica el deleter.
        // No se hace alloc: el value es el RESULTADO de una alocacion ya
        // hecha (VirtualAlloc, malloc, fopen, socket(), etc.).  El
        // cleanup en scope exit invoca deleter_fn(value) automaticamente.
        //
        // Layout: ALLOCA 8 (slot) + STORE value at [slot].  Cleanup:
        // LOAD ptr; if (ptr != 0) CALL deleter(ptr); zero slot.
        //
        // El nombre del deleter se almacena en CleanupAction::literal_deleter
        // con prefijo "@extern:lib:fn" si es extern, o el nombre puro si
        // es Vesta.  El emit_cleanups_all elige CALLN o CALLVM.
        if (is_unique_with || is_shared_with) {
            if (e->args.size() != 2) {
                error_at(e->loc, name + ": requiere 2 argumentos");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_payload = lower_expr(e->args[0].get());
            if (v_payload == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // Validar que arg[1] sea IdentExpr (type_checker ya lo verifico).
            if (e->args[1]->kind != ast::NodeKind::IdentExpr) {
                error_at(e->args[1]->loc,
                    name + ": el deleter debe ser un identificador de funcion");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const auto *deleter_id = static_cast<const ast::IdentExpr *>(e->args[1].get());
            // Capturamos el nombre del deleter; el cleanup lo usara.
            std::string deleter_label = tc_.lookup_extern_qualified(deleter_id->name);
            if (deleter_label.empty()) {
                // No es extern -> es funcion Vesta.  Usamos el nombre puro;
                // el cleanup emitira CALLVM @Absolute("code.<name>").
                deleter_label = deleter_id->name;
            } // else: ya viene con prefijo "@extern:lib:fn".
            // Tier 1: ALLOCA 16 + STORE value@+0 + STORE deleter_addr@+8.
            // El deleter_addr se materializa via RAW_ASM que captura
            // `@Absolute("code.<name>")` (Vesta) o un puntero null marcador
            // (extern, no soportado en SRET return aun).
            const ir::IrValueId v_slot = stack_alloc_buf(16, e->loc.line);
            {
                ir::IrInstr st{};
                st.op          = ir::IrOp::STORE;
                st.type        = ir::IrType::I64;
                st.operands    = {v_payload, v_slot};
                st.source_line = e->loc.line;
                fn_->append(current_block_, std::move(st));
            }
            // STORE deleter address en slot+8.  Materializamos la
            // direccion via RAW_ASM: `mov {dst}, @Absolute("code.<fn>")`.
            // El assembler resuelve la direccion al linker time.
            //
            // Limitacion: para deleters extern no podemos obtener una
            // direccion vesta-callable, por lo que usamos 0 (sentinel)
            // y el cleanup local conoce el deleter por compile-time via
            // literal_deleter.  SRET return con extern deleter no
            // preserva la info (futuro: anyadir tabla de deleter ids).
            const ir::IrValueId v_deleter_addr = fn_->new_value(ir::IrType::I64);
            if (deleter_label.rfind("@extern:", 0) == 0) {
                // Extern: no podemos materializar direccion como Vesta
                // function; almacenamos 0 y dependemos del literal_deleter
                // local para hacer el call correcto.  No sobrevive SRET.
                const ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0, e->loc.line);
                ir::IrInstr mov{};
                mov.op          = ir::IrOp::MOV;
                mov.type        = ir::IrType::I64;
                mov.dst         = v_deleter_addr;
                mov.operands    = {v_zero};
                mov.source_line = e->loc.line;
                fn_->append(current_block_, std::move(mov));
            } else {
                // Vesta: emitir LABEL_ADDR -> v_deleter_addr.
                ir::IrValueId v_label = emit_label_addr(deleter_label, e->loc.line);
                ir::IrInstr   mov{};
                mov.op          = ir::IrOp::MOV;
                mov.type        = ir::IrType::I64;
                mov.dst         = v_deleter_addr;
                mov.operands    = {v_label};
                mov.source_line = e->loc.line;
                fn_->append(current_block_, std::move(mov));
            }
            // STORE deleter_addr en slot+8.
            {
                const ir::IrValueId v_eight = emit_const(ir::IrType::I64, 8, e->loc.line);
                const ir::IrValueId v_slot8 = fn_->new_value(ir::IrType::PTR);
                ir::IrInstr add{};
                add.op          = ir::IrOp::ADD;
                add.type        = ir::IrType::I64;
                add.dst         = v_slot8;
                add.operands    = {v_slot, v_eight};
                add.source_line = e->loc.line;
                fn_->append(current_block_, std::move(add));
                ir::IrInstr st{};
                st.op          = ir::IrOp::STORE;
                st.type        = ir::IrType::I64;
                st.operands    = {v_deleter_addr, v_slot8};
                st.source_line = e->loc.line;
                fn_->append(current_block_, std::move(st));
            }
            // El slot contiene un valor con semantica de host_ptr / handle.
            fn_->values[v_slot].pointee_is_host_ptr = true;
            // Anotamos la accion de cleanup pendiente para que el cleanup
            // local pueda usar el deleter por compile-time (cero overhead).
            // El cleanup dinamico via slot+8 solo se activa cuando se
            // accede al smart pointer tras SRET (no tenemos info compile-time).
            pending_smartptr_deleter_ = deleter_label;
            out_value = v_slot;
            return true;
        }

        // ----- move(p) -----  transfer ownership.
        // El destino de move es el LHS del var-decl o de la asignacion;
        // este builtin SOLO marca el SSA value como "consumed".  El
        // codigo del mvtake real se emite en lower_var_decl cuando ve
        // que el init es CallExpr(move(...)).  Aqui devolvemos el slot
        // del origen tal cual: el lower_var_decl tomara responsabilidad
        // de emitir mvtake y zerificar el slot del origen.
        if (is_move) {
            if (e->args.size() != 1) {
                error_at(e->loc, "move: requiere 1 argumento");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_arg = lower_expr(e->args[0].get());
            // No emitimos mvtake aqui; lower_var_decl detecta el patron
            // CallExpr(move(...)) y emite la secuencia correcta.
            out_value = v_arg;
            return true;
        }

        // ----- Z.6: is_shared(obj) -> bool -----
        // Pipeline IR puro:
        //   1. RAW_ASM gchandle dst,src  -> dst = uint32 handle.
        //   2. CONST u64 0x80000000.
        //   3. IrOp::AND_BIT handle & MASK.
        //   4. IrOp::SHR_U result, 31 -> resultado en bit 0 (0 o 1).
        //   5. cast a bool.
        if (is_z6_isshared) {
            if (e->args.size() != 1) {
                error_at(e->loc, "is_shared: requiere 1 argumento");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_obj = lower_expr(e->args[0].get());
            if (v_obj == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // 1) gchandle obj -> handle (u64)
            const ir::IrValueId v_h = emit_gc_handle_for_ptr(v_obj, e->loc.line);
            // 2) const mask
            const ir::IrValueId v_mask =
                emit_const(ir::IrType::U64, 0x80000000ULL, e->loc.line);
            // 3) handle & mask
            const ir::IrValueId v_and = fn_->new_value(ir::IrType::U64);
            {
                ir::IrInstr ins{};
                ins.op          = ir::IrOp::AND;
                ins.type        = ir::IrType::U64;
                ins.dst         = v_and;
                ins.operands    = {v_h, v_mask};
                ins.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ins));
            }
            // 4) shr 31 -> bit 0 = 0|1
            const ir::IrValueId v_shift =
                emit_const(ir::IrType::U64, 31ULL, e->loc.line);
            const ir::IrValueId v_bit = fn_->new_value(ir::IrType::U64);
            {
                ir::IrInstr ins{};
                ins.op          = ir::IrOp::SHR;
                ins.type        = ir::IrType::U64;
                ins.dst         = v_bit;
                ins.operands    = {v_and, v_shift};
                ins.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ins));
            }
            // 5) cast a bool
            const ir::IrValueId v_bool =
                cast_if_needed(v_bit, ir::IrType::U64, ir::IrType::BOOL,
                                e->loc.line, /*explicit=*/true);
            out_value = v_bool;
            return true;
        }

        // ----- Z.7: share(obj) -> obj (in-place promotion via gcpromote) -----
        // Emite el opcode bytecode @c gcpromote (extended 0xA7) que aloca
        // en el SharedHeap, copia el objeto, registra en SharedHandleTable
        // y devuelve el nuevo host_ptr.  Si el objeto ya esta shared
        // (bit 31 en hash_code), el opcode es no-op idempotente.
        //
        // NOTA: el resultado de share() es una NUEVA referencia.  El
        // original sigue en el gc_heap local (se libera por sweep).
        // Patron correcto en Vex:
        //   Counter c = new Counter();
        //   c = share(c);                  // c ahora apunta a la copia shared
        if (is_z6_share) {
            if (e->args.size() != 1) {
                error_at(e->loc, "share: requiere 1 argumento");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_obj = lower_expr(e->args[0].get());
            if (v_obj == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_new = emit_gc_promote(v_obj, e->loc.line);
            fn_->values[v_new].is_gc_object = true; // marca de host_ptr a payload GC
            out_value = v_new;
            return true;
        }

        // ----- Z.7: unshare(obj) -> obj (deep copy a heap local) -----
        // Emite @c gcdemote (extended 0xA8).  Idempotente si NO es shared.
        if (is_z6_unshare) {
            if (e->args.size() != 1) {
                error_at(e->loc, "unshare: requiere 1 argumento");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_obj = lower_expr(e->args[0].get());
            if (v_obj == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_new = emit_gc_demote(v_obj, e->loc.line);
            fn_->values[v_new].is_gc_object = true;
            out_value = v_new;
            return true;
        }

        // ============================================================
        // Z.8 atomic primitives: bajan a opcodes atomicld/st/cas/add.
        // El host_ptr es un i64 que se interpreta como direccion
        // absoluta del host (no VM addr).  El usuario es responsable
        // de la alineacion a 8 bytes para lock-free.
        // ============================================================

        // atomic_load_i64(ptr) -> i64
        if (is_z8_atomic_load) {
            if (e->args.size() != 1) {
                error_at(e->loc, "atomic_load_i64: requiere 1 argumento");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_ptr = lower_expr(e->args[0].get());
            out_value = emit_atomic_ld_i64(v_ptr, e->loc.line);
            return true;
        }

        // atomic_store_i64(ptr, val) -> void
        if (is_z8_atomic_store) {
            if (e->args.size() != 2) {
                error_at(e->loc, "atomic_store_i64: requiere 2 argumentos");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_ptr = lower_expr(e->args[0].get());
            const ir::IrValueId v_val = lower_expr(e->args[1].get());
            emit_atomic_st_i64(v_ptr, v_val, e->loc.line);
            out_value = ir::IR_NO_VALUE;
            return true;
        }

        // atomic_cas_i64(ptr, exp, des) -> i64 (old value)
        if (is_z8_atomic_cas) {
            if (e->args.size() != 3) {
                error_at(e->loc, "atomic_cas_i64: requiere 3 argumentos");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_ptr = lower_expr(e->args[0].get());
            const ir::IrValueId v_exp = lower_expr(e->args[1].get());
            const ir::IrValueId v_des = lower_expr(e->args[2].get());
            out_value = emit_atomic_cas_i64(v_ptr, v_exp, v_des, e->loc.line);
            return true;
        }

        // atomic_add_i64(ptr, delta) -> i64 (old value)
        if (is_z8_atomic_add) {
            if (e->args.size() != 2) {
                error_at(e->loc, "atomic_add_i64: requiere 2 argumentos");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_ptr   = lower_expr(e->args[0].get());
            const ir::IrValueId v_delta = lower_expr(e->args[1].get());
            out_value = emit_atomic_add_i64(v_ptr, v_delta, e->loc.line);
            return true;
        }

        // shared_malloc(size) -> i64* (host_ptr).  Implementacion via CALLN
        // a un wrapper que usa @c vm.shared_heap.alloc.  Por ahora
        // delegamos al @c malloc estandar (raw_allocator) que tambien da
        // memoria host accesible cross-process (mismo address space).
        if (is_z8_shared_malloc) {
            if (e->args.size() != 1) {
                error_at(e->loc, "shared_malloc: requiere 1 argumento");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_size = lower_expr(e->args[0].get());
            const ir::IrValueId v_ptr = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_ptr].is_host_ptr = true;
            // Reuse @c alloc opcode (RAW_ALLOC IR op).  La memoria que
            // retorna es host_ptr, identico en todos los procesos (mismo
            // address space del OS).  Para promocion al SharedHeap real
            // (con tracking de GC), usar @c share() en el objeto creado.
            ir::IrInstr ins{};
            ins.op          = ir::IrOp::RAW_ALLOC;
            ins.type        = ir::IrType::PTR;
            ins.dst         = v_ptr;
            ins.operands    = {v_size};
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            out_value = v_ptr;
            return true;
        }

        // shared_free(ptr) -> void
        if (is_z8_shared_free) {
            if (e->args.size() != 1) {
                error_at(e->loc, "shared_free: requiere 1 argumento");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_ptr = lower_expr(e->args[0].get());
            ir::IrInstr ins{};
            ins.op          = ir::IrOp::RAW_FREE;
            ins.type        = ir::IrType::VOID;
            ins.dst         = ir::IR_NO_VALUE;
            ins.operands    = {v_ptr};
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            out_value = ir::IR_NO_VALUE;
            return true;
        }

        // Z.10: shared_heap_live_count() / shared_heap_bytes() / shared_gc_collect()
        // Lowering comun: const op + sharedstat opcode.
        if (is_z10_live_count || is_z10_bytes || is_z10_gc_collect) {
            const int op_code = is_z10_live_count ? 0
                              : is_z10_bytes      ? 1
                                                  : 2;
            const ir::IrType ret_type = is_z10_bytes
                ? ir::IrType::U64
                : (is_z10_live_count ? ir::IrType::U32 : ir::IrType::VOID);
            // raw_asm-elim wave 3: SHARED_STAT IR op dedicado.  El emit
            // del bytecode usa `r14` como dst dummy para el caso VOID
            // (op_code=2 gc_collect) automaticamente.
            const ir::IrValueId v_op = emit_const(
                ir::IrType::I32, (uint64_t)op_code, e->loc.line);
            const ir::IrValueId v_dst = (ret_type == ir::IrType::VOID)
                ? ir::IR_NO_VALUE
                : fn_->new_value(ret_type);
            ir::IrInstr ss{};
            ss.op           = ir::IrOp::SHARED_STAT;
            ss.type         = ret_type;
            ss.dst          = v_dst;
            ss.operands     = {v_op};
            ss.source_line  = e->loc.line;
            fn_->append(current_block_, std::move(ss));
            out_value = v_dst;
            return true;
        }

        // ----- ptr_of(p) -----  T* host, sin consumir el smart pointer.
        // unique<T>: LOAD ptr from [slot]; resultado is_host_ptr.
        // shared<T>: LOAD ctrl from [slot]; ADD 16; resultado is_host_ptr.
        if (is_get) {
            if (e->args.size() != 1) {
                error_at(e->loc, "ptr_of: requiere 1 argumento");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const Type        arg_t = e->args[0]->result_type;
            const ir::IrValueId v_slot = lower_expr(e->args[0].get());
            if (v_slot == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // LOAD ptr from [v_slot].
            const ir::IrValueId v_ptr = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_ptr].is_host_ptr = true;
            // BugFix R2: si el inner T es CLASS, marcar is_gc_object=true
            // para que el regalloc trate al SSA value como handle GC y
            // preserve su naturaleza a traves de CALLVIRTs (el GC scan
            // pratico de A.34.fix8 lo encuentra como root via stack).
            const bool inner_is_class =
                arg_t.pointee && arg_t.pointee->kind == PrimitiveKind::CLASS;
            if (inner_is_class) {
                fn_->values[v_ptr].is_gc_object = true;
            }
            {
                ir::IrInstr ld{};
                ld.op          = ir::IrOp::LOAD;
                ld.type        = ir::IrType::I64;
                ld.dst         = v_ptr;
                ld.operands    = {v_slot};
                ld.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ld));
            }
            if (arg_t.kind == PrimitiveKind::SHARED_PTR) {
                // shared<T>: payload esta en +16 del control block.
                const ir::IrValueId v_sixteen = emit_const(ir::IrType::I64, 16, e->loc.line);
                const ir::IrValueId v_pay = fn_->new_value(ir::IrType::PTR);
                fn_->values[v_pay].is_host_ptr = true;
                ir::IrInstr add{};
                add.op          = ir::IrOp::ADD;
                add.type        = ir::IrType::I64;
                add.dst         = v_pay;
                add.operands    = {v_ptr, v_sixteen};
                add.source_line = e->loc.line;
                fn_->append(current_block_, std::move(add));
                // BugFix R2: si inner es CLASS, el slot @+16 guarda el
                // host_ptr al objeto.  Hacer otro LOAD para obtenerlo y
                // marcarlo como is_gc_object para CALLVIRT.  Sin esto,
                // ptr_of(shared<Class>).method() recibia el addr del SLOT
                // (no del Class) -> CALLVIRT con this invalido.
                if (inner_is_class) {
                    const ir::IrValueId v_obj = fn_->new_value(ir::IrType::PTR);
                    fn_->values[v_obj].is_host_ptr = true;
                    fn_->values[v_obj].is_gc_object = true;
                    ir::IrInstr ld2{};
                    ld2.op          = ir::IrOp::LOAD;
                    ld2.type        = ir::IrType::I64;
                    ld2.dst         = v_obj;
                    ld2.operands    = {v_pay};
                    ld2.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(ld2));
                    out_value = v_obj;
                    return true;
                }
                out_value = v_pay;
                return true;
            }
            // unique<T>: ptr ES el payload.
            out_value = v_ptr;
            return true;
        }

        // ----- use_count(s) -----  i64 refcount del shared<T>.
        if (is_use_count) {
            if (e->args.size() != 1) {
                error_at(e->loc, "use_count: requiere 1 argumento");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_slot = lower_expr(e->args[0].get());
            if (v_slot == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // LOAD ctrl from [slot].
            const ir::IrValueId v_ctrl = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_ctrl].is_host_ptr = true;
            {
                ir::IrInstr ld{};
                ld.op          = ir::IrOp::LOAD;
                ld.type        = ir::IrType::I64;
                ld.dst         = v_ctrl;
                ld.operands    = {v_slot};
                ld.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ld));
            }
            // LOAD refcount from [ctrl + 0] (host memory).
            const ir::IrValueId v_rc = fn_->new_value(ir::IrType::I64);
            {
                ir::IrInstr ld{};
                ld.op          = ir::IrOp::LOAD;
                ld.type        = ir::IrType::I64;
                ld.dst         = v_rc;
                ld.operands    = {v_ctrl};
                ld.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ld));
            }
            out_value = v_rc;
            return true;
        }

        // =====================================================================
        // Borrow checker builtins: lend / lend_mut / read_borrow / write_borrow.
        // =====================================================================
        //
        // El borrow checker compile-time ya valido las reglas R1-R4.  El
        // lowering solo emite el codigo correspondiente con cero overhead
        // vs un raw pointer:
        //
        //   lend(owner)       -> ptr_of equivalente al unique<T>/shared<T>.
        //                        Para owner que NO es smart pointer (var
        //                        local plain), emite &owner via slot stack.
        //   lend_mut(owner)   -> mismo bytecode que lend; la distincion
        //                        es puramente compile-time.
        //   read_borrow(b)    -> LOAD a traves del host_ptr (movh).
        //   write_borrow(m,v) -> STORE a traves del host_ptr (movh).
        if (is_lend || is_lend_mut) {
            if (e->args.size() != 1) {
                error_at(e->loc, name + ": requiere 1 argumento (owner)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // Si el owner es unique<T>/shared<T>, equivale a ptr_of(owner)
            // que carga slot+0.  Si es una variable plain, devolvemos
            // &owner (su SSA value, que ya tiene is_host_ptr correcto
            // gracias al pre-pase de address-taken promotion: cualquier
            // local cuya direccion se toma con @c & se promociona a slot
            // estable en stack en lugar de vivir solo en un SSA value).
            const Type owner_t = e->args[0]->result_type;
            // F3 reborrow: si el arg ES un borrow/borrow_mut (var o param),
            // su SSA value YA ES el host_ptr al payload.  No queremos
            // emitir LOAD via read_local (eso es para slots de unique).
            // Bypass: usar lookup directamente cuando el arg es un
            // IdentExpr cuyo tipo es borrow.
            if (e->args[0]->kind == ast::NodeKind::IdentExpr
             && (owner_t.kind == PrimitiveKind::BORROW
              || owner_t.kind == PrimitiveKind::BORROW_MUT)) {
                auto *id = static_cast<ast::IdentExpr *>(e->args[0].get());
                const ir::IrValueId v = lookup(id->name);
                if (v != ir::IR_NO_VALUE) {
                    // El borrow_var ya es host_ptr; lo devolvemos tal cual.
                    // (read_borrow/write_borrow lo usaran con movh.)
                    out_value = v;
                    return true;
                }
            }
            // Owner plain (i32, i64, etc.): si es un IdentExpr de variable
            // address-taken, el SSA value del scope es la DIRECCION del
            // ALLOCA (no el valor).  Hacer lower_expr pasaria por lower_ident
            // que para address-taken locals emite un LOAD i32 (devuelve el
            // VALOR), corrompiendo read_borrow/write_borrow posteriores.
            // Bypass: lookup directo cuando el arg es IdentExpr y la var
            // esta address-taken (lo cual el scan_address_taken garantiza
            // que sea verdad para `lend(local)` en owner plain).
            if (e->args[0]->kind == ast::NodeKind::IdentExpr) {
                auto *id = static_cast<ast::IdentExpr *>(e->args[0].get());
                if (address_taken_locals_.count(id->name)) {
                    const ir::IrValueId v = lookup(id->name);
                    if (v != ir::IR_NO_VALUE) {
                        // v es PTR al slot del local.  Marcamos is_host_ptr
                        // (es un slot vm-mem en stack, pero la convencion
                        // para borrows es host_ptr; read_borrow/write_borrow
                        // emiten LOAD/STORE indirecto con movh sobre este).
                        // En realidad el slot vive en vm_mem (ALLOCA),
                        // asi que NO marcamos is_host_ptr aqui: los LOAD/
                        // STORE de read/write_borrow ya consultan eso del
                        // SSA value y emiten mov (no movh) si es slot VM.
                        out_value = v;
                        return true;
                    }
                }
            }
            const ir::IrValueId v_arg = lower_expr(e->args[0].get());
            if (v_arg == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            if (owner_t.kind == PrimitiveKind::UNIQUE_PTR
             || owner_t.kind == PrimitiveKind::SHARED_PTR) {
                // LOAD slot+0 (para unique) o ctrl+16 (shared payload).
                const ir::IrValueId v_ptr = fn_->new_value(ir::IrType::PTR);
                fn_->values[v_ptr].is_host_ptr = true;
                ir::IrInstr ld{};
                ld.op          = ir::IrOp::LOAD;
                ld.type        = ir::IrType::I64;
                ld.dst         = v_ptr;
                ld.operands    = {v_arg};
                ld.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ld));
                if (owner_t.kind == PrimitiveKind::SHARED_PTR) {
                    // Para shared, sumar 16 (offset del payload inline en
                    // ctrl_block: refcount@0 + deleter@8 + payload@16).
                    const ir::IrValueId v_sixteen =
                        emit_const(ir::IrType::I64, 16, e->loc.line);
                    const ir::IrValueId v_pay = fn_->new_value(ir::IrType::PTR);
                    fn_->values[v_pay].is_host_ptr = true;
                    ir::IrInstr add{};
                    add.op          = ir::IrOp::ADD;
                    add.type        = ir::IrType::I64;
                    add.dst         = v_pay;
                    add.operands    = {v_ptr, v_sixteen};
                    add.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(add));
                    out_value = v_pay;
                    return true;
                }
                out_value = v_ptr;
                return true;
            }
            // owner plain: el SSA value ya es la direccion (address-taken).
            // Lo devolvemos tal cual.
            out_value = v_arg;
            return true;
        }
        if (is_read_borrow) {
            if (e->args.size() != 1) {
                error_at(e->loc, "read_borrow: requiere 1 argumento (borrow)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_b = lower_expr(e->args[0].get());
            if (v_b == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // El borrow es un host_ptr.  Marcamos is_host_ptr=true.
            // Para borrow derivado de unique<T>/shared<T> ya lo esta;
            // para `lend(local_plain)` el local fue promocionado a host
            // heap (RAW_ALLOC) en el lowering de su var-decl, asi que
            // el slot ES un host_ptr.  Param borrows tambien son host
            // por convencion (caller responsabilidad).
            fn_->values[v_b].is_host_ptr = true;
            // B2 fix: para STRUCT (y otros tipos cuyo "valor" SSA es PTR
            // a buffer: ARRAY/OPTIONAL/RESULT/CLASS), pass-through del
            // host_ptr al struct.  Sin esto, LOAD payload_t=PTR cargaria
            // los primeros 8 bytes del struct como si fueran otro ptr
            // (mismo bug que tenia el Deref).  Con pass-through, el
            // caller puede hacer (read_borrow(b)).x que baja a ADD off +
            // LOAD i32 correctamente.
            const Type inner = e->args[0]->result_type.pointee
                ? *e->args[0]->result_type.pointee : Type{};
            if (inner.kind == PrimitiveKind::STRUCT
             || inner.kind == PrimitiveKind::ARRAY
             || inner.kind == PrimitiveKind::OPTIONAL
             || inner.kind == PrimitiveKind::RESULT
             || inner.kind == PrimitiveKind::CLASS) {
                out_value = v_b;
                return true;
            }
            // LOAD T from [v_b] para tipos escalares.
            const ir::IrType payload_t = ir_type_from_primitive(inner.kind);
            const ir::IrValueId v_dst = fn_->new_value(payload_t);
            ir::IrInstr ld{};
            ld.op          = ir::IrOp::LOAD;
            ld.type        = payload_t;
            ld.dst         = v_dst;
            ld.operands    = {v_b};
            ld.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ld));
            out_value = v_dst;
            return true;
        }
        if (is_write_borrow) {
            if (e->args.size() != 2) {
                error_at(e->loc, "write_borrow: requiere 2 argumentos (borrow_mut, value)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_b = lower_expr(e->args[0].get());
            const ir::IrValueId v_v = lower_expr(e->args[1].get());
            if (v_b == ir::IR_NO_VALUE || v_v == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            fn_->values[v_b].is_host_ptr = true;
            const Type inner = e->args[0]->result_type.pointee
                ? *e->args[0]->result_type.pointee : Type{};
            const ir::IrType payload_t = ir_type_from_primitive(inner.kind);
            ir::IrInstr st{};
            st.op          = ir::IrOp::STORE;
            st.type        = payload_t;
            st.operands    = {v_v, v_b};
            st.source_line = e->loc.line;
            fn_->append(current_block_, std::move(st));
            out_value = ir::IR_NO_VALUE;
            return true;
        }

        // ----- pid() -----
        // Devuelve el PID encoded del proceso actual via getpid r_dst.
        if (is_pid) {
            if (!e->args.empty()) {
                error_at(e->loc, "pid: no acepta argumentos");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            out_value = emit_getpid(e->loc.line);
            return true;
        }

        // ----- args_count() -> i32 -----
        // Devuelve el numero de argumentos del script (vm->script_args.size()).
        // Baja a `getargc r_dst`, deposita uint64 que el caller trunca a i32.
        if (is_args_count) {
            if (!e->args.empty()) {
                error_at(e->loc, "args_count: no acepta argumentos");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // getargc devuelve i64 a nivel IR; truncamos a i32 si el caller lo espera.
            ir::IrValueId v_n = emit_getargc(e->loc.line);
            v_n = cast_if_needed(v_n, ir::IrType::I64, ir::IrType::I32,
                                  e->loc.line, /*is_explicit=*/true);
            out_value = v_n;
            return true;
        }

        // ----- Builtins de terminal / VT100 -----
        // Cada uno emite via vio_print una secuencia ANSI estatica.
        // term_move(row, col) requiere format dinamico: emite la secuencia
        // como una mezcla de prints y print_int.  Sin overhead extra
        // gracias al buffer global de 64 KB del plugin vesta_io (todos
        // los prints en una misma frame se agrupan en 1 syscall).
        if (is_term_clear) {
            if (!e->args.empty()) {
                error_at(e->loc, "term_clear: no acepta argumentos");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            emit_print_string_literal("\x1b[2J\x1b[H", e->loc.line);
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        if (is_term_clear_line) {
            if (!e->args.empty()) {
                error_at(e->loc, "term_clear_line: no acepta argumentos");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            emit_print_string_literal("\x1b[2K", e->loc.line);
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        if (is_term_save_cursor) {
            emit_print_string_literal("\x1b[s", e->loc.line);
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        if (is_term_restore_cursor) {
            emit_print_string_literal("\x1b[u", e->loc.line);
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        if (is_term_hide_cursor) {
            emit_print_string_literal("\x1b[?25l", e->loc.line);
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        if (is_term_show_cursor) {
            emit_print_string_literal("\x1b[?25h", e->loc.line);
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        if (is_term_reset) {
            // Reset all attributes (color, style, bg, fg).
            emit_print_string_literal("\x1b[0m", e->loc.line);
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        if (is_term_move) {
            if (e->args.size() != 2 || !e->args[0] || !e->args[1]) {
                error_at(e->loc, "term_move: requiere 2 argumentos (row, col)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // Emite "\x1b[" + row + ";" + col + "H" usando print + print_int.
            emit_print_string_literal("\x1b[", e->loc.line);
            // Sintetizar print_int(row) y print_int(col) reusando
            // try_lower_builtin_call con args sintetizados.
            for (int i = 0; i < 2; ++i) {
                const ir::IrValueId v = lower_expr(e->args[i].get());
                if (v == ir::IR_NO_VALUE) {
                    out_value = ir::IR_NO_VALUE;
                    return true;
                }
                // CALLN vio_print_int(v).
                out_mod_->register_native_import(
                    std::string("stdlib/native/io/vesta_io"), "vio_print_int");
                ir::IrInstr ins{};
                ins.op          = ir::IrOp::CALLN;
                ins.type        = ir::IrType::VOID;
                ins.dst         = ir::IR_NO_VALUE;
                ins.func_name   = "stdlib/native/io/vesta_io:vio_print_int";
                ins.operands.push_back(v);
                ins.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ins));
                if (i == 0) {
                    emit_print_string_literal(";", e->loc.line);
                }
            }
            emit_print_string_literal("H", e->loc.line);
            out_value = ir::IR_NO_VALUE;
            return true;
        }

        // ----- getMethods(cls) / getFields(cls) -> i32 -----
        // Devuelve el numero de metodos / fields de instancia de la clase
        // via los opcodes existentes methodcount (0xDB) / fieldcount (0xDA).
        // Ambos depositan el count en R00 (no toman r_dst); capturamos a SSA.
        if (is_getMethods || is_getFields) {
            if (e->args.size() != 1 || !e->args[0]) {
                error_at(e->loc, std::string(is_getMethods ? "getMethods" : "getFields")
                    + ": requiere 1 argumento (cls)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_cls = lower_expr(e->args[0].get());
            if (v_cls == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // raw_asm-elim wave 2: REFLECT_COUNT con imm=0(methods) o 1(fields).
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I32);
            ir::IrInstr rc{};
            rc.op           = ir::IrOp::REFLECT_COUNT;
            rc.type         = ir::IrType::I32;
            rc.dst          = v_dst;
            rc.operands     = {v_cls};
            rc.imm          = is_getMethods ? 0 : 1;
            rc.source_line  = e->loc.line;
            fn_->append(current_block_, std::move(rc));
            out_value = v_dst;
            return true;
        }

        // ----- getMethodAt(cls, i) / getFieldAt(cls, i) -> i64 -----
        // Devuelve &cls->methods[i] / &cls->fields[i] via los opcodes nuevos
        // getmethat / getfldat (0x6E / 0x6F, variante reg-reg de getmethod /
        // getfield).  Ambos depositan el puntero (MethodInfo* / FieldInfo*)
        // en R00, o 0 si i fuera de rango / cls nulo.
        if (is_getMethodAt || is_getFieldAt) {
            if (e->args.size() != 2 || !e->args[0] || !e->args[1]) {
                error_at(e->loc, std::string(is_getMethodAt ? "getMethodAt" : "getFieldAt")
                    + ": requiere 2 argumentos (cls, i)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_cls = lower_expr(e->args[0].get());
            const ir::IrValueId v_idx = lower_expr(e->args[1].get());
            if (v_cls == ir::IR_NO_VALUE || v_idx == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // raw_asm-elim wave 2: REFLECT_AT con imm=0(method_at) o 1(field_at).
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr ra{};
            ra.op           = ir::IrOp::REFLECT_AT;
            ra.type         = ir::IrType::I64;
            ra.dst          = v_dst;
            ra.operands     = {v_cls, v_idx};
            ra.imm          = is_getMethodAt ? 0 : 1;
            ra.source_line  = e->loc.line;
            fn_->append(current_block_, std::move(ra));
            out_value = v_dst;
            return true;
        }

        // ----- args_get(i) -> string -----
        // Devuelve un StringObject GC-managed con el contenido de args[i].
        // Baja a `getarg r_dst, r_idx`.  Si i fuera de rango, devuelve
        // GC_NULL_HANDLE = 0 (que el frontend trata como string nulo).
        if (is_args_get) {
            if (e->args.size() != 1 || !e->args[0]) {
                error_at(e->loc,
                         "args_get: requiere 1 argumento (i32 indice)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_idx = lower_expr(e->args[0].get());
            if (v_idx == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            out_value = emit_getarg(v_idx, e->loc.line);
            return true;
        }

        // ----- msgsend(pid, value) -----
        // Reservamos 8 bytes en el frame del caller (alloca i8[8]),
        // escribimos `value` ahi como i64, y emitimos:
        //   msgsend r_pid, r_addr, r_len   (r_len = 8)
        // El opcode bytecode deposita 1/0 en R0 (ok flag), que capturamos
        // como i32 dst de la expresion.
        if (is_msgsend) {
            if (e->args.size() != 2) {
                error_at(e->loc, "msgsend: requiere 2 argumentos (pid, valor)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_pid = lower_expr(e->args[0].get());
            const ir::IrValueId v_val = lower_expr(e->args[1].get());
            if (v_pid == ir::IR_NO_VALUE || v_val == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // ALLOCA 8 bytes en stack para el buffer del mensaje.
            const ir::IrValueId v_buf = fn_->new_value(ir::IrType::PTR); {
                ir::IrInstr al{};
                al.op          = ir::IrOp::ALLOCA;
                al.type        = ir::IrType::I8;
                al.dst         = v_buf;
                al.imm         = 8;
                al.source_line = e->loc.line;
                fn_->append(current_block_, std::move(al));
            }
            // STORE i64 v_val en v_buf.
            {
                ir::IrInstr st{};
                st.op          = ir::IrOp::STORE;
                st.type        = ir::IrType::I64;
                st.operands    = {v_val, v_buf};
                st.source_line = e->loc.line;
                fn_->append(current_block_, std::move(st));
            }
            // msgsend r_pid, r_addr, r_len  -- usar IR op MSGSEND (0xD0) en lugar
            // de RAW_ASM.  Devuelve bool en R0 (1=ok, 0=error).
            const ir::IrValueId v_len = emit_const(ir::IrType::I64, 8, e->loc.line);
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I32);
            ir::IrInstr         ms{};
            ms.op           = ir::IrOp::MSGSEND;
            ms.type         = ir::IrType::I32;
            ms.dst          = v_dst;
            ms.operands     = {v_pid, v_buf, v_len};
            ms.set_is_call_site(true);
            ms.source_line  = e->loc.line;
            fn_->append(current_block_, std::move(ms));
            out_value = v_dst;
            return true;
        }

        // ----- msgrecv() -----
        // Reservamos 8 bytes en el frame, llamamos msgrecv (bloquea si
        // mailbox vacio; al despertar el proceso re-ejecuta msgrecv y
        // pasa con datos), y leemos el i64 del buffer.
        if (is_msgrecv) {
            if (!e->args.empty()) {
                error_at(e->loc, "msgrecv: no acepta argumentos");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // ALLOCA 8 bytes para el buffer destino.
            const ir::IrValueId v_buf = fn_->new_value(ir::IrType::PTR); {
                ir::IrInstr al{};
                al.op          = ir::IrOp::ALLOCA;
                al.type        = ir::IrType::I8;
                al.dst         = v_buf;
                al.imm         = 8;
                al.source_line = e->loc.line;
                fn_->append(current_block_, std::move(al));
            }
            // msgrecv r_buf, r_max  -- primer reg = buffer dest, segundo = max len.
            // Convencion del decoder/exec: reg1=r_buf, reg2=r_max (ver
            // exec_instr_msgrecv en exec_instruction_distrib.cpp).
            const ir::IrValueId v_max = emit_const(ir::IrType::I64, 8, e->loc.line); {
                ir::IrInstr mr{};
                mr.op           = ir::IrOp::MSGRECV;
                mr.type         = ir::IrType::VOID;
                mr.dst          = ir::IR_NO_VALUE;
                mr.operands     = {v_buf, v_max};
                mr.set_is_call_site(true); // bloquea -> save/restore live regs
                mr.source_line  = e->loc.line;
                fn_->append(current_block_, std::move(mr));
            }
            // LOAD i64 desde v_buf -> v_val (resultado).
            const ir::IrValueId v_val = fn_->new_value(ir::IrType::I64); {
                ir::IrInstr ld{};
                ld.op          = ir::IrOp::LOAD;
                ld.type        = ir::IrType::I64;
                ld.dst         = v_val;
                ld.operands    = {v_buf};
                ld.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ld));
            }
            out_value = v_val;
            return true;
        }

        // ----- future_alloc() -----
        // Emite la instruccion bytecode `future` (0x29) que crea un nuevo
        // FutureObject GC-managed en estado PENDING y deposita su GcHandle
        // en R0.  Capturamos R0 a {dst} como i64 para pasarlo a fulfill/await.
        if (is_future_alloc) {
            if (!e->args.empty()) {
                error_at(e->loc, "future_alloc: no acepta argumentos");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // future -> aloca FutureObject; R0 contiene el handle.
            const ir::IrValueId v_fut = fn_->new_value(ir::IrType::I64);
            ir::IrInstr         fu{};
            fu.op           = ir::IrOp::FUTURE;
            fu.type         = ir::IrType::I64;
            fu.dst          = v_fut;
            fu.set_is_call_site(true); // GC alloc
            fu.source_line  = e->loc.line;
            fn_->append(current_block_, std::move(fu));
            out_value = v_fut;
            return true;
        }

        // ----- fulfill(fut, value) -----
        // Emite `fulfill r_fut, r_val` (0x2B): resuelve el future con el
        // valor, despierta al waiter (si lo hay) via make_ready.  Devuelve
        // void (no captura R0).
        if (is_fulfill) {
            if (e->args.size() != 2) {
                error_at(e->loc, "fulfill: requiere 2 argumentos (fut, valor)");
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_fut = lower_expr(e->args[0].get());
            const ir::IrValueId v_val = lower_expr(e->args[1].get());
            if (v_fut == ir::IR_NO_VALUE || v_val == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            ir::IrInstr fu{};
            fu.op          = ir::IrOp::FULFILL;
            fu.type        = ir::IrType::VOID;
            fu.dst         = ir::IR_NO_VALUE;
            fu.operands    = {v_fut, v_val};
            fu.source_line = e->loc.line;
            fn_->append(current_block_, std::move(fu));
            out_value = ir::IR_NO_VALUE;
            return true;
        }

        // No deberia alcanzarse: todos los builtins listados arriba estan
        // cubiertos.
        return false;
    }
    /* non-static (visible desde lowering.cpp) */
    uint64_t intern_class_name(ir::IrModule &mod, const std::string &name) {
        std::vector<uint8_t> bytes(name.begin(), name.end());
        return mod.intern_static_data(std::move(bytes));
    }

    /**
     * @brief fix11 - reserva un slot de 8 bytes en static_data para
     * cachear el `ClassInfo*` de una clase.  El slot inicia en 0 y se
     * llena en `__module_init` despues del `defclass`; cada llamada
     * subsiguiente a `__new_<Class>` lee del slot directamente sin
     * pasar por `findclass` (ahorra ~8 instrucciones VM por alloc +
     * elimina el string lookup en el runtime ClassRegistry).
     *
     * Los bytes contienen: 8 ceros (el slot del ClassInfo*) + bytes
     * unicos del nombre de la clase + sentinel 0xFF para evitar
     * deduplicacion con otras clases o con el nombre del simbolo
     * (que es el patron de `intern_class_name`).  El runtime accede
     * SOLO a los primeros 8 bytes del slot.
     */
    /* non-static (visible desde lowering.cpp) */
    uint64_t intern_class_cache_slot(ir::IrModule &mod, const std::string &name) {
        std::vector<uint8_t> bytes(8, 0);                    // 8 zeros (cache)
        bytes.push_back(0xFF);                               // sentinel: distingue de intern_class_name
        bytes.insert(bytes.end(), name.begin(), name.end()); // nombre para unicidad
        return mod.intern_static_data(std::move(bytes));
    }

} // namespace vex
