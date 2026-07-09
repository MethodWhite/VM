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
    static void collect_assigned_vars(const ast::Node *      n,
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
    void Lowering::lower_synchronized(ast::SynchronizedStmt *s) {
        if (!s || !s->target) {
            error_at(s ? s->loc : SourceLoc{}, "lowering: synchronized sin target");
            return;
        }
        if (!s->body) {
            error_at(s->loc, "lowering: synchronized sin body");
            return;
        }

        // 1. Bajar el target a un valor SSA (host pointer al ObjectHeader).
        const ir::IrValueId v_obj = lower_expr(s->target.get());
        if (v_obj == ir::IR_NO_VALUE) return;

        // Crear bloques: body + handler (excepcion) + merge (continuacion).
        const ir::IrBlockId body_bb    = fn_->new_block("sync_body");
        const ir::IrBlockId handler_bb = fn_->new_block("sync_handler");
        const ir::IrBlockId merge_bb   = fn_->new_block("sync_merge");

        // 2. ptr -> handle + monenter en un RAW_ASM con {dst}={src0} (SSA-aware).
        // El regalloc asigna v_handle a un registro libre y monenter usa
        // ese mismo registro.  Crucial: no hardcodear r1/r2 aqui porque
        // colisionan con valores SSA vivos (el regalloc no inspecciona el
        // texto del RAW_ASM y no sabe que clobreamos).
        // Sprint 6.C: ptr -> handle via GC_HANDLE_FOR_PTR IR op + MONENTER IR op.
        const ir::IrValueId v_handle = emit_gc_handle_for_ptr(v_obj, s->loc.line);
        {
            ir::IrInstr me{};
            me.op          = ir::IrOp::MONENTER;
            me.type        = ir::IrType::VOID;
            me.dst         = ir::IR_NO_VALUE;
            me.operands    = {v_handle};
            me.source_line = s->loc.line;
            fn_->append(current_block_, std::move(me));
        }

        // tryenter catch-all: setup handler_pc y type=NULL via SSA values.
        const std::string   handler_label = fn_->name + "_" + fn_->blocks[handler_bb].name;
        const ir::IrValueId v_handler_pc  = emit_label_addr(handler_label, s->loc.line);
        const ir::IrValueId v_type_null   = emit_const(ir::IrType::I64, 0, s->loc.line); {
            ir::IrInstr ra{};
            ra.op          = ir::IrOp::TRYENTER;
            ra.type        = ir::IrType::VOID;
            ra.dst         = ir::IR_NO_VALUE;
            ra.operands    = {v_handler_pc, v_type_null};
            ra.source_line = s->loc.line;
            fn_->append(current_block_, std::move(ra));
        }

        // br body_bb.  Edges fantasmas a handler_bb (alcanzable via excepcion).
        {
            ir::IrInstr br{};
            br.op           = ir::IrOp::BR;
            br.target_block = body_bb;
            br.source_line  = s->loc.line;
            fn_->append(current_block_, std::move(br));
            fn_->blocks[current_block_].succs.push_back(body_bb);
            fn_->blocks[current_block_].succs.push_back(handler_bb);
            fn_->blocks[body_bb].preds.push_back(current_block_);
            fn_->blocks[handler_bb].preds.push_back(current_block_);
        }

        // 3. Push cleanup: tryleave + monexit en early-return.
        // Sprint 6.C: Kind::SYNC_EXIT emite TRYLEAVE + MONEXIT como IR ops.
        {
            CleanupAction act;
            act.kind        = CleanupAction::Kind::SYNC_EXIT;
            act.operands    = {v_handle};
            act.source_line = s->loc.line;
            cleanup_stack_.push_back(std::move(act));
        }

        // 4. Bajar el body.
        current_block_    = body_bb;
        block_terminated_ = false;
        lower_block(s->body.get());

        // 5. Pop cleanup (el body ya no necesita protegerse via emit_cleanups_all).
        cleanup_stack_.pop_back();

        // 6. Salida normal del body: TRYLEAVE + MONEXIT IR ops + br merge.
        if (!block_terminated_) {
            // Sprint 6.C: TRYLEAVE y MONEXIT son IR ops puros.
            {
                ir::IrInstr tl{};
                tl.op          = ir::IrOp::TRYLEAVE;
                tl.type        = ir::IrType::VOID;
                tl.dst         = ir::IR_NO_VALUE;
                tl.source_line = s->loc.line;
                fn_->append(current_block_, std::move(tl));
            }
            {
                ir::IrInstr me{};
                me.op          = ir::IrOp::MONEXIT;
                me.type        = ir::IrType::VOID;
                me.dst         = ir::IR_NO_VALUE;
                me.operands    = {v_handle};
                me.source_line = s->loc.line;
                fn_->append(current_block_, std::move(me));
            }

            ir::IrInstr brm{};
            brm.op           = ir::IrOp::BR;
            brm.target_block = merge_bb;
            brm.source_line  = s->loc.line;
            fn_->append(current_block_, std::move(brm));
            fn_->blocks[current_block_].succs.push_back(merge_bb);
            fn_->blocks[merge_bb].preds.push_back(current_block_);
            block_terminated_ = true;
        }

        // 7. Handler: alcanzable solo via excepcion del body.  do_throw nos
        // dejo el objeto excepcion en r0.  Hacemos monexit + rethrow para
        // que el caller decida que hacer con la excepcion.  Notese que NO
        // necesitamos tryleave aqui: do_throw ya consumio el frame al saltar.
        current_block_    = handler_bb;
        block_terminated_ = false; {
            // Sprint 6.C: handler = MONEXIT IR op + rethrow (RAW_ASM minimal).
            {
                ir::IrInstr me{};
                me.op          = ir::IrOp::MONEXIT;
                me.type        = ir::IrType::VOID;
                me.dst         = ir::IR_NO_VALUE;
                me.operands    = {v_handle};
                me.source_line = s->loc.line;
                fn_->append(current_block_, std::move(me));
            }
            // raw_asm-elim wave 3: rethrow IR op dedicado.  Terminator del
            // bloque (re-lanza current_exception; nada sigue a esta instr).
            ir::IrInstr rt{};
            rt.op          = ir::IrOp::RETHROW;
            rt.type        = ir::IrType::VOID;
            rt.dst         = ir::IR_NO_VALUE;
            rt.source_line = s->loc.line;
            fn_->append(current_block_, std::move(rt));
        }
        block_terminated_ = true; // rethrow es terminador del bloque

        // Continuar en merge (alcanzable solo via salida normal).
        current_block_    = merge_bb;
        block_terminated_ = false;
    }

    // ---------------------------------------------------------------------
    // spawn { body } -- arranca proceso hijo en scheduler actual.
    //
    // Estrategia:
    //   1. generate_spawn_helper compila el body como funcion sintetica
    //      __spawn_<N> con return type VOID + body original + hlt al final
    //      (NO ret: un proceso hijo no retorna a un caller que no existe).
    //   2. lower_spawn_expr emite RAW_ASM con:
    //        mov {dst_pc_holder}, @Absolute("code.__spawn_<N>")
    //        spawn {dst_pc_holder}
    //        mov {dst}, r0     ; r0 contiene el PID encoded del hijo
    //   3. SSA value devuelto = PID encoded como i64.
    // ---------------------------------------------------------------------

    // BugFix R3: scan recursivo del body para detectar IdentExprs que
    // referencian nombres NO definidos dentro del propio body.  Esos son
    // candidatos a captures que el spawn helper recibira como params.
    // Devuelve la lista de nombres unicos (orden de aparicion) que estan
    // en el scope encerrante actual via @c lookup.
    static void collect_spawn_captures_in_expr(
        const ast::Expr *e,
        std::unordered_set<std::string> &locals_defined,
        std::vector<std::string> &out_captures,
        std::unordered_set<std::string> &seen,
        Lowering &lw);
    static void collect_spawn_captures_in_stmt(
        const ast::Stmt *s,
        std::unordered_set<std::string> &locals_defined,
        std::vector<std::string> &out_captures,
        std::unordered_set<std::string> &seen,
        Lowering &lw);

    static void collect_spawn_captures_in_expr(
        const ast::Expr *e,
        std::unordered_set<std::string> &locals_defined,
        std::vector<std::string> &out_captures,
        std::unordered_set<std::string> &seen,
        Lowering &lw)
    {
        if (!e) return;
        switch (e->kind) {
            case ast::NodeKind::IdentExpr: {
                const auto *id = static_cast<const ast::IdentExpr *>(e);
                if (locals_defined.count(id->name)) return;
                if (seen.count(id->name)) return;
                // Buscar el nombre en TODOS los scopes activos del lowering.
                // Accedemos a scopes_ via friend-style desde el contexto
                // estatico (Lowering nos da acceso indirecto via lookup
                // publico mediante un wrapper inline).
                if (lw.spawn_capture_resolve_public(id->name) != ir::IR_NO_VALUE) {
                    seen.insert(id->name);
                    out_captures.push_back(id->name);
                }
                return;
            }
            case ast::NodeKind::FieldAccessExpr: {
                const auto *fa = static_cast<const ast::FieldAccessExpr *>(e);
                collect_spawn_captures_in_expr(fa->base.get(), locals_defined,
                                                out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::BinaryExpr: {
                const auto *b = static_cast<const ast::BinaryExpr *>(e);
                collect_spawn_captures_in_expr(b->lhs.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_expr(b->rhs.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::UnaryExpr: {
                const auto *u = static_cast<const ast::UnaryExpr *>(e);
                collect_spawn_captures_in_expr(u->operand.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::AssignExpr: {
                const auto *a = static_cast<const ast::AssignExpr *>(e);
                collect_spawn_captures_in_expr(a->target.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_expr(a->value.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::CallExpr: {
                const auto *c = static_cast<const ast::CallExpr *>(e);
                collect_spawn_captures_in_expr(c->callee.get(), locals_defined, out_captures, seen, lw);
                for (const auto &a : c->args)
                    collect_spawn_captures_in_expr(a.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::IndexExpr: {
                const auto *ix = static_cast<const ast::IndexExpr *>(e);
                collect_spawn_captures_in_expr(ix->base.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_expr(ix->index.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::NewExpr: {
                const auto *n = static_cast<const ast::NewExpr *>(e);
                for (const auto &a : n->args)
                    collect_spawn_captures_in_expr(a.get(), locals_defined, out_captures, seen, lw);
                if (n->array_size)
                    collect_spawn_captures_in_expr(n->array_size.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::CastExpr: {
                const auto *c = static_cast<const ast::CastExpr *>(e);
                collect_spawn_captures_in_expr(c->operand.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::TernaryExpr: {
                const auto *t = static_cast<const ast::TernaryExpr *>(e);
                collect_spawn_captures_in_expr(t->cond.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_expr(t->then_expr.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_expr(t->else_expr.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            default: return;
        }
    }

    static void collect_spawn_captures_in_stmt(
        const ast::Stmt *s,
        std::unordered_set<std::string> &locals_defined,
        std::vector<std::string> &out_captures,
        std::unordered_set<std::string> &seen,
        Lowering &lw)
    {
        if (!s) return;
        switch (s->kind) {
            case ast::NodeKind::BlockStmt: {
                const auto *b = static_cast<const ast::BlockStmt *>(s);
                for (const auto &st : b->body)
                    collect_spawn_captures_in_stmt(st.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::VarDeclStmt: {
                const auto *v = static_cast<const ast::VarDeclStmt *>(s);
                if (v->init)
                    collect_spawn_captures_in_expr(v->init.get(), locals_defined, out_captures, seen, lw);
                locals_defined.insert(v->name);
                return;
            }
            case ast::NodeKind::ExprStmt: {
                const auto *e = static_cast<const ast::ExprStmt *>(s);
                collect_spawn_captures_in_expr(e->expr.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::IfStmt: {
                const auto *i = static_cast<const ast::IfStmt *>(s);
                collect_spawn_captures_in_expr(i->cond.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_stmt(i->then_branch.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_stmt(i->else_branch.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::WhileStmt: {
                const auto *w = static_cast<const ast::WhileStmt *>(s);
                collect_spawn_captures_in_expr(w->cond.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_stmt(w->body.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::ForStmt: {
                const auto *f = static_cast<const ast::ForStmt *>(s);
                collect_spawn_captures_in_stmt(f->init.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_expr(f->cond.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_expr(f->step.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_stmt(f->body.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::SynchronizedStmt: {
                const auto *sy = static_cast<const ast::SynchronizedStmt *>(s);
                collect_spawn_captures_in_expr(sy->target.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_stmt(sy->body.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::ReturnStmt: {
                const auto *r = static_cast<const ast::ReturnStmt *>(s);
                if (r->value)
                    collect_spawn_captures_in_expr(r->value.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::ThrowStmt: {
                const auto *t = static_cast<const ast::ThrowStmt *>(s);
                if (t->value)
                    collect_spawn_captures_in_expr(t->value.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::TryStmt: {
                // Bug fix audit Z.2: scan recursivo del try body + catches + finally.
                // Sin esto, capturas usadas dentro de try{} se reportan como
                // "nombre no resuelto" en el spawn body.
                const auto *t = static_cast<const ast::TryStmt *>(s);
                if (t->body) collect_spawn_captures_in_stmt(
                    static_cast<const ast::Stmt *>(t->body.get()),
                    locals_defined, out_captures, seen, lw);
                for (const auto &cc : t->catches) {
                    if (cc.body) collect_spawn_captures_in_stmt(
                        static_cast<const ast::Stmt *>(cc.body.get()),
                        locals_defined, out_captures, seen, lw);
                }
                if (t->finally_body) collect_spawn_captures_in_stmt(
                    static_cast<const ast::Stmt *>(t->finally_body.get()),
                    locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::DoWhileStmt: {
                const auto *dw = static_cast<const ast::DoWhileStmt *>(s);
                collect_spawn_captures_in_stmt(dw->body.get(), locals_defined, out_captures, seen, lw);
                collect_spawn_captures_in_expr(dw->cond.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            case ast::NodeKind::ForEachStmt: {
                const auto *fe = static_cast<const ast::ForEachStmt *>(s);
                collect_spawn_captures_in_expr(fe->iter_expr.get(), locals_defined, out_captures, seen, lw);
                locals_defined.insert(fe->iter_name);
                collect_spawn_captures_in_stmt(fe->body.get(), locals_defined, out_captures, seen, lw);
                return;
            }
            default: return;
        }
    }

    std::string Lowering::generate_spawn_helper(ast::BlockStmt *body, const SourceLoc &loc) {
        const size_t      spawn_idx = spawn_func_counter_++;
        const std::string fn_name   = "__spawn_" + std::to_string(spawn_idx);

        // BugFix R3: pre-scan del body para detectar capturas (idents
        // libres que estan en el scope encerrante).  Cada captura se
        // pasara como param al helper via la convencion @c spawnargs.
        std::vector<std::string> captures;
        {
            std::unordered_set<std::string> locals_defined;
            std::unordered_set<std::string> seen;
            collect_spawn_captures_in_stmt(body, locals_defined, captures, seen, *this);
            // Limitamos a 11 captures (R1..R12 menos el implicit return reg).
            if (captures.size() > 11) {
                error_at(loc,
                    "spawn { body }: maximo 11 capturas soportadas (recibidos "
                    + std::to_string(captures.size()) + ")");
                captures.resize(11);
            }
        }
        // Capturar los SSA values de las capturas en el caller ANTES de
        // cambiar de contexto.  Esos seran los args de spawnargs.
        spawn_captured_ssa_values_.clear();
        spawn_captured_names_ = captures;
        for (const auto &nm : captures) {
            ir::IrValueId v = lookup(nm);
            spawn_captured_ssa_values_.push_back(v);
        }

        // Phase Z.9: escape analysis cross-process.  Para cada captura que
        // es un objeto GC (is_gc_object=true en su SSA value) Y NO fue
        // declarada con @c shared, emitir warning con sugerencia clara.
        // Esto detecta el bug clasico (t13): pasar un objeto local al
        // spawn body, que no puede resolverlo cross-process.
        for (size_t i = 0; i < captures.size(); ++i) {
            const ir::IrValueId v = spawn_captured_ssa_values_[i];
            if (v == ir::IR_NO_VALUE) continue;
            if (v >= fn_->values.size()) continue;
            const auto &val = fn_->values[v];
            // Solo warn para GC objects (host_ptrs a objetos GC).
            if (!val.is_gc_object) continue;
            // Si el local fue declarado con @c shared, todo OK.
            if (shared_locals_.count(captures[i])) continue;
            // Sugerencia con loc del spawn (no del var-decl original; el
            // user vera el spawn body que es donde el problema importa).
            diags_.warning(loc,
                "spawn captura '" + captures[i] +
                "' (objeto GC local-only); el child no podra resolver el "
                "host_ptr cross-process.\n"
                "  sugerencia: declara con 'shared': `shared T " +
                captures[i] + " = new T();`  o llama `" +
                captures[i] + " = share(" + captures[i] + ")` antes del spawn.");
        }

        // Guardar el contexto del lowering (estamos dentro de la funcion
        // padre que invoca spawn) y crear uno nuevo para la funcion hijo.
        ir::IrFunction *                                             saved_fn         = fn_;
        ir::IrBlockId                                                saved_block      = current_block_;
        bool                                                         saved_terminated = block_terminated_;
        std::vector<std::unordered_map<std::string, ir::IrValueId> > saved_scopes
                = std::move(scopes_);
        std::unordered_set<std::string> saved_addr_taken
                = std::move(address_taken_locals_);
        std::vector<CleanupAction> saved_cleanups
                = std::move(cleanup_stack_);

        // Construir la nueva IrFunction.
        ir::IrFunction child_fn;
        child_fn.name             = fn_name;
        child_fn.ret_type         = ir::IrType::VOID;
        const ir::IrBlockId entry = child_fn.new_block("entry");

        fn_               = &child_fn;
        current_block_    = entry;
        block_terminated_ = false;
        scopes_.clear();
        push_scope();
        address_taken_locals_.clear();
        host_bearing_locals_.clear();
        cleanup_stack_.clear();

        // BugFix R3: registrar las capturas como params del helper.  Cada
        // captura llega via la calling convention spawnargs (R1, R2, ...).
        for (size_t i = 0; i < captures.size(); ++i) {
            ir::IrValueId v = child_fn.new_value(ir::IrType::I64,
                                                  "%" + captures[i]);
            child_fn.values[v].is_param = true;
            // Si el SSA value original era host_ptr CLASS, propagarlo.
            if (spawn_captured_ssa_values_[i] != ir::IR_NO_VALUE
             && spawn_captured_ssa_values_[i] < saved_fn->values.size()) {
                const auto &src_val = saved_fn->values[spawn_captured_ssa_values_[i]];
                if (src_val.is_host_ptr)  child_fn.values[v].is_host_ptr = true;
                if (src_val.is_gc_object) child_fn.values[v].is_gc_object = true;
            }
            child_fn.params.push_back(v);
            // Bindear el nombre en el topmost scope para que IdentExpr
            // resuelva via @c lookup.
            scopes_.back()[captures[i]] = v;
        }

        // Setup de pila: ahora lo hace exec_instr_spawn directamente al
        // crear el proceso hijo (rsp/rbp = base unica por local_pid).
        // El frontend solo necesita asegurarse de que las instrucciones
        // de `enter`/`leave` y las locales caben en la region (1 MiB
        // por defecto, mas que suficiente para spawn bodies tipicos).

        if (body) lower_block(body);

        // Sprint 6.D: terminador HLT via IR op puro (no RAW_ASM).
        if (!block_terminated_) {
            ir::IrInstr h{};
            h.op          = ir::IrOp::HLT;
            h.type        = ir::IrType::VOID;
            h.dst         = ir::IR_NO_VALUE;
            h.source_line = loc.line;
            fn_->append(current_block_, std::move(h));
            block_terminated_ = true;
        }

        pop_scope();
        // Guardar el helper en la cola pendiente; se anyadira a out_mod_
        // al final de run() para que main se mantenga como primera funcion
        // (el emisor IR la trata como entry point y termina con hlt).
        pending_spawn_helpers_.push_back(std::move(child_fn));

        // Restaurar el contexto del padre.
        fn_                   = saved_fn;
        current_block_        = saved_block;
        block_terminated_     = saved_terminated;
        scopes_               = std::move(saved_scopes);
        address_taken_locals_ = std::move(saved_addr_taken);
        cleanup_stack_        = std::move(saved_cleanups);
        return fn_name;
    }

    // ---------------------------------------------------------------------
    // rspawn helper generator.  Identico a generate_spawn_helper
    // pero con `is_rspawn_body_ = true` activado mientras se baja el body
    // para que cualquier `return X` se intercepte en `lower_return` y se
    // transforme en `mov r0, X + hlt`.  El runtime distribuido captura R0
    // al detectar HALT en un proceso con `rspawn_future_id != 0` y envia
    // VDP_FUTURE_FULFILL al nodo origen con ese valor.
    // ---------------------------------------------------------------------
    std::string Lowering::generate_rspawn_helper(ast::BlockStmt *body, const SourceLoc &loc) {
        const size_t      spawn_idx = spawn_func_counter_++;
        const std::string fn_name   = "__rspawn_" + std::to_string(spawn_idx);

        // Guardar contexto del lowering del padre.
        ir::IrFunction *                                             saved_fn         = fn_;
        ir::IrBlockId                                                saved_block      = current_block_;
        bool                                                         saved_terminated = block_terminated_;
        std::vector<std::unordered_map<std::string, ir::IrValueId> > saved_scopes
                = std::move(scopes_);
        std::unordered_set<std::string> saved_addr_taken
                = std::move(address_taken_locals_);
        std::vector<CleanupAction> saved_cleanups
                = std::move(cleanup_stack_);
        bool saved_rspawn = is_rspawn_body_;

        ir::IrFunction child_fn;
        child_fn.name             = fn_name;
        child_fn.ret_type         = ir::IrType::VOID;
        const ir::IrBlockId entry = child_fn.new_block("entry");

        fn_               = &child_fn;
        current_block_    = entry;
        block_terminated_ = false;
        scopes_.clear();
        push_scope();
        address_taken_locals_.clear();
        host_bearing_locals_.clear();
        cleanup_stack_.clear();
        is_rspawn_body_ = true; // activar interception de return en lower_return

        if (body) lower_block(body);

        // Sprint 6.D: terminador HLT con R0=0 via IR op puro.
        if (!block_terminated_) {
            const ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0, loc.line);
            ir::IrInstr h{};
            h.op          = ir::IrOp::HLT;
            h.type        = ir::IrType::VOID;
            h.dst         = ir::IR_NO_VALUE;
            h.operands    = {v_zero};
            h.source_line = loc.line;
            fn_->append(current_block_, std::move(h));
            block_terminated_ = true;
        }

        pop_scope();
        pending_spawn_helpers_.push_back(std::move(child_fn));

        // Restaurar contexto.
        fn_                   = saved_fn;
        current_block_        = saved_block;
        block_terminated_     = saved_terminated;
        scopes_               = std::move(saved_scopes);
        address_taken_locals_ = std::move(saved_addr_taken);
        cleanup_stack_        = std::move(saved_cleanups);
        is_rspawn_body_       = saved_rspawn;
        return fn_name;
    }

    ir::IrValueId Lowering::lower_rspawn_expr(ast::RSpawnExpr *e) {
        if (!e || !e->body || !e->node_idx) {
            error_at(e ? e->loc : SourceLoc{}, "lowering: rspawn sin body o sin node_idx");
            return ir::IR_NO_VALUE;
        }
        // 1. Generar la funcion remota y obtener su nombre (label .vel).
        const std::string fn_name = generate_rspawn_helper(e->body.get(), e->loc);

        // 2. Cargar la direccion absoluta del helper en un SSA value.
        const ir::IrValueId v_pc = emit_label_addr(fn_name, e->loc.line);

        // 3. Bajar la expresion del node_idx y promover a I64.
        ir::IrValueId v_node = lower_expr(e->node_idx.get());
        if (v_node == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        const ir::IrType src_t = fn_->values[v_node].type;
        v_node                 = cast_if_needed(v_node, src_t, ir::IrType::I64, e->loc.line);

        // 4. RSPAWN IR op (0xD2): node_idx + fn_addr -> Future handle en R0.
        const ir::IrValueId v_fut = fn_->new_value(ir::IrType::I64); {
            ir::IrInstr rs{};
            rs.op           = ir::IrOp::RSPAWN;
            rs.type         = ir::IrType::I64;
            rs.dst          = v_fut;
            // Convencion del IR emitter de RSPAWN: operands[0]=node_idx,
            // operands[1]=fn_addr.  Ver case IrOp::RSPAWN en ir_emitter.cpp.
            rs.operands     = {v_node, v_pc};
            rs.set_is_call_site(true);
            rs.source_line  = e->loc.line;
            fn_->append(current_block_, std::move(rs));
        }
        return v_fut;
    }

    // ---------------------------------------------------------------------
    // closures: generate_lambda_helper + lower_lambda_expr.
    //
    // Diseno:
    //   - Cada @c LambdaExpr produce una @c IrFunction sintetica
    //     @c __lambda_<N>(p0, p1, ...) cuyos params son los declarados
    //     por la lambda.  Los captures se pasan via R14 (env_ptr) y se
    //     leen en el prologue del helper desde @c [r14 + 8*i] a SSA
    //     values con el nombre de la captura.  Asi el body trata las
    //     capturas como si fueran locales, sin distinguirlas de los
    //     params (el type checker ya las acumulo en e->captures).
    //
    //   - El call site emite:
    //       1. ALLOCA @c 8*N bytes para el env block (si N > 0).
    //       2. STORE de cada captura en @c [env + 8*i].
    //       3. ALLOCA 16 bytes para el "function value":
    //            `[+0 fn_addr][+8 env_addr]`.
    //       4. RAW_ASM @c mov ..., @c \@Absolute("code.__lambda_<N>")
    //          + STORE en @c [fv+0].
    //       5. STORE @c env_addr en @c [fv+8] (o 0 si sin captures).
    //
    //   - Al llamar a la closure (vease @c lower_call cuando callee es
    //     IdentExpr de tipo FUNCTION): se cargan @c fn_addr y @c env_addr
    //     del slot, y se emite IrOp::CALLCLOSURE con func_ptr=fn_addr y
    //     operands=[env_addr, args...].  El emisor IR coloca env en R14,
    //     args en R1..R12 via parallel-move y emite @c callvmr.
    // ---------------------------------------------------------------------

    std::string Lowering::generate_lambda_helper(ast::LambdaExpr *e) {
        const size_t      lam_idx = lambda_counter_++;
        const std::string fn_name = "__lambda_" + std::to_string(lam_idx);

        // Salvar contexto del padre para poder restaurarlo despues.
        ir::IrFunction *                                             saved_fn         = fn_;
        ir::IrBlockId                                                saved_block      = current_block_;
        bool                                                         saved_terminated = block_terminated_;
        std::vector<std::unordered_map<std::string, ir::IrValueId> > saved_scopes
                = std::move(scopes_);
        std::unordered_set<std::string> saved_addr_taken
                = std::move(address_taken_locals_);
        std::vector<CleanupAction> saved_cleanups
                = std::move(cleanup_stack_);
        // el helper sintetico es una FUNCION SEPARADA con su propia
        // firma.  No debe heredar el SRET del padre (que se referia al
        // retbuf del padre).  El helper retorna un valor escalar i32/i64
        // mediante R0, sin SRET.  Save y reset.
        const bool          saved_sret_active   = sret_active_;
        const ir::IrValueId saved_sret_retbuf   = sret_retbuf_;
        const uint64_t      saved_sret_buf_size = sret_buf_size_;
        const bool          saved_returns_fn    = current_fn_returns_function_;
        sret_active_                            = false;
        sret_retbuf_                            = ir::IR_NO_VALUE;
        sret_buf_size_                          = 0;
        // El helper en si mismo no retorna FUNCTION (los tests actuales
        // no anidan factories de closures dentro de lambdas).  Si en el
        // futuro lo necesitamos, detectarlo via e->result_type.pointee.
        current_fn_returns_function_ = false;

        // Construir la nueva IrFunction.
        ir::IrFunction child_fn;
        child_fn.name = fn_name;
        // Determinar return type del helper a partir del tipo deducido en
        // la lambda (e->result_type es Type{FUNCTION, ...}; pointee es el
        // return type).  Por defecto VOID si no hay informacion.
        ir::IrType ret_ir = ir::IrType::VOID;
        if (e->result_type.kind == PrimitiveKind::FUNCTION
            && e->result_type.pointee
            && e->result_type.pointee->kind != PrimitiveKind::VOID) {
            ret_ir = ir_type_from_primitive(e->result_type.pointee->kind);
        }
        child_fn.ret_type = ret_ir;

        // Declarar parametros en la signature del helper IR.  Usamos los
        // mismos nombres y tipos que el AST de la lambda; la convencion
        // del emisor coloca cada param en r1, r2, ...
        // Modelo del IR: @c IrFunction::params es @c vector<IrValueId>;
        // los nombres se mantienen aparte para hacer el bind() en el
        // scope local del lowering tras crear el bloque entry.
        std::vector<std::pair<std::string, ir::IrValueId> > param_bindings;
        param_bindings.reserve(e->params.size());
        for (size_t i = 0; i < e->params.size(); ++i) {
            ir::IrType pt  = ir::IrType::I64;
            const Type sem = tc_.resolve_type_node(e->params[i]->type.get());
            if (sem.kind != PrimitiveKind::COUNT
                && sem.kind != PrimitiveKind::VOID) {
                pt = ir_type_from_primitive(sem.kind);
            }
            ir::IrValueId pv             = child_fn.new_value(pt, "%" + e->params[i]->name);
            child_fn.values[pv].is_param = true;
            child_fn.params.push_back(pv);
            param_bindings.emplace_back(e->params[i]->name, pv);
        }

        const ir::IrBlockId entry = child_fn.new_block("entry");

        fn_               = &child_fn;
        current_block_    = entry;
        block_terminated_ = false;
        scopes_.clear();
        push_scope();
        address_taken_locals_.clear();
        host_bearing_locals_.clear();
        cleanup_stack_.clear();

        // Bind de los params en el scope local.
        for (auto &kv: param_bindings) bind(kv.first, kv.second);

        // Prologue de captures: leer cada @c [r14 + 8*i] a un SSA value
        // que se bindea con el nombre del capture.  Asi el resto del body
        // referencia las capturas como si fueran variables ordinarias.
        //
        // El env_ptr esta en R14 por convencion (callvmr -> R14 ya cargado
        // por el emisor al ejecutar CALLCLOSURE).  Para acceder a R14
        // desde el IR usamos un RAW_ASM `mov {dst}, r14` que captura el
        // valor a un SSA value.
        ir::IrValueId env_ptr = ir::IR_NO_VALUE;
        if (!e->captures.empty()) {
            env_ptr = child_fn.new_value(ir::IrType::PTR);
            // (gap O): si la lambda fue marcada como env_in_heap,
            // el env_ptr en R14 apunta a HEAP RAW (host_ptr), no a
            // stack VM.  Marcamos is_host_ptr para que LOAD/STORE
            // contra el env block emitan @c movh en lugar de @c mov.
            if (e->env_in_heap) {
                child_fn.values[env_ptr].is_host_ptr = true;
            }
            // raw_asm-elim wave 3: prologue del closure helper lee R14 (env_ptr)
            // via IrOp::READ_VM_REG.  Reemplaza el viejo RAW_ASM `mov {dst}, r14`.
            ir::IrInstr rr{};
            rr.op          = ir::IrOp::READ_VM_REG;
            rr.type        = ir::IrType::PTR;
            rr.dst         = env_ptr;
            rr.imm         = 14;   // R14 = env_ptr en la calling convention de callclosure
            rr.source_line = e->loc.line;
            child_fn.append(entry, std::move(rr));

            for (size_t i = 0; i < e->captures.size(); ++i) {
                // Tipo del capture: usar el tipo guardado por el type
                // checker.  Si por algun motivo es COUNT/VOID, defaulteamos
                // a i64 (el ancho de los slots del env).
                ir::IrType cap_ir = ir::IrType::I64;
                if (i < e->capture_types.size()
                    && e->capture_types[i].kind != PrimitiveKind::COUNT
                    && e->capture_types[i].kind != PrimitiveKind::VOID) {
                    cap_ir = ir_type_from_primitive(e->capture_types[i].kind);
                }

                // ¿Es esta captura mutable?  Si lo es, el env contiene un
                // PUNTERO a la celda en el outer scope (capture-by-ref);
                // si no, contiene el VALOR (capture-by-value, copiado).
                bool is_mutable = false;
                for (const auto &nm: e->mutable_captures) {
                    if (nm == e->captures[i]) {
                        is_mutable = true;
                        break;
                    }
                }

                // addr_i = env_ptr + 8*i (0 -> reusamos env_ptr directamente)
                ir::IrValueId addr_i = env_ptr;
                if (i > 0) {
                    addr_i = child_fn.new_value(ir::IrType::PTR);
                    // Propagar is_host_ptr para que el LOAD siguiente sepa
                    // emitir @c movh contra heap raw cuando el env vive en
                    // heap (caso de closures retornadas por una funcion,
                    // donde el env sobrevive al stack del creador via
                    // alocacion en heap GC).
                    if (child_fn.values[env_ptr].is_host_ptr) {
                        child_fn.values[addr_i].is_host_ptr = true;
                    }
                    ir::IrValueId off = emit_const(ir::IrType::I64,
                                                   static_cast<uint64_t>(i * 8),
                                                   e->loc.line);
                    ir::IrInstr ad{};
                    ad.op          = ir::IrOp::ADD;
                    ad.type        = ir::IrType::I64;
                    ad.dst         = addr_i;
                    ad.operands    = {env_ptr, off};
                    ad.source_line = e->loc.line;
                    child_fn.append(entry, std::move(ad));
                }

                // Leer 8 bytes (i64) del slot.  Para mutable_capture esto
                // es el PTR a la celda; para by-value es el valor.
                ir::IrValueId raw_v = child_fn.new_value(
                    is_mutable ? ir::IrType::PTR : ir::IrType::I64);
                ir::IrInstr ld{};
                ld.op          = ir::IrOp::LOAD;
                ld.type        = ir::IrType::I64;
                ld.dst         = raw_v;
                ld.operands    = {addr_i};
                ld.source_line = e->loc.line;
                child_fn.append(entry, std::move(ld));

                if (is_mutable) {
                    // Capture-by-reference: el SSA value `raw_v` es el PTR
                    // a la celda en el outer scope.  Lo bindeamos como
                    // address_taken_local del helper para que read_local /
                    // write_local emitan LOAD/STORE indirectos.
                    address_taken_locals_.insert(e->captures[i]);
                    bind(e->captures[i], raw_v);
                } else {
                    // Capture-by-value: cast/trunc si el tipo es mas
                    // estrecho que i64.
                    ir::IrValueId final_v = raw_v;
                    if (cap_ir != ir::IrType::I64 && cap_ir != ir::IrType::PTR) {
                        final_v = child_fn.new_value(cap_ir);
                        ir::IrInstr cv{};
                        cv.op          = ir::IrOp::TRUNC;
                        cv.type        = cap_ir;
                        cv.dst         = final_v;
                        cv.operands    = {raw_v};
                        cv.source_line = e->loc.line;
                        child_fn.append(entry, std::move(cv));
                    }
                    bind(e->captures[i], final_v);
                }
            }
        }

        // Lower del body.  Si no hay return explicito, anadimos un RET
        // void al final para garantizar terminador.  Este patron lo usan
        // tambien generate_spawn_helper / generate_rspawn_helper.
        if (e->body) lower_block(e->body.get());
        if (!block_terminated_) {
            ir::IrInstr rt{};
            rt.op          = ir::IrOp::RET;
            rt.type        = ir::IrType::VOID;
            rt.source_line = e->loc.line;
            fn_->append(current_block_, std::move(rt));
            block_terminated_ = true;
        }

        pop_scope();
        // Encolar el helper para volcado al modulo al final de run().
        pending_spawn_helpers_.push_back(std::move(child_fn));

        // Restaurar contexto del padre.
        fn_                   = saved_fn;
        current_block_        = saved_block;
        block_terminated_     = saved_terminated;
        scopes_               = std::move(saved_scopes);
        address_taken_locals_ = std::move(saved_addr_taken);
        cleanup_stack_        = std::move(saved_cleanups);
        // restaurar contexto SRET y returns_function del padre.
        sret_active_                 = saved_sret_active;
        sret_retbuf_                 = saved_sret_retbuf;
        sret_buf_size_               = saved_sret_buf_size;
        current_fn_returns_function_ = saved_returns_fn;
        return fn_name;
    }

    ir::IrValueId Lowering::lower_lambda_expr(ast::LambdaExpr *e) {
        if (!e || !e->body) {
            error_at(e ? e->loc : SourceLoc{}, "lowering: lambda sin body");
            return ir::IR_NO_VALUE;
        }
        // Capturar valores ANTES de generar el helper.  El helper modifica
        // pending_spawn_helpers_ y temporalmente intercambia el contexto
        // (fn_, scopes_), asi que los SSA values de los captures debemos
        // leerlos en el frame del CALLER, antes de cambiar de contexto.
        // El type checker ya pre-computo @c e->captures y @c e->capture_types
        // recorriendo el body y registrando IdentExpr externos.
        const size_t               N = e->captures.size();
        std::vector<ir::IrValueId> capture_vals;
        capture_vals.reserve(N);
        for (size_t i = 0; i < N; ++i) {
            // ¿Es esta captura mutable?  Si lo es, guardamos el PTR a la
            // celda outer (capture-by-reference).  Si no, el VALOR
            // (capture-by-value, snapshot).
            bool is_mutable = false;
            for (const auto &nm: e->mutable_captures) {
                if (nm == e->captures[i]) {
                    is_mutable = true;
                    break;
                }
            }

            ir::IrType cap_ir = ir::IrType::I64;
            if (i < e->capture_types.size()
                && e->capture_types[i].kind != PrimitiveKind::COUNT
                && e->capture_types[i].kind != PrimitiveKind::VOID) {
                cap_ir = ir_type_from_primitive(e->capture_types[i].kind);
            }
            ir::IrValueId v = ir::IR_NO_VALUE;
            if (is_mutable) {
                // Para captures mutables, scan_address_taken ya marco
                // la variable outer como address-taken.  lookup devuelve
                // la direccion del ALLOCA estable; eso es lo que
                // queremos guardar en el env.
                v = lookup(e->captures[i]);
            } else {
                // Capture-by-value: leer el valor actual.  Si es
                // address-taken (por algun otro motivo, e.g. &var),
                // read_local emite el LOAD apropiado.
                v = read_local(e->captures[i], cap_ir, e->loc.line);
            }
            if (v == ir::IR_NO_VALUE) {
                v = emit_const(cap_ir, 0, e->loc.line);
            }
            capture_vals.push_back(v);
        }

        // (gap O): MARCAR el flag env_in_heap ANTES de generar
        // el helper, ya que el helper inspecciona @c e->env_in_heap para
        // marcar @c env_ptr.is_host_ptr=true en su prologue.  La
        // condicion: hay capturas (N>0) Y la funcion contenedora
        // retorna FUNCTION.
        if (N > 0 && current_fn_returns_function_) {
            e->env_in_heap = true;
        }

        // Generar el helper sintetico con su prologue de captures.
        const std::string fn_name = generate_lambda_helper(e);
        e->synthetic_name         = fn_name;

        // marker: emitir MAKE_CLOSURE ANTES de la secuencia explicita
        // de ALLOCA env + STOREs + ALLOCA fv + STORE fn + STORE env.
        // Identifica la construccion completa para que el C2 JIT 
        // pueda hacer escape analysis y eventualmente promover env a stack /
        // eliminar la alocacion si la closure no escapa.  El IR emitter
        // actual lo trata como no-op; las instrucciones siguientes hacen el
        // trabajo real.  Capacidad de capturas marcadas como mutables: 16
        // (caben en bits 1..16 del imm; >= 16 deja el sobrante sin marcar y
        // el C2 cae al path conservativo).
        {
            uint64_t mutable_mask = 0;
            for (size_t i = 0; i < N && i < 16; ++i) {
                for (const auto &nm: e->mutable_captures) {
                    if (nm == e->captures[i]) {
                        mutable_mask |= (1ULL << i);
                        break;
                    }
                }
            }
            ir::IrInstr mc{};
            mc.op          = ir::IrOp::MAKE_CLOSURE;
            mc.type        = ir::IrType::VOID;
            mc.dst         = ir::IR_NO_VALUE;
            mc.operands    = capture_vals;          // N captures como SSA values
            mc.func_name   = fn_name;               // nombre del helper sintetico
            mc.imm         = (e->env_in_heap ? 1ULL : 0ULL)  // bit 0: env_kind
                           | (mutable_mask << 1);            // bits 1..16: mutable mask
            mc.source_line = e->loc.line;
            fn_->append(current_block_, std::move(mc));
        }

        // -------------------------------------------------------------
        // 1. Alocar env block (8*N bytes; un qword por capture sin huecos).
        //
        // Por defecto va en STACK del caller (ALLOCA) - cero overhead GC,
        // suficiente para closures que no escapan al scope donde nacen.
        //
        // (gap O): si la funcion contenedora retorna FUNCTION (es
        // decir: probablemente esta cerrando esta lambda como su valor
        // de retorno), alocamos el env block en HEAP RAW via RAW_ALLOC.
        // El env sobrevive al RET y la closure es invocable por el caller
        // sin use-after-free.  Coste: un leak controlado por env (no hay
        // free automatico todavia; se libera cuando el proceso muere).
        // Se marca @c is_host_ptr en el SSA value para que LOAD/STORE
        // contra el env block emitan @c movh (host mem) en lugar de
        // @c mov (vm mem).
        // -------------------------------------------------------------
        ir::IrValueId env_addr;
        if (N == 0) {
            env_addr = emit_const(ir::IrType::I64, 0, e->loc.line);
        } else if (e->env_in_heap) {
            // Heap GC-tracked via GC_ALLOC.  El bloque entra en HandleTable
            // y el GC ve el payload (mark_reachable lo escanea como qword
            // array): si algun capture es un GcHandle vivo, se mantiene
            // marcado transitivamente; cuando ningun root referencia el
            // env (function value muere), se libera en el proximo major_gc.
            //
            // Marcamos is_host_ptr=true para que los STOREs siguientes (de
            // los captures al env) emitan movh (memoria HOST), ya que el
            // GcHeap usa ArenaManager con VirtualAlloc/mmap (host).
            //
            // Sustituye al RAW_ALLOC anterior (cerrado: gap O / leak en
            // closures que escapan).  Coste vs RAW_ALLOC: 1 slot extra en
            // HandleTable + zero-init del payload (que ya hacia rawalloc).
            env_addr                          = fn_->new_value(ir::IrType::PTR);
            fn_->values[env_addr].is_host_ptr = true;
            const ir::IrValueId v_size        = emit_const(ir::IrType::I64,
                                                           static_cast<uint64_t>(N * 8),
                                                           e->loc.line);
            ir::IrInstr ins{};
            ins.op          = ir::IrOp::GC_ALLOC;
            ins.type        = ir::IrType::PTR;
            ins.dst         = env_addr;
            ins.operands    = {v_size};
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
        } else {
            // Stack via ALLOCA (cero overhead, valido si la closure no
            // escapa al scope donde nace).
            env_addr = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8; // unidad: 1 byte
            al.dst         = env_addr;
            al.imm         = static_cast<uint64_t>(N * 8);
            al.source_line = e->loc.line;
            fn_->append(current_block_, std::move(al));
        }

        // Escribir cada capture en su slot del env (ambos casos: stack y
        // heap).  STORE i64 para todos (uniformidad).  En el caso heap el
        // is_host_ptr propagado hace que el emisor IR use movh.
        if (N > 0) {
            for (size_t i = 0; i < N; ++i) {
                ir::IrValueId addr_i = env_addr;
                if (i > 0) {
                    addr_i = fn_->new_value(ir::IrType::PTR);
                    if (fn_->values[env_addr].is_host_ptr) {
                        fn_->values[addr_i].is_host_ptr = true;
                    }
                    ir::IrValueId off = emit_const(ir::IrType::I64,
                                                   static_cast<uint64_t>(i * 8),
                                                   e->loc.line);
                    ir::IrInstr ad{};
                    ad.op          = ir::IrOp::ADD;
                    ad.type        = ir::IrType::I64;
                    ad.dst         = addr_i;
                    ad.operands    = {env_addr, off};
                    ad.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(ad));
                }
                // Si el capture es de un tipo mas estrecho que i64,
                // primero promocionamos a i64 para que el slot sea
                // siempre 8 bytes.  Para PTR / I64 / valor de funcion
                // (16 bytes en sentido lexico, pero aqui el SSA value
                // es el puntero al slot, asi que i64 es correcto) la
                // promocion es identidad.
                ir::IrValueId v  = capture_vals[i];
                ir::IrType    vt = fn_->values[v].type;
                if (vt != ir::IrType::I64 && vt != ir::IrType::PTR) {
                    v = cast_if_needed(v, vt, ir::IrType::I64, e->loc.line);
                }

                ir::IrInstr st{};
                st.op          = ir::IrOp::STORE;
                st.type        = ir::IrType::I64;
                st.operands    = {v, addr_i};
                st.source_line = e->loc.line;
                fn_->append(current_block_, std::move(st));
            }
        }

        // -------------------------------------------------------------
        // 2. Alocar slot 16 bytes para el function value.
        // -------------------------------------------------------------
        ir::IrValueId fv_addr = fn_->new_value(ir::IrType::PTR); {
            ir::IrInstr al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8;
            al.dst         = fv_addr;
            al.imm         = 16;
            al.source_line = e->loc.line;
            fn_->append(current_block_, std::move(al));
        }

        // -------------------------------------------------------------
        // 3. Cargar fn_addr (= @Absolute("code.__lambda_<N>")) via LABEL_ADDR.
        // -------------------------------------------------------------
        ir::IrValueId fn_addr = emit_label_addr(fn_name, e->loc.line);

        // -------------------------------------------------------------
        // 4. STORE fn_addr en [fv_addr+0] y env_addr en [fv_addr+8].
        // -------------------------------------------------------------
        {
            ir::IrInstr st{};
            st.op          = ir::IrOp::STORE;
            st.type        = ir::IrType::I64;
            st.operands    = {fn_addr, fv_addr};
            st.source_line = e->loc.line;
            fn_->append(current_block_, std::move(st));
        } {
            ir::IrValueId fv_plus_8 = fn_->new_value(ir::IrType::PTR);
            ir::IrValueId off8      = emit_const(ir::IrType::I64, 8, e->loc.line);
            ir::IrInstr   ad{};
            ad.op          = ir::IrOp::ADD;
            ad.type        = ir::IrType::I64;
            ad.dst         = fv_plus_8;
            ad.operands    = {fv_addr, off8};
            ad.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ad));

            ir::IrInstr st{};
            st.op          = ir::IrOp::STORE;
            st.type        = ir::IrType::I64;
            st.operands    = {env_addr, fv_plus_8};
            st.source_line = e->loc.line;
            fn_->append(current_block_, std::move(st));
        }

        // El SSA value de la lambda es la direccion del slot de 16 bytes.
        // Cuando se asigna a una variable @c fn(...), bind() la registra
        // tal cual; cuando se llama, lower_call carga fn_addr y env_addr
        // del slot y emite CALLCLOSURE.
        return fv_addr;
    }

    // ---------------------------------------------------------------------
    // ADTs: lower_enum_constructor + lower_match_expr.
    //
    // Layout del slot del enum (mismo para todas las variantes):
    //   [+0  i64 tag]
    //   [+8  i64 payload[0]]
    //   [+16 i64 payload[1]] ...
    // Tamano total: 8 + 8 * max_payload_fields.  Cada payload se
    // promociona a i64 para tener acceso uniforme por offset.  Cero
    // alocaciones de heap; cero overhead GC; mismo modelo que
    // Optional / Result.
    // ---------------------------------------------------------------------

    ir::IrValueId Lowering::lower_enum_constructor(
        const std::string &                             enum_name,
        const std::string &                             variant_name,
        const std::vector<std::unique_ptr<ast::Expr> > &args,
        const SourceLoc &                               loc) {
        // Localizar el layout del enum y la variante.
        const auto &elays = tc_.enum_layouts();
        auto        it    = elays.find(enum_name);
        if (it == elays.end()) {
            error_at(loc, "lowering: enum desconocido '" + enum_name + "'");
            return ir::IR_NO_VALUE;
        }
        const EnumLayout &     elay = it->second;
        const EnumVariantInfo *var  = nullptr;
        for (const auto &v: elay.variants) {
            if (v.name == variant_name) {
                var = &v;
                break;
            }
        }
        if (!var) {
            error_at(loc, "lowering: variante desconocida '" + variant_name +
                     "' en enum '" + enum_name + "'");
            return ir::IR_NO_VALUE;
        }

        // marker: MAKE_VARIANT identifica la construccion completa de
        // un valor ADT.  Emitido ANTES de la secuencia ALLOCA + STOREs para
        // que el C2 JIT (Phase D.8) pueda reconocer el patron y aplicar
        // escape analysis (promocion del slot a regs si no escapa) +
        // case-splitting eficiente del match downstream.  No produce SSA
        // value; el emitter actual lo trata como no-op.
        //
        // Lower de los args ANTES del marker para que sus SSA values
        // esten disponibles como operandos.  Cada payload se promueve a
        // un slot de 8 bytes (i64).  Para floats (F32/F64) usamos BITCAST
        // (preserva los bits IEEE) en lugar de FTOI/F2I (que truncaria
        // el valor).  El bug se manifiesta como `Circle(5.0)` con payload
        // 0 porque FTOI(5.0) -> 5, pero luego al destructurar como f64
        // se interpreta 5 como bits IEEE de un denormal cerca de cero.
        std::vector<ir::IrValueId> payload_vals;
        payload_vals.reserve(args.size());
        for (size_t i = 0; i < args.size() && i < var->field_types.size(); ++i) {
            // Auto-promotion literal -> StringObject cuando el payload
            // tipo es STRING.  Sin esto, `Token.Word("hello")` almacena
            // el raw ptr del literal en lugar del GcHandle, y la
            // extraccion `case Word(s) => s` da un ptr invalido al
            // intentar usarlo como string.
            ir::IrValueId v;
            const ast::Expr *ae = args[i].get();
            if (var->field_types[i].kind == PrimitiveKind::STRING
             && ae && ae->kind == ast::NodeKind::StringLitExpr) {
                auto *slit = const_cast<ast::StringLitExpr *>(
                    static_cast<const ast::StringLitExpr *>(ae));
                v = lower_string_literal_to_string_object(slit);
            } else {
                v = lower_expr(args[i].get());
            }
            if (v == ir::IR_NO_VALUE) {
                v = emit_const(ir::IrType::I64, 0, loc.line);
            }
            ir::IrType vt = fn_->values[v].type;
            if (vt == ir::IrType::F64) {
                // bitcast f64 -> i64 (mismo ancho, preserva bits IEEE).
                ir::IrValueId v2 = fn_->new_value(ir::IrType::I64);
                ir::IrInstr   bc{};
                bc.op          = ir::IrOp::BITCAST;
                bc.type        = ir::IrType::I64;
                bc.dst         = v2;
                bc.operands    = {v};
                bc.source_line = loc.line;
                fn_->append(current_block_, std::move(bc));
                v = v2;
            } else if (vt == ir::IrType::F32) {
                // f32: primero ampliar a f64 (preserva el valor), luego
                // bitcast a i64 (preserva los bits IEEE).
                ir::IrValueId vw = fn_->new_value(ir::IrType::F64); {
                    ir::IrInstr ext{};
                    ext.op          = ir::IrOp::F32TOF64;
                    ext.type        = ir::IrType::F64;
                    ext.dst         = vw;
                    ext.operands    = {v};
                    ext.source_line = loc.line;
                    fn_->append(current_block_, std::move(ext));
                }
                ir::IrValueId v2 = fn_->new_value(ir::IrType::I64); {
                    ir::IrInstr bc{};
                    bc.op          = ir::IrOp::BITCAST;
                    bc.type        = ir::IrType::I64;
                    bc.dst         = v2;
                    bc.operands    = {vw};
                    bc.source_line = loc.line;
                    fn_->append(current_block_, std::move(bc));
                }
                v = v2;
            } else if (vt != ir::IrType::I64 && vt != ir::IrType::PTR) {
                // Tipos enteros mas estrechos: promocion normal a i64
                // (sign/zero-extend segun signedness).
                v = cast_if_needed(v, vt, ir::IrType::I64, loc.line);
            }
            payload_vals.push_back(v);
        }
        {
            ir::IrInstr mv{};
            mv.op          = ir::IrOp::MAKE_VARIANT;
            mv.type        = ir::IrType::VOID;
            mv.dst         = ir::IR_NO_VALUE;
            mv.operands    = payload_vals;
            mv.func_name   = enum_name + "." + variant_name;
            mv.imm         = static_cast<uint64_t>(var->tag);
            mv.source_line = loc.line;
            fn_->append(current_block_, std::move(mv));
        }

        // 1. ALLOCA slot del enum (size_bytes = 8 + 8*max_payload_fields).
        const ir::IrValueId addr = fn_->new_value(ir::IrType::PTR); {
            ir::IrInstr al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8;
            al.dst         = addr;
            al.imm         = static_cast<uint64_t>(elay.size_bytes);
            al.source_line = loc.line;
            fn_->append(current_block_, std::move(al));
        }

        // 2. STORE i64 tag en offset 0 (= addr).
        {
            ir::IrValueId tag_v = emit_const(ir::IrType::I64,
                                             static_cast<uint64_t>(var->tag),
                                             loc.line);
            ir::IrInstr st{};
            st.op          = ir::IrOp::STORE;
            st.type        = ir::IrType::I64;
            st.operands    = {tag_v, addr};
            st.source_line = loc.line;
            fn_->append(current_block_, std::move(st));
        }

        // 3. STORE de cada payload arg en offset 8 + 8*i (promovido a i64).
        // Reusa los payload_vals ya lowereados arriba (para el marker
        // MAKE_VARIANT): evita doble-lowering de los args.
        for (size_t i = 0; i < payload_vals.size(); ++i) {
            ir::IrValueId v = payload_vals[i];
            if (v == ir::IR_NO_VALUE) continue;

            // Calcular addr_i = addr + (8 + 8*i).
            const uint64_t off    = 8ULL + 8ULL * static_cast<uint64_t>(i);
            ir::IrValueId  addr_i = fn_->new_value(ir::IrType::PTR);
            ir::IrValueId  off_v  = emit_const(ir::IrType::I64, off, loc.line);
            ir::IrInstr    ad{};
            ad.op          = ir::IrOp::ADD;
            ad.type        = ir::IrType::I64;
            ad.dst         = addr_i;
            ad.operands    = {addr, off_v};
            ad.source_line = loc.line;
            fn_->append(current_block_, std::move(ad));

            ir::IrInstr st{};
            st.op          = ir::IrOp::STORE;
            st.type        = ir::IrType::I64;
            st.operands    = {v, addr_i};
            st.source_line = loc.line;
            fn_->append(current_block_, std::move(st));
        }

        // El SSA value de la expresion es la direccion del slot.
        return addr;
    }

    ir::IrValueId Lowering::lower_match_expr(ast::MatchExpr *e) {
        if (!e || !e->scrutinee) {
            error_at(e ? e->loc : SourceLoc{}, "lowering: match sin scrutinee");
            return ir::IR_NO_VALUE;
        }
        // Tipo del scrutinee debe ser STRUCT con struct_name en
        // enum_layouts_.  Si no, el type checker ya reporto error y
        // devolvemos NO_VALUE silenciosamente para no inundar.
        const Type st = e->scrutinee->result_type;
        if (st.kind != PrimitiveKind::STRUCT) return ir::IR_NO_VALUE;
        const auto &elays = tc_.enum_layouts();
        auto        it    = elays.find(st.struct_name);
        if (it == elays.end()) return ir::IR_NO_VALUE;
        const EnumLayout &elay = it->second;

        // 1. Lower del scrutinee -> SSA PTR al slot.
        ir::IrValueId scrut_addr = lower_expr(e->scrutinee.get());
        if (scrut_addr == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;

        // marker: MATCH_VARIANT identifica el inicio del match.  Emitido
        // ANTES del LOAD del tag + cadena cmp+br para que el C2 JIT
        // reconozca el patron y emita dispatch eficiente
        // (jumptable si tags densos, switch tree balanceado si dispersos).
        // No produce SSA value; el emitter actual lo trata como no-op.
        {
            size_t n_concrete = 0;
            for (const auto &arm: e->arms) {
                if (arm.variant_name != "_") n_concrete++;
            }
            ir::IrInstr mt{};
            mt.op          = ir::IrOp::MATCH_VARIANT;
            mt.type        = ir::IrType::VOID;
            mt.dst         = ir::IR_NO_VALUE;
            mt.operands    = {scrut_addr};
            mt.func_name   = st.struct_name;  // nombre del enum
            mt.imm         = static_cast<uint64_t>(n_concrete);
            mt.source_line = e->loc.line;
            fn_->append(current_block_, std::move(mt));
        }

        // 2. LOAD i64 del tag en offset 0.
        ir::IrValueId tag_v = fn_->new_value(ir::IrType::I64); {
            ir::IrInstr ld{};
            ld.op          = ir::IrOp::LOAD;
            ld.type        = ir::IrType::I64;
            ld.dst         = tag_v;
            ld.operands    = {scrut_addr};
            ld.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ld));
        }

        // 3. Construir bloques: uno por arm + uno default + uno merge.
        // Estrategia simple O(N): para cada arm con variant concreto,
        // emitimos un cmp_eq + br_cond a su body block.  Si ninguno
        // matchea, caemos en default_block (= arm "_") o merge_block
        // (continuacion del programa) si no hay default.
        const ir::IrBlockId merge_bb = fn_->new_block("match_end");
        // Localizar arm default (si existe).
        ssize_t default_arm_idx = -1;
        for (size_t i = 0; i < e->arms.size(); ++i) {
            if (e->arms[i].variant_name == "_") {
                default_arm_idx = static_cast<ssize_t>(i);
                break;
            }
        }
        ir::IrBlockId default_bb = (default_arm_idx >= 0)
                                       ? fn_->new_block("match_default")
                                       : merge_bb;

        // Pre-crear un bloque por cada arm concreto (no-default).
        std::vector<ir::IrBlockId> arm_blocks(e->arms.size(), ir::IR_NO_BLOCK);
        // Bug fix 2026-05-23: arm_fall_bbs[i] = fall_bb del cmp del arm i.
        // Si el arm tiene guard y falla en runtime, saltamos a este bloque
        // (que reanuda con el cmp del arm siguiente).
        std::vector<ir::IrBlockId> arm_fall_bbs(e->arms.size(), ir::IR_NO_BLOCK);
        for (size_t i = 0; i < e->arms.size(); ++i) {
            if (static_cast<ssize_t>(i) == default_arm_idx) continue;
            arm_blocks[i] = fn_->new_block(
                std::string("match_arm_") + e->arms[i].variant_name);
        }

        // Encadenar cmps en el bloque actual: si tag == tag_i, br a
        // arm_blocks[i]; si no, fall-through al siguiente cmp.  El
        // ultimo cmp falla -> br a default_bb (o merge_bb si no hay
        // default).
        for (size_t i = 0; i < e->arms.size(); ++i) {
            if (static_cast<ssize_t>(i) == default_arm_idx) continue;
            // Buscar el tag de la variante.
            const EnumVariantInfo *var = nullptr;
            for (const auto &v: elay.variants) {
                if (v.name == e->arms[i].variant_name) {
                    var = &v;
                    break;
                }
            }
            if (!var) continue;

            // cmp_eq tag_v == var->tag
            ir::IrValueId cmp_v     = fn_->new_value(ir::IrType::BOOL);
            ir::IrValueId tag_const = emit_const(ir::IrType::I64,
                                                 static_cast<uint64_t>(var->tag),
                                                 e->arms[i].loc.line); {
                ir::IrInstr cm{};
                cm.op          = ir::IrOp::CMP_EQ;
                cm.type        = ir::IrType::BOOL;
                cm.dst         = cmp_v;
                cm.operands    = {tag_v, tag_const};
                cm.source_line = e->arms[i].loc.line;
                fn_->append(current_block_, std::move(cm));
            }

            // br_cond cmp -> arm_blocks[i], else fall_block
            const ir::IrBlockId fall_bb = fn_->new_block("match_next");
            arm_fall_bbs[i] = fall_bb;  // para uso si la arm tiene guard
            ir::IrInstr         br{};
            br.op = ir::IrOp::BR_COND;
            br.operands.push_back(cmp_v);
            br.target_block = arm_blocks[i];
            br.false_block  = fall_bb;
            br.source_line  = e->arms[i].loc.line;
            fn_->append(current_block_, std::move(br));
            fn_->blocks[current_block_].succs.push_back(arm_blocks[i]);
            fn_->blocks[current_block_].succs.push_back(fall_bb);
            fn_->blocks[arm_blocks[i]].preds.push_back(current_block_);
            fn_->blocks[fall_bb].preds.push_back(current_block_);

            current_block_    = fall_bb;
            block_terminated_ = false;
        }
        // Tras la cadena de cmps, el bloque actual es la rama "ninguna
        // variante matcheada".  Saltamos al default si existe, o al
        // merge directamente.
        {
            ir::IrInstr br{};
            br.op           = ir::IrOp::BR;
            br.target_block = default_bb;
            br.source_line  = e->loc.line;
            fn_->append(current_block_, std::move(br));
            fn_->blocks[current_block_].succs.push_back(default_bb);
            fn_->blocks[default_bb].preds.push_back(current_block_);
        }

        // Snapshot bindings ANTES de las arms para PHI en merge.
        // Mismo patron que lower_try: cada arm asigna en el scope enclosing,
        // y al merge llegan multiples binding distintos.  Sin PHI el
        // binding final es el de la ultima arm lowered (no determinista).
        //
        // BUG FIX 2026-05-26: guardar TODOS los scopes enclosing, no solo
        // el inmediato.  Antes solo se restauraba scopes_.back() entre
        // arms; pero un arm puede modificar vars en cualquier nivel deeper
        // (e.g. `n` declarada en la funcion, dentro de un match dentro
        // de un while).  Esas mutaciones leakean al siguiente arm si
        // no restauramos toda la stack de scopes.
        const auto entry_all_scopes_match = scopes_;
        // Mantener compat con codigo que referencia entry_bindings_match
        // (solo el outer del match) para el PHI de merge.
        const auto entry_bindings_match = scopes_.back();

        struct ArmSnapshot {
            // Cada arm captura TODOS los niveles de scope post-body para
            // poder hacer PHI merge a multiples niveles.
            std::vector<std::unordered_map<std::string, ir::IrValueId>>
                                                           all_scopes;
            std::unordered_map<std::string, ir::IrValueId> bindings;
            ir::IrBlockId                                  pred;
            bool                                           reaches_merge;
        };
        std::vector<ArmSnapshot> arm_snaps;
        arm_snaps.reserve(e->arms.size());

        // 4. Emitir el body de cada arm (incluido el default).
        for (size_t i = 0; i < e->arms.size(); ++i) {
            const auto &        arm    = e->arms[i];
            const ir::IrBlockId target = (static_cast<ssize_t>(i) == default_arm_idx)
                                             ? default_bb
                                             : arm_blocks[i];
            current_block_    = target;
            block_terminated_ = false;
            // Restaurar TODOS los scopes enclosing al estado de entry
            // antes de cada arm.  Sin esto, mutaciones de outer-outer
            // scopes en arm_A se ven en arm_B (-> SSA values cruzados).
            scopes_ = entry_all_scopes_match;
            push_scope();

            // Bind de los payload bindings (si la variante tiene
            // payload).  Para cada binding i, emit LOAD i64 de
            // [scrut_addr + 8 + 8*i] y bind con el nombre del binding.
            //
            // Para tipos float, el slot guarda BITS IEEE como i64
            // (escrito por el constructor via BITCAST).  Aqui bitcasteamos
            // de vuelta a F64 antes del bind para que las operaciones
            // posteriores (multiplicacion, comparacion) usen FMUL/FCMP en
            // vez de IMUL/CMP int.  Para F32 originalmente guardado:
            // BITCAST i64 -> f64 + F64TOF32 narrow (recupera el valor).
            if (arm.variant_name != "_") {
                // Localizar la EnumVariantInfo para conocer field_types.
                const EnumVariantInfo *arm_var = nullptr;
                for (const auto &vinfo: elay.variants) {
                    if (vinfo.name == arm.variant_name) {
                        arm_var = &vinfo;
                        break;
                    }
                }
                for (size_t bi = 0; bi < arm.bindings.size(); ++bi) {
                    const uint64_t off    = 8ULL + 8ULL * static_cast<uint64_t>(bi);
                    ir::IrValueId  addr_i = fn_->new_value(ir::IrType::PTR);
                    ir::IrValueId  off_v  = emit_const(ir::IrType::I64, off,
                                                       arm.loc.line); {
                        ir::IrInstr ad{};
                        ad.op          = ir::IrOp::ADD;
                        ad.type        = ir::IrType::I64;
                        ad.dst         = addr_i;
                        ad.operands    = {scrut_addr, off_v};
                        ad.source_line = arm.loc.line;
                        fn_->append(current_block_, std::move(ad));
                    }
                    ir::IrValueId v = fn_->new_value(ir::IrType::I64); {
                        ir::IrInstr ld{};
                        ld.op          = ir::IrOp::LOAD;
                        ld.type        = ir::IrType::I64;
                        ld.dst         = v;
                        ld.operands    = {addr_i};
                        ld.source_line = arm.loc.line;
                        fn_->append(current_block_, std::move(ld));
                    }
                    // Si el field declarado es float, bitcastear i64 -> f64
                    // (recupera los bits IEEE escritos por el constructor).
                    if (arm_var && bi < arm_var->field_types.size()) {
                        const Type &ft = arm_var->field_types[bi];
                        if (ft.kind == PrimitiveKind::F64) {
                            ir::IrValueId v2 = fn_->new_value(ir::IrType::F64);
                            ir::IrInstr   bc{};
                            bc.op          = ir::IrOp::BITCAST;
                            bc.type        = ir::IrType::F64;
                            bc.dst         = v2;
                            bc.operands    = {v};
                            bc.source_line = arm.loc.line;
                            fn_->append(current_block_, std::move(bc));
                            v = v2;
                        } else if (ft.kind == PrimitiveKind::F32) {
                            // Recuperar f32: el slot guardo BITCAST(F32TOF64(x))
                            // como i64.  Invertimos: BITCAST i64->f64 +
                            // F64TOF32 narrow para volver al f32 original.
                            ir::IrValueId vd = fn_->new_value(ir::IrType::F64); {
                                ir::IrInstr bc{};
                                bc.op          = ir::IrOp::BITCAST;
                                bc.type        = ir::IrType::F64;
                                bc.dst         = vd;
                                bc.operands    = {v};
                                bc.source_line = arm.loc.line;
                                fn_->append(current_block_, std::move(bc));
                            }
                            ir::IrValueId v2 = fn_->new_value(ir::IrType::F32); {
                                ir::IrInstr nr{};
                                nr.op          = ir::IrOp::F64TOF32;
                                nr.type        = ir::IrType::F32;
                                nr.dst         = v2;
                                nr.operands    = {vd};
                                nr.source_line = arm.loc.line;
                                fn_->append(current_block_, std::move(nr));
                            }
                            v = v2;
                        }
                        // Para tipos enteros mas estrechos, dejar v como i64;
                        // el codigo de uso aplicara cast_if_needed cuando
                        // sea necesario (sign-extend / truncate).
                    }
                    bind(arm.bindings[bi], v);
                }
            }

            // Bug fix 2026-05-23: si la arm tiene guard, evaluarlo ANTES
            // del body.  Si falso, saltar al fall_bb (siguiente cmp).
            if (arm.guard && arm.variant_name != "_") {
                const ir::IrValueId guard_v = lower_expr(arm.guard.get());
                if (guard_v != ir::IR_NO_VALUE && arm_fall_bbs[i] != ir::IR_NO_BLOCK) {
                    const ir::IrBlockId body_bb = fn_->new_block("match_arm_body");
                    ir::IrInstr br{};
                    br.op = ir::IrOp::BR_COND;
                    br.operands.push_back(guard_v);
                    br.target_block = body_bb;
                    br.false_block  = arm_fall_bbs[i];
                    br.source_line  = arm.loc.line;
                    fn_->append(current_block_, std::move(br));
                    fn_->blocks[current_block_].succs.push_back(body_bb);
                    fn_->blocks[current_block_].succs.push_back(arm_fall_bbs[i]);
                    fn_->blocks[body_bb].preds.push_back(current_block_);
                    fn_->blocks[arm_fall_bbs[i]].preds.push_back(current_block_);
                    current_block_    = body_bb;
                    block_terminated_ = false;
                }
            }

            if (arm.body) lower_stmt(arm.body.get());
            // Capturar snapshot ANTES de pop_scope.  Guardamos TANTO
            // el scope outer inmediato (size-2) PARA compatibilidad con
            // el PHI merge existente, COMO la stack completa (size-2 y
            // todos los inferiores) para PHI multi-nivel.
            ArmSnapshot snap;
            snap.reaches_merge = !block_terminated_;
            if (snap.reaches_merge) {
                if (scopes_.size() >= 2) {
                    snap.bindings = scopes_[scopes_.size() - 2];
                    // Capturar todos los niveles enclosing (excluyendo
                    // el scope local del arm, size-1).  Necesario para
                    // que el PHI insert al merge atrape mutaciones de
                    // vars declaradas en funcion/loop body/etc.
                    snap.all_scopes.assign(scopes_.begin(),
                                            scopes_.begin() + (scopes_.size() - 1));
                }
                snap.pred = current_block_;
            } else {
                snap.pred = ir::IR_NO_BLOCK;
            }
            arm_snaps.push_back(std::move(snap));

            if (!block_terminated_) {
                // br merge_bb
                ir::IrInstr br{};
                br.op           = ir::IrOp::BR;
                br.target_block = merge_bb;
                br.source_line  = arm.loc.line;
                fn_->append(current_block_, std::move(br));
                fn_->blocks[current_block_].succs.push_back(merge_bb);
                fn_->blocks[merge_bb].preds.push_back(current_block_);
                block_terminated_ = true;
            }
            pop_scope();
        }

        // Si NO hubo arm default y el merge_bb es el destino del
        // fall-through "ninguna variante matcheo", aseguramos que
        // continuamos en merge_bb.  Si hubo default, el fall_block ya
        // saltaba a default_bb que a su vez hace br a merge_bb.
        current_block_    = merge_bb;
        block_terminated_ = false;

        // PHI insertion en merge_bb para variables modificadas
        // en multiples arms.  Mismo algoritmo que lower_try.
        // Reset TODOS los scopes al estado de entry; el PHI insert
        // multi-nivel sobreescribe las vars que difieren.
        scopes_ = entry_all_scopes_match;
        struct MergeContrib2 {
            ir::IrBlockId pred;
            const std::vector<std::unordered_map<std::string, ir::IrValueId>>
                          *all_scopes;
        };
        std::vector<MergeContrib2> contribs;
        contribs.reserve(arm_snaps.size());
        for (auto &as: arm_snaps) {
            if (as.reaches_merge) {
                contribs.push_back({as.pred, &as.all_scopes});
            }
        }
        // PHI insertion multi-nivel: por cada scope-level enclosing del
        // match, para cada var, si los arms producen valores distintos,
        // insertar PHI en merge_bb y actualizar el scope correspondiente.
        // Esto cierra el bug del match-en-loop donde mutaciones a vars
        // declaradas en niveles outer-outer (e.g. funcion) no eran
        // PHI-merged y la back-edge tomaba el valor del ULTIMO arm.
        if (contribs.size() >= 2) {
            const size_t n_levels = entry_all_scopes_match.size();
            for (size_t lvl = 0; lvl < n_levels; ++lvl) {
                for (const auto &kv: entry_all_scopes_match[lvl]) {
                    const std::string & name      = kv.first;
                    const ir::IrValueId entry_val = kv.second;
                    bool                any_diff  = false;
                    for (const auto &c: contribs) {
                        if (lvl >= c.all_scopes->size()) continue;
                        auto it_b = (*c.all_scopes)[lvl].find(name);
                        if (it_b == (*c.all_scopes)[lvl].end()) continue;
                        if (it_b->second != entry_val) {
                            any_diff = true;
                            break;
                        }
                    }
                    if (!any_diff) continue;

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
                    phi.source_line = e->loc.line;
                    for (const auto &c: contribs) {
                        ir::IrValueId in_val = entry_val;
                        if (lvl < c.all_scopes->size()) {
                            auto it_b = (*c.all_scopes)[lvl].find(name);
                            if (it_b != (*c.all_scopes)[lvl].end()) {
                                in_val = it_b->second;
                            }
                        }
                        ir::IrPhiArg arg{};
                        arg.value = in_val;
                        arg.block = c.pred;
                        phi.phi_args.push_back(arg);
                    }
                    fn_->blocks[merge_bb].instrs.insert(
                        fn_->blocks[merge_bb].instrs.begin(),
                        std::move(phi));
                    scopes_[lvl][name] = v_phi;
                }
            }
        } else if (contribs.size() == 1) {
            // Solo un arm reach merge: copiar sus all_scopes directos.
            const auto &as = *contribs[0].all_scopes;
            const size_t n_min = std::min(as.size(), scopes_.size());
            for (size_t lvl = 0; lvl < n_min; ++lvl) {
                for (const auto &kv: as[lvl]) {
                    scopes_[lvl][kv.first] = kv.second;
                }
            }
        }
        return ir::IR_NO_VALUE; // statement-like
    }

    ir::IrValueId Lowering::lower_spawn_expr(ast::SpawnExpr *e) {
        if (!e || !e->body) {
            error_at(e ? e->loc : SourceLoc{}, "lowering: spawn sin body");
            return ir::IR_NO_VALUE;
        }
        // 1. Generar la funcion hijo y obtener su nombre (label .vel).
        const std::string fn_name = generate_spawn_helper(e->body.get(), e->loc);

        // 2. Emit RAW_ASM en el bloque actual del padre:
        //    a) cargar la direccion absoluta de fn_name en {dst_pc}.
        //    b) ejecutar `spawn {dst_pc}` (Auto) o `spawnon {dst_pc}, {src1}`
        //       (Here / Pinned) segun la policy del SpawnExpr.
        //    c) capturar R0 al SSA value de la expresion via {dst}.
        const ir::IrValueId v_pc = emit_label_addr(fn_name, e->loc.line);

        // BugFix R3: si hay capturas, usar `spawnargs` en lugar de `spawn`.
        // Convencion: R1..R[N]=capturas, R15=N, spawnargs r_pc copia los
        // regs al child antes de make_ready.  Aplica para Auto policy.
        const auto &caps = spawn_captured_ssa_values_;
        if (e->policy == ast::SpawnExpr::Policy::Auto && !caps.empty()) {
            // raw_asm-elim wave 2: usar IrOp::SPAWN_ARGS nativo en lugar de
            // raw_asm.  El IR emitter ya genera el parallel-move correcto
            // del regalloc + spawnargs + restore.  operands[0]=r_pc,
            // [1..N]=args; dst=PID encoded en R0.
            const ir::IrValueId v_pid = fn_->new_value(ir::IrType::I64);
            std::vector<ir::IrValueId> ops;
            ops.push_back(v_pc);
            for (auto v : caps) ops.push_back(v);
            ir::IrInstr sa{};
            sa.op           = ir::IrOp::SPAWN_ARGS;
            sa.type         = ir::IrType::I64;
            sa.dst          = v_pid;
            sa.operands     = std::move(ops);
            sa.source_line  = e->loc.line;
            sa.set_is_call_site(true);
            fn_->append(current_block_, std::move(sa));
            return v_pid;
        }

        // si la policy es Auto, mantener el opcode SPAWN
        // (sin overhead).  Para Here y Pinned usar SPAWN_ON con el hint en
        // el segundo registro:
        //   - Here:   hint = -1 (signed) -> mismo scheduler que el padre.
        //   - Pinned: hint = expr        -> scheduler hint % num_schedulers.
        if (e->policy == ast::SpawnExpr::Policy::Auto) {
            // SPAWN IR op (0xF3): crea proceso hijo, PID encoded en R0/dst.
            const ir::IrValueId v_pid = fn_->new_value(ir::IrType::I64);
            ir::IrInstr         sp{};
            sp.op           = ir::IrOp::SPAWN;
            sp.type         = ir::IrType::I64;
            sp.dst          = v_pid;
            sp.operands     = {v_pc};
            sp.set_is_call_site(true);
            sp.source_line  = e->loc.line;
            fn_->append(current_block_, std::move(sp));
            return v_pid;
        }

        // Construir el SSA value del hint segun la policy.
        ir::IrValueId v_hint = ir::IR_NO_VALUE;
        if (e->policy == ast::SpawnExpr::Policy::Here) {
            // hint = -1 como i64 inmediato.
            v_hint = emit_const(ir::IrType::I64, static_cast<uint64_t>(-1LL),
                                e->loc.line);
        } else {
            // Pinned
            if (!e->sched_idx) {
                error_at(e->loc, "lowering: spawn on(...) sin expresion");
                return ir::IR_NO_VALUE;
            }
            v_hint = lower_expr(e->sched_idx.get());
            if (v_hint == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            // Promover a I64 si es necesario para que el runtime lea int64.
            const ir::IrType src_t = fn_->values[v_hint].type;
            v_hint                 = cast_if_needed(v_hint, src_t, ir::IrType::I64, e->loc.line);
        }

        // spawn_on IR op: combines fn_addr + hint -> PID encoded en R0.
        const ir::IrValueId v_pid = fn_->new_value(ir::IrType::I64);
        ir::IrInstr         sp{};
        sp.op           = ir::IrOp::SPAWN_ON;
        sp.type         = ir::IrType::I64;
        sp.dst          = v_pid;
        sp.operands     = {v_pc, v_hint};
        sp.set_is_call_site(true);
        sp.source_line  = e->loc.line;
        fn_->append(current_block_, std::move(sp));
        return v_pid;
    }

    // ---------------------------------------------------------------------
    // @Async sugar.  Transforma una FunctionDecl con flag
    // is_async en DOS funciones IR:
    //
    //   1. Wrapper publico `<name>` con firma `i64 <name>()` que el caller
    //      invoca.  Internamente:
    //        a. future_alloc -> fut handle
    //        b. spawn helper sintetica __async_<name>
    //        c. msgsend(child_pid, fut handle)
    //        d. return fut handle
    //
    //   2. Spawn helper `__async_<name>(void)` que ejecuta el body original
    //      como hijo cooperativo:
    //        a. msgrecv -> my_fut handle (set como async_fut_id_)
    //        b. lower body original (cada return X intercepta a fulfill+hlt)
    //        c. fallback al final: fulfill(my_fut, 0) + hlt
    //
    // El usuario escribe:
    //   @Async i64 compute() { return 42; }
    //   i32 main() { i64 r = await compute(); return r; }
    // ---------------------------------------------------------------------
    void Lowering::lower_async_function(ast::FunctionDecl *fd, ir::IrModule &out) {
        if (!fd || !fd->body) {
            error_at(fd ? fd->loc : SourceLoc{}, "@Async: funcion sin body");
            return;
        }
        const std::string helper_name = std::string("__async_") + fd->name;

        // Mejora II optimizada: numero de parametros del usuario.  Pasamos
        // los args al helper via @c spawnargs (R1..R[argc]) en lugar de
        // serializarlos a un buffer y enviarlos via msgsend.  El handle
        // del Future tambien viaja por R1 (slot 0 en la nueva calling
        // convention del helper: arg[0] = future handle, arg[1..N] = args).
        // Asi:
        //   - Wrapper: emit args en R2..R[N+1] + R1=fut + R15=N+1 + spawnargs.
        //     ~3 instr en lugar de ~12 (alloca + N+1 stores + msgsend).
        //   - Helper:  los params estan ya en R1..R[N+1], NO necesita
        //     ALLOCA + msgrecv + N+1 LOADs + casts.
        // Total: ~26 instr -> ~3 instr por @Async call (~9x mas rapido).
        const size_t n_params = fd->params.size();
        if (n_params + 1 > 12) {
            // Calling convention de spawnargs: R1..R[argc] con argc <= 12.
            // Reservamos R1 para el handle del Future, asi quedan 11 slots
            // para args del usuario.
            error_at(fd->loc,
                "@Async: numero de parametros excede el maximo (11)");
            return;
        }

        // ---------------------------------------------------------------
        // 1. Construir el SPAWN HELPER (lo encolamos en pending_spawn_helpers_
        //    para que se vuelque al final, despues de main).
        // ---------------------------------------------------------------
        ir::IrFunction *                                             saved_fn         = fn_;
        ir::IrBlockId                                                saved_block      = current_block_;
        bool                                                         saved_terminated = block_terminated_;
        std::vector<std::unordered_map<std::string, ir::IrValueId> > saved_scopes
                = std::move(scopes_);
        std::unordered_set<std::string> saved_addr_taken
                = std::move(address_taken_locals_);
        std::vector<CleanupAction> saved_cleanups
                = std::move(cleanup_stack_);
        ir::IrValueId saved_async_fut = async_fut_id_;

        ir::IrFunction helper_fn;
        helper_fn.name            = helper_name;
        helper_fn.ret_type        = ir::IrType::VOID;
        const ir::IrBlockId entry = helper_fn.new_block("entry");

        fn_               = &helper_fn;
        current_block_    = entry;
        block_terminated_ = false;
        scopes_.clear();
        push_scope();
        address_taken_locals_.clear();
        host_bearing_locals_.clear();
        cleanup_stack_.clear();

        // Mejora II optimizada: el helper recibe args directos en R1..R[N+1]
        // gracias a @c spawnargs (sin msgrecv + buffer).  Calling convention:
        //   R1         = handle del Future
        //   R2..R[N+1] = parametros del usuario en el orden declarado
        // Declarar formalmente los params como un IrFunction normal: el
        // primer param es el handle (siempre I64), el resto son los del
        // usuario con su tipo declarado.
        ir::IrValueId v_my_fut;
        {
            // Primer parametro IR: future handle (I64).
            v_my_fut = fn_->new_value(ir::IrType::I64, "__async_fut");
            fn_->values[v_my_fut].is_param = true;
            fn_->params.push_back(v_my_fut);
        }
        for (size_t pi = 0; pi < n_params; ++pi) {
            auto &p = fd->params[pi];
            // Determinar el IrType del param segun el tipo declarado.
            ir::IrType pt_ir = ir::IrType::I64;
            if (p->type) {
                if (auto *prim = dynamic_cast<ast::NamedTypeNode *>(p->type.get())) {
                    const std::string &nm = prim->name;
                    if      (nm == "i8")    pt_ir = ir::IrType::I8;
                    else if (nm == "i16")   pt_ir = ir::IrType::I16;
                    else if (nm == "i32" || nm == "int32_t")  pt_ir = ir::IrType::I32;
                    else if (nm == "i64" || nm == "int64_t")  pt_ir = ir::IrType::I64;
                    else if (nm == "u8")    pt_ir = ir::IrType::U8;
                    else if (nm == "u16")   pt_ir = ir::IrType::U16;
                    else if (nm == "u32" || nm == "uint32_t") pt_ir = ir::IrType::U32;
                    else if (nm == "u64" || nm == "uint64_t") pt_ir = ir::IrType::U64;
                    else if (nm == "f32" || nm == "float")    pt_ir = ir::IrType::F32;
                    else if (nm == "f64" || nm == "double")   pt_ir = ir::IrType::F64;
                    else if (nm == "bool")  pt_ir = ir::IrType::BOOL;
                    else if (nm == "char")  pt_ir = ir::IrType::I8;
                }
            }
            ir::IrValueId pv = fn_->new_value(pt_ir, p->name);
            fn_->values[pv].is_param = true;
            fn_->params.push_back(pv);
            bind(p->name, pv);
        }

        // 1b. Activar interception de return en lower_return.
        async_fut_id_ = v_my_fut;

        // 1c. Bajar el body original.  Cada `return X` -> fulfill + hlt.
        lower_block(fd->body.get());

        // 1d. Fallback: si el body cae naturalmente sin return, emitir
        //     fulfillhlt(my_fut, 0) para terminar el child con valor 0.
        //     Optimizado: 1 instr en lugar de fulfill+hlt separados.
        if (!block_terminated_) {
            const ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0, fd->loc.line);
            emit_fulfill_hlt(v_my_fut, v_zero, fd->loc.line);
            block_terminated_ = true;
        }

        pop_scope();
        // Encolamos el helper para volcarse al final de run() (despues de
        // main para preservar el orden de entry point).
        pending_spawn_helpers_.push_back(std::move(helper_fn));

        // Restaurar contexto del lowering.
        fn_                   = saved_fn;
        current_block_        = saved_block;
        block_terminated_     = saved_terminated;
        scopes_               = std::move(saved_scopes);
        address_taken_locals_ = std::move(saved_addr_taken);
        cleanup_stack_        = std::move(saved_cleanups);
        async_fut_id_         = saved_async_fut;

        // ---------------------------------------------------------------
        // 2. Construir el WRAPPER publico con el nombre de la funcion.
        //    El wrapper recibe los args del usuario via la calling convention
        //    normal CALLVM (R1..R12), aloca un Future, spawnea el helper,
        //    serializa (handle, args) en un buffer y los envia al helper
        //    via msgsend.  Devuelve el handle del Future al caller.
        // ---------------------------------------------------------------
        ir::IrFunction wrapper_fn;
        wrapper_fn.name             = fd->name;
        wrapper_fn.ret_type         = ir::IrType::I64; // bytecode level: handle
        const ir::IrBlockId w_entry = wrapper_fn.new_block("entry");

        fn_               = &wrapper_fn;
        current_block_    = w_entry;
        block_terminated_ = false;
        scopes_.clear();
        push_scope();
        address_taken_locals_.clear();
        host_bearing_locals_.clear();
        cleanup_stack_.clear();
        async_fut_id_ = ir::IR_NO_VALUE; // wrapper NO es async body

        // Mejora II: declarar los parametros del wrapper igual que en una
        // funcion normal.  Cada param se mapea a un IrType y se vincula
        // con su nombre para que su SSA value se pueda leer mas abajo
        // cuando serializamos los args al buffer.
        std::vector<ir::IrValueId> param_vals;
        param_vals.reserve(n_params);
        for (size_t pi = 0; pi < n_params; ++pi) {
            auto &p = fd->params[pi];
            ir::IrType pt_ir = ir::IrType::I64;
            if (p->type) {
                if (auto *prim = dynamic_cast<ast::NamedTypeNode *>(p->type.get())) {
                    const std::string &nm = prim->name;
                    if      (nm == "i8")    pt_ir = ir::IrType::I8;
                    else if (nm == "i16")   pt_ir = ir::IrType::I16;
                    else if (nm == "i32" || nm == "int32_t")  pt_ir = ir::IrType::I32;
                    else if (nm == "i64" || nm == "int64_t")  pt_ir = ir::IrType::I64;
                    else if (nm == "u8")    pt_ir = ir::IrType::U8;
                    else if (nm == "u16")   pt_ir = ir::IrType::U16;
                    else if (nm == "u32" || nm == "uint32_t") pt_ir = ir::IrType::U32;
                    else if (nm == "u64" || nm == "uint64_t") pt_ir = ir::IrType::U64;
                    else if (nm == "f32" || nm == "float")    pt_ir = ir::IrType::F32;
                    else if (nm == "f64" || nm == "double")   pt_ir = ir::IrType::F64;
                    else if (nm == "bool")  pt_ir = ir::IrType::BOOL;
                    else if (nm == "char")  pt_ir = ir::IrType::I8;
                }
            }
            const ir::IrValueId pv = fn_->new_value(pt_ir, p->name);
            fn_->values[pv].is_param = true;
            fn_->params.push_back(pv);
            bind(p->name, pv);
            param_vals.push_back(pv);
        }

        // 2a. fut = future_alloc() via IR op FUTURE.
        const ir::IrValueId v_fut = fn_->new_value(ir::IrType::I64); {
            ir::IrInstr fu{};
            fu.op           = ir::IrOp::FUTURE;
            fu.type         = ir::IrType::I64;
            fu.dst          = v_fut;
            fu.set_is_call_site(true);
            fu.source_line  = fd->loc.line;
            fn_->append(current_block_, std::move(fu));
        }

        // Mejora II optimizada: el wrapper usa IrOp::SPAWN_ARGS que aprovecha
        // el parallel-move del regalloc para colocar args correctamente en
        // R1..R[N+1] sin conflictos.  Calling convention:
        //   R1            = handle del Future
        //   R2..R[N+1]    = parametros del usuario coerced a i64
        //   R15           = N+1 (argc total, lo setea el emisor IR)
        //   spawnargs r_pc -> child PID encoded en R0 (devuelto al caller)
        //
        // Esto reemplaza la version previa con buffer + msgsend (~12 instr)
        // por ~2-3 instr fijas + N moves resueltos por parallel-move +
        // 1 spawnargs.  Elimina la contencion del lock del mailbox y la
        // copia de buffer.

        // 2b.1: Coerce cada param del usuario a i64 preservando bits.
        std::vector<ir::IrValueId> qword_args;
        qword_args.reserve(n_params);
        for (size_t pi = 0; pi < n_params; ++pi) {
            const ir::IrValueId v_param = param_vals[pi];
            const ir::IrType    pt_ir   = fn_->values[v_param].type;
            ir::IrValueId       v_qword = v_param;
            if (pt_ir == ir::IrType::F64) {
                v_qword = fn_->new_value(ir::IrType::I64);
                ir::IrInstr bc{};
                bc.op = ir::IrOp::BITCAST;
                bc.type = ir::IrType::I64;
                bc.dst = v_qword;
                bc.operands = {v_param};
                bc.source_line = fd->loc.line;
                fn_->append(current_block_, std::move(bc));
            } else if (pt_ir == ir::IrType::F32) {
                ir::IrValueId v_i32 = fn_->new_value(ir::IrType::I32);
                ir::IrInstr bc{};
                bc.op = ir::IrOp::BITCAST;
                bc.type = ir::IrType::I32;
                bc.dst = v_i32;
                bc.operands = {v_param};
                bc.source_line = fd->loc.line;
                fn_->append(current_block_, std::move(bc));
                v_qword = cast_if_needed(v_i32, ir::IrType::I32, ir::IrType::I64,
                                          fd->loc.line);
            } else if (pt_ir != ir::IrType::I64 && pt_ir != ir::IrType::U64
                    && pt_ir != ir::IrType::PTR) {
                v_qword = cast_if_needed(v_param, pt_ir, ir::IrType::I64,
                                          fd->loc.line);
            }
            qword_args.push_back(v_qword);
        }

        // 2b.2: Cargar la direccion del helper en un SSA value PTR.
        const ir::IrValueId v_pc = emit_label_addr(helper_name, fd->loc.line);

        // SPAWN_ARGS dedicado.  El emisor IR usa parallel-move para
        // resolver conflictos al colocar args en sus regs destino.
        // Operands: [r_pc, fut, arg1, arg2, ..., argN]
        const ir::IrValueId v_child = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ins{};
            ins.op          = ir::IrOp::SPAWN_ARGS;
            ins.type        = ir::IrType::I64;
            ins.dst         = v_child;
            ins.operands.reserve(2 + n_params);
            ins.operands.push_back(v_pc);   // r_pc
            ins.operands.push_back(v_fut);  // R1 = fut
            for (auto v : qword_args)        // R2..R[N+1] = args
                ins.operands.push_back(v);
            ins.source_line = fd->loc.line;
            fn_->append(current_block_, std::move(ins));
        }
        (void)v_child; // no usado mas; el child ya esta ejecutando

        // 2c. return fut.
        {
            ir::IrInstr ret{};
            ret.op          = ir::IrOp::RET;
            ret.type        = ir::IrType::I64;
            ret.operands    = {v_fut};
            ret.source_line = fd->loc.line;
            fn_->append(current_block_, std::move(ret));
            block_terminated_ = true;
        }

        pop_scope();
        propagate_is_gc_object_through_phis(wrapper_fn);
        out.add_function(std::move(wrapper_fn));

        // Restaurar el contexto (aunque ya estamos al final de la funcion).
        fn_                   = saved_fn;
        current_block_        = saved_block;
        block_terminated_     = saved_terminated;
        scopes_               = std::move(saved_scopes);
        address_taken_locals_ = std::move(saved_addr_taken);
        cleanup_stack_        = std::move(saved_cleanups);
        async_fut_id_         = saved_async_fut;
    }

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
    // Expresiones.
    // ---------------------------------------------------------------------

    ir::IrValueId Lowering::lower_string_lit(ast::StringLitExpr *e) {
        if (!out_mod_) {
            error_at(e->loc, "lowering: out_mod_ nulo al bajar StringLitExpr");
            return ir::IR_NO_VALUE;
        }
        // Convertir el contenido resuelto a vector<uint8_t> y registrarlo
        // (deduplicado) en static_data.  Los duplicados retornan el mismo
        // indice, ahorrando bytes en el .vel emitido.
        std::vector<uint8_t> bytes(e->value.begin(), e->value.end());
        const uint64_t       idx = out_mod_->intern_static_data(std::move(bytes));

        // Emitir IrOp::STR_LIT_ADDR -> el emisor genera "mov rDst, @Absolute(\"code.s_<idx>\")".
        const ir::IrValueId dst = fn_->new_value(ir::IrType::PTR);
        ir::IrInstr         ins{};
        ins.op          = ir::IrOp::STR_LIT_ADDR;
        ins.type        = ir::IrType::PTR;
        ins.dst         = dst;
        ins.imm         = idx;
        ins.source_line = e->loc.line;
        fn_->append(current_block_, std::move(ins));
        return dst;
    }

    // ---------------------------------------------------------------------
    // POO: clases en Vex. la integracion completa
    // (registro en module init, NEWOBJ + CALLVIRT, GETFIELD/SETFIELD)
    // se implementa por fases.  Cada metodo nuevo emite un error claro
    // hasta que su implementacion concreta este lista.
    // ---------------------------------------------------------------------

    // Forward decls de helpers definidos en lowering_builtin.cpp
    uint64_t intern_class_name(ir::IrModule &mod, const std::string &name);
    uint64_t intern_class_cache_slot(ir::IrModule &mod, const std::string &name);

    void Lowering::lower_class_methods(ast::ClassDecl *cd, ir::IrModule &out) {
        // Para cada metodo / constructor de la clase, generamos una
        // IrFunction con nombre <Class>__<method> y un primer parametro
        // implicito 'this' de tipo PTR.  Reusamos la maquinaria del
        // lowering normal: preparamos param_bindings, scope, address-taken
        // pre-pase y bajamos el body con lower_block.
        //
        // Las interfaces se omiten: sus metodos son abstractos (sin body)
        // y solo aportan la metadata de la firma para validacion.
        if (cd->is_interface) return;
        // Templates genericos (con type_params) se omiten: sus
        // monomorphizaciones concretas (que SI aparecen en mod_.decls)
        // se procesan normalmente.
        if (!cd->type_params.empty()) return;
        for (auto &m_uptr: cd->methods) {
            auto *m = m_uptr.get();
            if (!m || !m->body) continue;

            ir::IrFunction fn;
            // Mangling: ClassName__methodName; constructor usa "ctor".
            std::string suffix = m->is_constructor ? std::string("ctor") : m->name;
            fn.name            = cd->name + "__" + suffix;

            // Tipo de retorno + detect SRET (Result/Optional).  Para class
            // methods retornando Result/Optional cross-module, el callee
            // necesita retbuf hidden como SEGUNDO param (this=r1, retbuf=r2,
            // args=r3..).  Sin esto el callee alocaba retbuf en su propio
            // stack y devolvia el ptr via R0 -> use-after-free post-leave.
            Type sem_ret_m = Type{PrimitiveKind::VOID};
            if (m->return_type) sem_ret_m = tc_.resolve_type_node(m->return_type.get());
            const bool method_sret = !m->is_constructor
                && (sem_ret_m.kind == PrimitiveKind::OPTIONAL
                 || sem_ret_m.kind == PrimitiveKind::RESULT);
            if (m->is_constructor) {
                fn.ret_type = ir::IrType::VOID;
            } else if (method_sret) {
                fn.ret_type = ir::IrType::VOID;
            } else if (m->return_type
                && m->return_type->kind == ast::NodeKind::PrimitiveTypeNode) {
                auto *pt    = static_cast<ast::PrimitiveTypeNode *>(m->return_type.get());
                fn.ret_type = ir_type_from_primitive(pt->prim);
            } else if (m->return_type) {
                fn.ret_type    = (sem_ret_m.kind != PrimitiveKind::COUNT
                                     && sem_ret_m.kind != PrimitiveKind::VOID)
                                     ? ir_type_from_primitive(sem_ret_m.kind)
                                     : ir::IrType::VOID;
            } else {
                fn.ret_type = ir::IrType::VOID;
            }

            // Param 0: 'this' como PTR.  Sin contar en m->params.
            // Bug fix 2026-05-23: metodos estaticos NO tienen 'this' implicito.
            std::vector<std::pair<std::string, ir::IrValueId> > bindings;
            ir::IrValueId this_vid = ir::IR_NO_VALUE;
            if (!m->is_static) {
                this_vid = fn.new_value(ir::IrType::PTR, "%this");
                fn.values[this_vid].is_param                                 = true;
                // fix - this es siempre un host_ptr a un objeto GC; debe
                // ser refrescado tras cualquier CALL que pueda disparar GC.
                fn.values[this_vid].is_host_ptr  = true;
                fn.values[this_vid].is_gc_object = true;
                fn.params.push_back(this_vid);
                bindings.emplace_back("this", this_vid);
            }

            // SRET retbuf: param hidden tras `this` para methods que
            // retornan Result/Optional.  CALLVIRT debe marshalear retbuf
            // a r2 y args a r3..r(N+2).
            ir::IrValueId v_method_retbuf = ir::IR_NO_VALUE;
            if (method_sret) {
                v_method_retbuf = fn.new_value(ir::IrType::PTR, "%__retbuf");
                fn.values[v_method_retbuf].is_param = true;
                // BugFix sret-cross-mem (2026-06-04): metodos de clase con
                // SRET tambien reciben retbuf como host_ptr (caller hace
                // ALLOCA en host memory).  Sin esta marca el callee escribe
                // al retbuf con `mov` (VM mem) en lugar de `movh` (host)
                // -> el Result llega siempre en ceros al caller.  Caso
                // observado: file_io.FileReader.read_all retornaba con
                // tag=0, error=0 aunque el archivo se hubiera leido OK.
                fn.values[v_method_retbuf].is_host_ptr = true;
                fn.params.push_back(v_method_retbuf);
            }

            // Resto de parametros declarados.
            for (auto &p: m->params) {
                ir::IrType pt                = ir::IrType::I64;
                bool       param_is_class    = false;
                bool       param_is_host_ptr = false;
                if (p->type
                    && p->type->kind == ast::NodeKind::PrimitiveTypeNode) {
                    auto *ptn = static_cast<ast::PrimitiveTypeNode *>(p->type.get());
                    pt        = ir_type_from_primitive(ptn->prim);
                } else if (p->type) {
                    const Type sem = tc_.resolve_type_node(p->type.get());
                    if (sem.kind != PrimitiveKind::COUNT
                        && sem.kind != PrimitiveKind::VOID) {
                        pt = ir_type_from_primitive(sem.kind);
                    }
                    if (sem.kind == PrimitiveKind::CLASS) param_is_class = true;
                    // PTR/ARRAY consultan is_virtual (mismo criterio que
                    // en lower_function): T* host -> host_ptr=true,
                    // VirtualPtr<T> -> host_ptr=false.
                    if ((sem.kind == PrimitiveKind::PTR
                            || sem.kind == PrimitiveKind::ARRAY)
                        && !sem.is_virtual) {
                        param_is_host_ptr = true;
                    }
                }
                const ir::IrValueId vid = fn.new_value(pt, "%" + p->name);
                fn.values[vid].is_param = true;
                if (param_is_class) {
                    // Param de tipo CLASS es host_ptr a un objeto GC.
                    // Marcamos @c is_gc_object para que el regalloc, al
                    // salvar este reg alrededor de un CALL que pueda
                    // disparar GC, lo haga via @c gchandle (handle estable)
                    // y restaure con @c gcderef (host_ptr fresco post-GC).
                    fn.values[vid].is_host_ptr  = true;
                    fn.values[vid].is_gc_object = true;
                } else if (param_is_host_ptr) {
                    fn.values[vid].is_host_ptr = true;
                }
                fn.params.push_back(vid);
                bindings.emplace_back(p->name, vid);
            }

            // Configurar contexto del lowering para esta funcion.
            const ir::IrBlockId entry = fn.new_block("entry");
            fn_                       = &fn;
            current_block_            = entry;
            block_terminated_         = false;
            scopes_.clear();
            push_scope();
            for (auto &kv: bindings) bind(kv.first, kv.second);

            // Pre-pase de address-taken para variables locales del cuerpo.
            address_taken_locals_.clear();
            host_bearing_locals_.clear();
            // fix.cleanup-leak - limpiar el stack de cleanups entre
            // metodos de clase.  Sin esto, si un metodo anterior (e.g. el
            // ctor o un metodo previo) dejo cleanups colgados, el siguiente
            // metodo los hereda y al hacer `return` los ejecuta sobre
            // valores SSA que no le pertenecen, generando CALLVIRT a la
            // dtor con `this` apuntando a un i32 arbitrario -> crash.
            cleanup_stack_.clear();
            escaping_locals_.clear();
            try_spill_slots_.clear();
            scan_address_taken(m->body.get());
            // fix5 - escape detection tambien para metodos de clase.
            // Sin esto, vars locales que escapan via `this.field = local`
            // (ej. `this.head = n` en LinkedList.prepend) NO se marcaban
            // como escaping y mi cleanup RAW_ASM las dropeaba al exit del
            // metodo, dejando `this.field` con un handle invalido.
            scan_escaping_locals(m->body.get());
            // fix9 - eliminado el pre-pase scan_loops del metodo.
            // Las flags solo se usaban para decidir si activar cleanup
            // RAW_ASM (eliminado tras fix8 stack scanning).  Reset
            // a false explicito por consistencia con lower_function.
            current_fn_has_loops_ = false;
            current_fn_has_try_   = false;

            // Marcar que estamos dentro del lowering de un metodo de clase
            // (el resto del lowering puede consultar current_class_lowering_
            // para saber a que ClassLayout pertenece 'this').
            const std::string saved_class = current_class_lowering_;
            current_class_lowering_       = cd->name;

            // Bug fix 2026-05-23 (Audit 14): metodos con return type STRING
            // necesitan auto-promotion de string literals a StringObject en
            // `return "lit"`.  Sin propagar `current_fn_returns_string_` aqui,
            // `lower_return` solo veia el flag para funciones top-level y
            // emitia `mov r0, @Absolute(s_N)` (ptr crudo) en metodos -> el
            // caller recibia un ptr raw como GcHandle -> str_equals/strraw
            // sobre garbage.  Reset al salir del metodo.
            const bool saved_returns_str = current_fn_returns_string_;
            {
                bool is_string_ret = false;
                if (m->return_type
                    && m->return_type->kind == ast::NodeKind::PrimitiveTypeNode) {
                    auto *ptn = static_cast<ast::PrimitiveTypeNode *>(m->return_type.get());
                    is_string_ret = (ptn->prim == PrimitiveKind::STRING);
                } else if (m->return_type) {
                    const Type sem = tc_.resolve_type_node(m->return_type.get());
                    is_string_ret = (sem.kind == PrimitiveKind::STRING);
                }
                current_fn_returns_string_ = is_string_ret;
            }

            // Instrumentacion: vex_trace:enter al inicio del metodo
            // (igual filtro que en lower_function -- saltamos solo helpers
            // sinteticos; los ctors/dtors/metodos normales se instrumentan).
            if (instrument_mode_ != "none" && instrument_mode_ != ""
                && fn.name != "__module_init"
                && fn.name.compare(0, 6, "__new_") != 0
                && fn.name.compare(0, 8, "__async_") != 0
                && fn.name.compare(0, 9, "__lambda_") != 0
                && fn.name.compare(0, 8, "__spawn_") != 0) {
                emit_instrument_enter(fn.name, m->loc.line);
            }

            // SRET context para metodos retornando Result/Optional.
            // `lower_return` consulta @c sret_active_ para copiar el slot al
            // retbuf en vez de devolver ptr via R0.
            const bool      saved_sret_active   = sret_active_;
            ir::IrValueId   saved_sret_retbuf   = sret_retbuf_;
            uint64_t        saved_sret_buf_size = sret_buf_size_;
            if (method_sret) {
                sret_active_   = true;
                sret_retbuf_   = v_method_retbuf;
                sret_buf_size_ = (sem_ret_m.kind == PrimitiveKind::OPTIONAL)
                                     ? 16ULL : 24ULL;
            }

            lower_block(m->body.get());

            if (method_sret) {
                sret_active_   = saved_sret_active;
                sret_retbuf_   = saved_sret_retbuf;
                sret_buf_size_ = saved_sret_buf_size;
            }

            current_class_lowering_     = saved_class;
            current_fn_returns_string_  = saved_returns_str;

            // augmentacion automatica del destructor: si este metodo
            // es @c is_destructor, antes del cierre invocamos los dtors de
            // todos los fields destructibles (CLASS con has_destructor o
            // has_destructible_field).  Esto implementa RAII recursivo: el
            // dtor del contenedor libera la cadena ownerships sin que el
            // usuario tenga que escribir el codigo manualmente.
            //
            // Orden: campos en orden de declaracion (no inverso) por
            // simplicidad.  Para clases con ciclos (LinkedList -> Node ->
            // Node ...), el primer @c null encontrado corta la cadena
            // gracias al if (field != null) check.
            //
            // El check de null se hace via cmp_eq + br_cond.  Sin esto,
            // CALLVIRT a un puntero null crashea con NPE.
            if (m->is_destructor && !block_terminated_) {
                auto it_lay = tc_.class_layouts().find(cd->name);
                if (it_lay != tc_.class_layouts().end()) {
                    const ClassLayout &lay = it_lay->second;
                    for (const auto &f: lay.fields) {
                        if (f.type.kind != PrimitiveKind::CLASS) continue;
                        auto it_inner = tc_.class_layouts().find(f.type.struct_name);
                        if (it_inner == tc_.class_layouts().end()) continue;
                        const ClassLayout &inner = it_inner->second;
                        if (!inner.has_destructor) continue;
                        // Localizar vtable_index del dtor del inner.
                        uint32_t inner_dtor_idx = UINT32_MAX;
                        for (const auto &im: inner.methods) {
                            if (im.is_destructor) {
                                inner_dtor_idx = im.vtable_index;
                                break;
                            }
                        }
                        if (inner_dtor_idx == UINT32_MAX) continue;

                        // 1) addr = this + offset
                        const ir::IrValueId addr = emit_field_addr(
                            fn_, current_block_, this_vid, f.offset, m->loc.line);
                        // 2) handle = LOAD i64 addr (handle al inner obj
                        //    almacenado por @c lower_class_field_store).
                        const ir::IrValueId v_handle = fn_->new_value(ir::IrType::I64);
                        ir::IrInstr         ld{};
                        ld.op          = ir::IrOp::LOAD;
                        ld.type        = ir::IrType::I64;
                        ld.dst         = v_handle;
                        ld.operands    = {addr};
                        ld.source_line = m->loc.line;
                        fn_->append(current_block_, std::move(ld));
                        // raw_asm-elim 2026-05-28: 2b) host_ptr fresco via IrOp::GC_DEREF_HOST.
                        const ir::IrValueId field_val       = fn_->new_value(ir::IrType::I64);
                        fn_->values[field_val].is_host_ptr  = true;
                        fn_->values[field_val].is_gc_object = true;
                        ir::IrInstr deref{};
                        deref.op          = ir::IrOp::GC_DEREF_HOST;
                        deref.type        = ir::IrType::PTR;
                        deref.dst         = field_val;
                        deref.operands    = {v_handle};
                        deref.source_line = m->loc.line;
                        fn_->append(current_block_, std::move(deref));
                        // 3) is_null = (field_val == 0)
                        const ir::IrValueId zero    = emit_const(ir::IrType::I64, 0, m->loc.line);
                        const ir::IrValueId is_null = fn_->new_value(ir::IrType::BOOL);
                        ir::IrInstr         cmp{};
                        cmp.op          = ir::IrOp::CMP_EQ;
                        cmp.type        = ir::IrType::BOOL;
                        cmp.dst         = is_null;
                        cmp.operands    = {field_val, zero};
                        cmp.source_line = m->loc.line;
                        fn_->append(current_block_, std::move(cmp));
                        // 4) br_cond is_null skip do_dtor
                        const ir::IrBlockId do_bb   = fn_->new_block("dtor_field");
                        const ir::IrBlockId skip_bb = fn_->new_block("dtor_skip");
                        ir::IrInstr         br{};
                        br.op           = ir::IrOp::BR_COND;
                        br.operands     = {is_null};
                        br.target_block = skip_bb; // true (null) -> skip
                        br.false_block  = do_bb;   // false -> do_dtor
                        br.source_line  = m->loc.line;
                        fn_->append(current_block_, std::move(br));
                        // 5) do_bb: callvirt field_val, inner_dtor_idx; br skip
                        current_block_ = do_bb;
                        ir::IrInstr cv{};
                        cv.op          = ir::IrOp::CALLVIRT;
                        cv.type        = ir::IrType::VOID;
                        cv.dst         = ir::IR_NO_VALUE;
                        cv.operands    = {field_val};
                        cv.imm         = static_cast<uint64_t>(inner_dtor_idx);
                        cv.source_line = m->loc.line;
                        fn_->append(current_block_, std::move(cv));
                        ir::IrInstr brj{};
                        brj.op           = ir::IrOp::BR;
                        brj.target_block = skip_bb;
                        brj.source_line  = m->loc.line;
                        fn_->append(current_block_, std::move(brj));
                        // 6) merge en skip_bb -> continuar con el siguiente field.
                        current_block_    = skip_bb;
                        block_terminated_ = false;
                    }
                }
            }

            // Cierre: anadir RET por defecto si el body no termino con uno.
            if (!block_terminated_) {
                // Instrumentacion: vex_trace:leave antes del RET implicito.
                if (instrument_mode_ != "none" && instrument_mode_ != ""
                    && fn.name != "__module_init"
                    && fn.name.compare(0, 6, "__new_") != 0
                    && fn.name.compare(0, 8, "__async_") != 0
                    && fn.name.compare(0, 9, "__lambda_") != 0
                    && fn.name.compare(0, 8, "__spawn_") != 0) {
                    emit_instrument_exit(fn.name, ir::IR_NO_VALUE, m->loc.line);
                }
                ir::IrInstr ret{};
                ret.op   = ir::IrOp::RET;
                ret.type = fn.ret_type;
                if (fn.ret_type != ir::IrType::VOID) {
                    const ir::IrValueId zero = emit_const(fn.ret_type, 0, m->loc.line);
                    ret.operands.push_back(zero);
                }
                ret.source_line = m->loc.line;
                fn.append(current_block_, std::move(ret));
                block_terminated_ = true;
            }

            pop_scope();
            propagate_is_gc_object_through_phis(fn);

            // B.3 contract: si la clase es una instanciacion generica
            // (e.g., `Box_i32` viene de `class Box<T>`), marcar la
            // IrFunction con el template + type args legibles.  Util
            // para C2 / AOT (dedup de specializations) y para tools
            // (mostrar "Box<i32>::get" en stack traces vs "Box_i32__get").
            if (const auto *mi = tc_.monomorph_info(cd->name)) {
                fn.generic_template_name = mi->template_name;
                fn.generic_type_args     = mi->type_args;
            }

            out.add_function(std::move(fn));
            fn_ = nullptr;
        }
    }

    // -----------------------------------------------------------------
    // Helpers de generacion de codigo .vel para POO dinamica.
    // -----------------------------------------------------------------

    /**
     * @brief Registra el nombre como bytes UTF-8 en static_data y devuelve
     *        el indice para construir @c @Absolute("code.s_<idx>").
    }


    /**
     * @brief Emite el ASM que construye una FindClassParams (16 bytes) en
     *        stack y deja en @c r12 el ClassInfo* localizado.
     */
    void Lowering::generate_new_helpers(ir::IrModule &out) {
        // Para cada clase declarada en el modulo, generamos una funcion
        // __new_<Class>(arg1, ..., argN) -> handle (GcHandle).  El cuerpo
        // es RAW_ASM que hace findclass + newobj + callvirt al ctor.
        for (auto &decl: mod_.decls) {
            if (!decl || decl->kind != ast::NodeKind::ClassDecl) continue;
            auto *cd = static_cast<ast::ClassDecl *>(decl.get());
            // No se genera helper para interfaces: no son instanciables.
            if (cd->is_interface) continue;
            // Templates genericos (no monomorphizados): no instanciables.
            if (!cd->type_params.empty()) continue;
            auto it = tc_.class_layouts().find(cd->name);
            if (it == tc_.class_layouts().end()) continue;
            const ClassLayout &lay = it->second;

            // Localizar el constructor PROPIO (no heredado del super).
            // BugFix R1: si la clase deriva de un Base con ctor, los
            // methods incluyen Base.__ctor copiado al inicio (inherited).
            // Sin priorizar el ctor cuyo defining_class == cd->name, el
            // helper __new_<Derived> usaria los params del Base.__ctor
            // -> faltarian args en la llamada -> los campos propios del
            // Derived quedaban a 0.
            const ClassMethodInfo *ctor = nullptr;
            for (const auto &m: lay.methods) {
                if (m.is_constructor && m.defining_class == cd->name) {
                    ctor = &m;
                    break;
                }
            }
            // Fallback: si no hay ctor propio, usar el primero inherited.
            if (!ctor) {
                for (const auto &m: lay.methods) {
                    if (m.is_constructor) { ctor = &m; break; }
                }
            }
            // fix12 - si el ctor es zero-init trivial (solo asigna
            // campos a 0/null/false), saltarlo: el `gc_heap.alloc` ya
            // memset el payload a 0.  Solo aplica si el ctor existe Y
            // no tiene argumentos (ctor con args debe correr para que
            // los argumentos lleguen a campos).
            const bool ctor_is_trivial_zero_init =
                    ctor != nullptr
                    && ctor->is_zero_init_ctor
                    && ctor->param_types.empty();
            // Si el ctor es trivial zero-init, lo tratamos como si no existiera
            // (el helper omitira el callvirt y devolvera el objeto recien
            // alocado).  Esto ahorra ~9 instrucciones VM por `new` para POCs.
            const ClassMethodInfo *effective_ctor = ctor_is_trivial_zero_init
                                                        ? nullptr
                                                        : ctor;
            const size_t   nargs           = effective_ctor ? effective_ctor->param_types.size() : 0;
            const uint32_t ctor_vtable_idx = effective_ctor ? effective_ctor->vtable_index : 0;

            // Registrar el nombre de la clase como datos estaticos.
            const uint64_t name_idx = intern_class_name(out, cd->name);
            const uint32_t name_len = static_cast<uint32_t>(cd->name.size());
            // fix11 - reservar slot de cache para ClassInfo*.
            const uint64_t cache_idx = intern_class_cache_slot(out, cd->name);
            (void) name_idx;
            (void) name_len; // ya no se usa findclass aqui

            // Phase Z.6: emitir TANTO el helper local (__new_<Class>) como,
            // si la clase se uso con `shared` en algun var-decl, su variante
            // shared (__new_<Class>_shared).  El cuerpo es identico salvo
            // que la instruccion `newobj r1` se reemplaza por `newobjs r1`
            // (aloca en SharedHeap).  Generar ambos en el mismo bucle ahorra
            // duplicar toda la logica de calculo de ctor / cache slot.
            const bool need_shared_variant =
                (classes_used_shared_.count(cd->name) > 0);
            const int n_variants = need_shared_variant ? 2 : 1;

            for (int variant = 0; variant < n_variants; ++variant) {
            const bool is_shared_variant = (variant == 1);
            // Mnemonico para la instruccion alloc.  newobjs (Z.6) usa
            // exactamente el mismo encoding REG/FIXED_4 que newobj.
            const char *newobj_op = is_shared_variant ? "newobjs" : "newobj";

            // Construir IrFunction __new_<Class>[_shared].
            ir::IrFunction fn;
            fn.name     = is_shared_variant
                            ? ("__new_" + cd->name + "_shared")
                            : ("__new_" + cd->name);
            fn.ret_type = ir::IrType::PTR;

            // Params: replicar tipos del ctor (si existe).
            for (size_t i = 0; i < nargs; ++i) {
                const ir::IrType    pt  = ir_type_from_primitive(ctor->param_types[i].kind);
                const ir::IrValueId vid = fn.new_value(pt, "%a" + std::to_string(i));
                fn.values[vid].is_param = true;
                fn.params.push_back(vid);
            }

            const ir::IrBlockId entry = fn.new_block("entry");

            // raw_asm-elim Fase 1 (__new_X a IR): la variante LOCAL del helper
            // se construye con IR ops estructurados en vez de RAW_ASM textual,
            // para que el path vreg y el optimizer lo compilen sin el
            // mini-parser de raw_asm.  Patron equivalente al RAW_ASM del
            // bloque `else` de abajo:
            //   v_slot = STR_LIT_ADDR(cache_idx)   (@Absolute("code.s_N"))
            //   v_cls  = LOAD(v_slot)              (ClassInfo* cacheado, vm_mem)
            //   v_h    = NEWOBJ(v_cls)             (GcHandle, r0)
            //   v_this = GC_DEREF_HOST(v_h)        (host_ptr al ObjectHeader)
            //   --- si hay ctor efectivo: ---
            //   CALLVIRT(this=v_this, args=params, vtable_idx=ctor)
            //   v_ret  = GC_DEREF_HOST(v_h)        (RE-deref: el GC del ctor
            //                                       puede haber movido el obj)
            //   RET v_ret
            //   --- sin ctor: RET v_this (campos ya a 0 por gc_heap.alloc) ---
            // raw_asm-elim: la variante SHARED (__new_<X>_shared) tambien se
            // emite estructurada usando IrOp::NEWOBJS (newobjs -> SharedHeap)
            // en lugar de NEWOBJ.  Asi NINGUN __new_<X> queda en RAW_ASM.
            const bool emit_structured = true;
            if (emit_structured) {
                // v_slot = direccion del slot ClassInfo* cacheado (static_data).
                const ir::IrValueId v_slot = fn.new_value(ir::IrType::PTR);
                {
                    ir::IrInstr sa{};
                    sa.op          = ir::IrOp::STR_LIT_ADDR;
                    sa.type        = ir::IrType::PTR;
                    sa.dst         = v_slot;
                    sa.imm         = cache_idx;
                    sa.source_line = cd->loc.line;
                    fn.append(entry, std::move(sa));
                }
                // v_cls = LOAD(v_slot): ClassInfo* leido del slot (vm_mem).
                const ir::IrValueId v_cls = fn.new_value(ir::IrType::I64);
                {
                    ir::IrInstr ld{};
                    ld.op          = ir::IrOp::LOAD;
                    ld.type        = ir::IrType::I64;
                    ld.dst         = v_cls;
                    ld.operands    = {v_slot};
                    ld.source_line = cd->loc.line;
                    fn.append(entry, std::move(ld));
                }
                // v_h = NEWOBJ/NEWOBJS(v_cls): aloca el objeto -> GcHandle.  La
                // variante shared usa NEWOBJS (SharedHeap); el handle lleva el
                // bit 31 y GC_DEREF_HOST de abajo lo resuelve por el path shared.
                const ir::IrValueId v_h = fn.new_value(ir::IrType::I64);
                {
                    ir::IrInstr no{};
                    no.op          = is_shared_variant ? ir::IrOp::NEWOBJS
                                                       : ir::IrOp::NEWOBJ;
                    no.type        = ir::IrType::I64;
                    no.dst         = v_h;
                    no.operands    = {v_cls};
                    no.source_line = cd->loc.line;
                    fn.append(entry, std::move(no));
                }
                // v_this = GC_DEREF_HOST(v_h): host_ptr al ObjectHeader.
                const ir::IrValueId v_this = fn.new_value(ir::IrType::PTR);
                fn.values[v_this].is_host_ptr  = true;
                fn.values[v_this].is_gc_object = true;
                {
                    ir::IrInstr ra{};
                    ra.op          = ir::IrOp::GC_DEREF_HOST;
                    ra.type        = ir::IrType::PTR;
                    ra.dst         = v_this;
                    ra.operands    = {v_h};
                    ra.source_line = cd->loc.line;
                    fn.append(entry, std::move(ra));
                }

                if (effective_ctor) {
                    // CALLVIRT(this=v_this, args=params del helper).  El ctor
                    // es void; argc = nargs + 1 (this + args).
                    //
                    // CLAVE: v_this (host_ptr GC) vive a traves del call (se usa
                    // en el RET de abajo).  El mecanismo de preservacion de GC
                    // roots lo convierte a handle ANTES del call y lo refresca a
                    // host_ptr DESPUES (save_live_regs gchandle/push/pop/gcderef
                    // en el interp; spill + stackmap del commit 6 en el vreg) --
                    // exactamente lo que el RAW_ASM legacy hacia a mano.  Por
                    // eso NO re-derefamos ni marcamos el handle: marcar v_h
                    // (que YA es handle) como GC hacia que el interp le aplicara
                    // gchandle (host_ptr->handle) sobre un handle -> basura.
                    ir::IrInstr cv{};
                    cv.op          = ir::IrOp::CALLVIRT;
                    cv.type        = ir::IrType::VOID;
                    cv.dst         = ir::IR_NO_VALUE;
                    cv.operands.reserve(nargs + 1);
                    cv.operands.push_back(v_this);
                    for (size_t i = 0; i < nargs; ++i) {
                        cv.operands.push_back(fn.params[i]);
                    }
                    cv.imm         = static_cast<uint64_t>(ctor_vtable_idx);
                    cv.source_line = cd->loc.line;
                    fn.append(entry, std::move(cv));
                }

                // RET v_this (host_ptr al objeto; preservado/refrescado a
                // traves del callvirt si habia ctor).
                {
                    ir::IrInstr ret{};
                    ret.op          = ir::IrOp::RET;
                    ret.type        = ir::IrType::PTR;
                    ret.operands    = {v_this};
                    ret.source_line = cd->loc.line;
                    fn.append(entry, std::move(ret));
                }
            } else {
            // Construir RAW_ASM body.
            std::ostringstream asm_;

            // fix12 - optimizaciones bytecode-level del helper:
            //  (1) Cargar cache en r1 directamente (skip mov r1, r12).
            //  (2) push r0 directo (handle) en lugar de gchandle r12, r12.
            //  (3) xchg cur0, r1 post-newobj para obtener host_ptr en r1
            //      sin la secuencia xchg cur0, r12 + mov r1, r12.
            // Para nargs=0 estas tres optimizaciones combinan a 3 instr menos.
            // Para nargs>0 ahorran 1 instr (la shift derecha sigue necesaria).

            if (nargs == 0) {
                // Caso comun y ultra-optimizado: ctor sin args (o sin ctor,
                // o ctor zero-init trivial saltado por fix12).
                // Cargar ClassInfo* directo en r1 (sin pasar por r12).
                asm_ << "mov r1, @Absolute(\"code.s_" << cache_idx << "\")\n";
                asm_ << "mov r1, [r1]\n"; // r1 = ClassInfo* (cacheado)
                asm_ << "mov r15, 1\n";
                asm_ << newobj_op << " r1\n"; // r0 = GcHandle, r1 = ClassInfo*
                if (effective_ctor) {
                    // Preservar handle directamente con push r0 (sin gchandle).
                    // newobj acaba de devolver r0=handle; el GC del ctor body
                    // puede mover el objeto pero el handle es estable.
                    asm_ << "push r0\n";          // save handle pre-ctor
                    asm_ << "gcderef cur0, r0\n"; // cur0 = host_ptr
                    asm_ << "xchg cur0, r1\n";    // r1 = host_ptr (this); cur0 = ClassInfo*
                    asm_ << "mov r15, 1\n";
                    asm_ << "callvirt r1, " << ctor_vtable_idx << "\n";
                    asm_ << "pop r12\n";           // r12 = handle (restored)
                    asm_ << "gcderef cur0, r12\n"; // cur0 = host_ptr fresco
                    asm_ << "xchg cur0, r12\n";    // r12 = host_ptr
                    asm_ << "mov r0, r12\n";
                } else {
                    // Sin ctor (real o saltado por zero-init opt): convertir
                    // handle a host_ptr y devolverlo.  Los fields ya estan
                    // a 0 por el memset de gc_heap.alloc.
                    asm_ << "gcderef cur0, r0\n";
                    asm_ << "xchg cur0, r12\n"; // r12 = host_ptr
                    asm_ << "mov r0, r12\n";
                }
            } else {
                // Caso con args: necesitamos salvar/restaurar args alrededor
                // del newobj (que clobbera r1) y hacer shift derecha pre-callvirt.
                // Salvar args en stack: push r1..r_N (orden ascendente).
                for (size_t i = 0; i < nargs; ++i) {
                    asm_ << "push r" << (i + 1) << "\n";
                }
                // Cargar cache en r1 (los args ya estan salvados).
                asm_ << "mov r1, @Absolute(\"code.s_" << cache_idx << "\")\n";
                asm_ << "mov r1, [r1]\n"; // r1 = ClassInfo*
                asm_ << "mov r15, 1\n";
                asm_ << newobj_op << " r1\n"; // r0 = handle
                // Convertir handle a host_ptr en r12 (que esta libre).
                asm_ << "gcderef cur0, r0\n";
                asm_ << "xchg cur0, r12\n"; // r12 = host_ptr
                // Restaurar args en orden inverso (LIFO): r_N, ..., r1.
                for (size_t i = nargs; i > 0; --i) {
                    asm_ << "pop r" << i << "\n";
                }
                if (effective_ctor) {
                    // Shift derecha: r_{N+1}=r_N, ..., r2=r1.
                    for (size_t i = nargs + 1; i >= 2; --i) {
                        asm_ << "mov r" << i << ", r" << (i - 1) << "\n";
                    }
                    asm_ << "mov r1, r12\n"; // this = host_ptr
                    // fix - preservar handle a traves de callvirt
                    // porque el ctor body puede hacer GC moves.
                    asm_ << "gchandle r12, r12\n"; // r12 = handle
                    asm_ << "push r12\n";
                    asm_ << "mov r15, " << (nargs + 1) << "\n";
                    asm_ << "callvirt r1, " << ctor_vtable_idx << "\n";
                    asm_ << "pop r12\n";           // r12 = handle
                    asm_ << "gcderef cur0, r12\n"; // cur0 = host_ptr fresco
                    asm_ << "xchg cur0, r12\n";
                }
                asm_ << "mov r0, r12\n";
            }

            ir::IrInstr ra{};
            ra.op          = ir::IrOp::RAW_ASM;
            ra.type        = ir::IrType::PTR;
            ra.dst         = ir::IR_NO_VALUE;
            ra.func_name   = asm_.str();
            ra.source_line = cd->loc.line;
            fn.append(entry, std::move(ra));

            // Cerrar con RET PTR (r0 ya tiene el handle).
            ir::IrInstr ret{};
            ret.op   = ir::IrOp::RET;
            ret.type = ir::IrType::PTR;
            // No anyadimos operands; el emisor IR genera 'ret' simple y r0
            // ya esta cargado por el RAW_ASM previo.  Para que el emisor no
            // intente construir RET con un valor SSA, usamos VOID y luego
            // dejamos un fall-through.  Mejor: ret sin operandos como void.
            ret.type        = ir::IrType::VOID;
            ret.source_line = cd->loc.line;
            fn.append(entry, std::move(ret));
            } // fin else (path RAW_ASM legacy: ctor / nargs>0 / shared)

            propagate_is_gc_object_through_phis(fn);

            // B.3 contract: el helper @c __new_<Class> tambien lleva
            // metadata de monomorphizacion cuando la clase es una
            // instanciacion generica.  Asi C2/AOT pueden agrupar todas
            // las funciones (metodos + helpers) de una specialization
            // como una unidad.
            if (const auto *mi = tc_.monomorph_info(cd->name)) {
                fn.generic_template_name = mi->template_name;
                fn.generic_type_args     = mi->type_args;
            }

            // Bug fix 2026-05-23: registrar como helper PURO si el ctor
            // es trivial (sin callvirt al ctor user-defined).  Solo entonces
            // el DCE puede eliminar el __new_<X> cuando el handle no se usa.
            // Sin esto, ctors que pueden throw veian sus excepciones
            // swallow-eadas cuando el resultado del `new X()` no se usaba.
            if (!effective_ctor) {
                ir::register_pure_new_helper(fn.name);
            }
            out.add_function(std::move(fn));
            } // for variant in {local, shared}
        }
    }

    void Lowering::generate_module_init_function(ir::IrModule &out) {
        // Si no hay clases NI runtime globals que inicializar, no
        // generamos __module_init.
        bool any_class = false;
        for (auto &decl: mod_.decls) {
            if (decl && decl->kind == ast::NodeKind::ClassDecl) {
                any_class = true;
                break;
            }
        }
        const bool has_runtime_globals = !runtime_global_slots_.empty();
        if (!any_class && !has_runtime_globals) return;

        ir::IrFunction fn;
        fn.name                   = "__module_init";
        fn.ret_type               = ir::IrType::VOID;
        const ir::IrBlockId entry = fn.new_block("entry");

        // raw_asm-elim Fase 2c: __module_init se construye con IR ESTRUCTURADO
        // (en vez de un unico bloque RAW_ASM monolitico), para que el path vreg
        // del JIT lo compile sin el mini-parser de raw_asm y para que el futuro
        // AOT no vea bytecode opaco.  Patron equivalente al RAW_ASM legacy:
        //   - Un buffer de params FRESCO (ALLOCA 40 bytes = max de los structs
        //     24/32/40) por operacion.  El buffer NO se promueve a host (sus
        //     usos como operando de DEF*/FIND* son escape para
        //     promote_local_allocas) -> queda como vaddr valido que los ops de
        //     meta-OOP leen via params_vaddr.
        //   - STORE a vm_mem (el buffer es vaddr) arma cada struct; el DEF/FIND
        //     correspondiente lo consume inmediatamente.  Los ops meta-OOP son
        //     side-effecting + barreras de DSE/scheduler -> el optimizer no
        //     reordena ni elimina los STORE que arman el struct.
        //   - CADA clase y CADA advice se emiten en su PROPIO basic block
        //     (encadenados por BR).  Razon: el regalloc del .vel (interp) es
        //     fragil con un unico bloque enorme de cientos de temporales (una
        //     instruccion de mas voltea el spill a codigo incorrecto -> findclass
        //     lee params de una direccion basura).  Con un bloque por clase/
        //     advice los valores son LOCALES al bloque (buffers frescos, v_cls
        //     recargado del cache slot, valores AOP intra-advice) -> el liveness
        //     se resetea en cada frontera -> presion acotada -> regalloc correcto.
        //     Sin PHIs ni valores cross-block.
        const int ln = 0;  // ops sinteticas (sin linea fuente propia)
        ir::IrBlockId cur = entry;  // bloque actual (avanza con new_seg)

        // Crea un nuevo segmento (basic block) y encadena con BR desde el actual.
        auto new_seg = [&](const std::string &name) {
            const ir::IrBlockId nb = fn.new_block(name);
            ir::IrInstr br{};
            br.op = ir::IrOp::BR; br.type = ir::IrType::VOID;
            br.target_block = nb; br.source_line = ln;
            fn.append(cur, std::move(br));
            cur = nb;
        };

        // --- helpers locales para emitir IR sobre fn/cur ---
        auto emit_const64 = [&](uint64_t k) -> ir::IrValueId {
            const ir::IrValueId v = fn.new_value(ir::IrType::I64);
            ir::IrInstr c{};
            c.op = ir::IrOp::CONST; c.type = ir::IrType::I64;
            c.dst = v; c.imm = k; c.source_line = ln;
            fn.append(cur, std::move(c));
            return v;
        };
        auto emit_strlit = [&](uint64_t idx) -> ir::IrValueId {
            // Direccion vaddr del slot static_data idx (@Absolute("code.s_N")).
            const ir::IrValueId v = fn.new_value(ir::IrType::PTR);
            ir::IrInstr s{};
            s.op = ir::IrOp::STR_LIT_ADDR; s.type = ir::IrType::PTR;
            s.dst = v; s.imm = idx; s.source_line = ln;
            fn.append(cur, std::move(s));
            return v;
        };
        auto emit_label_addr = [&](const std::string &label) -> ir::IrValueId {
            // Direccion vaddr de un label de codigo (@Absolute("code.LABEL")).
            const ir::IrValueId v = fn.new_value(ir::IrType::PTR);
            ir::IrInstr s{};
            s.op = ir::IrOp::LABEL_ADDR; s.type = ir::IrType::PTR;
            s.dst = v; s.func_name = label; s.source_line = ln;
            fn.append(cur, std::move(s));
            return v;
        };
        // Buffer de params UNICO (40 bytes = max de los structs 24/32/40) en el
        // VM stack, alocado UNA sola vez y reusado para todos los defclass/
        // deffield/defmethod/findmethod/setmethdbg/findclass/addadvice.  Razones:
        //   - Un ALLOCA por OP inflaria el VM stack (subsp por op sin addsp hasta
        //     el `leave`); con muchas clases/metodos eso baja el VM-RSP cientos de
        //     bytes y cambia el layout VM-stack/heap -> destapa bugs latentes de
        //     scan/GC dependientes del layout (99/133/179 crasheaban en interp).
        //     Un solo ALLOCA mantiene el VM stack casi plano (como el RAW_ASM).
        //   - El multi-bloque (un block por clase/advice) + el SPILL cross-bloque
        //     del .vel regalloc evitan el clobber del registro del buffer (el bug
        //     del single-block era tenerlo en un registro vivo a traves de ~80
        //     ops; spillado a un slot y recargado por uso, no se clobbea).
        //   - La DSE-barrera de los ops meta-OOP preserva los STORE que arman el
        //     struct aunque se reuse buf+0 entre defs.
        ir::IrValueId shared_buf = ir::IR_NO_VALUE;
        auto fresh_buf = [&]() -> ir::IrValueId {
            if (shared_buf != ir::IR_NO_VALUE) return shared_buf;
            const ir::IrValueId v = fn.new_value(ir::IrType::PTR);
            ir::IrInstr al{};
            al.op = ir::IrOp::ALLOCA; al.type = ir::IrType::I8;  // unidad 1 byte
            al.dst = v; al.imm = 40; al.source_line = ln;
            fn.append(cur, std::move(al));  // is_host_ptr=false -> VM stack
            shared_buf = v;
            return v;
        };
        // STORE v_val en buf + off (vm_mem; el buffer es vaddr).
        auto store_at = [&](ir::IrValueId buf, uint64_t off, ir::IrValueId v_val) {
            ir::IrValueId v_addr = buf;
            if (off != 0) {
                v_addr = fn.new_value(ir::IrType::PTR);
                ir::IrInstr ad{};
                ad.op = ir::IrOp::ADD; ad.type = ir::IrType::I64;
                ad.dst = v_addr;
                ad.operands = {buf, emit_const64(off)};
                ad.source_line = ln;
                fn.append(cur, std::move(ad));  // is_host_ptr=false (vaddr)
            }
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE; st.type = ir::IrType::I64;
            st.operands = {v_val, v_addr}; st.source_line = ln;
            fn.append(cur, std::move(st));
        };
        // FINDCLASS/DEFCLASS/FINDMETHOD: dst = op(buf).  ClassInfo*/MethodInfo*
        // son host_ptr nativos (no GC) -> is_host_ptr=true, is_gc_object=false.
        auto emit_find1 = [&](ir::IrOp op, ir::IrValueId buf) -> ir::IrValueId {
            const ir::IrValueId v = fn.new_value(ir::IrType::PTR);
            fn.values[v].is_host_ptr = true;
            ir::IrInstr i{};
            i.op = op; i.type = ir::IrType::PTR; i.dst = v;
            i.operands = {buf}; i.source_line = ln;
            fn.append(cur, std::move(i));
            return v;
        };
        // DEFFIELD/DEFMETHOD: op(v_cls, buf) sin dst.
        auto emit_def2 = [&](ir::IrOp op, ir::IrValueId v_cls, ir::IrValueId buf) {
            ir::IrInstr i{};
            i.op = op; i.type = ir::IrType::VOID;
            i.operands = {v_cls, buf}; i.source_line = ln;
            fn.append(cur, std::move(i));
        };

        for (auto &decl: mod_.decls) {
            if (!decl || decl->kind != ast::NodeKind::ClassDecl) continue;
            auto *cd = static_cast<ast::ClassDecl *>(decl.get());
            auto  it = tc_.class_layouts().find(cd->name);
            if (it == tc_.class_layouts().end()) continue;
            const ClassLayout &lay = it->second;

            // Bloque propio para esta clase (presion de registros acotada).
            new_seg("cls_" + cd->name);

            const uint64_t cname_idx = intern_class_name(out, cd->name);
            const uint32_t cname_len = static_cast<uint32_t>(cd->name.size());

            // 1) Si hay superclase: FindClassParams (16B) + findclass -> v_super.
            ir::IrValueId v_super = ir::IR_NO_VALUE;
            if (!cd->super_name.empty()) {
                const uint64_t sname_idx = intern_class_name(out, cd->super_name);
                const uint32_t sname_len = static_cast<uint32_t>(cd->super_name.size());
                const ir::IrValueId b = fresh_buf();
                store_at(b, 0, emit_strlit(sname_idx));
                store_at(b, 8, emit_const64(sname_len));
                v_super = emit_find1(ir::IrOp::FINDCLASS, b);
            }

            // 2) DefClassParams (32B) + defclass -> v_cls.
            const ir::IrValueId b_dc = fresh_buf();
            store_at(b_dc, 0, emit_strlit(cname_idx));
            // [+8] (flags<<32)|name_len con flags=CLASS_VIS_PUBLIC=1.
            store_at(b_dc, 8, emit_const64((uint64_t(1) << 32) | uint64_t(cname_len)));
            // [+16] super_class (ClassInfo* o 0).
            store_at(b_dc, 16, (v_super != ir::IR_NO_VALUE) ? v_super : emit_const64(0));
            // [+24] reserved = 0.
            store_at(b_dc, 24, emit_const64(0));
            const ir::IrValueId v_cls = emit_find1(ir::IrOp::DEFCLASS, b_dc);

            // 2.5) Cachear el ClassInfo* en su slot static_data (lo lee
            // __new_<Class> sin findclass).  STORE a vm_mem (slot = vaddr).
            const uint64_t cache_idx = intern_class_cache_slot(out, cd->name);
            {
                const ir::IrValueId v_cache = emit_strlit(cache_idx);
                ir::IrInstr st{};
                st.op = ir::IrOp::STORE; st.type = ir::IrType::I64;
                st.operands = {v_cls, v_cache}; st.source_line = ln;
                fn.append(cur, std::move(st));
            }

            // Helper: recargar el ClassInfo* desde el cache slot (corto-vivo en
            // cada def para no estresar el regalloc con un v_cls vivo a traves
            // de muchos deffield/defmethod).
            auto reload_cls = [&]() -> ir::IrValueId {
                const ir::IrValueId v_a = emit_strlit(cache_idx);
                const ir::IrValueId v = fn.new_value(ir::IrType::PTR);
                fn.values[v].is_host_ptr = true;
                ir::IrInstr ld{};
                ld.op = ir::IrOp::LOAD; ld.type = ir::IrType::I64;
                ld.dst = v; ld.operands = {v_a}; ld.source_line = ln;
                fn.append(cur, std::move(ld));
                return v;
            };

            // 3) Fields PROPIOS (los heredados ya los copio define_class).
            for (size_t fi = lay.inherited_field_count; fi < lay.fields.size(); ++fi) {
                const auto &   f         = lay.fields[fi];
                const uint64_t fname_idx = intern_class_name(out, f.name);
                const uint32_t fname_len = static_cast<uint32_t>(f.name.size());
                const ir::IrValueId b = fresh_buf();
                store_at(b, 0, emit_strlit(fname_idx));
                // [+8] name_len (kind/access/is_static = 0 para field de instancia).
                store_at(b, 8, emit_const64(uint64_t(fname_len)));
                store_at(b, 16, emit_const64(8));   // size_bytes = 8 (1 slot)
                store_at(b, 24, emit_const64(0));   // type_class = 0 (primitive)
                emit_def2(ir::IrOp::DEFFIELD, reload_cls(), b);
            }

            // 3.5) Static fields PROPIOS (is_static=1 en bit +48 del packed).
            for (size_t si = lay.inherited_static_field_count;
                 si < lay.static_fields.size(); ++si) {
                const auto &   f         = lay.static_fields[si];
                const uint64_t fname_idx = intern_class_name(out, f.name);
                const uint32_t fname_len = static_cast<uint32_t>(f.name.size());
                const ir::IrValueId b = fresh_buf();
                store_at(b, 0, emit_strlit(fname_idx));
                store_at(b, 8, emit_const64(uint64_t(fname_len) | (uint64_t(1) << 48)));
                store_at(b, 16, emit_const64(8));
                store_at(b, 24, emit_const64(0));
                emit_def2(ir::IrOp::DEFFIELD, reload_cls(), b);
            }

            // Las interfaces no emiten defmethod (metodos abstractos sin code).
            if (cd->is_interface) continue;

            // 4) Metodos PROPIOS o sobreescritos (defining_class == cd->name).
            for (const auto &m: lay.methods) {
                if (!m.defining_class.empty() && m.defining_class != cd->name)
                    continue;  // heredado puro
                const std::string suffix      = m.is_constructor ? std::string("ctor") : m.name;
                const std::string owner_class = m.defining_class.empty()
                                                    ? cd->name : m.defining_class;
                const std::string method_label = owner_class + "__" + suffix;
                const uint64_t mname_idx = intern_class_name(out, m.name);
                const uint32_t mname_len = static_cast<uint32_t>(m.name.size());
                const std::string desc_str = "()";
                const uint64_t    desc_idx = intern_class_name(out, desc_str);
                const uint32_t    desc_len = static_cast<uint32_t>(desc_str.size());

                uint64_t mflags = 0;
                if (m.is_constructor) mflags |= (1ULL << 9);   // METHOD_FLAG_CONSTRUCTOR
                else                  mflags |= (1ULL << 10);  // METHOD_FLAG_VIRTUAL

                // DefMethodParams (40B) + defmethod.
                const ir::IrValueId b = fresh_buf();
                store_at(b, 0, emit_strlit(mname_idx));
                store_at(b, 8, emit_const64(uint64_t(mname_len) | (uint64_t(desc_len) << 32)));
                store_at(b, 16, emit_strlit(desc_idx));
                store_at(b, 24, emit_label_addr(method_label));   // code_vaddr
                store_at(b, 32, emit_const64(mflags));
                emit_def2(ir::IrOp::DEFMETHOD, reload_cls(), b);

                // Debug info (file:line) si la hay: findmethod + setmethdbg.
                if (!m.source_file.empty() && m.source_line > 0) {
                    const uint64_t fname_idx = intern_class_name(out, m.source_file);
                    const uint32_t fname_len = static_cast<uint32_t>(m.source_file.size());

                    // FindMethodParams (24B) + findmethod -> v_method.
                    const ir::IrValueId bf = fresh_buf();
                    store_at(bf, 0, reload_cls());
                    store_at(bf, 8, emit_strlit(mname_idx));
                    store_at(bf, 16, emit_const64(uint64_t(mname_len)));
                    const ir::IrValueId v_method = emit_find1(ir::IrOp::FINDMETHOD, bf);

                    // SetMethDebugParams (24B) + setmethdbg.
                    const ir::IrValueId bs = fresh_buf();
                    store_at(bs, 0, v_method);
                    store_at(bs, 8, emit_strlit(fname_idx));
                    store_at(bs, 16, emit_const64(uint64_t(fname_len)
                                                  | (uint64_t(m.source_line) << 32)));
                    ir::IrInstr smd{};
                    smd.op = ir::IrOp::SETMETHDBG; smd.type = ir::IrType::VOID;
                    smd.operands = {v_method, bs}; smd.source_line = ln;
                    fn.append(cur, std::move(smd));
                }
            }
        }

        // -----------------------------------------------------------------
        // 2do pase: AOP.  Tras registrar todas las clases/metodos, recorremos
        // los aspectos y emitimos findclass + findmethod (x2) + addadvice por
        // cada @Before/@After/@Around (cada advice en su propio bloque).
        // Requiere que las clases target ya esten registradas (2do pase).
        // -----------------------------------------------------------------
        for (auto &decl: mod_.decls) {
            if (!decl || decl->kind != ast::NodeKind::ClassDecl) continue;
            auto *cd = static_cast<ast::ClassDecl *>(decl.get());
            if (!cd->type_params.empty()) continue;  // template, no se procesa
            for (auto &m_uptr: cd->methods) {
                auto *m = m_uptr.get();
                if (!m || m->advice_kind == 0) continue;

                const std::string &target = m->advice_target;
                const size_t        dot   = target.find('.');
                if (dot == std::string::npos || dot == 0 || dot + 1 >= target.size()) {
                    error_at(m->loc,
                             "AOP: pointcut '" + target + "' no tiene formato 'Clase.metodo'");
                    continue;
                }
                const std::string tcls  = target.substr(0, dot);
                const std::string tmeth = target.substr(dot + 1);
                const uint8_t     rt_kind = static_cast<uint8_t>(m->advice_kind - 1);

                // Bloque propio para este advice (presion acotada).
                new_seg("aop_" + cd->name + "_" + m->name);

                // 1) findclass de la clase target -> v_tc.
                const uint64_t tcls_idx = intern_class_name(out, tcls);
                const uint32_t tcls_len = static_cast<uint32_t>(tcls.size());
                const ir::IrValueId b1 = fresh_buf();
                store_at(b1, 0, emit_strlit(tcls_idx));
                store_at(b1, 8, emit_const64(tcls_len));
                const ir::IrValueId v_tc = emit_find1(ir::IrOp::FINDCLASS, b1);

                // 2) findmethod del target -> v_tm.
                const uint64_t tmeth_idx = intern_class_name(out, tmeth);
                const uint32_t tmeth_len = static_cast<uint32_t>(tmeth.size());
                const ir::IrValueId b2 = fresh_buf();
                store_at(b2, 0, v_tc);
                store_at(b2, 8, emit_strlit(tmeth_idx));
                store_at(b2, 16, emit_const64(tmeth_len));
                const ir::IrValueId v_tm = emit_find1(ir::IrOp::FINDMETHOD, b2);

                // 3) findclass del aspecto -> v_ac.
                const uint64_t acls_idx = intern_class_name(out, cd->name);
                const uint32_t acls_len = static_cast<uint32_t>(cd->name.size());
                const ir::IrValueId b3 = fresh_buf();
                store_at(b3, 0, emit_strlit(acls_idx));
                store_at(b3, 8, emit_const64(acls_len));
                const ir::IrValueId v_ac = emit_find1(ir::IrOp::FINDCLASS, b3);

                // 4) findmethod del advice -> v_am.
                const uint64_t adm_idx = intern_class_name(out, m->name);
                const uint32_t adm_len = static_cast<uint32_t>(m->name.size());
                const ir::IrValueId b4 = fresh_buf();
                store_at(b4, 0, v_ac);
                store_at(b4, 8, emit_strlit(adm_idx));
                store_at(b4, 16, emit_const64(adm_len));
                const ir::IrValueId v_am = emit_find1(ir::IrOp::FINDMETHOD, b4);

                // 5) addadvice(target, advice, kind).
                ir::IrInstr aa{};
                aa.op = ir::IrOp::ADDADVICE; aa.type = ir::IrType::VOID;
                aa.operands = {v_tm, v_am}; aa.imm = rt_kind; aa.source_line = ln;
                fn.append(cur, std::move(aa));
            }
        }

        // L2.2: inicializar runtime globals con su literal de init.
        // Activamos el contexto del lowering (fn_=&fn, current_block_=cur)
        // temporalmente para reutilizar emit_const/STRMAKE/STORE.  Usamos
        // @c cur (el ultimo bloque encadenado), no @c entry, porque el
        // generador parte __module_init en multiples bloques.
        if (has_runtime_globals) {
            ir::IrFunction *saved_fn      = fn_;
            ir::IrBlockId   saved_block   = current_block_;
            bool            saved_term    = block_terminated_;
            fn_               = &fn;
            current_block_    = cur;
            block_terminated_ = false;
            for (auto &decl : mod_.decls) {
                if (!decl || decl->kind != ast::NodeKind::GlobalVarDecl) continue;
                auto *gv = static_cast<ast::GlobalVarDecl *>(decl.get());
                if (gv->is_const || !gv->init) continue;
                auto rit = runtime_global_slots_.find(gv->name);
                if (rit == runtime_global_slots_.end()) continue;
                const uint64_t slot_idx = rit->second;
                const int ln = gv->loc.line;
                // Computar el valor inicial via lower_expr (cubre IntLit,
                // StringLit-no-interpolado promovido a StringObject por
                // lower_string_literal_to_string_object, etc.).
                ir::IrValueId v_init = ir::IR_NO_VALUE;
                if (gv->init->kind == ast::NodeKind::StringLitExpr) {
                    auto *slit = static_cast<ast::StringLitExpr *>(gv->init.get());
                    v_init = lower_string_literal_to_string_object(slit);
                } else {
                    v_init = lower_expr(gv->init.get());
                }
                if (v_init == ir::IR_NO_VALUE) continue;
                // STR_LIT_ADDR -> slot addr, STORE init.
                ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR); {
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
                st.operands    = {v_init, v_addr};
                st.source_line = ln;
                fn_->append(current_block_, std::move(st));
            }
            fn_               = saved_fn;
            current_block_    = saved_block;
            block_terminated_ = saved_term;
        }

        ir::IrInstr ret{};
        ret.op          = ir::IrOp::RET;
        ret.type        = ir::IrType::VOID;
        ret.source_line = 0;
        fn.append(cur, std::move(ret));  // ultimo bloque encadenado

        propagate_is_gc_object_through_phis(fn);
        out.add_function(std::move(fn));
    }

    std::string Lowering::build_module_init_asm(ir::IrModule & /*out_module*/) {
        // No se usa: la generacion de __module_init se hace via
        // generate_module_init_function (IrFunction completa, no cadena).
        return std::string();
    }

    ir::IrValueId Lowering::lower_new_expr(ast::NewExpr *e) {
        // bug4: array allocation `new T[N]`.  Emit RAW_ALLOC(N * sizeof(T))
        // y devolver host_ptr al primer elemento.  Cada slot zero-init
        // por RawAllocator.
        if (e->array_size) {
            // Resolver sizeof(T) en bytes.
            uint64_t elem_size = 8;  // default qword (suficiente para
                                      // class refs, strings handles, ptrs).
            auto pk_size = [](const std::string &n) -> uint64_t {
                if (n == "i8" || n == "u8" || n == "bool" || n == "char") return 1;
                if (n == "i16" || n == "u16") return 2;
                if (n == "i32" || n == "u32" || n == "f32" || n == "float") return 4;
                if (n == "i64" || n == "u64" || n == "f64" || n == "double" || n == "string")
                    return 8;
                return 0;  // no es primitivo
            };
            const uint64_t prim_sz = pk_size(e->class_name);
            if (prim_sz > 0) {
                elem_size = prim_sz;
            } else {
                // Clase user: host_ptr de 8 bytes por slot.
                auto it_cls = tc_.class_layouts().find(e->class_name);
                if (it_cls != tc_.class_layouts().end()) {
                    elem_size = 8;  // refs a objetos GC
                } else {
                    // Struct value-type o enum.
                    auto it_st = tc_.struct_layouts().find(e->class_name);
                    if (it_st != tc_.struct_layouts().end()) {
                        elem_size = static_cast<uint64_t>(it_st->second.size_bytes);
                    } else {
                        auto it_en = tc_.enum_layouts().find(e->class_name);
                        if (it_en != tc_.enum_layouts().end()) {
                            elem_size = static_cast<uint64_t>(it_en->second.size_bytes);
                        }
                    }
                }
            }
            // Lower count + multiplica por elem_size.
            const ir::IrValueId v_count = lower_expr(e->array_size.get());
            if (v_count == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            // Coerce count a i64 si no lo es.
            const ir::IrType ct = fn_->values[v_count].type;
            ir::IrValueId v_count_i64 = (ct == ir::IrType::I64 || ct == ir::IrType::U64)
                ? v_count
                : cast_if_needed(v_count, ct, ir::IrType::I64, e->loc.line,
                                  /*is_explicit=*/true);
            // total = count * elem_size.
            ir::IrValueId v_total;
            if (elem_size == 1) {
                v_total = v_count_i64;
            } else {
                const ir::IrValueId v_es = emit_const(ir::IrType::I64,
                    static_cast<int64_t>(elem_size), e->loc.line);
                v_total = fn_->new_value(ir::IrType::I64);
                ir::IrInstr mul{};
                mul.op          = ir::IrOp::MUL;
                mul.type        = ir::IrType::I64;
                mul.dst         = v_total;
                mul.operands    = {v_count_i64, v_es};
                mul.source_line = e->loc.line;
                fn_->append(current_block_, std::move(mul));
            }
            // RAW_ALLOC(total) -> host_ptr.
            const ir::IrValueId v_ptr = fn_->new_value(ir::IrType::PTR);
            fn_->values[v_ptr].is_host_ptr = true;
            ir::IrInstr ra{};
            ra.op          = ir::IrOp::RAW_ALLOC;
            ra.type        = ir::IrType::PTR;
            ra.dst         = v_ptr;
            ra.operands    = {v_total};
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
            return v_ptr;
        }
        // BugFix R4: `new ExceptionClass(msg)` para predefined exceptions
        // (RuntimeException, ArithmeticException, etc.).  No hay __new_<X>
        // synthetic helper; emitimos newobj + store message inline.
        {
            auto it_cls_pre = tc_.class_layouts().find(e->class_name);
            if (it_cls_pre != tc_.class_layouts().end()
             && it_cls_pre->second.is_runtime_predefined
             && e->class_name != "FatalError") {
                if (e->args.size() != 1) {
                    error_at(e->loc,
                        "new " + e->class_name + "(msg): se espera 1 argumento");
                    return ir::IR_NO_VALUE;
                }
                // Auto-promocion literal -> StringObject (mismo patron que
                // lower_call para args string).
                ir::IrValueId v_msg;
                if (e->args[0]->kind == ast::NodeKind::StringLitExpr) {
                    auto *sl = static_cast<ast::StringLitExpr *>(e->args[0].get());
                    v_msg = lower_string_literal_to_string_object(sl);
                } else {
                    v_msg = lower_expr(e->args[0].get());
                }
                if (v_msg == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
                // findclass <exc_name> -> v_cls (host_ptr a ClassInfo).
                const uint64_t cname_idx = intern_class_name(*out_mod_, e->class_name);
                const uint32_t cname_len = static_cast<uint32_t>(e->class_name.size());
                const ir::IrValueId v_cls = emit_findclass_by_name(
                    cname_idx, cname_len, e->loc.line);
                // Emit NEWOBJ IR (regalloc-aware) -> handle in v_handle.
                // Luego un minimal RAW_ASM convierte handle -> host_ptr
                // via `gcderef cur0, rX; xchg cur0, rX`.  La conversion
                // misma no tiene IR op dedicado, pero el resto de la
                // operacion (NEWOBJ + STORE) si esta en IR puro.
                const ir::IrValueId v_handle = fn_->new_value(ir::IrType::I64);
                {
                    ir::IrInstr no{};
                    no.op          = ir::IrOp::NEWOBJ;
                    no.type        = ir::IrType::PTR;  // tipo simbolico
                    no.dst         = v_handle;
                    no.operands    = {v_cls};
                    no.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(no));
                }
                // raw_asm-elim 2026-05-28: handle -> host_ptr via IrOp::GC_DEREF_HOST.
                // El IR op produce dst sin clobber del src; equivalente
                // semanticamente al RAW_ASM previo `gcderef + xchg + mov`.
                const ir::IrValueId v_obj = fn_->new_value(ir::IrType::PTR);
                fn_->values[v_obj].is_host_ptr  = true;
                fn_->values[v_obj].is_gc_object = true;
                {
                    ir::IrInstr ra{};
                    ra.op          = ir::IrOp::GC_DEREF_HOST;
                    ra.type        = ir::IrType::PTR;
                    ra.dst         = v_obj;
                    ra.operands    = {v_handle};
                    ra.source_line = e->loc.line;
                    fn_->append(current_block_, std::move(ra));
                }
                // store v_msg at v_obj + 24 (message field offset, after
                // ObjectHeader).  is_host_ptr=true for v_obj => emite movh.
                const ir::IrValueId v_off = emit_const(ir::IrType::I64, 24, e->loc.line);
                const ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR);
                fn_->values[v_addr].is_host_ptr = true;
                {
                    ir::IrInstr ad{};
                    ad.op           = ir::IrOp::ADD;
                    ad.type         = ir::IrType::I64;
                    ad.dst          = v_addr;
                    ad.operands     = {v_obj, v_off};
                    ad.source_line  = e->loc.line;
                    fn_->append(current_block_, std::move(ad));
                }
                {
                    ir::IrInstr st{};
                    st.op           = ir::IrOp::STORE;
                    st.type         = ir::IrType::I64;
                    st.dst          = ir::IR_NO_VALUE;
                    st.operands     = {v_msg, v_addr};
                    st.source_line  = e->loc.line;
                    fn_->append(current_block_, std::move(st));
                }
                ssa_concrete_class_[v_obj] = e->class_name;
                return v_obj;
            }
        }

        // Bajar argumentos.  Bug fix 2026-05-23: si el ctor espera un
        // STRING param y el arg es un StringLitExpr, promover a StringObject.
        // Sin esto, `new P("alice")` pasaba el ptr raw del literal como
        // GcHandle invalido al ctor.
        const ClassMethodInfo *ctor_sig = nullptr;
        {
            auto it_cls_sig = tc_.class_layouts().find(e->class_name);
            if (it_cls_sig != tc_.class_layouts().end()) {
                for (const auto &m : it_cls_sig->second.methods) {
                    if (m.is_constructor && m.defining_class == e->class_name) {
                        ctor_sig = &m; break;
                    }
                }
                if (!ctor_sig) {
                    for (const auto &m : it_cls_sig->second.methods) {
                        if (m.is_constructor) { ctor_sig = &m; break; }
                    }
                }
            }
        }
        std::vector<ir::IrValueId> arg_vals;
        arg_vals.reserve(e->args.size());
        for (size_t ai = 0; ai < e->args.size(); ++ai) {
            auto &a = e->args[ai];
            const bool param_is_string =
                ctor_sig && ai < ctor_sig->param_types.size()
                && ctor_sig->param_types[ai].kind == PrimitiveKind::STRING;
            ir::IrValueId av;
            if (param_is_string && a->kind == ast::NodeKind::StringLitExpr) {
                auto *slit = static_cast<ast::StringLitExpr *>(a.get());
                av = lower_string_literal_to_string_object(slit);
            } else {
                av = lower_expr(a.get());
            }
            if (av == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            arg_vals.push_back(av);
        }

        // Emit IrInstr::CALL a __new_<ClassName>(args).  La funcion auxiliar
        // se genera al final de run() via generate_new_helpers.
        //
        // Phase Z.6: si @c e->is_shared, despachamos al helper
        // @c __new_<ClassName>_shared (emite @c newobjs en lugar de @c newobj
        // -> aloca en el SharedHeap).  El frontend marca @c is_shared cuando
        // el var-decl padre tiene modificador @c shared.
        const ir::IrValueId dst = fn_->new_value(ir::IrType::PTR);
        // fix - el resultado es un host_ptr a un objeto GESTIONADO por
        // GC.  Marcamos is_gc_object para que el regalloc, al spillarlo
        // alrededor de cualquier CALL posterior (que pueda disparar GC),
        // emita el dance gchandle/gcderef y refresque el host_ptr.
        fn_->values[dst].is_host_ptr  = true;
        fn_->values[dst].is_gc_object = true;
        // M.L7 ext: clase importada cross-module.  El helper @c __new_<X>
        // en el dep fue emitido con el nombre LOCAL del dep (e.g. "Buffer"),
        // no con el mangled del consumer ("buffer__Buffer").  Si el layout
        // tiene @c imported_helper_suffix , lo usamos como sufijo del label.
        std::string helper_class_name = e->class_name;
        {
            const auto &class_layouts = tc_.class_layouts();
            auto it_lay = class_layouts.find(e->class_name);
            if (it_lay != class_layouts.end()
             && !it_lay->second.imported_helper_suffix.empty()) {
                helper_class_name = it_lay->second.imported_helper_suffix;
            }
        }
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::CALL;
        ins.type        = ir::IrType::PTR;
        ins.dst         = dst;
        ins.func_name   = e->is_shared
                            ? ("__new_" + helper_class_name + "_shared")
                            : ("__new_" + helper_class_name);
        ins.operands    = std::move(arg_vals);
        ins.source_line = e->loc.line;
        fn_->append(current_block_, std::move(ins));
        // Trackear tipo concreto del resultado para devirtualizacion
        // de calls via interface receiver en compile time.
        ssa_concrete_class_[dst] = e->class_name;
        return dst;
    }

    ir::IrValueId Lowering::lower_this_expr(ast::ThisExpr *e) {
        const ir::IrValueId v = lookup("this");
        if (v == ir::IR_NO_VALUE) {
            error_at(e->loc, "lowering: 'this' fuera de contexto de metodo");
        }
        return v;
    }

    /**
     * @brief Helper: construye un IrValueId que apunta a @c base+offset,
     *        marcado como host_ptr (para que LOAD/STORE emitan @c movh).
     *        Si offset es 0, devuelve el base directamente.
     */
    static ir::IrValueId emit_field_addr(ir::IrFunction *fn,
                                         ir::IrBlockId   block,
                                         ir::IrValueId   base,
                                         uint32_t        offset,
                                         uint32_t        line) {
        if (offset == 0) {
            // El frontend marca el resultado como host_ptr para que LOAD/
            // STORE usen movh.  Si la base ya tiene is_host_ptr=true, la
            // propagacion es trivial; si no, lo forzamos aqui (siempre lo
            // sera para nuestros punteros de objeto Vex).
            fn->values[base].is_host_ptr = true;
            return base;
        }
        // Crear constante con el offset y sumar.
        ir::IrInstr         c{};
        const ir::IrValueId off_val   = fn->new_value(ir::IrType::I64);
        fn->values[off_val].is_const  = true;
        fn->values[off_val].const_val = offset;
        c.op                          = ir::IrOp::CONST;
        c.type                        = ir::IrType::I64;
        c.dst                         = off_val;
        c.imm                         = offset;
        c.source_line                 = line;
        fn->append(block, std::move(c));

        const ir::IrValueId addr = fn->new_value(ir::IrType::PTR);
        // Marcar host_ptr: las operaciones LOAD/STORE consultan este flag
        // para emitir mov (VM) o movh (host).  Las direcciones derivadas
        // de un host_ptr siguen siendo host_ptr.
        fn->values[addr].is_host_ptr = true;
        ir::IrInstr add{};
        add.op          = ir::IrOp::ADD;
        add.type        = ir::IrType::PTR;
        add.dst         = addr;
        add.operands    = {base, off_val};
        add.source_line = line;
        fn->append(block, std::move(add));
        return addr;
    }

    ir::IrValueId Lowering::lower_class_field_load(ast::FieldAccessExpr *e) {
        // Limitacion G (cerrada): @c property_kind == 3 marca acceso a
        // static field via @c ClassName.field.  El base es IdentExpr cuyo
        // nombre es la clase; lo resolvemos via findclass inline + getstatic
        // con offset compile-time.  No leemos el tipo del base con
        // @c check_expr (fallaria por "nombre no declarado") sino que
        // tomamos el ClassLayout directamente del nombre.
        if (e->property_kind == 3) {
            if (!e->base || e->base->kind != ast::NodeKind::IdentExpr) {
                error_at(e->loc, "lowering: static field con base no-ClassName");
                return ir::IR_NO_VALUE;
            }
            auto *base_id = static_cast<ast::IdentExpr *>(e->base.get());
            auto  it_cls  = tc_.class_layouts().find(base_id->name);
            if (it_cls == tc_.class_layouts().end()) {
                error_at(e->loc,
                         "lowering: clase desconocida '" + base_id->name + "'");
                return ir::IR_NO_VALUE;
            }
            const ClassLayout &lay_s = it_cls->second;
            uint32_t           s_off = 0;
            Type               s_typ = Type{PrimitiveKind::COUNT};
            bool               s_ok  = false;
            for (const auto &f: lay_s.static_fields) {
                if (f.name == e->field_name) {
                    s_off = f.offset;
                    s_typ = f.type;
                    s_ok  = true;
                    break;
                }
            }
            if (!s_ok) {
                error_at(e->loc,
                         "lowering: static field '" + e->field_name +
                         "' no encontrado en la clase '" + base_id->name + "'");
                return ir::IR_NO_VALUE;
            }
            // 1) Sprint 5: findclass via IR ops (ALLOCA + STORE + FINDCLASS).
            const uint64_t     cname_idx = intern_class_name(*out_mod_, base_id->name);
            const uint32_t     cname_len = static_cast<uint32_t>(base_id->name.size());
            const ir::IrValueId v_cls = emit_findclass_by_name(cname_idx, cname_len, e->loc.line);
            // 2) getstatic {dst}, {src0}, offset_imm  -> v_val.
            // El opcode lee SIEMPRE 8 bytes (i64).  Para tipos < i64 la
            // semantica de sign/zero-extension coincide porque setstatic
            // almacena los bits high del reg fuente que el productor
            // sign-extendio (LOAD/CONST genericos hacen shl+sar para signed).
            const ir::IrType    ir_t  = ir_type_from_primitive(s_typ.kind);
            // Emite GETSTATIC IR op; el bytecode lee i64 que truncamos por tipo
            // mas abajo si el SSA val se usa como ancho menor (semantica heredada).
            ir::IrValueId v_val = emit_getstatic(v_cls, static_cast<uint64_t>(s_off),
                                                  e->loc.line);
            // Cast al tipo logico del field si difiere de I64.
            if (ir_t != ir::IrType::I64) {
                v_val = cast_if_needed(v_val, ir::IrType::I64, ir_t,
                                       e->loc.line, /*is_explicit=*/true);
            }
            // Si el tipo del field es PTR host (no VirtualPtr), propagar
            // is_host_ptr al SSA value (mismo tratamiento que field de
            // instancia, ver final de esta funcion).
            // VirtualPtr (s_typ.is_virtual == true) NO recibe is_host_ptr.
            if (s_typ.kind == PrimitiveKind::PTR && !s_typ.is_virtual) {
                fn_->values[v_val].is_host_ptr = true;
            }
            return v_val;
        }

        const Type bt = e->base->result_type;
        if (bt.kind != PrimitiveKind::CLASS) {
            error_at(e->loc, "lowering: '.' sobre tipo no-clase en lower_class_field_load");
            return ir::IR_NO_VALUE;
        }
        auto it = tc_.class_layouts().find(bt.struct_name);
        if (it == tc_.class_layouts().end()) {
            error_at(e->loc,
                     "lowering: clase desconocida '" + bt.struct_name + "'");
            return ir::IR_NO_VALUE;
        }
        const ClassLayout &lay = it->second;
        // si el type checker marco el acceso como propiedad, emitir
        // CALLVIRT al getter `get_<field_name>` en vez de getfield.
        if (e->property_kind == 1) {
            const std::string      getter_name = std::string("get_") + e->field_name;
            const ClassMethodInfo *mtd         = nullptr;
            for (const auto &m: lay.methods) {
                if (!m.is_constructor && m.name == getter_name) {
                    mtd = &m;
                    break;
                }
            }
            if (!mtd) {
                error_at(e->loc,
                         "lowering: getter de propiedad '" + e->field_name +
                         "' no encontrado en la clase '" + bt.struct_name + "'");
                return ir::IR_NO_VALUE;
            }
            const ir::IrValueId obj = lower_expr(e->base.get());
            if (obj == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            const ir::IrType    ret_ir = ir_type_from_primitive(mtd->return_type.kind);
            const ir::IrValueId dst    = (ret_ir == ir::IrType::VOID)
                                             ? ir::IR_NO_VALUE
                                             : fn_->new_value(ret_ir);
            // Sprint edge-bugs (2026-06-03): si el metodo retorna CLASS, el
            // dst es un host_ptr a un objeto GC.  Marcarlo asi para que el
            // regalloc lo trate como GC-managed (save_live_regs lo convierte
            // a GcHandle antes de cualquier CALL siguiente).  Sin esto un
            // patron `p2 = p1.factory(); p3 = p2.factory();` rompe en interp:
            // el host_ptr de p2 queda stale tras el GC dentro del segundo
            // factory.  Mismo bug con `*->is_host_ptr` no marcado para
            // tipos PTR (e.g. `int* get_buf()`).
            if (dst != ir::IR_NO_VALUE) {
                const PrimitiveKind rk = mtd->return_type.kind;
                if (rk == PrimitiveKind::CLASS) {
                    fn_->values[dst].is_host_ptr  = true;
                    fn_->values[dst].is_gc_object = true;
                } else if ((rk == PrimitiveKind::PTR
                         || rk == PrimitiveKind::ARRAY)
                        && !mtd->return_type.is_virtual) {
                    fn_->values[dst].is_host_ptr = true;
                }
            }
            ir::IrInstr ins{};
            ins.op   = ir::IrOp::CALLVIRT;
            ins.type = ret_ir;
            ins.dst  = dst;
            ins.operands.push_back(obj);
            ins.imm         = static_cast<uint64_t>(mtd->vtable_index);
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            return dst;
        }
        uint32_t off  = 0;
        bool     ok   = false;
        Type     ftyp = Type{PrimitiveKind::COUNT};
        for (const auto &f: lay.fields) {
            if (f.name == e->field_name) {
                off  = f.offset;
                ftyp = f.type;
                ok   = true;
                break;
            }
        }
        if (!ok) {
            error_at(e->loc,
                     "lowering: campo '" + e->field_name +
                     "' no encontrado en la clase '" + bt.struct_name + "'");
            return ir::IR_NO_VALUE;
        }
        const ir::IrValueId obj = lower_expr(e->base.get());
        if (obj == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        // Bajar a addr = obj + off (host_ptr) + LOAD estandar.  El emisor
        // IR consulta is_host_ptr y emite movh (memoria host).  Esto evita
        // el patron cur0/gcderef que colisionaba con el regalloc.
        const ir::IrValueId addr = emit_field_addr(fn_, current_block_, obj, off,
                                                   e->loc.line);
        const ir::IrType    ir_t = ir_type_from_primitive(ftyp.kind);
        const ir::IrValueId dst  = fn_->new_value(ir_t);
        ir::IrInstr         ld{};
        ld.op          = ir::IrOp::LOAD;
        ld.type        = ir_t;
        ld.dst         = dst;
        ld.operands    = {addr};
        ld.source_line = e->loc.line;
        fn_->append(current_block_, std::move(ld));
        // fix: si el TIPO del campo es PTR (host pointer obtenido
        // via malloc o similar), propagar @c is_host_ptr=true al SSA value
        // resultante para que indexaciones / derefs posteriores emitan
        // @c movh y NO @c mov (que iria a VM memory y leeria garbage).
        // Sin esto, `box.p[i]` con `box.p: i32*` cargaba con `mov` en lugar
        // de `movh`, leyendo memoria virtual VM en vez del buffer host
        // de malloc -> garbage o segfault.
        // EXCEPCION: VirtualPtr<T> (ftyp.is_virtual == true) es una direccion
        // VM aunque el tipo base sea PTR.  El valor cargado es una VA del
        // espacio VM, NO un puntero host.  Marcar is_host_ptr=true sobre un
        // VirtualPtr causaria que `*field` emitiera movh en vez de mov,
        // interpretando la VA como direccion host -> segfault.
        if (ftyp.kind == PrimitiveKind::PTR && !ftyp.is_virtual) {
            fn_->values[dst].is_host_ptr = true;
        }
        // Dynamic arrays `T[]` (size==0) stored as fields also hold host_ptrs
        // (from `new T[N]` via RAW_ALLOC).  Sin esto, `box.data[i] = ...`
        // emitia `mov` (VM mem) en vez de `movh` (host mem) tras LOAD del
        // field -> escribia/leia en vm_mem en una direccion que es realmente
        // host -> valores corrompidos.  Bug bug4-extension.
        // Para arrays dinamicos (`T[]`, array_size==0) el field guarda
        // un host_ptr (de `new T[N]` que usa RAW_ALLOC).  El default de
        // @c Type::make_array es is_virtual=true pero ese flag aplica a
        // arrays SIZED en stack; los dinamicos son siempre host.
        if (ftyp.kind == PrimitiveKind::ARRAY && ftyp.array_size == 0) {
            fn_->values[dst].is_host_ptr = true;
        }
        // fix - field de tipo CLASS guarda un GcHandle (estable a
        // evacuacion del GC).  Tras LOADear el handle, hacemos @c gcderef
        // para obtener el host_ptr actual del objeto (refrescado tras
        // cualquier movimiento del GC).  Sin esta refresh, el ptr leido
        // del campo seria stale si el objeto migro a OldGen entre el store
        // y este load -> segfault al hacer @c callvirt o leer fields.
        if (ftyp.kind == PrimitiveKind::CLASS) {
            // raw_asm-elim 2026-05-28: gcderef + xchg -> IrOp::GC_DEREF_HOST.
            ir::IrValueId v_host             = fn_->new_value(ir::IrType::I64);
            fn_->values[v_host].is_host_ptr  = true;
            fn_->values[v_host].is_gc_object = true;
            ir::IrInstr deref{};
            deref.op          = ir::IrOp::GC_DEREF_HOST;
            deref.type        = ir::IrType::PTR;
            deref.dst         = v_host;
            deref.operands    = {dst};
            deref.source_line = e->loc.line;
            fn_->append(current_block_, std::move(deref));
            return v_host;
        }
        return dst;
    }

    ir::IrValueId Lowering::lower_class_field_store(ast::FieldAccessExpr *target,
                                                    ir::IrValueId         rhs,
                                                    const SourceLoc &     loc) {
        if (!target || rhs == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        // Limitacion G (cerrada): @c property_kind == 3 marca asignacion a
        // static field via @c ClassName.field = v.  Mismo patron que
        // lower_class_field_load: findclass inline + setstatic con offset
        // compile-time.  El base es IdentExpr (nombre de clase), no
        // referenciable como SSA value; resolvemos directo del layout.
        if (target->property_kind == 3) {
            if (!target->base
                || target->base->kind != ast::NodeKind::IdentExpr) {
                error_at(loc, "lowering: static field store con base no-ClassName");
                return ir::IR_NO_VALUE;
            }
            auto *base_id = static_cast<ast::IdentExpr *>(target->base.get());
            auto  it_cls  = tc_.class_layouts().find(base_id->name);
            if (it_cls == tc_.class_layouts().end()) {
                error_at(loc,
                         "lowering: clase desconocida '" + base_id->name + "'");
                return ir::IR_NO_VALUE;
            }
            const ClassLayout &lay_s = it_cls->second;
            uint32_t           s_off = 0;
            Type               s_typ = Type{PrimitiveKind::COUNT};
            bool               s_ok  = false;
            for (const auto &f: lay_s.static_fields) {
                if (f.name == target->field_name) {
                    s_off = f.offset;
                    s_typ = f.type;
                    s_ok  = true;
                    break;
                }
            }
            if (!s_ok) {
                error_at(loc,
                         "lowering: static field '" + target->field_name +
                         "' no encontrado en la clase '" + base_id->name + "'");
                return ir::IR_NO_VALUE;
            }
            // Coerce rhs al tipo del field si difieren.
            const ir::IrType    field_ir = ir_type_from_primitive(s_typ.kind);
            const ir::IrValueId rhs_cast = cast_if_needed(rhs, fn_->values[rhs].type,
                                                          field_ir, loc.line);
            // 1) Sprint 5: findclass via IR ops.
            const uint64_t cname_idx = intern_class_name(*out_mod_, base_id->name);
            const uint32_t cname_len = static_cast<uint32_t>(base_id->name.size());
            const ir::IrValueId v_cls = emit_findclass_by_name(cname_idx, cname_len, loc.line);
            // 2) setstatic.  Coerce rhs_cast a I64 si fuera necesario.
            ir::IrValueId v_val_i64 = rhs_cast;
            if (fn_->values[rhs_cast].type != ir::IrType::I64) {
                v_val_i64 = cast_if_needed(rhs_cast, fn_->values[rhs_cast].type,
                                            ir::IrType::I64, loc.line,
                                            /*is_explicit=*/true);
            }
            emit_setstatic(v_cls, v_val_i64, static_cast<uint64_t>(s_off), loc.line);
            return rhs_cast;
        }

        const Type bt = target->base->result_type;
        if (bt.kind != PrimitiveKind::CLASS) {
            error_at(loc, "lowering: '.' sobre tipo no-clase en lower_class_field_store");
            return ir::IR_NO_VALUE;
        }
        auto it = tc_.class_layouts().find(bt.struct_name);
        if (it == tc_.class_layouts().end()) {
            error_at(loc, "lowering: clase desconocida '" + bt.struct_name + "'");
            return ir::IR_NO_VALUE;
        }
        const ClassLayout &lay = it->second;
        // si el type checker marco el target como setter de
        // propiedad, emitir CALLVIRT al setter `set_<field_name>` en vez
        // de setfield.  El rhs se pasa como argumento del setter.
        if (target->property_kind == 2) {
            const std::string      setter_name = std::string("set_") + target->field_name;
            const ClassMethodInfo *mtd         = nullptr;
            for (const auto &m: lay.methods) {
                if (!m.is_constructor && m.name == setter_name) {
                    mtd = &m;
                    break;
                }
            }
            if (!mtd) {
                error_at(loc,
                         "lowering: setter de propiedad '" + target->field_name +
                         "' no encontrado en la clase '" + bt.struct_name + "'");
                return ir::IR_NO_VALUE;
            }
            const ir::IrValueId obj = lower_expr(target->base.get());
            if (obj == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            const ir::IrType param_ir = mtd->param_types.empty()
                                            ? ir::IrType::I64
                                            : ir_type_from_primitive(mtd->param_types.front().kind);
            const ir::IrValueId rhs_cast = cast_if_needed(rhs, fn_->values[rhs].type, param_ir, loc.line);
            ir::IrInstr         ins{};
            ins.op   = ir::IrOp::CALLVIRT;
            ins.type = ir::IrType::VOID;
            ins.dst  = ir::IR_NO_VALUE;
            ins.operands.push_back(obj);
            ins.operands.push_back(rhs_cast);
            ins.imm         = static_cast<uint64_t>(mtd->vtable_index);
            ins.source_line = loc.line;
            fn_->append(current_block_, std::move(ins));
            return rhs_cast;
        }
        uint32_t off  = 0;
        bool     ok   = false;
        Type     ftyp = Type{PrimitiveKind::COUNT};
        for (const auto &f: lay.fields) {
            if (f.name == target->field_name) {
                off  = f.offset;
                ftyp = f.type;
                ok   = true;
                break;
            }
        }
        if (!ok) {
            error_at(loc,
                     "lowering: campo '" + target->field_name +
                     "' no encontrado en la clase '" + bt.struct_name + "'");
            return ir::IR_NO_VALUE;
        }
        const ir::IrValueId obj = lower_expr(target->base.get());
        if (obj == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        const ir::IrValueId addr = emit_field_addr(fn_, current_block_, obj, off,
                                                   loc.line);
        const ir::IrType    ir_t     = ir_type_from_primitive(ftyp.kind);
        const ir::IrValueId rhs_cast = cast_if_needed(rhs, fn_->values[rhs].type, ir_t, loc.line);
        // Si el campo es CLASS, almacenamos el GcHandle (estable a evacuacion
        // del GC) en vez del host_ptr crudo.  Sin esto, una alocacion entre
        // el store y el siguiente load podria mover el objeto y dejar el ptr
        // guardado apuntando a memoria liberada/reusada -> segfault al
        // hacer @c callvirt sobre `this.field`.
        ir::IrValueId v_to_store = rhs_cast;
        if (ftyp.kind == PrimitiveKind::CLASS) {
            v_to_store = emit_gc_handle_for_ptr(rhs_cast, loc.line);
        }
        ir::IrInstr st{};
        st.op          = ir::IrOp::STORE;
        st.type        = ir_t;
        st.dst         = ir::IR_NO_VALUE;
        st.operands    = {v_to_store, addr};
        st.source_line = loc.line;
        fn_->append(current_block_, std::move(st));
        return rhs_cast;
    }

    ir::IrValueId Lowering::lower_class_method_call(ast::CallExpr *e) {
        // El callee es FieldAccessExpr cuyo base es de tipo CLASS.  Bajamos
        // el receptor, localizamos el vtable_index del metodo y emitimos
        // CALLVIRT.  El IR emitter coloca obj en r1 y args en r2..r_{N+1}
        // antes de la instruccion bytecode 'callvirt r1, vtable_idx'.
        if (!e->callee || e->callee->kind != ast::NodeKind::FieldAccessExpr) {
            error_at(e->loc, "lowering: callee no es field-access en class method call");
            return ir::IR_NO_VALUE;
        }
        auto *     fa = static_cast<ast::FieldAccessExpr *>(e->callee.get());

        // Bug fix 2026-05-23: metodos estaticos.  property_kind=4 marca una
        // llamada estatica `ClassName.method(args)`.  Emitimos CALLVM directo
        // a `<Class>__<method>` sin pasar this como primer arg.
        if (fa->property_kind == 4) {
            std::string class_name;
            if (fa->base && fa->base->kind == ast::NodeKind::IdentExpr) {
                class_name = static_cast<ast::IdentExpr *>(fa->base.get())->name;
            }
            if (class_name.empty()) {
                error_at(e->loc, "lowering: nombre de clase vacio en llamada estatica");
                return ir::IR_NO_VALUE;
            }
            auto it_cls = tc_.class_layouts().find(class_name);
            if (it_cls == tc_.class_layouts().end()) {
                error_at(e->loc, "lowering: clase '" + class_name + "' no encontrada");
                return ir::IR_NO_VALUE;
            }
            const ClassMethodInfo *static_mtd = nullptr;
            for (const auto &m : it_cls->second.methods) {
                if (m.is_constructor) continue;
                if (m.is_static && m.name == fa->field_name) { static_mtd = &m; break; }
            }
            if (!static_mtd) {
                error_at(e->loc,
                    "lowering: metodo estatico '" + class_name + "." +
                    fa->field_name + "' no encontrado");
                return ir::IR_NO_VALUE;
            }
            // Bajar args.
            std::vector<ir::IrValueId> arg_vals;
            arg_vals.reserve(e->args.size());
            for (size_t ai = 0; ai < e->args.size(); ++ai) {
                auto &a = e->args[ai];
                if (!a) return ir::IR_NO_VALUE;
                // Auto-promotion para args string literales.
                const bool param_is_string =
                    ai < static_mtd->param_types.size()
                    && static_mtd->param_types[ai].kind == PrimitiveKind::STRING;
                if (param_is_string
                    && a->kind == ast::NodeKind::StringLitExpr) {
                    auto *slit = static_cast<ast::StringLitExpr *>(a.get());
                    arg_vals.push_back(lower_string_literal_to_string_object(slit));
                } else {
                    const ir::IrValueId av = lower_expr(a.get());
                    if (av == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
                    arg_vals.push_back(av);
                }
            }
            const ir::IrType ret_ir = ir_type_from_primitive(static_mtd->return_type.kind);
            ir::IrValueId dst = (ret_ir == ir::IrType::VOID)
                ? ir::IR_NO_VALUE
                : fn_->new_value(ret_ir);
            ir::IrInstr ins{};
            ins.op   = ir::IrOp::CALL;
            ins.type = ret_ir;
            ins.dst  = dst;
            ins.func_name = class_name + "__" + fa->field_name;
            ins.operands  = arg_vals;
            ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ins));
            return dst;
        }

        const Type bt = fa->base->result_type;
        if (bt.kind != PrimitiveKind::CLASS) {
            error_at(e->loc, "lowering: receptor no es CLASS en method call");
            return ir::IR_NO_VALUE;
        }
        auto it = tc_.class_layouts().find(bt.struct_name);
        if (it == tc_.class_layouts().end()) {
            error_at(e->loc, "lowering: clase desconocida '" + bt.struct_name + "'");
            return ir::IR_NO_VALUE;
        }
        const ClassLayout &    lay = it->second;
        const ClassMethodInfo *mtd = nullptr;
        for (const auto &m: lay.methods) {
            if (!m.is_constructor && m.name == fa->field_name) {
                mtd = &m;
                break;
            }
        }
        if (!mtd) {
            error_at(e->loc,
                     "lowering: metodo '" + fa->field_name +
                     "' no encontrado en la clase '" + bt.struct_name + "'");
            return ir::IR_NO_VALUE;
        }
        // Bajar receptor y argumentos.
        const ir::IrValueId obj = lower_expr(fa->base.get());
        if (obj == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        std::vector<ir::IrValueId> arg_vals;
        arg_vals.reserve(e->args.size());
        for (size_t ai = 0; ai < e->args.size(); ++ai) {
            auto &a = e->args[ai];
            if (!a) return ir::IR_NO_VALUE;
            // Auto-promocion literal -> StringObject cuando el parametro
            // espera STRING y el arg es un StringLit no interpolado.
            // Mismo patron que @c lower_call usa para funciones libres:
            // sin esto, pasar @c helper("hola") a @c void helper(string s)
            // pushearia la direccion cruda del literal como i64 (PTR) y
            // el callee crashearia al hacer @c strraw s con ptr invalido.
            // Sin esto, str_cstr/str_bytes dentro del metodo trataban el
            // PTR del literal como GcHandle invalido y leian garbage.
            const bool param_is_string =
                ai < mtd->param_types.size()
                && mtd->param_types[ai].kind == PrimitiveKind::STRING;
            if (param_is_string
                && a->kind == ast::NodeKind::StringLitExpr) {
                auto *slit = static_cast<ast::StringLitExpr *>(a.get());
                // Tanto literales puros como interpolados: el helper
                // construye el StringObject correcto.
                const ir::IrValueId av =
                    lower_string_literal_to_string_object(slit);
                if (av == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
                arg_vals.push_back(av);
                continue;
            }
            const ir::IrValueId av = lower_expr(a.get());
            if (av == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            arg_vals.push_back(av);
        }
        const ir::IrType ret_ir_decl = ir_type_from_primitive(mtd->return_type.kind);

        // SRET en class method: si el metodo declara devolver Optional/Result,
        // su firma IR real es void + retbuf hidden como segundo param (tras
        // this).  El caller alloca el buffer (16 / 24 bytes) y lo pasa como
        // primer "arg" del CALLVIRT (entre obj y los args declarados).  El
        // dst del CALLVIRT es VOID; el SSA value visible al lowering es el
        // retbuf (PTR), que se bindea a la var-decl o se pasa a otras fns.
        const bool method_call_sret = (mtd->return_type.kind == PrimitiveKind::OPTIONAL
                                    || mtd->return_type.kind == PrimitiveKind::RESULT);
        ir::IrValueId v_method_call_retbuf = ir::IR_NO_VALUE;
        if (method_call_sret) {
            uint64_t buf_bytes = (mtd->return_type.kind == PrimitiveKind::RESULT)
                                     ? 24ULL : 16ULL;
            v_method_call_retbuf = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8;
            al.imm         = buf_bytes;
            al.dst         = v_method_call_retbuf;
            al.source_line = e->loc.line;
            // BugFix sret-cross-mem (2026-06-04): forzar host_alloca para
            // el retbuf de metodos Optional/Result.  Asi el callee escribe
            // con `movh` y el caller lee con `movh` consistentemente.
            al.set_host_alloca(true);
            fn_->append(current_block_, std::move(al));
            fn_->values[v_method_call_retbuf].is_host_ptr = true;
        }
        const ir::IrType ret_ir = method_call_sret ? ir::IrType::VOID : ret_ir_decl;

        // si el metodo esta marcado @Inline y el receptor NO es
        // interfaz (necesitamos la implementacion concreta), buscar el
        // AST ClassMethodDecl y expandir el cuerpo en el call site.
        // MVP: solo metodos cuyo body sea exactamente `{ return expr; }`.
        if (mtd->is_inline && !lay.is_interface) {
            const ast::ClassDecl *cd_orig = nullptr;
            for (auto &d: mod_.decls) {
                if (!d || d->kind != ast::NodeKind::ClassDecl) continue;
                auto *cdp = static_cast<const ast::ClassDecl *>(d.get());
                if (cdp->name == mtd->defining_class) {
                    cd_orig = cdp;
                    break;
                }
            }
            if (cd_orig) {
                const ast::ClassMethodDecl *mdecl = nullptr;
                for (const auto &um: cd_orig->methods) {
                    if (um && um->name == mtd->name && !um->is_constructor) {
                        mdecl = um.get();
                        break;
                    }
                }
                if (mdecl
                    && mdecl->body
                    && mdecl->body->body.size() == 1
                    && mdecl->body->body[0]
                    && mdecl->body->body[0]->kind == ast::NodeKind::ReturnStmt) {
                    auto *rs = static_cast<ast::ReturnStmt *>(mdecl->body->body[0].get());
                    if (rs->value) {
                        // Push scope con bindings: this -> obj, params -> args.
                        push_scope();
                        bind("this", obj);
                        const size_t np = std::min(mdecl->params.size(), arg_vals.size());
                        for (size_t i = 0; i < np; ++i) {
                            bind(mdecl->params[i]->name, arg_vals[i]);
                        }
                        const ir::IrValueId v = lower_expr(rs->value.get());
                        pop_scope();
                        return v;
                    }
                }
                // Si no se cumple la forma esperada, caer al CALLVIRT.
            }
        }

        const ir::IrValueId dst = (ret_ir == ir::IrType::VOID)
                                      ? ir::IR_NO_VALUE
                                      : fn_->new_value(ret_ir);
        // Sprint edge-bugs (2026-06-03): marcar dst con is_host_ptr/
        // is_gc_object cuando el metodo retorna CLASS/PTR.  Critico
        // para que el regalloc preserve el value a traves de calls
        // GC posteriores (save/restore con conversion a GcHandle).
        if (dst != ir::IR_NO_VALUE) {
            const PrimitiveKind rk = mtd->return_type.kind;
            if (rk == PrimitiveKind::CLASS) {
                fn_->values[dst].is_host_ptr  = true;
                fn_->values[dst].is_gc_object = true;
            } else if ((rk == PrimitiveKind::PTR
                     || rk == PrimitiveKind::ARRAY)
                    && !mtd->return_type.is_virtual) {
                fn_->values[dst].is_host_ptr = true;
            }
        }
        // El valor SSA "visible" al lowering tras el CALLVIRT.  Para SRET
        // es el retbuf (PTR); para calls normales es dst.
        const ir::IrValueId visible_dst = method_call_sret
                                              ? v_method_call_retbuf
                                              : dst;

        // -----------------------------------------------------------------
        // Devirtualizacion compile-time: si el tipo concreto del receptor
        // es estaticamente conocido (via @c ssa_concrete_class_), reescribir
        // el dispatch a CALLVIRT directo usando el vtable_idx del metodo en
        // la clase concreta.  Tanto port C como JIT consumen este CALLVIRT
        // como direct call, sin coste runtime de findmethod/callm.
        //
        // Aplica cuando el receptor es una variable interface tipada pero
        // su SSA value viene directamente de un @c new ConcreteClass()
        // (caso comun: @c IServicio s = new ImplA();).
        // -----------------------------------------------------------------
        if (lay.is_interface) {
            auto it_conc = ssa_concrete_class_.find(obj);
            if (it_conc != ssa_concrete_class_.end()) {
                const std::string &concrete_name = it_conc->second;
                auto it_lay = tc_.class_layouts().find(concrete_name);
                if (it_lay != tc_.class_layouts().end()) {
                    const auto &conc_lay = it_lay->second;
                    // Buscar metodo por nombre en la clase concreta.
                    for (const auto &cm: conc_lay.methods) {
                        if (cm.name == mtd->name && !cm.is_constructor) {
                            // CALLVIRT directo con el vtable_idx de la
                            // clase concreta -> backend devirtaliza.
                            ir::IrInstr cv{};
                            cv.op   = ir::IrOp::CALLVIRT;
                            cv.type = ret_ir;
                            cv.dst  = dst;
                            cv.imm  = static_cast<uint64_t>(cm.vtable_index);
                            cv.operands.push_back(obj);
                            // SRET: retbuf tras obj, antes de args.
                            if (method_call_sret)
                                cv.operands.push_back(v_method_call_retbuf);
                            for (auto av: arg_vals) cv.operands.push_back(av);
                            cv.source_line = e->loc.line;
                            fn_->append(current_block_, std::move(cv));
                            // Propagar tipo concreto del retorno si es
                            // tambien tipo class conocido.
                            return visible_dst;
                        }
                    }
                }
            }
        }
        if (lay.is_interface) {
            // Marcar obj como host_ptr (instancia GC-derivada).
            fn_->values[obj].is_host_ptr = true;

            // Dispatch de interfaz via ITABLE (en vez de findmethod+callm):
            // construimos un @c ItfCallParams (32 bytes) en stack con el
            // nombre de la interfaz, el nombre del metodo, el indice del metodo
            // (posicion en la declaracion de la interfaz = vtable_index) y el
            // numero de metodos de la interfaz, y emitimos un solo CALLITF.
            //   +0  iface_name_addr (8)
            //   +8  iface_name_len (lo32) | method_index (hi32)
            //   +16 method_name_addr (8)
            //   +24 method_name_len (lo32) | count (hi32)
            // El interp despacha via la itable lazy de la clase concreta
            // (indice O(1) tras el warmup); el JIT inlinea el scan de itables.
            const std::string &iface_name = bt.struct_name; // == lay.name
            const std::string &method_name = mtd->name;
            const uint64_t iface_idx  = intern_class_name(*out_mod_, iface_name);
            const uint32_t iface_len  = static_cast<uint32_t>(iface_name.size());
            const uint64_t method_idx_str = intern_class_name(*out_mod_, method_name);
            const uint32_t method_len = static_cast<uint32_t>(method_name.size());
            const uint32_t method_index = mtd->vtable_index;
            const uint32_t mcount       = static_cast<uint32_t>(lay.methods.size());

            // Buffer de ItfCallParams (32 bytes) construido UNA VEZ en el entry
            // block (block 0) de la funcion -- su contenido es 100%
            // loop-invariante (nombres + indices constantes).  El @c callitf en
            // el loop solo LEE el buffer.  Critico para correctness:
            //   (1) un ALLOCA por dispatch creceria el VM stack en loops
            //       calientes (pic_real 3M iter desbordaba el stack);
            //   (2) construir el struct DENTRO del loop hacia que el LICM/
            //       regalloc hoisteara un CONST a un reg de arg (r1) que el
            //       marshalling del call clobbeaba -> v_buf corrupto en iter 2+.
            // Construir en el entry (dominador de todo) evita ambos.  Cada call
            // site tiene su propio buffer (params distintos); como el call site
            // se baja UNA vez, son pocos buffers (1 por dispatch textual).
            //
            // Insertamos las instrucciones de construccion en block 0 ANTES de
            // su terminador (o al final si aun no esta terminado).
            std::vector<ir::IrInstr> setup;
            const ir::IrValueId v_buf = fn_->new_value(ir::IrType::PTR);
            {
                ir::IrInstr al{};
                al.op = ir::IrOp::ALLOCA; al.type = ir::IrType::I8; al.imm = 32;
                al.dst = v_buf; al.source_line = e->loc.line;
                setup.push_back(std::move(al));
            }
            // Helper local: STORE i64 @c val a @c v_buf + @c off (instrs -> setup).
            auto setup_store_at = [&](uint64_t off, ir::IrValueId val) {
                ir::IrValueId base = v_buf;
                if (off != 0) {
                    const ir::IrValueId v_off = fn_->new_value(ir::IrType::I64);
                    ir::IrInstr c{};
                    c.op = ir::IrOp::CONST; c.type = ir::IrType::I64; c.imm = off;
                    c.dst = v_off; c.source_line = e->loc.line;
                    setup.push_back(std::move(c));
                    base = fn_->new_value(ir::IrType::PTR);
                    ir::IrInstr add{};
                    add.op = ir::IrOp::ADD; add.type = ir::IrType::I64; add.dst = base;
                    add.operands = {v_buf, v_off}; add.source_line = e->loc.line;
                    setup.push_back(std::move(add));
                }
                ir::IrInstr st{};
                st.op = ir::IrOp::STORE; st.type = ir::IrType::I64;
                st.dst = ir::IR_NO_VALUE; st.operands = {val, base};
                st.source_line = e->loc.line;
                setup.push_back(std::move(st));
            };
            auto setup_const = [&](uint64_t k) -> ir::IrValueId {
                const ir::IrValueId v = fn_->new_value(ir::IrType::I64);
                ir::IrInstr c{};
                c.op = ir::IrOp::CONST; c.type = ir::IrType::I64; c.imm = k;
                c.dst = v; c.source_line = e->loc.line;
                setup.push_back(std::move(c));
                return v;
            };
            auto setup_str_lit = [&](uint64_t idx) -> ir::IrValueId {
                const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
                ir::IrInstr ns{};
                ns.op = ir::IrOp::STR_LIT_ADDR; ns.type = ir::IrType::PTR;
                ns.dst = v; ns.imm = idx; ns.source_line = e->loc.line;
                setup.push_back(std::move(ns));
                return v;
            };
            // [+0] iface_name_addr ; [+8] iface_len|method_index<<32 ;
            // [+16] method_name_addr ; [+24] method_len|count<<32.
            setup_store_at(0, setup_str_lit(iface_idx));
            setup_store_at(8, setup_const(static_cast<uint64_t>(iface_len)
                               | (static_cast<uint64_t>(method_index) << 32)));
            setup_store_at(16, setup_str_lit(method_idx_str));
            setup_store_at(24, setup_const(static_cast<uint64_t>(method_len)
                               | (static_cast<uint64_t>(mcount) << 32)));
            // ----------------------------------------------------------------
            // (C2): preparar devirt especulativa estatica del CALLITF.
            // Si la interfaz tiene pocos (<=K) implementors concretos NO-aspecto
            // con el metodo inlineable, resolvemos el ClassInfo* de cada uno
            // (findclass en el entry, loop-invariante) y registramos los
            // candidatos; el pase ir_pass_spec_devirt reescribe el CALLITF en
            // un guard-chain por tipo + fallback al dispatch via itable.
            //  - Solo metodos que devuelven valor (dst != IR_NO_VALUE) y
            //    no-SRET (v1).
            //  - Conservador con AOP: si el modulo declara cualquier @Aspect,
            //    NO especulamos (un advice podria targetear el metodo y el CALL
            //    directo del fast path lo saltaria); el dispatch via itable
            //    recorre la advice_chain correctamente.
            // Resolucion v1 (2a): findclass directo en el entry, 1x/invocacion
            // (despreciable para dispatch-en-loop).  2b lo cambiara a slot-cache
            // eager en __module_init (1x total).
            std::vector<ir::DevirtCandidate> spec_cands;
            if (dst != ir::IR_NO_VALUE && !method_call_sret) {
                bool module_has_aspect = false;
                for (const auto &kv : tc_.class_layouts()) {
                    if (kv.second.is_aspect) { module_has_aspect = true; break; }
                }
                if (!module_has_aspect) {
                    constexpr size_t K_MAX = 4;
                    // (cls_name, callee_ir_name) por implementor concreto.
                    std::vector<std::pair<std::string, std::string>> impls;
                    bool too_many = false;
                    for (const auto &kv : tc_.class_layouts()) {
                        const auto &cl = kv.second;
                        if (cl.is_interface || cl.is_aspect) continue;
                        bool implements = false;
                        for (const auto &in : cl.interface_names)
                            if (in == iface_name) { implements = true; break; }
                        if (!implements) continue;
                        // Localizar el metodo de la interfaz en la clase.
                        const std::string *owner = nullptr;
                        for (const auto &mm : cl.methods) {
                            if (mm.name == method_name && !mm.is_constructor) {
                                owner = mm.defining_class.empty()
                                            ? &cl.name : &mm.defining_class;
                                break;
                            }
                        }
                        if (!owner) continue;  // no deberia pasar si implements
                        impls.emplace_back(cl.name, *owner + "__" + method_name);
                        if (impls.size() > K_MAX) { too_many = true; break; }
                    }
                    if (!too_many && !impls.empty() && impls.size() <= K_MAX) {
                        const int ln = e->loc.line;
                        // Resolver cada ClassInfo* via findclass construido en el
                        // vector `setup` (que se splice en block 0, el entry).
                        auto setup_findclass = [&](const std::string &cls_name)
                                                   -> ir::IrValueId {
                            const uint64_t nidx = intern_class_name(*out_mod_, cls_name);
                            const uint32_t nlen = static_cast<uint32_t>(cls_name.size());
                            const ir::IrValueId vp = fn_->new_value(ir::IrType::PTR);
                            { ir::IrInstr al{}; al.op = ir::IrOp::ALLOCA;
                              al.type = ir::IrType::I8; al.dst = vp; al.imm = 16;
                              al.source_line = ln; setup.push_back(std::move(al)); }
                            const ir::IrValueId vna = fn_->new_value(ir::IrType::PTR);
                            { ir::IrInstr la{}; la.op = ir::IrOp::LABEL_ADDR;
                              la.type = ir::IrType::PTR; la.dst = vna;
                              la.func_name = "s_" + std::to_string(nidx);
                              la.source_line = ln; setup.push_back(std::move(la)); }
                            { ir::IrInstr st{}; st.op = ir::IrOp::STORE;
                              st.type = ir::IrType::I64; st.operands = {vna, vp};
                              st.source_line = ln; setup.push_back(std::move(st)); }
                            const ir::IrValueId vlen = setup_const(static_cast<uint64_t>(nlen));
                            const ir::IrValueId voff = setup_const(8);
                            const ir::IrValueId vp8  = fn_->new_value(ir::IrType::PTR);
                            { ir::IrInstr add{}; add.op = ir::IrOp::ADD;
                              add.type = ir::IrType::I64; add.dst = vp8;
                              add.operands = {vp, voff}; add.source_line = ln;
                              setup.push_back(std::move(add)); }
                            { ir::IrInstr st{}; st.op = ir::IrOp::STORE;
                              st.type = ir::IrType::I64; st.operands = {vlen, vp8};
                              st.source_line = ln; setup.push_back(std::move(st)); }
                            const ir::IrValueId vc = fn_->new_value(ir::IrType::PTR);
                            fn_->values[vc].is_host_ptr = true;
                            { ir::IrInstr fc{}; fc.op = ir::IrOp::FINDCLASS;
                              fc.type = ir::IrType::PTR; fc.dst = vc; fc.operands = {vp};
                              fc.set_is_call_site(true); fc.source_line = ln;
                              setup.push_back(std::move(fc)); }
                            return vc;
                        };
                        for (const auto &pr : impls) {
                            const ir::IrValueId v_cls = setup_findclass(pr.first);
                            spec_cands.push_back(ir::DevirtCandidate{ v_cls, pr.second });
                        }
                    }
                }
            }

            // Splice de las instrucciones de construccion en block 0 antes de
            // su terminador (op de control de flujo final).  Si block 0 no esta
            // terminado (dispatch en el propio entry), se anexan al final.
            {
                auto &e0 = fn_->blocks[0].instrs;
                size_t pos = e0.size();
                if (pos > 0) {
                    const ir::IrOp last = e0.back().op;
                    if (last == ir::IrOp::BR || last == ir::IrOp::BR_COND
                        || last == ir::IrOp::RET || last == ir::IrOp::UNREACHABLE
                        || last == ir::IrOp::TAILCALL || last == ir::IrOp::RETHROW
                        || last == ir::IrOp::THROW) {
                        pos = e0.size() - 1;
                    }
                }
                e0.insert(e0.begin() + pos,
                          std::make_move_iterator(setup.begin()),
                          std::make_move_iterator(setup.end()));
            }

            // CALLITF: operands[0]=obj, [1]=params_ptr, [2..]=args (retbuf SRET
            // como [2] si aplica).  func_name = "iface\x1fmethod", imm packed.
            ir::IrInstr ci{};
            ci.op   = ir::IrOp::CALLITF;
            ci.type = ret_ir;
            ci.dst  = dst;
            ci.func_name = iface_name;
            ci.func_name.push_back('\x1f');
            ci.func_name += method_name;
            ci.imm  = (static_cast<uint64_t>(mcount) << 32)
                    | static_cast<uint64_t>(method_index);
            ci.operands.push_back(obj);
            ci.operands.push_back(v_buf);
            if (method_call_sret)
                ci.operands.push_back(v_method_call_retbuf);
            for (auto av: arg_vals) ci.operands.push_back(av);
            ci.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ci));
            // registrar el site especulativo (keyed por el dst del
            // CALLITF).  El pase ir_pass_spec_devirt lo consume @O2.
            if (!spec_cands.empty())
                fn_->spec_devirt_sites[dst] = std::move(spec_cands);
            return visible_dst;
        }

        // Path por defecto: dispatch via vtable_idx (clase concreta).
        ir::IrInstr ins{};
        ins.op   = ir::IrOp::CALLVIRT;
        ins.type = ret_ir;
        ins.dst  = dst;
        // operands[0] = obj, operands[1..] = args declarados
        ins.operands.push_back(obj);
        // SRET: retbuf tras obj, antes de args declarados.
        if (method_call_sret)
            ins.operands.push_back(v_method_call_retbuf);
        for (auto av: arg_vals) ins.operands.push_back(av);
        ins.imm         = static_cast<uint64_t>(mtd->vtable_index);
        ins.source_line = e->loc.line;
        fn_->append(current_block_, std::move(ins));
        // Sprint edge-bugs (2026-06-03): marcar dst con flags GC.  Critico
        // para que el regalloc trate el value como host_ptr GC-managed
        // (save/restore convierte a GcHandle).  Esto es la correccion
        // arquitectural; queda un bug latente del INTERP en CALLVIRT
        // chained (p2 = factory(); p3 = p2.factory(); p3.field) donde el
        // host_ptr retornado puede ser stale -- el bug NO afecta JIT.
        // Documentado en limitaciones; los tests del Lombok @With usan
        // verificacion sin encadenar para evitar el bug latente.
        if (dst != ir::IR_NO_VALUE
            && mtd->return_type.kind == PrimitiveKind::CLASS) {
            fn_->values[dst].is_host_ptr  = true;
            fn_->values[dst].is_gc_object = true;
        }
        return visible_dst;
    }

    // ---------------------------------------------------------------------
    // Helpers de constantes y casts.
    // ---------------------------------------------------------------------

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
