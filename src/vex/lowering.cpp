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

namespace vex {
    /**
     * @brief Recorre un sub-arbol AST acumulando los nombres de variables
     *        que aparecen como destino de AssignExpr o operandos de ++/--.
     *
     * Lo necesita el lower_while() para construir los PHI nodes del bloque
     * header al estilo Braun.  No incluye las variables declaradas dentro
     * del propio sub-arbol (esas son locales al loop y no necesitan PHI);
     * el filtrado real se hace en el caller, que descarta cualquier nombre
     * que no exista en el scope justo antes del loop.
     *
     * @param n   Nodo a inspeccionar (puede ser nullptr).
     * @param out Set destino al que se anyaden los nombres.
     */
    /* non-static (visible desde lowering_stmt.cpp via extern decl) */
    void collect_assigned_vars(const ast::Node *      n,
                                      std::set<std::string> &out) {
        if (!n) return;
        switch (n->kind) {
            case ast::NodeKind::AssignExpr: {
                auto *a = static_cast<const ast::AssignExpr *>(n);
                if (a->target && a->target->kind == ast::NodeKind::IdentExpr) {
                    out.insert(static_cast<const ast::IdentExpr *>(a->target.get())->name);
                }
                collect_assigned_vars(a->value.get(), out);
                return;
            }
            case ast::NodeKind::UnaryExpr: {
                auto *     u       = static_cast<const ast::UnaryExpr *>(n);
                const bool mutates =
                (u->op == ast::UnOp::PreInc || u->op == ast::UnOp::PostInc
                    || u->op == ast::UnOp::PreDec || u->op == ast::UnOp::PostDec);
                if (mutates && u->operand
                    && u->operand->kind == ast::NodeKind::IdentExpr) {
                    out.insert(
                        static_cast<const ast::IdentExpr *>(u->operand.get())->name);
                }
                collect_assigned_vars(u->operand.get(), out);
                return;
            }
            case ast::NodeKind::BinaryExpr: {
                auto *b = static_cast<const ast::BinaryExpr *>(n);
                collect_assigned_vars(b->lhs.get(), out);
                collect_assigned_vars(b->rhs.get(), out);
                return;
            }
            case ast::NodeKind::CallExpr: {
                auto *c = static_cast<const ast::CallExpr *>(n);
                collect_assigned_vars(c->callee.get(), out);
                for (auto &a: c->args) collect_assigned_vars(a.get(), out);
                return;
            }
            case ast::NodeKind::BlockStmt: {
                auto *b = static_cast<const ast::BlockStmt *>(n);
                for (auto &s: b->body) collect_assigned_vars(s.get(), out);
                return;
            }
            case ast::NodeKind::ExprStmt: {
                auto *es = static_cast<const ast::ExprStmt *>(n);
                collect_assigned_vars(es->expr.get(), out);
                return;
            }
            case ast::NodeKind::IfStmt: {
                auto *is_ = static_cast<const ast::IfStmt *>(n);
                collect_assigned_vars(is_->cond.get(), out);
                collect_assigned_vars(is_->then_branch.get(), out);
                collect_assigned_vars(is_->else_branch.get(), out);
                return;
            }
            case ast::NodeKind::WhileStmt: {
                auto *w = static_cast<const ast::WhileStmt *>(n);
                collect_assigned_vars(w->cond.get(), out);
                collect_assigned_vars(w->body.get(), out);
                return;
            }
            case ast::NodeKind::DoWhileStmt: {
                auto *d = static_cast<const ast::DoWhileStmt *>(n);
                collect_assigned_vars(d->body.get(), out);
                collect_assigned_vars(d->cond.get(), out);
                return;
            }
            case ast::NodeKind::ForStmt: {
                auto *f = static_cast<const ast::ForStmt *>(n);
                collect_assigned_vars(f->init.get(), out);
                collect_assigned_vars(f->cond.get(), out);
                collect_assigned_vars(f->step.get(), out);
                collect_assigned_vars(f->body.get(), out);
                return;
            }
            case ast::NodeKind::ReturnStmt: {
                auto *r = static_cast<const ast::ReturnStmt *>(n);
                collect_assigned_vars(r->value.get(), out);
                return;
            }
            case ast::NodeKind::TryStmt: {
                auto *t = static_cast<const ast::TryStmt *>(n);
                collect_assigned_vars(t->body.get(), out);
                for (auto &cc: t->catches) {
                    collect_assigned_vars(cc.body.get(), out);
                }
                collect_assigned_vars(t->finally_body.get(), out);
                return;
            }
            case ast::NodeKind::SynchronizedStmt: {
                auto *ss = static_cast<const ast::SynchronizedStmt *>(n);
                collect_assigned_vars(ss->target.get(), out);
                collect_assigned_vars(ss->body.get(), out);
                return;
            }
            case ast::NodeKind::ThrowStmt: {
                auto *ts = static_cast<const ast::ThrowStmt *>(n);
                collect_assigned_vars(ts->value.get(), out);
                return;
            }
            case ast::NodeKind::MatchExpr: {
                auto *me = static_cast<const ast::MatchExpr *>(n);
                collect_assigned_vars(me->scrutinee.get(), out);
                for (auto &arm: me->arms) {
                    collect_assigned_vars(arm.body.get(), out);
                }
                return;
            }
            case ast::NodeKind::SpawnExpr: {
                auto *se = static_cast<const ast::SpawnExpr *>(n);
                collect_assigned_vars(se->body.get(), out);
                return;
            }
            case ast::NodeKind::RSpawnExpr: {
                auto *re = static_cast<const ast::RSpawnExpr *>(n);
                collect_assigned_vars(re->node_idx.get(), out);
                collect_assigned_vars(re->body.get(), out);
                return;
            }
            /* LabelStmt es solo un marker; el stmt siguiente vive en el
             * BlockStmt enclosing y se procesa por iteracion normal. */
            case ast::NodeKind::VarDeclStmt: {
                auto *v = static_cast<const ast::VarDeclStmt *>(n);
                // VarDeclStmt introduce una variable nueva: NO entra en el
                // set como mutacion (es definicion).  Pero su initializer
                // puede contener asignaciones a variables externas.
                collect_assigned_vars(v->init.get(), out);
                return;
            }
            default:
                return;
        }
    }

    // ---------------------------------------------------------------------
    // Constructor.
    // ---------------------------------------------------------------------

    Lowering::Lowering(ast::ModuleNode &mod, const TypeChecker &tc, Diagnostics &diags)
        : mod_(mod), tc_(tc), diags_(diags) {
        // Reservar capacidad razonable para el scope chain.  Programas
        // tipicos no suelen pasar de 5 scopes anidados.
        scopes_.reserve(8);
    }

    // ---------------------------------------------------------------------
    // Helpers de tipo.
    // ---------------------------------------------------------------------

    ir::IrType Lowering::ir_type_from_primitive(PrimitiveKind p) noexcept {
        // Mapeo directo.  Se usa una tabla constexpr indexada por enum
        // (PrimitiveKind y IrType comparten posiciones logicas pero los
        // valores numericos no coinciden, asi que este switch es la
        // version mantenible).
        switch (p) {
            case PrimitiveKind::VOID: return ir::IrType::VOID;
            case PrimitiveKind::BOOL: return ir::IrType::BOOL;
            // CHAR no tiene contraparte directa en ir::IrType; mapeamos a U8
            // (mismo ancho, semanticamente equivalente porque el frontend
            // solo lo usa como entero pequenyo sin codepoint awareness).
            case PrimitiveKind::CHAR: return ir::IrType::U8;
            case PrimitiveKind::I8: return ir::IrType::I8;
            case PrimitiveKind::I16: return ir::IrType::I16;
            case PrimitiveKind::I32: return ir::IrType::I32;
            case PrimitiveKind::I64: return ir::IrType::I64;
            case PrimitiveKind::U8: return ir::IrType::U8;
            case PrimitiveKind::U16: return ir::IrType::U16;
            case PrimitiveKind::U32: return ir::IrType::U32;
            case PrimitiveKind::U64: return ir::IrType::U64;
            case PrimitiveKind::F32: return ir::IrType::F32;
            case PrimitiveKind::F64: return ir::IrType::F64;
            case PrimitiveKind::PTR: return ir::IrType::PTR;
            // Para STRUCT y ARRAY no hay un IrType directo; en el lowering
            // ambos se representan via su PUNTERO base (PTR).  Cuando un
            // caller pasa STRUCT/ARRAY a este helper esperando un IrType
            // unitario, devolvemos PTR como aproximacion mas razonable.
            case PrimitiveKind::STRUCT: return ir::IrType::PTR;
            case PrimitiveKind::ARRAY: return ir::IrType::PTR;
            // CLASS es reference type: la "variable" guarda un puntero a
            // ObjectHeader, asi que el IrType subyacente es PTR.
            case PrimitiveKind::CLASS: return ir::IrType::PTR;
            // Optional/Result builtins: la variable guarda un puntero
            // (PTR) al buffer en stack alocado por Some/Ok/etc.  16 bytes
            // para Optional, 24 para Result; el lowering emite ALLOCA.
            case PrimitiveKind::OPTIONAL: return ir::IrType::PTR;
            case PrimitiveKind::RESULT: return ir::IrType::PTR;
            // string es un GcHandle opaco i64.
            case PrimitiveKind::STRING: return ir::IrType::I64;
            // tipos primitivos de coleccion: i64 handle host pointer.
            // Cero overhead vs llamar el plugin directo (sin wrapping).
            case PrimitiveKind::ARRAYLIST: return ir::IrType::I64;
            case PrimitiveKind::HASHMAP: return ir::IrType::I64;
            case PrimitiveKind::HASHSET: return ir::IrType::I64;
            case PrimitiveKind::QUEUE: return ir::IrType::I64;
            case PrimitiveKind::DEQUE: return ir::IrType::I64;
            case PrimitiveKind::TREEMAP: return ir::IrType::I64;
            case PrimitiveKind::TREESET: return ir::IrType::I64;
            case PrimitiveKind::STACK: return ir::IrType::I64;
            // Smart pointers: slot stack con host_ptr (8 bytes).
            case PrimitiveKind::UNIQUE_PTR: return ir::IrType::PTR;
            case PrimitiveKind::SHARED_PTR: return ir::IrType::PTR;
            // Borrows: host_ptr de 8 bytes (zero overhead vs T* raw).
            case PrimitiveKind::BORROW:     return ir::IrType::PTR;
            case PrimitiveKind::BORROW_MUT: return ir::IrType::PTR;
            // Future<T>: handle i64.
            case PrimitiveKind::FUTURE:     return ir::IrType::I64;
            // FUNCTION: par (fn_addr, env_addr); usamos PTR como aproximacion.
            case PrimitiveKind::FUNCTION:   return ir::IrType::PTR;
            case PrimitiveKind::COUNT: return ir::IrType::VOID;
        }
        return ir::IrType::VOID;
    }

