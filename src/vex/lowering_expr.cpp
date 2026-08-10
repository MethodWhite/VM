#include "vex/lowering.h"
#include "vex/collection_intrinsics.h"
#include "vex/comptime_introspect.h"
#include "ir/ir_optimizer.h"

#include <set>
#include <sstream>
#include <utility>

namespace vex {

    ir::IrValueId Lowering::lower_expr(ast::Expr *e) {
        if (!e) return ir::IR_NO_VALUE;
        switch (e->kind) {
            case ast::NodeKind::IntLitExpr: {
                auto *           ie = static_cast<ast::IntLitExpr *>(e);
                const ir::IrType t  = ir_type_from_primitive(e->result_type.kind);
                return emit_const(t == ir::IrType::VOID ? ir::IrType::I64 : t,
                                  ie->value, e->loc.line);
            }
            case ast::NodeKind::FloatLitExpr: {
                auto *     fe = static_cast<ast::FloatLitExpr *>(e);
                ir::IrType t  = ir_type_from_primitive(e->result_type.kind);
                if (t == ir::IrType::VOID) t = ir::IrType::F64;
                // Reinterpretamos los bits del double como uint64 para
                // alojarlos en el campo imm de la instruccion CONST.
                uint64_t bits;
                static_assert(sizeof(double) == sizeof(uint64_t),
                              "double debe ocupar 64 bits para reinterpret_cast");
                __builtin_memcpy(&bits, &fe->value, sizeof(double));
                return emit_const(t, bits, e->loc.line);
            }
            case ast::NodeKind::BoolLitExpr: {
                auto *be = static_cast<ast::BoolLitExpr *>(e);
                return emit_const(ir::IrType::BOOL, be->value ? 1 : 0, e->loc.line);
            }
            case ast::NodeKind::CharLitExpr: {
                auto *ce = static_cast<ast::CharLitExpr *>(e);
                return emit_const(ir::IrType::U8, ce->codepoint, e->loc.line);
            }
            case ast::NodeKind::StringLitExpr:
                return lower_string_lit(static_cast<ast::StringLitExpr *>(e));
            case ast::NodeKind::NullLitExpr:
                // Null se modela como el escalar 0 del tipo i64; el type
                // checker valida que solo se asigne a punteros / handles
                // (donde 0 es la representacion canonica de "ausente").
                return emit_const(ir::IrType::I64, 0, e->loc.line);
            case ast::NodeKind::IdentExpr:
                return lower_ident(static_cast<ast::IdentExpr *>(e));
            case ast::NodeKind::FieldAccessExpr:
                return lower_field_access(static_cast<ast::FieldAccessExpr *>(e));
            case ast::NodeKind::BinaryExpr:
                return lower_binary(static_cast<ast::BinaryExpr *>(e));
            case ast::NodeKind::UnaryExpr:
                return lower_unary(static_cast<ast::UnaryExpr *>(e));
            case ast::NodeKind::CallExpr:
                return lower_call(static_cast<ast::CallExpr *>(e));
            case ast::NodeKind::AssignExpr:
                return lower_assign(static_cast<ast::AssignExpr *>(e));
            case ast::NodeKind::TernaryExpr:
                return lower_ternary(static_cast<ast::TernaryExpr *>(e));
            case ast::NodeKind::TryExpr:
                return lower_try_expr(static_cast<ast::TryExpr *>(e));
            case ast::NodeKind::IndexExpr:
                return lower_index(static_cast<ast::IndexExpr *>(e));
            case ast::NodeKind::ThisExpr:
                return lower_this_expr(static_cast<ast::ThisExpr *>(e));
            case ast::NodeKind::NewExpr:
                return lower_new_expr(static_cast<ast::NewExpr *>(e));
            case ast::NodeKind::SuperCallExpr:
                return lower_super_call_expr(static_cast<ast::SuperCallExpr *>(e));
            case ast::NodeKind::SuperMethodCallExpr:
                return lower_super_method_call_expr(
                    static_cast<ast::SuperMethodCallExpr *>(e));
            case ast::NodeKind::SpawnExpr:
                return lower_spawn_expr(static_cast<ast::SpawnExpr *>(e));
            case ast::NodeKind::RSpawnExpr:
                return lower_rspawn_expr(static_cast<ast::RSpawnExpr *>(e));
            case ast::NodeKind::LambdaExpr:
                return lower_lambda_expr(static_cast<ast::LambdaExpr *>(e));
            case ast::NodeKind::MatchExpr:
                return lower_match_expr(static_cast<ast::MatchExpr *>(e));
            case ast::NodeKind::CastExpr:
                return lower_cast_expr(static_cast<ast::CastExpr *>(e));
            default:
                unsupported(e->loc, "expresion no soportada por el lowering actual");
                return ir::IR_NO_VALUE;
        }
    }

    ir::IrValueId Lowering::lower_cast_expr(ast::CastExpr *e) {
        if (!e || !e->operand) return ir::IR_NO_VALUE;
        const ir::IrValueId v_op = lower_expr(e->operand.get());
        if (v_op == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;

        const Type &dst_type = e->result_type;          // tipo destino del cast
        const Type &src_type = e->operand->result_type; // tipo del operando

        // Categorias.  PTR/ARRAY se tratan como pointer-like.
        auto is_ptr_like = [](const Type &t) {
            return t.kind == PrimitiveKind::PTR
                || t.kind == PrimitiveKind::ARRAY;
        };
        const bool dst_ptr = is_ptr_like(dst_type);
        const bool src_ptr = is_ptr_like(src_type);

        // ptr <-> ptr: el bit-pattern es identico, solo cambia la
        // interpretacion (host vs virtual, pointee).  No emitimos
        // ninguna instruccion IR; reusamos el SSA value tras propagar
        // los flags is_host_ptr/pointee_is_host_ptr al destino.
        if (dst_ptr && src_ptr) {
            // El SSA value sigue siendo el mismo bit-pattern.  Para
            // que LOAD/STORE posteriores emitan mov vs movh segun el
            // tipo DESTINO, marcamos el bit en el value resultante.
            // Convencion: VirtualPtr<T> -> is_host_ptr=false (memoria VM).
            //             T* (sin is_virtual) -> is_host_ptr=true (host).
            // Si el bit-pattern original era host_ptr=true y el destino
            // es VirtualPtr (is_virtual=true), el cast cambia la
            // interpretacion: el lowering ahora emitira mov en lugar
            // de movh.  El usuario asume las consecuencias.
            //
            // No clonamos el SSA value (eso obligaria a un MOV inutil);
            // creamos un nuevo SSA value vacio que comparte el reg con
            // el original via copy-prop natural del IR optimizer.  Para
            // ello emitimos un BITCAST de PTR a PTR (no-op a nivel
            // bytecode: se baja a `mov rd, rs` y la siguiente fase de
            // copy-prop suele eliminarlo).
            const ir::IrValueId dst = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr ins{};
            ins.op          = ir::IrOp::BITCAST;
            ins.type        = ir::IrType::PTR;
            ins.dst         = dst;
            ins.operands    = {v_op};
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            // Propagar flags segun el tipo destino.
            fn_->values[dst].is_host_ptr = !dst_type.is_virtual;
            // pointee_is_host_ptr: si el destino apunta a otro puntero
            // host (e.g. T**), el slot apuntado lleva un host_ptr.  Sin
            // tipo pointee accesible aqui, replicamos el flag del
            // operando original como aproximacion conservadora.
            fn_->values[dst].pointee_is_host_ptr =
                fn_->values[v_op].pointee_is_host_ptr;
            return dst;
        }

        // ptr <-> int: BITCAST.  El IR_OP::BITCAST esta diseñado para
        // exactamente este caso (preserva bits sin conversion numerica).
        if (dst_ptr || src_ptr) {
            const ir::IrType ir_dst = ir_type_from_primitive(dst_type.kind);
            const ir::IrType ir_use = (ir_dst == ir::IrType::VOID)
                                         ? (dst_ptr ? ir::IrType::PTR : ir::IrType::I64)
                                         : ir_dst;
            const ir::IrValueId dst = fn_->new_value(ir_use);
            ir::IrInstr ins{};
            ins.op          = ir::IrOp::BITCAST;
            ins.type        = ir_use;
            ins.dst         = dst;
            ins.operands    = {v_op};
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            if (dst_ptr) {
                fn_->values[dst].is_host_ptr = !dst_type.is_virtual;
            }
            return dst;
        }

        // num <-> num: delegar al helper existente.  Maneja int<->int
        // (TRUNC/ZEXT/SEXT/CAST), int<->float (ITOF/UITOF/FTOI/FTOUI) y
        // float<->float (F32TOF64/F64TOF32).  Pasamos @c is_explicit=true
        // para silenciar el warning de cast implicito (el usuario opto
        // por el cast explicitamente).
        const ir::IrType ir_from = ir_type_from_primitive(src_type.kind);
        const ir::IrType ir_to   = ir_type_from_primitive(dst_type.kind);
        if (ir_from == ir::IrType::VOID || ir_to == ir::IrType::VOID) {
            // Sin tipos validos en alguno de los lados, devolvemos el
            // operando sin convertir.  El type checker ya emitio el
            // error correspondiente.
            return v_op;
        }
        return cast_if_needed(v_op, ir_from, ir_to, e->loc.line, /*is_explicit=*/true);
    }

    // ---------------------------------------------------------------------
    // Helpers: tamano de tipo y aritmetica de punteros.
    // ---------------------------------------------------------------------

    size_t Lowering::size_of_type(const Type &t) const {
        if (t.kind == PrimitiveKind::STRUCT) {
            const auto &layouts = tc_.struct_layouts();
            auto        it      = layouts.find(t.struct_name);
            if (it != layouts.end()) return it->second.size_bytes;
            // bug4: STRUCT puede ser un ENUM (mismo kind).  Buscar en
            // enum_layouts_ tambien.  size_bytes = 8 (tag) + 8*max_payload.
            const auto &elayouts = tc_.enum_layouts();
            auto        ite      = elayouts.find(t.struct_name);
            return (ite == elayouts.end()) ? 0 : ite->second.size_bytes;
        }
        if (t.kind == PrimitiveKind::PTR) return 8;
        if (t.kind == PrimitiveKind::ARRAY) {
            // T[N] ocupa N*sizeof(T) bytes; T[] (size==0) decae a puntero.
            // bug4: para T[] (dynamic) sin tamano fijo, sizeof = 8 (el
            // valor es un host_ptr al buffer).  Caller que pregunte
            // sizeof(slot) obtiene la pointer-size correcta.
            if (!t.pointee) return 0;
            if (t.array_size == 0) return 8;  // host_ptr al buffer
            return static_cast<size_t>(t.array_size) * size_of_type(*t.pointee);
        }
        // CLASS: ref host_ptr al objeto GC.
        if (t.kind == PrimitiveKind::CLASS) return 8;
        // STRING: GcHandle u64.
        if (t.kind == PrimitiveKind::STRING) return 8;
        return primitive_size_bytes(t.kind);
    }

    ir::IrValueId Lowering::lower_index_addr(ast::IndexExpr *e) {
        // Calcula base + index * sizeof(*base) y devuelve el puntero al
        // elemento.  Si sizeof == 1 omitimos la multiplicacion para
        // mantener el .vel mas claro en el caso comun de char/i8.
        if (!e->base || !e->index) {
            error_at(e->loc, "lowering: subscript con base o indice nulo");
            return ir::IR_NO_VALUE;
        }
        const Type bt          = e->base->result_type;
        const bool is_ptr_like =
                (bt.kind == PrimitiveKind::PTR || bt.kind == PrimitiveKind::ARRAY)
                && static_cast<bool>(bt.pointee);
        if (!is_ptr_like) {
            error_at(e->loc, "lowering: '[]' sobre tipo no-PTR ni array");
            return ir::IR_NO_VALUE;
        }
        const size_t esz = size_of_type(*bt.pointee);
        if (esz == 0) {
            error_at(e->loc,
                     "lowering: sizeof del tipo apuntado es 0 (void* u struct desconocido)");
            return ir::IR_NO_VALUE;
        }
        const ir::IrValueId base_v = lower_expr(e->base.get());
        ir::IrValueId       idx_v  = lower_expr(e->index.get());
        if (base_v == ir::IR_NO_VALUE || idx_v == ir::IR_NO_VALUE)
            return ir::IR_NO_VALUE;
        // Promover index a I64 para sumarlo al puntero (que el emisor
        // trata como i64 en aritmetica).
        idx_v = cast_if_needed(idx_v, fn_->values[idx_v].type, ir::IrType::I64,
                               e->loc.line);
        // Escalar por sizeof(pointee) si != 1.
        ir::IrValueId offset = idx_v;
        if (esz != 1) {
            const ir::IrValueId sz_v = emit_const(ir::IrType::I64, (uint64_t) esz,
                                                  e->loc.line);
            const ir::IrValueId scaled = fn_->new_value(ir::IrType::I64);
            ir::IrInstr         mul{};
            mul.op          = ir::IrOp::MUL;
            mul.type        = ir::IrType::I64;
            mul.dst         = scaled;
            mul.operands    = {idx_v, sz_v};
            mul.source_line = e->loc.line;
            fn_->append(current_block_, std::move(mul));
            offset = scaled;
        }
        const ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
        // Propagar is_host_ptr: p[i] vive en el mismo espacio que p.
        fn_->values[addr].is_host_ptr = fn_->values[base_v].is_host_ptr;
        ir::IrInstr add{};
        add.op          = ir::IrOp::ADD;
        add.type        = ir::IrType::PTR;
        add.dst         = addr;
        add.operands    = {base_v, offset};
        add.source_line = e->loc.line;
        fn_->append(current_block_, std::move(add));
        return addr;
    }

    ir::IrValueId Lowering::lower_index(ast::IndexExpr *e) {
        const ir::IrValueId addr = lower_index_addr(e);
        if (addr == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        // Bug fix 2026-05-23: para arrays multi-dim `m[i]` donde m es
        // `T[N][M]`, el "valor" de la sub-array NO es un puntero cargado
        // desde la memoria del array, sino la DIRECCION BASE de la fila.
        // Sin esto, `lower_index(m[i])` emitia LOAD ptr [m+i*sizeof(row)]
        // que leia bytes del primer elemento como si fueran un host_ptr ->
        // aliasing entre filas distintas, valores incorrectos.
        if (e->result_type.kind == PrimitiveKind::ARRAY
         || e->result_type.kind == PrimitiveKind::STRUCT) {
            // Resultado es un sub-array o struct value-type; addr ya es la
            // direccion correcta del elemento.  Sin esto, `arr[i].field` con
            // `arr: Struct[N]` cargaba el primer qword del struct como un
            // ptr y luego sumaba el offset -> aliasing entre elementos.
            return addr;
        }
        const ir::IrType    ft  = ir_type_from_primitive(e->result_type.kind);
        const ir::IrValueId dst = fn_->new_value(ft);
        ir::IrInstr         ld{};
        ld.op          = ir::IrOp::LOAD;
        ld.type        = ft;
        ld.dst         = dst;
        ld.operands    = {addr};
        ld.source_line = e->loc.line;
        fn_->append(current_block_, std::move(ld));
        return dst;
    }

    ir::IrValueId Lowering::lower_ident(ast::IdentExpr *e) {
        /* si estamos dentro de un @Macro body Y el name
         * resuelve a un comptime global int, emit LOAD desde el slot
         * @c static_data correspondiente.  Asi el macro ve el VALOR
         * ACTUAL (mutado por invocaciones previas), no el inicial. */
        if (current_fn_is_macro_) {
            auto cit = tc_.comptime_const_values().find(e->name);
            if (cit != tc_.comptime_const_values().end() && !cit->second.is_str) {
                /* Pero PRIORIDAD a comptime_const_locals_ del lowering
                 * (sobreescrito por comptime for body): si la var esta
                 * en el stack dinamico, hicimos override -- usar ese
                 * valor inline en lugar del slot global. */
                bool overridden = false;
                for (auto it = lowering_comptime_scopes_.rbegin();
                     it != lowering_comptime_scopes_.rend(); ++it) {
                    if (it->find(e->name) != it->end()) { overridden = true; break; }
                }
                if (!overridden) {
                    const uint64_t slot_idx = get_or_create_comptime_global_slot(e->name);
                    if (slot_idx != UINT64_MAX) {
                        const int ln = e->loc.line;
                        ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR); {
                            ir::IrInstr is{};
                            is.op          = ir::IrOp::STR_LIT_ADDR;
                            is.type        = ir::IrType::PTR;
                            is.dst         = v_addr;
                            is.imm         = slot_idx;
                            is.source_line = ln;
                            fn_->append(current_block_, std::move(is));
                        }
                        ir::IrValueId v_val = fn_->new_value(ir::IrType::I64); {
                            ir::IrInstr ld{};
                            ld.op          = ir::IrOp::LOAD;
                            ld.type        = ir::IrType::I64;
                            ld.dst         = v_val;
                            ld.operands    = {v_addr};
                            ld.source_line = ln;
                            fn_->append(current_block_, std::move(ld));
                        }
                        return v_val;
                    }
                }
            }
        }

        // A.39: primero consultamos el stack dinamico de comptime const
        // del lowering -- usado por `comptime for` para override del
        // index por iteracion sin re-correr el type checker.
        for (auto it = lowering_comptime_scopes_.rbegin();
             it != lowering_comptime_scopes_.rend(); ++it) {
            auto hit = it->find(e->name);
            if (hit != it->end()) {
                if (hit->second.is_str) {
                    /* String comptime override (raro pero soportado). */
                    std::vector<uint8_t> bytes(
                        hit->second.str_value.begin(),
                        hit->second.str_value.end());
                    const uint64_t idx = out_mod_->intern_static_data(
                        std::move(bytes));
                    ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
                    {
                        ir::IrInstr is{};
                        is.op          = ir::IrOp::STR_LIT_ADDR;
                        is.type        = ir::IrType::PTR;
                        is.dst         = v_addr;
                        is.imm         = idx;
                        is.source_line = e->loc.line;
                        fn_->append(current_block_, std::move(is));
                    }
                    ir::IrValueId v_len = emit_const(ir::IrType::I64,
                        static_cast<uint64_t>(hit->second.str_value.size()),
                        e->loc.line);
                    ir::IrValueId v_str = emit_strmake(v_addr, v_len, e->loc.line);
                    return v_str;
                }
                return emit_const(hit->second.ir_t,
                    static_cast<uint64_t>(hit->second.value), e->loc.line);
            }
        }

        // A.38/A.39: si el type checker ya resolvio este ident a un
        // comptime const (annotacion en el AST), inline-amos el valor
        // como CONST directo.  Cubre locales (que ya no estan en la
        // tabla del type checker) y globales.  Strings comptime se
        // materializan via STR_LIT_ADDR + STRMAKE.
        if (e->comptime_const_resolved) {
            if (e->comptime_const_is_str) {
                /* Construir StringObject inline desde los bytes del
                 * comptime string -- mismo patron que typename<T>(). */
                std::vector<uint8_t> bytes(e->comptime_const_str.begin(),
                                            e->comptime_const_str.end());
                const uint64_t idx = out_mod_->intern_static_data(
                    std::move(bytes));
                // v4: propagar atributos (@align/@hot/@cold/@section)
                // al static_data_meta del idx recien interno.  Lookup
                // en comptime_const_values_ por nombre para extraer
                // los atributos guardados durante el type check.
                {
                    const auto &ccv = tc_.comptime_const_values();
                    auto cit = ccv.find(e->name);
                    if (cit != ccv.end()
                     && idx < out_mod_->static_data.size()) {
                        auto &m = out_mod_->static_data.meta_at(idx);
                        if (cit->second.attr_align > 0) {
                            m.alignment = cit->second.attr_align;
                        }
                        m.flags |= ir::IrModule::SD_FLAG_IMMUTABLE;
                        if (cit->second.attr_hot)
                            m.flags |= ir::IrModule::SD_FLAG_HOT;
                        if (cit->second.attr_cold)
                            m.flags |= ir::IrModule::SD_FLAG_COLD;
                        if (!cit->second.attr_section.empty())
                            m.section_name = cit->second.attr_section;
                    }
                }
                ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR); {
                    ir::IrInstr is{};
                    is.op          = ir::IrOp::STR_LIT_ADDR;
                    is.type        = ir::IrType::PTR;
                    is.dst         = v_addr;
                    is.imm         = idx;
                    is.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(is));
                }
                ir::IrValueId v_len = emit_const(ir::IrType::I64,
                    static_cast<uint64_t>(e->comptime_const_str.size()),
                    e->loc.line);
                ir::IrValueId v_str = emit_strmake(v_addr, v_len, e->loc.line);
                return v_str;
            }
            ir::IrType t = ir_type_from_primitive(e->result_type.kind);
            return emit_const(t,
                static_cast<uint64_t>(e->comptime_const_int), e->loc.line);
        }

        // Phase M.L7: globals const IMPORTADAS de otro modulo via .vexi.
        // El TypeChecker las registro con su valor literal embedded en
        // @c imported_global_consts_; emitimos CONST inline igual que
        // las locales (cero overhead).
        // v4: tambien soporta strings importados via STRMAKE.
        {
            auto it = tc_.imported_global_consts().find(e->name);
            if (it != tc_.imported_global_consts().end()) {
                if (it->second.is_str) {
                    // Materializar StringObject inline.
                    const std::string &sv = it->second.str_value;
                    std::vector<uint8_t> pbytes(sv.begin(), sv.end());
                    const uint64_t p_idx =
                        out_mod_->intern_static_data(std::move(pbytes));
                    ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
                    {
                        ir::IrInstr is{};
                        is.op          = ir::IrOp::STR_LIT_ADDR;
                        is.type        = ir::IrType::PTR;
                        is.dst         = v_addr;
                        is.imm         = p_idx;
                        is.source_line = e->loc.line;
                        fn_->append(current_block_, std::move(is));
                    }
                    ir::IrValueId v_len = emit_const(
                        ir::IrType::I64,
                        static_cast<uint64_t>(sv.size()),
                        e->loc.line);
                    return emit_strmake(v_addr, v_len, e->loc.line);
                }
                ir::IrType t = ir_type_from_primitive(it->second.type.kind);
                return emit_const(t, it->second.value, e->loc.line);
            }
        }

