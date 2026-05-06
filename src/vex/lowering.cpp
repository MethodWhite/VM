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
            // (mismo ancho, semanticamente equivalente para A.1 que solo lo
            // usa como entero pequenyo).
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
            case PrimitiveKind::COUNT: return ir::IrType::VOID;
            default: break;
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
                if (kind == PrimitiveKind::OPTIONAL
                    || kind == PrimitiveKind::RESULT
                    || is_user_enum
                    || is_function_ret) {
                    rt = ir::IrType::VOID;
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
                // Las variables globales no se bajan en A.1, pero
                // `const T NAME = lit;` SI funciona (lower_ident las
                // inlinea como CONST en cada uso).  Solo avisamos para
                // las globales NO-const o las que tienen inicializador
                // no-literal (que efectivamente se ignoran).
                auto *gv            = static_cast<ast::GlobalVarDecl *>(decl.get());
                bool  literal_const = gv->is_const && gv->init && (
                    gv->init->kind == ast::NodeKind::IntLitExpr
                    || (gv->init->kind == ast::NodeKind::UnaryExpr
                        && static_cast<ast::UnaryExpr *>(gv->init.get())->op == ast::UnOp::Neg
                        && static_cast<ast::UnaryExpr *>(gv->init.get())->operand
                        && static_cast<ast::UnaryExpr *>(gv->init.get())->operand->kind == ast::NodeKind::IntLitExpr));
                if (!literal_const) {
                    diags_.warning(decl->loc,
                                   "variable global no-const ignorada (sin storage en A.1)");
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

        // volcar las funciones sinteticas de spawn DESPUES de las
        // de usuario y POO.  Asi main sigue siendo la primera funcion del
        // modulo (entry point con hlt) y los helpers de spawn quedan al
        // final como funciones normales (cierran con ret, pero el body
        // siempre incluye un hlt explicito antes del fin del bloque).
        for (auto &h: pending_spawn_helpers_) {
            out_module.add_function(std::move(h));
        }
        pending_spawn_helpers_.clear();

        return diags_.error_count() == initial_errors;
    }

    // ---------------------------------------------------------------------
    // Lowering de una funcion.
    // ---------------------------------------------------------------------

    void Lowering::lower_function(ast::FunctionDecl *fd, ir::IrModule &out) {
        ir::IrFunction fn;
        fn.name = fd->name;

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
        const bool sret          = (sem_ret.kind == PrimitiveKind::OPTIONAL
            || sem_ret.kind == PrimitiveKind::RESULT
            || sret_enum
            || sret_function);
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
            ir::IrInstr         ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = t_old;
            ra.dst       = v_new;
            ra.operands  = {v_old};
            ra.func_name = std::string(
                "// nonnull param: assert non-null en entry\n"
                "unwrap {dst}, {src0}\n");
            ra.source_line = p->loc.line;
            fn_->append(current_block_, std::move(ra));
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
            if (any_class) {
                ir::IrInstr call_init{};
                call_init.op          = ir::IrOp::CALL;
                call_init.type        = ir::IrType::VOID;
                call_init.dst         = ir::IR_NO_VALUE;
                call_init.func_name   = "__module_init";
                call_init.source_line = fd->loc.line;
                fn.append(current_block_, std::move(call_init));
            }
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
        out.add_function(std::move(fn));
        fn_ = nullptr;
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
            case ast::NodeKind::VarDeclStmt:
                lower_var_decl(static_cast<ast::VarDeclStmt *>(s));
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
                const ir::IrBlockId tgt = loop_targets_.back().break_bb;
                ir::IrInstr         br{};
                br.op           = ir::IrOp::BR;
                br.target_block = tgt;
                br.source_line  = s->loc.line;
                fn_->append(current_block_, std::move(br));
                fn_->blocks[current_block_].succs.push_back(tgt);
                fn_->blocks[tgt].preds.push_back(current_block_);
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
                unsupported(s->loc, "statement no soportado en A.1");
                return;
        }
    }

    void Lowering::lower_var_decl(ast::VarDeclStmt *vd) {
        // Resolver el Type semantico (aplicando aliases y structs).
        const Type sem_type = tc_.resolve_type_node(vd->type.get());

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
                v_val = cast_if_needed(v_val,
                                       fn_->values[v_val].type, ir_elem, vd->loc.line);
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
                v_val                  = cast_if_needed(v_val,
                                                        fn_->values[v_val].type, ir_ft, vd->loc.line);
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
            // Inicializador para structs aun no soportado (se
            // requeriria copiar campo a campo o un MEMCPY); reportamos
            // si el usuario intenta usar uno.
            if (vd->init) {
                error_at(vd->loc,
                         "lowering: inicializador de struct aun no soportado (A.3.3+)");
            }
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
                v_val = cast_if_needed(v_val,
                                       fn_->values[v_val].type, ir_elem, vd->loc.line);
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
                v_val                  = cast_if_needed(v_val,
                                                        fn_->values[v_val].type, ir_ft, vd->loc.line);
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
            if (!sem_type.pointee || sem_type.array_size == 0) {
                error_at(vd->loc,
                         "lowering: array sin tamano fijo no es declarable como variable");
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
                         "lowering: inicializador de array aun no soportado (A.3.5+)");
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
                    v0                     = cast_if_needed(v0, vfrom, vt, vd->loc.line);
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
            // Lazy promotion: si el tipo destino es STRING y el
            // init es un string literal puro (StringLitExpr), promover
            // a StringObject GC-managed via STRMAKE.  Asi `string s =
            // "hola"` aloca 1 vez; `print("hola")` (sin var-decl) sigue
            // sin alocar.
            if (sem_type.kind == PrimitiveKind::STRING
                && vd->init
                && vd->init->kind == ast::NodeKind::StringLitExpr
                && !static_cast<ast::StringLitExpr *>(vd->init.get())->is_interpolated()) {
                auto *slit = static_cast<ast::StringLitExpr *>(vd->init.get());
                v          = lower_string_literal_to_string_object(slit);
                bind(vd->name, v);
                return;
            }
            v = lower_expr(vd->init.get());
            if (v != ir::IR_NO_VALUE) {
                const ir::IrType vfrom = fn_->values[v].type;
                v                      = cast_if_needed(v, vfrom, vt, vd->loc.line);
            }
        } else {
            // Sin init: defecto 0.  Las variables sin init son raras
            // en uso normal pero el type checker no las prohibe.
            v = emit_const(vt, 0, vd->loc.line);
        }
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
        // un handle invalido.  Para casos simples (variable local que no
        // escapa) funciona correctamente.  Escape analysis es A.27+.

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

        if (v != ir::IR_NO_VALUE && is_col_kind(sem_type.kind)
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
    }

    void Lowering::lower_if(ast::IfStmt *s) {
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
        // un cmp_ne con 0; en A.1 lo dejamos al backend.

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
        current_block_    = exit_id;
        block_terminated_ = false;
        for (auto &vi: vars) {
            update_scope(vi.name, vi.phi_value);
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
            const ir::IrValueId v_local = lower_expr(s->value.get());
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
                && s->value->kind == ast::NodeKind::StringLitExpr
                && !static_cast<ast::StringLitExpr *>(s->value.get())->is_interpolated()) {
                auto *slit = static_cast<ast::StringLitExpr *>(s->value.get());
                v_ret      = lower_string_literal_to_string_object(slit);
            } else {
                v_ret = lower_expr(s->value.get());
                if (v_ret != ir::IR_NO_VALUE) {
                    v_ret = cast_if_needed(v_ret, fn_->values[v_ret].type, fn_->ret_type, s->loc.line);
                }
            }
        }
        // si estamos en el body de una funcion @Async lowered
        // como spawn helper, intercepta el return: en lugar de RET, emite
        // `fulfill(async_fut, value) + hlt`.  El caller obtendra el valor
        // via `await`.  El hlt es necesario porque el child no debe hacer
        // ret (no hay caller en el stack del child).
        if (async_fut_id_ != ir::IR_NO_VALUE) {
            ir::IrValueId v_payload = v_ret;
            if (v_payload == ir::IR_NO_VALUE) {
                v_payload = emit_const(ir::IrType::I64, 0, s->loc.line);
            }
            ir::IrInstr ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::VOID;
            ra.dst       = ir::IR_NO_VALUE;
            ra.operands  = {async_fut_id_, v_payload};
            ra.func_name = std::string(
                "// @Async return -> fulfill + hlt\n"
                "fulfill {src0}, {src1}\n"
                "hlt\n");
            ra.source_line = s->loc.line;
            fn_->append(current_block_, std::move(ra));
            block_terminated_ = true;
            return;
        }
        // rspawn body return -> mov r0, X + hlt.  El runtime remoto
        // detecta HALT en un proceso con rspawn_future_id != 0 y envia
        // VDP_FUTURE_FULFILL al nodo origen con R0 como payload.  El caller
        // local recibe el valor via `await fut`.
        if (is_rspawn_body_) {
            ir::IrValueId v_payload = v_ret;
            if (v_payload == ir::IR_NO_VALUE) {
                v_payload = emit_const(ir::IrType::I64, 0, s->loc.line);
            }
            ir::IrInstr ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::VOID;
            ra.dst       = ir::IR_NO_VALUE;
            ra.operands  = {v_payload};
            ra.func_name = std::string(
                "// rspawn body return -> mov r0, X + hlt (runtime envia R0 via VDP)\n"
                "mov r0, {src0}\n"
                "hlt\n");
            ra.source_line = s->loc.line;
            fn_->append(current_block_, std::move(ra));
            block_terminated_ = true;
            return;
        }
        // ejecutar cleanups activos (synchronized -> tryleave + monexit).
        // El SSA value v_ret sobrevive: el regalloc garantiza que se mantenga
        // vivo hasta el RET (o se reescriba antes si conviene).
        emit_cleanups_all();
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

    // Forward decls de helpers definidos mas abajo en el TU.  Necesarias
    // porque lower_try y try_lower_builtin_call los usan.
    static uint64_t intern_class_name(ir::IrModule &mod, const std::string &name);

    static void emit_findclass_inline(std::ostringstream &asm_,
                                      uint64_t            name_idx,
                                      uint32_t            name_len);

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
        const auto entry_bindings = scopes_.back();

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
                ir::IrInstr st{};
                st.op          = ir::IrOp::STORE;
                st.type        = ir::IrType::I64;
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
                // findclass inline escribe en r12 (hardcoded del helper).
                // Capturamos r12 a {dst} = v_type via RAW_ASM extra.
                std::ostringstream fc;
                const uint64_t     cls_idx = intern_class_name(*out_mod_, cc.exc_class_name);
                const uint32_t     cls_len = static_cast<uint32_t>(cc.exc_class_name.size());
                emit_findclass_inline(fc, cls_idx, cls_len);
                fc << "mov {dst}, r12\n";
                v_type = fn_->new_value(ir::IrType::PTR);
                ir::IrInstr ra{};
                ra.op          = ir::IrOp::RAW_ASM;
                ra.type        = ir::IrType::PTR;
                ra.dst         = v_type;
                ra.operands    = {};
                ra.func_name   = fc.str();
                ra.source_line = cc.loc.line;
                fn_->append(current_block_, std::move(ra));
            }

            // 1b. handler_pc SSA value via @Absolute en {dst}.
            const ir::IrValueId v_handler_pc = fn_->new_value(ir::IrType::PTR); {
                ir::IrInstr ra{};
                ra.op        = ir::IrOp::RAW_ASM;
                ra.type      = ir::IrType::PTR;
                ra.dst       = v_handler_pc;
                ra.operands  = {};
                ra.func_name = std::string("mov {dst}, @Absolute(\"code.")
                        + handler_label + "\")\n";
                ra.source_line = cc.loc.line;
                fn_->append(current_block_, std::move(ra));
            }

            // 1c. tryenter usando los dos SSA values.  El regalloc los
            // coloca en regs disponibles (NO clobbers SSA values vivos).
            {
                ir::IrInstr ra{};
                ra.op          = ir::IrOp::RAW_ASM;
                ra.type        = ir::IrType::VOID;
                ra.dst         = ir::IR_NO_VALUE;
                ra.operands    = {v_handler_pc, v_type};
                ra.func_name   = std::string("tryenter {src0}, {src1}\n");
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

            // tryleave por cada catch (en orden inverso al apilado).
            std::ostringstream tl;
            for (size_t i = 0; i < n_catches; ++i) tl << "tryleave\n";
            ir::IrInstr ra2{};
            ra2.op          = ir::IrOp::RAW_ASM;
            ra2.type        = ir::IrType::VOID;
            ra2.dst         = ir::IR_NO_VALUE;
            ra2.func_name   = tl.str();
            ra2.source_line = s->loc.line;
            fn_->append(current_block_, std::move(ra2));
            emit_finally_then_merge(current_block_, s->loc.line);
        }

        // Restaurar bindings al estado de entry para que cada handler
        // empiece con el mismo scope que tenia el try originalmente.
        scopes_.back() = entry_bindings;

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
            // Restaurar scope al estado de entry antes de lower el catch.
            scopes_.back() = entry_bindings;
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
            if (to_pop > 0) {
                std::ostringstream tl;
                for (size_t k = 0; k < to_pop; ++k) tl << "tryleave\n";
                ir::IrInstr rap{};
                rap.op          = ir::IrOp::RAW_ASM;
                rap.type        = ir::IrType::VOID;
                rap.dst         = ir::IR_NO_VALUE;
                rap.func_name   = tl.str();
                rap.source_line = cc.loc.line;
                fn_->append(current_block_, std::move(rap));
            }
            push_scope();
            if (!cc.var_name.empty()) {
                const ir::IrValueId v_exc = fn_->new_value(ir::IrType::PTR);
                ir::IrInstr         cap{};
                cap.op          = ir::IrOp::RAW_ASM;
                cap.type        = ir::IrType::PTR;
                cap.dst         = v_exc;
                cap.func_name   = std::string("// catch: bind r0 -> var\nmov {dst}, r0\n");
                cap.source_line = cc.loc.line;
                fn_->append(current_block_, std::move(cap));
                bind(cc.var_name, v_exc);
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
        // Restaurar al entry para insertar PHI sobre una base limpia.
        scopes_.back() = entry_bindings;

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
                scopes_.back()[name] = v_phi;
            }
        } else if (contribs.size() == 1) {
            // Solo un predecesor alcanza el merge: usar sus bindings.
            for (const auto &kv: *contribs[0].bindings) {
                scopes_.back()[kv.first] = kv.second;
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
            ld.type        = ir::IrType::I64;
            ld.dst         = v_load;
            ld.operands    = {v_slot};
            ld.source_line = s->loc.line;
            fn_->append(current_block_, std::move(ld));
            scopes_.back()[name] = v_load;
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
                    ir::IrInstr ra{};
                    ra.op          = ir::IrOp::RAW_ASM;
                    ra.type        = ir::IrType::VOID;
                    ra.dst         = ir::IR_NO_VALUE;
                    ra.operands    = std::move(opnds);
                    ra.func_name   = it->asm_text;
                    ra.source_line = it->source_line;
                    fn_->append(current_block_, std::move(ra));
                    break;
                }
                case CleanupAction::Kind::CALLN_FREE: {
                    // A.30.next - CALLN al free nativo de la coleccion
                    // (variante GC o no-GC).  Para la variante *_gc
                    // prependemos un GETPROC como primer argumento; el
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
        const ir::IrValueId v_handle = fn_->new_value(ir::IrType::HANDLE); {
            ir::IrInstr ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::HANDLE;
            ra.dst       = v_handle;
            ra.operands  = {v_obj};
            ra.func_name = std::string(
                "// === synchronized (obj) { ... }: ptr -> handle + monenter ===\n"
                "gchandle {dst}, {src0}\n"
                "monenter {dst}\n");
            ra.source_line = s->loc.line;
            fn_->append(current_block_, std::move(ra));
        }

        // tryenter catch-all: setup handler_pc y type=NULL via SSA values.
        const std::string   handler_label = fn_->name + "_" + fn_->blocks[handler_bb].name;
        const ir::IrValueId v_handler_pc  = fn_->new_value(ir::IrType::PTR);
        const ir::IrValueId v_type_null   = emit_const(ir::IrType::I64, 0, s->loc.line); {
            ir::IrInstr ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::PTR;
            ra.dst       = v_handler_pc;
            ra.operands  = {};
            ra.func_name = std::string("mov {dst}, @Absolute(\"code.")
                    + handler_label + "\")\n";
            ra.source_line = s->loc.line;
            fn_->append(current_block_, std::move(ra));
        } {
            ir::IrInstr ra{};
            ra.op          = ir::IrOp::RAW_ASM;
            ra.type        = ir::IrType::VOID;
            ra.dst         = ir::IR_NO_VALUE;
            ra.operands    = {v_handler_pc, v_type_null};
            ra.func_name   = std::string("tryenter {src0}, {src1}\n");
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
        {
            CleanupAction act;
            act.kind        = CleanupAction::Kind::RAW_ASM;
            act.operands    = {v_handle};
            act.source_line = s->loc.line;
            act.asm_text    = "// cleanup synchronized: tryleave + monexit\n"
                    "tryleave\n"
                    "monexit {src0}\n";
            cleanup_stack_.push_back(std::move(act));
        }

        // 4. Bajar el body.
        current_block_    = body_bb;
        block_terminated_ = false;
        lower_block(s->body.get());

        // 5. Pop cleanup (el body ya no necesita protegerse via emit_cleanups_all).
        cleanup_stack_.pop_back();

        // 6. Salida normal del body: tryleave + monexit + br merge.
        if (!block_terminated_) {
            ir::IrInstr ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::VOID;
            ra.dst       = ir::IR_NO_VALUE;
            ra.operands  = {v_handle};
            ra.func_name = std::string(
                "// === synchronized: salida normal -> tryleave + monexit ===\n"
                "tryleave\n"
                "monexit {src0}\n");
            ra.source_line = s->loc.line;
            fn_->append(current_block_, std::move(ra));

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
            ir::IrInstr ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::VOID;
            ra.dst       = ir::IR_NO_VALUE;
            ra.operands  = {v_handle};
            ra.func_name = std::string(
                "// === synchronized handler: monexit + rethrow ===\n"
                "monexit {src0}\n"
                "rethrow\n");
            ra.source_line = s->loc.line;
            fn_->append(current_block_, std::move(ra));
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

    std::string Lowering::generate_spawn_helper(ast::BlockStmt *body, const SourceLoc &loc) {
        const size_t      spawn_idx = spawn_func_counter_++;
        const std::string fn_name   = "__spawn_" + std::to_string(spawn_idx);

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

        // Setup de pila: ahora lo hace exec_instr_spawn directamente al
        // crear el proceso hijo (rsp/rbp = base unica por local_pid).
        // El frontend solo necesita asegurarse de que las instrucciones
        // de `enter`/`leave` y las locales caben en la region (1 MiB
        // por defecto, mas que suficiente para spawn bodies tipicos).

        if (body) lower_block(body);

        // Garantizar terminador HLT.  Si el body ya termino (ej. con throw
        // sin handler exterior, o con un return ilegal), sobreescribimos
        // poniendo un hlt fresco en un nuevo bloque.  Pero en uso normal
        // (body sin terminadores) simplemente anyadimos hlt al final.
        if (!block_terminated_) {
            ir::IrInstr ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::VOID;
            ra.dst       = ir::IR_NO_VALUE;
            ra.func_name = std::string(
                "// === fin del cuerpo de spawn: halt el proceso ===\n"
                "hlt\n");
            ra.source_line = loc.line;
            fn_->append(current_block_, std::move(ra));
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

        // Garantizar terminador HLT con R0=0 si el body cae sin return.
        if (!block_terminated_) {
            ir::IrInstr ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::VOID;
            ra.dst       = ir::IR_NO_VALUE;
            ra.func_name = std::string(
                "// === fin del cuerpo de rspawn: r0=0 + hlt ===\n"
                "mov r0, 0\n"
                "hlt\n");
            ra.source_line = loc.line;
            fn_->append(current_block_, std::move(ra));
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
        const ir::IrValueId v_pc = fn_->new_value(ir::IrType::PTR); {
            ir::IrInstr ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::PTR;
            ra.dst       = v_pc;
            ra.operands  = {};
            ra.func_name = std::string("// === rspawn(node) { ... }: load fn addr + rspawn ===\n"
                        "mov {dst}, @Absolute(\"code.")
                    + fn_name + "\")\n";
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
        }

        // 3. Bajar la expresion del node_idx y promover a I64.
        ir::IrValueId v_node = lower_expr(e->node_idx.get());
        if (v_node == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
        const ir::IrType src_t = fn_->values[v_node].type;
        v_node                 = cast_if_needed(v_node, src_t, ir::IrType::I64, e->loc.line);

        // 4. Emit `rspawn r_fn, r_node` + capturar R0 al SSA dst (Future handle).
        const ir::IrValueId v_fut = fn_->new_value(ir::IrType::I64); {
            ir::IrInstr ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::I64;
            ra.dst       = v_fut;
            ra.operands  = {v_pc, v_node};
            ra.func_name = std::string("rspawn {src0}, {src1}\n"
                "mov {dst}, r0\n");
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
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
            ir::IrInstr ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::PTR;
            ra.dst       = env_ptr;
            ra.func_name = "// === A.10 closures: leer env_ptr de R14 ===\n"
                    "mov {dst}, r14\n";
            ra.source_line = e->loc.line;
            child_fn.append(entry, std::move(ra));

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
                    // A.15 (gap O): propagar is_host_ptr para que el LOAD
                    // siguiente sepa emitir movh contra heap raw cuando
                    // el env vive en heap.
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
            // Heap raw via RAW_ALLOC.  Marcamos is_host_ptr para que los
            // STOREs siguientes emitan movh.  Sin free explicito (leak
            // controlado: el env vive hasta el fin del proceso).
            env_addr                          = fn_->new_value(ir::IrType::PTR);
            fn_->values[env_addr].is_host_ptr = true;
            const ir::IrValueId v_size        = emit_const(ir::IrType::I64,
                                                           static_cast<uint64_t>(N * 8),
                                                           e->loc.line);
            ir::IrInstr ins{};
            ins.op          = ir::IrOp::RAW_ALLOC;
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
        // 3. Cargar fn_addr (= @Absolute("code.__lambda_<N>")) en SSA.
        // -------------------------------------------------------------
        ir::IrValueId fn_addr = fn_->new_value(ir::IrType::I64); {
            ir::IrInstr ra{};
            ra.op          = ir::IrOp::RAW_ASM;
            ra.type        = ir::IrType::I64;
            ra.dst         = fn_addr;
            ra.func_name   = "mov {dst}, @Absolute(\"code." + fn_name + "\")\n";
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
        }

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
        for (size_t i = 0; i < args.size() && i < var->field_types.size(); ++i) {
            ir::IrValueId v = lower_expr(args[i].get());
            if (v == ir::IR_NO_VALUE) continue;
            // Promover a i64 si el tipo es mas estrecho.  Para PTR / i64
            // es identidad.
            ir::IrType vt = fn_->values[v].type;
            if (vt != ir::IrType::I64 && vt != ir::IrType::PTR) {
                v = cast_if_needed(v, vt, ir::IrType::I64, loc.line);
            }

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
        const auto entry_bindings_match = scopes_.back();

        struct ArmSnapshot {
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
            // Restaurar bindings al estado de entry antes de cada arm.
            scopes_.back() = entry_bindings_match;
            push_scope();

            // Bind de los payload bindings (si la variante tiene
            // payload).  Para cada binding i, emit LOAD i64 de
            // [scrut_addr + 8 + 8*i] y bind con el nombre del binding.
            if (arm.variant_name != "_") {
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
                    bind(arm.bindings[bi], v);
                }
            }

            if (arm.body) lower_stmt(arm.body.get());
            // Capturar snapshot ANTES de pop_scope (las
            // asignaciones a vars del scope outer se reflejan en el
            // segundo nivel de scopes_, accedido via scopes_[size-2]).
            ArmSnapshot snap;
            snap.reaches_merge = !block_terminated_;
            if (snap.reaches_merge) {
                // El scope OUTER (donde estan las vars compartidas) es
                // size-2 antes de pop_scope.  Capturamos esa copia.
                if (scopes_.size() >= 2) {
                    snap.bindings = scopes_[scopes_.size() - 2];
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
        scopes_.back() = entry_bindings_match;
        struct MergeContrib2 {
            ir::IrBlockId                                         pred;
            const std::unordered_map<std::string, ir::IrValueId> *bindings;
        };
        std::vector<MergeContrib2> contribs;
        contribs.reserve(arm_snaps.size());
        for (auto &as: arm_snaps) {
            if (as.reaches_merge) {
                contribs.push_back({as.pred, &as.bindings});
            }
        }
        if (contribs.size() >= 2) {
            for (const auto &kv: entry_bindings_match) {
                const std::string & name      = kv.first;
                const ir::IrValueId entry_val = kv.second;
                bool                any_diff  = false;
                for (const auto &c: contribs) {
                    auto it_b = c.bindings->find(name);
                    if (it_b == c.bindings->end()) continue;
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
                    auto          it_b   = c.bindings->find(name);
                    ir::IrValueId in_val = (it_b != c.bindings->end())
                                               ? it_b->second
                                               : entry_val;
                    ir::IrPhiArg arg{};
                    arg.value = in_val;
                    arg.block = c.pred;
                    phi.phi_args.push_back(arg);
                }
                fn_->blocks[merge_bb].instrs.insert(
                    fn_->blocks[merge_bb].instrs.begin(),
                    std::move(phi));
                scopes_.back()[name] = v_phi;
            }
        } else if (contribs.size() == 1) {
            for (const auto &kv: *contribs[0].bindings) {
                scopes_.back()[kv.first] = kv.second;
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
        const ir::IrValueId v_pc = fn_->new_value(ir::IrType::PTR); {
            ir::IrInstr ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::PTR;
            ra.dst       = v_pc;
            ra.operands  = {};
            ra.func_name = std::string("// === spawn { ... }: load fn addr + spawn ===\n"
                        "mov {dst}, @Absolute(\"code.")
                    + fn_name + "\")\n";
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
        }

        // si la policy es Auto, mantener el opcode SPAWN
        // (sin overhead).  Para Here y Pinned usar SPAWN_ON con el hint en
        // el segundo registro:
        //   - Here:   hint = -1 (signed) -> mismo scheduler que el padre.
        //   - Pinned: hint = expr        -> scheduler hint % num_schedulers.
        if (e->policy == ast::SpawnExpr::Policy::Auto) {
            const ir::IrValueId v_pid = fn_->new_value(ir::IrType::I64);
            ir::IrInstr         ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::I64;
            ra.dst       = v_pid;
            ra.operands  = {v_pc};
            ra.func_name = std::string("spawn {src0}\n"
                "mov {dst}, r0\n");
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
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

        // Emit `spawnon {src0}, {src1}` + capturar R0.
        const ir::IrValueId v_pid = fn_->new_value(ir::IrType::I64);
        ir::IrInstr         ra{};
        ra.op        = ir::IrOp::RAW_ASM;
        ra.type      = ir::IrType::I64;
        ra.dst       = v_pid;
        ra.operands  = {v_pc, v_hint};
        ra.func_name = std::string("spawnon {src0}, {src1}\n"
            "mov {dst}, r0\n");
        ra.source_line = e->loc.line;
        fn_->append(current_block_, std::move(ra));
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

        // 1a. msgrecv -> my_fut.  Buffer en stack 8 bytes, msgrecv, load.
        const ir::IrValueId v_buf = fn_->new_value(ir::IrType::PTR); {
            ir::IrInstr al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8;
            al.dst         = v_buf;
            al.imm         = 8;
            al.source_line = fd->loc.line;
            fn_->append(current_block_, std::move(al));
        }
        const ir::IrValueId v_max = emit_const(ir::IrType::I64, 8, fd->loc.line); {
            ir::IrInstr ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::VOID;
            ra.dst       = ir::IR_NO_VALUE;
            ra.operands  = {v_buf, v_max};
            ra.func_name = std::string("// @Async helper: recibir handle del Future\n"
                "msgrecv {src0}, {src1}\n");
            ra.source_line = fd->loc.line;
            fn_->append(current_block_, std::move(ra));
        }
        const ir::IrValueId v_my_fut = fn_->new_value(ir::IrType::I64); {
            ir::IrInstr ld{};
            ld.op          = ir::IrOp::LOAD;
            ld.type        = ir::IrType::I64;
            ld.dst         = v_my_fut;
            ld.operands    = {v_buf};
            ld.source_line = fd->loc.line;
            fn_->append(current_block_, std::move(ld));
        }

        // 1b. Activar interception de return en lower_return.
        async_fut_id_ = v_my_fut;

        // 1c. Bajar el body original.  Cada `return X` -> fulfill + hlt.
        lower_block(fd->body.get());

        // 1d. Fallback: si el body cae naturalmente sin return, emitir
        //     un fulfill default (0) + hlt para terminar el child.
        if (!block_terminated_) {
            const ir::IrValueId v_zero = emit_const(ir::IrType::I64, 0, fd->loc.line);
            ir::IrInstr         ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::VOID;
            ra.dst       = ir::IR_NO_VALUE;
            ra.operands  = {v_my_fut, v_zero};
            ra.func_name = std::string("// @Async helper: sin return -> fulfill(0) + hlt\n"
                "fulfill {src0}, {src1}\n"
                "hlt\n");
            ra.source_line = fd->loc.line;
            fn_->append(current_block_, std::move(ra));
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
        // ---------------------------------------------------------------
        ir::IrFunction wrapper_fn;
        wrapper_fn.name             = fd->name;
        wrapper_fn.ret_type         = ir::IrType::I64;
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

        // 2a. fut = future_alloc().
        const ir::IrValueId v_fut = fn_->new_value(ir::IrType::I64); {
            ir::IrInstr ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::I64;
            ra.dst       = v_fut;
            ra.func_name = std::string("// @Async wrapper: alloc Future\n"
                "future\n"
                "mov {dst}, r0\n");
            ra.source_line = fd->loc.line;
            fn_->append(current_block_, std::move(ra));
        }

        // 2b. child = spawn helper.
        const ir::IrValueId v_pc = fn_->new_value(ir::IrType::PTR); {
            ir::IrInstr ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::PTR;
            ra.dst       = v_pc;
            ra.func_name = std::string("// @Async wrapper: load helper addr\n"
                        "mov {dst}, @Absolute(\"code.")
                    + helper_name + "\")\n";
            ra.source_line = fd->loc.line;
            fn_->append(current_block_, std::move(ra));
        }
        const ir::IrValueId v_child = fn_->new_value(ir::IrType::I64); {
            ir::IrInstr ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::I64;
            ra.dst       = v_child;
            ra.operands  = {v_pc};
            ra.func_name = std::string("// @Async wrapper: spawn child\n"
                "spawn {src0}\n"
                "mov {dst}, r0\n");
            ra.source_line = fd->loc.line;
            fn_->append(current_block_, std::move(ra));
        }

        // 2c. msgsend(child, fut).  Buffer + store + msgsend.
        const ir::IrValueId v_sbuf = fn_->new_value(ir::IrType::PTR); {
            ir::IrInstr al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8;
            al.dst         = v_sbuf;
            al.imm         = 8;
            al.source_line = fd->loc.line;
            fn_->append(current_block_, std::move(al));
        } {
            ir::IrInstr st{};
            st.op          = ir::IrOp::STORE;
            st.type        = ir::IrType::I64;
            st.operands    = {v_fut, v_sbuf};
            st.source_line = fd->loc.line;
            fn_->append(current_block_, std::move(st));
        }
        const ir::IrValueId v_len = emit_const(ir::IrType::I64, 8, fd->loc.line); {
            ir::IrInstr ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::VOID;
            ra.dst       = ir::IR_NO_VALUE;
            ra.operands  = {v_child, v_sbuf, v_len};
            ra.func_name = std::string("// @Async wrapper: msgsend(child, fut)\n"
                "msgsend {src0}, {src1}, {src2}\n");
            ra.source_line = fd->loc.line;
            fn_->append(current_block_, std::move(ra));
        }

        // 2d. return fut.
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
        ra.op          = ir::IrOp::RAW_ASM;
        ra.type        = ir::IrType::VOID;
        ra.dst         = ir::IR_NO_VALUE;
        ra.operands    = {v_obj};
        ra.func_name   = std::string("throw {src0}\n");
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
                // Null en A.1 no esta cableado al tipo de referencia;
                // por ahora se emite como cero del tipo i64.
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
            case ast::NodeKind::IndexExpr:
                return lower_index(static_cast<ast::IndexExpr *>(e));
            case ast::NodeKind::ThisExpr:
                return lower_this_expr(static_cast<ast::ThisExpr *>(e));
            case ast::NodeKind::NewExpr:
                return lower_new_expr(static_cast<ast::NewExpr *>(e));
            case ast::NodeKind::SpawnExpr:
                return lower_spawn_expr(static_cast<ast::SpawnExpr *>(e));
            case ast::NodeKind::RSpawnExpr:
                return lower_rspawn_expr(static_cast<ast::RSpawnExpr *>(e));
            case ast::NodeKind::LambdaExpr:
                return lower_lambda_expr(static_cast<ast::LambdaExpr *>(e));
            case ast::NodeKind::MatchExpr:
                return lower_match_expr(static_cast<ast::MatchExpr *>(e));
            default:
                unsupported(e->loc, "expresion no soportada en A.1");
                return ir::IR_NO_VALUE;
        }
    }

    // ---------------------------------------------------------------------
    // Helpers: tamano de tipo y aritmetica de punteros.
    // ---------------------------------------------------------------------

    size_t Lowering::size_of_type(const Type &t) const {
        if (t.kind == PrimitiveKind::STRUCT) {
            const auto &layouts = tc_.struct_layouts();
            auto        it      = layouts.find(t.struct_name);
            return (it == layouts.end()) ? 0 : it->second.size_bytes;
        }
        if (t.kind == PrimitiveKind::PTR) return 8;
        if (t.kind == PrimitiveKind::ARRAY) {
            // T[N] ocupa N*sizeof(T) bytes; T[] (size==0) decae a puntero
            // y no tiene tamano propio (el caller no deberia preguntar).
            if (!t.pointee || t.array_size == 0) return 0;
            return static_cast<size_t>(t.array_size) * size_of_type(*t.pointee);
        }
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
            // Otros tipos de inicializador (StringLit, FloatLit, BinaryExpr)
            // -- a implementar cuando los necesite.
            break;
        }
        // A.18 fase B.3 - Constantes ENC_* (encoding numerico).
        // Cero overhead: emit const i32 inline en el call site.
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
        // A.16 - Identificadores ANSI magicos.  Si el nombre es una
        // constante predefinida (RED, GREEN, BOLD, RESET, etc.), emitir
        // un STR_LIT_ADDR a la cadena ANSI correspondiente.  Cero
        // overhead vs un string literal explicito; la deteccion es un
        // lookup O(1) en una tabla estatica.
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
        // 1. Registrar bytes en static_data + STR_LIT_ADDR.
        std::vector<uint8_t> bytes(slit->value.begin(), slit->value.end());
        const uint64_t       lit_idx = out_mod_->intern_static_data(std::move(bytes));
        const uint64_t       lit_len = (uint64_t) slit->value.size();

        ir::IrValueId v_addr = fn_->new_value(ir::IrType::PTR); {
            ir::IrInstr is{};
            is.op          = ir::IrOp::STR_LIT_ADDR;
            is.type        = ir::IrType::PTR;
            is.dst         = v_addr;
            is.imm         = lit_idx;
            is.source_line = slit->loc.line;
            fn_->append(current_block_, std::move(is));
        }
        ir::IrValueId v_len = emit_const(ir::IrType::I64, lit_len,
                                         slit->loc.line);

        // 2. Emitir strmake via RAW_ASM con substitucion {src0}=v_addr,
        //    {src1}=v_len, {dst}=handle.  El opcode strmake actualmente
        //    asume encoding UTF-8 internamente; la compactacion auto
        //    detecta ASCII puro y reduce.
        ir::IrValueId v_handle = fn_->new_value(ir::IrType::I64);
        ir::IrInstr   ra{};
        ra.op          = ir::IrOp::RAW_ASM;
        ra.type        = ir::IrType::I64;
        ra.dst         = v_handle;
        ra.operands    = {v_addr, v_len};
        ra.func_name   = std::string("strmake {dst}, {src0}, {src1}\n");
        ra.source_line = slit->loc.line;
        fn_->append(current_block_, std::move(ra));
        return v_handle;
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
        // 2. fn_addr = @Absolute("code.<fn_name>") via RAW_ASM.
        ir::IrValueId fn_addr = fn_->new_value(ir::IrType::I64); {
            ir::IrInstr ra{};
            ra.op          = ir::IrOp::RAW_ASM;
            ra.type        = ir::IrType::I64;
            ra.dst         = fn_addr;
            ra.func_name   = "mov {dst}, @Absolute(\"code." + fn_name + "\")\n";
            ra.source_line = line;
            fn_->append(current_block_, std::move(ra));
        }
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
        // Tipos canonicos del checker.
        const PrimitiveKind ltk = e->lhs ? e->lhs->result_type.kind : PrimitiveKind::COUNT;
        const PrimitiveKind rtk = e->rhs ? e->rhs->result_type.kind : PrimitiveKind::COUNT;

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
                if (!sl->is_interpolated()) {
                    return lower_string_literal_to_string_object(sl);
                }
            }
            return lower_expr(ex);
        };
        const bool lhs_is_str = (ltk == PrimitiveKind::STRING) ||
        (ltk == PrimitiveKind::PTR && e->lhs &&
            e->lhs->kind == ast::NodeKind::StringLitExpr &&
            !static_cast<ast::StringLitExpr *>(e->lhs.get())->is_interpolated());
        const bool rhs_is_str = (rtk == PrimitiveKind::STRING) ||
        (rtk == PrimitiveKind::PTR && e->rhs &&
            e->rhs->kind == ast::NodeKind::StringLitExpr &&
            !static_cast<ast::StringLitExpr *>(e->rhs.get())->is_interpolated());
        const bool any_real_str = (ltk == PrimitiveKind::STRING) ||
                (rtk == PrimitiveKind::STRING);
        if (lhs_is_str && rhs_is_str && any_real_str) {
            ir::IrValueId v_a = coerce_string_operand(e->lhs.get());
            ir::IrValueId v_b = coerce_string_operand(e->rhs.get());
            if (v_a == ir::IR_NO_VALUE || v_b == ir::IR_NO_VALUE)
                return ir::IR_NO_VALUE;
            if (e->op == ast::BinOp::Add) {
                // strcat -> rope handle (i64 STRING).
                ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
                ir::IrInstr   ra{};
                ra.op          = ir::IrOp::RAW_ASM;
                ra.type        = ir::IrType::I64;
                ra.dst         = v_dst;
                ra.operands    = {v_a, v_b};
                ra.func_name   = std::string("strcat {dst}, {src0}, {src1}\n");
                ra.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ra));
                return v_dst;
            }
            if (e->op == ast::BinOp::Eq || e->op == ast::BinOp::Neq) {
                ir::IrValueId v_cmp = fn_->new_value(ir::IrType::I64);
                ir::IrInstr   ra{};
                ra.op          = ir::IrOp::RAW_ASM;
                ra.type        = ir::IrType::I64;
                ra.dst         = v_cmp;
                ra.operands    = {v_a, v_b};
                ra.func_name   = std::string("strcmp {dst}, {src0}, {src1}\n");
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
            if (!e->operand || e->operand->kind != ast::NodeKind::IdentExpr) {
                error_at(e->loc, "lowering: ++/-- requieren un identificador como operando");
                return ir::IR_NO_VALUE;
            }
            auto *              id      = static_cast<ast::IdentExpr *>(e->operand.get());
            const ir::IrType    vt      = ir_type_from_primitive(e->operand->result_type.kind);
            const ir::IrValueId old_val = read_local(id->name, vt, e->loc.line);
            if (old_val == ir::IR_NO_VALUE) {
                error_at(e->loc, "lowering: nombre no resuelto: '" + id->name + "'");
                return ir::IR_NO_VALUE;
            }
            const bool is_inc = (e->op == ast::UnOp::PreInc || e->op == ast::UnOp::PostInc);

            // Generar la constante "1" del mismo tipo (entero) y la operacion.
            const ir::IrValueId one     = emit_const(vt, 1, e->loc.line);
            const ir::IrValueId new_val = fn_->new_value(vt);
            ir::IrInstr         op_ins{};
            op_ins.op          = is_inc ? ir::IrOp::ADD : ir::IrOp::SUB;
            op_ins.type        = vt;
            op_ins.dst         = new_val;
            op_ins.operands    = {old_val, one};
            op_ins.source_line = e->loc.line;
            fn_->append(current_block_, std::move(op_ins));

            // Escribir el nuevo valor (LOAD/STORE si address-taken o
            // update_scope SSA si no).
            write_local(id->name, new_val, vt, e->loc.line);

            // Resultado de la expresion: prefijo devuelve el nuevo valor;
            // postfijo devuelve el valor anterior (semantica C clasica).
            const bool is_pre = (e->op == ast::UnOp::PreInc || e->op == ast::UnOp::PreDec);
            return is_pre ? new_val : old_val;
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
                ra.op          = ir::IrOp::RAW_ASM;
                ra.type        = vt;
                ra.dst         = dst;
                ra.operands    = {v};
                ra.func_name   = std::string("unwrap {dst}, {src0}\n");
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
                // Capturamos r0 a {dst} para que el resultado sea un SSA value.
                const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
                ir::IrInstr         ra{};
                ra.op        = ir::IrOp::RAW_ASM;
                ra.type      = ir::IrType::I64;
                ra.dst       = v_dst;
                ra.operands  = {v};
                ra.func_name = std::string("// await {src0}\n"
                    "await {src0}\n"
                    "mov {dst}, r0\n");
                ra.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ra));
                return v_dst;
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
        const bool callee_is_sret = (callee_kind == PrimitiveKind::OPTIONAL
            || callee_kind == PrimitiveKind::RESULT
            || callee_is_enum_sret
            || callee_is_function_sret);
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
            }
            v_call_retbuf = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8; // unidad: 1 byte
            al.dst         = v_call_retbuf;
            al.imm         = buf_bytes;
            al.source_line = e->loc.line;
            fn_->append(current_block_, std::move(al));
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
                if (!sl->is_interpolated()) {
                    arg_ids.push_back(lower_string_literal_to_string_object(sl));
                    promote_to_string = true;
                }
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
        ins.func_name   = id->name;
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
                    if (!slit->is_interpolated()) {
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
            ir::IrValueId rhs = lower_expr(e->value.get());
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

            // A.18.x.1 - WRITE de bit field: read-modify-write.  Si el
            // campo es bit field, leemos el storage word completo, le
            // limpiamos los bits del rango con AND inverse_mask, le
            // metemos el valor con OR ((rhs & mask) << bit_offset), y
            // hacemos STORE de vuelta.  Para campo normal: STORE directo.
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
            ir::IrValueId rhs = lower_expr(e->value.get());
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
                ir::IrValueId rhs = lower_expr(e->value.get());
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
        ir::IrValueId rhs = lower_expr(e->value.get());
        if (rhs == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;

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
                case ast::AssignOp::BitOrAssign: bop = ast::BinOp::BitOr;
                    break;
                case ast::AssignOp::BitXorAssign: bop = ast::BinOp::BitXor;
                    break;
                case ast::AssignOp::ShlAssign: bop = ast::BinOp::Shl;
                    break;
                case ast::AssignOp::ShrAssign: bop = ast::BinOp::Shr;
                    break;
                case ast::AssignOp::Assign: break; // ya filtrado arriba
            }
            rhs = emit_binop_ir(bop, l, r, common, e->loc);
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

    // Forward decls de los helpers definidos mas abajo en el TU.  Necesarias
    // porque try_lower_builtin_call los invoca en la implementacion de
    // forName/getClass 
    static uint64_t intern_class_name(ir::IrModule &mod, const std::string &name);

    static void emit_findclass_inline(std::ostringstream &asm_,
                                      uint64_t            name_idx,
                                      uint32_t            name_len);

    bool Lowering::try_lower_builtin_call(ast::CallExpr *e, ir::IrValueId &out_value) {
        // Solo manejamos identifier-callees (validado en lower_call).
        if (!e->callee || e->callee->kind != ast::NodeKind::IdentExpr) return false;
        const auto *       id   = static_cast<const ast::IdentExpr *>(e->callee.get());
        const std::string &name = id->name;

        // Conjunto de nombres builtin reconocidos.  Si el nombre no esta
        // aqui devolvemos false para que lower_call siga con la ruta
        // generica (CALL a una funcion del usuario).
        const bool is_print     = (name == "print");
        const bool is_println   = (name == "println");
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
        // futures builtins.
        const bool is_future_alloc = (name == "future_alloc");
        const bool is_fulfill      = (name == "fulfill");
        // carga dinamica de modulos.
        const bool is_loadmodule = (name == "loadmodule");
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
        const bool is_any_math   = is_math_sqrt || is_math_pow || is_math_fabs
                || is_math_floor || is_math_ceil || is_math_round
                || is_math_fmin || is_math_fmax
                || is_math_log || is_math_log2 || is_math_log10
                || is_math_sin || is_math_cos || is_math_tan
                || is_math_abs || is_math_imin || is_math_imax
                || is_math_clamp;
        // A.18 - builtins de string.
        const bool is_str_length  = (name == "str_length");
        const bool is_str_bytes   = (name == "str_bytes");
        const bool is_str_cstr    = (name == "str_cstr");
        const bool is_str_wstr    = (name == "str_wstr");
        const bool is_str_hash    = (name == "str_hash");
        const bool is_str_intern  = (name == "str_intern");
        const bool is_str_concat  = (name == "str_concat");
        const bool is_str_equals  = (name == "str_equals");
        const bool is_str_make    = (name == "str_make");
        const bool is_str_convert = (name == "str_convert");
        const bool is_any_builtin = is_print || is_println || is_echo
                || is_flush || is_print_int
                || is_print_uint || is_print_hex
                || is_print_float || is_print_bool
                || is_print_char || is_print_color
                || is_print_cstr
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
                || is_future_alloc || is_fulfill
                || is_loadmodule
                || is_ffi_open || is_ffi_sym || is_ffi_call
                || is_panic
                || is_str_length || is_str_bytes
                || is_str_cstr || is_str_wstr
                || is_str_hash || is_str_intern
                || is_str_concat || is_str_equals
                || is_str_make || is_str_convert
                || is_any_math
                || is_col_ctor
                || is_dispose;
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

        auto emit_print_typed_value = [&](ast::Expr *ex) {
            if (!ex) return;
            const Type t = ex->result_type;
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
            // Caso STRING: el valor es GcHandle a un StringObject.  Emitir
            // STRRAW para obtener host_ptr al buffer + STRGETBYTES para la
            // longitud en bytes, y usar vio_print_buf para emitir el bloque
            // sin cortar en NUL (binary-safe; preserva multi-byte UTF-8).
            if (t.kind == PrimitiveKind::STRING) {
                ir::IrValueId v_ptr            = fn_->new_value(ir::IrType::PTR);
                fn_->values[v_ptr].is_host_ptr = true;
                ir::IrInstr rw{};
                rw.op          = ir::IrOp::RAW_ASM;
                rw.type        = ir::IrType::PTR;
                rw.dst         = v_ptr;
                rw.operands    = {v};
                rw.func_name   = std::string("strraw {dst}, {src0}\n");
                rw.source_line = ex->loc.line;
                fn_->append(current_block_, std::move(rw));
                ir::IrValueId v_len = fn_->new_value(ir::IrType::I64);
                ir::IrInstr   gb{};
                gb.op          = ir::IrOp::RAW_ASM;
                gb.type        = ir::IrType::I64;
                gb.dst         = v_len;
                gb.operands    = {v};
                gb.func_name   = std::string("strgetbytes {dst}, {src0}\n");
                gb.source_line = ex->loc.line;
                fn_->append(current_block_, std::move(gb));
                out_mod_->register_native_import(lib, "vio_print_buf");
                ir::IrInstr ins{};
                ins.op          = ir::IrOp::CALLN;
                ins.type        = ir::IrType::VOID;
                ins.dst         = ir::IR_NO_VALUE;
                ins.func_name   = lib + ":vio_print_buf";
                ins.operands    = {v_ptr, v_len};
                ins.source_line = ex->loc.line;
                fn_->append(current_block_, std::move(ins));
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
            } else {
                v = cast_if_needed(v, vt, promote, ex->loc.line);
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
                    // + ... + parts[N].  Cada fragmento -> 1 CALLN.
                    const size_t ne = sl->interp_exprs.size();
                    const size_t np = sl->interp_parts.size();
                    for (size_t i = 0; i < ne; ++i) {
                        if (i < np && !sl->interp_parts[i].empty()) {
                            emit_print_string_literal(sl->interp_parts[i],
                                                      ex->loc.line);
                        }
                        emit_print_typed_value(sl->interp_exprs[i].get());
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
            emit_print_typed_value(ex);
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
            || is_print_cstr) {
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
            v = cast_if_needed(v, fn_->values[v].type, ir::IrType::I64,
                               e->loc.line);
            std::string func;
            if (is_print_uint) func = "vio_print_uint";
            else if (is_print_hex) func = "vio_print_hex";
            else if (is_print_float) func = "vio_print_float";
            else if (is_print_bool) func = "vio_print_bool";
            else if (is_print_char) func = "vio_print_char";
            else if (is_print_color) func = "vio_print_color";
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
            std::ostringstream oss;
            // Cargar @Absolute("code.s_<idx>") en r12 (path_addr) y len en r11.
            oss << "mov r12, @Absolute(\"code.s_" << path_idx << "\")\n";
            oss << "mov r11, " << path_len << "\n";
            // Emitir loadmod r12, r11.  El runtime auto-invoca el main del
            // modulo via callvm-equivalente; cuando RET, R0 contiene el
            // init_pc (puede ser 0 si load fallo o si el main lo sobrescribio).
            oss << "loadmod r12, r11\n";
            // Capturar R0 al SSA dst para que Vex pueda inspeccionarlo.
            oss << "mov {dst}, r0\n";
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr         ra{};
            ra.op          = ir::IrOp::RAW_ASM;
            ra.type        = ir::IrType::I64;
            ra.dst         = v_dst;
            ra.func_name   = oss.str();
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
            out_value = v_dst;
            return true;
        }

        // ===== A.27.1 - dispose(xs) =====
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
            std::ostringstream oss;
            oss << "mov r12, @Absolute(\"code.s_" << path_idx << "\")\n";
            oss << "mov r11, " << path_len << "\n";
            oss << "dlopen {dst}, r12, r11\n";
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr         ra{};
            ra.op          = ir::IrOp::RAW_ASM;
            ra.type        = ir::IrType::I64;
            ra.dst         = v_dst;
            ra.func_name   = oss.str();
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
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
            // Mover handle a r12 via RAW_ASM con substitucion {src0}, luego
            // cargar name_addr en r11 y len en r10, llamar dlsym.
            std::ostringstream oss;
            oss << "mov r12, {src0}\n"; // handle (preserva valor SSA)
            oss << "mov r11, @Absolute(\"code.s_" << name_idx << "\")\n";
            oss << "mov r10, " << name_len << "\n";
            oss << "dlsym {dst}, r12, r11, r10\n";
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr         ra{};
            ra.op          = ir::IrOp::RAW_ASM;
            ra.type        = ir::IrType::I64;
            ra.dst         = v_dst;
            ra.operands    = {v_handle};
            ra.func_name   = oss.str();
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
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
            std::string       func_name;
            size_t            expected_args = 1;
            ir::IrType        ret_ir        = ir::IrType::F64;
            ir::IrType        arg_ir        = ir::IrType::I64; // por defecto pasa bits f64 como i64
            bool              dst_is_float  = true;
            if (is_math_sqrt) {
                func_name = "vmath_sqrt";
            } else if (is_math_pow) {
                func_name     = "vmath_pow";
                expected_args = 2;
            } else if (is_math_fabs) {
                func_name = "vmath_fabs";
            } else if (is_math_floor) {
                func_name = "vmath_floor";
            } else if (is_math_ceil) {
                func_name = "vmath_ceil";
            } else if (is_math_round) {
                func_name = "vmath_round";
            } else if (is_math_fmin) {
                func_name     = "vmath_fmin";
                expected_args = 2;
            } else if (is_math_fmax) {
                func_name     = "vmath_fmax";
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
            std::ostringstream oss;
            // Cargar addr en r12 y len en r11; emitir panic directo.
            // Si hay tryenter activo, panic invoca throw_fatal -> do_throw
            // -> salta al handler.  Si no, mata el proceso.
            oss << "mov r12, @Absolute(\"code.s_" << msg_idx << "\")\n";
            oss << "mov r11, " << msg_len << "\n";
            oss << "panic r12, r11\n";
            ir::IrInstr ra{};
            ra.op          = ir::IrOp::RAW_ASM;
            ra.type        = ir::IrType::VOID;
            ra.dst         = ir::IR_NO_VALUE;
            ra.func_name   = oss.str();
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
            out_value = ir::IR_NO_VALUE;
            return true;
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
                if (!sl->is_interpolated()) {
                    v_str = lower_string_literal_to_string_object(sl);
                } else {
                    v_str = lower_expr(ae);
                }
            } else {
                v_str = lower_expr(ae);
            }
            if (v_str == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // wstr requiere strconv UTF16 + strraw (2 ops).
            if (is_str_wstr) {
                ir::IrValueId v_conv = fn_->new_value(ir::IrType::I64);
                ir::IrInstr   cv{};
                cv.op          = ir::IrOp::RAW_ASM;
                cv.type        = ir::IrType::I64;
                cv.dst         = v_conv;
                cv.operands    = {v_str};
                cv.func_name   = std::string("strconv {dst}, {src0}, 3\n");
                cv.source_line = e->loc.line;
                fn_->append(current_block_, std::move(cv));
                ir::IrValueId v_raw            = fn_->new_value(ir::IrType::PTR);
                fn_->values[v_raw].is_host_ptr = true;
                ir::IrInstr rw{};
                rw.op          = ir::IrOp::RAW_ASM;
                rw.type        = ir::IrType::PTR;
                rw.dst         = v_raw;
                rw.operands    = {v_conv};
                rw.func_name   = std::string("strraw {dst}, {src0}\n");
                rw.source_line = e->loc.line;
                fn_->append(current_block_, std::move(rw));
                out_value = v_raw;
                return true;
            }
            // Resto: 1 sola instruccion bytecode.
            std::string op;
            ir::IrType  ret_ty       = ir::IrType::I64;
            bool        dst_host_ptr = false;
            if (is_str_length) {
                op = "strlen";
            } else if (is_str_bytes) {
                op = "strgetbytes";
            } else if (is_str_cstr) {
                op           = "strraw";
                ret_ty       = ir::IrType::PTR;
                dst_host_ptr = true;
            } else if (is_str_hash) {
                op = "strhash";
            } else {
                op = "strintern";
            } // STRING type
            ir::IrValueId v_dst = fn_->new_value(ret_ty);
            if (dst_host_ptr) fn_->values[v_dst].is_host_ptr = true;
            ir::IrInstr ra{};
            ra.op          = ir::IrOp::RAW_ASM;
            ra.type        = ret_ty;
            ra.dst         = v_dst;
            ra.operands    = {v_str};
            ra.func_name   = op + " {dst}, {src0}\n";
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
            out_value = v_dst;
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
                    if (!sl->is_interpolated()) {
                        return lower_string_literal_to_string_object(sl);
                    }
                }
                return lower_expr(ex);
            };
            ir::IrValueId v_a = coerce_to_string_handle(e->args[0].get());
            ir::IrValueId v_b = coerce_to_string_handle(e->args[1].get());
            if (v_a == ir::IR_NO_VALUE || v_b == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const char *  op    = is_str_concat ? "strcat" : "strcmp";
            ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr   ra{};
            ra.op          = ir::IrOp::RAW_ASM;
            ra.type        = ir::IrType::I64;
            ra.dst         = v_dst;
            ra.operands    = {v_a, v_b};
            ra.func_name   = std::string(op) + " {dst}, {src0}, {src1}\n";
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
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
            ir::IrValueId v_h = fn_->new_value(ir::IrType::I64);
            ir::IrInstr   ra{};
            ra.op          = ir::IrOp::RAW_ASM;
            ra.type        = ir::IrType::I64;
            ra.dst         = v_h;
            ra.operands    = {v_ptr, v_len};
            ra.func_name   = std::string("strmake {dst}, {src0}, {src1}\n");
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
            out_value = v_h;
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
            ir::IrValueId v_h = fn_->new_value(ir::IrType::I64);
            ir::IrInstr   ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::I64;
            ra.dst       = v_h;
            ra.operands  = {v_s};
            ra.func_name = std::string("strconv {dst}, {src0}, ")
                    + std::to_string(enc_val) + "\n";
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
            out_value = v_h;
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
            // que el ir_emitter substituye por el registro asignado a
            // v_dst tras regalloc.  El patron findclass deja el resultado
            // en r12; lo movemos a {dst}.
            std::ostringstream oss;
            emit_findclass_inline(oss, name_idx, name_len);
            oss << "mov {dst}, r12\n";

            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr         ra{};
            ra.op          = ir::IrOp::RAW_ASM;
            ra.type        = ir::IrType::I64;
            ra.dst         = v_dst;
            ra.func_name   = oss.str();
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
            out_value = v_dst;
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
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr         ra{};
            ra.op       = ir::IrOp::RAW_ASM;
            ra.type     = ir::IrType::I64;
            ra.dst      = v_dst;
            ra.operands = {v_buf};
            // {src0} = reg del primer operando, {dst} = reg destino.
            ra.func_name   = std::string("findfield {dst}, {src0}\n");
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
            out_value = v_dst;
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
            // findmethod via RAW_ASM con substitucion {dst}/{src0}.
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64); {
                ir::IrInstr ra{};
                ra.op          = ir::IrOp::RAW_ASM;
                ra.type        = ir::IrType::I64;
                ra.dst         = v_dst;
                ra.operands    = {v_buf};
                ra.func_name   = std::string("findmethod {dst}, {src0}\n");
                ra.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ra));
            }
            out_value = v_dst;
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
                ir::IrInstr ra{};
                ra.op        = ir::IrOp::RAW_ASM;
                ra.type      = ir::IrType::I64;
                ra.dst       = v_host;
                ra.operands  = {v_handle};
                ra.func_name = std::string(
                    "gcderef cur0, {src0}\n"
                    "xchg cur0, {dst}\n");
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
        auto stack_alloc_buf = [&](uint64_t bytes, uint32_t line) -> ir::IrValueId {
            const ir::IrValueId v_buf = fn_->new_value(ir::IrType::PTR);
            ir::IrInstr         al{};
            al.op          = ir::IrOp::ALLOCA;
            al.type        = ir::IrType::I8;
            al.imm         = bytes;
            al.dst         = v_buf;
            al.source_line = line;
            fn_->append(current_block_, std::move(al));
            // ALLOCA devuelve direccion de memoria VM (subsp + readcur);
            // no es host_ptr.  Las STORE/LOAD subsiguientes usaran `mov`.
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
            const ir::IrValueId v_buf = stack_alloc_buf(16, e->loc.line);
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
            const ir::IrValueId v_buf  = stack_alloc_buf(16, e->loc.line);
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
            const ir::IrValueId v_payload = lower_expr(e->args[0].get());
            if (v_payload == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            const ir::IrValueId v_buf = stack_alloc_buf(24, e->loc.line);
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
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I32);
            ir::IrInstr         ra{};
            ra.op       = ir::IrOp::RAW_ASM;
            ra.type     = ir::IrType::I32;
            ra.dst      = v_dst;
            ra.operands = {v_arg};
            // Pasos:
            //   isnull {dst}, {src0}  ; {dst} = 1 si nulo, 0 si no
            //   mov r14, 1
            //   xor {dst}, r14        ; invertir el bit (1 -> 0, 0 -> 1)
            // El XOR de la VM es bit-a-bit; como solo el bit 0 esta puesto,
            // queda el resultado deseado.
            ra.func_name = std::string(
                "isnull {dst}, {src0}\n"
                "mov r14, 1\n"
                "xor {dst}, r14\n");
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
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
                uw.op          = ir::IrOp::RAW_ASM;
                uw.type        = ir::IrType::I64;
                uw.dst         = v_chk;
                uw.operands    = {v_flag};
                uw.func_name   = std::string("unwrap {dst}, {src0}\n");
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
            ra.op          = ir::IrOp::RAW_ASM;
            ra.type        = t;
            ra.dst         = v_dst;
            ra.operands    = {v_arg};
            ra.func_name   = std::string("unwrap {dst}, {src0}\n");
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
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr         ra{};
            ra.op          = ir::IrOp::RAW_ASM;
            ra.type        = ir::IrType::I64;
            ra.dst         = v_dst;
            ra.func_name   = std::string("proceed\nmov {dst}, r0\n");
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
            out_value = v_dst;
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
            const char *mnem = is_wait
                                   ? "monwait"
                                   : is_notify
                                         ? "monnoti"
                                         : "monnota";
            // Una sola RAW_ASM hace ptr->handle + operacion en el mismo handle.
            const ir::IrValueId v_handle = fn_->new_value(ir::IrType::HANDLE);
            ir::IrInstr         ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::HANDLE;
            ra.dst       = v_handle;
            ra.operands  = {v_obj};
            ra.func_name = std::string("// ") + name + "(obj) -> gchandle + " + mnem + "\n"
                    + "gchandle {dst}, {src0}\n"
                    + mnem + " {dst}\n";
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
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
            const ir::IrValueId v_pid = fn_->new_value(ir::IrType::I64);
            ir::IrInstr         ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::I64;
            ra.dst       = v_pid;
            ra.func_name = std::string("// pid() -> getpid {dst}\n"
                "getpid {dst}\n");
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
            out_value = v_pid;
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
            // msgsend r_pid, r_addr, r_len  (longitud=8).  El opcode
            // requiere 3 registros; preparamos r_len como CONST 8 SSA.
            const ir::IrValueId v_len = emit_const(ir::IrType::I64, 8, e->loc.line);
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I32);
            ir::IrInstr         ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::I32;
            ra.dst       = v_dst;
            ra.operands  = {v_pid, v_buf, v_len};
            ra.func_name = std::string("// msgsend(pid, value)\n"
                "msgsend {src0}, {src1}, {src2}\n"
                "mov {dst}, r0\n");
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
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
                ir::IrInstr ra{};
                ra.op        = ir::IrOp::RAW_ASM;
                ra.type      = ir::IrType::VOID;
                ra.dst       = ir::IR_NO_VALUE;
                ra.operands  = {v_buf, v_max};
                ra.func_name = std::string("// msgrecv() -> bloquea hasta dato\n"
                    "msgrecv {src0}, {src1}\n");
                ra.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ra));
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
            const ir::IrValueId v_fut = fn_->new_value(ir::IrType::I64);
            ir::IrInstr         ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::I64;
            ra.dst       = v_fut;
            ra.func_name = std::string("// future_alloc()\n"
                "future\n"
                "mov {dst}, r0\n");
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
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
            ir::IrInstr ra{};
            ra.op        = ir::IrOp::RAW_ASM;
            ra.type      = ir::IrType::VOID;
            ra.dst       = ir::IR_NO_VALUE;
            ra.operands  = {v_fut, v_val};
            ra.func_name = std::string("// fulfill(fut, val)\n"
                "fulfill {src0}, {src1}\n");
            ra.source_line = e->loc.line;
            fn_->append(current_block_, std::move(ra));
            out_value = ir::IR_NO_VALUE;
            return true;
        }

        // No deberia alcanzarse: todos los builtins listados arriba estan
        // cubiertos.
        return false;
    }

    // ---------------------------------------------------------------------
    // POO: clases en Vex. la integracion completa
    // (registro en module init, NEWOBJ + CALLVIRT, GETFIELD/SETFIELD)
    // se implementa por fases.  Cada metodo nuevo emite un error claro
    // hasta que su implementacion concreta este lista.
    // ---------------------------------------------------------------------

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

            // Tipo de retorno.
            if (m->is_constructor) {
                fn.ret_type = ir::IrType::VOID;
            } else if (m->return_type
                && m->return_type->kind == ast::NodeKind::PrimitiveTypeNode) {
                auto *pt    = static_cast<ast::PrimitiveTypeNode *>(m->return_type.get());
                fn.ret_type = ir_type_from_primitive(pt->prim);
            } else if (m->return_type) {
                const Type sem = tc_.resolve_type_node(m->return_type.get());
                fn.ret_type    = (sem.kind != PrimitiveKind::COUNT
                                     && sem.kind != PrimitiveKind::VOID)
                                     ? ir_type_from_primitive(sem.kind)
                                     : ir::IrType::VOID;
            } else {
                fn.ret_type = ir::IrType::VOID;
            }

            // Param 0: 'this' como PTR.  Sin contar en m->params.
            std::vector<std::pair<std::string, ir::IrValueId> > bindings;
            const ir::IrValueId                                 this_vid = fn.new_value(ir::IrType::PTR, "%this");
            fn.values[this_vid].is_param                                 = true;
            // fix - this es siempre un host_ptr a un objeto GC; debe
            // ser refrescado tras cualquier CALL que pueda disparar GC.
            fn.values[this_vid].is_host_ptr  = true;
            fn.values[this_vid].is_gc_object = true;
            fn.params.push_back(this_vid);
            bindings.emplace_back("this", this_vid);

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
                    // A.32.fix - param de tipo CLASS es host_ptr GC; el
                    // regalloc debe refrescarlo via gchandle/gcderef.
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

            lower_block(m->body.get());

            current_class_lowering_ = saved_class;

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
                        // 2b) field_val = host_ptr fresco via gcderef + xchg.
                        const ir::IrValueId field_val       = fn_->new_value(ir::IrType::I64);
                        fn_->values[field_val].is_host_ptr  = true;
                        fn_->values[field_val].is_gc_object = true;
                        ir::IrInstr deref{};
                        deref.op        = ir::IrOp::RAW_ASM;
                        deref.type      = ir::IrType::I64;
                        deref.dst       = field_val;
                        deref.operands  = {v_handle};
                        deref.func_name = std::string(
                            "gcderef cur0, {src0}\n"
                            "xchg cur0, {dst}\n");
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
     */
    static uint64_t intern_class_name(ir::IrModule &mod, const std::string &name) {
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
    static uint64_t intern_class_cache_slot(ir::IrModule &mod, const std::string &name) {
        std::vector<uint8_t> bytes(8, 0);                    // 8 zeros (cache)
        bytes.push_back(0xFF);                               // sentinel: distingue de intern_class_name
        bytes.insert(bytes.end(), name.begin(), name.end()); // nombre para unicidad
        return mod.intern_static_data(std::move(bytes));
    }

    /**
     * @brief Emite el ASM que escribe un qword en una direccion de memoria
     *        VM @c base+offset usando @c mov [reg], reg64.  Las structs
     *        DefXxxParams viven en stack (memoria VM), por lo que NO se
     *        usa @c writecur (que escribe en memoria HOST a traves de un
     *        cursor) sino el MOV con direccionamiento SIB simple.
     */
    static void emit_store_qword_vm(std::ostringstream &asm_,
                                    const std::string & base_reg,
                                    uint32_t            offset,
                                    const std::string & val_expr) {
        // Usamos r5 como puntero efectivo; r6 como valor a escribir.
        asm_ << "mov r5, " << base_reg << "\n";
        if (offset > 0) asm_ << "addu r5, " << offset << "\n";
        asm_ << "mov r6, " << val_expr << "\n";
        asm_ << "mov [r5], r6\n";
    }

    /**
     * @brief Emite el ASM que construye una FindClassParams (16 bytes) en
     *        stack y deja en @c r12 el ClassInfo* localizado.
     */
    static void emit_findclass_inline(std::ostringstream &asm_,
                                      uint64_t            name_idx,
                                      uint32_t            name_len) {
        asm_ << "subsp rsp, 16\n";
        asm_ << "mov r12, rsp\n";
        // [+0] name_addr = @Absolute("code.s_<idx>")
        emit_store_qword_vm(asm_, "r12", 0,
                            "@Absolute(\"code.s_" + std::to_string(name_idx) + "\")");
        // [+8] name_len|0
        emit_store_qword_vm(asm_, "r12", 8, std::to_string(name_len));
        // findclass
        asm_ << "findclass r12, r12\n";
        asm_ << "addsp rsp, 16\n";
    }

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

            // Localizar el primer constructor.  Si la clase no tiene ctor
            // explicito, generamos un new sin invocar callvirt (objeto
            // recien construido sin inicializacion adicional).
            const ClassMethodInfo *ctor = nullptr;
            for (const auto &m: lay.methods) {
                if (m.is_constructor) {
                    ctor = &m;
                    break;
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

            // Construir IrFunction __new_<Class>.
            ir::IrFunction fn;
            fn.name     = "__new_" + cd->name;
            fn.ret_type = ir::IrType::PTR;

            // Params: replicar tipos del ctor (si existe).
            for (size_t i = 0; i < nargs; ++i) {
                const ir::IrType    pt  = ir_type_from_primitive(ctor->param_types[i].kind);
                const ir::IrValueId vid = fn.new_value(pt, "%a" + std::to_string(i));
                fn.values[vid].is_param = true;
                fn.params.push_back(vid);
            }

            const ir::IrBlockId entry = fn.new_block("entry");

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
                asm_ << "newobj r1\n"; // r0 = GcHandle, r1 = ClassInfo*
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
                asm_ << "newobj r1\n"; // r0 = handle
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

            out.add_function(std::move(fn));
        }
    }

    void Lowering::generate_module_init_function(ir::IrModule &out) {
        // Si no hay clases, no generamos nada (no se inserta prologo en main).
        bool any_class = false;
        for (auto &decl: mod_.decls) {
            if (decl && decl->kind == ast::NodeKind::ClassDecl) {
                any_class = true;
                break;
            }
        }
        if (!any_class) return;

        ir::IrFunction fn;
        fn.name                   = "__module_init";
        fn.ret_type               = ir::IrType::VOID;
        const ir::IrBlockId entry = fn.new_block("entry");

        std::ostringstream asm_;

        for (auto &decl: mod_.decls) {
            if (!decl || decl->kind != ast::NodeKind::ClassDecl) continue;
            auto *cd = static_cast<ast::ClassDecl *>(decl.get());
            auto  it = tc_.class_layouts().find(cd->name);
            if (it == tc_.class_layouts().end()) continue;
            const ClassLayout &lay = it->second;

            // 1) Construir DefClassParams (32 bytes) en stack y llamar defclass.
            const uint64_t cname_idx = intern_class_name(out, cd->name);
            const uint32_t cname_len = static_cast<uint32_t>(cd->name.size());

            asm_ << "// === defclass " << cd->name << " ===\n";

            // Si hay superclase, hacer findclass del super primero para
            // tener su ClassInfo*.  El bytecode de findclass requiere
            // sus propios FindClassParams en stack.
            std::string super_reg = "0"; // por defecto sin super
            if (!cd->super_name.empty()) {
                const uint64_t sname_idx = intern_class_name(out, cd->super_name);
                const uint32_t sname_len = static_cast<uint32_t>(cd->super_name.size());
                emit_findclass_inline(asm_, sname_idx, sname_len);
                // findclass deja el resultado en r12.  Movemos a r11 para
                // preservar durante el defclass.
                asm_ << "mov r11, r12\n";
                super_reg = "r11";
            }

            asm_ << "subsp rsp, 32\n";
            asm_ << "mov r3, rsp\n";
            // [+0] name_addr
            emit_store_qword_vm(asm_, "r3", 0,
                                "@Absolute(\"code.s_" + std::to_string(cname_idx) + "\")");
            // [+8] (flags<<32)|name_len = (1<<32)|len  con flags=CLASS_VIS_PUBLIC=1
            {
                uint64_t packed = (uint64_t(1) << 32) | uint64_t(cname_len);
                emit_store_qword_vm(asm_, "r3", 8, std::to_string(packed));
            }
            // [+16] super_class: 0 si sin herencia, o el ClassInfo* del super.
            emit_store_qword_vm(asm_, "r3", 16, super_reg);
            // [+24] reserved = 0
            emit_store_qword_vm(asm_, "r3", 24, "0");

            asm_ << "defclass r1, r3\n";
            asm_ << "addsp rsp, 32\n";
            asm_ << "mov r12, r1\n"; // r12 = ClassInfo* persiste durante deffield/defmethod
            // fix11 - cachear el ClassInfo* en su slot static_data
            // para que __new_<Class> lo lea directamente sin findclass.
            // El slot fue reservado en generate_new_helpers (intern_class_cache_slot).
            {
                const uint64_t cache_idx = intern_class_cache_slot(out, cd->name);
                asm_ << "mov r5, @Absolute(\"code.s_" << cache_idx << "\")\n";
                asm_ << "mov [r5], r1\n"; // cache[Class] = ClassInfo*
            }

            // 2) Para cada field PROPIO: DefFieldParams + deffield.
            // Los heredados ya los anyadio define_class en el loader al
            // copiar la jerarquia del super; re-emitirlos aqui causaria
            // duplicado y add_field devolveria error.
            for (size_t fi = lay.inherited_field_count;
                 fi < lay.fields.size(); ++fi) {
                const auto &   f         = lay.fields[fi];
                const uint64_t fname_idx = intern_class_name(out, f.name);
                const uint32_t fname_len = static_cast<uint32_t>(f.name.size());
                asm_ << "// deffield " << f.name << "\n";
                asm_ << "subsp rsp, 32\n";
                asm_ << "mov r3, rsp\n";
                // [+0] name_addr
                emit_store_qword_vm(asm_, "r3", 0,
                                    "@Absolute(\"code.s_" + std::to_string(fname_idx) + "\")");
                // [+8] (kind|access<<8|is_static<<16|0)<<32 | name_len
                // Empaquetamos: name_len en lo bajo y los flags / kind /
                // access / is_static en la palabra alta.  Nuestro layout C
                // espera bytes individuales en +12, +13, +14, +15, asi que
                // ponemos: (name_len) | (kind<<32) | (access<<40) | (is_static<<48).
                {
                    uint64_t packed = uint64_t(fname_len);
                    packed |= (uint64_t(0)) << 32; // kind = FIELD_PRIMITIVE
                    packed |= (uint64_t(0)) << 40; // access = FIELD_PUBLIC
                    packed |= (uint64_t(0)) << 48; // is_static = false
                    emit_store_qword_vm(asm_, "r3", 8, std::to_string(packed));
                }
                // [+16] (size_bytes) | (_pad2<<32)
                emit_store_qword_vm(asm_, "r3", 16, "8"); // 8 bytes por slot
                // [+24] type_class = 0 (primitive)
                emit_store_qword_vm(asm_, "r3", 24, "0");

                asm_ << "deffield r12, r3\n";
                asm_ << "addsp rsp, 32\n";
            }

            // 2.5) Para cada static field PROPIO: DefFieldParams + deffield
            // con is_static=1.  Solo se emite para los static fields nuevos
            // (los heredados ya los anyadio define_class del loader al
            // copiar el padre).  Layout identico al loop anterior pero con
            // is_static=1 en byte +14 del packed.
            for (size_t si = lay.inherited_static_field_count;
                 si < lay.static_fields.size(); ++si) {
                const auto &   f         = lay.static_fields[si];
                const uint64_t fname_idx = intern_class_name(out, f.name);
                const uint32_t fname_len = static_cast<uint32_t>(f.name.size());
                asm_ << "// deffield static " << f.name << "\n";
                asm_ << "subsp rsp, 32\n";
                asm_ << "mov r3, rsp\n";
                emit_store_qword_vm(asm_, "r3", 0,
                                    "@Absolute(\"code.s_" + std::to_string(fname_idx) + "\")"); {
                    uint64_t packed = uint64_t(fname_len);
                    packed |= (uint64_t(0)) << 32; // kind = FIELD_PRIMITIVE
                    packed |= (uint64_t(0)) << 40; // access = FIELD_PUBLIC
                    packed |= (uint64_t(1)) << 48; // is_static = TRUE
                    emit_store_qword_vm(asm_, "r3", 8, std::to_string(packed));
                }
                emit_store_qword_vm(asm_, "r3", 16, "8"); // 8 bytes por slot
                emit_store_qword_vm(asm_, "r3", 24, "0"); // type_class = 0
                asm_ << "deffield r12, r3\n";
                asm_ << "addsp rsp, 32\n";
            }

            // 3) Para cada metodo PROPIO o sobreescrito: DefMethodParams +
            // defmethod.  Los metodos heredados sin override se copian
            // automaticamente por define_class al instalar la jerarquia
            // (loader); re-emitirlos aqui no es necesario.  El criterio
            // de "propio" es @c defining_class == cd->name.
            //
            // Las interfaces NO emiten defmethod: sus metodos son
            // abstractos (sin code_vaddr).  defclass las registra para
            // que la reflexion las encuentre, pero no aportan vtable.
            if (cd->is_interface) {
                asm_ << "// === fin interface " << cd->name << " (sin defmethod) ===\n";
                continue;
            }
            for (const auto &m: lay.methods) {
                if (!m.defining_class.empty()
                    && m.defining_class != cd->name)
                    continue; // heredado puro
                std::string suffix      = m.is_constructor ? std::string("ctor") : m.name;
                std::string owner_class = m.defining_class.empty()
                                              ? cd->name
                                              : m.defining_class;
                std::string method_label = owner_class + "__" + suffix;
                // Strings para name y descriptor.
                const uint64_t mname_idx = intern_class_name(out, m.name);
                const uint32_t mname_len = static_cast<uint32_t>(m.name.size());
                // Descriptor minimal: solo se usa para reflexion/AOP; aqui
                // ponemos una cadena vacia (idx -1) o simplemente la firma
                // no se valida al definir.  Usamos una cadena "" via idx 0
                // si esta disponible.  Para simplificar, registramos "()".
                const std::string desc_str = "()";
                const uint64_t    desc_idx = intern_class_name(out, desc_str);
                const uint32_t    desc_len = static_cast<uint32_t>(desc_str.size());

                uint64_t mflags = 0;
                if (m.is_constructor) mflags |= /*METHOD_FLAG_CONSTRUCTOR*/ (1ULL << 9);
                else mflags |= /*METHOD_FLAG_VIRTUAL*/ (1ULL << 10);

                asm_ << "// defmethod " << method_label << "\n";
                asm_ << "subsp rsp, 40\n";
                asm_ << "mov r3, rsp\n";
                // [+0] name_addr
                emit_store_qword_vm(asm_, "r3", 0,
                                    "@Absolute(\"code.s_" + std::to_string(mname_idx) + "\")");
                // [+8] (descriptor_len<<32)|name_len
                {
                    uint64_t packed = uint64_t(mname_len)
                            | (uint64_t(desc_len) << 32);
                    emit_store_qword_vm(asm_, "r3", 8, std::to_string(packed));
                }
                // [+16] descriptor_addr
                emit_store_qword_vm(asm_, "r3", 16,
                                    "@Absolute(\"code.s_" + std::to_string(desc_idx) + "\")");
                // [+24] code_vaddr = label del metodo
                emit_store_qword_vm(asm_, "r3", 24,
                                    "@Absolute(\"code." + method_label + "\")");
                // [+32] flags
                emit_store_qword_vm(asm_, "r3", 32, std::to_string(mflags));

                asm_ << "defmethod r12, r3\n";
                asm_ << "addsp rsp, 40\n";

                // Registrar debug info (file + line) para el
                // metodo recien definido.  defmethod retorna vtable_idx
                // en R0, no el MethodInfo*.  Lo obtenemos via findmethod
                // por nombre y emitimos setmethdbg con un params struct
                // de 24 bytes en stack: { method_ptr, file_addr, file_len,
                // start_line }.  Solo si tenemos source info valida.
                if (!m.source_file.empty() && m.source_line > 0) {
                    const uint64_t fname_idx = intern_class_name(out, m.source_file);
                    const uint32_t fname_len = static_cast<uint32_t>(m.source_file.size());

                    // 1. findmethod r5, r4 -> R0 = MethodInfo* del metodo
                    //    recien definido.  r4 -> FindMethodParams { class, name_addr, name_len }.
                    //    Usamos r4 como base porque emit_store_qword_vm
                    //    clobrea r5 y r6 internamente.
                    asm_ << "subsp rsp, 24\n";
                    asm_ << "mov r4, rsp\n";
                    emit_store_qword_vm(asm_, "r4", 0, "r12"); // class_ptr
                    emit_store_qword_vm(asm_, "r4", 8,
                                        "@Absolute(\"code.s_" + std::to_string(mname_idx) + "\")"); {
                        // [+16] name_len (i32, padding superior libre).
                        std::ostringstream tmp;
                        tmp << mname_len;
                        emit_store_qword_vm(asm_, "r4", 16, tmp.str());
                    }
                    asm_ << "findmethod r5, r4\n";
                    asm_ << "addsp rsp, 24\n";
                    // Preservar method_ptr en r3 (no clobrado por
                    // emit_store_qword_vm que usa r5/r6 internamente).
                    asm_ << "mov r3, r5\n";

                    // 2. SetMethDebugParams (24 bytes): { method_ptr,
                    //    file_addr, file_len, start_line }.  Usamos r4
                    //    como base para no clobrear r3 (method_ptr).
                    asm_ << "subsp rsp, 24\n";
                    asm_ << "mov r4, rsp\n";
                    emit_store_qword_vm(asm_, "r4", 0, "r3"); // method_ptr
                    emit_store_qword_vm(asm_, "r4", 8,
                                        "@Absolute(\"code.s_" + std::to_string(fname_idx) + "\")"); {
                        // [+16] file_len (i32) | [+20] start_line (i32).
                        const uint64_t packed = uint64_t(fname_len)
                                | (uint64_t(m.source_line) << 32);
                        std::ostringstream tmp;
                        tmp << packed;
                        emit_store_qword_vm(asm_, "r4", 16, tmp.str());
                    }
                    asm_ << "setmethdbg r3, r4\n";
                    asm_ << "addsp rsp, 24\n";
                }
            }
            asm_ << "// === fin clase " << cd->name << " ===\n";
        }

        // -----------------------------------------------------------------
        // 2do pase: AOP.  Tras definir todas las clases y metodos, recorremos
        // los aspectos y emitimos findclass + findmethod + addadvice por
        // cada @Before/@After/@Around encontrado.  Esto requiere que las
        // clases target ya esten registradas (de ahi el segundo pase).
        // -----------------------------------------------------------------
        for (auto &decl: mod_.decls) {
            if (!decl || decl->kind != ast::NodeKind::ClassDecl) continue;
            auto *cd = static_cast<ast::ClassDecl *>(decl.get());
            if (!cd->type_params.empty()) continue; // template, no se procesa
            for (auto &m_uptr: cd->methods) {
                auto *m = m_uptr.get();
                if (!m || m->advice_kind == 0) continue;

                // Pointcut: "ClassName.methodName" (formato exacto v0).
                const std::string &target = m->advice_target;
                size_t             dot    = target.find('.');
                if (dot == std::string::npos || dot == 0
                    || dot + 1 >= target.size()) {
                    error_at(m->loc,
                             "AOP: pointcut '" + target + "' no tiene formato 'Clase.metodo'");
                    continue;
                }
                const std::string tcls  = target.substr(0, dot);
                const std::string tmeth = target.substr(dot + 1);

                // Convertir advice_kind del AST (1=BEFORE, 2=AFTER, 3=AROUND)
                // al kind del runtime (0=BEFORE, 1=AFTER, 2=AROUND).
                const uint8_t rt_kind = static_cast<uint8_t>(m->advice_kind - 1);

                asm_ << "// === advice " << cd->name << "." << m->name
                        << " " << (rt_kind == 0
                                       ? "BEFORE"
                                       : rt_kind == 1
                                             ? "AFTER"
                                             : "AROUND")
                        << " " << target << " ===\n";

                // 1) findclass de la clase target -> r10
                const uint64_t tcls_idx = intern_class_name(out, tcls);
                const uint32_t tcls_len = static_cast<uint32_t>(tcls.size());
                emit_findclass_inline(asm_, tcls_idx, tcls_len);
                asm_ << "mov r10, r12\n"; // r10 = ClassInfo* target

                // 2) findmethod del target -> r9
                const uint64_t tmeth_idx = intern_class_name(out, tmeth);
                const uint32_t tmeth_len = static_cast<uint32_t>(tmeth.size());
                asm_ << "subsp rsp, 24\n";
                asm_ << "mov r3, rsp\n";
                emit_store_qword_vm(asm_, "r3", 0, "r10"); // class_ptr
                emit_store_qword_vm(asm_, "r3", 8,
                                    "@Absolute(\"code.s_" + std::to_string(tmeth_idx) + "\")");
                emit_store_qword_vm(asm_, "r3", 16, std::to_string(tmeth_len));
                asm_ << "findmethod r9, r3\n";
                asm_ << "addsp rsp, 24\n";

                // 3) findclass del aspect -> r8 (luego findmethod del advice)
                const uint64_t acls_idx = intern_class_name(out, cd->name);
                const uint32_t acls_len = static_cast<uint32_t>(cd->name.size());
                emit_findclass_inline(asm_, acls_idx, acls_len);
                asm_ << "mov r8, r12\n"; // r8 = ClassInfo* aspect

                // 4) findmethod del advice -> r7
                const uint64_t adm_idx = intern_class_name(out, m->name);
                const uint32_t adm_len = static_cast<uint32_t>(m->name.size());
                asm_ << "subsp rsp, 24\n";
                asm_ << "mov r3, rsp\n";
                emit_store_qword_vm(asm_, "r3", 0, "r8");
                emit_store_qword_vm(asm_, "r3", 8,
                                    "@Absolute(\"code.s_" + std::to_string(adm_idx) + "\")");
                emit_store_qword_vm(asm_, "r3", 16, std::to_string(adm_len));
                asm_ << "findmethod r7, r3\n";
                asm_ << "addsp rsp, 24\n";

                // 5) addadvice r9 (target), r7 (advice), kind imm
                asm_ << "addadvice r9, r7, " << static_cast<int>(rt_kind) << "\n";
            }
        }

        ir::IrInstr ra{};
        ra.op          = ir::IrOp::RAW_ASM;
        ra.type        = ir::IrType::VOID;
        ra.dst         = ir::IR_NO_VALUE;
        ra.func_name   = asm_.str();
        ra.source_line = 0;
        fn.append(entry, std::move(ra));

        ir::IrInstr ret{};
        ret.op          = ir::IrOp::RET;
        ret.type        = ir::IrType::VOID;
        ret.source_line = 0;
        fn.append(entry, std::move(ret));

        out.add_function(std::move(fn));
    }

    std::string Lowering::build_module_init_asm(ir::IrModule & /*out_module*/) {
        // No se usa: la generacion de __module_init se hace via
        // generate_module_init_function (IrFunction completa, no cadena).
        return std::string();
    }

    ir::IrValueId Lowering::lower_new_expr(ast::NewExpr *e) {
        // Bajar argumentos.
        std::vector<ir::IrValueId> arg_vals;
        arg_vals.reserve(e->args.size());
        for (auto &a: e->args) {
            const ir::IrValueId av = lower_expr(a.get());
            if (av == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            arg_vals.push_back(av);
        }

        // Emit IrInstr::CALL a __new_<ClassName>(args).  La funcion auxiliar
        // se genera al final de run() via generate_new_helpers.
        const ir::IrValueId dst = fn_->new_value(ir::IrType::PTR);
        // fix - el resultado es un host_ptr a un objeto GESTIONADO por
        // GC.  Marcamos is_gc_object para que el regalloc, al spillarlo
        // alrededor de cualquier CALL posterior (que pueda disparar GC),
        // emita el dance gchandle/gcderef y refresque el host_ptr.
        fn_->values[dst].is_host_ptr  = true;
        fn_->values[dst].is_gc_object = true;
        ir::IrInstr ins{};
        ins.op          = ir::IrOp::CALL;
        ins.type        = ir::IrType::PTR;
        ins.dst         = dst;
        ins.func_name   = "__new_" + e->class_name;
        ins.operands    = std::move(arg_vals);
        ins.source_line = e->loc.line;
        fn_->append(current_block_, std::move(ins));
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
            // 1) findclass inline -> v_cls (mismo patron que forName).
            const uint64_t     cname_idx = intern_class_name(*out_mod_, base_id->name);
            const uint32_t     cname_len = static_cast<uint32_t>(base_id->name.size());
            std::ostringstream fc_oss;
            emit_findclass_inline(fc_oss, cname_idx, cname_len);
            fc_oss << "mov {dst}, r12\n";
            const ir::IrValueId v_cls = fn_->new_value(ir::IrType::I64); {
                ir::IrInstr fc{};
                fc.op          = ir::IrOp::RAW_ASM;
                fc.type        = ir::IrType::I64;
                fc.dst         = v_cls;
                fc.func_name   = fc_oss.str();
                fc.source_line = e->loc.line;
                fn_->append(current_block_, std::move(fc));
            }
            // 2) getstatic {dst}, {src0}, offset_imm  -> v_val.
            // El opcode lee SIEMPRE 8 bytes (i64).  Para tipos < i64 la
            // semantica de sign/zero-extension coincide porque setstatic
            // almacena los bits high del reg fuente que el productor
            // sign-extendio (LOAD/CONST genericos hacen shl+sar para signed).
            const ir::IrType    ir_t  = ir_type_from_primitive(s_typ.kind);
            const ir::IrValueId v_val = fn_->new_value(ir_t);
            ir::IrInstr         gs{};
            gs.op        = ir::IrOp::RAW_ASM;
            gs.type      = ir_t;
            gs.dst       = v_val;
            gs.operands  = {v_cls};
            gs.func_name = std::string("getstatic {dst}, {src0}, ") +
                    std::to_string(s_off) + "\n";
            gs.source_line = e->loc.line;
            fn_->append(current_block_, std::move(gs));
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
        // fix - field de tipo CLASS guarda un GcHandle (estable a
        // evacuacion del GC).  Tras LOADear el handle, hacemos @c gcderef
        // para obtener el host_ptr actual del objeto (refrescado tras
        // cualquier movimiento del GC).  Sin esta refresh, el ptr leido
        // del campo seria stale si el objeto migro a OldGen entre el store
        // y este load -> segfault al hacer @c callvirt o leer fields.
        if (ftyp.kind == PrimitiveKind::CLASS) {
            ir::IrValueId v_host             = fn_->new_value(ir::IrType::I64);
            fn_->values[v_host].is_host_ptr  = true;
            fn_->values[v_host].is_gc_object = true;
            ir::IrInstr deref{};
            deref.op        = ir::IrOp::RAW_ASM;
            deref.type      = ir::IrType::I64;
            deref.dst       = v_host;
            deref.operands  = {dst};
            deref.func_name = std::string(
                "gcderef cur0, {src0}\n"
                "xchg cur0, {dst}\n");
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
            // 1) findclass inline -> v_cls.
            const uint64_t     cname_idx = intern_class_name(*out_mod_, base_id->name);
            const uint32_t     cname_len = static_cast<uint32_t>(base_id->name.size());
            std::ostringstream fc_oss;
            emit_findclass_inline(fc_oss, cname_idx, cname_len);
            fc_oss << "mov {dst}, r12\n";
            const ir::IrValueId v_cls = fn_->new_value(ir::IrType::I64); {
                ir::IrInstr fc{};
                fc.op          = ir::IrOp::RAW_ASM;
                fc.type        = ir::IrType::I64;
                fc.dst         = v_cls;
                fc.func_name   = fc_oss.str();
                fc.source_line = loc.line;
                fn_->append(current_block_, std::move(fc));
            }
            // 2) setstatic {src0}, {src1}, offset_imm  (sin dst).
            // Convencion del opcode: r0=r_class, r1=r_value.
            ir::IrInstr ss{};
            ss.op        = ir::IrOp::RAW_ASM;
            ss.type      = ir::IrType::VOID;
            ss.dst       = ir::IR_NO_VALUE;
            ss.operands  = {v_cls, rhs_cast};
            ss.func_name = std::string("setstatic {src0}, {src1}, ") +
                    std::to_string(s_off) + "\n";
            ss.source_line = loc.line;
            fn_->append(current_block_, std::move(ss));
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
            ir::IrValueId v_handle = fn_->new_value(ir::IrType::I64);
            ir::IrInstr   h{};
            h.op          = ir::IrOp::RAW_ASM;
            h.type        = ir::IrType::I64;
            h.dst         = v_handle;
            h.operands    = {rhs_cast};
            h.func_name   = std::string("gchandle {dst}, {src0}\n");
            h.source_line = loc.line;
            fn_->append(current_block_, std::move(h));
            v_to_store = v_handle;
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
        for (auto &a: e->args) {
            const ir::IrValueId av = lower_expr(a.get());
            if (av == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            arg_vals.push_back(av);
        }
        const ir::IrType ret_ir = ir_type_from_primitive(mtd->return_type.kind);

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

        // -----------------------------------------------------------------
        // Polimorfismo via interface type.
        //
        // Si el receptor esta tipado como interfaz, el vtable_index del
        // metodo en la INTERFAZ no necesariamente coincide con el slot del
        // mismo metodo en la CLASE concreta del objeto receptor (cada
        // clase puede tener su propio orden de metodos).  Para resolver
        // correctamente, hacemos lookup por nombre en runtime:
        //   1. getclass r12, obj            ; ClassInfo* del objeto real
        //   2. findmethod r_method, params  ; busca metodo por nombre
        //   3. callm r1=obj, r_method       ; dispatch dinamico
        //
        // Coste: ~3-4 instrucciones extra por llamada vs CALLVIRT directo,
        // pero garantiza correctness sin requerir method packing entre
        // implementadoras.  Las llamadas a clases concretas (no-interfaz)
        // siguen usando CALLVIRT con vtable_idx -> overhead cero.
        // -----------------------------------------------------------------
        if (lay.is_interface) {
            // Marcar obj como host_ptr (instancia GC-derivada).
            fn_->values[obj].is_host_ptr = true;

            // 1. Cargar ClassInfo* del objeto: load *(obj + 0).
            const ir::IrValueId v_cls = fn_->new_value(ir::IrType::I64); {
                ir::IrInstr ld{};
                ld.op          = ir::IrOp::LOAD;
                ld.type        = ir::IrType::I64;
                ld.dst         = v_cls;
                ld.operands    = {obj};
                ld.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ld));
            }

            // 2. Construir FindMethodParams (24 bytes) en stack.
            const uint64_t name_idx = intern_class_name(*out_mod_, mtd->name);
            const uint32_t name_len = static_cast<uint32_t>(mtd->name.size());

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
                ir::IrInstr st{};
                st.op          = ir::IrOp::STORE;
                st.type        = ir::IrType::I64;
                st.dst         = ir::IR_NO_VALUE;
                st.operands    = {v_cls, v_buf};
                st.source_line = e->loc.line;
                fn_->append(current_block_, std::move(st));
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
                ir::IrInstr st{};
                st.op          = ir::IrOp::STORE;
                st.type        = ir::IrType::I64;
                st.dst         = ir::IR_NO_VALUE;
                st.operands    = {v_name, v_buf8};
                st.source_line = e->loc.line;
                fn_->append(current_block_, std::move(st));
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
                ir::IrInstr st{};
                st.op          = ir::IrOp::STORE;
                st.type        = ir::IrType::I64;
                st.dst         = ir::IR_NO_VALUE;
                st.operands    = {v_len, v_buf16};
                st.source_line = e->loc.line;
                fn_->append(current_block_, std::move(st));
            }
            // 3. findmethod via RAW_ASM: dst = MethodInfo*.
            const ir::IrValueId v_method = fn_->new_value(ir::IrType::I64); {
                ir::IrInstr ra{};
                ra.op          = ir::IrOp::RAW_ASM;
                ra.type        = ir::IrType::I64;
                ra.dst         = v_method;
                ra.operands    = {v_buf};
                ra.func_name   = std::string("findmethod {dst}, {src0}\n");
                ra.source_line = e->loc.line;
                fn_->append(current_block_, std::move(ra));
            }
            // 4. CALLM: operands[0]=obj, operands[1]=method, [2..]=args.
            ir::IrInstr cm{};
            cm.op   = ir::IrOp::CALLM;
            cm.type = ret_ir;
            cm.dst  = dst;
            cm.operands.push_back(obj);
            cm.operands.push_back(v_method);
            for (auto av: arg_vals) cm.operands.push_back(av);
            cm.source_line = e->loc.line;
            fn_->append(current_block_, std::move(cm));
            return dst;
        }

        // Path por defecto: dispatch via vtable_idx (clase concreta).
        ir::IrInstr ins{};
        ins.op   = ir::IrOp::CALLVIRT;
        ins.type = ret_ir;
        ins.dst  = dst;
        // operands[0] = obj, operands[1..] = args declarados
        ins.operands.push_back(obj);
        for (auto av: arg_vals) ins.operands.push_back(av);
        ins.imm         = static_cast<uint64_t>(mtd->vtable_index);
        ins.source_line = e->loc.line;
        fn_->append(current_block_, std::move(ins));
        // fix - si el metodo devuelve un objeto CLASS, el dst es un
        // host_ptr GC.  Marcar para refresh seguro tras CALLs subsiguientes.
        if (dst != ir::IR_NO_VALUE
            && mtd->return_type.kind == PrimitiveKind::CLASS) {
            fn_->values[dst].is_host_ptr  = true;
            fn_->values[dst].is_gc_object = true;
        }
        return dst;
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

    ir::IrValueId Lowering::cast_if_needed(ir::IrValueId v,
                                           ir::IrType    from, ir::IrType to,
                                           uint32_t      source_line) {
        if (from == to || v == ir::IR_NO_VALUE) return v;

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
            op = from_signed ? ir::IrOp::ITOF : ir::IrOp::UITOF;
        } else {
            // Entero -> entero: TRUNC si reduce ancho, ZEXT si amplia unsigned,
            // SEXT si amplia signed.  CAST funciona como fallback generico.
            op = ir::IrOp::CAST;
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
                    // A.10 closures: las captures mutadas dentro del body
                    // de la lambda deben ser address-taken en el outer
                    // scope (capture-by-reference).  El env block guarda
                    // su PUNTERO; el helper hace LOAD/STORE indirectos.
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
                case ast::NodeKind::ReturnStmt: {
                    auto *r = static_cast<ast::ReturnStmt *>(st);
                    visit_expr(r->value.get());
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
            // A.17.y - Si la variable tiene slot activo en un try, ALSO
            // emitir STORE al slot para que el throw + catch vea el ultimo
            // valor escrito incluso tras corrupcion de registros.
            auto it_slot = try_spill_slots_.find(name);
            if (it_slot != try_spill_slots_.end()) {
                ir::IrInstr st{};
                st.op          = ir::IrOp::STORE;
                st.type        = ir::IrType::I64;
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
                     std::string("lowering: caracteristica aun no soportada en A.1: ") + feature);
    }

    void Lowering::error_at(SourceLoc loc, std::string msg) {
        diags_.error(std::move(loc), std::move(msg));
    }
} // namespace vex