    // ---------------------------------------------------------------------
    // Run principal.
    // ---------------------------------------------------------------------

    bool Lowering::run(ir::IrModule &out_module, const std::string &module_name) {
        const size_t initial_errors = diags_.error_count();
        out_module.name             = module_name;
        out_module.format           = "velb";
        // Guardar puntero al modulo de salida para que los lowering de
        // expresiones (StringLitExpr, builtins FFI) puedan registrar
        // datos estaticos y imports nativos sin pasar el modulo en cada
        // signature.
        out_mod_ = &out_module;

        // Inferir el fichero fuente del primer AST node con loc.file no
        // vacio.  Esto se usa en warnings emitidos por @c cast_if_needed
        // que solo recibe @c source_line.  Sin esta inferencia, los
        // warnings se imprimirian sin nombre de fichero.
        for (auto &d : mod_.decls) {
            if (!d) continue;
            if (!d->loc.file.empty()) {
                current_file_ = d->loc.file;
                break;
            }
        }

        // EMITIR PRIMERO los IntrospectInfo chunks
        // y poblar @c introspect_idx_by_name_ ANTES de bajar funciones,
        // para que @c find_type("Literal") pueda resolver el indice del
        // chunk en compile-time durante el lowering de main / otras
        // funciones.  Los layouts ya estan calculados por el type checker.
        emit_introspect_info_chunks();

        // Pase 1: registrar el tipo de retorno de cada funcion para validar
        // las llamadas.  Esto en un programa real ya esta en el type checker,
        // pero lo replicamos aqui para no acoplar la API.
        //
        // Adicionalmente registramos el PrimitiveKind semantico (OPTIONAL/
        // RESULT/...) en @c fn_ret_kind_ para que @c lower_call detecte
        // las funciones sret y aloque el retbuf en el caller.
        for (auto &decl: mod_.decls) {
            if (!decl) continue;
            if (decl->kind == ast::NodeKind::FunctionDecl) {
                auto *        fd   = static_cast<ast::FunctionDecl *>(decl.get());
                ir::IrType    rt   = ir::IrType::VOID;
                PrimitiveKind kind = PrimitiveKind::VOID;
                if (fd->return_type
                    && fd->return_type->kind == ast::NodeKind::PrimitiveTypeNode) {
                    auto *pt = static_cast<ast::PrimitiveTypeNode *>(fd->return_type.get());
                    rt       = ir_type_from_primitive(pt->prim);
                    kind     = pt->prim;
                } else if (fd->return_type) {
                    // Para tipos no-primitivos (NamedTypeNode con CLASS,
                    // Optional<T>, Result<V,E>, ARRAY, PTR, alias), usar
                    // el tipo semantico resuelto.  Sin esto, las llamadas
                    // a funciones que devuelven Result/Optional pierden
                    // su PTR de retorno y la asignacion al var-decl falla.
                    const Type sem = tc_.resolve_type_node(fd->return_type.get());
                    if (sem.kind != PrimitiveKind::COUNT
                        && sem.kind != PrimitiveKind::VOID) {
                        rt   = ir_type_from_primitive(sem.kind);
                        kind = sem.kind;
                    }
                }
                // sret: las funciones que devuelven Optional/Result o un
                // enum declarado tienen ret_type IR = VOID y un retbuf
                // hidden como primer param.  Sin este ajuste,
                // fn_return_types_ apuntaria a PTR y los callers crearian
                // un dst SSA "huerfano" que el emisor intentaria escribir
                // desde la salida (que no existe).
                bool is_user_enum = false;
                if (kind == PrimitiveKind::STRUCT && fd->return_type) {
                    const Type sem_check = tc_.resolve_type_node(fd->return_type.get());
                    if (sem_check.kind == PrimitiveKind::STRUCT) {
                        const auto &elays = tc_.enum_layouts();
                        if (elays.find(sem_check.struct_name) != elays.end()) {
                            is_user_enum = true;
                        }
                    }
                }
                // (gap O): detectar funciones que devuelven FUNCTION
                // y registrarlas para SRET.  Mismo patron que enums: el
                // tipo IR pasa a VOID y el caller pasa un retbuf hidden
                // de 16 bytes (slot del function value).
                bool is_function_ret = false;
                if (kind == PrimitiveKind::FUNCTION && fd->return_type) {
                    is_function_ret = true;
                    fn_returns_function_.insert(fd->name);
                }
                // Smart pointers: SRET de 8 bytes para `unique<T>` o
                // `shared<T>`.  Sin esto, devolver un smart pointer
                // desde una funcion seria inseguro (su slot vive en el
                // stack del callee y muere al RET).  Con SRET el caller
                // aloca el slot y el callee copia los 8 bytes ahi.
                bool is_smartptr_ret = false;
                if ((kind == PrimitiveKind::UNIQUE_PTR
                  || kind == PrimitiveKind::SHARED_PTR)
                  && fd->return_type) {
                    is_smartptr_ret = true;
                    fn_returns_smartptr_.insert(fd->name);
                }
                if (kind == PrimitiveKind::OPTIONAL
                    || kind == PrimitiveKind::RESULT
                    || is_user_enum
                    || is_function_ret
                    || is_smartptr_ret) {
                    rt = ir::IrType::VOID;
                }
                // Item 9: @Async wrapper retorna i64 (Future handle), no T.
                // El tipo logico T se preserva como Future<T> en el sig del
                // type checker para el `await fut`, pero el bytecode del
                // wrapper devuelve i64 raw en R0.  Sin este ajuste, el
                // lowering del call site marca dst con tipo T (e.g. f64),
                // emite un cast i64->f64 erroneo (FTOI cambia el value),
                // y await opera sobre handle corrupto.
                if (fd->is_async) {
                    rt   = ir::IrType::I64;
                    kind = PrimitiveKind::I64;
                }
                fn_return_types_[fd->name] = rt;
                fn_ret_kind_[fd->name]     = kind;
                if (is_user_enum) {
                    // Marcar como sret enum.  Guardamos el nombre para
                    // que el caller pueda buscar el size_bytes.
                    const Type sem_check        = tc_.resolve_type_node(fd->return_type.get());
                    fn_ret_enum_name_[fd->name] = sem_check.struct_name;
                }
            } else if (decl->kind == ast::NodeKind::ExternFnDecl) {
                // FFI declarativo: registrar tipo de retorno y
                // mapeo nombre -> libreria nativa para que @c lower_call
                // emita CALLN @Method("<lib>:<name>") en vez de CALLVM.
                auto *     efd = static_cast<ast::ExternFnDecl *>(decl.get());
                ir::IrType rt  = ir::IrType::VOID;
                if (efd->return_type
                    && efd->return_type->kind == ast::NodeKind::PrimitiveTypeNode) {
                    auto *pt = static_cast<ast::PrimitiveTypeNode *>(efd->return_type.get());
                    if (pt->prim != PrimitiveKind::VOID) {
                        rt = ir_type_from_primitive(pt->prim);
                    }
                } else if (efd->return_type) {
                    const Type sem = tc_.resolve_type_node(efd->return_type.get());
                    if (sem.kind != PrimitiveKind::COUNT
                        && sem.kind != PrimitiveKind::VOID) {
                        rt = ir_type_from_primitive(sem.kind);
                    }
                }
                fn_return_types_[efd->name]       = rt;
                extern_lib_by_fn_name_[efd->name] = efd->lib;
            }
        }

        // Pase 2: bajar cada funcion.
        //
        // ORDEN IMPORTANTE: el emisor IR (ir_emitter.cpp::ir_emit_module)
        // marca como "entry point" la PRIMERA funcion del modulo, lo que
        // hace que esa funcion termine con 'hlt' (detiene la VM) en lugar
        // de 'ret'.  Por tanto si dejamos las funciones en el orden en que
        // aparecen en el .vex, una funcion como 'factorial' que se declara
        // antes de 'main' acabaria como entry point y la primera llamada
        // recursiva detendria la VM.  Solucion: bajamos 'main' primero
        // (si existe), luego el resto en orden de declaracion.
        ast::FunctionDecl *main_decl = nullptr;
        for (auto &decl: mod_.decls) {
            if (decl && decl->kind == ast::NodeKind::FunctionDecl) {
                auto *fd = static_cast<ast::FunctionDecl *>(decl.get());
                if (fd->name == "main") {
                    main_decl = fd;
                    break;
                }
            }
        }
        // L2.2: pre-scan global runtime vars y reservar slots ANTES de
        // bajar main.  Sin esto, lower_ident("g") en main encuentra
        // runtime_global_slots_ vacio y emite "nombre no resuelto".
        for (auto &decl : mod_.decls) {
            if (!decl || decl->kind != ast::NodeKind::GlobalVarDecl) continue;
            auto *gv = static_cast<ast::GlobalVarDecl *>(decl.get());
            if (gv->is_const || gv->is_comptime) continue;
            if (!gv->type || gv->type->kind != ast::NodeKind::PrimitiveTypeNode) continue;
            auto *pt = static_cast<ast::PrimitiveTypeNode *>(gv->type.get());
            switch (pt->prim) {
                case PrimitiveKind::STRING:
                case PrimitiveKind::I8: case PrimitiveKind::I16:
                case PrimitiveKind::I32: case PrimitiveKind::I64:
                case PrimitiveKind::U8: case PrimitiveKind::U16:
                case PrimitiveKind::U32: case PrimitiveKind::U64:
                case PrimitiveKind::F32: case PrimitiveKind::F64:
                case PrimitiveKind::BOOL: case PrimitiveKind::CHAR:
                case PrimitiveKind::PTR:
                    (void)get_or_create_runtime_global_slot(gv->name);
                    break;
                default: break;
            }
        }
        if (main_decl) lower_function(main_decl, out_module);

        for (auto &decl: mod_.decls) {
            if (!decl) continue;
            if (decl->kind == ast::NodeKind::FunctionDecl) {
                auto *fd = static_cast<ast::FunctionDecl *>(decl.get());
                if (fd == main_decl) continue; // ya bajada
                if (fd->is_async) {
                    lower_async_function(fd, out_module);
                } else {
                    lower_function(fd, out_module);
                }
            } else if (decl->kind == ast::NodeKind::GlobalVarDecl) {
                // Las variables globales con storage real no estan soportadas
                // en el frontend Vex actual.  Pero `const T NAME = lit;` SI
                // funciona porque @c lower_ident las inlinea como CONST en
                // cada uso (no necesitan storage).  Solo avisamos para las
                // globales NO-const o las que tienen inicializador no-literal
                // (que efectivamente se ignoran).
                auto *gv            = static_cast<ast::GlobalVarDecl *>(decl.get());
                bool  literal_const = gv->is_const && gv->init && (
                    gv->init->kind == ast::NodeKind::IntLitExpr
                    || gv->init->kind == ast::NodeKind::StringLitExpr  // 2026-05-23 const string global
                    || (gv->init->kind == ast::NodeKind::UnaryExpr
                        && static_cast<ast::UnaryExpr *>(gv->init.get())->op == ast::UnOp::Neg
                        && static_cast<ast::UnaryExpr *>(gv->init.get())->operand
                        && static_cast<ast::UnaryExpr *>(gv->init.get())->operand->kind == ast::NodeKind::IntLitExpr));
                /* A.38/A.39: `comptime const` y `static_assert` (que se
                 * envuelve como GlobalVarDecl dummy con type=void) no
                 * tienen storage runtime y NO necesitan warning. */
                bool  is_comptime_silent =
                    gv->is_comptime
                 || (gv->type
                  && gv->type->kind == ast::NodeKind::PrimitiveTypeNode
                  && static_cast<ast::PrimitiveTypeNode *>(gv->type.get())->prim
                       == PrimitiveKind::VOID);
                // L2.2: globales runtime no-const obtienen storage real
                // via slot en static_data inicializado por __module_init.
                // Solo se reserva si tiene tipo basico soportado: STRING o
                // enteros/floats que caben en 8 bytes.
                bool runtime_global_supported = false;
                if (!gv->is_const && !is_comptime_silent && gv->type
                 && gv->type->kind == ast::NodeKind::PrimitiveTypeNode) {
                    auto *pt = static_cast<ast::PrimitiveTypeNode *>(gv->type.get());
                    switch (pt->prim) {
                        case PrimitiveKind::STRING:
                        case PrimitiveKind::I8:
                        case PrimitiveKind::I16:
                        case PrimitiveKind::I32:
                        case PrimitiveKind::I64:
                        case PrimitiveKind::U8:
                        case PrimitiveKind::U16:
                        case PrimitiveKind::U32:
                        case PrimitiveKind::U64:
                        case PrimitiveKind::F32:
                        case PrimitiveKind::F64:
                        case PrimitiveKind::BOOL:
                        case PrimitiveKind::CHAR:
                        case PrimitiveKind::PTR:
                            runtime_global_supported = true;
                            (void)get_or_create_runtime_global_slot(gv->name);
                            break;
                        default: break;
                    }
                }
                if (!literal_const && !is_comptime_silent
                 && !runtime_global_supported) {
                    diags_.warning(decl->loc,
                                   "variable global no-const ignorada (sin storage real en este frontend)");
                }
            }
        }

        // Bajar metodos de clases al final.  Vienen DESPUES de las
        // funciones top-level para no tomar la posicion de "entry point"
        // del emisor IR (que termina la primera funcion con hlt).  Cada
        // metodo se compila como IrFunction con nombre <Class>__<method>
        // y un primer parametro implicito 'this' de tipo PTR.
        for (auto &decl: mod_.decls) {
            if (!decl || decl->kind != ast::NodeKind::ClassDecl) continue;
            auto *cd = static_cast<ast::ClassDecl *>(decl.get());
            lower_class_methods(cd, out_module);
        }

        // Generar funciones auxiliares de POO:
        //  - __new_<X>(args) por cada clase: encapsula findclass+newobj+ctor.
        //  - __module_init(): registra todas las clases via defclass+...
        // Estas se anyaden al modulo despues de las funciones de usuario;
        // el prologo de main incluye una llamada a __module_init para
        // garantizar que las clases esten registradas antes del cuerpo.
        generate_new_helpers(out_module);
        generate_module_init_function(out_module);

        // Exportar metadata POO al @c IrModule para que el port transpiler
        // (port-C, etc.) emita codigo POO eficiente sin reconstruir las
        // clases desde @c __module_init.  Llamar tras lower_class_methods
        // para que los @c IrMethod::ir_fn_name apunten a IrFunctions ya
        // emitidas en @c out_module.functions.
        export_classes_to_ir(out_module);

        // volcar las funciones sinteticas de spawn DESPUES de las
        // de usuario y POO.  Asi main sigue siendo la primera funcion del
        // modulo (entry point con hlt) y los helpers de spawn quedan al
        // final como funciones normales (cierran con ret, pero el body
        // siempre incluye un hlt explicito antes del fin del bloque).
        for (auto &h: pending_spawn_helpers_) {
            propagate_is_gc_object_through_phis(h);
            out_module.add_function(std::move(h));
        }
        pending_spawn_helpers_.clear();

        return diags_.error_count() == initial_errors;
    }