        // L2.2: globales runtime no-const con storage en static_data.
        // Antes del const-globals scan para preferir el storage real cuando
        // existe.  Cero overhead vs constantes (un LOAD adicional, ~2 ns).
        {
            auto sit = runtime_global_slots_.find(e->name);
            if (sit != runtime_global_slots_.end()) {
                // Detectar el tipo declarado del global para emitir LOAD
                // con el ancho correcto.  Default i64.
                ir::IrType t = ir::IrType::I64;
                bool is_string = false;
                for (auto &decl : mod_.decls) {
                    if (!decl || decl->kind != ast::NodeKind::GlobalVarDecl) continue;
                    auto *gv = static_cast<ast::GlobalVarDecl *>(decl.get());
                    if (gv->name != e->name) continue;
                    if (gv->type
                     && gv->type->kind == ast::NodeKind::PrimitiveTypeNode) {
                        auto *pt = static_cast<ast::PrimitiveTypeNode *>(gv->type.get());
                        t = ir_type_from_primitive(pt->prim);
                        is_string = (pt->prim == PrimitiveKind::STRING);
                    }
                    break;
                }
                const uint64_t slot_idx = sit->second;
                const int ln = e->loc.line;
                ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR); {
                    ir::IrInstr is{};
                    is.op          = ir::IrOp::STR_LIT_ADDR;
                    is.type        = ir::IrType::PTR;
                    is.dst         = v_addr;
                    is.imm         = slot_idx;
                    is.source_line = ln;
                    fn_->append(current_block_, std::move(is));
                }
                // Para STRING, LOAD i64 (GcHandle); para otros, LOAD con
                // ancho declarado.
                const ir::IrType load_t = is_string ? ir::IrType::I64 : t;
                ir::IrValueId v_val = fn_->new_value(load_t); {
                    ir::IrInstr ld{};
                    ld.op          = ir::IrOp::LOAD;
                    ld.type        = load_t;
                    ld.dst         = v_val;
                    ld.operands    = {v_addr};
                    ld.source_line = ln;
                    fn_->append(current_block_, std::move(ld));
                }
                return v_val;
            }
        }

        // const-globals - inlining de constantes globales `const T NAME = lit;`.
        // Vex aun no genera storage para variables globales (solo warning),
        // pero para const con inicializador literal podemos emitir un CONST
        // inline en el call site.  Cero overhead, util para nombrar codigos
        // de tecla (KEY_*), VK constants, magic numbers.
        for (auto &decl: mod_.decls) {
            if (!decl || decl->kind != ast::NodeKind::GlobalVarDecl) continue;
            auto *gv = static_cast<ast::GlobalVarDecl *>(decl.get());
            if (gv->name != e->name) continue;
            if (!gv->is_const || !gv->init) break;
            // Tipo destino: del declarado (i32, i64, ...).  Default i64.
            ir::IrType t = ir::IrType::I64;
            if (gv->type
                && gv->type->kind == ast::NodeKind::PrimitiveTypeNode) {
                auto *pt = static_cast<ast::PrimitiveTypeNode *>(gv->type.get());
                t        = ir_type_from_primitive(pt->prim);
            }
            // Constante entera positiva: directamente IntLitExpr.
            if (gv->init->kind == ast::NodeKind::IntLitExpr) {
                auto *lit = static_cast<ast::IntLitExpr *>(gv->init.get());
                return emit_const(t, static_cast<uint64_t>(lit->value), e->loc.line);
            }
            // Constante entera negativa: UnaryExpr Neg sobre IntLitExpr.
            if (gv->init->kind == ast::NodeKind::UnaryExpr) {
                auto *u = static_cast<ast::UnaryExpr *>(gv->init.get());
                if (u->op == ast::UnOp::Neg
                    && u->operand
                    && u->operand->kind == ast::NodeKind::IntLitExpr) {
                    auto *  lit = static_cast<ast::IntLitExpr *>(u->operand.get());
                    int64_t v   = -static_cast<int64_t>(lit->value);
                    return emit_const(t, static_cast<uint64_t>(v), e->loc.line);
                }
            }
            // Bug fix 2026-05-23: const string GLOBAL = "literal"; cada uso
            // inlinea un STRMAKE del literal a StringObject (mismo patron que
            // var-decl local).  Sin esto, `const string g = "x"` daba error
            // de tipo o un global sin storage.
            // NOTA: cada uso aloca un StringObject nuevo (no se cachea).  Si
            // el codigo invoca el global N veces dentro de un loop, hay N
            // allocaciones GC.  Aceptable para constantes string usadas una
            // vez; el usuario que necesite single-alloc puede hacer
            // `const string g = ...; string cached = g;` al inicio.
            if (gv->is_const
                && gv->init->kind == ast::NodeKind::StringLitExpr
                && gv->type
                && gv->type->kind == ast::NodeKind::PrimitiveTypeNode
                && static_cast<ast::PrimitiveTypeNode *>(gv->type.get())->prim
                       == PrimitiveKind::STRING) {
                auto *slit = static_cast<ast::StringLitExpr *>(gv->init.get());
                return lower_string_literal_to_string_object(slit);
            }
            // Otros tipos de inicializador (FloatLit, BinaryExpr)
            // -- a implementar cuando los necesite.
            break;
        }
        // Constantes ENC_* (encoding numerico para builtins de string como
        // @c str_convert(s, ENC_UTF16)).  Cero overhead: emit const i32
        // inline en el call site -- no necesitan storage como simbolos.
        {
            static const struct {
                const char *name;
                int32_t     v;
            } ENC_LU[] = {
                        {"ENC_ASCII", 0}, {"ENC_ANSI", 1}, {"ENC_UTF8", 2},
                        {"ENC_UTF16", 3}, {"ENC_UTF32", 4},
                    };
            for (const auto &m: ENC_LU) {
                if (e->name == m.name) {
                    return emit_const(ir::IrType::I32,
                                      static_cast<uint64_t>(m.v),
                                      e->loc.line);
                }
            }
        }
        // Identificadores ANSI magicos.  Si el nombre es una constante
        // predefinida (RED, GREEN, BOLD, RESET, etc.), emitir un
        // STR_LIT_ADDR a la cadena ANSI correspondiente.  Cero overhead
        // vs un string literal explicito; la deteccion es un lookup O(1)
        // en una tabla estatica.  Permite que el usuario escriba
        // @c println("Error: ${RED}..${RESET}") sin necesidad de
        // memorizar los codigos ANSI ni de declararlos como constantes.
        {
            static const struct {
                const char *name;
                const char *seq;
            }
                    ANSI[] = {
                        {"BLACK", "\x1b[30m"},
                        {"RED", "\x1b[31m"},
                        {"GREEN", "\x1b[32m"},
                        {"YELLOW", "\x1b[33m"},
                        {"BLUE", "\x1b[34m"},
                        {"MAGENTA", "\x1b[35m"},
                        {"CYAN", "\x1b[36m"},
                        {"WHITE", "\x1b[37m"},
                        {"BR_BLACK", "\x1b[90m"},
                        {"BR_RED", "\x1b[91m"},
                        {"BR_GREEN", "\x1b[92m"},
                        {"BR_YELLOW", "\x1b[93m"},
                        {"BR_BLUE", "\x1b[94m"},
                        {"BR_MAGENTA", "\x1b[95m"},
                        {"BR_CYAN", "\x1b[96m"},
                        {"BR_WHITE", "\x1b[97m"},
                        {"BG_BLACK", "\x1b[40m"},
                        {"BG_RED", "\x1b[41m"},
                        {"BG_GREEN", "\x1b[42m"},
                        {"BG_YELLOW", "\x1b[43m"},
                        {"BG_BLUE", "\x1b[44m"},
                        {"BG_MAGENTA", "\x1b[45m"},
                        {"BG_CYAN", "\x1b[46m"},
                        {"BG_WHITE", "\x1b[47m"},
                        {"BOLD", "\x1b[1m"},
                        {"DIM", "\x1b[2m"},
                        {"ITALIC", "\x1b[3m"},
                        {"UNDERLINE", "\x1b[4m"},
                        {"BLINK", "\x1b[5m"},
                        {"REVERSE", "\x1b[7m"},
                        {"RESET", "\x1b[0m"},
                        {"CLEAR_SCREEN", "\x1b[2J"},
                        {"CURSOR_HOME", "\x1b[H"},
                    };
            for (const auto &m: ANSI) {
                if (e->name == m.name) {
                    const std::string    seq = m.seq;
                    std::vector<uint8_t> bytes(seq.begin(), seq.end());
                    const uint64_t       lit_idx = out_mod_->intern_static_data(
                        std::move(bytes));
                    const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
                    ir::IrInstr         is{};
                    is.op          = ir::IrOp::STR_LIT_ADDR;
                    is.type        = ir::IrType::PTR;
                    is.dst         = v;
                    is.imm         = lit_idx;
                    is.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(is));
                    return v;
                }
            }
        }
        // Gap N: coercion de funcion top-level a function value.
        // Si el ident esta tipado como FUNCTION pero NO existe en
        // ningun scope local, asumimos que el type checker resolvio el
        // nombre a una funcion top-level (Symbol::Function) y patcheo
        // el result_type para forzar esta promocion.  Emitimos un slot
        // de 16 bytes en stack: fn_addr = @Absolute("code.<name>"),
        // env_addr = 0.  El callee (helper sintetico de lambda o el
        // CALLCLOSURE) ignora env_addr cuando es 0 (no toca r14).
        if (e->result_type.kind == PrimitiveKind::FUNCTION
            && lookup(e->name) == ir::IR_NO_VALUE) {
            return emit_topfn_value(e->name, e->loc.line);
        }
        // Para variables address-taken devolvemos el valor cargado (LOAD)
        // en lugar de la direccion guardada en scope.  Para STRUCT y
        // ARRAY, en cambio, el "valor" en uso es la propia direccion (no
        // son SSA-rvalues), por lo que lookup() es lo correcto: cualquier
        // uso posterior (subscript, decay-to-pointer al pasar a funcion,
        // arr + n) opera sobre la addr base.
        if (e->result_type.kind == PrimitiveKind::STRUCT
            || e->result_type.kind == PrimitiveKind::ARRAY
            || e->result_type.kind == PrimitiveKind::OPTIONAL
            || e->result_type.kind == PrimitiveKind::RESULT) {
            // Para STRUCT/ARRAY/OPTIONAL/RESULT la variable guarda
            // directamente la direccion del buffer (heap o stack); el
            // ident se resuelve via lookup, sin LOAD adicional.
            const ir::IrValueId v = lookup(e->name);
            if (v == ir::IR_NO_VALUE)
                error_at(e->loc, "lowering: nombre no resuelto: '" + e->name + "'");
            return v;
        }
        const ir::IrType    ir_ty = ir_type_from_primitive(e->result_type.kind);
        const ir::IrValueId v     = read_local(e->name, ir_ty, e->loc.line);
        if (v == ir::IR_NO_VALUE)
            error_at(e->loc, "lowering: nombre no resuelto: '" + e->name + "'");
        return v;
    }

    ir::IrValueId Lowering::lower_string_literal_to_string_object(
        ast::StringLitExpr *slit) {
        // Helper local: emite STRMAKE de un trozo literal y devuelve
        // el handle StringObject resultante.
        auto make_part_handle = [&](const std::string &part_text,
                                     int line) -> ir::IrValueId {
            std::vector<uint8_t> pbytes(part_text.begin(), part_text.end());
            const uint64_t       p_idx = out_mod_->intern_static_data(
                std::move(pbytes));
            const uint64_t       p_len = (uint64_t) part_text.size();
            ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR); {
                ir::IrInstr is{};
                is.op          = ir::IrOp::STR_LIT_ADDR;
                is.type        = ir::IrType::PTR;
                is.dst         = v_addr;
                is.imm         = p_idx;
                is.source_line = line;
                fn_->append(current_block_, std::move(is));
            }
            ir::IrValueId v_len    = emit_const(ir::IrType::I64, p_len, line);
            ir::IrValueId v_handle = emit_strmake(v_addr, v_len, line);
            return v_handle;
        };

        // Fast path: string literal SIN interpolacion -> 1 sola STRMAKE.
        if (!slit->is_interpolated()) {
            return make_part_handle(slit->value, slit->loc.line);
        }

        // Path interpolado: construimos el StringObject final como
        // cadena de STRCATs sobre los parts literales y los exprs
        // interpolados.  Layout: parts[0] + exprs[0] + parts[1] + ...
        // + parts[N] (siempre N+1 parts para N exprs).
        //
        // Cada `${expr}` se baja a un StringObject handle.  Strings
        // pasan tal cual; tipos primitivos (int/uint/bool/char/ptr/gc)
        // se pasan por un helper nativo que escribe su representacion
        // ASCII en un buffer VM y luego construimos el StringObject
        // via STRMAKE desde ese buffer.
        const int line = slit->loc.line;

        // Helper: emite la secuencia ALLOCA + CALLN(stringify_to_vmbuf)
        // + STRMAKE para un valor primitivo.  El `native_fn` es el nombre
        // de la funcion en `stdlib/native/io/vesta_io` que toma
        // (proc_ptr, vm_addr, value) y devuelve la longitud escrita.
        // El buffer VM es ALLOCA de 32 bytes (suficiente para todos los
        // tipos: i64=20+signo, hex=18, "false"=5, char UTF-8=4).
        auto stringify_primitive = [&](ir::IrValueId v_val,
                                        const char *native_fn,
                                        int ln) -> ir::IrValueId {
            // 1. ALLOCA 32 bytes (buffer en stack VM).
            ir::IrValueId v_buf = fn_->new_value(ir::IrType::PTR); {
                ir::IrInstr al{};
                al.op          = ir::IrOp::ALLOCA;
                al.type        = ir::IrType::I8;
                al.dst         = v_buf;
                al.imm         = 32;
                al.source_line = ln;
                fn_->append(current_block_, std::move(al));
            }
            // 2. proc_ptr via getproc.
            ir::IrValueId v_proc = fn_->new_value(ir::IrType::PTR); {
                ir::IrInstr gp{};
                gp.op          = ir::IrOp::GETPROC;
                gp.type        = ir::IrType::PTR;
                gp.dst         = v_proc;
                gp.source_line = ln;
                fn_->append(current_block_, std::move(gp));
            }
            // 3. CALLN al stringify nativo: returns length escrita en buf.
            //    Registramos el import con el linker para que la
            //    relocation se resuelva contra el plugin nativo.
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
            // 4. STRMAKE desde el buffer VM.  El opcode strmake (no _h)
            //    lee de vm_mem que es exactamente donde el helper escribio.
            ir::IrValueId v_h = emit_strmake(v_buf, v_len, ln);
            return v_h;
        };

        auto coerce_to_string_handle = [&](ast::Expr *ex) -> ir::IrValueId {
            if (!ex) return ir::IR_NO_VALUE;
            if (ex->kind == ast::NodeKind::StringLitExpr) {
                auto *sl = static_cast<ast::StringLitExpr *>(ex);
                return lower_string_literal_to_string_object(sl);
            }
            ir::IrValueId v = lower_expr(ex);
            if (v == ir::IR_NO_VALUE) return v;
            const PrimitiveKind ek = ex->result_type.kind;
            const int           ln = ex->loc.line;
            // Strings: pasan directamente.
            if (ek == PrimitiveKind::STRING) return v;
            // Stringify por tipo primitivo.  Cada categoria mapea a un
            // helper nativo en vesta_io.
            switch (ek) {
                case PrimitiveKind::I8:
                case PrimitiveKind::I16:
                case PrimitiveKind::I32:
                case PrimitiveKind::I64:
                    return stringify_primitive(v, "vio_int_to_vmbuf", ln);
                case PrimitiveKind::U8:
                case PrimitiveKind::U16:
                case PrimitiveKind::U32:
                case PrimitiveKind::U64:
                    return stringify_primitive(v, "vio_uint_to_vmbuf", ln);
                case PrimitiveKind::BOOL:
                    return stringify_primitive(v, "vio_bool_to_vmbuf", ln);
                case PrimitiveKind::CHAR:
                    return stringify_primitive(v, "vio_char_to_vmbuf", ln);
                case PrimitiveKind::PTR:
                case PrimitiveKind::ARRAY:
                    return stringify_primitive(v, "vio_ptr_to_vmbuf", ln);
                case PrimitiveKind::F64: {
                    // BugFix R7: BITCAST f64->i64 (preserva bits IEEE) y
                    // delega a vio_float_to_vmbuf.
                    ir::IrValueId v_bits = fn_->new_value(ir::IrType::I64);
                    ir::IrInstr bc{};
                    bc.op   = ir::IrOp::BITCAST;
                    bc.type = ir::IrType::I64;
                    bc.dst  = v_bits;
                    bc.operands = {v};
                    bc.source_line = ln;
                    fn_->append(current_block_, std::move(bc));
                    return stringify_primitive(v_bits, "vio_float_to_vmbuf", ln);
                }
                case PrimitiveKind::F32: {
                    // BugFix R7: f32 -> f64 (re-encode) -> BITCAST i64.
                    ir::IrValueId v_f64 = fn_->new_value(ir::IrType::F64);
                    ir::IrInstr ext{};
                    ext.op   = ir::IrOp::F32TOF64;
                    ext.type = ir::IrType::F64;
                    ext.dst  = v_f64;
                    ext.operands = {v};
                    ext.source_line = ln;
                    fn_->append(current_block_, std::move(ext));
                    ir::IrValueId v_bits = fn_->new_value(ir::IrType::I64);
                    ir::IrInstr bc{};
                    bc.op   = ir::IrOp::BITCAST;
                    bc.type = ir::IrType::I64;
                    bc.dst  = v_bits;
                    bc.operands = {v_f64};
                    bc.source_line = ln;
                    fn_->append(current_block_, std::move(bc));
                    return stringify_primitive(v_bits, "vio_float_to_vmbuf", ln);
                }
                case PrimitiveKind::CLASS: {
                    // BugFix R7: convert host_ptr -> GcHandle via gchandle
                    // RAW_ASM y delegar a vio_gchandle_to_vmbuf.
                    ir::IrValueId v_handle = emit_gc_handle_for_ptr(v, ln);
                    return stringify_primitive(v_handle, "vio_gchandle_to_vmbuf", ln);
                }
                default: break;
            }
            error_at(ex->loc,
                     "interpolacion `${expr}` en contexto string: tipo "
                     "no soportado todavia (struct/enum/optional/result). "
                     "Construye el mensaje con `print` o usa los builtins "
                     "de stringify explicito por ahora.");
            return ir::IR_NO_VALUE;
        };

        auto make_strcat = [&](ir::IrValueId a, ir::IrValueId b,
                                int ln) -> ir::IrValueId {
            return emit_strcat(a, b, static_cast<uint32_t>(ln));
        };

        const size_t ne = slit->interp_exprs.size();
        const size_t np = slit->interp_parts.size();
        ir::IrValueId acc = ir::IR_NO_VALUE;

        // Parte literal inicial (parts[0]).  La emitimos siempre (incluso
        // si es vacia) cuando ne > 0 porque necesitamos un acumulador
        // para los STRCAT subsiguientes; si es vacia, el primer STRCAT
        // se evita anclando el acc al primer expr handle.
        if (np > 0 && !slit->interp_parts[0].empty()) {
            acc = make_part_handle(slit->interp_parts[0], line);
        }
        for (size_t i = 0; i < ne; ++i) {
            ir::IrValueId expr_h = coerce_to_string_handle(
                slit->interp_exprs[i].get());
            if (expr_h == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            if (acc == ir::IR_NO_VALUE) {
                acc = expr_h;
            } else {
                acc = make_strcat(acc, expr_h, line);
            }
            if (i + 1 < np && !slit->interp_parts[i + 1].empty()) {
                ir::IrValueId p_h = make_part_handle(
                    slit->interp_parts[i + 1], line);
                acc = make_strcat(acc, p_h, line);
            }
        }
        if (acc == ir::IR_NO_VALUE) {
            // Edge case: todas las partes vacias y sin exprs.  Devolvemos
            // un StringObject vacio para mantener el contrato (handle
            // valido siempre).
            acc = make_part_handle(std::string(), line);
        }
        return acc;
    }

    ir::IrValueId Lowering::emit_topfn_value(const std::string &fn_name, int line) {
        // 1. ALLOCA 16 bytes para el slot del function value.
        ir::IrValueId fv_addr = fn_->new_value(ir::IrType::PTR); {
            ir::IrInstr al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8;
            al.dst         = fv_addr;
            al.imm         = 16;
            al.source_line = line;
            fn_->append(current_block_, std::move(al));
        }
        // 2. fn_addr via LABEL_ADDR IR op (Sprint 3).
        ir::IrValueId fn_addr = emit_label_addr(fn_name, line);
        // 3. env_addr = 0 (sin captures; el callee no debe leer r14).
        ir::IrValueId env_addr = emit_const(ir::IrType::I64, 0, line);
        // 4. STORE fn_addr en [fv_addr+0].
        {
            ir::IrInstr st{};
            st.op          = ir::IrOp::STORE;
            st.type        = ir::IrType::I64;
            st.operands    = {fn_addr, fv_addr};
            st.source_line = line;
            fn_->append(current_block_, std::move(st));
        }
        // 5. STORE env_addr en [fv_addr+8].
        {
            ir::IrValueId fv_plus_8 = fn_->new_value(ir::IrType::PTR);
            ir::IrValueId off8      = emit_const(ir::IrType::I64, 8, line);
            ir::IrInstr   ad{};
            ad.op          = ir::IrOp::ADD;
            ad.type        = ir::IrType::I64;
            ad.dst         = fv_plus_8;
            ad.operands    = {fv_addr, off8};
            ad.source_line = line;
            fn_->append(current_block_, std::move(ad));

            ir::IrInstr st{};
            st.op          = ir::IrOp::STORE;
            st.type        = ir::IrType::I64;
            st.operands    = {env_addr, fv_plus_8};
            st.source_line = line;
            fn_->append(current_block_, std::move(st));
        }
        return fv_addr;
    }

    // ---------------------------------------------------------------------
    // Field access: p.x  (lectura) y  p.x = v  (escritura).
    //
    // Modelo: las variables tipo struct se representan en scope como un
    // IrValueId de tipo PTR que apunta a la zona de memoria reservada
    // por ALLOCA (ver lower_var_decl, caso STRUCT).  Para acceder a un
    // campo:
    //   1. Bajar la base -> ptr al inicio del struct.
    //   2. Sumar el offset del campo (consultado al StructLayout del
    //      type checker) con un IR ADD.
    //   3. Emitir LOAD (lectura) o STORE (escritura) sobre ese puntero.
    //
    // Si offset == 0 (primer campo del struct) la suma se omite y se
    // reusa directamente el ptr base.  Esta optimizacion local evita
    // ruido en el .vel para el caso comun de "campo cero".
    // ---------------------------------------------------------------------

    ir::IrValueId Lowering::lower_field_addr(ast::FieldAccessExpr *e) {
        const ir::IrValueId base = lower_expr(e->base.get());
        if (base == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;

        const Type bt = e->base->result_type;
        if (bt.kind != PrimitiveKind::STRUCT) {
            error_at(e->loc, "lowering: '.' sobre tipo no-struct");
            return ir::IR_NO_VALUE;
        }
        const auto &layouts = tc_.struct_layouts();
        auto        it      = layouts.find(bt.struct_name);
        if (it == layouts.end()) {
            error_at(e->loc,
                     "lowering: layout no disponible para struct '" + bt.struct_name + "'");
            return ir::IR_NO_VALUE;
        }
        const StructLayout &lay    = it->second;
        uint32_t            offset = 0;
        bool                found  = false;
        for (const auto &f: lay.fields) {
            if (f.name == e->field_name) {
                offset = f.offset;
                found  = true;
                break;
            }
        }
        if (!found) {
            error_at(e->loc, "lowering: campo '" + e->field_name +
                     "' no encontrado en struct '" + bt.struct_name + "'");
            return ir::IR_NO_VALUE;
        }

        if (offset == 0) return base;

        // ptr_field = ptr_base + offset.  Tratamos los punteros como i64
        // a efectos aritmeticos (la VM no distingue tipos de puntero a
        // este nivel; la aritmetica ya escalada queda en el caller).
        const ir::IrValueId off_val  = emit_const(ir::IrType::I64, offset, e->loc.line);
        const ir::IrValueId fld_addr = fn_->new_value(ir::IrType::PTR);
        // B1 fix: heredar is_host_ptr del base.  Sin esto, el LOAD/STORE
        // posterior sobre fld_addr emite `mov` (memoria VM) cuando el base
        // es host_ptr -> lee/escribe garbage.  Caso observado:
        // `(*ptr_of(unique_struct)).y` con offset=4 leia 0 (memoria VM
        // aleatoria) en lugar del valor real del campo.
        fn_->values[fld_addr].is_host_ptr = fn_->values[base].is_host_ptr;
        ir::IrInstr         ins{};
        ins.op          = ir::IrOp::ADD;
        ins.type        = ir::IrType::PTR;
        ins.dst         = fld_addr;
        ins.operands    = {base, off_val};
        ins.source_line = e->loc.line;
        fn_->append(current_block_, std::move(ins));
        return fld_addr;
    }

    ir::IrValueId Lowering::lower_field_access(ast::FieldAccessExpr *e) {
        // ADTs: variante sin payload `Color.Red` (sin parens).  El
        // type checker la marco con property_kind=99.  Despachar al
        // constructor de variante con args vacio en lugar del manejo
        // generico de field-access (struct/clase) que fallaria al no
        // encontrar un campo llamado "Red" en un struct.
        if (e->property_kind == 99
            && e->base
            && e->base->kind == ast::NodeKind::IdentExpr) {
            auto *base_id = static_cast<ast::IdentExpr *>(e->base.get());
            static const std::vector<std::unique_ptr<ast::Expr> > empty_args;
            return lower_enum_constructor(base_id->name, e->field_name,
                                          empty_args, e->loc);
        }
        // Cross-module variante sin payload `lib.Op.Nop`.  La base es
        // FieldAccessExpr(lib, "Op") cuyo result_type es el enum type
        // (STRUCT con struct_name = mangled enum name).
        if (e->property_kind == 99
            && e->base
            && e->base->kind == ast::NodeKind::FieldAccessExpr) {
            const Type &bt = e->base->result_type;
            if (bt.kind == PrimitiveKind::STRUCT) {
                static const std::vector<std::unique_ptr<ast::Expr> > empty_args;
                return lower_enum_constructor(bt.struct_name, e->field_name,
                                              empty_args, e->loc);
            }
        }
        // Si el receptor es CLASS, ruta especifica via GETFIELD (offset
        // relativo al payload, sin header) en lugar del esquema struct
        // (LOAD desde direccion calculada).
        if (e->base && e->base->result_type.kind == PrimitiveKind::CLASS) {
            return lower_class_field_load(e);
        }
        // Limitacion G (cerrada): @c property_kind == 3 marca acceso a
        // static field via @c ClassName.field.  El base es IdentExpr cuyo
        // nombre NO es una variable (es un nombre de clase) asi que su
        // result_type es VOID/COUNT y no entra en la rama anterior.
        // Despachamos directamente a @c lower_class_field_load que sabe
        // emitir findclass + getstatic sin tocar @c base.
        if (e->property_kind == 3) {
            return lower_class_field_load(e);
        }
        // M.L7 ext: @c namespace.CONSTANT.  El type checker marca con
        // @c property_kind=4 y rellena @c ns_index para que aqui podamos
        // consultar el Sym y -- si es kind=1 (Variable/Const) con literal
        // disponible -- inlinear el valor como CONST (cero overhead vs
        // const local).
        if (e->property_kind == 4
         && e->ns_index != 0xFFFFFFFFu) {
            const auto &nss = tc_.imported_namespaces();
            if (e->ns_index < nss.size()) {
                const auto &ns = nss[e->ns_index];
                auto it_sym = ns.by_name.find(e->field_name);
                if (it_sym != ns.by_name.end()) {
                    const auto &sym = ns.symbols[it_sym->second];
                    if (sym.kind == 1 && sym.has_const_value) {
                        ir::IrType ft = ir_type_from_primitive(
                            e->result_type.kind);
                        return emit_const(ft,
                            static_cast<uint64_t>(sym.const_value),
                            e->loc.line);
                    }
                    // v4: comptime const string cross-module.  El blob
                    // se materializo en imported_global_consts_ con
                    // is_str=true.  Aqui lo emitimos como STRMAKE para
                    // obtener un StringObject usable.
                    if (sym.kind == 1
                     && sym.var_type.kind == PrimitiveKind::STRING) {
                        const auto &ics = tc_.imported_global_consts();
                        auto it_ic = ics.find(e->field_name);
                        if (it_ic != ics.end() && it_ic->second.is_str) {
                            const std::string &sv = it_ic->second.str_value;
                            std::vector<uint8_t> pbytes(sv.begin(), sv.end());
                            const uint64_t p_idx =
                                out_mod_->intern_static_data(std::move(pbytes));
                            // v4: marcar el meta como immutable + imported.
                            // El JIT puede inlinear el ptr directo (no es
                            // realocable) y el AOT lo segrega a .rodata.
                            if (p_idx < out_mod_->static_data.size()) {
                                auto &m = out_mod_->static_data.meta_at(p_idx);
                                m.flags |= ir::IrModule::SD_FLAG_IMMUTABLE;
                                m.flags |= ir::IrModule::SD_FLAG_IMPORTED;
                            }
                            ir::IrValueId v_addr =
                                fn_->new_value(ir::IrType::PTR);
                            {
                                ir::IrInstr is{};
                                is.op          = ir::IrOp::STR_LIT_ADDR;
                                is.type        = ir::IrType::PTR;
                                is.dst         = v_addr;
                                is.imm         = p_idx;
                                is.source_line = e->loc.line;
                                fn_->append(current_block_, std::move(is));
                            }
                            ir::IrValueId v_len = emit_const(
                                ir::IrType::I64,
                                static_cast<uint64_t>(sv.size()),
                                e->loc.line);
                            return emit_strmake(v_addr, v_len, e->loc.line);
                        }
                    }
                }
            }
        }
        const ir::IrValueId addr = lower_field_addr(e);
        if (addr == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;

        const ir::IrType    ft  = ir_type_from_primitive(e->result_type.kind);
        const ir::IrValueId dst = fn_->new_value(ft);
        ir::IrInstr         ins{};
        ins.op          = ir::IrOp::LOAD;
        ins.type        = ft;
        ins.dst         = dst;
        ins.operands    = {addr};
        ins.source_line = e->loc.line;
        fn_->append(current_block_, std::move(ins));

        // Propagar is_host_ptr cuando el campo es un puntero/array host.
        // Critico para casos como `(*virtual_ptr).buf` donde el struct
        // tiene field `u8* buf` -- el LOAD lee desde VM mem (porque la
        // direccion del struct es VM addr), pero el VALOR cargado ES un
        // host_ptr (resultado de malloc) y los accesos byte-level posteriores
        // necesitan emitir movh.  Sin esto, `(*vp).buf[i] = x` emite mov
        // (VM mem) en lugar de movh (host mem) -> escribe al lugar erroneo.
        if (e->result_type.kind == PrimitiveKind::PTR
         || e->result_type.kind == PrimitiveKind::ARRAY) {
            if (!e->result_type.is_virtual) {
                fn_->values[dst].is_host_ptr = true;
            }
        }
        // Si el campo es de tipo CLASS, el LOAD devuelve un host_ptr a
        // un objeto GC.  Marcar para gc-aware save/restore alrededor de
        // CALLs subsiguientes.
        if (e->result_type.kind == PrimitiveKind::CLASS) {
            fn_->values[dst].is_host_ptr  = true;
            fn_->values[dst].is_gc_object = true;
        }

        // Bit field: aplicar SHR + AND para extraer.
        // Buscamos el StructFieldInfo del campo accedido para conocer
        // bit_offset/bit_width.  Si bit_width=0, es campo normal y
        // saltamos.
        const Type bt = e->base ? e->base->result_type : Type{};
        if (bt.kind == PrimitiveKind::STRUCT) {
            const auto &layouts = tc_.struct_layouts();
            auto        it_l    = layouts.find(bt.struct_name);
            if (it_l != layouts.end()) {
                for (const auto &f: it_l->second.fields) {
                    if (f.name == e->field_name && f.bit_width > 0) {
                        // shifted = dst >> bit_offset; masked = shifted & mask.
                        ir::IrValueId v_shifted = dst;
                        if (f.bit_offset > 0) {
                            ir::IrValueId v_shamt = emit_const(ft,
                                                               (uint64_t) f.bit_offset, e->loc.line);
                            v_shifted = fn_->new_value(ft);
                            ir::IrInstr sh{};
                            sh.op          = ir::IrOp::SHR;
                            sh.type        = ft;
                            sh.dst         = v_shifted;
                            sh.operands    = {dst, v_shamt};
                            sh.source_line = e->loc.line;
                            fn_->append(current_block_, std::move(sh));
                        }
                        // mask = (1 << bit_width) - 1.
                        const uint64_t mask = (f.bit_width == 64)
                                                  ? UINT64_MAX
                                                  : ((uint64_t(1) << f.bit_width) - 1);
                        ir::IrValueId v_mask   = emit_const(ft, mask, e->loc.line);
                        ir::IrValueId v_masked = fn_->new_value(ft);
                        ir::IrInstr   an{};
                        an.op          = ir::IrOp::AND;
                        an.type        = ft;
                        an.dst         = v_masked;
                        an.operands    = {v_shifted, v_mask};
                        an.source_line = e->loc.line;
                        fn_->append(current_block_, std::move(an));
                        return v_masked;
                    }
                }
            }
        }
        return dst;
    }

    ir::IrValueId Lowering::lower_binary(ast::BinaryExpr *e) {
        // Operator overloading (dunder): el type checker dejo el metodo
        // __op__ en e->overload_method.  Despachamos a lhs.__op__(rhs)
        // sintetizando un CallExpr y bajandolo con lower_call.
        if (!e->overload_method.empty() && e->lhs && e->rhs) {
            auto meth = std::make_unique<ast::FieldAccessExpr>();
            meth->base = std::move(e->lhs);
            meth->field_name = e->overload_method;
            meth->property_kind = 0;
            meth->loc = e->loc;
            meth->result_type = e->result_type;

            auto call = std::make_unique<ast::CallExpr>();
            call->callee = std::move(meth);
            call->loc = e->loc;
            call->result_type = e->result_type;
            call->args.push_back(std::move(e->rhs));

            return lower_call(call.get());
        }

        // Tipos canonicos del checker.
        const PrimitiveKind ltk = e->lhs ? e->lhs->result_type.kind : PrimitiveKind::COUNT;
        const PrimitiveKind rtk = e->rhs ? e->rhs->result_type.kind : PrimitiveKind::COUNT;

        // Bug fix 2026-05-23 (LR2): struct == struct via comparacion
        // field-a-field.  El type checker valida que ambos lados sean el
        // mismo struct nombrado.  Lowering emite: para cada campo, LOAD
        // del field en cada lado, CMP_EQ, AND acumulado.  Resultado BOOL.
        // Solo == y != (los otros operadores no aplican).
        if ((e->op == ast::BinOp::Eq || e->op == ast::BinOp::Neq)
         && ltk == PrimitiveKind::STRUCT
         && rtk == PrimitiveKind::STRUCT
         && e->lhs && e->rhs
         && e->lhs->result_type.struct_name == e->rhs->result_type.struct_name
         && !e->lhs->result_type.struct_name.empty()) {
            const auto &name = e->lhs->result_type.struct_name;
            auto it_sl = tc_.struct_layouts().find(name);
            if (it_sl != tc_.struct_layouts().end()) {
                const StructLayout &lay = it_sl->second;
                const ir::IrValueId lhs_addr = lower_expr(e->lhs.get());
                const ir::IrValueId rhs_addr = lower_expr(e->rhs.get());
                if (lhs_addr == ir::IR_NO_VALUE || rhs_addr == ir::IR_NO_VALUE)
                    return ir::IR_NO_VALUE;
                // Comparar field por field.  Acumular result en v_acc (BOOL).
                ir::IrValueId v_acc = emit_const(ir::IrType::I64, 1, e->loc.line);
                // Set de offsets ya comparados (evita re-comparar bit field
                // packed words).
                std::set<uint32_t> compared_offsets;
                for (const auto &f : lay.fields) {
                    if (f.bit_width > 0) {
                        // Bit field: comparamos el storage word completo
                        // (size_bytes en el offset) una sola vez por offset.
                        if (compared_offsets.count(f.offset)) continue;
                        compared_offsets.insert(f.offset);
                    }
                    const ir::IrType field_ir = ir_type_from_primitive(f.type.kind);
                    // addr_lhs = lhs + offset
                    ir::IrValueId v_off = emit_const(ir::IrType::I64,
                                                      (uint64_t)f.offset, e->loc.line);
                    ir::IrValueId v_lhs_at = fn_->new_value(ir::IrType::PTR);
                    {
                        ir::IrInstr add{};
                        add.op = ir::IrOp::ADD; add.type = ir::IrType::I64;
                        add.dst = v_lhs_at; add.operands = {lhs_addr, v_off};
                        add.source_line = e->loc.line;
                        fn_->append(current_block_, std::move(add));
                    }
                    ir::IrValueId v_off2 = emit_const(ir::IrType::I64,
                                                       (uint64_t)f.offset, e->loc.line);
                    ir::IrValueId v_rhs_at = fn_->new_value(ir::IrType::PTR);
                    {
                        ir::IrInstr add{};
                        add.op = ir::IrOp::ADD; add.type = ir::IrType::I64;
                        add.dst = v_rhs_at; add.operands = {rhs_addr, v_off2};
                        add.source_line = e->loc.line;
                        fn_->append(current_block_, std::move(add));
                    }
                    // LOAD a y b
                    ir::IrValueId v_a = fn_->new_value(field_ir);
                    {
                        ir::IrInstr ld{};
                        ld.op = ir::IrOp::LOAD; ld.type = field_ir;
                        ld.dst = v_a; ld.operands = {v_lhs_at};
                        ld.source_line = e->loc.line;
                        fn_->append(current_block_, std::move(ld));
                    }
                    ir::IrValueId v_b = fn_->new_value(field_ir);
                    {
                        ir::IrInstr ld{};
                        ld.op = ir::IrOp::LOAD; ld.type = field_ir;
                        ld.dst = v_b; ld.operands = {v_rhs_at};
                        ld.source_line = e->loc.line;
                        fn_->append(current_block_, std::move(ld));
                    }
                    // cmp_eq a, b -> v_field_eq
                    ir::IrValueId v_field_eq = fn_->new_value(ir::IrType::BOOL);
                    {
                        ir::IrInstr cmp{};
                        cmp.op = ir::IrOp::CMP_EQ; cmp.type = ir::IrType::BOOL;
                        cmp.dst = v_field_eq;
                        cmp.operands = {v_a, v_b};
                        cmp.source_line = e->loc.line;
                        fn_->append(current_block_, std::move(cmp));
                    }
                    // v_acc = v_acc & v_field_eq
                    ir::IrValueId v_new_acc = fn_->new_value(ir::IrType::I64);
                    {
                        ir::IrInstr an{};
                        an.op = ir::IrOp::AND; an.type = ir::IrType::I64;
                        an.dst = v_new_acc;
                        an.operands = {v_acc, v_field_eq};
                        an.source_line = e->loc.line;
                        fn_->append(current_block_, std::move(an));
                    }
                    v_acc = v_new_acc;
                }
                // Para !=, negamos via XOR con 1.
                if (e->op == ast::BinOp::Neq) {
                    ir::IrValueId v_one = emit_const(ir::IrType::I64, 1, e->loc.line);
                    ir::IrValueId v_neg = fn_->new_value(ir::IrType::I64);
                    ir::IrInstr xo{};
                    xo.op = ir::IrOp::XOR; xo.type = ir::IrType::I64;
                    xo.dst = v_neg;
                    xo.operands = {v_acc, v_one};
                    xo.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(xo));
                    v_acc = v_neg;
                }
                return v_acc;
            }
        }

        // Short-circuit evaluation para `&&` y `||`.  Sin esto, ambos
        // operandos se evalúan siempre, lo que es incorrecto para patrones
        // como `i > 0 && this.data[i - 1] != 10` (con i==0, el rhs leeria
        // data[-1] y crashearia).  Ademas se evita evaluar efectos
        // colaterales innecesarios (CALLs en el rhs, dereferencias etc).
        //
        // Estrategia: usar PHI en el merge.  El predecesor del lhs aporta
        // el valor por defecto (false para &&, true para ||); el predecesor
        // del rhs aporta el valor del rhs.  Sin ALLOCA -- importante en
        // bucles, donde un ALLOCA en la condicion del while crearia un
        // ALLOCA por iteracion (stack growth ilimitado).
        if (e->op == ast::BinOp::LogicalAnd
            || e->op == ast::BinOp::LogicalOr) {
            const bool is_and = (e->op == ast::BinOp::LogicalAnd);
            // 1) Bajar lhs.  Si la propia lhs lleva un short-circuit anidado
            //    @c current_block_ ya no es el original; lo capturamos tras
            //    el lower_expr para anclar correctamente las aristas CFG.
            const ir::IrValueId v_lhs = lower_expr(e->lhs.get());
            if (v_lhs == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            const ir::IrBlockId lhs_end_bb = current_block_;
            const ir::IrBlockId rhs_bb     = fn_->new_block(is_and ? "andsc_rhs" : "orsc_rhs");
            const ir::IrBlockId default_bb = fn_->new_block(is_and ? "andsc_def" : "orsc_def");
            const ir::IrBlockId merge_bb   = fn_->new_block(is_and ? "andsc_merge" : "orsc_merge");
            // 2) BR_COND lhs:
            //   && : true -> rhs_bb (evaluar rhs); false -> default_bb (false)
            //   || : true -> default_bb (true);    false -> rhs_bb (evaluar rhs)
            {
                ir::IrInstr br{};
                br.op           = ir::IrOp::BR_COND;
                br.type         = ir::IrType::VOID;
                br.operands     = {v_lhs};
                br.target_block = is_and ? rhs_bb : default_bb;
                br.false_block  = is_and ? default_bb : rhs_bb;
                br.source_line  = e->loc.line;
                fn_->append(lhs_end_bb, std::move(br));
            }
            // CFG: lhs_end_bb -> {rhs_bb, default_bb}.  CRITICO: sin esto el
            // dataflow de liveness no puede propagar valores back-edge a
            // traves del CFG -> el regalloc reusa registros de valores aun
            // vivos -> crash en runtime.
            fn_->blocks[lhs_end_bb].succs.push_back(rhs_bb);
            fn_->blocks[lhs_end_bb].succs.push_back(default_bb);
            fn_->blocks[rhs_bb].preds.push_back(lhs_end_bb);
            fn_->blocks[default_bb].preds.push_back(lhs_end_bb);
            // 3) Bloque default: emitir const por defecto y BR merge.
            current_block_                = default_bb;
            block_terminated_             = false;
            const ir::IrValueId v_default = emit_const(
                ir::IrType::BOOL, is_and ? 0u : 1u, e->loc.line); {
                ir::IrInstr brd{};
                brd.op           = ir::IrOp::BR;
                brd.type         = ir::IrType::VOID;
                brd.target_block = merge_bb;
                brd.source_line  = e->loc.line;
                fn_->append(current_block_, std::move(brd));
            }
            fn_->blocks[default_bb].succs.push_back(merge_bb);
            fn_->blocks[merge_bb].preds.push_back(default_bb);
            const ir::IrBlockId default_pred = default_bb;
            // 4) Bloque rhs: bajar rhs (puede crear bloques intermedios si
            //    el rhs tiene su propio short-circuit), capturar el bloque
            //    final donde queda el resultado, y BR al merge desde ahi.
            current_block_            = rhs_bb;
            block_terminated_         = false;
            const ir::IrValueId v_rhs = lower_expr(e->rhs.get());
            if (v_rhs == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            const ir::IrValueId v_rhs_b = cast_if_needed(
                v_rhs, fn_->values[v_rhs].type, ir::IrType::BOOL, e->loc.line);
            const ir::IrBlockId rhs_pred = current_block_;
            if (!block_terminated_) {
                ir::IrInstr brm{};
                brm.op           = ir::IrOp::BR;
                brm.type         = ir::IrType::VOID;
                brm.target_block = merge_bb;
                brm.source_line  = e->loc.line;
                fn_->append(current_block_, std::move(brm));
                fn_->blocks[rhs_pred].succs.push_back(merge_bb);
                fn_->blocks[merge_bb].preds.push_back(rhs_pred);
            }
            // 5) Bloque merge: PHI(default desde default_pred, rhs desde rhs_pred).
            current_block_            = merge_bb;
            block_terminated_         = false;
            const ir::IrValueId v_res = fn_->new_value(ir::IrType::BOOL);
            ir::IrInstr         phi{};
            phi.op   = ir::IrOp::PHI;
            phi.type = ir::IrType::BOOL;
            phi.dst  = v_res;
            phi.phi_args.push_back({v_default, default_pred});
            phi.phi_args.push_back({v_rhs_b, rhs_pred});
            phi.source_line = e->loc.line;
            fn_->append(current_block_, std::move(phi));
            return v_res;
        }

        // Operadores nativos para STRING.
        //   s + t   -> strcat (ROPE O(1)).  Resultado tipo STRING.
        //   s == t  -> strcmp + cmp_eq con 0.  Resultado tipo BOOL.
        //   s != t  -> strcmp + cmp_ne con 0.  Resultado tipo BOOL.
        // Auto-coerce de literales: si un operando es un literal de string
        // (no interpolado, tipo PTR) y el otro es STRING, se promueve el
        // literal a StringObject via STRMAKE para no romper la regla de
        // "ambos operandos STRING" en el bytecode.
        auto coerce_string_operand = [&](ast::Expr *ex) -> ir::IrValueId {
            if (ex && ex->kind == ast::NodeKind::StringLitExpr) {
                auto *sl = static_cast<ast::StringLitExpr *>(ex);
                /* Interpolados TAMBIEN se promueven: lower_string_literal_
                 * to_string_object construye la cadena de trozos
                 * (STRMAKE+STRCAT).  Excluirlos (como se hacia antes) hacia
                 * que `s + "a ${x}"` o `s += "${x}"` cayera en la ruta
                 * aritmetica generica que no concatena (o peor, devolvia
                 * cadena vacia / crasheaba). */
                return lower_string_literal_to_string_object(sl);
            }
            return lower_expr(ex);
        };
        /* En el body de @Macro los StringLitExpr pueden no haber pasado
         * por check_string (que setea result_type=PTR).  Aceptamos
         * literales como strings aunque su result_type sea VOID -- solo
         * importa el StringLitExpr kind.  Incluye interpolados. */
        auto is_string_lit_node = [](const ast::Expr *ex) -> bool {
            return ex && ex->kind == ast::NodeKind::StringLitExpr;
        };
        const bool lhs_is_str = (ltk == PrimitiveKind::STRING) ||
            (is_string_lit_node(e->lhs.get())
             && (ltk == PrimitiveKind::PTR || ltk == PrimitiveKind::VOID));
        const bool rhs_is_str = (rtk == PrimitiveKind::STRING) ||
            (is_string_lit_node(e->rhs.get())
             && (rtk == PrimitiveKind::PTR || rtk == PrimitiveKind::VOID));
        const bool any_real_str = (ltk == PrimitiveKind::STRING) ||
                (rtk == PrimitiveKind::STRING);
        if (lhs_is_str && rhs_is_str && any_real_str) {
            ir::IrValueId v_a = coerce_string_operand(e->lhs.get());
            ir::IrValueId v_b = coerce_string_operand(e->rhs.get());
            if (v_a == ir::IR_NO_VALUE || v_b == ir::IR_NO_VALUE)
                return ir::IR_NO_VALUE;
            if (e->op == ast::BinOp::Add) {
                // strcat -> rope handle (i64 STRING).
                return emit_strcat(v_a, v_b, e->loc.line);
            }
            if (e->op == ast::BinOp::Eq || e->op == ast::BinOp::Neq) {
                ir::IrValueId v_cmp = fn_->new_value(ir::IrType::I64);
                ir::IrInstr   ra{};
                ra.op          = ir::IrOp::STRCMP;
                ra.type        = ir::IrType::I64;
                ra.dst         = v_cmp;
                ra.operands    = {v_a, v_b};
                ra.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ra));
                ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0, e->loc.line);
                ir::IrValueId v_bool = fn_->new_value(ir::IrType::BOOL);
                ir::IrInstr   cmp{};
                cmp.op = (e->op == ast::BinOp::Eq)
                             ? ir::IrOp::CMP_EQ
                             : ir::IrOp::CMP_NE;
                cmp.type        = ir::IrType::BOOL;
                cmp.dst         = v_bool;
                cmp.operands    = {v_cmp, v_zero};
                cmp.source_line = e->loc.line;
                fn_->append(current_block_, std::move(cmp));
                return v_bool;
            }
            error_at(e->loc, "operador no soportado entre strings");
            return ir::IR_NO_VALUE;
        }

        // Aritmetica puntero (PTR + int, PTR - int, PTR - PTR).  El type
        // checker ya valido las combinaciones; aqui escalamos el offset
        // por sizeof(*ptr) y emitimos ADD/SUB.  Aceptamos tambien ARRAY
        // como base para soportar `arr + n` (decay implicito).
        if ((e->op == ast::BinOp::Add || e->op == ast::BinOp::Sub)
            && (ltk == PrimitiveKind::PTR || ltk == PrimitiveKind::ARRAY)
            && is_integral(rtk)) {
            const Type pty = e->lhs->result_type;
            if (!pty.pointee) {
                error_at(e->loc, "lowering: aritmetica de puntero sin pointee");
                return ir::IR_NO_VALUE;
            }
            const size_t esz = size_of_type(*pty.pointee);
            if (esz == 0) {
                error_at(e->loc,
                         "lowering: aritmetica sobre void* o pointee con sizeof 0");
                return ir::IR_NO_VALUE;
            }
            ir::IrValueId base_v = lower_expr(e->lhs.get());
            ir::IrValueId idx_v  = lower_expr(e->rhs.get());
            if (base_v == ir::IR_NO_VALUE || idx_v == ir::IR_NO_VALUE)
                return ir::IR_NO_VALUE;
            idx_v = cast_if_needed(idx_v, fn_->values[idx_v].type, ir::IrType::I64,
                                   e->loc.line);
            ir::IrValueId offset = idx_v;
            if (esz != 1) {
                const ir::IrValueId sz_v = emit_const(ir::IrType::I64,
                                                      (uint64_t) esz, e->loc.line);
                const ir::IrValueId scaled = fn_->new_value(ir::IrType::I64);
                ir::IrInstr         mul{};
                mul.op          = ir::IrOp::MUL;
                mul.type        = ir::IrType::I64;
                mul.dst         = scaled;
                mul.operands    = {idx_v, sz_v};
                mul.source_line = e->loc.line;
                fn_->append(current_block_, std::move(mul));
                offset = scaled;
            }
            const ir::IrValueId dst = fn_->new_value(ir::IrType::PTR);
            // Propagar el flag is_host_ptr desde el puntero base.  El
            // resultado de la aritmetica sigue apuntando al mismo espacio
            // (host o VM) que el operando original.
            fn_->values[dst].is_host_ptr = fn_->values[base_v].is_host_ptr;
            ir::IrInstr ins{};
            ins.op = (e->op == ast::BinOp::Add)
                         ? ir::IrOp::ADD
                         : ir::IrOp::SUB;
            ins.type        = ir::IrType::PTR;
            ins.dst         = dst;
            ins.operands    = {base_v, offset};
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            return dst;
        }

        // PTR - PTR -> i64 (numero de elementos).  Calcula (a - b) / sizeof(*p).
        if (e->op == ast::BinOp::Sub
            && ltk == PrimitiveKind::PTR && rtk == PrimitiveKind::PTR) {
            const Type   pty = e->lhs->result_type;
            const size_t esz = (pty.pointee ? size_of_type(*pty.pointee) : 0);
            if (esz == 0) {
                error_at(e->loc, "lowering: p - q requiere pointee con sizeof > 0");
                return ir::IR_NO_VALUE;
            }
            const ir::IrValueId la = lower_expr(e->lhs.get());
            const ir::IrValueId lb = lower_expr(e->rhs.get());
            if (la == ir::IR_NO_VALUE || lb == ir::IR_NO_VALUE)
                return ir::IR_NO_VALUE;
            const ir::IrValueId diff = fn_->new_value(ir::IrType::I64);
            ir::IrInstr         sub{};
            sub.op          = ir::IrOp::SUB;
            sub.type        = ir::IrType::I64;
            sub.dst         = diff;
            sub.operands    = {la, lb};
            sub.source_line = e->loc.line;
            fn_->append(current_block_, std::move(sub));
            if (esz == 1) return diff;
            const ir::IrValueId sz_v = emit_const(ir::IrType::I64, (uint64_t) esz,
                                                  e->loc.line);
            const ir::IrValueId q = fn_->new_value(ir::IrType::I64);
            ir::IrInstr         div{};
            div.op          = ir::IrOp::DIV;
            div.type        = ir::IrType::I64;
            div.dst         = q;
            div.operands    = {diff, sz_v};
            div.source_line = e->loc.line;
            fn_->append(current_block_, std::move(div));
            return q;
        }

        // Comparaciones de PTR vs PTR: tratamos como uint64 sin promocion.
        const bool is_ptr_cmp =
                (ltk == PrimitiveKind::PTR && rtk == PrimitiveKind::PTR)
                && (e->op == ast::BinOp::Eq || e->op == ast::BinOp::Neq
                    || e->op == ast::BinOp::Lt || e->op == ast::BinOp::Le
                    || e->op == ast::BinOp::Gt || e->op == ast::BinOp::Ge);

        ir::IrValueId l = lower_expr(e->lhs.get());
        ir::IrValueId r = lower_expr(e->rhs.get());
        if (l == ir::IR_NO_VALUE || r == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;

        if (is_ptr_cmp) {
            // Saltar la promocion numerica y emitir directamente CMP_*
            // (variantes unsigned para comparaciones de orden).
            ir::IrOp op = ir::IrOp::CMP_EQ;
            switch (e->op) {
                case ast::BinOp::Eq: op = ir::IrOp::CMP_EQ;
                    break;
                case ast::BinOp::Neq: op = ir::IrOp::CMP_NE;
                    break;
                case ast::BinOp::Lt: op = ir::IrOp::CMP_ULT;
                    break;
                case ast::BinOp::Le: op = ir::IrOp::CMP_ULE;
                    break;
                case ast::BinOp::Gt: op = ir::IrOp::CMP_UGT;
                    break;
                case ast::BinOp::Ge: op = ir::IrOp::CMP_UGE;
                    break;
                default: break;
            }
            const ir::IrValueId dst = fn_->new_value(ir::IrType::BOOL);
            ir::IrInstr         ins{};
            ins.op          = op;
            ins.type        = ir::IrType::BOOL;
            ins.dst         = dst;
            ins.operands    = {l, r};
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            return dst;
        }

        // Para operadores aritmeticos / bitwise / comparacion: promovemos
        // ambos operandos al tipo comun.
        const PrimitiveKind common = (ltk == PrimitiveKind::BOOL && rtk == PrimitiveKind::BOOL)
                                         ? PrimitiveKind::BOOL
                                         : promote_arith(ltk, rtk);
        const ir::IrType common_ir = ir_type_from_primitive(common);
        const bool       is_float  = is_floating(common);
        const bool       is_unsign = is_integral(common) && !is_signed_integral(common);

        l = cast_if_needed(l, ir_type_from_primitive(ltk), common_ir, e->loc.line);
        r = cast_if_needed(r, ir_type_from_primitive(rtk), common_ir, e->loc.line);

        // Seleccionar opcode segun categoria.
        ir::IrOp   op        = ir::IrOp::ADD;
        ir::IrType result_ir = common_ir;
        switch (e->op) {
            case ast::BinOp::Add: op = is_float ? ir::IrOp::FADD : ir::IrOp::ADD;
                break;
            case ast::BinOp::Sub: op = is_float ? ir::IrOp::FSUB : ir::IrOp::SUB;
                break;
            case ast::BinOp::Mul: op = is_float ? ir::IrOp::FMUL : ir::IrOp::MUL;
                break;
            case ast::BinOp::Div: op = is_float ? ir::IrOp::FDIV : ir::IrOp::DIV;
                break;
            case ast::BinOp::Mod: op = ir::IrOp::MOD;
                break;

            case ast::BinOp::Eq:
                op = is_float ? ir::IrOp::FCMP_EQ : ir::IrOp::CMP_EQ;
                result_ir = ir::IrType::BOOL;
                break;
            case ast::BinOp::Neq:
                op = is_float ? ir::IrOp::FCMP_NE : ir::IrOp::CMP_NE;
                result_ir = ir::IrType::BOOL;
                break;
            case ast::BinOp::Lt:
                op = is_float
                         ? ir::IrOp::FCMP_LT
                         : (is_unsign ? ir::IrOp::CMP_ULT : ir::IrOp::CMP_LT);
                result_ir = ir::IrType::BOOL;
                break;
            case ast::BinOp::Le:
                op = is_float
                         ? ir::IrOp::FCMP_LE
                         : (is_unsign ? ir::IrOp::CMP_ULE : ir::IrOp::CMP_LE);
                result_ir = ir::IrType::BOOL;
                break;
            case ast::BinOp::Gt:
                op = is_float
                         ? ir::IrOp::FCMP_GT
                         : (is_unsign ? ir::IrOp::CMP_UGT : ir::IrOp::CMP_GT);
                result_ir = ir::IrType::BOOL;
                break;
            case ast::BinOp::Ge:
                op = is_float
                         ? ir::IrOp::FCMP_GE
                         : (is_unsign ? ir::IrOp::CMP_UGE : ir::IrOp::CMP_GE);
                result_ir = ir::IrType::BOOL;
                break;

            case ast::BinOp::LogicalAnd: op = ir::IrOp::AND;
                result_ir = ir::IrType::BOOL;
                break;
            case ast::BinOp::LogicalOr: op = ir::IrOp::OR;
                result_ir = ir::IrType::BOOL;
                break;

            case ast::BinOp::BitAnd: op = ir::IrOp::AND;
                break;
            case ast::BinOp::BitOr: op = ir::IrOp::OR;
                break;
            case ast::BinOp::BitXor: op = ir::IrOp::XOR;
                break;
            case ast::BinOp::Shl: op = ir::IrOp::SHL;
                break;
            case ast::BinOp::Shr: op = is_unsign ? ir::IrOp::SHR : ir::IrOp::SAR;
                break;
        }

        const ir::IrValueId dst = fn_->new_value(result_ir);
        ir::IrInstr         ins{};
        ins.op          = op;
        ins.type        = result_ir;
        ins.dst         = dst;
        ins.operands    = {l, r};
        ins.source_line = e->loc.line;
        fn_->append(current_block_, std::move(ins));
        return dst;
    }

    ir::IrValueId Lowering::lower_unary(ast::UnaryExpr *e) {
        // Caso especial: ++/-- requieren leer la variable, sumar/restar 1
        // y reescribir el valor.  Para variables address-taken pasamos por
        // LOAD/STORE; para SSA puro hacemos update_scope (Braun on-the-fly).
        if (e->op == ast::UnOp::PreInc || e->op == ast::UnOp::PreDec
            || e->op == ast::UnOp::PostInc || e->op == ast::UnOp::PostDec) {
            if (!e->operand) {
                error_at(e->loc, "lowering: ++/-- sin operando");
                return ir::IR_NO_VALUE;
            }
            const ir::IrType vt = ir_type_from_primitive(e->operand->result_type.kind);
            const bool is_inc = (e->op == ast::UnOp::PreInc || e->op == ast::UnOp::PostInc);
            const bool is_pre = (e->op == ast::UnOp::PreInc || e->op == ast::UnOp::PreDec);
            auto compute_new = [&](ir::IrValueId old_val) -> ir::IrValueId {
                const ir::IrValueId one = emit_const(vt, 1, e->loc.line);
                const ir::IrValueId nv  = fn_->new_value(vt);
                ir::IrInstr o{};
                o.op = is_inc ? ir::IrOp::ADD : ir::IrOp::SUB;
                o.type = vt; o.dst = nv; o.operands = {old_val, one};
                o.source_line = e->loc.line;
                fn_->append(current_block_, std::move(o));
                return nv;
            };
            // IdentExpr: ruta original via read_local/write_local.
            if (e->operand->kind == ast::NodeKind::IdentExpr) {
                auto *id = static_cast<ast::IdentExpr *>(e->operand.get());
                const ir::IrValueId old_val = read_local(id->name, vt, e->loc.line);
                if (old_val == ir::IR_NO_VALUE) {
                    error_at(e->loc, "lowering: nombre no resuelto: '" + id->name + "'");
                    return ir::IR_NO_VALUE;
                }
                const ir::IrValueId new_val = compute_new(old_val);
                write_local(id->name, new_val, vt, e->loc.line);
                return is_pre ? new_val : old_val;
            }
            // bug4: FieldAccessExpr (this.x++, obj.x++) sobre struct field.
            // Class fields se manejan separadamente (CLASS property o getfield).
            if (e->operand->kind == ast::NodeKind::FieldAccessExpr) {
                auto *fa = static_cast<ast::FieldAccessExpr *>(e->operand.get());
                // LOAD valor actual via lower_field_access (genera ADD off + LOAD).
                const ir::IrValueId old_val = lower_field_access(fa);
                if (old_val == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
                const ir::IrValueId new_val = compute_new(old_val);
                // STORE new_val a la misma direccion via assign synthetic.
                // Construir un AssignExpr ad-hoc para reusar lower_assign.
                ast::AssignExpr asn;
                asn.op = ast::AssignOp::Assign;
                asn.loc = e->loc;
                // No podemos mover el operand; pero podemos usar el ptr
                // directamente.  Mejor: emit STORE manual al field addr.
                if (fa->base && fa->base->result_type.kind == PrimitiveKind::STRUCT) {
                    const ir::IrValueId addr = lower_field_addr(fa);
                    if (addr == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
                    ir::IrInstr st{};
                    st.op = ir::IrOp::STORE;
                    st.type = vt;
                    st.operands = {new_val, addr};
                    st.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(st));
                    return is_pre ? new_val : old_val;
                }
                // CLASS field: usar setfield via lower_class_field_store.
                if (fa->base && fa->base->result_type.kind == PrimitiveKind::CLASS) {
                    lower_class_field_store(fa, new_val, e->loc);
                    return is_pre ? new_val : old_val;
                }
                error_at(e->loc, "lowering: ++/-- sobre field no soportado en este contexto");
                return ir::IR_NO_VALUE;
            }
            // IndexExpr (arr[i]++).
            if (e->operand->kind == ast::NodeKind::IndexExpr) {
                auto *ix = static_cast<ast::IndexExpr *>(e->operand.get());
                const ir::IrValueId addr = lower_index_addr(ix);
                if (addr == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
                // LOAD valor actual.
                const ir::IrValueId old_val = fn_->new_value(vt);
                ir::IrInstr ld{};
                ld.op = ir::IrOp::LOAD; ld.type = vt; ld.dst = old_val;
                ld.operands = {addr}; ld.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ld));
                const ir::IrValueId new_val = compute_new(old_val);
                ir::IrInstr st{};
                st.op = ir::IrOp::STORE; st.type = vt;
                st.operands = {new_val, addr}; st.source_line = e->loc.line;
                fn_->append(current_block_, std::move(st));
                return is_pre ? new_val : old_val;
            }
            // UnaryExpr Deref (*p++).
            if (e->operand->kind == ast::NodeKind::UnaryExpr) {
                auto *un = static_cast<ast::UnaryExpr *>(e->operand.get());
                if (un->op == ast::UnOp::Deref) {
                    const ir::IrValueId addr = lower_expr(un->operand.get());
                    if (addr == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
                    const ir::IrValueId old_val = fn_->new_value(vt);
                    ir::IrInstr ld{};
                    ld.op = ir::IrOp::LOAD; ld.type = vt; ld.dst = old_val;
                    ld.operands = {addr}; ld.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(ld));
                    const ir::IrValueId new_val = compute_new(old_val);
                    ir::IrInstr st{};
                    st.op = ir::IrOp::STORE; st.type = vt;
                    st.operands = {new_val, addr}; st.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(st));
                    return is_pre ? new_val : old_val;
                }
            }
            error_at(e->loc, "lowering: ++/-- requieren un lvalue");
            return ir::IR_NO_VALUE;
        }

        // AddrOf: devolver la direccion del lvalue.
        if (e->op == ast::UnOp::AddrOf) {
            if (!e->operand) {
                error_at(e->loc, "lowering: '&' sin operando");
                return ir::IR_NO_VALUE;
            }
            // & sobre IdentExpr local address-taken: scope guarda la addr.
            if (e->operand->kind == ast::NodeKind::IdentExpr) {
                auto *              id   = static_cast<ast::IdentExpr *>(e->operand.get());
                const ir::IrValueId addr = lookup(id->name);
                if (addr == ir::IR_NO_VALUE) {
                    error_at(e->loc, "lowering: nombre no resuelto: '" + id->name + "'");
                    return ir::IR_NO_VALUE;
                }
                if (!address_taken_locals_.count(id->name)
                    && e->operand->result_type.kind != PrimitiveKind::STRUCT) {
                    // Defensa: el pre-pase deberia haber marcado esta var,
                    // pero si por alguna razon no lo hizo, emitir error
                    // claro en lugar de devolver una SSA value como addr.
                    error_at(e->loc,
                             "lowering: '&" + id->name + "' sobre variable no promocionada");
                    return ir::IR_NO_VALUE;
                }
                return addr;
            }
            // & sobre p.x: la direccion del campo es lower_field_addr.
            if (e->operand->kind == ast::NodeKind::FieldAccessExpr) {
                return lower_field_addr(static_cast<ast::FieldAccessExpr *>(e->operand.get()));
            }
            // & sobre p[i]: la direccion del elemento es lower_index_addr.
            if (e->operand->kind == ast::NodeKind::IndexExpr) {
                return lower_index_addr(static_cast<ast::IndexExpr *>(e->operand.get()));
            }
            // & sobre *p (idempotente): devolvemos el propio puntero.
            if (e->operand->kind == ast::NodeKind::UnaryExpr) {
                auto *un = static_cast<ast::UnaryExpr *>(e->operand.get());
                if (un->op == ast::UnOp::Deref) {
                    return lower_expr(un->operand.get());
                }
            }
            error_at(e->loc, "lowering: '&' aplicado a un no-lvalue");
            return ir::IR_NO_VALUE;
        }

        // Deref: emit LOAD desde el puntero.
        if (e->op == ast::UnOp::Deref) {
            const ir::IrValueId p = lower_expr(e->operand.get());
            if (p == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            // B1 fix: para STRUCT/ARRAY/OPTIONAL/RESULT, el "valor"
            // semantico es el propio PTR al buffer; un `LOAD ptr` cargaria
            // erroneamente los primeros 8 bytes del struct como si fueran
            // otro puntero (bug observado en `(*ptr_of(unique_struct)).x`
            // que emitia 2 LOADs encadenados).  Pass-through: devolvemos
            // el ptr tal cual y deja que el field-access posterior haga
            // ADD offset + LOAD i32 correctamente.
            //
            // Para CLASS aplicamos el mismo pass-through: el `*obj` con
            // obj=Class no tiene sentido como "leer el header"; el campo
            // access posterior (.field) lee i32 del offset apropiado.
            if (e->result_type.kind == PrimitiveKind::STRUCT
             || e->result_type.kind == PrimitiveKind::ARRAY
             || e->result_type.kind == PrimitiveKind::OPTIONAL
             || e->result_type.kind == PrimitiveKind::RESULT
             || e->result_type.kind == PrimitiveKind::CLASS) {
                // Limpiar is_virtual del ptr origen (si aplicable) antes
                // de devolverlo, igual que en el path LOAD normal.
                if (e->operand && e->operand->result_type.is_virtual) {
                    fn_->values[p].is_host_ptr        = false;
                    fn_->values[p].pointee_is_host_ptr = false;
                }
                return p;
            }
            // Fix defensivo VirtualPtr: un VirtualPtr<T> es por definicion una
            // direccion en el espacio de memoria virtual de la VM.  Si por
            // propagacion de is_host_ptr (emit_field_addr, copy-prop del IR
            // optimizer, etc.) el flag quedo marcado en el SSA value del
            // puntero, limpiarlo aqui antes de emitir el LOAD.  Sin esto,
            // el emitter IR elige 'movh' (acceso a memoria host) en lugar
            // de 'mov' (acceso VM) y causa SIGSEGV al intentar desreferenciar
            // una direccion virtual de la VM como si fuera puntero del host.
            if (e->operand && e->operand->result_type.is_virtual) {
                fn_->values[p].is_host_ptr        = false;
                fn_->values[p].pointee_is_host_ptr = false;
            }
            const ir::IrType    ft  = ir_type_from_primitive(e->result_type.kind);
            const ir::IrValueId dst = fn_->new_value(ft);
            ir::IrInstr         ins{};
            ins.op          = ir::IrOp::LOAD;
            ins.type        = ft;
            ins.dst         = dst;
            ins.operands    = {p};
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            // Limitacion A (cerrada) parte 2: si el puntero p apunta a un
            // slot VM cuyo CONTENIDO es un host_ptr (caso indirecto via
            // address-of: @c i32** pp = &p; *pp), propagar @c is_host_ptr
            // al destino del LOAD.  Sin esto, un STORE posterior usando
            // dst como puntero (e.g. @c **pp = v) emitiria mov en lugar
            // de movh y corromperia memoria VM.  El bit lo marco
            // @c write_local en el SSA value del slot.
            if (fn_->values[p].pointee_is_host_ptr) {
                fn_->values[dst].is_host_ptr = true;
            }
            // Multi-nivel de punteros host (i64****, etc.): cada deref
            // devuelve un valor que ES OTRO puntero host (apunta a una
            // celda en memoria del host malloc'eado).  Sin propagar el
            // bit, el siguiente deref emitiria mov (memoria VM) en
            // lugar de movh (memoria host) y leeria garbage.
            //
            // Heuristica: si el operando es un puntero host (is_host_ptr)
            // Y el tipo de resultado del deref es OTRO puntero (PTR no
            // virtual), entonces el valor cargado tambien es host_ptr.
            // Lo mismo simetricamente para VirtualPtr<VirtualPtr<...>>:
            // si el operando es VirtualPtr y el resultado es OTRO
            // VirtualPtr, el valor cargado es una direccion VM (NO
            // host_ptr).
            if (e->result_type.kind == PrimitiveKind::PTR
             || e->result_type.kind == PrimitiveKind::ARRAY) {
                if (e->result_type.is_virtual) {
                    fn_->values[dst].is_host_ptr = false;
                } else if (fn_->values[p].is_host_ptr) {
                    // El resultado del deref de un host_ptr es OTRO
                    // host_ptr.  Asi p4=host_ptr -> *p4 = i64*** que
                    // apunta a celda host -> tambien host_ptr.
                    fn_->values[dst].is_host_ptr = true;
                }
            }
            return dst;
        }

        const ir::IrValueId v = lower_expr(e->operand.get());
        if (v == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;

        const ir::IrType vt       = fn_->values[v].type;
        const bool       is_float = is_floating(e->operand->result_type.kind);
        ir::IrValueId    dst      = fn_->new_value(vt);
        ir::IrInstr      ins{};
        ins.dst         = dst;
        ins.type        = vt;
        ins.source_line = e->loc.line;
        ins.operands    = {v};

        switch (e->op) {
            case ast::UnOp::Neg:
                ins.op = is_float ? ir::IrOp::FNEG : ir::IrOp::NEG;
                break;
            case ast::UnOp::Pos:
                // Unario + es identidad; emitir un MOV es la opcion mas barata.
                ins.op = ir::IrOp::MOV;
                break;
            case ast::UnOp::LogicalNot:
                // !x  <=>  cmp.eq x, 0
            {
                const ir::IrValueId zero = emit_const(vt, 0, e->loc.line);
                ins.op                   = is_float ? ir::IrOp::FCMP_EQ : ir::IrOp::CMP_EQ;
                ins.type                 = ir::IrType::BOOL;
                ins.operands             = {v, zero};
                fn_->values[dst].type    = ir::IrType::BOOL;
            }
            break;
            case ast::UnOp::BitNot:
                ins.op = ir::IrOp::NOT;
                break;
            case ast::UnOp::Unwrap: {
                // !!x  <=>  unwrap(x): assert non-null + return value.
                // Lowering directo a la instruccion bytecode @c unwrap
                // (0x26) via RAW_ASM con tokens {dst}/{src0}; mismo
                // patron que el builtin unwrap() en try_lower_builtin_call.
                ir::IrInstr ra{};
                ra.op          = ir::IrOp::UNWRAP;
                ra.type        = vt;
                ra.dst         = dst;
                ra.operands    = {v};
                ra.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ra));
                if (fn_->values[v].is_host_ptr) {
                    fn_->values[dst].is_host_ptr = true;
                }
                return dst;
            }
            case ast::UnOp::Await: {
                // `await fut` bloquea hasta que el future este resuelto.
                // El bytecode `await r_fut` (0x2A) suspende el proceso si el
                // future esta PENDING (state -> WAIT_IO, blocking=true).  Al
                // ser resuelto via fulfill desde otro proceso, el waiter se
                // re-planifica y await re-ejecuta, devolviendo r0 = result.
                // Capturamos r0 a {dst} como i64 (el bytecode siempre devuelve
                // i64 raw; el frontend hace cast/bitcast al tipo logico T).
                const ir::IrValueId v_raw = fn_->new_value(ir::IrType::I64); {
                    ir::IrInstr aw{};
                    aw.op           = ir::IrOp::AWAIT;
                    aw.type         = ir::IrType::I64;
                    aw.dst          = v_raw;
                    aw.operands     = {v};
                    aw.set_is_call_site(true); // bloquea -> save/restore live regs
                    aw.source_line  = e->loc.line;
                    fn_->append(current_block_, std::move(aw));
                }
                // Mejora II: si el operando del await es Future<T>, el frontend
                // sabe el tipo T y puede convertir el i64 raw al tipo logico
                // adecuado.  Para tipos < 8 bytes hace TRUNC; para floats hace
                // BITCAST (no FTOI que cambia el valor).  Si el operando NO
                // es Future<T> (legacy: i64/i32/u64/u32 directos), devolvemos
                // el v_raw sin cast.
                const Type op_type = e->operand ? e->operand->result_type : Type{};
                if (op_type.kind == PrimitiveKind::FUTURE && op_type.pointee) {
                    const PrimitiveKind tk = op_type.pointee->kind;
                    if (tk == PrimitiveKind::F64) {
                        ir::IrValueId v_dst = fn_->new_value(ir::IrType::F64);
                        ir::IrInstr bc{};
                        bc.op = ir::IrOp::BITCAST;
                        bc.type = ir::IrType::F64;
                        bc.dst = v_dst;
                        bc.operands = {v_raw};
                        bc.source_line = e->loc.line;
                        fn_->append(current_block_, std::move(bc));
                        return v_dst;
                    }
                    if (tk == PrimitiveKind::F32) {
                        // i64 -> trunc i32 -> bitcast f32.
                        ir::IrValueId v_i32 = fn_->new_value(ir::IrType::I32); {
                            ir::IrInstr tr{};
                            tr.op = ir::IrOp::TRUNC;
                            tr.type = ir::IrType::I32;
                            tr.dst = v_i32;
                            tr.operands = {v_raw};
                            tr.source_line = e->loc.line;
                            fn_->append(current_block_, std::move(tr));
                        }
                        ir::IrValueId v_dst = fn_->new_value(ir::IrType::F32);
                        ir::IrInstr bc{};
                        bc.op = ir::IrOp::BITCAST;
                        bc.type = ir::IrType::F32;
                        bc.dst = v_dst;
                        bc.operands = {v_i32};
                        bc.source_line = e->loc.line;
                        fn_->append(current_block_, std::move(bc));
                        return v_dst;
                    }
                    // Tipos enteros mas estrechos (i8..i32, u8..u32, bool, char):
                    // cast_if_needed selecciona TRUNC con la mascara correcta.
                    const ir::IrType pt_ir = ir_type_from_primitive(tk);
                    if (pt_ir != ir::IrType::I64) {
                        return cast_if_needed(v_raw, ir::IrType::I64, pt_ir,
                                              e->loc.line);
                    }
                }
                return v_raw;
            }
            default:
                // PreInc/PostInc/PreDec/PostDec ya filtrados arriba.
                unsupported(e->loc, "operador unario no soportado");
                return ir::IR_NO_VALUE;
        }

        fn_->append(current_block_, std::move(ins));
        return dst;
    }

    ir::IrValueId Lowering::lower_call(ast::CallExpr *e) {
        /* A.43.10: macros Lisp con splice/emit.  Si el type checker
         * sustituyo la llamada por un AST expandido (campo macro_expanded
         * no-null), bajamos directamente el AST sustituido en lugar de
         * emitir una llamada al builtin.  Esto convierte el call site
         * en codigo runtime real generado a partir de string compile-time. */
        if (e->macro_expanded) {
            return lower_expr(e->macro_expanded.get());
        }

        // Phase M.7: llamada a funcion de namespace importado, ej.
        // `lib_a.valor_a(args)`.  El TypeChecker marca el FieldAccessExpr
        // callee con property_kind=4 y resuelve la firma del simbolo en
        // imported_namespaces_.  Aqui obtenemos el mangled_label y
        // emitimos CALL como si fuera una llamada normal.
        if (e->callee
         && e->callee->kind == ast::NodeKind::FieldAccessExpr) {
            auto *fa = static_cast<ast::FieldAccessExpr *>(e->callee.get());
            if (fa->property_kind == 4
             && fa->base
             && fa->base->kind == ast::NodeKind::IdentExpr) {
                auto *idb = static_cast<ast::IdentExpr *>(fa->base.get());
                // Localizar el namespace EXACTO via ns_index que el
                // TypeChecker dejo en el FieldAccessExpr.  Sentinel
                // UINT32_MAX significa no resuelto (defensivo).
                std::string mangled_label;
                ir::IrType ret_ir = ir::IrType::I64;
                bool found = false;
                // SRET cross-module: capturar el PrimitiveKind real del
                // retorno del callee para detectar Optional/Result que
                // requieren retbuf hidden como primer arg.
                PrimitiveKind callee_kind_ns = PrimitiveKind::VOID;
                // Capturar tambien los tipos de parametros del callee para
                // auto-promotion literal -> StringObject al lowering args.
                std::vector<Type> ns_param_types;
                if (fa->ns_index != 0xFFFFFFFFu) {
                    const auto &nss = tc_.imported_namespaces();
                    if (fa->ns_index < nss.size()) {
                        const auto &ns = nss[fa->ns_index];
                        auto its = ns.by_name.find(fa->field_name);
                        if (its != ns.by_name.end()) {
                            const auto &sym = ns.symbols[its->second];
                            mangled_label = sym.mangled_label;
                            // Para namespaces inline la sig esta vacia
                            // (se rellena durante run()); buscamos la
                            // real via function_sig_by_name.
                            const FunctionSig *real_sig =
                                tc_.function_sig_by_name(mangled_label);
                            if (real_sig) {
                                ret_ir = ir_type_from_primitive(
                                    real_sig->return_type.kind);
                                callee_kind_ns = real_sig->return_type.kind;
                                ns_param_types = real_sig->param_types;
                            } else {
                                ret_ir = ir_type_from_primitive(
                                    sym.sig.return_type.kind);
                                callee_kind_ns = sym.sig.return_type.kind;
                                ns_param_types = sym.sig.param_types;
                            }
                            found = true;
                        }
                    }
                }
                if (!found) {
                    // L2.1: static method de clase cross-class (e.g. `Stats.inc()`).
                    // El TypeChecker marca @c property_kind=4 pero deja
                    // @c ns_index=UINT32_MAX porque la "clase" no se registra como
                    // namespace.  Aqui resolvemos el metodo via class_layouts del
                    // tc y emitimos CALL al mangled label @c ClassName__method.
                    auto it_cls = tc_.class_layouts().find(idb->name);
                    if (it_cls != tc_.class_layouts().end()) {
                        for (const auto &m : it_cls->second.methods) {
                            if (m.is_static && !m.is_constructor
                             && m.name == fa->field_name) {
                                mangled_label = idb->name + "__" + m.name;
                                ret_ir = ir_type_from_primitive(m.return_type.kind);
                                found = true;
                                break;
                            }
                        }
                    }
                }
                if (!found) {
                    diags_.error(e->loc,
                        "namespace '" + idb->name + "' no resuelto en lowering");
                    return ir::IR_NO_VALUE;
                }
                // SRET cross-module: si el callee declara devolver
                // Optional<T>, Result<V,E>, un enum declarado por usuario
                // (encoded como STRUCT con struct_name = enum_name) o un
                // struct value-type, su firma IR real es void y espera un
                // retbuf hidden como primer argumento.  Sin este marshalling,
                // el callee escribe a R1 (garbage) y el caller lee basura.
                // Mismo patron que la rama IdentExpr-callee de lower_call.
                std::string callee_struct_name;
                if (fa->ns_index != 0xFFFFFFFFu) {
                    const auto &nss = tc_.imported_namespaces();
                    if (fa->ns_index < nss.size()) {
                        const auto &ns = nss[fa->ns_index];
                        auto its = ns.by_name.find(fa->field_name);
                        if (its != ns.by_name.end()) {
                            callee_struct_name =
                                ns.symbols[its->second].sig.return_type.struct_name;
                        }
                    }
                }
                // Detectar enum (user enum) o struct via lookup en layouts.
                bool callee_is_enum_ret_ns   = false;
                bool callee_is_struct_ret_ns = false;
                uint64_t enum_struct_size_ns = 0;
                if (callee_kind_ns == PrimitiveKind::STRUCT
                 && !callee_struct_name.empty()) {
                    const auto &elays = tc_.enum_layouts();
                    auto it_e = elays.find(callee_struct_name);
                    if (it_e != elays.end()) {
                        callee_is_enum_ret_ns = true;
                        enum_struct_size_ns =
                            static_cast<uint64_t>(it_e->second.size_bytes);
                    } else {
                        const auto &slays = tc_.struct_layouts();
                        auto it_s = slays.find(callee_struct_name);
                        if (it_s != slays.end()) {
                            callee_is_struct_ret_ns = true;
                            enum_struct_size_ns =
                                static_cast<uint64_t>(it_s->second.size_bytes);
                        }
                    }
                }
                const bool callee_is_sret_ns =
                    (callee_kind_ns == PrimitiveKind::OPTIONAL
                  || callee_kind_ns == PrimitiveKind::RESULT
                  || callee_is_enum_ret_ns
                  || callee_is_struct_ret_ns);
                ir::IrValueId v_call_retbuf_ns = ir::IR_NO_VALUE;
                if (callee_is_sret_ns) {
                    uint64_t buf_bytes;
                    if (callee_kind_ns == PrimitiveKind::RESULT) {
                        buf_bytes = 24ULL;
                    } else if (callee_kind_ns == PrimitiveKind::OPTIONAL) {
                        buf_bytes = 16ULL;
                    } else {
                        buf_bytes = enum_struct_size_ns;
                    }
                    v_call_retbuf_ns = fn_->new_value(ir::IrType::PTR);
                    ir::IrInstr al{};
                    al.op          = ir::IrOp::ALLOCA;
                    al.type        = ir::IrType::I8;
                    al.dst         = v_call_retbuf_ns;
                    al.imm         = buf_bytes;
                    al.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(al));
                }
                // Lower args.  Si es sret, el retbuf va PRIMERO.
                std::vector<ir::IrValueId> arg_vals;
                arg_vals.reserve(e->args.size() + (callee_is_sret_ns ? 1 : 0));
                if (callee_is_sret_ns) arg_vals.push_back(v_call_retbuf_ns);
                // Auto-promotion literal -> StringObject cuando el
                // parametro espera STRING.  Mismo patron que el local
                // IdentExpr-callee path (lower_call lineas ~10125+).
                // Sin esto, `lib.fn("hola")` pushea el ptr crudo del
                // literal en lugar del GcHandle, y el callee crashea
                // al hacer strraw sobre puntero invalido.
                for (size_t ai = 0; ai < e->args.size(); ++ai) {
                    auto &a = e->args[ai];
                    bool promote = false;
                    if (ai < ns_param_types.size()
                     && ns_param_types[ai].kind == PrimitiveKind::STRING
                     && a && a->kind == ast::NodeKind::StringLitExpr) {
                        auto *slit = static_cast<ast::StringLitExpr *>(a.get());
                        arg_vals.push_back(lower_string_literal_to_string_object(slit));
                        promote = true;
                    }
                    if (!promote) {
                        ir::IrValueId v = lower_expr(a.get());
                        arg_vals.push_back(v);
                    }
                }
                // Para sret la firma IR es VOID; el "valor" SSA del CALL es
                // el retbuf que se bindea al var-decl o se pasa a otras fns.
                ir::IrValueId dst = ir::IR_NO_VALUE;
                if (!callee_is_sret_ns) {
                    dst = (ret_ir == ir::IrType::VOID)
                              ? ir::IR_NO_VALUE
                              : fn_->new_value(ret_ir);
                }
                ir::IrInstr ins{};
                ins.op          = ir::IrOp::CALL;
                ins.type        = callee_is_sret_ns ? ir::IrType::VOID : ret_ir;
                ins.dst         = dst;
                ins.operands    = std::move(arg_vals);
                ins.func_name   = mangled_label;
                ins.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ins));
                return callee_is_sret_ns ? v_call_retbuf_ns : dst;
            }
        }
        // A.39: si el callee es una `comptime fn` y todos los args son
        // comptime-evaluables, ejecutamos la fn en compile-time y
        // emitimos el resultado como CONST.  Caso comun: usar comptime
        // fn dentro de un `comptime for` body donde los args son
        // constantes por iteracion.
        if (e->callee
         && e->callee->kind == ast::NodeKind::IdentExpr) {
            auto *cid = static_cast<ast::IdentExpr *>(e->callee.get());
            const auto &cfns = tc_.comptime_fns();
            auto cit = cfns.find(cid->name);
            if (cit != cfns.end()) {
                /* Phase MC.17.3: si estamos dentro de un @Macro body
                 * lowereado a IR Y el callee es OTRO @Macro, NO
                 * intentamos comptime-eval; en su lugar caemos al
                 * lowering normal mas abajo que emitira CALLVM regular
                 * a `__macro_<callee>`.  Los args pueden ser params del
                 * macro contenedor (runtime values) lo cual es valido. */
                if (current_fn_is_macro_
                 && cit->second && cit->second->is_macro) {
                    /* Caer al lowering normal de CallExpr -- no
                     * intentar comptime eval aqui.  El rewrite del
                     * nombre callee_name -> __macro_<name> se hace al
                     * emitir el IrInstr::CALL al final de lower_call. */
                    goto skip_comptime_eval_for_macro_to_macro;
                }
                ComptimeEvalResult r = comptime_eval_expr(tc_, e);
                if (!r.ok) {
                    error_at(e->loc,
                        "llamada a comptime fn '" + cid->name +
                        "' no es comptime-evaluable (argumento runtime?)");
                    return ir::IR_NO_VALUE;
                }
                const uint32_t src_line = e->loc.line;
                /* A.43.16: para @Macro fns, el type checker ya parseó +
                 * type-checó la expresion generada y la guardó en
                 * `e->macro_expanded`.  La rama temprana al inicio de
                 * lower_call (A.43.10) ya hizo lower_expr del AST
                 * sustituido y retornó antes de llegar aqui.  Asi que
                 * en este punto NO esperamos un @Macro -- todos los
                 * callees con string return son los comptime fns
                 * regulares que materializan StringObject. */
                if (r.is_str) {
                    /* Construir StringObject inline. */
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
                    return v_str;
                }
                /* Tipo de retorno declarado por la fn. */
                ir::IrType t = ir::IrType::I64;
                auto *fn_decl = cfns.at(cid->name);
                if (fn_decl && fn_decl->return_type) {
                    Type rt = tc_.resolve_type_node(fn_decl->return_type.get());
                    t = ir_type_from_primitive(rt.kind);
                }
                return emit_const(t, (uint64_t)r.value, src_line);
            }
        }
    skip_comptime_eval_for_macro_to_macro:

        // constructor de variante de enum: el type checker lo
        // marco con FieldAccessExpr::property_kind = 99.  Se trata como
        // un CallExpr cuyo callee es FieldAccessExpr(IdentExpr(enum_name),
        // variant_name).  Lowering: alocar slot del enum, escribir tag +
        // payloads, devolver puntero al slot.
        if (e->callee
            && e->callee->kind == ast::NodeKind::FieldAccessExpr) {
            auto *fa = static_cast<ast::FieldAccessExpr *>(e->callee.get());
            if (fa->property_kind == 99
                && fa->base
                && fa->base->kind == ast::NodeKind::IdentExpr) {
                auto *base_id = static_cast<ast::IdentExpr *>(fa->base.get());
                return lower_enum_constructor(base_id->name, fa->field_name,
                                              e->args, e->loc);
            }
            // M.L7 ext: enum constructor cross-module
            // (`command.Command.InsertChar(65)`).  El base es FieldAccess
            // que el type checker ya resolvio a Type{STRUCT, mangled_enum}.
            // Usamos el struct_name del result_type como enum_name.
            if (fa->property_kind == 99
                && fa->base
                && fa->base->kind == ast::NodeKind::FieldAccessExpr) {
                const auto &bt = fa->base->result_type;
                if (bt.kind == PrimitiveKind::STRUCT
                 && !bt.struct_name.empty()) {
                    return lower_enum_constructor(bt.struct_name,
                                                   fa->field_name,
                                                   e->args, e->loc);
                }
            }
        }
        // Metodos OO sobre tipo string.  Mapping:
        //   s.length() -> str_length(s)
        //   s.bytes()  -> str_bytes(s)
        //   s.cstr()   -> str_cstr(s)
        //   s.wstr()   -> str_wstr(s)
        //   s.hash()   -> str_hash(s)
        //   s.intern() -> str_intern(s)
        //   s.equals(t)-> str_equals(s, t)
        //   s.concat(t)-> str_concat(s, t)
        // Cero overhead: se reescribe el call al builtin equivalente con
        // self como primer arg.
        if (e->callee
            && e->callee->kind == ast::NodeKind::FieldAccessExpr) {
            auto *fa = static_cast<ast::FieldAccessExpr *>(e->callee.get());
            if (fa->base && fa->base->result_type.kind == PrimitiveKind::STRING) {
                static const char *METHOD_TO_BUILTIN[][2] = {
                    {"length", "str_length"},
                    {"bytes", "str_bytes"},
                    {"cstr", "str_cstr"},
                    {"wstr", "str_wstr"},
                    {"hash", "str_hash"},
                    {"intern", "str_intern"},
                    {"equals", "str_equals"},
                    {"concat", "str_concat"},
                };
                for (const auto &m: METHOD_TO_BUILTIN) {
                    if (fa->field_name == m[0]) {
                        // Construir un IdentExpr del builtin + reescribir
                        // args: [base, ...e->args].
                        ast::CallExpr synth;
                        synth.loc    = e->loc;
                        auto id      = std::make_unique<ast::IdentExpr>();
                        id->loc      = e->loc;
                        id->name     = m[1];
                        synth.callee = std::move(id);
                        // Ojo: NO movemos los originales (los devolvemos
                        // intactos).  Para evitar deep-clone, hacemos un
                        // approach sucio: temporalmente robamos los args
                        // del CallExpr original, llamamos try_lower_builtin,
                        // y restauramos.
                        std::vector<std::unique_ptr<ast::Expr> > saved_args;
                        saved_args.reserve(e->args.size() + 1);
                        saved_args.push_back(std::move(fa->base));
                        for (auto &a: e->args) saved_args.push_back(std::move(a));
                        synth.args = std::move(saved_args);
                        ir::IrValueId out;
                        bool          ok = try_lower_builtin_call(&synth, out);
                        // Restaurar: mover args de vuelta a originales.
                        fa->base = std::move(synth.args[0]);
                        for (size_t i = 0; i < e->args.size(); ++i) {
                            e->args[i] = std::move(synth.args[i + 1]);
                        }
                        if (ok) return out;
                    }
                }
            }
            // Reflexion OO: dispatch ergonomico cuando el type checker
            // marco el FieldAccessExpr con property_kind 100..106.
            // Reescribe el call al builtin standalone equivalente.
            //   100: forName(name)               estatico, no toma self
            //   101: getMethod(cls, name)
            //   102: getField(cls, name)
            //   103: newInstance(cls)
            //   104: getMethods(cls)            (placeholder; no impl runtime aun)
            //   105: invoke(method, this, args...)
            //   106: getClass(obj)
            if (fa->property_kind >= 100 && fa->property_kind <= 106) {
                static const char *KIND_TO_BUILTIN[] = {
                    "forName",     // 100
                    "getMethod",   // 101
                    "getField",    // 102
                    "newInstance", // 103
                    "getMethods",  // 104
                    "invoke",      // 105
                    "getClass",    // 106
                };
                const char *bn = KIND_TO_BUILTIN[fa->property_kind - 100];
                ast::CallExpr synth;
                synth.loc       = e->loc;
                auto id         = std::make_unique<ast::IdentExpr>();
                id->loc         = e->loc;
                id->name        = bn;
                synth.callee    = std::move(id);
                // Para los metodos de instancia (101..103, 105, 106) prepend
                // el base (self) como primer argumento.  Para forName (100)
                // solo los args originales.  El base original sera devuelto
                // tras la lower.
                std::vector<std::unique_ptr<ast::Expr>> saved_args;
                const bool prepend_self = (fa->property_kind != 100);
                saved_args.reserve(e->args.size() + (prepend_self ? 1 : 0));
                if (prepend_self) {
                    saved_args.push_back(std::move(fa->base));
                }
                for (auto &a : e->args) saved_args.push_back(std::move(a));
                synth.args = std::move(saved_args);
                ir::IrValueId out;
                const bool ok = try_lower_builtin_call(&synth, out);
                // Restaurar args originales para no afectar el AST.
                size_t k = 0;
                if (prepend_self) {
                    fa->base = std::move(synth.args[k++]);
                }
                for (size_t i = 0; i < e->args.size(); ++i, ++k) {
                    e->args[i] = std::move(synth.args[k]);
                }
                if (ok) return out;
                // try_lower_builtin_call devolvio false (e.g. argumento
                // ausente o mal formado); el error ya se reporto.  Devolvemos
                // un valor invalido para que el caller no use un IrValueId
                // basura.
                return ir::IR_NO_VALUE;
            }
            // Bug fix 2026-05-23: metodos estaticos.  property_kind=4 marca
            // llamada estatica `ClassName.method()` que NO tiene receptor
            // CLASS; el dispatch va a lower_class_method_call que detecta
            // property_kind=4 y emite CALLVM directo.
            if (fa->property_kind == 4) {
                return lower_class_method_call(e);
            }
            if (fa->base && fa->base->result_type.kind == PrimitiveKind::CLASS) {
                return lower_class_method_call(e);
            }
            // ===== dispatch de metodos de coleccion primitiva =====
            // Si la base es uno de los tipos coleccion (ARRAYLIST, HASHMAP,
            // ...), buscamos el metodo en la tabla COL_METHODS y emitimos
            // CALLN directo al native_fn con (handle, ...args).  Cero
            // overhead vs llamar el plugin manualmente; sin vtable ni
            // CALLVIRT (no son objetos GC, son handles host pointer).
            if (fa->base && is_col_kind(fa->base->result_type.kind)) {
                const ColMethod *cm = find_col_method(fa->base->result_type.kind,
                                                      fa->field_name);
                if (cm) {
                    // decidir si la coleccion retiene refs GC.
                    // El frontend setea pointee/pointee2 al resolver el tipo
                    // declarado (`ArrayList<string>` etc.).  Si es GC y la
                    // operacion tiene variante *_gc, llamamos a esa con un
                    // `getproc` extra como primer argumento.  Si la coleccion
                    // se declaro sin <T> (legacy o tipo opaco i64), pointee
                    // es nulo y caemos al camino no-GC de cero overhead.
                    const Type &  recv_ty = fa->base->result_type;
                    PrimitiveKind elem_k  = PrimitiveKind::VOID;
                    PrimitiveKind val_k   = PrimitiveKind::VOID;
                    if (recv_ty.pointee) elem_k = recv_ty.pointee->kind;
                    if (recv_ty.pointee2) val_k = recv_ty.pointee2->kind;
                    const bool gc_aware = (cm->native_fn_gc != nullptr)
                            && col_needs_gc_aware(recv_ty.kind, elem_k, val_k);

                    // Lower base (handle).
                    const ir::IrValueId v_handle = lower_expr(fa->base.get());
                    if (v_handle == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
                    // Lower args.
                    std::vector<ir::IrValueId> arg_ids;
                    arg_ids.reserve(2 + e->args.size());
                    if (gc_aware) {
                        // proc va PRIMERO en las variantes *_gc.
                        arg_ids.push_back(emit_getproc(e->loc.line));
                    }
                    arg_ids.push_back(v_handle);
                    for (auto &a: e->args) {
                        arg_ids.push_back(lower_expr(a.get()));
                    }
                    const char *fn_name = gc_aware ? cm->native_fn_gc : cm->native_fn;
                    out_mod_->register_native_import(COL_NATIVE_LIB, fn_name);
                    const ir::IrType    ret_ir = ir_type_from_primitive(cm->ret);
                    const ir::IrValueId v_dst  = (ret_ir == ir::IrType::VOID)
                                                     ? ir::IR_NO_VALUE
                                                     : fn_->new_value(ret_ir);
                    ir::IrInstr ins{};
                    ins.op          = ir::IrOp::CALLN;
                    ins.type        = ret_ir;
                    ins.dst         = v_dst;
                    ins.func_name   = std::string(COL_NATIVE_LIB) + ":" + fn_name;
                    ins.operands    = std::move(arg_ids);
                    ins.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(ins));
                    return v_dst;
                }
            }
        }
        // Resto: llamada directa a funcion top-level.
        if (!e->callee || e->callee->kind != ast::NodeKind::IdentExpr) {
            error_at(e->loc, "lowering: callee no es identificador");
            return ir::IR_NO_VALUE;
        }
        auto *id = static_cast<ast::IdentExpr *>(e->callee.get());

        // FFI declarativo: si el callee es una funcion extern
        // (registrada en extern_lib_by_fn_name_ via ExternFnDecl), emitir
        // directamente CALLN @Method("<lib>:<name>") con args en R1..RN.
        // Cero overhead vs llamadas a plugins propios: usa exactamente la
        // misma maquinaria del ensamblador (LoadLibraryA + GetProcAddress).
        {
            auto it_ext = extern_lib_by_fn_name_.find(id->name);
            if (it_ext != extern_lib_by_fn_name_.end()) {
                const std::string &lib = it_ext->second;
                out_mod_->register_native_import(lib, id->name);
                std::vector<ir::IrValueId> arg_ids;
                arg_ids.reserve(e->args.size());
                for (auto &a: e->args) {
                    arg_ids.push_back(lower_expr(a.get()));
                }
                ir::IrType ret_ir = ir::IrType::VOID;
                auto       it_rt  = fn_return_types_.find(id->name);
                if (it_rt != fn_return_types_.end()) ret_ir = it_rt->second;
                const ir::IrValueId dst = (ret_ir == ir::IrType::VOID)
                                              ? ir::IR_NO_VALUE
                                              : fn_->new_value(ret_ir);
                ir::IrInstr ins{};
                ins.op          = ir::IrOp::CALLN;
                ins.type        = ret_ir;
                ins.dst         = dst;
                ins.func_name   = lib + ":" + id->name;
                ins.operands    = std::move(arg_ids);
                ins.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ins));
                return dst;
            }
        }

        // Antes de tomar el camino generico, intentamos identificarla como
        // un builtin (println / print) que se traduce a una llamada FFI
        // a vesta_io.  El builtin emite por su cuenta el codigo necesario
        // (registro de bytes en static_data, getproc, calln vio_println).
        ir::IrValueId builtin_ret = ir::IR_NO_VALUE;
        if (try_lower_builtin_call(e, builtin_ret)) {
            return builtin_ret;
        }

        // -----------------------------------------------------------------
        // closures: si el identificador es una variable LOCAL cuyo
        // tipo es FUNCTION (function pointer / closure), tratamos esto
        // como llamada indirecta.  El type checker ya marco
        // @c id->result_type como Type{FUNCTION, params, ret} en este caso
        // y la variable esta bindeada en el scope al SSA value que
        // devolvio @c lower_lambda_expr (puntero al function value de
        // 16 bytes).  Aqui:
        //   1. Cargar fn_addr de [fv_addr + 0]
        //   2. Cargar env_addr de [fv_addr + 8]
        //   3. Bajar args
        //   4. Emitir CALLCLOSURE(fn_addr, env_addr, args...)
        // El emisor IR (caso CALLCLOSURE) coloca env en R14, args en
        // R1..R12 y emite @c callvmr fn_addr.
        if (id->result_type.kind == PrimitiveKind::FUNCTION) {
            // Direccion del function value (16 bytes en stack).  Si es
            // una variable address-taken, read_local devuelve el LOAD;
            // si es directa, devuelve el SSA value tal cual.  Para
            // function values el bind ya guarda la direccion del slot.
            ir::IrValueId fv_addr = lookup(id->name);
            if (fv_addr == ir::IR_NO_VALUE) {
                error_at(e->loc, "lowering: closure no resuelto: '" + id->name + "'");
                return ir::IR_NO_VALUE;
            }

            // LOAD fn_addr de [fv_addr + 0].
            ir::IrValueId fn_addr = fn_->new_value(ir::IrType::I64); {
                ir::IrInstr ld{};
                ld.op          = ir::IrOp::LOAD;
                ld.type        = ir::IrType::I64;
                ld.dst         = fn_addr;
                ld.operands    = {fv_addr};
                ld.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ld));
            }

            // LOAD env_addr de [fv_addr + 8].
            ir::IrValueId env_addr; {
                ir::IrValueId fv_plus_8 = fn_->new_value(ir::IrType::PTR);
                ir::IrValueId off8      = emit_const(ir::IrType::I64, 8, e->loc.line);
                ir::IrInstr   ad{};
                ad.op          = ir::IrOp::ADD;
                ad.type        = ir::IrType::I64;
                ad.dst         = fv_plus_8;
                ad.operands    = {fv_addr, off8};
                ad.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ad));

                env_addr = fn_->new_value(ir::IrType::I64);
                ir::IrInstr ld{};
                ld.op          = ir::IrOp::LOAD;
                ld.type        = ir::IrType::I64;
                ld.dst         = env_addr;
                ld.operands    = {fv_plus_8};
                ld.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ld));
            }

            // Bajar args.  El primer operando de CALLCLOSURE es env_addr
            // (convencion del opcode); luego van los args declarados.
            std::vector<ir::IrValueId> arg_ids;
            arg_ids.reserve(1 + e->args.size());
            arg_ids.push_back(env_addr);
            for (auto &a: e->args) {
                arg_ids.push_back(lower_expr(a.get()));
            }

            // Tipo de retorno deducido del FUNCTION type del callee.
            ir::IrType ret_ir = ir::IrType::VOID;
            if (id->result_type.pointee
                && id->result_type.pointee->kind != PrimitiveKind::VOID) {
                ret_ir = ir_type_from_primitive(id->result_type.pointee->kind);
            }
            ir::IrValueId dst = (ret_ir == ir::IrType::VOID)
                                    ? ir::IR_NO_VALUE
                                    : fn_->new_value(ret_ir);

            ir::IrInstr ins{};
            ins.op          = ir::IrOp::CALLCLOSURE;
            ins.type        = ret_ir;
            ins.dst         = dst;
            ins.func_ptr    = fn_addr;
            ins.operands    = std::move(arg_ids);
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            return dst;
        }

        // Resolver tipo de retorno.
        ir::IrType ret_ir = ir::IrType::I64;
        auto       it     = fn_return_types_.find(id->name);
        if (it != fn_return_types_.end()) ret_ir = it->second;

        // sret: si el callee declara devolver Optional<T>, Result<V,E> o
        // un enum declarado por usuario, su firma IR real es void y
        // espera un retbuf hidden como primer argumento.  Aqui en el
        // caller alocamos el buffer (16, 24 o size_bytes del enum) en
        // stack y lo pasamos.  El "valor" SSA del CALL es la direccion
        // del retbuf, que el caller bindea a la variable del var-decl o
        // pasa como argumento a otras funciones.
        PrimitiveKind callee_kind = PrimitiveKind::VOID;
        auto          it_kind     = fn_ret_kind_.find(id->name);
        if (it_kind != fn_ret_kind_.end()) callee_kind = it_kind->second;
        // ADTs: detectar enum SRET via fn_ret_enum_name_.
        auto       it_enum_ret         = fn_ret_enum_name_.find(id->name);
        const bool callee_is_enum_sret = (it_enum_ret != fn_ret_enum_name_.end());
        // (gap O): detectar funcion que retorna FUNCTION via
        // fn_returns_function_; el slot tiene siempre 16 bytes.
        const bool callee_is_function_sret =
                (fn_returns_function_.find(id->name) != fn_returns_function_.end());
        // Smart pointers: detectar funcion que retorna unique<T>/shared<T>
        // via fn_returns_smartptr_; el slot tiene 8 bytes (host_ptr).
        const bool callee_is_smartptr_sret =
                (fn_returns_smartptr_.find(id->name) != fn_returns_smartptr_.end());
        const bool callee_is_sret = (callee_kind == PrimitiveKind::OPTIONAL
            || callee_kind == PrimitiveKind::RESULT
            || callee_is_enum_sret
            || callee_is_function_sret
            || callee_is_smartptr_sret);
        ir::IrValueId v_call_retbuf = ir::IR_NO_VALUE;
        if (callee_is_sret) {
            uint64_t buf_bytes = 16ULL; // default Optional
            if (callee_is_enum_sret) {
                const auto &elays = tc_.enum_layouts();
                auto        it_e  = elays.find(it_enum_ret->second);
                if (it_e != elays.end()) {
                    buf_bytes = static_cast<uint64_t>(it_e->second.size_bytes);
                }
            } else if (callee_kind == PrimitiveKind::RESULT) {
                buf_bytes = 24ULL;
            } else if (callee_is_function_sret) {
                buf_bytes = 16ULL; // function value: fn_addr + env_addr
            } else if (callee_is_smartptr_sret) {
                // unique<T> Tier 1 = 16 bytes (ptr+deleter); shared<T> = 8 (ctrl_ptr).
                // No tenemos info del kind aqui sin parsear la firma; usamos 16
                // que cubre ambos (shared solo usa los primeros 8 bytes; la
                // segunda mitad del slot es padding).
                buf_bytes = 16ULL;
            }
            v_call_retbuf = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8; // unidad: 1 byte
            al.dst         = v_call_retbuf;
            al.imm         = buf_bytes;
            al.source_line = e->loc.line;
            // BugFix sret-cross-mem (2026-06-04): forzar host_alloca SOLO
            // para retbuf de Optional/Result/enum (donde el bug cross-mem
            // se manifiesta).  NO para FUNCTION/smart-ptr cuyo lowering
            // tiene su propio manejo de memoria (env_addr=heap GC, ctrl_ptr).
            const bool is_optres_retbuf =
                (callee_kind == PrimitiveKind::OPTIONAL
              || callee_kind == PrimitiveKind::RESULT
              || callee_is_enum_sret);
            if (is_optres_retbuf) {
                al.set_host_alloca(true);
            }
            fn_->append(current_block_, std::move(al));
            if (is_optres_retbuf) {
                fn_->values[v_call_retbuf].is_host_ptr = true;
            }
        }

        // Bajar argumentos.  Si es sret, el retbuf va PRIMERO (convencion
        // espejo al lower_function que lo recibe como primer parametro).
        //
        // Fix - auto-promocion literal -> StringObject cuando el
        // parametro espera STRING (mismo patron que operadores +/==/!=).
        //  Sin esto, pasar `helper("hola")` a
        // `void helper(string s)` empuja la direccion del literal en
        // memoria VM (PTR) en vez del GcHandle al StringObject, y el
        // callee crashea al hacer `strraw s` con un puntero invalido.
        const FunctionSig *        callee_sig = tc_.function_sig_by_name(id->name);
        std::vector<ir::IrValueId> arg_ids;
        arg_ids.reserve(e->args.size() + (callee_is_sret ? 1 : 0));
        if (callee_is_sret) arg_ids.push_back(v_call_retbuf);
        for (size_t i = 0; i < e->args.size(); ++i) {
            ast::Expr *ae = e->args[i].get();
            // Detectar (param STRING, arg StringLitExpr no interpolado) y
            // promover el literal a StringObject inline via STRMAKE.
            bool promote_to_string = false;
            if (callee_sig && i < callee_sig->param_types.size()
                && callee_sig->param_types[i].kind == PrimitiveKind::STRING
                && ae && ae->kind == ast::NodeKind::StringLitExpr) {
                auto *sl = static_cast<ast::StringLitExpr *>(ae);
                // Tanto literales puros como interpolados: el helper
                // construye el StringObject correcto.
                arg_ids.push_back(lower_string_literal_to_string_object(sl));
                promote_to_string = true;
            }
            if (!promote_to_string) {
                arg_ids.push_back(lower_expr(ae));
            }
        }

        // Para sret la "firma" de retorno es VOID; el dst SSA visible al
        // resto del lowering es el retbuf (PTR).  Para calls normales el
        // dst es el valor devuelto via RET.
        ir::IrValueId dst = ir::IR_NO_VALUE;
        if (!callee_is_sret) {
            dst = (ret_ir == ir::IrType::VOID)
                      ? ir::IR_NO_VALUE
                      : fn_->new_value(ret_ir);
        }
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::CALL;
        ins.type        = callee_is_sret ? ir::IrType::VOID : ret_ir;
        ins.dst         = dst;
        /*   : si el callee es una @Macro user-defined,
         * rewriting al nombre prefijado `__macro_<name>` que el lowering
         * uso al generar la IrFunction.  Esto permite que un @Macro
         * llame a otro via CALLVM normal sin pasar por AST eval. */
        std::string callee_name = id->name;
        {
            auto fn_it = tc_.comptime_fns().find(id->name);
            if (fn_it != tc_.comptime_fns().end()
             && fn_it->second && fn_it->second->is_macro) {
                callee_name = "__macro_" + id->name;
            }
        }
        /* Phase M.5: si la funcion fue importada cross-module con
         * mangling, el label emitido en el .vel es el mangled
         * (`lib__foo`) aunque el nombre visible del usuario es `foo`.
         * Consultamos function_sig_by_name para detectar el caso. */
        {
            const FunctionSig *fs = tc_.function_sig_by_name(id->name);
            if (fs && !fs->mangled_label.empty()) {
                callee_name = fs->mangled_label;
            }
        }
        ins.func_name   = std::move(callee_name);
        ins.operands    = std::move(arg_ids);
        ins.source_line = e->loc.line;
        fn_->append(current_block_, std::move(ins));
        return callee_is_sret ? v_call_retbuf : dst;
    }

    // ---------------------------------------------------------------------
    // Lowering de asignaciones.
    //
    // En el modelo SSA-construction de Braun, una asignacion `x = expr` no
    // produce ninguna instruccion explicita: simplemente actualiza el mapa
    // "nombre -> IrValueId actual" en el scope donde @c x esta definida.
    // El siguiente uso de @c x leera ese nuevo IrValueId via lookup().
    //
    // Las asignaciones compuestas (+=, -=, *=, ...) se traducen a un
    // binop IR seguido del mismo update; reusan emit_binop_ir() para no
    // duplicar la logica de seleccion de opcode aritmetico/bitwise.
    // ---------------------------------------------------------------------

    // Helper: emite un IrInstr binario y devuelve el IrValueId del resultado.
    // Se usa tanto en lower_binary() como en compound assignments.
    ir::IrValueId Lowering::emit_binop_ir(ast::BinOp       op,
                                          ir::IrValueId    lhs_val,
                                          ir::IrValueId    rhs_val,
                                          PrimitiveKind    common,
                                          const SourceLoc &loc) {
        const ir::IrType common_ir = ir_type_from_primitive(common);
        const bool       is_float  = is_floating(common);
        const bool       is_unsign = is_integral(common) && !is_signed_integral(common);

        ir::IrOp   ir_op  = ir::IrOp::ADD;
        ir::IrType res_ir = common_ir;
        switch (op) {
            case ast::BinOp::Add: ir_op = is_float ? ir::IrOp::FADD : ir::IrOp::ADD;
                break;
            case ast::BinOp::Sub: ir_op = is_float ? ir::IrOp::FSUB : ir::IrOp::SUB;
                break;
            case ast::BinOp::Mul: ir_op = is_float ? ir::IrOp::FMUL : ir::IrOp::MUL;
                break;
            case ast::BinOp::Div: ir_op = is_float ? ir::IrOp::FDIV : ir::IrOp::DIV;
                break;
            case ast::BinOp::Mod: ir_op = ir::IrOp::MOD;
                break;
            case ast::BinOp::BitAnd: ir_op = ir::IrOp::AND;
                break;
            case ast::BinOp::BitOr: ir_op = ir::IrOp::OR;
                break;
            case ast::BinOp::BitXor: ir_op = ir::IrOp::XOR;
                break;
            case ast::BinOp::Shl: ir_op = ir::IrOp::SHL;
                break;
            case ast::BinOp::Shr: ir_op = is_unsign ? ir::IrOp::SHR : ir::IrOp::SAR;
                break;
            // Las comparaciones / logicos no suelen aparecer en compound
            // assignment (no existen ==, &&, etc.), pero las dejamos por
            // completitud; result_ir se cambia a BOOL.
            case ast::BinOp::Eq: ir_op = is_float ? ir::IrOp::FCMP_EQ : ir::IrOp::CMP_EQ;
                res_ir = ir::IrType::BOOL;
                break;
            case ast::BinOp::Neq: ir_op = is_float ? ir::IrOp::FCMP_NE : ir::IrOp::CMP_NE;
                res_ir = ir::IrType::BOOL;
                break;
            case ast::BinOp::Lt:
                ir_op = is_float
                            ? ir::IrOp::FCMP_LT
                            : (is_unsign ? ir::IrOp::CMP_ULT : ir::IrOp::CMP_LT);
                res_ir = ir::IrType::BOOL;
                break;
            case ast::BinOp::Le:
                ir_op = is_float
                            ? ir::IrOp::FCMP_LE
                            : (is_unsign ? ir::IrOp::CMP_ULE : ir::IrOp::CMP_LE);
                res_ir = ir::IrType::BOOL;
                break;
            case ast::BinOp::Gt:
                ir_op = is_float
                            ? ir::IrOp::FCMP_GT
                            : (is_unsign ? ir::IrOp::CMP_UGT : ir::IrOp::CMP_GT);
                res_ir = ir::IrType::BOOL;
                break;
            case ast::BinOp::Ge:
                ir_op = is_float
                            ? ir::IrOp::FCMP_GE
                            : (is_unsign ? ir::IrOp::CMP_UGE : ir::IrOp::CMP_GE);
                res_ir = ir::IrType::BOOL;
                break;
            case ast::BinOp::LogicalAnd: ir_op = ir::IrOp::AND;
                res_ir = ir::IrType::BOOL;
                break;
            case ast::BinOp::LogicalOr: ir_op = ir::IrOp::OR;
                res_ir = ir::IrType::BOOL;
                break;
        }

        const ir::IrValueId dst = fn_->new_value(res_ir);
        ir::IrInstr         ins{};
        ins.op          = ir_op;
        ins.type        = res_ir;
        ins.dst         = dst;
        ins.operands    = {lhs_val, rhs_val};
        ins.source_line = loc.line;
        fn_->append(current_block_, std::move(ins));
        return dst;
    }

    // Mapea AssignOp compuesto al BinOp aritmetico/bitwise correspondiente.
    // Se usa por todas las rutas de lower_assign que necesitan implementar
    // x op= v (struct field, class field, p[i], *p y la ya existente para
    // identifier).  Devuelve BinOp::Add para Assign (no deberia llamarse
    // con ese caso; el caller filtra antes).
    static ast::BinOp compound_assign_op_to_binop(ast::AssignOp op) {
        switch (op) {
            case ast::AssignOp::AddAssign: return ast::BinOp::Add;
            case ast::AssignOp::SubAssign: return ast::BinOp::Sub;
            case ast::AssignOp::MulAssign: return ast::BinOp::Mul;
            case ast::AssignOp::DivAssign: return ast::BinOp::Div;
            case ast::AssignOp::ModAssign: return ast::BinOp::Mod;
            case ast::AssignOp::BitAndAssign: return ast::BinOp::BitAnd;
            case ast::AssignOp::BitOrAssign: return ast::BinOp::BitOr;
            case ast::AssignOp::BitXorAssign: return ast::BinOp::BitXor;
            case ast::AssignOp::ShlAssign: return ast::BinOp::Shl;
            case ast::AssignOp::ShrAssign: return ast::BinOp::Shr;
            case ast::AssignOp::Assign: return ast::BinOp::Add;
        }
        return ast::BinOp::Add;
    }

    /**
     * @brief A.39 - emite el cuerpo de un `comptime for` desenrollado.
     *
     * Evalua lo/hi en compile-time (ya validados por type checker) y
     * por cada valor del index push scope con i=valor, lower body, pop.
     * Cero loop runtime: N copias del body emitidas en secuencia, con
     * el index sustituido como CONST en cada copia via
     * @c lowering_comptime_scopes_ (consultado por @c lower_ident).
     */
    void Lowering::lower_comptime_for(ast::ComptimeForStmt *s) {
        if (!s || !s->lo_expr || !s->hi_expr || !s->body) return;
        const ComptimeEvalResult lo = comptime_eval_expr(tc_, s->lo_expr.get());
        const ComptimeEvalResult hi = comptime_eval_expr(tc_, s->hi_expr.get());
        if (!lo.ok || !hi.ok || lo.is_str || hi.is_str) {
            error_at(s->loc,
                "comptime for: rango no evaluable (lo/hi deben ser enteros comptime)");
            return;
        }
        /* Limite defensivo para evitar explosion de codigo. */
        const int64_t lo_v = lo.value;
        int64_t       hi_v = hi.value;
        if (s->inclusive) hi_v += 1;
        if (hi_v - lo_v > 4096) {
            error_at(s->loc,
                "comptime for: rango excede 4096 iteraciones; usar un "
                "loop runtime en su lugar");
            return;
        }
        /* A.39: el bind del index lo hacemos en DOS lugares:
         *   1. `lowering_comptime_scopes_` para que @c lower_ident lo
         *      inline como CONST en el codigo runtime emitido.
         *   2. `tc.comptime_const_locals_` para que @c comptime_eval_expr
         *      pueda resolverlo cuando aparezca como arg de un comptime fn
         *      o builtin comptime.  Sin esto, `fact(k)` desde el body
         *      del for fallaria con "no comptime-evaluable" porque k
         *      no estaria en tc's stack. */
        auto &mut_tc = const_cast<TypeChecker &>(tc_);
        for (int64_t i = lo_v; i < hi_v; ++i) {
            /* Push lowering scope. */
            std::unordered_map<std::string, ComptimeLocalEntry> scope;
            ComptimeLocalEntry ent;
            ent.value = i;
            ent.ir_t  = ir::IrType::I64;
            scope[s->var_name] = ent;
            lowering_comptime_scopes_.push_back(std::move(scope));
            /* Push tc scope. */
            mut_tc.push_comptime_scope();
            TypeChecker::ComptimeConst c;
            c.type  = Type{PrimitiveKind::I64};
            c.value = i;
            mut_tc.register_comptime_local(s->var_name, std::move(c));
            /* Lower body. */
            lower_stmt(s->body.get());
            /* Pop. */
            mut_tc.pop_comptime_scope();
            lowering_comptime_scopes_.pop_back();
        }
    }

    /**
     * @brief A.38 - lowering del operador ternario `cond ? then : else`.
     *
     * Estructura CFG identica a lower_if con PHI en el merge:
     *   current -> br_cond cond, then_bb, else_bb
     *   then_bb -> lower(then_expr) -> br merge_bb
     *   else_bb -> lower(else_expr) -> br merge_bb
     *   merge   -> %r = phi [then_val, then_end] [else_val, else_end]
     *
     * El tipo resultado se toma del then_expr (ya validado por el type
     * checker que tt y et son asignables entre si).  Si difieren, se
     * aplica @c cast_if_needed al else para igualar.  Si then es un side-
     * effecting expr y la cond es comptime-evaluable, una optimizacion
     * futura podria dead-branch-eliminate; por ahora siempre emite el if.
     */
    ir::IrValueId Lowering::lower_ternary(ast::TernaryExpr *e) {
        const uint32_t src_line = e->loc.line;
        if (!e->cond || !e->then_expr || !e->else_expr) {
            error_at(e->loc, "lowering: ternario incompleto");
            return ir::IR_NO_VALUE;
        }
        /* Bajar cond y crear los 3 bloques. */
        ir::IrValueId cond = lower_expr(e->cond.get());
        if (cond == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        const ir::IrBlockId then_bb  = fn_->new_block(
            "ter_then_" + std::to_string(ternary_counter_));
        const ir::IrBlockId else_bb  = fn_->new_block(
            "ter_else_" + std::to_string(ternary_counter_));
        const ir::IrBlockId merge_bb = fn_->new_block(
            "ter_merge_" + std::to_string(ternary_counter_));
        ++ternary_counter_;
        /* br_cond. */
        {
            ir::IrInstr br{};
            br.op           = ir::IrOp::BR_COND;
            br.operands.push_back(cond);
            br.target_block = then_bb;
            br.false_block  = else_bb;
            br.source_line  = src_line;
            fn_->append(current_block_, std::move(br));
            fn_->blocks[current_block_].succs.push_back(then_bb);
            fn_->blocks[current_block_].succs.push_back(else_bb);
            fn_->blocks[then_bb].preds.push_back(current_block_);
            fn_->blocks[else_bb].preds.push_back(current_block_);
        }
        /* Bajar then_expr en then_bb. */
        current_block_ = then_bb;
        ir::IrValueId then_val = lower_expr(e->then_expr.get());
        ir::IrBlockId then_end = current_block_;
        ir::IrType    then_t   = (then_val != ir::IR_NO_VALUE)
            ? fn_->values[then_val].type
            : ir::IrType::I64;
        {
            ir::IrInstr brm{};
            brm.op           = ir::IrOp::BR;
            brm.target_block = merge_bb;
            brm.source_line  = src_line;
            fn_->append(then_end, std::move(brm));
            fn_->blocks[then_end].succs.push_back(merge_bb);
            fn_->blocks[merge_bb].preds.push_back(then_end);
        }
        /* Bajar else_expr en else_bb. */
        current_block_ = else_bb;
        ir::IrValueId else_val = lower_expr(e->else_expr.get());
        ir::IrBlockId else_end = current_block_;
        /* Coerce else_val al tipo de then si difieren. */
        if (else_val != ir::IR_NO_VALUE) {
            const ir::IrType else_t = fn_->values[else_val].type;
            if (else_t != then_t) {
                else_val = cast_if_needed(else_val, else_t, then_t, src_line);
            }
        }
        {
            ir::IrInstr brm{};
            brm.op           = ir::IrOp::BR;
            brm.target_block = merge_bb;
            brm.source_line  = src_line;
            fn_->append(else_end, std::move(brm));
            fn_->blocks[else_end].succs.push_back(merge_bb);
            fn_->blocks[merge_bb].preds.push_back(else_end);
        }
        /* Merge: PHI. */
        current_block_ = merge_bb;
        if (then_val == ir::IR_NO_VALUE || else_val == ir::IR_NO_VALUE) {
            error_at(e->loc, "ternario: una de las ramas no produjo valor");
            return ir::IR_NO_VALUE;
        }
        ir::IrValueId result = fn_->new_value(then_t);
        ir::IrInstr   phi{};
        phi.op   = ir::IrOp::PHI;
        phi.type = then_t;
        phi.dst  = result;
        phi.phi_args.push_back({then_val, then_end});
        phi.phi_args.push_back({else_val, else_end});
        phi.source_line = src_line;
        fn_->append(merge_bb, std::move(phi));
        return result;
    }

    /**
     * @brief P2: lowering del operador `?` postfix para Result<V,E>.
     *
     * Desugar:
     * @code
     *   let v = expr?;
     * @endcode
     * a:
     * @code
     *   let __tmp = expr;       // SSA value = PTR al slot Result (24 bytes)
     *   if (tag(__tmp) == 0) {  // Err branch
     *     // copy __tmp (24 bytes) al sret_retbuf del caller
     *     // RET void
     *   }
     *   // Ok branch: extraer V de [__tmp + 8]
     * @endcode
     *
     * Layout del slot Result<V,E> (24 bytes):
     *   +0  i32 tag (0=Err, 1=Ok)
     *   +8  V      (Ok payload)
     *   +16 E      (Err payload)
     */
    ir::IrValueId Lowering::lower_try_expr(ast::TryExpr *e) {
        if (!e || !e->operand) {
            error_at(e ? e->loc : SourceLoc{}, "lowering: TryExpr sin operand");
            return ir::IR_NO_VALUE;
        }
        const uint32_t src_line = e->loc.line;

        // 1. Bajar el operand -> SSA PTR al slot Result.
        const ir::IrValueId v_buf = lower_expr(e->operand.get());
        if (v_buf == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;

        // 2. LOAD i32 del tag en offset 0.
        const ir::IrValueId tag_v = fn_->new_value(ir::IrType::I32); {
            ir::IrInstr ld{};
            ld.op          = ir::IrOp::LOAD;
            ld.type        = ir::IrType::I32;
            ld.dst         = tag_v;
            ld.operands    = {v_buf};
            ld.source_line = src_line;
            fn_->append(current_block_, std::move(ld));
        }

        // 3. Comparacion tag == 0 (=Err).
        const ir::IrValueId zero_v = emit_const(ir::IrType::I32, 0, src_line);
        const ir::IrValueId cond_v = fn_->new_value(ir::IrType::BOOL); {
            ir::IrInstr cm{};
            cm.op          = ir::IrOp::CMP_EQ;
            cm.type        = ir::IrType::BOOL;
            cm.dst         = cond_v;
            cm.operands    = {tag_v, zero_v};
            cm.source_line = src_line;
            fn_->append(current_block_, std::move(cm));
        }

        // 4. Crear bloques: err_bb (early-return), ok_bb (extract value).
        const ir::IrBlockId err_bb = fn_->new_block(
            "try_err_" + std::to_string(ternary_counter_));
        const ir::IrBlockId ok_bb  = fn_->new_block(
            "try_ok_"  + std::to_string(ternary_counter_));
        ++ternary_counter_;

        // br_cond: si tag==0 -> err_bb, else -> ok_bb.
        {
            ir::IrInstr br{};
            br.op           = ir::IrOp::BR_COND;
            br.operands.push_back(cond_v);
            br.target_block = err_bb;
            br.false_block  = ok_bb;
            br.source_line  = src_line;
            fn_->append(current_block_, std::move(br));
            fn_->blocks[current_block_].succs.push_back(err_bb);
            fn_->blocks[current_block_].succs.push_back(ok_bb);
            fn_->blocks[err_bb].preds.push_back(current_block_);
            fn_->blocks[ok_bb].preds.push_back(current_block_);
        }

        // 5. err_bb: copia v_buf (24 bytes) al sret_retbuf + RET.
        // Mismo patron que lower_return cuando sret_active_ es true.
        current_block_ = err_bb;
        block_terminated_ = false;
        if (sret_active_ && sret_retbuf_ != ir::IR_NO_VALUE) {
            const uint64_t qwords = sret_buf_size_ / 8;
            for (uint64_t qi = 0; qi < qwords; ++qi) {
                const uint64_t off = qi * 8;
                const ir::IrValueId v_off    = emit_const(ir::IrType::I64, off, src_line);
                const ir::IrValueId v_src_at = fn_->new_value(ir::IrType::PTR); {
                    ir::IrInstr add{};
                    add.op          = ir::IrOp::ADD;
                    add.type        = ir::IrType::I64;
                    add.dst         = v_src_at;
                    add.operands    = {v_buf, v_off};
                    add.source_line = src_line;
                    fn_->append(current_block_, std::move(add));
                }
                // BugFix 163 (2026-06-05): propagar is_host_ptr de v_buf al LOAD
                // side (igual que el STORE side abajo).  Sin esto el LOAD del
                // Err a copiar usaba `mov` (VM) en vez de `movh` (host) y leia
                // basura -> error(r) != el valor real (path de error de `?`).
                fn_->values[v_src_at].is_host_ptr = fn_->values[v_buf].is_host_ptr;
                const ir::IrValueId v_tmp = fn_->new_value(ir::IrType::I64); {
                    ir::IrInstr ld{};
                    ld.op          = ir::IrOp::LOAD;
                    ld.type        = ir::IrType::I64;
                    ld.dst         = v_tmp;
                    ld.operands    = {v_src_at};
                    ld.source_line = src_line;
                    fn_->append(current_block_, std::move(ld));
                }
                const ir::IrValueId v_off2   = emit_const(ir::IrType::I64, off, src_line);
                const ir::IrValueId v_dst_at = fn_->new_value(ir::IrType::PTR); {
                    ir::IrInstr add{};
                    add.op          = ir::IrOp::ADD;
                    add.type        = ir::IrType::I64;
                    add.dst         = v_dst_at;
                    add.operands    = {sret_retbuf_, v_off2};
                    add.source_line = src_line;
                    fn_->append(current_block_, std::move(add));
                }
                // BugFix sret-cross-mem (2026-06-04): propagar is_host_ptr.
                fn_->values[v_dst_at].is_host_ptr =
                    fn_->values[sret_retbuf_].is_host_ptr;
                {
                    ir::IrInstr st{};
                    st.op          = ir::IrOp::STORE;
                    st.type        = ir::IrType::I64;
                    st.operands    = {v_tmp, v_dst_at};
                    st.source_line = src_line;
                    fn_->append(current_block_, std::move(st));
                }
            }
        }
        // Emit cleanups (synchronized, etc.) y RET.
        emit_cleanups_all();
        {
            ir::IrInstr ret{};
            ret.op          = ir::IrOp::RET;
            ret.type        = ir::IrType::VOID;
            ret.source_line = src_line;
            fn_->append(current_block_, std::move(ret));
            block_terminated_ = true;
        }

        // 6. ok_bb: extraer V de v_buf+8.  El tipo V se obtiene del result_type
        // que el type checker ya validamos (pointee del Result).
        current_block_ = ok_bb;
        block_terminated_ = false;
        const Type result_t = e->result_type;
        const ir::IrType payload_t = (result_t.kind != PrimitiveKind::VOID
                                       && result_t.kind != PrimitiveKind::COUNT)
                                      ? ir_type_from_primitive(result_t.kind)
                                      : ir::IrType::I64;
        const ir::IrValueId v_off8 = emit_const(ir::IrType::I64, 8, src_line);
        const ir::IrValueId v_at8  = fn_->new_value(ir::IrType::PTR); {
            ir::IrInstr add{};
            add.op          = ir::IrOp::ADD;
            add.type        = ir::IrType::I64;
            add.dst         = v_at8;
            add.operands    = {v_buf, v_off8};
            add.source_line = src_line;
            fn_->append(current_block_, std::move(add));
        }
        // BugFix 163 (2026-06-05): propagar is_host_ptr de v_buf a v_at8.  El
        // buffer del Result temporal del operando es un host_ptr; sin esta
        // marca, el LOAD de V emitia `mov` (VM mem) en vez de `movh` (host) y
        // leia 0/basura.  La rama err ya lo propagaba (de ahi que err funcione
        // y ok no).  Aplica al value extraction de la rama ok.
        fn_->values[v_at8].is_host_ptr = fn_->values[v_buf].is_host_ptr;
        const ir::IrValueId v_dst = fn_->new_value(payload_t); {
            ir::IrInstr ld{};
            ld.op          = ir::IrOp::LOAD;
            ld.type        = payload_t;
            ld.dst         = v_dst;
            ld.operands    = {v_at8};
            ld.source_line = src_line;
            fn_->append(current_block_, std::move(ld));
        }
        return v_dst;
    }

    ir::IrValueId Lowering::lower_assign(ast::AssignExpr *e) {
        // admitimos como lvalue: IdentExpr (variable simple) o
        // FieldAccessExpr (p.x = v).  Otros lvalues (deref de puntero,
        // indexado de array)
        if (!e->target) {
            error_at(e->loc, "lowering: target de '=' nulo");
            return ir::IR_NO_VALUE;
        }
        // Caso FieldAccessExpr: dos rutas distintas por tipo de receptor.
        if (e->target->kind == ast::NodeKind::FieldAccessExpr) {
            auto *fa = static_cast<ast::FieldAccessExpr *>(e->target.get());
            // CLASS o static field (limitacion G cerrada, property_kind=3):
            // ruta SETFIELD con offset (lower_class_field_store), que
            // detecta property_kind=3 y emite findclass + setstatic.
            if ((fa->base && fa->base->result_type.kind == PrimitiveKind::CLASS)
                || fa->property_kind == 3) {
                // fix.lazy-string - si el field es de tipo STRING y el
                // rhs es un string literal no interpolado, promovemos el
                // literal a StringObject (STRMAKE) ANTES del store.  Sin
                // esto, escribiriamos el host_ptr al literal en static_data
                // dentro del slot del field, que luego se interpretaria como
                // GcHandle invalido y crashearia al primer acceso.  La
                // promocion ya se hace para var-decl (`string s = "lit"`)
                // pero faltaba esta ruta para `this.field = "lit"` y
                // `obj.field = "lit"`.
                ir::IrValueId rhs      = ir::IR_NO_VALUE;
                bool          promoted = false;
                if (e->value
                    && e->value->kind == ast::NodeKind::StringLitExpr
                    && fa->base
                    && fa->base->result_type.kind == PrimitiveKind::CLASS) {
                    auto *slit = static_cast<ast::StringLitExpr *>(e->value.get());
                    // Promovemos tanto literales puros como interpolados:
                    // el helper detecta el caso y emite STRMAKE simple
                    // (puro) o cadena STRMAKE+STRCAT (interpolado).
                    auto it_cls = tc_.class_layouts().find(
                        fa->base->result_type.struct_name);
                    if (it_cls != tc_.class_layouts().end()) {
                        for (const auto &f: it_cls->second.fields) {
                            if (f.name == fa->field_name
                                && f.type.kind == PrimitiveKind::STRING) {
                                rhs      = lower_string_literal_to_string_object(slit);
                                promoted = true;
                                break;
                            }
                        }
                    }
                }
                if (!promoted) {
                    rhs = lower_expr(e->value.get());
                }
                if (rhs == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
                if (e->op != ast::AssignOp::Assign) {
                    // Compound: leer valor actual via getter o GETFIELD,
                    // aplicar el op, escribir via setter o SETFIELD.  Reusa
                    // lower_class_field_load (maneja getters de propiedades
                    // y GETFIELD por offset) para cero duplicacion logica.
                    ir::IrValueId cur = lower_class_field_load(fa);
                    if (cur == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
                    const ast::BinOp bop = compound_assign_op_to_binop(e->op);
                    rhs                  = emit_binop_ir(bop, cur, rhs,
                                                         fa->result_type.kind, e->loc);
                }
                return lower_class_field_store(fa, rhs, e->loc);
            }
            // STRUCT: ruta original via lower_field_addr + STORE.
            const ir::IrValueId addr = lower_field_addr(fa);
            if (addr == ir::IR_NO_VALUE) {
                (void) lower_expr(e->value.get());
                return ir::IR_NO_VALUE;
            }
            // Bug fix 2026-05-23 (Audit 45): auto-promotion del string literal
            // a StringObject cuando el field STRUCT es de tipo string.  Misma
            // motivacion que CLASS arriba: sin esto el host_ptr al literal
            // se guarda como GcHandle invalido en el slot.
            ir::IrValueId rhs;
            if (fa->result_type.kind == PrimitiveKind::STRING
                && e->value
                && e->value->kind == ast::NodeKind::StringLitExpr) {
                auto *slit = static_cast<ast::StringLitExpr *>(e->value.get());
                rhs = lower_string_literal_to_string_object(slit);
            } else {
                rhs = lower_expr(e->value.get());
            }
            if (rhs == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;

            const ir::IrType ft = ir_type_from_primitive(fa->result_type.kind);
            // Compound assign: leer el valor actual del campo (con
            // extraccion de bit field si aplica), aplicar el operador,
            // y luego seguir con la ruta de store normal (que tambien
            // maneja bit field RMW).
            if (e->op != ast::AssignOp::Assign) {
                ir::IrValueId cur = lower_field_access(fa);
                if (cur == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
                const ast::BinOp bop = compound_assign_op_to_binop(e->op);
                rhs                  = emit_binop_ir(bop, cur, rhs,
                                                     fa->result_type.kind, e->loc);
            }
            rhs = cast_if_needed(rhs, fn_->values[rhs].type, ft, e->loc.line);

            // WRITE de bit field: read-modify-write.  Si el campo es bit
            // field, leemos el storage word completo, le limpiamos los
            // bits del rango con AND inverse_mask, le metemos el valor
            // con OR ((rhs & mask) << bit_offset), y hacemos STORE de
            // vuelta.  Para campo normal (no-bitfield): STORE directo
            // del rhs sin lectura previa.
            const Type bt = fa->base ? fa->base->result_type : Type{};
            if (bt.kind == PrimitiveKind::STRUCT) {
                const auto &layouts = tc_.struct_layouts();
                auto        it_l    = layouts.find(bt.struct_name);
                if (it_l != layouts.end()) {
                    for (const auto &f: it_l->second.fields) {
                        if (f.name == fa->field_name && f.bit_width > 0) {
                            // 1. LOAD storage word completo.
                            ir::IrValueId v_old = fn_->new_value(ft); {
                                ir::IrInstr ld{};
                                ld.op          = ir::IrOp::LOAD;
                                ld.type        = ft;
                                ld.dst         = v_old;
                                ld.operands    = {addr};
                                ld.source_line = e->loc.line;
                                fn_->append(current_block_, std::move(ld));
                            }
                            // 2. mask = (1 << bit_width) - 1 (en el tipo
                            //    del storage; truncar a tamano del LOAD).
                            const uint64_t mask = (f.bit_width == 64)
                                                      ? UINT64_MAX
                                                      : ((uint64_t(1) << f.bit_width) - 1);
                            const uint64_t inv_mask =
                                    ~(mask << f.bit_offset);
                            // 3. cleared = old & inv_mask
                            ir::IrValueId v_inv = emit_const(ft, inv_mask, e->loc.line);
                            ir::IrValueId v_clr = fn_->new_value(ft); {
                                ir::IrInstr an{};
                                an.op          = ir::IrOp::AND;
                                an.type        = ft;
                                an.dst         = v_clr;
                                an.operands    = {v_old, v_inv};
                                an.source_line = e->loc.line;
                                fn_->append(current_block_, std::move(an));
                            }
                            // 4. trimmed = rhs & mask  (clamp a rango).
                            ir::IrValueId v_msk = emit_const(ft, mask, e->loc.line);
                            ir::IrValueId v_tr  = fn_->new_value(ft); {
                                ir::IrInstr an{};
                                an.op          = ir::IrOp::AND;
                                an.type        = ft;
                                an.dst         = v_tr;
                                an.operands    = {rhs, v_msk};
                                an.source_line = e->loc.line;
                                fn_->append(current_block_, std::move(an));
                            }
                            // 5. shifted = trimmed << bit_offset
                            ir::IrValueId v_sh = v_tr;
                            if (f.bit_offset > 0) {
                                ir::IrValueId v_amt = emit_const(ft,
                                                                 (uint64_t) f.bit_offset, e->loc.line);
                                v_sh = fn_->new_value(ft);
                                ir::IrInstr sh{};
                                sh.op          = ir::IrOp::SHL;
                                sh.type        = ft;
                                sh.dst         = v_sh;
                                sh.operands    = {v_tr, v_amt};
                                sh.source_line = e->loc.line;
                                fn_->append(current_block_, std::move(sh));
                            }
                            // 6. new = cleared | shifted
                            ir::IrValueId v_new = fn_->new_value(ft); {
                                ir::IrInstr or_{};
                                or_.op          = ir::IrOp::OR;
                                or_.type        = ft;
                                or_.dst         = v_new;
                                or_.operands    = {v_clr, v_sh};
                                or_.source_line = e->loc.line;
                                fn_->append(current_block_, std::move(or_));
                            }
                            // 7. STORE new -> addr
                            ir::IrInstr st{};
                            st.op          = ir::IrOp::STORE;
                            st.type        = ft;
                            st.dst         = ir::IR_NO_VALUE;
                            st.operands    = {v_new, addr};
                            st.source_line = e->loc.line;
                            fn_->append(current_block_, std::move(st));
                            return rhs;
                        }
                    }
                }
            }
            // Campo normal: STORE directo.
            ir::IrInstr st{};
            st.op          = ir::IrOp::STORE;
            st.type        = ft;
            st.dst         = ir::IR_NO_VALUE;
            st.operands    = {rhs, addr}; // STORE: operands[0]=val, operands[1]=ptr
            st.source_line = e->loc.line;
            fn_->append(current_block_, std::move(st));
            return rhs;
        }
        // Caso IndexExpr: 'p[i] = v' equivale a *(p + i*sizeof(*p)) = v.
        // Reusamos lower_index_addr para calcular el puntero del elemento.
        if (e->target->kind == ast::NodeKind::IndexExpr) {
            auto *              ix   = static_cast<ast::IndexExpr *>(e->target.get());
            const ir::IrValueId addr = lower_index_addr(ix);
            if (addr == ir::IR_NO_VALUE) {
                (void) lower_expr(e->value.get());
                return ir::IR_NO_VALUE;
            }
            // Caso struct-value en slot de array: `arr[i] = struct_expr`
            // necesita memcpy de sizeof(Struct) bytes desde el RHS PTR al
            // slot (igual que `*ptr = struct_value`).  Sin esto, solo se
            // copia el primer qword.
            if ((ix->result_type.kind == PrimitiveKind::STRUCT
              || ix->result_type.kind == PrimitiveKind::ARRAY)
             && e->op == ast::AssignOp::Assign) {
                uint64_t struct_size = 0;
                if (ix->result_type.kind == PrimitiveKind::STRUCT) {
                    const auto &layouts = tc_.struct_layouts();
                    auto it = layouts.find(ix->result_type.struct_name);
                    if (it != layouts.end()) {
                        struct_size = static_cast<uint64_t>(it->second.size_bytes);
                    }
                    if (struct_size == 0) {
                        const auto &elays = tc_.enum_layouts();
                        auto ite = elays.find(ix->result_type.struct_name);
                        if (ite != elays.end()) {
                            struct_size = static_cast<uint64_t>(ite->second.size_bytes);
                        }
                    }
                }
                if (struct_size > 0 && (struct_size % 8) == 0) {
                    const ir::IrValueId src = lower_expr(e->value.get());
                    if (src == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
                    const bool src_host = fn_->values[src].is_host_ptr;
                    const bool dst_host = fn_->values[addr].is_host_ptr;
                    const uint64_t qwords = struct_size / 8;
                    for (uint64_t q = 0; q < qwords; ++q) {
                        ir::IrValueId off_src = src;
                        ir::IrValueId off_dst = addr;
                        if (q > 0) {
                            const uint64_t byte_off = q * 8;
                            ir::IrValueId v_off = emit_const(ir::IrType::I64,
                                                               byte_off,
                                                               e->loc.line);
                            {
                                ir::IrValueId v_new = fn_->new_value(ir::IrType::PTR);
                                if (src_host) fn_->values[v_new].is_host_ptr = true;
                                ir::IrInstr ad{};
                                ad.op       = ir::IrOp::ADD;
                                ad.type     = ir::IrType::I64;
                                ad.dst      = v_new;
                                ad.operands = {src, v_off};
                                ad.source_line = e->loc.line;
                                fn_->append(current_block_, std::move(ad));
                                off_src = v_new;
                            }
                            {
                                ir::IrValueId v_new = fn_->new_value(ir::IrType::PTR);
                                if (dst_host) fn_->values[v_new].is_host_ptr = true;
                                ir::IrInstr ad{};
                                ad.op       = ir::IrOp::ADD;
                                ad.type     = ir::IrType::I64;
                                ad.dst      = v_new;
                                ad.operands = {addr, v_off};
                                ad.source_line = e->loc.line;
                                fn_->append(current_block_, std::move(ad));
                                off_dst = v_new;
                            }
                        }
                        ir::IrValueId v_qw = fn_->new_value(ir::IrType::I64);
                        {
                            ir::IrInstr ld{};
                            ld.op       = ir::IrOp::LOAD;
                            ld.type     = ir::IrType::I64;
                            ld.dst      = v_qw;
                            ld.operands = {off_src};
                            ld.source_line = e->loc.line;
                            fn_->append(current_block_, std::move(ld));
                        }
                        {
                            ir::IrInstr st{};
                            st.op       = ir::IrOp::STORE;
                            st.type     = ir::IrType::I64;
                            st.dst      = ir::IR_NO_VALUE;
                            st.operands = {v_qw, off_dst};
                            st.source_line = e->loc.line;
                            fn_->append(current_block_, std::move(st));
                        }
                    }
                    return addr;
                }
            }
            // Bug fix 2026-05-23 (Audit 44): auto-promotion de string literal
            // a StringObject cuando el slot del array es de tipo string.
            // Sin esto, `arr[i] = "lit"` almacenaba la direccion raw del
            // literal y `str_length(arr[i])` daba 0 / garbage.
            ir::IrValueId rhs;
            if (ix->result_type.kind == PrimitiveKind::STRING
                && e->value
                && e->value->kind == ast::NodeKind::StringLitExpr) {
                auto *slit = static_cast<ast::StringLitExpr *>(e->value.get());
                rhs = lower_string_literal_to_string_object(slit);
            } else {
                rhs = lower_expr(e->value.get());
            }
            if (rhs == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            const ir::IrType pt = ir_type_from_primitive(ix->result_type.kind);
            // Compound assign: LOAD elemento, op, STORE de vuelta a la
            // misma direccion (calculada una sola vez).  Cubre +=, -= y
            // todos los compound enteros/float sobre arrays e indexados.
            if (e->op != ast::AssignOp::Assign) {
                ir::IrValueId v_old = fn_->new_value(pt);
                ir::IrInstr   ld{};
                ld.op          = ir::IrOp::LOAD;
                ld.type        = pt;
                ld.dst         = v_old;
                ld.operands    = {addr};
                ld.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ld));
                const ast::BinOp bop = compound_assign_op_to_binop(e->op);
                rhs                  = emit_binop_ir(bop, v_old, rhs,
                                                     ix->result_type.kind, e->loc);
            }
            rhs = cast_if_needed(rhs, fn_->values[rhs].type, pt, e->loc.line);
            ir::IrInstr st{};
            st.op          = ir::IrOp::STORE;
            st.type        = pt;
            st.dst         = ir::IR_NO_VALUE;
            st.operands    = {rhs, addr};
            st.source_line = e->loc.line;
            fn_->append(current_block_, std::move(st));
            return rhs;
        }
        // Caso UnaryExpr(Deref, p): '*p = v' escribe a traves del puntero.
        if (e->target->kind == ast::NodeKind::UnaryExpr) {
            auto *un = static_cast<ast::UnaryExpr *>(e->target.get());
            if (un->op == ast::UnOp::Deref) {
                // Bajar el puntero (operando del Deref) y el valor.
                const ir::IrValueId addr = lower_expr(un->operand.get());
                if (addr == ir::IR_NO_VALUE) {
                    (void) lower_expr(e->value.get());
                    return ir::IR_NO_VALUE;
                }
                // Caso struct-value assign `*ptr = struct_expr`: el lowering
                // generico emite un solo STORE de 8 bytes (ptr value), lo
                // que SOLO copia el primer qword del struct.  Para structs
                // reales necesitamos memcpy del payload completo.
                if ((un->result_type.kind == PrimitiveKind::STRUCT
                  || un->result_type.kind == PrimitiveKind::ARRAY)
                 && e->op == ast::AssignOp::Assign) {
                    // Calcular sizeof.  STRUCT: lookup en struct_layouts_;
                    // ARRAY: type.array_size * sizeof(elt) si conocido.
                    uint64_t struct_size = 0;
                    if (un->result_type.kind == PrimitiveKind::STRUCT) {
                        const auto &layouts = tc_.struct_layouts();
                        auto it = layouts.find(un->result_type.struct_name);
                        if (it != layouts.end()) {
                            struct_size = static_cast<uint64_t>(it->second.size_bytes);
                        }
                        // Tambien enum (encoded como STRUCT con struct_name).
                        if (struct_size == 0) {
                            const auto &elays = tc_.enum_layouts();
                            auto ite = elays.find(un->result_type.struct_name);
                            if (ite != elays.end()) {
                                struct_size = static_cast<uint64_t>(ite->second.size_bytes);
                            }
                        }
                    }
                    if (struct_size > 0 && (struct_size % 8) == 0) {
                        // Bajar RHS para obtener el PTR fuente.
                        const ir::IrValueId src = lower_expr(e->value.get());
                        if (src == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
                        // Copia qword a qword.  Para size_bytes no multiplo
                        // de 8 usariamos byte-loops; los structs Vex tienen
                        // padding a 8-bytes por field-alignment, asi que
                        // size_bytes siempre es multiplo de 8 para Vex
                        // structs.  Defensa por bytes <8: fall-through.
                        // Propagamos is_host_ptr de src/addr a los LOAD/STORE
                        // para emitir movh cuando corresponda.
                        const bool src_host  = fn_->values[src].is_host_ptr;
                        const bool dst_host  = fn_->values[addr].is_host_ptr;
                        const uint64_t qwords = struct_size / 8;
                        for (uint64_t q = 0; q < qwords; ++q) {
                            // src + q*8
                            ir::IrValueId off_src = src;
                            ir::IrValueId off_dst = addr;
                            if (q > 0) {
                                const uint64_t byte_off = q * 8;
                                ir::IrValueId v_off = emit_const(ir::IrType::I64,
                                                                   byte_off,
                                                                   e->loc.line);
                                // src + off
                                {
                                    ir::IrValueId v_new = fn_->new_value(ir::IrType::PTR);
                                    if (src_host) fn_->values[v_new].is_host_ptr = true;
                                    ir::IrInstr ad{};
                                    ad.op          = ir::IrOp::ADD;
                                    ad.type        = ir::IrType::I64;
                                    ad.dst         = v_new;
                                    ad.operands    = {src, v_off};
                                    ad.source_line = e->loc.line;
                                    fn_->append(current_block_, std::move(ad));
                                    off_src = v_new;
                                }
                                {
                                    ir::IrValueId v_new = fn_->new_value(ir::IrType::PTR);
                                    if (dst_host) fn_->values[v_new].is_host_ptr = true;
                                    ir::IrInstr ad{};
                                    ad.op          = ir::IrOp::ADD;
                                    ad.type        = ir::IrType::I64;
                                    ad.dst         = v_new;
                                    ad.operands    = {addr, v_off};
                                    ad.source_line = e->loc.line;
                                    fn_->append(current_block_, std::move(ad));
                                    off_dst = v_new;
                                }
                            }
                            // LOAD i64 del src + q*8
                            ir::IrValueId v_qw = fn_->new_value(ir::IrType::I64);
                            {
                                ir::IrInstr ld{};
                                ld.op          = ir::IrOp::LOAD;
                                ld.type        = ir::IrType::I64;
                                ld.dst         = v_qw;
                                ld.operands    = {off_src};
                                ld.source_line = e->loc.line;
                                fn_->append(current_block_, std::move(ld));
                            }
                            // STORE al dst + q*8
                            {
                                ir::IrInstr st{};
                                st.op          = ir::IrOp::STORE;
                                st.type        = ir::IrType::I64;
                                st.dst         = ir::IR_NO_VALUE;
                                st.operands    = {v_qw, off_dst};
                                st.source_line = e->loc.line;
                                fn_->append(current_block_, std::move(st));
                            }
                        }
                        return addr;
                    }
                    // Si no se pudo calcular el size, cae al path generico
                    // (que solo copia 8 bytes -- bug documentado).
                }
                // Bug fix 2026-05-23 (Audit 45): auto-promotion para `*p = "lit"`
                // cuando p es string* (deref produce STRING).
                ir::IrValueId rhs;
                if (un->result_type.kind == PrimitiveKind::STRING
                    && e->value
                    && e->value->kind == ast::NodeKind::StringLitExpr) {
                    auto *slit = static_cast<ast::StringLitExpr *>(e->value.get());
                    rhs = lower_string_literal_to_string_object(slit);
                } else {
                    rhs = lower_expr(e->value.get());
                }
                if (rhs == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;

                const ir::IrType pt = ir_type_from_primitive(un->result_type.kind);
                // Compound assign sobre '*p': LOAD valor actual, op, STORE.
                if (e->op != ast::AssignOp::Assign) {
                    ir::IrValueId v_old = fn_->new_value(pt);
                    ir::IrInstr   ld{};
                    ld.op          = ir::IrOp::LOAD;
                    ld.type        = pt;
                    ld.dst         = v_old;
                    ld.operands    = {addr};
                    ld.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(ld));
                    const ast::BinOp bop = compound_assign_op_to_binop(e->op);
                    rhs                  = emit_binop_ir(bop, v_old, rhs,
                                                         un->result_type.kind, e->loc);
                }
                rhs = cast_if_needed(rhs, fn_->values[rhs].type, pt, e->loc.line);
                ir::IrInstr st{};
                st.op          = ir::IrOp::STORE;
                st.type        = pt;
                st.dst         = ir::IR_NO_VALUE;
                st.operands    = {rhs, addr};
                st.source_line = e->loc.line;
                fn_->append(current_block_, std::move(st));
                return rhs;
            }
        }
        if (e->target->kind != ast::NodeKind::IdentExpr) {
            error_at(e->loc,
                     "lowering: el lado izquierdo de '=' debe ser un identificador o un acceso a campo");
            (void) lower_expr(e->value.get());
            return ir::IR_NO_VALUE;
        }
        auto *id = static_cast<ast::IdentExpr *>(e->target.get());

        // Bajar el lado derecho.
        // Bug fix 2026-05-23 (Audit 48): auto-promotion del string literal a
        // StringObject cuando el target es una var local de tipo string.  Sin
        // esto, `s = "lit"` (post var-decl) almacenaba el host_ptr al literal
        // como GcHandle invalido.
        ir::IrValueId rhs;
        if (id->result_type.kind == PrimitiveKind::STRING
            && e->value
            && e->value->kind == ast::NodeKind::StringLitExpr) {
            auto *slit = static_cast<ast::StringLitExpr *>(e->value.get());
            rhs = lower_string_literal_to_string_object(slit);
        } else {
            rhs = lower_expr(e->value.get());
        }
        if (rhs == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;

        /* L2.2: target es global runtime con storage en static_data.
         * Emit STORE al slot.  Soporta `=` directo y compound assigns
         * via load-modify-store. */
        {
            auto rit = runtime_global_slots_.find(id->name);
            if (rit != runtime_global_slots_.end()) {
                const uint64_t slot_idx = rit->second;
                const int ln = e->loc.line;
                ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR); {
                    ir::IrInstr is{};
                    is.op          = ir::IrOp::STR_LIT_ADDR;
                    is.type        = ir::IrType::PTR;
                    is.dst         = v_addr;
                    is.imm         = slot_idx;
                    is.source_line = ln;
                    fn_->append(current_block_, std::move(is));
                }
                // Compound assign: load cur + combine.
                if (e->op != ast::AssignOp::Assign) {
                    ir::IrValueId v_cur = fn_->new_value(ir::IrType::I64); {
                        ir::IrInstr ld{};
                        ld.op          = ir::IrOp::LOAD;
                        ld.type        = ir::IrType::I64;
                        ld.dst         = v_cur;
                        ld.operands    = {v_addr};
                        ld.source_line = ln;
                        fn_->append(current_block_, std::move(ld));
                    }
                    const ast::BinOp bop = compound_assign_op_to_binop(e->op);
                    rhs = emit_binop_ir(bop, v_cur, rhs,
                                         PrimitiveKind::I64, e->loc);
                }
                ir::IrInstr st{};
                st.op          = ir::IrOp::STORE;
                st.type        = ir::IrType::I64;
                st.operands    = {rhs, v_addr};
                st.source_line = ln;
                fn_->append(current_block_, std::move(st));
                return rhs;
            }
        }

        /* Phase MC.17.2: si estamos dentro de un @Macro Y el target es
         * un comptime global int, emit STORE al slot @c static_data
         * correspondiente.  Soporta `=` directo y compound `+=`/`-=`
         * (el caller computa cur op rhs en `rhs` antes de llegar aqui). */
        if (current_fn_is_macro_) {
            auto cit = tc_.comptime_const_values().find(id->name);
            if (cit != tc_.comptime_const_values().end() && !cit->second.is_str) {
                const uint64_t slot_idx =
                    get_or_create_comptime_global_slot(id->name);
                if (slot_idx != UINT64_MAX) {
                    const int ln = e->loc.line;
                    /* Si compound assign, leer valor actual y combinar
                     * con rhs ANTES del store.  Esto es paralelo al
                     * camino general que sigue mas abajo, pero como
                     * salimos antes de llegar a ese punto, lo
                     * replicamos aqui inline para compound. */
                    if (e->op != ast::AssignOp::Assign) {
                        /* Compound assign sobre global: load cur from
                         * slot + combine + store back. */
                        ir::IrValueId v_addr_load = fn_->new_value(ir::IrType::PTR);
                        {
                            ir::IrInstr is{};
                            is.op          = ir::IrOp::STR_LIT_ADDR;
                            is.type        = ir::IrType::PTR;
                            is.dst         = v_addr_load;
                            is.imm         = slot_idx;
                            is.source_line = ln;
                            fn_->append(current_block_, std::move(is));
                        }
                        ir::IrValueId v_cur = fn_->new_value(ir::IrType::I64);
                        {
                            ir::IrInstr ld{};
                            ld.op          = ir::IrOp::LOAD;
                            ld.type        = ir::IrType::I64;
                            ld.dst         = v_cur;
                            ld.operands    = {v_addr_load};
                            ld.source_line = ln;
                            fn_->append(current_block_, std::move(ld));
                        }
                        /* Combine via emit_binop equivalent.  Mapeamos
                         * AssignOp -> BinOp y emitimos.  Para simplicidad
                         * solo cubrimos los compound mas comunes; otros
                         * caen al camino general (que falla porque
                         * write_local no encontrara el name). */
                        ast::BinOp bop = ast::BinOp::Add;
                        bool       supported = true;
                        switch (e->op) {
                            case ast::AssignOp::AddAssign: bop = ast::BinOp::Add; break;
                            case ast::AssignOp::SubAssign: bop = ast::BinOp::Sub; break;
                            case ast::AssignOp::MulAssign: bop = ast::BinOp::Mul; break;
                            case ast::AssignOp::DivAssign: bop = ast::BinOp::Div; break;
                            case ast::AssignOp::ModAssign: bop = ast::BinOp::Mod; break;
                            case ast::AssignOp::BitAndAssign: bop = ast::BinOp::BitAnd; break;
                            case ast::AssignOp::BitOrAssign:  bop = ast::BinOp::BitOr;  break;
                            case ast::AssignOp::BitXorAssign: bop = ast::BinOp::BitXor; break;
                            case ast::AssignOp::ShlAssign: bop = ast::BinOp::Shl; break;
                            case ast::AssignOp::ShrAssign: bop = ast::BinOp::Shr; break;
                            default: supported = false; break;
                        }
                        if (supported) {
                            /* Use emit_binop_ir (mismo helper que el
                             * camino normal de compound assign).  Common
                             * = I64 (los globals son int de 64-bit). */
                            rhs = emit_binop_ir(bop, v_cur, rhs,
                                                 PrimitiveKind::I64, e->loc);
                        }
                    }
                    /* STORE rhs al slot. */
                    ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
                    {
                        ir::IrInstr is{};
                        is.op          = ir::IrOp::STR_LIT_ADDR;
                        is.type        = ir::IrType::PTR;
                        is.dst         = v_addr;
                        is.imm         = slot_idx;
                        is.source_line = ln;
                        fn_->append(current_block_, std::move(is));
                    }
                    ir::IrInstr st{};
                    st.op          = ir::IrOp::STORE;
                    st.type        = ir::IrType::I64;
                    st.operands    = {rhs, v_addr};
                    st.source_line = ln;
                    fn_->append(current_block_, std::move(st));
                    return rhs;
                }
            }
        }

        // Tipo destino: el del simbolo en el scope (o el result_type del
        // target que el type checker dejo).
        const ir::IrType dst_ir = ir_type_from_primitive(e->target->result_type.kind);

        // Para asignaciones compuestas (+=, -=, etc.) cargamos el valor
        // actual y combinamos.  El operador ASCII '=' simplemente se
        // ignora aqui y va directo al write_local con rhs.
        if (e->op != ast::AssignOp::Assign) {
            // Lectura previa respeta promocion address-taken.
            const ir::IrValueId cur = read_local(id->name, dst_ir, e->loc.line);
            if (cur == ir::IR_NO_VALUE) {
                error_at(e->loc, "lowering: nombre no resuelto: '" + id->name + "'");
                return ir::IR_NO_VALUE;
            }
            // Promocion al tipo comun entre cur y rhs (igual que en
            // lower_binary).  En la mayoria de casos ambos tienen el
            // tipo de la variable; el cast es trivial.
            const PrimitiveKind ltk    = e->target->result_type.kind;
            const PrimitiveKind rtk    = e->value->result_type.kind;
            const PrimitiveKind common = (ltk == PrimitiveKind::BOOL && rtk == PrimitiveKind::BOOL)
                                             ? PrimitiveKind::BOOL
                                             : promote_arith(ltk, rtk);
            const ir::IrType common_ir = ir_type_from_primitive(common);

            ir::IrValueId l = cast_if_needed(cur, ir_type_from_primitive(ltk),
                                             common_ir, e->loc.line);
            ir::IrValueId r = cast_if_needed(rhs, ir_type_from_primitive(rtk),
                                             common_ir, e->loc.line);

            // Mapear AssignOp a su BinOp equivalente.
            ast::BinOp bop = ast::BinOp::Add;
            switch (e->op) {
                case ast::AssignOp::AddAssign: bop = ast::BinOp::Add;
                    break;
                case ast::AssignOp::SubAssign: bop = ast::BinOp::Sub;
                    break;
                case ast::AssignOp::MulAssign: bop = ast::BinOp::Mul;
                    break;
                case ast::AssignOp::DivAssign: bop = ast::BinOp::Div;
                    break;
                case ast::AssignOp::ModAssign: bop = ast::BinOp::Mod;
                    break;
                case ast::AssignOp::BitAndAssign: bop = ast::BinOp::BitAnd;
                    break;
                case ast::AssignOp::BitOrAssign:  bop = ast::BinOp::BitOr;
                    break;
                case ast::AssignOp::BitXorAssign: bop = ast::BinOp::BitXor;
                    break;
                case ast::AssignOp::ShlAssign: bop = ast::BinOp::Shl;
                    break;
                case ast::AssignOp::ShrAssign: bop = ast::BinOp::Shr;
                    break;
                case ast::AssignOp::Assign: break; // ya filtrado arriba
            }
            /* Bug fix (port de Desmon fa13d6a8): `s += "algo ${x}"` devolvia
             * cadena VACIA -- el compound assign de STRING iba por
             * emit_binop_ir(BinOp::Add), que emite ADD aritmetico y NO
             * concatena.  Para STRING la unica suma es STRCAT; emitirla aqui
             * (el RHS literal ya vino promovido a StringObject arriba). */
            if (e->op == ast::AssignOp::AddAssign
                && e->target->result_type.kind == PrimitiveKind::STRING) {
                rhs = emit_strcat(l, r, e->loc.line);
            } else {
                rhs = emit_binop_ir(bop, l, r, common, e->loc);
            }
        }

        // Cast final al tipo declarado de la variable y actualizar el scope.
        const ir::IrType rhs_ir = (rhs != ir::IR_NO_VALUE)
                                      ? fn_->values[rhs].type
                                      : dst_ir;
        rhs = cast_if_needed(rhs, rhs_ir, dst_ir, e->loc.line);
        write_local(id->name, rhs, dst_ir, e->loc.line);
        return rhs;
    }

    // ---------------------------------------------------------------------
    // Lowering de literales de string y builtins FFI.
    // ---------------------------------------------------------------------

} // namespace vex

