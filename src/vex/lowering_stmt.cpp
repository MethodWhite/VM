#include "vex/lowering.h"
#include "vex/collection_intrinsics.h"
#include "vex/comptime_introspect.h"
#include "ir/ir_optimizer.h"

#include <functional>
#include <set>
#include <sstream>
#include <utility>

namespace vex {

namespace {
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
            default: return 8;
        }
    }
}

// Forward decl of helpers defined in lowering.cpp used by stmt lowering
static void collect_assigned_vars(const ast::Node *n,
                                   std::set<std::string> &out);

static void scan_assign(ast::Stmt *s, std::set<std::string> &out);
static void scan_read_expr(ast::Expr *e, std::set<std::string> &out);

    void Lowering::lower_stmt(ast::Stmt *s) {
        if (!s) return;
        switch (s->kind) {
            case ast::NodeKind::BlockStmt:
                lower_block(static_cast<ast::BlockStmt *>(s));
                return;
            case ast::NodeKind::VarDeclStmt: {
                /* A.39: `comptime const NAME = expr;` local NO emite
                 * codigo runtime.  Para comptime const declarados dentro
                 * de un `comptime for` body, los re-evaluamos en CADA
                 * iteracion via el stack dinamico del lowering -- el
                 * type checker solo evalua una vez con el valor inicial.
                 * Asi `comptime const SQ = j * j;` se actualiza por iter. */
                auto *vd = static_cast<ast::VarDeclStmt *>(s);
                /* Phase MC.17.1: cuando estamos dentro de un @Macro body
                 * que se baja a IR, las VarDeclStmt marcadas
                 * @c is_comptime se tratan como vars runtime regulares.
                 * El macro corre en VM y los locales se computan en
                 * cada invocacion -- mismo resultado semantico que la
                 * evaluacion AST que ocurria one-time. */
                if (vd->is_comptime && current_fn_is_macro_) {
                    lower_var_decl(vd);
                    return;
                }
                if (vd->is_comptime) {
                    if (!lowering_comptime_scopes_.empty() && vd->init) {
                        /* Re-evaluar el init con el stack dinamico actual. */
                        auto &mut_tc = const_cast<TypeChecker &>(tc_);
                        const ComptimeEvalResult r =
                            comptime_eval_expr(mut_tc, vd->init.get());
                        if (r.ok) {
                            ComptimeLocalEntry ent;
                            if (r.is_str) {
                                ent.is_str = true; ent.str_value = r.str;
                            } else {
                                ent.value = r.value;
                            }
                            ent.ir_t = ir::IrType::I64;
                            /* Bind en el scope DEL TOPE actual (cae al pop
                             * del enclosing comptime for o block). */
                            lowering_comptime_scopes_.back()[vd->name] = ent;
                            /* Tambien actualizar tc para que evaluaciones
                             * posteriores en el body lo vean. */
                            TypeChecker::ComptimeConst c;
                            c.type   = tc_.resolve_type_node(vd->type.get());
                            c.is_str = r.is_str;
                            if (r.is_str) c.str_value = r.str;
                            else          c.value     = r.value;
                            mut_tc.register_comptime_local(vd->name, std::move(c));
                        }
                    }
                    return;
                }
                lower_var_decl(vd);
                return;
            }
            case ast::NodeKind::ComptimeBlockStmt:
                /* A.39: el bloque comptime no emite codigo runtime.  El
                 * type checker ya proceso sus stmts (comptime const +
                 * static_assert + comptime for/if).  Cualquier valor
                 * comptime queda anotado en los IdentExpr correspondientes. */
                return;
            case ast::NodeKind::ComptimeForStmt:
                lower_comptime_for(static_cast<ast::ComptimeForStmt *>(s));
                return;
            case ast::NodeKind::ExprStmt: {
                auto *es = static_cast<ast::ExprStmt *>(s);
                if (es->expr) (void) lower_expr(es->expr.get());
                return;
            }
            case ast::NodeKind::IfStmt:
                lower_if(static_cast<ast::IfStmt *>(s));
                return;
            case ast::NodeKind::ReturnStmt:
                lower_return(static_cast<ast::ReturnStmt *>(s));
                return;
            case ast::NodeKind::WhileStmt:
                lower_while(static_cast<ast::WhileStmt *>(s));
                return;
            case ast::NodeKind::DoWhileStmt:
                lower_do_while(static_cast<ast::DoWhileStmt *>(s));
                return;
            case ast::NodeKind::ForStmt:
                lower_for(static_cast<ast::ForStmt *>(s));
                return;
            case ast::NodeKind::BreakStmt: {
                if (loop_targets_.empty()) {
                    error_at(s->loc, "'break' fuera de un loop");
                    return;
                }
                LoopTargets &lt = loop_targets_.back();
                // Registrar este bloque y el snapshot del scope para que
                // lower_while/for complete los PHIs del exit_bb con los SSA
                // values en este punto.  Sin esto, las variables modificadas
                // en el body antes del `break` NO se propagan al exit del
                // loop (los lectores del exit ven los SSA values del
                // INICIO de la iteracion, no del final).  Mismo patron que
                // continue_preds/continue_scopes.
                lt.break_preds.push_back(current_block_);
                lt.break_scopes.push_back(scopes_);
                ir::IrInstr         br{};
                br.op           = ir::IrOp::BR;
                br.target_block = lt.break_bb;
                br.source_line  = s->loc.line;
                fn_->append(current_block_, std::move(br));
                fn_->blocks[current_block_].succs.push_back(lt.break_bb);
                fn_->blocks[lt.break_bb].preds.push_back(current_block_);
                block_terminated_ = true;
                return;
            }
            case ast::NodeKind::ContinueStmt: {
                if (loop_targets_.empty()) {
                    error_at(s->loc, "'continue' fuera de un loop");
                    return;
                }
                LoopTargets &lt = loop_targets_.back();
                // Registrar este bloque y el snapshot del scope para
                // que el lower_while/for complete los PHIs del header
                // con los SSA values en este punto de la ejecucion.
                lt.continue_preds.push_back(current_block_);
                lt.continue_scopes.push_back(scopes_);
                ir::IrInstr br{};
                br.op           = ir::IrOp::BR;
                br.target_block = lt.continue_bb;
                br.source_line  = s->loc.line;
                fn_->append(current_block_, std::move(br));
                fn_->blocks[current_block_].succs.push_back(lt.continue_bb);
                fn_->blocks[lt.continue_bb].preds.push_back(current_block_);
                block_terminated_ = true;
                return;
            }
            case ast::NodeKind::LabelStmt: {
                // Buscar/crear el bloque destino para esta label.  Si
                // ya hay un goto que la referencio, el bloque ya esta
                // creado (declared=false en ese momento); aqui lo
                // declaramos.  Caemos al label_bb desde el bloque
                // actual via BR (transparente: el codigo lineal
                // continua en label_bb tras la label).
                auto *        ls = static_cast<ast::LabelStmt *>(s);
                auto          it = goto_labels_.find(ls->name);
                ir::IrBlockId lab_bb;
                if (it == goto_labels_.end()) {
                    lab_bb = fn_->new_block(std::string("lbl_") + ls->name);
                    GotoEntry ge;
                    ge.block               = lab_bb;
                    ge.declared            = true;
                    ge.first_use_loc       = ls->loc;
                    goto_labels_[ls->name] = ge;
                } else {
                    if (it->second.declared) {
                        error_at(ls->loc,
                                 std::string("label '") + ls->name +
                                 "' ya declarada en esta funcion");
                        return;
                    }
                    it->second.declared = true;
                    lab_bb              = it->second.block;
                }
                // Conectar el bloque actual al label_bb si todavia no
                // termino (fall-through al label).
                if (!block_terminated_) {
                    ir::IrInstr br{};
                    br.op           = ir::IrOp::BR;
                    br.target_block = lab_bb;
                    br.source_line  = ls->loc.line;
                    fn_->append(current_block_, std::move(br));
                    fn_->blocks[current_block_].succs.push_back(lab_bb);
                    fn_->blocks[lab_bb].preds.push_back(current_block_);
                }
                current_block_    = lab_bb;
                block_terminated_ = false;
                return;
            }
            case ast::NodeKind::GotoStmt: {
                auto *        gs = static_cast<ast::GotoStmt *>(s);
                auto          it = goto_labels_.find(gs->label);
                ir::IrBlockId lab_bb;
                if (it == goto_labels_.end()) {
                    // Forward goto: crear el bloque ahora; se marcara
                    // declarado al ver la label correspondiente.
                    lab_bb = fn_->new_block(std::string("lbl_") + gs->label);
                    GotoEntry ge;
                    ge.block                = lab_bb;
                    ge.declared             = false;
                    ge.first_use_loc        = gs->loc;
                    goto_labels_[gs->label] = ge;
                } else {
                    lab_bb = it->second.block;
                }
                ir::IrInstr br{};
                br.op           = ir::IrOp::BR;
                br.target_block = lab_bb;
                br.source_line  = gs->loc.line;
                fn_->append(current_block_, std::move(br));
                fn_->blocks[current_block_].succs.push_back(lab_bb);
                fn_->blocks[lab_bb].preds.push_back(current_block_);
                block_terminated_ = true;
                return;
            }
            case ast::NodeKind::TryStmt:
                lower_try(static_cast<ast::TryStmt *>(s));
                return;
            case ast::NodeKind::ThrowStmt:
                lower_throw(static_cast<ast::ThrowStmt *>(s));
                return;
            case ast::NodeKind::ForEachStmt:
                lower_foreach(static_cast<ast::ForEachStmt *>(s));
                return;
            case ast::NodeKind::SynchronizedStmt:
                lower_synchronized(static_cast<ast::SynchronizedStmt *>(s));
                return;
            default:
                unsupported(s->loc, "statement no soportado por el lowering actual");
                return;
        }
    }

    void Lowering::lower_var_decl(ast::VarDeclStmt *vd) {
        // Resolver el Type semantico (aplicando aliases y structs).
        // A.43.7: con `auto`/`var` (vd->infer_type), el AST no tiene
        // TypeNode -> el type checker ya computo y guardo el tipo en
        // `vd->init->result_type` durante check_expr.  Lo reusamos sin
        // re-evaluar el init.
        Type sem_type = vd->type
            ? tc_.resolve_type_node(vd->type.get())
            : (vd->init ? vd->init->result_type : Type{});

        // Phase Z.6: propagar el modificador @c shared del var-decl al
        // @c NewExpr del init.  Si el init es `new T(...)` y el var-decl
        // tiene `shared`, el `new` debe alocar en el SharedHeap en lugar
        // del gc_heap local.  El @c lower_new_expr detecta la marca y
        // emite `__new_<Class>_shared` (que internamente usa @c newobjs).
        if (vd->is_shared && vd->init
            && vd->init->kind == ast::NodeKind::NewExpr) {
            auto *ne = static_cast<ast::NewExpr *>(vd->init.get());
            ne->is_shared = true;
            // Registrar la clase como usada en modo shared para que
            // generate_new_helpers genere su variante `__new_<X>_shared`.
            if (!ne->class_name.empty()) {
                classes_used_shared_.insert(ne->class_name);
            }
        }

        // Phase Z.9: si el var-decl tiene `shared`, registrar el nombre en
        // @c shared_locals_ para que el escape analyzer en spawn capture
        // no genere warning (es shared explicitamente).
        if (vd->is_shared) {
            shared_locals_.insert(vd->name);
        }

        // Tracking para fix #1 newInstance: si el tipo declarado es alias
        // `Class` y el init es `Class.forName("X")` con X literal, registrar
        // var_name -> "X" para que `cls.newInstance()` luego pueda emitir
        // `new X()` directo (con ctor invocado).  Detectamos via
        // FieldAccessExpr con property_kind=100 (forName) que el type
        // checker ya marco.
        if (vd->type && vd->type->kind == ast::NodeKind::NamedTypeNode) {
            const auto *nt = static_cast<const ast::NamedTypeNode *>(vd->type.get());
            const bool is_class_alias = (nt->name == "Class");
            if (is_class_alias && vd->init
                && vd->init->kind == ast::NodeKind::CallExpr) {
                auto *ce = static_cast<ast::CallExpr *>(vd->init.get());
                if (ce->callee
                    && ce->callee->kind == ast::NodeKind::FieldAccessExpr) {
                    auto *fa = static_cast<ast::FieldAccessExpr *>(ce->callee.get());
                    // property_kind 100 = forName (estatico, sin self).
                    if (fa->property_kind == 100
                        && ce->args.size() == 1
                        && ce->args[0]
                        && ce->args[0]->kind == ast::NodeKind::StringLitExpr) {
                        auto *slit = static_cast<ast::StringLitExpr *>(ce->args[0].get());
                        if (!slit->is_interpolated()) {
                            class_origin_of_local_[vd->name] = slit->value;
                        }
                    }
                }
            } else if (is_class_alias) {
                // Init no-trackeable -> borrar entrada previa por seguridad.
                class_origin_of_local_.erase(vd->name);
            }
        }

        // Array init C-style: `i32 arr[N] = {e0, e1, ...};`.
        if (sem_type.kind == PrimitiveKind::ARRAY && vd->init
            && vd->init->kind == ast::NodeKind::InitListExpr) {
            auto *il = static_cast<ast::InitListExpr *>(vd->init.get());
            if (il->is_designated) {
                error_at(vd->loc,
                         "lowering: init designado '.field=' no aplica a arrays");
                return;
            }
            const Type     elem_t  = sem_type.pointee ? *sem_type.pointee : Type{};
            const uint32_t elem_sz = (uint32_t) primitive_size_bytes(elem_t.kind);
            if (elem_sz == 0) {
                error_at(vd->loc, "lowering: tipo del elemento sin sizeof");
                return;
            }
            const uint32_t arr_size = sem_type.array_size > 0
                                          ? (uint32_t) sem_type.array_size
                                          : (uint32_t) il->elements.size();
            if (il->elements.size() > arr_size) {
                error_at(vd->loc, "lowering: init list excede tamano de array");
                return;
            }
            ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr   al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8;
            al.dst         = addr;
            al.imm         = (uint64_t) arr_size * elem_sz;
            al.source_line = vd->loc.line;
            fn_->append(current_block_, std::move(al));
            const ir::IrType ir_elem = ir_type_from_primitive(elem_t.kind);
            for (size_t i = 0; i < il->elements.size(); ++i) {
                ir::IrValueId v_val = lower_expr(il->elements[i].get());
                if (v_val == ir::IR_NO_VALUE) continue;
                // Suprimir warning de narrowing si el elemento es literal
                // (`{10, 20, ...}` con i64-defaulted literals encajando en
                // el tipo de elemento).  Mismo razonamiento que en
                // var-decl con init literal.
                const bool elem_is_literal =
                        il->elements[i]->kind == ast::NodeKind::IntLitExpr
                     || il->elements[i]->kind == ast::NodeKind::FloatLitExpr
                     || il->elements[i]->kind == ast::NodeKind::BoolLitExpr
                     || il->elements[i]->kind == ast::NodeKind::CharLitExpr
                     || il->elements[i]->kind == ast::NodeKind::NullLitExpr;
                v_val = cast_if_needed(v_val,
                                       fn_->values[v_val].type, ir_elem, vd->loc.line,
                                       /*is_explicit=*/elem_is_literal);
                ir::IrValueId v_addr_i = addr;
                if (i > 0) {
                    ir::IrValueId v_off = emit_const(ir::IrType::I64,
                                                     (uint64_t) (i * elem_sz), vd->loc.line);
                    v_addr_i = fn_->new_value(ir::IrType::PTR);
                    ir::IrInstr ad{};
                    ad.op          = ir::IrOp::ADD;
                    ad.type        = ir::IrType::I64;
                    ad.dst         = v_addr_i;
                    ad.operands    = {addr, v_off};
                    ad.source_line = vd->loc.line;
                    fn_->append(current_block_, std::move(ad));
                }
                ir::IrInstr st{};
                st.op          = ir::IrOp::STORE;
                st.type        = ir_elem;
                st.dst         = ir::IR_NO_VALUE;
                st.operands    = {v_val, v_addr_i};
                st.source_line = vd->loc.line;
                fn_->append(current_block_, std::move(st));
            }
            bind(vd->name, addr);
            return;
        }

        // Struct init C-style: `Point p = {.x=1, .y=2};` o
        // posicional `Point p = {1, 2};`.
        if (sem_type.kind == PrimitiveKind::STRUCT && vd->init
            && vd->init->kind == ast::NodeKind::InitListExpr) {
            auto *      il      = static_cast<ast::InitListExpr *>(vd->init.get());
            const auto &layouts = tc_.struct_layouts();
            auto        it_l    = layouts.find(sem_type.struct_name);
            if (it_l == layouts.end()) {
                error_at(vd->loc,
                         "lowering: struct '" + sem_type.struct_name + "' sin layout");
                return;
            }
            const StructLayout &lay  = it_l->second;
            ir::IrValueId       addr = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr         al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8;
            al.dst         = addr;
            al.imm         = (uint64_t) lay.size_bytes;
            al.source_line = vd->loc.line;
            fn_->append(current_block_, std::move(al));
            // Zero los storage words de bit fields antes del
            // loop para evitar que el RMW lea basura del ALLOCA.  Los
            // unique (offset, size) ya estan en lay.fields para bit
            // fields; emit STORE 0 una sola vez por word.
            std::set<std::pair<uint32_t, uint32_t> > zeroed_bf;
            for (const auto &f: lay.fields) {
                if (f.bit_width == 0) continue;
                auto key = std::make_pair(f.offset, f.size);
                if (!zeroed_bf.insert(key).second) continue;
                ir::IrType    ft_zero  = ir_type_from_primitive(f.type.kind);
                ir::IrValueId v_zero   = emit_const(ft_zero, 0, vd->loc.line);
                ir::IrValueId v_addr_w = addr;
                if (f.offset > 0) {
                    ir::IrValueId v_off = emit_const(ir::IrType::I64,
                                                     (uint64_t) f.offset, vd->loc.line);
                    v_addr_w = fn_->new_value(ir::IrType::PTR);
                    ir::IrInstr ad{};
                    ad.op          = ir::IrOp::ADD;
                    ad.type        = ir::IrType::I64;
                    ad.dst         = v_addr_w;
                    ad.operands    = {addr, v_off};
                    ad.source_line = vd->loc.line;
                    fn_->append(current_block_, std::move(ad));
                }
                ir::IrInstr st{};
                st.op          = ir::IrOp::STORE;
                st.type        = ft_zero;
                st.dst         = ir::IR_NO_VALUE;
                st.operands    = {v_zero, v_addr_w};
                st.source_line = vd->loc.line;
                fn_->append(current_block_, std::move(st));
            }
            for (size_t i = 0; i < il->elements.size(); ++i) {
                const StructFieldInfo *fi = nullptr;
                if (il->is_designated) {
                    const std::string &fname = il->field_names[i];
                    for (const auto &f: lay.fields) {
                        if (f.name == fname) {
                            fi = &f;
                            break;
                        }
                    }
                    if (!fi) {
                        error_at(vd->loc,
                                 "lowering: campo '" + fname + "' no existe");
                        continue;
                    }
                } else {
                    if (i >= lay.fields.size()) {
                        error_at(vd->loc, "lowering: init list excede campos");
                        break;
                    }
                    fi = &lay.fields[i];
                }
                ir::IrValueId v_val = lower_expr(il->elements[i].get());
                if (v_val == ir::IR_NO_VALUE) continue;
                const ir::IrType ir_ft = ir_type_from_primitive(fi->type.kind);
                const bool elem_is_literal =
                        il->elements[i]->kind == ast::NodeKind::IntLitExpr
                     || il->elements[i]->kind == ast::NodeKind::FloatLitExpr
                     || il->elements[i]->kind == ast::NodeKind::BoolLitExpr
                     || il->elements[i]->kind == ast::NodeKind::CharLitExpr
                     || il->elements[i]->kind == ast::NodeKind::NullLitExpr;
                v_val = cast_if_needed(v_val,
                                       fn_->values[v_val].type, ir_ft, vd->loc.line,
                                       /*is_explicit=*/elem_is_literal);
                ir::IrValueId v_addr = addr;
                if (fi->offset > 0) {
                    ir::IrValueId v_off = emit_const(ir::IrType::I64,
                                                     (uint64_t) fi->offset, vd->loc.line);
                    v_addr = fn_->new_value(ir::IrType::PTR);
                    ir::IrInstr ad{};
                    ad.op          = ir::IrOp::ADD;
                    ad.type        = ir::IrType::I64;
                    ad.dst         = v_addr;
                    ad.operands    = {addr, v_off};
                    ad.source_line = vd->loc.line;
                    fn_->append(current_block_, std::move(ad));
                }
                // Bit field en init list: read-modify-write.
                // El ALLOCA inicial deja basura; debemos LOAD el storage
                // word actual, limpiar los bits del rango con AND ~mask,
                // OR con (val<<offset), STORE.  Igual que en lower_assign
                // para bit fields.
                if (fi->bit_width > 0) {
                    ir::IrValueId v_old = fn_->new_value(ir_ft);
                    ir::IrInstr   ld{};
                    ld.op          = ir::IrOp::LOAD;
                    ld.type        = ir_ft;
                    ld.dst         = v_old;
                    ld.operands    = {v_addr};
                    ld.source_line = vd->loc.line;
                    fn_->append(current_block_, std::move(ld));
                    const uint64_t mask = (fi->bit_width == 64)
                                              ? UINT64_MAX
                                              : ((uint64_t(1) << fi->bit_width) - 1);
                    const uint64_t inv_mask = ~(mask << fi->bit_offset);
                    ir::IrValueId  v_inv    = emit_const(ir_ft, inv_mask, vd->loc.line);
                    ir::IrValueId  v_clr    = fn_->new_value(ir_ft); {
                        ir::IrInstr an{};
                        an.op          = ir::IrOp::AND;
                        an.type        = ir_ft;
                        an.dst         = v_clr;
                        an.operands    = {v_old, v_inv};
                        an.source_line = vd->loc.line;
                        fn_->append(current_block_, std::move(an));
                    }
                    ir::IrValueId v_msk = emit_const(ir_ft, mask, vd->loc.line);
                    ir::IrValueId v_tr  = fn_->new_value(ir_ft); {
                        ir::IrInstr an{};
                        an.op          = ir::IrOp::AND;
                        an.type        = ir_ft;
                        an.dst         = v_tr;
                        an.operands    = {v_val, v_msk};
                        an.source_line = vd->loc.line;
                        fn_->append(current_block_, std::move(an));
                    }
                    ir::IrValueId v_sh = v_tr;
                    if (fi->bit_offset > 0) {
                        ir::IrValueId v_amt = emit_const(ir_ft,
                                                         (uint64_t) fi->bit_offset, vd->loc.line);
                        v_sh = fn_->new_value(ir_ft);
                        ir::IrInstr sh{};
                        sh.op          = ir::IrOp::SHL;
                        sh.type        = ir_ft;
                        sh.dst         = v_sh;
                        sh.operands    = {v_tr, v_amt};
                        sh.source_line = vd->loc.line;
                        fn_->append(current_block_, std::move(sh));
                    }
                    ir::IrValueId v_new = fn_->new_value(ir_ft); {
                        ir::IrInstr or_{};
                        or_.op          = ir::IrOp::OR;
                        or_.type        = ir_ft;
                        or_.dst         = v_new;
                        or_.operands    = {v_clr, v_sh};
                        or_.source_line = vd->loc.line;
                        fn_->append(current_block_, std::move(or_));
                    }
                    ir::IrInstr st{};
                    st.op          = ir::IrOp::STORE;
                    st.type        = ir_ft;
                    st.dst         = ir::IR_NO_VALUE;
                    st.operands    = {v_new, v_addr};
                    st.source_line = vd->loc.line;
                    fn_->append(current_block_, std::move(st));
                    continue;
                }
                ir::IrInstr st{};
                st.op          = ir::IrOp::STORE;
                st.type        = ir_ft;
                st.dst         = ir::IR_NO_VALUE;
                st.operands    = {v_val, v_addr};
                st.source_line = vd->loc.line;
                fn_->append(current_block_, std::move(st));
            }
            bind(vd->name, addr);
            return;
        }

        // Caso 1: variable de tipo struct.  Reservamos memoria local con
        // ALLOCA del IR (el emisor lo baja a 'subsp rsp, N + readcur') y
        // guardamos el IrValueId del puntero como "current value" de la
        // variable en scope.  El acceso a campos via FieldAccessExpr
        // calcula offsets desde este puntero base.
        if (sem_type.kind == PrimitiveKind::STRUCT) {
            const auto &layouts = tc_.struct_layouts();
            auto        it      = layouts.find(sem_type.struct_name);
            // ADTs: si NO esta en struct_layouts, puede ser un enum
            // (compartimos PrimitiveKind::STRUCT para reusar el camino
            // de value-type).  Buscar en enum_layouts_ y alocar slot
            // de @c size_bytes (8 + 8*max_payload_fields).
            if (it == layouts.end()) {
                const auto &elays = tc_.enum_layouts();
                auto        ite   = elays.find(sem_type.struct_name);
                if (ite != elays.end()) {
                    const EnumLayout &  elay  = ite->second;
                    const ir::IrValueId eaddr = fn_->new_value(ir::IrType::PTR);
                    ir::IrInstr         eal{};
                    eal.op          = ir::IrOp::ALLOCA;
                    eal.type        = ir::IrType::I8;
                    eal.dst         = eaddr;
                    eal.imm         = static_cast<uint64_t>(elay.size_bytes);
                    eal.source_line = vd->loc.line;
                    fn_->append(current_block_, std::move(eal));
                    // Si hay inicializador (constructor de variante),
                    // lower_expr produce un SSA value PTR al slot recien
                    // construido por @c lower_enum_constructor.  En vez
                    // de COPY-ar, simplemente bindeamos al slot del
                    // inicializador (la variable APUNTA al slot del
                    // constructor; el ALLOCA arriba queda sin uso pero
                    // el optimizer DCE lo eliminara).  Esto es equivalente
                    // semanticamente y evita un memcpy de @c size_bytes.
                    if (vd->init) {
                        ir::IrValueId init_addr = lower_expr(vd->init.get());
                        if (init_addr != ir::IR_NO_VALUE) {
                            bind(vd->name, init_addr);
                            return;
                        }
                    }
                    bind(vd->name, eaddr);
                    return;
                }
                error_at(vd->loc,
                         "lowering: struct/enum desconocido '" + sem_type.struct_name + "'");
                return;
            }
            const StructLayout &lay = it->second;
            // ALLOCA del IR reserva count * sizeof(T) bytes; pasamos
            // tipo i8 para que count sea exactamente size_bytes.  El
            // emisor lo traduce a 'subsp rsp, N' + 'readcur rDst'.
            const ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr         ins{};
            ins.op          = ir::IrOp::ALLOCA;
            ins.type        = ir::IrType::I8; // unidad: 1 byte
            ins.dst         = addr;
            ins.imm         = (uint64_t) lay.size_bytes;
            ins.source_line = vd->loc.line;
            fn_->append(current_block_, std::move(ins));
            bind(vd->name, addr);
            // B3 fix: si hay inicializador, lower-lo como PTR al struct
            // origen y copiar qword-by-qword al slot ALLOCA recien creado.
            // Soporta:
            //   - Call result: `Punto v = funcion_que_devuelve_struct(...)`
            //   - read_borrow: `Punto v = read_borrow(b)` (B2 pass-through)
            //   - Otros SSA values PTR a struct.
            // El init list (que SI estaba soportado) se maneja en la rama
            // de mas arriba antes de llegar aqui (linea 1837).
            if (vd->init) {
                const ir::IrValueId v_src = lower_expr(vd->init.get());
                if (v_src != ir::IR_NO_VALUE) {
                    // Heredar is_host_ptr del source para los LOADs.  Si
                    // el src viene de read_borrow / ptr_of (unique), es
                    // host_ptr; si viene de un struct stack ALLOCA es VM.
                    const bool src_is_host = fn_->values[v_src].is_host_ptr;
                    // Copia qword-by-qword (size_bytes redondeado a 8).
                    const uint64_t qwords = (lay.size_bytes + 7) / 8;
                    for (uint64_t qi = 0; qi < qwords; ++qi) {
                        const uint64_t off = qi * 8;
                        const ir::IrValueId v_off = emit_const(ir::IrType::I64,
                            static_cast<int64_t>(off), vd->loc.line);
                        // src + off
                        const ir::IrValueId v_src_at = fn_->new_value(ir::IrType::PTR);
                        fn_->values[v_src_at].is_host_ptr = src_is_host;
                        {
                            ir::IrInstr ad{};
                            ad.op          = ir::IrOp::ADD;
                            ad.type        = ir::IrType::I64;
                            ad.dst         = v_src_at;
                            ad.operands    = {v_src, v_off};
                            ad.source_line = vd->loc.line;
                            fn_->append(current_block_, std::move(ad));
                        }
                        // LOAD i64 from src+off
                        const ir::IrValueId v_word = fn_->new_value(ir::IrType::I64);
                        {
                            ir::IrInstr ld{};
                            ld.op          = ir::IrOp::LOAD;
                            ld.type        = ir::IrType::I64;
                            ld.dst         = v_word;
                            ld.operands    = {v_src_at};
                            ld.source_line = vd->loc.line;
                            fn_->append(current_block_, std::move(ld));
                        }
                        // dst slot (ALLOCA, VM stack) + off
                        const ir::IrValueId v_dst_at = fn_->new_value(ir::IrType::PTR);
                        // dst NO es host_ptr (slot ALLOCA en VM stack).
                        {
                            ir::IrInstr ad{};
                            ad.op          = ir::IrOp::ADD;
                            ad.type        = ir::IrType::I64;
                            ad.dst         = v_dst_at;
                            ad.operands    = {addr, v_off};
                            ad.source_line = vd->loc.line;
                            fn_->append(current_block_, std::move(ad));
                        }
                        // STORE i64 [dst+off] = word
                        {
                            ir::IrInstr st{};
                            st.op          = ir::IrOp::STORE;
                            st.type        = ir::IrType::I64;
                            st.operands    = {v_word, v_dst_at};
                            st.source_line = vd->loc.line;
                            fn_->append(current_block_, std::move(st));
                        }
                    }
                }
            }
            return;
        }

        // C-style string init para arrays byte-like: `u8[N] arr = "literal"`.
        // Detecta el patron y emite STOREs byte-a-byte del contenido del
        // string literal, con zerificacion del resto si N > strlen.  Si
        // strlen > N reporta error (truncation, comportamiento C).
        // No se promueve el literal a StringObject (es array de bytes
        // crudo, sin GC).  Aceptamos solo literales no interpolados.
        if (sem_type.kind == PrimitiveKind::ARRAY && vd->init
            && vd->init->kind == ast::NodeKind::StringLitExpr
            && sem_type.pointee
            && (sem_type.pointee->kind == PrimitiveKind::U8
             || sem_type.pointee->kind == PrimitiveKind::I8
             || sem_type.pointee->kind == PrimitiveKind::CHAR)) {
            auto *sl = static_cast<ast::StringLitExpr *>(vd->init.get());
            if (sl->is_interpolated()) {
                error_at(vd->loc, "init de array con string no acepta interpolacion");
                return;
            }
            const std::string &bytes  = sl->value;
            const uint32_t     str_n  = (uint32_t)bytes.size();
            const uint32_t     arr_n  = sem_type.array_size > 0
                                          ? (uint32_t)sem_type.array_size
                                          : str_n;
            if (str_n > arr_n) {
                error_at(vd->loc, "literal de string (" + std::to_string(str_n)
                                + " bytes) mas grande que el array (" + std::to_string(arr_n) + ")");
                return;
            }
            const Type      elem_t   = *sem_type.pointee;
            const ir::IrType ir_elem  = ir_type_from_primitive(elem_t.kind);
            const uint32_t   elem_sz  = (uint32_t)primitive_size_bytes(elem_t.kind);
            // ALLOCA del array (siempre arr_n elementos).
            ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
            {
                ir::IrInstr al{};
                al.op          = ir::IrOp::ALLOCA;
                al.type        = ir::IrType::I8;
                al.dst         = addr;
                al.imm         = (uint64_t)arr_n * elem_sz;
                al.source_line = vd->loc.line;
                fn_->append(current_block_, std::move(al));
            }
            // STORE byte-a-byte del string.
            for (uint32_t i = 0; i < str_n; ++i) {
                ir::IrValueId v_val = emit_const(ir_elem,
                                                 (uint64_t)(uint8_t)bytes[i],
                                                 vd->loc.line);
                ir::IrValueId v_addr_i = addr;
                if (i > 0) {
                    ir::IrValueId v_off = emit_const(ir::IrType::I64,
                                                     (uint64_t)i * elem_sz,
                                                     vd->loc.line);
                    v_addr_i = fn_->new_value(ir::IrType::PTR);
                    ir::IrInstr ad{};
                    ad.op          = ir::IrOp::ADD;
                    ad.type        = ir::IrType::I64;
                    ad.dst         = v_addr_i;
                    ad.operands    = {addr, v_off};
                    ad.source_line = vd->loc.line;
                    fn_->append(current_block_, std::move(ad));
                }
                ir::IrInstr st{};
                st.op          = ir::IrOp::STORE;
                st.type        = ir_elem;
                st.operands    = {v_val, v_addr_i};
                st.source_line = vd->loc.line;
                fn_->append(current_block_, std::move(st));
            }
            // Zerificar el resto (semantica C: padding a cero).
            for (uint32_t i = str_n; i < arr_n; ++i) {
                ir::IrValueId v_zero = emit_const(ir_elem, 0, vd->loc.line);
                ir::IrValueId v_off  = emit_const(ir::IrType::I64,
                                                   (uint64_t)i * elem_sz,
                                                   vd->loc.line);
                ir::IrValueId v_addr_i = fn_->new_value(ir::IrType::PTR);
                ir::IrInstr   ad{};
                ad.op          = ir::IrOp::ADD;
                ad.type        = ir::IrType::I64;
                ad.dst         = v_addr_i;
                ad.operands    = {addr, v_off};
                ad.source_line = vd->loc.line;
                fn_->append(current_block_, std::move(ad));
                ir::IrInstr st{};
                st.op          = ir::IrOp::STORE;
                st.type        = ir_elem;
                st.operands    = {v_zero, v_addr_i};
                st.source_line = vd->loc.line;
                fn_->append(current_block_, std::move(st));
            }
            bind(vd->name, addr);
            return;
        }

        // Array init C-style: `i32 arr[N] = {e0, e1, ...};`.
        // Detectamos InitListExpr en el inicializador y emitimos:
        //   ALLOCA del array (igual que sin init).
        //   Por cada elemento: STORE val a (base + i * sizeof(T)).
        //   bind nombre al PTR base.
        // Solo positional (sin .field=); reportamos error si is_designated.
        if (sem_type.kind == PrimitiveKind::ARRAY && vd->init
            && vd->init->kind == ast::NodeKind::InitListExpr) {
            auto *il = static_cast<ast::InitListExpr *>(vd->init.get());
            if (il->is_designated) {
                error_at(vd->loc,
                         "lowering: init designado '.field=' no aplica a arrays");
                return;
            }
            const Type     elem_t  = sem_type.pointee ? *sem_type.pointee : Type{};
            const uint32_t elem_sz =
                    (uint32_t) primitive_size_bytes(elem_t.kind);
            if (elem_sz == 0) {
                error_at(vd->loc, "lowering: tipo del elemento sin sizeof");
                return;
            }
            const uint32_t arr_size = sem_type.array_size > 0
                                          ? (uint32_t) sem_type.array_size
                                          : (uint32_t) il->elements.size();
            if (il->elements.size() > arr_size) {
                error_at(vd->loc,
                         "lowering: init list mas elementos que el array");
                return;
            }
            // ALLOCA arr_size * elem_sz bytes.
            ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr   al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8;
            al.dst         = addr;
            al.imm         = (uint64_t) arr_size * elem_sz;
            al.source_line = vd->loc.line;
            fn_->append(current_block_, std::move(al));
            // STORE de cada elemento.
            const ir::IrType ir_elem = ir_type_from_primitive(elem_t.kind);
            for (size_t i = 0; i < il->elements.size(); ++i) {
                ir::IrValueId v_val = lower_expr(il->elements[i].get());
                if (v_val == ir::IR_NO_VALUE) continue;
                // Suprimir warning de narrowing si el elemento es literal
                // (`{10, 20, ...}` con i64-defaulted literals encajando en
                // el tipo de elemento).  Mismo razonamiento que en
                // var-decl con init literal.
                const bool elem_is_literal =
                        il->elements[i]->kind == ast::NodeKind::IntLitExpr
                     || il->elements[i]->kind == ast::NodeKind::FloatLitExpr
                     || il->elements[i]->kind == ast::NodeKind::BoolLitExpr
                     || il->elements[i]->kind == ast::NodeKind::CharLitExpr
                     || il->elements[i]->kind == ast::NodeKind::NullLitExpr;
                v_val = cast_if_needed(v_val,
                                       fn_->values[v_val].type, ir_elem, vd->loc.line,
                                       /*is_explicit=*/elem_is_literal);
                ir::IrValueId v_addr_i = addr;
                if (i > 0) {
                    ir::IrValueId v_off = emit_const(ir::IrType::I64,
                                                     (uint64_t) (i * elem_sz), vd->loc.line);
                    v_addr_i = fn_->new_value(ir::IrType::PTR);
                    ir::IrInstr ad{};
                    ad.op          = ir::IrOp::ADD;
                    ad.type        = ir::IrType::I64;
                    ad.dst         = v_addr_i;
                    ad.operands    = {addr, v_off};
                    ad.source_line = vd->loc.line;
                    fn_->append(current_block_, std::move(ad));
                }
                ir::IrInstr st{};
                st.op          = ir::IrOp::STORE;
                st.type        = ir_elem;
                st.dst         = ir::IR_NO_VALUE;
                st.operands    = {v_val, v_addr_i};
                st.source_line = vd->loc.line;
                fn_->append(current_block_, std::move(st));
            }
            bind(vd->name, addr);
            return;
        }

        // Struct init C-style: `Point p = {.x = 1, .y = 2};`
        // o `Point p = {1, 2};` (positional).  ALLOCA del struct + STORE
        // de cada campo en su offset.
        if (sem_type.kind == PrimitiveKind::STRUCT && vd->init
            && vd->init->kind == ast::NodeKind::InitListExpr) {
            auto *      il      = static_cast<ast::InitListExpr *>(vd->init.get());
            const auto &layouts = tc_.struct_layouts();
            auto        it_l    = layouts.find(sem_type.struct_name);
            if (it_l == layouts.end()) {
                error_at(vd->loc,
                         "lowering: struct '" + sem_type.struct_name + "' sin layout");
                return;
            }
            const StructLayout &lay = it_l->second;
            // ALLOCA del struct.
            ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr   al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8;
            al.dst         = addr;
            al.imm         = (uint64_t) lay.size_bytes;
            al.source_line = vd->loc.line;
            fn_->append(current_block_, std::move(al));
            // STORE cada elemento al campo correspondiente.
            for (size_t i = 0; i < il->elements.size(); ++i) {
                const StructFieldInfo *fi = nullptr;
                if (il->is_designated) {
                    const std::string &fname = il->field_names[i];
                    for (const auto &f: lay.fields) {
                        if (f.name == fname) {
                            fi = &f;
                            break;
                        }
                    }
                    if (!fi) {
                        error_at(vd->loc,
                                 "lowering: campo '" + fname + "' no existe en struct '"
                                 + sem_type.struct_name + "'");
                        continue;
                    }
                } else {
                    if (i >= lay.fields.size()) {
                        error_at(vd->loc, "lowering: init list excede campos del struct");
                        break;
                    }
                    fi = &lay.fields[i];
                }
                ir::IrValueId v_val = lower_expr(il->elements[i].get());
                if (v_val == ir::IR_NO_VALUE) continue;
                const ir::IrType ir_ft = ir_type_from_primitive(fi->type.kind);
                const bool elem_is_literal =
                        il->elements[i]->kind == ast::NodeKind::IntLitExpr
                     || il->elements[i]->kind == ast::NodeKind::FloatLitExpr
                     || il->elements[i]->kind == ast::NodeKind::BoolLitExpr
                     || il->elements[i]->kind == ast::NodeKind::CharLitExpr
                     || il->elements[i]->kind == ast::NodeKind::NullLitExpr;
                v_val = cast_if_needed(v_val,
                                       fn_->values[v_val].type, ir_ft, vd->loc.line,
                                       /*is_explicit=*/elem_is_literal);
                ir::IrValueId v_addr = addr;
                if (fi->offset > 0) {
                    ir::IrValueId v_off = emit_const(ir::IrType::I64,
                                                     (uint64_t) fi->offset, vd->loc.line);
                    v_addr = fn_->new_value(ir::IrType::PTR);
                    ir::IrInstr ad{};
                    ad.op          = ir::IrOp::ADD;
                    ad.type        = ir::IrType::I64;
                    ad.dst         = v_addr;
                    ad.operands    = {addr, v_off};
                    ad.source_line = vd->loc.line;
                    fn_->append(current_block_, std::move(ad));
                }
                // Bit field: necesitaria read-modify-write; en init list
                // simple solo se admiten campos normales.  Reportar error
                // si fi->bit_width > 0 (uso raro: usar asignacion despues).
                if (fi->bit_width > 0) {
                    error_at(vd->loc,
                             "lowering: init list no soporta bit fields aun");
                    continue;
                }
                ir::IrInstr st{};
                st.op          = ir::IrOp::STORE;
                st.type        = ir_ft;
                st.dst         = ir::IR_NO_VALUE;
                st.operands    = {v_val, v_addr};
                st.source_line = vd->loc.line;
                fn_->append(current_block_, std::move(st));
            }
            bind(vd->name, addr);
            return;
        }

        // Array nativo T[N]: identico a struct desde la optica del lowering.
        // Reservamos N*sizeof(T) bytes en stack y guardamos la direccion base
        // como "valor" de la variable.  Los accesos arr[i] se desugan a
        // ADD(addr, i*sizeof(T)) + LOAD/STORE igual que para T*; el tipo del
        // pointee se obtiene del propio sem_type para escalar el offset.
        if (sem_type.kind == PrimitiveKind::ARRAY) {
            // bug4: array dinamico `T[]` con init `new T[N]` o assigned
            // desde otro host_ptr.  El slot guarda el host_ptr al buffer
            // alocado en heap.  Cuando array_size == 0 y hay init, bindeo
            // el local al SSA value del init (host_ptr) sin ALLOCA stack.
            if (!sem_type.pointee || sem_type.array_size == 0) {
                if (vd->init) {
                    const ir::IrValueId v_init = lower_expr(vd->init.get());
                    if (v_init != ir::IR_NO_VALUE) {
                        // Mark is_host_ptr para que LOAD/STORE indirectos
                        // emitan movh.  El IR del new T[N] ya lo marca.
                        bind(vd->name, v_init);
                        return;
                    }
                }
                error_at(vd->loc,
                         "lowering: array sin tamano fijo requiere init con `new T[N]`");
                return;
            }
            const size_t bytes = size_of_type(sem_type);
            if (bytes == 0) {
                error_at(vd->loc,
                         "lowering: tamano del array es 0 (tipo de elemento desconocido?)");
                return;
            }
            const ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr         ins{};
            ins.op          = ir::IrOp::ALLOCA;
            ins.type        = ir::IrType::I8;
            ins.dst         = addr;
            ins.imm         = (uint64_t) bytes;
            ins.source_line = vd->loc.line;
            fn_->append(current_block_, std::move(ins));
            bind(vd->name, addr);
            if (vd->init) {
                error_at(vd->loc,
                         "lowering: inicializador de array aun no soportado en esta ruta");
            }
            return;
        }

        // Caso 2: tipos primitivos / PTR (camino tradicional).
        ir::IrType vt = ir::IrType::I64;
        if (vd->type && vd->type->kind == ast::NodeKind::PrimitiveTypeNode) {
            auto *pt = static_cast<ast::PrimitiveTypeNode *>(vd->type.get());
            vt       = ir_type_from_primitive(pt->prim);
        } else if (sem_type.kind != PrimitiveKind::COUNT
            && sem_type.kind != PrimitiveKind::VOID) {
            // Alias resuelto a primitivo / PTR.
            vt = ir_type_from_primitive(sem_type.kind);
        }

        // variable address-taken (&x aparece en algun sitio del body).
        // Reservamos memoria local con ALLOCA y emitimos un STORE inicial
        // si hay inicializador.  El scope guarda la DIRECCION (no el valor),
        // y read_local/write_local hacen LOAD/STORE para todos los usos.
        if (address_taken_locals_.count(vd->name)) {
            const size_t        bytes = ir_type_size(vt); // tamano del tipo escalar
            const ir::IrValueId addr  = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr         ai{};
            ai.op          = ir::IrOp::ALLOCA;
            ai.type        = ir::IrType::I8; // unidad: 1 byte
            ai.dst         = addr;
            ai.imm         = (uint64_t) bytes;
            ai.source_line = vd->loc.line;
            fn_->append(current_block_, std::move(ai));
            bind(vd->name, addr);

            // Store del valor inicial (o 0 si no hay init).
            ir::IrValueId v0 = ir::IR_NO_VALUE;
            if (vd->init) {
                v0 = lower_expr(vd->init.get());
                if (v0 != ir::IR_NO_VALUE) {
                    const ir::IrType vfrom = fn_->values[v0].type;
                    // Suprimir el warning de cast implicito cuando el
                    // init es un literal: `u8 init = 0` no merece
                    // alarma porque el valor es estatico y conocido en
                    // compile-time; es un patron habitual y el type
                    // checker ya valida el rango.
                    const bool init_is_literal =
                            vd->init->kind == ast::NodeKind::IntLitExpr
                         || vd->init->kind == ast::NodeKind::FloatLitExpr
                         || vd->init->kind == ast::NodeKind::BoolLitExpr
                         || vd->init->kind == ast::NodeKind::CharLitExpr
                         || vd->init->kind == ast::NodeKind::NullLitExpr;
                    v0 = cast_if_needed(v0, vfrom, vt, vd->loc.line,
                                        /*is_explicit=*/init_is_literal);
                }
            }
            if (v0 == ir::IR_NO_VALUE) v0 = emit_const(vt, 0, vd->loc.line);

            ir::IrInstr st{};
            st.op          = ir::IrOp::STORE;
            st.type        = vt;
            st.dst         = ir::IR_NO_VALUE;
            st.operands    = {v0, addr};
            st.source_line = vd->loc.line;
            fn_->append(current_block_, std::move(st));
            return;
        }

        ir::IrValueId v = ir::IR_NO_VALUE;
        if (vd->init) {
            // ----- Smart pointer move: unique/shared = move(p) -----
            // Patron especial: si el tipo destino es unique<T>/shared<T>
            // y el init es CallExpr(IdentExpr("move"), [p]), transferimos
            // ownership via mvtake (1 instr VM: copia + zero source).
            //
            // Lowering:
            //   1. lower p -> v_src_slot (SSA value que es la direccion
            //                            del slot stack del origen).
            //   2. ALLOCA 8 bytes -> v_dst_slot.
            //   3. Emit `mvtake [dst], [src]` via RAW_ASM.
            //   4. Marcar pointee_is_host_ptr en v_dst_slot.
            //
            // El cleanup del origen (registrado al declarar p) seguira
            // ejecutandose al exit del scope; vera 0 en el slot (zerificado
            // por mvtake) y RAW_FREE(0) sera no-op limpio.
            if ((sem_type.kind == PrimitiveKind::UNIQUE_PTR
              || sem_type.kind == PrimitiveKind::SHARED_PTR)
                && vd->init->kind == ast::NodeKind::CallExpr) {
                auto *ce = static_cast<ast::CallExpr *>(vd->init.get());
                if (ce->callee
                    && ce->callee->kind == ast::NodeKind::IdentExpr
                    && ce->args.size() == 1) {
                    auto *cid = static_cast<ast::IdentExpr *>(ce->callee.get());
                    if (cid->name == "move") {
                        const ir::IrValueId v_src = lower_expr(ce->args[0].get());
                        if (v_src != ir::IR_NO_VALUE) {
                            // unique<T> Tier 1: slot = 16 bytes (ptr + deleter).
                            // shared<T>: slot = 8 bytes (ctrl_block_ptr).
                            const uint32_t slot_bytes =
                                (sem_type.kind == PrimitiveKind::UNIQUE_PTR) ? 16 : 8;
                            // ALLOCA para el slot destino.
                            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::PTR);
                            {
                                ir::IrInstr al{};
                                al.op          = ir::IrOp::ALLOCA;
                                al.type        = ir::IrType::I8;
                                al.dst         = v_dst;
                                al.imm         = slot_bytes;
                                al.source_line = vd->loc.line;
                                fn_->append(current_block_, std::move(al));
                            }
                            // Emit mvtake [v_dst+0], [v_src+0] (ptr).
                            // Para unique<T> tambien emit mvtake [v_dst+8], [v_src+8] (deleter).
                            emit_mvtake(v_dst, v_src, vd->loc.line);
                            if (slot_bytes == 16) {
                                // Segundo qword: deleter.  Calculamos los dos
                                // punteros +8 y emitimos otro mvtake.
                                const ir::IrValueId v_eight  = emit_const(ir::IrType::I64, 8, vd->loc.line);
                                const ir::IrValueId v_dst8   = fn_->new_value(ir::IrType::PTR);
                                const ir::IrValueId v_src8   = fn_->new_value(ir::IrType::PTR);
                                {
                                    ir::IrInstr add{};
                                    add.op          = ir::IrOp::ADD;
                                    add.type        = ir::IrType::I64;
                                    add.dst         = v_dst8;
                                    add.operands    = {v_dst, v_eight};
                                    add.source_line = vd->loc.line;
                                    fn_->append(current_block_, std::move(add));
                                }
                                {
                                    ir::IrInstr add{};
                                    add.op          = ir::IrOp::ADD;
                                    add.type        = ir::IrType::I64;
                                    add.dst         = v_src8;
                                    add.operands    = {v_src, v_eight};
                                    add.source_line = vd->loc.line;
                                    fn_->append(current_block_, std::move(add));
                                }
                                emit_mvtake(v_dst8, v_src8, vd->loc.line);
                            }
                            fn_->values[v_dst].pointee_is_host_ptr = true;
                            v = v_dst;
                            goto bind_and_cleanup;
                        }
                    }
                }
            }
            // Lazy promotion: si el tipo destino es STRING y el
            // init es un string literal puro (StringLitExpr), promover
            // a StringObject GC-managed via STRMAKE.  Asi `string s =
            // "hola"` aloca 1 vez; `print("hola")` (sin var-decl) sigue
            // sin alocar.
            if (sem_type.kind == PrimitiveKind::STRING
                && vd->init
                && vd->init->kind == ast::NodeKind::StringLitExpr) {
                // Tanto literales puros como interpolados se promueven
                // a StringObject GC-managed; el helper detecta el caso
                // y emite STRMAKE simple o cadena de STRMAKE+STRCAT
                // segun corresponda.
                auto *slit = static_cast<ast::StringLitExpr *>(vd->init.get());
                v          = lower_string_literal_to_string_object(slit);
                bind(vd->name, v);
                return;
            }
            v = lower_expr(vd->init.get());
            if (v != ir::IR_NO_VALUE) {
                const ir::IrType vfrom = fn_->values[v].type;
                // Misma supresion de warning que en la rama
                // address-taken: literales no merecen alarma de
                // narrowing porque el valor es compile-time conocido.
                const bool init_is_literal =
                        vd->init->kind == ast::NodeKind::IntLitExpr
                     || vd->init->kind == ast::NodeKind::FloatLitExpr
                     || vd->init->kind == ast::NodeKind::BoolLitExpr
                     || vd->init->kind == ast::NodeKind::CharLitExpr
                     || vd->init->kind == ast::NodeKind::NullLitExpr;
                v = cast_if_needed(v, vfrom, vt, vd->loc.line,
                                   /*is_explicit=*/init_is_literal);
            }
        } else {
            // Sin init: defecto 0.  Las variables sin init son raras
            // en uso normal pero el type checker no las prohibe.
            v = emit_const(vt, 0, vd->loc.line);
        }
    bind_and_cleanup:
        bind(vd->name, v);

        // auto-free de colecciones primitivas.  Si el tipo del var
        // es uno de los tipos coleccion (ARRAYLIST, HASHMAP, etc), registrar
        // un cleanup en cleanup_stack_ que llame al free fn correspondiente
        // del plugin nativo al exit del scope/funcion.  El cleanup se emite
        // como RAW_ASM (consistente con synchronized) que prepara R1=handle,
        // R15=1, y emite calln al @Method del free.  Cero overhead en el
        // hot path (solo se emite al exit; CALL clean exits sin frame).
        //
        // Limitacion: si el handle se devuelve (return xs) o se asigna a
        // otra variable que vive mas, el free aqui dejaria al caller con
        // un handle invalido.  El escape analysis basico marca esos
        // locales en @c escaping_locals_ y omite el cleanup para ellos;
        // los locales realmente locales si reciben el free automatico.

        // Destructor automatico (RAII) para instancias locales de
        // clase Vex que tienen `~ClassName()` declarado y NO escapan.
        // Emite CALLVIRT al destructor al exit del scope/funcion via
        // cleanup_stack_, mismo mecanismo que el auto-free de colecciones.
        if (v != ir::IR_NO_VALUE
            && sem_type.kind == PrimitiveKind::CLASS
            && escaping_locals_.find(vd->name) == escaping_locals_.end()) {
            const auto &class_layouts = tc_.class_layouts();
            auto        it_cls        = class_layouts.find(sem_type.struct_name);
            if (it_cls != class_layouts.end()) {
                const ClassLayout &    lay  = it_cls->second;
                const ClassMethodInfo *dtor = nullptr;
                for (const auto &mi: lay.methods) {
                    if (mi.is_destructor) {
                        dtor = &mi;
                        break;
                    }
                }
                if (dtor) {
                    // cleanup CALL_DTOR: el regalloc ve un CALLVIRT
                    // real y preserva los regs vivos del scope (incluido el
                    // reg de v_ret en lower_return).  refresh_name garantiza
                    // que el cleanup vea el binding actual del local si fue
                    // reasignado tras el var-decl.
                    CleanupAction act;
                    act.kind              = CleanupAction::Kind::CALL_DTOR;
                    act.operands          = {v};
                    act.source_line       = vd->loc.line;
                    act.refresh_name      = vd->name;
                    act.dtor_vtable_index = dtor->vtable_index;
                    cleanup_stack_.push_back(std::move(act));
                }
                // fix9 - eliminado el cleanup RAW_ASM `gchandle+drop`
                // para CLASS sin destructor (era el fix).  Ya no
                // necesario tras fix8 (GC stack scanning conservativo
                // con interior scan en OldGen): los handles que no aparecen
                // en stack/regs/external_refs son barridos automaticamente
                // por el major_gc.  Las restricciones que el fix antiguo
                // imponia (scopes_.size()<=2, !current_fn_has_try_) ya no
                // aplican.
            }
        }

        // fix9 - eliminado el cleanup RAW_ASM para `i64 obj =
        // newInstance(cls)` (era el fix2).  Mismo razonamiento que
        // el caso CLASS sin destructor: el GC stack scanning fix8
        // colecta automaticamente cualquier handle que no aparezca en
        // stack/regs vivos, sin importar si el var-decl es CLASS o I64
        // ni si la funcion tiene try/catch.

        // BugFix R9: SOLO registrar cleanup si el init es directamente un
        // constructor de coleccion (`arraylist(n)`, `hashmap(n)`, etc.).
        // Otras formas (cast, asignacion de otra var, return de func)
        // son ALIAS del mismo handle -> el owner original ya tiene cleanup;
        // duplicarlo causa double-free al exit.  Ejemplo: `ArrayList l1 =
        // (ArrayList)groups.get(1)` crea un alias del handle ya owned por
        // list1; sin este check, l1 se libera al exit Y list1 tambien
        // -> corrupcion del heap (exit 127).
        bool init_is_col_ctor = false;
        if (vd->init && vd->init->kind == ast::NodeKind::CallExpr) {
            auto *ce = static_cast<ast::CallExpr *>(vd->init.get());
            if (ce->callee && ce->callee->kind == ast::NodeKind::IdentExpr) {
                auto *id = static_cast<ast::IdentExpr *>(ce->callee.get());
                if (find_col_ctor(id->name) != nullptr) {
                    init_is_col_ctor = true;
                }
            }
        }
        if (v != ir::IR_NO_VALUE && is_col_kind(sem_type.kind)
            && init_is_col_ctor
            && escaping_locals_.find(vd->name) == escaping_locals_.end()) {
            // solo registramos cleanup si el local NO escapa
            // (ni return ni asignacion a campo/slot/deref).  Si escapa,
            // el caller toma posesion del handle y lo libera.
            const ColType *ct = find_col_type(sem_type.kind);
            if (ct) {
                // elegir variante *_free_gc cuando la coleccion
                // retiene refs GC (e.g. ArrayList<string>).  El frontend
                // setea pointee/pointee2 en sem_type al resolver el tipo
                // declarado; col_needs_gc_aware decide.
                PrimitiveKind elem_k = PrimitiveKind::VOID;
                PrimitiveKind val_k  = PrimitiveKind::VOID;
                if (sem_type.pointee) elem_k = sem_type.pointee->kind;
                if (sem_type.pointee2) val_k = sem_type.pointee2->kind;
                const bool gc_aware = (ct->native_free_fn_gc != nullptr)
                        && col_needs_gc_aware(sem_type.kind, elem_k, val_k);
                const char *fn_name = gc_aware ? ct->native_free_fn_gc : ct->native_free_fn;
                out_mod_->register_native_import(COL_NATIVE_LIB, fn_name);
                CleanupAction act;
                act.kind         = CleanupAction::Kind::CALLN_FREE;
                act.operands     = {v};
                act.source_line  = vd->loc.line;
                act.refresh_name = vd->name;
                act.func_name    = std::string(COL_NATIVE_LIB) + ":" + fn_name;
                act.needs_proc   = gc_aware;
                cleanup_stack_.push_back(std::move(act));
            }
        }

        // ---- Smart pointers: registrar cleanup automatico al scope exit ----
        //
        // Para @c unique<T>: SMARTPTR_FREE con literal_deleter="free" (default
        // Tier 0) o nombre de funcion deleter custom (set por unique_with).
        // Para @c shared<T>: SHAREDPTR_REL (refcount--; GC libera).
        //
        // Solo se registra si el local NO escapa (escaping_locals_).  Si
        // escapa, el caller toma posesion (return) o lo guarda
        // (asignacion a field/slot/deref), por lo que NO se debe liberar
        // aqui.
        if (v != ir::IR_NO_VALUE
            && (sem_type.kind == PrimitiveKind::UNIQUE_PTR
             || sem_type.kind == PrimitiveKind::SHARED_PTR)
            && escaping_locals_.find(vd->name) == escaping_locals_.end()) {
            CleanupAction act;
            act.operands        = {v};
            act.source_line     = vd->loc.line;
            act.refresh_name    = vd->name;
            if (sem_type.kind == PrimitiveKind::UNIQUE_PTR) {
                act.kind            = CleanupAction::Kind::SMARTPTR_FREE;
                // Decision del literal_deleter (cleanup mas eficiente
                // posible segun la info compile-time disponible):
                //
                //   pending_smartptr_deleter_ no vacio
                //     -> init fue unique_with(_, deleter) -> usar ese deleter.
                //
                //   init es CallExpr (factory que devuelve unique<T>)
                //     -> dejar literal_deleter vacio -> dispatch dinamico
                //        via slot+8 al runtime (lee deleter del slot).
                //
                //   otro (init es unique_box, IdentExpr, etc.)
                //     -> usar "free" (Tier 1 con sentinel; el slot[+8]=0).
                if (!pending_smartptr_deleter_.empty()) {
                    act.literal_deleter = pending_smartptr_deleter_;
                } else if (vd->init
                        && vd->init->kind == ast::NodeKind::CallExpr) {
                    auto *ce = static_cast<ast::CallExpr *>(vd->init.get());
                    bool is_factory_call = false;
                    if (ce->callee && ce->callee->kind == ast::NodeKind::IdentExpr) {
                        auto *cid = static_cast<ast::IdentExpr *>(ce->callee.get());
                        // Si el callee no es un builtin de smart pointer
                        // (unique_box/unique_with/move/...), asumimos
                        // factory de usuario y usamos dispatch dinamico.
                        const std::string &n = cid->name;
                        is_factory_call = (n != "unique_box" && n != "unique_with"
                                        && n != "shared_box" && n != "shared_with"
                                        && n != "move");
                    }
                    if (is_factory_call) {
                        act.literal_deleter = "";  // dispatch dinamico
                    } else {
                        act.literal_deleter = "free";
                    }
                } else {
                    act.literal_deleter = "free";  // Tier 1 con sentinel
                }
                act.slot_size       = 16;  // Tier 1

                // Bug fix bug2: si el inner T es una CLASS Vex con destructor,
                // registrar el vtable_index para que el cleanup invoque
                // `~T()` sobre el objeto contenido ANTES de liberar el slot.
                // Sin esto, `unique_box(new Recurso(1))` perdia el destructor
                // al exit del scope -- el slot se RAW_FREE'aba pero el
                // Recurso quedaba huerfano (eventual GC pero sin ~Recurso).
                if (sem_type.pointee
                 && sem_type.pointee->kind == PrimitiveKind::CLASS) {
                    const auto &cls_layouts = tc_.class_layouts();
                    auto it_cls = cls_layouts.find(sem_type.pointee->struct_name);
                    if (it_cls != cls_layouts.end()) {
                        // Marcar siempre como inner GC class para que el
                        // cleanup NO haga RAW_FREE del host_ptr (que es un
                        // host_ptr a un objeto GC, no a memoria RAW_ALLOC).
                        act.inner_is_gc_class = true;
                        for (const auto &mi: it_cls->second.methods) {
                            if (mi.is_destructor) {
                                act.inner_dtor_vtable_index = mi.vtable_index;
                                break;
                            }
                        }
                    }
                }
            } else {
                act.kind            = CleanupAction::Kind::SHAREDPTR_REL;
                act.slot_size       = 8;
            }
            cleanup_stack_.push_back(std::move(act));
        }
        // Limpiar pending_smartptr_deleter_ tras consumirlo (o si el
        // var-decl no era smart pointer pero hubo un unique_with previo
        // sin var-decl asociado, evitar contaminacion del siguiente).
        pending_smartptr_deleter_.clear();
    }

    void Lowering::lower_if(ast::IfStmt *s) {
        // Sprint 3-B: `comptime if` -- dead-branch elimination.
        // El type checker ya valido que `cond` es comptime-evaluable y
        // ya descarto la rama no tomada del check.  Aqui simplemente
        // bajamos al SIN if/branch/phi.  Cero overhead vs codigo
        // hardcoded: el bytecode emitido es identico al de la rama
        // tomada sin marcador alguno de la condicion.
        if (s->is_comptime && s->cond) {
            const ComptimeEvalResult r = comptime_eval_expr(tc_, s->cond.get());
            if (r.ok) {
                if (r.value != 0) {
                    if (s->then_branch) lower_stmt(s->then_branch.get());
                } else {
                    if (s->else_branch) lower_stmt(s->else_branch.get());
                }
                return;
            }
            /* Si por algun motivo la evaluacion falla aqui (no deberia,
             * el type checker ya valido), caemos al lowering normal --
             * mas vale conservador que crash. */
        }

        // Patron CFG: cond -> br_cond %c, then, else; cada rama termina
        // con br merge (si no aborto antes en otro terminador).
        //
        // SSA construction (Braun on-the-fly): si una variable es asignada
        // en al menos una rama, en el merge insertamos un PHI con un arg
        // por cada predecesor del merge.  Sin esto, el binding del scope
        // tras el if seria el del UlTIMO branch ejecutado por el lowering
        // (no-determinista entre runs distintos del compilador y, mas
        // importante, INCORRECTO en runtime ya que el regalloc puede
        // poner la variable en registros distintos en cada rama).
        //
        // Algoritmo:
        //   1. Snapshot completo de los bindings ANTES del if (entry_bindings).
        //   2. Tras lower del then -> snapshot then_bindings y restaurar entry.
        //   3. Tras lower del else (si existe) -> snapshot else_bindings.
        //      Si no hay else, else_bindings = entry_bindings.
        //   4. En el merge: por cada nombre cuyo binding difiere entre
        //      then_bindings y else_bindings (o difiere de entry), emitir
        //      un PHI con args [(then_val, then_pred), (else_val, else_pred)]
        //      y rebindear el nombre al PHI.
        //
        // Solo aplicamos esto a variables del scope ENCLOSING (no a
        // declaradas dentro de las propias ramas; esas mueren al pop_scope
        // implicito del block).
        //
        // Casos especiales:
        //   - Si then o else terminan abruptamente (return/break/throw),
        //     ese predecesor no llega al merge y no contribuye al PHI.
        //   - Si AMBAS ramas terminan, no hay merge alcanzable; el codigo
        //     post-if es muerto.  Pero el lowering aun lo procesa.
        const ir::IrValueId cond = lower_expr(s->cond.get());
        // Si el tipo no es BOOL ya, el optimizador / emisor lo trataran
        // como "non-zero is true".  Para mas claridad podriamos insertar
        // un cmp_ne con 0 explicito, pero el bytecode @c jmp.jne ya hace
        // exactamente esa comparacion contra 0 sin instruccion adicional,
        // asi que delegar al backend es la opcion mas eficiente.

        const ir::IrBlockId then_bb  = fn_->new_block("if_then");
        const bool          has_else = s->else_branch != nullptr;
        const ir::IrBlockId else_bb  = has_else ? fn_->new_block("if_else") : ir::IR_NO_BLOCK;
        const ir::IrBlockId merge_bb = fn_->new_block("if_merge");

        // Snapshot de los bindings activos antes de empezar las ramas.
        // Lo usamos despues para detectar variables modificadas en cada
        // rama y para restaurar el entry antes de bajar la rama else.
        std::vector<std::unordered_map<std::string, ir::IrValueId> > entry_scopes
                = scopes_;

        // br.cond cond, then_bb, (else_bb o merge_bb si no hay else)
        ir::IrInstr br{};
        br.op = ir::IrOp::BR_COND;
        br.operands.push_back(cond);
        br.target_block = then_bb;
        br.false_block  = has_else ? else_bb : merge_bb;
        br.source_line  = s->loc.line;
        fn_->append(current_block_, std::move(br));
        // Mantener la CFG explicita para validacion del IR.
        fn_->blocks[current_block_].succs.push_back(then_bb);
        fn_->blocks[current_block_].succs.push_back(has_else ? else_bb : merge_bb);
        fn_->blocks[then_bb].preds.push_back(current_block_);
        if (has_else) fn_->blocks[else_bb].preds.push_back(current_block_);
        else fn_->blocks[merge_bb].preds.push_back(current_block_);

        // Rama then.
        current_block_    = then_bb;
        block_terminated_ = false;
        lower_stmt(s->then_branch.get());
        // Snapshot de los bindings tras el then; bloque actual al final
        // del then (si no termino).
        std::vector<std::unordered_map<std::string, ir::IrValueId> > then_scopes
                = scopes_;
        ir::IrBlockId then_pred          = current_block_;
        const bool    then_falls_through = !block_terminated_;
        if (then_falls_through) {
            // br merge_bb
            ir::IrInstr brm{};
            brm.op           = ir::IrOp::BR;
            brm.target_block = merge_bb;
            brm.source_line  = s->loc.line;
            fn_->append(current_block_, std::move(brm));
            fn_->blocks[current_block_].succs.push_back(merge_bb);
            fn_->blocks[merge_bb].preds.push_back(current_block_);
            block_terminated_ = true;
        }

        // Restaurar bindings antes de bajar la rama else (los bindings de
        // then no deben "filtrarse" al else; cada rama parte del entry).
        scopes_ = entry_scopes;

        // Rama else (si existe).
        ir::IrBlockId                                                else_pred          = ir::IR_NO_BLOCK;
        bool                                                         else_falls_through = false;
        std::vector<std::unordered_map<std::string, ir::IrValueId> > else_scopes;
        if (has_else) {
            current_block_    = else_bb;
            block_terminated_ = false;
            lower_stmt(s->else_branch.get());
            else_scopes        = scopes_;
            else_pred          = current_block_;
            else_falls_through = !block_terminated_;
            if (else_falls_through) {
                ir::IrInstr brm{};
                brm.op           = ir::IrOp::BR;
                brm.target_block = merge_bb;
                brm.source_line  = s->loc.line;
                fn_->append(current_block_, std::move(brm));
                fn_->blocks[current_block_].succs.push_back(merge_bb);
                fn_->blocks[merge_bb].preds.push_back(current_block_);
                block_terminated_ = true;
            }
        } else {
            // Sin else: el "branch else" es el propio entry, que cae
            // directo al merge sin pasar por else_bb.  Su pred del merge
            // es el bloque del que venia el if (anyadido arriba via
            // fn_->blocks[merge_bb].preds.push_back(current_block_)
            // antes de la rama then).  El else_pred en ese caso es ese
            // pred original (current_block_ ANTES del then).  Recuperamos
            // de la CFG: el primer pred del merge tras llamar a anadir el
            // entry-no-else es exactamente ese.
            // Como simplificacion: dejamos else_scopes = entry_scopes y
            // else_pred = el primer pred del merge (que es current_block_
            // del entry original justo antes del br).  Lo identificamos
            // por exclusion: cualquier pred != then_pred.
            else_scopes = entry_scopes;
            for (auto pid: fn_->blocks[merge_bb].preds) {
                if (pid != then_pred) {
                    else_pred = pid;
                    break;
                }
            }
            else_falls_through = true; // el camino "no-else" siempre llega
        }

        // -------- Insertar PHIs en el merge --------
        // Solo si AMBAS ramas (o then-fall + no-else) llegan al merge.
        // Si una sola rama llega, el binding correcto es el de esa rama
        // (no necesita PHI; la otra es codigo muerto pre-merge).
        current_block_          = merge_bb;
        block_terminated_       = false;
        const bool then_reaches = then_falls_through;
        const bool else_reaches = else_falls_through;
        if (then_reaches && else_reaches) {
            // Recorremos cada nivel de scope (ENTRY = referencia comun).
            // Para cada nombre que existia en entry_scopes, comparamos
            // los bindings finales de then y else.  Si difieren entre si
            // o respecto al entry, emitimos PHI.
            //
            // Notacion: scope_idx = nivel; tomamos como referencia el
            // depth original (entry_scopes.size()).  Si las ramas
            // anyaden scopes nuevos, los ignoramos (variables locales a
            // la rama).
            const size_t depth      = entry_scopes.size();
            const size_t depth_then = then_scopes.size();
            const size_t depth_else = else_scopes.size();
            for (size_t lvl = 0; lvl < depth; ++lvl) {
                if (lvl >= depth_then || lvl >= depth_else) break;
                for (auto &kv: entry_scopes[lvl]) {
                    const std::string &name = kv.first;
                    auto               itt  = then_scopes[lvl].find(name);
                    auto               ite  = else_scopes[lvl].find(name);
                    if (itt == then_scopes[lvl].end()
                        || ite == else_scopes[lvl].end())
                        continue;
                    const ir::IrValueId vt = itt->second;
                    const ir::IrValueId ve = ite->second;
                    // Si ambas ramas dejan el mismo SSA value, no hay
                    // necesidad de PHI: el binding ya es coherente.
                    if (vt == ve) {
                        // Asegurar que el scope merge tiene el valor
                        // correcto (en caso de que entry_scopes lo
                        // tuviera distinto pero ambas ramas coinciden).
                        scopes_[lvl][name] = vt;
                        continue;
                    }
                    // Crear el PHI en el merge.  Usamos el tipo del SSA
                    // del then (deberia ser igual al del else; el type
                    // checker lo garantiza al haber validado las dos
                    // asignaciones contra el tipo declarado).
                    const ir::IrType phi_ty = fn_->values[vt].type;
                    ir::IrValueId    phi_v  = fn_->new_value(phi_ty);
                    ir::IrInstr      phi{};
                    phi.op   = ir::IrOp::PHI;
                    phi.type = phi_ty;
                    phi.dst  = phi_v;
                    phi.phi_args.push_back({vt, then_pred});
                    if (else_pred != ir::IR_NO_BLOCK) {
                        phi.phi_args.push_back({ve, else_pred});
                    }
                    phi.source_line = s->loc.line;
                    // INSERTAR al INICIO del merge_bb (PHIs siempre van al
                    // principio del bloque).  fn_->append solo hace
                    // push_back, asi que insertamos manualmente.
                    fn_->blocks[merge_bb].instrs.insert(
                        fn_->blocks[merge_bb].instrs.begin(),
                        std::move(phi));
                    scopes_[lvl][name] = phi_v;
                }
            }
        } else if (then_reaches) {
            // Solo then llega: usa los bindings de then.
            scopes_ = then_scopes;
        } else if (else_reaches) {
            // Solo else llega: usa los bindings de else.
            scopes_ = else_scopes;
        } else {
            // Ninguna rama llega al merge (ambas hicieron return / break /
            // throw).  El merge es codigo muerto pero el lowering aun lo
            // procesa; mantener entry_scopes evita usos indefinidos.
            scopes_ = entry_scopes;
        }
    }

    // ---------------------------------------------------------------------
    // Lowering de loops via SSA construction on-the-fly (Braun et al.).
    //
    // Patron general para 'while (cond) body':
    //
    //     entry:
    //         ...
    //         br header
    //     header:
    //         x = phi.T [x_pre, entry], [x_loop, body_end]    ; uno por var
    //         cond_v = lower(cond)
    //         br.cond cond_v, body, exit
    //     body:
    //         (lowering del body; las asignaciones cambian scope[x] -> nuevo IrValueId)
    //         br header                                      ; back-edge
    //     exit:
    //         (continuacion del codigo posterior al loop)
    //
    // El paso clave es identificar las variables que se modifican dentro
    // del cond+body y emitir un PHI por cada una en el header.  El primer
    // arg del PHI viene del entry (el valor previo al loop); el segundo
    // arg se anyade al final, una vez bajado el body, con el valor que
    // queda en scope tras la ultima iteracion.
    //
    // Las variables NO modificadas no necesitan PHI: el lookup() las
    // encuentra a traves del scope chain con su valor pre-loop.
    // ---------------------------------------------------------------------

    void Lowering::lower_while(ast::WhileStmt *s) {
        if (!s) return;

        // Pre-walk: variables mutadas en cond+body.
        std::set<std::string> modified;
        collect_assigned_vars(s->cond.get(), modified);
        collect_assigned_vars(s->body.get(), modified);

        // Filtrar: solo nos interesan las que ya existen en el scope antes
        // del loop (variables externas).  Las locales declaradas dentro del
        // body no necesitan PHI.
        struct VarInfo {
            std::string   name;
            ir::IrType    type;
            ir::IrValueId pre_loop;
            ir::IrValueId phi_value;
            size_t        phi_idx; // posicion del PHI dentro de header.instrs
        };
        std::vector<VarInfo> vars;
        vars.reserve(modified.size());
        for (const auto &name: modified) {
            ir::IrValueId pre = lookup(name);
            if (pre == ir::IR_NO_VALUE) continue; // variable local al body, ignorar
            VarInfo vi;
            vi.name     = name;
            vi.type     = fn_->values[pre].type;
            vi.pre_loop = pre;
            vars.push_back(vi);
        }

        // Crear bloques para el patron CFG estandar de while.
        const ir::IrBlockId entry_block = current_block_;
        const ir::IrBlockId header_id   = fn_->new_block("while_header");
        const ir::IrBlockId body_id     = fn_->new_block("while_body");
        const ir::IrBlockId exit_id     = fn_->new_block("while_exit");

        // 1. Entry -> header (BR incondicional).
        {
            ir::IrInstr br{};
            br.op           = ir::IrOp::BR;
            br.target_block = header_id;
            br.source_line  = s->loc.line;
            fn_->append(entry_block, std::move(br));
        }
        fn_->blocks[entry_block].succs.push_back(header_id);
        fn_->blocks[header_id].preds.push_back(entry_block);

        // 2. En el header, emitir un PHI por cada variable mutada.  Solo
        //    se anyade el primer arg (entry); el back-edge se completa
        //    despues de bajar el body.
        for (auto &vi: vars) {
            vi.phi_value = fn_->new_value(vi.type);
            ir::IrInstr phi{};
            phi.op   = ir::IrOp::PHI;
            phi.type = vi.type;
            phi.dst  = vi.phi_value;
            phi.phi_args.push_back({vi.pre_loop, entry_block});
            phi.source_line = s->loc.line;
            fn_->append(header_id, std::move(phi));
            vi.phi_idx = fn_->blocks[header_id].instrs.size() - 1;
            // Dentro del loop, las lecturas de `name` deben ver el valor del PHI.
            update_scope(vi.name, vi.phi_value);
        }

        // 3. Bajar la condicion en el header y emitir BR_COND.  La cond
        //    puede crear bloques intermedios (e.g. short-circuit `&&`/`||`
        //    construye rhs_bb + default_bb + merge_bb).  En ese caso al
        //    volver de @c lower_expr el @c current_block_ NO es header_id
        //    sino el merge_bb del short-circuit.  Emitir el BR_COND en
        //    @c current_block_ y registrar el predecesor real del body/exit
        //    es lo correcto; el header_id queda terminado por el BR_COND
        //    interno del short-circuit.
        current_block_       = header_id;
        block_terminated_    = false;
        ir::IrValueId cond_v = lower_expr(s->cond.get());
        if (cond_v == ir::IR_NO_VALUE) {
            // Defensa: si la condicion fallo en bajar, abortar el loop.
            return;
        }
        const ir::IrBlockId cond_end_block = current_block_; {
            ir::IrInstr brc{};
            brc.op           = ir::IrOp::BR_COND;
            brc.operands     = {cond_v};
            brc.target_block = body_id;
            brc.false_block  = exit_id;
            brc.source_line  = s->loc.line;
            fn_->append(cond_end_block, std::move(brc));
        }
        fn_->blocks[cond_end_block].succs.push_back(body_id);
        fn_->blocks[cond_end_block].succs.push_back(exit_id);
        fn_->blocks[body_id].preds.push_back(cond_end_block);
        fn_->blocks[exit_id].preds.push_back(cond_end_block);
        block_terminated_ = true;

        // 4. Bajar el body en body_id.  Push targets de break/continue
        //    para que cualquier @c BreakStmt o @c ContinueStmt anidado
        //    sepa adonde saltar.
        loop_targets_.push_back({header_id, exit_id, {}, {}});
        current_block_    = body_id;
        block_terminated_ = false;
        lower_stmt(s->body.get());
        // Capturar targets ANTES del pop para usar continue_preds en
        // la fase de completar PHIs.
        LoopTargets lt = std::move(loop_targets_.back());
        loop_targets_.pop_back();

        // Completar PHIs del header con cada `continue` que se hizo
        //     dentro del body.  Cada continue contribuye con un arg al
        //     PHI usando los SSA values del scope al momento del
        //     continue (capturados en lt.continue_scopes).
        for (size_t ci = 0; ci < lt.continue_preds.size(); ++ci) {
            const ir::IrBlockId cpred = lt.continue_preds[ci];
            const auto &        csnap = lt.continue_scopes[ci];
            for (auto &vi: vars) {
                ir::IrValueId v = ir::IR_NO_VALUE;
                // Buscar la var en el scope-snapshot (de mas profundo
                // al mas externo, igual que lookup() haria).
                for (auto it = csnap.rbegin(); it != csnap.rend(); ++it) {
                    auto j = it->find(vi.name);
                    if (j != it->end()) {
                        v = j->second;
                        break;
                    }
                }
                if (v == ir::IR_NO_VALUE) v = vi.phi_value;
                fn_->blocks[header_id].instrs[vi.phi_idx]
                        .phi_args.push_back({v, cpred});
            }
        }

        // Si el body no termino con un return/break, anyadir el back-edge
        //    al header.  Si fue break/continue, el lowering de esos statements
        //    ya emitio BR al target adecuado y marco @c block_terminated_.
        if (!block_terminated_) {
            const ir::IrBlockId body_end_id = current_block_; {
                ir::IrInstr br{};
                br.op           = ir::IrOp::BR;
                br.target_block = header_id;
                br.source_line  = s->loc.line;
                fn_->append(body_end_id, std::move(br));
            }
            fn_->blocks[body_end_id].succs.push_back(header_id);
            fn_->blocks[header_id].preds.push_back(body_end_id);
            block_terminated_ = true;

            // 6. Completar PHIs con el valor que queda en scope tras la
            //    ultima iteracion (back-edge).
            for (auto &vi: vars) {
                ir::IrValueId post = lookup(vi.name);
                if (post == ir::IR_NO_VALUE) post = vi.phi_value;
                fn_->blocks[header_id].instrs[vi.phi_idx]
                        .phi_args.push_back({post, body_end_id});
                // Bug D fix: si algun arg del PHI es is_gc_object (e.g.
                // una asignacion en el body propaga un host_ptr GC al
                // PHI value), el PHI value mismo debe heredar el flag
                // para que save_live_regs de futuros CALLs lo guarde
                // como gchandle (estable a evacuacion del GC).  Sin
                // esto, valores CLASS que entran al loop como NULL
                // (no-GC) y se asignan en iter 1 a un objeto real,
                // tienen sus host_ptrs invalidados en iter 2+.
                if (static_cast<size_t>(post) < fn_->values.size()
                 && fn_->values[post].is_gc_object) {
                    fn_->values[vi.phi_value].is_gc_object = true;
                }
                if (static_cast<size_t>(vi.pre_loop) < fn_->values.size()
                 && fn_->values[vi.pre_loop].is_gc_object) {
                    fn_->values[vi.phi_value].is_gc_object = true;
                }
            }
        } else {
            // Body termina con un return: el back-edge nunca se ejecuta.
            // Completamos el PHI con el propio phi_value (placeholder
            // semanticamente correcto: si nunca se llega, no se observa).
            for (auto &vi: vars) {
                fn_->blocks[header_id].instrs[vi.phi_idx]
                        .phi_args.push_back({vi.phi_value, header_id});
            }
        }

        // 7. Continuar en exit_id.  Las variables modificadas tienen como
        //    valor "vivo" el del PHI: al salir del loop por la condicion
        //    falsa, la ultima escritura observable es la del header.
        // 7.b LANG.fix-7: si hay breaks, el exit_id tiene multiples preds
        //    (cond_end_block + cada break_pred).  Cada break visita el
        //    exit con un snapshot distinto del scope, asi que necesitamos
        //    insertar PHIs al inicio del exit_id para que las variables
        //    modificadas converjan correctamente.  Sin esto, el codigo
        //    despues del while ve el valor del header (vi.phi_value)
        //    incluso cuando el break vino tras modificaciones (e.g.
        //    `while (true) { i = i + 1; if (i >= 5) break; }` -- el
        //    `i` post-loop debe ser 5, no el phi del header que es 4).
        current_block_    = exit_id;
        block_terminated_ = false;
        if (!lt.break_preds.empty()) {
            // Por cada var modificada, crear PHI en el exit_id con args
            // {phi_value @ cond_end_block} + un arg por cada break_pred
            // con el snapshot del scope en ese punto.
            for (auto &vi: vars) {
                ir::IrInstr phi{};
                phi.op  = ir::IrOp::PHI;
                phi.dst = fn_->new_value(vi.type);
                phi.type = vi.type;
                phi.source_line = s->loc.line;
                // Propagar flags importantes del PHI del header.
                if (static_cast<size_t>(vi.phi_value) < fn_->values.size()) {
                    fn_->values[phi.dst].is_gc_object =
                        fn_->values[vi.phi_value].is_gc_object;
                    fn_->values[phi.dst].is_host_ptr =
                        fn_->values[vi.phi_value].is_host_ptr;
                }
                // Edge desde cond_end_block: trae el phi_value del header
                // (visto al evaluar la condicion como falsa).
                phi.phi_args.push_back({vi.phi_value, cond_end_block});
                // Edges desde cada break_pred: usan el snapshot del scope.
                for (size_t bi = 0; bi < lt.break_preds.size(); ++bi) {
                    const ir::IrBlockId bpred = lt.break_preds[bi];
                    const auto &        bsnap = lt.break_scopes[bi];
                    ir::IrValueId v = ir::IR_NO_VALUE;
                    for (auto it = bsnap.rbegin(); it != bsnap.rend(); ++it) {
                        auto j = it->find(vi.name);
                        if (j != it->end()) { v = j->second; break; }
                    }
                    if (v == ir::IR_NO_VALUE) v = vi.phi_value;
                    // Propagar flags GC si el value tiene.
                    if (static_cast<size_t>(v) < fn_->values.size()
                        && fn_->values[v].is_gc_object) {
                        fn_->values[phi.dst].is_gc_object = true;
                    }
                    phi.phi_args.push_back({v, bpred});
                }
                // Insertar el PHI al inicio del exit_id (PHIs van al
                // inicio del bloque por convencion SSA).
                fn_->blocks[exit_id].instrs.insert(
                    fn_->blocks[exit_id].instrs.begin(), std::move(phi));
                update_scope(vi.name, fn_->blocks[exit_id].instrs[0].dst);
            }
        } else {
            for (auto &vi: vars) {
                update_scope(vi.name, vi.phi_value);
            }
        }
    }

    // ---------------------------------------------------------------------
    // do-while.
    //
    // Patron CFG (la primera iteracion del body se ejecuta sin chequear cond):
    //
    //     entry:
    //         ...
    //         br body
    //     body:
    //         x = phi.T [x_pre, entry], [x_loop, header]    ; uno por var
    //         (lowering del body)
    //         br header
    //     header:
    //         cond_v = lower(cond)
    //         br.cond cond_v, body, exit                    ; back-edge a body
    //     exit:
    //
    // Diferencia con while: el body es donde se insertan los PHIs (no el
    // header), porque body es el unico bloque con dos predecesores
    // (entry para la primera iteracion + header para las siguientes).  El
    // header solo evalua la condicion y no escribe variables, asi que el
    // valor que "llega" al body desde el header coincide con el valor al
    // final del body (lookup tras bajar el body).
    // ---------------------------------------------------------------------
    void Lowering::lower_do_while(ast::DoWhileStmt *s) {
        if (!s) return;

        std::set<std::string> modified;
        collect_assigned_vars(s->body.get(), modified);
        collect_assigned_vars(s->cond.get(), modified);

        struct VarInfo {
            std::string   name;
            ir::IrType    type;
            ir::IrValueId pre_loop;
            ir::IrValueId phi_value;
            size_t        phi_idx;
        };
        std::vector<VarInfo> vars;
        vars.reserve(modified.size());
        for (const auto &name: modified) {
            ir::IrValueId pre = lookup(name);
            if (pre == ir::IR_NO_VALUE) continue;
            VarInfo vi;
            vi.name     = name;
            vi.type     = fn_->values[pre].type;
            vi.pre_loop = pre;
            vars.push_back(vi);
        }

        const ir::IrBlockId entry_block = current_block_;
        const ir::IrBlockId body_id     = fn_->new_block("dowhile_body");
        const ir::IrBlockId header_id   = fn_->new_block("dowhile_header");
        const ir::IrBlockId exit_id     = fn_->new_block("dowhile_exit");

        // entry -> body (BR incondicional para la primera iteracion).
        {
            ir::IrInstr br{};
            br.op           = ir::IrOp::BR;
            br.target_block = body_id;
            br.source_line  = s->loc.line;
            fn_->append(entry_block, std::move(br));
        }
        fn_->blocks[entry_block].succs.push_back(body_id);
        fn_->blocks[body_id].preds.push_back(entry_block);

        // PHIs en body.  El primer pred es entry; el segundo (header) se
        // completa al final.
        for (auto &vi: vars) {
            vi.phi_value = fn_->new_value(vi.type);
            ir::IrInstr phi{};
            phi.op   = ir::IrOp::PHI;
            phi.type = vi.type;
            phi.dst  = vi.phi_value;
            phi.phi_args.push_back({vi.pre_loop, entry_block});
            phi.source_line = s->loc.line;
            fn_->append(body_id, std::move(phi));
            vi.phi_idx = fn_->blocks[body_id].instrs.size() - 1;
            update_scope(vi.name, vi.phi_value);
        }

        // Bajar body en body_id.  Push targets de break/continue del
        // do-while.  En do-while continue salta al header (que evalua
        // cond y decide back-edge); break salta al exit.
        loop_targets_.push_back({header_id, exit_id, {}, {}});
        current_block_    = body_id;
        block_terminated_ = false;
        lower_stmt(s->body.get());
        loop_targets_.pop_back();

        // Si el body no termino con return, BR a header.
        if (!block_terminated_) {
            const ir::IrBlockId body_end_id = current_block_; {
                ir::IrInstr br{};
                br.op           = ir::IrOp::BR;
                br.target_block = header_id;
                br.source_line  = s->loc.line;
                fn_->append(body_end_id, std::move(br));
            }
            fn_->blocks[body_end_id].succs.push_back(header_id);
            fn_->blocks[header_id].preds.push_back(body_end_id);
            block_terminated_ = true;
        } else {
            // body termina con return: header nunca se alcanza.  Aun asi
            // necesitamos completar los PHIs con un placeholder para
            // mantener el IR estructuralmente valido.
            for (auto &vi: vars) {
                fn_->blocks[body_id].instrs[vi.phi_idx]
                        .phi_args.push_back({vi.phi_value, body_id});
            }
            current_block_    = exit_id;
            block_terminated_ = false;
            for (auto &vi: vars) update_scope(vi.name, vi.phi_value);
            return;
        }

        // En header: bajar cond + BR_COND a body|exit.  Como en lower_while,
        // la cond puede crear bloques intermedios (short-circuit `&&`/`||`);
        // el BR_COND debe emitirse en @c current_block_ tras lower_expr.
        current_block_       = header_id;
        block_terminated_    = false;
        ir::IrValueId cond_v = lower_expr(s->cond.get());
        if (cond_v == ir::IR_NO_VALUE) return;
        const ir::IrBlockId cond_end_block = current_block_; {
            ir::IrInstr brc{};
            brc.op           = ir::IrOp::BR_COND;
            brc.operands     = {cond_v};
            brc.target_block = body_id; // back-edge
            brc.false_block  = exit_id;
            brc.source_line  = s->loc.line;
            fn_->append(cond_end_block, std::move(brc));
        }
        fn_->blocks[cond_end_block].succs.push_back(body_id);
        fn_->blocks[cond_end_block].succs.push_back(exit_id);
        fn_->blocks[body_id].preds.push_back(cond_end_block);
        fn_->blocks[exit_id].preds.push_back(cond_end_block);
        block_terminated_ = true;

        // Patchar PHIs de body con el back-edge desde el bloque que termina
        // la cond (puede ser != header si hubo short-circuit).
        for (auto &vi: vars) {
            ir::IrValueId loop_val = lookup(vi.name);
            if (loop_val == ir::IR_NO_VALUE) loop_val = vi.phi_value;
            fn_->blocks[body_id].instrs[vi.phi_idx]
                    .phi_args.push_back({loop_val, cond_end_block});
        }

        current_block_    = exit_id;
        block_terminated_ = false;
        // Tras salir del loop, las variables tienen su ultimo valor:
        // como el body se ejecuto y luego el header decidio salir, el
        // valor "vivo" en exit es el mismo que llego al header (lookup
        // en el momento del BR_COND).  No hace falta tocar scope aqui.
        (void) vars;
    }

    void Lowering::lower_for(ast::ForStmt *s) {
        if (!s) return;

        // for(init; cond; step) body
        //
        // Lowering con vars de loop ADDRESS-TAKEN: las variables modificadas
        // en cond/body/step se convierten en stack slots (ALLOCA + LOAD/STORE)
        // durante la vida del loop.  Sin esto, los PHI nodes cross-block
        // disparan un bug del linear scan (back-edges con def lineal posterior
        // al use), reusando el reg de la var del loop dentro del body y
        // corrompiendo el back-edge.  El coste es 1 LOAD/STORE extra por
        // acceso a var del loop, despreciable comparado con la operacion del
        // loop tipica.
        //
        // CFG:
        //   entry -> [init] -> [ALLOCA + STORE init] -> header
        //   header -> br_cond cond -> body | exit
        //   body  -> step (fall-through al final, o via continue)
        //   step  -> header (back-edge; las vars se actualizan via STORE)
        //   exit  (target de break; tras el loop, las vars vuelven a SSA
        //   leyendo del slot)
        push_scope();
        if (s->init) {
            lower_stmt(s->init.get());
            if (block_terminated_) {
                pop_scope();
                return;
            }
        }

        // Pre-walk: variables mutadas en cond+body+step.
        std::set<std::string> modified;
        if (s->cond) collect_assigned_vars(s->cond.get(), modified);
        if (s->body) collect_assigned_vars(s->body.get(), modified);
        if (s->step) collect_assigned_vars(s->step.get(), modified);

        // Para cada var: alocar slot, STORE el valor inicial, marcarla como
        // address-taken, bindear el name al addr.  Las lecturas usaran LOAD
        // y las escrituras usaran STORE (mismo mecanismo que &local).
        struct LoopVarInfo {
            std::string   name;
            ir::IrType    type;
            ir::IrValueId addr;     // SSA value del puntero (PTR)
            ir::IrValueId pre_loop; // SSA original antes del loop
        };
        std::vector<LoopVarInfo> vars;
        vars.reserve(modified.size());
        for (const auto &name: modified) {
            ir::IrValueId pre = lookup(name);
            if (pre == ir::IR_NO_VALUE) continue;
            LoopVarInfo vi;
            vi.name = name;

            // Si la var ya esta address-taken (e.g. usada en un loop
            // previo, o el usuario hizo &name), `pre` ES la addr del slot
            // existente.  En ese caso NO alocamos un slot nuevo: reusamos
            // el slot existente.  Sin esta deteccion, dos for/while
            // consecutivos sobre la misma variable creaban slots nuevos
            // sin sincronizar y los STORE/LOAD posteriores leeran el slot
            // viejo del primer loop.
            if (address_taken_locals_.count(name)) {
                // pre es la addr del slot existente.  El tipo del valor
                // que vive en el slot lo recuperamos del bind original
                // (la addr es PTR pero el slot guarda i32/i64/etc).
                // Como mi info de tipo solo se materializa al alocar,
                // inferimos i64 para tipo desconocido aqui.
                vi.type     = ir::IrType::I64;
                vi.addr     = pre;
                vi.pre_loop = pre;
                vars.push_back(vi);
                continue;
            }
            vi.type     = fn_->values[pre].type;
            vi.pre_loop = pre;

            // ALLOCA 8 bytes (i64) en current block (entry block del for).
            ir::IrValueId addr = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr   al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8;
            al.dst         = addr;
            al.imm         = 8;
            al.source_line = s->loc.line;
            fn_->append(current_block_, std::move(al));

            // STORE pre_loop (el VALOR original SSA) al slot.
            ir::IrInstr st{};
            st.op          = ir::IrOp::STORE;
            st.type        = vi.type;
            st.operands    = {pre, addr};
            st.source_line = s->loc.line;
            fn_->append(current_block_, std::move(st));

            vi.addr = addr;
            vars.push_back(vi);
            // Marcar address-taken y bindear el nombre a la direccion del slot
            // (igual que `&x` haria con un local).  read_local/write_local
            // detectan esto y emiten LOAD/STORE.
            address_taken_locals_.insert(name);
            update_scope(name, addr);
        }

        const ir::IrBlockId entry_block = current_block_;
        const ir::IrBlockId header_id   = fn_->new_block("for_header");
        const ir::IrBlockId body_id     = fn_->new_block("for_body");
        const ir::IrBlockId step_id     = fn_->new_block("for_step");
        const ir::IrBlockId exit_id     = fn_->new_block("for_exit");

        // entry -> header
        {
            ir::IrInstr br{};
            br.op           = ir::IrOp::BR;
            br.target_block = header_id;
            br.source_line  = s->loc.line;
            fn_->append(entry_block, std::move(br));
        }
        fn_->blocks[entry_block].succs.push_back(header_id);
        fn_->blocks[header_id].preds.push_back(entry_block);

        // header: bajar cond + br_cond.  Si no hay cond, asumimos true.
        current_block_    = header_id;
        block_terminated_ = false;
        ir::IrValueId cond_v;
        if (s->cond) {
            cond_v = lower_expr(s->cond.get());
            if (cond_v == ir::IR_NO_VALUE) {
                pop_scope();
                return;
            }
        } else {
            cond_v = emit_const(ir::IrType::BOOL, 1, s->loc.line);
        }
        const ir::IrBlockId cond_end_block = current_block_; {
            ir::IrInstr brc{};
            brc.op           = ir::IrOp::BR_COND;
            brc.operands     = {cond_v};
            brc.target_block = body_id;
            brc.false_block  = exit_id;
            brc.source_line  = s->loc.line;
            fn_->append(cond_end_block, std::move(brc));
        }
        fn_->blocks[cond_end_block].succs.push_back(body_id);
        fn_->blocks[cond_end_block].succs.push_back(exit_id);
        fn_->blocks[body_id].preds.push_back(cond_end_block);
        fn_->blocks[exit_id].preds.push_back(cond_end_block);

        // Body: push targets {continue=step, break=exit}.
        loop_targets_.push_back({step_id, exit_id, {}, {}});
        current_block_    = body_id;
        block_terminated_ = false;
        if (s->body) lower_stmt(s->body.get());
        LoopTargets lt = std::move(loop_targets_.back());
        loop_targets_.pop_back();

        // Si el body cayo (no return/break), BR a step.
        if (!block_terminated_) {
            const ir::IrBlockId body_end_id = current_block_;
            ir::IrInstr         brm{};
            brm.op           = ir::IrOp::BR;
            brm.target_block = step_id;
            brm.source_line  = s->loc.line;
            fn_->append(body_end_id, std::move(brm));
            fn_->blocks[body_end_id].succs.push_back(step_id);
            fn_->blocks[step_id].preds.push_back(body_end_id);
        }

        current_block_    = step_id;
        block_terminated_ = false;
        if (s->step) {
            (void) lower_expr(s->step.get());
        }
        if (!block_terminated_) {
            const ir::IrBlockId step_end_id = current_block_;
            ir::IrInstr         brm{};
            brm.op           = ir::IrOp::BR;
            brm.target_block = header_id;
            brm.source_line  = s->loc.line;
            fn_->append(step_end_id, std::move(brm));
            fn_->blocks[step_end_id].succs.push_back(header_id);
            fn_->blocks[header_id].preds.push_back(step_end_id);
        }
        // Suprimir el unused warning si hay continue_preds (las edges ya
        // estan registradas por ContinueStmt; no hace falta hacer nada
        // adicional aqui).
        (void) lt;

        // Continuar en exit.  Las vars del loop siguen address-taken; los
        // accesos posteriores van por LOAD del slot.  No las desmarcamos
        // de @c address_taken_locals_ porque tipicamente la var muere al
        // salir del scope del for (e.g. `j` declarada en el init).  Para
        // vars del scope exterior (e.g. `sum`), permanecer address-taken
        // tiene un costo despreciable y mantiene la semantica consistente.
        current_block_    = exit_id;
        block_terminated_ = false;
        pop_scope();
    }

    void Lowering::lower_return(ast::ReturnStmt *s) {
        // sret: si la funcion declara devolver Optional/Result, no
        // emitimos un RET con valor; en cambio:
        //   1. Bajamos s->value a un buffer local (Some/Ok/Err producen
        //      una ALLOCA stack-local en esta funcion).
        //   2. MEMCPY del buffer local al retbuf que recibimos del caller.
        //   3. RET void.
        // Esto es la convencion sret estandar: sin heap allocation, sin
        // leaks; el caller decide donde vive el resultado.
        if (sret_active_ && s->value) {
            // M7 — in-place SRET para `return unique_box(...)`: si la
            // funcion devuelve un smart pointer (sret_buf_size_=16) y el
            // value retornado es un CallExpr a `unique_box`/`shared_box`,
            // construimos el smart pointer DIRECTAMENTE en el retbuf del
            // caller, saltandonos la copia qword-a-qword al final.  El
            // lowering de unique_box consulta @c unique_box_target_slot_
            // y lo usa como slot en vez de hacer stack_alloc_buf.
            bool inplace_sret = false;
            if (sret_buf_size_ == 16
             && s->value->kind == ast::NodeKind::CallExpr) {
                auto *ce = static_cast<ast::CallExpr *>(s->value.get());
                if (ce->callee
                    && ce->callee->kind == ast::NodeKind::IdentExpr) {
                    auto *id = static_cast<ast::IdentExpr *>(ce->callee.get());
                    if (id->name == "unique_box"
                     || id->name == "shared_box"
                     || id->name == "unique_with"
                     || id->name == "shared_with") {
                        inplace_sret = true;
                        unique_box_target_slot_ = sret_retbuf_;
                    }
                }
            }
            const ir::IrValueId v_local = lower_expr(s->value.get());
            unique_box_target_slot_ = ir::IR_NO_VALUE;  // limpiar siempre
            if (inplace_sret) {
                // El smart pointer ya se construyo IN-PLACE sobre el
                // retbuf del caller.  No hace falta copia final; saltamos
                // al RET directamente.
                emit_cleanups_all();
                // Instrumentacion: emitir vex_trace:leave antes del RET
                // tambien en este path SRET (mismo filtro que lower_return
                // del path normal).  Sin esto, fns que retornan
                // unique<T>/shared<T> NO cierran el trace y producen un
                // arbol descuadrado.
                if (instrument_mode_ != "none" && instrument_mode_ != ""
                    && fn_ != nullptr) {
                    const std::string &fname = fn_->name;
                    const bool is_helper = fname == "__module_init"
                        || fname.compare(0, 6, "__new_") == 0
                        || fname.compare(0, 8, "__async_") == 0
                        || fname.compare(0, 9, "__lambda_") == 0
                        || fname.compare(0, 8, "__spawn_") == 0;
                    if (!is_helper) {
                        emit_instrument_exit(fname, sret_retbuf_, s->loc.line);
                    }
                }
                ir::IrInstr ret{};
                ret.op          = ir::IrOp::RET;
                ret.type        = ir::IrType::VOID;
                ret.source_line = s->loc.line;
                fn_->append(current_block_, std::move(ret));
                block_terminated_ = true;
                return;
            }
            if (v_local != ir::IR_NO_VALUE) {
                // Copia qword-a-qword (16 bytes = 2 qwords; 24 = 3).
                // No usamos MEMCPY/vmcopy porque vmcopy es VM->host y
                // ambos buffers (local y retbuf) viven en VM memory.
                // El bucle desenrollado emite LOAD i64 + STORE i64 por
                // cada slot; el regalloc reusa los temporales.
                const uint64_t qwords = sret_buf_size_ / 8; // 2 o 3
                for (uint64_t qi = 0; qi < qwords; ++qi) {
                    const uint64_t off = qi * 8;
                    // src+off
                    const ir::IrValueId v_off    = emit_const(ir::IrType::I64, off, s->loc.line);
                    const ir::IrValueId v_src_at = fn_->new_value(ir::IrType::PTR); {
                        ir::IrInstr add{};
                        add.op          = ir::IrOp::ADD;
                        add.type        = ir::IrType::I64;
                        add.dst         = v_src_at;
                        add.operands    = {v_local, v_off};
                        add.source_line = s->loc.line;
                        fn_->append(current_block_, std::move(add));
                    }
                    // LOAD i64 from src+off
                    const ir::IrValueId v_tmp = fn_->new_value(ir::IrType::I64); {
                        ir::IrInstr ld{};
                        ld.op          = ir::IrOp::LOAD;
                        ld.type        = ir::IrType::I64;
                        ld.dst         = v_tmp;
                        ld.operands    = {v_src_at};
                        ld.source_line = s->loc.line;
                        fn_->append(current_block_, std::move(ld));
                    }
                    // dst+off
                    const ir::IrValueId v_off2   = emit_const(ir::IrType::I64, off, s->loc.line);
                    const ir::IrValueId v_dst_at = fn_->new_value(ir::IrType::PTR); {
                        ir::IrInstr add{};
                        add.op          = ir::IrOp::ADD;
                        add.type        = ir::IrType::I64;
                        add.dst         = v_dst_at;
                        add.operands    = {sret_retbuf_, v_off2};
                        add.source_line = s->loc.line;
                        fn_->append(current_block_, std::move(add));
                    }
                    // BugFix sret-cross-mem (2026-06-04): propagar
                    // is_host_ptr de sret_retbuf_ al v_dst_at para que el
                    // STORE downstream emita `movh` (host) en lugar de
                    // `mov` (VM mem).  El retbuf SIEMPRE vive en host
                    // memory (ALLOCA del caller); sin esta propagacion
                    // el STORE escribe a vm_mem mientras el caller lee
                    // host -> Result tag/value/error siempre en cero.
                    fn_->values[v_dst_at].is_host_ptr =
                        fn_->values[sret_retbuf_].is_host_ptr;
                    // STORE i64 [dst+off] = tmp
                    {
                        ir::IrInstr st{};
                        st.op          = ir::IrOp::STORE;
                        st.type        = ir::IrType::I64;
                        st.operands    = {v_tmp, v_dst_at};
                        st.source_line = s->loc.line;
                        fn_->append(current_block_, std::move(st));
                    }
                }
            }
            // ejecutar cleanups (e.g. monexit de synchronized
            // activos) justo ANTES del RET sret.  Las copias al retbuf ya
            // se completaron arriba; los cleanups solo modifican estado
            // global (mailboxes, monitores) sin tocar el retbuf.
            emit_cleanups_all();
            // Instrumentacion: emitir vex_trace:leave antes del RET sret.
            // Sin esto, fns que retornan Optional<T>/Result<V,E> NO cierran
            // el trace y producen un arbol descuadrado en la salida.
            if (instrument_mode_ != "none" && instrument_mode_ != ""
                && fn_ != nullptr) {
                const std::string &fname = fn_->name;
                const bool is_helper = fname == "__module_init"
                    || fname.compare(0, 6, "__new_") == 0
                    || fname.compare(0, 8, "__async_") == 0
                    || fname.compare(0, 9, "__lambda_") == 0
                    || fname.compare(0, 8, "__spawn_") == 0;
                if (!is_helper) {
                    emit_instrument_exit(fname, sret_retbuf_, s->loc.line);
                }
            }
            ir::IrInstr ret{};
            ret.op          = ir::IrOp::RET;
            ret.type        = ir::IrType::VOID;
            ret.source_line = s->loc.line;
            fn_->append(current_block_, std::move(ret));
            block_terminated_ = true;
            return;
        }
        // Camino normal (no sret): bajar el valor de retorno PRIMERO, luego
        // emitir los cleanups (que pueden tocar registros pero no afectan
        // el SSA value computado), y finalmente el RET.
        ir::IrValueId v_ret = ir::IR_NO_VALUE;
        if (s->value) {
            // Auto-promotion: si la funcion declara devolver `string` y el
            // valor de retorno es un string literal sin interpolacion,
            // promocionar al StringObject GC-managed via STRMAKE (mismo
            // patron que `lower_var_decl` para `string s = "lit"`).  Sin
            // esto, `return "abc"` devolveria el ptr crudo (host) a los
            // bytes en static_data y el caller intentaria tratarlo como
            // GcHandle, llamando a strraw/strlen sobre basura.
            if (current_fn_returns_string_
                && s->value->kind == ast::NodeKind::StringLitExpr) {
                // Tanto literales puros como interpolados: el helper
                // construye el StringObject (1 STRMAKE para puros,
                // cadena de STRMAKE+STRCAT para interpolados).
                auto *slit = static_cast<ast::StringLitExpr *>(s->value.get());
                v_ret      = lower_string_literal_to_string_object(slit);
            } else {
                v_ret = lower_expr(s->value.get());
                if (v_ret != ir::IR_NO_VALUE) {
                    // Item 9: si estamos en el helper @Async, NO castear
                    // al ret_type del helper (que es VOID o i64).  El block
                    // siguiente (async_fut_id_) hace el BITCAST/cast
                    // correcto para preservar bits (no value).  Sin esto,
                    // un `return f64_value` se castea F64->I64 via FTOUI
                    // (cambia value, no preserva bits), corrompiendo el
                    // payload del fulfill.
                    if (async_fut_id_ == ir::IR_NO_VALUE) {
                        v_ret = cast_if_needed(v_ret, fn_->values[v_ret].type, fn_->ret_type, s->loc.line);
                    }
                }
            }
        }
        // si estamos en el body de una funcion @Async lowered
        // como spawn helper, intercepta el return: en lugar de RET, emite
        // `fulfill(async_fut, value) + hlt`.  El caller obtendra el valor
        // via `await`.  El hlt es necesario porque el child no debe hacer
        // ret (no hay caller en el stack del child).
        //
        // Mejora II: el bytecode `fulfill r_fut, r_value` espera un i64
        // raw como payload.  Si el `return X` del usuario produjo un valor
        // de tipo distinto (i32/i16/i8/bool/char/f32/f64), debemos coercerlo
        // a i64 preservando la semantica de bits para que el `await` del
        // caller pueda recuperarlo correctamente.  Para floats: BITCAST
        // (no ITOF que cambiaria el valor).  Para enteros estrechos:
        // cast_if_needed (zext/sext segun signedness).
        if (async_fut_id_ != ir::IR_NO_VALUE) {
            ir::IrValueId v_payload = v_ret;
            if (v_payload == ir::IR_NO_VALUE) {
                v_payload = emit_const(ir::IrType::I64, 0, s->loc.line);
            } else {
                const ir::IrType pt = fn_->values[v_payload].type;
                if (pt == ir::IrType::F64) {
                    ir::IrValueId v_bits = fn_->new_value(ir::IrType::I64);
                    ir::IrInstr bc{};
                    bc.op = ir::IrOp::BITCAST;
                    bc.type = ir::IrType::I64;
                    bc.dst = v_bits;
                    bc.operands = {v_payload};
                    bc.source_line = s->loc.line;
                    fn_->append(current_block_, std::move(bc));
                    v_payload = v_bits;
                } else if (pt == ir::IrType::F32) {
                    // f32 -> bits i32 -> zero-extend a i64.
                    ir::IrValueId v_i32 = fn_->new_value(ir::IrType::I32);
                    ir::IrInstr bc{};
                    bc.op = ir::IrOp::BITCAST;
                    bc.type = ir::IrType::I32;
                    bc.dst = v_i32;
                    bc.operands = {v_payload};
                    bc.source_line = s->loc.line;
                    fn_->append(current_block_, std::move(bc));
                    v_payload = cast_if_needed(v_i32, ir::IrType::I32,
                                                ir::IrType::I64, s->loc.line);
                } else if (pt != ir::IrType::I64 && pt != ir::IrType::U64
                        && pt != ir::IrType::PTR) {
                    v_payload = cast_if_needed(v_payload, pt, ir::IrType::I64,
                                                s->loc.line);
                }
            }
            // raw_asm-elim 2026-05-28: usar IrOp::FULFILL_HLT directo.
            // Fusion atomica fulfill+hlt en 1 instr VM, mismo bytecode.
            ir::IrInstr fh{};
            fh.op          = ir::IrOp::FULFILL_HLT;
            fh.type        = ir::IrType::VOID;
            fh.dst         = ir::IR_NO_VALUE;
            fh.operands    = {async_fut_id_, v_payload};
            fh.source_line = s->loc.line;
            fn_->append(current_block_, std::move(fh));
            block_terminated_ = true;
            return;
        }
        // rspawn body return -> mov r0, X + hlt.  El runtime remoto
        // detecta HALT en un proceso con rspawn_future_id != 0 y envia
        // VDP_FUTURE_FULFILL al nodo origen con R0 como payload.  El caller
        // local recibe el valor via `await fut`.
        if (is_rspawn_body_) {
            // raw_asm-elim wave 3: rspawn body return via IrOp::RSPAWN_RETURN.
            // Emite `mov r0, payload + hlt` fusionado; el runtime VDP detecta
            // HALT en un proceso con rspawn_future_id != 0 y envia el valor.
            ir::IrValueId v_payload = v_ret;
            if (v_payload == ir::IR_NO_VALUE) {
                v_payload = emit_const(ir::IrType::I64, 0, s->loc.line);
            }
            ir::IrInstr rr{};
            rr.op          = ir::IrOp::RSPAWN_RETURN;
            rr.type        = ir::IrType::VOID;
            rr.dst         = ir::IR_NO_VALUE;
            rr.operands    = {v_payload};
            rr.source_line = s->loc.line;
            fn_->append(current_block_, std::move(rr));
            block_terminated_ = true;
            return;
        }
        // ejecutar cleanups activos (synchronized -> tryleave + monexit).
        // El SSA value v_ret sobrevive: el regalloc garantiza que se mantenga
        // vivo hasta el RET (o se reescriba antes si conviene).
        emit_cleanups_all();
        // Instrumentacion: vex_trace:exit antes del RET explicito.  Skipea
        // helpers internos (mismo filtro que en lower_function).
        if (instrument_mode_ != "none" && instrument_mode_ != ""
            && fn_ != nullptr) {
            const std::string &fname = fn_->name;
            const bool is_helper = fname == "__module_init"
                || fname.compare(0, 6, "__new_") == 0
                || fname.compare(0, 8, "__async_") == 0
                || fname.compare(0, 9, "__lambda_") == 0
                || fname.compare(0, 8, "__spawn_") == 0;
            if (!is_helper) {
                emit_instrument_exit(fname, v_ret, s->loc.line);
            }
        }
        ir::IrInstr ret{};
        ret.op          = ir::IrOp::RET;
        ret.type        = fn_->ret_type;
        ret.source_line = s->loc.line;
        if (v_ret != ir::IR_NO_VALUE) {
            ret.operands.push_back(v_ret);
        }
        fn_->append(current_block_, std::move(ret));
        block_terminated_ = true;
    }

    // =========================================================================
    //  Instrumentacion (vex_trace:enter / vex_trace:exit)
    // =========================================================================
    //
    // Como el lowering emite CALLN a un nombre @c "vex_trace:enter" /
    // @c "vex_trace:exit", todos los backends (bytecode VM, JIT, port C,
    // futuros) heredan la instrumentacion automaticamente.  Cada backend
    // resuelve el simbolo a su forma:
    //   - bytecode VM: CALLN se resuelve via stdlib/native/runtime/vex_trace.dll
    //   - JIT: idem (mismo CALLN dispatch)
    //   - port C: emit_native_call lo bridgea a fprintf stderr (default)
    //             o el usuario provee su propia implementacion.

    void Lowering::emit_instrument_enter(const std::string &fn_name,
                                          uint32_t line) {
        if (!fn_ || !out_mod_) return;
        // 1. Internar el nombre como literal en static_data.  Incluye nul
        //    terminator para que sea NUL-terminated C string utilizable
        //    por strdup/printf en cualquier backend hosted.
        std::vector<uint8_t> bytes(fn_name.begin(), fn_name.end());
        bytes.push_back(0);
        const uint64_t name_idx = out_mod_->intern_static_data(std::move(bytes));

        // 2. STR_LIT_ADDR: cargar ptr al literal en un SSA value.
        const ir::IrValueId v_name = fn_->new_value(ir::IrType::PTR);
        {
            ir::IrInstr sa{};
            sa.op          = ir::IrOp::STR_LIT_ADDR;
            sa.type        = ir::IrType::PTR;
            sa.dst         = v_name;
            sa.imm         = name_idx;
            sa.source_line = line;
            fn_->append(current_block_, std::move(sa));
        }

        // 3. CALLN void a "vex_trace:enter"(proc_ptr, name_ptr).
        //    El proc_ptr lo obtenemos via @c getproc; el plugin nativo
        //    lo usa para @c vm_read_bytes del nombre.  En port C el
        //    bridge ignora el proc_ptr.
        const ir::IrValueId v_proc = emit_getproc(line);
        ir::IrInstr call{};
        call.op          = ir::IrOp::CALLN;
        call.type        = ir::IrType::VOID;
        call.dst         = ir::IR_NO_VALUE;
        // El @c lib_path incluye el subdir bajo @c stdlib/native/ para
        // que el loader pueda resolver la DLL via path relativo al
        // @c vm.exe (igual convencion que vesta_io / vesta_math).
        call.func_name   = "stdlib/native/runtime/vex_trace:enter";
        call.operands    = {v_proc, v_name};
        call.source_line = line;
        fn_->append(current_block_, std::move(call));

        // 4. Registrar el import nativo para que el linker .velb
        //    incluya la libreria.
        out_mod_->register_native_import(
            "stdlib/native/runtime/vex_trace", "enter");
    }

    void Lowering::emit_instrument_exit(const std::string &fn_name,
                                         ir::IrValueId v_ret,
                                         uint32_t line) {
        if (!fn_ || !out_mod_) return;
        std::vector<uint8_t> bytes(fn_name.begin(), fn_name.end());
        bytes.push_back(0);
        const uint64_t name_idx = out_mod_->intern_static_data(std::move(bytes));

        const ir::IrValueId v_name = fn_->new_value(ir::IrType::PTR);
        {
            ir::IrInstr sa{};
            sa.op          = ir::IrOp::STR_LIT_ADDR;
            sa.type        = ir::IrType::PTR;
            sa.dst         = v_name;
            sa.imm         = name_idx;
            sa.source_line = line;
            fn_->append(current_block_, std::move(sa));
        }

        // Si la funcion es void, pasar 0 como return value placeholder.
        ir::IrValueId v_val = v_ret;
        if (v_val == ir::IR_NO_VALUE) {
            v_val = emit_const(ir::IrType::I64, 0, line);
        }

        const ir::IrValueId v_proc = emit_getproc(line);
        ir::IrInstr call{};
        call.op          = ir::IrOp::CALLN;
        call.type        = ir::IrType::VOID;
        call.dst         = ir::IR_NO_VALUE;
        // Usamos @c leave en lugar de @c exit para evitar colision con la
        // libc @c exit() cuando el port C emite @c extern declarations.
        call.func_name   = "stdlib/native/runtime/vex_trace:leave";
        call.operands    = {v_proc, v_name, v_val};
        call.source_line = line;
        fn_->append(current_block_, std::move(call));

        out_mod_->register_native_import(
            "stdlib/native/runtime/vex_trace", "leave");
    }

    // Forward decls de helpers definidos mas abajo en el TU.  Necesarias
    // porque lower_try y try_lower_builtin_call los usan.
    static uint64_t intern_class_name(ir::IrModule &mod, const std::string &name);


    /// usado por lower_class_methods para emitir el CALLVIRT a
    /// destructores de fields destructibles del contenedor.
    static ir::IrValueId emit_field_addr(ir::IrFunction *fn,
                                         ir::IrBlockId   block,
                                         ir::IrValueId   base,
                                         uint32_t        offset,
                                         uint32_t        line);

    // ---------------------------------------------------------------------
    // try / catch / throw.
    //
    // Estrategia: usamos las instrucciones bytecode existentes
    // tryenter / tryleave / throw.  El IR no tiene un nodo dedicado para
    // exception frames; emitimos RAW_ASM con substitucion {dst}/{srcN}
    // para colocar el handler PC y el ClassInfo* en registros.
    //
    // Layout de bloques (1 catch, sin finally):
    //   current      -> RAW_ASM: findclass exc + tryenter handler, type
    //                  -> br body
    //   body         -> lower(try body)
    //                  -> RAW_ASM: tryleave + jmp merge
    //   handler      -> bind r0 a var (si la hay) + lower(catch body)
    //                  -> br merge
    //   merge        -> continuacion
    //
    // Multi-catch / finally: pendientes (deferidos en MVP).
    //
    // El handler PC se obtiene como @Absolute("code.<fn>_<handler.name>")
    // donde <handler.name> incluye el sufijo numerico que new_block anyade.
    // Asi el linker resuelve la referencia sin necesitar metadata extra.
    // ---------------------------------------------------------------------

    void Lowering::lower_try(ast::TryStmt *s) {
        if (!s->body) {
            error_at(s->loc, "lowering: try sin body");
            return;
        }
        if (s->catches.empty() && !s->finally_body) return;

        // Snapshot del scope ANTES del try.  Necesario para
        // detectar variables modificadas dentro del body o de algun
        // catch y emitir PHI nodes en el merge.  Sin esto, el binding
        // del nombre tras el try queda con el del UlTIMO branch lowered
        // (no determinista) y, en runtime, el regalloc puede colocar la
        // variable en registros distintos en cada rama -> el merge lee
        // el registro equivocado.  Ej: `i32 v=0; try { v=42; } catch
        // (E e) { v=99; } println(v);` antes y devolvia basura.
        // Snapshot del scope chain APLANADO: combina todos los scopes
        // visibles desde el current_block_ en un solo mapa, con el
        // innermost ganando en caso de colision.  Necesario para que
        // `this` y los parametros del metodo (que viven en el outer
        // scope, NO en scopes_.back()) sean spillables a traves de
        // try/catch.  Sin esto, entry_bindings.find("this") fallaba y
        // la reload del catch leia basura.
        std::unordered_map<std::string, ir::IrValueId> entry_bindings;
        for (const auto &sc: scopes_) {
            for (const auto &kv: sc) {
                entry_bindings[kv.first] = kv.second; // innermost wins
            }
        }

        // Pre-scan: detectar variables del scope outer que se
        // asignan dentro del body o de algun catch.  Reservamos un slot
        // 8 bytes para cada una y guardamos su valor de entrada.
        // Durante el body+catches, write_local emite STORE adicional al
        // slot.  En el merge LOAD del slot -> bind nombre.  Esto evita
        // el problema del regalloc: el throw salta los pop pendientes
        // pero el slot vive en stack VM y conserva el valor correcto.
        std::unordered_set<std::string> assigned_in_try;
        // Helper recursivo: visita un Stmt y registra IdentExpr en
        // lhs de assign cuyo nombre exista en el scope outer.
        std::function<void(const ast::Stmt *)> scan_assign =
                [&](const ast::Stmt *st) {
            if (!st) return;
            // Casos relevantes: ExprStmt con AssignExpr o BinaryExpr con
            // op=ASSIGN; BlockStmt con multiples stmts; If con then/else;
            // While/For con body; ReturnStmt; throw, try (anidado).
            switch (st->kind) {
                case ast::NodeKind::ExprStmt: {
                    auto *es = static_cast<const ast::ExprStmt *>(st);
                    // Recursar en la expr para detectar assign.
                    std::function<void(const ast::Expr *)> visit_expr =
                            [&](const ast::Expr *e) {
                        if (!e) return;
                        if (e->kind == ast::NodeKind::AssignExpr) {
                            auto *ae = static_cast<const ast::AssignExpr *>(e);
                            if (ae->target
                                && ae->target->kind == ast::NodeKind::IdentExpr) {
                                auto *id = static_cast<const ast::IdentExpr *>(
                                    ae->target.get());
                                if (entry_bindings.count(id->name)) {
                                    assigned_in_try.insert(id->name);
                                }
                            }
                            visit_expr(ae->value.get());
                            return;
                        }
                        if (e->kind == ast::NodeKind::BinaryExpr) {
                            auto *be = static_cast<const ast::BinaryExpr *>(e);
                            visit_expr(be->lhs.get());
                            visit_expr(be->rhs.get());
                            return;
                        }
                        if (e->kind == ast::NodeKind::CallExpr) {
                            auto *ce = static_cast<const ast::CallExpr *>(e);
                            for (auto &a: ce->args) visit_expr(a.get());
                            return;
                        }
                    };
                    visit_expr(es->expr.get());
                    break;
                }
                case ast::NodeKind::BlockStmt: {
                    auto *b = static_cast<const ast::BlockStmt *>(st);
                    for (auto &s2: b->body) scan_assign(s2.get());
                    break;
                }
                case ast::NodeKind::IfStmt: {
                    auto *ifs = static_cast<const ast::IfStmt *>(st);
                    scan_assign(ifs->then_branch.get());
                    scan_assign(ifs->else_branch.get());
                    break;
                }
                case ast::NodeKind::WhileStmt: {
                    auto *ws = static_cast<const ast::WhileStmt *>(st);
                    scan_assign(ws->body.get());
                    break;
                }
                case ast::NodeKind::ForStmt: {
                    auto *fs = static_cast<const ast::ForStmt *>(st);
                    scan_assign(fs->body.get());
                    break;
                }
                case ast::NodeKind::TryStmt: {
                    auto *ts = static_cast<const ast::TryStmt *>(st);
                    scan_assign(ts->body.get());
                    for (auto &cc: ts->catches) scan_assign(cc.body.get());
                    if (ts->finally_body) scan_assign(ts->finally_body.get());
                    break;
                }
                case ast::NodeKind::VarDeclStmt: {
                    // var-decl introduce nuevo nombre; el init expr
                    // puede contener assigns a otras vars.
                    auto *vd = static_cast<const ast::VarDeclStmt *>(st);
                    if (vd->init) {
                        std::function<void(const ast::Expr *)> visit2 =
                                [&](const ast::Expr *e) {
                            if (!e) return;
                            if (e->kind == ast::NodeKind::AssignExpr) {
                                auto *ae = static_cast<const ast::AssignExpr *>(e);
                                if (ae->target
                                    && ae->target->kind == ast::NodeKind::IdentExpr) {
                                    auto *id = static_cast<const ast::IdentExpr *>(
                                        ae->target.get());
                                    if (entry_bindings.count(id->name)) {
                                        assigned_in_try.insert(id->name);
                                    }
                                }
                                visit2(ae->value.get());
                            }
                        };
                        visit2(vd->init.get());
                    }
                    break;
                }
                default: break;
            }
        };
        scan_assign(s->body.get());
        for (const auto &cc: s->catches) scan_assign(cc.body.get());

        // Pre-scan adicional: detectar variables del scope outer que se
        // LEEN dentro de los catches (incluyendo `this` y parametros del
        // metodo).  Sin esto, un throw clobreaba los registros y el
        // catch leia basura: por ejemplo `try { foo(); } catch (E e)
        // { this.dlog(...); }` fallaba con CALLVIRT null porque r1
        // (this) ya no era valido tras el throw.  Tratamos READ-en-catch
        // igual que assign-en-body: spill al entry value y reload por LOAD
        // en el merge.  Cubre `this`, parametros, locales no-modificadas
        // y cualquier otro binding del entry scope.
        // NOTA: solo escaneamos los CATCH bodies (no el try-body) porque
        // dentro del try-body los registros se mantienen normales hasta
        // el throw; el problema es post-throw -> handler.
        std::function<void(const ast::Expr *)> scan_read_expr;
        std::function<void(const ast::Stmt *)> scan_read_stmt;
        // Helper: comprueba si `name` esta visible en CUALQUIER scope
        // (no solo el innermost).  `this` y los parametros del metodo
        // viven en el outer scope, por lo que entry_bindings (que solo
        // tiene el innermost) no los ve.
        auto is_visible_in_any_scope = [&](const std::string &name) -> bool {
            for (const auto &sc: scopes_) {
                if (sc.count(name)) return true;
            }
            return false;
        };
        scan_read_expr = [&](const ast::Expr *e) {
            if (!e) return;
            switch (e->kind) {
                case ast::NodeKind::IdentExpr: {
                    auto *id = static_cast<const ast::IdentExpr *>(e);
                    if (is_visible_in_any_scope(id->name)) {
                        assigned_in_try.insert(id->name);
                    }
                    return;
                }
                case ast::NodeKind::ThisExpr: {
                    // `this` se resuelve via lookup("this") en
                    // lower_this_expr, igual que un IdentExpr.  Vive
                    // en el outer scope (function-level binding), no
                    // en entry_bindings (innermost).  Por eso usamos
                    // is_visible_in_any_scope.
                    if (is_visible_in_any_scope("this")) {
                        assigned_in_try.insert("this");
                    }
                    return;
                }
                case ast::NodeKind::AssignExpr: {
                    auto *ae = static_cast<const ast::AssignExpr *>(e);
                    scan_read_expr(ae->target.get());
                    scan_read_expr(ae->value.get());
                    return;
                }
                case ast::NodeKind::BinaryExpr: {
                    auto *be = static_cast<const ast::BinaryExpr *>(e);
                    scan_read_expr(be->lhs.get());
                    scan_read_expr(be->rhs.get());
                    return;
                }
                case ast::NodeKind::UnaryExpr: {
                    auto *ue = static_cast<const ast::UnaryExpr *>(e);
                    scan_read_expr(ue->operand.get());
                    return;
                }
                case ast::NodeKind::CallExpr: {
                    auto *ce = static_cast<const ast::CallExpr *>(e);
                    scan_read_expr(ce->callee.get());
                    for (auto &a: ce->args) scan_read_expr(a.get());
                    return;
                }
                case ast::NodeKind::FieldAccessExpr: {
                    auto *fa = static_cast<const ast::FieldAccessExpr *>(e);
                    scan_read_expr(fa->base.get());
                    return;
                }
                case ast::NodeKind::IndexExpr: {
                    auto *ix = static_cast<const ast::IndexExpr *>(e);
                    scan_read_expr(ix->base.get());
                    scan_read_expr(ix->index.get());
                    return;
                }
                case ast::NodeKind::CastExpr: {
                    auto *ce = static_cast<const ast::CastExpr *>(e);
                    scan_read_expr(ce->operand.get());
                    return;
                }
                case ast::NodeKind::NewExpr: {
                    auto *ne = static_cast<const ast::NewExpr *>(e);
                    for (auto &a: ne->args) scan_read_expr(a.get());
                    return;
                }
                default: return;
            }
        };
        scan_read_stmt = [&](const ast::Stmt *st) {
            if (!st) return;
            switch (st->kind) {
                case ast::NodeKind::ExprStmt: {
                    auto *es = static_cast<const ast::ExprStmt *>(st);
                    scan_read_expr(es->expr.get());
                    return;
                }
                case ast::NodeKind::BlockStmt: {
                    auto *b = static_cast<const ast::BlockStmt *>(st);
                    for (auto &s2: b->body) scan_read_stmt(s2.get());
                    return;
                }
                case ast::NodeKind::VarDeclStmt: {
                    auto *vd = static_cast<const ast::VarDeclStmt *>(st);
                    if (vd->init) scan_read_expr(vd->init.get());
                    return;
                }
                case ast::NodeKind::IfStmt: {
                    auto *ifs = static_cast<const ast::IfStmt *>(st);
                    scan_read_expr(ifs->cond.get());
                    scan_read_stmt(ifs->then_branch.get());
                    scan_read_stmt(ifs->else_branch.get());
                    return;
                }
                case ast::NodeKind::WhileStmt: {
                    auto *ws = static_cast<const ast::WhileStmt *>(st);
                    scan_read_expr(ws->cond.get());
                    scan_read_stmt(ws->body.get());
                    return;
                }
                case ast::NodeKind::ForStmt: {
                    auto *fs = static_cast<const ast::ForStmt *>(st);
                    scan_read_stmt(fs->init.get());
                    scan_read_expr(fs->cond.get());
                    scan_read_expr(fs->step.get());
                    scan_read_stmt(fs->body.get());
                    return;
                }
                case ast::NodeKind::ReturnStmt: {
                    auto *rs = static_cast<const ast::ReturnStmt *>(st);
                    if (rs->value) scan_read_expr(rs->value.get());
                    return;
                }
                case ast::NodeKind::ThrowStmt: {
                    auto *ts = static_cast<const ast::ThrowStmt *>(st);
                    if (ts->value) scan_read_expr(ts->value.get());
                    return;
                }
                case ast::NodeKind::TryStmt: {
                    auto *ts = static_cast<const ast::TryStmt *>(st);
                    scan_read_stmt(ts->body.get());
                    for (auto &cc: ts->catches) scan_read_stmt(cc.body.get());
                    if (ts->finally_body) scan_read_stmt(ts->finally_body.get());
                    return;
                }
                case ast::NodeKind::SynchronizedStmt: {
                    auto *ss = static_cast<const ast::SynchronizedStmt *>(st);
                    scan_read_expr(ss->target.get());
                    scan_read_stmt(ss->body.get());
                    return;
                }
                default: return;
            }
        };
        for (const auto &cc: s->catches) scan_read_stmt(cc.body.get());
        if (s->finally_body) scan_read_stmt(s->finally_body.get());

        // Reservar slots y guardar entry value para cada var asignada.
        // Save try_spill_slots_ previo (puede haber try anidado).
        auto saved_spill_slots = try_spill_slots_;
        for (const auto &name: assigned_in_try) {
            // Solo si no esta ya address-taken (otro mecanismo cubre).
            if (address_taken_locals_.count(name)) continue;
            // Alocar slot 8 bytes y STORE entry value.
            ir::IrValueId v_slot = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr   al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8;
            al.dst         = v_slot;
            al.imm         = 8;
            al.source_line = s->loc.line;
            fn_->append(current_block_, std::move(al));
            // STORE entry binding al slot (sera visible en catch via LOAD).
            auto it_e = entry_bindings.find(name);
            if (it_e != entry_bindings.end() && it_e->second != ir::IR_NO_VALUE) {
                // Usar el tipo real del valor (no i64 hardcoded) para
                // que la STORE coincida con la LOAD posterior y no haya
                // ambiguedad sobre los bytes altos del slot 8-byte.
                ir::IrType st_ty = ir::IrType::I64;
                if (it_e->second < fn_->values.size()) {
                    st_ty = fn_->values[it_e->second].type;
                }
                ir::IrInstr st{};
                st.op          = ir::IrOp::STORE;
                st.type        = st_ty;
                st.dst         = ir::IR_NO_VALUE;
                st.operands    = {it_e->second, v_slot};
                st.source_line = s->loc.line;
                fn_->append(current_block_, std::move(st));
            }
            try_spill_slots_[name] = v_slot;
        }

        // Multi-catch: cada catch tiene su propio tryenter ANTES del body.
        // El runtime apila los frames; do_throw los recorre desde el tope
        // (el ultimo apilado se prueba primero).  Para que el ORDEN
        // textual del codigo Vex se respete (catch[0] se prueba primero),
        // apilamos los catches en orden INVERSO: ultimo primero, primero
        // ultimo (queda en el tope).
        const size_t               n_catches = s->catches.size();
        std::vector<ir::IrBlockId> handler_bbs;
        handler_bbs.reserve(n_catches);
        for (size_t i = 0; i < n_catches; ++i) {
            handler_bbs.push_back(fn_->new_block("try_handler"));
        }
        const ir::IrBlockId body_bb  = fn_->new_block("try_body");
        const ir::IrBlockId merge_bb = fn_->new_block("try_merge");

        // 1. Por cada catch (en orden inverso): emitir handler_pc + type
        //    como SSA values con {dst}/{src} substitution, luego emitir
        //    tryenter con esos SSA values.  CRITICO: NO usar mov r1/r2
        //    hardcoded (destruiria SSA values vivos del caller, e.g. el
        //    target de un synchronized o cualquier variable local que el
        //    regalloc haya colocado en r1 o r2).
        for (size_t i = n_catches; i > 0; --i) {
            const size_t            ci            = i - 1;
            const ast::CatchClause &cc            = s->catches[ci];
            const std::string       handler_label =
                    fn_->name + "_" + fn_->blocks[handler_bbs[ci]].name;

            // type SSA value: 0 (catch-all) o findclass(name).
            ir::IrValueId v_type = ir::IR_NO_VALUE;
            if (cc.exc_class_name.empty()) {
                v_type = emit_const(ir::IrType::I64, 0, cc.loc.line);
            } else {
                // Sprint 5: emit_findclass_by_name reemplaza la RAW_ASM
                // textual con secuencia IR pura (ALLOCA + STORE + FINDCLASS).
                const uint64_t cls_idx = intern_class_name(*out_mod_, cc.exc_class_name);
                const uint32_t cls_len = static_cast<uint32_t>(cc.exc_class_name.size());
                v_type = emit_findclass_by_name(cls_idx, cls_len, cc.loc.line);
            }

            // 1b. handler_pc SSA value via LABEL_ADDR IR op (Sprint 3).
            const ir::IrValueId v_handler_pc = emit_label_addr(handler_label, cc.loc.line);

            // 1c. tryenter usando los dos SSA values.  El regalloc los
            // coloca en regs disponibles (NO clobbers SSA values vivos).
            {
                ir::IrInstr ra{};
                ra.op          = ir::IrOp::TRYENTER;
                ra.type        = ir::IrType::VOID;
                ra.dst         = ir::IR_NO_VALUE;
                ra.operands    = {v_handler_pc, v_type};
                ra.source_line = cc.loc.line;
                fn_->append(current_block_, std::move(ra));
            }
        }

        // br body_bb.
        ir::IrInstr br_to_body{};
        br_to_body.op           = ir::IrOp::BR;
        br_to_body.target_block = body_bb;
        br_to_body.source_line  = s->loc.line;
        fn_->append(current_block_, std::move(br_to_body));
        fn_->blocks[current_block_].succs.push_back(body_bb);
        fn_->blocks[body_bb].preds.push_back(current_block_);
        // Edges fantasmas a cada handler (alcanzables via excepcion).
        for (ir::IrBlockId hb: handler_bbs) {
            fn_->blocks[current_block_].succs.push_back(hb);
            fn_->blocks[hb].preds.push_back(current_block_);
        }

        // Helper local: emite el bloque finally (clonado en cada salida) y
        // luego un branch al merge.  El finally en MVP se INLINEA en cada
        // exit point; alternativa mas eficiente es un bloque compartido
        // con tabla de continuaciones (fuera de scope).
        auto emit_finally_then_merge = [&](ir::IrBlockId from,
                                           uint32_t      line) {
            (void) from;
            if (s->finally_body) {
                lower_stmt(s->finally_body.get());
                if (block_terminated_) return; // finally returned/threw
            }
            ir::IrInstr brm{};
            brm.op           = ir::IrOp::BR;
            brm.target_block = merge_bb;
            brm.source_line  = line;
            fn_->append(current_block_, std::move(brm));
            fn_->blocks[current_block_].succs.push_back(merge_bb);
            fn_->blocks[merge_bb].preds.push_back(current_block_);
            block_terminated_ = true;
        };

        // 2. Body del try.
        current_block_    = body_bb;
        block_terminated_ = false;
        lower_stmt(s->body.get());

        // Snapshot post-body para PHI.  Si el body alcanza el
        // merge (no terminado por return/throw/break), guardamos su
        // binding final para cada variable que diferia del entry.
        std::unordered_map<std::string, ir::IrValueId> body_bindings;
        ir::IrBlockId                                  body_pred          = ir::IR_NO_BLOCK;
        bool                                           body_reaches_merge = false;
        if (!block_terminated_) {
            body_bindings      = scopes_.back();
            body_pred          = current_block_;
            body_reaches_merge = true;

            // Sprint 6.D: tryleave por cada catch via IR ops puros.
            for (size_t i = 0; i < n_catches; ++i) {
                ir::IrInstr tl{};
                tl.op          = ir::IrOp::TRYLEAVE;
                tl.type        = ir::IrType::VOID;
                tl.dst         = ir::IR_NO_VALUE;
                tl.source_line = s->loc.line;
                fn_->append(current_block_, std::move(tl));
            }
            emit_finally_then_merge(current_block_, s->loc.line);
        }

        // NO resetear scopes_.back() = entry_bindings.  Esa operacion creaba
        // shadows de outer-scope vars (e.g. el contador @c j de un while loop
        // enclosing) en el inner scope, lo que rompia el back-edge phi del
        // loop: cuando el inner scope se pop'aba, las actualizaciones de outer
        // vars hechas DESPUES del try se perdian.
        //
        // El reset originalmente intentaba que cada catch viera el "estado
        // entry" de las vars.  Eso esta garantizado por OTROS mecanismos:
        //   - Vars asignadas en try o leidas en catch estan en try_spill_slots_
        //     y se reload via LOAD desde el slot en el catch entry.
        //   - Vars no tocadas en try mantienen su valor entry (ningun
        //     update_scope se llamo para ellas).
        // Asi el reset era redundante para vars relevantes y daninyo para
        // vars outer-scope no relacionadas.

        // 3. Handlers (uno por catch).  do_throw consume el frame del
        //    catch que captura.  Los catches MAS EXTERNOS (declarados
        //    despues en source) tambien ya se consumieron porque el
        //    matching va de arriba hacia abajo en exc_frame_stack y al
        //    saltar al handler ese frame se elimina.  Pero los catches
        //    MAS INTERNOS (declarados antes) podrian seguir en la pila
        //    si no fueron el match.  Para simplificar: dentro del handler
        //    no necesitamos re-pop porque do_throw ya consumio el frame
        //    matched, y los demas frames eran adicionales (catches
        //    posteriores no relacionados).  En el modelo actual cada
        //    catch tiene un tryenter independiente por lo que el
        //    do_throw consume exactamente uno; los otros siguen vivos
        //    hasta que terminemos el bloque try.  Solucion: emitir
        //    `tryleave` por cada catch RESTANTE en el handler.
        // Por cada catch, capturamos: post-bindings (para PHI)
        // + pred final + flag de alcance del merge.  Tras lower del catch
        // restauramos el scope a entry_bindings para que el siguiente
        // catch (o el merge) parta de un estado limpio.
        struct CatchSnapshot {
            std::unordered_map<std::string, ir::IrValueId> bindings;
            ir::IrBlockId                                  pred;
            bool                                           reaches_merge;
        };
        std::vector<CatchSnapshot> catch_snaps;
        catch_snaps.reserve(n_catches);

        for (size_t ci = 0; ci < n_catches; ++ci) {
            const ast::CatchClause &cc = s->catches[ci];
            current_block_             = handler_bbs[ci];
            block_terminated_          = false;
            // NO reset a entry_bindings (creaba shadows -- ver explicacion arriba).
            // El catch reload via LOAD desde spill slots se encarga de restaurar
            // las vars relevantes al valor entry.
            // Pop tryenters restantes (todos excepto el que se consumio).
            // Los restantes son: todos excepto el del catch[ci].
            // Como tryenter se apilaron en orden INVERSO (catch[n-1] en el
            // fondo, catch[0] en el tope), el frame consumido por
            // do_throw para catch[ci] es el ci-esimo desde el tope.
            // Frames POR ENCIMA (mas recientes que ci) ya fueron
            // descartados por do_throw mientras buscaba el match.
            // Frames POR DEBAJO (anteriores a ci) siguen vivos: hay que
            // popearlos.  Numero a popear: n_catches - 1 - ci.
            const size_t to_pop = n_catches - 1 - ci;
            // Sprint 6.D: tryleave por catch via IR ops puros.
            for (size_t k = 0; k < to_pop; ++k) {
                ir::IrInstr tl{};
                tl.op          = ir::IrOp::TRYLEAVE;
                tl.type        = ir::IrType::VOID;
                tl.dst         = ir::IR_NO_VALUE;
                tl.source_line = cc.loc.line;
                fn_->append(current_block_, std::move(tl));
            }
            push_scope();
            // CRITICO: la primera instruccion del catch debe ser el
            // `mov {dst}, r0` que captura la excepcion -- r0 lleva el
            // puntero al FatalError y CUALQUIER instruccion previa
            // (LOAD desde stack, etc.) puede clobrearlo.
            if (!cc.var_name.empty()) {
                const ir::IrValueId v_exc = fn_->new_value(ir::IrType::PTR);
                fn_->values[v_exc].is_host_ptr = true; // catch recibe FatalError* host
                ir::IrInstr         lp{};
                lp.op          = ir::IrOp::LANDINGPAD;
                lp.type        = ir::IrType::PTR;
                lp.dst         = v_exc;
                lp.source_line = cc.loc.line;
                fn_->append(current_block_, std::move(lp));
                bind(cc.var_name, v_exc);
            }
            // Recargar TODOS los nombres spilled desde su slot DESPUES
            // del bind de la excepcion (para no clobrear r0).  Bindeamos
            // en el scope OUTER (no en el inner del catch) para que:
            //   1. El write_local del catch body actualice ese scope.
            //   2. El binding sobreviva al pop_scope siguiente.
            //   3. La rama finally+merge vea el ultimo valor.
            // Sin esto, las vars spilled (incluido `this` y parametros)
            // quedaban con valor stale tras el throw -- el regalloc
            // clobreaba sus registros durante el unwind.
            // Buscar el scope outer (penultimo); el inner es el catch
            // que acabamos de pushear.
            std::unordered_map<std::string, ir::IrValueId> *outer_scope =
                (scopes_.size() >= 2) ? &scopes_[scopes_.size() - 2]
                                      : &scopes_.back();
            for (const auto &kv: try_spill_slots_) {
                const std::string & name   = kv.first;
                const ir::IrValueId v_slot = kv.second;
                if (saved_spill_slots.count(name)
                    && saved_spill_slots.at(name) == v_slot) {
                    continue;  // slot heredado de try outer
                }
                ir::IrType ity = ir::IrType::I64;
                auto it_e = entry_bindings.find(name);
                if (it_e != entry_bindings.end()
                    && it_e->second != ir::IR_NO_VALUE
                    && it_e->second < fn_->values.size()) {
                    ity = fn_->values[it_e->second].type;
                }
                ir::IrValueId v_load = fn_->new_value(ity);
                ir::IrInstr   ld{};
                ld.op          = ir::IrOp::LOAD;
                // Usar el tipo REAL del valor (no i64 hardcoded).  Si
                // el value era i32 pero leemos i64, los bytes altos del
                // slot 8-byte alloca son basura no inicializada (la
                // STORE inicial solo escribio 4 bytes) y corrompen la
                // aritmetica posterior.
                ld.type        = ity;
                ld.dst         = v_load;
                ld.operands    = {v_slot};
                ld.source_line = cc.loc.line;
                fn_->append(current_block_, std::move(ld));
                // Propagar is_host_ptr / is_gc_object del entry_value
                // si lo tenia, para que el catch maneje host pointers
                // y GC objects correctamente sin perder los flags.
                if (it_e != entry_bindings.end()
                    && it_e->second != ir::IR_NO_VALUE
                    && it_e->second < fn_->values.size()) {
                    const auto &src_val = fn_->values[it_e->second];
                    fn_->values[v_load].is_host_ptr =
                        src_val.is_host_ptr;
                    fn_->values[v_load].is_gc_object =
                        src_val.is_gc_object;
                    fn_->values[v_load].pointee_is_host_ptr =
                        src_val.pointee_is_host_ptr;
                }
                // Buscar el scope (NO el inner del catch) donde el name
                // vive y actualizarlo.  Si esta en multiples niveles,
                // actualizamos el mas cercano al exterior (sin tocar
                // el inner del catch).
                bool updated = false;
                if (scopes_.size() >= 2) {
                    for (auto it = scopes_.rbegin() + 1; it != scopes_.rend(); ++it) {
                        if (it->count(name)) {
                            (*it)[name] = v_load;
                            updated = true;
                            break;
                        }
                    }
                }
                if (!updated) {
                    (*outer_scope)[name] = v_load;
                }
            }
            if (cc.body) lower_stmt(cc.body.get());
            pop_scope();
            // capturar snapshot del scope antes de pop_scope NO
            // porque queremos las modificaciones de variables del scope
            // ENCLOSING (no las del catch var, que vivian en el inner
            // scope ya popeado).
            CatchSnapshot snap;
            snap.reaches_merge = !block_terminated_;
            if (snap.reaches_merge) {
                snap.bindings = scopes_.back();
                snap.pred     = current_block_;
                emit_finally_then_merge(current_block_, cc.loc.line);
            } else {
                snap.pred = ir::IR_NO_BLOCK;
            }
            catch_snaps.push_back(std::move(snap));
        }

        // 4. Merge + PHI.
        current_block_    = merge_bb;
        block_terminated_ = false;
        // NO RESETEAMOS scopes_.back() a entry_bindings.  Esa operacion
        // creaba sombras de variables outer-scope dentro del inner scope
        // del while-body / for-body / etc, lo que rompia loops con
        // try/catch dentro: cuando el inner scope se desapilaba al cierre
        // del body, las actualizaciones de las vars outer (e.g. el
        // contador de iteracion @c j en @c j=j+1) se perdian.  El PHI
        // back-edge subsequent tomaba el valor STALE del outer scope,
        // creando un self-loop @c j = phi[entry, j] -> loop infinito.
        //
        // En su lugar, propagamos las actualizaciones via @c update_scope
        // que walks scopes inside-out y actualiza el binding en su scope
        // nativo.  Asi outer-scope vars se actualizan en outer, inner-scope
        // vars (e.g. catch var) en inner.  Sin shadowing.

        // Coleccionar todos los predecesores que alcanzan el merge.
        // Cada uno aporta su binding final para cada variable.
        struct MergeContrib {
            ir::IrBlockId                                         pred;
            const std::unordered_map<std::string, ir::IrValueId> *bindings;
        };
        std::vector<MergeContrib> contribs;
        contribs.reserve(1 + n_catches);
        if (body_reaches_merge) {
            contribs.push_back({body_pred, &body_bindings});
        }
        for (auto &cs: catch_snaps) {
            if (cs.reaches_merge) {
                contribs.push_back({cs.pred, &cs.bindings});
            }
        }
        // Solo necesitamos PHI si >= 2 predecesores aportan al merge
        // y la variable difiere entre alguno de ellos y el entry.
        if (contribs.size() >= 2) {
            for (const auto &kv: entry_bindings) {
                const std::string & name      = kv.first;
                const ir::IrValueId entry_val = kv.second;
                // Detectar si algun pred difiere del entry.
                bool any_diff = false;
                for (const auto &c: contribs) {
                    auto it = c.bindings->find(name);
                    if (it == c.bindings->end()) continue;
                    if (it->second != entry_val) {
                        any_diff = true;
                        break;
                    }
                }
                if (!any_diff) continue;

                // Construir PHI: por cada pred, su binding (entry si
                // el pred no tiene binding actualizado).
                ir::IrType ity = ir::IrType::I64;
                if (entry_val != ir::IR_NO_VALUE
                    && entry_val < fn_->values.size()) {
                    ity = fn_->values[entry_val].type;
                }
                ir::IrValueId v_phi = fn_->new_value(ity);
                ir::IrInstr   phi{};
                phi.op          = ir::IrOp::PHI;
                phi.type        = ity;
                phi.dst         = v_phi;
                phi.source_line = s->loc.line;
                for (const auto &c: contribs) {
                    auto          it     = c.bindings->find(name);
                    ir::IrValueId in_val = (it != c.bindings->end())
                                               ? it->second
                                               : entry_val;
                    ir::IrPhiArg arg{};
                    arg.value = in_val;
                    arg.block = c.pred;
                    phi.phi_args.push_back(arg);
                }
                // Insertar al INICIO del merge_bb (PHI siempre al tope).
                fn_->blocks[merge_bb].instrs.insert(
                    fn_->blocks[merge_bb].instrs.begin(),
                    std::move(phi));
                // Update in-place en el scope NATIVO de la variable, no
                // en scopes_.back() (que creaba shadow + perdia el update
                // al pop_scope del while/for body).  Si la var no esta en
                // ningun scope (debio ser declarada en outer pero por algun
                // motivo no aparece), fallback a scopes_.back() crea binding
                // local -- aceptable.
                update_scope(name, v_phi);
            }
        } else if (contribs.size() == 1) {
            // Solo un predecesor alcanza el merge: copiar SOLO los
            // bindings que difieren del entry, y propagarlos a su scope
            // nativo via update_scope (no shadow en scopes_.back()).
            for (const auto &kv: *contribs[0].bindings) {
                auto it_entry = entry_bindings.find(kv.first);
                if (it_entry == entry_bindings.end()
                 || it_entry->second != kv.second) {
                    update_scope(kv.first, kv.second);
                }
            }
        }

        // LOAD del slot de cada var spilled y bind al nombre.
        // Esto OVERRIDES el PHI o single-pred binding: el slot tiene la
        // verdad mas reciente independientemente de regalloc.  Necesario
        // porque el throw + RSP unwind puede dejar registros corruptos.
        for (const auto &kv: try_spill_slots_) {
            const std::string & name   = kv.first;
            const ir::IrValueId v_slot = kv.second;
            // Solo override para vars que estaban en el slot ESTE try
            // (no las del saved_spill_slots de un try outer).
            if (saved_spill_slots.count(name)
                && saved_spill_slots.at(name) == v_slot) {
                continue;
            }
            ir::IrType ity  = ir::IrType::I64;
            auto       it_e = entry_bindings.find(name);
            if (it_e != entry_bindings.end()
                && it_e->second != ir::IR_NO_VALUE
                && it_e->second < fn_->values.size()) {
                ity = fn_->values[it_e->second].type;
            }
            ir::IrValueId v_load = fn_->new_value(ity);
            ir::IrInstr   ld{};
            ld.op          = ir::IrOp::LOAD;
            // Usar el tipo real del valor (no i64 hardcoded) para
            // evitar leer bytes basura del slot 8-byte alloca.
            ld.type        = ity;
            ld.dst         = v_load;
            ld.operands    = {v_slot};
            ld.source_line = s->loc.line;
            fn_->append(current_block_, std::move(ld));
            // Propagar flags is_host_ptr/is_gc_object del entry value.
            if (it_e != entry_bindings.end()
                && it_e->second != ir::IR_NO_VALUE
                && it_e->second < fn_->values.size()) {
                const auto &src_val = fn_->values[it_e->second];
                fn_->values[v_load].is_host_ptr =
                    src_val.is_host_ptr;
                fn_->values[v_load].is_gc_object =
                    src_val.is_gc_object;
                fn_->values[v_load].pointee_is_host_ptr =
                    src_val.pointee_is_host_ptr;
            }
            // Update en scope nativo via update_scope (no shadow).
            // Mismo razonamiento que el merge PHI arriba: la sombra
            // en scopes_.back() se perdia al pop_scope del while body
            // y dejaba al back-edge del while leyendo el binding stale
            // del outer scope.
            update_scope(name, v_load);
        }

        // Restaurar try_spill_slots_ del nivel exterior.
        try_spill_slots_ = std::move(saved_spill_slots);
    }

    // ---------------------------------------------------------------------
    // foreach: for (T x : col) body  -- desazucarado a counted loop.
    //
    // Patron sintetizado para `for (T x : arr) body` con arr: T[N]:
    //   {
    //     i32 __idx = 0;
    //     while (__idx < N) {
    //       T x = arr[__idx];
    //       body
    //       __idx = __idx + 1;
    //     }
    //   }
    //
    // Para requerimos N conocido en compile time (T[N]).  Para
    // T[] (decay-to-pointer) sin tamano se necesita un parametro de
    // longitud explicito o un objeto Array<T> managed (deferido).
    // ---------------------------------------------------------------------
    void Lowering::lower_foreach(ast::ForEachStmt *s) {
        if (!s->iter_expr) {
            error_at(s->loc, "lowering: foreach sin coleccion");
            return;
        }
        // El tipo del iter_expr debe ser ARRAY[N].  Lo extraemos del
        // result_type que el type checker fija.
        const Type col_t = s->iter_expr->result_type;
        if (col_t.kind != PrimitiveKind::ARRAY) {
            error_at(s->loc, "lowering: foreach requiere un array (no se soporta T[] sin tamano en MVP)");
            return;
        }
        if (col_t.array_size == 0) {
            error_at(s->loc, "lowering: foreach requiere un array con tamano fijo T[N]");
            return;
        }
        const uint64_t N = static_cast<uint64_t>(col_t.array_size);

        // Sintetizamos el cuerpo extendido como AST nuevo y lo bajamos
        // via lower_stmt (mas robusto que generar IR a mano y permite
        // reusar todas las optimizaciones).  Usamos nombres "__"
        // prefijados para evitar choque con identificadores del usuario.

        auto block = std::make_unique<ast::BlockStmt>();
        block->loc = s->loc;

        // i64 __idx = 0;
        auto idx_decl = std::make_unique<ast::VarDeclStmt>();
        idx_decl->loc = s->loc; {
            auto pt        = std::make_unique<ast::PrimitiveTypeNode>();
            pt->loc        = s->loc;
            pt->prim       = PrimitiveKind::I64;
            idx_decl->type = std::move(pt);
            idx_decl->name = "__fe_idx";
            auto z         = std::make_unique<ast::IntLitExpr>();
            z->loc         = s->loc;
            z->value       = 0;
            z->result_type = Type{PrimitiveKind::I64};
            idx_decl->init = std::move(z);
        }
        block->body.push_back(std::move(idx_decl));

        // while (__fe_idx < N) { ... }
        auto while_stmt = std::make_unique<ast::WhileStmt>();
        while_stmt->loc = s->loc; {
            auto cond         = std::make_unique<ast::BinaryExpr>();
            cond->loc         = s->loc;
            cond->op          = ast::BinOp::Lt;
            auto lhs          = std::make_unique<ast::IdentExpr>();
            lhs->loc          = s->loc;
            lhs->name         = "__fe_idx";
            lhs->result_type  = Type{PrimitiveKind::I64};
            auto rhs          = std::make_unique<ast::IntLitExpr>();
            rhs->loc          = s->loc;
            rhs->value        = N;
            rhs->result_type  = Type{PrimitiveKind::I64};
            cond->lhs         = std::move(lhs);
            cond->rhs         = std::move(rhs);
            cond->result_type = Type{PrimitiveKind::BOOL};
            while_stmt->cond  = std::move(cond);
        }
        // body interno: { T x = col[__fe_idx]; user_body; __fe_idx = __fe_idx+1; }
        auto inner_block = std::make_unique<ast::BlockStmt>();
        inner_block->loc = s->loc;
        // T x = col[__fe_idx];
        const Type elem_t = (col_t.pointee != nullptr)
                                ? *col_t.pointee
                                : Type{PrimitiveKind::I64}; {
            auto vd  = std::make_unique<ast::VarDeclStmt>();
            vd->loc  = s->loc;
            auto pt  = std::make_unique<ast::PrimitiveTypeNode>();
            pt->loc  = s->loc;
            pt->prim = elem_t.kind;
            vd->type = std::move(pt);
            vd->name = s->iter_name;
            // expr: col[__fe_idx].  IMPORTANTE: rellenamos result_type a
            // mano porque este nodo no pasa por check_expr.  Sin esto el
            // LOAD del lowering usaria size por defecto (8 bytes) y
            // leeria mas alla del slot del array.
            auto idx_expr         = std::make_unique<ast::IndexExpr>();
            idx_expr->loc         = s->loc;
            idx_expr->result_type = elem_t;
            // base: cloning iter_expr is delicate (unique_ptr); the simplest
            // is moving it INTO the desugared tree.  iter_expr ya no se
            // usara mas alla de este lowering, asi que es seguro mover.
            idx_expr->base = std::move(s->iter_expr);
            // El base preservaba su result_type (ARRAY[N] o PTR) ya
            // resuelto por el type checker.
            auto idx_id         = std::make_unique<ast::IdentExpr>();
            idx_id->loc         = s->loc;
            idx_id->name        = "__fe_idx";
            idx_id->result_type = Type{PrimitiveKind::I64};
            idx_expr->index     = std::move(idx_id);
            vd->init            = std::move(idx_expr);
            inner_block->body.push_back(std::move(vd));
        }
        // user body
        if (s->body) inner_block->body.push_back(std::move(s->body));
        // __fe_idx = __fe_idx + 1;
        {
            auto inc_stmt       = std::make_unique<ast::ExprStmt>();
            inc_stmt->loc       = s->loc;
            auto assign         = std::make_unique<ast::AssignExpr>();
            assign->loc         = s->loc;
            assign->op          = ast::AssignOp::Assign;
            auto target         = std::make_unique<ast::IdentExpr>();
            target->loc         = s->loc;
            target->name        = "__fe_idx";
            target->result_type = Type{PrimitiveKind::I64};
            assign->target      = std::move(target);
            auto add            = std::make_unique<ast::BinaryExpr>();
            add->loc            = s->loc;
            add->op             = ast::BinOp::Add;
            auto a_lhs          = std::make_unique<ast::IdentExpr>();
            a_lhs->loc          = s->loc;
            a_lhs->name         = "__fe_idx";
            a_lhs->result_type  = Type{PrimitiveKind::I64};
            auto a_rhs          = std::make_unique<ast::IntLitExpr>();
            a_rhs->loc          = s->loc;
            a_rhs->value        = 1;
            a_rhs->result_type  = Type{PrimitiveKind::I64};
            add->lhs            = std::move(a_lhs);
            add->rhs            = std::move(a_rhs);
            add->result_type    = Type{PrimitiveKind::I64};
            assign->value       = std::move(add);
            assign->result_type = Type{PrimitiveKind::I64};
            inc_stmt->expr      = std::move(assign);
            inner_block->body.push_back(std::move(inc_stmt));
        }
        while_stmt->body = std::move(inner_block);
        block->body.push_back(std::move(while_stmt));

        // Bajar el bloque sintetico.  Como las expresiones internas no
        // pasaron por check_expr, sus result_type pueden estar incompletos
        // (especialmente IndexExpr).  Para minimizar fallos, marcamos
        // los tipos manualmente arriba; el lowering tolera ausencia de
        // result_type en ciertos casos.
        lower_stmt(block.get());
    }

    // ---------------------------------------------------------------------
    // emit_cleanups_all: emite todos los cleanups activos en orden inverso
    // (mas reciente primero).  No modifica el stack: el caller (e.g.,
    // lower_synchronized) hace su pop por flujo normal.
    // ---------------------------------------------------------------------
    void Lowering::emit_cleanups_all() {
        emit_cleanups_range(0, cleanup_stack_.size());
    }

    void Lowering::emit_cleanups_range(size_t start, size_t end) {
        if (end > cleanup_stack_.size()) end = cleanup_stack_.size();
        if (start >= end) return;
        // Recorrer [start, end) en orden INVERSO (LIFO).  El cleanup mas
        // reciente (top del stack) se ejecuta primero, igual que destructores
        // C++ en el orden inverso a su construccion.
        for (size_t k = end; k-- > start;) {
            const CleanupAction *      it    = &cleanup_stack_[k];
            std::vector<ir::IrValueId> opnds = it->operands;
            // refresh: sustituir operands[0] con el binding ACTUAL
            // del local (permite dispose(xs)+cleanup idempotente, etc.).
            if (!it->refresh_name.empty() && !opnds.empty()) {
                const ir::IrValueId v_now = lookup(it->refresh_name);
                if (v_now != ir::IR_NO_VALUE) {
                    opnds[0] = v_now;
                }
            }
            switch (it->kind) {
                case CleanupAction::Kind::CALL_DTOR: {
                    // emitir CALLVIRT real para que el regalloc lo
                    // trate como CALL y preserve regs caller-saved vivos
                    // (especialmente el reg que lleva v_ret en lower_return).
                    ir::IrInstr cv{};
                    cv.op          = ir::IrOp::CALLVIRT;
                    cv.type        = ir::IrType::VOID;
                    cv.dst         = ir::IR_NO_VALUE;
                    cv.operands    = std::move(opnds);
                    cv.imm         = static_cast<uint64_t>(it->dtor_vtable_index);
                    cv.source_line = it->source_line;
                    fn_->append(current_block_, std::move(cv));
                    break;
                }
                case CleanupAction::Kind::RAW_ASM: {
                    // raw_asm-elim wave 2: dead code.  Todas las creaciones
                    // de CleanupAction setean su @c kind explicitamente a un
                    // valor especifico (CALL_DTOR/CALLN_FREE/SMARTPTR_FREE/
                    // SHAREDPTR_REL/SYNC_EXIT).  Si esta rama se alcanza, es
                    // un bug del frontend que olvido setear el kind; emitir
                    // diagnostico claro en lugar de raw_asm opaco.
                    error_at(SourceLoc{"", it->source_line, 1},
                        "internal: CleanupAction con Kind::RAW_ASM (default) "
                        "alcanzado al exit del scope; el frontend debe setear "
                        "un kind especifico (CALL_DTOR/CALLN_FREE/SMARTPTR_FREE/etc.)");
                    break;
                }
                case CleanupAction::Kind::SYNC_EXIT: {
                    // Sprint 6.C: tryleave + monexit como IR ops puros.
                    {
                        ir::IrInstr tl{};
                        tl.op          = ir::IrOp::TRYLEAVE;
                        tl.type        = ir::IrType::VOID;
                        tl.dst         = ir::IR_NO_VALUE;
                        tl.source_line = it->source_line;
                        fn_->append(current_block_, std::move(tl));
                    }
                    if (!opnds.empty()) {
                        ir::IrInstr me{};
                        me.op          = ir::IrOp::MONEXIT;
                        me.type        = ir::IrType::VOID;
                        me.dst         = ir::IR_NO_VALUE;
                        me.operands    = {opnds[0]};
                        me.source_line = it->source_line;
                        fn_->append(current_block_, std::move(me));
                    }
                    break;
                }
                case CleanupAction::Kind::CALLN_FREE: {
                    // CALLN al free nativo de la coleccion (variante GC
                    // o no-GC).  Para la variante *_gc prependemos un
                    // GETPROC como primer argumento; el
                    // regalloc trata el CALLN como call normal y preserva
                    // los regs vivos del caller.
                    std::vector<ir::IrValueId> args;
                    if (it->needs_proc) {
                        args.reserve(opnds.size() + 1);
                        args.push_back(emit_getproc(it->source_line));
                    } else {
                        args.reserve(opnds.size());
                    }
                    for (auto vid: opnds) args.push_back(vid);
                    ir::IrInstr cf{};
                    cf.op          = ir::IrOp::CALLN;
                    cf.type        = ir::IrType::VOID;
                    cf.dst         = ir::IR_NO_VALUE;
                    cf.func_name   = it->func_name;
                    cf.operands    = std::move(args);
                    cf.source_line = it->source_line;
                    fn_->append(current_block_, std::move(cf));
                    break;
                }
                case CleanupAction::Kind::SMARTPTR_FREE: {
                    // Cleanup de @c unique<T> en scope exit.
                    //
                    // Tier 1 layout: slot[+0]=ptr, slot[+8]=deleter_addr.
                    //   deleter_addr == 0 -> sentinel: RAW_FREE(ptr).
                    //   deleter_addr != 0 -> CALLVMR(deleter_addr, ptr).
                    //
                    // Si literal_deleter esta poblado (caso comun:
                    // var-decl con init = unique_box/unique_with), usamos
                    // ese conocimiento compile-time para emitir el cleanup
                    // mas eficiente (RAW_FREE directo, CALLVM @Absolute
                    // fijo, o CALLN @Method para extern wrappers).
                    //
                    // Si NO esta poblado (caso SRET: el unique vino de
                    // una funcion que lo creo internamente), leemos el
                    // deleter_addr del slot+8 y dispatchamos dinamicamente.
                    const ir::IrValueId v_ptr = fn_->new_value(ir::IrType::PTR);
                    fn_->values[v_ptr].is_host_ptr = true;
                    {
                        ir::IrInstr ld{};
                        ld.op          = ir::IrOp::LOAD;
                        ld.type        = ir::IrType::I64;
                        ld.dst         = v_ptr;
                        ld.operands    = opnds;  // [v_slot]
                        ld.source_line = it->source_line;
                        fn_->append(current_block_, std::move(ld));
                    }

                    // Bug fix bug2: si el inner T es una CLASS Vex con
                    // destructor, invocar `~T()` ANTES del free.  El
                    // CALLVIRT requiere host_ptr no nulo; emitimos guard
                    // implicito via skip si v_ptr == 0 (no debe ocurrir
                    // tras unique_box(new T()), pero defensive).
                    //
                    // Bug fix adicional: si inner_is_gc_class, el host_ptr
                    // que vive en el slot apunta a un objeto GC-managed
                    // (no a RAW_ALLOC memory).  Hacer RAW_FREE corromperia
                    // el heap.  Solo invocamos el destructor + dejamos
                    // que el GC libere el objeto cuando ningun root lo
                    // referencie (stack scanning A.34.fix8).
                    if (it->inner_dtor_vtable_index > 0) {
                        ir::IrInstr cv{};
                        cv.op          = ir::IrOp::CALLVIRT;
                        cv.type        = ir::IrType::VOID;
                        cv.dst         = ir::IR_NO_VALUE;
                        cv.operands    = {v_ptr};
                        cv.imm         = static_cast<uint64_t>(it->inner_dtor_vtable_index);
                        cv.source_line = it->source_line;
                        fn_->append(current_block_, std::move(cv));
                    }
                    if (it->inner_is_gc_class) {
                        // El objeto inner es GC-managed: NO hacer RAW_FREE
                        // del host_ptr (el GC se encarga del inner object).
                        // El SLOT (raw-alloced de 8/16 bytes) tambien necesita
                        // liberarse, pero usa slot_addr (opnds[0]) no v_ptr.
                        // Sin embargo, el slot RAW_ALLOC vive solo si fue
                        // unique_box (que sigue siendo Tier 0 sin slot RAW).
                        // En Tier 1 el slot es ALLOCA stack (no requiere free).
                        // Por simplicidad: skip el free completo en este caso.
                        // El GC libera el inner; el ALLOCA stack se libera
                        // al exit del frame automaticamente.
                        break;
                    }

                    if (it->literal_deleter.empty()) {
                        // SRET case: el smart pointer vino de una funcion
                        // (factory).  No tenemos info compile-time del
                        // deleter; lo leemos dinamicamente del slot+8.
                        // Si deleter_addr == 0 -> RAW_FREE; si != 0 ->
                        // callvmr al puntero (deleter Vesta).
                        const ir::IrValueId v_eight  = emit_const(ir::IrType::I64, 8, it->source_line);
                        const ir::IrValueId v_slot8  = fn_->new_value(ir::IrType::PTR);
                        {
                            ir::IrInstr add{};
                            add.op          = ir::IrOp::ADD;
                            add.type        = ir::IrType::I64;
                            add.dst         = v_slot8;
                            add.operands    = {opnds[0], v_eight};
                            add.source_line = it->source_line;
                            fn_->append(current_block_, std::move(add));
                        }
                        const ir::IrValueId v_del = fn_->new_value(ir::IrType::I64);
                        {
                            ir::IrInstr ldd{};
                            ldd.op          = ir::IrOp::LOAD;
                            ldd.type        = ir::IrType::I64;
                            ldd.dst         = v_del;
                            ldd.operands    = {v_slot8};
                            ldd.source_line = it->source_line;
                            fn_->append(current_block_, std::move(ldd));
                        }
                        const uint32_t lbl = ++cleanup_label_seq_;
                        const std::string default_lbl = "__sp_def_" + std::to_string(lbl);
                        const std::string skip_lbl    = "__sp_skip_" + std::to_string(lbl);
                        const std::string done_lbl    = "__sp_done_" + std::to_string(lbl);
                        // cmpu ptr, 0; jmp.je done  (skip si moved)
                        // cmpu deleter, 0; jmp.je default  (deleter=0 -> RAW_FREE)
                        // mov r1, ptr; mov r15, 1; callvmr deleter; jmp done
                        // default: mov r1, ptr; (RAW_FREE inline)
                        // raw_asm-elim wave 2: SMARTPTR_FREE kind=0 (SRET_DISPATCH).
                        // El emitter expande a la secuencia equivalente con
                        // labels unicos via contador thread-local; mismo
                        // bytecode emitido.  Eliminamos done_lbl/default_lbl
                        // ya que el emitter los genera internamente.
                        ir::IrInstr sf{};
                        sf.op           = ir::IrOp::SMARTPTR_FREE;
                        sf.type         = ir::IrType::VOID;
                        sf.dst          = ir::IR_NO_VALUE;
                        sf.operands     = {v_ptr, v_del};
                        sf.imm          = 0;   /* SRET_DISPATCH */
                        sf.source_line  = it->source_line;
                        sf.set_is_call_site(true);
                        fn_->append(current_block_, std::move(sf));
                        (void)done_lbl; (void)default_lbl; /* labels no usadas (emitter las genera) */
                    } else if (it->literal_deleter == "free") {
                        // Deleter por defecto: RAW_FREE (null-safe).
                        ir::IrInstr fr{};
                        fr.op          = ir::IrOp::RAW_FREE;
                        fr.type        = ir::IrType::VOID;
                        fr.dst         = ir::IR_NO_VALUE;
                        fr.operands    = {v_ptr};
                        fr.source_line = it->source_line;
                        fn_->append(current_block_, std::move(fr));
                    } else if (it->literal_deleter.rfind("@extern:", 0) == 0) {
                        // raw_asm-elim wave 2: SMARTPTR_FREE kind=1 (EXTERN_CALLN).
                        const std::string fn_label =
                            it->literal_deleter.substr(8);  // skip "@extern:"
                        ir::IrInstr sf{};
                        sf.op           = ir::IrOp::SMARTPTR_FREE;
                        sf.type         = ir::IrType::VOID;
                        sf.dst          = ir::IR_NO_VALUE;
                        sf.operands     = {v_ptr};
                        sf.imm          = 1;   /* EXTERN_CALLN */
                        sf.func_name    = fn_label;  /* "<lib>:<fn>" */
                        sf.source_line  = it->source_line;
                        sf.set_is_call_site(true);
                        fn_->append(current_block_, std::move(sf));
                    } else {
                        // raw_asm-elim wave 2: SMARTPTR_FREE kind=2 (VESTA_CALLVM).
                        ir::IrInstr sf{};
                        sf.op           = ir::IrOp::SMARTPTR_FREE;
                        sf.type         = ir::IrType::VOID;
                        sf.dst          = ir::IR_NO_VALUE;
                        sf.operands     = {v_ptr};
                        sf.imm          = 2;   /* VESTA_CALLVM */
                        sf.func_name    = it->literal_deleter;  /* "<fn_label>" */
                        sf.source_line  = it->source_line;
                        sf.set_is_call_site(true);
                        fn_->append(current_block_, std::move(sf));
                    }
                    break;
                }
                case CleanupAction::Kind::SHAREDPTR_REL: {
                    // Sprint 6.C: cleanup de @c shared<T> via IR ops puros.
                    //
                    // Implementacion: LOAD ctrl; si ctrl != 0, LOAD rc; SUB 1; STORE rc.
                    // No emitimos free explicito porque el GcHeap se encarga
                    // de liberar bloques sin roots cuando se ejecuta major_gc.
                    //
                    // Antes: 7 lineas de RAW_ASM con jmp.je por label.
                    // Ahora: 7 IR ops (LOAD + CMP + BR_COND + 2 bloques + LOAD + SUB + STORE).
                    if (opnds.empty()) break;
                    const ir::IrValueId v_slot = opnds[0];
                    // ctrl = LOAD i64 [v_slot]   (host_ptr al control block).
                    const ir::IrValueId v_ctrl = fn_->new_value(ir::IrType::PTR);
                    fn_->values[v_ctrl].is_host_ptr = true;
                    {
                        ir::IrInstr ld{};
                        ld.op          = ir::IrOp::LOAD;
                        ld.type        = ir::IrType::I64;
                        ld.dst         = v_ctrl;
                        ld.operands    = {v_slot};
                        ld.source_line = it->source_line;
                        fn_->append(current_block_, std::move(ld));
                    }
                    // cmp ctrl, 0  -- si moved/null, skip.
                    const ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0, it->source_line);
                    const ir::IrValueId v_cmp  = fn_->new_value(ir::IrType::BOOL);
                    {
                        ir::IrInstr cmp{};
                        cmp.op          = ir::IrOp::CMP_NE;
                        cmp.type        = ir::IrType::I64;
                        cmp.dst         = v_cmp;
                        cmp.operands    = {v_ctrl, v_zero};
                        cmp.source_line = it->source_line;
                        fn_->append(current_block_, std::move(cmp));
                    }
                    // br.cond v_cmp, dec_bb, skip_bb.
                    const ir::IrBlockId dec_bb  = fn_->new_block("sh_dec");
                    const ir::IrBlockId skip_bb = fn_->new_block("sh_skip");
                    {
                        ir::IrInstr br{};
                        br.op           = ir::IrOp::BR_COND;
                        br.operands     = {v_cmp};
                        br.target_block = dec_bb;
                        br.false_block  = skip_bb;
                        br.source_line  = it->source_line;
                        fn_->append(current_block_, std::move(br));
                        fn_->blocks[current_block_].succs.push_back(dec_bb);
                        fn_->blocks[current_block_].succs.push_back(skip_bb);
                        fn_->blocks[dec_bb].preds.push_back(current_block_);
                        fn_->blocks[skip_bb].preds.push_back(current_block_);
                    }
                    // dec_bb: refcount-- (LOAD + SUB + STORE).
                    current_block_ = dec_bb;
                    const ir::IrValueId v_rc = fn_->new_value(ir::IrType::I64);
                    {
                        ir::IrInstr ld{};
                        ld.op          = ir::IrOp::LOAD;
                        ld.type        = ir::IrType::I64;
                        ld.dst         = v_rc;
                        ld.operands    = {v_ctrl};
                        ld.source_line = it->source_line;
                        fn_->append(current_block_, std::move(ld));
                    }
                    const ir::IrValueId v_one    = emit_const(ir::IrType::I64, 1, it->source_line);
                    const ir::IrValueId v_rc_dec = fn_->new_value(ir::IrType::I64);
                    {
                        ir::IrInstr sub{};
                        sub.op          = ir::IrOp::SUB;
                        sub.type        = ir::IrType::I64;
                        sub.dst         = v_rc_dec;
                        sub.operands    = {v_rc, v_one};
                        sub.source_line = it->source_line;
                        fn_->append(current_block_, std::move(sub));
                    }
                    {
                        ir::IrInstr st{};
                        st.op          = ir::IrOp::STORE;
                        st.type        = ir::IrType::I64;
                        st.operands    = {v_rc_dec, v_ctrl};
                        st.source_line = it->source_line;
                        fn_->append(current_block_, std::move(st));
                    }
                    // br skip_bb (merge).
                    {
                        ir::IrInstr br{};
                        br.op           = ir::IrOp::BR;
                        br.target_block = skip_bb;
                        br.source_line  = it->source_line;
                        fn_->append(current_block_, std::move(br));
                        fn_->blocks[dec_bb].succs.push_back(skip_bb);
                        fn_->blocks[skip_bb].preds.push_back(dec_bb);
                    }
                    // current_block_ = skip_bb para que el siguiente cleanup
                    // se siga emitiendo en orden lineal.
                    current_block_ = skip_bb;
                    break;
                }
            }
        }
    }

    // ---------------------------------------------------------------------
    // synchronized (obj) { body }   (cierre completo con cleanup)
    //
    // Lowering con exception safety + return safety:
    //   1. Bajar target -> ptr -> gchandle -> monenter (todo en 1 RAW_ASM).
    //   2. Emitir tryenter catch-all con handler que hace monexit + rethrow.
    //   3. Push CleanupAction(tryleave + monexit) al cleanup_stack_.
    //   4. Bajar el body.  Si hace `return`, lower_return correra todos los
    //      cleanups del stack (incluyendo el nuestro) antes del RET.
    //   5. Pop CleanupAction.
    //   6. Si el body NO termino: emitir tryleave + monexit (cleanup normal).
    //   7. Bloque handler (alcanzable solo via excepcion del body): emitir
    //      monexit + rethrow.
    //
    // Resultado: el monitor se libera SIEMPRE, sea por:
    //   - flujo normal: paso 6.
    //   - return temprano: emit_cleanups_all() en lower_return.
    //   - throw: el handler del paso 7 lo libera y re-lanza.
    // ---------------------------------------------------------------------
    void Lowering::lower_throw(ast::ThrowStmt *s) {
        if (!s->value) {
            error_at(s->loc, "lowering: throw sin valor");
            return;
        }
        const ir::IrValueId v_obj = lower_expr(s->value.get());
        if (v_obj == ir::IR_NO_VALUE) return;
        // throw r_obj: el bytecode `throw` (0xD3) busca handler en
        // exc_frame_stack y salta al handler_pc.  Emitimos via RAW_ASM
        // con {src0} = reg de v_obj para que el regalloc materialice el
        // valor en algun reg accesible.
        ir::IrInstr ra{};
        ra.op          = ir::IrOp::THROW;
        ra.type        = ir::IrType::VOID;
        ra.dst         = ir::IR_NO_VALUE;
        ra.operands    = {v_obj};
        ra.source_line = s->loc.line;
        fn_->append(current_block_, std::move(ra));
        // throw es un terminador del flujo: marcamos el bloque para que
        // el emisor no intente continuar tras el throw.  El IR optimizer
        // reportara unreachable code si lo hay despues.
        block_terminated_ = true;
    }

    // ---------------------------------------------------------------------
    } // namespace vex