    // ---------------------------------------------------------------------
    // Sprint 4 (A.37.s4): IntrospectInfo POD chunks.
    //
    // Para cada layout marcado @Introspect emitimos UN chunk en
    // static_data con este layout self-contained (todas las direcciones
    // son offsets relativos al inicio del chunk, asi no hace falta
    // relocation cross-chunk):
    //
    // HEADER (24 bytes):
    //   +0   u32 kind         (0=Prim, 1=Class, 2=Struct, 3=Enum)
    //   +4   u32 size_bytes
    //   +8   u32 align_bytes
    //   +12  u32 field_count
    //   +16  u32 name_off     -- offset interno a los bytes del nombre
    //   +20  u32 name_len
    //
    // FIELDS array (16 bytes cada uno):  ofset 24 + i*16
    //   +0   u32 offset       -- offset del field DENTRO de la instancia
    //   +4   u32 size_bytes
    //   +8   u32 name_off     -- offset interno
    //   +12  u32 name_len
    //
    // NAMES area: empieza tras los FIELDS.  Bytes raw, sin NUL
    // terminator (la longitud va en name_len; el accesor type_info_name
    // construye un StringObject via STRMAKE con name_addr + name_len).
    // ---------------------------------------------------------------------
    void Lowering::emit_introspect_info_chunks() {
        auto build_chunk = [this](const std::string &name,
                                   uint32_t           kind,
                                   uint32_t           size_bytes,
                                   uint32_t           align_bytes,
                                   const std::vector<std::pair<std::string,
                                                                std::pair<uint32_t,
                                                                           uint32_t>>>
                                       &fields) -> std::vector<uint8_t> {
            const uint32_t field_count = static_cast<uint32_t>(fields.size());
            const size_t   header_sz   = 24;
            const size_t   fields_sz   = field_count * 16;
            /* Calculamos los offsets de los nombres tras la tabla de fields. */
            uint32_t name_off = static_cast<uint32_t>(header_sz + fields_sz);
            uint32_t name_len = static_cast<uint32_t>(name.size());
            std::vector<std::pair<uint32_t, uint32_t>> field_name_ranges;
            field_name_ranges.reserve(fields.size());
            uint32_t cur = name_off + name_len;
            for (auto &f : fields) {
                const uint32_t flen = static_cast<uint32_t>(f.first.size());
                field_name_ranges.push_back({cur, flen});
                cur += flen;
            }
            /* Reservamos el buffer con tamano exacto y rellenamos. */
            std::vector<uint8_t> buf;
            buf.resize(cur, 0);
            auto put_u32 = [&buf](size_t at, uint32_t v) {
                buf[at + 0] = static_cast<uint8_t>(v & 0xFF);
                buf[at + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
                buf[at + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
                buf[at + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
            };
            put_u32(0,  kind);
            put_u32(4,  size_bytes);
            put_u32(8,  align_bytes);
            put_u32(12, field_count);
            put_u32(16, name_off);
            put_u32(20, name_len);
            for (size_t i = 0; i < fields.size(); ++i) {
                const size_t base = header_sz + i * 16;
                put_u32(base + 0,  fields[i].second.first);   /* offset */
                put_u32(base + 4,  fields[i].second.second);  /* size */
                put_u32(base + 8,  field_name_ranges[i].first);  /* name_off */
                put_u32(base + 12, field_name_ranges[i].second); /* name_len */
            }
            /* Copiar bytes del nombre del tipo. */
            for (size_t i = 0; i < name.size(); ++i) buf[name_off + i] = static_cast<uint8_t>(name[i]);
            /* Copiar bytes de los nombres de fields. */
            for (size_t i = 0; i < fields.size(); ++i) {
                const auto &nm = fields[i].first;
                const uint32_t pos = field_name_ranges[i].first;
                for (size_t j = 0; j < nm.size(); ++j) {
                    buf[pos + j] = static_cast<uint8_t>(nm[j]);
                }
            }
            return buf;
        };

        /* Structs marcados @Introspect. */
        for (const auto &kv : tc_.struct_layouts()) {
            const auto &lay = kv.second;
            if (!lay.is_introspect) continue;
            std::vector<std::pair<std::string, std::pair<uint32_t, uint32_t>>> fs;
            fs.reserve(lay.fields.size());
            for (const auto &f : lay.fields) {
                fs.push_back({f.name, {f.offset, f.size}});
            }
            std::vector<uint8_t> chunk = build_chunk(
                lay.name, /*Struct=*/2, lay.size_bytes, lay.align_bytes, fs);
            const uint64_t idx = out_mod_->intern_static_data(std::move(chunk));
            introspect_idx_by_name_[lay.name] = idx;
        }
        /* Clases marcadas @Introspect.  No emitimos metodos por ahora
         * (Sprint 4 MVP cubre solo fields; Sprint 5 anyade methods). */
        for (const auto &kv : tc_.class_layouts()) {
            const auto &lay = kv.second;
            if (!lay.is_introspect) continue;
            std::vector<std::pair<std::string, std::pair<uint32_t, uint32_t>>> fs;
            fs.reserve(lay.fields.size());
            for (const auto &f : lay.fields) {
                fs.push_back({f.name, {f.offset, f.size}});
            }
            std::vector<uint8_t> chunk = build_chunk(
                lay.name, /*Class=*/1, lay.size_bytes, /*align=*/8, fs);
            const uint64_t idx = out_mod_->intern_static_data(std::move(chunk));
            introspect_idx_by_name_[lay.name] = idx;
        }
        /* Enums marcados @Introspect.  Listamos variantes como "fields"
         * sinteticos con offset=tag, size=0 -- conveccion para el MVP;
         * el usuario sabe que el campo offset en realidad es el tag. */
        for (const auto &kv : tc_.enum_layouts()) {
            const auto &lay = kv.second;
            if (!lay.is_introspect) continue;
            std::vector<std::pair<std::string, std::pair<uint32_t, uint32_t>>> fs;
            fs.reserve(lay.variants.size());
            for (const auto &v : lay.variants) {
                fs.push_back({v.name, {v.tag, 0}});
            }
            std::vector<uint8_t> chunk = build_chunk(
                lay.name, /*Enum=*/3, lay.size_bytes, /*align=*/8, fs);
            const uint64_t idx = out_mod_->intern_static_data(std::move(chunk));
            introspect_idx_by_name_[lay.name] = idx;
        }
    }

    // ---------------------------------------------------------------------
    // Lowering de una funcion.
    // ---------------------------------------------------------------------

    /**
     * @brief Phase MC.1 -- detecta si el body de un @Macro contiene
     * caracteristicas que el IR runtime NO soporta todavia.
     *
     * Devuelve la primera razon encontrada (string descriptivo) o cadena
     * vacia si el body es lowerable.  Used by @c lower_function para
     * decidir si lowear o saltar el body al IR.
     *
     * Patrones detectados como NO soportados (todavia):
     *   - Calls a builtins comptime-only (`comptime_concat`, `to_str`,
     *     `gensym`, `comptime_compile`, etc.).
     *   - Calls con type_args (introspect: `sizeof<T>`, `field_name<T>`,
     *     `comptime_type<T>`, etc.).
     *   - VarDeclStmt con `is_comptime=true` (comptime var/const) --
     *     requiere puente de memoria compartida (MC.5).
     *   - ExprStmt con AssignExpr a IdentExpr global comptime --
     *     mismo motivo que arriba.
     *
     * En sprints posteriores (MC.4, MC.5) cada categoria se vuelve
     * "soportada" anadiendo un FFI runtime + bridge de memoria.
     */
    static std::string macro_body_unsupported_reason(const TypeChecker &tc,
                                                       const ast::Stmt *s);
    static std::string macro_body_unsupported_reason_expr(const TypeChecker &tc,
                                                            const ast::Expr *e);

    static std::string macro_body_unsupported_reason_expr(const TypeChecker &tc,
                                                            const ast::Expr *e) {
        if (!e) return "";
        switch (e->kind) {
            case ast::NodeKind::IdentExpr: {
                /* Phase MC.17.2: refs a `comptime const` (INMUTABLES)
                 * globales de tipo int SE ACEPTAN -- se materializan
                 * como slot @c static_data de 8 bytes, leidos via
                 * LOAD i64.  El valor es fijo, no hay divergencia
                 * posible con el AST evaluator.
                 *
                 * `comptime var` (MUTABLES) siguen rechazados porque
                 * la VM y el AST evaluator mantendrian copias separadas
                 * que se desincronizarian con @Pure memoization
                 * (test 156).  Soportarlos requiere shared memory
                 * cross-AST/VM (deferred). */
                const auto *id = static_cast<const ast::IdentExpr *>(e);
                auto cit = tc.comptime_const_values().find(id->name);
                if (cit != tc.comptime_const_values().end()) {
                    if (cit->second.is_str) {
                        return "ref a comptime global string '" + id->name + "'";
                    }
                    if (cit->second.is_mutable) {
                        return "ref a comptime var (mutable) global '" + id->name + "'";
                    }
                    /* comptime const int OK. */
                    return "";
                }
                return "";
            }
            case ast::NodeKind::CallExpr: {
                const auto *ce = static_cast<const ast::CallExpr *>(e);
                /* Calls con type-args -> introspect: NO soportado v1. */
                if (!ce->type_args.empty()) {
                    return "introspect builtin con type_args (sizeof<T>, etc.)";
                }
                /* Calls a builtins comptime-only por nombre. */
                if (ce->callee && ce->callee->kind == ast::NodeKind::IdentExpr) {
                    const auto *id = static_cast<const ast::IdentExpr *>(ce->callee.get());
                    /* Phase MC.15B+C: los builtins que YA estan aliasados a
                     * sus equivalentes runtime str_* en @c lower_call NO
                     * deben rechazarse aqui -- el lowering los soporta.
                     * Los demas siguen siendo comptime-only.
                     *
                     * Lowereables (MC.15B: concat/streq/strlen; MC.15C:
                     * to_str/chr/ord/substr/gensym):
                     *   comptime_concat  -> STRCAT
                     *   comptime_streq   -> STRCMP + cmp
                     *   comptime_strlen  -> STRLEN
                     *   comptime_to_str  -> CALLN(vio_int_to_vmbuf) + STRMAKE
                     *   comptime_chr     -> CALLN(vio_char_to_vmbuf) + STRMAKE
                     *   comptime_ord     -> STRRAW + LOAD u8
                     *   comptime_substr  -> STRSLICE
                     *   gensym           -> CALLN(vio_gensym)
                     *
                     * Restantes (MC.15D futuro): repeat, replace, contains,
                     * compile, emit_expr, type, print/ct_print.
                     */
                    /* Restantes comptime-only tras MC.18+MC.20:
                     *   comptime_compile / compile      -- generacion de codigo dinamica
                     *   comptime_emit_expr / emit_expr  -- splice de AST en compile-time
                     *   comptime_type                   -- type-as-value
                     *
                     * `comptime_print`, `ct_print` -> `println` (MC.18).
                     * `static_assert` -> virtual lib `vesta_comptime`
                     * via FFI bridge (MC.20).  El lowering emite CALLN
                     * a "vesta_comptime:static_assert" que el Loader
                     * resuelve via @c lookup_virtual_fn al cargar el
                     * .velb. */
                    static const std::unordered_set<std::string> COMPTIME_ONLY = {
                        "comptime_compile", "comptime_emit_expr",
                        "comptime_type",
                        "compile", "emit_expr",
                    };
                    if (COMPTIME_ONLY.count(id->name)) {
                        return "builtin comptime-only '" + id->name + "'";
                    }
                    /* Phase MC.17.3: calls a @Macros user-defined SE ACEPTAN
                     * (la callee tambien se baja a IR con nombre
                     * `__macro_<callee>`, asi que emitimos CALLVM regular
                     * a esa label).  Calls a comptime fns NO-@Macro
                     * siguen rechazados (necesitarian inline o lower
                     * propio que no esta hecho). */
                    auto fn_it = tc.comptime_fns().find(id->name);
                    if (fn_it != tc.comptime_fns().end()) {
                        if (fn_it->second && fn_it->second->is_macro) {
                            /* Aceptamos.  El callee macro tambien sera
                             * lowereado por el linker (al final del
                             * pase).  Si su body resulta no-lowereable,
                             * el __macro_<callee> no existira y la
                             * CALLVM fallara en runtime -- ese caso
                             * cae al fallback AST por inconsistencia. */
                            return "";
                        }
                        return "call a comptime fn user-defined '" + id->name + "'";
                    }
                }
                /* Recurse en args. */
                for (const auto &a : ce->args) {
                    auto r = macro_body_unsupported_reason_expr(tc, a.get());
                    if (!r.empty()) return r;
                }
                auto r = macro_body_unsupported_reason_expr(tc, ce->callee.get());
                if (!r.empty()) return r;
                return "";
            }
            case ast::NodeKind::BinaryExpr: {
                const auto *bn = static_cast<const ast::BinaryExpr *>(e);
                auto r = macro_body_unsupported_reason_expr(tc, bn->lhs.get());
                if (!r.empty()) return r;
                return macro_body_unsupported_reason_expr(tc, bn->rhs.get());
            }
            case ast::NodeKind::InitListExpr: {
                /* Init list `{a, b, c}` o `{.x=1, .y=2}` requiere ALLOCA
                 * tipado del destino (array o struct) en runtime.  El
                 * path del macro lowering no lo soporta; cae al AST
                 * evaluator que SI maneja arrays/structs (A.42). */
                return "init list `{...}` en macro body (usa AST eval)";
            }
            case ast::NodeKind::IndexExpr: {
                /* Array indexing `arr[i]` requiere conocer el tipo de
                 * `arr` y el sizeof del elemento para emitir ADD + LOAD
                 * correctos.  En el body de un macro las vars locales
                 * pueden ser arrays comptime que NO existen en runtime;
                 * fallback al AST evaluator. */
                return "array indexing `arr[i]` en macro body (usa AST eval)";
            }
            case ast::NodeKind::UnaryExpr: {
                const auto *un = static_cast<const ast::UnaryExpr *>(e);
                return macro_body_unsupported_reason_expr(tc, un->operand.get());
            }
            case ast::NodeKind::TernaryExpr: {
                const auto *te = static_cast<const ast::TernaryExpr *>(e);
                auto r = macro_body_unsupported_reason_expr(tc, te->cond.get());
                if (!r.empty()) return r;
                r = macro_body_unsupported_reason_expr(tc, te->then_expr.get());
                if (!r.empty()) return r;
                return macro_body_unsupported_reason_expr(tc, te->else_expr.get());
            }
            case ast::NodeKind::AssignExpr: {
                const auto *ae = static_cast<const ast::AssignExpr *>(e);
                auto r = macro_body_unsupported_reason_expr(tc, ae->target.get());
                if (!r.empty()) return r;
                return macro_body_unsupported_reason_expr(tc, ae->value.get());
            }
            default:
                return "";
        }
    }

    static std::string macro_body_unsupported_reason(const TypeChecker &tc,
                                                       const ast::Stmt *s) {
        if (!s) return "";
        switch (s->kind) {
            case ast::NodeKind::BlockStmt: {
                const auto *bs = static_cast<const ast::BlockStmt *>(s);
                for (const auto &st : bs->body) {
                    auto r = macro_body_unsupported_reason(tc, st.get());
                    if (!r.empty()) return r;
                }
                return "";
            }
            case ast::NodeKind::VarDeclStmt: {
                const auto *vd = static_cast<const ast::VarDeclStmt *>(s);
                /*   (1/3): `comptime var/const` LOCALES dentro
                 * de un macro body ya no se rechazan.  El lowering los
                 * trata como vars runtime regulares (en `lower_var_decl`
                 * detectamos el flag y descartamos la rama comptime
                 * cuando current_fn_is_macro_=true).  El VM computa el
                 * init en cada invocacion -- mismo resultado semantico
                 * que la evaluacion AST que ocurria one-time.
                 *
                 * Validamos solo el init si esta presente. */
                /* Vars locales de tipo array nativo `T[N]` o struct
                 * nominal NO son lowereables en este path (requeriria
                 * ALLOCA + sizeof del elemento + path completo de
                 * struct layout).  Fallback al AST evaluator que SI
                 * maneja arrays/structs comptime (A.41+A.42). */
                if (vd->type) {
                    const auto *t = vd->type.get();
                    if (t->kind == ast::NodeKind::ArrayTypeNode) {
                        return "var local de tipo array en macro body (usa AST eval)";
                    }
                    if (t->kind == ast::NodeKind::NamedTypeNode) {
                        /* Si el nombre matchea un struct declarado, es
                         * un struct value-type que no lowereamos en el
                         * body del macro. */
                        const auto *nt = static_cast<const ast::NamedTypeNode *>(t);
                        if (tc.struct_layouts().find(nt->name) != tc.struct_layouts().end()) {
                            return "var local de tipo struct '" + nt->name
                                 + "' en macro body (usa AST eval)";
                        }
                    }
                }
                if (vd->init) {
                    return macro_body_unsupported_reason_expr(tc, vd->init.get());
                }
                return "";
            }
            case ast::NodeKind::ExprStmt: {
                const auto *es = static_cast<const ast::ExprStmt *>(s);
                return macro_body_unsupported_reason_expr(tc, es->expr.get());
            }
            case ast::NodeKind::ReturnStmt: {
                const auto *rs = static_cast<const ast::ReturnStmt *>(s);
                return macro_body_unsupported_reason_expr(tc, rs->value.get());
            }
            case ast::NodeKind::IfStmt: {
                const auto *is = static_cast<const ast::IfStmt *>(s);
                auto r = macro_body_unsupported_reason_expr(tc, is->cond.get());
                if (!r.empty()) return r;
                r = macro_body_unsupported_reason(tc, is->then_branch.get());
                if (!r.empty()) return r;
                if (is->else_branch) {
                    return macro_body_unsupported_reason(tc, is->else_branch.get());
                }
                return "";
            }
            case ast::NodeKind::WhileStmt: {
                const auto *ws = static_cast<const ast::WhileStmt *>(s);
                auto r = macro_body_unsupported_reason_expr(tc, ws->cond.get());
                if (!r.empty()) return r;
                return macro_body_unsupported_reason(tc, ws->body.get());
            }
            case ast::NodeKind::DoWhileStmt: {
                const auto *ds = static_cast<const ast::DoWhileStmt *>(s);
                auto r = macro_body_unsupported_reason_expr(tc, ds->cond.get());
                if (!r.empty()) return r;
                return macro_body_unsupported_reason(tc, ds->body.get());
            }
            case ast::NodeKind::ForStmt: {
                const auto *fs = static_cast<const ast::ForStmt *>(s);
                if (fs->init) {
                    auto r = macro_body_unsupported_reason(tc, fs->init.get());
                    if (!r.empty()) return r;
                }
                if (fs->cond) {
                    auto r = macro_body_unsupported_reason_expr(tc, fs->cond.get());
                    if (!r.empty()) return r;
                }
                if (fs->step) {
                    auto r = macro_body_unsupported_reason_expr(tc, fs->step.get());
                    if (!r.empty()) return r;
                }
                return macro_body_unsupported_reason(tc, fs->body.get());
            }
            case ast::NodeKind::ComptimeBlockStmt:
            case ast::NodeKind::ComptimeForStmt:
                return "comptime block/for en macro body (requiere MC.5)";
            default:
                return "";
        }
    }

    /* Pre-pase de annotation de tipos para body de @Macro.
     *
     * Los macros NO pasan por `check_functions` (los saltea porque su
     * body se interpreta solo al call site).  Pero MC.1 los baja a IR para
     * que la VM eval pueda ejecutarlos.  Sin annotation de tipos, los
     * IdentExpr en el body tienen result_type=VOID -- `lower_binary` no
     * detecta el caso `code == "OK"` con `code: string` y emite cmpjmp
     * directo sobre los handles en lugar de STRCMP runtime.
     *
     * Este walker recorre el body y annota result_type de los IdentExpr
     * cuyo nombre matchee un param del macro.  Es minimal -- solo cubre
     * el caso de params; otras vars locales se annotan al llamarlas via
     * lower_expr (que internamente usa el scope del lowering).  */
    static void annotate_macro_param_idents(
            ast::Stmt *s,
            const std::unordered_map<std::string, Type> &param_types) {
        if (!s) return;
        std::function<void(ast::Expr *)> walk_expr = [&](ast::Expr *e) {
            if (!e) return;
            if (e->kind == ast::NodeKind::IdentExpr) {
                auto *id = static_cast<ast::IdentExpr *>(e);
                auto it = param_types.find(id->name);
                if (it != param_types.end()) {
                    /* Siempre sobreescribir: dentro del body del macro
                     * los IdentExpr no fueron type-checkeados; el campo
                     * puede tener un default heredado del parser. */
                    id->result_type = it->second;
                }
                return;
            }
            if (e->kind == ast::NodeKind::BinaryExpr) {
                auto *bn = static_cast<ast::BinaryExpr *>(e);
                walk_expr(bn->lhs.get());
                walk_expr(bn->rhs.get());
                return;
            }
            if (e->kind == ast::NodeKind::UnaryExpr) {
                auto *un = static_cast<ast::UnaryExpr *>(e);
                walk_expr(un->operand.get());
                return;
            }
            if (e->kind == ast::NodeKind::CallExpr) {
                auto *ce = static_cast<ast::CallExpr *>(e);
                walk_expr(ce->callee.get());
                for (auto &a : ce->args) walk_expr(a.get());
                return;
            }
            if (e->kind == ast::NodeKind::AssignExpr) {
                auto *ae = static_cast<ast::AssignExpr *>(e);
                walk_expr(ae->target.get());
                walk_expr(ae->value.get());
                return;
            }
            if (e->kind == ast::NodeKind::TernaryExpr) {
                auto *te = static_cast<ast::TernaryExpr *>(e);
                walk_expr(te->cond.get());
                walk_expr(te->then_expr.get());
                walk_expr(te->else_expr.get());
                return;
            }
            if (e->kind == ast::NodeKind::IndexExpr) {
                auto *ix = static_cast<ast::IndexExpr *>(e);
                walk_expr(ix->base.get());
                walk_expr(ix->index.get());
                return;
            }
            if (e->kind == ast::NodeKind::FieldAccessExpr) {
                auto *fa = static_cast<ast::FieldAccessExpr *>(e);
                walk_expr(fa->base.get());
                return;
            }
            if (e->kind == ast::NodeKind::CastExpr) {
                auto *ca = static_cast<ast::CastExpr *>(e);
                walk_expr(ca->operand.get());
                return;
            }
            /* Otros tipos de expresion: no necesitan recursion para el
             * caso de annotation de params (literals, ThisExpr, etc.). */
        };
        switch (s->kind) {
            case ast::NodeKind::BlockStmt: {
                auto *bs = static_cast<ast::BlockStmt *>(s);
                for (auto &st : bs->body) annotate_macro_param_idents(st.get(), param_types);
                break;
            }
            case ast::NodeKind::VarDeclStmt: {
                auto *vd = static_cast<ast::VarDeclStmt *>(s);
                if (vd->init) walk_expr(vd->init.get());
                break;
            }
            case ast::NodeKind::ExprStmt: {
                auto *es = static_cast<ast::ExprStmt *>(s);
                walk_expr(es->expr.get());
                break;
            }
            case ast::NodeKind::ReturnStmt: {
                auto *rs = static_cast<ast::ReturnStmt *>(s);
                if (rs->value) walk_expr(rs->value.get());
                break;
            }
            case ast::NodeKind::IfStmt: {
                auto *ifs = static_cast<ast::IfStmt *>(s);
                walk_expr(ifs->cond.get());
                annotate_macro_param_idents(ifs->then_branch.get(), param_types);
                if (ifs->else_branch) annotate_macro_param_idents(ifs->else_branch.get(), param_types);
                break;
            }
            case ast::NodeKind::WhileStmt: {
                auto *ws = static_cast<ast::WhileStmt *>(s);
                walk_expr(ws->cond.get());
                annotate_macro_param_idents(ws->body.get(), param_types);
                break;
            }
            case ast::NodeKind::DoWhileStmt: {
                auto *ds = static_cast<ast::DoWhileStmt *>(s);
                walk_expr(ds->cond.get());
                annotate_macro_param_idents(ds->body.get(), param_types);
                break;
            }
            case ast::NodeKind::ForStmt: {
                auto *fs = static_cast<ast::ForStmt *>(s);
                if (fs->init) annotate_macro_param_idents(fs->init.get(), param_types);
                if (fs->cond) walk_expr(fs->cond.get());
                if (fs->step) walk_expr(fs->step.get());
                annotate_macro_param_idents(fs->body.get(), param_types);
                break;
            }
            default:
                break;
        }
    }

    void Lowering::lower_function(ast::FunctionDecl *fd, ir::IrModule &out) {
        // Bug fix 2026-05-23: forward declarations no tienen body -- skip.
        if (fd->is_forward_decl || !fd->body) return;
        /* A.39: comptime fn (no-macro) NO se baja a IR.  Su body solo
         * se evalua en compile-time cuando es invocada desde un contexto
         * comptime.
         *
         * Phase MC.1 (A.43.22): @Macro bodies SI se lowean al IR (con
         * nombre `__macro_<original>`) cuando el body es lowerable.
         * Esto valida que la pipeline IR -> bytecode soporta el codigo
         * del macro; futuros sprints MC.2+ ejecutan ese bytecode via
         * una ComptimeVM para acelerar la metaprogramacion ~10-1000x.
         * Por ahora el IR queda en el modulo como dead code; el call
         * site del macro sigue usando el evaluator AST. */
        if (fd->is_comptime) {
            if (!fd->is_macro) return;
            /* @Macro: intentar lowear el body al IR.  Si contiene
             * caracteristicas no soportadas todavia (introspect,
             * comptime var, builtins comptime-only), saltar limpiamente
             * y dejar que el evaluator AST haga el trabajo. */
            const std::string reason =
                macro_body_unsupported_reason(tc_, fd->body.get());
            if (!reason.empty()) {
                /* No soportado -- fallback silencioso al AST eval.
                 * Capturamos el reason para diagnostico via
                 * VESTA_MC_VERBOSE (el usuario lo ve como
                 * "[mc-lower] M_xxx: AST-only (usa Y)"). */
                ++macro_skipped_count_;
                macro_skip_reasons_.emplace_back(fd->name, reason);
                return;
            }
            /* Pre-pase de annotation: los macros no pasan por
             * `check_functions` asi que los IdentExpr en el body tienen
             * result_type=VOID.  Anotamos los IdentExpr que matcheen
             * params del macro para que `lower_binary` detecte el caso
             * `code == "OK"` con `code: string` y emita STRCMP runtime.
             *
             * Bug en demo 162: comparaciones de string dentro del body
             * del macro emitian `cmpjmp` directo sobre los handles GC
             * sin invocar STRMAKE/STRCMP -> resultados incorrectos. */
            std::unordered_map<std::string, Type> macro_param_types;
            for (auto &p : fd->params) {
                if (p && p->type) {
                    macro_param_types[p->name] = tc_.resolve_type_node(p->type.get());
                }
            }
            if (!macro_param_types.empty()) {
                annotate_macro_param_idents(fd->body.get(), macro_param_types);
            }
            /* Continuar al lowering normal con nombre prefijado. */
        }
        /* Phase MC.17.1: setear flag para que lower_var_decl trate
         * `comptime var/const` LOCALES como vars runtime regulares.
         * Reset al salir de la funcion. */
        const bool prev_is_macro = current_fn_is_macro_;
        current_fn_is_macro_ = (fd->is_comptime && fd->is_macro);
        struct ScopeGuard {
            bool *flag;
            bool  saved;
            ~ScopeGuard() { *flag = saved; }
        } macro_flag_guard{&current_fn_is_macro_, prev_is_macro};

        ir::IrFunction fn;
        /* Phase MC.1: nombre prefijado para macros lowered al IR.
         * Asi no colisionan con funciones runtime y son identificables
         * por el TypeChecker para invocacion desde ComptimeVM (MC.2). */
        if (fd->is_macro && fd->is_comptime) {
            fn.name              = "__macro_" + fd->name;
            fn.is_macro_compiled = true;
            ++macro_lowered_count_;
            /* Phase MC.2: registrar en el ComptimeRuntime para que el
             * TypeChecker pueda intentar invocar via VM en futuras
             * iteraciones de la compilacion.  En MC.2 el entry_pc es
             * 0 (placeholder) -- MC.3 lo populara con la direccion
             * real tras el linker resolve. */
            const_cast<TypeChecker &>(tc_).comptime_runtime()
                .register_macro(fn.name, /*entry_pc=*/0);
        } else {
            fn.name = fd->name;
        }

        // Tipo de retorno.  Aceptamos tipos primitivos directamente o
        // pasamos por resolve_type_node para PointerTypeNode/ArrayTypeNode
        // (mapeados a IrType::PTR via ir_type_from_primitive).
        Type sem_ret = fd->return_type
                           ? tc_.resolve_type_node(fd->return_type.get())
                           : Type{PrimitiveKind::VOID};
        // sret: si la funcion declara devolver Optional<T>,
        // Result<V,E> o un enum declarado por usuario, internamente la
        // convertimos en void + un parametro hidden retbuf:ptr al inicio.
        // El callee escribe el resultado en el buffer del caller, evitando
        // heap allocation y leaks.
        const auto &elays_check = tc_.enum_layouts();
        const bool  sret_enum   =
                sem_ret.kind == PrimitiveKind::STRUCT
                && elays_check.find(sem_ret.struct_name) != elays_check.end();
        // (gap O): SRET para funciones que retornan FUNCTION.  El
        // slot del function value tiene 16 bytes (fn_addr + env_addr).
        const bool sret_function = (sem_ret.kind == PrimitiveKind::FUNCTION);
        // Smart pointers: SRET de 8 bytes para `unique<T>` / `shared<T>`.
        const bool sret_smartptr = (sem_ret.kind == PrimitiveKind::UNIQUE_PTR
            || sem_ret.kind == PrimitiveKind::SHARED_PTR);
        const bool sret          = (sem_ret.kind == PrimitiveKind::OPTIONAL
            || sem_ret.kind == PrimitiveKind::RESULT
            || sret_enum
            || sret_function
            || sret_smartptr);
        if (fd->return_type
            && fd->return_type->kind == ast::NodeKind::PrimitiveTypeNode) {
            auto *pt    = static_cast<ast::PrimitiveTypeNode *>(fd->return_type.get());
            fn.ret_type = ir_type_from_primitive(pt->prim);
        } else if (fd->return_type) {
            if (sret) {
                fn.ret_type = ir::IrType::VOID;
            } else {
                fn.ret_type = (sem_ret.kind != PrimitiveKind::COUNT
                                  && sem_ret.kind != PrimitiveKind::VOID)
                                  ? ir_type_from_primitive(sem_ret.kind)
                                  : ir::IrType::VOID;
            }
        } else {
            fn.ret_type = ir::IrType::VOID;
        }

        // Parametros: cada uno es un IrValue con is_param=true.
        std::vector<std::pair<std::string, ir::IrValueId> > param_bindings;
        param_bindings.reserve(fd->params.size() + (sret ? 1 : 0));
        // Hidden retbuf param para sret (si aplica): primero en la lista.
        ir::IrValueId v_retbuf = ir::IR_NO_VALUE;
        if (sret) {
            v_retbuf                     = fn.new_value(ir::IrType::PTR, "%__retbuf");
            fn.values[v_retbuf].is_param = true;
            // BugFix sret-cross-mem (2026-06-04): SOLO marcar host_ptr para
            // SRET de Optional/Result/enum (donde el caller aloca host).
            // Para FUNCTION/smart-ptr el callee tiene su propio manejo y
            // marcarlo host rompe el copia in-place.
            const bool sret_optres_like =
                (sem_ret.kind == PrimitiveKind::OPTIONAL
              || sem_ret.kind == PrimitiveKind::RESULT
              || sret_enum);
            if (sret_optres_like) {
                fn.values[v_retbuf].is_host_ptr = true;
            }
            fn.params.push_back(v_retbuf);
        }
        for (auto &p: fd->params) {
            ir::IrType pt                = ir::IrType::I64;
            bool       param_is_class    = false;
            bool       param_is_host_ptr = false;
            if (p->type && p->type->kind == ast::NodeKind::PrimitiveTypeNode) {
                auto *ptn = static_cast<ast::PrimitiveTypeNode *>(p->type.get());
                pt        = ir_type_from_primitive(ptn->prim);
            } else if (p->type) {
                // Para tipos compuestos (PointerTypeNode, ArrayTypeNode,
                // NamedTypeNode resuelto) usamos el helper de tipos del
                // checker para obtener el Type semantico y mapear su kind.
                const Type sem = tc_.resolve_type_node(p->type.get());
                if (sem.kind != PrimitiveKind::COUNT
                    && sem.kind != PrimitiveKind::VOID) {
                    pt = ir_type_from_primitive(sem.kind);
                }
                if (sem.kind == PrimitiveKind::CLASS) param_is_class = true;
                // Punteros raw (`T*`) y arrays (`T[]`) consultan @c is_virtual
                // del Type para decidir naturaleza del SSA value:
                //   T*               (is_virtual=false) -> host_ptr=true
                //   VirtualPtr<T>    (is_virtual=true)  -> host_ptr=false
                //   T[N] (decay)     (is_virtual=true)  -> host_ptr=false
                // Sin esta propagacion, indexar @c bdat[i] en parametros
                // emite mov (memoria VM) para tipos host -> garbage.
                if ((sem.kind == PrimitiveKind::PTR
                        || sem.kind == PrimitiveKind::ARRAY)
                    && !sem.is_virtual) {
                    param_is_host_ptr = true;
                }
                // BugFix sret-cross-mem (2026-06-04): los parametros de
                // tipo Optional<T>/Result<V,E> son PTRs al buffer SRET
                // alocado por el caller (que ahora siempre es host_alloca).
                // Sin marcar is_host_ptr=true, isOk/value/error en el callee
                // emiten LOAD con `mov` (VM mem) en lugar de `movh` (host)
                // -> Result llega zeroed al usar dentro del callee.
                if (sem.kind == PrimitiveKind::OPTIONAL
                    || sem.kind == PrimitiveKind::RESULT) {
                    param_is_host_ptr = true;
                }
                // Limitacion conocida (Phase A): `T[]` como tipo de
                // parametro es ambiguo entre array dinamico (host_ptr de
                // new T[N]) y stack-decay (VM addr de T[N] local).  Sin
                // overload resolution o anotacion explicita, mantenemos
                // is_virtual=true por defecto (VM addr) que cubre el
                // patron decay-to-pointer del test 11_arrays_nativos.
                // Para HOFs sobre arrays dinamicos (e.g. `Shape[]` de
                // new Shape[N]), usar `T*` explicito en el parametro
                // hasta que la distincion se resuelva en el type system.
            }
            const ir::IrValueId vid = fn.new_value(pt, "%" + p->name);
            fn.values[vid].is_param = true;
            if (param_is_class) {
                fn.values[vid].is_host_ptr  = true;
                fn.values[vid].is_gc_object = true;
            } else if (param_is_host_ptr) {
                fn.values[vid].is_host_ptr = true;
            }
            fn.params.push_back(vid);
            param_bindings.emplace_back(p->name, vid);
        }

        // Bloque entry.
        const ir::IrBlockId entry = fn.new_block("entry");
        // Conectar el estado del lowering al de esta funcion.
        fn_               = &fn;
        current_block_    = entry;
        block_terminated_ = false;
        scopes_.clear();
        push_scope();
        for (auto &kv: param_bindings) bind(kv.first, kv.second);

        // sret: configurar el contexto de la funcion actual.  Si
        // declara devolver Optional/Result, retbuf es el primer param
        // hidden y todos los `return` copiaran al buffer del caller en
        // vez de devolver un valor.
        sret_active_ = sret;
        sret_retbuf_ = v_retbuf;
        if (sret_enum) {
            // Tamano dinamico segun el enum declarado.
            auto it_e      = elays_check.find(sem_ret.struct_name);
            sret_buf_size_ = it_e != elays_check.end()
                                 ? static_cast<uint64_t>(it_e->second.size_bytes)
                                 : 16ULL;
        } else if (sret_function) {
            // el slot del function value es siempre 16 bytes.
            sret_buf_size_ = 16ULL;
        } else if (sret_smartptr) {
            // Smart pointer slot.  unique<T> usa Tier 1 (16 bytes: ptr + deleter).
            // shared<T> usa 8 bytes (host_ptr al control block; deleter
            // vive en el control block del GcHeap).  No tenemos forma
            // simple de discriminar aqui (sem_ret.kind UNIQUE vs SHARED);
            // usamos 16 para unique y 8 para shared.
            sret_buf_size_ = (sem_ret.kind == PrimitiveKind::UNIQUE_PTR) ? 16ULL : 8ULL;
        } else if (sret) {
            sret_buf_size_ = (sem_ret.kind == PrimitiveKind::OPTIONAL ? 16ULL : 24ULL);
        } else {
            sret_buf_size_ = 0ULL;
        }
        // (gap O): activar el modo "env en heap" para todos los
        // lambdas creados dentro del body de esta funcion.  Asi el env
        // sobrevive al RET y el caller puede invocar la closure sin
        // use-after-free.  Se restaura al salir de @c lower_function.
        const bool prev_returns_fn   = current_fn_returns_function_;
        current_fn_returns_function_ = sret_function;
        // Para `string get_x() { return "lit"; }` -- propaga al
        // lower_return para que detecte el literal y lo promueva via
        // STRMAKE en vez de devolver el ptr crudo.
        const bool prev_returns_str = current_fn_returns_string_;
        current_fn_returns_string_  = (sem_ret.kind == PrimitiveKind::STRING);

        // nonnull en parametros: por cada parametro declarado con
        // `T !!name` (o `nonnull T name`), inyectamos un `unwrap` al
        // entry de la funcion.  Si el caller pasa null, la excepcion
        // NullPointerException se lanza inmediatamente con stack trace
        // apuntando al entry del callee, lo que da diagnosticos
        // tempranos en vez de fallos lejanos al primer uso del param.
        for (size_t pi = 0; pi < fd->params.size(); ++pi) {
            const auto &p = fd->params[pi];
            if (!p || !p->type || !p->type->is_nonnull) continue;
            const ir::IrValueId v_old = param_bindings[pi].second;
            const ir::IrType    t_old = fn_->values[v_old].type;
            const ir::IrValueId v_new = fn_->new_value(t_old);
            // raw_asm-elim 2026-05-28: nonnull param check via IrOp::UNWRAP
            // (lanza FATAL_NULL_POINTER si src==0).  Reemplaza RAW_ASM.
            ir::IrInstr uw{};
            uw.op          = ir::IrOp::UNWRAP;
            uw.type        = t_old;
            uw.dst         = v_new;
            uw.operands    = {v_old};
            uw.source_line = p->loc.line;
            fn_->append(current_block_, std::move(uw));
            // Re-bind: futuros usos de p->name resuelven al valor unwrapped.
            if (fn_->values[v_old].is_host_ptr) {
                fn_->values[v_new].is_host_ptr = true;
            }
            // Sustituir el binding del scope (push_scope nuevo + el viejo
            // se reemplaza re-bindeando con bind() que sobrescribe).
            bind(p->name, v_new);
        }

        // Pre-pase: identificar variables locales cuya direccion se toma con
        // '&'.  Influye en lower_var_decl (ALLOCA en lugar de SSA) y en
        // read_local / write_local (LOAD/STORE).
        address_taken_locals_.clear();
        host_bearing_locals_.clear();
        // Limpiar mapa de labels de goto (per-funcion).
        goto_labels_.clear();
        if (fd->body) scan_address_taken(fd->body.get());
        // fix9 - eliminados los pre-pases scan_try / scan_loops.
        // Las flags `current_fn_has_try_` y `current_fn_has_loops_` solo
        // se usaban para decidir si emitir el cleanup RAW_ASM de fix
        // / fix5.  Tras fix8 (GC stack scanning conservativo),
        // esos cleanups ya no se emiten; los handles sin roots los colecta
        // el major_gc automaticamente.  Las flags quedan declaradas pero
        // siempre false, para minimizar el delta del header (eliminarlas
        // requiere actualizar miembros que pueden estar referenciados en
        // codigo no escaneado).
        current_fn_has_try_   = false;
        current_fn_has_loops_ = false;
        //escape detection para colecciones primitivas: detectar
        // locales cuyo handle se devuelve, asigna a campo o se almacena en
        // memoria.  Los marcados quedan fuera del cleanup automatico.
        escaping_locals_.clear();
        if (fd->body) scan_escaping_locals(fd->body.get());

        // limpiar el stack de cleanups (synchronized activos) al
        // entrar a una nueva funcion.  Cada funcion arranca sin cleanups;
        // las acciones se acumulan al bajar synchronized y se consumen al
        // emitir return o al cerrar el scope normalmente.
        cleanup_stack_.clear();

        // Si esta es 'main' y el modulo declara clases, insertar prologo
        // que invoca __module_init para registrarlas en el ClassRegistry
        // antes de ejecutar el cuerpo del usuario.
        if (fd->name == "main") {
            bool any_class = false;
            for (auto &decl: mod_.decls) {
                if (decl && decl->kind == ast::NodeKind::ClassDecl) {
                    any_class = true;
                    break;
                }
            }
            // Phase M6.b L.6: el root puede no declarar clases pero importar
            // alguna de un dep via `import "lib" only Counter;`.  En ese
            // caso, class_layouts() del TypeChecker contiene la clase
            // importada y necesitamos llamar a __module_init (que el merge
            // trae del dep) para registrarla en el ClassRegistry runtime.
            //
            // IMPORTANTE: filtrar las clases runtime-predefined (FatalError
            // etc.) que SIEMPRE estan en class_layouts y no requieren
            // __module_init.  Tambien filtrar clases declaradas localmente
            // en mod_.decls (ya cubiertas por el check de any_class arriba).
            if (!any_class) {
                std::unordered_set<std::string> local_class_names;
                for (auto &decl : mod_.decls) {
                    if (decl && decl->kind == ast::NodeKind::ClassDecl) {
                        local_class_names.insert(
                            static_cast<const ast::ClassDecl *>(decl.get())->name);
                    }
                }
                for (const auto &kv : tc_.class_layouts()) {
                    if (kv.second.is_runtime_predefined) continue;
                    if (local_class_names.count(kv.first)) continue;
                    // Clase no-local + no-runtime = importada de un dep.
                    any_class = true;
                    break;
                }
            }
            // L2.2: tambien llamar __module_init si hay globals runtime
            // que requieren inicializacion (string="lit" etc.).
            bool need_init = any_class || !runtime_global_slots_.empty();
            if (need_init) {
                ir::IrInstr call_init{};
                call_init.op          = ir::IrOp::CALL;
                call_init.type        = ir::IrType::VOID;
                call_init.dst         = ir::IR_NO_VALUE;
                call_init.func_name   = "__module_init";
                call_init.source_line = fd->loc.line;
                fn.append(current_block_, std::move(call_init));
            }
        }

        // Instrumentacion: vex_trace:enter al inicio.  Solo para funciones
        // de usuario (saltamos __module_init, __new_*, __async_*, __lambda_*,
        // __spawn_* y wrappers internos).  El bytecode VM, JIT y ports
        // heredan la instrumentacion porque vive en el IR.
        if (instrument_mode_ != "none" && instrument_mode_ != ""
            && fd->name != "__module_init"
            && fd->name.compare(0, 6, "__new_") != 0
            && fd->name.compare(0, 8, "__async_") != 0
            && fd->name.compare(0, 9, "__lambda_") != 0
            && fd->name.compare(0, 8, "__spawn_") != 0) {
            emit_instrument_enter(fd->name, fd->loc.line);
        }

        // Cuerpo.
        if (fd->body) {
            lower_block(fd->body.get());
        }

        // Cerrar la funcion: si la ultima instruccion no es terminador,
        // anyadir RET con valor por defecto (0) en funciones no-void, o
        // RET sin valor en void.
        if (!block_terminated_) {
            // emitir cleanups de auto-free de colecciones antes
            // del RET implicito.  Garantiza liberacion incluso si la
            // funcion cae al final sin un return explicito.
            emit_cleanups_all();
            // Instrumentacion: vex_trace:exit antes del RET implicito.
            if (instrument_mode_ != "none" && instrument_mode_ != ""
                && fd->name != "__module_init"
                && fd->name.compare(0, 6, "__new_") != 0
                && fd->name.compare(0, 8, "__async_") != 0
                && fd->name.compare(0, 9, "__lambda_") != 0
                && fd->name.compare(0, 8, "__spawn_") != 0) {
                emit_instrument_exit(fd->name, ir::IR_NO_VALUE, fd->loc.line);
            }
            ir::IrInstr ret{};
            ret.op   = ir::IrOp::RET;
            ret.type = fn.ret_type;
            if (fn.ret_type != ir::IrType::VOID) {
                const ir::IrValueId zero = emit_const(fn.ret_type, 0, fd->loc.line);
                ret.operands.push_back(zero);
            }
            ret.source_line = fd->loc.line;
            fn.append(current_block_, std::move(ret));
            block_terminated_ = true;
        }

        pop_scope();
        // (gap O): restaurar el flag de "funcion retorna FUNCTION".
        current_fn_returns_function_ = prev_returns_fn;
        current_fn_returns_string_   = prev_returns_str;
        // Validar que todas las labels referenciadas por gotos esten
        // declaradas; si alguna se quedo sin declarar es uso de una
        // label inexistente (`goto missing_label`).
        for (const auto &kv: goto_labels_) {
            if (!kv.second.declared) {
                error_at(kv.second.first_use_loc,
                         std::string("label '") + kv.first +
                         "' usada en goto pero nunca declarada");
            }
        }
        propagate_is_gc_object_through_phis(fn);
        out.add_function(std::move(fn));
        fn_ = nullptr;
    }

    // Bug D fix: propagar is_gc_object a traves de todos los PHI nodes hasta
    // punto fijo.  Cualquier IrValue cuya origen sea un host_ptr GC-managed
    // (clase) debe heredar el flag para que save_live_regs del IR emitter
    // convierta a gchandle pre-CALL (estable a evacuacion del GC) en lugar
    // de pushar host_ptr crudo.  Sin esta propagacion, los PHIs de
    // loops/if-merges con NULL inicial + valor real en back-edge perdian el
    // flag, causando segfaults tras cualquier CALL (e.g. str_make) que
    // disparara GC con head/tail vivos en regs.
    void Lowering::propagate_is_gc_object_through_phis(ir::IrFunction &fn) {
        bool gc_changed = true;
        while (gc_changed) {
            gc_changed = false;
            for (auto &blk: fn.blocks) {
                for (auto &ins: blk.instrs) {
                    if (ins.op != ir::IrOp::PHI) continue;
                    if (ins.dst == ir::IR_NO_VALUE) continue;
                    if (static_cast<size_t>(ins.dst) >= fn.values.size()) continue;
                    if (fn.values[ins.dst].is_gc_object) continue;
                    for (const auto &arg: ins.phi_args) {
                        if (static_cast<size_t>(arg.value) < fn.values.size()
                         && fn.values[arg.value].is_gc_object) {
                            fn.values[ins.dst].is_gc_object = true;
                            gc_changed = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------------
    // Statements.
    // ---------------------------------------------------------------------

    void Lowering::lower_block(ast::BlockStmt *b) {
        push_scope();
        // scope-local cleanup deferido por interaccion con
        // try/catch (el catch handler puede saltar en medio de un body
        // dejando cleanups de inner scopes en estado intermedio que
        // causan SEGFAULT al RET).
        //
        // Mantenemos el comportamiento original (cleanup al RET via
        // emit_cleanups_all en lower_return / final de lower_function).
        // Resultado: destructores corren al RET, no por iteracion del
        // loop.  Para destructores por iteracion, refactorizar el body
        // del loop a un helper auxiliar (cuyo RET dispara el dtor).
        bool warned_unreachable = false;
        for (auto &s: b->body) {
            if (block_terminated_) {
                if (s && s->kind == ast::NodeKind::LabelStmt) {
                    lower_stmt(s.get());
                    warned_unreachable = false;
                    continue;
                }
                if (!warned_unreachable) {
                    diags_.warning(s->loc, "codigo inalcanzable tras terminador");
                    warned_unreachable = true;
                }
                continue;
            }
            lower_stmt(s.get());
        }
        pop_scope();
    }

} // namespace vex
